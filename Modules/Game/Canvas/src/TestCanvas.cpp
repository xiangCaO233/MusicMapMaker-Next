#include "canvas/TestCanvas.h"
#include "canvas/CanvasDefs.h"

#include "graphic/vk/VKContext.h"
#include "graphic/glfw/GLFWHeader.h"

namespace MMM
{
namespace Canvas
{

TestCanvas::TestCanvas(const int w, const int h, const std::string_view window_title)
{
    // 创建窗口
    m_windowHandle =
        glfwCreateWindow(w, h, window_title.data(), nullptr, nullptr);
}

TestCanvas::~TestCanvas()
{
    // 释放窗口
    if ( m_windowHandle ) {
        glfwDestroyWindow(m_windowHandle);
    }
}

bool TestCanvas::shouldClose()
{
    return m_windowHandle && glfwWindowShouldClose(m_windowHandle);
}

// 获取窗口句柄
GLFWwindow* TestCanvas::getWindowHandle() const {
    return m_windowHandle;
}

// 具体的 GLFW 操作封装在这
void TestCanvas::update(Graphic::VKContext& vkContext) const {
    auto& renderer = vkContext.getRenderer();
    glfwPollEvents();
    if ( m_windowHandle ) {
        // 更新窗口
        // 执行渲染
        renderer.render();
    }
}
}  // namespace Canvas
}  // namespace MMM
