#pragma once

#include "graphic/glfw/GLFWHeader.h"
#include <atomic>
#include <chrono>

namespace MMM::Graphic
{
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

    static void framebufferResizeCallback(GLFWwindow* window, int w, int h);

private:
    GLFWwindow*                           m_windowHandle{ nullptr };
    std::chrono::steady_clock::time_point m_lastResizeTime;
    mutable std::atomic<bool>             m_resizePending{ false };
};

}  // namespace MMM::Graphic
