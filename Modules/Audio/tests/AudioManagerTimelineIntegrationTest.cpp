#include "audio/AudioManager.h"

#include "config/AppConfig.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;

// 本集成测试启动真实 SDL 音频后端，验证 AudioManager 复合时间线的完整链路：
//
// - 空片段时间线仍由谱面结束时间提供独立播放时钟；
// - 资源级音量、速度、音高与 EQ 在加载阶段离线处理，不泄漏成全局参数；
// - 同文件按路径只解码一次，不同资源配置各自准备 PCM；
// - 玩家与 BGM KeySound 控制按 block 生效但不重建调度；
// - 同一 Effect 的自动采样与 HitEffect 共享准备后的 PCM；
// - ScrubUpdate 不清理音效池，提交 Seek 才执行完整低频清理；
// - 旧 loadBGM 入口仍包装到复合时间线而不恢复旧 SourceNode 时钟；
// - 重复文件、负起点、静音片段、缺失资源和自然结束可共同工作；
// - unload 释放不再使用的完整解码音轨；
// - Streaming 选择到达播放与试听，而分析入口仍强制完整缓存。
//
// 后端时钟由真实回调线程推进，测试只在有界 waitUntil 中轮询公开状态。固定
// sleep 仅用于确认一段已经稳定的暂停或控制状态不会自行变化，不用于同步数据。
//
// 场景共享同一个 AudioManager 单例，因此每个函数负责恢复自己修改的全局参数，
// 临时音效池也在返回前卸载。需要观察调度接管时使用 clockSnapshot 的
// generation，
// 需要观察播放推进时使用公开秒数和状态；测试不接触管理器内部节点指针。
//
// 各场景只通过公开 API 观察以下三层状态，避免测试和内部节点布局耦合：
//
// - 加载结果描述资源发现、准备和片段调度是否完整；
// - 管理器 getter 描述业务线程可见的控制值与播放位置；
// - clockSnapshot 描述音频回调是否接管了指定代次的调度。
//
// 浮点时长断言使用与语义匹配的容差：纯状态缓存采用接近机器精度的阈值，
// 经过采样率换算的片段长度允许约二十毫秒，真实回调推进位置则只检查方向和
// 合理下界。这样既能发现参数串层，又不会把设备 block 大小当作固定协议。
//
// 共享单例要求各场景遵守以下清理约定：
//
// - 修改全局速度后恢复为 1.0；
// - 修改全局音高后恢复为零；
// - 创建主轨 EQ 后在场景末尾销毁；
// - 注册临时音效后在所有可达失败出口卸载；
// - 切换 Streaming 后恢复 Cached；
// - 最终由 main 统一 shutdown 音频后端。
//
// 加载计数的含义也按处理阶段分别断言：
//
// - requestedSourceCount 按规范化物理路径去重；
// - preparedResourceCount 按会改变 PCM 的 DSP 身份去重；
// - loadedClipCount 统计进入调度的有效事件；
// - missingClipCount 统计因资源不可用而降级的事件。
//
// volume 与 muted 属于运行时混音控制，不改变准备 PCM 身份；playbackSpeed、
// playbackPitch 和 EQ 会改变离线处理结果，必须进入资源身份。相关场景故意让
// 这些字段交叉变化，以防实现简单地按完整配置或仅按路径缓存。
//
// 测试不校验扬声器实际输出内容；这里的验收边界是图构造、公开状态、缓存身份
// 与真实回调时钟。音频内容正确性由资源处理器和 MixerNode 的独立测试覆盖。

/// @brief 在真实音频后端回调推进期间等待一个有界条件。
/// @tparam Predicate 无参数条件函数类型。
/// @param predicate 待满足条件。
/// @param timeout 最长等待时间。
/// @return 条件在超时前满足时返回 true。
template<typename Predicate>
bool waitUntil(Predicate&&               predicate,
               std::chrono::milliseconds timeout = 2000ms)
{
    // steady_clock 不受系统时间校准影响，适合作为测试超时边界。
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while ( std::chrono::steady_clock::now() < deadline ) {
        if ( predicate() ) return true;
        // 测试线程短暂让出 CPU；生产 UI 与逻辑路径不使用这种阻塞轮询。
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

/// @brief 判断加载结果是否包含指定诊断。
/// @param result 待检查加载结果。
/// @param code 目标诊断类型。
/// @return 至少包含一项时返回 true。
bool hasDiagnostic(const MMM::Audio::AudioTimelineLoadResult&  result,
                   MMM::Audio::AudioTimelineLoadDiagnosticCode code)
{
    // 只按稳定枚举匹配，诊断文本用于展示而不应成为测试协议。
    return std::any_of(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [code](const MMM::Audio::AudioTimelineLoadDiagnostic& diagnostic) {
            return diagnostic.code == code;
        });
}

/// @brief 验证无音频片段时谱面结束时间仍可独立推进。
/// @param manager 已初始化音频管理器。
/// @return 验证通过时返回 true。
///
/// 加载零事件、80 ms 谱面结束时间并指定指纹。加载结果必须区分请求资源数、
/// 已准备资源数、片段数和缺失数均为零，同时管理器仍报告已加载时间线。
/// 播放后真实后端应在约 80 ms 自然停止，证明时钟不依赖任何 PCM 片段存在。
///
/// 加载阶段还检查 fingerprint 原样保留和 getTotalTime 在两毫秒误差内。自然
/// 结束后 getCurrentTime 应至少达到 75 ms，避免后端错误地一启动就停止却仍
/// 满足状态断言。超时缩短为一秒，因为理论播放长度只有 80 ms。
bool testEmptyTimelineClock(MMM::Audio::AudioManager& manager)
{
    // 空列表不是卸载操作：它仍需创建带指纹和谱面尾端的有效调度。
    const auto result = manager.loadAudioTimeline({}, 0.08, "empty-timeline");
    if ( !result.success || result.loadedClipCount != 0U ||
         result.missingClipCount != 0U || result.requestedSourceCount != 0U ||
         result.preparedResourceCount != 0U ||
         !manager.hasLoadedAudioTimeline() ||
         manager.getLoadedAudioTimelineFingerprint() != "empty-timeline" ||
         std::abs(manager.getTotalTime() - 0.08) > 0.002 ) {
        XERROR("Empty timeline was not constructed as an independent clock");
        return false;
    }

    // 播放结束允许两毫秒计时误差，但必须由后端状态真正变为 Stopped。
    manager.play();
    if ( !waitUntil(
             [&]() {
                 return manager.getStatus() ==
                        MMM::Audio::PlaybackStatus::Stopped;
             },
             1000ms) ||
         manager.getCurrentTime() < 0.075 ) {
        XERROR("Empty timeline did not reach its chart-defined end");
        return false;
    }
    return true;
}

/// @brief 验证单片段资源 DSP 生效且不会提升为全局预览参数。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 验证通过时返回 true。
///
/// 单资源同时配置 0.75 音量、1.5 速度、+3 半音和十段 EQ，再叠加事件音量
/// 0.5。加载后全局速度仍为一、全局音高为零、主 EQ 关闭，证明处理没有提升到
/// 预览图。总时长则必须按资源 1.5 倍速度缩短并加上 10 ms 事件起点。
///
/// 结果计数还要求 requestedSourceCount、preparedResourceCount、loadedClipCount
/// 均为一且没有缺失或诊断。诊断列表中的 message 非空检查作为接口质量兜底，
/// 即便本场景预期列表为空也保持未来新增非致命诊断时的约束。
bool testSingleClipResourceProcessing(MMM::Audio::AudioManager& manager,
                                      const std::string&        samplePath)
{
    // 同时启用所有资源级处理项，使任一路径遗漏都能由时长或全局状态暴露。
    MMM::AudioTrackConfig config;
    config.volume        = 0.75F;
    config.playbackSpeed = 1.5F;
    config.playbackPitch = 3.0F;
    config.eqEnabled     = true;
    config.eqPreset      = static_cast<int>(MMM::Audio::EQPreset::TenBand);
    config.eqBandGains.assign(10U, 3.0F);
    config.eqBandQs.assign(10U, 1.2F);

    // 资源配置只属于这一个 PreparedTimelineAudio，不应改写 manager 全局控制。
    const auto result =
        manager.loadAudioTimeline({ MMM::Audio::AudioTimelineLoadEvent{
                                      .eventId               = 11U,
                                      .resourceKey           = "main",
                                      .filePath              = samplePath,
                                      .effectiveStartSeconds = 0.01,
                                      .eventVolume           = 0.5F,
                                      .resourceConfig        = config,
                                  } },
                                  0.02,
                                  "single-timeline");

    if ( !result.success || result.requestedSourceCount != 1U ||
         result.preparedResourceCount != 1U || result.loadedClipCount != 1U ||
         result.missingClipCount != 0U || !result.diagnostics.empty() ||
         std::any_of(result.diagnostics.begin(),
                     result.diagnostics.end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.empty();
                     }) ||
         std::abs(manager.getPlaybackSpeed() - 1.0) > 1.0e-6 ||
         std::abs(manager.getPlaybackPitch()) > 1.0e-6 ||
         manager.isMainTrackEQEnabled() ) {
        XERROR(
            "Per-resource advanced settings leaked into the composite graph");
        return false;
    }

    const auto   rawTrack = manager.getBGMTrack();
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    // getBGMTrack 保留兼容语义，返回首个解码源而非处理后的资源副本。
    if ( !rawTrack || sampleRate <= 0.0 ) return false;
    // 原始 AudioTrack 帧数除资源速度得到处理后片段长度，再叠加事件起点。
    const double expectedEnd =
        0.01 + static_cast<double>(rawTrack->num_frames()) /
                   (sampleRate * static_cast<double>(config.playbackSpeed));
    if ( std::abs(manager.getTotalTime() - expectedEnd) > 0.02 ) {
        XERROR("Per-resource playbackSpeed did not change clip duration");
        return false;
    }
    return true;
}

/// @brief 验证玩家与 BGM 逐轨静音、增益不会重建时间线调度。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 状态可独立切换且调度代次、时长和片段数不变时返回 true。
///
/// 两个 BGM 片段分别位于轨道 0 与 2，加载后先等待 scheduleGeneration 被后端
/// 接管。随后修改玩家区、玩家轨一、BGM 轨二和 BGM 区的 mute/gain，检查相邻
/// 轨道不受影响，并确认代次、片段数和总时长完全不变。最后恢复单位控制。
///
/// 玩家控制没有直接对应本场景 BGM 片段，但仍通过 getter 验证固定控制库入口。
/// BGM 轨二与轨一交叉查询用于发现索引偏移错误。30 ms 后 generation 不变是
/// 核心约束：这些参数由 MixerNode 实时读取，不能触发资源准备或时间线替换。
bool testKeySoundTrackMutes(MMM::Audio::AudioManager& manager,
                            const std::string&        samplePath)
{
    // 使用不连续轨道索引，覆盖控制库中间空槽的默认值和索引稳定性。
    const auto result =
        manager.loadAudioTimeline({ MMM::Audio::AudioTimelineLoadEvent{
                                        .eventId               = 501U,
                                        .resourceKey           = "lane-zero",
                                        .filePath              = samplePath,
                                        .effectiveStartSeconds = 0.0,
                                        .bgmTrackIndex         = 0U,
                                    },
                                    MMM::Audio::AudioTimelineLoadEvent{
                                        .eventId               = 502U,
                                        .resourceKey           = "lane-two",
                                        .filePath              = samplePath,
                                        .effectiveStartSeconds = 0.02,
                                        .bgmTrackIndex         = 2U,
                                    } },
                                  0.08,
                                  "key-sound-track-mutes");
    if ( !result.success || result.loadedClipCount != 2U ||
         result.scheduleGeneration == 0U ) {
        return false;
    }
    if ( !waitUntil(
             [&manager, scheduleGeneration = result.scheduleGeneration]() {
                 const auto snapshot = manager.getAudioTimelineClockSnapshot();
                 return snapshot.valid &&
                        snapshot.scheduleGeneration == scheduleGeneration;
             }) ) {
        XERROR("Key sound timeline schedule was not applied by the backend");
        return false;
    }

    // 固定加载元数据基线，用于证明实时控制不走 replaceSchedule。
    const double totalTime          = manager.getTotalTime();
    const auto   scheduleGeneration = result.scheduleGeneration;
    manager.setPlayerKeySoundAreaMuted(true);
    manager.setPlayerKeySoundTrackMuted(1U, true);
    manager.setPlayerKeySoundTrackGain(1U, 1.25F);
    manager.setBgmKeySoundTrackMuted(2U, true);
    manager.setBgmKeySoundTrackGain(2U, 0.5F);
    manager.setBgmKeySoundAreaMuted(true);
    manager.setBgmKeySoundAreaGain(0.75F);
    // 给真实回调至少一个 block 读取新值；控制 API 自身不依赖该等待才返回。
    std::this_thread::sleep_for(30ms);
    if ( !manager.isPlayerKeySoundAreaMuted() ||
         !manager.isPlayerKeySoundTrackMuted(1U) ||
         manager.isPlayerKeySoundTrackMuted(2U) ||
         std::abs(manager.getPlayerKeySoundTrackGain(1U) - 1.25F) > 1.0e-4F ||
         !manager.isBgmKeySoundTrackMuted(2U) ||
         manager.isBgmKeySoundTrackMuted(1U) ||
         std::abs(manager.getBgmKeySoundTrackGain(2U) - 0.5F) > 1.0e-4F ||
         !manager.isBgmKeySoundAreaMuted() ||
         std::abs(manager.getBgmKeySoundAreaGain() - 0.75F) > 1.0e-4F ||
         manager.getLoadedAudioTimelineClipCount() != 2U ||
         std::abs(manager.getTotalTime() - totalTime) > 0.002 ||
         manager.getAudioTimelineClockSnapshot().scheduleGeneration !=
             scheduleGeneration ) {
        XERROR("Key sound runtime control changed timeline schedule");
        return false;
    }

    // 恢复所有改动值并复查，避免共享 manager 把静音传给后续播放场景。
    manager.setPlayerKeySoundAreaMuted(false);
    manager.setPlayerKeySoundTrackMuted(1U, false);
    manager.setPlayerKeySoundTrackGain(1U, 1.0F);
    manager.setBgmKeySoundTrackMuted(2U, false);
    manager.setBgmKeySoundTrackGain(2U, 1.0F);
    manager.setBgmKeySoundAreaMuted(false);
    manager.setBgmKeySoundAreaGain(1.0F);
    // 恢复检查也是后续场景的前置条件，不能只依赖 setter 无条件成功。
    return !manager.isPlayerKeySoundAreaMuted() &&
           !manager.isPlayerKeySoundTrackMuted(1U) &&
           std::abs(manager.getPlayerKeySoundTrackGain(1U) - 1.0F) < 1.0e-4F &&
           !manager.isBgmKeySoundTrackMuted(2U) &&
           std::abs(manager.getBgmKeySoundTrackGain(2U) - 1.0F) < 1.0e-4F &&
           !manager.isBgmKeySoundAreaMuted() &&
           std::abs(manager.getBgmKeySoundAreaGain() - 1.0F) < 1.0e-4F;
}

/// @brief 验证同一文件的不同资源倍率可并存且全局预览倍率不改写资源时长。
/// @param manager 已初始化音频管理器。
/// @param samplePath 同时供快慢两种资源配置使用的音频。
/// @return 解码去重、DSP 分离和全局速度隔离均正确时返回 true。
///
/// 同一路径创建 2.0 倍与 0.5 倍两个资源，预期请求源数为一、准备资源数为二、
/// 片段数为二。复合结束位置由较慢片段决定。之后把全局预览速度设为 1.75，
/// getTotalTime 仍应保持资源定义时长，全局 getter 则反映新预览倍率。
///
/// fast 与 slow 的 resourceKey 和 eventId 各自不同，但 filePath 相同，明确区分
/// 解码身份与 DSP 身份。chartEndSeconds 为零，使复合总时长完全由较慢资源决定。
/// 测试结束恢复全局速度一，确保后续共享管理器场景从中性预览倍率开始。
bool testIndependentResourceAndGlobalSpeed(MMM::Audio::AudioManager& manager,
                                           const std::string&        samplePath)
{
    // filePath 决定解码去重，resourceKey 与配置共同决定准备结果身份。
    MMM::AudioTrackConfig fastConfig;
    fastConfig.playbackSpeed = 2.0F;
    MMM::AudioTrackConfig slowConfig;
    slowConfig.playbackSpeed = 0.5F;

    manager.setPlaybackSpeed(1.0);
    const auto result = manager.loadAudioTimeline(
        {
            {
                .eventId               = 21U,
                .resourceKey           = "fast",
                .filePath              = samplePath,
                .effectiveStartSeconds = 0.0,
                .eventVolume           = 1.0F,
                .resourceConfig        = fastConfig,
            },
            {
                .eventId               = 22U,
                .resourceKey           = "slow",
                .filePath              = samplePath,
                .effectiveStartSeconds = 0.0,
                .eventVolume           = 1.0F,
                .resourceConfig        = slowConfig,
            },
        },
        0.0,
        "independent-resource-speeds");
    const auto   rawTrack = manager.getBGMTrack();
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    // 这里分别核对一份解码源、两份资源准备结果和两个调度片段。
    if ( !result.success || result.requestedSourceCount != 1U ||
         result.preparedResourceCount != 2U || result.loadedClipCount != 2U ||
         !rawTrack || sampleRate <= 0.0 ) {
        XERROR("Different resource speeds did not load together");
        return false;
    }

    // 两个片段同起点，0.5 倍慢速资源拥有最长处理后 PCM。
    const double expectedEnd =
        static_cast<double>(rawTrack->num_frames()) /
        (sampleRate * static_cast<double>(slowConfig.playbackSpeed));
    const double resourceDefinedEnd = manager.getTotalTime();
    manager.setPlaybackSpeed(1.75);
    if ( std::abs(resourceDefinedEnd - expectedEnd) > 0.02 ||
         std::abs(manager.getTotalTime() - resourceDefinedEnd) > 1.0e-9 ||
         std::abs(manager.getPlaybackSpeed() - 1.75) > 1.0e-9 ) {
        XERROR("Global preview speed overwrote per-resource duration");
        return false;
    }
    manager.setPlaybackSpeed(1.0);
    return true;
}

/// @brief 验证同一 Effect 的自动采样和 Note HitEffect 共用资源 DSP PCM。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 缓存身份、资源时长、初始静音和全局变速隔离均正确时返回 true。
///
/// 自动采样先以静音、1.6 倍、+5 半音和 EQ 的资源配置进入时间线；随后用相同
/// key/path/config 注册 HitEffect 并排队加载。音效池必须复用同一准备 PCM，
/// 时长与时间线片段完全一致，资源音量和静音仍作为池控制保留。
/// 全局预览速度不得再次改变任一准备 PCM 的固有时长。
///
/// 配置选择 muted=true 是为了确认 PCM 缓存身份不包含运行时静音，池仍可在解除
/// 静音后播放同一准备内容。queueBoundNoteSoundEffectLoad 标记该池为绑定音效，
/// waitUntil 内显式驱动 updateQueuedSoundEffectLoads 模拟应用低频更新循环。
/// 任何失败分支都卸载 EFFECT_KEY，避免共享 manager 留下注册或池状态。
bool testDualUseEffectSharesPreparedAudio(MMM::Audio::AudioManager& manager,
                                          const std::string&        samplePath)
{
    // 固定 key 同时作为时间线资源身份和音效池注册身份，触发共享查询路径。
    constexpr const char* EFFECT_KEY = "dual-use-effect";
    MMM::AudioTrackConfig config;
    config.volume        = 0.63F;
    config.muted         = true;
    config.playbackSpeed = 1.6F;
    config.playbackPitch = 5.0F;
    config.eqEnabled     = true;
    config.eqPreset      = static_cast<int>(MMM::Audio::EQPreset::TenBand);
    config.eqBandGains.assign(10U, 2.0F);
    config.eqBandQs.assign(10U, 1.1F);

    manager.setPlaybackSpeed(1.0);
    const auto result =
        manager.loadAudioTimeline({ MMM::Audio::AudioTimelineLoadEvent{
                                      .eventId               = 31U,
                                      .resourceKey           = EFFECT_KEY,
                                      .filePath              = samplePath,
                                      .effectiveStartSeconds = 0.0,
                                      .eventVolume           = 1.0F,
                                      .resourceConfig        = config,
                                  } },
                                  0.0,
                                  "dual-use-effect-timeline");
    const auto rawTrack = manager.getBGMTrack();
    const auto sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( !result.success || result.requestedSourceCount != 1U ||
         result.preparedResourceCount != 1U || result.loadedClipCount != 1U ||
         !rawTrack || sampleRate <= 0.0 ) {
        XERROR("Dual-use Effect timeline could not be prepared");
        return false;
    }

    // 队列更新由测试线程低频轮询，直到准备完成或有界超时。
    manager.registerSoundEffect(EFFECT_KEY, samplePath, config);
    if ( !manager.queueBoundNoteSoundEffectLoad(EFFECT_KEY) ||
         !waitUntil([&]() {
             manager.updateQueuedSoundEffectLoads();
             return manager.isSoundEffectLoaded(EFFECT_KEY);
         }) ) {
        XERROR("Dual-use Effect could not create its HitEffect pool");
        return false;
    }

    // 两个消费者应从同一 DSP 结果获得完全相等的时长，而不是各自近似处理。
    const double timelineDuration = manager.getTotalTime();
    const double sfxDuration      = manager.getSFXDuration(EFFECT_KEY);
    const double expectedDuration =
        static_cast<double>(rawTrack->num_frames()) /
        (sampleRate * static_cast<double>(config.playbackSpeed));
    if ( !manager.isSFXUsingSharedTimelineAudio(EFFECT_KEY) ||
         std::abs(timelineDuration - sfxDuration) > 1.0e-9 ||
         std::abs(sfxDuration - expectedDuration) > 0.02 ||
         std::abs(manager.getSFXPoolVolume(EFFECT_KEY) - config.volume) >
             1.0e-6F ||
         !manager.getSFXPoolMute(EFFECT_KEY) ) {
        XERROR(
            "Dual-use Effect did not share prepared DSP PCM or resource gain");
        manager.unloadSoundEffect(EFFECT_KEY);
        return false;
    }

    // 资源速度已经烘焙进共享 PCM，全局速度只作用复合图外层。
    manager.setPlaybackSpeed(1.75);
    if ( std::abs(manager.getTotalTime() - timelineDuration) > 1.0e-9 ||
         std::abs(manager.getSFXDuration(EFFECT_KEY) - sfxDuration) > 1.0e-9 ) {
        XERROR("Global preview speed was applied twice to dual-use Effect PCM");
        manager.unloadSoundEffect(EFFECT_KEY);
        return false;
    }

    // ScrubUpdate 是连续交互预览，不应遍历并停止已经排定的音效 voice。
    manager.setSFXPoolMute(EFFECT_KEY, false, false);
    manager.seek(0.0);
    manager.playSoundEffectScheduled(EFFECT_KEY, 60.0);
    if ( !manager.isSFXPlaying(EFFECT_KEY) ) {
        XERROR("Scrub seek setup did not schedule the dual-use Effect");
        manager.unloadSoundEffect(EFFECT_KEY);
        return false;
    }
    // 连续拖动更新只改时间锚点，不能为每次鼠标移动遍历所有音效池。
    manager.seek(0.01, MMM::Audio::AudioSeekMode::ScrubUpdate);
    if ( !manager.isSFXPlaying(EFFECT_KEY) ) {
        XERROR("Scrub update unexpectedly traversed and cleared SFX pools");
        manager.unloadSoundEffect(EFFECT_KEY);
        return false;
    }
    // 普通 Seek 表示提交操作，必须清除旧位置排定的 SFX。
    manager.seek(0.01);
    if ( manager.isSFXPlaying(EFFECT_KEY) ) {
        XERROR("Committed seek did not clear scheduled SFX pools");
        manager.unloadSoundEffect(EFFECT_KEY);
        return false;
    }

    manager.setPlaybackSpeed(1.0);
    manager.unloadSoundEffect(EFFECT_KEY);
    // 正常出口也显式释放池，避免单例持有共享 PCM 影响后续生命周期场景。
    return true;
}

/// @brief 验证旧单 BGM 入口只包装零秒单事件且不恢复旧 SourceNode 时钟。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 验证通过时返回 true。
///
/// 调用前故意设置全局音高、拉伸质量和主 EQ。loadBGM 应只把旧 API 输入包装
/// 成零秒单事件复合时间线：资源速度离线处理，但全局音高、质量与 EQ 仍保持。
/// BGM 路径和片段数也必须通过新时间线状态查询正确暴露。
///
/// 资源 config 的 playbackPitch=-4 与 playbackSpeed=1.75 属于离线片段处理；
/// 调用前设置的全局 pitch=2、quality=Fast、TenBand EQ 则属于复合图外层。
/// loadBGM 后全局 speed 被新兼容入口规范为一，但其他显式预览参数必须保留。
bool testLegacyBgmWrapper(MMM::Audio::AudioManager& manager,
                          const std::string&        samplePath)
{
    // 资源配置和全局预览配置故意不同，防止测试把两层同值误判为正确隔离。
    MMM::AudioTrackConfig config;
    config.volume        = 0.7F;
    config.playbackSpeed = 1.75F;
    config.playbackPitch = -4.0F;
    manager.setPlaybackPitch(2.0);
    manager.setPlaybackQuality(MMM::Audio::AudioManager::StretchQuality::Fast);
    manager.createMainTrackEQ(MMM::Audio::EQPreset::TenBand);
    if ( !manager.loadBGM(samplePath, config) ||
         !manager.hasLoadedAudioTimeline() ||
         manager.getLoadedAudioTimelineClipCount() != 1U ||
         manager.getLoadedBGMPath() != samplePath ||
         std::abs(manager.getPlaybackSpeed() - 1.0) > 1.0e-6 ||
         std::abs(manager.getPlaybackPitch() - 2.0) > 1.0e-6 ||
         manager.getPlaybackQuality() !=
             MMM::Audio::AudioManager::StretchQuality::Fast ||
         !manager.isMainTrackEQEnabled() ) {
        XERROR("Legacy BGM wrapper bypassed the composite timeline");
        return false;
    }
    // 恢复共享管理器状态，避免后续场景继承此测试的全局预览参数。
    manager.setPlaybackPitch(0.0);
    manager.setPlaybackQuality(MMM::Audio::AudioManager::StretchQuality::Finer);
    manager.destroyMainTrackEQ();
    return true;
}

/// @brief 验证重复文件、多资源、缺失资源及暂停、Seek、停止和自然结束。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 验证通过时返回 true。
///
/// 四个事件包含：负起点主资源、同文件不同事件音量、同文件静音资源配置、缺失
/// 文件。路径去重后请求源数为二，处理配置按静音不改变 PCM 的规则复用为一，
/// 三个有效事件进入调度，一个缺失事件产生诊断但不让整体加载失败。
/// 随后用真实后端覆盖全局变速、暂停冻结、Seek、恢复、Stop 回零和自然结束。
///
/// 三个有效事件共享同一物理文件：mainConfig 与 effectConfig 仅音量不同，而
/// mutedConfig 只改变运行时静音，因此资源 DSP 缓存可共同复用一份 PCM。
/// chartEndSeconds=0.5 明显长于短样本片段，保证总时长由谱面定义而非资源尾部。
/// MissingResource 是允许降级诊断，其余加载阶段仍须成功构造可播放时间线。
bool testCompositePlayback(MMM::Audio::AudioManager& manager,
                           const std::string&        samplePath)
{
    // 三份配置用于覆盖运行时增益、运行时静音和默认 DSP 身份归一化。
    MMM::AudioTrackConfig mainConfig;
    mainConfig.volume = 0.8F;
    MMM::AudioTrackConfig effectConfig;
    effectConfig.volume = 0.6F;
    MMM::AudioTrackConfig mutedConfig;
    mutedConfig.volume = 1.0F;
    mutedConfig.muted  = true;

    const auto missingPath = (std::filesystem::path(samplePath).parent_path() /
                              "missing-audio-timeline-resource.wav")
                                 .string();
    // 缺失路径位于样本同目录且使用固定不存在文件名，避免平台临时目录差异。
    const auto result = manager.loadAudioTimeline(
        {
            {
                .eventId               = 1U,
                .resourceKey           = "main-a",
                .filePath              = samplePath,
                .effectiveStartSeconds = -0.01,
                .eventVolume           = 1.0F,
                .resourceConfig        = mainConfig,
            },
            {
                .eventId               = 2U,
                .resourceKey           = "effect-a",
                .filePath              = samplePath,
                .effectiveStartSeconds = 0.01,
                .eventVolume           = 0.5F,
                .resourceConfig        = effectConfig,
            },
            {
                .eventId               = 3U,
                .resourceKey           = "main-b",
                .filePath              = samplePath,
                .effectiveStartSeconds = 0.02,
                .eventVolume           = 1.0F,
                .resourceConfig        = mutedConfig,
            },
            {
                .eventId               = 4U,
                .resourceKey           = "missing",
                .filePath              = missingPath,
                .effectiveStartSeconds = 0.0,
                .eventVolume           = 1.0F,
                .resourceConfig        = effectConfig,
            },
        },
        0.5,
        "composite-timeline");

    // 缺失资源允许局部降级，但不能进入已加载片段或准备资源统计。
    if ( !result.success || result.requestedSourceCount != 2U ||
         result.preparedResourceCount != 1U || result.loadedClipCount != 3U ||
         result.missingClipCount != 1U ||
         manager.getLoadedAudioTimelineClipCount() != 3U ||
         manager.getMissingAudioTimelineClipCount() != 1U ||
         manager.getLoadedAudioTimelineFingerprint() != "composite-timeline" ||
         !hasDiagnostic(
             result,
             MMM::Audio::AudioTimelineLoadDiagnosticCode::MissingResource) ||
         manager.getTotalTime() < 0.49 ) {
        XERROR("Composite timeline phases did not deduplicate sources and DSP");
        return false;
    }

    // 变速后同时等待位置推进与实际速度确认，证明外层 stretcher 已接管。
    manager.setPlaybackSpeed(1.5);
    manager.seek(0.0);
    manager.play();
    if ( !waitUntil([&]() {
             return manager.getCurrentTime() > 0.04 &&
                    std::abs(manager.getActualPlaybackSpeed() - 1.5) <= 0.02;
         }) ) {
        XERROR("Composite timeline did not follow global preview speed");
        return false;
    }

    // Pause 先等待公开状态确认，再比较两个时间点确保位置冻结。
    manager.pause();
    if ( !waitUntil([&]() {
             return manager.getStatus() == MMM::Audio::PlaybackStatus::Paused;
         }) ) {
        XERROR("Composite timeline did not pause");
        return false;
    }
    // 暂停后把速度恢复为一，验证参数更新本身不会偷偷推进冻结时钟。
    manager.setPlaybackSpeed(1.0);
    std::this_thread::sleep_for(30ms);
    const double pausedTime = manager.getCurrentTime();
    std::this_thread::sleep_for(40ms);
    if ( std::abs(manager.getCurrentTime() - pausedTime) > 0.002 ) {
        XERROR("Paused composite timeline continued advancing");
        return false;
    }

    // 暂停状态 Seek 应立即把时间锚点移动到 0.2 秒而不自动播放。
    manager.seek(0.2);
    if ( !waitUntil(
             [&]() { return std::abs(manager.getCurrentTime() - 0.2) < 0.01; },
             1000ms) ) {
        XERROR("Composite timeline seek did not reach target");
        return false;
    }

    // 从 Seek 位置恢复后等待超过 230 ms，证明播放确实继续而非只改状态标志。
    manager.play();
    if ( !waitUntil([&]() { return manager.getCurrentTime() > 0.23; }) ) {
        XERROR("Composite timeline did not resume after seek");
        return false;
    }

    // Stop 既改变状态又回零，两个公开条件必须在同一稳定阶段成立。
    manager.stop();
    if ( !waitUntil(
             [&]() { return std::abs(manager.getCurrentTime()) < 0.002; },
             1000ms) ||
         manager.getStatus() != MMM::Audio::PlaybackStatus::Stopped ) {
        XERROR("Composite timeline stop did not clear its position");
        return false;
    }

    // 从尾部前 20 ms 播放，缩短等待并验证自然结束发布 Stopped。
    manager.seek(manager.getTotalTime() - 0.02);
    manager.play();
    // 自然结束不同于显式 stop：由回调越过谱面尾端并发布最终状态。
    if ( !waitUntil(
             [&]() {
                 return manager.getStatus() ==
                        MMM::Audio::PlaybackStatus::Stopped;
             },
             1000ms) ) {
        XERROR("Composite timeline did not publish natural completion");
        return false;
    }
    return true;
}

/// @brief 验证卸载时间线会释放音频池中已无使用者的完整解码音轨。
/// @param manager 已初始化音频管理器。
/// @param samplePath 可解码短音频路径。
/// @return 时间线卸载后原始音轨不再被缓存强引用保活时返回 true。
///
/// weak_ptr 在卸载前必须有效，证明测试确实观察到本次加载的 AudioTrack；调用
/// unloadAudioTimeline 后不再有时间线、准备 PCM 或 AudioPool 强引用，因而应
/// 立即过期。该场景不保留 getBGMTrack 返回的临时 shared_ptr。
///
/// 资源配置使用默认值，避免 DSP 生成自有 PCM 后让原始音轨生命周期与测试目标
/// 发生混淆。指纹独立设置，确保 load 调用确实替换了前一场景时间线。
bool testTimelineUnloadReleasesDecodedTrack(MMM::Audio::AudioManager& manager,
                                            const std::string& samplePath)
{
    // 不在局部变量中保留 shared_ptr，确保 weak_ptr 只观察管理器内部所有权。
    const auto result =
        manager.loadAudioTimeline({ MMM::Audio::AudioTimelineLoadEvent{
                                      .eventId               = 601U,
                                      .resourceKey           = "release-track",
                                      .filePath              = samplePath,
                                      .effectiveStartSeconds = 0.0,
                                  } },
                                  0.0,
                                  "release-decoded-track");
    std::weak_ptr<ice::AudioTrack> decodedTrack = manager.getBGMTrack();
    if ( !result.success || decodedTrack.expired() ) {
        XERROR("Decoded track was unavailable before timeline unload");
        return false;
    }

    // 卸载负责断开音频图并清理池缓存，不要求等待另一个音频 block。
    manager.unloadAudioTimeline();
    if ( !decodedTrack.expired() ) {
        XERROR("Timeline unload left decoded track retained by AudioPool");
        return false;
    }
    return true;
}

/// @brief 流式偏好必须到达真实时间线与试听，分析仍返回完整缓存。
/// @param manager 已初始化且可重复切换解码策略的管理器。
/// @param path 同时用于 BGM、试听、复合时间线和分析的音频路径。
/// @return 播放路径为 Streaming、分析为 CACHY 且回归场景通过时返回 true。
///
/// 先切换 Streaming 并用旧 loadBGM 检查原始 AudioTrack 策略，再加载和卸载试听
/// 轨。随后在同一管理器与同一路径上运行复合播放和资源 DSP 场景，能发现缓存
/// 命中时错误沿用先前 CACHY 资源的问题。分析入口必须显式选择完整缓存，因为
/// 离线波形或 BPM 分析需要稳定随机访问。最后恢复 Cached 并复测旧包装入口。
///
/// streamedBgm 同时要求加载成功、BGMTrack 非空且 cachingStrategy 为 STREAMING。
/// audition 只检查加载接口，因为其节点在卸载后不再暴露底层 track。最终返回
/// 把播放、分析和恢复后的旧入口结果合并，任一策略泄漏都会使场景失败。
bool testStreamingSelection(MMM::Audio::AudioManager& manager,
                            const std::string&        path)
{
    // 先设置策略再触发任何加载，避免已有缓存掩盖策略选择入口的错误。
    manager.setDecodingMode(MMM::Config::AudioDecodingMode::Streaming);
    // 现有集成用例覆盖实际播放时钟、复合事件和资源级 DSP 的加载成功。
    // 在相同管理器上切换，能发现路径缓存命中时忽略新策略的问题。
    const bool streamedBgm = manager.loadBGM(path, MMM::AudioTrackConfig{}) &&
                             manager.getBGMTrack() &&
                             manager.getBGMTrack()->cachingStrategy() ==
                                 ice::CachingStrategy::STREAMING;
    // 试听只检查成功并立即卸载，避免独立 SourceNode 干扰复合播放时钟。
    const bool audition =
        manager.loadAuditionTrack(path, MMM::AudioTrackConfig{});
    manager.unloadAuditionTrack();
    const bool playback = streamedBgm && audition &&
                          testCompositePlayback(manager, path) &&
                          testSingleClipResourceProcessing(manager, path);
    // 分析职责优先稳定完整 PCM，不继承用户为播放选择的低内存流式策略。
    const auto analysis = manager.loadTrackForAnalysis(path);
    const bool cachedAnalysis =
        analysis && analysis->cachingStrategy() == ice::CachingStrategy::CACHY;
    // 释放时间线后再恢复配置，后续进程内调用不会继承本测试的流式偏好。
    manager.unloadAudioTimeline();
    manager.setDecodingMode(MMM::Config::AudioDecodingMode::Cached);
    return playback && cachedAnalysis && testLegacyBgmWrapper(manager, path);
}

}  // namespace

/// @brief 运行 AudioManager 自动采样时间线集成测试。
/// @param argc 参数数量。
/// @param argv 第一个参数为可解码短音频路径。
/// @return 全部验证通过时返回零。
///
/// 主函数把后端固定为 SDL 并清空设备名，让测试使用可用默认输出设备。应用线程
/// 池必须先于 AudioManager 初始化，资源解码和队列加载依赖其生命周期。全局
/// 速度、音高和 EQ 在场景开始前恢复中性状态，测试结束按相反顺序关闭管理器和
/// 线程池，避免后台任务访问已析构单例。
///
/// 执行顺序从最小空时钟逐步增加资源处理、控制、共享缓存和综合播放复杂度，
/// 最后才切换 Streaming。这样失败现场通常仍保留在职责最接近的首个场景中。
/// samplePath 由 CTest 传入项目短音频资产，所有场景只读该资源及构造缺失路径。
int main(int argc, char** argv)
{
    // 样本路径必须由调用者明确提供，测试不猜测工作目录或安装资源位置。
    if ( argc < 2 ) {
        XERROR("Usage: AudioManagerTimelineIntegrationTest <sample_path>");
        return 1;
    }

    // 日志在后端初始化前启动，设备或资源失败会保留完整诊断。
    XLogger::init("AudioManagerTimelineIntegrationTest");
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    settings.audioPlaybackBackend = MMM::Config::AudioPlaybackBackend::SDL;
    settings.sdlAudioOutputDeviceName.clear();

    // 管理器 init 会创建真实音频图和后端回调，线程池必须已经可用。
    MMM::Runtime::AppThreadPool::instance().init();
    auto& manager = MMM::Audio::AudioManager::instance();
    manager.init();
    manager.setPlaybackSpeed(1.0);
    manager.setPlaybackPitch(0.0);
    manager.destroyMainTrackEQ();

    const std::string samplePath = argv[1];
    // 每个场景在前一个成功后运行，保证失败日志指向首个破坏契约的职责层。
    // 场景短路执行，避免前一集成失败留下的音频图状态造成大量次生错误。
    const bool passed =
        testEmptyTimelineClock(manager) &&
        testSingleClipResourceProcessing(manager, samplePath) &&
        testKeySoundTrackMutes(manager, samplePath) &&
        testIndependentResourceAndGlobalSpeed(manager, samplePath) &&
        testDualUseEffectSharesPreparedAudio(manager, samplePath) &&
        testLegacyBgmWrapper(manager, samplePath) &&
        testCompositePlayback(manager, samplePath) &&
        testTimelineUnloadReleasesDecodedTrack(manager, samplePath) &&
        testStreamingSelection(manager, samplePath);

    // 先停止音频后端和释放图，再关闭资源线程池。
    manager.shutdown();
    MMM::Runtime::AppThreadPool::instance().shutdown();
    return passed ? 0 : 1;
}
