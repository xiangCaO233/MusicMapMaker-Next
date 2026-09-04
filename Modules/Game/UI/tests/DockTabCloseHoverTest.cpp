#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>

namespace
{
/// @brief 回归测试中的 Dock 宿主窗口名称。
constexpr const char* DOCK_HOST_NAME = "DockTabCloseHoverHost";

/// @brief 回归测试中的第一个 Dock 窗口名称。
constexpr const char* FIRST_WINDOW_NAME = "First Beatmap";

/// @brief 回归测试中的第二个 Dock 窗口名称。
constexpr const char* SECOND_WINDOW_NAME = "Second Beatmap";

/// @brief 关闭按钮悬浮时必须出现的固定危险操作色。
constexpr ImU32 EXPECTED_CLOSE_HOVER_COLOR = IM_COL32(222, 48, 62, 255);

/// @brief 绘制一帧包含两个可关闭窗口的 DockSpace。
/// @param mousePosition 本帧鼠标屏幕坐标。
/// @warning 测试 UI 路径：每帧只提交一个 DockSpace 与两个窗口。
void drawDockFrame(const ImVec2& mousePosition)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mousePosition.x, mousePosition.y);
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(640.0F, 360.0F), ImGuiCond_Always);
    ImGui::Begin(DOCK_HOST_NAME,
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus);
    const ImGuiID dockspaceId = ImGui::GetID("DockTabCloseHoverDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0F, 0.0F));
    ImGui::End();

    bool firstOpen = true;
    ImGui::SetNextWindowDockID(dockspaceId, ImGuiCond_Always);
    ImGui::Begin(FIRST_WINDOW_NAME, &firstOpen);
    MMM::UI::FeedbackCurrentWindowCloseButton(true, &firstOpen);
    ImGui::End();

    bool secondOpen = true;
    ImGui::SetNextWindowDockID(dockspaceId, ImGuiCond_Always);
    ImGui::Begin(SECOND_WINDOW_NAME, &secondOpen);
    MMM::UI::FeedbackCurrentWindowCloseButton(true, &secondOpen);
    ImGui::End();

    ImGui::Render();
}

/// @brief 计算绘制列表中使用指定颜色的顶点数。
/// @param drawList 待检查的绘制列表。
/// @param color 目标打包颜色。
/// @return 颜色完全匹配的顶点数。
int countVerticesWithColor(const ImDrawList* drawList, ImU32 color)
{
    if ( !drawList ) {
        return 0;
    }

    int count = 0;
    for ( const ImDrawVert& vertex : drawList->VtxBuffer ) {
        if ( vertex.col == color ) {
            ++count;
        }
    }
    return count;
}

/// @brief 获取指定 Dock 窗口关闭按钮的中心坐标。
/// @param windowName 目标窗口名称。
/// @param center 输出关闭按钮中心坐标。
/// @return Dock 标签与关闭按钮几何可用时返回 true。
bool resolveCloseButtonCenter(const char* windowName, ImVec2* center)
{
    ImGuiWindow* window = ImGui::FindWindowByName(windowName);
    if ( !window || !window->DockNode || !window->DockNode->TabBar ||
         !center ) {
        return false;
    }

    ImGuiTabBar*  tabBar = window->DockNode->TabBar;
    ImGuiTabItem* target = nullptr;
    for ( auto& tab : tabBar->Tabs ) {
        if ( tab.Window == window ) {
            target = &tab;
            break;
        }
    }
    if ( !target ) {
        return false;
    }

    const float tabX = tabBar->BarRect.Min.x +
                       std::trunc(target->Offset - tabBar->ScrollingAnim);
    const float buttonSize = ImGui::GetFontSize();
    const float buttonX    = std::max(
        tabX, tabX + target->Width - tabBar->FramePadding.x - buttonSize);
    const float buttonY = tabBar->BarRect.Min.y + tabBar->FramePadding.y;
    *center = ImVec2(buttonX + buttonSize * 0.5F, buttonY + buttonSize * 0.5F);
    return true;
}

/// @brief 验证关闭按钮悬浮同时补齐标签高亮与红色危险高亮。
/// @return 成功时返回 0；否则返回可定位失败阶段的非零编码。
int testCloseHoverRendersTabAndDangerHighlights()
{
    ImGuiStyle& style                 = ImGui::GetStyle();
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.12F, 0.78F, 0.44F, 1.0F);
    const ImU32 expectedTabHoverColor =
        ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_TabHovered]);

    drawDockFrame(ImVec2(-1000.0F, -1000.0F));
    drawDockFrame(ImVec2(-1000.0F, -1000.0F));

    ImVec2 closeCenter;
    if ( !resolveCloseButtonCenter(SECOND_WINDOW_NAME, &closeCenter) ) {
        return 1;
    }
    drawDockFrame(closeCenter);
    if ( !resolveCloseButtonCenter(SECOND_WINDOW_NAME, &closeCenter) ) {
        return 1;
    }
    drawDockFrame(closeCenter);
    if ( !resolveCloseButtonCenter(SECOND_WINDOW_NAME, &closeCenter) ) {
        return 1;
    }
    drawDockFrame(closeCenter);

    ImGuiWindow* window = ImGui::FindWindowByName(SECOND_WINDOW_NAME);
    if ( !window || !window->DockNode || !window->DockNode->TabBar ) {
        return 2;
    }
    const ImGuiID closeButtonId = ImHashStr("#CLOSE", 0, window->ID);
    if ( GImGui->HoveredId != closeButtonId ) {
        return 3;
    }
    const ImDrawList* hostDrawList =
        window->DockNode->TabBar->Window
            ? window->DockNode->TabBar->Window->DrawList
            : nullptr;
    if ( countVerticesWithColor(hostDrawList, expectedTabHoverColor) < 4 ) {
        return 4;
    }
    if ( countVerticesWithColor(hostDrawList, EXPECTED_CLOSE_HOVER_COLOR) <
         4 ) {
        return 5;
    }
    return 0;
}
}  // namespace

/// @brief 运行 Dock 标签关闭按钮悬浮视觉回归测试。
/// @return 测试通过时返回 0。
int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0F, 360.0F);
    io.DeltaTime   = 1.0F / 60.0F;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    unsigned char* fontPixels = nullptr;
    int            fontWidth  = 0;
    int            fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    const bool fontReady = fontPixels && fontWidth > 0 && fontHeight > 0;

    MMM::UI::SetInteractionFeedbackEnabled(false);
    const int result =
        fontReady ? testCloseHoverRendersTabAndDangerHighlights() : 6;
    ImGui::DestroyContext();
    return result;
}
