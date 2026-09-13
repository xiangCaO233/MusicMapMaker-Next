#include "ui/UIManager.h"
#include "audio/AudioManager.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/input/translators/ImGuiTranslator.h"
#include "event/input/translators/UniversalCodepoint.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/GLFWNativeEvent.h"
#include "event/ui/iwindow/UIWindowKeyEvent.h"
#include "event/ui/iwindow/UIWindowMouseEvent.h"
#include "event/ui/menu/ProjectLoadedEvent.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"
#include "ui/IAuxiliaryWindowView.h"
#include "ui/ICanvasView.h"
#include "ui/ICanvasWorkspaceService.h"
#include "ui/IEditorApplicationService.h"
#include "ui/IParallelUiPreparable.h"
#include "ui/IRenderableView.h"
#include "ui/ITextureLoader.h"
#include "ui/imgui/CanvasTabManager.h"
#include "ui/imgui/ClipboardBridge.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/audio/AudioSpectrumView.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/imgui/manager/ProjectAudioToolView.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/imgui/markdown/MarkdownImageCache.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/project/ProjectDropRouter.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughService.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include "ui/walkthrough/WelcomeView.h"
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
///
/// 工作区持久化和动态视图清理都使用该键，不应替换为本地化标题。
constexpr const char* AUDIO_WAVEFORM_VIEW_NAME = "AudioWaveform";

/// @brief 主音轨频谱窗口的稳定 UIManager 视图名。
///
/// 与 AudioSpectrumView 的可见窗口标题相互独立。
constexpr const char* AUDIO_SPECTRUM_VIEW_NAME = "AudioSpectrum";

/// @brief BPM 测量工具窗口的稳定 UIManager 视图名。
///
/// 项目工作区只保存是否打开，不从标题反推身份。
constexpr const char* BPM_MEASUREMENT_TOOL_VIEW_NAME = "BpmMeasurementTool";

/// @brief 项目音频工具窗口的稳定 UIManager 视图名。
///
/// 项目切换时按此键捕获、恢复和清理视图。
constexpr const char* PROJECT_AUDIO_TOOL_VIEW_NAME = "ProjectAudioTool";

/// @brief 独立设置窗口的稳定 UIManager 视图名。
///
/// 设置属于应用级视图，不随项目工作区动态清理。
constexpr const char* SETTINGS_VIEW_NAME = "SettingsWindow";

/// @brief 独立批注表窗口的稳定 UIManager 视图名。
///
/// 该键用于工作区恢复批注表开关。
constexpr const char* ANNOTATION_TABLE_VIEW_NAME = "AnnotationTableWindow";

/// @brief 主窗口标题栏宿主 ImGui 窗口名。
///
/// 收集原生拖拽阻挡区时排除自身，避免标题栏把全部基础拖拽区域遮掉。
constexpr std::string_view TOP_MENU_BAR_HOST_NAME = "TopMenuBarHost";

/// @brief 判断主窗口当前是否允许播放 UI 交互音效。
/// @param window 主原生窗口观察指针。
/// @return 未最小化或没有绑定窗口时返回 true。
///
/// 无窗口或尚未创建 GLFW handle 的启动阶段允许反馈，由后续音频/UI
/// 状态自行决定是否 实际播放；最小化窗口则禁止无意义交互声。
/// @warning UI 热路径：每帧查询一次 GLFW 窗口标志，只读取平台窗口状态。
bool isInteractionFeedbackAllowed(Graphic::NativeWindow* window)
{
    // 未绑定原生窗口时没有可查询的最小化状态。
    if ( !window || !window->getWindowHandle() ) {
        return true;
    }

    // GLFW_ICONIFIED 为真时窗口不可见，不播放交互反馈。
    return glfwGetWindowAttrib(window->getWindowHandle(), GLFW_ICONIFIED) !=
           GLFW_TRUE;
}

/// @brief 判断两个拖拽矩形是否相交。
/// @param lhs 第一个矩形。
/// @param rhs 第二个矩形。
/// @return 两个矩形存在正面积交集时返回 true。
///
/// 仅边界接触不视为阻挡；非正面积输入直接拒绝。
bool dragAreasIntersect(const Event::DragArea& lhs, const Event::DragArea& rhs)
{
    // 退化矩形不参与标题栏拖拽几何。
    if ( lhs.w <= 0.0f || lhs.h <= 0.0f || rhs.w <= 0.0f || rhs.h <= 0.0f ) {
        return false;
    }

    // 分离轴条件取反后写成四个严格不等式。
    return lhs.x < rhs.x + rhs.w && lhs.x + lhs.w > rhs.x &&
           lhs.y < rhs.y + rhs.h && lhs.y + lhs.h > rhs.y;
}

/// @brief 判断 ImGui 窗口是否应阻挡主窗口标题栏原生拖拽。
/// @param window 候选 ImGui 窗口。
/// @param viewport 当前主视口。
/// @return 该窗口可阻挡标题栏原生拖拽时返回 true。
///
/// 只考虑主视口中的活动根窗口。子窗口由其根窗口边界覆盖，无鼠标输入窗口不应阻挡
/// 原生拖拽，标题栏宿主自身也必须排除。
bool shouldBlockNativeDragForWindow(const ImGuiWindow&   window,
                                    const ImGuiViewport& viewport)
{
    // 非活动、隐藏或折叠窗口没有可点击内容区域。
    if ( !window.WasActive || window.Hidden || window.Collapsed ) {
        return false;
    }
    // 平台多视口中的独立窗口不属于主原生窗口标题栏坐标系。
    if ( window.Viewport != &viewport ) {
        return false;
    }
    // 子窗口由根窗口处理，无鼠标输入窗口允许事件穿透。
    if ( (window.Flags & ImGuiWindowFlags_ChildWindow) != 0 ||
         (window.Flags & ImGuiWindowFlags_NoMouseInputs) != 0 ) {
        return false;
    }

    // 空名称防御性转换为 string_view，标题栏宿主不阻挡自身。
    const std::string_view name = window.Name ? window.Name : "";
    return name != TOP_MENU_BAR_HOST_NAME;
}

/// @brief 收集遮挡主窗口标题栏原生拖拽区的 ImGui 窗口矩形。
/// @param dragAreas 当前标题栏基础拖拽区域。
/// @return 与基础拖拽区相交且应排除的窗口矩形。
///
/// 返回坐标相对主视口原点，与标题栏 DragArea 事件坐标一致。每个窗口只要命中任一
/// 基础区域就追加一次，避免同一阻挡矩形重复发送。
/// @warning UI 热路径：每帧最多遍历当前 ImGui 根窗口列表；只做几何判断。
std::vector<Event::DragArea> collectNativeDragBlockedAreas(
    const std::vector<Event::DragArea>& dragAreas)
{
    // 无基础拖拽区或 ImGui 尚未初始化时返回空阻挡集合。
    std::vector<Event::DragArea> blockedAreas;
    if ( dragAreas.empty() || !ImGui::GetCurrentContext() ) {
        return blockedAreas;
    }

    // 主视口缺失时无法完成平台坐标换算。
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if ( !viewport ) {
        return blockedAreas;
    }

    // 最坏按焦点顺序窗口数预留容量，循环内避免反复扩容。
    ImGuiContext& context = *ImGui::GetCurrentContext();
    blockedAreas.reserve(static_cast<size_t>(context.WindowsFocusOrder.Size));
    for ( ImGuiWindow* window : context.WindowsFocusOrder ) {
        // 过滤空指针及不具备阻挡语义的窗口。
        if ( !window || !shouldBlockNativeDragForWindow(*window, *viewport) ) {
            continue;
        }

        // ImGui 窗口矩形从屏幕坐标转换为主视口局部坐标。
        const ImRect    rect    = window->Rect();
        Event::DragArea blocker = {
            rect.Min.x - viewport->Pos.x,
            rect.Min.y - viewport->Pos.y,
            rect.GetWidth(),
            rect.GetHeight(),
        };
        for ( const auto& dragArea : dragAreas ) {
            // 命中任一基础区即可作为整体阻挡矩形提交。
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
///
/// 音轨控制器通过前缀识别，其余项目工具使用固定键。应用级设置、欢迎页等视图不在
/// 此清单中，切换项目时保持存活。
bool isProjectWorkspaceDynamicView(const std::string& name)
{
    return name.rfind("TrackController_", 0) == 0 ||
           name == AUDIO_WAVEFORM_VIEW_NAME ||
           name == AUDIO_SPECTRUM_VIEW_NAME ||
           name == BPM_MEASUREMENT_TOOL_VIEW_NAME ||
           name == PROJECT_AUDIO_TOOL_VIEW_NAME;
}

/// @brief 查询项目或皮肤中是否仍存在工作区保存的音轨。
/// @param project 当前项目。
/// @param trackId 音轨 ID。
/// @param type 传入工作区保存类型，找到项目资源时会被实际类型覆盖。
/// @param trackName 传入工作区保存名称，找到项目资源时会被实际名称覆盖。
/// @return 音轨存在时返回 true。
///
/// 项目资源优先，并用当前资源类型和 ID 覆盖工作区旧快照。效果音还允许来自皮肤
/// audioPaths；主音轨不能从皮肤音效回退。
bool resolveWorkspaceAudioTrack(
    const std::vector<AudioResource>& audioResources,
    const std::string& trackId, AudioTrackControllerUI::TrackType& type,
    std::string& trackName)
{
    // 项目资源 ID 是工作区恢复的首选稳定标识。
    for ( const auto& resource : audioResources ) {
        if ( resource.m_id == trackId ) {
            // 当前资源类型覆盖持久化快照，兼容项目配置变化。
            type = resource.m_type == AudioTrackType::Main
                       ? AudioTrackControllerUI::TrackType::Main
                       : AudioTrackControllerUI::TrackType::Effect;
            // 项目资源当前使用 ID 作为控制器显示名。
            trackName = resource.m_id;
            return true;
        }
    }

    if ( type == AudioTrackControllerUI::TrackType::Effect ) {
        // 项目未找到的效果音可解析为当前皮肤内置资源。
        auto& skinData = Config::SkinManager::instance().getData();
        if ( skinData.audioPaths.contains(trackId) ) {
            if ( trackName.empty() ) {
                // 旧工作区未保存名称时使用稳定资源 ID。
                trackName = trackId;
            }
            return true;
        }
    }

    return false;
}

/// @brief 判断文本是否拥有指定前缀。
/// @param text 被检查文本。
/// @param prefix 需要匹配的前缀。
/// @return 文本以该前缀开头时返回 true。
///
/// 显式长度检查避免 substr 对短文本产生不必要对象或越界假设。
bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
}

/// @brief
/// 过滤项目工作区中的多视口平台状态与应用级欢迎页，避免项目覆盖全局窗口。
/// @param iniData 原始 ImGui ini 数据。
/// @return 移除平台 viewport 段和字段后的 ImGui ini 数据。
///
/// 项目只拥有编辑工作区窗口布局，不拥有操作系统平台窗口位置，也不拥有应用级欢迎
/// 页。函数按行保留原始换行与未知字段，仅跳过明确的 section 或 viewport 属性。
std::string sanitizeProjectWorkspaceIni(std::string_view iniData)
{
    // 输出最大不超过输入，提前保留容量避免多次扩容。
    std::string sanitized;
    sanitized.reserve(iniData.size());

    // 标志持续到下一个 section 标题，覆盖整段 Viewport 或 Welcome Window。
    bool   skipViewportSection = false;
    size_t lineStart           = 0;
    while ( lineStart < iniData.size() ) {
        // line 包含原始换行，便于直接追加时保持 ini 文本结构。
        const size_t nextLine = iniData.find('\n', lineStart);
        const size_t lineEnd =
            nextLine == std::string_view::npos ? iniData.size() : nextLine + 1;
        std::string_view line = iniData.substr(lineStart, lineEnd - lineStart);

        // 判断字段时临时移除 CRLF 或 LF，不修改最终保留内容。
        std::string_view lineWithoutEnd = line;
        if ( !lineWithoutEnd.empty() && lineWithoutEnd.back() == '\n' ) {
            lineWithoutEnd.remove_suffix(1);
        }
        if ( !lineWithoutEnd.empty() && lineWithoutEnd.back() == '\r' ) {
            lineWithoutEnd.remove_suffix(1);
        }

        if ( startsWith(lineWithoutEnd, "[Viewport][") ) {
            // 多视口平台段整体由应用和操作系统管理。
            skipViewportSection = true;
        } else if ( startsWith(lineWithoutEnd, "[") ) {
            // 新 section 结束上一跳过状态；欢迎页窗口段单独继续跳过。
            skipViewportSection = startsWith(lineWithoutEnd, "[Window][") &&
                                  lineWithoutEnd.ends_with("###WelcomePage]");
        }

        // 即使普通 Window 段内出现 viewport 字段，也逐项剔除平台绑定信息。
        if ( !skipViewportSection &&
             !startsWith(lineWithoutEnd, "ViewportId=") &&
             !startsWith(lineWithoutEnd, "ViewportPos=") &&
             !startsWith(lineWithoutEnd, "ViewportSize=") &&
             !startsWith(lineWithoutEnd, "ViewportOwned=") ) {
            sanitized.append(line.data(), line.size());
        }

        // 推进到下一原始行，最后无换行行也能正常结束。
        lineStart = lineEnd;
    }

    return sanitized;
}

/// @brief 捕获当前帧可安全传给后台 UI 准备任务的只读快照。
/// @return 当前帧 UI 快照。
///
/// 快照集中读取所有非线程安全 ImGui、皮肤和配置状态。并行准备视图只接收这些值和
/// 稳定字体观察指针，不允许在工作任务中访问全局 UI 单例。
/// @warning UI 热路径：每帧调用，只复制轻量配置和观察指针。
UiFrameSnapshot captureUiFrameSnapshot()
{
    // 配置、皮肤和 ImGui Style 均在当前 UI 帧读取一次。
    auto&       appConfig  = Config::AppConfig::instance();
    const auto& settings   = appConfig.getEditorSettings();
    const auto& aesthetics = settings.aesthetics;
    auto&       skinCfg    = Config::SkinManager::instance();
    const auto& style      = ImGui::GetStyle();

    // DPI 至少为一倍，避免并行测量生成不可交互的极小尺寸。
    UiFrameSnapshot snapshot;
    snapshot.dpiScale = std::max(1.0f, appConfig.getWindowContentScale());
    // Frame 几何来自当前 ImGui 主题和字体状态。
    snapshot.framePadding           = style.FramePadding;
    snapshot.frameHeight            = ImGui::GetFrameHeight();
    snapshot.frameHeightWithSpacing = ImGui::GetFrameHeightWithSpacing();
    // 三类业务字体和 fallback 指针由 SkinManager/ImGui 持有生命周期。
    snapshot.contentFont     = skinCfg.getFont("content");
    snapshot.menuFont        = skinCfg.getFont("menu");
    snapshot.fileManagerFont = skinCfg.getFont("filemanager");
    snapshot.fallbackFont    = ImGui::GetFont();
    snapshot.fontSize        = ImGui::GetFontSize();
    // 翻译版本和字体偏好是布局缓存的重要失效键。
    snapshot.translationVersion = skinCfg.getTranslator().getVersion();
    snapshot.language           = settings.language;
    snapshot.preferredAsciiFont = settings.preferredAsciiFont;
    snapshot.preferredCjkFont   = settings.preferredCjkFont;
    snapshot.fontSizeMultiplier = settings.fontSizeMultiplier;
    snapshot.uiScaleMultiplier  = settings.uiScaleMultiplier;
    // 美学 padding、spacing 与侧栏宽度影响具体视图最小尺寸。
    snapshot.windowPadding      = aesthetics.windowPadding;
    snapshot.itemSpacing        = aesthetics.itemSpacing;
    snapshot.sidebarWidthConfig = skinCfg.getLayoutConfig("side_bar.width");
    return snapshot;
}
}  // namespace

/// @brief 请求下一帧打开应用级欢迎页。
///
/// 只设置边沿标志，实际视图创建和注册在 onUpdateUI 的有效 ImGui 帧中完成。
void UIManager::openWelcome()
{
    m_openWelcome = true;
}
/// @brief 获取 UIManager 持有的引导服务。
/// @return 生命周期与 UIManager 相同的服务引用。
Walkthrough::Service& UIManager::walkthroughService()
{
    return *m_walkthrough;
}
/// @brief 获取当前 UI 管理器独占的演练突出层。
/// @return 仅允许 UI 线程逐帧使用的状态引用。
/// @warning UI 热路径：只解引用稳定 unique_ptr，不进行状态复制。
Walkthrough::Spotlight& UIManager::walkthroughSpotlight()
{
    return *m_walkthroughSpotlight;
}

/// @brief 构造 UI 管理器、引导服务、拖放路由并订阅项目生命周期事件。
///
/// 事件回调可能来自非 UI
/// 线程，只构造值更新并写入并发队列，或设置发布/消费明确的
/// 原子标志。视图创建、销毁、工作区恢复和 ImGui 状态加载全部延迟到 UI 帧。
///
/// 欢迎页初始开关来自软件设置；Clay
/// 文本测量桥接在任何布局构建前初始化。自动保存
/// 仅在原生窗口失焦或最小化事件发生时请求，不在事件回调中直接落盘。
UIManager::UIManager()
{
    // 引导进度与页面定义均位于当前配置根，测试可通过隔离配置根保护个人设置。
    m_walkthrough = std::make_unique<Walkthrough::Service>(
        Config::AppPaths::configRootPath() / "walkthrough-progress.json",
        Config::AppPaths::configRootPath() / "walkthroughs");
    // 突出层只保存当前引导和本帧控件几何，不依赖项目或 GPU 资源生命周期。
    m_walkthroughSpotlight = std::make_unique<Walkthrough::Spotlight>();
    // 引导动作通过菜单工具打开项目选择器，不捕获 UIManager 实例。
    m_walkthrough->registerAction("open_folder",
                                  [] { MenuUtil::openProjectFolderPicker(); });
    // 拖放路由由 UIManager 独占并随其销毁。
    m_projectDropRouter = std::make_unique<ProjectDropRouter>();
    // 启动是否显示欢迎页只读取一次，用户可在设置中影响下次启动。
    m_openWelcome = Config::AppConfig::instance()
                        .getEditorSettings()
                        .m_showWelcomeOnStartup;
    // Clay 需要 ImGui 字体测量回调才能计算文本布局。
    CLayWrapperCore::instance().setupClayTextMeasurement();

    // 保存所有订阅 ID，析构时逐项精确注销。
    auto& eventBus = Event::EventBus::instance();
    m_projectOpenStartedSubId =
        eventBus.subscribe<Event::ProjectOpenStartedEvent>(
            [this](const Event::ProjectOpenStartedEvent& event) {
                // 发布 transition 信号让同帧其他视图立即停止读取旧项目。
                m_projectTransitionSignal.store(true,
                                                std::memory_order_release);
                // 具体生命周期转换排队到 UI 线程消费。
                ProjectUiLifecycleUpdate update;
                update.kind        = ProjectUiLifecycleKind::OpenStarted;
                update.projectRoot = Config::utf8ToPath(event.m_projectPath);
                m_pendingProjectLifecycleUpdates.enqueue(std::move(update));
            });
    m_projectOpenProgressSubId =
        eventBus.subscribe<Event::ProjectOpenProgressEvent>(
            [this](const Event::ProjectOpenProgressEvent& event) {
                // 进度事件复制所有文本和值，队列不保留事件引用。
                ProjectOpenProgressState update;
                update.active   = true;
                update.stage    = event.m_stage;
                update.fraction = event.m_fraction;
                update.detail   = event.m_detail;
                m_pendingProjectOpenProgressUpdates.enqueue(std::move(update));
            });
    m_projectLoadedSubId = eventBus.subscribe<Event::ProjectLoadedEvent>(
        [this](const Event::ProjectLoadedEvent& event) {
            // 加载完成更新携带规范化项目根和可选完整 UI 快照。
            ProjectUiLifecycleUpdate update;
            update.kind        = ProjectUiLifecycleKind::Opened;
            update.projectRoot = Config::utf8ToPath(event.m_projectPath);

            EditorProjectUiSnapshot snapshot;
            if ( m_editorApplicationService &&
                 m_editorApplicationService->currentProjectUiSnapshot(
                     snapshot) &&
                 snapshot.projectRoot.lexically_normal() ==
                     update.projectRoot.lexically_normal() ) {
                // 只有服务快照与事件根一致时才作为恢复真值。
                update.workspace          = std::move(snapshot.workspace);
                update.audioResources     = std::move(snapshot.audioResources);
                update.hasProjectSnapshot = true;
            }
            m_pendingProjectLifecycleUpdates.enqueue(std::move(update));
        });
    m_projectClosedSubId = eventBus.subscribe<Event::ProjectClosedEvent>(
        [this](const Event::ProjectClosedEvent& event) {
            // 关闭事件只排队根路径，当前工作区捕获由 UI 线程按状态处理。
            ProjectUiLifecycleUpdate update;
            update.kind        = ProjectUiLifecycleKind::Closed;
            update.projectRoot = event.m_projectPath;
            m_pendingProjectLifecycleUpdates.enqueue(std::move(update));
        });
    m_projectOpenFailedSubId =
        eventBus.subscribe<Event::ProjectOpenFailedEvent>(
            [this](const Event::ProjectOpenFailedEvent& event) {
                // 打开失败会结束 transition
                // 并恢复无项目或旧状态，延迟统一处理。
                ProjectUiLifecycleUpdate update;
                update.kind        = ProjectUiLifecycleKind::OpenFailed;
                update.projectRoot = Config::utf8ToPath(event.m_projectPath);
                m_pendingProjectLifecycleUpdates.enqueue(std::move(update));
            });
    m_temporaryProjectSaveResultSubId =
        eventBus.subscribe<Event::TemporaryProjectSaveResultEvent>(
            [this](const Event::TemporaryProjectSaveResultEvent& event) {
                // 只有保存成功且返回新路径时才切换活动工作区根。
                if ( !event.m_success || event.m_savedProjectPath.empty() ) {
                    return;
                }
                ProjectUiLifecycleUpdate update;
                // 临时项目另存后视图状态不变，只更新持久化归属根。
                update.kind = ProjectUiLifecycleKind::RootChanged;
                update.projectRoot =
                    Config::utf8ToPath(event.m_savedProjectPath);
                m_pendingProjectLifecycleUpdates.enqueue(std::move(update));
            });
    m_nativeWindowFocusSubId = eventBus.subscribe<Event::GLFWNativeEvent>(
        [this](const Event::GLFWNativeEvent& event) {
            // 只有明确失焦或最小化事件触发自动保存。
            const bool focusLost =
                event.type ==
                    Event::NativeEventType::GLFW_WINDOW_FOCUS_CHANGED &&
                event.hasStateChange && !event.isFocused;
            const bool minimized =
                event.type == Event::NativeEventType::GLFW_ICONFY_WINDOW;
            if ( !focusLost && !minimized ) return;
            if ( m_editorApplicationService ) {
                // 服务异步处理保存请求，原生事件回调不做文件 IO。
                m_editorApplicationService->requestAutoSave(
                    EditorAutoSaveReason::NativeWindowFocusLost);
            }
        });
}

/// @brief 注销全部事件订阅并解除原生文件选择器主窗口绑定。
///
/// 回调均捕获 this，必须在成员析构前移除。订阅 ID
/// 为零表示对应注册未成功或未发生， 逐项检查避免向 EventBus 传入无效 ID。
UIManager::~UIManager()
{
    // 先解除外部静态文件选择器对 NativeWindow handle 的观察。
    NativeFileDialog::bindMainWindow(nullptr);
    auto& eventBus = Event::EventBus::instance();
    if ( m_projectOpenStartedSubId != 0 ) {
        eventBus.unsubscribe<Event::ProjectOpenStartedEvent>(
            m_projectOpenStartedSubId);
    }
    if ( m_projectOpenProgressSubId != 0 ) {
        eventBus.unsubscribe<Event::ProjectOpenProgressEvent>(
            m_projectOpenProgressSubId);
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
    if ( m_nativeWindowFocusSubId != 0 ) {
        eventBus.unsubscribe<Event::GLFWNativeEvent>(m_nativeWindowFocusSubId);
    }
}

/// @brief 绑定主原生窗口并同步给原生文件选择器。
/// @param window 非拥有 NativeWindow 指针，可为空以解除绑定。
///
/// UIManager 不管理窗口生命周期；调用方必须在窗口销毁前传入 nullptr
/// 或先销毁管理器。
void UIManager::setNativeWindow(Graphic::NativeWindow* window)
{
    // 保存观察指针供反馈、拖拽区和窗口位置工作区状态使用。
    m_nativeWindow = window;
    // NFD 只需要平台 handle，空窗口对应 nullptr。
    NativeFileDialog::bindMainWindow(window ? window->getWindowHandle()
                                            : nullptr);
}

/// @brief 获取当前绑定的主原生窗口。
/// @return 非拥有指针；尚未绑定时为空。
Graphic::NativeWindow* UIManager::getNativeWindow() const
{
    return m_nativeWindow;
}

/// @brief 绑定当前协作房间观察指针。
/// @param room 由网络层拥有的房间，可为空。
///
/// 视图通过 UIManager 取得房间，但所有权和线程生命周期仍由 Collaboration
/// 服务管理。
void UIManager::setCollaborationRoom(
    Network::Collaboration::CollaborationRoom* room)
{
    m_collaborationRoom = room;
}

/// @brief 获取当前协作房间。
/// @return 非拥有房间指针；未连接时为空。
Network::Collaboration::CollaborationRoom*
UIManager::getCollaborationRoom() const
{
    return m_collaborationRoom;
}

/// @brief 获取主窗口的平台边框适配器。
/// @return 窗口和适配器存在时返回非拥有指针，否则为空。
Graphic::IWindowFrameAdapter* UIManager::getWindowFrameAdapter() const
{
    return m_nativeWindow ? m_nativeWindow->getWindowFrameAdapter() : nullptr;
}

/// @brief 查询项目打开或关闭过渡是否仍在进行。
/// @return 跨线程 transition 信号为真时返回 true。
/// @warning UI 热路径：使用 acquire 读取发布状态，不得增加阻塞等待。
bool UIManager::isProjectTransitionInProgress() const
{
    return m_projectTransitionSignal.load(std::memory_order_acquire);
}

/// @brief 获取 UI 线程最近消费的项目打开进度。
/// @return 仅在下一次生命周期消费前有效的只读引用。
const ProjectOpenProgressState& UIManager::getProjectOpenProgress() const
{
    return m_projectOpenProgress;
}

/// @brief 查询 UI 生命周期是否持有活动项目工作区。
/// @return 已完成项目打开且尚未关闭时返回 true。
bool UIManager::hasActiveProjectUiState() const
{
    return m_projectLifecycleState.hasActiveProject;
}

/// @brief 判断时间线窗口是否正在拖动 Timing 框选区域。
/// @return 时间线正在框选时返回 true。
/// @warning UI 热路径：空格快捷键按下时调用；只读取已注册视图的本地状态。
bool UIManager::isTimelineTimingMarqueeSelecting()
{
    const auto* timeline = getCanvasView("TimelineWindow");
    return timeline && timeline->isTimingMarqueeSelecting();
}

/// @brief 判断时间线窗口是否正在通过抓取工具拖动 Timing。
/// @return 时间线正在拖动 Timing 时返回 true。
/// @warning UI 热路径：空格快捷键按下时调用；只读取已注册视图的本地状态。
bool UIManager::isTimelineTimingDragging()
{
    const auto* timeline = getCanvasView("TimelineWindow");
    return timeline && timeline->isTimingDragging();
}

/// @brief 获取当前 UI 工作区归属的项目根目录。
/// @return UIManager 持有的规范路径引用；无项目时为空。
const std::filesystem::path& UIManager::getActiveProjectRoot() const
{
    return m_activeProjectRoot;
}

/// @brief 替换标题栏提供的原生窗口拖拽基础区域。
/// @param areas 主视口局部坐标中的矩形集合。
///
/// 参数按值接收后移动，调用方可在提交后复用原容器；阻挡区在每帧同步时动态计算。
void UIManager::setNativeWindowDragAreas(std::vector<Event::DragArea> areas)
{
    m_nativeWindowDragAreas = std::move(areas);
}

/// @brief 发布当前标题栏拖拽区及其 ImGui 窗口阻挡区。
///
/// 事件消费者据此更新平台命中测试。阻挡区必须每帧根据活动窗口重新收集，基础区则由
/// 顶部菜单布局显式设置。
/// @warning UI 热路径：遍历当前根窗口并发布轻量几何事件，不得做平台阻塞调用。
void UIManager::syncNativeWindowDragAreas()
{
    // 没有原生窗口时没有平台命中测试消费者。
    if ( !m_nativeWindow ) {
        return;
    }

    // 事件携带管理器来源和稳定宿主名，便于平台层区分更新者。
    Event::UpdateDragAreaEvent event;
    event.uiManager    = this;
    event.sourceUiName = std::string(TOP_MENU_BAR_HOST_NAME);
    event.areas        = m_nativeWindowDragAreas;
    // blockedAreas 仅包含与基础拖拽区相交的活动根窗口。
    event.blockedAreas = collectNativeDragBlockedAreas(m_nativeWindowDragAreas);
    Event::EventBus::instance().publish(event);
}

/// @brief 把当前项目视图、ImGui 布局和主窗口位置写入项目工作区对象。
///
/// 仅活动且非过渡项目允许捕获。函数通过应用服务取得可变工作区，视图开关由专用
/// helper 写入，ImGui ini 保留当前完整布局，主窗口 placement 在绑定时一并记录。
/// @warning 低频保存路径：会序列化 ImGui ini 和查询平台窗口，不得逐帧调用。
void UIManager::captureProjectWorkspaceState()
{
    // 过渡中或无项目时不能把临时 UI 状态写入任意项目。
    if ( isProjectTransitionInProgress() ||
         !m_projectLifecycleState.hasActiveProject ) {
        return;
    }

    // 应用服务负责项目模型所有权和工作区持久化入口。
    if ( !m_editorApplicationService ) {
        return;
    }
    auto* workspace = m_editorApplicationService->mutableCurrentWorkspace(
        m_activeProjectRoot);
    // 根路径不匹配或项目已切换时服务可拒绝返回对象。
    if ( !workspace ) {
        return;
    }
    // 先捕获动态视图及画布开关。
    captureProjectWorkspaceViews(*workspace);

    // ImGui 提供当前上下文 ini 内存快照，复制到项目值对象。
    size_t      iniSize = 0;
    const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
    if ( iniData && iniSize > 0 ) {
        workspace->m_imguiIniData.assign(iniData, iniSize);
    }

    if ( m_nativeWindow ) {
        // 主窗口位置属于项目工作区，但只有绑定原生窗口时才可取得。
        auto& windowState = workspace->m_mainWindow;
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
///
/// 视图按稳定键惰性创建；已存在时复用其停靠和表单状态。只有从关闭到打开的边沿
/// 播放弹窗反馈，重复切换标签页不会重复发声。
void UIManager::openSettingsWindow(MMM::Event::SettingsTab tab)
{
    // 先记录原打开状态，视图创建后用于反馈判定。
    auto*      settingsView = getView<SettingsView>(SETTINGS_VIEW_NAME);
    const bool wasOpen      = settingsView && settingsView->isOpen();
    if ( !settingsView ) {
        // 可见标题本地化，注册身份使用固定英文键。
        auto view =
            std::make_unique<SettingsView>(TR("title.settings_manager").data());
        settingsView = view.get();
        registerView(SETTINGS_VIEW_NAME, std::move(view));
    }

    if ( settingsView ) {
        // 打开指定页并请求下一帧中心停靠及键盘焦点。
        settingsView->open(tab);
        settingsView->requestDockToCenter();
        settingsView->requestFocus();
        if ( !wasOpen ) {
            ::MMM::UI::PlayPopupOpenFeedback();
        }
    }
}

/// @brief 请求下一次资源准备阶段重载皮肤相关图形资源。
///
/// 多次请求合并为一个布尔脏位，资源阶段会先等待 GPU 再失效 shader 与纹理缓存。
/// @warning 低频资源重载路径：皮肤热切换后调用，只置脏位；实际 Vulkan
/// 资源释放和重建在 onPrepareResources 中执行。
void UIManager::requestSkinResourceReload()
{
    m_skinResourceReloadRequested = true;
}

/// @brief 为新打开的音轨控制器选择默认 Dock 节点。
/// @return 目标 Dock 节点 ID；无法解析时返回 0。
///
/// 优先使用活动非占位画布的 Dock，活动项无效时按工作区顺序寻找首个有效画布，
/// 最后回退主 DockSpace 中心节点。
ImGuiID UIManager::resolveAudioControllerDockId()
{
    // 没有画布工作区服务时无法枚举候选。
    auto* workspace = getCanvasWorkspaceService();
    if ( !workspace ) {
        return 0;
    }
    // fillEntries 返回轻量工作区顺序快照。
    std::vector<CanvasWorkspaceEntry> entries;
    workspace->fillEntries(entries);

    /// 把工作区条目解析为有效 Canvas Dock ID。
    auto resolveEntryDockId = [this](const CanvasWorkspaceEntry& entry) {
        // Logo 占位项没有实际画布窗口。
        if ( entry.isLogoPlaceholder ) {
            return static_cast<ImGuiID>(0);
        }

        // cameraId 也是 UIManager 中 Canvas 视图的注册键。
        auto* canvas = getCanvasView(entry.cameraId);
        if ( !canvas ) {
            return static_cast<ImGuiID>(0);
        }
        return canvas->getDockId();
    };

    // 活动条目最符合用户当前上下文，优先尝试。
    const int32_t activeIndex = workspace->getActiveEntryIndex();
    if ( activeIndex >= 0 &&
         activeIndex < static_cast<int32_t>(entries.size()) ) {
        ImGuiID activeDockId =
            resolveEntryDockId(entries[static_cast<size_t>(activeIndex)]);
        if ( activeDockId != 0 ) {
            return activeDockId;
        }
    }

    // 活动项不可用时按持久化顺序寻找首个真实画布。
    for ( const auto& entry : entries ) {
        ImGuiID dockId = resolveEntryDockId(entry);
        if ( dockId != 0 ) {
            return dockId;
        }
    }

    // 尚无任何画布时使用主 DockSpace 中心节点。
    return MainDockSpaceUI::getCenterDockId();
}

/// @brief 打开音轨控制器并默认停靠到谱面画布标签组。
/// @param trackId 音轨标识符。
/// @param trackName 音轨显示名称。
/// @param type 音轨类型。
///
/// 效果音在打开控制器前请求应用服务确保运行时音轨已加载。视图按 trackId
/// 派生的稳定 键去重，已有控制器只重新请求停靠和焦点。
void UIManager::openAudioTrackController(const std::string& trackId,
                                         const std::string& trackName,
                                         AudioTrackControllerUI::TrackType type)
{
    if ( type == AudioTrackControllerUI::TrackType::Effect ) {
        // 项目或皮肤效果音由应用服务解析并加载，失败不阻止窗口展示。
        if ( m_editorApplicationService ) {
            (void)m_editorApplicationService->ensureEffectAudioTrackLoaded(
                trackId);
        }
    }

    // makeViewName 对 trackId 加统一前缀，供工作区动态视图识别。
    std::string viewName   = AudioTrackControllerUI::makeViewName(trackId);
    auto*       controller = getView<AudioTrackControllerUI>(viewName);
    if ( !controller ) {
        // 空显示名回退稳定 ID，避免无标题窗口。
        auto view = std::make_unique<AudioTrackControllerUI>(
            trackId, trackName.empty() ? trackId : trackName, type);
        controller = view.get();
        registerView(viewName, std::move(view));
    }

    if ( controller ) {
        // Dock 请求与 Focus 请求由控制器在其下一次 update 消费。
        controller->requestDockTo(resolveAudioControllerDockId());
        controller->requestFocus();
    }
}

/// @brief 打开或聚焦当前项目的音频资源布局工具。
///
/// 过渡中或无项目时拒绝打开。视图惰性创建，打开状态通过应用服务立即标记并请求保存，
/// 使项目工作区恢复能够保留该工具。
void UIManager::openProjectAudioTool()
{
    // 工具依赖活动项目资源，不能在无项目或切换窗口中使用。
    if ( !hasActiveProjectUiState() || isProjectTransitionInProgress() ) {
        return;
    }

    // 稳定注册键用于工作区捕获和恢复。
    auto* tool = getView<ProjectAudioToolView>(PROJECT_AUDIO_TOOL_VIEW_NAME);
    const bool wasOpen = tool && tool->isOpen();
    if ( !tool ) {
        // 可见标题来自翻译，所有权交给 UIManager。
        auto view = std::make_unique<ProjectAudioToolView>(
            TR("title.project_audio_tool").data());
        tool = view.get();
        registerView(PROJECT_AUDIO_TOOL_VIEW_NAME, std::move(view));
    }
    // 防御注册失败；正常路径总能取得实例。
    if ( !tool ) return;

    // 设置打开、请求焦点并通知项目模型持久化状态。
    tool->setOpen(true);
    tool->requestFocus();
    if ( m_editorApplicationService ) {
        m_editorApplicationService->markProjectAudioToolOpenAndSave();
    }
    if ( !wasOpen ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 重新加载当前已打开控制器引用的项目音效。
///
/// 只扫描 TrackController 前缀视图并过滤效果音类型。主音轨由独立 BGM
/// 生命周期管理， 未打开的效果音无需因皮肤重载提前解码。
/// @warning 低频皮肤重载路径：每个已打开音效控制器最多触发一次单文件
/// 解码，禁止放入每帧 UI 更新。
void UIManager::reloadOpenEffectAudioTracks()
{
    // 注册表拥有所有动态控制器，名称前缀先做低成本过滤。
    for ( const auto& [name, view] : m_uiviews ) {
        if ( name.rfind("TrackController_", 0) != 0 || !view ) {
            continue;
        }

        // getActualInstance 处理可能的视图包装层，再按运行时类型使用。
        auto* controller =
            static_cast<AudioTrackControllerUI*>(view->getActualInstance());
        // 非效果控制器不参与此重载路径。
        if ( !controller || controller->getTrackType() !=
                                AudioTrackControllerUI::TrackType::Effect ) {
            continue;
        }
        // 服务根据当前项目或皮肤重新解析同一稳定 ID。
        if ( m_editorApplicationService ) {
            (void)m_editorApplicationService->ensureEffectAudioTrackLoaded(
                controller->getTrackId());
        }
    }
}

/// @brief 在无活动项目时恢复侧栏默认文件浏览器工作区。
///
/// 同时同步 SideBarUI 选中枚举和 FloatingManagerUI
/// 子视图显隐，避免标题栏状态与实际 浮动内容不一致。
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

/// @brief 在 UI 线程消费项目进度与生命周期并发队列。
///
/// 先合并所有进度更新，再按到达顺序归约生命周期状态。Opened 准备待恢复工作区，
/// Closed/OpenFailed 清理活动根，RootChanged 只迁移同一活动工作区归属。
/// @warning UI 热路径：每帧调用；队列消费只做值移动，不直接执行项目 IO。
void UIManager::consumePendingProjectLifecycleUpdates()
{
    // 进度事件可能高频到达，循环消费到当前队列为空。
    ProjectOpenProgressState progressUpdate;
    while ( m_pendingProjectOpenProgressUpdates.try_dequeue(progressUpdate) ) {
        applyProjectOpenProgress(m_projectOpenProgress,
                                 progressUpdate.stage,
                                 progressUpdate.fraction,
                                 std::move(progressUpdate.detail));
    }

    // 生命周期事件必须保持队列顺序以正确归约状态机。
    ProjectUiLifecycleUpdate update;
    while ( m_pendingProjectLifecycleUpdates.try_dequeue(update) ) {
        // 纯状态转换由 reduce helper 集中定义。
        m_projectLifecycleState =
            reduceProjectUiLifecycleState(m_projectLifecycleState, update.kind);

        switch ( update.kind ) {
        case ProjectUiLifecycleKind::OpenStarted: {
            // 进度详情优先显示项目根末级目录，空时回退完整路径。
            auto detailPath = update.projectRoot.filename();
            if ( detailPath.empty() ) {
                detailPath = update.projectRoot;
            }
            beginProjectOpenProgress(m_projectOpenProgress,
                                     Config::pathToUtf8(detailPath));
            break;
        }
        case ProjectUiLifecycleKind::Opened:
            // 活动根与可选工作区快照在同一事件中切换。
            m_activeProjectRoot                = std::move(update.projectRoot);
            m_noProjectWorkspaceDefaultApplied = false;
            m_pendingProjectWorkspace          = ProjectWorkspaceState{};
            m_pendingProjectAudioResources.clear();
            if ( update.hasProjectSnapshot ) {
                // 快照值移动到 UI 待恢复槽，下一阶段再操作视图。
                m_pendingProjectWorkspace = std::move(update.workspace);
                m_pendingProjectAudioResources =
                    std::move(update.audioResources);
            }
            // 完成进度并 release 发布过渡结束。
            m_projectWorkspaceRestorePending = true;
            finishProjectOpenProgress(m_projectOpenProgress);
            m_projectTransitionSignal.store(false, std::memory_order_release);
            break;
        case ProjectUiLifecycleKind::Closed:
        case ProjectUiLifecycleKind::OpenFailed:
            if ( !m_projectLifecycleState.hasActiveProject ) {
                // 状态机确认无活动项目后才清理根和待恢复数据。
                m_activeProjectRoot.clear();
                m_projectWorkspaceRestorePending = false;
                m_pendingProjectAudioResources.clear();
            }
            if ( update.kind == ProjectUiLifecycleKind::OpenFailed ) {
                // 失败明确结束进度和过渡；普通 Closed 可能属于切换中间态。
                finishProjectOpenProgress(m_projectOpenProgress);
                m_projectTransitionSignal.store(false,
                                                std::memory_order_release);
            }
            break;
        case ProjectUiLifecycleKind::RootChanged:
            // 临时项目另存只在当前确有活动项目时更新根。
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

/// @brief 根据生命周期状态恢复、清理或周期捕获项目工作区。
///
/// 过渡期间完全暂停工作区操作。无项目状态只应用一次默认侧栏；新项目打开时先清理
/// 旧动态视图、加载经过清洗的 ImGui ini、应用主窗口位置，再恢复视图开关。
///
/// 恢复后延迟半秒首次捕获，正常活动项目每两秒更新内存工作区快照。计时使用 ImGui
/// 时间且不阻塞线程，最终落盘由项目保存流程负责。
/// @warning UI 热路径：每帧调用；完整捕获只在低频计时分支发生。
void UIManager::syncProjectWorkspaceState()
{
    // 切换期间旧新项目归属未稳定，不清理也不写入工作区。
    if ( isProjectTransitionInProgress() ) {
        return;
    }

    if ( shouldApplyNoProjectWorkspace(m_projectLifecycleState) ) {
        // 离开项目后先销毁所有项目动态工具。
        if ( !m_workspaceProjectPath.empty() ) {
            clearProjectWorkspaceViews();
        }
        m_workspaceProjectPath.clear();
        // 默认侧栏只应用一次，保留用户在无项目状态下后续手动操作。
        if ( !m_noProjectWorkspaceDefaultApplied ) {
            applyNoProjectDefaultWorkspace();
            m_noProjectWorkspaceDefaultApplied = true;
        }
        return;
    }

    m_noProjectWorkspaceDefaultApplied = false;
    if ( m_projectWorkspaceRestorePending ) {
        // 新根成为当前工作区身份，旧动态视图在恢复前全部清理。
        m_workspaceProjectPath = Config::pathToUtf8(m_activeProjectRoot);
        clearProjectWorkspaceViews();

        const auto& workspace = m_pendingProjectWorkspace;
        if ( !workspace.m_imguiIniData.empty() ) {
            // 过滤平台多视口和欢迎页后才交给 ImGui 加载。
            std::string sanitizedIni =
                sanitizeProjectWorkspaceIni(workspace.m_imguiIniData);
            if ( !sanitizedIni.empty() ) {
                // 欢迎页先释放对旧 Dock 节点的假设。
                if ( auto* welcome = getView<WelcomeView>("Welcome") )
                    welcome->prepareForDockLayoutChange();
                ImGui::LoadIniSettingsFromMemory(sanitizedIni.data(),
                                                 sanitizedIni.size());
                // 标记主 DockSpace 已接收项目布局，避免默认布局覆盖。
                MainDockSpaceUI::markProjectWorkspaceLayoutLoaded();
            }
        }

        if ( m_nativeWindow && workspace.m_mainWindow.m_valid ) {
            // 只有有效 placement 且窗口已绑定时恢复平台窗口几何。
            const auto& windowState = workspace.m_mainWindow;
            m_nativeWindow->applyWindowPlacement(windowState.m_x,
                                                 windowState.m_y,
                                                 windowState.m_width,
                                                 windowState.m_height,
                                                 windowState.m_maximized);
        }

        // 视图恢复在布局和窗口位置之后执行，使新窗口能找到目标 Dock。
        restoreProjectWorkspaceViews(workspace, m_pendingProjectAudioResources);

        // 延迟首次捕获，给新注册视图至少一个完整布局帧。
        m_nextWorkspaceCaptureTime       = ImGui::GetTime() + 0.5;
        m_projectWorkspaceRestorePending = false;
        m_pendingProjectAudioResources.clear();
    }

    // 活动项目按非阻塞时间门槛周期捕获工作区。
    double now = ImGui::GetTime();
    if ( now >= m_nextWorkspaceCaptureTime ) {
        captureProjectWorkspaceState();
        m_nextWorkspaceCaptureTime = now + 2.0;
    }
}

/// @brief 把当前打开的项目相关视图状态写入工作区值对象。
/// @param workspace 接收视图开关、控制器和侧栏状态的项目工作区。
///
/// 函数先清零所有受管理字段，再按 UI 注册顺序收集打开视图，保证已经关闭或注销的
/// 窗口不会残留在旧快照中。Canvas 辅助窗口与侧栏通过各自能力接口读取。
void UIManager::captureProjectWorkspaceViews(ProjectWorkspaceState& workspace)
{
    // 重建音轨控制器数组，并为所有单例工具建立关闭基线。
    workspace.m_audioControllers.clear();
    workspace.m_audioWaveformOpen      = false;
    workspace.m_audioSpectrumOpen      = false;
    workspace.m_bpmMeasurementToolOpen = false;
    workspace.m_projectAudioToolOpen   = false;
    workspace.m_bpmMeasurementAudioTrackId.clear();
    workspace.m_timingPointsTableOpen  = false;
    workspace.m_annotationTableOpen    = false;
    workspace.m_overlapCheckOpen       = false;
    workspace.m_metadataEditorOpen     = false;
    workspace.m_noteMetadataEditorOpen = false;

    // m_uiSequence 提供确定的视图捕获顺序。
    for ( const auto& name : m_uiSequence ) {
        auto viewIt = m_uiviews.find(name);
        // 已注销或关闭视图不写入工作区。
        if ( viewIt == m_uiviews.end() || !viewIt->second->isOpen() ) {
            continue;
        }

        if ( name.rfind("TrackController_", 0) == 0 ) {
            // 控制器保存稳定音轨 ID、显示名和可序列化类型名。
            auto* controller = getView<AudioTrackControllerUI>(name);
            if ( !controller ) {
                continue;
            }

            // 状态按值追加，恢复时会再次验证资源是否仍存在。
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
            // 单例分析视图只需保存打开布尔值。
            workspace.m_audioWaveformOpen = true;
        } else if ( name == AUDIO_SPECTRUM_VIEW_NAME ) {
            workspace.m_audioSpectrumOpen = true;
        } else if ( name == BPM_MEASUREMENT_TOOL_VIEW_NAME ) {
            // BPM 工具额外保存当前选择的音频资源 ID。
            auto* tool = getView<BpmMeasurementToolView>(name);
            if ( tool ) {
                workspace.m_bpmMeasurementToolOpen = true;
                workspace.m_bpmMeasurementAudioTrackId =
                    tool->getSelectedAudioTrackId();
            }
        } else if ( name == PROJECT_AUDIO_TOOL_VIEW_NAME ) {
            workspace.m_projectAudioToolOpen = true;
        }
    }

    // 时间点表属于 Timeline Canvas 内部辅助窗口，不在 UIManager 注册表中。
    if ( auto* timeline = getCanvasView("TimelineWindow") ) {
        workspace.m_timingPointsTableOpen = timeline->isTimingPointsTableOpen();
    }
    // 批注表通过通用 AuxiliaryWindow 能力保存打开状态。
    if ( auto* annotationTable =
             getAuxiliaryWindowView(ANNOTATION_TABLE_VIEW_NAME) ) {
        workspace.m_annotationTableOpen = annotationTable->isWindowOpen();
    }

    // 侧栏隐藏时序列化 None，否则把当前子视图 ID 转为稳定工作区名称。
    if ( auto* sideBarManager = getView<FloatingManagerUI>("SideBarManager") ) {
        SideBarTab activeTab = SideBarTab::None;
        if ( sideBarManager->isVisible() ) {
            activeTab = SubViewIdToTab(sideBarManager->getCurrentSubViewId());
        }
        workspace.m_sidebarActiveTab =
            SideBarUI::workspaceNameFromTab(activeTab);
    }
}

/// @brief 根据项目工作区重新创建动态视图并恢复辅助窗口状态。
/// @param workspace 待恢复的项目工作区快照。
/// @param audioResources 当前项目音频资源列表。
///
/// 每个音轨控制器恢复前重新解析当前项目或皮肤资源，跳过已经删除的 ID。所有单例
/// 工具按稳定键去重，Canvas 内部表格通过能力接口更新。
void UIManager::restoreProjectWorkspaceViews(
    const ProjectWorkspaceState&      workspace,
    const std::vector<AudioResource>& audioResources)
{
    // 侧栏工作区名称先转换为枚举，并同步标题栏与浮动管理器。
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
        // 空 ID 无法形成稳定控制器注册键。
        if ( controllerState.m_trackId.empty() ) {
            continue;
        }

        // 类型和名称从快照恢复，随后允许当前资源覆盖陈旧值。
        auto trackType = AudioTrackControllerUI::workspaceNameToTrackType(
            controllerState.m_trackType);
        std::string trackName = controllerState.m_trackName.empty()
                                    ? controllerState.m_trackId
                                    : controllerState.m_trackName;
        if ( !resolveWorkspaceAudioTrack(audioResources,
                                         controllerState.m_trackId,
                                         trackType,
                                         trackName) ) {
            // 当前项目和皮肤都不存在的资源不恢复窗口。
            continue;
        }
        if ( trackType == AudioTrackControllerUI::TrackType::Effect ) {
            // 效果音控制器创建前请求运行时加载对应音轨。
            if ( m_editorApplicationService ) {
                (void)m_editorApplicationService->ensureEffectAudioTrackLoaded(
                    controllerState.m_trackId);
            }
        }

        // 已由其他恢复路径注册的控制器保持现有实例。
        std::string viewName =
            AudioTrackControllerUI::makeViewName(controllerState.m_trackId);
        if ( getView<AudioTrackControllerUI>(viewName) ) {
            continue;
        }

        // 新控制器所有权交给 UIManager，首次 update 将建立窗口。
        registerView(viewName,
                     std::make_unique<AudioTrackControllerUI>(
                         controllerState.m_trackId, trackName, trackType));
    }

    if ( workspace.m_audioWaveformOpen &&
         !getView<AudioWaveformView>(AUDIO_WAVEFORM_VIEW_NAME) ) {
        // 波形单例以当前翻译标题重建。
        registerView(AUDIO_WAVEFORM_VIEW_NAME,
                     std::make_unique<AudioWaveformView>(
                         TR("ui.audio_manager.waveform_title").data()));
    }

    if ( workspace.m_audioSpectrumOpen &&
         !getView<AudioSpectrumView>(AUDIO_SPECTRUM_VIEW_NAME) ) {
        // 频谱单例由自身首次打开时准备分析缓存。
        registerView(AUDIO_SPECTRUM_VIEW_NAME,
                     std::make_unique<AudioSpectrumView>(
                         TR("ui.audio_manager.spectrum_title").data()));
    }

    if ( workspace.m_bpmMeasurementToolOpen ) {
        // BPM 工具可复用已存在实例，再恢复其音轨选择。
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

    if ( workspace.m_projectAudioToolOpen &&
         !getView<ProjectAudioToolView>(PROJECT_AUDIO_TOOL_VIEW_NAME) ) {
        // 项目音频工具按单例键恢复。
        registerView(PROJECT_AUDIO_TOOL_VIEW_NAME,
                     std::make_unique<ProjectAudioToolView>(
                         TR("title.project_audio_tool").data()));
    }

    // 恢复 Canvas 内部与独立辅助窗口开关。
    if ( auto* timeline = getCanvasView("TimelineWindow") ) {
        timeline->setTimingPointsTableOpen(workspace.m_timingPointsTableOpen);
    }
    if ( auto* annotationTable =
             getAuxiliaryWindowView(ANNOTATION_TABLE_VIEW_NAME) ) {
        annotationTable->setWindowOpen(workspace.m_annotationTableOpen);
    }
}

/// @brief 注销所有随项目工作区生命周期变化的动态视图。
///
/// 先收集名称再逐项注销，避免遍历 m_uiSequence 时修改同一容器。应用级视图和固定
/// Canvas 不受影响。
void UIManager::clearProjectWorkspaceViews()
{
    // 临时数组只包含当前注册顺序中的动态名称。
    std::vector<std::string> dynamicViews;
    for ( const auto& name : m_uiSequence ) {
        if ( isProjectWorkspaceDynamicView(name) ) {
            dynamicViews.push_back(name);
        }
    }

    // unregister 会同步更新三个序列并等待必要 GPU 生命周期。
    for ( const auto& name : dynamicViews ) {
        unregisterView(name);
    }
}

/// @brief 注入 Game 组合根提供的画布工作区服务。
/// @param service 新服务唯一所有权，可为空以移除能力。
///
/// 注入发生在 UI 更新前，UIManager 负责后续生命周期。
void UIManager::setCanvasWorkspaceService(
    std::unique_ptr<ICanvasWorkspaceService> service)
{
    m_canvasWorkspaceService = std::move(service);
}

/// @brief 获取已注入画布工作区服务的观察指针。
/// @return 服务存在时返回非拥有指针，否则为空。
ICanvasWorkspaceService* UIManager::getCanvasWorkspaceService() const
{
    return m_canvasWorkspaceService.get();
}

/// @brief 注入 Game 组合根提供的编辑器应用服务。
/// @param service 新服务唯一所有权，可为空。
///
/// 服务桥接项目模型、音频加载、自动保存和 FPS 发布，避免 UIManager
/// 依赖应用实现。
void UIManager::setEditorApplicationService(
    std::unique_ptr<IEditorApplicationService> service)
{
    m_editorApplicationService = std::move(service);
}

/// @brief 获取已注入编辑器应用服务的观察指针。
/// @return 服务存在时返回非拥有指针，否则为空。
IEditorApplicationService* UIManager::getEditorApplicationService() const
{
    return m_editorApplicationService.get();
}

/// @brief 按注册名查询画布能力观察指针。
/// @param name UIManager 稳定视图名。
/// @return 视图存在且实现 Canvas 能力时返回非拥有指针，否则为空。
/// @warning UI 热路径：只查询本地注册表并调用能力访问器。
ICanvasView* UIManager::getCanvasView(const std::string& name) const
{
    const auto it = m_uiviews.find(name);
    return it == m_uiviews.end() ? nullptr : it->second->asCanvasView();
}

/// @brief 按注册名查询独立窗口能力观察指针。
/// @param name UIManager 稳定视图名。
/// @return 视图存在且实现辅助窗口能力时返回非拥有指针，否则为空。
/// @warning UI 热路径：只查询本地注册表并调用能力访问器。
IAuxiliaryWindowView* UIManager::getAuxiliaryWindowView(
    const std::string& name) const
{
    const auto it = m_uiviews.find(name);
    return it == m_uiviews.end() ? nullptr
                                 : it->second->asAuxiliaryWindowView();
}

/// @brief 注册视图并接管其唯一所有权。
/// @param name 视图稳定注册名。
/// @param view 待接管的非空视图。
///
/// 注册顺序决定更新和工作区捕获顺序；可渲染能力与纹理加载能力分别加入专用序列。
/// 同名注册会替换 map 所有权但仍追加序列，因此调用方必须先检查去重。
void UIManager::registerView(const std::string&       name,
                             std::unique_ptr<IUIView> view)
{
    // 通用序列包含所有视图并保持注册顺序。
    m_uiSequence.push_back(name);

    if ( view->renderable() ) {
        // 可渲染视图参与离屏命令任务枚举。
        m_renderableUiSequence.push_back(name);
        XINFO("Registered Renderable [{}] UIView", name);
    } else {
        XINFO("Registered General [{}] UIView", name);
    }

    // 纹理加载能力独立于 renderable 标志查询。
    auto textureLoader = view->asTextureLoader();
    if ( textureLoader ) {
        m_textureLoaderSequence.push_back(name);
    }

    // 最后移动所有权到注册表，前面能力检查期间对象仍由参数持有。
    m_uiviews[name] = std::move(view);
}

/// @brief 注销并安全销毁一个视图。
/// @param name 待注销的稳定注册名。
///
/// 若视图拥有 GPU 资源，销毁前执行必要等待；无论 map
/// 是否存在都清理三个名称序列， 使重复注销保持幂等。
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

/// @brief 等待必要 GPU 使用后销毁全部已注册视图。
///
/// 用于应用整体关闭；当前实现只清空所有权 map，管理器随后整体销毁其他序列。
void UIManager::clearAllViews()
{
    /// @brief 当前仍注册在 UIManager 内的视图条目。
    for ( auto& entry : m_uiviews ) {
        waitForGpuBeforeDestroyView(*entry.second);
    }
    m_uiviews.clear();
}

/// @brief 为全部能力视图准备离屏帧缓冲和纹理资源。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param swapchain 当前交换链。
/// @param cmdPool 资源上传命令池。
/// @param queue 资源上传队列。
///
/// 皮肤强制重载与普通脏资源更新互斥：强制路径等待设备空闲并重建全部相关资源后
/// 返回；普通路径只处理各视图自己的 need 标志。项目过渡期间跳过动态视图。
/// @warning 热路径：每帧渲染准备阶段执行；重建和纹理重载只能由低频脏位触发。
/// 皮肤热切换分支会调用 waitIdle，只能由设置页切换皮肤触发。
void UIManager::onPrepareResources(vk::PhysicalDevice&   physicalDevice,
                                   vk::Device&           logicalDevice,
                                   Graphic::VKSwapchain& swapchain,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
    // exchange 消费一次皮肤重载请求。
    bool forceSkinResourceReload =
        std::exchange(m_skinResourceReloadRequested, false);
    if ( forceSkinResourceReload && isProjectTransitionInProgress() ) {
        /// 项目动态视图在切换失败后可能继续存活，强制皮肤重载必须延迟到
        /// 切换结束后统一执行，不能在占位期间丢弃这次请求。
        m_skinResourceReloadRequested = true;
        forceSkinResourceReload       = false;
    }

    if ( forceSkinResourceReload ) {
        // 强制资源替换前确保 GPU 不再引用旧纹理与 framebuffer。
        (void)logicalDevice.waitIdle();
        // 先刷新渲染器共享皮肤纹理。
        if ( auto context = Graphic::VKContext::get() ) {
            context->get().getRenderer().reloadSkinTextures();
        }

        for ( const auto& name : m_renderableUiSequence ) {
            // 项目动态视图在过渡中保持旧资源直到生命周期稳定。
            if ( isProjectTransitionInProgress() &&
                 isProjectWorkspaceDynamicView(name) ) {
                continue;
            }
            auto renderableView = m_uiviews[name]->asRenderableView();
            if ( renderableView ) {
                // 失效 shader 缓存后立即重建离屏目标。
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
                // 强制调用 reload；needReload 查询用于让实现更新内部状态契约。
                textureLoader->reloadTextures(
                    physicalDevice, logicalDevice, cmdPool, queue);
                (void)textureLoader->needReload();
            }
        }
        // 强制路径已覆盖普通脏检查，本帧直接结束。
        return;
    }

    // 普通路径只重建明确标记的离屏帧缓冲。
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

    // 普通路径只调用报告需要重载的纹理能力视图。
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

/// @brief 驱动一帧全局 UI 生命周期、数据准备和视图更新。
///
/// 每帧先推进不抢焦点的保存进度气泡，再依次消费项目事件、同步工作区、清理关闭
/// 视图、派发输入和剪贴板，并为实现
/// IParallelUiPreparable
/// 的视图准备纯数据缓存。声明主线程要求的字体测量串行执行，
/// 其余多个任务可使用应用线程池并以 latch 在视图更新前汇合。
///
/// 视图按本帧开始时的注册数量更新；update
/// 中新注册的视图延迟到下一帧，避免序列扩容 使当前遍历失效。欢迎页也等待
/// CanvasTabManager 真正初始化画布后再创建。
/// @warning 热路径：每帧 ImGui 更新阶段执行；禁止在此加入文件系统扫描、完整 ECS
/// 遍历或完整排序。
void UIManager::onUpdateUI()
{
    // 控件矩形属于单帧即时布局，任何视图开始提交前先清除旧坐标。
    m_walkthroughSpotlight->beginFrame();
    // 引导服务先推进页面与动作状态。
    m_walkthrough->update();
    // 拖放命令只入队到逻辑线程，不因后台保存进度阻塞当前 ImGui 帧。
    m_projectDropRouter->update(true);
    if ( auto* dock = getView<MainDockSpaceUI>("MainDockSpaceUI") ) {
        // 进度与结果共用非交互气泡；绘制不会切换 ImGui 导航或 Dock 标签。
        dock->updateSaveFeedback();
    }
    if ( m_editorApplicationService ) {
        // FPS 只发布当前 ImGui 平滑帧率值，不做额外统计。
        m_editorApplicationService->publishRenderFps(ImGui::GetIO().Framerate);
    }
    // 最小化时禁用反馈；可见窗口恢复正常全局鼠标反馈处理。
    SetInteractionFeedbackEnabled(isInteractionFeedbackAllowed(m_nativeWindow));
    ProcessGlobalMouseFeedback();

    consumePendingProjectLifecycleUpdates();
    syncProjectWorkspaceState();

    // 先收集关闭视图名称，避免遍历注册表时直接 erase。
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
            // GPU 能力视图销毁前确保资源不再被飞行命令使用。
            waitForGpuBeforeDestroyView(*viewIt->second);
            m_uiviews.erase(viewIt);
        }
        std::erase(m_uiSequence, name);
        // 同时从纹理加载器和可渲染序列中移除。
        std::erase(m_renderableUiSequence, name);
        std::erase(m_textureLoaderSequence, name);
    }

    // 全局 ImGui 输入事件每帧只转换和发布一次。
    DispatchGlobalUIEvents();
    ClipboardBridge::publishPendingEditorClipboard();

    // 预先准备视图数据；字体测量留在主线程，纯数据任务才进入线程池。
    // 四个容器复用容量，分别保存候选、总需准备、主线程与并行分组。
    m_uiPrepareCandidates.clear();
    m_uiPrepareViews.clear();
    m_mainThreadUiPrepareViews.clear();
    m_parallelUiPrepareViews.clear();
    m_uiPrepareCandidates.reserve(m_uiSequence.size());
    for ( const auto& name : m_uiSequence ) {
        // 过渡中跳过侧栏管理器和所有项目动态视图。
        if ( isProjectTransitionInProgress() &&
             (name == "SideBarManager" ||
              isProjectWorkspaceDynamicView(name)) ) {
            continue;
        }
        // 序列与 map 若暂时不同步则跳过陈旧名称。
        auto it = m_uiviews.find(name);
        if ( it == m_uiviews.end() ) {
            continue;
        }

        // 能力接口避免 UIManager 知道具体视图类型。
        IParallelUiPreparable* preparable =
            it->second->asParallelUiPreparable();
        if ( preparable ) {
            m_uiPrepareCandidates.push_back(preparable);
        }
    }

    if ( !m_uiPrepareCandidates.empty() ) {
        // 一份不可变快照共享给本帧全部准备任务。
        const UiFrameSnapshot snapshot = captureUiFrameSnapshot();
        m_uiPrepareViews.reserve(m_uiPrepareCandidates.size());
        m_mainThreadUiPrepareViews.reserve(m_uiPrepareCandidates.size());
        m_parallelUiPrepareViews.reserve(m_uiPrepareCandidates.size());
        for ( IParallelUiPreparable* preparable : m_uiPrepareCandidates ) {
            // 只把缓存未命中的视图加入实际工作集合。
            if ( preparable->needsParallelUiPrepare(snapshot) ) {
                m_uiPrepareViews.push_back(preparable);
                if ( preparable->requiresMainThreadUiPrepare() ) {
                    // 可能触发字体 Atlas 写入的测量必须串行。
                    m_mainThreadUiPrepareViews.push_back(preparable);
                } else {
                    // 明确只处理纯数据的视图允许并发。
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
            // 只有多个纯数据任务时才值得进入线程池。
            std::latch prepareLatch(
                static_cast<std::ptrdiff_t>(m_parallelUiPrepareViews.size()));
            for ( IParallelUiPreparable* preparable :
                  m_parallelUiPrepareViews ) {
                appThreadPool->enqueue_void(
                    [preparable, &snapshot, &prepareLatch]() {
                        // snapshot 与 latch 生命周期延续到下方 wait 返回。
                        preparable->prepareUiFrameData(snapshot);
                        prepareLatch.count_down();
                    });
            }
            // UI 线程等待准备结束后才允许视图消费结果；等待范围仅纯 CPU
            // 短任务。
            prepareLatch.wait();
        } else {
            // 无线程池或只有一个任务时直接串行，避免调度开销。
            for ( IParallelUiPreparable* preparable :
                  m_parallelUiPrepareViews ) {
                preparable->prepareUiFrameData(snapshot);
            }
        }
        // 所有准备完成后按同一总集合顺序切换缓存。
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
            // 先前视图更新可能注销后续名称，查找失败时跳过。
            continue;
        }

        // 视图内部提交 ImGui 控件及必要画笔数据。
        it->second->update(this);
    }

    // 主画布由 CanvasTabManager 在 update 中注册，欢迎页必须排在它之后。
    // 逻辑会话可能迟到，保留打开请求直到画布确实存在，不靠固定延时猜测就绪。
    if ( m_openWelcome ) {
        // CanvasTabManager 是判断编辑画布是否完成初始化的具体宿主。
        const auto* tabs = getView<CanvasTabManager>("CanvasTabManager");
        if ( !m_canvasWorkspaceService ||
             (tabs && tabs->hasInitializedCanvas()) ) {
            if ( !getView<MarkdownImageCache>("WalkthroughImages") )
                // 欢迎页 Markdown 图片缓存按稳定键惰性注册。
                registerView("WalkthroughImages",
                             std::make_unique<MarkdownImageCache>());
            if ( auto* view = getView<WelcomeView>("Welcome") )
                // 已存在欢迎页时切回主页。
                view->showHome();
            else
                // 不存在时创建，首次 update 在下一帧发生。
                registerView("Welcome", std::make_unique<WelcomeView>());
            // 新注册视图下一帧才更新，编辑器先创建窗口，欢迎页随后请求选中。
            m_openWelcome = false;
        }
    }

    if ( auto* sideBarManager = getView<FloatingManagerUI>("SideBarManager") ) {
        // DockSpace 完成后恢复侧栏 resize 鼠标状态。
        sideBarManager->restoreDockResizeMouseAfterDockSpace();
    }

    // 所有控件完成目标上报后再绘制突出层，亮区使用本帧最终布局坐标。
    m_walkthroughSpotlight->render(
        Config::AppConfig::instance().getWindowContentScale(),
        TR("ui.walkthrough.spotlight_acknowledge").data());

    // 帧末跟踪根窗口焦点并同步平台标题栏命中区域。
    trackImGuiFocusForAutoSave();
    syncNativeWindowDragAreas();
}

/// @brief 检测 ImGui 根窗口焦点转移并提交低频自动保存事件。
///
/// 只记录导航窗口的 RootWindow ID，使同一停靠树内子窗口切换不会被误判成离开
/// 编辑区域。首次调用仅建立基线；旧根窗口非零且 ID 发生变化时才请求自动保存。
/// @warning 热路径：每帧末尾调用，只允许读取 ImGui 状态和发布轻量保存请求。
void UIManager::trackImGuiFocusForAutoSave()
{
    // 零值表示当前没有可跟踪的 ImGui 导航窗口。
    ImGuiID focusedRootId = 0;
    if ( ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow ) {
        // 使用根窗口消除 Dock 节点内部焦点切换产生的噪声。
        ImGuiWindow* root = focused->RootWindow;
        if ( root ) focusedRootId = root->ID;
    }

    if ( !m_imGuiFocusTrackingInitialized ) {
        // 初始化帧不代表发生焦点丢失，只保存当前状态。
        m_lastFocusedImGuiRootId        = focusedRootId;
        m_imGuiFocusTrackingInitialized = true;
        return;
    }
    if ( m_lastFocusedImGuiRootId != 0 &&
         focusedRootId != m_lastFocusedImGuiRootId ) {
        if ( m_editorApplicationService ) {
            // 服务自行合并和落盘，渲染线程只提交原因标记。
            m_editorApplicationService->requestAutoSave(
                EditorAutoSaveReason::ImGuiWindowFocusLost);
        }
    }
    // 无论是否触发保存，都推进下一帧比较基线。
    m_lastFocusedImGuiRootId = focusedRootId;
}

/// @brief 串行录制当前帧全部离屏渲染任务。
/// @param cmd 本批任务共用的主命令缓冲。
/// @param frameIndex 当前并发帧索引。
///
/// 该入口保留给单命令缓冲调用方；并行渲染器可通过任务数量和单任务入口自行拆分。
/// @warning
/// 热路径：每帧命令录制阶段执行；只允许遍历可渲染视图序列并委托录制命令。
void UIManager::onRecordOffscreen(vk::CommandBuffer& cmd, uint32_t frameIndex)
{
    // 在循环前固定数量，要求录制期间不修改可渲染视图序列。
    const uint32_t taskCount = getOffscreenRecordTaskCount();
    for ( uint32_t taskIndex = 0; taskIndex < taskCount; ++taskIndex ) {
        // 单任务入口统一处理边界、过渡状态和能力检查。
        onRecordOffscreenTask(cmd, frameIndex, taskIndex);
    }
}

/// @brief 获取当前帧可并行录制的离屏视图数量。
/// @return 当前可渲染视图序列的数量。
///
/// 返回值只在本轮任务枚举期间有效；注册或注销视图后调用方必须重新查询。
/// 数量使用 uint32_t 与渲染任务调度接口保持一致。
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
    // 防御调用方持有的旧任务数量，避免序列缩短后越界。
    if ( taskIndex >= m_renderableUiSequence.size() ) {
        return;
    }

    const auto& name = m_renderableUiSequence[taskIndex];
    // 会话过渡中动态视图可能仍引用即将释放的项目资源。
    if ( isProjectTransitionInProgress() &&
         isProjectWorkspaceDynamicView(name) ) {
        return;
    }
    const auto& views = m_uiviews;
    auto        it    = views.find(name);
    // 名称序列与注册表短暂不同步时安全跳过该任务。
    if ( it == views.end() ) {
        return;
    }

    // renderable 标记仍需通过能力接口取得实际录制对象。
    auto renderableView = it->second->asRenderableView();
    if ( renderableView && renderableView->shouldRecordOffscreen() ) {
        // 每个任务只录制自己的离屏目标，不提交队列或等待 GPU。
        renderableView->recordCmds(cmd, frameIndex);
    }
}

/// @brief 将本帧 ImGui 捕获状态转换为项目统一输入事件。
///
/// 鼠标事件归属悬停窗口，键盘事件归属导航焦点窗口。函数只发布 ImGui 已捕获或
/// 明确应交给编辑器的输入，防止原始平台事件被各视图重复转换。
///
/// 鼠标移动保留屏幕坐标与本帧增量，滚轮把水平和垂直分量合并为单个事件；按钮
/// 事件携带翻译后的按钮、修饰键和当前位置。键盘扫描只覆盖 ImGui NamedKey，
/// 按下边沿允许重复以支持连续操作，释放边沿不解析 Unicode codepoint。
/// @warning 热路径：每帧调用一次；NamedKey 扫描为固定范围，不得加入动态排序、
/// 文件访问或阻塞操作。
void UIManager::DispatchGlobalUIEvents()
{
    // ImGuiIO 是本帧捕获意图和输入边沿的唯一事实来源。
    ImGuiIO& io = ImGui::GetIO();

    // 分别记录焦点和悬停窗口，避免键盘、鼠标事件被错误归到同一来源。
    std::string focusedWindowName = "";
    std::string hoveredWindowName = "";

    if ( ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow ) {
        // Name 包含 ImGui 稳定标识，可供订阅者识别来源视图。
        focusedWindowName = focused->Name;
    }
    if ( ImGuiWindow* hovered = ImGui::GetCurrentContext()->HoveredWindow ) {
        hoveredWindowName = hovered->Name;
    }

    // 只有 ImGui 请求捕获鼠标时才发布带窗口来源的项目事件。
    if ( io.WantCaptureMouse ) {
        // 静止帧不制造零位移事件，减少事件总线噪声。
        if ( io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ) {
            // 事件对象只在栈上构造，由 EventBus 在 publish 调用内同步分发。
            Event::UIWindowMouseMoveEvent e;
            e.uiManager = this;
            // 鼠标事件始终归属当前悬停窗口。
            e.sourceUiName = hoveredWindowName;
            e.pos          = { io.MousePos.x, io.MousePos.y };
            e.delta        = { io.MouseDelta.x, io.MouseDelta.y };
            Event::EventBus::instance().publish(e);
        }

        // 水平或垂直任一滚轮非零时才发布一次二维滚动事件。
        if ( io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ) {
            // 保留 ImGui 的水平在前、垂直在后约定映射到二维 offset。
            Event::UIWindowMouseScrollEvent e;
            e.uiManager    = this;
            e.sourceUiName = hoveredWindowName;
            e.pos          = { io.MousePos.x, io.MousePos.y };
            e.offset       = { io.MouseWheelH, io.MouseWheel };
            Event::EventBus::instance().publish(e);
        }

        // ImGui 当前支持的五个鼠标按钮共享同一种项目事件。
        for ( int i = 0; i < 5; ++i ) {
            bool pressed  = ImGui::IsMouseClicked(i);
            bool released = ImGui::IsMouseReleased(i);
            if ( pressed || released ) {
                // 未发生边沿的按钮不会产生持续按住事件。
                Event::UIWindowMouseButtonEvent e;
                e.uiManager    = this;
                e.sourceUiName = hoveredWindowName;
                e.button       = Event::Translator::ImGui::GetMouseButton(i);
                // 同帧同时出现时按按下边沿优先，保持旧行为确定性。
                e.action = pressed ? Event::Input::Action::Press
                                   : Event::Input::Action::Release;
                e.mods   = Event::Translator::ImGui::GetMods();
                e.pos    = { io.MousePos.x, io.MousePos.y };
                Event::EventBus::instance().publish(e);
            }
        }
    }

    // 文本输入关闭但仍有焦点窗口时，继续派发编辑器快捷键。
    const bool hasFocusedWindow = !focusedWindowName.empty();
    // WantTextInput 时仍由 ImGui 捕获键盘；非文本焦点也保留编辑器快捷键。
    const bool shouldDispatchKeyboard =
        io.WantCaptureKeyboard || (hasFocusedWindow && !io.WantTextInput);
    if ( shouldDispatchKeyboard ) {
        // NamedKey 是固定枚举区间，覆盖导航键、功能键与常见键盘按键。
        for ( int i = ImGuiKey_NamedKey_BEGIN; i < ImGuiKey_NamedKey_END;
              ++i ) {
            ImGuiKey key = static_cast<ImGuiKey>(i);

            // true 允许 ImGui 根据其重复节奏产生持续按键边沿。
            bool pressed  = ImGui::IsKeyPressed(key, true);
            bool released = ImGui::IsKeyReleased(key);

            if ( pressed || released ) {
                Event::UIWindowKeyPressEvent e;
                e.uiManager = this;
                // 键盘事件归属导航焦点窗口，而不是鼠标悬停窗口。
                e.sourceUiName = focusedWindowName;

                e.key = Event::Translator::ImGui::GetKey(key);
                // 同一帧两种边沿并存时沿用按下优先的确定规则。
                e.action   = pressed ? Event::Input::Action::Press
                                     : Event::Input::Action::Release;
                e.mods     = Event::Translator::ImGui::GetMods();
                e.scancode = 0;

                if ( e.action != Event::Input::Action::Release ) {
                    // 仅按下和重复事件解析字符，释放事件没有字符语义。
                    // Translator
                    // 结合修饰键解析布局相关字符，物理扫描码保持未提供。
                    e.codepoint =
                        Event::Translator::ResolveCodepoint(e.key, e.mods);
                } else {
                    // 零值明确表示该事件不携带文本输入。
                    e.codepoint = 0;
                }

                // 所有视图通过同一事件总线接收，避免直接互相调用。
                Event::EventBus::instance().publish(e);
            }
        }
    }
}

/// @brief 在销毁可能持有 Vulkan 资源的视图前等待 GPU 完成在途命令。
/// @param view 即将移除所有权的视图。
///
/// 普通纯 ImGui 视图可立即销毁；离屏渲染或纹理加载视图可能仍被提交中的命令缓冲
/// 引用，因此必须取得全局 Vulkan 上下文并等待设备空闲。
/// @warning 不可中断操作：可能调用
/// vkDeviceWaitIdle；只能在视图销毁低频路径执行。
void UIManager::waitForGpuBeforeDestroyView(IUIView& view)
{
    /// @brief 目标视图是否持有可能被命令缓冲引用的 Vulkan 资源。
    bool ownsGpuResources = view.renderable() || view.asTextureLoader();
    if ( !ownsGpuResources ) {
        // 没有 GPU 能力的视图只持有 CPU/ImGui 状态。
        return;
    }

    /// @brief 当前 Vulkan 上下文查询结果。
    auto contextResult = Graphic::VKContext::get();
    if ( !contextResult ) {
        // 上下文已拆除时无法等待；记录原因并允许关闭流程继续。
        XWARN("UIManager: skip GPU idle wait before destroying [{}]: {}",
              view.m_name,
              contextResult.error());
        return;
    }

    // 低频销毁边界统一等待，防止纹理或 framebuffer 提前释放。
    (void)contextResult->get().getLogicalDevice().waitIdle();
}

}  // namespace MMM::UI
