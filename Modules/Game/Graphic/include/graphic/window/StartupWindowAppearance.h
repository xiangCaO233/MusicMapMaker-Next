#pragma once

namespace MMM::Graphic
{
/// @brief 启动窗口外框的逻辑圆角半径，绘制和原生裁剪共同使用。
/// @note 仅用于启动资源窗，正式编辑器保留各平台既有窗口形态。
/// @details 两边按同一窗口缩放换算，避免系统剪掉 UI 的可见边框。
inline constexpr float STARTUP_WINDOW_CORNER_RADIUS = 16.0f;

}  // namespace MMM::Graphic
