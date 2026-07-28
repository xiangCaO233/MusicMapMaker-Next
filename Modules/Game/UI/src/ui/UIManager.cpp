#include "ui/UIManager.h"
#include "audio/AudioManager.h"
#include "canvas/Basic2DCanvas.h"
#include "canvas/TimelineCanvas.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/input/translators/ImGuiTranslator.h"
#include "event/input/translators/UniversalCodepoint.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/iwindow/UIWindowKeyEvent.h"
#include "event/ui/iwindow/UIWindowMouseEvent.h"
#include "event/ui/menu/ProjectLoadedEvent.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "runtime/AppThreadPool.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/IRenderableView.h"
#include "ui/ITextureLoader.h"
#include "ui/imgui/ClipboardBridge.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/audio/AudioSpectrumView.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <ice/thread/ThreadPool.hpp>
#include <latch>
#include <string_view>
#include <utility>
#include <vector>

namespace MMM::UI
{

namespace
{
/// @brief 主音轨波形窗口的稳定 UIManager 视图名。
constexpr const char* AUDIO_WAVEFORM_VIEW_NAME = "AudioWaveform";

/// @brief 主音轨频谱窗口的稳定 UIManager 视图名。
constexpr const char* AUDIO_SPECTRUM_VIEW_NAME = "AudioSpectrum";

/// @brief BPM 测量工具窗口的稳定 UIManager 视图名。
constexpr const char* BPM_MEASUREMENT_TOOL_VIEW_NAME = "BpmMeasurementTool";

/// @brief 独立设置窗口的稳定 UIManager 视图名。
constexpr const char* SETTINGS_VIEW_NAME = "SettingsWindow";

/// @brief 主窗口标题栏宿主 ImGui 窗口名。
constexpr std::string_view TOP_MENU_BAR_HOST_NAME = "TopMenuBarHost";

/// @brief 判断主窗口当前是否允许播放 UI 交互音效。
/// @param window 主原生窗口观察指针。
/// @return 未最小化或没有绑定窗口时返回 true。
/// @warning UI 热路径：每帧查询一次 GLFW 窗口标志，只读取平台窗口状态。
bool isInteractionFeedbackAllowed(Graphic::NativeWindow* window)
{
    if ( !window || !window->getWindowHandle() ) {
        return true;
    }

    return glfwGetWindowAttrib(window->getWindowHandle(), GLFW_ICONIFIED) !=
           GLFW_TRUE;
}

/// @brief 判断两个拖拽矩形是否相交。
/// @param lhs 第一个矩形。
/// @param rhs 第二个矩形。
/// @return 两个矩形存在正面积交集时返回 true。
bool dragAreasIntersect(const Event::DragArea& lhs, const Event::DragArea& rhs)
{
    if ( lhs.w <= 0.0f || lhs.h <= 0.0f || rhs.w <= 0.0f || rhs.h <= 0.0f ) {
        return false;
    }

    return lhs.x < rhs.x + rhs.w && lhs.x + lhs.w > rhs.x &&
           lhs.y < rhs.y + rhs.h && lhs.y + lhs.h > rhs.y;
}

/// @brief 判断 ImGui 窗口是否应阻挡主窗口标题栏原生拖拽。
/// @param window 候选 ImGui 窗口。
/// @param viewport 当前主视口。
/// @return 该窗口可阻挡标题栏原生拖拽时返回 true。
bool shouldBlockNativeDragForWindow(const ImGuiWindow&   window,
                                    const ImGuiViewport& viewport)
{
    if ( !window.WasActive || window.Hidden || window.Collapsed ) {
        return false;
    }
    if ( window.Viewport != &viewport ) {
        return false;
    }
    if ( (window.Flags & ImGuiWindowFlags_ChildWindow) != 0 ||
         (window.Flags & ImGuiWindowFlags_NoMouseInputs) != 0 ) {
        return false;
    }

    const std::string_view name = window.Name ? window.Name : "";
    return name != TOP_MENU_BAR_HOST_NAME;
}

/// @brief 收集遮挡主窗口标题栏原生拖拽区的 ImGui 窗口矩形。
/// @param dragAreas 当前标题栏基础拖拽区域。
/// @return 与基础拖拽区相交且应排除的窗口矩形。
/// @warning UI 热路径：每帧最多遍历当前 ImGui 根窗口列表；只做几何判断。
std::vector<Event::DragArea> collectNativeDragBlockedAreas(
    const std::vector<Event::DragArea>& dragAreas)
{
    std::vector<Event::DragArea> blockedAreas;
    if ( dragAreas.empty() || !ImGui::GetCurrentContext() ) {
        return blockedAreas;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if ( !viewport ) {
        return blockedAreas;
    }

    ImGuiContext& context = *ImGui::GetCurrentContext();
    blockedAreas.reserve(static_cast<size_t>(context.WindowsFocusOrder.Size));
    for ( ImGuiWindow* window : context.WindowsFocusOrder ) {
        if ( !window || !shouldBlockNativeDragForWindow(*window, *viewport) ) {
            continue;
        }

        const ImRect    rect    = window->Rect();
        Event::DragArea blocker = {
            rect.Min.x - viewport->Pos.x,
            rect.Min.y - viewport->Pos.y,
            rect.GetWidth(),
            rect.GetHeight(),
        };
        for ( const auto& dragArea : dragAreas ) {
            if ( dragAreasIntersect(blocker, dragArea) ) {
                blockedAreas.push_back(blocker);
                break;
            }
        }
    }
    return blockedAreas;
}

/// @brief 判断视图名是否是项目工作区动态视图。
/// @param name UIManager 中注册的视图名。
/// @return 需要随项目切换清理的动态视图返回 true。
bool isProjectWorkspaceDynamicView(const std::string& name)
{
    return name.rfind("TrackController_", 0) == 0 ||
           name == AUDIO_WAVEFORM_VIEW_NAME ||
           name == AUDIO_SPECTRUM_VIEW_NAME ||
           name == BPM_MEASUREMENT_TOOL_VIEW_NAME;
}

/// @brief 查询项目或皮肤中是否仍存在工作区保存的音轨。
/// @param project 当前项目。
/// @param trackId 音轨 ID。
/// @param type 传入工作区保存类型，找到项目资源时会被实际类型覆盖。
/// @param trackName 传入工作区保存名称，找到项目资源时会被实际名称覆盖。
/// @return 音轨存在时返回 true。
bool resolveWorkspaceAudioTrack(
    const std::vector<AudioResource>& audioResources,
    const std::string& trackId, AudioTrackControllerUI::TrackType& type,
    std::string& trackName)
{
    for ( const auto& resource : audioResources ) {
        if ( resource.m_id == trackId ) {
            type      = resource.m_type == AudioTrackType::Main
                            ? AudioTrackControllerUI::TrackType::Main
                            : AudioTrackControllerUI::TrackType::Effect;
            trackName = resource.m_id;
            return true;
        }
    }

    if ( type == AudioTrackControllerUI::TrackType::Effect ) {
        auto& skinData = Config::SkinManager::instance().getData();
        if ( skinData.audioPaths.contains(trackId) ) {
            if ( trackName.empty() ) {
                trackName = trackId;
            }
            return true;
        }
    }

    return false;
}

/// @brief 在打开音效控制器前按需加载对应音效。
/// @param trackId 需要试听或控制的音效资源 ID。
/// @return 音效已经加载或成功加载时返回 true。
/// @warning 低频显式交互路径：仅在用户打开或恢复音效控制器时调用，可能
/// 访问文件系统并等待单个音效解码，禁止放入每帧 UI 更新。
bool ensureEffectAudioTrackLoaded(const std::string& trackId)
{
    auto& audio = Audio::AudioManager::instance();
    if ( audio.isSoundEffectLoaded(trackId) ) {
        return true;
    }

    auto* project = Logic::ProjectController::instance().currentProject();
    if ( project ) {
        for ( const auto& resource : project->m_audioResources ) {
            if ( resource.m_id != trackId ||
                 resource.m_type != AudioTrackType::Effect ) {
                continue;
            }

            const auto absolutePath =
                project->m_projectRoot / Config::utf8ToPath(resource.m_path);
            audio.registerSoundEffect(
                trackId, Config::pathToUtf8(absolutePath), resource.m_config);
            return audio.ensureSoundEffectLoaded(trackId);
        }
    }

    const auto& skinData = Config::SkinManager::instance().getData();
    if ( auto path = skinData.audioPaths.find(trackId);
         path != skinData.audioPaths.end() ) {
        audio.registerSoundEffect(trackId,
                                  Config::pathToUtf8(path->second),
                                  audio.getSFXPoolVolume(trackId));
    }
    return audio.ensureSoundEffectLoaded(trackId);
}

/// @brief 判断文本是否拥有指定前缀。
/// @param text 被检查文本。
/// @param prefix 需要匹配的前缀。
/// @return 文本以该前缀开头时返回 true。
bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
}

/// @brief 过滤项目工作区 ImGui ini 中跨项目不稳定的多视口平台状态。
/// @param iniData 原始 ImGui ini 数据。
/// @return 移除平台 viewport 段和字段后的 ImGui ini 数据。
std::string sanitizeProjectWorkspaceIni(std::string_view iniData)
{
    std::string sanitized;
    sanitized.reserve(iniData.size());

    bool   skipViewportSection = false;
    size_t lineStart           = 0;
    while ( lineStart < iniData.size() ) {
        const size_t nextLine = iniData.find('\n', lineStart);
        const size_t lineEnd =
            nextLine == std::string_view::npos ? iniData.size() : nextLine + 1;
        std::string_view line = iniData.substr(lineStart, lineEnd - lineStart);

        std::string_view lineWithoutEnd = line;
        if ( !lineWithoutEnd.empty() && lineWithoutEnd.back() == '\n' ) {
            lineWithoutEnd.remove_suffix(1);
        }
        if ( !lineWithoutEnd.empty() && lineWithoutEnd.back() == '\r' ) {
            lineWithoutEnd.remove_suffix(1);
        }

        if ( startsWith(lineWithoutEnd, "[Viewport][") ) {
            skipViewportSection = true;
        } else if ( startsWith(lineWithoutEnd, "[") ) {
            skipViewportSection = false;
        }

        if ( !skipViewportSection &&
             !startsWith(lineWithoutEnd, "ViewportId=") &&
             !startsWith(lineWithoutEnd, "ViewportPos=") &&
             !startsWith(lineWithoutEnd, "ViewportSize=") &&
             !startsWith(lineWithoutEnd, "ViewportOwned=") ) {
            sanitized.append(line.data(), line.size());
        }

        lineStart = lineEnd;
    }

    return sanitized;
}

/// @brief 捕获当前帧可安全传给后台 UI 准备任务的只读快照。
/// @return 当前帧 UI 快照。
/// @warning UI 热路径：每帧调用，只复制轻量配置和观察指针。
UiFrameSnapshot captureUiFrameSnapshot()
{
    auto&       appConfig  = Config::AppConfig::instance();
    const auto& settings   = appConfig.getEditorSettings();
    const auto& aesthetics = settings.aesthetics;
    auto&       skinCfg    = Config::SkinManager::instance();
    const auto& style      = ImGui::GetStyle();

    UiFrameSnapshot snapshot;
    snapshot.dpiScale     = std::max(1.0f, appConfig.getWindowContentScale());
    snapshot.framePadding = style.FramePadding;
    snapshot.frameHeight  = ImGui::GetFrameHeight();
    snapshot.frameHeightWithSpacing = ImGui::GetFrameHeightWithSpacing();
    snapshot.contentFont            = skinCfg.getFont("content");
    snapshot.menuFont               = skinCfg.getFont("menu");
    snapshot.fileManagerFont        = skinCfg.getFont("filemanager");
    snapshot.fallbackFont           = ImGui::GetFont();
    snapshot.fontSize               = ImGui::GetFontSize();
    snapshot.translationVersion     = skinCfg.getTranslator().getVersion();
    snapshot.language               = settings.language;
    snapshot.preferredAsciiFont     = settings.preferredAsciiFont;
    snapshot.preferredCjkFont       = settings.preferredCjkFont;
    snapshot.fontSizeMultiplier     = settings.fontSizeMultiplier;
    snapshot.uiScaleMultiplier      = settings.uiScaleMultiplier;
    snapshot.windowPadding          = aesthetics.windowPadding;
    snapshot.itemSpacing            = aesthetics.itemSpacing;
    snapshot.sidebarWidthConfig     = skinCfg.getLayoutConfig("side_bar.width");
    return snapshot;
}
}  // namespace

UIManager::UIManager()
{
    CLayWrapperCore::instance().setupClayTextMeasurement();

    auto& eventBus = Event::EventBus::instance();
    m_projectOpenStartedSubId =
        eventBus.subscribe<Event::ProjectOpenStartedEvent>(
            [this](const Event::ProjectOpenStartedEvent& event) {
                m_projectTransitionSignal.store(true,
                                                std::memory_order_release);
                ProjectUiLifecycleUpdate update;
                update.kind        = ProjectUiLifecycleKind::OpenStarted;
                update.projectRoot = Config::utf8ToPath(event.m_projectPath);
                m_pendingProjectLifecycleUpdates.enqueue(std::move(update));
            });
    m_projectLoadedSubId = eventBus.subscribe<Event::ProjectLoadedEvent>(
        [this](const Event::ProjectLoadedEvent& event) {
            ProjectUiLifecycleUpdate update;
            update.kind        = ProjectUiLifecycleKind::Opened;
            update.projectRoot = Config::utf8ToPath(event.m_projectPath);

            const auto* project =
                Logic::ProjectController::instance().currentProject();
            if ( project && project->m_projectRoot.lexically_normal() ==
                                update.projectRoot.lexically_normal() ) {
                update.workspace          = project->m_settings.m_workspace;
                update.audioResources     = project->m_audioResources;
                update.hasProjectSnapshot = true;
            }
            m_pendingProjectLifecycleUpdates.enqueue(std::move(update));
        });
    m_projectClosedSubId = eventBus.subscribe<Event::ProjectClosedEvent>(
        [this](const Event::ProjectClosedEvent& event) {
            ProjectUiLifecycleUpdate update;
            update.kind        = ProjectUiLifecycleKind::Closed;
            update.projectRoot = event.m_projectPath;
            m_pendingProjectLifecycleUpdates.enqueue(std::move(update));
        });
    m_projectOpenFailedSubId =
        eventBus.subscribe<Event::ProjectOpenFailedEvent>(
            [this](const Event::ProjectOpenFailedEvent& event) {
                ProjectUiLifecycleUpdate update;
                update.kind        = ProjectUiLifecycleKind::OpenFailed;
                update.projectRoot = Config::utf8ToPath(event.m_projectPath);
                m_pendingProjectLifecycleUpdates.enqueue(std::move(update));
            });
    m_temporaryProjectSaveResultSubId =
        eventBus.subscribe<Event::TemporaryProjectSaveResultEvent>(
            [this](const Event::TemporaryProjectSaveResultEvent& event) {
                if ( !event.m_success || event.m_savedProjectPath.empty() ) {
                    return;
                }
                ProjectUiLifecycleUpdate update;
                update.kind = ProjectUiLifecycleKind::RootChanged;
                update.projectRoot =
                    Config::utf8ToPath(event.m_savedProjectPath);
                m_pendingProjectLifecycleUpdates.enqueue(std::move(update));
            });
}

UIManager::~UIManager()
{
    auto& eventBus = Event::EventBus::instance();
    if ( m_projectOpenStartedSubId != 0 ) {
        eventBus.unsubscribe<Event::ProjectOpenStartedEvent>(
            m_projectOpenStartedSubId);
    }
    if ( m_projectLoadedSubId != 0 ) {
        eventBus.unsubscribe<Event::ProjectLoadedEvent>(m_projectLoadedSubId);
    }
    if ( m_projectClosedSubId != 0 ) {
        eventBus.unsubscribe<Event::ProjectClosedEvent>(m_projectClosedSubId);
    }
    if ( m_projectOpenFailedSubId != 0 ) {
        eventBus.unsubscribe<Event::ProjectOpenFailedEvent>(
            m_projectOpenFailedSubId);
    }
    if ( m_temporaryProjectSaveResultSubId != 0 ) {
        eventBus.unsubscribe<Event::TemporaryProjectSaveResultEvent>(
            m_temporaryProjectSaveResultSubId);
    }
}

void UIManager::setNativeWindow(Graphic::NativeWindow* window)
{
    m_nativeWindow = window;
}

Graphic::NativeWindow* UIManager::getNativeWindow() const
{
    return m_nativeWindow;
}

Graphic::IWindowFrameAdapter* UIManager::getWindowFrameAdapter() const
{
    return m_nativeWindow ? m_nativeWindow->getWindowFrameAdapter() : nullptr;
}

bool UIManager::isProjectTransitionInProgress() const
{
    return m_projectTransitionSignal.load(std::memory_order_acquire);
}

bool UIManager::hasActiveProjectUiState() const
{
    return m_projectLifecycleState.hasActiveProject;
}

/// @brief 判断时间线窗口是否正在拖动 Timing 框选区域。
/// @return 时间线正在框选时返回 true。
/// @warning UI 热路径：空格快捷键按下时调用；只读取已注册视图的本地状态。
bool UIManager::isTimelineTimingMarqueeSelecting()
{
    const auto* timeline = getView<Canvas::TimelineCanvas>("TimelineWindow");
    return timeline && timeline->isTimingMarqueeSelecting();
}

/// @brief 判断时间线窗口是否正在通过抓取工具拖动 Timing。
/// @return 时间线正在拖动 Timing 时返回 true。
/// @warning UI 热路径：空格快捷键按下时调用；只读取已注册视图的本地状态。
bool UIManager::isTimelineTimingDragging()
{
    const auto* timeline = getView<Canvas::TimelineCanvas>("TimelineWindow");
    return timeline && timeline->isTimingDragging();
}

const std::filesystem::path& UIManager::getActiveProjectRoot() const
{
    return m_activeProjectRoot;
}

void UIManager::setNativeWindowDragAreas(std::vector<Event::DragArea> areas)
{
    m_nativeWindowDragAreas = std::move(areas);
}

void UIManager::syncNativeWindowDragAreas()
{
    if ( !m_nativeWindow ) {
        return;
    }

    Event::UpdateDragAreaEvent event;
    event.uiManager    = this;
    event.sourceUiName = std::string(TOP_MENU_BAR_HOST_NAME);
    event.areas        = m_nativeWindowDragAreas;
    event.blockedAreas = collectNativeDragBlockedAreas(m_nativeWindowDragAreas);
    Event::EventBus::instance().publish(event);
}

void UIManager::captureProjectWorkspaceState()
{
    if ( isProjectTransitionInProgress() ||
         !m_projectLifecycleState.hasActiveProject ) {
        return;
    }

    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.lexically_normal() !=
                         m_activeProjectRoot.lexically_normal() ) {
        return;
    }

    auto& workspace = project->m_settings.m_workspace;
    captureProjectWorkspaceViews(workspace);

    size_t      iniSize = 0;
    const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
    if ( iniData && iniSize > 0 ) {
        workspace.m_imguiIniData.assign(iniData, iniSize);
    }

    if ( m_nativeWindow ) {
        auto& windowState = workspace.m_mainWindow;
        m_nativeWindow->getWindowPlacement(windowState.m_x,
                                           windowState.m_y,
                                           windowState.m_width,
                                           windowState.m_height,
                                           windowState.m_maximized);
        windowState.m_valid = true;
    }
}

/// @brief 打开独立设置窗口，切换到指定标签页并请求中心停靠和聚焦。
/// @param tab 需要激活的设置标签页。
void UIManager::openSettingsWindow(MMM::Event::SettingsTab tab)
{
    auto*      settingsView = getView<SettingsView>(SETTINGS_VIEW_NAME);
    const bool wasOpen      = settingsView && settingsView->isOpen();
    if ( !settingsView ) {
        auto view =
            std::make_unique<SettingsView>(TR("title.settings_manager").data());
        settingsView = view.get();
        registerView(SETTINGS_VIEW_NAME, std::move(view));
    }

    if ( settingsView ) {
        settingsView->open(tab);
        settingsView->requestDockToCenter();
        settingsView->requestFocus();
        if ( !wasOpen ) {
            ::MMM::UI::PlayPopupOpenFeedback();
        }
    }
}

/// @brief 请求下一次资源准备阶段重载皮肤相关图形资源。
/// @warning 低频资源重载路径：皮肤热切换后调用，只置脏位；实际 Vulkan
/// 资源释放和重建在 onPrepareResources 中执行。
void UIManager::requestSkinResourceReload()
{
    m_skinResourceReloadRequested = true;
}

/// @brief 为新打开的音轨控制器选择默认 Dock 节点。
/// @return 目标 Dock 节点 ID；无法解析时返回 0。
ImGuiID UIManager::resolveAudioControllerDockId()
{
    auto& engine  = Logic::EditorEngine::instance();
    auto  entries = engine.getSessionEntries();

    auto resolveEntryDockId = [this](const Logic::SessionEntry& entry) {
        if ( entry.isLogoPlaceholder ) {
            return static_cast<ImGuiID>(0);
        }

        auto* canvas = getView<Canvas::Basic2DCanvas>(entry.cameraId);
        if ( !canvas ) {
            return static_cast<ImGuiID>(0);
        }
        return canvas->getDockId();
    };

    const int32_t activeIndex = engine.getActiveSessionIndex();
    if ( activeIndex >= 0 &&
         activeIndex < static_cast<int32_t>(entries.size()) ) {
        ImGuiID activeDockId =
            resolveEntryDockId(entries[static_cast<size_t>(activeIndex)]);
        if ( activeDockId != 0 ) {
            return activeDockId;
        }
    }

    for ( const auto& entry : entries ) {
        ImGuiID dockId = resolveEntryDockId(entry);
        if ( dockId != 0 ) {
            return dockId;
        }
    }

    return MainDockSpaceUI::getCenterDockId();
}

/// @brief 打开音轨控制器并默认停靠到谱面画布标签组。
/// @param trackId 音轨标识符。
/// @param trackName 音轨显示名称。
/// @param type 音轨类型。
void UIManager::openAudioTrackController(const std::string& trackId,
                                         const std::string& trackName,
                                         AudioTrackControllerUI::TrackType type)
{
    if ( type == AudioTrackControllerUI::TrackType::Effect ) {
        (void)ensureEffectAudioTrackLoaded(trackId);
    }

    std::string viewName   = AudioTrackControllerUI::makeViewName(trackId);
    auto*       controller = getView<AudioTrackControllerUI>(viewName);
    if ( !controller ) {
        auto view = std::make_unique<AudioTrackControllerUI>(
            trackId, trackName.empty() ? trackId : trackName, type);
        controller = view.get();
        registerView(viewName, std::move(view));
    }

    if ( controller ) {
        controller->requestDockTo(resolveAudioControllerDockId());
        controller->requestFocus();
    }
}

/// @brief 重新加载当前已打开控制器引用的项目音效。
/// @warning 低频皮肤重载路径：每个已打开音效控制器最多触发一次单文件
/// 解码，禁止放入每帧 UI 更新。
void UIManager::reloadOpenEffectAudioTracks()
{
    for ( const auto& [name, view] : m_uiviews ) {
        if ( name.rfind("TrackController_", 0) != 0 || !view ) {
            continue;
        }

        auto* controller =
            static_cast<AudioTrackControllerUI*>(view->getActualInstance());
        if ( !controller || controller->getTrackType() !=
                                AudioTrackControllerUI::TrackType::Effect ) {
            continue;
        }
        (void)ensureEffectAudioTrackLoaded(controller->getTrackId());
    }
}

void UIManager::applyNoProjectDefaultWorkspace()
{
    if ( auto* sideBar = getView<SideBarUI>("SideBarUI") ) {
        sideBar->setActiveTab(SideBarTab::FileExplorer);
    }
    if ( auto* sideBarManager = getView<FloatingManagerUI>("SideBarManager") ) {
        sideBarManager->restoreSubViewState(
            TabToSubViewId(SideBarTab::FileExplorer), true);
    }
}

void UIManager::consumePendingProjectLifecycleUpdates()
{
    ProjectUiLifecycleUpdate update;
    while ( m_pendingProjectLifecycleUpdates.try_dequeue(update) ) {
        m_projectLifecycleState =
            reduceProjectUiLifecycleState(m_projectLifecycleState, update.kind);

        switch ( update.kind ) {
        case ProjectUiLifecycleKind::OpenStarted: break;
        case ProjectUiLifecycleKind::Opened:
            m_activeProjectRoot                = std::move(update.projectRoot);
            m_noProjectWorkspaceDefaultApplied = false;
            m_pendingProjectWorkspace          = ProjectWorkspaceState{};
            m_pendingProjectAudioResources.clear();
            if ( update.hasProjectSnapshot ) {
                m_pendingProjectWorkspace = std::move(update.workspace);
                m_pendingProjectAudioResources =
                    std::move(update.audioResources);
            }
            m_projectWorkspaceRestorePending = true;
            m_projectTransitionSignal.store(false, std::memory_order_release);
            break;
        case ProjectUiLifecycleKind::Closed:
        case ProjectUiLifecycleKind::OpenFailed:
            if ( !m_projectLifecycleState.hasActiveProject ) {
                m_activeProjectRoot.clear();
                m_projectWorkspaceRestorePending = false;
                m_pendingProjectAudioResources.clear();
            }
            if ( update.kind == ProjectUiLifecycleKind::OpenFailed ) {
                m_projectTransitionSignal.store(false,
                                                std::memory_order_release);
            }
            break;
        case ProjectUiLifecycleKind::RootChanged:
            if ( m_projectLifecycleState.hasActiveProject &&
                 !update.projectRoot.empty() ) {
                m_activeProjectRoot = std::move(update.projectRoot);
                m_workspaceProjectPath =
                    Config::pathToUtf8(m_activeProjectRoot);
            }
            break;
        }
    }
}

void UIManager::syncProjectWorkspaceState()
{
    if ( isProjectTransitionInProgress() ) {
        return;
    }

    if ( shouldApplyNoProjectWorkspace(m_projectLifecycleState) ) {
        if ( !m_workspaceProjectPath.empty() ) {
            clearProjectWorkspaceViews();
        }
        m_workspaceProjectPath.clear();
        if ( !m_noProjectWorkspaceDefaultApplied ) {
            applyNoProjectDefaultWorkspace();
            m_noProjectWorkspaceDefaultApplied = true;
        }
        return;
    }

    m_noProjectWorkspaceDefaultApplied = false;
    if ( m_projectWorkspaceRestorePending ) {
        m_workspaceProjectPath = Config::pathToUtf8(m_activeProjectRoot);
        clearProjectWorkspaceViews();

        const auto& workspace = m_pendingProjectWorkspace;
        if ( !workspace.m_imguiIniData.empty() ) {
            std::string sanitizedIni =
                sanitizeProjectWorkspaceIni(workspace.m_imguiIniData);
            if ( !sanitizedIni.empty() ) {
                ImGui::LoadIniSettingsFromMemory(sanitizedIni.data(),
                                                 sanitizedIni.size());
                MainDockSpaceUI::markProjectWorkspaceLayoutLoaded();
            }
        }

        if ( m_nativeWindow && workspace.m_mainWindow.m_valid ) {
            const auto& windowState = workspace.m_mainWindow;
            m_nativeWindow->applyWindowPlacement(windowState.m_x,
                                                 windowState.m_y,
                                                 windowState.m_width,
                                                 windowState.m_height,
                                                 windowState.m_maximized);
        }

        restoreProjectWorkspaceViews(workspace, m_pendingProjectAudioResources);

        m_nextWorkspaceCaptureTime       = ImGui::GetTime() + 0.5;
        m_projectWorkspaceRestorePending = false;
        m_pendingProjectAudioResources.clear();
    }

    double now = ImGui::GetTime();
    if ( now >= m_nextWorkspaceCaptureTime ) {
        captureProjectWorkspaceState();
        m_nextWorkspaceCaptureTime = now + 2.0;
    }
}

void UIManager::captureProjectWorkspaceViews(ProjectWorkspaceState& workspace)
{
    workspace.m_audioControllers.clear();
    workspace.m_audioWaveformOpen      = false;
    workspace.m_audioSpectrumOpen      = false;
    workspace.m_bpmMeasurementToolOpen = false;
    workspace.m_bpmMeasurementAudioTrackId.clear();
    workspace.m_timingPointsTableOpen  = false;
    workspace.m_overlapCheckOpen       = false;
    workspace.m_metadataEditorOpen     = false;
    workspace.m_noteMetadataEditorOpen = false;

    for ( const auto& name : m_uiSequence ) {
        auto viewIt = m_uiviews.find(name);
        if ( viewIt == m_uiviews.end() || !viewIt->second->isOpen() ) {
            continue;
        }

        if ( name.rfind("TrackController_", 0) == 0 ) {
            auto* controller = getView<AudioTrackControllerUI>(name);
            if ( !controller ) {
                continue;
            }

            ProjectWorkspaceAudioControllerState controllerState;
            controllerState.m_trackId   = controller->getTrackId();
            controllerState.m_trackName = controller->getTrackName();
            controllerState.m_trackType =
                AudioTrackControllerUI::trackTypeToWorkspaceName(
                    controller->getTrackType());
            workspace.m_audioControllers.push_back(controllerState);
            continue;
        }

        if ( name == AUDIO_WAVEFORM_VIEW_NAME ) {
            workspace.m_audioWaveformOpen = true;
        } else if ( name == AUDIO_SPECTRUM_VIEW_NAME ) {
            workspace.m_audioSpectrumOpen = true;
        } else if ( name == BPM_MEASUREMENT_TOOL_VIEW_NAME ) {
            auto* tool = getView<BpmMeasurementToolView>(name);
            if ( tool ) {
                workspace.m_bpmMeasurementToolOpen = true;
                workspace.m_bpmMeasurementAudioTrackId =
                    tool->getSelectedAudioTrackId();
            }
        }
    }

    if ( auto* timeline = getView<Canvas::TimelineCanvas>("TimelineWindow") ) {
        workspace.m_timingPointsTableOpen = timeline->isTimingPointsTableOpen();
    }

    if ( auto* sideBarManager = getView<FloatingManagerUI>("SideBarManager") ) {
        SideBarTab activeTab = SideBarTab::None;
        if ( sideBarManager->isVisible() ) {
            activeTab = SubViewIdToTab(sideBarManager->getCurrentSubViewId());
        }
        workspace.m_sidebarActiveTab =
            SideBarUI::workspaceNameFromTab(activeTab);
    }
}

void UIManager::restoreProjectWorkspaceViews(
    const ProjectWorkspaceState&      workspace,
    const std::vector<AudioResource>& audioResources)
{
    SideBarTab sideBarTab =
        SideBarUI::workspaceNameToTab(workspace.m_sidebarActiveTab);
    if ( auto* sideBar = getView<SideBarUI>("SideBarUI") ) {
        sideBar->setActiveTab(sideBarTab);
    }
    if ( auto* sideBarManager = getView<FloatingManagerUI>("SideBarManager") ) {
        sideBarManager->restoreSubViewState(sideBarTab == SideBarTab::None
                                                ? std::string{}
                                                : TabToSubViewId(sideBarTab),
                                            sideBarTab != SideBarTab::None);
    }

    for ( const auto& controllerState : workspace.m_audioControllers ) {
        if ( controllerState.m_trackId.empty() ) {
            continue;
        }

        auto trackType = AudioTrackControllerUI::workspaceNameToTrackType(
            controllerState.m_trackType);
        std::string trackName = controllerState.m_trackName.empty()
                                    ? controllerState.m_trackId
                                    : controllerState.m_trackName;
        if ( !resolveWorkspaceAudioTrack(audioResources,
                                         controllerState.m_trackId,
                                         trackType,
                                         trackName) ) {
            continue;
        }
        if ( trackType == AudioTrackControllerUI::TrackType::Effect ) {
            (void)ensureEffectAudioTrackLoaded(controllerState.m_trackId);
        }

        std::string viewName =
            AudioTrackControllerUI::makeViewName(controllerState.m_trackId);
        if ( getView<AudioTrackControllerUI>(viewName) ) {
            continue;
        }

        registerView(viewName,
                     std::make_unique<AudioTrackControllerUI>(
                         controllerState.m_trackId, trackName, trackType));
    }

    if ( workspace.m_audioWaveformOpen &&
         !getView<AudioWaveformView>(AUDIO_WAVEFORM_VIEW_NAME) ) {
        registerView(AUDIO_WAVEFORM_VIEW_NAME,
                     std::make_unique<AudioWaveformView>(
                         TR("ui.audio_manager.waveform_title").data()));
    }

    if ( workspace.m_audioSpectrumOpen &&
         !getView<AudioSpectrumView>(AUDIO_SPECTRUM_VIEW_NAME) ) {
        registerView(AUDIO_SPECTRUM_VIEW_NAME,
                     std::make_unique<AudioSpectrumView>(
                         TR("ui.audio_manager.spectrum_title").data()));
    }

    if ( workspace.m_bpmMeasurementToolOpen ) {
        auto* bpmTool =
            getView<BpmMeasurementToolView>(BPM_MEASUREMENT_TOOL_VIEW_NAME);
        if ( !bpmTool ) {
            auto toolView = std::make_unique<BpmMeasurementToolView>(
                TR("ui.tools.bpm_measure").data());
            bpmTool = toolView.get();
            registerView(BPM_MEASUREMENT_TOOL_VIEW_NAME, std::move(toolView));
        }
        if ( bpmTool ) {
            bpmTool->openWithAudioTrack(workspace.m_bpmMeasurementAudioTrackId);
        }
    }

    if ( auto* timeline = getView<Canvas::TimelineCanvas>("TimelineWindow") ) {
        timeline->setTimingPointsTableOpen(workspace.m_timingPointsTableOpen);
    }
}

void UIManager::clearProjectWorkspaceViews()
{
    std::vector<std::string> dynamicViews;
    for ( const auto& name : m_uiSequence ) {
        if ( isProjectWorkspaceDynamicView(name) ) {
            dynamicViews.push_back(name);
        }
    }

    for ( const auto& name : dynamicViews ) {
        unregisterView(name);
    }
}

/// @brief 注册视图，转交所有权
void UIManager::registerView(const std::string&       name,
                             std::unique_ptr<IUIView> view)
{
    m_uiSequence.push_back(name);

    if ( view->renderable() ) {
        m_renderableUiSequence.push_back(name);
        XINFO("Registered Renderable [{}] UIView", name);
    } else {
        XINFO("Registered General [{}] UIView", name);
    }

    auto textureLoader = view->asTextureLoader();
    if ( textureLoader ) {
        m_textureLoaderSequence.push_back(name);
    }

    m_uiviews[name] = std::move(view);
}

/// @brief 注销并销毁视图
void UIManager::unregisterView(const std::string& name)
{
    /// @brief 即将被注销的视图迭代器。
    auto viewIt = m_uiviews.find(name);
    if ( viewIt != m_uiviews.end() ) {
        waitForGpuBeforeDestroyView(*viewIt->second);
        m_uiviews.erase(viewIt);
    }
    std::erase(m_uiSequence, name);
    std::erase(m_renderableUiSequence, name);
    std::erase(m_textureLoaderSequence, name);
    XINFO("Unregistered [{}] UIView", name);
}

/// @brief 清理所有ui
void UIManager::clearAllViews()
{
    /// @brief 当前仍注册在 UIManager 内的视图条目。
    for ( auto& entry : m_uiviews ) {
        waitForGpuBeforeDestroyView(*entry.second);
    }
    m_uiviews.clear();
}

/// @brief 准备资源
/// @warning 热路径：每帧渲染准备阶段执行；重建和纹理重载只能由低频脏位触发。
/// 皮肤热切换分支会调用 waitIdle，只能由设置页切换皮肤触发。
void UIManager::onPrepareResources(vk::PhysicalDevice&   physicalDevice,
                                   vk::Device&           logicalDevice,
                                   Graphic::VKSwapchain& swapchain,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
    bool forceSkinResourceReload =
        std::exchange(m_skinResourceReloadRequested, false);
    if ( forceSkinResourceReload && isProjectTransitionInProgress() ) {
        /// 项目动态视图在切换失败后可能继续存活，强制皮肤重载必须延迟到
        /// 切换结束后统一执行，不能在占位期间丢弃这次请求。
        m_skinResourceReloadRequested = true;
        forceSkinResourceReload       = false;
    }

    if ( forceSkinResourceReload ) {
        (void)logicalDevice.waitIdle();
        if ( auto context = Graphic::VKContext::get() ) {
            context->get().getRenderer().reloadSkinTextures();
        }

        for ( const auto& name : m_renderableUiSequence ) {
            if ( isProjectTransitionInProgress() &&
                 isProjectWorkspaceDynamicView(name) ) {
                continue;
            }
            auto renderableView = m_uiviews[name]->asRenderableView();
            if ( renderableView ) {
                renderableView->requestSkinResourceReload();
                renderableView->reCreateFrameBuffer(
                    physicalDevice, logicalDevice, swapchain, cmdPool, queue);
            }
        }

        for ( const auto& name : m_textureLoaderSequence ) {
            if ( isProjectTransitionInProgress() &&
                 isProjectWorkspaceDynamicView(name) ) {
                continue;
            }
            auto textureLoader = m_uiviews[name]->asTextureLoader();
            if ( textureLoader ) {
                textureLoader->reloadTextures(
                    physicalDevice, logicalDevice, cmdPool, queue);
                (void)textureLoader->needReload();
            }
        }
        return;
    }

    // 检查并重建所有离屏帧缓冲
    for ( const auto& name : m_renderableUiSequence ) {
        if ( isProjectTransitionInProgress() &&
             isProjectWorkspaceDynamicView(name) ) {
            continue;
        }
        auto renderableView = m_uiviews[name]->asRenderableView();
        if ( renderableView && renderableView->needReCreateFrameBuffer() ) {
            renderableView->reCreateFrameBuffer(
                physicalDevice, logicalDevice, swapchain, cmdPool, queue);
        }
    }

    // 检查并重载所有纹理
    for ( const auto& name : m_textureLoaderSequence ) {
        if ( isProjectTransitionInProgress() &&
             isProjectWorkspaceDynamicView(name) ) {
            continue;
        }
        auto textureLoader = m_uiviews[name]->asTextureLoader();
        if ( textureLoader && textureLoader->needReload() ) {
            textureLoader->reloadTextures(
                physicalDevice, logicalDevice, cmdPool, queue);
        }
    }
}

/// @brief 更新ui
/// @warning 热路径：每帧 ImGui 更新阶段执行；禁止在此加入文件系统扫描、完整 ECS
/// 遍历或完整排序。
void UIManager::onUpdateUI()
{
    Logic::EditorEngine::instance().publishRenderFps(ImGui::GetIO().Framerate);
    SetInteractionFeedbackEnabled(isInteractionFeedbackAllowed(m_nativeWindow));
    ProcessGlobalMouseFeedback();

    consumePendingProjectLifecycleUpdates();
    syncProjectWorkspaceState();

    // 清理已关闭的 IUIView
    std::vector<std::string> toRemove;
    for ( auto& [name, view] : m_uiviews ) {
        if ( !view->isOpen() ) {
            toRemove.push_back(name);
        }
    }

    for ( const auto& name : toRemove ) {
        /// @brief 当前待销毁视图的迭代器。
        auto viewIt = m_uiviews.find(name);
        if ( viewIt != m_uiviews.end() ) {
            waitForGpuBeforeDestroyView(*viewIt->second);
            m_uiviews.erase(viewIt);
        }
        std::erase(m_uiSequence, name);
        // 同时也从纹理加载器和可渲染序列中移除（如果存在）
        std::erase(m_renderableUiSequence, name);
        std::erase(m_textureLoaderSequence, name);
    }

    // 派发 ImGui 事件 (每帧仅 1 次)
    DispatchGlobalUIEvents();
    ClipboardBridge::publishPendingEditorClipboard();

    // 预先准备视图数据；字体测量留在主线程，纯数据任务才允许进入线程池。
    m_uiPrepareCandidates.clear();
    m_uiPrepareViews.clear();
    m_mainThreadUiPrepareViews.clear();
    m_parallelUiPrepareViews.clear();
    m_uiPrepareCandidates.reserve(m_uiSequence.size());
    for ( const auto& name : m_uiSequence ) {
        if ( isProjectTransitionInProgress() &&
             (name == "SideBarManager" ||
              isProjectWorkspaceDynamicView(name)) ) {
            continue;
        }
        auto it = m_uiviews.find(name);
        if ( it == m_uiviews.end() ) {
            continue;
        }

        IParallelUiPreparable* preparable =
            it->second->asParallelUiPreparable();
        if ( preparable ) {
            m_uiPrepareCandidates.push_back(preparable);
        }
    }

    if ( !m_uiPrepareCandidates.empty() ) {
        const UiFrameSnapshot snapshot = captureUiFrameSnapshot();
        m_uiPrepareViews.reserve(m_uiPrepareCandidates.size());
        m_mainThreadUiPrepareViews.reserve(m_uiPrepareCandidates.size());
        m_parallelUiPrepareViews.reserve(m_uiPrepareCandidates.size());
        for ( IParallelUiPreparable* preparable : m_uiPrepareCandidates ) {
            if ( preparable->needsParallelUiPrepare(snapshot) ) {
                m_uiPrepareViews.push_back(preparable);
                if ( preparable->requiresMainThreadUiPrepare() ) {
                    m_mainThreadUiPrepareViews.push_back(preparable);
                } else {
                    m_parallelUiPrepareViews.push_back(preparable);
                }
            }
        }

        // ImGui 1.92 的文本测量可能按需烘焙字形并写入共享 FontAtlas；
        // 声明主线程约束的视图必须先串行准备，禁止与线程池任务并发访问字体状态。
        for ( IParallelUiPreparable* preparable : m_mainThreadUiPrepareViews ) {
            preparable->prepareUiFrameData(snapshot);
        }

        auto* appThreadPool = MMM::Runtime::AppThreadPool::instance().get();
        if ( appThreadPool && m_parallelUiPrepareViews.size() > 1 ) {
            std::latch prepareLatch(
                static_cast<std::ptrdiff_t>(m_parallelUiPrepareViews.size()));
            for ( IParallelUiPreparable* preparable :
                  m_parallelUiPrepareViews ) {
                appThreadPool->enqueue_void(
                    [preparable, &snapshot, &prepareLatch]() {
                        preparable->prepareUiFrameData(snapshot);
                        prepareLatch.count_down();
                    });
            }
            prepareLatch.wait();
        } else {
            for ( IParallelUiPreparable* preparable :
                  m_parallelUiPrepareViews ) {
                preparable->prepareUiFrameData(snapshot);
            }
        }
        for ( IParallelUiPreparable* preparable : m_uiPrepareViews ) {
            preparable->swapPreparedUiFrameData();
        }
    }

    // 按注册顺序更新本帧开始前已存在的 UI。
    // update() 过程中可能注册新视图，使用索引和名称副本避免迭代器失效。
    const size_t initialViewCount = m_uiSequence.size();
    for ( size_t i = 0; i < initialViewCount && i < m_uiSequence.size(); ++i ) {
        const std::string name = m_uiSequence[i];
        auto              it   = m_uiviews.find(name);
        if ( it == m_uiviews.end() ) {
            continue;
        }

        // 内部触发 ImGui 渲染和画笔收集
        it->second->update(this);
    }

    if ( auto* sideBarManager = getView<FloatingManagerUI>("SideBarManager") ) {
        sideBarManager->restoreDockResizeMouseAfterDockSpace();
    }

    syncNativeWindowDragAreas();
}

/// @brief 录制所有离屏渲染指令
/// @warning
/// 热路径：每帧命令录制阶段执行；只允许遍历可渲染视图序列并委托录制命令。
void UIManager::onRecordOffscreen(vk::CommandBuffer& cmd, uint32_t frameIndex)
{
    const uint32_t taskCount = getOffscreenRecordTaskCount();
    for ( uint32_t taskIndex = 0; taskIndex < taskCount; ++taskIndex ) {
        onRecordOffscreenTask(cmd, frameIndex, taskIndex);
    }
}

/// @brief 获取当前帧可并行录制的离屏视图数量。
/// @return 当前可渲染视图序列的数量。
/// @warning 渲染热路径：每帧命令录制前调用，只读取稳定序列长度。
uint32_t UIManager::getOffscreenRecordTaskCount() const
{
    return static_cast<uint32_t>(m_renderableUiSequence.size());
}

/// @brief 录制指定可渲染视图的离屏命令。
/// @param cmd 当前任务独占的命令缓冲。
/// @param frameIndex 当前并发帧索引。
/// @param taskIndex 可渲染视图序列索引。
/// @warning 渲染热路径：可能在渲染线程池中执行，只能读取 UIManager
/// 的稳定视图表并录制对应视图。
void UIManager::onRecordOffscreenTask(vk::CommandBuffer& cmd,
                                      uint32_t frameIndex, uint32_t taskIndex)
{
    if ( taskIndex >= m_renderableUiSequence.size() ) {
        return;
    }

    const auto& name = m_renderableUiSequence[taskIndex];
    if ( isProjectTransitionInProgress() &&
         isProjectWorkspaceDynamicView(name) ) {
        return;
    }
    const auto& views = m_uiviews;
    auto        it    = views.find(name);
    if ( it == views.end() ) {
        return;
    }

    auto renderableView = it->second->asRenderableView();
    if ( renderableView && renderableView->shouldRecordOffscreen() ) {
        renderableView->recordCmds(cmd, frameIndex);
    }
}


/// @brief 分派所有imgui事件
void UIManager::DispatchGlobalUIEvents()
{
    ImGuiIO& io = ImGui::GetIO();

    // 1. 获取当前正在被操作（Focus/Hover）的 ImGui 窗口名称
    // 这取代了原来在 IUIView 中写死的 m_name
    std::string focusedWindowName = "";
    std::string hoveredWindowName = "";

    if ( ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow ) {
        focusedWindowName = focused->Name;
    }
    if ( ImGuiWindow* hovered = ImGui::GetCurrentContext()->HoveredWindow ) {
        hoveredWindowName = hovered->Name;
    }

    // ==========================================
    // 处理鼠标事件 (每帧仅 1 次)
    // ==========================================
    // 只有当鼠标真的在 ImGui 窗口上时，才分发带窗口名字的事件
    if ( io.WantCaptureMouse ) {
        // 提取鼠标移动
        if ( io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ) {
            Event::UIWindowMouseMoveEvent e;
            e.uiManager    = this;
            e.sourceUiName = hoveredWindowName;  // 鼠标事件归属悬停窗口
            e.pos          = { io.MousePos.x, io.MousePos.y };
            e.delta        = { io.MouseDelta.x, io.MouseDelta.y };
            Event::EventBus::instance().publish(e);
        }

        // 提取滚轮
        if ( io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ) {
            Event::UIWindowMouseScrollEvent e;
            e.uiManager    = this;
            e.sourceUiName = hoveredWindowName;
            e.pos          = { io.MousePos.x, io.MousePos.y };
            e.offset       = { io.MouseWheelH, io.MouseWheel };
            Event::EventBus::instance().publish(e);
        }

        // 提取点击
        for ( int i = 0; i < 5; ++i ) {
            bool pressed  = ImGui::IsMouseClicked(i);
            bool released = ImGui::IsMouseReleased(i);
            if ( pressed || released ) {
                Event::UIWindowMouseButtonEvent e;
                e.uiManager    = this;
                e.sourceUiName = hoveredWindowName;
                e.button       = Event::Translator::ImGui::GetMouseButton(i);
                e.action       = pressed ? Event::Input::Action::Press
                                         : Event::Input::Action::Release;
                e.mods         = Event::Translator::ImGui::GetMods();
                e.pos          = { io.MousePos.x, io.MousePos.y };
                Event::EventBus::instance().publish(e);
            }
        }
    }

    // ==========================================
    // 处理键盘事件 (每帧仅 1 次)
    // ==========================================
    const bool hasFocusedWindow = !focusedWindowName.empty();
    const bool shouldDispatchKeyboard =
        io.WantCaptureKeyboard || (hasFocusedWindow && !io.WantTextInput);
    if ( shouldDispatchKeyboard ) {
        // 全局遍历一次枚举 (约 100 次循环，对 CPU 来说仅需几纳秒，完全不拉胯)
        for ( int i = ImGuiKey_NamedKey_BEGIN; i < ImGuiKey_NamedKey_END;
              ++i ) {
            ImGuiKey key = static_cast<ImGuiKey>(i);

            bool pressed  = ImGui::IsKeyPressed(key, true);
            bool released = ImGui::IsKeyReleased(key);

            if ( pressed || released ) {
                Event::UIWindowKeyPressEvent e;
                e.uiManager    = this;
                e.sourceUiName = focusedWindowName;  // 键盘事件归属焦点窗口

                e.key      = Event::Translator::ImGui::GetKey(key);
                e.action   = pressed ? Event::Input::Action::Press
                                     : Event::Input::Action::Release;
                e.mods     = Event::Translator::ImGui::GetMods();
                e.scancode = 0;

                if ( e.action != Event::Input::Action::Release ) {
                    e.codepoint =
                        Event::Translator::ResolveCodepoint(e.key, e.mods);
                } else {
                    e.codepoint = 0;
                }

                Event::EventBus::instance().publish(e);
            }
        }
    }
}

/// @brief 在销毁可能持有 Vulkan 资源的视图前等待 GPU 完成在途命令。
/// @warning 不可中断操作：可能调用
/// vkDeviceWaitIdle；只能在视图销毁低频路径执行。
void UIManager::waitForGpuBeforeDestroyView(IUIView& view)
{
    /// @brief 目标视图是否持有可能被命令缓冲引用的 Vulkan 资源。
    bool ownsGpuResources = view.renderable() || view.asTextureLoader();
    if ( !ownsGpuResources ) {
        return;
    }

    /// @brief 当前 Vulkan 上下文查询结果。
    auto contextResult = Graphic::VKContext::get();
    if ( !contextResult ) {
        XWARN("UIManager: skip GPU idle wait before destroying [{}]: {}",
              view.m_name,
              contextResult.error());
        return;
    }

    (void)contextResult->get().getLogicalDevice().waitIdle();
}

}  // namespace MMM::UI
