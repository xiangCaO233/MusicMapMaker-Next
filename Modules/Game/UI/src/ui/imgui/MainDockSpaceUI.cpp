#include "ui/imgui/MainDockSpaceUI.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/GLFWNativeEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/glfw/window/adapters/IWindowFrameAdapter.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/utils/UIWidgetUtils.h"
#include <GLFW/glfw3.h>
#include <ImGuiFileDialog.h>
#include <concurrentqueue.h>
#include <filesystem>
#include <fmt/format.h>
#include <nfd.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MMM::UI
{

MainDockSpaceUI::MainDockSpaceUI(const std::string& name)
    : IUIView(name), ITextureLoader(name)
{
    Event::EventBus::instance().subscribe<Event::GLFWNativeEvent>(
        [&](Event::GLFWNativeEvent event) {
            if ( event.hasStateChange &&
                 event.type ==
                     Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE ) {
                m_isMaximized = event.isMaximized;
            }
        });

    Event::EventBus::instance().subscribe<Event::AudioImportTriggerEvent>(
        [&](Event::AudioImportTriggerEvent event) {
            m_pendingImportPath   = event.path;
            m_showImportTypeModal = true;
        });
}

MainDockSpaceUI::~MainDockSpaceUI() = default;

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

/// @brief 临时项目路径提示载荷。
struct TemporaryProjectPromptPayload {
    /// @brief 原始谱面包路径。
    std::string sourcePackagePath;

    /// @brief 临时项目缓存目录。
    std::string cacheProjectPath;
};

/// @brief 临时项目保存结果载荷。
struct TemporaryProjectSaveResultPayload {
    /// @brief 是否保存成功。
    bool success{ false };

    /// @brief 保存成功后的正式项目目录。
    std::string savedProjectPath;

    /// @brief 失败时的错误信息。
    std::string errorMessage;
};

/// @brief 音频资源变更结果的跨线程 UI 载荷。
struct AudioResourceMutationResultPayload {
    /// @brief 本次资源操作类型。
    Event::AudioResourceMutationOperation operation{
        Event::AudioResourceMutationOperation::UpdateType
    };

    /// @brief 操作目标的稳定资源 ID。
    std::string resourceId;

    /// @brief 操作是否成功。
    bool success{ false };

    /// @brief 阻止操作的全部谱面路径。
    std::vector<std::string> blockingBeatmapPaths;

    /// @brief 逻辑层返回的失败原因。
    std::string errorMessage;
};

/// @brief 获取临时项目只读提示队列。
moodycamel::ConcurrentQueue<TemporaryProjectPromptPayload>&
temporaryProjectEditBlockedQueue()
{
    static moodycamel::ConcurrentQueue<TemporaryProjectPromptPayload> queue;
    return queue;
}

/// @brief 获取临时项目关闭提示队列。
moodycamel::ConcurrentQueue<TemporaryProjectPromptPayload>&
temporaryProjectClosePromptQueue()
{
    static moodycamel::ConcurrentQueue<TemporaryProjectPromptPayload> queue;
    return queue;
}

/// @brief 获取临时项目保存结果队列。
moodycamel::ConcurrentQueue<TemporaryProjectSaveResultPayload>&
temporaryProjectSaveResultQueue()
{
    static moodycamel::ConcurrentQueue<TemporaryProjectSaveResultPayload> queue;
    return queue;
}

/// @brief 获取音频资源变更结果队列。
moodycamel::ConcurrentQueue<AudioResourceMutationResultPayload>&
audioResourceMutationResultQueue()
{
    static moodycamel::ConcurrentQueue<AudioResourceMutationResultPayload>
        queue;
    return queue;
}

/// @brief 获取协作访客本机项目打开拦截通知队列。
moodycamel::ConcurrentQueue<bool>& collaborationProjectOpenBlockedQueue()
{
    static moodycamel::ConcurrentQueue<bool> queue;
    return queue;
}

/// @brief 获取离线房间谱面编辑拦截通知队列。
moodycamel::ConcurrentQueue<bool>& collaborationOfflineEditBlockedQueue()
{
    static moodycamel::ConcurrentQueue<bool> queue;
    return queue;
}

/// @brief 构建音频资源变更的完整用户提示。
/// @param result 待展示结果。
/// @return 包含目标资源及全部阻止谱面的多行提示。
std::string buildAudioResourceMutationMessage(
    const AudioResourceMutationResultPayload& result)
{
    std::string message;
    if ( result.success ) {
        switch ( result.operation ) {
        case Event::AudioResourceMutationOperation::UpdateType:
            message = "音频资源类型已更新";
            break;
        case Event::AudioResourceMutationOperation::Rename:
            message = "音频轨道与文件名已重命名";
            break;
        case Event::AudioResourceMutationOperation::Remove:
            message = "音频资源已删除";
            break;
        case Event::AudioResourceMutationOperation::MovePath:
            message = "音频资源路径已同步";
            break;
        }
    } else {
        message = result.errorMessage.empty() ? "音频资源操作失败"
                                              : result.errorMessage;
    }

    if ( !result.resourceId.empty() ) {
        message += fmt::format("\n资源：{}", result.resourceId);
    }
    if ( !result.blockingBeatmapPaths.empty() ) {
        message += "\n阻止操作的谱面：";
        for ( const auto& beatmapPath : result.blockingBeatmapPaths ) {
            message += fmt::format("\n- {}", beatmapPath);
        }
    }
    return message;
}

/// @brief 根据当前临时项目构建提示载荷。
TemporaryProjectPromptPayload makeCurrentTemporaryProjectPayload()
{
    const auto info =
        Logic::EditorEngine::instance().currentTemporaryProjectInfo();
    return TemporaryProjectPromptPayload{
        Config::pathToUtf8(info.m_sourcePackagePath),
        Config::pathToUtf8(info.m_cacheProjectPath),
    };
}

/// @brief 订阅临时项目及音频资源变更结果事件。
void ensureTemporaryProjectSubscriptions()
{
    static bool subscribed = false;
    if ( subscribed ) return;

    auto& eventBus = Event::EventBus::instance();
    eventBus.subscribe<Event::TemporaryProjectEditBlockedEvent>(
        [](const Event::TemporaryProjectEditBlockedEvent& event) {
            temporaryProjectEditBlockedQueue().enqueue(
                TemporaryProjectPromptPayload{ event.m_sourcePackagePath,
                                               event.m_cacheProjectPath });
        });
    eventBus.subscribe<Event::TemporaryProjectClosePromptRequestedEvent>(
        [](const Event::TemporaryProjectClosePromptRequestedEvent&) {
            temporaryProjectClosePromptQueue().enqueue(
                makeCurrentTemporaryProjectPayload());
        });
    eventBus.subscribe<Event::TemporaryProjectSaveResultEvent>(
        [](const Event::TemporaryProjectSaveResultEvent& event) {
            temporaryProjectSaveResultQueue().enqueue(
                TemporaryProjectSaveResultPayload{ event.m_success,
                                                   event.m_savedProjectPath,
                                                   event.m_errorMessage });
        });
    eventBus.subscribe<Event::AudioResourceMutationResultEvent>(
        [](const Event::AudioResourceMutationResultEvent& event) {
            audioResourceMutationResultQueue().enqueue(
                AudioResourceMutationResultPayload{
                    event.m_operation,
                    event.m_resourceId,
                    event.m_success,
                    event.m_blockingBeatmapPaths,
                    event.m_errorMessage,
                });
        });
    eventBus.subscribe<Event::CollaborationProjectOpenBlockedEvent>(
        [](const Event::CollaborationProjectOpenBlockedEvent&) {
            collaborationProjectOpenBlockedQueue().enqueue(true);
        });
    eventBus.subscribe<Event::CollaborationOfflineEditBlockedEvent>(
        [](const Event::CollaborationOfflineEditBlockedEvent&) {
            collaborationOfflineEditBlockedQueue().enqueue(true);
        });

    subscribed = true;
}

/// @brief 在当前内容区域内绘制可换行文本。
/// @param text 待绘制文本。
/// @warning UI 绘制路径：只设置 ImGui 文本换行位置并绘制文本。
void drawWrappedText(std::string_view text)
{
    const char* textBegin = text.empty() ? "" : text.data();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                           ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(textBegin, textBegin + text.size());
    ImGui::PopTextWrapPos();
}

/// @brief 绘制标签和可换行值。
/// @param label 标签文本。
/// @param value 值文本。
/// @warning UI 绘制路径：只绘制 ImGui 文本。
void drawWrappedLabelValue(std::string_view label, std::string_view value)
{
    const char* labelBegin = label.empty() ? "" : label.data();
    ImGui::TextUnformatted(labelBegin, labelBegin + label.size());
    ImGui::SameLine();
    drawWrappedText(value);
}

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

/// @brief 更新主 DockSpace、顶部菜单、全局文件对话框和应用级模态弹窗。
/// @param sourceManager 当前 UI 管理器。
/// @warning UI 热路径：每帧执行；除用户明确触发的文件选择器和窗口关闭确认外，
/// 禁止加入文件系统扫描、阻塞等待或完整数据重建。
void MainDockSpaceUI::update(UIManager* sourceManager)
{
    ensureTemporaryProjectSubscriptions();
    consumeTemporaryProjectQueues();

    const float deltaSeconds = ImGui::GetIO().DeltaTime;
    m_statusMessageService.update(deltaSeconds);
    m_saveResultFeedback.update(deltaSeconds);
    m_beatmapLoadDiagnosticFeedback.update();
    m_mainMenuview.update(sourceManager, m_statusMessageService);

    auto&                engine   = Logic::EditorEngine::instance();
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    ImGuiViewport*       viewport = ImGui::GetMainViewport();
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();
    m_saveResultFeedback.render(dpiScale);

    // --- 0. IGFD 翻译，当前因库封装暂跳过 ---

    if ( auto* nativeWindow =
             sourceManager ? sourceManager->getNativeWindow() : nullptr ) {
        m_isMaximized = nativeWindow->isWindowMaximized();
    }

    const auto& editorSettings =
        Config::AppConfig::instance().getEditorConfig().settings;
    const auto& aesthetics = editorSettings.aesthetics;

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
        // --- 项目目录选择器 ---
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

        // --- 临时项目保存目录选择器 ---
        {
            Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
            if ( ImGuiFileDialog::Instance()->IsOpened(
                     "TemporaryProjectSaveFolderPicker") ) {
                Utils::prepareCenteredModalWindow({ 600, 400 });
            }
            if ( ImGuiFileDialog::Instance()->Display(
                     "TemporaryProjectSaveFolderPicker",
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

                    m_temporaryProjectSaveInProgress = true;
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdSaveTemporaryProject{ folderPath }));
                }
                ImGuiFileDialog::Instance()->Close();
            }
        }

        // --- 音频导入选择器 ---
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

        // --- ASCII 字体选择器 ---
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

        // --- CJK 字体选择器 ---
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
        ::MMM::UI::FeedbackOpenPopup("AudioImportTypeModal");
        m_showImportTypeModal = false;
    }

    {
        Utils::CenteredModalPopupScope importModalScope(dpiScale);
        if ( importModalScope.begin("AudioImportTypeModal") ) {
            ImGui::Text("%s", TR("ui.audio_import.type_hint").data());
            ImGui::Spacing();

            if ( ::MMM::UI::FeedbackButton(TR("ui.audio_track.main").data(),
                                           { 120, 0 }) ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdImportAudio{
                        m_pendingImportPath, AudioTrackType::Main }));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.audio_track.effect").data(),
                                           { 120, 0 }) ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdImportAudio{
                        m_pendingImportPath, AudioTrackType::Effect }));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           { 80, 0 }) ) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    // --- 5.5 文件覆盖确认模态弹窗 ---
    if ( m_showOverwriteModal ) {
        ::MMM::UI::FeedbackOpenPopup("OverwriteConfirmModal");
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

            if ( ::MMM::UI::FeedbackButton(TR("ui.common.confirm").data(),
                                           { 120, 0 }) ) {
                if ( m_onOverwriteConfirm ) m_onOverwriteConfirm();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           { 120, 0 }) ) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    renderCollaborationSafetyPopups(dpiScale);
    renderTemporaryProjectPopups(dpiScale, viewport);
    if ( m_exitAfterTemporaryProjectSave && viewport->PlatformHandle ) {
        m_temporaryProjectExitConfirmed = true;
        glfwSetWindowShouldClose(
            static_cast<GLFWwindow*>(viewport->PlatformHandle), GLFW_TRUE);
        m_exitAfterTemporaryProjectSave = false;
    }

    // --- 6. 退出确认模态弹窗 ---
    if ( viewport->PlatformHandle ) {
        GLFWwindow* nativeWin = (GLFWwindow*)viewport->PlatformHandle;
        if ( glfwWindowShouldClose(nativeWin) ) {
            if ( !m_temporaryProjectExitConfirmed &&
                 engine.isTemporaryProjectOpen() ) {
                glfwSetWindowShouldClose(nativeWin, GLFW_FALSE);
                const auto info = engine.currentTemporaryProjectInfo();
                m_temporaryProjectSourcePath =
                    Config::pathToUtf8(info.m_sourcePackagePath);
                m_temporaryProjectCachePath =
                    Config::pathToUtf8(info.m_cacheProjectPath);
                m_temporaryProjectSaveError.clear();
                m_temporaryProjectAfterSaveAction =
                    TemporaryProjectAfterSaveAction::ExitApp;
                m_showTemporaryProjectCloseModal = true;
            } else if ( !m_temporaryProjectExitConfirmed &&
                        engine.hasUnsavedChanges() ) {
                // 拦截关闭请求，显示确认对话框
                glfwSetWindowShouldClose(nativeWin, GLFW_FALSE);
                const std::string exitPopupName = fmt::format(
                    "{}###ExitConfirmation", TR("ui.exit.confirm_title"));
                ::MMM::UI::FeedbackOpenPopup(exitPopupName.c_str());
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

            if ( ::MMM::UI::FeedbackButton(TR("ui.file.save").data(),
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
            if ( ::MMM::UI::FeedbackButton(TR("ui.exit.dont_save").data(),
                                           ImVec2(120 * dpiScale, 0)) ) {
                if ( viewport->PlatformHandle ) {
                    glfwSetWindowShouldClose(
                        (GLFWwindow*)viewport->PlatformHandle, GLFW_TRUE);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.help.cancel").data(),
                                           ImVec2(120 * dpiScale, 0)) ) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // --- 7. 主菜单延迟弹窗宿主 ---
    {
        constexpr ImGuiWindowFlags popupHostFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1.0f, 1.0f), ImGuiCond_Always);
        if ( ImGui::Begin("MainMenuPopupHost", nullptr, popupHostFlags) ) {
            m_pgoUploadConsentWindow.render(dpiScale);
            m_mainMenuview.renderDeferredPopups(
                sourceManager, dpiScale, m_statusMessageService);
        }
        ImGui::End();
    }
}

/// @brief 处理临时项目提示、保存结果及音频资源变更结果队列。
void MainDockSpaceUI::consumeTemporaryProjectQueues()
{
    TemporaryProjectPromptPayload prompt;
    while ( temporaryProjectEditBlockedQueue().try_dequeue(prompt) ) {
        m_temporaryProjectSourcePath = prompt.sourcePackagePath;
        m_temporaryProjectCachePath  = prompt.cacheProjectPath;
        m_temporaryProjectSaveError.clear();
        m_temporaryProjectAfterSaveAction =
            TemporaryProjectAfterSaveAction::None;
        m_showTemporaryProjectReadOnlyModal = true;
    }

    while ( temporaryProjectClosePromptQueue().try_dequeue(prompt) ) {
        m_temporaryProjectSourcePath = prompt.sourcePackagePath;
        m_temporaryProjectCachePath  = prompt.cacheProjectPath;
        m_temporaryProjectSaveError.clear();
        m_temporaryProjectAfterSaveAction =
            TemporaryProjectAfterSaveAction::CloseProject;
        m_showTemporaryProjectCloseModal = true;
    }

    TemporaryProjectSaveResultPayload saveResult;
    while ( temporaryProjectSaveResultQueue().try_dequeue(saveResult) ) {
        m_temporaryProjectSaveInProgress = false;
        if ( !saveResult.success ) {
            m_temporaryProjectSaveError = saveResult.errorMessage.empty()
                                              ? "保存临时项目失败"
                                              : saveResult.errorMessage;
            if ( m_temporaryProjectAfterSaveAction ==
                 TemporaryProjectAfterSaveAction::None ) {
                m_showTemporaryProjectReadOnlyModal = true;
            } else {
                m_showTemporaryProjectCloseModal = true;
            }
            continue;
        }

        m_temporaryProjectSaveError.clear();
        m_temporaryProjectSourcePath.clear();
        m_temporaryProjectCachePath = saveResult.savedProjectPath;
        if ( m_temporaryProjectAfterSaveAction ==
             TemporaryProjectAfterSaveAction::CloseProject ) {
            Event::EventBus::instance().publish(
                Event::ProjectCloseRequestedEvent{});
        } else if ( m_temporaryProjectAfterSaveAction ==
                    TemporaryProjectAfterSaveAction::ExitApp ) {
            m_exitAfterTemporaryProjectSave = true;
        }
        m_temporaryProjectAfterSaveAction =
            TemporaryProjectAfterSaveAction::None;
    }

    AudioResourceMutationResultPayload mutationResult;
    while ( audioResourceMutationResultQueue().try_dequeue(mutationResult) ) {
        if ( auto context = Graphic::VKContext::get() ) {
            context->get().showCenterNotification(
                buildAudioResourceMutationMessage(mutationResult),
                mutationResult.success ? 3.0F : 10.0F);
        }
    }

    bool collaborationSafetySignal = false;
    while ( collaborationProjectOpenBlockedQueue().try_dequeue(
        collaborationSafetySignal) ) {
        m_showCollaborationProjectOpenBlockedModal = true;
    }
    while ( collaborationOfflineEditBlockedQueue().try_dequeue(
        collaborationSafetySignal) ) {
        m_showCollaborationOfflineEditBlockedModal = true;
    }
}

/// @brief 请求选择临时项目正式保存位置。
void MainDockSpaceUI::requestTemporaryProjectSaveFolder()
{
    m_temporaryProjectSaveError.clear();

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    if ( editorSettings.filePickerStyle == Config::FilePickerStyle::Native ) {
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t* outPath = nullptr;
        nfdresult_t  result  = NFD_PickFolder(&outPath, nullptr);
        if ( result == NFD_OKAY ) {
            m_temporaryProjectSaveInProgress = true;
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSaveTemporaryProject{ outPath }));
            NFD_FreePath(outPath);
        }
        return;
    }

    IGFD::FileDialogConfig fdConfig;
    fdConfig.path              = editorSettings.lastFilePickerPath;
    fdConfig.countSelectionMax = 1;
    fdConfig.flags             = ImGuiFileDialogFlags_Modal;
    const bool wasOpen         = ImGuiFileDialog::Instance()->IsOpened(
        "TemporaryProjectSaveFolderPicker");
    ImGuiFileDialog::Instance()->OpenDialog(
        "TemporaryProjectSaveFolderPicker", "保存临时项目", nullptr, fdConfig);
    if ( !wasOpen && ImGuiFileDialog::Instance()->IsOpened(
                         "TemporaryProjectSaveFolderPicker") ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 渲染临时项目只读和关闭确认弹窗。
void MainDockSpaceUI::renderTemporaryProjectPopups(float          dpiScale,
                                                   ImGuiViewport* viewport)
{
    if ( m_showTemporaryProjectReadOnlyModal ) {
        ::MMM::UI::FeedbackOpenPopup(
            "临时项目只读###TemporaryProjectReadOnlyModal");
        m_showTemporaryProjectReadOnlyModal = false;
    }

    {
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin("临时项目只读###TemporaryProjectReadOnlyModal",
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(560.0f * dpiScale, 0.0f)) ) {
            drawWrappedText(
                "当前打开的是临时项目。要修改谱面或项目资源，请先选择正式保存位"
                "置。");
            ImGui::Spacing();
            drawWrappedLabelValue("打开文件：", m_temporaryProjectSourcePath);
            drawWrappedLabelValue("缓存项目：", m_temporaryProjectCachePath);
            if ( !m_temporaryProjectSaveError.empty() ) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                   "%s",
                                   m_temporaryProjectSaveError.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::BeginDisabled(m_temporaryProjectSaveInProgress);
            if ( ::MMM::UI::FeedbackButton("选择保存位置",
                                           ImVec2(140.0f * dpiScale, 0.0f)) ) {
                m_temporaryProjectAfterSaveAction =
                    TemporaryProjectAfterSaveAction::None;
                requestTemporaryProjectSaveFolder();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton("继续只读",
                                           ImVec2(120.0f * dpiScale, 0.0f)) ) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    if ( m_showTemporaryProjectCloseModal ) {
        ::MMM::UI::FeedbackOpenPopup(
            "保存临时项目###TemporaryProjectCloseModal");
        m_showTemporaryProjectCloseModal = false;
    }

    {
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin("保存临时项目###TemporaryProjectCloseModal",
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(560.0f * dpiScale, 0.0f)) ) {
            drawWrappedText("该项目为临时项目，是否保存项目？");
            ImGui::Spacing();
            drawWrappedLabelValue("打开文件：", m_temporaryProjectSourcePath);
            drawWrappedLabelValue("缓存项目：", m_temporaryProjectCachePath);
            if ( !m_temporaryProjectSaveError.empty() ) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                   "%s",
                                   m_temporaryProjectSaveError.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::BeginDisabled(m_temporaryProjectSaveInProgress);
            if ( ::MMM::UI::FeedbackButton("保存项目",
                                           ImVec2(120.0f * dpiScale, 0.0f)) ) {
                requestTemporaryProjectSaveFolder();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton("不保存",
                                           ImVec2(120.0f * dpiScale, 0.0f)) ) {
                if ( m_temporaryProjectAfterSaveAction ==
                     TemporaryProjectAfterSaveAction::ExitApp ) {
                    if ( viewport && viewport->PlatformHandle ) {
                        m_temporaryProjectExitConfirmed = true;
                        glfwSetWindowShouldClose(
                            static_cast<GLFWwindow*>(viewport->PlatformHandle),
                            GLFW_TRUE);
                    }
                } else {
                    Event::EventBus::instance().publish(
                        Event::ProjectCloseRequestedEvent{});
                }
                m_temporaryProjectAfterSaveAction =
                    TemporaryProjectAfterSaveAction::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.help.cancel").data(),
                                           ImVec2(120.0f * dpiScale, 0.0f)) ) {
                m_temporaryProjectAfterSaveAction =
                    TemporaryProjectAfterSaveAction::None;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }
}

void MainDockSpaceUI::renderCollaborationSafetyPopups(float dpiScale)
{
    const std::string offlinePopupId =
        TR("ui.collaboration.offline_edit.title").toString() +
        "###CollaborationOfflineEditBlockedModal";
    const std::string projectPopupId =
        TR("ui.collaboration.project_open_blocked.title").toString() +
        "###CollaborationProjectOpenBlockedModal";
    if ( m_showCollaborationOfflineEditBlockedModal ) {
        FeedbackOpenPopup(offlinePopupId.c_str());
        m_showCollaborationOfflineEditBlockedModal = false;
    } else if ( m_showCollaborationProjectOpenBlockedModal ) {
        FeedbackOpenPopup(projectPopupId.c_str());
        m_showCollaborationProjectOpenBlockedModal = false;
    }

    {
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(offlinePopupId.c_str(),
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(520.0F * dpiScale, 0.0F)) ) {
            ImGui::TextWrapped(
                "%s", TR("ui.collaboration.offline_edit.message").data());
            ImGui::Spacing();
            if ( FeedbackButton(TR("ui.common.confirm").data(),
                                ImVec2(120.0F * dpiScale, 0.0F)) ) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    {
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(projectPopupId.c_str(),
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(520.0F * dpiScale, 0.0F)) ) {
            ImGui::TextWrapped(
                "%s",
                TR("ui.collaboration.project_open_blocked.message").data());
            ImGui::Spacing();
            if ( FeedbackButton(TR("ui.common.confirm").data(),
                                ImVec2(120.0F * dpiScale, 0.0F)) ) {
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
