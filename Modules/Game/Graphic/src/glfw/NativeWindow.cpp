#include "graphic/glfw/window/NativeWindow.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{
NativeWindow::NativeWindow(int w, int h, const char* wtitle)
{
    if ( !glfwVulkanSupported() ) {
        XERROR("GLFW: Vulkan Not Supported");
    }
    // 不再需要初始化 ImGui 的辅助窗口
    m_windowHandle = glfwCreateWindow(w, h, wtitle, nullptr, nullptr);

    // 设置用户指针，方便回调函数访问类成员
    glfwSetWindowUserPointer(m_windowHandle, this);
    glfwSetFramebufferSizeCallback(m_windowHandle,
                                   &NativeWindow::framebufferResizeCallback);
}

NativeWindow::~NativeWindow()
{
    if ( m_windowHandle ) {
        glfwDestroyWindow(m_windowHandle);
    }
}

void NativeWindow::framebufferResizeCallback(GLFWwindow* window, int w, int h)
{
    auto app =
        reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(window));
    app->m_framebufferResized = true;
}

bool NativeWindow::shouldClose() const
{
    return m_windowHandle && glfwWindowShouldClose(m_windowHandle);
}

void NativeWindow::pollEvents() const
{
    glfwPollEvents();  // 处理操作系统的鼠标、键盘、关闭等事件
}

GLFWwindow* NativeWindow::getWindowHandle() const
{
    return m_windowHandle;
}

void NativeWindow::getFramebufferSize(int& width, int& height) const
{
    glfwGetFramebufferSize(m_windowHandle, &width, &height);
}
}  // namespace MMM::Graphic
