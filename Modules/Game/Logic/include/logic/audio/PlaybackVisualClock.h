#pragma once

#include "audio/AudioTimelineClock.h"
#include "config/EditorSettings.h"

#include <cstdint>

namespace MMM::Logic
{

/// @brief 将离散音频 block 锚点转换为连续且按历史策略周期校准的视觉播放时钟。
class PlaybackVisualClock final
{
public:
    /// @brief 使用最新一致音频快照推进视觉时钟。
    /// @param snapshot 音频线程发布的一致 block 快照。
    /// @param nowSteadySeconds 当前逻辑帧的 steady_clock 秒数。
    /// @param config 音画同步校准参数。
    /// @return 当前连续谱面时间，单位秒。
    /// @warning
    /// 逻辑热路径：每个播放中 Session update 调用；只允许常量时间计算，
    /// 禁止分配、锁、文件访问或无界重试。
    [[nodiscard]] double update(
        const Audio::AudioTimelineClockSnapshot& snapshot,
        double nowSteadySeconds, const Config::SyncConfig& config) noexcept;

    /// @brief 查询指定壁钟时刻的连续时间，不接收新的音频校准。
    /// @param nowSteadySeconds steady_clock 秒数。
    /// @return 当前推演时间，单位秒。
    [[nodiscard]] double currentTimeAt(double nowSteadySeconds) const noexcept;

    /// @brief 在不接收音频校准的情况下解析并记录指定壁钟时刻。
    /// @param nowSteadySeconds steady_clock 秒数。
    /// @return 当前推演时间，单位秒。
    /// @warning
    /// 逻辑热路径：同步 follower 每次 update 调用；只做常量级壁钟计算。
    [[nodiscard]] double resolveAt(double nowSteadySeconds) noexcept;

    /// @brief 判断当前是否已有可用于连续推演的有效锚点。
    [[nodiscard]] bool initialized() const noexcept;

    /// @brief 获取最近一次对外解析 currentTime 时对应的 steady_clock 秒数。
    [[nodiscard]] double lastResolvedSteadyTime() const noexcept;

    /// @brief 在速度或控制权切换时保持当前位置并重建连续锚点。
    /// @param positionSeconds 新锚点谱面时间。
    /// @param nowSteadySeconds 新锚点壁钟时间。
    /// @param playbackRate 新播放倍率。
    /// @param playing 是否继续按壁钟推进。
    void rebase(double positionSeconds, double nowSteadySeconds,
                double playbackRate, bool playing) noexcept;

    /// @brief 清除全部锚点与校准历史。
    void reset() noexcept;

private:
    /// @brief 将指定音频观测直接建立为新纪元锚点。
    void reanchor(const Audio::AudioTimelineClockSnapshot& snapshot,
                  double nowSteadySeconds) noexcept;

    /// @brief 当前是否已拥有可推演锚点。
    bool m_initialized{ false };
    /// @brief 锚点对应的谱面时间。
    double m_anchorPositionSeconds{ 0.0 };
    /// @brief 锚点对应的 steady_clock 时间。
    double m_anchorSteadySeconds{ 0.0 };
    /// @brief 当前谱面时间相对壁钟的推进倍率。
    double m_playbackRate{ 1.0 };
    /// @brief 当前锚点是否随壁钟推进。
    bool m_playing{ false };

    /// @brief 低通后的音频时间原点 `steady - position / rate`。
    double m_smoothedAudioOriginSeconds{ 0.0 };
    /// @brief 是否已有可用于周期校准的低通原点。
    bool m_hasSmoothedAudioOrigin{ false };
    /// @brief 上一次执行周期校准的壁钟时间。
    double m_lastCalibrationSteadySeconds{ 0.0 };
    /// @brief m_lastResolvedTime 对应的逻辑 update 壁钟时间。
    double m_lastResolvedSteadySeconds{ 0.0 };

    /// @brief 上一次接收的音频发布序列。
    std::uint64_t m_lastObservationSequence{ 0U };
    /// @brief 上一次接收的 transport 纪元。
    std::uint64_t m_lastEpoch{ 0U };
    /// @brief 上一次接收的音频调度代次。
    std::uint64_t m_lastScheduleGeneration{ 0U };
    /// @brief 上一次接收的 Seek 邮箱序列。
    std::uint64_t m_lastSeekSequence{ 0U };
    /// @brief 上一次接收的已应用 Seek 邮箱序列。
    std::uint64_t m_lastAppliedSeekSequence{ 0U };
    /// @brief 上一次接收的播放控制邮箱序列。
    std::uint64_t m_lastPlaybackSequence{ 0U };
    /// @brief 上一次接收的已应用播放控制邮箱序列。
    std::uint64_t m_lastAppliedPlaybackSequence{ 0U };
    /// @brief 上一次接收的有效播放状态。
    Audio::AudioTimelinePlaybackState m_lastState{
        Audio::AudioTimelinePlaybackState::Stopped
    };
    /// @brief 上一次 update 已对外返回的时间。
    double m_lastResolvedTime{ 0.0 };
};

}  // namespace MMM::Logic
