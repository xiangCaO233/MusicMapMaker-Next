#include "ui/imgui/MainDockSpaceUI.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "graphic/glfw/window/adapters/IWindowFrameAdapter.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/utils/UIWidgetUtils.h"
#include <GLFW/glfw3.h>
#include <ImGuiFileDialog.h>
#include <filesystem>
#include <fmt/format.h>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 无边框窗口缩放热区基础宽度。
constexpr float NATIVE_FRAME_RESIZE_HIT_THICKNESS = 7.0f;

/// @brief 无边框窗口自绘边框基础宽度。
constexpr float NATIVE_FRAME_BORDER_THICKNESS = 1.0f;

/// @brief 无边框窗口自绘内阴影层数。
constexpr int NATIVE_FRAME_SHADOW_LAYERS = 5;

/// @brief 无边框窗口自绘圆角基础半径。
constexpr float NATIVE_FRAME_ROUNDING = 10.0f;

/// @brief 无边框窗口边缘命中结果。
struct NativeFrameHit {
    /// @brief 是否命中可缩放边缘。
    bool m_hit{ false };

    /// @brief 命中的缩放方向。
    Graphic::WindowFrameResizeEdge m_edge{
        Graphic::WindowFrameResizeEdge::Right
    };

    /// @brief 命中方向对应的 ImGui 鼠标光标。
    ImGuiMouseCursor m_cursor{ ImGuiMouseCursor_Arrow };
};

/// @brief 根据主视口和鼠标位置解析无边框窗口缩放命中。
/// @param viewport 主 ImGui 视口。
/// @param dpiScale 当前 DPI 缩放。
/// @return 缩放命中结果。
/// @warning UI 热路径：每帧主窗口边缘检测调用；只做常量规模几何判断。
NativeFrameHit resolveNativeFrameHit(const ImGuiViewport& viewport,
                                     float                dpiScale)
{
    if ( !ImGui::IsMousePosValid() ) {
        return {};
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 min   = viewport.Pos;
    const ImVec2 max   = { viewport.Pos.x + viewport.Size.x,
                           viewport.Pos.y + viewport.Size.y };
    if ( mouse.x < min.x || mouse.y < min.y || mouse.x > max.x ||
         mouse.y > max.y ) {
        return {};
    }

    const float thickness = std::max(
        4.0f, std::floor(NATIVE_FRAME_RESIZE_HIT_THICKNESS * dpiScale));
    const bool left   = mouse.x <= min.x + thickness;
    const bool right  = mouse.x >= max.x - thickness;
    const bool top    = mouse.y <= min.y + thickness;
    const bool bottom = mouse.y >= max.y - thickness;

    if ( top && left ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::TopLeft,
                 ImGuiMouseCursor_ResizeNWSE };
    }
    if ( top && right ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::TopRight,
                 ImGuiMouseCursor_ResizeNESW };
    }
    if ( bottom && left ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::BottomLeft,
                 ImGuiMouseCursor_ResizeNESW };
    }
    if ( bottom && right ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::BottomRight,
                 ImGuiMouseCursor_ResizeNWSE };
    }
    if ( left ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::Left,
                 ImGuiMouseCursor_ResizeEW };
    }
    if ( right ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::Right,
                 ImGuiMouseCursor_ResizeEW };
    }
    if ( top ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::Top,
                 ImGuiMouseCursor_ResizeNS };
    }
    if ( bottom ) {
        return { true,
                 Graphic::WindowFrameResizeEdge::Bottom,
                 ImGuiMouseCursor_ResizeNS };
    }

    return {};
}
}  // namespace

void MainDockSpaceUI::update(UIManager* sourceManager)
{
    m_mainMenuview.update(sourceManager);

    auto&                engine   = Logic::EditorEngine::instance();
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    ImGuiViewport*       viewport = ImGui::GetMainViewport();
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    // --- 0. IGFD Translations (Currently skipped due to library encapsulation) ---

    if ( viewport->PlatformHandle ) {
        if ( GLFWwindow* nativeWin = (GLFWwindow*)viewport->PlatformHandle ) {
            m_isMaximized =
                glfwGetWindowAttrib(nativeWin, GLFW_MAXIMIZED) == GLFW_TRUE;
        }
    }

    auto& editorSettings = engine.getEditorConfig().settings;
    auto& aesthetics     = editorSettings.aesthetics;

    float windowPaddingVal = std::floor(aesthetics.windowPadding * dpiScale);

    float sidebarWidth =
        SideBarUI::GetSidebarWidth(dpiScale) + 2.0f * windowPaddingVal;
    float toolbarWidth = std::floor(32.0f * dpiScale) + 2.0f * windowPaddingVal;
    float toolbarLayoutWidth =
        editorSettings.fixedToolWindow ? toolbarWidth : 0.0f;

    float       extraPaddingY = std::floor(4.0f * dpiScale);
    ImGuiStyle& style         = ImGui::GetStyle();

    // --- 同步全局样式与 DPI 感知的圆角 (Premium Look) ---
    float windowRound     = std::floor(aesthetics.windowRounding * dpiScale);
    float frameRound      = std::floor(aesthetics.frameRounding * dpiScale);
    style.WindowRounding  = windowRound;
    style.ChildRounding   = windowRound;
    style.FrameRounding   = frameRound;
    style.PopupRounding   = frameRound;
    style.TabRounding     = frameRound;
    style.ItemSpacing     = { std::floor(aesthetics.itemSpacing * dpiScale),
                              std::floor(aesthetics.itemSpacing * dpiScale) };
    style.WindowPadding   = { windowPaddingVal, windowPaddingVal };
    style.FrameBorderSize = 0.0f;

    float menuBarHeight =
        ImGui::GetFontSize() + (style.FramePadding.y + extraPaddingY) * 2.0f;
    float statusBarHeight = menuBarHeight;

    handleNativeWindowFrameInteraction(sourceManager, dpiScale);

    // --- 1. 顶部菜单栏 ---
    renderMenuBar(sourceManager,
                  menuBarHeight,
                  sidebarWidth,
                  toolbarLayoutWidth,
                  dpiScale);

    // --- 2. 停靠空间 ---
    renderDockingSpace(sourceManager,
                       menuBarHeight,
                       statusBarHeight,
                       sidebarWidth,
                       toolbarLayoutWidth);

    // --- 3. 底部状态栏 ---
    renderStatusBar(sourceManager, statusBarHeight, dpiScale);

    // --- 4. 右侧工具栏 (保持原样调用的简易块) ---
    {
        float floatGap = std::floor(aesthetics.windowGap * dpiScale);
        if ( editorSettings.fixedToolWindow ) {
            ImGui::SetNextWindowPos(
                ImVec2(viewport->WorkPos.x + viewport->WorkSize.x -
                           toolbarWidth - floatGap,
                       viewport->WorkPos.y + menuBarHeight + floatGap),
                ImGuiCond_Always);
            ImGui::SetNextWindowSize(
                ImVec2(toolbarWidth,
                       viewport->WorkSize.y - menuBarHeight - statusBarHeight -
                           2.0f * floatGap));
        }
        ImGui::SetNextWindowViewport(viewport->ID);
        m_toolbarView.update(sourceManager);
    }

    renderNativeWindowFrameOverlay(sourceManager, dpiScale);

    // --- 4. 全局弹出式对话框 ---
    if ( editorSettings.filePickerStyle == Config::FilePickerStyle::Unified ) {
        // --- Project Folder Picker ---
        {
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened(
                     "ProjectFolderPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "ProjectFolderPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    std::string folderPath =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    if ( folderPath.empty() ) {
                        folderPath =
                            ImGuiFileDialog::Instance()->GetCurrentPath();
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
        }

        // --- Save As File Picker ---
        {
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("SaveAsFilePicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "SaveAsFilePicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    std::string filePath =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    filePath = m_mainMenuview.applySaveAsSelectedFormatToPath(
                        filePath);

                    if ( std::filesystem::exists(
                             Config::utf8ToPath(filePath)) ) {
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
        }

        // --- Pack File Picker ---
        {
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("PackFilePicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "PackFilePicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    std::string filePath =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    filePath =
                        m_mainMenuview.applyPackSelectedFormatToPath(filePath);

                    auto config = engine.getEditorConfig();
                    config.settings.lastFilePickerPath =
                        ImGuiFileDialog::Instance()->GetCurrentPath();
                    engine.setEditorConfig(config);

                    if ( std::filesystem::exists(
                             Config::utf8ToPath(filePath)) ) {
                        m_pendingOverwritePath     = filePath;
                        this->m_onOverwriteConfirm = [this, filePath]() {
                            m_mainMenuview.requestPackBeatmapTo(filePath);
                        };
                        m_showOverwriteModal = true;
                    } else {
                        m_mainMenuview.requestPackBeatmapTo(filePath);
                    }
                }
                ImGuiFileDialog::Instance()->Close();
            }
        }

        // --- Audio Import Picker ---
        {
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("AudioImportPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "AudioImportPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
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
        }

        // --- Ascii Font Picker ---
        {
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("AsciiFontPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "AsciiFontPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
                if ( ImGuiFileDialog::Instance()->IsOk() ) {
                    std::string filePath =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    auto config = engine.getEditorConfig();
                    config.settings.preferredAsciiFont = filePath;
                    engine.setEditorConfig(config);
                    if ( auto ctx = Graphic::VKContext::get() )
                        ctx->get().requestFontRebuild();
                }
                ImGuiFileDialog::Instance()->Close();
            }
        }

        // --- Cjk Font Picker ---
        {
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened("CjkFontPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "CjkFontPicker",
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings,
                     { 600, 400 }) ) {
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
    }

    // --- 5. 音频导入类型选择模态弹窗 ---
    if ( m_showImportTypeModal ) {
        ImGui::OpenPopup("AudioImportTypeModal");
        m_showImportTypeModal = false;
    }

    {
        Utils::CenteredModalPopupScope importModalScope(dpiScale);
        if ( importModalScope.begin("AudioImportTypeModal") ) {
            ImGui::Text("%s", TR("ui.audio_import.type_hint").data());
            ImGui::Spacing();

            if ( ImGui::Button(TR("ui.audio_track.main").data(), { 120, 0 }) ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdImportAudio{
                        m_pendingImportPath, AudioTrackType::Main }));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_track.effect").data(),
                               { 120, 0 }) ) {
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
    }

    // --- 5.5 文件覆盖确认模态弹窗 ---
    if ( m_showOverwriteModal ) {
        ImGui::OpenPopup("OverwriteConfirmModal");
        m_showOverwriteModal = false;
    }

    {
        Utils::CenteredModalPopupScope overwriteModalScope(dpiScale);
        if ( overwriteModalScope.begin("OverwriteConfirmModal") ) {
            ImGui::Text("%s", TR("ui.file.overwrite.title").data());
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("%s",
                        TR_FMT("ui.file.overwrite.msg", m_pendingOverwritePath)
                            .c_str());
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
    }

    // --- 6. 退出确认模态弹窗 ---
    if ( viewport->PlatformHandle ) {
        GLFWwindow* nativeWin = (GLFWwindow*)viewport->PlatformHandle;
        if ( glfwWindowShouldClose(nativeWin) ) {
            if ( engine.hasUnsavedChanges() ) {
                // 拦截关闭请求，显示确认对话框
                glfwSetWindowShouldClose(nativeWin, GLFW_FALSE);
                const std::string exitPopupName = fmt::format(
                    "{}###ExitConfirmation", TR("ui.exit.confirm_title"));
                ImGui::OpenPopup(exitPopupName.c_str());
            }
        }
    }

    const std::string exitPopupName =
        fmt::format("{}###ExitConfirmation", TR("ui.exit.confirm_title"));
    {
        Utils::CenteredModalPopupScope exitModalScope(dpiScale);
        if ( exitModalScope.begin(exitPopupName.c_str()) ) {
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto        session = engine.getActiveSession();
            std::string mapName = "Unknown";
            if ( session && session->getContext().currentBeatmap ) {
                mapName = session->getContext()
                              .currentBeatmap->m_baseMapMetadata.name;
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
                    glfwSetWindowShouldClose(
                        (GLFWwindow*)viewport->PlatformHandle, GLFW_TRUE);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.exit.dont_save").data(),
                               ImVec2(120 * dpiScale, 0)) ) {
                if ( viewport->PlatformHandle ) {
                    glfwSetWindowShouldClose(
                        (GLFWwindow*)viewport->PlatformHandle, GLFW_TRUE);
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
}

void MainDockSpaceUI::handleNativeWindowFrameInteraction(
    UIManager* sourceManager, float dpiScale)
{
    if ( !sourceManager || m_isMaximized ) {
        return;
    }

    auto* frameAdapter = sourceManager->getWindowFrameAdapter();
    if ( !frameAdapter || !frameAdapter->supportsClientFrameRequests() ) {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if ( !viewport ) {
        return;
    }

    const NativeFrameHit hit = resolveNativeFrameHit(*viewport, dpiScale);
    if ( !hit.m_hit ) {
        return;
    }

    ImGui::SetMouseCursor(hit.m_cursor);
}

void MainDockSpaceUI::renderNativeWindowFrameOverlay(UIManager* sourceManager,
                                                     float      dpiScale) const
{
    auto* frameAdapter =
        sourceManager ? sourceManager->getWindowFrameAdapter() : nullptr;
    if ( !frameAdapter || !frameAdapter->usesClientFrameOverlay() ) {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if ( !viewport ) {
        return;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
    if ( !drawList ) {
        return;
    }

    const ImVec2 min = viewport->Pos;
    const ImVec2 max = { viewport->Pos.x + viewport->Size.x,
                         viewport->Pos.y + viewport->Size.y };
    const float  borderThickness =
        std::max(1.0f, std::floor(NATIVE_FRAME_BORDER_THICKNESS * dpiScale));
    const float rounding =
        m_isMaximized
            ? 0.0f
            : std::floor(NATIVE_FRAME_ROUNDING * std::max(1.0f, dpiScale));

    if ( !m_isMaximized ) {
        for ( int layer = NATIVE_FRAME_SHADOW_LAYERS; layer > 0; --layer ) {
            const float inset = static_cast<float>(layer);
            const float alpha =
                0.035f *
                (static_cast<float>(NATIVE_FRAME_SHADOW_LAYERS - layer + 1) /
                 static_cast<float>(NATIVE_FRAME_SHADOW_LAYERS));
            const ImU32 shadowColor =
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, alpha));
            drawList->AddRect({ min.x + inset, min.y + inset },
                              { max.x - inset, max.y - inset },
                              shadowColor,
                              std::max(0.0f, rounding - inset),
                              0,
                              borderThickness);
        }
    }

    ImVec4 border = ImGui::GetStyleColorVec4(ImGuiCol_Border);
    border.w      = std::max(border.w, 0.55f);
    drawList->AddRect({ min.x + 0.5f, min.y + 0.5f },
                      { max.x - 0.5f, max.y - 0.5f },
                      ImGui::GetColorU32(border),
                      rounding,
                      0,
                      borderThickness);
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
