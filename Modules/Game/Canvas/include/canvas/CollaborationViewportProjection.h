#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace MMM::Canvas
{

/// @brief 远端视口在本地画布中的横向可见范围。
struct CollaborationViewportHorizontalRange {
    /// @brief 裁剪后的左边界。
    float leftX{ 0.0F };
    /// @brief 裁剪后的右边界。
    float rightX{ 0.0F };
};

/// @brief 将协作视野框锚定到本地轨道区并裁剪到画布内。
/// @param localContentLeft 本地相机下协作轨道区左边界。
/// @param localContentRight 本地相机下协作轨道区右边界。
/// @param canvasWidth 本地画布宽度。
/// @return 输入有效时返回经过画布边缘裁剪的范围。
/// @warning UI 热路径：每个远端参与者调用一次；只执行常量数值计算，禁止加入
/// 分配或阻塞操作。
inline std::optional<CollaborationViewportHorizontalRange>
projectCollaborationViewportHorizontalRange(float localContentLeft,
                                            float localContentRight,
                                            float canvasWidth)
{
    if ( !std::isfinite(localContentLeft) ||
         !std::isfinite(localContentRight) || !std::isfinite(canvasWidth) ||
         canvasWidth <= 4.0F || localContentRight <= localContentLeft ) {
        return std::nullopt;
    }

    const double rawLeft  = static_cast<double>(localContentLeft);
    const double rawRight = static_cast<double>(localContentRight);
    if ( !std::isfinite(rawLeft) || !std::isfinite(rawRight) ) {
        return std::nullopt;
    }

    constexpr float EDGE_PADDING          = 2.0F;
    constexpr float MINIMUM_VISIBLE_WIDTH = 3.0F;
    const float     maximumX              = canvasWidth - EDGE_PADDING;
    if ( rawRight <= static_cast<double>(EDGE_PADDING) ) {
        return CollaborationViewportHorizontalRange{
            EDGE_PADDING, EDGE_PADDING + MINIMUM_VISIBLE_WIDTH
        };
    }
    if ( rawLeft >= static_cast<double>(maximumX) ) {
        return CollaborationViewportHorizontalRange{
            maximumX - MINIMUM_VISIBLE_WIDTH, maximumX
        };
    }

    float leftX =
        std::clamp(static_cast<float>(rawLeft), EDGE_PADDING, maximumX);
    float rightX =
        std::clamp(static_cast<float>(rawRight), EDGE_PADDING, maximumX);
    if ( rightX - leftX < MINIMUM_VISIBLE_WIDTH ) {
        if ( leftX <= EDGE_PADDING ) {
            rightX = std::min(maximumX, leftX + MINIMUM_VISIBLE_WIDTH);
        } else {
            leftX = std::max(EDGE_PADDING, rightX - MINIMUM_VISIBLE_WIDTH);
        }
    }
    return CollaborationViewportHorizontalRange{ leftX, rightX };
}

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

/// @brief 将本地画布 Y 坐标反投影为协作视野边界时间。
/// @param canvasY 待反投影的本地画布 Y 坐标。
/// @param visualTime 本地判定线对应的视觉时间。
/// @param visibleTimeStart 本地整幅画布底边对应的时间。
/// @param visibleTimeEnd 本地整幅画布顶边对应的时间。
/// @param judgmentLineY 本地判定线 Y 坐标。
/// @param canvasHeight 本地画布高度。
/// @return 输入有效且锚点可映射时返回视觉时间。
/// @warning UI 热路径：发布本地协作视野时调用两次；只执行常量数值计算，
/// 禁止加入分配或阻塞操作。
inline std::optional<double> unprojectCollaborationViewportTime(
    float canvasY, double visualTime, double visibleTimeStart,
    double visibleTimeEnd, float judgmentLineY, float canvasHeight)
{
    if ( !std::isfinite(canvasY) || !std::isfinite(visualTime) ||
         !std::isfinite(visibleTimeStart) || !std::isfinite(visibleTimeEnd) ||
         !std::isfinite(judgmentLineY) || !std::isfinite(canvasHeight) ||
         canvasHeight <= 0.0F || judgmentLineY < 0.0F ||
         judgmentLineY > canvasHeight ) {
        return std::nullopt;
    }

    constexpr double POSITION_EPSILON = 1e-6;
    const auto interpolate = [](float  value,
                                float  firstY,
                                float  secondY,
                                double firstTime,
                                double secondTime) -> std::optional<double> {
        const double denominator =
            static_cast<double>(secondY) - static_cast<double>(firstY);
        if ( std::abs(denominator) <= POSITION_EPSILON ) {
            return std::nullopt;
        }
        const double ratio =
            (static_cast<double>(value) - static_cast<double>(firstY)) /
            denominator;
        const double time = firstTime + ratio * (secondTime - firstTime);
        if ( !std::isfinite(time) ) return std::nullopt;
        return time;
    };

    if ( std::abs(static_cast<double>(canvasY - judgmentLineY)) <=
         POSITION_EPSILON ) {
        return visualTime;
    }
    if ( canvasY > judgmentLineY ) {
        return interpolate(
            canvasY, canvasHeight, judgmentLineY, visibleTimeStart, visualTime);
    }
    return interpolate(
        canvasY, judgmentLineY, 0.0F, visualTime, visibleTimeEnd);
}

}  // namespace MMM::Canvas
