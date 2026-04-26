#include "ui/imgui/MainDockSpaceUI.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include <GLFW/glfw3.h>
#include <ImGuiFileDialog.h>
#include <utility>

namespace MMM::UI
{

void MainDockSpaceUI::update(UIManager* sourceManager)
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float                dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    if ( !m_initializedWindow && viewport->PlatformHandle ) {
        if ( GLFWwindow* nativeWin = (GLFWwindow*)viewport->PlatformHandle ) {
            m_isMaximized = glfwGetWindowAttrib(nativeWin, GLFW_MAXIMIZED);
            m_initializedWindow = true;
        }
    }

    float sidebarWidth =
        std::floor(std::stof(skinCfg.getLayoutConfig("side_bar.width")) * dpiScale);
    float toolbarWidth     = std::floor(32.0f * dpiScale);

    float       extraPaddingY     = std::floor(4.0f * dpiScale);
    ImGuiStyle& style             = ImGui::GetStyle();
    float       menuBarHeight =
        ImGui::GetFontSize() + (style.FramePadding.y + extraPaddingY) * 2.0f;

    // --- 1. 顶部菜单栏 ---
    renderMenuBar(
        sourceManager, menuBarHeight, sidebarWidth, toolbarWidth, dpiScale);

    // --- 2. 停靠空间 ---
    renderDockingSpace(
        sourceManager, menuBarHeight, sidebarWidth, toolbarWidth);

    // --- 3. 右侧工具栏 (保持原样调用的简易块) ---
    {
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - toolbarWidth,
                   viewport->WorkPos.y + menuBarHeight));
        ImGui::SetNextWindowSize(
            ImVec2(toolbarWidth, viewport->WorkSize.y - menuBarHeight));
        ImGui::SetNextWindowViewport(viewport->ID);
        m_toolbarView.update(sourceManager);
    }

    // --- 4. 全局弹出式对话框 ---
    auto& engine         = Logic::EditorEngine::instance();
    auto& editorSettings = engine.getEditorConfig().settings;
    if ( editorSettings.filePickerStyle == Config::FilePickerStyle::Unified ) {
        // --- Project Folder Picker ---
        if ( ImGuiFileDialog::Instance()->Display("ProjectFolderPicker",
                                                  ImGuiWindowFlags_NoCollapse,
                                                  { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                std::string folderPath =
                    ImGuiFileDialog::Instance()->GetFilePathName();
                if ( folderPath.empty() ) {
                    folderPath = ImGuiFileDialog::Instance()->GetCurrentPath();
                }

                auto config = engine.getEditorConfig();
                config.settings.lastFilePickerPath =
                    ImGuiFileDialog::Instance()->GetCurrentPath();
                engine.setEditorConfig(config);

                Event::OpenProjectEvent ev;
                ev.m_projectPath = std::filesystem::path(
                    reinterpret_cast<const char8_t*>(folderPath.c_str()));
                Event::EventBus::instance().publish(ev);
            }
            ImGuiFileDialog::Instance()->Close();
        }

        // --- Save As File Picker ---
        if ( ImGuiFileDialog::Instance()->Display("SaveAsFilePicker",
                                                  ImGuiWindowFlags_NoCollapse,
                                                  { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                std::string filePath =
                    ImGuiFileDialog::Instance()->GetFilePathName();

                auto config = engine.getEditorConfig();
                config.settings.lastFilePickerPath =
                    ImGuiFileDialog::Instance()->GetCurrentPath();
                engine.setEditorConfig(config);

                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdSaveBeatmapAs{ filePath }));
            }
            ImGuiFileDialog::Instance()->Close();
        }

        // --- Pack File Picker ---
        if ( ImGuiFileDialog::Instance()->Display("PackFilePicker",
                                                  ImGuiWindowFlags_NoCollapse,
                                                  { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                std::string filePath =
                    ImGuiFileDialog::Instance()->GetFilePathName();

                auto config = engine.getEditorConfig();
                config.settings.lastFilePickerPath =
                    ImGuiFileDialog::Instance()->GetCurrentPath();
                engine.setEditorConfig(config);

                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdPackBeatmap{ filePath }));
            }
            ImGuiFileDialog::Instance()->Close();
        }
    }
}

bool MainDockSpaceUI::needReload()
{
    return std::exchange(m_needReload, false);
}

void MainDockSpaceUI::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                     vk::Device&         logicalDevice,
                                     vk::CommandPool& cmdPool, vk::Queue& queue)
{
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();
    m_logo_texture = loadTextureResource(
        Config::SkinManager::instance().getAssetPath("logo"),
        static_cast<uint32_t>(24 * dpiScale),
        physicalDevice,
        logicalDevice,
        cmdPool,
        queue,
        { { .83f, .83f, .83f, .83f } });
}

};  // namespace MMM::UI
