#include "TestCanvas.h"
#include "CanvasDefs.h"

#include "glfw/GLFWHeader.h"

namespace MMM
{
namespace Canvas
{

TestCanvas::TestCanvas(int w, int h, std::string_view windwo_title)
{
    // 创建窗口
    m_windowHandle =
        glfwCreateWindow(w, h, windwo_title.data(), nullptr, nullptr);
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

// 具体的 GLFW 操作封装在这
void TestCanvas::update()
{
    if ( m_windowHandle ) {
        glfwPollEvents();
        glfwSwapBuffers(m_windowHandle);
    }
}
}  // namespace Canvas
}  // namespace MMM
