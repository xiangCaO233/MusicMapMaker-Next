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
/// @brief 设置指定音效池音量并按需保存为常驻配置。
/// @param key 音效池标识。
/// @param volume 目标音量。
/// @param isPermanent 是否写入常驻配置。
void AudioManager::setSFXPoolVolume(const std::string& key, float volume,
                                    bool isPermanent)
{
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
        it->second->updateEffectiveVolume(m_globalVolume, getSFXPoolMute(key));
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
        float sfxFinalVol = (m_globalMuted || m_sfxGainMuted)
                                ? 0.0f
                                : m_globalVolume * m_sfxGain;
        it->second->updateEffectiveVolume(sfxFinalVol, muted);
    }
}

/// @brief 根据同步变速配置切换音效池路由。
/// @param syncSpeed 是否让音效跟随主音轨变速器。
void AudioManager::updateSFXSyncSpeedRouting(bool syncSpeed)
{
    if ( !m_mainMixer || !m_preStretcherMixer ) return;

    for ( auto& [key, pool] : m_sfxPools ) {
        auto mixer = pool->getMixer();
        if ( !mixer ) continue;

        if ( syncSpeed ) {
            m_mainMixer->remove_source(mixer);
            m_preStretcherMixer->add_source(mixer);
        } else {
            m_preStretcherMixer->remove_source(mixer);
            m_mainMixer->add_source(mixer);
        }
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
    if ( !m_audioPool || !m_threadPool || !m_mainMixer ) return false;

    // 检查是否已经有配置好的音量 (来自 EditorSettings 或之前的加载)
    float activeVolume = defaultVolume;
    auto& sfxCfg = Config::AppConfig::instance().getEditorSettings().sfxConfig;
    if ( sfxCfg.permanentSfxVolumes.count(key) ) {
        activeVolume = sfxCfg.permanentSfxVolumes.at(key);
    }

    XINFO(
        "Preloading SFX: {} from {} (Volume: {})", key, filePath, activeVolume);
    auto trackWeak = m_audioPool->get_or_load(*m_threadPool, filePath);
    auto track     = trackWeak.lock();

    if ( !track ) {
        XERROR("Failed to load SFX track: {}", filePath);
        return false;
    }

    auto pool = std::make_shared<SoundEffectPool>(track);
    pool->init(8);  // 预分配 8 个并发节点
    pool->setVolume(activeVolume);
    float sfxFinalVol =
        (m_globalMuted || m_sfxGainMuted) ? 0.0f : m_globalVolume * m_sfxGain;
    pool->updateEffectiveVolume(sfxFinalVol, getSFXPoolMute(key));

    // 根据配置决定连接到哪个 Mixer
    if ( sfxCfg.hitSfxSyncSpeed ) {
        m_preStretcherMixer->add_source(pool->getMixer());
    } else {
        m_mainMixer->add_source(pool->getMixer());
    }

    m_sfxPools[key]         = std::move(pool);
    m_sfxLeadInSeconds[key] = std::max(0.0, leadInSeconds);
    return true;
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
        }
        m_sfxPools.erase(it);
        m_sfxLeadInSeconds.erase(key);
        m_sfxMutes.erase(key);
        XINFO("Unloaded SFX: {}", key);
    }
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

    m_sfxLeadInSeconds.clear();
    m_sfxMutes.clear();
}

/// @brief 立即播放指定音效。
/// @param key 音效池标识。
/// @param volumeFactor 本次播放额外音量倍率。
void AudioManager::playSoundEffect(const std::string& key, float volumeFactor)
{
    if ( getSFXPoolMute(key) ) return;

    auto it = m_sfxPools.find(key);
    if ( it == m_sfxPools.end() ) return;

    float sfxFinalVol =
        (m_globalMuted || m_sfxGainMuted) ? 0.0f : m_globalVolume * m_sfxGain;
    it->second->play(sfxFinalVol * it->second->getVolume() * volumeFactor);
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
void AudioManager::playSoundEffectScheduled(const std::string& key,
                                            double             targetTime,
                                            float              volumeFactor)
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

    float sfxFinalVol =
        (m_globalMuted || m_sfxGainMuted) ? 0.0f : m_globalVolume * m_sfxGain;
    it->second->playScheduled(
        sfxFinalVol * it->second->getVolume() * volumeFactor,
        targetFrame,
        bgmRef);
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
