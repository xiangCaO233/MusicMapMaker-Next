#include "audio/AudioManager.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"

#include <algorithm>

#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>

namespace MMM::Audio
{
namespace
{
/** @brief 将项目声道模式转换为 IonCachyEngine 声道模式。 */
ice::MixBusChannelMode toIceChannelMode(MixerChannelMode mode)
{
    switch ( mode ) {
    case MixerChannelMode::MuteLeft: return ice::MixBusChannelMode::MuteLeft;
    case MixerChannelMode::MuteRight: return ice::MixBusChannelMode::MuteRight;
    case MixerChannelMode::CopyLeftToRight:
        return ice::MixBusChannelMode::CopyLeftToRight;
    case MixerChannelMode::CopyRightToLeft:
        return ice::MixBusChannelMode::CopyRightToLeft;
    case MixerChannelMode::Stereo: return ice::MixBusChannelMode::Stereo;
    }
    return ice::MixBusChannelMode::Stereo;
}

/** @brief 将 IonCachyEngine 声道模式转换为项目声道模式。 */
MixerChannelMode fromIceChannelMode(ice::MixBusChannelMode mode)
{
    switch ( mode ) {
    case ice::MixBusChannelMode::MuteLeft: return MixerChannelMode::MuteLeft;
    case ice::MixBusChannelMode::MuteRight: return MixerChannelMode::MuteRight;
    case ice::MixBusChannelMode::CopyLeftToRight:
        return MixerChannelMode::CopyLeftToRight;
    case ice::MixBusChannelMode::CopyRightToLeft:
        return MixerChannelMode::CopyRightToLeft;
    case ice::MixBusChannelMode::Stereo: return MixerChannelMode::Stereo;
    }
    return MixerChannelMode::Stereo;
}
}  // namespace
/// @brief 设置主音轨音量并立即应用到音频节点。
/// @param volume 目标音量。
void AudioManager::setMainTrackVolume(float volume)
{
    m_mainTrackVolume = std::clamp(volume, 0.0f, 1.0f);
    if ( m_bgmSource ) {
        float finalVol = (m_globalMuted || m_bgmGainMuted)
                             ? 0.0f
                             : m_mainTrackVolume * m_globalVolume * m_bgmGain;
        if ( m_mainTrackMuted ) finalVol = 0.0f;
        m_bgmSource->setvolume(finalVol);
    }
}

/// @brief 获取主音轨音量。
/// @return 主音轨音量。
float AudioManager::getMainTrackVolume() const
{
    return m_mainTrackVolume;
}

/// @brief 设置主音轨静音状态并立即应用。
/// @param muted 是否静音。
void AudioManager::setMainTrackMute(bool muted)
{
    m_mainTrackMuted = muted;
    if ( m_bgmSource ) {
        float finalVol = (m_globalMuted || m_bgmGainMuted)
                             ? 0.0f
                             : m_mainTrackVolume * m_globalVolume * m_bgmGain;
        if ( m_mainTrackMuted ) finalVol = 0.0f;
        m_bgmSource->setvolume(finalVol);
    }
}

/// @brief 获取主音轨是否静音。
/// @return 静音时返回 true。
bool AudioManager::isMainTrackMuted() const
{
    return m_mainTrackMuted;
}

/// @brief 设置全局音量、保存配置并刷新所有轨道有效音量。
/// @param volume 目标全局音量。
void AudioManager::setGlobalVolume(float volume)
{
    m_globalVolume = std::clamp(volume, 0.0f, 1.0f);

    // 同步到配置并保存
    auto& settings        = Config::AppConfig::instance().getEditorSettings();
    settings.globalVolume = m_globalVolume;
    Config::AppConfig::instance().save();

    // 重新应用全局音量到主音轨
    if ( m_bgmSource ) {
        float finalVol = (m_globalMuted || m_bgmGainMuted)
                             ? 0.0f
                             : m_mainTrackVolume * m_globalVolume * m_bgmGain;
        if ( m_mainTrackMuted ) finalVol = 0.0f;
        m_bgmSource->setvolume(finalVol);
    }

    // 重新应用全局音量到所有音效池
    for ( auto& [key, pool] : m_sfxPools ) {
        float sfxFinalVol = (m_globalMuted || m_sfxGainMuted)
                                ? 0.0f
                                : m_globalVolume * m_sfxGain;
        pool->updateEffectiveVolume(sfxFinalVol, getSFXPoolMute(key));
    }
}

/// @brief 设置全局静音状态并刷新所有轨道有效音量。
/// @param muted 是否静音。
void AudioManager::setGlobalMute(bool muted)
{
    m_globalMuted = muted;

    // 同步到配置
    auto& settings       = Config::AppConfig::instance().getEditorSettings();
    settings.globalMuted = m_globalMuted;
    Config::AppConfig::instance().save();

    setGlobalVolume(m_globalVolume);  // 重新应用所有音量
}

/// @brief 获取全局是否静音。
/// @return 静音时返回 true。
bool AudioManager::isGlobalMuted() const
{
    return m_globalMuted;
}

/// @brief 获取主混音输出左声道电平。
/// @return 左声道电平。
float AudioManager::getOutputLevelL() const
{
    if ( m_mainMixer ) return m_mainMixer->get_left_level();
    return 0.0f;
}

/// @brief 获取主混音输出右声道电平。
/// @return 右声道电平。
float AudioManager::getOutputLevelR() const
{
    if ( m_mainMixer ) return m_mainMixer->get_right_level();
    return 0.0f;
}

/// @brief 获取主音轨左声道实时电平。
/// @return 左声道电平。
float AudioManager::getMainTrackLevelL() const
{
    if ( m_bgmSource ) return m_bgmSource->get_left_level();
    return 0.0f;
}

/// @brief 获取主音轨右声道实时电平。
/// @return 右声道电平。
float AudioManager::getMainTrackLevelR() const
{
    if ( m_bgmSource ) return m_bgmSource->get_right_level();
    return 0.0f;
}

/// @brief 获取指定音效池左声道实时电平。
/// @param key 音效池标识。
/// @return 左声道电平。
float AudioManager::getSFXPoolLevelL(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        if ( auto mixer = it->second->getMixer() ) {
            return mixer->get_left_level();
        }
    }
    return 0.0f;
}

/// @brief 获取指定音效池右声道实时电平。
/// @param key 音效池标识。
/// @return 右声道电平。
float AudioManager::getSFXPoolLevelR(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        if ( auto mixer = it->second->getMixer() ) {
            return mixer->get_right_level();
        }
    }
    return 0.0f;
}

/// @brief 获取全局音量。
/// @return 当前全局音量。
float AudioManager::getGlobalVolume() const
{
    return m_globalVolume;
}

/// @brief 设置主混音器左声道静音。
/// @param muted 是否静音。
void AudioManager::setMainMixerLeftMute(bool muted)
{
    if ( m_mainMixer ) {
        m_mainMixer->set_mute_left(muted);
    }
}

/// @brief 获取主混音器左声道是否静音。
/// @return 静音时返回 true。
bool AudioManager::isMainMixerLeftMuted() const
{
    if ( m_mainMixer ) {
        return m_mainMixer->is_mute_left();
    }
    return false;
}

/// @brief 设置主混音器右声道静音。
/// @param muted 是否静音。
void AudioManager::setMainMixerRightMute(bool muted)
{
    if ( m_mainMixer ) {
        m_mainMixer->set_mute_right(muted);
    }
}

/// @brief 获取主混音器右声道是否静音。
/// @return 静音时返回 true。
bool AudioManager::isMainMixerRightMuted() const
{
    if ( m_mainMixer ) {
        return m_mainMixer->is_mute_right();
    }
    return false;
}

/** @brief 设置主混音器双声道输出模式。 */
void AudioManager::setMainMixerChannelMode(MixerChannelMode mode)
{
    if ( m_mainMixer ) {
        m_mainMixer->set_channel_mode(toIceChannelMode(mode));
    }
}

/** @brief 获取主混音器双声道输出模式。 */
MixerChannelMode AudioManager::getMainMixerChannelMode() const
{
    if ( m_mainMixer ) {
        return fromIceChannelMode(m_mainMixer->get_channel_mode());
    }
    return MixerChannelMode::Stereo;
}

/// @brief 设置 BGM 全局增益、保存配置并刷新主音轨音量。
/// @param gain 目标增益。
void AudioManager::setBGMGain(float gain)
{
    m_bgmGain = std::clamp(gain, 0.0f, 1.0f);

    auto& settings   = Config::AppConfig::instance().getEditorSettings();
    settings.bgmGain = m_bgmGain;
    Config::AppConfig::instance().save();

    setMainTrackVolume(m_mainTrackVolume);  // 重新应用
}

/// @brief 获取 BGM 全局增益。
/// @return 当前 BGM 增益。
float AudioManager::getBGMGain() const
{
    return m_bgmGain;
}

/// @brief 设置 BGM 增益静音状态并刷新主音轨音量。
/// @param muted 是否静音。
void AudioManager::setBGMGainMute(bool muted)
{
    m_bgmGainMuted = muted;

    auto& settings        = Config::AppConfig::instance().getEditorSettings();
    settings.bgmGainMuted = m_bgmGainMuted;
    Config::AppConfig::instance().save();

    setMainTrackVolume(m_mainTrackVolume);
}

/// @brief 获取 BGM 增益是否静音。
/// @return 静音时返回 true。
bool AudioManager::isBGMGainMuted() const
{
    return m_bgmGainMuted;
}

/// @brief 设置 SFX 全局增益、保存配置并刷新音效音量。
/// @param gain 目标增益。
void AudioManager::setSFXGain(float gain)
{
    m_sfxGain = std::clamp(gain, 0.0f, 1.0f);

    auto& settings   = Config::AppConfig::instance().getEditorSettings();
    settings.sfxGain = m_sfxGain;
    Config::AppConfig::instance().save();

    setGlobalVolume(m_globalVolume);  // 重新应用 SFX 部分
}

/// @brief 获取 SFX 全局增益。
/// @return 当前 SFX 增益。
float AudioManager::getSFXGain() const
{
    return m_sfxGain;
}

/// @brief 设置 SFX 增益静音状态并刷新音效音量。
/// @param muted 是否静音。
void AudioManager::setSFXGainMute(bool muted)
{
    m_sfxGainMuted = muted;

    auto& settings        = Config::AppConfig::instance().getEditorSettings();
    settings.sfxGainMuted = m_sfxGainMuted;
    Config::AppConfig::instance().save();

    setGlobalVolume(m_globalVolume);
}

/// @brief 获取 SFX 增益是否静音。
/// @return 静音时返回 true。
bool AudioManager::isSFXGainMuted() const
{
    return m_sfxGainMuted;
}

}  // namespace MMM::Audio
