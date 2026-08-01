#include "logic/audio/PlaybackVisualClock.h"

#include <algorithm>
#include <cmath>

namespace MMM::Logic
{
namespace
{

/// @brief 新音频 block 原点低通系数。
constexpr double AUDIO_ANCHOR_LOWPASS_ALPHA = 0.05;

/// @brief 小于该值的音画误差不进入校准。
constexpr double SYNC_DEADZONE_SECONDS = 0.002;

/// @brief 超过该值的误差视为离散跳转并立即重建锚点。
constexpr double LARGE_ERROR_SECONDS = 0.5;

/// @brief 规范化播放倍率。
[[nodiscard]] double sanitizeRate(double rate) noexcept
{
    return std::isfinite(rate) && rate > 0.0 ? rate : 1.0;
}

/// @brief 规范化壁钟值。
[[nodiscard]] double sanitizeNow(double now, double fallback) noexcept
{
    return std::isfinite(now) ? now : fallback;
}

/// @brief 判断音频线程是否确认了上一次逻辑更新已经观察的控制请求。
/// @param applied 当前已应用邮箱版本。
/// @param lastApplied 上一次观察到的已应用邮箱版本。
/// @param requested 当前请求邮箱版本。
/// @param lastRequested 上一次观察到的请求邮箱版本。
/// @return applied 切换到已观察的当前请求时返回 true。
[[nodiscard]] bool acknowledgesObservedRequest(
    std::uint64_t applied, std::uint64_t lastApplied, std::uint64_t requested,
    std::uint64_t lastRequested) noexcept
{
    return applied != lastApplied && applied == requested &&
           requested == lastRequested;
}

}  // namespace

double PlaybackVisualClock::update(
    const Audio::AudioTimelineClockSnapshot& snapshot, double nowSteadySeconds,
    const Config::SyncConfig& config) noexcept
{
    nowSteadySeconds = sanitizeNow(nowSteadySeconds, m_anchorSteadySeconds);
    if ( !snapshot.valid || snapshot.sampleRate == 0U ) {
        const double resolved       = currentTimeAt(nowSteadySeconds);
        m_lastResolvedTime          = resolved;
        m_lastResolvedSteadySeconds = nowSteadySeconds;
        return resolved;
    }

    const double rate        = sanitizeRate(snapshot.playbackRate);
    const double rawPosition = snapshot.positionSeconds();
    const double observationTime =
        sanitizeNow(snapshot.steadyTimeSeconds(), nowSteadySeconds);
    const bool isPlaying =
        snapshot.state == Audio::AudioTimelinePlaybackState::Playing;
    const double syncInterval = std::isfinite(config.syncInterval)
                                    ? std::max(config.syncInterval, 0.0)
                                    : 10.0;

    if ( !m_initialized ) {
        reanchor(snapshot, nowSteadySeconds);
        return m_lastResolvedTime;
    }

    const double previousResolved = currentTimeAt(nowSteadySeconds);
    const bool   scheduleChanged =
        snapshot.scheduleGeneration != m_lastScheduleGeneration;
    const bool seekRequestChanged = snapshot.seekSequence != m_lastSeekSequence;
    const bool seekAcknowledged =
        acknowledgesObservedRequest(snapshot.appliedSeekSequence,
                                    m_lastAppliedSeekSequence,
                                    snapshot.seekSequence,
                                    m_lastSeekSequence);
    const bool playbackAcknowledged =
        acknowledgesObservedRequest(snapshot.appliedPlaybackSequence,
                                    m_lastAppliedPlaybackSequence,
                                    snapshot.playbackSequence,
                                    m_lastPlaybackSequence);
    const bool controlAcknowledged = seekAcknowledged || playbackAcknowledged;
    const bool postControlDiscontinuity =
        snapshot.epoch != snapshot.controlEpoch;
    const double predictedAtObservation = currentTimeAt(observationTime);
    const bool   scheduleAckKeepsContinuity =
        scheduleChanged && controlAcknowledged && !postControlDiscontinuity &&
        std::isfinite(rawPosition) && std::isfinite(predictedAtObservation) &&
        std::abs(rawPosition - predictedAtObservation) <= LARGE_ERROR_SECONDS;

    bool discontinuity = false;
    if ( seekRequestChanged ) {
        discontinuity = true;
    } else if ( scheduleChanged ) {
        discontinuity = !scheduleAckKeepsContinuity;
    } else if ( snapshot.epoch != m_lastEpoch ) {
        discontinuity = !controlAcknowledged || postControlDiscontinuity;
    }

    const bool rateChanged  = std::abs(rate - m_playbackRate) > 1e-9;
    const bool stateChanged = snapshot.state != m_lastState;
    const bool playbackCommandChanged =
        snapshot.playbackSequence != m_lastPlaybackSequence;

    if ( discontinuity ) {
        reanchor(snapshot, nowSteadySeconds);
        return m_lastResolvedTime;
    }

    if ( rateChanged ) {
        rebase(previousResolved, nowSteadySeconds, rate, isPlaying);
        m_hasSmoothedAudioOrigin       = false;
        m_lastCalibrationSteadySeconds = nowSteadySeconds - syncInterval;
    } else if ( stateChanged || playbackCommandChanged ) {
        if ( snapshot.state == Audio::AudioTimelinePlaybackState::Paused ) {
            const double transitionTime = std::clamp(
                observationTime, m_anchorSteadySeconds, nowSteadySeconds);
            rebase(
                currentTimeAt(transitionTime), nowSteadySeconds, rate, false);
        } else if ( isPlaying ) {
            double resumedPosition = rawPosition;
            if ( std::abs(resumedPosition - previousResolved) <=
                 LARGE_ERROR_SECONDS ) {
                resumedPosition = previousResolved;
            }
            rebase(resumedPosition, nowSteadySeconds, rate, true);
            if ( m_lastState != Audio::AudioTimelinePlaybackState::Playing ) {
                m_hasSmoothedAudioOrigin       = false;
                m_lastCalibrationSteadySeconds = nowSteadySeconds;
            }
        } else {
            rebase(rawPosition, nowSteadySeconds, rate, false);
        }
    }

    const bool newObservation = snapshot.sequence != m_lastObservationSequence;
    if ( newObservation && isPlaying ) {
        const double observedOrigin = observationTime - rawPosition / rate;
        if ( std::isfinite(observedOrigin) ) {
            if ( !m_hasSmoothedAudioOrigin ) {
                m_smoothedAudioOriginSeconds = observedOrigin;
                m_hasSmoothedAudioOrigin     = true;
            } else {
                m_smoothedAudioOriginSeconds +=
                    (observedOrigin - m_smoothedAudioOriginSeconds) *
                    AUDIO_ANCHOR_LOWPASS_ALPHA;
            }
        }
    }

    double resolved = currentTimeAt(nowSteadySeconds);
    if ( isPlaying && m_hasSmoothedAudioOrigin &&
         nowSteadySeconds - m_lastCalibrationSteadySeconds >= syncInterval ) {
        const double audioEstimate =
            (nowSteadySeconds - m_smoothedAudioOriginSeconds) * rate;
        const double error             = audioEstimate - resolved;
        m_lastCalibrationSteadySeconds = nowSteadySeconds;

        if ( std::isfinite(error) && std::abs(error) > LARGE_ERROR_SECONDS ) {
            rebase(audioEstimate, nowSteadySeconds, rate, true);
        } else if ( std::isfinite(error) &&
                    std::abs(error) > SYNC_DEADZONE_SECONDS ) {
            resolved += error * static_cast<double>(config.integralFactor);
            rebase(resolved, nowSteadySeconds, rate, true);
        }
    }

    resolved = currentTimeAt(nowSteadySeconds);

    m_lastObservationSequence     = snapshot.sequence;
    m_lastEpoch                   = snapshot.epoch;
    m_lastScheduleGeneration      = snapshot.scheduleGeneration;
    m_lastSeekSequence            = snapshot.seekSequence;
    m_lastAppliedSeekSequence     = snapshot.appliedSeekSequence;
    m_lastPlaybackSequence        = snapshot.playbackSequence;
    m_lastAppliedPlaybackSequence = snapshot.appliedPlaybackSequence;
    m_lastState                   = snapshot.state;
    m_lastResolvedTime            = resolved;
    m_lastResolvedSteadySeconds   = nowSteadySeconds;
    return resolved;
}

double PlaybackVisualClock::currentTimeAt(
    double nowSteadySeconds) const noexcept
{
    if ( !m_initialized ) return 0.0;
    if ( !m_playing || !std::isfinite(nowSteadySeconds) ) {
        return m_anchorPositionSeconds;
    }
    const double elapsed =
        std::max(nowSteadySeconds - m_anchorSteadySeconds, 0.0);
    const double result = m_anchorPositionSeconds + elapsed * m_playbackRate;
    return std::isfinite(result) ? result : m_anchorPositionSeconds;
}

double PlaybackVisualClock::resolveAt(double nowSteadySeconds) noexcept
{
    nowSteadySeconds = sanitizeNow(nowSteadySeconds, m_anchorSteadySeconds);
    const double resolved       = currentTimeAt(nowSteadySeconds);
    m_lastResolvedTime          = resolved;
    m_lastResolvedSteadySeconds = nowSteadySeconds;
    return resolved;
}

bool PlaybackVisualClock::initialized() const noexcept
{
    return m_initialized;
}

double PlaybackVisualClock::lastResolvedSteadyTime() const noexcept
{
    return m_lastResolvedSteadySeconds;
}

void PlaybackVisualClock::rebase(double positionSeconds,
                                 double nowSteadySeconds, double playbackRate,
                                 bool playing) noexcept
{
    m_initialized = true;
    m_anchorPositionSeconds =
        std::isfinite(positionSeconds) ? positionSeconds : 0.0;
    m_anchorSteadySeconds =
        std::isfinite(nowSteadySeconds) ? nowSteadySeconds : 0.0;
    m_playbackRate              = sanitizeRate(playbackRate);
    m_playing                   = playing;
    m_lastResolvedTime          = m_anchorPositionSeconds;
    m_lastResolvedSteadySeconds = m_anchorSteadySeconds;
}

void PlaybackVisualClock::reset() noexcept
{
    *this = PlaybackVisualClock{};
}

void PlaybackVisualClock::reanchor(
    const Audio::AudioTimelineClockSnapshot& snapshot,
    double                                   nowSteadySeconds) noexcept
{
    const double rate = sanitizeRate(snapshot.playbackRate);
    const double observationTime =
        sanitizeNow(snapshot.steadyTimeSeconds(), nowSteadySeconds);
    const bool playing =
        snapshot.state == Audio::AudioTimelinePlaybackState::Playing;
    double position = snapshot.positionSeconds();
    if ( playing ) {
        position += std::max(nowSteadySeconds - observationTime, 0.0) * rate;
    }
    rebase(position, nowSteadySeconds, rate, playing);

    m_lastObservationSequence      = snapshot.sequence;
    m_lastEpoch                    = snapshot.epoch;
    m_lastScheduleGeneration       = snapshot.scheduleGeneration;
    m_lastSeekSequence             = snapshot.seekSequence;
    m_lastAppliedSeekSequence      = snapshot.appliedSeekSequence;
    m_lastPlaybackSequence         = snapshot.playbackSequence;
    m_lastAppliedPlaybackSequence  = snapshot.appliedPlaybackSequence;
    m_lastState                    = snapshot.state;
    m_lastResolvedTime             = position;
    m_lastCalibrationSteadySeconds = nowSteadySeconds;

    const double observedOrigin =
        observationTime - snapshot.positionSeconds() / rate;
    m_smoothedAudioOriginSeconds =
        std::isfinite(observedOrigin) ? observedOrigin : nowSteadySeconds;
    m_hasSmoothedAudioOrigin = playing;
}

}  // namespace MMM::Logic
