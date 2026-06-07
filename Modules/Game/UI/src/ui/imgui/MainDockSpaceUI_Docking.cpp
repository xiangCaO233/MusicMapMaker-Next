#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/UIManager.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include <algorithm>

namespace MMM::UI
{

void MainDockSpaceUI::renderDockingSpace(UIManager* sourceManager,
                                         float      menuBarHeight,
                                         float      statusBarHeight,
                                         float sidebarWidth, float toolbarWidth)
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    const bool fixedToolWindow =
        Config::AppConfig::instance().getEditorSettings().fixedToolWindow;
    float floatGap = std::floor(aesthetics.windowGap * dpiScale);

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + sidebarWidth + 2.0f * floatGap,
               viewport->WorkPos.y + menuBarHeight + floatGap));
    ImGui::SetNextWindowSize(ImVec2(
        viewport->WorkSize.x - sidebarWidth - toolbarWidth - 4.0f * floatGap,
        viewport->WorkSize.y - menuBarHeight - statusBarHeight -
            2.0f * floatGap));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags dock_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking;

    float windowRound = std::floor(aesthetics.windowRounding * dpiScale);
    float frameRound  = std::floor(aesthetics.frameRounding * dpiScale);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    float separatorSize =
        std::min(8.0f * dpiScale, std::max(3.0f * dpiScale, floatGap));
    ImGui::PushStyleVar(ImGuiStyleVar_DockingSeparatorSize, separatorSize);

    // --- 宿主窗口使用 0 内边距以撑满容器 ---
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

    ImFont* titleFont = skinCfg.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

    ImGui::Begin("RightDockHost", nullptr, dock_flags);
    // 立即弹出 WindowPadding，防止其应用到停靠在其中的子窗口
    ImGui::PopStyleVar(1);

    auto* sideBarManager =
        sourceManager->getView<FloatingManagerUI>("SideBarManager");
    if ( sideBarManager ) {
        sideBarManager->applyDockResizeConstraintsBeforeDockSpace(
            sourceManager);
    }

    ImGuiID dockspace_id = ImGui::GetID("MyMainDockSpace");
    ImGui::DockSpace(
        dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    if ( sideBarManager ) {
        sideBarManager->restoreDockResizeMouseAfterDockSpace();
    }

    if ( titleFont ) ImGui::PopFont();

    static float lastDpiScale           = -1.0f;
    static bool  lastFixedToolWindow    = true;
    static bool  hasLastFixedToolWindow = false;
    static bool  is_first_time          = true;
    bool shouldResetLayout = (std::abs(dpiScale - lastDpiScale) > 0.001f) ||
                             !hasLastFixedToolWindow ||
                             lastFixedToolWindow != fixedToolWindow;
    bool projectLayoutLoaded =
        MainDockSpaceUI::consumeProjectWorkspaceLayoutLoaded();
    if ( projectLayoutLoaded ) {
        is_first_time          = false;
        lastDpiScale           = dpiScale;
        lastFixedToolWindow    = fixedToolWindow;
        hasLastFixedToolWindow = true;
        shouldResetLayout      = false;
    }

    if ( is_first_time || shouldResetLayout ) {
        is_first_time          = false;
        lastDpiScale           = dpiScale;
        lastFixedToolWindow    = fixedToolWindow;
        hasLastFixedToolWindow = true;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        const float dockspaceWidth = viewport->WorkSize.x - sidebarWidth -
                                     toolbarWidth - 4.0f * floatGap;
        ImGui::DockBuilderSetNodeSize(
            dockspace_id,
            ImVec2(dockspaceWidth,
                   viewport->WorkSize.y - menuBarHeight - statusBarHeight -
                       2.0f * floatGap));

        ImGuiID dock_id_left;
        ImGuiID dock_id_right;
        float   sidebarRatio = std::stof(
            skinCfg.getLayoutConfig("floating_windows.window1.initial_ratio"));
        auto dir =
            skinCfg.getLayoutConfig("floating_windows.window1.initial_side");
        ImGuiDir sidebarDir = (dir == "right") ? ImGuiDir_Right : ImGuiDir_Left;

        dock_id_left = ImGui::DockBuilderSplitNode(
            dockspace_id, sidebarDir, sidebarRatio, nullptr, &dock_id_right);

        ImGuiID dock_id_work = dock_id_right;
        if ( !fixedToolWindow ) {
            const float toolNodeRatio = std::clamp(
                (std::floor(32.0f * dpiScale) +
                 2.0f * std::floor(aesthetics.windowPadding * dpiScale)) /
                    std::max(dockspaceWidth, 1.0f),
                0.035f,
                0.12f);
            ImGuiID dock_id_tool = 0;
            dock_id_tool         = ImGui::DockBuilderSplitNode(dock_id_work,
                                                               ImGuiDir_Right,
                                                               toolNodeRatio,
                                                               nullptr,
                                                               &dock_id_work);
            ImGui::DockBuilderDockWindow("Toolbar", dock_id_tool);
            MainDockSpaceUI::setToolDockId(dock_id_tool);
        } else {
            MainDockSpaceUI::setToolDockId(0);
        }

        ImGuiID dock_id_center_canvas;
        ImGuiID dock_id_preview;
        dock_id_preview = ImGui::DockBuilderSplitNode(dock_id_work,
                                                      ImGuiDir_Right,
                                                      0.20f,
                                                      nullptr,
                                                      &dock_id_center_canvas);

        ImGuiID dock_id_center;
        ImGuiID dock_id_timeline;
        dock_id_timeline = ImGui::DockBuilderSplitNode(dock_id_center_canvas,
                                                       ImGuiDir_Right,
                                                       0.28f,
                                                       nullptr,
                                                       &dock_id_center);

        ImGui::DockBuilderDockWindow("SideBarManager", dock_id_left);
        ImGui::DockBuilderDockWindow("Timeline", dock_id_timeline);
        ImGui::DockBuilderDockWindow("Basic2DCanvas", dock_id_center);
        ImGui::DockBuilderDockWindow("PreviewWindow", dock_id_preview);

        MainDockSpaceUI::setCenterDockId(dock_id_center);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    if ( ImGuiDockNode* centerNode =
             ImGui::DockBuilderGetCentralNode(dockspace_id) ) {
        MainDockSpaceUI::setCenterDockId(centerNode->ID);
    } else {
        MainDockSpaceUI::setCenterDockId(0);
    }

    ImGui::End();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(6);
}

}  // namespace MMM::UI
