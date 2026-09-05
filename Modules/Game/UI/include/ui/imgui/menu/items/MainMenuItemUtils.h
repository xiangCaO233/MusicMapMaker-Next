#pragma once

namespace MMM::UI
{

/// @brief 绘制带图标列的菜单项。
/// @param icon 图标文本，可为空。
/// @param label 菜单显示文本。
/// @param shortcut 快捷键提示文本。
/// @param enabled 是否允许点击。
/// @param selected 当前勾选状态。
/// @return 菜单项被点击时返回 true。
/// @warning UI 热路径：仅封装样式栈和 FeedbackMenuItemEx。
bool renderMainMenuIconItem(const char* icon, const char* label,
                            const char* shortcut = nullptr, bool enabled = true,
                            bool selected = false);

}  // namespace MMM::UI
