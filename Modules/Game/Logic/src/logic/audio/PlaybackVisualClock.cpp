#include "logic/audio/PlaybackVisualClock.h"

#include <algorithm>
#include <cmath>

namespace MMM::Logic
{
namespace
{

/// @brief 新音频 block 原点低通系数。
constexpr double AUDIO_ANCHOR_LOWPASS_ALPHA = 0.05;
// 系数按新观测次数应用，重复读取同一 block 不会加快收敛。

/// @brief 小于该值的音画误差不进入校准。
constexpr double SYNC_DEADZONE_SECONDS = 0.002;
// 死区使用谱面秒，倍率已在音频估计计算中体现。

/// @brief 超过该值的误差视为离散跳转并立即重建锚点。
constexpr double LARGE_ERROR_SECONDS = 0.5;
// 此阈值也用于恢复播放的连续性判定，与正常低通系数职责不同。

/// @brief 规范化播放倍率。
/// @param rate 快照携带的倍率。
/// @return 有限正倍率原样返回，其余回退为正常速度。
[[nodiscard]] double sanitizeRate(double rate) noexcept
{
    // 正倍率也是原点公式中除数非零的前提，不能只检查有限性。
    return std::isfinite(rate) && rate > 0.0 ? rate : 1.0;
}

/// @brief 规范化壁钟值。
/// @param now 待检查的单调时钟秒数。
/// @param fallback 输入非有限时沿用的锚点时钟。
/// @return 可用于本次计算的时钟值。
[[nodiscard]] double sanitizeNow(double now, double fallback) noexcept
{
    // 不读取系统时钟补值，保证所有计算仍处于调用方提供的同一时间基准。
    return std::isfinite(now) ? now : fallback;
}

/// @brief 判断音频线程是否确认了上一次逻辑更新已经观察的控制请求。
/// @param applied 当前已应用邮箱版本。
/// @param lastApplied 上一次观察到的已应用邮箱版本。
/// @param requested 当前请求邮箱版本。
/// @param lastRequested 上一次观察到的请求邮箱版本。
/// @return applied 切换到已观察的当前请求时返回 true。
/// @note 只比较版本，不等待确认；未确认时仍正常推进本地时间。
[[nodiscard]] bool acknowledgesObservedRequest(
    std::uint64_t applied, std::uint64_t lastApplied, std::uint64_t requested,
    std::uint64_t lastRequested) noexcept
{
    // 要求请求版本上一轮已经见过，区分控制确认与本轮刚发起的新跳转。
    return applied != lastApplied && applied == requested &&
           requested == lastRequested;
}

}  // namespace

/// @brief 将音频观测融入连续视觉时间，并处理控制状态变化。
/// @param snapshot 音频层已取得的一致快照，本函数不直接读取音频线程状态。
/// @param nowSteadySeconds 当前逻辑轮次的单调时钟秒数。
/// @param config 周期校准间隔和误差补偿系数。
/// @pre integralFactor 由配置层提供为可用补偿系数。
/// @details 连续外推不要求每次逻辑更新都有新的音频观测。
/// @return 当前谱面时间，单位秒。
/// @warning 每个播放会话的 update
/// 热路径；保持常量计算，不加锁、分配或等待音频。
double PlaybackVisualClock::update(
    const Audio::AudioTimelineClockSnapshot& snapshot, double nowSteadySeconds,
    const Config::SyncConfig& config) noexcept
{
    nowSteadySeconds = sanitizeNow(nowSteadySeconds, m_anchorSteadySeconds);
    if ( !snapshot.valid || snapshot.sampleRate == 0U ) {
        // 暂无可用采样时继续沿已有锚点推演，不能把短暂缺失当成停止或回到零点。
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
    // 零间隔允许每轮校准；非法间隔回退历史默认值，不引入等待或休眠。
    const double syncInterval = std::isfinite(config.syncInterval)
                                    ? std::max(config.syncInterval, 0.0)
                                    : 10.0;

    if ( !m_initialized ) {
        // 第一个有效快照没有可保留的视觉历史，直接建立锚点和版本基线。
        reanchor(snapshot, nowSteadySeconds);
        return m_lastResolvedTime;
    }

    // 状态转换前先按旧倍率推演当前时间，变速时以此点衔接，避免累计时间跳变。
    const double previousResolved = currentTimeAt(nowSteadySeconds);
    const bool   scheduleChanged =
        snapshot.scheduleGeneration != m_lastScheduleGeneration;
    const bool seekRequestChanged = snapshot.seekSequence != m_lastSeekSequence;
    /// @brief 本轮是否确认了上一轮已观察的 Seek 请求。
    const bool seekAcknowledged =
        acknowledgesObservedRequest(snapshot.appliedSeekSequence,
                                    m_lastAppliedSeekSequence,
                                    snapshot.seekSequence,
                                    m_lastSeekSequence);
    /// @brief 本轮是否确认了已观察的播放或暂停请求。
    const bool playbackAcknowledged =
        acknowledgesObservedRequest(snapshot.appliedPlaybackSequence,
                                    m_lastAppliedPlaybackSequence,
                                    snapshot.playbackSequence,
                                    m_lastPlaybackSequence);
    const bool controlAcknowledged = seekAcknowledged || playbackAcknowledged;
    // 控制确认附带的 epoch
    // 变化不一定是新跳转；控制之后再发生的变化才需单独处理。
    const bool postControlDiscontinuity =
        snapshot.epoch != snapshot.controlEpoch;
    const double predictedAtObservation = currentTimeAt(observationTime);
    // 在音频观测时刻比较两条时间线，而不是拿旧音频位置与本轮已推进的视觉位置比较。
    const bool scheduleAckKeepsContinuity =
        scheduleChanged && controlAcknowledged && !postControlDiscontinuity &&
        std::isfinite(rawPosition) && std::isfinite(predictedAtObservation) &&
        std::abs(rawPosition - predictedAtObservation) <= LARGE_ERROR_SECONDS;

    /// @brief 是否必须放弃旧时间原点，而非只调整斜率或状态。
    bool discontinuity = false;
    if ( seekRequestChanged ) {
        // 新 Seek 请求必须立即反映到视觉，即使音频线程还未发布应用确认。
        discontinuity = true;
    } else if ( scheduleChanged ) {
        // 调度替换默认视为离散变化，仅允许已观察控制的近距离确认保持连续。
        discontinuity = !scheduleAckKeepsContinuity;
    } else if ( snapshot.epoch != m_lastEpoch ) {
        discontinuity = !controlAcknowledged || postControlDiscontinuity;
    }

    const bool rateChanged  = std::abs(rate - m_playbackRate) > 1e-9;
    const bool stateChanged = snapshot.state != m_lastState;
    // 控制版本即使未改变状态枚举也需处理，重复播放命令可能带来新的确认时刻。
    const bool playbackCommandChanged =
        snapshot.playbackSequence != m_lastPlaybackSequence;

    if ( discontinuity ) {
        // 真正跳转不经过低通追赶，清除旧时间原点并直接采用新观测。
        reanchor(snapshot, nowSteadySeconds);
        return m_lastResolvedTime;
    }

    if ( rateChanged ) {
        // 速度改变后旧音频原点公式不再适用，保留视觉位置但重启音频原点估计。
        rebase(previousResolved, nowSteadySeconds, rate, isPlaying);
        m_hasSmoothedAudioOrigin = false;
        // 置为到期，使新倍率原点可立即校准，不沿用旧倍率下的等待周期。
        m_lastCalibrationSteadySeconds = nowSteadySeconds - syncInterval;
    } else if ( stateChanged || playbackCommandChanged ) {
        if ( snapshot.state == Audio::AudioTimelinePlaybackState::Paused ) {
            // 暂停发生在观测时刻，将其限制在本锚点到当前逻辑时刻之间，避免多推进一轮。
            /// @brief 暂停实际生效的可接受时刻，限制到当前锚点的有效区间。
            const double transitionTime = std::clamp(
                observationTime, m_anchorSteadySeconds, nowSteadySeconds);
            rebase(
                currentTimeAt(transitionTime), nowSteadySeconds, rate, false);
        } else if ( isPlaying ) {
            // 短距离恢复采用当前视觉位置接续；明显不同的位置仍尊重音频给出的起点。
            double resumedPosition = rawPosition;
            if ( std::abs(resumedPosition - previousResolved) <=
                 LARGE_ERROR_SECONDS ) {
                resumedPosition = previousResolved;
            }
            rebase(resumedPosition, nowSteadySeconds, rate, true);
            if ( m_lastState != Audio::AudioTimelinePlaybackState::Playing ) {
                // 从非播放态恢复时给新观测重新积累原点，不立即沿用暂停前的校准历史。
                m_hasSmoothedAudioOrigin       = false;
                m_lastCalibrationSteadySeconds = nowSteadySeconds;
            }
        } else {
            // 停止等非播放状态固定在快照位置，不再按壁钟外推。
            rebase(rawPosition, nowSteadySeconds, rate, false);
        }
    }

    // 同一音频 block 可被多个逻辑轮次读取，低通仅吸收新发布的观测一次。
    const bool newObservation = snapshot.sequence != m_lastObservationSequence;
    if ( newObservation && isPlaying ) {
        // position = (steady - origin) * rate；滤波原点而非阶梯式 block 位置。
        const double observedOrigin = observationTime - rawPosition / rate;
        if ( std::isfinite(observedOrigin) ) {
            if ( !m_hasSmoothedAudioOrigin ) {
                // 首个原点直接采用，避免从默认零值缓慢逼近一个很大的单调时钟值。
                m_smoothedAudioOriginSeconds = observedOrigin;
                m_hasSmoothedAudioOrigin     = true;
            } else {
                // 每个新 block 只吸收一小部分偏差，减轻观测发布时间抖动。
                m_smoothedAudioOriginSeconds +=
                    (observedOrigin - m_smoothedAudioOriginSeconds) *
                    AUDIO_ANCHOR_LOWPASS_ALPHA;
            }
        }
    }

    double resolved = currentTimeAt(nowSteadySeconds);
    if ( isPlaying && m_hasSmoothedAudioOrigin &&
         nowSteadySeconds - m_lastCalibrationSteadySeconds >= syncInterval ) {
        // 校准也是非阻塞的到期检查；间隔内视觉位置仍持续按锚点推进。
        /// @brief 平滑原点在本轮壁钟时刻对应的音频位置。
        const double audioEstimate =
            (nowSteadySeconds - m_smoothedAudioOriginSeconds) * rate;
        const double error = audioEstimate - resolved;
        // 到期检查后推进周期，死区内也不在后续每轮重复执行这次校准。
        m_lastCalibrationSteadySeconds = nowSteadySeconds;

        if ( std::isfinite(error) && std::abs(error) > LARGE_ERROR_SECONDS ) {
            // 大偏差直接接上音频估计，避免长时间追赶明显失效的视觉锚点。
            rebase(audioEstimate, nowSteadySeconds, rate, true);
        } else if ( std::isfinite(error) &&
                    std::abs(error) > SYNC_DEADZONE_SECONDS ) {
            // 死区外的小偏差按配置比例校正；死区内保持原锚点，避免微小反复抖动。
            resolved += error * static_cast<double>(config.integralFactor);
            rebase(resolved, nowSteadySeconds, rate, true);
        }
    }

    // 校准可能已重建锚点，统一再次解析，返回值与下一轮推演的起点一致。
    resolved = currentTimeAt(nowSteadySeconds);

    // 只有有效快照进入版本历史，无效快照不会吞掉尚未处理的控制变化。
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

/// @brief 只根据锚点和倍率推演谱面时间。
/// @param nowSteadySeconds 目标单调时钟时刻。
/// @return 未初始化返回零，暂停时返回固定位置，播放时返回线性推演值。
/// @warning 高频只读计算，不触发音频采样或周期校准。
/// @note 不更新最近解析记录；需要记录时使用 resolveAt。
double PlaybackVisualClock::currentTimeAt(
    double nowSteadySeconds) const noexcept
{
    if ( !m_initialized ) return 0.0;
    if ( !m_playing || !std::isfinite(nowSteadySeconds) ) {
        return m_anchorPositionSeconds;
    }
    // 对早于锚点的查询不反向外推，避免时钟输入回退造成视觉倒退。
    const double elapsed =
        std::max(nowSteadySeconds - m_anchorSteadySeconds, 0.0);
    const double result = m_anchorPositionSeconds + elapsed * m_playbackRate;
    // 极端输入溢出时返回有限锚点，不能把无穷值继续传给画布坐标换算。
    return std::isfinite(result) ? result : m_anchorPositionSeconds;
}

/// @brief 不接收新音频观测，仅推演并记录一次对外时间。
/// @param nowSteadySeconds 本次解析的单调时钟秒数。
/// @note 不消费快照序列，后续 update 仍能识别尚未处理的音频观测。
/// @return 当前连续谱面时间。
/// @warning follower 每 update 调用；只更新本地值，不等待主会话。
double PlaybackVisualClock::resolveAt(double nowSteadySeconds) noexcept
{
    nowSteadySeconds = sanitizeNow(nowSteadySeconds, m_anchorSteadySeconds);
    const double resolved       = currentTimeAt(nowSteadySeconds);
    m_lastResolvedTime          = resolved;
    m_lastResolvedSteadySeconds = nowSteadySeconds;
    return resolved;
}

/// @brief 查询是否已建立视觉锚点。
/// @return rebase 或有效观测建立锚点后返回真，reset 后返回假。
bool PlaybackVisualClock::initialized() const noexcept
{
    return m_initialized;
}

/// @brief 获取最近一次对外解析对应的单调时钟。
/// @return 壁钟秒数，不是谱面位置或音频观测时间。
double PlaybackVisualClock::lastResolvedSteadyTime() const noexcept
{
    return m_lastResolvedSteadySeconds;
}

/// @brief 以给定连续位置重建视觉锚点。
/// @param positionSeconds 锚点谱面时间，单位秒。
/// @param nowSteadySeconds 与位置对应的单调时钟。
/// @param playbackRate 后续外推倍率。
/// @param playing 是否允许时间继续随壁钟推进。
/// @details 暂停锚点也可存负时间，表示谱面起点之前而非未初始化。
/// @note 不重置音频版本和低通历史，调用方按具体状态转换决定是否清理。
void PlaybackVisualClock::rebase(double positionSeconds,
                                 double nowSteadySeconds, double playbackRate,
                                 bool playing) noexcept
{
    // 位置和壁钟分别规范化，避免坏值污染之后全部推演；不改变正常的负谱面时间。
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

/// @brief 同时清除锚点、控制版本和校准历史，回到未初始化状态。
/// @note 不等同于暂停，下一次有效观测会重新建立时间原点。
void PlaybackVisualClock::reset() noexcept
{
    // 整体恢复默认值，避免仅清锚点却留下旧请求版本而吞掉下一次控制变化。
    *this = PlaybackVisualClock{};
}

/// @brief 用音频快照替换整套视觉锚点及观测历史。
/// @param snapshot 初次有效观测或离散跳转后的音频状态。
/// @details 暂停和停止状态不补偿延迟，避免静止位置随消息延迟漂移。
/// @param nowSteadySeconds 当前逻辑时刻。
/// @note 与 rebase 不同，此入口同步消费快照版本并重启低通校准。
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
        // 音频位置属于观测时刻，补上至当前逻辑时刻的推进，抵消 block 发布延迟。
        position += std::max(nowSteadySeconds - observationTime, 0.0) * rate;
    }
    rebase(position, nowSteadySeconds, rate, playing);

    // 直接建锚同时记住所有请求和确认版本，下一轮不重复处理同一个跳转。
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

    // 新纪元从原始观测估计原点，不将上个纪元的低通结果带过来。
    const double observedOrigin =
        observationTime - snapshot.positionSeconds() / rate;
    m_smoothedAudioOriginSeconds =
        std::isfinite(observedOrigin) ? observedOrigin : nowSteadySeconds;
    // 非播放状态不启用校准，恢复播放后重新采用有效观测。
    m_hasSmoothedAudioOrigin = playing;
}

}  // namespace MMM::Logic
