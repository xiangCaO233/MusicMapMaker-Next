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

    /**
     * @brief 全屏
     */
    void ToggleFullscreen();

    inline bool shouldRecreate() const
    {
        if ( !m_resizePending ) return false;

        // 如果距离最后一次 resize 事件已经过去了 200ms，认为拖动已停止
        auto now      = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - m_lastResizeTime)
                            .count();

        if ( duration > 200 ) {
            m_resizePending = false;
            return true;
        }
        return false;
    }

    static void GLFW_KeyCallback(GLFWwindow* w, int key, int scancode,
                                 int action, int mods);

    static void GLFW_DropCallback(GLFWwindow* w, int count, const char** paths);

    static void framebufferResizeCallback(GLFWwindow* window, int w, int h);

private:
    GLFWwindow*                           m_windowHandle{ nullptr };
    std::chrono::steady_clock::time_point m_lastResizeTime;
    mutable std::atomic<bool>             m_resizePending{ false };
    static double                         s_lastMouseX;
    static double                         s_lastMouseY;
    static bool                           s_firstMouse;
    int m_backupPos[2]  = { 100, 100 };   // 默认备份位置
    int m_backupSize[2] = { 1280, 720 };  // 默认备份尺寸

#ifdef _WIN32
    std::unique_ptr<Win32WindowAdapter>
        m_win32Adapter;  ///< Win32 窗口事件适配器
#endif
};

}  // namespace MMM::Graphic
