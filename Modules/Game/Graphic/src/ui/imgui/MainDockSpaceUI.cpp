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
    float                sidebarWidth = 32.0f;

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

        // 1. 声明并建立 DockSpace (必须在 Builder 逻辑之前或同一层级)
        ImGuiID dockspace_id = ImGui::GetID("MyMainDockSpace");
        // 注意：这个 DockSpace 必须每帧都调用
        ImGui::DockSpace(
            dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

        static bool is_first_time = true;
        if ( is_first_time ) {
            is_first_time = false;
            // --- 第一步：彻底清空这个 ID 下的所有节点 ---
            ImGui::DockBuilderRemoveNode(dockspace_id);

            // --- 第二步：添加一个新的根节点，并设置总大小 ---
            ImGui::DockBuilderAddNode(dockspace_id,
                                      ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(
                dockspace_id,
                ImVec2(viewport->WorkSize.x - sidebarWidth,
                       viewport->WorkSize.y - menuBarHeight));

            // --- 第三步：拆分空间 ---
            ImGuiID dock_id_left;
            ImGuiID dock_id_right;
            // 从总空间 (dockspace_id) 中拆出 35% 给左边，剩下的 65%
            // 会被分给右边 (dock_id_right)
            dock_id_left = ImGui::DockBuilderSplitNode(
                dockspace_id, ImGuiDir_Left, 0.35f, nullptr, &dock_id_right);

            // --- 第四步：把窗口填进拆好的坑位里 ---

            // 文件管理器 -> 停靠在左侧坑位
            ImGui::DockBuilderDockWindow(TR("title.FileManager"), dock_id_left);

            // 画布 -> 停靠在剩余的右侧（中心）坑位
            // 【关键】不要停靠到 dockspace_id，要停靠到 split 出来的右侧 ID
            ImGui::DockBuilderDockWindow("Basic2DCanvas", dock_id_right);

            // --- 第五步：完成构建 ---
            ImGui::DockBuilderFinish(dockspace_id);
        }

        ImGui::End();
        ImGui::PopStyleVar(3);
    }
}

};  // namespace MMM::Graphic::UI
