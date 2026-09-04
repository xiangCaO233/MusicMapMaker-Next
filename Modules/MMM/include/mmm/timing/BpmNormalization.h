#pragma once

#include <algorithm>
#include <cmath>

namespace MMM
{

/// @brief BPM 安全计算范围的下边界。
inline constexpr double MIN_NORMALIZED_BPM = 0.1;

/// @brief BPM 安全计算范围的上边界。
inline constexpr double MAX_NORMALIZED_BPM = 10000.0;

/// @brief 无法从原值判断收敛方向时使用的默认 BPM。
inline constexpr double DEFAULT_NORMALIZED_BPM = 120.0;

/// @brief 将 BPM 按原值方向收敛到安全计算范围。
/// @param bpm 待规范化的 BPM；有限越界值和无穷值收敛到最近边界。
/// @param nanFallback 原值为 NaN 时使用的回退值，该值也会按同一范围规范化。
/// @return 位于 [0.1, 10000.0] 内的 BPM。
[[nodiscard]] inline double normalizeBpmValue(
    double bpm, double nanFallback = DEFAULT_NORMALIZED_BPM) noexcept
{
    if ( std::isnan(bpm) ) {
        bpm = std::isnan(nanFallback) ? DEFAULT_NORMALIZED_BPM : nanFallback;
    }
    return std::clamp(bpm, MIN_NORMALIZED_BPM, MAX_NORMALIZED_BPM);
}

}  // namespace MMM
