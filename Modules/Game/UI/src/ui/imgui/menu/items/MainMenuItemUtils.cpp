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
/// @param selected 当前勾选状态。
/// @return 菜单项被点击时返回 true。
/// @warning UI 热路径：仅封装样式栈和 FeedbackMenuItemEx。
/// @note 函数结束前严格对称恢复 ImGui 样式栈，不影响后续菜单项。
bool renderMainMenuIconItem(const char* icon, const char* label,
                            const char* shortcut, bool enabled, bool selected)
{
    ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

    const float gap = ImGui::CalcTextSize(" ").x * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(gap, 0));

    // 空图标仍保留固定列宽，使同一菜单中的标签保持对齐。
    const char* iconPtr = icon ? icon : "  ";
    const bool  clicked = ::MMM::UI::FeedbackMenuItemEx(
        label, iconPtr, shortcut, selected, enabled);

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return clicked;
}

}  // namespace MMM::UI
