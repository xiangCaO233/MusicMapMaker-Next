#include "ui/UIManager.h"
#include "canvas/Basic2DCanvas.h"
#include "canvas/TimelineCanvas.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/input/translators/ImGuiTranslator.h"
#include "event/input/translators/UniversalCodepoint.h"
#include "event/ui/iwindow/UIWindowKeyEvent.h"
#include "event/ui/iwindow/UIWindowMouseEvent.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "ui/IRenderableView.h"
#include "ui/ITextureLoader.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/audio/AudioSpectrumView.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/imgui/tools/BpmMeasurementToolView.h"
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
bool resolveWorkspaceAudioTrack(const Project&                     project,
                                const std::string&                 trackId,
                                AudioTrackControllerUI::TrackType& type,
                                std::string&                       trackName)
{
    for ( const auto& resource : project.m_audioResources ) {
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
}  // namespace

void UIManager::setNativeWindow(Graphic::NativeWindow* window)
{
    m_nativeWindow = window;
}

void UIManager::captureProjectWorkspaceState()
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
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
    auto* settingsView = getView<SettingsView>(SETTINGS_VIEW_NAME);
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
    }
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

void UIManager::syncProjectWorkspaceState()
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        if ( !m_workspaceProjectPath.empty() ) {
            clearProjectWorkspaceViews();
        }
        m_workspaceProjectPath.clear();
        m_workspaceProjectInstance = nullptr;
        if ( !m_noProjectWorkspaceDefaultApplied ) {
            applyNoProjectDefaultWorkspace();
            m_noProjectWorkspaceDefaultApplied = true;
        }
        return;
    }

    m_noProjectWorkspaceDefaultApplied = false;
    std::string projectPath = Config::pathToUtf8(project->m_projectRoot);
    if ( projectPath != m_workspaceProjectPath ||
         project != m_workspaceProjectInstance ) {
        m_workspaceProjectPath     = projectPath;
        m_workspaceProjectInstance = project;
        clearProjectWorkspaceViews();

        const auto& workspace = project->m_settings.m_workspace;
        if ( !workspace.m_imguiIniData.empty() ) {
            ImGui::LoadIniSettingsFromMemory(workspace.m_imguiIniData.data(),
                                             workspace.m_imguiIniData.size());
            MainDockSpaceUI::markProjectWorkspaceLayoutLoaded();
        }

        if ( m_nativeWindow && workspace.m_mainWindow.m_valid ) {
            const auto& windowState = workspace.m_mainWindow;
            m_nativeWindow->applyWindowPlacement(windowState.m_x,
                                                 windowState.m_y,
                                                 windowState.m_width,
                                                 windowState.m_height,
                                                 windowState.m_maximized);
        }

        restoreProjectWorkspaceViews(workspace);

        m_nextWorkspaceCaptureTime = ImGui::GetTime() + 0.5;
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

    if ( auto* mainDock = getView<MainDockSpaceUI>("MainDockSpaceUI") ) {
        workspace.m_overlapCheckOpen =
            mainDock->m_mainMenuview.isOverlapCheckWindowOpen();
        workspace.m_metadataEditorOpen =
            mainDock->m_mainMenuview.isMetadataEditorWindowOpen();
        workspace.m_noteMetadataEditorOpen =
            mainDock->m_mainMenuview.isNoteMetadataEditorWindowOpen();
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
    const ProjectWorkspaceState& workspace)
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        return;
    }

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
        if ( !resolveWorkspaceAudioTrack(
                 *project, controllerState.m_trackId, trackType, trackName) ) {
            continue;
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

    if ( auto* mainDock = getView<MainDockSpaceUI>("MainDockSpaceUI") ) {
        mainDock->m_mainMenuview.setOverlapCheckWindowOpen(
            workspace.m_overlapCheckOpen);
        mainDock->m_mainMenuview.setMetadataEditorWindowOpen(
            workspace.m_metadataEditorOpen);
        mainDock->m_mainMenuview.setNoteMetadataEditorWindowOpen(
            workspace.m_noteMetadataEditorOpen);
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
void UIManager::onPrepareResources(vk::PhysicalDevice&   physicalDevice,
                                   vk::Device&           logicalDevice,
                                   Graphic::VKSwapchain& swapchain,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
    // 检查并重建所有离屏帧缓冲
    for ( const auto& name : m_renderableUiSequence ) {
        auto renderableView = m_uiviews[name]->asRenderableView();
        if ( renderableView && renderableView->needReCreateFrameBuffer() ) {
            renderableView->reCreateFrameBuffer(
                physicalDevice, logicalDevice, swapchain, cmdPool, queue);
        }
    }

    // 检查并重载所有纹理
    for ( const auto& name : m_textureLoaderSequence ) {
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

    const auto& name  = m_renderableUiSequence[taskIndex];
    const auto& views = m_uiviews;
    auto        it    = views.find(name);
    if ( it == views.end() ) {
        return;
    }

    auto renderableView = it->second->asRenderableView();
    if ( renderableView ) {
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
    if ( io.WantCaptureKeyboard ) {
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
