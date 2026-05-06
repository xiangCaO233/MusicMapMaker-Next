#include "ui/imgui/MainDockSpaceUI.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "config/skin/SkinConfig.h"

namespace MMM::UI
{

void MainDockSpaceUI::renderStatusBar(UIManager* sourceManager, float statusBarHeight, float dpiScale)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // 设置状态栏位置：位于主视口底部
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    // 窗口标志：无标题栏、禁止收缩、禁止调整大小、禁止移动、禁止置顶、禁止停靠、无滚动条
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | 
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | 
                                    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar;

    // 移除圆角和边框，以及内边距
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * dpiScale, 0.0f)); // 左右留点边距，上下为0
    
    // 背景颜色 (稍微暗一点或者使用菜单栏颜色)
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);

    if (ImGui::Begin("StatusBar", nullptr, window_flags))
    {
        // 在状态栏顶部画一根分隔线
        ImVec2 p1 = ImGui::GetWindowPos();
        ImVec2 p2 = ImVec2(p1.x + ImGui::GetWindowWidth(), p1.y);
        ImGui::GetWindowDrawList()->AddLine(p1, p2, ImGui::GetColorU32(ImGuiCol_Border), 1.0f);

        // 垂直居中处理
        float textHeight = ImGui::GetFontSize();
        float offsetY = (statusBarHeight - textHeight) / 2.0f;
        ImGui::SetCursorPosY(offsetY);

        // 渲染状态栏内容
        ImGui::Text("Ready");
        
        ImGui::SameLine();
        ImGui::SetCursorPosY(offsetY); // Separator also needs same Y offset if drawn as text/line
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        
        ImGui::SetCursorPosY(offsetY);
        // 示例内容：显示当前鼠标位置或其它信息
        ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("Mouse: (%.1f, %.1f)", io.MousePos.x, io.MousePos.y);

        ImGui::End();
    }

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

} // namespace MMM::UI
