#include "ui/imgui/MainDockSpaceUI.h"
#include "imgui.h"
#include "log/colorful-log.h"

namespace MMM::Graphic::UI
{

MainDockSpaceUI::MainDockSpaceUI() {}
MainDockSpaceUI::~MainDockSpaceUI() {}

void MainDockSpaceUI::update()
{
    // ================== 新增：全窗口透明 DockSpace ==================
    {
        static bool               opt_fullscreen  = true;
        static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

        // 1. 设置 DockSpace 为透明并允许背景点击穿透
        // ImGuiDockNodeFlags_PassthruCentralNode 会让中心节点不绘制背景
        dockspace_flags |= ImGuiDockNodeFlags_PassthruCentralNode;

        // 2. 配置全屏窗口参数
        ImGuiWindowFlags window_flags =
            ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
        if ( opt_fullscreen ) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            window_flags |= ImGuiWindowFlags_NoTitleBar |
                            ImGuiWindowFlags_NoCollapse |
                            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus |
                            ImGuiWindowFlags_NoNavFocus;
        }

        // 3. 关键：将背景设为透明
        window_flags |= ImGuiWindowFlags_NoBackground;

        // 4. 开始窗口
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("MyMainDockSpaceWindow", nullptr, window_flags);
        ImGui::PopStyleVar();

        if ( opt_fullscreen ) ImGui::PopStyleVar(2);

        // 5. 提交 DockSpace
        ImGuiIO& io = ImGui::GetIO();
        if ( io.ConfigFlags & ImGuiConfigFlags_DockingEnable ) {
            ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        }

        // 这里可以添加常驻的菜单栏（如果需要）
        if ( ImGui::BeginMenuBar() ) {
            if ( ImGui::BeginMenu("File") ) {
                if ( ImGui::MenuItem("Exit") ) { /* 退出逻辑 */
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        ImGui::End();
    }
    // =============================================================
}

};  // namespace MMM::Graphic::UI
