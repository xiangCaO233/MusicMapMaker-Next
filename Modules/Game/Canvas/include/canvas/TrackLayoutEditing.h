#pragma once

#include "config/visual/TrackLayoutConfig.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MMM::Canvas
{

/// @brief 轨道布局单边允许保留的最小归一化跨度。
inline constexpr float TRACK_LAYOUT_MIN_SPAN = 0.01f;

/// @brief 轨道布局编辑器当前命中的拖拽部位。
enum class TrackLayoutDragHandle {
    None,          ///< 未命中任何可拖拽部位。
    Left,          ///< 左边界。
    Top,           ///< 上边界。
    Right,         ///< 右边界。
    Bottom,        ///< 下边界。
    JudgmentLine,  ///< 判定线位置把手。
    Move           ///< 整体移动把手。
};

/// @brief 将判定线位置规整到画布归一化范围。
/// @param position 待规整的判定线位置。
/// @return 位于 `[0,1]` 的判定线位置；非有限值恢复为默认位置。
/// @warning UI 热路径纯计算：布局工具每帧调用；不得引入分配或阻塞操作。
[[nodiscard]] inline float sanitizeJudgmentLinePosition(float position)
{
    if ( !std::isfinite(position) ) {
        return 0.85f;
    }
    return std::clamp(position, 0.0f, 1.0f);
}

/// @brief 将轨道布局规整到可安全编辑的合法范围。
/// @param layout 待规整布局。
/// @return 满足四边位于 `[0,1]` 且宽高不小于最小跨度的布局。
/// @warning UI 热路径纯计算：布局工具每帧调用；不得引入分配或阻塞操作。
[[nodiscard]] inline Config::TrackLayout sanitizeTrackLayout(
    Config::TrackLayout layout)
{
    const Config::TrackLayout fallback;
    layout.left  = std::isfinite(layout.left) ? layout.left : fallback.left;
    layout.top   = std::isfinite(layout.top) ? layout.top : fallback.top;
    layout.right = std::isfinite(layout.right) ? layout.right : fallback.right;
    layout.bottom =
        std::isfinite(layout.bottom) ? layout.bottom : fallback.bottom;

    layout.left = std::clamp(layout.left, 0.0f, 1.0f - TRACK_LAYOUT_MIN_SPAN);
    layout.right =
        std::clamp(layout.right, layout.left + TRACK_LAYOUT_MIN_SPAN, 1.0f);
    layout.top = std::clamp(layout.top, 0.0f, 1.0f - TRACK_LAYOUT_MIN_SPAN);
    layout.bottom =
        std::clamp(layout.bottom, layout.top + TRACK_LAYOUT_MIN_SPAN, 1.0f);
    return layout;
}

/// @brief 按指针位置拖动轨道布局的一条边界。
/// @param layout 拖动开始时的布局。
/// @param handle 当前拖动边界。
/// @param normalizedPointer 指针在对应轴上的归一化坐标。
/// @return 应用边界约束后的布局。
/// @warning UI 热路径纯计算：边界拖动期间每帧调用；不得引入分配或阻塞操作。
[[nodiscard]] inline Config::TrackLayout resizeTrackLayout(
    Config::TrackLayout layout, TrackLayoutDragHandle handle,
    float normalizedPointer)
{
    layout = sanitizeTrackLayout(layout);
    if ( !std::isfinite(normalizedPointer) ) {
        return layout;
    }

    switch ( handle ) {
    case TrackLayoutDragHandle::Left:
        layout.left = std::clamp(
            normalizedPointer, 0.0f, layout.right - TRACK_LAYOUT_MIN_SPAN);
        break;
    case TrackLayoutDragHandle::Top:
        layout.top = std::clamp(
            normalizedPointer, 0.0f, layout.bottom - TRACK_LAYOUT_MIN_SPAN);
        break;
    case TrackLayoutDragHandle::Right:
        layout.right = std::clamp(
            normalizedPointer, layout.left + TRACK_LAYOUT_MIN_SPAN, 1.0f);
        break;
    case TrackLayoutDragHandle::Bottom:
        layout.bottom = std::clamp(
            normalizedPointer, layout.top + TRACK_LAYOUT_MIN_SPAN, 1.0f);
        break;
    case TrackLayoutDragHandle::JudgmentLine:
    case TrackLayoutDragHandle::Move:
    case TrackLayoutDragHandle::None: break;
    }
    return layout;
}

/// @brief 平移整个轨道布局并保持其宽高不变。
/// @param layout 拖动开始时的布局。
/// @param deltaX 指针相对起点的归一化横向位移。
/// @param deltaY 指针相对起点的归一化纵向位移。
/// @return 保持完整矩形位于 `[0,1]` 内的布局。
/// @warning UI 热路径纯计算：整体拖动期间每帧调用；不得引入分配或阻塞操作。
[[nodiscard]] inline Config::TrackLayout moveTrackLayout(
    Config::TrackLayout layout, float deltaX, float deltaY)
{
    layout = sanitizeTrackLayout(layout);
    if ( !std::isfinite(deltaX) || !std::isfinite(deltaY) ) {
        return layout;
    }

    const float width  = layout.right - layout.left;
    const float height = layout.bottom - layout.top;
    const float left   = std::clamp(layout.left + deltaX, 0.0f, 1.0f - width);
    const float top    = std::clamp(layout.top + deltaY, 0.0f, 1.0f - height);
    layout.left        = left;
    layout.right       = left + width;
    layout.top         = top;
    layout.bottom      = top + height;
    return layout;
}

/// @brief 将整个轨道布局中心移动到指定画布像素坐标。
/// @param layout 当前轨道布局。
/// @param centerX 目标中心相对画布左侧的像素坐标。
/// @param centerY 目标中心相对画布顶部的像素坐标。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @return 保持宽高且完整位于画布内的布局。
/// @warning UI 热路径纯计算：整体轨道吸附期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::TrackLayout moveTrackLayoutToPixelCenter(
    Config::TrackLayout layout, float centerX, float centerY,
    float viewportWidth, float viewportHeight)
{
    layout = sanitizeTrackLayout(layout);
    if ( !std::isfinite(centerX) || !std::isfinite(centerY) ||
         !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) ||
         viewportWidth <= 0.0f || viewportHeight <= 0.0f ) {
        return layout;
    }

    const float currentCenterX = (layout.left + layout.right) * 0.5f;
    const float currentCenterY = (layout.top + layout.bottom) * 0.5f;
    return moveTrackLayout(layout,
                           centerX / viewportWidth - currentCenterX,
                           centerY / viewportHeight - currentCenterY);
}

/// @brief 在画布像素坐标中命中轨道边界、判定线或整体移动把手。
/// @param layout 当前轨道布局。
/// @param judgmentLinePosition 当前判定线归一化位置。
/// @param pointerX 指针相对画布左侧的像素坐标。
/// @param pointerY 指针相对画布顶部的像素坐标。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @param edgeHitRadius 边界两侧的命中半径。
/// @param moveHandleRadius 中心整体移动把手的命中半径。
/// @return 命中的拖拽部位；未命中时返回 None。
/// @warning UI 热路径纯计算：布局工具每帧调用；只允许常量级数值比较。
[[nodiscard]] inline TrackLayoutDragHandle hitTestTrackLayout(
    Config::TrackLayout layout, float judgmentLinePosition, float pointerX,
    float pointerY, float viewportWidth, float viewportHeight,
    float edgeHitRadius, float moveHandleRadius)
{
    if ( !std::isfinite(pointerX) || !std::isfinite(pointerY) ||
         !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) ||
         !std::isfinite(edgeHitRadius) || !std::isfinite(moveHandleRadius) ||
         viewportWidth <= 0.0f || viewportHeight <= 0.0f ||
         edgeHitRadius < 0.0f || moveHandleRadius < 0.0f ) {
        return TrackLayoutDragHandle::None;
    }

    layout              = sanitizeTrackLayout(layout);
    const float left    = layout.left * viewportWidth;
    const float right   = layout.right * viewportWidth;
    const float top     = layout.top * viewportHeight;
    const float bottom  = layout.bottom * viewportHeight;
    const float centerX = (left + right) * 0.5f;
    const float centerY = (top + bottom) * 0.5f;
    const float judgmentLineY =
        sanitizeJudgmentLinePosition(judgmentLinePosition) * viewportHeight;

    if ( std::abs(pointerX - centerX) <= moveHandleRadius &&
         std::abs(pointerY - centerY) <= moveHandleRadius ) {
        return TrackLayoutDragHandle::Move;
    }
    if ( std::abs(pointerX - right) <= moveHandleRadius &&
         std::abs(pointerY - judgmentLineY) <= edgeHitRadius ) {
        return TrackLayoutDragHandle::JudgmentLine;
    }

    TrackLayoutDragHandle closest = TrackLayoutDragHandle::None;
    float closestDistance         = std::numeric_limits<float>::infinity();
    auto  consider =
        [&](TrackLayoutDragHandle handle, float distance, bool withinSpan) {
            if ( withinSpan && distance <= edgeHitRadius &&
                 distance < closestDistance ) {
                closest         = handle;
                closestDistance = distance;
            }
        };

    consider(
        TrackLayoutDragHandle::Left,
        std::abs(pointerX - left),
        pointerY >= top - edgeHitRadius && pointerY <= bottom + edgeHitRadius);
    consider(
        TrackLayoutDragHandle::Right,
        std::abs(pointerX - right),
        pointerY >= top - edgeHitRadius && pointerY <= bottom + edgeHitRadius);
    consider(
        TrackLayoutDragHandle::Top,
        std::abs(pointerY - top),
        pointerX >= left - edgeHitRadius && pointerX <= right + edgeHitRadius);
    consider(
        TrackLayoutDragHandle::Bottom,
        std::abs(pointerY - bottom),
        pointerX >= left - edgeHitRadius && pointerX <= right + edgeHitRadius);
    return closest;
}

}  // namespace MMM::Canvas
