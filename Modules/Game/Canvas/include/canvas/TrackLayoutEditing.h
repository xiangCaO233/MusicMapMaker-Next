#pragma once

#include "config/visual/TrackLayoutConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

/// @brief 可独立调整的主画布辅助区域。
enum class AuxiliaryLayoutRegion : std::uint8_t {
    None = 0,    ///< 未选中辅助区域。
    Draft,       ///< 草稿轨道区。
    Annotation,  ///< 批注区。
    Bgm,         ///< BGM 轨道区。
};

/// @brief 辅助区域横向拖动句柄。
enum class HorizontalRegionDragHandle : std::uint8_t {
    None = 0,  ///< 未命中辅助区域。
    Left,      ///< 调整左边界与宽度。
    Right,     ///< 调整右边界与宽度。
    Move,      ///< 仅移动 X 位置。
};

/// @brief 已解析的辅助区域归一化横向边界。
struct HorizontalRegionBounds {
    /// @brief 左边界比例。
    float left{ 0.0F };
    /// @brief 区域总宽度比例。
    float width{ 0.1F };
    /// @brief 返回右边界比例。
    [[nodiscard]] float right() const { return left + width; }
};

/// @brief 规整辅助区域横向边界。
/// @param bounds 待规整边界。
/// @return 保持正宽度且数值有限的边界。
[[nodiscard]] inline HorizontalRegionBounds sanitizeHorizontalRegionBounds(
    HorizontalRegionBounds bounds)
{
    // 区域可以位于当前视口之外，以便相机横向移动后继续访问。
    constexpr float minPosition = -4.0F;
    constexpr float maxPosition = 4.0F;
    constexpr float minWidth    = 0.005F;
    constexpr float maxWidth    = 4.0F;
    if ( !std::isfinite(bounds.left) ) bounds.left = 0.0F;
    if ( !std::isfinite(bounds.width) ) bounds.width = 0.1F;
    bounds.left  = std::clamp(bounds.left, minPosition, maxPosition);
    bounds.width = std::clamp(bounds.width, minWidth, maxWidth);
    return bounds;
}

/// @brief 横向缩放辅助区域，不修改任何纵向配置。
/// @param start 拖动开始时的边界。
/// @param handle 左侧或右侧句柄。
/// @param pointerX 当前归一化横坐标。
/// @return 缩放后的有效边界。
[[nodiscard]] inline HorizontalRegionBounds resizeHorizontalRegion(
    HorizontalRegionBounds start, HorizontalRegionDragHandle handle,
    float pointerX)
{
    start = sanitizeHorizontalRegionBounds(start);
    if ( !std::isfinite(pointerX) ) return start;
    constexpr float minWidth = 0.005F;
    if ( handle == HorizontalRegionDragHandle::Left ) {
        // 右边界固定，左边界不会越过最小宽度。
        const float right = start.right();
        start.left        = std::min(pointerX, right - minWidth);
        start.width       = right - start.left;
    } else if ( handle == HorizontalRegionDragHandle::Right ) {
        // 左边界固定，只更新区域总宽度。
        start.width = std::max(minWidth, pointerX - start.left);
    }
    return sanitizeHorizontalRegionBounds(start);
}

/// @brief 横向移动辅助区域并保持宽度。
/// @param start 拖动开始时的边界。
/// @param deltaX 归一化横向位移。
/// @return 仅左边界变化的有效边界。
[[nodiscard]] inline HorizontalRegionBounds moveHorizontalRegion(
    HorizontalRegionBounds start, float deltaX)
{
    start = sanitizeHorizontalRegionBounds(start);
    if ( !std::isfinite(deltaX) ) return start;
    start.left += deltaX;
    return sanitizeHorizontalRegionBounds(start);
}

/// @brief 在像素空间命中辅助区域横向句柄。
/// @param bounds 归一化横向边界。
/// @param top 顶部像素边界，来自主轨道区。
/// @param bottom 底部像素边界，来自主轨道区。
/// @param pointerX 指针像素横坐标。
/// @param pointerY 指针像素纵坐标。
/// @param viewportWidth 逻辑视口宽度。
/// @param edgeHitRadius 边缘命中半径。
/// @param moveHandleRadius 中心移动句柄半径。
/// @return 最近的横向句柄；纵向边界只参与命中，不可编辑。
[[nodiscard]] inline HorizontalRegionDragHandle hitTestHorizontalRegion(
    HorizontalRegionBounds bounds, float top, float bottom, float pointerX,
    float pointerY, float viewportWidth, float edgeHitRadius,
    float moveHandleRadius)
{
    bounds = sanitizeHorizontalRegionBounds(bounds);
    if ( viewportWidth <= 0.0F || pointerY < top - edgeHitRadius ||
         pointerY > bottom + edgeHitRadius ) {
        return HorizontalRegionDragHandle::None;
    }
    const float left   = bounds.left * viewportWidth;
    const float right  = bounds.right() * viewportWidth;
    const float center = (left + right) * 0.5F;
    // 边缘优先于中心，窄区域仍可稳定缩放。
    if ( std::abs(pointerX - left) <= edgeHitRadius ) {
        return HorizontalRegionDragHandle::Left;
    }
    if ( std::abs(pointerX - right) <= edgeHitRadius ) {
        return HorizontalRegionDragHandle::Right;
    }
    const float centerY = (top + bottom) * 0.5F;
    if ( std::abs(pointerX - center) <= moveHandleRadius &&
         std::abs(pointerY - centerY) <= moveHandleRadius ) {
        return HorizontalRegionDragHandle::Move;
    }
    return HorizontalRegionDragHandle::None;
}

}  // namespace MMM::Canvas
