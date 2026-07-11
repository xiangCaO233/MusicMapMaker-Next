#include "ui/imgui/menu/items/MainMenuItemUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <imgui.h>

namespace MMM::UI
{

/// @brief 绘制带图标列的菜单项。
/// @param icon 图标文本，可为空。
/// @param label 菜单显示文本。
/// @param shortcut 快捷键提示文本。
/// @param enabled 是否允许点击。
/// @return 菜单项被点击时返回 true。
/// @warning UI 热路径：仅封装样式栈和 FeedbackMenuItemEx。
bool renderMainMenuIconItem(const char* icon, const char* label,
                            const char* shortcut, bool enabled)
{
    ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

    const float gap = ImGui::CalcTextSize(" ").x * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(gap, 0));

    const char* iconPtr = icon ? icon : "  ";
    const bool  clicked =
        ::MMM::UI::FeedbackMenuItemEx(label, iconPtr, shortcut, false, enabled);

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return clicked;
}

}  // namespace MMM::UI
