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
};

}  // namespace MMM::Graphic
#endif
