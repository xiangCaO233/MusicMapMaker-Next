#pragma once

#include "graphic/glfw/GLFWHeader.h"

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

    bool isFramebufferResized() const { return m_framebufferResized; }
    void resetFramebufferResized() { m_framebufferResized = false; }

    static void framebufferResizeCallback(GLFWwindow* window, int w, int h);

private:
    bool        m_framebufferResized{ false };
    GLFWwindow* m_windowHandle{ nullptr };
};

}  // namespace MMM::Graphic
