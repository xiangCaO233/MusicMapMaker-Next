#include "ui/imgui/MainDockSpaceUI.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/GLFWNativeEvent.h"
#include "event/ui/UpdateDragAreaEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include <ImGuiFileDialog.h>
#include <nfd.h>
#include <utility>

namespace MMM::UI
{

void MainDockSpaceUI::update(UIManager* sourceManager)
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    static float         sidebarWidth =
        std::stof(skinCfg.getLayoutConfig("side_bar.width"));

    // 工具栏固定宽度计算 (按钮尺寸 + Padding)
    // 这里我们先预设一个值，后续可以从配置读取
    float toolbarWidth = 26.0f;



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

        // --- [1] 窗口级样式（Push 2 次） ---
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

        ImGui::Begin("TopMenuBarHost", nullptr, menu_flags);

        float buttonSize = ImGui::GetFrameHeight();

        if ( ImGui::BeginMenuBar() ) {
            float  buttonSize          = ImGui::GetFrameHeight();
            ImVec2 defaultFramePadding = ImGui::GetStyle().FramePadding;

            // --- 【核心修复：统一推入扁平化变量】 ---
            // 这两个变量必须在整个 MenuBar 生命周期内生效
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                                0.0f);  // 强制无圆角
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,
                                0.0f);  // 强制无边框

            // --- 定义图标绘制 Lambda ---
            auto DrawIconButton = [&](const char* str_id,
                                      std::unique_ptr<Graphic::VKTexture>& tex,
                                      float  btnSize,
                                      ImVec4 hoverColor) -> bool {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);

                bool clicked = ImGui::Button(str_id, ImVec2(btnSize, btnSize));

                if ( tex ) {
                    ImTextureID imTexId  = (ImTextureID)tex->getImTextureID();
                    float       iconSize = btnSize * 0.65f;
                    ImVec2      p_min    = ImGui::GetItemRectMin();
                    float  offsetX = std::floor((btnSize - iconSize) * 0.5f);
                    float  offsetY = std::floor((btnSize - iconSize) * 0.5f);
                    ImVec2 img_p1  = { p_min.x + offsetX, p_min.y + offsetY };
                    ImVec2 img_p2  = { img_p1.x + iconSize,
                                       img_p1.y + iconSize };
                    ImU32  tint    = ImGui::IsItemActive()
                                         ? IM_COL32(180, 180, 180, 255)
                                         : IM_COL32_WHITE;
                    ImGui::GetWindowDrawList()->AddImage(
                        imTexId, img_p1, img_p2, { 0, 0 }, { 1, 1 }, tint);
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(1);
                return clicked;
            };

            auto DrawFontIconButton = [&](const char* icon,
                                          float       btnSize,
                                          ImVec4      hoverColor) -> bool {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);

                // 应用图标颜色
                Config::Color iconColor = skinCfg.getColor("icon");
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImVec4(iconColor.r, iconColor.g, iconColor.b, iconColor.a));

                bool clicked = ImGui::Button(icon, ImVec2(btnSize, btnSize));

                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(1);
                return clicked;
            };


            // ================== 1. Logo 区域 ==================
            ImGui::SetCursorPosX(0.0f);
            DrawIconButton(
                "##logo", m_logo_texture, buttonSize, ImVec4(1, 1, 1, 0.1f));

            // ================== 2. 标准菜单区域 ==================
            // 菜单项需要正常的 Padding，所以局部推入
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(10.0f, defaultFramePadding.y));
            ImGui::SetCursorPosX(buttonSize + 4.0f);

            // 应用菜单字体
            ImFont* menuFont = skinCfg.getFont("menu");
            if ( menuFont ) ImGui::PushFont(menuFont);

            m_mainMenuview.update();

            if ( menuFont ) ImGui::PopFont();

            ImGui::PopStyleVar(1);  // 弹出菜单专用 Padding

            // --- 记录拖拽区域起点 ---
            float dragStartX = ImGui::GetCursorPosX();

            // ================== 3. 右侧按钮组区域 ==================
            float numberOfButtons = 3;
            float dragEndX =
                ImGui::GetWindowWidth() - (buttonSize * numberOfButtons);
            ImGui::SetCursorPosX(dragEndX);

            // --- 动态上报拖拽区域 ---
            static float lastDragStartX = -1.0f;
            static float lastDragEndX   = -1.0f;
            if ( dragStartX != lastDragStartX || dragEndX != lastDragEndX ) {
                lastDragStartX = dragStartX;
                lastDragEndX   = dragEndX;

                Event::UpdateDragAreaEvent e;
                e.uiManager    = sourceManager;
                e.sourceUiName = "TopMenuBarHost";
                e.areas.push_back(
                    { dragStartX, 0.0f, dragEndX - dragStartX, menuBarHeight });
                Event::EventBus::instance().publish(e);
            }

            // --- 跨平台双击最大化支持 ---
            ImGui::SetCursorPosX(dragStartX);
            ImGui::InvisibleButton(
                "DragArea", ImVec2(dragEndX - dragStartX, menuBarHeight));
            if ( ImGui::IsItemHovered() &&
                 ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
                Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                    .type = NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE });
            }
            ImGui::SetCursorPosX(dragEndX);

            // 按钮组之间不留缝隙
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

            if ( DrawFontIconButton(
                     ICON_MMM_MINIMIZE, buttonSize, ImVec4(1, 1, 1, 0.1f)) ) {
                Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                    .type = NativeEventType::GLFW_ICONFY_WINDOW });
            }
            ImGui::SameLine();
            if ( DrawFontIconButton(
                     ICON_MMM_MAXIMIZE, buttonSize, ImVec4(1, 1, 1, 0.1f)) ) {
                Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                    .type = NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE });
            }
            ImGui::SameLine();
            if ( DrawFontIconButton(ICON_MMM_CLOSE,
                                    buttonSize,
                                    ImVec4(0.9f, 0.1f, 0.1f, 1.0f)) ) {
                Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                    .type = NativeEventType::GLFW_CLOSE_WINDOW });
            }

            ImGui::PopStyleVar(1);  // 弹出 ItemSpacing

            // 弹出最开始推入的 FrameRounding 和 FrameBorderSize ---
            ImGui::PopStyleVar(2);

            ImGui::EndMenuBar();
        }
        ImGui::End();

        // --- [4] 弹出窗口级样式（Pop 2 次） ---
        ImGui::PopStyleVar(2);
    }

    // ================== B. 右侧停靠空间窗口 ==================
    {
        // 位置偏移：X + sidebarWidth, Y + 菜单高度
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + sidebarWidth,
                                       viewport->WorkPos.y + menuBarHeight));
        // 尺寸：宽 - sidebarWidth - toolbarWidth, 高 - 菜单高度
        ImGui::SetNextWindowSize(
            ImVec2(viewport->WorkSize.x - sidebarWidth - toolbarWidth,
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

        // 为 DockSpace 推入标题字体，确保停靠窗口的 Tab 标签使用大字体
        ImFont* titleFont = skinCfg.getFont("title");
        if ( titleFont ) ImGui::PushFont(titleFont);

        ImGui::Begin("RightDockHost", nullptr, dock_flags);

        // 1. 声明并建立 DockSpace (必须在 Builder 逻辑之前或同一层级)
        ImGuiID dockspace_id = ImGui::GetID("MyMainDockSpace");
        // 注意：这个 DockSpace 必须每帧都调用
        ImGui::DockSpace(
            dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

        if ( titleFont ) ImGui::PopFont();

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
                ImVec2(viewport->WorkSize.x - sidebarWidth - toolbarWidth,
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
            // 从总空间 (dockspace_id) 中拆出 Sidebar
            // 比例给左边，剩下的给右边区域
            dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id,
                                                       sidebarDir,
                                                       sidebarRatio,
                                                       nullptr,
                                                       &dock_id_right);

            // 在右侧区域进一步拆分，分出 20% 给最右侧的预览窗
            ImGuiID dock_id_center_canvas;
            ImGuiID dock_id_preview;
            dock_id_preview =
                ImGui::DockBuilderSplitNode(dock_id_right,
                                            ImGuiDir_Right,
                                            0.20f,
                                            nullptr,
                                            &dock_id_center_canvas);

            // 在主画布区域进一步拆分，分出 20% 给左侧的时间线
            ImGuiID dock_id_center;
            ImGuiID dock_id_timeline;
            dock_id_timeline =
                ImGui::DockBuilderSplitNode(dock_id_center_canvas,
                                            ImGuiDir_Left,
                                            0.20f,
                                            nullptr,
                                            &dock_id_center);

            // --- 第四步：把窗口填进拆好的坑位里 ---

            // 文件管理器 -> 停靠在左侧坑位
            ImGui::DockBuilderDockWindow("SideBarManager", dock_id_left);

            // 时间线 -> 停靠在中心左侧
            ImGui::DockBuilderDockWindow("Timeline", dock_id_timeline);

            // 主画布 -> 停靠在中心坑位
            ImGui::DockBuilderDockWindow("Basic2DCanvas", dock_id_center);

            // 预览窗口 -> 停靠在最右侧
            ImGui::DockBuilderDockWindow("PreviewWindow", dock_id_preview);

            // --- 第五步：完成构建 ---
            ImGui::DockBuilderFinish(dockspace_id);
        }

        ImGui::End();
        ImGui::PopStyleVar(3);

        // ================== C. 右侧工具栏窗口 (固定) ==================
        {
            ImGui::SetNextWindowPos(ImVec2(
                viewport->WorkPos.x + viewport->WorkSize.x - toolbarWidth,
                viewport->WorkPos.y + menuBarHeight));
            ImGui::SetNextWindowSize(
                ImVec2(toolbarWidth, viewport->WorkSize.y - menuBarHeight));
            ImGui::SetNextWindowViewport(viewport->ID);
            m_toolbarView.update(sourceManager);
        }

        // ImGui::ShowDemoWindow();
    }

    // 处理全局的 ImGuiFileDialog 的显示 (统一文件选择器)
    auto& engine         = Logic::EditorEngine::instance();
    auto& editorSettings = engine.getEditorConfig().settings;
    if ( editorSettings.filePickerStyle == Config::FilePickerStyle::Unified &&
         ImGuiFileDialog::Instance()->Display("ProjectFolderPicker",
                                              ImGuiWindowFlags_NoCollapse,
                                              { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            std::string folderPath =
                ImGuiFileDialog::Instance()->GetFilePathName();
            if ( folderPath.empty() ) {
                folderPath = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            Event::OpenProjectEvent ev;
            ev.m_projectPath = folderPath;
            Event::EventBus::instance().publish(ev);
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if ( editorSettings.filePickerStyle == Config::FilePickerStyle::Unified &&
         ImGuiFileDialog::Instance()->Display(
             "PackFilePicker", ImGuiWindowFlags_NoCollapse, { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            std::string filePath =
                ImGuiFileDialog::Instance()->GetFilePathName();
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdPackBeatmap{ filePath }));
        }
        ImGuiFileDialog::Instance()->Close();
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
        queue,
        { { .83f, .83f, .83f, .83f } });
}

};  // namespace MMM::UI
