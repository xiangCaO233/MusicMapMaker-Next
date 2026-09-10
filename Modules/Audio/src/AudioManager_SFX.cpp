#include "BackgroundSpectrumAnalyzer.h"
#include "audio/AudioManager.h"
#include "audio/AudioTimelineMixerNode.h"
#include "audio/AudioTimelineResourceProcessor.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/thread/ThreadPool.hpp>

namespace MMM::Audio
{
namespace
{
// 本文件负责音效登记、异步准备、混音路由和播放提交。SoundEffectPool 管理单个
// key 下的并发 voice；AudioManager 管理 key 到资源描述和池的映射。
//
// 音效生命周期分为三个状态集合：
//
// - m_registeredSoundEffects 保存资源描述和 revision；
// - m_pendingSoundEffectLoads 与队列保存等待准备的请求；
// - m_sfxPools 保存已经接入音频图的可播放池。
//
// revision 把同 key 的旧后台结果与新登记隔离。文件路径或 DSP 身份变化时递增
// revision；完成队列只接纳仍与当前登记匹配的结果，过期任务可自然结束但不能
// 覆盖新资源。
//
// hiteffect.* 和谱面绑定采样进入 m_hitEffectMixer。该总线整体根据用户的同步
// 变速设置接到主 TimeStretcher 之前或之后。普通 SFX 和 ui.* 始终直接进入主
// Mixer，但 ui.* 使用独立交互音量分组。
//
// 项目 Effect 的 volume、muted、速度、音高和 EQ 属于资源配置；皮肤音效则可
// 从 AppConfig 读取永久音量与静音覆盖。两者登记时显式区分，避免用户皮肤偏好
// 覆盖谱面资源定义。
//
// updateQueuedSoundEffectLoads 是逻辑线程的低频泵：非阻塞回收已完成 future、
// 尝试取出少量准备结果、验证 revision 并接池，最后把新任务补到并发上限。真正
// 的文件解码和离线 DSP 在线程池执行。
//
// 异步任务遵守以下线程边界：
//
// - 逻辑线程独占登记表、pending、queue、task 列表和音频图；
// - worker 读取请求值副本并使用线程安全的 AudioPool；
// - worker 只在短锁内向 prepared 结果队列追加；
// - 逻辑线程用 try_lock 取结果，争用时立即跳过本帧；
// - shutdown 等待全部 future 后才释放 AudioPool。
//
// m_activeSoundEffectLoadCount 统计已经提交但尚未由逻辑线程取出的任务。结果一旦
// 从 prepared 队列取出就递减，无论 revision 是否仍有效。clearSoundEffects 丢弃
// 已完成结果时也按数量扣减，避免并发槽位永久占用。
//
// 音量组合顺序为资源基础音量、单次 volumeFactor、全局音量与所属 SFX 分组。
// 池静音通过有效音量清零，不停止 voice；KeySound 的轨道和类别增益则由池内
// StereoGainNode 每个 block 读取，二者职责不能合并成登记时的一次性数值。
//
// 播放入口绝不隐式加载资源。调用方应在皮肤初始化时同步预载常用音效，或在
// 谱面扫描时为绑定资源排队；实际触发只查 m_sfxPools。这样交互和谱面播放路径
// 不会因首次出现某个 key 而执行文件系统访问或等待解码。
// 未加载或已静音的触发会安静返回，不创建占位 voice 或补记延迟任务。

/// @brief 需要跟随主音轨变速器的打击音效 key 前缀。
constexpr const char* HIT_SOUND_EFFECT_KEY_PREFIX = "hiteffect.";

/// @brief 使用独立交互音量的音效 key 前缀。
constexpr const char* INTERACTION_SOUND_EFFECT_KEY_PREFIX = "ui.";

/// @brief 判断音效是否属于谱面打击音效。
/// @param key 音效池标识。
/// @return 属于打击音效时返回 true。
bool isHitSoundEffectKey(const std::string& key)
{
    // rfind(..., 0) 是无分配前缀判断，key 本身不做大小写归一化。
    return key.rfind(HIT_SOUND_EFFECT_KEY_PREFIX, 0) == 0;
}

/// @brief 判断音效是否属于界面交互音效。
/// @param key 音效池标识。
/// @return 属于交互音效时返回 true。
bool isInteractionSoundEffectKey(const std::string& key)
{
    // ui. 命名空间由内置皮肤音效约定，决定独立音量分组。
    return key.rfind(INTERACTION_SOUND_EFFECT_KEY_PREFIX, 0) == 0;
}

/// @brief 将资源或皮肤音效基础音量规范化到线性单位范围。
/// @param volume 外部资源或配置音量。
/// @return 有限的 0 到 1 线性音量；异常值按静音处理。
float sanitizedSoundEffectVolume(float volume) noexcept
{
    // NaN 不能进入池增益，否则会污染整个输出 block。
    return std::isfinite(volume) ? std::clamp(volume, 0.0F, 1.0F) : 0.0F;
}

/// @brief 读取最近一次主时间线音频 block 的起始帧。
/// @param context 生命周期覆盖音频后端的 AudioTimelineMixerNode。
/// @return 负时间线位置按零帧返回。
/// @warning 音频回调热路径：只执行一次 relaxed 原子读取。
std::size_t readTimelineBlockStart(const void* context) noexcept
{
    if ( !context ) return 0U;
    const auto* timeline = static_cast<const AudioTimelineMixerNode*>(context);
    const auto  position = timeline->blockStartFrame();
    // 负时间线预滚无法表示为 size_t，调度参考统一钳制到零。
    return position > 0 ? static_cast<std::size_t>(position) : 0U;
}
}  // namespace

/// @brief 判断指定音效是否应接入打击音效总线。
/// @param key 音效资源标识。
/// @return 内置打击音效或谱面绑定采样返回 true。
bool AudioManager::usesHitEffectRouting(const std::string& key) const
{
    // 内置前缀无需登记即可识别；项目资源由绑定标志补充分类。
    if ( isHitSoundEffectKey(key) ) return true;
    const auto registration = m_registeredSoundEffects.find(key);
    return registration != m_registeredSoundEffects.end() &&
           registration->second.m_isBoundNoteSound;
}

/// @brief 根据音效类型获取当前有效基础音量。
/// @param key 音效池标识。
/// @return 已包含全局音量和对应总线增益的基础音量。
float AudioManager::getSFXEffectiveGain(const std::string& key) const
{
    // 全局静音是最外层覆盖，直接短路所有音效分组。
    if ( m_globalMuted ) return 0.0f;

    if ( isInteractionSoundEffectKey(key) ) {
        // 交互音效不继承普通 SFX 分组，仍共同受全局音量控制。
        return m_interactionSfxGainMuted
                   ? 0.0f
                   : m_globalVolume * m_interactionSfxGain;
    }

    return m_sfxGainMuted ? 0.0f : m_globalVolume * m_sfxGain;
}

/// @brief 设置指定音效池音量并按需保存为常驻配置。
/// @param key 音效池标识。
/// @param volume 目标音量。
/// @param isPermanent 是否写入常驻配置。
///
/// 无论池是否已加载，登记描述都会先更新；永久配置只在池存在时按既有行为写入。
/// 已加载池随后组合当前全局分组和静音，立即刷新全部 voice 的有效音量。
void AudioManager::setSFXPoolVolume(const std::string& key, float volume,
                                    bool isPermanent)
{
    volume = sanitizedSoundEffectVolume(volume);
    // 登记值供未来重新加载使用，项目资源配置保持与默认音量一致。
    auto registration = m_registeredSoundEffects.find(key);
    if ( registration != m_registeredSoundEffects.end() ) {
        registration->second.m_defaultVolume         = volume;
        registration->second.m_resourceConfig.volume = volume;
    }

    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        it->second->setVolume(volume);

        if ( isPermanent ) {
            // 皮肤常驻偏好按 key 保存，项目资源调用方不会请求该分支。
            auto& sfxCfg =
                Config::AppConfig::instance().getEditorSettings().sfxConfig;
            sfxCfg.permanentSfxVolumes[key] = volume;
            Config::AppConfig::instance().save();
        }

        // 重新组合全局、分组、池基础值与静音，不直接猜测节点当前音量。
        it->second->updateEffectiveVolume(getSFXEffectiveGain(key),
                                          getSFXPoolMute(key));
    }
}

/// @brief 设置指定音效池静音状态并按需保存为常驻配置。
/// @param key 音效池标识。
/// @param muted 是否静音。
/// @param isPermanent 是否写入常驻配置。
///
/// 静音映射独立于池生命周期保存，因此尚未加载的登记项也会在 attach 时继承。
void AudioManager::setSFXPoolMute(const std::string& key, bool muted,
                                  bool isPermanent)
{
    // 先更新运行时映射，后续 getter 和异步 attach 都能看到新值。
    m_sfxMutes[key] = muted;
    if ( auto registration = m_registeredSoundEffects.find(key);
         registration != m_registeredSoundEffects.end() ) {
        registration->second.m_resourceConfig.muted = muted;
    }

    if ( isPermanent ) {
        // 永久覆盖只写用户配置，不改变资源 DSP 缓存身份。
        auto& sfxCfg =
            Config::AppConfig::instance().getEditorSettings().sfxConfig;
        sfxCfg.permanentSfxMutes[key] = muted;
        Config::AppConfig::instance().save();
    }

    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        // 已加载池通过有效音量清零，不销毁或停止已有 voice。
        it->second->updateEffectiveVolume(getSFXEffectiveGain(key), muted);
    }
}

/// @brief 根据同步变速配置切换打击音效池路由。
/// @param syncSpeed 是否让 hiteffect.* 音效跟随主音轨变速器。
void AudioManager::updateSFXSyncSpeedRouting(bool syncSpeed)
{
    if ( !m_mainMixer || !m_preStretcherMixer || !m_hitEffectMixer ||
         !m_hitEffectSpectrumCapture ) {
        return;
    }

    for ( auto& [key, pool] : m_sfxPools ) {
        auto mixer = pool->getMixer();
        if ( !mixer ) continue;

        // 先从所有可能旧父总线移除，使重复调用也保持单一路由。
        m_mainMixer->remove_source(mixer);
        m_preStretcherMixer->remove_source(mixer);
        m_hitEffectMixer->remove_source(mixer);
        if ( usesHitEffectRouting(key) ) {
            // HitEffect 统一进入专用总线，普通和交互音效直达主总线。
            m_hitEffectMixer->add_source(mixer);
        } else {
            m_mainMixer->add_source(mixer);
        }
    }
    m_mainMixer->remove_source(m_hitEffectSpectrumCapture);
    // 频谱捕获包装 HitEffect 总线，必须与总线一起位于拉伸前或拉伸后。
    m_preStretcherMixer->remove_source(m_hitEffectSpectrumCapture);
    if ( syncSpeed ) {
        m_preStretcherMixer->add_source(m_hitEffectSpectrumCapture);
    } else {
        m_mainMixer->add_source(m_hitEffectSpectrumCapture);
    }
}

/// @brief 获取指定音效池音量。
/// @param key 音效池标识。
/// @return 音效池音量。
float AudioManager::getSFXPoolVolume(const std::string& key) const
{
    // 已加载池保存当前实际基础值，优先级最高。
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        return it->second->getVolume();
    }

    auto registration = m_registeredSoundEffects.find(key);
    if ( registration != m_registeredSoundEffects.end() &&
         registration->second.m_usesProjectResourceConfig ) {
        // 项目资源定义不能被同 key 的历史皮肤永久配置覆盖。
        return registration->second.m_defaultVolume;
    }

    const auto& sfxCfg =
        Config::AppConfig::instance().getEditorSettings().sfxConfig;
    // 未加载皮肤音效优先返回用户保存的常驻覆盖。
    if ( auto volume = sfxCfg.permanentSfxVolumes.find(key);
         volume != sfxCfg.permanentSfxVolumes.end() ) {
        return volume->second;
    }

    if ( registration != m_registeredSoundEffects.end() ) {
        // 没有覆盖时使用登记默认值，未知 key 最终回退单位音量。
        return registration->second.m_defaultVolume;
    }
    return 1.0f;
}

/// @brief 获取指定音效池是否静音。
/// @param key 音效池标识。
/// @return 静音时返回 true。
///
/// 静音映射允许在池加载前存在；未知 key 默认不静音。
bool AudioManager::getSFXPoolMute(const std::string& key) const
{
    auto it = m_sfxMutes.find(key);
    if ( it != m_sfxMutes.end() ) {
        return it->second;
    }
    return false;
}

/// @brief 获取指定音效池音频时长。
/// @param key 音效池标识。
/// @return 音频时长，单位为秒。
///
/// 时长来自已经准备的 PCM，不包含 lead-in 或排定延迟。
double AudioManager::getSFXDuration(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        return it->second->getDuration();
    }
    return 0.0;
}

/// @brief 判断指定 Note 音效池是否复用了自动采样时间线的预处理 PCM。
/// @param key 项目 Effect 资源标识。
/// @return 两条播放路径持有同一缓存对象时返回 true。
bool AudioManager::isSFXUsingSharedTimelineAudio(const std::string& key) const
{
    const auto pool         = m_sfxPools.find(key);
    const auto registration = m_registeredSoundEffects.find(key);
    if ( pool == m_sfxPools.end() ||
         registration == m_registeredSoundEffects.end() ) {
        return false;
    }

    const auto cached = m_audioTimelineResourceCache.find(
        registration->second.m_processingCacheKey);
    // 弱缓存可能已过期，必须先锁定再比较对象身份。
    if ( cached == m_audioTimelineResourceCache.end() ) return false;
    const auto preparedAudio = cached->second.preparedAudio.lock();
    return preparedAudio &&
           pool->second->usesPreparedAudio(preparedAudio.get());
}

/// @brief 获取指定音效池最近一次播放进度。
/// @param key 音效池标识。
/// @return 播放进度，单位为秒。
///
/// 仅查询最近提交 voice 的源游标，不代表池内所有并发实例的最大进度。
double AudioManager::getSFXPlaybackTime(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        return it->second->getLatestPlaybackTime();
    }
    return 0.0;
}

/// @brief 登记可按需加载的音效文件。
/// @param key 音效标识符。
/// @param filePath 音效文件绝对路径。
/// @param defaultVolume 首次加载时使用的默认音量。
/// @param leadInSeconds 文件开头到有效出声点的延迟。
/// @warning 低频资源登记路径：只更新内存描述，不访问文件系统。
///
/// 该重载面向皮肤音效，仅提供音量和 lead-in；其余资源 DSP 保持默认值。
void AudioManager::registerSoundEffect(const std::string& key,
                                       const std::string& filePath,
                                       float              defaultVolume,
                                       double             leadInSeconds)
{
    AudioTrackConfig resourceConfig;
    resourceConfig.volume = defaultVolume;
    registerSoundEffectImpl(
        key, filePath, resourceConfig, leadInSeconds, false);
}

/// @brief 登记使用项目资源完整 DSP 配置的按需音效。
/// @param key 项目 Effect 的稳定资源标识。
/// @param filePath 音效文件绝对路径。
/// @param resourceConfig 项目声明的完整资源配置。
/// @param leadInSeconds 文件开头到有效出声点的延迟。
void AudioManager::registerSoundEffect(const std::string&      key,
                                       const std::string&      filePath,
                                       const AudioTrackConfig& resourceConfig,
                                       double                  leadInSeconds)
{
    registerSoundEffectImpl(key, filePath, resourceConfig, leadInSeconds, true);
}

/// @brief 统一登记皮肤音效或项目 Effect。
/// @param key 音效稳定标识。
/// @param filePath 音效文件绝对路径。
/// @param resourceConfig 参与处理缓存身份的资源配置。
/// @param leadInSeconds 有效声音之前的非负提前量。
/// @param usesProjectResourceConfig true 时禁止皮肤永久配置覆盖资源值。
///
/// 重登记若只改变运行时音量或静音，可复用已有处理 PCM；路径、速度、音高或 EQ
/// 等处理身份变化时递增 revision、断开旧池，并按原绑定需求重新排队。
void AudioManager::registerSoundEffectImpl(
    const std::string& key, const std::string& filePath,
    const AudioTrackConfig& resourceConfig, double leadInSeconds,
    bool usesProjectResourceConfig)
{
    const float normalizedVolume =
        sanitizedSoundEffectVolume(resourceConfig.volume);
    auto       existingRegistration = m_registeredSoundEffects.find(key);
    const bool wasBoundNoteSound =
        existingRegistration != m_registeredSoundEffects.end() &&
        existingRegistration->second.m_isBoundNoteSound;
    const bool wasLoaded  = m_sfxPools.contains(key);
    const bool wasPending = m_pendingSoundEffectLoads.contains(key);
    // processingCacheKey 排除纯运行时控制，只描述会改变准备 PCM 的字段。
    const auto processingCacheKey =
        makeAudioResourceProcessingCacheKey(filePath, resourceConfig);
    const bool processingChanged =
        existingRegistration != m_registeredSoundEffects.end() &&
        existingRegistration->second.m_processingCacheKey != processingCacheKey;
    const std::uint64_t revision =
        existingRegistration == m_registeredSoundEffects.end() ||
                processingChanged
            ? m_nextSoundEffectRevision++
            : existingRegistration->second.m_revision;
    // 未改变处理身份时保留 revision，使正在完成的后台结果仍可接纳。

    if ( processingChanged && wasLoaded ) {
        // 旧池持有旧 PCM，必须先从图断开；登记和静音映射仍保留。
        detachSoundEffectPool(key);
    }

    m_registeredSoundEffects[key] = RegisteredSoundEffect{
        .m_filePath                  = filePath,
        .m_resourceConfig            = resourceConfig,
        .m_defaultVolume             = normalizedVolume,
        .m_leadInSeconds             = std::max(0.0, leadInSeconds),
        .m_isBoundNoteSound          = wasBoundNoteSound,
        .m_usesProjectResourceConfig = usesProjectResourceConfig,
        .m_processingCacheKey        = processingCacheKey,
        .m_revision                  = revision,
    };
    // 保留此前绑定标志，皮肤重载不能把已经见过的谱面采样降回普通 SFX。
    if ( processingChanged ) {
        // 旧 pending revision 失效；其 future 可完成但会在接纳阶段被丢弃。
        m_pendingSoundEffectLoads.erase(key);
    }

    if ( usesProjectResourceConfig ) {
        // 项目 Effect 严格采用资源静音，不读取用户永久皮肤偏好。
        m_sfxMutes[key] = resourceConfig.muted;
    } else {
        bool        activeMute = false;
        const auto& sfxCfg =
            Config::AppConfig::instance().getEditorSettings().sfxConfig;
        if ( const auto configuredMute = sfxCfg.permanentSfxMutes.find(key);
             configuredMute != sfxCfg.permanentSfxMutes.end() ) {
            activeMute = configuredMute->second;
        }
        // 没有显式配置时默认可听，和注册默认音量共同组成皮肤初值。
        m_sfxMutes[key] = activeMute;
    }

    auto loadedPool = m_sfxPools.find(key);
    if ( loadedPool != m_sfxPools.end() ) {
        // 处理身份不变的已加载池只需更新运行时基础音量。
        float activeVolume = normalizedVolume;
        if ( !usesProjectResourceConfig ) {
            const auto& sfxCfg =
                Config::AppConfig::instance().getEditorSettings().sfxConfig;
            if ( const auto configuredVolume =
                     sfxCfg.permanentSfxVolumes.find(key);
                 configuredVolume != sfxCfg.permanentSfxVolumes.end() ) {
                activeVolume = configuredVolume->second;
            }
            // 皮肤音效常驻音量优先于本次皮肤登记默认值。
        }
        loadedPool->second->setVolume(activeVolume);
        loadedPool->second->updateEffectiveVolume(getSFXEffectiveGain(key),
                                                  getSFXPoolMute(key));
    }
    m_sfxLeadInSeconds[key] = std::max(0.0, leadInSeconds);
    // lead-in 不进入 PCM 身份，只影响每次目标时间到调度时间的换算。

    if ( processingChanged && wasBoundNoteSound && (wasLoaded || wasPending) ) {
        // 绑定采样需要异步恢复可用性，普通按需音效等待下一次显式加载。
        static_cast<void>(queueBoundNoteSoundEffectLoad(key));
    }
}

/// @brief 使用已完成资源 DSP 的 PCM 创建音效池并接入混音图。
/// @param key 已登记的音效标识。
/// @param preparedAudio 完成资源级 DSP 的不可变 PCM。
/// @return 已存在或成功创建并接入池时返回 true。
/// @warning 低频接入路径，会分配 voice 并修改 MixBus 来源。
bool AudioManager::attachSoundEffectPool(
    const std::string&                           key,
    std::shared_ptr<const PreparedTimelineAudio> preparedAudio)
{
    if ( m_sfxPools.contains(key) ) {
        // 幂等调用不重复连接同一池，现有实例保持播放状态。
        return true;
    }
    if ( !preparedAudio || preparedAudio->numFrames() == 0U || !m_mainMixer ||
         !m_hitEffectMixer ) {
        // 空 PCM 或不完整主图不能创建可自然结束的有效 voice。
        return false;
    }

    const auto registration = m_registeredSoundEffects.find(key);
    if ( registration == m_registeredSoundEffects.end() ) {
        return false;
    }

    float activeVolume = registration->second.m_defaultVolume;
    // 项目资源使用登记值；皮肤资源允许读取用户持久覆盖。
    if ( !registration->second.m_usesProjectResourceConfig ) {
        const auto& sfxCfg =
            Config::AppConfig::instance().getEditorSettings().sfxConfig;
        if ( const auto configuredVolume = sfxCfg.permanentSfxVolumes.find(key);
             configuredVolume != sfxCfg.permanentSfxVolumes.end() ) {
            activeVolume = configuredVolume->second;
        }
    }

    auto pool = std::make_shared<SoundEffectPool>(std::move(preparedAudio),
                                                  m_keySoundControls.get());
    // 绑定采样按需增长以节省大量项目资源内存；常用皮肤音效预热八个 voice。
    pool->init(registration->second.m_isBoundNoteSound ? 1 : 8);
    pool->setVolume(activeVolume);
    pool->updateEffectiveVolume(getSFXEffectiveGain(key), getSFXPoolMute(key));

    if ( usesHitEffectRouting(key) ) {
        // HitEffect 专用总线的位置由 updateSFXSyncSpeedRouting 整体决定。
        m_hitEffectMixer->add_source(pool->getMixer());
    } else {
        m_mainMixer->add_source(pool->getMixer());
    }

    m_sfxPools[key] = std::move(pool);
    // 最后发布映射，表示池的图连接和初始有效音量均已完成。
    m_sfxLeadInSeconds[key] = registration->second.m_leadInSeconds;
    return true;
}

/// @brief 断开并释放音效池，但保留资源登记和静音状态。
/// @param key 待断开的音效标识。
/// @warning 低频资源路径，会从所有可能父总线移除池来源。
void AudioManager::detachSoundEffectPool(const std::string& key)
{
    auto pool = m_sfxPools.find(key);
    if ( pool == m_sfxPools.end() ) return;

    auto mixer = pool->second->getMixer();
    if ( mixer ) {
        // 不依赖当前路由配置，从所有候选总线移除以保证完整断开。
        if ( m_mainMixer ) m_mainMixer->remove_source(mixer);
        if ( m_preStretcherMixer ) m_preStretcherMixer->remove_source(mixer);
        if ( m_hitEffectMixer ) m_hitEffectMixer->remove_source(mixer);
    }
    m_sfxPools.erase(pool);
    // lead-in 与可播放池同步清除，登记中的原始值仍保留供再次 attach。
    m_sfxLeadInSeconds.erase(key);
}

/// @brief 确保已登记音效完成解码并接入混音器。
/// @param key 音效标识符。
/// @return 已加载或成功加载时返回 true。
/// @warning 低频显式加载路径：可能访问文件系统并等待解码，禁止在每帧
/// UI、渲染、逻辑 update 或音频回调中调用。
bool AudioManager::ensureSoundEffectLoaded(const std::string& key)
{
    if ( m_sfxPools.contains(key) ) {
        return true;
    }
    if ( !m_audioPool || !m_threadPool || !m_mainMixer || !m_hitEffectMixer ) {
        return false;
    }

    auto registration = m_registeredSoundEffects.find(key);
    if ( registration == m_registeredSoundEffects.end() ) {
        return false;
    }

    XINFO("Loading registered SFX: {} from {} (Volume: {})",
          key,
          registration->second.m_filePath,
          registration->second.m_defaultVolume);
    // 显式同步加载接管该 key，先取消 pending 标记避免重复接纳异步结果。
    m_pendingSoundEffectLoads.erase(key);
    auto trackWeak = m_audioPool->get_or_load(*m_threadPool,
                                              registration->second.m_filePath,
                                              ice::CachingStrategy::CACHY);
    auto track     = trackWeak.lock();
    // 音效需要离线 DSP 和并发随机读取，因此始终请求完整 CACHY 音轨。
    if ( !track ) {
        XERROR("Failed to load SFX track: {}", registration->second.m_filePath);
        return false;
    }

    auto preparedAudio = getOrPrepareAudioTimelineResource(
        registration->second.m_filePath,
        track,
        registration->second.m_resourceConfig);
    // 统一缓存 helper 允许与自动采样时间线共享同一处理结果。
    return attachSoundEffectPool(key, std::move(preparedAudio));
}

/// @brief 将物件绑定音效加入后台按需加载队列。
/// @param key 谱面物件绑定的项目音效资源标识。
/// @return 已加载、已排队或成功加入队列时返回 true。
/// @warning 逻辑预读热路径：仅访问内存登记表并对首次出现的资源排队，
/// 不访问文件系统或等待解码。
bool AudioManager::queueBoundNoteSoundEffectLoad(const std::string& key)
{
    if ( key.empty() ) return false;

    auto registration = m_registeredSoundEffects.find(key);
    if ( registration == m_registeredSoundEffects.end() ) {
        return false;
    }

    const bool needsReroute = !registration->second.m_isBoundNoteSound;
    // 首次作为谱面绑定采样出现后永久提升分类，后续重登记仍保留。
    registration->second.m_isBoundNoteSound = true;

    if ( auto loaded = m_sfxPools.find(key); loaded != m_sfxPools.end() ) {
        if ( needsReroute ) {
            // 已加载普通池提升为 HitEffect 时立即迁移到专用总线。
            auto mixer = loaded->second->getMixer();
            if ( mixer && m_mainMixer && m_preStretcherMixer &&
                 m_hitEffectMixer ) {
                m_mainMixer->remove_source(mixer);
                m_preStretcherMixer->remove_source(mixer);
                m_hitEffectMixer->remove_source(mixer);
                m_hitEffectMixer->add_source(mixer);
            }
        }
        return true;
    }

    if ( m_pendingSoundEffectLoads.contains(key) ) {
        // 同 revision 已在排队或执行，重复预读请求视为成功。
        return true;
    }

    m_pendingSoundEffectLoads[key] = registration->second.m_revision;
    // pending 映射是接纳令牌，队列项携带同一 revision 快照。
    m_queuedSoundEffectLoads.push_back({
        .key            = key,
        .filePath       = registration->second.m_filePath,
        .resourceConfig = registration->second.m_resourceConfig,
        .revision       = registration->second.m_revision,
    });
    return true;
}

/// @brief 推进后台音效加载任务并回收已退役的时间线调度资源。
/// @param maxPreparedPerUpdate 单次调用最多接入的音效数量。
/// @warning
/// 逻辑轮询路径：无退役状态时只执行一次无锁指针读取；时间线发生替换时，
/// 本调用可能在控制线程析构 PCM 缓存。
void AudioManager::updateQueuedSoundEffectLoads(
    std::size_t maxPreparedPerUpdate)
{
    if ( m_audioTimelineNode ) {
        // 与音效泵共享低频时机回收时间线旧快照，避免另设每帧扫描。
        static_cast<void>(m_audioTimelineNode->reclaimRetiredSchedules());
    }

    m_soundEffectLoadTasks.erase(
        std::remove_if(m_soundEffectLoadTasks.begin(),
                       m_soundEffectLoadTasks.end(),
                       [](std::future<void>& task) {
                           // wait_for(0) 只探测完成状态，不阻塞逻辑线程。
                           return task.valid() &&
                                  task.wait_for(std::chrono::seconds(0)) ==
                                      std::future_status::ready;
                       }),
        m_soundEffectLoadTasks.end());

    std::deque<PreparedSoundEffectLoad> prepared;
    {
        std::unique_lock<std::mutex> lock(m_preparedSoundEffectLoadsMutex,
                                          std::try_to_lock);
        // worker 正在提交结果时本帧直接跳过，绝不等待互斥量。
        if ( lock.owns_lock() ) {
            while ( prepared.size() < maxPreparedPerUpdate &&
                    !m_preparedSoundEffectLoads.empty() ) {
                // 单次最多接入调用方给定数量，限制图修改和析构尖峰。
                prepared.push_back(
                    std::move(m_preparedSoundEffectLoads.front()));
                m_preparedSoundEffectLoads.pop_front();
            }
        }
    }

    for ( auto& result : prepared ) {
        // 每个取出的结果对应一个曾递增的 active
        // 任务，不论是否过期都要归还计数。
        if ( m_activeSoundEffectLoadCount > 0U ) {
            --m_activeSoundEffectLoadCount;
        }

        const auto pending = m_pendingSoundEffectLoads.find(result.key);
        if ( pending == m_pendingSoundEffectLoads.end() ||
             pending->second != result.revision ) {
            // pending 已取消或 revision 改变时，后台结果不得进入当前登记。
            continue;
        }
        m_pendingSoundEffectLoads.erase(pending);
        // 先消费接纳令牌，失败结果不会在后续 update 中重复处理。

        const auto registration = m_registeredSoundEffects.find(result.key);
        if ( registration == m_registeredSoundEffects.end() ||
             registration->second.m_revision != result.revision ) {
            // 登记可能在 worker 执行期间被卸载或替换，再次验证最终身份。
            continue;
        }
        if ( !result.track || !result.preparedAudio ) {
            // 解码或 DSP 任一失败都不创建空池，保留登记供未来重试。
            XERROR("Failed to prepare bound note SFX: {}", result.key);
            continue;
        }
        XDEBUG("Prepared bound note SFX: {}", result.key);
        auto preparedAudio = getOrPrepareAudioTimelineResource(
            registration->second.m_filePath,
            result.track,
            registration->second.m_resourceConfig,
            std::move(result.preparedAudio));
        // worker 候选纳入共享弱缓存后，再在逻辑线程修改 MixBus 图。
        attachSoundEffectPool(result.key, std::move(preparedAudio));
    }

    constexpr std::size_t MAX_CONCURRENT_SOUND_EFFECT_LOADS = 4U;
    // 固定并发上限控制大项目同时解码和 DSP 的内存峰值。
    while ( m_activeSoundEffectLoadCount < MAX_CONCURRENT_SOUND_EFFECT_LOADS &&
            !m_queuedSoundEffectLoads.empty() && m_audioPool && m_threadPool ) {
        QueuedSoundEffectLoad request =
            std::move(m_queuedSoundEffectLoads.front());
        // 出队后若令牌已失效直接丢弃，不启动无意义后台任务。
        m_queuedSoundEffectLoads.pop_front();

        const auto pending      = m_pendingSoundEffectLoads.find(request.key);
        const auto registration = m_registeredSoundEffects.find(request.key);
        if ( pending == m_pendingSoundEffectLoads.end() ||
             pending->second != request.revision ||
             registration == m_registeredSoundEffects.end() ||
             registration->second.m_revision != request.revision ) {
            continue;
        }

        m_soundEffectLoadTasks.push_back(m_threadPool->enqueue(
            [this, request = std::move(request)]() mutable {
                // worker 只做解码和离线 DSP，不读取或修改音频图容器。
                auto trackWeak =
                    m_audioPool->get_or_load(*m_threadPool,
                                             request.filePath,
                                             ice::CachingStrategy::CACHY);
                auto                    track = trackWeak.lock();
                PreparedSoundEffectLoad preparedResult{
                    .key           = std::move(request.key),
                    .revision      = request.revision,
                    .track         = track,
                    .preparedAudio = prepareAudioTimelineResource(
                        track, request.resourceConfig),
                };
                // 完整结果在一次短锁内移交，逻辑线程随后验证 revision。
                std::lock_guard<std::mutex> lock(
                    m_preparedSoundEffectLoadsMutex);
                m_preparedSoundEffectLoads.push_back(std::move(preparedResult));
            }));
        ++m_activeSoundEffectLoadCount;
        // 仅成功提交 future 后增加活动数，和结果出队时的递减配对。
    }
}

/// @brief 查询指定音效是否已经完成解码并创建音效池。
/// @param key 音效标识符。
/// @return 音效池已存在时返回 true。
///
/// pending 或 worker 已完成但尚未接入都仍返回 false。
bool AudioManager::isSoundEffectLoaded(const std::string& key) const
{
    return m_sfxPools.contains(key);
}

/// @brief 预加载音效文件并接入对应混音器。
/// @param key 音效池标识。
/// @param filePath 音效文件绝对路径。
/// @param defaultVolume 默认音量。
/// @param leadInSeconds 文件开头到有效出声点的延迟，单位为秒。
/// @return 加载成功时返回 true。
bool AudioManager::preloadSoundEffect(const std::string& key,
                                      const std::string& filePath,
                                      float defaultVolume, double leadInSeconds)
{
    // 先登记统一资源描述，再复用同步确保加载入口。
    registerSoundEffect(key, filePath, defaultVolume, leadInSeconds);
    return ensureSoundEffectLoaded(key);
}

/// @brief 卸载指定音效池并断开混音路由。
/// @param key 音效池标识。
void AudioManager::unloadSoundEffect(const std::string& key)
{
    if ( m_sfxPools.contains(key) ) {
        detachSoundEffectPool(key);
        XINFO("Unloaded SFX: {}", key);
    }
    m_registeredSoundEffects.erase(key);
    // 清登记和 pending 使尚在运行的旧 revision 结果自动失效。
    m_pendingSoundEffectLoads.erase(key);
    m_sfxLeadInSeconds.erase(key);
    m_sfxMutes.erase(key);
    static_cast<void>(releaseUnusedTrackCache());
    // 池释放后及时回收不再由时间线或其他音效共享的完整解码轨。
}

/// @brief 停止并释放所有已加载音效池。
/// @warning 低频资源重载路径：皮肤热切换时调用，会清空所有 SFX pool
/// 和调度状态，禁止放入播放热路径。
void AudioManager::clearSoundEffects()
{
    // 先停止所有 voice，再逐池断开，避免图移除时仍有排定来源。
    clearAllScheduledSoundEffects();

    std::vector<std::string> keys;
    // 复制 key 后调用 unload，避免遍历期间修改 m_sfxPools 迭代器。
    keys.reserve(m_sfxPools.size());
    for ( const auto& [key, pool] : m_sfxPools ) {
        (void)pool;
        keys.push_back(key);
    }

    for ( const auto& key : keys ) {
        unloadSoundEffect(key);
    }

    m_registeredSoundEffects.clear();
    // 清除尚未开始的队列和接纳令牌，运行中 future 会按旧 revision 被丢弃。
    m_queuedSoundEffectLoads.clear();
    m_pendingSoundEffectLoads.clear();
    m_sfxLeadInSeconds.clear();
    m_sfxMutes.clear();

    std::lock_guard<std::mutex> lock(m_preparedSoundEffectLoadsMutex);
    const std::size_t discardedCount = m_preparedSoundEffectLoads.size();
    // 已完成但未接入的结果也占 active 计数，丢弃时需要同步扣除。
    m_preparedSoundEffectLoads.clear();
    m_activeSoundEffectLoadCount =
        discardedCount < m_activeSoundEffectLoadCount
            ? m_activeSoundEffectLoadCount - discardedCount
            : 0U;
}

/// @brief 等待所有后台音效文件探测任务完成。
/// @warning 仅允许在 AudioManager 关闭路径调用，会阻塞等待线程池任务。
void AudioManager::waitForQueuedSoundEffectLoads()
{
    // shutdown 才允许逐个等待，确保 worker 不再访问 AudioPool
    // 和线程池观察指针。
    for ( auto& task : m_soundEffectLoadTasks ) {
        if ( task.valid() ) {
            task.wait();
        }
    }
    m_soundEffectLoadTasks.clear();

    // future 全部结束后可安全清空结果、队列和活动计数。
    std::lock_guard<std::mutex> lock(m_preparedSoundEffectLoadsMutex);
    m_preparedSoundEffectLoads.clear();
    m_queuedSoundEffectLoads.clear();
    m_pendingSoundEffectLoads.clear();
    m_activeSoundEffectLoadCount = 0U;
}

/// @brief 立即播放指定音效。
/// @param key 音效池标识。
/// @param volumeFactor 本次播放额外音量倍率。
/// @param pitchSemitones 本次播放的音高偏移，单位为半音。
void AudioManager::playSoundEffect(const std::string& key, float volumeFactor,
                                   double pitchSemitones)
{
    // 显式池静音在提交前短路，不占用一个 voice。
    if ( getSFXPoolMute(key) ) return;

    auto it = m_sfxPools.find(key);
    // 播放热入口不隐式解码；未加载音效由预载或异步队列负责。
    if ( it == m_sfxPools.end() ) return;

    it->second->play(volumeFactor, pitchSemitones);
}

/// @brief 获取指定音效池是否正在播放。
/// @param key 音效池标识。
/// @return 正在播放时返回 true。
bool AudioManager::isSFXPlaying(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        return it->second->isPlaying();
    }
    return false;
}

/// @brief 获取指定音效池是否暂停。
/// @param key 音效池标识。
/// @return 暂停时返回 true。
bool AudioManager::isSFXPaused(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        return it->second->isPaused();
    }
    return false;
}

/// @brief 暂停指定音效池。
/// @param key 音效池标识。
void AudioManager::pauseSoundEffect(const std::string& key)
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        it->second->pause();
    }
}

/// @brief 恢复指定音效池。
/// @param key 音效池标识。
void AudioManager::resumeSoundEffect(const std::string& key)
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        it->second->resume();
    }
}

/// @brief 停止指定音效池的全部实例并复位播放进度。
/// @param key 音效池标识符。
void AudioManager::stopSoundEffect(const std::string& key)
{
    // stopAll 只影响目标池，其他 key 的并发 voice 保持运行。
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        it->second->stopAll();
    }
}

/// @brief 按主音轨时间计划播放指定音效。
/// @param key 音效池标识。
/// @param targetTime 目标有效出声时间，单位为秒。
/// @param volumeFactor 本次播放额外音量倍率。
/// @param stereoEnvelope 本次播放的线性双声道增益包络。
/// @param playbackControl 玩家轨道与绑定类别的运行时控制描述。
///
/// targetTime 表示听感起点；资源自身 lead-in 会提前调度文件播放。同步路由保留
/// 绝对谱面帧，非同步路由换算为输出域相对延迟。
void AudioManager::playSoundEffectScheduled(
    const std::string& key, double targetTime, float volumeFactor,
    const StereoGainEnvelope&      stereoEnvelope,
    const KeySoundPlaybackControl& playbackControl)
{
    // 排定入口同样不触发加载，静音或缺池时保持无副作用。
    if ( getSFXPoolMute(key) ) return;

    auto it = m_sfxPools.find(key);
    if ( it == m_sfxPools.end() ) return;

    if ( !m_audioTimelineLoaded || !m_audioTimelineNode ) return;

    double samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    // 资源 lead-in 从目标有效出声时间中扣除，结果钳制到时间线零点。
    const double leadInSeconds =
        m_sfxLeadInSeconds.contains(key) ? m_sfxLeadInSeconds[key] : 0.0;
    const double scheduledTime = std::max(0.0, targetTime - leadInSeconds);
    size_t       targetFrame = static_cast<size_t>(scheduledTime * samplerate);

    // 时间线节点在后端关闭前保持常驻，供轻量 provider 读取原子 block 时钟。
    auto* const timelineClock = m_audioTimelineNode.get();

    const auto currentPosition = m_audioTimelineNode->positionFrame();
    // 逻辑位置允许负预滚，调度 planner 的无符号当前帧按零处理。
    const std::size_t currentReferenceFrame =
        currentPosition > 0 ? static_cast<std::size_t>(currentPosition) : 0U;
    const bool syncSpeed = Config::AppConfig::instance()
                               .getEditorSettings()
                               .sfxConfig.hitSfxSyncSpeed;
    const SoundEffectSchedulePlan schedule = planSoundEffectSchedule(
        targetFrame, currentReferenceFrame, m_speed, syncSpeed);
    // OpenAL 负责空间化输出，不叠加 SDL 使用的手工左右包络。
    const StereoGainEnvelope effectiveEnvelope =
        m_playbackBackend == Config::AudioPlaybackBackend::SDL
            ? stereoEnvelope
            : StereoGainEnvelope{};
    if ( schedule.mode == SoundEffectScheduleMode::AbsoluteTimelineFrame ) {
        // estimated delay 只用于极短样本首块包络定位，绝对起播仍看参考时钟。
        const std::size_t scheduledDelayFrames =
            targetFrame > currentReferenceFrame
                ? targetFrame - currentReferenceFrame
                : 0U;
        it->second->playScheduled(volumeFactor,
                                  schedule.frame,
                                  timelineClock,
                                  &readTimelineBlockStart,
                                  effectiveEnvelope,
                                  scheduledDelayFrames,
                                  playbackControl);
    } else {
        // 非同步路由使用已除以预览倍率的输出域相对延迟。
        it->second->playScheduledRelative(
            volumeFactor, schedule.frame, effectiveEnvelope, playbackControl);
    }
}

/// @brief 清空并停止所有音效池中正在播放和预定的音效。
/// @warning 低频完整提交路径，会遍历全部池；ScrubUpdate 不得调用。
void AudioManager::clearAllScheduledSoundEffects()
{
    for ( auto& [key, pool] : m_sfxPools ) {
        // key 只用于结构化遍历，停止语义由每个池独立完成。
        if ( pool ) {
            pool->stopAll();
        }
    }
}

}  // namespace MMM::Audio
