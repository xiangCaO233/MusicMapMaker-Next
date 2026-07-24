#pragma once

#include "config/visual/CanvasComponentConfig.h"
#include <algorithm>
#include <cmath>

namespace MMM::Logic
{

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
    placement.anchorX = std::clamp(placement.anchorX, 0.0f, 1.0f);
    placement.anchorY = std::clamp(placement.anchorY, 0.0f, 1.0f);
    return placement;
}

/// @brief 计算画布组件的响应式像素边界。
/// @param type 组件类型。
/// @param placement 组件布局。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @return 完整限制在画布内的组件边界。
/// @warning 热路径：布局编辑和快照生成时调用；只允许常量级数值计算。
[[nodiscard]] inline CanvasComponentBounds canvasComponentBounds(
    Config::CanvasComponentType             type,
    const Config::CanvasComponentPlacement& placement, float viewportWidth,
    float viewportHeight)
{
    if ( !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) ||
         viewportWidth <= 0.0f || viewportHeight <= 0.0f ) {
        return {};
    }

    float height = std::clamp(viewportHeight * 0.045f, 26.0f, 42.0f);
    float aspect = 1.0f;
    switch ( type ) {
    case Config::CanvasComponentType::JudgmentLineTime: aspect = 5.2f; break;
    case Config::CanvasComponentType::Count: break;
    }
    float width = std::min(viewportWidth, height * aspect);
    height      = std::min(viewportHeight, height);

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
/// @param type 组件类型。
/// @param placement 原布局。
/// @param centerX 目标像素中心横坐标。
/// @param centerY 目标像素中心纵坐标。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @return 更新后的归一化布局。
/// @warning 热路径：组件拖动期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::CanvasComponentPlacement moveCanvasComponent(
    Config::CanvasComponentType      type,
    Config::CanvasComponentPlacement placement, float centerX, float centerY,
    float viewportWidth, float viewportHeight)
{
    if ( viewportWidth <= 0.0f || viewportHeight <= 0.0f ||
         !std::isfinite(centerX) || !std::isfinite(centerY) ) {
        return sanitizeCanvasComponentPlacement(placement);
    }

    placement.anchorX = centerX / viewportWidth;
    placement.anchorY = centerY / viewportHeight;
    placement         = sanitizeCanvasComponentPlacement(placement);
    const auto bounds =
        canvasComponentBounds(type, placement, viewportWidth, viewportHeight);
    placement.anchorX = ((bounds.left + bounds.right) * 0.5f) / viewportWidth;
    placement.anchorY = ((bounds.top + bounds.bottom) * 0.5f) / viewportHeight;
    return sanitizeCanvasComponentPlacement(placement);
}

}  // namespace MMM::Logic
