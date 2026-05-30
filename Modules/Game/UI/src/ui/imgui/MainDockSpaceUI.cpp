#include "ui/imgui/MainDockSpaceUI.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "ui/imgui/SideBarUI.h"
#include <GLFW/glfw3.h>
#include <ImGuiFileDialog.h>
#include <filesystem>
#include <fmt/format.h>
#include <utility>

namespace MMM::UI
{

void MainDockSpaceUI::update(UIManager* sourceManager)
{
    m_mainMenuview.update(sourceManager);

    auto&                engine   = Logic::EditorEngine::instance();
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    // --- 0. IGFD Translations (Currently skipped due to library encapsulation) ---

    if ( !m_initializedWindow && viewport->PlatformHandle ) {
        if ( GLFWwindow* nativeWin = (GLFWwindow*)viewport->PlatformHandle ) {
            m_isMaximized = glfwGetWindowAttrib(nativeWin, GLFW_MAXIMIZED);
            m_initializedWindow = true;
        }
    }

    auto& editorSettings = engine.getEditorConfig().settings;
    auto& aesthetics     = editorSettings.aesthetics;

    float windowPaddingVal = std::floor(aesthetics.windowPadding * dpiScale);

    float sidebarWidth =
        SideBarUI::GetSidebarWidth(dpiScale) + 2.0f * windowPaddingVal;
    float toolbarWidth = std::floor(32.0f * dpiScale) + 2.0f * windowPaddingVal;

    float       extraPaddingY = std::floor(4.0f * dpiScale);
    ImGuiStyle& style         = ImGui::GetStyle();

    // --- 同步全局样式与 DPI 感知的圆角 (Premium Look) ---
    float windowRound      = std::floor(aesthetics.windowRounding * dpiScale);
    float frameRound       = std::floor(aesthetics.frameRounding * dpiScale);
    style.WindowRounding   = windowRound;
    style.ChildRounding    = windowRound;
    style.FrameRounding    = frameRound;
    style.PopupRounding    = frameRound;
    style.TabRounding      = frameRound;
    style.ItemSpacing      = { std::floor(aesthetics.itemSpacing * dpiScale),
                               std::floor(aesthetics.itemSpacing * dpiScale) };
    style.WindowPadding    = { windowPaddingVal, windowPaddingVal };
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize  = 0.0f;

    float menuBarHeight =
        ImGui::GetFontSize() + (style.FramePadding.y + extraPaddingY) * 2.0f;
    float statusBarHeight = menuBarHeight;

    // --- 1. 顶部菜单栏 ---
    renderMenuBar(
        sourceManager, menuBarHeight, sidebarWidth, toolbarWidth, dpiScale);

    // --- 2. 停靠空间 ---
    renderDockingSpace(sourceManager,
                       menuBarHeight,
                       statusBarHeight,
                       sidebarWidth,
                       toolbarWidth);

    // --- 3. 底部状态栏 ---
    renderStatusBar(sourceManager, statusBarHeight, dpiScale);

    // --- 4. 右侧工具栏 (保持原样调用的简易块) ---
    {
        float floatGap = std::floor(aesthetics.windowGap * dpiScale);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - toolbarWidth -
                       floatGap,
                   viewport->WorkPos.y + menuBarHeight + floatGap),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(toolbarWidth,
                                        viewport->WorkSize.y - menuBarHeight -
                                            statusBarHeight - 2.0f * floatGap));
        ImGui::SetNextWindowViewport(viewport->ID);
        m_toolbarView.update(sourceManager);
    }

    // --- 4. 全局弹出式对话框 ---
    if ( editorSettings.filePickerStyle == Config::FilePickerStyle::Unified ) {
        // --- Project Folder Picker ---
        {
            static bool wasOpen = false;
            bool        isOpen =
                ImGuiFileDialog::Instance()->IsOpened("ProjectFolderPicker");
            if ( isOpen && !wasOpen ) {
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                        ImGuiCond_Always,
                                        ImVec2(0.5f, 0.5f));
            }
            wasOpen = isOpen;
        }
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
                ev.m_projectPath = Config::utf8ToPath(folderPath);
                Event::EventBus::instance().publish(ev);
            }
            ImGuiFileDialog::Instance()->Close();
        }

        // --- Save As File Picker ---
        {
            static bool wasOpen = false;
            bool        isOpen =
                ImGuiFileDialog::Instance()->IsOpened("SaveAsFilePicker");
            if ( isOpen && !wasOpen ) {
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                        ImGuiCond_Always,
                                        ImVec2(0.5f, 0.5f));
            }
            wasOpen = isOpen;
        }
        if ( ImGuiFileDialog::Instance()->Display("SaveAsFilePicker",
                                                  ImGuiWindowFlags_NoCollapse,
                                                  { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                std::string filePath =
                    ImGuiFileDialog::Instance()->GetFilePathName();
                filePath =
                    m_mainMenuview.applySaveAsSelectedFormatToPath(filePath);

                auto config = engine.getEditorConfig();
                config.settings.lastFilePickerPath =
                    ImGuiFileDialog::Instance()->GetCurrentPath();
                engine.setEditorConfig(config);

                if ( std::filesystem::exists(Config::utf8ToPath(filePath)) ) {
                    m_pendingOverwritePath     = filePath;
                    this->m_onOverwriteConfirm = [this, filePath]() {
                        m_mainMenuview.requestSaveBeatmapAs(filePath);
                    };
                    m_showOverwriteModal = true;
                } else {
                    m_mainMenuview.requestSaveBeatmapAs(filePath);
                }
            }
            ImGuiFileDialog::Instance()->Close();
        }

        // --- Pack File Picker ---
        {
            static bool wasOpen = false;
            bool        isOpen =
                ImGuiFileDialog::Instance()->IsOpened("PackFilePicker");
            if ( isOpen && !wasOpen ) {
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                        ImGuiCond_Always,
                                        ImVec2(0.5f, 0.5f));
            }
            wasOpen = isOpen;
        }
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

                if ( std::filesystem::exists(Config::utf8ToPath(filePath)) ) {
                    m_pendingOverwritePath     = filePath;
                    this->m_onOverwriteConfirm = [filePath]() {
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdPackBeatmap{ filePath }));
                    };
                    m_showOverwriteModal = true;
                } else {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdPackBeatmap{ filePath }));
                }
            }
            ImGuiFileDialog::Instance()->Close();
        }

        // --- Audio Import Picker ---
        {
            static bool wasOpen = false;
            bool        isOpen =
                ImGuiFileDialog::Instance()->IsOpened("AudioImportPicker");
            if ( isOpen && !wasOpen ) {
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                        ImGuiCond_Always,
                                        ImVec2(0.5f, 0.5f));
            }
            wasOpen = isOpen;
        }
        if ( ImGuiFileDialog::Instance()->Display("AudioImportPicker",
                                                  ImGuiWindowFlags_NoCollapse,
                                                  { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                std::string filePath =
                    ImGuiFileDialog::Instance()->GetFilePathName();

                auto config = engine.getEditorConfig();
                config.settings.lastFilePickerPath =
                    ImGuiFileDialog::Instance()->GetCurrentPath();
                engine.setEditorConfig(config);

                // 不再直接执行指令，而是触发类型选择弹窗
                m_pendingImportPath   = filePath;
                m_showImportTypeModal = true;
            }
            ImGuiFileDialog::Instance()->Close();
        }

        // --- Ascii Font Picker ---
        {
            static bool wasOpen = false;
            bool        isOpen =
                ImGuiFileDialog::Instance()->IsOpened("AsciiFontPicker");
            if ( isOpen && !wasOpen ) {
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                        ImGuiCond_Always,
                                        ImVec2(0.5f, 0.5f));
            }
            wasOpen = isOpen;
        }
        if ( ImGuiFileDialog::Instance()->Display("AsciiFontPicker",
                                                  ImGuiWindowFlags_NoCollapse,
                                                  { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                std::string filePath =
                    ImGuiFileDialog::Instance()->GetFilePathName();
                auto config                        = engine.getEditorConfig();
                config.settings.preferredAsciiFont = filePath;
                engine.setEditorConfig(config);
                if ( auto ctx = Graphic::VKContext::get() )
                    ctx->get().requestFontRebuild();
            }
            ImGuiFileDialog::Instance()->Close();
        }

        // --- Cjk Font Picker ---
        {
            static bool wasOpen = false;
            bool        isOpen =
                ImGuiFileDialog::Instance()->IsOpened("CjkFontPicker");
            if ( isOpen && !wasOpen ) {
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                        ImGuiCond_Always,
                                        ImVec2(0.5f, 0.5f));
            }
            wasOpen = isOpen;
        }
        if ( ImGuiFileDialog::Instance()->Display(
                 "CjkFontPicker", ImGuiWindowFlags_NoCollapse, { 600, 400 }) ) {
            if ( ImGuiFileDialog::Instance()->IsOk() ) {
                std::string filePath =
                    ImGuiFileDialog::Instance()->GetFilePathName();
                auto config                      = engine.getEditorConfig();
                config.settings.preferredCjkFont = filePath;
                engine.setEditorConfig(config);
                if ( auto ctx = Graphic::VKContext::get() )
                    ctx->get().requestFontRebuild();
            }
            ImGuiFileDialog::Instance()->Close();
        }
    }

    // --- 5. 音频导入类型选择模态弹窗 ---
    if ( m_showImportTypeModal ) {
        ImGui::OpenPopup("AudioImportTypeModal");
        m_showImportTypeModal = false;
    }

    {
        static bool importWasOpen = false;
        bool        importIsOpen  = ImGui::IsPopupOpen("AudioImportTypeModal");
        if ( importIsOpen && !importWasOpen ) {
            ImGui::SetNextWindowPos(
                viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        importWasOpen = importIsOpen;
    }
    if ( ImGui::BeginPopupModal(
             "AudioImportTypeModal", nullptr, ImGuiWindowFlags_None) ) {
        ImGui::Text("%s", TR("ui.audio_import.type_hint").data());
        ImGui::Spacing();

        if ( ImGui::Button(TR("ui.audio_track.main").data(), { 120, 0 }) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdImportAudio{
                    m_pendingImportPath, AudioTrackType::Main }));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.audio_track.effect").data(), { 120, 0 }) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdImportAudio{
                    m_pendingImportPath, AudioTrackType::Effect }));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.common.cancel").data(), { 80, 0 }) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // --- 5.5 文件覆盖确认模态弹窗 ---
    if ( m_showOverwriteModal ) {
        ImGui::OpenPopup("OverwriteConfirmModal");
        m_showOverwriteModal = false;
    }

    {
        static bool overwriteWasOpen = false;
        bool overwriteIsOpen = ImGui::IsPopupOpen("OverwriteConfirmModal");
        if ( overwriteIsOpen && !overwriteWasOpen ) {
            ImGui::SetNextWindowPos(
                viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        overwriteWasOpen = overwriteIsOpen;
    }
    if ( ImGui::BeginPopupModal(
             "OverwriteConfirmModal", nullptr, ImGuiWindowFlags_None) ) {
        ImGui::Text("%s", TR("ui.file.overwrite.title").data());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text(
            "%s",
            TR_FMT("ui.file.overwrite.msg", m_pendingOverwritePath).c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( ImGui::Button(TR("ui.common.confirm").data(), { 120, 0 }) ) {
            if ( m_onOverwriteConfirm ) m_onOverwriteConfirm();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.common.cancel").data(), { 120, 0 }) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // --- 6. 退出确认模态弹窗 ---
    if ( viewport->PlatformHandle ) {
        GLFWwindow* nativeWin = (GLFWwindow*)viewport->PlatformHandle;
        if ( glfwWindowShouldClose(nativeWin) ) {
            if ( engine.hasUnsavedChanges() ) {
                // 拦截关闭请求，显示确认对话框
                glfwSetWindowShouldClose(nativeWin, GLFW_FALSE);
                ImGui::OpenPopup(fmt::format("{}###ExitConfirmation",
                                             TR("ui.exit.confirm_title"))
                                     .c_str());
            }
        }
    }

    {
        std::string exitPopupName =
            fmt::format("{}###ExitConfirmation", TR("ui.exit.confirm_title"));
        static bool exitWasOpen = false;
        bool        exitIsOpen  = ImGui::IsPopupOpen(exitPopupName.c_str());
        if ( exitIsOpen && !exitWasOpen ) {
            ImGui::SetNextWindowPos(
                viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        exitWasOpen = exitIsOpen;
    }
    if ( ImGui::BeginPopupModal(
             fmt::format("{}###ExitConfirmation", TR("ui.exit.confirm_title"))
                 .c_str(),
             nullptr,
             ImGuiWindowFlags_None) ) {
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto        session = engine.getActiveSession();
        std::string mapName = "Unknown";
        if ( session && session->getContext().currentBeatmap ) {
            mapName =
                session->getContext().currentBeatmap->m_baseMapMetadata.name;
        }

        ImGui::TextUnformatted(
            TR_FMT("ui.exit.confirm_msg_fmt", mapName).c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( ImGui::Button(TR("ui.file.save").data(),
                           ImVec2(120 * dpiScale, 0)) ) {
            engine.pushCommand(Logic::CmdSaveBeatmap{});
            // 注意：由于保存是异步的，这里直接设置退出可能会导致保存未完成
            // 但在当前的单线程逻辑模型中，指令会按顺序处理
            if ( viewport->PlatformHandle ) {
                glfwSetWindowShouldClose((GLFWwindow*)viewport->PlatformHandle,
                                         GLFW_TRUE);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.exit.dont_save").data(),
                           ImVec2(120 * dpiScale, 0)) ) {
            if ( viewport->PlatformHandle ) {
                glfwSetWindowShouldClose((GLFWwindow*)viewport->PlatformHandle,
                                         GLFW_TRUE);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.help.cancel").data(),
                           ImVec2(120 * dpiScale, 0)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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
