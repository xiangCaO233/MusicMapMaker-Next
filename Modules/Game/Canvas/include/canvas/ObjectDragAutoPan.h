#pragma once

#include "logic/session/CanvasCamera.h"

#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>

namespace MMM::Canvas
{

/// @brief 物件拖拽横向自动平移越过侧边轨道区后保留的固定逻辑像素。
inline constexpr float OBJECT_DRAG_AUTO_PAN_REDUNDANCY = 48.0F;

/// @brief 物件拖拽边缘自动平移的基础速度，单位逻辑像素每秒。
inline constexpr float OBJECT_DRAG_AUTO_PAN_PIXELS_PER_SECOND = 540.0F;

/// @brief 计算物件拖拽靠近单轴视口边缘时的内容平移量。
/// @param coordinate 指针在该轴上的局部坐标。
/// @param extent 视口在该轴上的尺寸。
/// @param deltaTime 当前 UI 帧间隔。
/// @param sensitivity 用户配置的边缘滚动灵敏度。
/// @return 内容应在本帧平移的逻辑像素；靠近起始边缘时为正。
/// @warning UI 热路径：物件拖动期间每帧调用，只做常量数值运算。
[[nodiscard]] inline float objectDragAutoPanAxisDelta(float coordinate,
                                                      float extent,
                                                      float deltaTime,
                                                      float sensitivity)
{
    // 无效几何或非正灵敏度直接禁用本轴，避免 NaN 进入相机平移命令。
    if ( !std::isfinite(coordinate) || !std::isfinite(extent) ||
         extent <= 1.0F || !std::isfinite(sensitivity) ||
         sensitivity <= 0.0F ) {
        return 0.0F;
    }

    // 边缘热区随视口尺寸变化，但限制在可操作的 24 至 64 像素之间。
    const float margin      = std::clamp(extent * 0.08F, 24.0F, 64.0F);
    float       penetration = 0.0F;
    float       direction   = 0.0F;
    if ( coordinate < margin ) {
        // 靠近起始边缘时内容向正方向移动，把边缘外区域带入视口。
        penetration = margin - coordinate;
        direction   = 1.0F;
    } else if ( coordinate > extent - margin ) {
        // 靠近结束边缘时方向相反，penetration 仍保持非负强度。
        penetration = coordinate - (extent - margin);
        direction   = -1.0F;
    }
    if ( penetration <= 0.0F ) return 0.0F;

    // 帧间隔限制抑制断点或窗口卡顿后的单帧巨幅跳跃，并为无效值提供
    // 60 FPS 回退；它不阻塞交互，也不改变正常帧率下的速度积分。
    const float frameSeconds = std::clamp(
        std::isfinite(deltaTime) && deltaTime > 0.0F ? deltaTime : 1.0F / 60.0F,
        1.0F / 240.0F,
        1.0F / 15.0F);
    const float ramp = std::clamp(penetration / margin, 0.0F, 2.0F);
    // 二次曲线让热区内侧平缓起步，拖出视口后最多提升到四倍基础强度。
    return direction * OBJECT_DRAG_AUTO_PAN_PIXELS_PER_SECOND * ramp * ramp *
           sensitivity * frameSeconds;
}

/// @brief 计算左键物件拖拽的二维边缘自动平移量。
/// @param mousePos 指针相对画布的位置。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @param deltaTime 当前 UI 帧间隔。
/// @param sensitivity 用户配置的边缘滚动灵敏度。
/// @return 直接传给 CmdPanCanvas 的二维内容位移。
/// @warning UI 热路径：物件拖动期间每帧调用，不分配内存。
[[nodiscard]] inline glm::vec2 objectDragAutoPanDelta(glm::vec2 mousePos,
                                                      float     viewportWidth,
                                                      float     viewportHeight,
                                                      float     deltaTime,
                                                      float     sensitivity)
{
    // 两轴独立计算，允许拖到角落时同时横向和纵向平移。
    return {
        objectDragAutoPanAxisDelta(
            mousePos.x, viewportWidth, deltaTime, sensitivity),
        objectDragAutoPanAxisDelta(
            mousePos.y, viewportHeight, deltaTime, sensitivity),
    };
}

/// @brief 将物件拖拽的横向自动平移限制在侧边轨道区外边缘内。
/// @param requestedDelta 本帧请求的横向内容位移。
/// @param viewportWidth 当前画布视口逻辑宽度。
/// @param projection 当前草稿、玩家与 BGM 轨道投影。
/// @param redundancy 最外侧轨道与对应视口边缘间保留的固定逻辑像素。
/// @return 不会继续越过草稿区或 BGM 区边界的本帧横向位移。
/// @warning UI 热路径：物件拖动期间每帧调用，只做常量数值运算。
[[nodiscard]] inline float clampObjectDragHorizontalAutoPanDelta(
    float requestedDelta, float viewportWidth,
    const Logic::CanvasLaneProjection& projection,
    float redundancy = OBJECT_DRAG_AUTO_PAN_REDUNDANCY)
{
    // 投影无效时无法确定轨道外边界，宁可停止横向自动平移。
    if ( !projection.valid || !std::isfinite(requestedDelta) ||
         !std::isfinite(viewportWidth) || viewportWidth <= 0.0F ||
         !std::isfinite(redundancy) ) {
        return 0.0F;
    }

    // 留白最多取视口一半，防止左右安全边界发生反转。
    redundancy = std::clamp(redundancy, 0.0F, viewportWidth * 0.5F);
    if ( requestedDelta > 0.0F ) {
        // 正位移把左侧草稿区带入视口，仅允许移动到最左轨保留 redundancy。
        const float available = redundancy - projection.draftLeftX;
        return std::min(requestedDelta, std::max(0.0F, available));
    }
    if ( requestedDelta < 0.0F ) {
        // 存在 BGM 轨时以其右边界为准，否则退回玩家区右边界。
        const float rightmostTrackX = projection.bgmLaneCount > 0
                                          ? projection.bgmRightX
                                          : projection.player.rightX;
        const float available = rightmostTrackX - (viewportWidth - redundancy);
        // requestedDelta 为负，返回值下界由仍可带入视口的距离决定。
        return std::max(requestedDelta, -std::max(0.0F, available));
    }
    return 0.0F;
}

}  // namespace MMM::Canvas
