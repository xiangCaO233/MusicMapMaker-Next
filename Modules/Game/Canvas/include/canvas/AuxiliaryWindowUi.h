#pragma once

namespace MMM::Canvas
{

/// @brief 判断当前 ImGui 辅助窗口标题栏是否仍可从显示器工作区访问。
/// @param dpiScale 当前窗口内容 DPI 缩放。
/// @return 标题栏仍有可操作区域时返回 true。
/// @warning UI 热路径：只在独立窗口打开时遍历少量显示器条目。
bool isCurrentAuxiliaryWindowReachable(float dpiScale);

/// @brief 必要时把当前 ImGui 辅助窗口恢复到主工作区中央。
/// @param requested 是否收到位置恢复请求。
/// @param dpiScale 当前窗口内容 DPI 缩放。
/// @warning UI 低频路径：只在项目恢复或菜单激活时执行位置检查。
void recoverCurrentAuxiliaryWindow(bool requested, float dpiScale);

}  // namespace MMM::Canvas
