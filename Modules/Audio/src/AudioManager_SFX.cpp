#include "BackgroundSpectrumAnalyzer.h"
#include "audio/AudioManager.h"
#include "audio/AudioTimelineMixerNode.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/manage/AudioPool.hpp>

namespace MMM::Audio
{
namespace
{
/// @brief 需要跟随主音轨变速器的打击音效 key 前缀。
constexpr const char* HIT_SOUND_EFFECT_KEY_PREFIX = "hiteffect.";

/// @brief 使用独立交互音量的音效 key 前缀。
constexpr const char* INTERACTION_SOUND_EFFECT_KEY_PREFIX = "ui.";

/// @brief 判断音效是否属于谱面打击音效。
/// @param key 音效池标识。
/// @return 属于打击音效时返回 true。
bool isHitSoundEffectKey(const std::string& key)
{
    return key.rfind(HIT_SOUND_EFFECT_KEY_PREFIX, 0) == 0;
}

/// @brief 判断音效是否属于界面交互音效。
/// @param key 音效池标识。
/// @return 属于交互音效时返回 true。
bool isInteractionSoundEffectKey(const std::string& key)
{
    return key.rfind(INTERACTION_SOUND_EFFECT_KEY_PREFIX, 0) == 0;
}
}  // namespace

/// @brief 判断指定音效是否应接入打击音效总线。
/// @param key 音效资源标识。
/// @return 内置打击音效或谱面绑定采样返回 true。
bool AudioManager::usesHitEffectRouting(const std::string& key) const
{
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
    if ( m_globalMuted ) return 0.0f;

    if ( isInteractionSoundEffectKey(key) ) {
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
void AudioManager::setSFXPoolVolume(const std::string& key, float volume,
                                    bool isPermanent)
{
    auto registration = m_registeredSoundEffects.find(key);
    if ( registration != m_registeredSoundEffects.end() ) {
        registration->second.m_defaultVolume = volume;
    }

    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        it->second->setVolume(volume);

        if ( isPermanent ) {
            // 保存到编辑器配置
            auto& sfxCfg =
                Config::AppConfig::instance().getEditorSettings().sfxConfig;
            sfxCfg.permanentSfxVolumes[key] = volume;
            Config::AppConfig::instance().save();
        }

        // 刷新实际音量 (考虑静音)
        it->second->updateEffectiveVolume(getSFXEffectiveGain(key),
                                          getSFXPoolMute(key));
    }
}

/// @brief 设置指定音效池静音状态并按需保存为常驻配置。
/// @param key 音效池标识。
/// @param muted 是否静音。
/// @param isPermanent 是否写入常驻配置。
void AudioManager::setSFXPoolMute(const std::string& key, bool muted,
                                  bool isPermanent)
{
    m_sfxMutes[key] = muted;

    if ( isPermanent ) {
        auto& sfxCfg =
            Config::AppConfig::instance().getEditorSettings().sfxConfig;
        sfxCfg.permanentSfxMutes[key] = muted;
        Config::AppConfig::instance().save();
    }

    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
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

        m_mainMixer->remove_source(mixer);
        m_preStretcherMixer->remove_source(mixer);
        m_hitEffectMixer->remove_source(mixer);
        if ( usesHitEffectRouting(key) ) {
            m_hitEffectMixer->add_source(mixer);
        } else {
            m_mainMixer->add_source(mixer);
        }
    }
    m_mainMixer->remove_source(m_hitEffectSpectrumCapture);
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
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        return it->second->getVolume();
    }

    const auto& sfxCfg =
        Config::AppConfig::instance().getEditorSettings().sfxConfig;
    if ( auto volume = sfxCfg.permanentSfxVolumes.find(key);
         volume != sfxCfg.permanentSfxVolumes.end() ) {
        return volume->second;
    }

    auto registration = m_registeredSoundEffects.find(key);
    if ( registration != m_registeredSoundEffects.end() ) {
        return registration->second.m_defaultVolume;
    }
    return 1.0f;
}

/// @brief 获取指定音效池是否静音。
/// @param key 音效池标识。
/// @return 静音时返回 true。
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
double AudioManager::getSFXDuration(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        return it->second->getDuration();
    }
    return 0.0;
}

/// @brief 获取指定音效池最近一次播放进度。
/// @param key 音效池标识。
/// @return 播放进度，单位为秒。
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
void AudioManager::registerSoundEffect(const std::string& key,
                                       const std::string& filePath,
                                       float              defaultVolume,
                                       double             leadInSeconds)
{
    auto       existingRegistration = m_registeredSoundEffects.find(key);
    const bool wasBoundNoteSound =
        existingRegistration != m_registeredSoundEffects.end() &&
        existingRegistration->second.m_isBoundNoteSound;
    const bool loadedPathChanged =
        existingRegistration != m_registeredSoundEffects.end() &&
        existingRegistration->second.m_filePath != filePath &&
        m_sfxPools.contains(key);
    if ( loadedPathChanged ) {
        unloadSoundEffect(key);
    }

    m_registeredSoundEffects[key] = RegisteredSoundEffect{
        .m_filePath         = filePath,
        .m_defaultVolume    = defaultVolume,
        .m_leadInSeconds    = std::max(0.0, leadInSeconds),
        .m_isBoundNoteSound = wasBoundNoteSound,
        .m_revision         = m_nextSoundEffectRevision++,
    };
    m_pendingSoundEffectLoads.erase(key);

    auto loadedPool = m_sfxPools.find(key);
    if ( loadedPool == m_sfxPools.end() ) {
        return;
    }

    float       activeVolume = defaultVolume;
    const auto& sfxCfg =
        Config::AppConfig::instance().getEditorSettings().sfxConfig;
    if ( auto configuredVolume = sfxCfg.permanentSfxVolumes.find(key);
         configuredVolume != sfxCfg.permanentSfxVolumes.end() ) {
        activeVolume = configuredVolume->second;
    }
    loadedPool->second->setVolume(activeVolume);
    loadedPool->second->updateEffectiveVolume(getSFXEffectiveGain(key),
                                              getSFXPoolMute(key));
    m_sfxLeadInSeconds[key] = std::max(0.0, leadInSeconds);
}

/// @brief 使用已探测的音轨创建音效池并接入混音图。
/// @param key 已登记的音效资源标识。
/// @param track 后台加载完成的音轨。
/// @return 成功接入或已存在时返回 true。
bool AudioManager::attachSoundEffectPool(const std::string&               key,
                                         std::shared_ptr<ice::AudioTrack> track)
{
    if ( m_sfxPools.contains(key) ) {
        return true;
    }
    if ( !track || !m_mainMixer || !m_hitEffectMixer ) {
        return false;
    }

    const auto registration = m_registeredSoundEffects.find(key);
    if ( registration == m_registeredSoundEffects.end() ) {
        return false;
    }

    float       activeVolume = registration->second.m_defaultVolume;
    const auto& sfxCfg =
        Config::AppConfig::instance().getEditorSettings().sfxConfig;
    if ( auto configuredVolume = sfxCfg.permanentSfxVolumes.find(key);
         configuredVolume != sfxCfg.permanentSfxVolumes.end() ) {
        activeVolume = configuredVolume->second;
    }

    auto pool = std::make_shared<SoundEffectPool>(std::move(track));
    pool->init(registration->second.m_isBoundNoteSound ? 1 : 8);
    pool->setVolume(activeVolume);
    pool->updateEffectiveVolume(getSFXEffectiveGain(key), getSFXPoolMute(key));

    if ( usesHitEffectRouting(key) ) {
        m_hitEffectMixer->add_source(pool->getMixer());
    } else {
        m_mainMixer->add_source(pool->getMixer());
    }

    m_sfxPools[key]         = std::move(pool);
    m_sfxLeadInSeconds[key] = registration->second.m_leadInSeconds;
    return true;
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
    m_pendingSoundEffectLoads.erase(key);
    auto trackWeak = m_audioPool->get_or_load(*m_threadPool,
                                              registration->second.m_filePath);
    auto track     = trackWeak.lock();
    if ( !track ) {
        XERROR("Failed to load SFX track: {}", registration->second.m_filePath);
        return false;
    }

    return attachSoundEffectPool(key, std::move(track));
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
    registration->second.m_isBoundNoteSound = true;

    if ( auto loaded = m_sfxPools.find(key); loaded != m_sfxPools.end() ) {
        if ( needsReroute ) {
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
        return true;
    }

    m_pendingSoundEffectLoads[key] = registration->second.m_revision;
    m_queuedSoundEffectLoads.push_back({
        .key      = key,
        .filePath = registration->second.m_filePath,
        .revision = registration->second.m_revision,
    });
    return true;
}

/// @brief 推进后台音效加载任务并接入已准备好的音效池。
/// @param maxPreparedPerUpdate 单次调用最多接入的音效数量。
/// @warning 逻辑低频轮询路径：调用方必须节流；单次只启动固定数量后台任务，
/// 文件探测和解码均在线程池执行。
void AudioManager::updateQueuedSoundEffectLoads(
    std::size_t maxPreparedPerUpdate)
{
    m_soundEffectLoadTasks.erase(
        std::remove_if(m_soundEffectLoadTasks.begin(),
                       m_soundEffectLoadTasks.end(),
                       [](std::future<void>& task) {
                           return task.valid() &&
                                  task.wait_for(std::chrono::seconds(0)) ==
                                      std::future_status::ready;
                       }),
        m_soundEffectLoadTasks.end());

    std::deque<PreparedSoundEffectLoad> prepared;
    {
        std::unique_lock<std::mutex> lock(m_preparedSoundEffectLoadsMutex,
                                          std::try_to_lock);
        if ( lock.owns_lock() ) {
            while ( prepared.size() < maxPreparedPerUpdate &&
                    !m_preparedSoundEffectLoads.empty() ) {
                prepared.push_back(
                    std::move(m_preparedSoundEffectLoads.front()));
                m_preparedSoundEffectLoads.pop_front();
            }
        }
    }

    for ( auto& result : prepared ) {
        if ( m_activeSoundEffectLoadCount > 0U ) {
            --m_activeSoundEffectLoadCount;
        }

        const auto pending = m_pendingSoundEffectLoads.find(result.key);
        if ( pending == m_pendingSoundEffectLoads.end() ||
             pending->second != result.revision ) {
            continue;
        }
        m_pendingSoundEffectLoads.erase(pending);

        const auto registration = m_registeredSoundEffects.find(result.key);
        if ( registration == m_registeredSoundEffects.end() ||
             registration->second.m_revision != result.revision ) {
            continue;
        }
        if ( !result.track ) {
            XERROR("Failed to prepare bound note SFX: {}", result.key);
            continue;
        }
        XINFO("Prepared bound note SFX: {}", result.key);
        attachSoundEffectPool(result.key, std::move(result.track));
    }

    constexpr std::size_t MAX_CONCURRENT_SOUND_EFFECT_LOADS = 4U;
    while ( m_activeSoundEffectLoadCount < MAX_CONCURRENT_SOUND_EFFECT_LOADS &&
            !m_queuedSoundEffectLoads.empty() && m_audioPool && m_threadPool ) {
        QueuedSoundEffectLoad request =
            std::move(m_queuedSoundEffectLoads.front());
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
                auto trackWeak =
                    m_audioPool->get_or_load(*m_threadPool, request.filePath);
                PreparedSoundEffectLoad preparedResult{
                    .key      = std::move(request.key),
                    .revision = request.revision,
                    .track    = trackWeak.lock(),
                };
                std::lock_guard<std::mutex> lock(
                    m_preparedSoundEffectLoadsMutex);
                m_preparedSoundEffectLoads.push_back(std::move(preparedResult));
            }));
        ++m_activeSoundEffectLoadCount;
    }
}

/// @brief 查询指定音效是否已经完成解码并创建音效池。
/// @param key 音效标识符。
/// @return 音效池已存在时返回 true。
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
    registerSoundEffect(key, filePath, defaultVolume, leadInSeconds);
    return ensureSoundEffectLoaded(key);
}

/// @brief 卸载指定音效池并断开混音路由。
/// @param key 音效池标识。
void AudioManager::unloadSoundEffect(const std::string& key)
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        auto mixer = it->second->getMixer();
        if ( mixer ) {
            m_mainMixer->remove_source(mixer);
            m_preStretcherMixer->remove_source(mixer);
            if ( m_hitEffectMixer ) {
                m_hitEffectMixer->remove_source(mixer);
            }
        }
        m_sfxPools.erase(it);
        XINFO("Unloaded SFX: {}", key);
    }
    m_registeredSoundEffects.erase(key);
    m_pendingSoundEffectLoads.erase(key);
    m_sfxLeadInSeconds.erase(key);
    m_sfxMutes.erase(key);
}

/// @brief 停止并释放所有已加载音效池。
/// @warning 低频资源重载路径：皮肤热切换时调用，会清空所有 SFX pool
/// 和调度状态，禁止放入播放热路径。
void AudioManager::clearSoundEffects()
{
    clearAllScheduledSoundEffects();

    std::vector<std::string> keys;
    keys.reserve(m_sfxPools.size());
    for ( const auto& [key, pool] : m_sfxPools ) {
        (void)pool;
        keys.push_back(key);
    }

    for ( const auto& key : keys ) {
        unloadSoundEffect(key);
    }

    m_registeredSoundEffects.clear();
    m_queuedSoundEffectLoads.clear();
    m_pendingSoundEffectLoads.clear();
    m_sfxLeadInSeconds.clear();
    m_sfxMutes.clear();

    std::lock_guard<std::mutex> lock(m_preparedSoundEffectLoadsMutex);
    const std::size_t discardedCount = m_preparedSoundEffectLoads.size();
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
    for ( auto& task : m_soundEffectLoadTasks ) {
        if ( task.valid() ) {
            task.wait();
        }
    }
    m_soundEffectLoadTasks.clear();

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
    if ( getSFXPoolMute(key) ) return;

    auto it = m_sfxPools.find(key);
    if ( it == m_sfxPools.end() ) return;

    it->second->play(
        getSFXEffectiveGain(key) * it->second->getVolume() * volumeFactor,
        pitchSemitones);
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

/// @brief 按主音轨时间计划播放指定音效。
/// @param key 音效池标识。
/// @param targetTime 目标有效出声时间，单位为秒。
/// @param volumeFactor 本次播放额外音量倍率。
/// @param stereoEnvelope 本次播放的线性双声道增益包络。
void AudioManager::playSoundEffectScheduled(
    const std::string& key, double targetTime, float volumeFactor,
    const StereoGainEnvelope& stereoEnvelope)
{
    if ( getSFXPoolMute(key) ) return;

    auto it = m_sfxPools.find(key);
    if ( it == m_sfxPools.end() ) return;

    if ( !m_audioTimelineNode ) return;

    double samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    const double leadInSeconds =
        m_sfxLeadInSeconds.contains(key) ? m_sfxLeadInSeconds[key] : 0.0;
    const double scheduledTime = std::max(0.0, targetTime - leadInSeconds);
    size_t       targetFrame = static_cast<size_t>(scheduledTime * samplerate);

    // 获取复合时间线播放位置的闭包，用于 SourceNode 内部参考。
    auto timelineReference = [this]() -> size_t {
        if ( m_audioTimelineNode ) {
            const auto position = m_audioTimelineNode->positionFrame();
            return position > 0 ? static_cast<std::size_t>(position) : 0U;
        }
        return 0;
    };

    const auto        currentPosition = m_audioTimelineNode->positionFrame();
    const std::size_t currentReferenceFrame =
        currentPosition > 0 ? static_cast<std::size_t>(currentPosition) : 0U;
    const std::size_t scheduledDelayFrames =
        targetFrame > currentReferenceFrame
            ? targetFrame - currentReferenceFrame
            : 0U;
    const StereoGainEnvelope effectiveEnvelope =
        m_playbackBackend == Config::AudioPlaybackBackend::SDL
            ? stereoEnvelope
            : StereoGainEnvelope{};
    it->second->playScheduled(
        getSFXEffectiveGain(key) * it->second->getVolume() * volumeFactor,
        targetFrame,
        timelineReference,
        effectiveEnvelope,
        scheduledDelayFrames);
}

/// @brief 清空并停止所有正在播放和预定的音效

/// @brief 清空并停止所有音效池中正在播放和预定的音效。
void AudioManager::clearAllScheduledSoundEffects()
{
    for ( auto& [key, pool] : m_sfxPools ) {
        if ( pool ) {
            pool->stopAll();
        }
    }
}

}  // namespace MMM::Audio
