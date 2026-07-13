#pragma once

#include "graphic/glfw/GLFWHeader.h"
#include "graphic/glfw/window/adapters/IWindowFrameHost.h"
#include <atomic>
#include <chrono>
#include <memory>

namespace MMM::Graphic
{
class IWindowFrameAdapter;

/// @brief GLFW 主窗口封装，负责窗口生命周期和平台 frame adapter 转发。
class NativeWindow : public IWindowFrameHost
{
public:
    /// @brief 构造主窗口。
    /// @param w 初始窗口宽度。
    /// @param h 初始窗口高度。
    /// @param wtitle 初始窗口标题。
    NativeWindow(int w, int h, const char* wtitle);

    /// @brief 析构主窗口并释放平台 frame adapter。
    ~NativeWindow();

    // 禁用拷贝和移动
    NativeWindow(NativeWindow&&)                 = delete;
    NativeWindow(const NativeWindow&)            = delete;
    NativeWindow& operator=(NativeWindow&&)      = delete;
    NativeWindow& operator=(const NativeWindow&) = delete;

    /// @brief 判断窗口是否收到关闭请求。
    /// @return 收到关闭请求时返回 true。
    bool shouldClose() const;

    /// @brief 轮询 GLFW 窗口事件。
    void pollEvents() const;  // 替换 update，窗口只负责处理事件

    /// @brief 获取 GLFW 主窗口句柄。
    /// @return GLFW 窗口句柄；未创建时返回 nullptr。
    GLFWwindow* getWindowHandle() const;

    /// @brief 获取 framebuffer 尺寸，用于 swapchain 重建。
    /// @param width 输出 framebuffer 宽度。
    /// @param height 输出 framebuffer 高度。
    void getFramebufferSize(int& width, int& height) const;

    /// @brief 从当前资源目录重新加载主窗口图标。
    /// @warning 启动低频路径：会读取并解码图标文件，禁止放入每帧渲染路径。
    void reloadWindowIcon();

    /// @brief 按逻辑尺寸调整窗口并在当前显示器工作区居中。
    /// @param width 目标逻辑宽度。
    /// @param height 目标逻辑高度。
    /// @warning 启动低频路径：会修改原生窗口矩形并触发交换链重建请求。
    void resizeAndCenter(int width, int height);

    /// @brief 获取窗口位置、尺寸和最大化状态。
    /// @param x 输出窗口左上角 X 坐标。
    /// @param y 输出窗口左上角 Y 坐标。
    /// @param width 输出窗口宽度。
    /// @param height 输出窗口高度。
    /// @param maximized 输出窗口是否最大化。
    void getWindowPlacement(int& x, int& y, int& width, int& height,
                            bool& maximized) const;

    /// @brief 应用窗口位置、尺寸和最大化状态。
    /// @param x 目标窗口左上角 X 坐标。
    /// @param y 目标窗口左上角 Y 坐标。
    /// @param width 目标窗口宽度。
    /// @param height 目标窗口高度。
    /// @param maximized 是否恢复为最大化窗口。
    void applyWindowPlacement(int x, int y, int width, int height,
                              bool maximized);

    /// @brief 切换窗口最大化或还原状态。
    void toggleMaximized();

    /// @brief 判断窗口是否处于原生或 macOS 模拟最大化状态。
    /// @return 当前窗口应被视为最大化时返回 true。
    [[nodiscard]] bool isWindowMaximized() const;

    /// @brief 获取无原生装饰窗口的平台适配器。
    /// @return 平台适配器观察指针；当前平台无适配器时返回 nullptr。
    [[nodiscard]] IWindowFrameAdapter* getWindowFrameAdapter() const;

    /// @brief 获取 GLFW 主窗口句柄。
    /// @return GLFW 窗口句柄；未创建时返回 nullptr。
    [[nodiscard]] GLFWwindow* getFrameWindowHandle() const override;

    /// @brief 获取普通窗口模式下的还原矩形。
    /// @param x 输出左上角 X 坐标。
    /// @param y 输出左上角 Y 坐标。
    /// @param width 输出窗口宽度。
    /// @param height 输出窗口高度。
    void getNormalFramePlacement(int& x, int& y, int& width,
                                 int& height) const override;

    /// @brief 写入普通窗口模式下的还原矩形。
    /// @param x 左上角 X 坐标。
    /// @param y 左上角 Y 坐标。
    /// @param width 窗口宽度。
    /// @param height 窗口高度。
    void setNormalFramePlacement(int x, int y, int width, int height) override;

    /// @brief 判断当前平台窗口是否处于最大化状态。
    /// @return 当前窗口应被视为最大化时返回 true。
    [[nodiscard]] bool isFrameMaximized() const override;

    /// @brief 为标题栏拖动还原最大化窗口，并保持鼠标相对窗口位置。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 成功从最大化状态还原时返回 true。
    /// @warning 输入低频路径：仅在标题栏拖动超过阈值后执行，会修改窗口矩形。
    bool restoreFrameForClientMove(double cursorX, double cursorY) override;

    /**
     * @brief 全屏
     */
    void ToggleFullscreen();

    /// @brief 判断窗口尺寸是否完成消抖并需要重建交换链。
    /// @warning 热路径/原子：渲染循环每帧读取 resize 标志；GLFW framebuffer
    /// 回调写入，只传递脏状态，使用 relaxed 避免额外同步成本。
    inline bool shouldRecreate() const
    {
        if ( !m_resizePending.load(std::memory_order_relaxed) ) return false;

        // 如果距离最后一次 resize 事件已经过去了 200ms，认为拖动已停止
        auto now      = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - m_lastResizeTime)
                            .count();

        if ( duration > 200 ) {
            m_resizePending.store(false, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    static void GLFW_KeyCallback(GLFWwindow* w, int key, int scancode,
                                 int action, int mods);

    static void GLFW_DropCallback(GLFWwindow* w, int count, const char** paths);

    /// @brief GLFW 窗口最小化/恢复回调入口。
    /// @param window GLFW 窗口句柄。
    /// @param iconified GLFW 最小化状态。
    static void GLFW_IconifyCallback(GLFWwindow* window, int iconified);

    static void framebufferResizeCallback(GLFWwindow* window, int w, int h);

    /// @brief 记录普通窗口移动后的可还原位置。
    /// @param window GLFW 窗口句柄。
    /// @param x 窗口左上角 X 坐标。
    /// @param y 窗口左上角 Y 坐标。
    static void windowPosCallback(GLFWwindow* window, int x, int y);

    /// @brief 记录普通窗口缩放后的可还原尺寸。
    /// @param window GLFW 窗口句柄。
    /// @param width 窗口宽度。
    /// @param height 窗口高度。
    static void windowSizeCallback(GLFWwindow* window, int width, int height);

private:
    /// @brief 判断当前窗口状态是否适合写入普通窗口还原矩形。
    /// @return 普通窗口模式下返回 true，最大化、最小化或全屏时返回 false。
    bool canRememberCurrentWindowPlacement() const;

    /// @brief 判断历史窗口尺寸是否疑似误保存了最大化后的工作区尺寸。
    /// @param width 历史窗口宽度。
    /// @param height 历史窗口高度。
    /// @return 尺寸贴近当前显示器工作区时返回 true。
    bool isLikelyMaximizedPlacement(int width, int height) const;

    /// @brief 从 GLFW 当前状态更新普通窗口还原矩形。
    void rememberCurrentWindowPlacement();

    /// @brief 程序化设置窗口位置和尺寸，不写入普通窗口还原矩形。
    /// @param x 目标窗口左上角 X 坐标。
    /// @param y 目标窗口左上角 Y 坐标。
    /// @param width 目标窗口宽度。
    /// @param height 目标窗口高度。
    void setWindowPlacementWithoutRemembering(int x, int y, int width,
                                              int height);

    /// @brief 在 macOS 上应用无边框窗口的模拟最大化矩形。
    /// @return 成功进入模拟最大化状态时返回 true。
    bool applyEmulatedMaximizedPlacement();

    /// @brief 刷新无原生装饰窗口的平台外形。
    void refreshWindowFrameShape();

    /// @brief 写入普通窗口还原矩形。
    /// @param x 窗口左上角 X 坐标。
    /// @param y 窗口左上角 Y 坐标。
    /// @param width 窗口宽度。
    /// @param height 窗口高度。
    void rememberWindowPlacement(int x, int y, int width, int height);

    /// @brief 处理窗口最小化/恢复事件，并在任务栏恢复时保留最大化状态。
    /// @param iconified GLFW 最小化状态。
    /// @warning 低频平台事件：仅由 GLFW iconify 回调触发，恢复时可能调用
    /// glfwMaximizeWindow 纠正 Windows 对无边框最大化窗口的普通还原。
    void handleWindowIconify(int iconified);

    GLFWwindow*                           m_windowHandle{ nullptr };
    std::chrono::steady_clock::time_point m_lastResizeTime;
    /// @brief 是否有待消抖的窗口尺寸变化。
    /// @warning 热路径/原子：由 GLFW 回调写入、渲染循环读取；只表示 resize
    /// 脏位，不同步其他数据。
    mutable std::atomic<bool> m_resizePending{ false };
    static double             s_lastMouseX;
    static double             s_lastMouseY;
    static bool               s_firstMouse;
    /// @brief 最近一次普通窗口模式下的左上角位置，用于最大化后的还原。
    int m_normalWindowPos[2] = { 100, 100 };

    /// @brief 最近一次普通窗口模式下的尺寸，用于最大化后的还原。
    int m_normalWindowSize[2] = { 1400, 900 };

    /// @brief 全屏切换前的窗口位置备份。
    int m_backupPos[2] = { 100, 100 };

    /// @brief 全屏切换前的窗口尺寸备份。
    int m_backupSize[2] = { 1400, 900 };

    /// @brief 最小化前窗口是否最大化，用于任务栏恢复时重新应用最大化。
    bool m_restoreMaximizedAfterIconify{ false };

    /// @brief 最近一次由用户或系统请求的主窗口最大化状态。
    bool m_lastRequestedMaximized{ false };

    /// @brief 是否正在忽略程序化窗口位置或尺寸回调。
    bool m_ignoreWindowPlacementCallbacks{ false };

    /// @brief macOS 无边框窗口是否处于应用层模拟最大化状态。
    bool m_emulatedMaximized{ false };

    /// @brief 无原生装饰窗口的平台行为适配器。
    std::unique_ptr<IWindowFrameAdapter> m_windowFrameAdapter;
};

}  // namespace MMM::Graphic
