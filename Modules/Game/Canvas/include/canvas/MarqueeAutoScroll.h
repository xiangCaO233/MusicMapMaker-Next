#pragma once

namespace MMM::Logic
{
struct RenderSnapshot;
}  // namespace MMM::Logic

namespace MMM::Canvas
{

/// @brief 计算框选靠近边缘时自动滚动后的目标显示时间。
/// @param snapshot 当前 UI 渲染快照。
/// @param viewportHeight 当前画布视口高度，单位像素。
/// @param mouseY 本地鼠标 Y 坐标，单位像素。
/// @param deltaTime 当前 UI 帧间隔，单位秒。
/// @param isAccelerated 是否应用 Shift 加速。
/// @param scrolled 输出是否需要自动滚动。
/// @return 自动滚动后的目标显示时间，单位秒。
/// @warning UI 热路径：框选拖拽期间每帧调用；只读取不可变快照和视觉配置。
double marqueeAutoScrollTargetTime(const Logic::RenderSnapshot& snapshot,
                                   float viewportHeight, float mouseY,
                                   float deltaTime, bool isAccelerated,
                                   bool& scrolled);

}  // namespace MMM::Canvas
