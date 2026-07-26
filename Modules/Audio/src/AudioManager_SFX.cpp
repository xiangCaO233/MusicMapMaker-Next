#include "BackgroundSpectrumAnalyzer.h"
#include "audio/AudioManager.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <memory>
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
        if ( isHitSoundEffectKey(key) ) {
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
    const bool loadedPathChanged =
        existingRegistration != m_registeredSoundEffects.end() &&
        existingRegistration->second.m_filePath != filePath &&
        m_sfxPools.contains(key);
    if ( loadedPathChanged ) {
        unloadSoundEffect(key);
    }

    m_registeredSoundEffects[key] = RegisteredSoundEffect{
        .m_filePath      = filePath,
        .m_defaultVolume = defaultVolume,
        .m_leadInSeconds = std::max(0.0, leadInSeconds),
    };

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

    float       activeVolume = registration->second.m_defaultVolume;
    const auto& sfxCfg =
        Config::AppConfig::instance().getEditorSettings().sfxConfig;
    if ( auto configuredVolume = sfxCfg.permanentSfxVolumes.find(key);
         configuredVolume != sfxCfg.permanentSfxVolumes.end() ) {
        activeVolume = configuredVolume->second;
    }

    XINFO("Loading registered SFX: {} from {} (Volume: {})",
          key,
          registration->second.m_filePath,
          activeVolume);
    auto trackWeak = m_audioPool->get_or_load(*m_threadPool,
                                              registration->second.m_filePath);
    auto track     = trackWeak.lock();
    if ( !track ) {
        XERROR("Failed to load SFX track: {}", registration->second.m_filePath);
        return false;
    }

    auto pool = std::make_shared<SoundEffectPool>(track);
    pool->init(8);
    pool->setVolume(activeVolume);
    pool->updateEffectiveVolume(getSFXEffectiveGain(key), getSFXPoolMute(key));

    if ( isHitSoundEffectKey(key) ) {
        m_hitEffectMixer->add_source(pool->getMixer());
    } else {
        m_mainMixer->add_source(pool->getMixer());
    }

    m_sfxPools[key]         = std::move(pool);
    m_sfxLeadInSeconds[key] = registration->second.m_leadInSeconds;
    return true;
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
    m_sfxLeadInSeconds.clear();
    m_sfxMutes.clear();
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

    if ( !m_bgmSource ) return;

    double samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    const double leadInSeconds =
        m_sfxLeadInSeconds.contains(key) ? m_sfxLeadInSeconds[key] : 0.0;
    const double scheduledTime = std::max(0.0, targetTime - leadInSeconds);
    size_t       targetFrame = static_cast<size_t>(scheduledTime * samplerate);

    // 获取 BGM 播放位置的闭包，用于 SourceNode 内部参考
    auto bgmRef = [this]() -> size_t {
        if ( m_bgmSource ) return m_bgmSource->get_playpos();
        return 0;
    };

    const std::size_t currentReferenceFrame = m_bgmSource->get_playpos();
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
        bgmRef,
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
