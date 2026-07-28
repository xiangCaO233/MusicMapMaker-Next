#include "audio/AudioManager.h"
#include "audio/AudioTimelineMixerNode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/TimeStretcher.hpp>

namespace MMM::Audio
{
namespace
{
/// @brief 将秒数安全转换为主时间线帧。
AudioTimelineFrame playbackSecondsToFrame(double seconds) noexcept
{
    if ( !std::isfinite(seconds) ) return 0;
    const long double frames =
        static_cast<long double>(seconds) *
        static_cast<long double>(ice::ICEConfig::internal_format.samplerate);
    constexpr auto MIN_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::min());
    constexpr auto MAX_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::max());
    if ( frames <= MIN_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::min();
    }
    if ( frames >= MAX_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::max();
    }
    return static_cast<AudioTimelineFrame>(std::llround(frames));
}

/// @brief 将项目拉伸质量转换为 IonCachyEngine 质量。
/// @param quality 项目全局预览质量。
/// @return 对应引擎枚举。
ice::TimeStretchQuality toIceStretchQuality(
    AudioManager::StretchQuality quality) noexcept
{
    switch ( quality ) {
    case AudioManager::StretchQuality::Fast:
        return ice::TimeStretchQuality::Fast;
    case AudioManager::StretchQuality::Balanced:
        return ice::TimeStretchQuality::Balanced;
    case AudioManager::StretchQuality::Finer:
        return ice::TimeStretchQuality::Finer;
    case AudioManager::StretchQuality::Best:
        return ice::TimeStretchQuality::Best;
    }
    return ice::TimeStretchQuality::Finer;
}
}  // namespace

/// @brief 开始或恢复复合音频时间线。
void AudioManager::play()
{
    if ( m_audioTimelineLoaded && m_audioTimelineNode ) {
        const bool restartFinishedStream = m_audioTimelineNode->finished();
        if ( restartFinishedStream ) {
            resetMainTimeStretcher();
        }
        m_audioTimelineNode->play();
        if ( m_stretcher ) {
            m_stretcher->set_paused(false);
        }
    }
}

/// @brief 暂停复合音频时间线。
void AudioManager::pause()
{
    if ( m_audioTimelineLoaded && m_audioTimelineNode ) {
        m_audioTimelineNode->pause();
        if ( m_stretcher ) {
            m_stretcher->set_paused(true);
        }
    }
}

/// @brief 停止复合音频时间线并回到起始位置。
void AudioManager::stop()
{
    if ( m_audioTimelineLoaded && m_audioTimelineNode ) {
        m_audioTimelineNode->stop();
        resetMainTimeStretcher();
        if ( m_stretcher ) {
            m_stretcher->set_paused(true);
        }
        clearAllScheduledSoundEffects();
    }
}

/// @brief 跳转复合音频时间线播放位置。
/// @param seconds 目标时间，单位为秒。
void AudioManager::seek(double seconds)
{
    if ( m_audioTimelineLoaded && m_audioTimelineNode ) {
        m_audioTimelineNode->seek(playbackSecondsToFrame(seconds));
        resetMainTimeStretcher();
        clearAllScheduledSoundEffects();
    }
}

/// @brief 获取当前播放状态。
/// @return 当前播放状态。
PlaybackStatus AudioManager::getStatus() const
{
    if ( !m_audioTimelineLoaded || !m_audioTimelineNode ) {
        return PlaybackStatus::Stopped;
    }
    const auto requestedState = m_audioTimelineNode->requestedState();
    if ( m_stretcher && m_stretcher->is_paused() ) {
        return requestedState == AudioTimelinePlaybackState::Paused
                   ? PlaybackStatus::Paused
                   : PlaybackStatus::Stopped;
    }
    if ( m_audioTimelineNode->finished() ) {
        if ( m_stretcher && !m_stretcher->is_final_input_drained() ) {
            return PlaybackStatus::Playing;
        }
        return PlaybackStatus::Stopped;
    }
    switch ( requestedState ) {
    case AudioTimelinePlaybackState::Stopped: return PlaybackStatus::Stopped;
    case AudioTimelinePlaybackState::Paused: return PlaybackStatus::Paused;
    case AudioTimelinePlaybackState::Playing: break;
    }
    return PlaybackStatus::Playing;
}

/// @brief 获取复合音频时间线当前播放时间。
/// @return 当前播放时间，单位为秒。
double AudioManager::getCurrentTime() const
{
    if ( !m_audioTimelineLoaded || !m_audioTimelineNode ) return 0.0;
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) return 0.0;

    return static_cast<double>(m_audioTimelineNode->positionFrame()) /
           sampleRate;
}

/// @brief 获取谱面内容与所有自动采样共同决定的复合时长。
/// @return 总时长，单位为秒。
double AudioManager::getTotalTime() const
{
    if ( !m_audioTimelineLoaded || !m_audioTimelineNode ) return 0.0;
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) return 0.0;
    return static_cast<double>(m_audioTimelineNode->timelineEndFrame()) /
           sampleRate;
}

/// @brief 请求在下一个 block 边界清除 Rubber Band 历史采样。
/// @warning 低频播放控制路径：只写入 lock-free discontinuity 邮箱。
void AudioManager::resetMainTimeStretcher()
{
    if ( !m_stretcher ) return;
    static_cast<void>(m_stretcher->request_discontinuity());
}

/// @brief 设置复合时间线全局预览播放倍率。
/// @param speed 目标播放倍率。
void AudioManager::setPlaybackSpeed(double speed)
{
    m_speed = std::isfinite(speed) ? std::clamp(speed, 0.1, 4.0) : 1.0;
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

/// @brief 设置复合时间线全局预览音高偏移。
/// @param semitones 半音偏移量。
void AudioManager::setPlaybackPitch(double semitones)
{
    m_playbackPitch =
        std::isfinite(semitones) ? std::clamp(semitones, -24.0, 24.0) : 0.0;
    if ( m_stretcher ) {
        m_stretcher->set_pitch_semitones(m_playbackPitch);
    }
}

/// @brief 获取复合时间线全局预览音高偏移。
/// @return 半音偏移量。
double AudioManager::getPlaybackPitch() const
{
    return m_playbackPitch;
}

/// @brief 设置复合时间线全局预览拉伸质量。
/// @param quality 目标拉伸质量。
void AudioManager::setPlaybackQuality(StretchQuality quality)
{
    switch ( quality ) {
    case StretchQuality::Fast:
    case StretchQuality::Balanced:
    case StretchQuality::Finer:
    case StretchQuality::Best: m_playbackQuality = quality; break;
    default: m_playbackQuality = StretchQuality::Finer; break;
    }
    if ( m_stretcher ) {
        m_stretcher->set_quality(toIceStretchQuality(m_playbackQuality));
    }
}

/// @brief 获取复合时间线全局预览拉伸质量。
/// @return 当前拉伸质量。
AudioManager::StretchQuality AudioManager::getPlaybackQuality() const
{
    return m_playbackQuality;
}

/// @brief 开始或恢复独立试听音轨播放。
void AudioManager::playAudition()
{
    if ( !m_auditionSource ) {
        return;
    }

    const double totalTime = getAuditionTotalTime();
    if ( totalTime > 0.0 && getAuditionCurrentTime() >= totalTime - 0.001 ) {
        seekAudition(0.0);
    }
    m_auditionSource->play();
    m_auditionStatus = PlaybackStatus::Playing;
}

/// @brief 暂停独立试听音轨播放。
void AudioManager::pauseAudition()
{
    if ( !m_auditionSource ) {
        return;
    }

    m_auditionSource->pause();
    if ( m_auditionStatus != PlaybackStatus::Stopped ) {
        m_auditionStatus = PlaybackStatus::Paused;
    }
}

/// @brief 停止独立试听音轨并回到起始位置。
void AudioManager::stopAudition()
{
    if ( m_auditionSource ) {
        m_auditionSource->pause();
        m_auditionSource->set_playpos(static_cast<size_t>(0));
    }
    m_auditionStatus = PlaybackStatus::Stopped;
}

/// @brief 跳转独立试听音轨播放位置。
/// @param seconds 目标时间，单位为秒。
void AudioManager::seekAudition(double seconds)
{
    if ( !m_auditionSource ) {
        return;
    }

    const PlaybackStatus statusBeforeSeek = getAuditionStatus();
    const double         clampedTime =
        std::clamp(seconds, 0.0, std::max(0.0, getAuditionTotalTime()));
    m_auditionSource->set_playpos(std::chrono::duration<double>(clampedTime));
    if ( statusBeforeSeek == PlaybackStatus::Stopped ) {
        m_auditionStatus = PlaybackStatus::Stopped;
    }
}

/// @brief 获取独立试听音轨的当前播放状态。
/// @return 当前试听播放状态。
PlaybackStatus AudioManager::getAuditionStatus() const
{
    if ( !m_auditionSource ) {
        return PlaybackStatus::Stopped;
    }
    if ( m_auditionSource->isplaying() ) {
        return PlaybackStatus::Playing;
    }
    return m_auditionStatus == PlaybackStatus::Paused ? PlaybackStatus::Paused
                                                      : PlaybackStatus::Stopped;
}

/// @brief 获取独立试听音轨当前播放时间。
/// @return 当前播放时间，单位为秒。
double AudioManager::getAuditionCurrentTime() const
{
    if ( !m_auditionSource ) {
        return 0.0;
    }

    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) {
        return 0.0;
    }
    return static_cast<double>(m_auditionSource->get_playpos()) / sampleRate;
}

/// @brief 获取独立试听音轨总时长。
/// @return 总时长，单位为秒。
double AudioManager::getAuditionTotalTime() const
{
    if ( !m_auditionSource ) {
        return 0.0;
    }
    return std::chrono::duration_cast<std::chrono::duration<double>>(
               m_auditionSource->total_time())
        .count();
}

/// @brief 设置独立试听音轨播放倍率。
/// @param speed 目标播放倍率。
void AudioManager::setAuditionPlaybackSpeed(double speed)
{
    m_auditionSpeed = std::clamp(speed, 0.1, 4.0);
    if ( m_auditionStretcher ) {
        m_auditionStretcher->set_playback_ratio(m_auditionSpeed);
    }
}

/// @brief 获取独立试听音轨请求的播放倍率。
/// @return 当前请求的播放倍率。
double AudioManager::getAuditionPlaybackSpeed() const
{
    return m_auditionSpeed;
}

/// @brief 获取独立试听拉伸器实际生效的播放倍率。
/// @return 当前实际播放倍率。
double AudioManager::getActualAuditionPlaybackSpeed() const
{
    if ( m_auditionStretcher ) {
        return m_auditionStretcher->get_actual_playback_ratio();
    }
    return m_auditionSpeed;
}

}  // namespace MMM::Audio
