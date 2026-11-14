#pragma once

#include <imgui.h>
#include <memory>
#include <string>
#include <string_view>

namespace MMM::UI
{
class UIManager;
class MainDockSpaceUI;

/// @brief 浮动辅助窗口可以使用的屏幕区域，不包含平台常驻工具栏。
/// @details 原点与尺寸均为当前 ImGui 屏幕坐标，不保存设备或窗口所有权。
struct WorkspaceWindowArea {
    ImVec2 m_origin;  ///< 区域左上角，包含当前主视口工作区偏移。
    ImVec2 m_size;    ///< 可用尺寸；平台遮挡只在外壳内部计算。
};
/// @brief 工作区外壳策略；桌面与移动端分别组合布局，不改变编辑器业务视图。
class IWorkspacePresentation
{
public:
    /// @brief 由 UIManager 独占策略并在视图停止更新后销毁。
    virtual ~IWorkspacePresentation() = default;
    /// @brief 在控件与原始画布输入之前确定本帧布局和输入归属。
    /// @warning 每帧执行；禁止文件访问、长锁和同步等待。
    virtual void beginFrame(UIManager& manager) {}
    /// @brief 绘制平台自己的工作区外壳，业务弹窗仍由公共主视图处理。
    virtual void renderWorkspace(MainDockSpaceUI& dock, UIManager& manager) = 0;
    /// @brief 选择普通更新序列中的视图；返回 false 表示由外壳延后组合。
    virtual bool updatesView(std::string_view name) const { return true; }
    /// @brief 在业务视图之后提交抽屉及其输入遮罩。
    /// @warning 每帧执行；仅允许固定规模状态更新与 UI 绘制。
    virtual void endFrame(UIManager& manager) {}
    /// @brief 统一窗口入口，允许平台替换停靠、关闭和输入规则。
    virtual bool beginWindow(std::string_view id, const std::string& title,
                             bool* open, ImGuiWindowFlags flags);
    /// @brief 原始画布输入是否被外壳独占；不用于抽屉内部控件。
    virtual bool blocksCanvasInput() const { return false; }
    /// @brief 平台是否使用鼠标悬浮反馈，触摸外壳只使用按下反馈。
    virtual bool allowsHoverFeedback() const { return true; }
    /// @brief 平台可接管窄时间线类型标签；默认 false 保留桌面原覆盖层。
    /// @warning 每帧只绘制固定五个标签，不读取会话或创建资源。
    virtual bool renderTimelineLaneLabels(ImVec2 origin, ImVec2 size)
    {
        return false;
    }
    /// @brief 公共辅助窗口的安全区域，桌面默认返回完整主视口工作区。
    /// @warning 每帧固定标量读取，不访问窗口树或执行跨线程查询。
    virtual WorkspaceWindowArea floatingWindowArea() const;
    /// @brief 文本模态的可用区域，移动端可进一步扣除
    /// IME；普通辅助窗不随键盘缩小。
    /// @warning 每帧仅读取外壳缓存几何，桌面默认与 floatingWindowArea 一致。
    virtual WorkspaceWindowArea textInputWindowArea() const;
    /// @brief 当前数值组件允许水平拖动；桌面无需额外登记。
    virtual void registerHorizontalDragItem() {}
    /// @brief 竖直数值条沿纵向调值，横向仍允许浏览内容；桌面保持原生输入。
    virtual void registerVerticalDragItem() {}
    /// @brief 是否恢复桌面停靠树及原生窗口位置，移动外壳独立管理几何。
    virtual bool usesDesktopLayout() const { return true; }
    /// @brief 是否显示应用内音频输出设备选择；移动端交由系统音频路由管理。
    virtual bool showsAudioOutputDeviceSelector() const { return true; }
    /// @brief 外壳是否提供鼠标相关设置、快捷键页和方案选择，桌面始终保留。
    /// @warning 读取 UI 线程能力快照，不在设置控件内查询系统设备。
    virtual bool showsPointerSettings() const { return true; }
    /// @brief 公共模态的操作按钮，由平台决定触摸尺寸和图标排布。
    /// @warning 每帧只绘制按钮，不在策略内部执行对应业务操作。
    virtual bool actionButton(const char* label, const char* icon, ImVec2 size);
    /// @brief 根据平台可用空间限制公共文件对话框。
    virtual ImVec2 fileDialogSize(float scale, ImVec2 available) const;
};

/// @brief 工厂实现由 CMake 按平台选择，公共视图不包含平台条件编译。
std::unique_ptr<IWorkspacePresentation> createWorkspacePresentation();

/// @brief 当前 UI 帧的非拥有策略绑定，不向工作线程传播可变 UI 状态。
class WorkspaceFrameScope final
{
public:
    /// @brief 绑定本帧策略，支持测试中的嵌套 UI 上下文。
    explicit WorkspaceFrameScope(IWorkspacePresentation& presentation);
    /// @brief 恢复进入作用域之前的策略观察指针。
    ~WorkspaceFrameScope();

private:
    /// @brief 外层 UI 帧的观察指针，不拥有其生命周期。
    IWorkspacePresentation* m_previous;
};
/// @brief 查询当前外壳安全区域；无帧作用域时使用完整主视口工作区。
/// @warning 每帧只借用策略并复制四个标量，不分配或等待。
WorkspaceWindowArea workspaceFloatingWindowArea();
/// @brief 查询文本表单专用区域，不将临时键盘尺寸写入普通浮动窗布局。
/// @warning 每帧只复制几何；没有帧绑定时返回完整主视口工作区。
WorkspaceWindowArea workspaceTextInputWindowArea();
/// @brief 供 LayoutContext 与直接窗口入口共用，不改变默认桌面行为。
bool beginWorkspaceWindow(std::string_view id, const std::string& title,
                          bool* open, ImGuiWindowFlags flags = 0);
/// @brief 供画布的原始输入守卫查询当前外壳；没有 UI 帧时返回 false。
bool workspaceBlocksCanvasInput();
/// @brief 供公共反馈设施查询平台策略；没有 UI 帧时保留桌面悬浮行为。
bool workspaceAllowsHoverFeedback();
/// @brief 平台中立的时间线标签入口，无策略时由原画布继续绘制。
/// @warning 每帧只进行一次非拥有策略转发。
bool workspaceRenderTimelineLaneLabels(ImVec2 origin, ImVec2 size);
/// @brief 表格项目内容高度；触屏至少保留 48 个逻辑像素，桌面原值返回。
/// @details 不包含 CellPadding；显式步长的裁剪器须另加上下单元格留白。
/// @warning 每个可见列表固定次数测量，不分配或访问设备。
float workspaceTableItemHeight(float desktopHeight, float scale);
/// @brief 移动表格统一留白，纯文字行也至少达到 48 个逻辑像素。
/// @details 仅计算样式值，由移动外壳在帧内成对应用，不更改持久化主题。
/// @warning 每帧仅测量当前字体，不执行资源操作或访问跨线程可变状态。
ImVec2 workspaceTableCellPadding(float scale);
/// @brief 为刚提交的数值控件登记横向拖动，默认桌面路径为空操作。
/// @warning 每帧固定次数策略转发，不保留组件引用。
void workspaceRegisterHorizontalDragItem();
/// @brief 为刚提交的竖滑条登记触摸方向，默认桌面路径为空操作。
/// @warning 每帧只转发当前平台，不保存组件引用或延迟本地数值变化。
void workspaceRegisterVerticalDragItem();
/// @brief 查询本帧的音频设备选择能力；没有工作区作用域时保留桌面行为。
bool workspaceShowsAudioOutputDeviceSelector();
/// @brief 公共弹窗复用当前平台操作按钮，无帧作用域时保持桌面入口。
bool workspaceActionButton(const char* label, const char* icon, ImVec2 size);
}  // namespace MMM::UI

namespace MMM::UI
{
/// @brief 当前帧的键鼠设置能力；无外壳的桌面测试保留原有入口。
/// @warning 常量级虚调用，不枚举设备、不等待平台线程。
bool workspaceShowsPointerSettings();
/// @brief 移动端用中性触控文案替换设备限定词，桌面返回原翻译键。
/// @warning 返回静态字符串或输入借用，不分配临时文本。
std::string_view workspaceSettingLabelKey(std::string_view key);
}  // namespace MMM::UI
