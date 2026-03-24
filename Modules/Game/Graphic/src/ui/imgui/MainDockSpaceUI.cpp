#include "ui/imgui/MainDockSpaceUI.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/ui/GLFWNativeEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include <utility>

namespace MMM::Graphic::UI
{

void MainDockSpaceUI::update()
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    static float         sidebarWidth =
        std::stof(skinCfg.getLayoutConfig("side_bar.width"));

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
        float buttonSize = ImGui::GetFrameHeight();
        if ( ImGui::BeginMenuBar() ) {
            // 定义 Lambda (内部 Pop 2个 Color，不改变外部计数)
            auto DrawIconButton = [&](const char*                 str_id,
                                      std::unique_ptr<VKTexture>& tex,
                                      float                       btnSize,
                                      ImVec4 hoverColor) -> bool {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);

                bool clicked = ImGui::Button(str_id, ImVec2(btnSize, btnSize));

                if ( tex ) {
                    ImTextureID imTexId  = (ImTextureID)tex->getImTextureID();
                    float       iconSize = btnSize * 0.65f;
                    ImVec2      p_min    = ImGui::GetItemRectMin();
                    float       offsetX  = (btnSize - iconSize) * 0.5f;
                    float       offsetY  = (btnSize - iconSize) * 0.5f;
                    ImVec2 img_p1 = { p_min.x + offsetX, p_min.y + offsetY };
                    ImVec2 img_p2 = { img_p1.x + iconSize,
                                      img_p1.y + iconSize };
                    ImU32  tint   = ImGui::IsItemActive()
                                        ? IM_COL32(180, 180, 180, 255)
                                        : IM_COL32_WHITE;
                    ImGui::GetWindowDrawList()->AddImage(
                        imTexId, img_p1, img_p2, { 0, 0 }, { 1, 1 }, tint);
                }

                ImGui::PopStyleColor(2);  // 弹出 Lambda 内部的
                return clicked;
            };
            // 【修正点 1】统一在此处 Push 样式，计数：Color 1个, Var 2个
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            // --- 1. 前置图标 (例如 Logo) ---
            DrawIconButton(
                "##logo", m_logo_texture, buttonSize, ImVec4(1, 1, 1, 0.1f));
            // 按照 Push 的数量精确 Pop
            ImGui::PopStyleVar(2);    // 弹出 Rounding 和 Spacing
            ImGui::PopStyleColor(1);  // 弹出外部的 Button Color

            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);  // 垂直分割线

            if ( ImGui::BeginMenu(TR("ui.file")) ) {
                ImGui::EndMenu();
            }
            if ( ImGui::BeginMenu(TR("ui.edit")) ) {
                ImGui::EndMenu();
            }
            // --- 3. 计算并跳转到右侧 ---
            float numberOfButtons = 3;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() -
                                 (buttonSize * numberOfButtons));

            // 【修正点 1】统一在此处 Push 样式，计数：Color 1个, Var 2个
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));


            // A. 最小化按钮
            if ( DrawIconButton("##Minimize",
                                m_minimize_texture,
                                buttonSize,
                                ImVec4(1, 1, 1, 0.1f)) ) {
                Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                    .type = NativeEventType::GLFW_ICONFY_WINDOW });
            }

            // B. 最大化 / 还原按钮
            ImGui::SameLine();
            if ( DrawIconButton("##Maximize",
                                m_maxmize_texture,
                                buttonSize,
                                ImVec4(1, 1, 1, 0.1f)) ) {
                Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                    .type = NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE });
            }

            // C. 关闭按钮
            ImGui::SameLine();
            if ( DrawIconButton("##Close",
                                m_close_texture,
                                buttonSize,
                                ImVec4(0.9f, 0.1f, 0.1f, 1.0f)) ) {
                Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                    .type = NativeEventType::GLFW_CLOSE_WINDOW });
            }

            // 按照 Push 的数量精确 Pop
            ImGui::PopStyleVar(2);    // 弹出 Rounding 和 Spacing
            ImGui::PopStyleColor(1);  // 弹出外部的 Button Color

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
            auto& skinCfg = Config::SkinManager::instance();

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
            float   sidebarRatio = std::stof(skinCfg.getLayoutConfig(
                "floating_windows.window1.initial_ratio"));
            auto    dir          = skinCfg.getLayoutConfig(
                "floating_windows.window1.initial_side");
            ImGuiDir sidebarDir;
            if ( dir == "left" ) {
                sidebarDir = ImGuiDir_Left;
            } else if ( dir == "right" ) {
                sidebarDir = ImGuiDir_Right;
            }
            // 从总空间 (dockspace_id) 中拆出 35% 给左边，剩下的 65%
            // 会被分给右边 (dock_id_right)
            dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id,
                                                       sidebarDir,
                                                       sidebarRatio,
                                                       nullptr,
                                                       &dock_id_right);

            // --- 第四步：把窗口填进拆好的坑位里 ---

            // 文件管理器 -> 停靠在左侧坑位
            ImGui::DockBuilderDockWindow(TR("title.file_manager"),
                                         dock_id_left);

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
/// @brief 是否需要重载
bool MainDockSpaceUI::needReload()
{
    return std::exchange(m_needReload, false);
}

/// @brief 重载纹理
void MainDockSpaceUI::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                     vk::Device&         logicalDevice,
                                     vk::CommandPool& cmdPool, vk::Queue& queue)
{
    ///@brief 图标纹理
    m_logo_texture = loadTextureResource(
        Config::SkinManager::instance().getAssetPath("logo"),
        24,
        physicalDevice,
        logicalDevice,
        cmdPool,
        queue);
    ;

    ///@brief 最小化图标纹理
    m_minimize_texture = loadTextureResource(
        Config::SkinManager::instance().getAssetPath("menubar.minimize"),
        24,
        physicalDevice,
        logicalDevice,
        cmdPool,
        queue);

    ///@brief 最大化图标纹理
    m_maxmize_texture = loadTextureResource(
        Config::SkinManager::instance().getAssetPath("menubar.maximize"),
        24,
        physicalDevice,
        logicalDevice,
        cmdPool,
        queue);
    ;

    ///@brief 关闭图标纹理
    m_close_texture = loadTextureResource(
        Config::SkinManager::instance().getAssetPath("menubar.close"),
        24,
        physicalDevice,
        logicalDevice,
        cmdPool,
        queue);
    ;
}

};  // namespace MMM::Graphic::UI
