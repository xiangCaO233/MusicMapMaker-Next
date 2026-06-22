#pragma once

struct GLFWwindow;

namespace MMM::Graphic
{

/// @brief 平台窗口边框适配器访问主窗口状态的宿主接口。
class IWindowFrameHost
{
public:
    /// @brief 默认析构函数。
    virtual ~IWindowFrameHost() = default;

    IWindowFrameHost()                                   = default;
    IWindowFrameHost(IWindowFrameHost&&)                 = delete;
    IWindowFrameHost(const IWindowFrameHost&)            = delete;
    IWindowFrameHost& operator=(IWindowFrameHost&&)      = delete;
    IWindowFrameHost& operator=(const IWindowFrameHost&) = delete;

    /// @brief 获取 GLFW 主窗口句柄。
    /// @return GLFW 窗口句柄；未创建时返回 nullptr。
    [[nodiscard]] virtual GLFWwindow* getFrameWindowHandle() const = 0;

    /// @brief 获取普通窗口模式下的还原矩形。
    /// @param x 输出左上角 X 坐标。
    /// @param y 输出左上角 Y 坐标。
    /// @param width 输出窗口宽度。
    /// @param height 输出窗口高度。
    virtual void getNormalFramePlacement(int& x, int& y, int& width,
                                         int& height) const = 0;

    /// @brief 写入普通窗口模式下的还原矩形。
    /// @param x 左上角 X 坐标。
    /// @param y 左上角 Y 坐标。
    /// @param width 窗口宽度。
    /// @param height 窗口高度。
    virtual void setNormalFramePlacement(int x, int y, int width,
                                         int height) = 0;

    /// @brief 判断当前平台窗口是否处于最大化状态。
    /// @return 当前窗口应被视为最大化时返回 true。
    [[nodiscard]] virtual bool isFrameMaximized() const = 0;

    /// @brief 为标题栏拖动还原最大化窗口，并保持鼠标相对位置。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 成功从最大化状态还原时返回 true。
    /// @warning 输入低频路径：仅在标题栏拖动已超过阈值时执行，会调整窗口矩形。
    virtual bool restoreFrameForClientMove(double cursorX, double cursorY) = 0;
};

}  // namespace MMM::Graphic
