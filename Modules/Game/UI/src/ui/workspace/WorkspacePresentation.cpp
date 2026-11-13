#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace MMM::UI
{
/// @brief 桌面平台保留原有文字按钮，不改变菜单动作的激活方式。
/// @details 图标参数只供触摸平台使用，桌面布局尺寸仍由调用方决定。
/// @warning 每帧只转发统一反馈按钮，不读取平台或执行持久化。
bool IWorkspacePresentation::actionButton(const char* label, const char*,
                                          ImVec2      size)
{
    return FeedbackButton(label, size);
}

namespace
{
/// @brief 仅 UI 线程使用的借用指针，帧作用域结束后立即恢复。
thread_local IWorkspacePresentation* activePresentation = nullptr;
/// @brief 没有平台绑定时保留桌面主视口几何，不猜测移动端保留宽度。
/// @warning 只在 UI 帧内读取 ImGui 标量，没有设备或会话查询。
WorkspaceWindowArea viewportWindowArea()
{
    const auto* viewport = ImGui::GetMainViewport();
    return { viewport->WorkPos, viewport->WorkSize };
}
}  // namespace

/// @brief 默认平台使用完整工作区，已保存的桌面窗口布局继续由调用方管理。
WorkspaceWindowArea IWorkspacePresentation::floatingWindowArea() const
{
    return viewportWindowArea();
}
/// @brief 统一转发当前帧的平台布局，调用方不包含移动外壳的私有状态。
/// @warning 高频查询只读 UI 线程已发布的矩形，不等待后台或复制所有权。
// 无帧绑定的 CPU 测试与桌面默认视图也必须获得同一坐标口径。
WorkspaceWindowArea workspaceFloatingWindowArea()
{
    return activePresentation ? activePresentation->floatingWindowArea()
                              : viewportWindowArea();
}

/// @brief 默认文本窗不额外预留输入法区域，移动策略独立提供遮挡几何。
/// @warning 每帧只读取当前策略，不访问平台线程。
WorkspaceWindowArea IWorkspacePresentation::textInputWindowArea() const
{
    return floatingWindowArea();
}
/// @brief 只为模态表单转发输入安全区域，普通工作窗保持其原尺寸。
/// @details 键盘是临时遮挡，不能把其高度固化成浮动窗口的保存布局。
/// @warning 高频入口不查询设备、不复制所有权或等待会话。
WorkspaceWindowArea workspaceTextInputWindowArea()
{
    return activePresentation ? activePresentation->textInputWindowArea()
                              : viewportWindowArea();
}

/// @brief 依当前平台选择触摸图文或桌面文字按钮。
/// @details 公共模态不引入平台宏，默认桌面与无绑定测试仍用原反馈入口。
/// 策略只提供外观，是否下载或关闭弹窗继续由调用方消费激活结果。
/// @warning 固定次数虚调用，无所有权复制或后台任务。
bool workspaceActionButton(const char* label, const char* icon, ImVec2 size)
{
    return activePresentation
               ? activePresentation->actionButton(label, icon, size)
               : FeedbackButton(label, size);
}

/// @brief 默认窗口适配保持桌面参数的原始语义。
/// @details 不在公共入口推断设备类型；移动实现显式覆盖本方法。
/// 返回 false 时调用方仍需 End，与 ImGui 原始契约一致。
bool IWorkspacePresentation::beginWindow(std::string_view,
                                         const std::string& title, bool* open,
                                         ImGuiWindowFlags flags)
{
    // 桌面和未覆盖的平台继续使用原始窗口参数。
    return ImGui::Begin(title.c_str(), open, flags);
}

/// @brief 默认文件选择器维持原有桌面逻辑尺寸。
/// @details 可用区域和倍率只供其他平台覆盖，不重复缩放桌面布局。
ImVec2 IWorkspacePresentation::fileDialogSize(float, ImVec2) const
{
    // 桌面原有尺寸不受移动端尺寸策略影响。
    return { 600.0f, 400.0f };
}

/// @brief 保存外层绑定后安装本帧借用引用。
/// @details 策略必须覆盖作用域生命周期，不复制任何共享所有权。
WorkspaceFrameScope::WorkspaceFrameScope(IWorkspacePresentation& presentation)
    : m_previous(activePresentation)
{
    activePresentation = &presentation;
}

/// @brief 恢复外层绑定，允许测试顺序创建不同平台上下文。
WorkspaceFrameScope::~WorkspaceFrameScope()
{
    // 不保留已销毁 UIManager 的指针，测试可安全重新创建管理器。
    activePresentation = m_previous;
}

/// @brief 将窗口提交到当前帧策略，没有策略时使用默认桌面入口。
/// @warning 每个窗口调用一次；只访问本线程观察指针，不分配策略。
bool beginWorkspaceWindow(std::string_view id, const std::string& title,
                          bool* open, ImGuiWindowFlags flags)
{
    return activePresentation
               ? activePresentation->beginWindow(id, title, open, flags)
               : ImGui::Begin(title.c_str(), open, flags);
}

/// @brief 查询本帧移动外壳是否拥有画布手势。
/// @details 无绑定的纯业务测试不应被平台输入状态阻挡。
bool workspaceBlocksCanvasInput()
{
    return activePresentation && activePresentation->blocksCanvasInput();
}

/// @brief 公共反馈保留默认桌面行为，触摸策略主动关闭悬浮音效。
/// @warning 只读取帧内稳定策略，不查询操作系统设备列表。
bool workspaceAllowsHoverFeedback()
{
    return !activePresentation || activePresentation->allowsHoverFeedback();
}

/// @brief 标签布局由平台组合根决定，公共画布不包含任何移动实现头。
/// @warning 固定一次虚调用，桌面默认返回 false，保留其原有绘制路径。
bool workspaceRenderTimelineLaneLabels(ImVec2 origin, ImVec2 size)
{
    return activePresentation &&
           activePresentation->renderTimelineLaneLabels(origin, size);
}

/// @brief 保持桌面密度，触屏整行的实际可点击内容不小于手指目标。
/// @details 调用方应把返回值同时用于行布局与选择区，而非仅扩大背景。
/// @warning 每帧常量级字体测量，不查询配置或跨线程状态。
float workspaceTableItemHeight(float desktopHeight, float scale)
{
    // 外壳能力沿用公共触屏分支；无绑定测试仍保持桌面尺寸。
    if ( workspaceAllowsHoverFeedback() ) return desktopHeight;
    return std::ceil(
        std::max({ desktopHeight, 48.0f * scale, ImGui::GetFrameHeight() }));
}

/// @brief 统一表格的基础呼吸空间，同时尊重主题中更大的既有留白。
/// @details 纯文本表格、表头及自动换行到下一列的行均继承该样式。
/// 可点击列表另行测量内容高度，避免把单元格留白误当成命中区。
/// @warning 移动外壳每帧调用一次，只做固定规模数学和 ImGui 样式读取。
ImVec2 workspaceTableCellPadding(float scale)
{
    const auto padding = ImGui::GetStyle().CellPadding;
    // 不硬改字体和全局倍率；高 DPI 与用户大字号分别参与尺寸下限。
    return { std::max(padding.x, 8.0f * scale),
             std::max(
                 { padding.y,
                   6.0f * scale,
                   std::ceil((48.0f * scale - ImGui::GetFontSize()) * .5f) }) };
}

/// @brief 数值组件仅提交语义，是否进行触摸方向仲裁由当前外壳决定。
/// @details 无平台作用域的桌面和测试不改变原生输入行为。
/// @warning 固定次数虚调用，不复制所有权或创建每帧登记容器。
void workspaceRegisterHorizontalDragItem()
{
    if ( activePresentation ) activePresentation->registerHorizontalDragItem();
}

/// @brief 竖滑条发布方向语义，不在公共控件中判断操作系统。
/// @warning 每个可见竖滑条固定次数转发，无分配或拥有型引用复制。
void workspaceRegisterVerticalDragItem()
{
    if ( activePresentation ) activePresentation->registerVerticalDragItem();
}

/// @brief 将平台设备选择能力传给共享混音视图，不向业务视图泄漏平台宏。
/// @details 布局准备在 UI 主线程捕获此值，后台计算只读取快照中的布尔量。
/// 无作用域的桌面测试与旧入口保持原有可见性，不依赖设备枚举推断平台。
/// @warning 每帧只读取当前线程观察指针和固定能力，不访问音频后端。
bool workspaceShowsAudioOutputDeviceSelector()
{
    return !activePresentation ||
           activePresentation->showsAudioOutputDeviceSelector();
}
}  // namespace MMM::UI

namespace MMM::UI
{
/// @brief 共用查询保证分类、设置行与搜索结果使用同一份设备能力。
/// @warning UI 每帧只借用已绑定外壳，不获取任何跨线程资源。
bool workspaceShowsPointerSettings()
{
    return !activePresentation || activePresentation->showsPointerSettings();
}
/// @brief 设备用语只在移动外壳替换，桌面翻译和自定义方案入口保持原样。
/// @warning 每行只比较固定数量翻译键，无文件或设备访问。
std::string_view workspaceSettingLabelKey(std::string_view key)
{
    if ( !activePresentation || activePresentation->usesDesktopLayout() )
        return key;
    // 独立移动翻译键保留桌面的鼠标、滚轮与修饰键说明。
    // 搜索索引同样采用这些键，显示名称必须能定位回实际控件。
    for ( const auto& [original, mobile] :
          { std::pair{ "ui.settings.software.save_format",
                       "ui.mobile.settings.save_format" },
            std::pair{ "ui.settings.editor.reverse_scroll",
                       "ui.mobile.settings.reverse_scroll" },
            std::pair{ "ui.settings.editor.scroll_snap",
                       "ui.mobile.settings.scroll_snap" },
            std::pair{ "ui.settings.editor.snap_floor_tooltip",
                       "ui.mobile.settings.snap_floor_tooltip" },
            std::pair{
                "ui.settings.editor.disable_scroll_accel_while_drawing",
                "ui.mobile.settings.disable_scroll_accel_while_drawing" },
            std::pair{ "ui.settings.editor.paste_position.mouse_cursor",
                       "ui.mobile.settings.paste_position" } } )
        if ( key == original ) return mobile;
    return key;
}
}  // namespace MMM::UI
