#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/imgui/MainDockSpaceUI.h"

namespace MMM::UI
{

void MainDockSpaceUI::renderDockingSpace(UIManager* sourceManager,
                                         float      menuBarHeight,
                                         float sidebarWidth, float toolbarWidth)
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + sidebarWidth,
                                   viewport->WorkPos.y + menuBarHeight));
    ImGui::SetNextWindowSize(
        ImVec2(viewport->WorkSize.x - sidebarWidth - toolbarWidth,
               viewport->WorkSize.y - menuBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags dock_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImFont* titleFont = skinCfg.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont);

    ImGui::Begin("RightDockHost", nullptr, dock_flags);

    ImGuiID dockspace_id = ImGui::GetID("MyMainDockSpace");
    ImGui::DockSpace(
        dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

    if ( titleFont ) ImGui::PopFont();

    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    static float lastDpiScale  = -1.0f;
    static bool  is_first_time = true;
    bool shouldResetLayout     = (std::abs(dpiScale - lastDpiScale) > 0.001f);

    if ( is_first_time || shouldResetLayout ) {
        is_first_time = false;
        lastDpiScale  = dpiScale;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(
            dockspace_id,
            ImVec2(viewport->WorkSize.x - sidebarWidth - toolbarWidth,
                   viewport->WorkSize.y - menuBarHeight));

        ImGuiID dock_id_left;
        ImGuiID dock_id_right;
        float   sidebarRatio = std::stof(
            skinCfg.getLayoutConfig("floating_windows.window1.initial_ratio"));
        auto dir =
            skinCfg.getLayoutConfig("floating_windows.window1.initial_side");
        ImGuiDir sidebarDir = (dir == "right") ? ImGuiDir_Right : ImGuiDir_Left;

        dock_id_left = ImGui::DockBuilderSplitNode(
            dockspace_id, sidebarDir, sidebarRatio, nullptr, &dock_id_right);

        ImGuiID dock_id_center_canvas;
        ImGuiID dock_id_preview;
        dock_id_preview = ImGui::DockBuilderSplitNode(dock_id_right,
                                                      ImGuiDir_Right,
                                                      0.20f,
                                                      nullptr,
                                                      &dock_id_center_canvas);

        ImGuiID dock_id_center;
        ImGuiID dock_id_timeline;
        dock_id_timeline = ImGui::DockBuilderSplitNode(dock_id_center_canvas,
                                                       ImGuiDir_Right,
                                                       0.20f,
                                                       nullptr,
                                                       &dock_id_center);

        ImGui::DockBuilderDockWindow("SideBarManager", dock_id_left);
        ImGui::DockBuilderDockWindow("Timeline", dock_id_timeline);
        ImGui::DockBuilderDockWindow("Basic2DCanvas", dock_id_center);
        ImGui::DockBuilderDockWindow("PreviewWindow", dock_id_preview);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
}

}  // namespace MMM::UI
