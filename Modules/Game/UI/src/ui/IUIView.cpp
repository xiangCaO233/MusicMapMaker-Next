#include "ui/IUIView.h"

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"

#include <cmath>

namespace MMM::UI
{

/// @brief 创建基础 UI 视图并申请独立 Clay 窗口上下文。
/// @param name 用于视图注册和窗口身份的稳定名称。
IUIView::IUIView(const std::string& name)
    : m_name(name)
    , m_layoutCtx(CLayWrapperCore::instance().createWindowContext())
{
    // Clay 上下文由单例统一创建，视图只保存对应句柄。
}

/// @brief 销毁基础 UI 视图持有的 Clay 窗口上下文。
/// @warning 视图必须在 CLayWrapperCore 单例销毁前释放。
IUIView::~IUIView()
{
    // 与构造时的 createWindowContext 严格配对，避免布局状态泄漏。
    CLayWrapperCore::instance().destroyWindowContext(m_layoutCtx);
}

/// @brief 建立单帧 ImGui 窗口和对应 Clay 指针输入上下文。
/// @param layoutContext 当前视图独占的 Clay 窗口上下文。
/// @param windowName 传给 ImGui 的可见标题与稳定内部 ID。
/// @param customWindowFlags 是否采用调用方提供的窗口标志。
/// @param windowFlags 自定义 ImGui 窗口标志。
/// @param open 可选窗口打开状态指针。
/// @param dockId 可选的目标 Dock 节点 ID。
/// @param dockCond 应用目标 Dock ID 的 ImGui 条件。
/// @warning UI 热路径：每帧构造；只压入样式、开始窗口并同步当前指针状态。
/// @note 析构函数负责 End 窗口并恢复六项样式变量。
LayoutContext::LayoutContext(CLayWrapperCore::WindowContext& layoutContext,
                             const std::string&              windowName,
                             bool                            customWindowFlags,
                             ImGuiWindowFlags windowFlags, bool* open,
                             ImGuiID dockId, ImGuiCond dockCond)
{
    // 每个视图先切换自己的 Clay context，隔离布局树和输入状态。
    CLayWrapperCore::instance().makeCurrent(layoutContext.context);

    // 标题字体允许皮肤覆盖；缺失时沿用当前 ImGui 默认字体。
    auto&   skinManager = Config::SkinManager::instance();
    ImFont* titleFont   = skinManager.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

    // 所有尺寸类美学参数按当前内容缩放换算为设备像素。
    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    // floor 让圆角和间距落在像素边界，减少缩放后的模糊边缘。
    const float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    const float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    const ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    // 缓存比例供同一帧的视图布局逻辑复用。
    m_dpiScale = dpiScale;
    // 六项局部样式必须与析构中的 PopStyleVar(6) 保持数量一致。
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    if ( dockId != 0 ) {
        // 仅显式指定 Dock 节点时设置下一窗口，避免覆盖用户布局。
        ImGui::SetNextWindowDockID(dockId, dockCond);
    }

    // Begin 前保存打开状态，以识别原生关闭按钮在本帧产生的边沿。
    const bool wasOpenBeforeBegin = open != nullptr && *open;
    if ( customWindowFlags ) {
        // 自定义模式完整采用调用方标志。
        ImGui::Begin(windowName.c_str(), open, windowFlags);
    } else {
        // 默认模式让 ImGui 使用标准窗口行为。
        ImGui::Begin(windowName.c_str(), open);
    }
    // 补充原生关闭按钮的统一音效和 Dock 标签悬停反馈。
    FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, open);

    // 标题字体只覆盖 Begin 阶段，窗口内容恢复调用前字体。
    if ( titleFont ) ImGui::PopFont();

    // 保存内容起点和剩余范围，析构时用 Dummy 占据 Clay 绘制区域。
    m_startPos    = ImGui::GetCursorScreenPos();
    m_avail       = ImGui::GetContentRegionAvail();
    m_mousePos    = ImGui::GetMousePos();
    m_isMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    // Clay 使用相对内容起点的指针坐标，与 ImGui 屏幕坐标完成转换。
    Clay_SetPointerState(
        { m_mousePos.x - m_startPos.x, m_mousePos.y - m_startPos.y },
        m_isMouseDown);
}

/// @brief 结束当前 LayoutContext 管理的 ImGui 窗口并恢复局部样式。
/// @warning UI 热路径：必须与构造函数在同一作用域严格配对。
LayoutContext::~LayoutContext()
{
    // Dummy 将光标推进到预留内容末尾，让 ImGui 正确计算窗口尺寸与滚动范围。
    ImGui::SetCursorScreenPos(m_startPos);
    ImGui::Dummy(m_avail);
    // End 必须在恢复窗口级样式前调用，使本窗口完整使用统一主题。
    ImGui::End();
    // 恢复构造阶段压入的 WindowPadding、圆角和 ItemSpacing。
    ImGui::PopStyleVar(6);
}

}  // namespace MMM::UI
