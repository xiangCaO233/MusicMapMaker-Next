#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace MMM::Logic
{

/// @brief 主画布玩家轨道在逻辑像素空间中的横向投影。
struct CanvasTrackProjection {
    /// @brief 玩家轨道区左边界。
    float leftX{ 0.0F };

    /// @brief 玩家轨道区右边界。
    float rightX{ 0.0F };

    /// @brief 单条玩家轨道宽度。
    float singleTrackWidth{ 0.0F };

    /// @brief 投影参数是否可用于坐标换算。
    bool valid{ false };

    /// @brief 判断逻辑像素横坐标是否位于玩家轨道区。
    /// @param x 待判断的横坐标。
    /// @return 位于轨道区闭区间内时返回 true。
    [[nodiscard]] bool contains(float x) const
    {
        return valid && std::isfinite(x) && x >= leftX && x <= rightX;
    }

    /// @brief 将逻辑像素横坐标换算为从零开始的玩家轨道索引。
    /// @param x 待换算的横坐标。
    /// @param trackCount 玩家轨道数量。
    /// @return 限制在有效范围内的轨道索引；投影无效时返回 0。
    [[nodiscard]] std::int32_t trackAt(float x, std::int32_t trackCount) const
    {
        if ( !valid || trackCount <= 0 || !std::isfinite(x) ) {
            return 0;
        }
        const auto track = static_cast<std::int32_t>(
            std::floor((x - leftX) / singleTrackWidth));
        return std::clamp(track, std::int32_t{ 0 }, trackCount - 1);
    }
};

/// @brief 计算应用横向相机偏移后的玩家轨道投影。
/// @param viewportWidth 视口逻辑宽度。
/// @param trackCount 玩家轨道数量。
/// @param layoutLeft 玩家轨道区左边界比例。
/// @param layoutRight 玩家轨道区右边界比例。
/// @param horizontalOffsetX 相机产生的内容横向逻辑像素偏移。
/// @return 可供渲染、拾取和工具坐标换算共用的轨道投影。
/// @warning 逻辑与渲染热路径可能每帧调用；只允许常量级数值运算。
[[nodiscard]] inline CanvasTrackProjection calculatePlayerTrackProjection(
    float viewportWidth, std::int32_t trackCount, float layoutLeft,
    float layoutRight, float horizontalOffsetX)
{
    CanvasTrackProjection result;
    if ( !std::isfinite(viewportWidth) || viewportWidth <= 0.0F ||
         trackCount <= 0 ) {
        return result;
    }

    if ( !std::isfinite(layoutLeft) ) {
        layoutLeft = 0.0F;
    }
    if ( !std::isfinite(layoutRight) ) {
        layoutRight = 1.0F;
    }
    if ( layoutLeft >= layoutRight ) {
        layoutRight = layoutLeft + 0.01F;
    }
    if ( !std::isfinite(horizontalOffsetX) ) {
        horizontalOffsetX = 0.0F;
    }

    result.leftX  = viewportWidth * layoutLeft + horizontalOffsetX;
    result.rightX = viewportWidth * layoutRight + horizontalOffsetX;
    result.singleTrackWidth =
        (result.rightX - result.leftX) / static_cast<float>(trackCount);
    result.valid = std::isfinite(result.leftX) &&
                   std::isfinite(result.rightX) &&
                   std::isfinite(result.singleTrackWidth) &&
                   result.singleTrackWidth > 0.0F;
    return result;
}

/// @brief 在逻辑视口宽度变化时等比例换算横向相机偏移。
/// @param horizontalOffsetX 旧视口下的内容横向偏移。
/// @param oldViewportWidth 旧视口逻辑宽度。
/// @param newViewportWidth 新视口逻辑宽度。
/// @return 新视口下保持相同比例位移的横向偏移。
/// @warning 视口更新路径调用；只允许常量级数值运算。
[[nodiscard]] inline float resizeCanvasHorizontalOffset(float horizontalOffsetX,
                                                        float oldViewportWidth,
                                                        float newViewportWidth)
{
    if ( !std::isfinite(horizontalOffsetX) ) {
        return 0.0F;
    }
    if ( !std::isfinite(oldViewportWidth) || oldViewportWidth <= 0.0F ||
         !std::isfinite(newViewportWidth) || newViewportWidth <= 0.0F ) {
        return horizontalOffsetX;
    }
    return horizontalOffsetX * newViewportWidth / oldViewportWidth;
}

}  // namespace MMM::Logic
