#include "ui/imgui/MainDockSpaceUI.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"

namespace MMM::Graphic::UI
{

void MainDockSpaceUI::update()
{
    const ImGuiViewport* viewport     = ImGui::GetMainViewport();
    float                sidebarWidth = 48.0f;

    // 1. 获取菜单栏标准高度
    float menuBarHeight = ImGui::GetFrameHeight();

    // ================== A. 顶部菜单栏窗口 ==================
    {
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, menuBarHeight));
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags menu_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBackground;

        ImGui::Begin("TopMenuBarHost", nullptr, menu_flags);
        if ( ImGui::BeginMenuBar() ) {
            if ( ImGui::BeginMenu(TR("ui.file")) ) {
                ImGui::EndMenu();
            }
            if ( ImGui::BeginMenu(TR("ui.edit")) ) {
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::End();
    }

    // ================== B. 右侧停靠空间窗口 ==================
    {
        // 位置偏移：X + 48, Y + 菜单高度
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + sidebarWidth,
                                       viewport->WorkPos.y + menuBarHeight));
        // 尺寸：宽 - 48, 高 - 菜单高度
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - sidebarWidth,
                                        viewport->WorkSize.y - menuBarHeight));
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags dock_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        ImGui::Begin("RightDockHost", nullptr, dock_flags);

        ImGuiID dockspace_id = ImGui::GetID("MyMainDockSpace");
        // 这里 Size 传 (0,0) 表示占满 RightDockHost 窗口
        ImGui::DockSpace(
            dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

        static bool is_first_time = true;
        if ( is_first_time ) {
            is_first_time = false;
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id,
                                      ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(
                dockspace_id,
                ImVec2(viewport->WorkSize.x - sidebarWidth,
                       viewport->WorkSize.y - menuBarHeight));
            ImGui::DockBuilderDockWindow("Basic2DCanvas", dockspace_id);
            ImGui::DockBuilderFinish(dockspace_id);
        }

        ImGui::End();
        ImGui::PopStyleVar(3);
    }
}

};  // namespace MMM::Graphic::UI
