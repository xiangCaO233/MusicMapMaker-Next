#pragma once

#include <imgui.h>

namespace MMM::UI::Utils
{

/// @brief 基于当前 ImGui 主题提供 UI 语义颜色工具。
/// @details 危险、警告和高亮颜色为稳定语义色；禁用色与透明按钮状态从当前
/// ImGui 文本色派生，以适配不同皮肤。
class UIThemeUtils
{
public:
    /// @brief 获取错误或危险状态使用的高对比红色。
    /// @return RGBA 危险语义色。
    static ImVec4 getDangerColor()
    {
        // 当前在所有主题中保持一致，后续可按主题微调。
        return { 1.0f, 0.2f, 0.2f, 1.0f };
    }

    /// @brief 获取需要注意但不表示失败的黄色。
    /// @return RGBA 警告语义色。
    static ImVec4 getWarningColor() { return { 1.0f, 0.8f, 0.0f, 1.0f }; }

    /// @brief 获取选中或重点信息使用的蓝色。
    /// @return RGBA 高亮语义色。
    static ImVec4 getHighlightColor()
    {
        // 使用稳定的蓝色高亮。
        return { 0.2f, 0.6f, 1.0f, 1.0f };
    }

    /// @brief 从当前文本色派生半透明禁用颜色。
    /// @return 保留文本 RGB、透明度减半的 RGBA 颜色。
    static ImVec4 getDisabledColor()
    {
        // 复用文本色并降低透明度，模拟禁用状态。
        ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        return { textCol.x, textCol.y, textCol.z, textCol.w * 0.5f };
    }

    /// @brief 压入透明按钮的常态、悬浮和按下颜色。
    /// @warning UI 热路径：每次调用必须与一次 popTransparentButtonStyles 配对。
    static void pushTransparentButtonStyles()
    {
        ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(textCol.x, textCol.y, textCol.z, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(textCol.x, textCol.y, textCol.z, 0.2f));
    }

    /// @brief 弹出透明按钮辅助函数压入的三项颜色。
    /// @pre 当前 ImGui 样式栈顶必须来自 pushTransparentButtonStyles。
    static void popTransparentButtonStyles() { ImGui::PopStyleColor(3); }
};

}  // namespace MMM::UI::Utils
