#pragma once

#include "graphic/glfw/GLFWHeader.h"
#include <atomic>
#include <chrono>
#include <memory>

namespace MMM::Graphic
{
class Win32WindowAdapter;

class NativeWindow
{

public:
    NativeWindow(int w, int h, const char* wtitle);
    ~NativeWindow();

    // 禁用拷贝和移动
    NativeWindow(NativeWindow&&)                 = delete;
    NativeWindow(const NativeWindow&)            = delete;
    NativeWindow& operator=(NativeWindow&&)      = delete;
    NativeWindow& operator=(const NativeWindow&) = delete;

    bool shouldClose() const;
    void pollEvents() const;  // 替换 update，窗口只负责处理事件

    GLFWwindow* getWindowHandle() const;

    // 获取窗口宽高，用于 Swapchain 重建
    void getFramebufferSize(int& width, int& height) const;

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

    /// @brief 写入普通窗口还原矩形。
    /// @param x 窗口左上角 X 坐标。
    /// @param y 窗口左上角 Y 坐标。
    /// @param width 窗口宽度。
    /// @param height 窗口高度。
    void rememberWindowPlacement(int x, int y, int width, int height);

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
    int m_normalWindowSize[2] = { 1280, 720 };

    /// @brief 全屏切换前的窗口位置备份。
    int m_backupPos[2] = { 100, 100 };

    /// @brief 全屏切换前的窗口尺寸备份。
    int m_backupSize[2] = { 1280, 720 };

#ifdef _WIN32
    std::unique_ptr<Win32WindowAdapter>
        m_win32Adapter;  ///< Win32 窗口事件适配器
#endif
};

}  // namespace MMM::Graphic
