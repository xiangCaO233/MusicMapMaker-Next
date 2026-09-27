#include "audio/AudioManager.h"
#include "BackgroundSpectrumAnalyzer.h"
#include "audio/AudioTimelineMixerNode.h"
#include "audio/KeySoundControl.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ice/config/config.hpp>
#include <ice/core/MixBus.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/out/IReceiver.hpp>
#include <ice/out/play/openal/ALPlayer.hpp>
#include <ice/out/play/sdl/SDLPlayer.hpp>

namespace MMM::Audio
{
namespace
{
// 本文件负责 AudioManager 的进程级生命周期和输出后端管理。业务职责按以下
// 边界拆到其他实现文件，避免后端重建逻辑与高频播放控制互相穿插：
//
// - AudioManager_Playback.cpp：播放、暂停、定位和循环；
// - AudioManager_BGM.cpp：复合时间线与 BGM 资源加载；
// - AudioManager_SFX.cpp：交互音效和绑定音效池；
// - AudioManager_EQ.cpp：主轨均衡器；
// - AudioManager_Mix.cpp：分层音量和静音控制。
//
// 主音频图的信号顺序为时间线与同步 HitEffect、TimeStretcher、主混音器和输出
// 后端。不同步变速的 HitEffect 直接进入主混音器。频谱捕获节点只旁路观察
// PCM，不拥有播放时钟，也不能改变上游调度。
//
// 后端或设备切换属于低频设置操作。切换时先销毁旧接收器，再尝试新配置；若
// 失败则恢复旧配置。配置文件只在新后端已成功启动后更新，防止下次启动重复
// 使用本次无法打开的选择。
//
// 生命周期所有权遵循以下约束：
//
// - AppThreadPool 由 Runtime 模块所有；
// - AudioManager 只保存线程池观察指针；
// - AudioPool 由 AudioManager 独占；
// - 各音频节点由图中的 shared_ptr 边共同保活；
// - m_player 独占系统输出接收器；
// - m_openALPlayer 只观察 m_player 内的具体对象。
//
// 后端发布遵循以下不变量：
//
// - m_player 非空表示设备已经 open 且回调已经 start；
// - m_playbackBackend 描述 m_player 的实际类型；
// - m_openALPlayer 仅在 OpenAL 接收器存活时非空；
// - 失败的局部 nextPlayer 从不写入成员；
// - 每次成功 init_backend 最终都有对应 quit_backend。
//
// 设备名称使用空字符串统一表示系统默认项。SDL 的数值设备 ID 仅在一次枚举和
// open 之间使用，避免把后端临时 ID 写入配置。OpenAL 名称也在每次打开时交给
// 后端解析，因此拔插设备后可以根据 allowDefaultDeviceFallback 明确降级。
//
// 错误处理不使用异常。后端初始化、设备打开和线程启动都以返回值形成明确提交
// 点；提交前的资源留在局部所有权中，失败路径负责 close 与 quit。恢复旧选择
// 失败时保留空播放器状态并输出诊断，让上层界面可以继续接受下一次设置，而不
// 暴露类型与实际后端不一致的半成品对象。
//
// 本文件中的 sleep 仅用于 OpenAL 设备打开失败后的有界低频重试。它不位于
// 播放、渲染或逻辑更新链，也不用于等待音频状态同步；正常回调推进完全由后端
// 自身线程负责。

/// @brief 获取音频播放后端的日志名称。
/// @param backend 音频播放后端类型。
/// @return 用于日志输出的后端名称。
///
/// 该文本只用于诊断，不参与配置序列化或后端选择；未知枚举保留可读兜底值。
const char* getPlaybackBackendName(Config::AudioPlaybackBackend backend)
{
    switch ( backend ) {
    case Config::AudioPlaybackBackend::SDL: return "SDL";
    case Config::AudioPlaybackBackend::OpenAL: return "OpenAL";
    default: return "Unknown";
    }
}

/// @brief 获取日志中显示的输出设备名称。
/// @param deviceName 配置中的设备名称。
/// @return 用于日志输出的设备名称。
///
/// 仅转换空字符串的展示形式，不改变实际传给后端的设备选择值。
std::string outputDeviceNameForLog(const std::string& deviceName)
{
    return deviceName.empty() ? std::string("Default") : deviceName;
}

/// @brief 枚举指定播放后端的输出设备。
/// @param backend 目标播放后端。
/// @return 输出设备列表，第一项为默认设备。
///
/// 后端返回的空名称会被过滤，防止它与应用预置的默认项重复。列表保持后端原始
/// 顺序，不在此层排序或去重，以免设备设置界面和驱动优先级不一致。
/// @warning 低频设置查询，可能触发系统设备枚举。
std::vector<AudioOutputDevice> listOutputDevicesForBackend(
    Config::AudioPlaybackBackend backend)
{
    // 空名称是两个后端共同认可的默认设备标识，始终放在列表首项。
    std::vector<AudioOutputDevice> devices;
    devices.push_back(AudioOutputDevice{ "", true });

    switch ( backend ) {
    case Config::AudioPlaybackBackend::SDL:
        // SDL 提供稳定数值 ID，但持久化时使用名称以跨进程重新枚举。
        for ( const auto& device : ice::SDLPlayer::list_devices() ) {
            if ( !device.name.empty() ) {
                devices.push_back(AudioOutputDevice{ device.name, false });
            }
        }
        break;
    case Config::AudioPlaybackBackend::OpenAL:
        // OpenAL 直接按设备名称打开，不需要保留后端内部枚举索引。
        for ( const auto& device : ice::ALPlayer::list_devices() ) {
            if ( !device.name.empty() ) {
                devices.push_back(AudioOutputDevice{ device.name, false });
            }
        }
        break;
    default: break;
    }

    return devices;
}

/// @brief 判断设备列表中是否包含指定设备名称。
/// @param devices 已枚举的设备列表。
/// @param deviceName 设备名称。
/// @return 找到时返回 true。
///
/// 使用精确名称比较；大小写和别名解释交由后端，应用不猜测平台规则。
bool containsOutputDeviceName(const std::vector<AudioOutputDevice>& devices,
                              const std::string&                    deviceName)
{
    return std::any_of(devices.begin(), devices.end(), [&](const auto& device) {
        return device.name == deviceName;
    });
}

/// @brief 读取主时间线最近发布的 discontinuity 代际。
/// @param context 生命周期覆盖音频后端的 AudioTimelineMixerNode。
/// @return 当前时间线代际。
/// @warning 音频回调热路径：只执行一次 relaxed 原子读取。
///
/// context 为空时返回零代次，允许 TimeStretcher 在图拆卸边界安全停止查询。
std::uint64_t readTimelineEpoch(const void* context) noexcept
{
    if ( !context ) return 0U;
    return static_cast<const AudioTimelineMixerNode*>(context)->epoch();
}

/// @brief 在变速器拉取上游前取得不跨循环或自然结束点的连续输入区间。
/// @param context 生命周期覆盖音频后端的 AudioTimelineMixerNode。
/// @param maximumInputFrames 本段最多允许拉取的输入帧数。
/// @return 连续输入帧数及段尾动作。
/// @warning 音频回调热路径：只推进时间线控制邮箱，不分配、不阻塞。
///
/// 返回值把项目时间线边界映射到 ICE 协议；frameCount 原样保留，不能在本层
/// 合并相邻区间，否则拉伸器可能跨过 Seek 或循环点消费旧采样。
ice::TimeStretcher::InputSpan readTimelineInputBoundary(
    void* context, std::size_t maximumInputFrames) noexcept
{
    if ( !context ) return {};

    // MixerNode 使用项目枚举描述边界，这里只做无状态协议映射。
    const auto boundary =
        static_cast<AudioTimelineMixerNode*>(context)->prepareInputBoundary(
            maximumInputFrames);
    auto kind = ice::TimeStretcher::InputBoundary::None;
    switch ( boundary.kind ) {
    case AudioTimelineInputBoundaryKind::None: break;
    case AudioTimelineInputBoundaryKind::Discontinuity:
        kind = ice::TimeStretcher::InputBoundary::Discontinuity;
        break;
    case AudioTimelineInputBoundaryKind::Final:
        kind = ice::TimeStretcher::InputBoundary::Final;
        break;
    }
    return {
        .frameCount = boundary.frameCount,
        .boundary   = kind,
    };
}

/// @brief 在上游自然结束的同一 block 通知拉伸器提交 final 输入。
/// @param context 生命周期覆盖音频后端的 TimeStretcher。
/// @warning 音频回调热路径：只写入 lock-free 代际邮箱。
///
/// 空上下文用于图正在拆卸的防御场景；正常运行时指针始终指向 m_stretcher。
void requestFinalStretcherInput(void* context) noexcept
{
    if ( !context ) return;
    static_cast<void>(
        static_cast<ice::TimeStretcher*>(context)->request_final_input());
}

/// @brief 记录后端可见的 OpenAL 播放设备。
///
/// 仅在最终打开失败时调用，集中输出一条设备清单，避免正常启动日志噪声。
void logOpenALDeviceDiagnostics()
{
    // 失败诊断重新枚举一次，记录驱动实际可见的设备而非应用缓存列表。
    const auto devices = ice::ALPlayer::list_devices();
    if ( devices.empty() ) {
        XERROR("OpenAL reported no playback devices.");
        return;
    }

    std::string deviceNames;
    for ( const auto& device : devices ) {
        // 分隔符只在已有内容后追加，日志不会出现空的首段或尾段。
        if ( !deviceNames.empty() ) {
            deviceNames += " | ";
        }
        deviceNames += device.name;
    }
    XINFO("OpenAL reported playback devices: {}", deviceNames);
}
}  // namespace

/// @brief 获取音频管理器全局实例。
/// @return 音频管理器全局实例引用。
///
/// 函数局部静态保证线程安全构造；调用方仍须由应用生命周期串行调用 init 和
/// shutdown，单例本身不为重复初始化提供并发协调。
AudioManager& AudioManager::instance()
{
    static AudioManager inst;
    return inst;
}

/// @brief 构造音频管理器并从编辑器配置初始化音量状态。
///
/// 构造阶段只复制持久配置和创建轻量控制库，不接触系统音频设备。真正的引擎
/// 对象由 init 创建，以便应用线程池和日志系统先完成初始化。全局、BGM、SFX
/// 与交互 SFX 四层增益分别保留，后续混音 getter 在读取时组合它们。
///
/// HitEffect 配置同时初始化玩家区域、绑定组和非绑定组。永久音效的逐项静音
/// 只恢复配置中明确保存的 key，尚未注册的池会在创建时读取这份映射。
AudioManager::AudioManager()
    : m_keySoundControls(std::make_unique<KeySoundControlBank>())
{
    // 只持有配置单例的短期引用，AudioManager 内部保存独立运行时副本。
    auto& settings       = Config::AppConfig::instance().getEditorSettings();
    m_globalVolume       = settings.globalVolume;
    m_globalMuted        = settings.globalMuted;
    m_bgmGain            = settings.bgmGain;
    m_bgmGainMuted       = settings.bgmGainMuted;
    m_sfxGain            = settings.sfxGain;
    m_sfxGainMuted       = settings.sfxGainMuted;
    m_interactionSfxGain = settings.interactionSfxGain;
    m_interactionSfxGainMuted = settings.interactionSfxGainMuted;
    m_mainTrackVolume         = 1.0f;
    // 解码模式会映射为 ICE 缓存策略，必须在任何资源加载前初始化。
    setDecodingMode(settings.audioDecodingMode);
    m_playbackBackend        = settings.audioPlaybackBackend;
    m_sdlOutputDeviceName    = settings.sdlAudioOutputDeviceName;
    m_openALOutputDeviceName = settings.openALAudioOutputDeviceName;
    m_openALSpatialConfig    = settings.openALSpatialConfig;
    m_keySoundControls->setPlayerAreaMuted(!settings.sfxConfig.enableHitSfx);
    // UI 使用“启用”语义，实时控制库使用“静音”语义，因此此处取反。
    m_keySoundControls->setEffectGroupMuted(
        KeySoundEffectGroup::Unbound, !settings.sfxConfig.enableUnboundHitSfx);
    m_keySoundControls->setEffectGroupGain(
        KeySoundEffectGroup::Unbound, settings.sfxConfig.unboundHitSfxGain);
    m_keySoundControls->setEffectGroupMuted(
        KeySoundEffectGroup::Bound, !settings.sfxConfig.enableBoundHitSfx);
    m_keySoundControls->setEffectGroupGain(KeySoundEffectGroup::Bound,
                                           settings.sfxConfig.boundHitSfxGain);

    // 池可能稍后才注册，先保存 key 到静音状态的持久映射。
    for ( const auto& [key, muted] : settings.sfxConfig.permanentSfxMutes ) {
        m_sfxMutes[key] = muted;
    }
}

/// @brief 销毁音频管理器。
AudioManager::~AudioManager() = default;

/// @brief 初始化音频后端、线程池、音频池、播放器和主混音图。
///
/// 初始化顺序刻意从资源设施到音频图、最后到接收器。播放器 start 后可能立即
/// 在后端线程拉取 m_mainMixer，因此所有上游节点、prepare 容量和回调上下文
/// 必须在 createPlaybackBackend 前稳定存在。
///
/// @warning 低频生命周期入口，会分配多个音频节点、枚举并打开系统设备；调用方
/// 必须在应用启动阶段执行，禁止从 UI、逻辑或音频每帧路径重复调用。
void AudioManager::init()
{
    XINFO("Initializing AudioManager...");

    m_threadPool = MMM::Runtime::AppThreadPool::instance().get();
    // 缺少线程池时保留明确日志；同步加载仍可工作，但异步音效队列不可用。
    if ( !m_threadPool ) {
        XERROR("AppThreadPool is not initialized before AudioManager::init.");
    }
    m_audioPool = std::make_unique<ice::AudioPool>();

    m_mainMixer         = std::make_shared<ice::MixBus>();
    m_preStretcherMixer = std::make_shared<ice::MixBus>();
    m_hitEffectMixer    = std::make_shared<ice::MixBus>();
    const std::size_t maximumBlockFrames =
        std::max<std::size_t>(ice::ICEConfig::default_buffer_size, 1U);
    // 三个 MixBus 都预留同一最大 block，回调期只复用内部缓冲而不扩容。
    m_mainMixer->prepare(ice::ICEConfig::internal_format, maximumBlockFrames);
    m_preStretcherMixer->prepare(ice::ICEConfig::internal_format,
                                 maximumBlockFrames);
    m_hitEffectMixer->prepare(ice::ICEConfig::internal_format,
                              maximumBlockFrames);
    m_audioTimelineNode = std::make_shared<AudioTimelineMixerNode>(
        std::vector<PreparedTimelineClip>{},
        0,
        maximumBlockFrames,
        m_keySoundControls.get());
    // 时间线节点持有调度时钟，频谱捕获只包装它并转发 PCM。
    m_bgmSpectrumCapture =
        std::make_shared<BackgroundSpectrumCaptureNode>(m_audioTimelineNode);
    m_stretcher = std::make_shared<ice::TimeStretcher>();
    // 变速器唯一上游是预拉伸混音器，资源级速度已在加载阶段离线处理。
    m_stretcher->set_inputnode(m_preStretcherMixer);
    static_cast<void>(m_stretcher->prepare(ice::ICEConfig::internal_format,
                                           maximumBlockFrames));
    m_stretcher->set_playback_ratio(m_speed);
    m_stretcher->set_pitch_semitones(m_playbackPitch);
    m_stretcher->set_discontinuity_generation_provider(
        m_audioTimelineNode.get(), &readTimelineEpoch);
    // epoch 与输入边界回调让拉伸器在 Seek、循环和自然结束时清空旧缓存。
    m_stretcher->set_input_boundary_provider(m_audioTimelineNode.get(),
                                             &readTimelineInputBoundary);
    m_audioTimelineNode->setFinalInputListener(m_stretcher.get(),
                                               &requestFinalStretcherInput);
    // 主时间线先进入频谱捕获，再加入预拉伸总线。
    m_preStretcherMixer->add_source(m_bgmSpectrumCapture);
    m_mainMixer->add_source(m_stretcher);
    m_hitEffectSpectrumCapture =
        std::make_shared<BackgroundSpectrumCaptureNode>(m_hitEffectMixer);
    m_backgroundSpectrumAnalyzer =
        std::make_unique<BackgroundSpectrumAnalyzer>();
    // HitEffect 是否随谱面变速决定它接入拉伸器之前还是之后。
    const bool syncHitEffects = Config::AppConfig::instance()
                                    .getEditorSettings()
                                    .sfxConfig.hitSfxSyncSpeed;
    if ( syncHitEffects ) {
        m_preStretcherMixer->add_source(m_hitEffectSpectrumCapture);
    } else {
        m_mainMixer->add_source(m_hitEffectSpectrumCapture);
    }

    if ( createPlaybackBackend(m_playbackBackend) ) {
        XINFO("Audio playback backend opened with configured backend: {}.",
              getPlaybackBackendName(m_playbackBackend));
    } else {
        // 配置后端不可用时只回退本次运行；持久设置仍保留供设备恢复后使用。
        XERROR(
            "Failed to open configured audio backend: {}, falling back to "
            "SDL.",
            getPlaybackBackendName(m_playbackBackend));
        if ( createPlaybackBackend(Config::AudioPlaybackBackend::SDL) ) {
            XINFO("Audio playback backend opened with fallback backend: SDL.");
        } else {
            XERROR("Failed to open fallback audio backend: SDL.");
            m_playbackBackend = Config::AudioPlaybackBackend::SDL;
        }
    }
    XINFO("AudioManager initialized.");
}

/// @brief 关闭播放器并释放所有音频引擎资源。
///
/// 首先等待后台音效加载并卸载业务资源，然后停止接收器。播放器停止后音频回调
/// 不再读取节点，才可以按从上游业务状态到下游图节点的顺序释放所有权。
///
/// @warning 低频生命周期入口，可能等待已提交线程池任务并关闭系统音频设备；
/// 禁止在实时回调或每帧更新中调用。
void AudioManager::shutdown()
{
    XINFO("Shutting down AudioManager...");
    waitForQueuedSoundEffectLoads();
    // 队列任务可能捕获 AudioPool；必须在释放池之前全部完成。
    unloadAudioTimeline();
    destroyPlaybackBackend();
    clearSoundEffects();
    unloadAuditionTrack();

    m_bgmTrack.reset();
    // 兼容 BGM 观察状态与新复合时间线状态一起清空。
    m_bgmPath.clear();
    m_bgmSyncKey.clear();
    m_audioTimelineNode.reset();
    m_audioTimelineLoaded = false;
    m_audioTimelineFingerprint.clear();
    m_audioTimelineClipCount        = 0U;
    m_missingAudioTimelineClipCount = 0U;
    m_audioTimelineResourceCache.clear();
    // 先断开时间线资源，再释放频谱包装、拉伸器和各级总线。
    m_bgmSpectrumCapture.reset();
    m_stretcher.reset();
    m_mainEQ.reset();
    m_mainEQPreset = EQPreset::None;
    m_backgroundSpectrumAnalyzer.reset();
    m_hitEffectSpectrumCapture.reset();
    m_hitEffectMixer.reset();
    m_mainMixer.reset();
    m_preStretcherMixer.reset();
    m_player.reset();
    m_audioPool.reset();
    // threadPool 由应用所有，AudioManager 只清除观察指针而不负责 shutdown。
    m_threadPool = nullptr;
    XINFO("AudioManager shutdown.");
}

/// @brief 切换音频播放后端。
/// @param backend 目标播放后端。
/// @return 切换成功时返回 true。
///
/// 同一后端且播放器有效时直接成功。真实切换会先停止旧后端，再创建新后端；
/// 新后端失败时尽力恢复 previousBackend，并向调用者返回 false。只有新选择成功
/// 才写入 AppConfig。
///
/// @warning 低频设置入口，会中断当前设备输出并重建后端。
bool AudioManager::setPlaybackBackend(Config::AudioPlaybackBackend backend)
{
    // 既比较枚举也要求 player 有效，允许相同后端修复已丢失的接收器。
    if ( backend == m_playbackBackend && m_player ) {
        return true;
    }

    const auto previousBackend = m_playbackBackend;
    // 两种后端库拥有独立全局 init/quit 生命周期，不能同时保留旧接收器。
    destroyPlaybackBackend();

    if ( !createPlaybackBackend(backend) ) {
        XERROR(
            "Failed to switch audio backend from {} to {}, trying to restore "
            "previous backend.",
            getPlaybackBackendName(previousBackend),
            getPlaybackBackendName(backend));
        if ( !createPlaybackBackend(previousBackend) ) {
            XERROR("Failed to restore previous audio backend: {}.",
                   getPlaybackBackendName(previousBackend));
        } else {
            XINFO("Previous audio backend restored: {}.",
                  getPlaybackBackendName(previousBackend));
        }
        // 即使恢复成功，本次用户请求仍未完成，返回 false 保留失败语义。
        return false;
    }

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    // 成功启动后再持久化，保证保存值至少在当前环境验证可用。
    settings.audioPlaybackBackend = backend;
    Config::AppConfig::instance().save();
    XINFO("Audio playback backend switched from {} to {}.",
          getPlaybackBackendName(previousBackend),
          getPlaybackBackendName(backend));
    return true;
}

/// @brief 获取当前正在使用的音频播放后端。
/// @return 当前播放后端。
///
/// 切换失败并恢复旧后端后仍返回旧值；没有播放器时返回最后一次尝试后的成员值。
Config::AudioPlaybackBackend AudioManager::getPlaybackBackend() const
{
    return m_playbackBackend;
}

/// @brief 枚举当前播放后端可用的输出设备。
/// @return 输出设备列表，第一项始终为系统默认设备。
/// @warning 低频设置查询，可能调用系统后端枚举；禁止在音频回调中使用。
std::vector<AudioOutputDevice> AudioManager::listOutputDevices() const
{
    return listOutputDevicesForBackend(m_playbackBackend);
}

/// @brief 设置当前播放后端使用的输出设备。
/// @param deviceName 设备名称；空字符串表示系统默认设备。
/// @return 成功切换并持久化时返回 true。
///
/// 非空名称必须存在于当前重新枚举的设备列表。打开新设备失败时恢复成员配置并
/// 尝试重建旧设备；恢复结果只影响运行可用性，本次调用仍返回 false。
///
/// @warning 低频设置入口，会停止当前输出并重新打开系统设备。
bool AudioManager::setOutputDeviceName(const std::string& deviceName)
{
    const auto  backend            = m_playbackBackend;
    const auto& previousDeviceName = getConfiguredOutputDeviceName(backend);
    if ( deviceName == previousDeviceName && m_player ) {
        return true;
    }

    const auto devices = listOutputDevicesForBackend(backend);
    // 默认设备由空字符串表达，不要求枚举结果包含具体默认设备名称。
    if ( !deviceName.empty() &&
         !containsOutputDeviceName(devices, deviceName) ) {
        XERROR("Configured audio output device is not available: {}.",
               deviceName);
        return false;
    }

    const std::string previousDeviceNameCopy = previousDeviceName;
    // previousDeviceName 引用指向成员，修改前必须复制以供失败恢复和日志使用。
    setConfiguredOutputDeviceName(backend, deviceName);
    destroyPlaybackBackend();

    if ( !createPlaybackBackend(backend, false) ) {
        XERROR(
            "Failed to switch audio output device from {} to {}, trying to "
            "restore previous device.",
            outputDeviceNameForLog(previousDeviceNameCopy),
            outputDeviceNameForLog(deviceName));
        setConfiguredOutputDeviceName(backend, previousDeviceNameCopy);
        // 恢复阶段允许后端自己的默认回退，优先让应用重新获得声音输出。
        if ( !createPlaybackBackend(backend) ) {
            XERROR("Failed to restore previous audio output device: {}.",
                   outputDeviceNameForLog(previousDeviceNameCopy));
        }
        return false;
    }

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    // 同步保存两个后端的独立选择，切换后端时仍能恢复各自上次设备。
    settings.sdlAudioOutputDeviceName    = m_sdlOutputDeviceName;
    settings.openALAudioOutputDeviceName = m_openALOutputDeviceName;
    Config::AppConfig::instance().save();
    XINFO("Audio output device switched from {} to {}.",
          outputDeviceNameForLog(previousDeviceNameCopy),
          outputDeviceNameForLog(deviceName));
    return true;
}

/// @brief 获取当前播放后端配置的输出设备名称。
/// @return 设备名称；空字符串表示系统默认设备。
///
/// 返回内部配置引用，调用方不得跨后端切换或设备更新长期保存该引用。
const std::string& AudioManager::getOutputDeviceName() const
{
    return getConfiguredOutputDeviceName(m_playbackBackend);
}

/// @brief 设置 OpenAL 后端空间化输出参数。
/// @param config OpenAL 空间化配置。
/// @return 当前后端为 OpenAL 并成功应用时返回 true。
///
/// 配置总是先写入内存并持久化；当前不是 OpenAL 时返回 false 只表示没有即时
/// 应用，后续创建 OpenAL 后端仍会使用这份缓存值。
bool AudioManager::setOpenALSpatialConfig(
    const Config::OpenALSpatialConfig& config)
{
    m_openALSpatialConfig = config;

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    settings.openALSpatialConfig = config;
    Config::AppConfig::instance().save();

    return applyOpenALSpatialConfig();
}

/// @brief 获取当前 OpenAL 空间化输出配置。
/// @return OpenAL 空间化配置。
///
/// 返回持久配置的运行时副本，与当前是否启用 OpenAL 后端无关。
const Config::OpenALSpatialConfig& AudioManager::getOpenALSpatialConfig() const
{
    return m_openALSpatialConfig;
}

/// @brief 创建并启动指定播放后端。
/// @param backend 目标播放后端。
/// @param allowDefaultDeviceFallback 指定设备打开失败时是否回退到默认设备。
/// @return 成功创建并启动时返回 true。
/// @warning 低频后端切换路径；OpenAL 打开失败时会短暂 sleep 后重试，
/// 禁止在每帧或音频热路径中调用。
///
/// 本函数执行五个阶段：验证主混音器、初始化后端库、构造接收器、打开设备、
/// 启动回调。任一阶段失败都会关闭已创建对象并配对调用后端 quit。只有 start
/// 成功后才把局部 nextPlayer 移入成员，使其他线程不会看到半初始化接收器。
///
/// SDL 持久化设备名称但打开时需重新映射成当前枚举 ID；OpenAL 可直接按名称
/// 打开。allowDefaultDeviceFallback 只影响选中设备失败后的退路，不会改写用户
/// 保存的名称。
bool AudioManager::createPlaybackBackend(Config::AudioPlaybackBackend backend,
                                         bool allowDefaultDeviceFallback)
{
    if ( !m_mainMixer ) {
        // 没有根节点时接收器即使启动也无法安全拉取，直接拒绝构建。
        return false;
    }

    std::unique_ptr<ice::IReceiver> nextPlayer;
    // OpenAL 裸观察指针只在 nextPlayer 的具体对象生命周期内有效。
    m_openALPlayer = nullptr;

    switch ( backend ) {
    case Config::AudioPlaybackBackend::SDL:
        // 后端全局初始化必须和 destroyPlaybackBackend 中 quit 成对。
        if ( !ice::SDLPlayer::init_backend() ) {
            XERROR("Failed to initialize audio playback backend: {}.",
                   getPlaybackBackendName(backend));
            return false;
        }
        nextPlayer = std::make_unique<ice::SDLPlayer>();
        break;
    case Config::AudioPlaybackBackend::OpenAL:
        if ( !ice::ALPlayer::init_backend() ) {
            XERROR("Failed to initialize audio playback backend: {}.",
                   getPlaybackBackendName(backend));
            return false;
        }
        {
            // 在所有权移入基类指针前记录类型化观察地址，供空间化设置使用。
            auto alPlayer  = std::make_unique<ice::ALPlayer>();
            m_openALPlayer = alPlayer.get();
            nextPlayer     = std::move(alPlayer);
        }
        break;
    default:
        XERROR("Unsupported audio playback backend: {}.",
               getPlaybackBackendName(backend));
        return false;
    }

    nextPlayer->set_source(m_mainMixer);
    // 设备打开差异集中在局部闭包，外层重试与清理保持后端无关。
    auto openSelectedOutputDevice = [&]() {
        const auto& deviceName = getConfiguredOutputDeviceName(backend);
        if ( backend == Config::AudioPlaybackBackend::SDL ) {
            auto* sdlPlayer = static_cast<ice::SDLPlayer*>(nextPlayer.get());
            if ( deviceName.empty() ) {
                // 空配置直接委托 SDL 选择系统默认输出。
                return sdlPlayer->open();
            }

            const auto sdlDevices = ice::SDLPlayer::list_devices();
            auto       deviceIt   = std::find_if(
                sdlDevices.begin(), sdlDevices.end(), [&](const auto& device) {
                    return device.name == deviceName;
                });
            if ( deviceIt == sdlDevices.end() ) {
                // 名称可能因拔插或驱动更新失效，按调用方策略决定是否回退。
                XERROR("SDL audio output device not found: {}.", deviceName);
                return allowDefaultDeviceFallback ? sdlPlayer->open() : false;
            }

            if ( sdlPlayer->open(deviceIt->id) ) {
                return true;
            }

            XERROR("Failed to open SDL audio output device: {}.", deviceName);
            return allowDefaultDeviceFallback ? sdlPlayer->open() : false;
        }

        if ( backend == Config::AudioPlaybackBackend::OpenAL ) {
            if ( !m_openALPlayer ) {
                return false;
            }
            if ( deviceName.empty() ) {
                // OpenAL 默认设备同样由无参数 open 表达。
                return m_openALPlayer->open();
            }

            if ( m_openALPlayer->open(deviceName) ) {
                return true;
            }

            const auto& detail = m_openALPlayer->getLastError();
            // 后端详情先于通用错误输出，便于关联驱动返回原因。
            if ( !detail.empty() ) {
                XERROR("OpenAL backend detail: {}", detail);
            }
            XERROR("Failed to open OpenAL audio output device: {}.",
                   deviceName);
            return allowDefaultDeviceFallback ? m_openALPlayer->open() : false;
        }

        return false;
    };

    const int openAttemptCount =
        backend == Config::AudioPlaybackBackend::OpenAL ? 3 : 1;
    // OpenAL 设备在系统服务刚恢复时可能短暂不可用；SDL 失败则立即返回。
    bool backendOpened = false;
    for ( int attempt = 1; attempt <= openAttemptCount; ++attempt ) {
        if ( openSelectedOutputDevice() ) {
            backendOpened = true;
            if ( attempt > 1 ) {
                XINFO(
                    "Audio playback backend opened after retry: {} "
                    "(attempt {}/{}).",
                    getPlaybackBackendName(backend),
                    attempt,
                    openAttemptCount);
            }
            break;
        }

        if ( attempt < openAttemptCount ) {
            // 线性退避只存在于显式低频切换路径，总等待上界保持可预测。
            const auto retryDelay = std::chrono::milliseconds(100 * attempt);
            XERROR(
                "Audio playback backend open attempt {}/{} failed: {}, "
                "retrying in {} ms.",
                attempt,
                openAttemptCount,
                getPlaybackBackendName(backend),
                retryDelay.count());
            nextPlayer->close();
            // 每次重试前关闭不完整设备句柄，但保留后端库和接收器对象。
            std::this_thread::sleep_for(retryDelay);
        }
    }

    if ( !backendOpened ) {
        XERROR("Failed to open audio playback backend: {}.",
               getPlaybackBackendName(backend));
        if ( backend == Config::AudioPlaybackBackend::OpenAL &&
             m_openALPlayer ) {
            // 最终失败时补充错误详情和设备清单，区分名称问题与驱动问题。
            const auto& detail = m_openALPlayer->getLastError();
            if ( !detail.empty() ) {
                XERROR("OpenAL backend detail: {}", detail);
            }
            logOpenALDeviceDiagnostics();
        }
        nextPlayer->close();
        // 局部对象析构不足以结束后端全局状态，必须显式配对 quit。
        if ( backend == Config::AudioPlaybackBackend::SDL ) {
            ice::SDLPlayer::quit_backend();
        } else {
            ice::ALPlayer::quit_backend();
            m_openALPlayer = nullptr;
        }
        return false;
    }

    if ( backend == Config::AudioPlaybackBackend::SDL ) {
        // 成功后反查实际 ID，仅用于报告默认设备最终解析到的名称。
        auto*      sdlPlayer = static_cast<ice::SDLPlayer*>(nextPlayer.get());
        const auto deviceId  = sdlPlayer->get_current_device();
        const auto devices   = ice::SDLPlayer::list_devices();
        auto       deviceIt  = std::find_if(
            devices.begin(), devices.end(), [&](const auto& device) {
                return device.id == deviceId;
            });
        if ( deviceIt != devices.end() ) {
            XINFO("SDL playback device opened: {}", deviceIt->name);
        }
    }

    if ( backend == Config::AudioPlaybackBackend::OpenAL ) {
        if ( m_openALPlayer ) {
            const auto& openedDevice = m_openALPlayer->getOpenedDeviceName();
            if ( !openedDevice.empty() ) {
                XINFO("OpenAL playback device opened: {}", openedDevice);
            }
        }
        applyOpenALSpatialConfig();
        // 空间化参数在设备上下文创建后才能提交给 OpenAL。
    }

    if ( !nextPlayer->start() ) {
        // open 成功不代表回调线程可启动，start 失败仍需完整回滚。
        XERROR("Failed to start audio playback backend: {}.",
               getPlaybackBackendName(backend));
        if ( backend == Config::AudioPlaybackBackend::OpenAL &&
             m_openALPlayer ) {
            const auto& detail = m_openALPlayer->getLastError();
            if ( !detail.empty() ) {
                XERROR("OpenAL backend detail: {}", detail);
            }
        }
        nextPlayer->close();
        if ( backend == Config::AudioPlaybackBackend::SDL ) {
            ice::SDLPlayer::quit_backend();
        } else {
            ice::ALPlayer::quit_backend();
            m_openALPlayer = nullptr;
        }
        return false;
    }

    m_player = std::move(nextPlayer);
    // 最后发布播放器和后端枚举，表示对象现已可供其余控制入口使用。
    m_playbackBackend = backend;
    return true;
}

/// @brief 获取指定后端配置的输出设备名称。
/// @param backend 目标播放后端。
/// @return 设备名称；空字符串表示系统默认设备。
///
/// 非 OpenAL 枚举统一落到 SDL 配置，调用者须先验证 backend 的合法性。
const std::string& AudioManager::getConfiguredOutputDeviceName(
    Config::AudioPlaybackBackend backend) const
{
    if ( backend == Config::AudioPlaybackBackend::OpenAL ) {
        return m_openALOutputDeviceName;
    }
    return m_sdlOutputDeviceName;
}

/// @brief 更新指定后端配置的输出设备名称。
/// @param backend 目标播放后端。
/// @param deviceName 设备名称；空字符串表示系统默认设备。
void AudioManager::setConfiguredOutputDeviceName(
    Config::AudioPlaybackBackend backend, const std::string& deviceName)
{
    // 两个名称独立保存，当前后端切换不会覆盖另一后端的用户选择。
    if ( backend == Config::AudioPlaybackBackend::OpenAL ) {
        m_openALOutputDeviceName = deviceName;
        return;
    }
    m_sdlOutputDeviceName = deviceName;
}

/// @brief 停止并释放当前播放后端。
///
/// 先停止回调、关闭设备、销毁接收器，再结束对应后端的全局状态。OpenAL 类型化
/// 观察指针最后置空，避免关闭过程中尚需读取错误详情时提前失效。
///
/// @warning 低频生命周期操作，可能等待后端线程结束。
void AudioManager::destroyPlaybackBackend()
{
    if ( m_player ) {
        // stop 保证后续节点释放前不再有回调读取主混音器。
        m_player->stop();
        m_player->close();
        m_player.reset();
    }

    if ( m_playbackBackend == Config::AudioPlaybackBackend::OpenAL ) {
        // quit 必须与创建时成功的具体后端 init 配对。
        ice::ALPlayer::quit_backend();
    } else {
        ice::SDLPlayer::quit_backend();
    }
    m_openALPlayer = nullptr;
}

/// @brief 将缓存的 OpenAL 空间化参数应用到当前后端。
/// @return 当前后端为 OpenAL 并成功应用时返回 true。
///
/// 参数按一个逻辑批次提交到当前播放器；无 OpenAL 播放器时不清除缓存，后续
/// createPlaybackBackend 会在设备打开后再次调用本函数。
bool AudioManager::applyOpenALSpatialConfig()
{
    if ( !m_openALPlayer ) {
        return false;
    }

    m_openALPlayer->set_spatial_output_enabled(m_openALSpatialConfig.enabled);
    // 即使当前关闭空间化也同步参数，下一次启用无需重新加载配置。
    m_openALPlayer->set_spatial_parameters(
        m_openALSpatialConfig.directionX,
        m_openALSpatialConfig.directionY,
        m_openALSpatialConfig.directionZ,
        m_openALSpatialConfig.distance,
        m_openALSpatialConfig.referenceDistance,
        m_openALSpatialConfig.maxDistance,
        m_openALSpatialConfig.rolloffFactor);
    return true;
}

}  // namespace MMM::Audio
