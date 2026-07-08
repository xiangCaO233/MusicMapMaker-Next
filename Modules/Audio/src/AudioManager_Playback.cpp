#include "audio/AudioManager.h"

#include <algorithm>
#include <chrono>

#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/TimeStretcher.hpp>

namespace MMM::Audio
{
/// @brief 开始或恢复主音轨播放。
void AudioManager::play()
{
    if ( m_bgmSource ) {
        m_bgmSource->play();
        m_status = PlaybackStatus::Playing;
    }
}

/// @brief 暂停主音轨播放。
void AudioManager::pause()
{
    if ( m_bgmSource ) {
        m_bgmSource->pause();
        m_status = PlaybackStatus::Paused;
    }
}

/// @brief 停止主音轨播放并回到起始位置。
void AudioManager::stop()
{
    if ( m_bgmSource ) {
        m_bgmSource->pause();
        m_bgmSource->set_playpos(static_cast<size_t>(0));
        m_status = PlaybackStatus::Stopped;
        clearAllScheduledSoundEffects();
    }
}

/// @brief 跳转主音轨播放位置。
/// @param seconds 目标时间，单位为秒。
void AudioManager::seek(double seconds)
{
    if ( m_bgmSource ) {
        m_bgmSource->set_playpos(std::chrono::duration<double>(seconds));
        clearAllScheduledSoundEffects();
    }
}

/// @brief 获取当前播放状态。
/// @return 当前播放状态。
PlaybackStatus AudioManager::getStatus() const
{
    return m_status;
}

/// @brief 获取主音轨当前播放时间。
/// @return 当前播放时间，单位为秒。
double AudioManager::getCurrentTime() const
{
    if ( !m_bgmSource ) return 0.0;

    auto   pos = m_bgmSource->get_playpos();
    double samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( samplerate <= 0 ) return 0.0;

    return static_cast<double>(pos) / samplerate;
}

/// @brief 获取主音轨总时长。
/// @return 总时长，单位为秒。
double AudioManager::getTotalTime() const
{
    if ( !m_bgmSource ) return 0.0;
    return std::chrono::duration_cast<std::chrono::duration<double>>(
               m_bgmSource->total_time())
        .count();
}

/// @brief 设置主音轨播放倍率。
/// @param speed 目标播放倍率。
void AudioManager::setPlaybackSpeed(double speed)
{
    m_speed = std::clamp(speed, 0.1, 4.0);
    if ( m_stretcher ) {
        m_stretcher->set_playback_ratio(m_speed);
    }
}

/// @brief 获取用户配置的播放倍率。
/// @return 当前播放倍率。
double AudioManager::getPlaybackSpeed() const
{
    return m_speed;
}

/// @brief 获取音频拉伸器实际生效的播放倍率。
/// @return 实际播放倍率。
double AudioManager::getActualPlaybackSpeed() const
{
    if ( m_stretcher ) {
        return m_stretcher->get_actual_playback_ratio();
    }
    return m_speed;
}

/// @brief 设置主音轨音高偏移。
/// @param semitones 半音偏移量。
void AudioManager::setPlaybackPitch(double semitones)
{
    // -24.0 到 24.0 的范围检查由 TimeStretcher 内部负责。
    if ( m_stretcher ) {
        m_stretcher->set_pitch_semitones(semitones);
    }
}

/// @brief 获取主音轨音高偏移。
/// @return 半音偏移量。
double AudioManager::getPlaybackPitch() const
{
    if ( m_stretcher ) {
        return m_stretcher->get_pitch_semitones();
    }
    return 0.0;
}

/// @brief 设置主音轨变速拉伸质量。
/// @param quality 目标拉伸质量。
void AudioManager::setPlaybackQuality(StretchQuality quality)
{
    if ( m_stretcher ) {
        ice::TimeStretchQuality iceQuality;
        switch ( quality ) {
        case StretchQuality::Fast:
            iceQuality = ice::TimeStretchQuality::Fast;
            break;
        case StretchQuality::Balanced:
            iceQuality = ice::TimeStretchQuality::Balanced;
            break;
        case StretchQuality::Finer:
            iceQuality = ice::TimeStretchQuality::Finer;
            break;
        case StretchQuality::Best:
            iceQuality = ice::TimeStretchQuality::Best;
            break;
        default: iceQuality = ice::TimeStretchQuality::Finer; break;
        }
        m_stretcher->set_quality(iceQuality);
    }
}

/// @brief 获取主音轨变速拉伸质量。
/// @return 当前拉伸质量。
AudioManager::StretchQuality AudioManager::getPlaybackQuality() const
{
    if ( m_stretcher ) {
        auto iceQuality = m_stretcher->get_quality();
        switch ( iceQuality ) {
        case ice::TimeStretchQuality::Fast: return StretchQuality::Fast;
        case ice::TimeStretchQuality::Balanced: return StretchQuality::Balanced;
        case ice::TimeStretchQuality::Finer: return StretchQuality::Finer;
        case ice::TimeStretchQuality::Best: return StretchQuality::Best;
        }
    }
    return StretchQuality::Finer;
}

}  // namespace MMM::Audio
