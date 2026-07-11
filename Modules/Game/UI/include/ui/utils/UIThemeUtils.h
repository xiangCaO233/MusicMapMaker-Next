#pragma once

#include <imgui.h>

namespace MMM::UI::Utils
{

/// @brief 基于当前 ImGui 主题提供 UI 语义颜色工具。
class UIThemeUtils
{
public:
    static ImVec4 getDangerColor()
    {
        // 当前在所有主题中保持一致，后续可按主题微调。
        return { 1.0f, 0.2f, 0.2f, 1.0f };
    }

    static ImVec4 getWarningColor() { return { 1.0f, 0.8f, 0.0f, 1.0f }; }

    static ImVec4 getHighlightColor()
    {
        // 使用稳定的蓝色高亮。
        return { 0.2f, 0.6f, 1.0f, 1.0f };
    }

    static ImVec4 getDisabledColor()
    {
        // 复用文本色并降低透明度，模拟禁用状态。
        ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        return { textCol.x, textCol.y, textCol.z, textCol.w * 0.5f };
    }

    // 基于当前主题文本色生成透明按钮样式，悬浮和按下状态只使用低透明度。
    static void pushTransparentButtonStyles()
    {
        ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(textCol.x, textCol.y, textCol.z, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(textCol.x, textCol.y, textCol.z, 0.2f));
    }

    static void popTransparentButtonStyles() { ImGui::PopStyleColor(3); }
};

}  // namespace MMM::UI::Utils
