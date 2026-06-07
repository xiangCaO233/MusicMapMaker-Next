#pragma once

namespace MMM::Graphic
{

/// @brief 无原生装饰窗口的缩放方向。
enum class WindowFrameResizeEdge {
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
};

/// @brief 无原生装饰窗口的平台行为适配接口。
class IWindowFrameAdapter
{
public:
    /// @brief 默认析构函数。
    virtual ~IWindowFrameAdapter() = default;

    IWindowFrameAdapter()                                      = default;
    IWindowFrameAdapter(IWindowFrameAdapter&&)                 = delete;
    IWindowFrameAdapter(const IWindowFrameAdapter&)            = delete;
    IWindowFrameAdapter& operator=(IWindowFrameAdapter&&)      = delete;
    IWindowFrameAdapter& operator=(const IWindowFrameAdapter&) = delete;

    /// @brief 请求平台窗口系统从当前鼠标位置开始移动窗口。
    /// @return 成功交给平台窗口系统处理时返回 true。
    virtual bool requestMove() = 0;

    /// @brief 请求平台窗口系统从当前鼠标位置开始缩放窗口。
    /// @param edge 缩放方向。
    /// @return 成功交给平台窗口系统处理时返回 true。
    virtual bool requestResize(WindowFrameResizeEdge edge) = 0;

    /// @brief 处理 GLFW 鼠标按键输入中的无装饰窗口交互。
    /// @param button GLFW 鼠标按键值。
    /// @param action GLFW 鼠标动作值。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 已发起平台窗口交互时返回 true。
    /// @warning 输入热路径：GLFW 鼠标回调中执行；只能做常量规模命中判断。
    virtual bool handleClientMouseButton(int button, int action, double cursorX,
                                         double cursorY) = 0;

    /// @brief 处理 GLFW 鼠标移动输入中的无装饰窗口交互。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 已发起平台窗口交互时返回 true。
    /// @warning 输入热路径：GLFW 鼠标移动回调中执行；只能做常量规模命中判断。
    virtual bool handleClientCursorPos(double cursorX, double cursorY) = 0;

    /// @brief 处理 GLFW 主窗口焦点状态变化。
    /// @param focused 主窗口获得焦点时为 true。
    /// @warning 输入热路径：GLFW 焦点回调中执行；只能更新常量规模状态。
    virtual void handleClientFocusChange(bool focused) { (void)focused; }

    /// @brief 判断是否需要 UI 层主动发起移动和缩放请求。
    /// @return 需要 UI 层调用 requestMove/requestResize 时返回 true。
    [[nodiscard]] virtual bool supportsClientFrameRequests() const = 0;

    /// @brief 判断是否需要 UI 层绘制无装饰窗口边框。
    /// @return 需要 UI 层补绘边框和阴影时返回 true。
    [[nodiscard]] virtual bool usesClientFrameOverlay() const = 0;

    /// @brief 刷新平台窗口外形或系统边框状态。
    virtual void refreshFrameShape() = 0;
};

}  // namespace MMM::Graphic
