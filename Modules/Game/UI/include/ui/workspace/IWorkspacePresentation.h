#pragma once

#include <imgui.h>
#include <memory>
#include <string>
#include <string_view>

namespace MMM::UI
{
class UIManager;
class MainDockSpaceUI;

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
    /// @brief 是否恢复桌面停靠树及原生窗口位置，移动外壳独立管理几何。
    virtual bool usesDesktopLayout() const { return true; }
    /// @brief 是否显示应用内音频输出设备选择；移动端交由系统音频路由管理。
    virtual bool showsAudioOutputDeviceSelector() const { return true; }
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
/// @brief 供 LayoutContext 与直接窗口入口共用，不改变默认桌面行为。
bool beginWorkspaceWindow(std::string_view id, const std::string& title,
                          bool* open, ImGuiWindowFlags flags = 0);
/// @brief 供画布的原始输入守卫查询当前外壳；没有 UI 帧时返回 false。
bool workspaceBlocksCanvasInput();
/// @brief 供公共反馈设施查询平台策略；没有 UI 帧时保留桌面悬浮行为。
bool workspaceAllowsHoverFeedback();
/// @brief 查询本帧的音频设备选择能力；没有工作区作用域时保留桌面行为。
bool workspaceShowsAudioOutputDeviceSelector();
}  // namespace MMM::UI
