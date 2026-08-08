#pragma once

#ifdef _WIN32
#    include "event/ui/UpdateDragAreaEvent.h"
#    include "graphic/glfw/window/adapters/IWindowFrameAdapter.h"
#    include <vector>
#    include <windows.h>

struct GLFWwindow;

namespace MMM::Graphic
{

/**
 * @class Win32WindowAdapter
 * @brief Windows 原生窗口适配器
 *
 * 负责接收跨平台的窗口事件，并将其转换为 Win32 特有的无边框窗口行为
 * 包括处理拖拽响应、无边框阴影、圆角等特性
 */
class Win32WindowAdapter final : public IWindowFrameAdapter
{
public:
    /**
     * @brief 构造函数
     * @param window GLFW 窗口句柄指针
     */
    explicit Win32WindowAdapter(GLFWwindow* window);

    /**
     * @brief 析构函数
     */
    ~Win32WindowAdapter();

    // 禁用拷贝和移动
    Win32WindowAdapter(Win32WindowAdapter&&)                 = delete;
    Win32WindowAdapter(const Win32WindowAdapter&)            = delete;
    Win32WindowAdapter& operator=(Win32WindowAdapter&&)      = delete;
    Win32WindowAdapter& operator=(const Win32WindowAdapter&) = delete;

    /// @brief Win32 通过 WM_NCHITTEST 处理拖动，不需要 UI 主动发起移动。
    /// @return 固定返回 false。
    bool requestMove() override;

    /// @brief Win32 通过 WM_NCHITTEST 处理缩放，不需要 UI 主动发起缩放。
    /// @param edge 缩放方向。
    /// @return 固定返回 false。
    bool requestResize(WindowFrameResizeEdge edge) override;

    /// @brief 判断是否需要 UI 层主动发起移动和缩放请求。
    /// @return Win32 固定返回 false。
    [[nodiscard]] bool supportsClientFrameRequests() const override;

    /// @brief 判断是否需要 UI 层补绘边框和阴影。
    /// @return Win32 固定返回 false，使用 DWM 处理。
    [[nodiscard]] bool usesClientFrameOverlay() const override;

    /// @brief 刷新 Win32 无边框窗口的 DWM 外观状态。
    void refreshFrameShape() override;

    /// @brief 将独立平台窗口登记为主窗口所有的任务栏窗口组成员。
    /// @param window 待登记的独立平台窗口。
    /// @param mainWindow 任务栏窗口组对应的主窗口。
    /// @warning 渲染热路径会重复调用；仅在归属变化时写入原生 owner 和 HWND
    /// 属性。
    static void associateTaskbarGroupWindow(HWND window, HWND mainWindow);

    /// @brief Win32 不从 GLFW 鼠标按键回调发起窗口 frame 行为。
    /// @param button GLFW 鼠标按键值。
    /// @param action GLFW 鼠标动作值。
    /// @param cursorX 鼠标 X 坐标。
    /// @param cursorY 鼠标 Y 坐标。
    /// @return 固定返回 false。
    bool handleClientMouseButton(int button, int action, double cursorX,
                                 double cursorY) override;

    /// @brief Win32 不从 GLFW 鼠标移动回调发起窗口 frame 行为。
    /// @param cursorX 鼠标 X 坐标。
    /// @param cursorY 鼠标 Y 坐标。
    /// @return 固定返回 false。
    bool handleClientCursorPos(double cursorX, double cursorY) override;

private:
    /// @brief 判断 Win32 窗口当前是否明确处于最大化状态。
    /// @param hWnd Win32 窗口句柄。
    /// @return 当前窗口为最大化时返回 true。
    [[nodiscard]] bool windowPlacementWantsMaximized(HWND hWnd) const;

    /// @brief 在最小化发生前记录后续应恢复到的最大化状态。
    /// @param hWnd Win32 窗口句柄。
    void rememberRestoreStateBeforeMinimize(HWND hWnd);

    /// @brief 判断窗口是否仍带有恢复最大化标记。
    /// @param hWnd Win32 窗口句柄。
    /// @return 仍需恢复最大化时返回 true。
    [[nodiscard]] bool hasRestoreMaximizedProperty(HWND hWnd) const;

    /// @brief 任务栏恢复后重新应用最大化，修正无边框窗口被还原成普通窗口。
    /// @param hWnd Win32 窗口句柄。
    void restoreMaximizedAfterMinimize(HWND hWnd);

    /// @brief 在 Win32 完成普通恢复消息后，应用排队的最大化恢复。
    /// @param hWnd Win32 窗口句柄。
    void applyQueuedMaximizedRestore(HWND hWnd);

    /**
     * @brief Win32 窗口过程消息拦截器
     * @param hWnd 窗口句柄
     * @param uMsg 消息 ID
     * @param wParam 消息 wParam 参数
     * @param lParam 消息 lParam 参数
     * @param uIdSubclass 子类化 ID
     * @param dwRefData 附加引用数据（这里传入 this 指针）
     * @return LRESULT 消息处理结果
     */
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR uIdSubclass,
                                       DWORD_PTR dwRefData);

    /**
     * @brief 处理拖拽区域更新事件
     * @param e 拖拽区域更新事件
     */
    void onUpdateDragArea(const Event::UpdateDragAreaEvent& e);

private:
    GLFWwindow* m_window{ nullptr };           ///< 关联的 GLFW 窗口指针
    HWND        m_hwnd{ nullptr };             ///< 关联的 Win32 原生窗口句柄
    std::vector<Event::DragArea> m_dragAreas;  ///< 缓存的允许拖拽的矩形区域列表

    /// @brief 缓存的标题栏拖拽排除区域列表。
    std::vector<Event::DragArea> m_blockedDragAreas;

    /// @brief 最近一次非最小化尺寸消息是否表示窗口处于最大化。
    bool m_lastKnownMaximized{ false };

    /// @brief 最小化前是否最大化，用于任务栏恢复后重新最大化。
    bool m_restoreMaximizedAfterMinimize{ false };

    /// @brief 当前是否正在通过 ShowWindow 应用最大化恢复，避免同步消息递归。
    bool m_applyingMaximizedRestore{ false };

    /// @brief 是否已经排队延迟最大化恢复消息。
    bool m_maximizedRestorePosted{ false };

    /// @brief Alt+Tab 恢复最小化的最大化窗口后，是否忽略下一次 SC_RESTORE。
    bool m_ignoreNextRestoreSysCommand{ false };

    /// @brief 是否已经排队清理临时 SC_RESTORE 忽略标记的消息。
    bool m_restoreIgnoreClearPosted{ false };
};

}  // namespace MMM::Graphic
#endif
