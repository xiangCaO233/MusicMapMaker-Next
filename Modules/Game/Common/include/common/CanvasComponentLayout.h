#pragma once

#include "config/visual/CanvasComponentConfig.h"
#include <algorithm>
#include <cmath>

namespace MMM::Logic
{

/// @brief 可选画布组件的拖动部位。
enum class CanvasComponentDragHandle {
    None,
    Move,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

/// @brief 允许保存的最小字号高度比例。
inline constexpr float CANVAS_COMPONENT_MIN_FONT_SIZE_RATIO = 0.0125f;
/// @brief 允许保存的最大字号高度比例。
inline constexpr float CANVAS_COMPONENT_MAX_FONT_SIZE_RATIO = 0.25f;

/// @brief 画布组件在当前视口中的像素边界。
struct CanvasComponentBounds {
    float left{ 0.0f };
    float top{ 0.0f };
    float right{ 0.0f };
    float bottom{ 0.0f };

    /// @brief 取得边界宽度。
    /// @return 非负像素宽度。
    [[nodiscard]] float width() const { return std::max(0.0f, right - left); }

    /// @brief 取得边界高度。
    /// @return 非负像素高度。
    [[nodiscard]] float height() const { return std::max(0.0f, bottom - top); }

    /// @brief 判断像素点是否位于边界内。
    /// @param x 画布局部横坐标。
    /// @param y 画布局部纵坐标。
    /// @return 点位于边界内时返回 true。
    [[nodiscard]] bool contains(float x, float y) const
    {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
};

/// @brief 画布组件布局计算使用的二维像素点。
struct CanvasComponentPoint {
    /// @brief 横坐标。
    float x{ 0.0f };
    /// @brief 纵坐标。
    float y{ 0.0f };
};

/// @brief 规整画布组件的归一化锚点。
/// @param placement 待规整布局。
/// @return 锚点有限且处于画布范围内的布局。
[[nodiscard]] inline Config::CanvasComponentPlacement
sanitizeCanvasComponentPlacement(Config::CanvasComponentPlacement placement)
{
    const Config::CanvasComponentPlacement fallback;
    if ( !std::isfinite(placement.anchorX) ) {
        placement.anchorX = fallback.anchorX;
    }
    if ( !std::isfinite(placement.anchorY) ) {
        placement.anchorY = fallback.anchorY;
    }
    if ( !std::isfinite(placement.fontSizeRatio) ) {
        placement.fontSizeRatio = fallback.fontSizeRatio;
    }
    placement.anchorX       = std::clamp(placement.anchorX, 0.0f, 1.0f);
    placement.anchorY       = std::clamp(placement.anchorY, 0.0f, 1.0f);
    placement.fontSizeRatio = std::clamp(placement.fontSizeRatio,
                                         CANVAS_COMPONENT_MIN_FONT_SIZE_RATIO,
                                         CANVAS_COMPONENT_MAX_FONT_SIZE_RATIO);
    return placement;
}

/// @brief 计算画布组件的像素边界。
/// @param placement 组件布局。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @param contentWidth 组件内容宽度。
/// @param contentHeight 组件内容高度。
/// @return 完整限制在画布内的组件边界。
/// @warning 热路径：布局编辑和快照生成时调用；只允许常量级数值计算。
[[nodiscard]] inline CanvasComponentBounds canvasComponentBounds(
    const Config::CanvasComponentPlacement& placement, float viewportWidth,
    float viewportHeight, float contentWidth, float contentHeight)
{
    if ( !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) ||
         viewportWidth <= 0.0f || viewportHeight <= 0.0f ) {
        return {};
    }

    const float width = std::clamp(
        std::isfinite(contentWidth) ? contentWidth : 0.0f, 0.0f, viewportWidth);
    const float height =
        std::clamp(std::isfinite(contentHeight) ? contentHeight : 0.0f,
                   0.0f,
                   viewportHeight);

    const auto  sanitized  = sanitizeCanvasComponentPlacement(placement);
    const float halfWidth  = width * 0.5f;
    const float halfHeight = height * 0.5f;
    const float centerX    = width >= viewportWidth
                                 ? viewportWidth * 0.5f
                                 : std::clamp(sanitized.anchorX * viewportWidth,
                                           halfWidth,
                                           viewportWidth - halfWidth);
    const float centerY    = height >= viewportHeight
                                 ? viewportHeight * 0.5f
                                 : std::clamp(sanitized.anchorY * viewportHeight,
                                           halfHeight,
                                           viewportHeight - halfHeight);
    return { centerX - halfWidth,
             centerY - halfHeight,
             centerX + halfWidth,
             centerY + halfHeight };
}

/// @brief 将画布组件移动到指定像素中心并保持完整可见。
/// @param placement 原布局。
/// @param centerX 目标像素中心横坐标。
/// @param centerY 目标像素中心纵坐标。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @param contentWidth 组件内容宽度。
/// @param contentHeight 组件内容高度。
/// @return 更新后的归一化布局。
/// @warning 热路径：组件拖动期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::CanvasComponentPlacement moveCanvasComponent(
    Config::CanvasComponentPlacement placement, float centerX, float centerY,
    float viewportWidth, float viewportHeight, float contentWidth,
    float contentHeight)
{
    if ( viewportWidth <= 0.0f || viewportHeight <= 0.0f ||
         !std::isfinite(centerX) || !std::isfinite(centerY) ) {
        return sanitizeCanvasComponentPlacement(placement);
    }

    placement.anchorX = centerX / viewportWidth;
    placement.anchorY = centerY / viewportHeight;
    placement         = sanitizeCanvasComponentPlacement(placement);
    const auto bounds = canvasComponentBounds(
        placement, viewportWidth, viewportHeight, contentWidth, contentHeight);
    placement.anchorX = ((bounds.left + bounds.right) * 0.5f) / viewportWidth;
    placement.anchorY = ((bounds.top + bounds.bottom) * 0.5f) / viewportHeight;
    return sanitizeCanvasComponentPlacement(placement);
}

/// @brief 命中组件移动区域或四角缩放把手。
/// @param bounds 组件像素边界。
/// @param pointerX 指针横坐标。
/// @param pointerY 指针纵坐标。
/// @param cornerRadius 四角命中半径。
/// @return 命中的拖动部位。
[[nodiscard]] inline CanvasComponentDragHandle hitTestCanvasComponent(
    const CanvasComponentBounds& bounds, float pointerX, float pointerY,
    float cornerRadius)
{
    const auto nearCorner = [&](float x, float y) {
        return std::abs(pointerX - x) <= cornerRadius &&
               std::abs(pointerY - y) <= cornerRadius;
    };
    if ( nearCorner(bounds.left, bounds.top) ) {
        return CanvasComponentDragHandle::TopLeft;
    }
    if ( nearCorner(bounds.right, bounds.top) ) {
        return CanvasComponentDragHandle::TopRight;
    }
    if ( nearCorner(bounds.left, bounds.bottom) ) {
        return CanvasComponentDragHandle::BottomLeft;
    }
    if ( nearCorner(bounds.right, bounds.bottom) ) {
        return CanvasComponentDragHandle::BottomRight;
    }
    return bounds.contains(pointerX, pointerY)
               ? CanvasComponentDragHandle::Move
               : CanvasComponentDragHandle::None;
}

/// @brief 从拖动部位取得固定不动的对角点。
/// @param bounds 拖动开始时的组件边界。
/// @param handle 当前拖动部位。
/// @return 缩放时保持固定的对角点。
[[nodiscard]] inline CanvasComponentPoint canvasComponentOppositeCorner(
    const CanvasComponentBounds& bounds, CanvasComponentDragHandle handle)
{
    switch ( handle ) {
    case CanvasComponentDragHandle::TopLeft:
        return { bounds.right, bounds.bottom };
    case CanvasComponentDragHandle::TopRight:
        return { bounds.left, bounds.bottom };
    case CanvasComponentDragHandle::BottomLeft:
        return { bounds.right, bounds.top };
    case CanvasComponentDragHandle::BottomRight:
        return { bounds.left, bounds.top };
    case CanvasComponentDragHandle::None:
    case CanvasComponentDragHandle::Move: break;
    }
    return { (bounds.left + bounds.right) * 0.5f,
             (bounds.top + bounds.bottom) * 0.5f };
}

/// @brief 按四角拖动结果等比调整组件字号和中心位置。
/// @param placement 拖动开始时的布局。
/// @param handle 当前缩放把手。
/// @param startBounds 拖动开始时的组件边界。
/// @param pointerX 当前指针横坐标。
/// @param pointerY 当前指针纵坐标。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @return 更新后的布局。
/// @warning 热路径：组件缩放期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::CanvasComponentPlacement resizeCanvasComponent(
    Config::CanvasComponentPlacement placement,
    CanvasComponentDragHandle handle, const CanvasComponentBounds& startBounds,
    float pointerX, float pointerY, float viewportWidth, float viewportHeight)
{
    placement = sanitizeCanvasComponentPlacement(placement);
    if ( handle == CanvasComponentDragHandle::None ||
         handle == CanvasComponentDragHandle::Move ||
         startBounds.width() <= 0.0f || startBounds.height() <= 0.0f ||
         viewportWidth <= 0.0f || viewportHeight <= 0.0f ) {
        return placement;
    }

    const CanvasComponentPoint opposite =
        canvasComponentOppositeCorner(startBounds, handle);
    CanvasComponentPoint startCorner;
    switch ( handle ) {
    case CanvasComponentDragHandle::TopLeft:
        startCorner = { startBounds.left, startBounds.top };
        break;
    case CanvasComponentDragHandle::TopRight:
        startCorner = { startBounds.right, startBounds.top };
        break;
    case CanvasComponentDragHandle::BottomLeft:
        startCorner = { startBounds.left, startBounds.bottom };
        break;
    case CanvasComponentDragHandle::BottomRight:
        startCorner = { startBounds.right, startBounds.bottom };
        break;
    case CanvasComponentDragHandle::None:
    case CanvasComponentDragHandle::Move: return placement;
    }

    const CanvasComponentPoint startVector{ startCorner.x - opposite.x,
                                            startCorner.y - opposite.y };
    const CanvasComponentPoint pointerVector{ pointerX - opposite.x,
                                              pointerY - opposite.y };
    const float                denominator =
        startVector.x * startVector.x + startVector.y * startVector.y;
    if ( denominator <= 1e-6f ) return placement;

    const float scale = std::max(
        0.01f,
        (pointerVector.x * startVector.x + pointerVector.y * startVector.y) /
            denominator);
    const float startRatio   = placement.fontSizeRatio;
    placement.fontSizeRatio  = std::clamp(startRatio * scale,
                                         CANVAS_COMPONENT_MIN_FONT_SIZE_RATIO,
                                         CANVAS_COMPONENT_MAX_FONT_SIZE_RATIO);
    const float appliedScale = placement.fontSizeRatio / startRatio;
    const float width        = startBounds.width() * appliedScale;
    const float height       = startBounds.height() * appliedScale;

    const bool growsLeft = handle == CanvasComponentDragHandle::TopLeft ||
                           handle == CanvasComponentDragHandle::BottomLeft;
    const bool growsUp = handle == CanvasComponentDragHandle::TopLeft ||
                         handle == CanvasComponentDragHandle::TopRight;
    const float centerX =
        opposite.x + (growsLeft ? -width * 0.5f : width * 0.5f);
    const float centerY =
        opposite.y + (growsUp ? -height * 0.5f : height * 0.5f);
    return moveCanvasComponent(placement,
                               centerX,
                               centerY,
                               viewportWidth,
                               viewportHeight,
                               width,
                               height);
}

}  // namespace MMM::Logic
