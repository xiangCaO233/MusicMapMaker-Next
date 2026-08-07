#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace MMM::Canvas
{

/// @brief 使用本地可见边界和判定线锚点投影协作视野时间。
/// @param time 待投影的远端视觉时间。
/// @param visualTime 本地判定线对应的视觉时间。
/// @param visibleTimeStart 本地画布底边对应的时间。
/// @param visibleTimeEnd 本地画布顶边对应的时间。
/// @param judgmentLineY 本地判定线 Y 坐标。
/// @param canvasHeight 本地画布高度。
/// @return 输入有效且边界可映射时返回画布 Y 坐标。
/// @warning UI 热路径：缺少 ScrollSegment 时每个远端边界调用一次；只执行常量
/// 数值计算，禁止加入分配或阻塞操作。
inline std::optional<float> projectCollaborationViewportTime(
    double time, double visualTime, double visibleTimeStart,
    double visibleTimeEnd, float judgmentLineY, float canvasHeight)
{
    if ( !std::isfinite(time) || !std::isfinite(visualTime) ||
         !std::isfinite(visibleTimeStart) || !std::isfinite(visibleTimeEnd) ||
         !std::isfinite(judgmentLineY) || !std::isfinite(canvasHeight) ||
         canvasHeight <= 0.0F ) {
        return std::nullopt;
    }

    constexpr double TIME_EPSILON = 1e-9;
    const auto       between = [](double value, double first, double second) {
        return value >= std::min(first, second) - TIME_EPSILON &&
               value <= std::max(first, second) + TIME_EPSILON;
    };
    const auto interpolate = [](double value,
                                double firstTime,
                                double secondTime,
                                float  firstY,
                                float  secondY) -> std::optional<float> {
        const double denominator = secondTime - firstTime;
        if ( std::abs(denominator) <= TIME_EPSILON ) return std::nullopt;
        const double ratio     = (value - firstTime) / denominator;
        const double projected = static_cast<double>(firstY) +
                                 ratio * static_cast<double>(secondY - firstY);
        if ( !std::isfinite(projected) ) return std::nullopt;
        return static_cast<float>(projected);
    };

    if ( std::abs(time - visualTime) <= TIME_EPSILON ) {
        return judgmentLineY;
    }
    if ( between(time, visibleTimeStart, visualTime) ) {
        return interpolate(
            time, visibleTimeStart, visualTime, canvasHeight, judgmentLineY);
    }
    if ( between(time, visualTime, visibleTimeEnd) ) {
        return interpolate(
            time, visualTime, visibleTimeEnd, judgmentLineY, 0.0F);
    }

    const bool startSide = (visualTime >= visibleTimeStart)
                               ? time < visibleTimeStart
                               : time > visibleTimeStart;
    return startSide
               ? interpolate(time,
                             visibleTimeStart,
                             visualTime,
                             canvasHeight,
                             judgmentLineY)
               : interpolate(
                     time, visualTime, visibleTimeEnd, judgmentLineY, 0.0F);
}

}  // namespace MMM::Canvas
