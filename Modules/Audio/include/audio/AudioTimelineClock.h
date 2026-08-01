#pragma once

#include "audio/AudioTimelineTransport.h"

#include <cmath>
#include <cstdint>

namespace MMM::Audio
{

/// @brief 音频线程发布给逻辑线程的一致时间线时钟快照。
///
/// positionFrame 与 steadyTimeNanoseconds 必须来自同一个已完成音频 block
/// 的发布点。逻辑线程可据此建立连续壁钟，不应直接逐帧显示离散 block 位置。
struct AudioTimelineClockSnapshot {
    /// @brief 最近已完成 block 后下一次输入消费的时间线帧。
    AudioTimelineFrame positionFrame{ 0 };

    /// @brief positionFrame 发布时的 steady_clock 纳秒时间戳。
    std::int64_t steadyTimeNanoseconds{ 0 };

    /// @brief 时间线内部采样率。
    std::uint32_t sampleRate{ 0U };

    /// @brief 当前全局预览播放倍率。
    double playbackRate{ 1.0 };

    /// @brief 控制命令与音频 block 共同决定的有效播放状态。
    AudioTimelinePlaybackState state{ AudioTimelinePlaybackState::Stopped };

    /// @brief Seek、Stop 或循环回绕后的传输纪元。
    std::uint64_t epoch{ 0U };

    /// @brief 本 block 应用调度与控制后、处理循环边界前的传输纪元。
    std::uint64_t controlEpoch{ 0U };

    /// @brief 当前音频线程调度状态在本节点内的单调代次。
    std::uint64_t scheduleGeneration{ 0U };

    /// @brief 最近稳定的 Seek 邮箱版本，用于暂停态即时识别跳转。
    std::uint64_t seekSequence{ 0U };

    /// @brief 音频线程最近已经应用的 Seek 邮箱版本。
    std::uint64_t appliedSeekSequence{ 0U };

    /// @brief 最近稳定的播放命令邮箱版本。
    std::uint64_t playbackSequence{ 0U };

    /// @brief 音频线程最近已经应用的播放命令邮箱版本。
    std::uint64_t appliedPlaybackSequence{ 0U };

    /// @brief 音频线程一致发布的序列锁版本，偶数表示完整快照。
    std::uint64_t sequence{ 0U };

    /// @brief 非循环时间线是否已经生成完最后一段输入。
    bool finished{ false };

    /// @brief 快照是否来自已加载且一致的时间线。
    bool valid{ false };

    /// @brief 将帧位置换算为秒。
    /// @return 采样率有效时返回时间线秒数，否则返回零。
    [[nodiscard]] double positionSeconds() const noexcept
    {
        if ( sampleRate == 0U ) return 0.0;
        return static_cast<double>(positionFrame) /
               static_cast<double>(sampleRate);
    }

    /// @brief 将 steady_clock 纳秒时间戳换算为秒。
    /// @return 有限的 steady_clock 秒数。
    [[nodiscard]] double steadyTimeSeconds() const noexcept
    {
        constexpr double NANOSECONDS_PER_SECOND = 1'000'000'000.0;
        const double     result =
            static_cast<double>(steadyTimeNanoseconds) / NANOSECONDS_PER_SECOND;
        return std::isfinite(result) ? result : 0.0;
    }
};

}  // namespace MMM::Audio
