#include "common/EditTool.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/ui/GLFWNativeEvent.h"
#include "event/ui/UISettingsTabEvent.h"
#include "ui/IEditorApplicationService.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include "ui/workspace/MobileButton.h"
#include "ui/workspace/MobileTouchScroll.h"
#include "ui/workspace/MobileWorkspaceModel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <imgui_internal.h>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 移动端独立组合根，业务窗口只提供内容，不参与桌面停靠规则。
class MobileWorkspacePresentation final : public IWorkspacePresentation
{
public:
    /// @brief 平台返回通知在 SDL/UI 线程消费，不从 Java 线程修改 ImGui。
    MobileWorkspacePresentation()
    {
        // 订阅回调只在原生事件泵消费时执行，不在 Java 回调线程访问 UI。
        // 返回请求合并为单个布尔意图，连续帧不会积压多个确认窗口。
        m_backSubscription =
            Event::EventBus::instance().subscribe<Event::GLFWNativeEvent>(
                [this](const Event::GLFWNativeEvent& event) {
                    if ( event.type ==
                         Event::NativeEventType::MOBILE_BACK_REQUEST )
                        m_backRequested = true;
                });
    }
    /// @brief 先解除观察回调，再销毁 UI 线程独占的手势状态。
    ~MobileWorkspacePresentation() override
    {
        Event::EventBus::instance().unsubscribe<Event::GLFWNativeEvent>(
            m_backSubscription);
    }
    /// @brief 布局和手势先于原始画布输入，拖动过程中不使用定时阻塞。
    void beginFrame(UIManager& manager) override;
    /// @brief 只创建移动主画布宿主，不绘制桌面菜单栏和状态栏。
    void renderWorkspace(MainDockSpaceUI&, UIManager& manager) override;
    /// @brief 管理器由抽屉组合，窗口开关由移动端入口管理。
    bool updatesView(std::string_view name) const override;
    /// @brief 业务绘制完成后叠加把手、遮罩、抽屉和专用退出确认。
    void endFrame(UIManager& manager) override;
    /// @brief 集中调整公共窗口入口，不在每个业务窗口插入平台宏。
    bool beginWindow(std::string_view id, const std::string& title, bool* open,
                     ImGuiWindowFlags flags) override;
    /// @brief 抽屉与关闭手势释放保护统一阻挡画布原始输入。
    bool blocksCanvasInput() const override
    {
        return m_model.blocksCanvasInput() || m_touchScroll.ownsPointer();
    }
    /// @brief 手指没有悬浮状态，外壳采用按下与选中反馈。
    bool allowsHoverFeedback() const override { return false; }
    /// @brief 手机旋转和抽屉不消费桌面停靠树，也不覆盖其持久化内容。
    bool usesDesktopLayout() const override { return false; }
    /// @brief 移动端不显示桌面输出设备组合框，继续使用现有音量控制。
    bool showsAudioOutputDeviceSelector() const override { return false; }
    /// @brief 文件对话框始终保留设备工作区边距。
    ImVec2 fileDialogSize(float scale, ImVec2 available) const override
    {
        return { std::min(600.0f * scale, available.x * 0.92f),
                 std::min(400.0f * scale, available.y * 0.88f) };
    }

private:
    /// @brief 固定位置的移动外壳窗口，不写入桌面 imgui.ini。
    bool beginPanel(const char* id, MobileRect rect, bool background = true);
    /// @brief 绘制最小触摸区域与细握柄，点击和拖动共用状态机。
    void drawHandle(const char* id, MobileRect rect, bool opened = false);
    /// @brief 左侧第一级提供常用分类，第二级保留现有业务管理器。
    void drawLeftDrawer(UIManager& manager, MobileRect rect);
    /// @brief 右侧以明确的选中状态提供三个窗口开关。
    void drawRightDrawer(UIManager& manager, MobileRect rect);
    /// @brief 独立的触摸工具条，工具命令复用原业务入口。
    void drawToolbar(UIManager& manager);
    /// @brief 退出确认是独立移动端窗口，不复用桌面菜单退出弹窗。
    void drawExitConfirmation();

    /// @brief 纯手势状态只属于 UI 线程，关闭抽屉不影响文件选择状态。
    MobileWorkspaceModel m_model;
    /// @brief 全局空白拖动滚动，只接管触屏，业务画布仍保留自己的绘制手势。
    MobileTouchScroll m_touchScroll;
    /// @brief 下一帧收到的原生返回请求，由 SDL 事件泵写入。
    bool m_backRequested{}, m_initialized{}, m_drawingDrawer{};
    /// @brief 窗口开关属于移动外壳，初次启动全部收起。
    bool m_showToolbar{}, m_showTiming{}, m_showPreview{};
    /// @brief 当前帧几何不跨线程共享，旋转后立即重新计算。
    ImVec2 m_origin{}, m_size{};
    float  m_scale{ 1 }, m_padding{}, m_gap{}, m_toolbarWidth{};
    /// @brief 中央宿主接收画布、欢迎和独立设置标签，不接收资源管理器停靠。
    ImGuiID m_centerDock{};
    /// @brief 绘制管理器时的固定内容矩形，退出作用域后不再使用。
    MobileRect m_contentRect;
    /// @brief 返回事件订阅由组合根生命周期管理。
    Event::SubscriptionID m_backSubscription{};
};

/// @brief 在业务视图读取输入之前建立本帧移动工作区快照。
/// @details 只读取显示区域和应用配置，不查询项目会话或文件系统。
/// 显示开关与当前几何分开保存，旋转不会重新创建文件管理器。
/// 字号使用绝对倍率写入，不在皮肤的上一帧缩放结果上继续累乘。
/// @warning UI 每帧执行；只有固定规模数学、局部状态与输入采样。
void MobileWorkspacePresentation::beginFrame(UIManager& manager)
{
    // 工作区使用平台已经扣除安全区后的矩形，不硬编码状态栏高度。
    const auto* viewport = ImGui::GetMainViewport();
    m_origin             = viewport->WorkPos;
    m_size               = viewport->WorkSize;
    auto& config         = Config::AppConfig::instance();
    auto& settings       = config.getEditorSettings();
    m_scale              = config.getWindowContentScale();
    // 留白来自用户审美配置，按钮增大时不会挤占这部分边界。
    // 间距设置下限避免高密度屏幕上的相邻命中区视觉粘连。
    m_padding = std::max(0.0f, settings.aesthetics.windowPadding * m_scale);
    m_gap = std::max(2.0f * m_scale, settings.aesthetics.windowGap * m_scale);
    // 字号在平台外壳中统一提高，保留用户二次缩放，不逐帧累乘。
    ImGui::GetStyle().FontScaleMain =
        std::clamp(settings.fontSizeMultiplier * 1.25f, 1.0f, 2.5f);
    // 首次进入移动外壳只保留两个中心页签，辅助窗口默认收起。
    // 这不是改写用户桌面布局，平台工厂已经在更早的阶段分离。
    if ( !m_initialized ) {
        m_showTiming  = false;
        m_showPreview = false;
        m_initialized = true;
    }
    // 共享窗口仍通过原有显示字段判断是否准备内容。
    // 移动状态只发布最新值，不为未变化的开关执行保存。
    settings.showTimelineWindow = m_showTiming;
    settings.showPreviewWindow  = m_showPreview;
    m_toolbarWidth = m_showToolbar ? 64.0f * m_scale + 2.0f * m_padding : 0.0f;
    m_model.resize(m_size.x, m_size.y, m_scale);
    // 原生返回可能来自业务弹窗，不能越过它额外弹出退出确认。
    // 本外壳自己的退出确认仍允许再次返回取消，保持返回层级一致。
    const bool modalOpen = ImGui::GetTopMostPopupModal() != nullptr;
    const bool popupOpen = ImGui::IsPopupOpen(
        nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if ( std::exchange(m_backRequested, false) ) {
        // 普通弹层可以越过抽屉边界；返回先关闭最上层弹层，不连带关闭父抽屉。
        if ( popupOpen && !modalOpen )
            ImGui::ClosePopupToLevel(
                ImGui::GetCurrentContext()->OpenPopupStack.Size - 1, true);
        else if ( !modalOpen || m_model.exitConfirmation() ) {
            const auto* window = ImGui::FindWindowByName("###SettingsWindow");
            // 返回先离开当前设置标签，不把关闭设置当作退出整个软件。
            if ( !modalOpen && m_model.drawer() == MobileDrawer::None &&
                 window && window->WasActive && window->DockTabIsVisible ) {
                if ( auto* settings =
                         manager.getView<IUIView>("SettingsWindow") )
                    settings->setOpen(false);
                // 视图即将移除，结束可能仍在进行的快捷键录制状态。
                ShortcutUtils::setShortcutRecordingActive(false);
            } else
                m_model.requestBack();
        }
    }
    // 同一帧采样一份触摸坐标，避免两个窗口分别建立不同手势起点。
    // 高层模态独占输入时，模型保持状态而不是抢占其点击。
    const auto& io = ImGui::GetIO();
    // 更高层模态窗口保留输入所有权，不用抽屉抢走文件选择或业务确认。
    m_model.update({ io.MousePos.x - m_origin.x,
                     io.MousePos.y - m_origin.y,
                     ImGui::IsMouseClicked(0),
                     ImGui::IsMouseDown(0),
                     ImGui::IsMouseReleased(0) },
                   io.DeltaTime,
                   popupOpen);
    m_touchScroll.beginFrame(m_scale);
    // 仅预采样触点；控件提交后才确认空白，不抢先吞掉按钮按下。
}

/// @brief 从公共更新序列中移出由抽屉承载的管理器窗口。
/// @param name UIManager 注册时的稳定名称，不使用本地化标题匹配。
/// @details 返回 false 不代表关闭视图，也不会释放纹理或重置选择。
/// 项目向导等独立业务模态继续走公共流程，不能因移动布局而丢失。
/// @warning 每帧固定数量字符串比较，不扫描视图容器。
bool MobileWorkspacePresentation::updatesView(std::string_view name) const
{
    // 新增业务面板默认仍走公共循环，只有明确由外壳接管的类型才跳过。
    // 视图仍归 UIManager 持有；隐藏抽屉只暂停其 UI 提交，不销毁业务状态。
    return name != "SideBarUI" && name != "SideBarManager" &&
           name != "ToolPluginView";
}

/// @brief 开始移动外壳自管的非停靠面板，调用方始终配对 End。
/// @param rect 工作区相对矩形，可因抽屉动画而部分位于屏幕外。
/// @param background 透明握柄不绘制普通窗口底色。
/// @details 显示层级与导航焦点分离，避免每帧打断输入框编辑。
/// @warning 不调用平台窗口 API，只提交当前 ImGui 视口内的窗口。
bool MobileWorkspacePresentation::beginPanel(const char* id, MobileRect rect,
                                             bool background)
{
    // 固定内部名字隔离各抽屉状态，可见文案变化不会创建新的窗口身份。
    // 先把相对坐标转换为当前视口坐标，支持系统安全区发生变化。
    // 面板尺寸始终保持非负，狭窄屏幕与动画起点都不能生成无效窗口。
    ImGui::SetNextWindowPos({ m_origin.x + rect.m_x, m_origin.y + rect.m_y });
    ImGui::SetNextWindowSize(
        { std::max(1.0f, rect.m_width), std::max(1.0f, rect.m_height) });
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::SetNextWindowDockID(0);
    // 移动面板不是可拖拽的桌面浮窗，不允许被用户停靠到画布标签中。
    // 位置与尺寸由外壳管理，因此不写入通用 imgui.ini。
    auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
    if ( !background ) flags |= ImGuiWindowFlags_NoBackground;
    // Begin 即使不可见也必须由调用方 End，保留 ImGui 栈平衡。
    // 普通弹出菜单已经打开时不重排窗口，避免遮罩盖住其菜单项。
    const bool visible = ImGui::Begin(id, nullptr, flags);
    // 只调整显示层级，不重新聚焦，输入框和下拉菜单继续保留自己的焦点。
    if ( !ImGui::IsPopupOpen(
             nullptr,
             ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) )
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    return visible;
}

/// @brief 建立容纳画布、欢迎与独立设置标签的中央宿主。
/// @param manager 提供既有工具服务，外壳不持有编辑会话所有权。
/// @details 移动宿主拥有独立 Dock ID，桌面 ini 中的侧栏分割不参与恢复。
/// 时间线、预览和触摸工具条使用自己的边缘区域，不占中央页签。
/// 窗口显隐只改变本帧区域，不销毁已经建立的编辑会话。
/// @warning 每帧只操作固定节点，不遍历谱面或等待逻辑线程。
void MobileWorkspacePresentation::renderWorkspace(MainDockSpaceUI&,
                                                  UIManager& manager)
{
    auto& style = ImGui::GetStyle();
    // 只调整本外壳的基础密度，小圆角比胶囊形更容易辨认相邻触摸目标。
    style.FrameRounding = 3.0f * m_scale;
    style.TabRounding   = 3.0f * m_scale;
    style.ScrollbarSize = std::max(style.ScrollbarSize, 18.0f * m_scale);
    // 标签页也是触摸入口，不能只放大抽屉按钮而留下细窄的页签。
    // 用实际字号反推内边距，使标签和标准输入框至少接近 48dp。
    style.FramePadding = {
        12.0f * m_scale,
        std::max(4.0f * m_scale, (48.0f * m_scale - ImGui::GetFontSize()) * .5f)
    };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{});
    // 辅助窗口从同一份可用宽度中扣除，不让画布和工具条重复预留空间。
    // 比例上限使较窄设备仍保留可以辨认的中央编辑区。
    const float timingWidth =
        m_showTiming ? std::min(280.0f * m_scale, m_size.x * 0.28f) : 0.0f;
    const float previewWidth =
        m_showPreview ? std::min(200.0f * m_scale, m_size.x * 0.2f) : 0.0f;
    MobileRect rect{ m_gap,
                     m_gap,
                     std::max(1.0f,
                              m_size.x - 2 * m_gap - m_toolbarWidth -
                                  timingWidth - previewWidth),
                     std::max(1.0f, m_size.y - 2 * m_gap) };
    ImGui::SetNextWindowPos({ m_origin.x + rect.m_x, m_origin.y + rect.m_y });
    ImGui::SetNextWindowSize({ rect.m_width, rect.m_height });
    // 这个窗口只承载 DockSpace，不绘制菜单、状态信息或业务按钮。
    // 固定宿主不能因背景点击跑到抽屉上面。
    ImGui::Begin("MobileCanvasHost",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    m_centerDock = ImGui::GetID("MobileMainCanvasDock");
    // 页签仍可切换，但不提供分割、拖出、窗口菜单或关闭入口。
    // 单独的移动 Dock 身份保证桌面分栏状态不会恢复到当前工作区。
    const auto flags = ImGuiDockNodeFlags_NoDockingSplit |
                       ImGuiDockNodeFlags_NoUndocking |
                       ImGuiDockNodeFlags_NoWindowMenuButton |
                       ImGuiDockNodeFlags_NoCloseButton;
    // 节点仅在缺失时创建，不能为了旋转或打开抽屉逐帧重建标签。
    // 已创建画布继续保留相同 ImGui 身份与业务对象。
    if ( !ImGui::DockBuilderGetNode(m_centerDock) ) {
        ImGui::DockBuilderAddNode(m_centerDock,
                                  ImGuiDockNodeFlags_DockSpace | flags);
        ImGui::DockBuilderSetNodeSize(m_centerDock,
                                      { rect.m_width, rect.m_height });
        ImGui::DockBuilderFinish(m_centerDock);
    }
    // 中央节点与桌面 ini 不共用身份；历史分栏不会污染移动端启动布局。
    MainDockSpaceUI::setCenterDockId(m_centerDock);
    MainDockSpaceUI::setToolDockId(0);
    ImGui::DockSpace(m_centerDock, {}, flags);
    ImGui::End();
    ImGui::PopStyleVar();
    if ( m_showToolbar ) drawToolbar(manager);
}

/// @brief 将共享业务窗口接入移动外壳的当前绘制作用域。
/// @param id 视图稳定身份，负责区分画布、欢迎页和边缘编辑窗口。
/// @param title 保留业务提供的标题及内部 ID，不据可见文字推断窗口类型。
/// @details 关闭指针由移动外壳统一移除，显示开关不再依赖标签 X。
/// 抽屉内容与中央画布共用业务对象，但采用不同的几何和输入归属。
/// @warning 仅在 UI 窗口 Begin 前执行，禁止从并行准备任务调用。
bool MobileWorkspacePresentation::beginWindow(std::string_view   id,
                                              const std::string& title, bool*,
                                              ImGuiWindowFlags   flags)
{
    // 所有普通窗口不再传入关闭指针；显示与收起只能通过移动端入口改变。
    flags |=
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    // 分类使用注册名，不依赖皮肤语言或当前谱面名称。
    // 画布改显示标题仍保留 ### 后的 ID，切换项目不会建立另一套窗口状态。
    const bool mainCanvas = id.starts_with("Canvas_") || id == "Basic2DCanvas";
    // 中央宿主只接收主画布和欢迎页，其他管理器不占用这里的标签空间。
    // 抽屉打开时窗口输入与原始画布命令必须同时阻挡。
    // 设置构造名随翻译变化，窗口标题的 ### 后缀才是稳定身份。
    const bool settingsWindow = title.ends_with("###SettingsWindow");
    if ( mainCanvas || id == "Welcome" || settingsWindow ) {
        // 使用整个中心区域，不把桌面最小宽度约束带入移动端标签。
        if ( settingsWindow )
            ImGui::SetNextWindowSizeConstraints({ 1, 1 }, m_size);
        ImGui::SetNextWindowDockID(m_centerDock, ImGuiCond_Always);
        flags |= ImGuiWindowFlags_NoMove;
        if ( m_model.blocksCanvasInput() ) flags |= ImGuiWindowFlags_NoInputs;
        if ( mainCanvas ) {
            const std::string name = "主画布###" + std::string(id);
            return ImGui::Begin(name.c_str(), nullptr, flags);
        }
        // 内容层沿用原有配置与数据，外壳只替换显示位置和关闭方式。
        // 这个标记只在同步绘制作用域有效，不能传给后台数据准备任务。
    } else if ( m_drawingDrawer ) {
        // 同一内容窗口复用原本的视图对象，只有移动外壳决定其位置和边界。
        ImGui::SetNextWindowDockID(0);
        ImGui::SetNextWindowPos(
            { m_origin.x + m_contentRect.m_x, m_origin.y + m_contentRect.m_y });
        ImGui::SetNextWindowSize({ std::max(1.0f, m_contentRect.m_width),
                                   std::max(1.0f, m_contentRect.m_height) });
        // 桌面视图可能预先写入较大的最小宽度，抽屉必须覆盖该约束。
        // 固定内容矩形同时限制上下界，防止设置面板越过关闭遮罩。
        const ImVec2 contentSize{ std::max(1.0f, m_contentRect.m_width),
                                  std::max(1.0f, m_contentRect.m_height) };
        ImGui::SetNextWindowSizeConstraints(contentSize, contentSize);
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoSavedSettings;
        // 时间线和预览各自保留独立区域，显示开关不会创建重复编辑器。
        // 其宽度计算与中央宿主扣除值一致，避免窗口相互覆盖。
    } else if ( id == "TimelineWindow" || id == "Timeline" ||
                id == "PreviewWindow" ) {
        const bool  preview = id == "PreviewWindow";
        const float width = preview
                                ? std::min(200.0f * m_scale, m_size.x * 0.2f)
                                : std::min(280.0f * m_scale, m_size.x * 0.28f);
        const float previewWidth =
            m_showPreview ? std::min(200.0f * m_scale, m_size.x * 0.2f) : 0.0f;
        ImGui::SetNextWindowDockID(0);
        ImGui::SetNextWindowPos({ m_origin.x + m_size.x - m_gap -
                                      m_toolbarWidth - width -
                                      (preview ? 0 : previewWidth),
                                  m_origin.y + m_gap });
        ImGui::SetNextWindowSize(
            { width, std::max(1.0f, m_size.y - 2 * m_gap) });
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking;
        if ( m_model.blocksCanvasInput() ) flags |= ImGuiWindowFlags_NoInputs;
    } else {
        // 设置等临时业务窗口不允许落入中央 Dock，避免挤走两个主要页签。
        ImGui::SetNextWindowDockID(0);
        flags |= ImGuiWindowFlags_NoDocking;
    }
    // 所有分支在同一个位置开始窗口，保证返回值和 Begin/End 契约一致。
    // 只提升当前抽屉内容的显示层级，不主动夺走文本编辑焦点。
    const bool visible = ImGui::Begin(title.c_str(), nullptr, flags);
    if ( m_drawingDrawer &&
         !ImGui::IsPopupOpen(
             nullptr,
             ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) )
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    return visible;
}

/// @brief 只绘制握柄，不再建立第二套点击或拖动状态。
/// @param id 当前一侧把手的稳定窗口身份。
/// @param rect 纯模型给出的完整触摸区，不等于细握柄的可见宽度。
/// @param opened 为 true 时只绘制外沿握柄，收起把手则保留入口图标。
/// @details 移动外壳在所有背景内容之后绘制握柄，便于拇指发现入口。
/// 命中区与系统最外侧返回区域错开，图形上不添加桌面悬浮说明。
/// 左侧文件夹表示资源入口，右侧滑杆表示工作区显示控制。
/// 图标只提供识别，不额外创建一个与手势模型竞争的按钮 Item。
/// @warning 固定图元绘制，不申请纹理或执行异步工作。
void MobileWorkspacePresentation::drawHandle(const char* id, MobileRect rect,
                                             bool opened)
{
    // 把手窗口自身不添加内容内边距，整块透明区域都是触摸目标。
    // 可见握柄只占其中少量宽度，避免长期遮住画布。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{});
    if ( beginPanel(id, rect, false) ) {
        if ( opened ) {
            // 不创建第二个收起按钮，点击和拖动都由同一个把手命中区处理。
            // 展开把手只绘制细握柄，透明命中区仍保持完整触摸宽度。
            // 它位于面板边缘，不占用任何业务内容行或标签栏。
            const auto  pos = ImGui::GetWindowPos();
            const float x   = pos.x + rect.m_width * .5f;
            ImGui::GetWindowDrawList()->AddRectFilled(
                { x - 3 * m_scale, pos.y + 12 * m_scale },
                { x + 3 * m_scale, pos.y + rect.m_height - 12 * m_scale },
                ImGui::GetColorU32(ImGuiCol_Text),
                3 * m_scale);
        } else {
            const auto color = ImGui::GetColorU32(ImGuiCol_Text);
            const auto pos   = ImGui::GetWindowPos();
            // 握柄本身不是按钮，48dp 透明命中区已由纯手势状态机处理。
            // 细握柄在命中区中居中，不把图形边缘误当成触摸边缘。
            // 颜色来自当前主题，浅色与深色皮肤均保留足够辨识度。
            const float x    = pos.x + rect.m_width * 0.5f;
            auto*       draw = ImGui::GetWindowDrawList();
            // 图标说明入口职责，短握柄说明可以拖动；两者共享原来的触摸目标。
            // 背景只占命中区内部，不扩大对主画布的遮挡范围。
            draw->AddRectFilled(
                { x - 18 * m_scale, pos.y + 4 * m_scale },
                { x + 18 * m_scale, pos.y + rect.m_height - 4 * m_scale },
                ImGui::GetColorU32(ImGuiCol_Button),
                4 * m_scale);
            auto* font = Config::SkinManager::instance().getFont("pure_icons");
            // 字体已由皮肤生命周期持有，把手不缓存跨皮肤重载的指针。
            // 回退字体仅用于皮肤缺少专用图标字体时保持绘制接口有效。
            if ( !font ) font = ImGui::GetFont();
            const char* icon = id == std::string_view("MobileLeftHandle")
                                   ? ICON_MMM_FOLDER_OPEN
                                   : ICON_MMM_SLIDERS;
            // 专用字体尺寸明确，避免当前窗口文字字体缺少私用区图标。
            const auto bounds =
                font->CalcTextSizeA(22 * m_scale, 100 * m_scale, 0, icon);
            // 用实际字形宽度居中，两个入口不会因为图标本身宽度不同而偏移。
            draw->AddText(font,
                          22 * m_scale,
                          { x - bounds.x * .5f, pos.y + 12 * m_scale },
                          color,
                          icon);
            draw->AddRectFilled(
                { x - 2 * m_scale, pos.y + rect.m_height - 29 * m_scale },
                { x + 2 * m_scale, pos.y + rect.m_height - 12 * m_scale },
                color,
                2 * m_scale);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

/// @brief 组合类别导航和当前业务管理器，形成联动的两级抽屉。
/// @param manager 管理内容视图的原宿主，借用仅在本帧有效。
/// @param rect 两级抽屉的整体矩形，遮罩判定也使用此同一边界。
/// @details 资源类别选择既有 FloatingManager 子视图，设置由右侧专用入口打开。
/// 关闭抽屉不清空活动类别，下一次打开仍能直接看到原来的内容。
/// @warning 每帧只绘制固定导航项和当前内容，不复制文件树或音轨列表。
void MobileWorkspacePresentation::drawLeftDrawer(UIManager& manager,
                                                 MobileRect rect)
{
    auto* sidebar  = manager.getView<SideBarUI>("SideBarUI");
    auto* floating = manager.getView<FloatingManagerUI>("SideBarManager");
    // 第一级宽度先容纳文字按钮，再增加左右配置留白。
    // 第二级使用整体抽屉的剩余区域，与第一级共享动画位移。
    // 窄屏压缩导航建议宽度，不压缩按钮外侧由用户配置的留白。
    const float railWidth =
        (m_size.x < 600.0f * m_scale ? 64.0f : 76.0f) * m_scale + 2 * m_padding;
    MobileRect rail{
        rect.m_x, rect.m_y, std::min(railWidth, rect.m_width), rect.m_height
    };
    if ( beginPanel("MobileNavigationDrawer", rail) ) {
        const std::array tabs{
            std::pair{ SideBarTab::Search, ICON_MMM_SEARCH },
            std::pair{ SideBarTab::FileExplorer, ICON_MMM_FOLDER_OPEN },
            std::pair{ SideBarTab::AudioExplorer, ICON_MMM_MUSIC },
            std::pair{ SideBarTab::BeatMapExplorer, ICON_MMM_FILE },
            std::pair{ SideBarTab::Collaboration, ICON_MMM_USERS }
        };
        // 类别列表固定且数量有限，不在绘制路径扫描业务视图注册表。
        // 选中状态直接可见，不要求用户通过悬停才能区分入口。
        for ( const auto& [tab, icon] : tabs ) {
            const auto label    = TabToShortLabel(tab);
            const bool selected = sidebar && sidebar->getActiveTab() == tab;
            // 切换类别保留其他资源管理器的当前业务选择。
            // 其他类别直接复用已注册的子视图，不重复构造资源管理器。
            ImGui::PushID(static_cast<int>(tab));
            // 分类枚举保证稳定身份，图标或翻译变化不会丢失按下反馈。
            const bool clicked = MobileNavigationButton(
                "category",
                icon,
                label.c_str(),
                { rail.m_width - 2 * m_padding, 72 * m_scale },
                selected);
            ImGui::PopID();
            if ( clicked ) {
                if ( floating && sidebar ) {
                    sidebar->setActiveTab(tab);
                    floating->restoreSubViewState(TabToSubViewId(tab), true);
                }
            }
        }
    }
    ImGui::End();
    m_contentRect   = { rect.m_x + rail.m_width + m_gap,
                        rect.m_y,
                        std::max(1.0f, rect.m_width - rail.m_width - m_gap),
                        rect.m_height };
    m_drawingDrawer = true;
    // 借用标记仅包围同步 update，公共窗口入口据此应用内容矩形。
    // 业务视图和文件选择状态的所有权仍留在原管理器中。
    if ( floating && floating->isVisible() ) {
        floating->updateAsDrawer(&manager);
    }
    m_drawingDrawer = false;
    // 立刻离开抽屉作用域，随后绘制的弹窗不会错误继承第二级尺寸。
}

/// @brief 通过大面积选中按钮控制辅助编辑窗口的显示状态。
/// @param rect 完整内容矩形，外沿把手不参与面板内部高度分配。
/// @details 标签和底色直接表达状态，不使用悬浮 tooltip 解释开关。
/// 开关属于移动工作区会话，不把每次触摸写入配置文件。
/// 几何变化在下一帧统一计算，避免同帧部分窗口使用旧边界。
/// @warning 用户激活时只改布尔值，连续绘制不执行持久化 I/O。
void MobileWorkspacePresentation::drawRightDrawer(UIManager& manager,
                                                  MobileRect rect)
{
    // 三个开关彼此独立，开关按钮自身始终留在抽屉内。
    // 因此关闭工具栏不会同时移除重新打开它的入口。
    if ( beginPanel("MobileWorkspaceControls", rect) ) {
        // 开关属于显式操作，不利用悬浮、长按或隐蔽的右键菜单来发现。
        // 固定标题与选中底色使用户可以直接判断当前工作区状态。
        ImGui::TextUnformatted("显示窗口");
        // 状态文案与稳定 ID 分开，切换显示状态后仍从同一按钮继续渐暗。
        // 图标表达窗口职责，文字和选中底色共同表达当前开关状态。
        // 隐藏窗口只停止显示，不终止其会话或重新加载谱面资源。
        // 选中底色表达状态，开关不依赖 tooltip 或小型复选框。
        if ( MobileActionButton("MobileToolbarToggle",
                                ICON_MMM_SLIDERS,
                                m_showToolbar ? "工具栏：显示" : "工具栏：隐藏",
                                { rect.m_width - 2 * m_padding, 56 * m_scale },
                                m_showToolbar) )
            m_showToolbar = !m_showToolbar;
        if ( MobileActionButton(
                 "MobileTimingToggle",
                 ICON_MMM_CLOCK,
                 m_showTiming ? "Timing 编辑：显示" : "Timing 编辑：隐藏",
                 { rect.m_width - 2 * m_padding, 56 * m_scale },
                 m_showTiming) )
            m_showTiming = !m_showTiming;
        if ( MobileActionButton("MobilePreviewToggle",
                                ICON_MMM_EYE,
                                m_showPreview ? "预览区：显示" : "预览区：隐藏",
                                { rect.m_width - 2 * m_padding, 56 * m_scale },
                                m_showPreview) )
            m_showPreview = !m_showPreview;
        // 设置由专用入口打开为中心大标签，不占用资源管理器的窄内容区。
        ImGui::Separator();
        if ( MobileActionButton(
                 "MobileSettings",
                 ICON_MMM_COG,
                 "设置",
                 { rect.m_width - 2 * m_padding, 56 * m_scale }) ) {
            manager.openSettingsWindow(Event::SettingsTab::Software);
            m_model.closeDrawer();
        }
    }
    ImGui::End();
}

/// @brief 绘制独立的触摸工具条，工具语义由既有命令通道解释。
/// @param manager 提供当前工具权威状态，不以按钮最近点击代替业务状态。
/// @details 工具高亮从应用服务读取，不以按钮上次点击结果冒充权威状态。
/// 每个按钮保持方形倾向，同时为中文标签保留实际测量出的宽度。
/// 外框宽度包含配置留白，不能只给按钮分配宽度导致边缘裁切。
/// @warning 每帧只读稳定服务观察指针，点击时投递低频编辑命令。
void MobileWorkspacePresentation::drawToolbar(UIManager& manager)
{
    MobileRect rect{ m_size.x - m_toolbarWidth - m_gap,
                     m_gap,
                     m_toolbarWidth,
                     m_size.y - 2 * m_gap };
    if ( beginPanel("MobileTouchToolbar", rect) ) {
        // 基础工具只组合公共命令，原有工具实现仍掌管选取和物件修改。
        // 此处不复制编辑器手势状态机，也不模拟键盘快捷键。
        const std::array tools{ std::pair{ "抓取", Logic::EditTool::Move },
                                std::pair{ "选取", Logic::EditTool::Marquee },
                                std::pair{ "绘制", Logic::EditTool::Draw } };
        const auto*      service = manager.getEditorApplicationService();
        // 图标沿用桌面工具的语义，文字常驻，触摸用户无需猜测图形含义。
        const std::array icons{ ICON_MMM_HAND,
                                ICON_MMM_SQUARE_SELECT,
                                ICON_MMM_PEN };
        // 图标顺序与固定工具表对应，不能依据当前工具名称动态查找字体资源。
        std::size_t index = 0;
        // 服务尚未绑定时不显示虚假的选中工具，点击仍走统一命令路由。
        for ( const auto& [label, tool] : tools ) {
            if ( MobileNavigationButton(
                     label,
                     icons[index],
                     label,
                     { rect.m_width - 2 * m_padding, 72 * m_scale },
                     service && service->currentTool() == tool) )
                MenuUtil::dispatchCommand(Logic::CmdChangeTool{ tool });
            ++index;
        }
    }
    ImGui::End();
}

/// @brief 用移动端自己的确认窗口表达退出意图，不复用桌面菜单布局。
/// @details 窗口没有原生 X，只有继续编辑或确认退出两种明确结果。
/// 确认前不发送关闭命令，返回手势也不会直接结束原生线程。
/// 取消会保留业务视图与当前编辑会话，下一次手势可以重新操作。
/// @warning 打开时仅绘制固定内容；实际退出由正常窗口生命周期处理。
void MobileWorkspacePresentation::drawExitConfirmation()
{
    // 模型拥有是否退出的意图，ImGui Popup 仅承担这一状态的显示。
    // 业务退出事件必须等确认按钮，不根据 Popup 是否消失推断同意。
    if ( m_model.exitConfirmation() )
        ImGui::OpenPopup("确认退出###MobileExitConfirmation");
    // 确认窗口按当前安全区居中，旋转后仍保留屏幕边距。
    // 不缓存上一次方向的像素位置，防止按钮落到屏幕之外。
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        { viewport->WorkPos.x + viewport->WorkSize.x * .5f,
          viewport->WorkPos.y + viewport->WorkSize.y * .5f },
        ImGuiCond_Always,
        { .5f, .5f });
    ImGui::SetNextWindowSize(
        { std::min(420.0f * m_scale, viewport->WorkSize.x * .9f), 0 });
    if ( ImGui::BeginPopupModal(
             "确认退出###MobileExitConfirmation",
             nullptr,
             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove) ) {
        // 再次返回可以撤销退出意图，但不能让 ImGui 留下无状态的模态窗口。
        // 再次系统返回已经取消模型状态时，同帧结束真正的 ImGui 模态。
        // 这里不能继续提交确认按钮，否则同一次返回可能留下第二个退出入口。
        if ( !m_model.exitConfirmation() ) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        // 退出必须由独立确认按钮完成；手势、遮罩点击均不能直接终止进程。
        ImGui::TextWrapped("是否退出 MusicMapMaker？");
        ImGui::Dummy({ 0, 12 * m_scale });
        if ( MobileActionButton("cancelExit",
                                ICON_MMM_ARROW_LEFT,
                                "继续编辑",
                                { -1, 56 * m_scale }) ) {
            m_model.finishExitConfirmation(false);
            ImGui::CloseCurrentPopup();
        }
        if ( MobileActionButton("confirmExit",
                                ICON_MMM_CLOSE,
                                "确认退出",
                                { -1, 56 * m_scale }) ) {
            m_model.finishExitConfirmation(true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/// @brief 按背景、遮罩、抽屉、确认层的顺序完成移动帧组合。
/// @param manager 当前 UI 宿主，业务子视图仍由其统一持有和销毁。
/// @details 更高层业务模态存在时不重新提升抽屉，保留其输入优先级。
/// 原始画布输入和 ImGui 窗口层级共同阻挡，不能仅依赖视觉暗化。
/// 返回事件只在确认后消费一次，避免下一帧重复发送应用关闭命令。
/// @warning 每帧固定数量面板与状态检查，不执行文件、网络或跨线程等待。
void MobileWorkspacePresentation::endFrame(UIManager& manager)
{
    // 业务模态拥有比抽屉更高的优先级，文件选择与确认流程不受外壳干扰。
    // 遮罩不是装饰效果，它和原始输入守卫一起建立明确的输入归属。
    const bool popup = ImGui::GetTopMostPopupModal() != nullptr;
    // 外壳面板使用同一内边距作用域，所有路径最后统一弹出样式。
    // 遮罩和内容按提交顺序叠放，但不会使用前景绘制覆盖业务弹窗。
    // 关闭动画结束前继续保留遮罩，避免抽屉还可见时背景先收到输入。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2{ m_padding, m_padding });
    if ( !popup && m_model.drawer() != MobileDrawer::None ) {
        // 遮罩窗口占据背景输入层，原始画布输入另由模型守卫共同阻挡。
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
                              ImVec4{ 0, 0, 0, .42f * m_model.fraction() });
        beginPanel("MobileDrawerScrim", { 0, 0, m_size.x, m_size.y });
        ImGui::End();
        ImGui::PopStyleColor();
        const auto rect = m_model.drawerRect();
        // 面板内容用满高度，外沿把手单独叠加，不再扣除顶部收起条。
        if ( m_model.drawer() == MobileDrawer::Left )
            drawLeftDrawer(manager, rect);
        else
            drawRightDrawer(manager, rect);
        drawHandle("MobileDrawerGrip", m_model.drawerGrip(), true);
        // 外沿把手与抽屉共享展开比例，内容区域不因把手显隐改变宽高。
        // 普通弹层仍由 ImGui 保持在上方，握柄不会抢占其输入。
    }
    if ( !popup && m_model.drawer() == MobileDrawer::None ) {
        drawHandle("MobileLeftHandle", m_model.leftHandle());
        drawHandle("MobileRightHandle", m_model.rightHandle());
    }
    drawExitConfirmation();
    m_touchScroll.endFrame();
    ImGui::PopStyleVar();
    // 确认只发布既有窗口命令，继续走工程收尾和资源销毁流程。
    // 不调用进程终止、Activity.finish 或跨线程 join 来模拟退出。
    if ( m_model.takeExitRequest() ) {
        // 请求经过现有关闭处理器，项目保存与线程停止继续由原生命周期负责。
        Event::GLFWNativeEvent request;
        request.type = Event::NativeEventType::GLFW_CLOSE_WINDOW;
        Event::EventBus::instance().publish(request);
    }
}
}  // namespace

/// @brief Android 目标唯一的平台外壳工厂，与桌面工厂不同时链接。
/// @details 将创建与选择策略移到构建边界，业务视图无需散布平台宏。
/// @return 由 UIManager 独占的长生命周期外壳实例。
std::unique_ptr<IWorkspacePresentation> createWorkspacePresentation()
{
    // 创建发生在 UIManager 初始化期；后续帧只复用模型和窗口存储。
    // 工厂定义只链接一次，不依赖运行时平台枚举或 Android 宏分支。
    return std::make_unique<MobileWorkspacePresentation>();
}
}  // namespace MMM::UI
