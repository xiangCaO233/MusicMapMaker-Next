#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <string>

namespace MMM::UI::Utils
{

/**
 * @brief 渲染一个带有水平自动滚动动画的 Selectable。
 *        当文本宽度超出给定的 width 时，会自动开启往复滚动动画。
 */
template<typename OnClick>
static void renderScrollingSelectable(const std::string& id,
                                      const std::string& text, float width,
                                      float height, OnClick onClick,
                                      const std::string& tooltip = "")
{
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImVec2 textSize  = ImGui::CalcTextSize(text.c_str());

    // 绘制背景 Selectable (不带文本)
    std::string selectableId = "##selectable_" + id;
    if ( ImGui::Selectable(
             selectableId.c_str(), false, 0, ImVec2(width, height)) ) {
        onClick();
    }

    if ( !tooltip.empty() && ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }

    // 计算滚动位移
    float offset       = 0.0f;
    float padding      = 8.0f;
    float visibleWidth = width - padding;

    if ( textSize.x > visibleWidth ) {
        float scrollRange = textSize.x - visibleWidth + 40.0f;
        float time        = (float)ImGui::GetTime();
        // 平滑往复滚动，两端停顿
        float t = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
        t       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
        offset  = t * scrollRange;
    }

    // 垂直居中计算
    float textH   = ImGui::GetFontSize();
    float offsetY = (height - textH) * 0.5f;

    // 应用剪切矩形并绘制文本
    ImGui::PushClipRect(
        cursorPos, ImVec2(cursorPos.x + width, cursorPos.y + height), true);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(cursorPos.x - offset, cursorPos.y + offsetY),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.c_str());
    ImGui::PopClipRect();
}

/**
 * @brief 在 Clay 布局块中渲染 CollapsingHeader，并确保其宽度适配布局边界。
 */
static bool renderCollapsingHeader(const char* label, bool* p_state,
                                   struct Clay_BoundingBox r,
                                   ImGuiTreeNodeFlags      flags = 0)
{
    ImGui::SetCursorScreenPos({ r.x, r.y });

    // 应用 SettingsView_Tabs 中的技巧，使 CollapsingHeader 受到 Clay
    // 布局边界的影响
    ImGuiWindow* win         = ImGui::GetCurrentWindow();
    float        savedWRMaxX = win->WorkRect.Max.x;
    win->WorkRect.Max.x      = r.x + r.width;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });

    bool open = ImGui::CollapsingHeader(
        label, flags | (*p_state ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    *p_state = open;

    ImGui::PopStyleVar();
    win->WorkRect.Max.x = savedWRMaxX;

    return open;
}

/**
 * @brief 渲染带自动滚动的树节点（针对文件浏览器）。
 */
template<typename OnClick>
static bool renderScrollingTreeNode(const std::string& id,
                                    const std::string& text, float width,
                                    float height, bool isLeaf, OnClick onClick,
                                    const std::string& tooltip = "")
{
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImVec2 textSize  = ImGui::CalcTextSize(text.c_str());

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if ( isLeaf )
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    // 强制占满可用宽度
    ImGui::SetNextItemAllowOverlap();
    bool open      = ImGui::TreeNodeEx(id.c_str(), flags, "");
    bool isClicked = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();

    if ( !tooltip.empty() && ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }

    // 绘制滚动文本
    float  arrowWidth         = ImGui::GetTreeNodeToLabelSpacing();
    float  textAvailableWidth = width - arrowWidth;
    ImVec2 textStartPos       = { cursorPos.x + arrowWidth, cursorPos.y };

    float offset       = 0.0f;
    float padding      = 8.0f;
    float visibleWidth = textAvailableWidth - padding;

    if ( textSize.x > visibleWidth ) {
        float scrollRange = textSize.x - visibleWidth + 40.0f;
        float time        = (float)ImGui::GetTime();
        float t           = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
        t                 = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
        offset            = t * scrollRange;
    }

    float textH   = ImGui::GetFontSize();
    float offsetY = (height - textH) * 0.5f;

    ImGui::PushClipRect(
        textStartPos,
        ImVec2(textStartPos.x + textAvailableWidth, textStartPos.y + height),
        true);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(textStartPos.x - offset, textStartPos.y + offsetY),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.c_str());
    ImGui::PopClipRect();

    if ( isClicked ) onClick();

    return open;
}

}  // namespace MMM::UI::Utils
