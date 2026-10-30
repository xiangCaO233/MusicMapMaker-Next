#include "ui/workspace/IWorkspacePresentation.h"

namespace MMM::UI
{
namespace
{
/// @brief 仅 UI 线程使用的借用指针，帧作用域结束后立即恢复。
thread_local IWorkspacePresentation* activePresentation = nullptr;
}  // namespace

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
