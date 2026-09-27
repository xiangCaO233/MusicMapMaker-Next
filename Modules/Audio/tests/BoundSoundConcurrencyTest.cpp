#include "audio/AudioTimelineMixerNode.h"
#include "audio/KeySoundControl.h"
#include "audio/SoundEffectPool.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/core/MixBus.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/manage/dec/IDecoderFactory.hpp>
#include <ice/manage/dec/IDecoderInstance.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace
{
// 本测试直接驱动 SoundEffectPool 的 MixBus，不打开系统音频设备。这样可以用固定
// block 和参考帧确定性验证多 voice 生命周期、调度帧域和实时控制快照。
//
// 覆盖的并发不变量包括：
//
// - 同一 AudioTrack 可被数十个池同时读取；
// - 每个池保存独立播放游标，不能退化成共享 SourceNode；
// - 停止中的 voice 在回调确认前不能被新播放复用；
// - 带变调 voice 必须排空 stretcher final 后才能回池；
// - 零帧资源不能创建永久占位 voice；
// - 已排定 voice 每个 block 读取最新 KeySound 控制。
//
// 测试只调用 process 推进图，因此 referenceFrame 由测试显式增加。绝对时间线
// 调度读取该参考帧；非同步变速音效则在提交时换算成相对输出延迟。两种帧域不
// 可混用，否则预览倍率不是一时会提前或延后触发。
//
// 样本级比较使用常量 PCM 与未控制 reference 池。controlled 池的期望结果由
// reference 样本乘以轨道增益和类别增益得到，避免把引擎自身淡入或声道处理
// 误判为控制错误。
//
// 每个场景验证的故障模式互不重叠：
//
// - 多池场景发现跨池共享游标或来源上限；
// - 调度计划场景发现时间线帧与输出延迟帧混用；
// - 空轨场景发现无样本 voice 泄漏；
// - stop 后播放场景发现停止代次覆盖新命令；
// - final drain 场景发现 stretcher 尾部未完成即复用；
// - 控制场景发现只在排定时快照增益而非逐 block 读取。
//
// sourceCount 用于验证池内实例数量，而不是可听 voice 数。停止命令发布到回调
// 后，旧来源仍可能暂时存在；这正是 stop 后立即 play 场景要求扩容的原因。完成
// drain 后来源仍留在 mixer 中供复用，因此复用断言要求数量保持一而非变为零。
//
// 测试固定使用 ICEConfig::internal_format，保证生成 PCM、AudioBuffer 和实际样本
// 资源处于同一声道与采样率契约。它不假设具体采样率数值，绝对帧仅用于相对
// 调度和进度差异。
//
// 所有阈值都有明确含义：40/48 允许尾部排程尚未发声，八个进度桶证明游标不
// 相同，SAMPLE_EPSILON 吸收浮点混音误差。它们不用于掩盖超时或随机竞态。
//
// ThreadPool 只承担真实样本的一次解码，不参与 voice 推进。测试开始消费 mixer
// 前已经锁定 AudioTrack 强引用，因此并发断言不会受后台加载完成时机影响。空轨
// 场景的自定义工厂同样返回立即可用实例，使零帧行为可重复。
//
// 测试没有跨线程同时调用控制 API；实时安全由 KeySoundControlTest 覆盖。这里
// 验证的是控制快照从一次控制提交到下一个音频 block 的可见性，以及已经排定的
// voice 不需要重新创建即可采用新值。
// 因此失败结果可以直接归因于池调度或实例生命周期，而不是设备后端差异。

/// @brief 显式提供空 PCM，避免测试依赖尚未实现的解码策略。
class EmptyDecoder final : public ice::IDecoderInstance
{
public:
    /// @brief 空资源允许定位但始终没有样本。
    /// @return 始终为 true，游标对零帧结果没有影响。
    bool seek(std::size_t) override { return true; }
    /// @brief 不写目标内存，以零帧表示空资源。
    /// @return 始终读取零帧。
    std::size_t read(float**, std::size_t) override { return 0; }
    /// @brief 沿用测试初始化后的内部格式。
    /// @return ICE 内部统一音频格式。
    const ice::AudioDataFormat& get_source_format() const override
    {
        return ice::ICEConfig::internal_format;
    }
    /// @brief 空 PCM 的总帧数固定为零。
    /// @return 零。
    std::size_t get_source_total_frames() const override { return 0; }
};
/// @brief 将探测成功与解码零帧分别建模，覆盖无有效声音的加载结果。
class EmptyDecoderFactory final : public ice::IDecoderFactory
{
public:
    /// @brief 填充可用元信息，解码内容仍由空实例决定。
    /// @param info 接收内部音频格式的媒体信息。
    /// @return 始终探测成功，使失败只来自零帧语义。
    bool probe(std::string_view, ice::MediaInfo& info) const override
    {
        info        = {};
        info.format = ice::ICEConfig::internal_format;
        return true;
    }
    /// @brief 返回独立空实例，无文件访问或后台共享游标。
    /// @return 新建的空解码实例。
    std::unique_ptr<ice::IDecoderInstance> create_instance(
        std::string_view, const ice::AudioDataFormat&) const override
    {
        return std::make_unique<EmptyDecoder>();
    }
};

/// @brief 从测试上下文读取当前参考帧。
/// @param context 指向测试栈上 referenceFrame 的观察指针。
/// @return 当前帧；空上下文回退为零。
/// @warning 模拟音频回调读取，只做一次无分配值读取。
std::size_t readReferenceFrame(const void* context) noexcept
{
    return context ? *static_cast<const std::size_t*>(context) : 0U;
}

/// @brief 验证数十个采样播放池可保持互相独立的播放进度。
/// @param samplePath 短音效测试资源路径。
/// @return 至少 40 个播放池同时发声且具有多个独立进度时返回 true。
///
/// 创建 48 个独立池，每个池只预热一个 voice，并按 64 帧间隔排定。主 MixBus
/// 推进到 4096 帧后，至少 40 个池应已经开始播放，量化后的进度还应形成至少
/// 八个桶，证明它们没有共享同一个播放游标。
bool testDozensOfIndependentSampleVoices(
    const std::filesystem::path& samplePath)
{
    // 真实短音效只解码一次，全部池共享不可变 AudioTrack 数据。
    ice::ThreadPool threadPool(4);
    ice::AudioPool  audioPool;
    auto track = audioPool.get_or_load(threadPool, samplePath.string()).lock();
    if ( !track || track->num_frames() == 0U ) {
        XERROR("Failed to decode bound sound concurrency test sample");
        return false;
    }

    auto mainMixer = std::make_shared<ice::MixBus>();
    std::vector<std::shared_ptr<MMM::Audio::SoundEffectPool>> voices;
    constexpr std::size_t                                     VOICE_COUNT = 48U;
    constexpr std::size_t START_FRAME_SPACING                             = 64U;
    voices.reserve(VOICE_COUNT);

    std::size_t referenceFrame = 0U;
    // 每个池独占内部 voice，但其 mixer 都作为来源汇入同一主总线。
    for ( std::size_t index = 0U; index < VOICE_COUNT; ++index ) {
        auto voice = std::make_shared<MMM::Audio::SoundEffectPool>(track);
        voice->init(1);
        mainMixer->add_source(voice->getMixer());

        const std::size_t targetFrame = index * START_FRAME_SPACING;
        // 绝对目标帧和调度帧一致，referenceFrame 回调决定何时激活。
        voice->playScheduled(1.0F,
                             targetFrame,
                             &referenceFrame,
                             &readReferenceFrame,
                             {},
                             targetFrame);
        voices.push_back(std::move(voice));
    }

    constexpr std::size_t BUFFER_FRAMES = 256U;
    ice::AudioBuffer buffer(ice::ICEConfig::internal_format, BUFFER_FRAMES);
    // 固定 block 推进所有来源，输出内容无需检查即可驱动 voice 状态机。
    while ( referenceFrame < 4096U ) {
        mainMixer->process(buffer);
        referenceFrame += BUFFER_FRAMES;
    }

    std::size_t       activeVoiceCount = 0U;
    std::set<int64_t> progressBuckets;
    // 把秒数放大后取整，仅用于区分进度，不依赖精确浮点相等。
    for ( const auto& voice : voices ) {
        const double playbackTime = voice->getLatestPlaybackTime();
        if ( playbackTime <= 0.0 ) continue;
        ++activeVoiceCount;
        progressBuckets.insert(
            static_cast<int64_t>(std::llround(playbackTime * 10000.0)));
    }

    if ( activeVoiceCount < 40U || progressBuckets.size() < 8U ) {
        // 阈值允许最晚排定的少数 voice 尚未进入有效样本区间。
        XERROR(
            "Bound sound concurrency lost independent voices: active={}, "
            "progressBuckets={}",
            activeVoiceCount,
            progressBuckets.size());
        return false;
    }
    return true;
}

/// @brief 验证 0.5x 与 2x 下同步和非同步路由使用正确帧域。
/// @return 四种调度计划均符合预期时返回 true。
///
/// 同步音效跟随主时间线，始终保留绝对 TARGET_FRAME；非同步音效绕过主拉伸器，
/// 其墙钟延迟必须用 FRAME_DELTA 除以预览速度换算到输出帧域。
bool testPreviewSpeedScheduleRouting()
{
    // 目标位于当前帧之后 480 帧，使 0.5x 与 2x 换算都得到精确整数。
    constexpr std::size_t CURRENT_FRAME = 1200U;
    constexpr std::size_t TARGET_FRAME  = 1680U;
    constexpr std::size_t FRAME_DELTA   = TARGET_FRAME - CURRENT_FRAME;

    // 同时覆盖慢放和快放，避免实现只在某一方向上碰巧整数正确。
    const auto syncedHalf = MMM::Audio::planSoundEffectSchedule(
        TARGET_FRAME, CURRENT_FRAME, 0.5, true);
    const auto syncedDouble = MMM::Audio::planSoundEffectSchedule(
        TARGET_FRAME, CURRENT_FRAME, 2.0, true);
    const auto independentHalf = MMM::Audio::planSoundEffectSchedule(
        TARGET_FRAME, CURRENT_FRAME, 0.5, false);
    const auto independentDouble = MMM::Audio::planSoundEffectSchedule(
        TARGET_FRAME, CURRENT_FRAME, 2.0, false);

    const bool valid =
        syncedHalf.mode ==
            MMM::Audio::SoundEffectScheduleMode::AbsoluteTimelineFrame &&
        syncedDouble.mode ==
            MMM::Audio::SoundEffectScheduleMode::AbsoluteTimelineFrame &&
        syncedHalf.frame == TARGET_FRAME &&
        syncedDouble.frame == TARGET_FRAME &&
        independentHalf.mode ==
            MMM::Audio::SoundEffectScheduleMode::RelativeOutputDelay &&
        independentDouble.mode ==
            MMM::Audio::SoundEffectScheduleMode::RelativeOutputDelay &&
        independentHalf.frame == FRAME_DELTA * 2U &&
        independentDouble.frame == FRAME_DELTA / 2U;
    // 计划对象同时约束 mode 和 frame，单独帧值相等不足以证明路由正确。
    if ( !valid ) {
        XERROR(
            "Preview speed SFX schedule routing mismatch: syncHalf={}, "
            "syncDouble={}, independentHalf={}, independentDouble={}",
            syncedHalf.frame,
            syncedDouble.frame,
            independentHalf.frame,
            independentDouble.frame);
    }
    return valid;
}

/// @brief 验证空帧音轨不会创建或永久占用音效 voice。
/// @param samplePath 用作测试音轨身份的资源路径。
/// @return 重复播放后池中仍无实例时返回 true。
///
/// 自定义解码器让 probe 成功但总帧数为零，区分“文件无法识别”和“有效空资源”。
/// 两种播放入口重复调用后，Mixer sourceCount 必须仍为零。
bool testZeroFrameTrackDoesNotOccupyVoice(
    const std::filesystem::path& samplePath)
{
    // 每次 create_instance 都返回独立空解码器，不依赖磁盘文件内容。
    ice::ThreadPool threadPool(2);
    auto            zeroFrameTrack =
        ice::AudioTrack::create(samplePath.string(),
                                threadPool,
                                std::make_shared<EmptyDecoderFactory>(),
                                ice::CachingStrategy::CACHY);
    if ( !zeroFrameTrack || zeroFrameTrack->num_frames() != 0U ) {
        XERROR("Failed to create explicit zero-frame track");
        return false;
    }

    MMM::Audio::SoundEffectPool pool(zeroFrameTrack);
    pool.init(2);
    for ( std::size_t iteration = 0U; iteration < 8U; ++iteration ) {
        // 交替即时和相对调度入口，覆盖二者共同的零帧快速拒绝。
        pool.play(1.0F);
        pool.playScheduledRelative(1.0F, 32U);
    }

    ice::AudioBuffer buffer(ice::ICEConfig::internal_format, 64U);
    // 额外 process 一次，确保没有延迟到首个回调才创建的占位来源。
    pool.getMixer()->process(buffer);
    const bool valid =
        pool.getMixer()->sourceCount() == 0U && !pool.isPlaying();
    if ( !valid ) {
        XERROR("Zero-frame SFX allocated persistent voices: {}",
               pool.getMixer()->sourceCount());
    }
    return valid;
}

/// @brief 验证停止中的实例不会被立即复用，新播放也不会被旧停止覆盖。
/// @param track 已完整缓存的短音效。
/// @return stop 后立即播放可正常推进且使用独立实例时返回 true。
///
/// pool 初始容量为一。旧 voice 先以延迟状态排定，再 stopAll 标记停止；紧接着
/// play 必须临时扩到第二个 voice，而不是复用尚未被回调确认停止的实例。
bool testStopThenImmediatePlayKeepsNewVoice(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    MMM::Audio::SoundEffectPool pool(track);
    pool.init(1);
    pool.playScheduledRelative(1.0F, 512U);
    // stopAll 只发送控制命令，真正清理发生在下一次 mixer process。
    pool.stopAll();
    pool.play(1.0F);

    const std::size_t voicesBeforeProcess = pool.getMixer()->sourceCount();
    ice::AudioBuffer  buffer(ice::ICEConfig::internal_format, 128U);
    pool.getMixer()->process(buffer);
    const bool valid =
        voicesBeforeProcess == 2U && pool.getLatestPlaybackTime() > 0.0;
    // sourceCount=2 证明新播放没有夺取旧实例，正进度证明新实例未被旧 stop
    // 覆盖。
    if ( !valid ) {
        XERROR(
            "stop->play reused stopping voice or cancelled new playback: "
            "voices={}, playback={}",
            voicesBeforeProcess,
            pool.getLatestPlaybackTime());
    }
    return valid;
}

/// @brief 验证带变调的音效在 Source final 后完整 drain 并可复用。
/// @param track 已完整缓存的短音效。
/// @return drain 后再次播放未扩容时返回 true。
///
/// 变调会让 TimeStretcher 在源 PCM 结束后保留尾部输出。测试推进源 block 数再加
/// 256 个余量，等待 final 完整排空；随后再次播放应复用唯一 voice。
bool testFinalDrainReturnsVoiceToPool(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    constexpr std::size_t       BLOCK_FRAMES = 128U;
    MMM::Audio::SoundEffectPool pool(track);
    pool.init(1);
    pool.play(1.0F, 3.0);

    ice::AudioBuffer  buffer(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    const std::size_t sourceBlocks =
        (track->num_frames() + BLOCK_FRAMES - 1U) / BLOCK_FRAMES;
    // 余量按 block 而非 sleep 推进，使测试与机器速度和声卡无关。
    for ( std::size_t block = 0U; block < sourceBlocks + 256U; ++block ) {
        pool.getMixer()->process(buffer);
    }

    pool.play(1.0F, 3.0);
    // 若 final drain 未归还 voice，此次播放会使 sourceCount 增加到二。
    const bool valid = pool.getMixer()->sourceCount() == 1U;
    if ( !valid ) {
        XERROR("Final-drained SFX voice was not reused: {}",
               pool.getMixer()->sourceCount());
    }
    return valid;
}

/// @brief 验证已排程实例每个输出 block 重新读取轨道和类别控制。
/// @return 排程后修改增益或静音可在下一 block 生效时返回 true。
///
/// reference 与 controlled 使用同一常量 PCM。首阶段期望 0.5 轨道增益乘 1.5
/// Bound 类别增益得到 0.75；随后切换轨道静音；最后解除静音并把类别增益改为
/// 0.5，期望总倍率变为 0.25。整个过程不重新排定 voice。
bool testScheduledVoiceReadsLiveKeySoundControls()
{
    // PCM 长度覆盖全部三个控制阶段及可能的拉伸器启动延迟。
    constexpr std::size_t   BLOCK_FRAMES   = 128U;
    constexpr std::size_t   TOTAL_FRAMES   = BLOCK_FRAMES * 64U;
    constexpr std::uint32_t TRACK_INDEX    = 7U;
    constexpr float         SAMPLE_EPSILON = 2.0e-4F;

    std::vector<std::vector<float>> channels(
        ice::ICEConfig::internal_format.channels,
        std::vector<float>(TOTAL_FRAMES));
    // 每个声道使用不同常量，确保控制倍率对所有声道一致应用。
    for ( std::size_t channel = 0U; channel < channels.size(); ++channel ) {
        const float sample = 0.125F * static_cast<float>(channel + 1U);
        std::fill(channels[channel].begin(), channels[channel].end(), sample);
    }
    const auto audio = MMM::Audio::PreparedTimelineAudio::fromOwnedChannels(
        std::move(channels));
    // owned channels 保证测试期间 PCM 地址稳定且不依赖 AudioPool。
    if ( !audio ) return false;

    MMM::Audio::KeySoundControlBank controls;
    controls.setPlayerTrackGain(TRACK_INDEX, 0.5F);
    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound, 1.5F);
    // reference 不绑定控制库，作为相同 voice DSP 的未经缩放基线。
    MMM::Audio::SoundEffectPool reference(audio);
    MMM::Audio::SoundEffectPool controlled(audio, &controls);
    reference.init(1);
    controlled.init(1);

    const auto playbackControl = MMM::Audio::KeySoundPlaybackControl{
        .enabled          = true,
        .playerTrackIndex = TRACK_INDEX,
        .effectGroup      = MMM::Audio::KeySoundEffectGroup::Bound,
    };
    reference.playScheduledRelative(1.0F, 0U);
    controlled.playScheduledRelative(1.0F, 0U, {}, playbackControl);

    ice::AudioBuffer referenceBlock(ice::ICEConfig::internal_format,
                                    BLOCK_FRAMES);
    ice::AudioBuffer controlledBlock(ice::ICEConfig::internal_format,
                                     BLOCK_FRAMES);
    bool             foundAudibleBlock = false;
    // Stretcher 可能有启动延迟，循环直到首个非静音参考 block。
    for ( std::size_t block = 0U; block < 32U; ++block ) {
        reference.getMixer()->process(referenceBlock);
        controlled.getMixer()->process(controlledBlock);

        float referencePeak = 0.0F;
        for ( std::size_t channel = 0U; channel < referenceBlock.num_channels();
              ++channel ) {
            for ( std::size_t frame = 0U; frame < BLOCK_FRAMES; ++frame ) {
                referencePeak = std::max(
                    referencePeak,
                    std::abs(referenceBlock.raw_ptrs()[channel][frame]));
                const float expected =
                    referenceBlock.raw_ptrs()[channel][frame] * 0.75F;
                // 首阶段倍率为玩家轨 0.5 乘 Bound 类别 1.5。
                if ( std::abs(controlledBlock.raw_ptrs()[channel][frame] -
                              expected) > SAMPLE_EPSILON ) {
                    XERROR("Scheduled Key sound gain did not match reference");
                    return false;
                }
            }
        }
        if ( referencePeak > SAMPLE_EPSILON ) {
            // 首个可听 block 已完成逐样本比较，不需继续消耗测试 PCM。
            foundAudibleBlock = true;
            break;
        }
    }
    if ( !foundAudibleBlock ) {
        // 没有基线信号时后续全零无法证明静音控制确实生效。
        XERROR("Scheduled Key sound test never produced an audible block");
        return false;
    }

    controls.setPlayerTrackMuted(TRACK_INDEX, true);
    // 控制变更发生在两次 process 之间，下一 block 必须立即全静音。
    reference.getMixer()->process(referenceBlock);
    controlled.getMixer()->process(controlledBlock);
    for ( std::size_t channel = 0U; channel < controlledBlock.num_channels();
          ++channel ) {
        // 静音使用精确零约束，避免极小泄漏在大量 voice 叠加后可听。
        for ( std::size_t frame = 0U; frame < BLOCK_FRAMES; ++frame ) {
            if ( controlledBlock.raw_ptrs()[channel][frame] != 0.0F ) {
                XERROR("Scheduled Key sound mute leaked into the next block");
                return false;
            }
        }
    }

    controls.setPlayerTrackMuted(TRACK_INDEX, false);
    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound, 0.5F);
    // 同一排定 voice 恢复后读取新快照，总倍率变为 0.5 乘 0.5。
    reference.getMixer()->process(referenceBlock);
    controlled.getMixer()->process(controlledBlock);
    for ( std::size_t channel = 0U; channel < controlledBlock.num_channels();
          ++channel ) {
        // 恢复阶段再次逐样本比较，证明 voice 未因静音而停止或重建。
        for ( std::size_t frame = 0U; frame < BLOCK_FRAMES; ++frame ) {
            const float expected =
                referenceBlock.raw_ptrs()[channel][frame] * 0.25F;
            if ( std::abs(controlledBlock.raw_ptrs()[channel][frame] -
                          expected) > SAMPLE_EPSILON ) {
                XERROR("Scheduled Key sound did not resume with live gain");
                return false;
            }
        }
    }
    return true;
}
}  // namespace

/// @brief 运行绑定采样多声部并发测试。
/// @param argc 参数数量。
/// @param argv 第一个参数为短音效资源路径。
/// @return 测试通过时返回 0。
///
/// 主函数先解码一次共享短样本，供涉及真实 PCM 生命周期的场景使用；其余场景
/// 自行建立隔离图。短路执行保留首个失败职责的日志，避免次生状态掩盖根因。
int main(int argc, char** argv)
{
    // 路径由 CMake 测试定义传入，不依赖运行目录推断资产位置。
    if ( argc < 2 ) {
        XERROR("Usage: BoundSoundConcurrencyTest <sample_path>");
        return 1;
    }
    const std::filesystem::path samplePath(argv[1]);
    ice::ThreadPool             threadPool(2);
    ice::AudioPool              audioPool;
    auto track = audioPool.get_or_load(threadPool, samplePath.string()).lock();
    // 主共享轨必须完整缓存且非空，后续 voice 生命周期断言才有意义。
    if ( !track || track->num_frames() == 0U ) {
        XERROR("Failed to decode SFX lifecycle test sample");
        return 1;
    }

    const bool passed = testDozensOfIndependentSampleVoices(samplePath) &&
                        testPreviewSpeedScheduleRouting() &&
                        testZeroFrameTrackDoesNotOccupyVoice(samplePath) &&
                        testStopThenImmediatePlayKeepsNewVoice(track) &&
                        testFinalDrainReturnsVoiceToPool(track) &&
                        testScheduledVoiceReadsLiveKeySoundControls();
    // 各测试对象均为局部值，返回前会先销毁池再销毁共享 AudioPool。
    return passed ? 0 : 1;
}
