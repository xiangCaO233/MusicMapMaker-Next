#pragma once

namespace MMM::UI
{
/// @brief 绘制布局方案管理入口，返回是否已发布新的布局。
/// @note 所有键数布局保存在同一方案；轨道和组件仍按当前键数调整。
/// @warning 每帧只展示引用，捕获、文件选择和保存仅响应明确用户操作。
bool drawLayoutPresetControls();
/// @brief 推进布局方案文件选择器，必须独立于布局弹层的显示状态调用。
/// @note 关闭布局面板也必须完成已打开的模态选择器生命周期。
/// @warning 空闲帧只检查打开状态，不读写文件。
void drawLayoutPresetFileDialogs();
}  // namespace MMM::UI
