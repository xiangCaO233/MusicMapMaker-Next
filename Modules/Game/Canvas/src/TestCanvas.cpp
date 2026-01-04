#include "TestCanvas.h"
#include "CanvasDefs.h"
#include "colorful-log.h"
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.hpp>

namespace MMM
{
namespace Canvas
{
// 定义内部实现类，持有 GLFW上下文和GLFWwindow
struct TestCanvas::GLFWContextImpl
{
    class WindowContext
    {
    public:
        WindowContext()
        {
            if ( !glfwInit() )
            {
                XERROR("GLFW Init Failed");
                return;
            }
        }
        ~WindowContext() { glfwTerminate(); }
    };
    WindowContext m_context;
    GLFWwindow*   m_windowHandle = nullptr;
};

TestCanvas::TestCanvas(int w, int h)
    : m_glfwImplPtr(std::make_unique<GLFWContextImpl>())
{

    m_glfwImplPtr->m_windowHandle =
        glfwCreateWindow(w, h, "MusicMapMaker", nullptr, nullptr);
}

TestCanvas::~TestCanvas()
{
    if ( m_glfwImplPtr->m_windowHandle )
    {
        glfwDestroyWindow(m_glfwImplPtr->m_windowHandle);
    }
}

bool TestCanvas::shouldClose()
{
    return m_glfwImplPtr->m_windowHandle &&
           glfwWindowShouldClose(m_glfwImplPtr->m_windowHandle);
}

// 核心修改：将具体的 GLFW 操作封装在这里
void TestCanvas::update()
{
    if ( m_glfwImplPtr->m_windowHandle )
    {
        glfwPollEvents();
        glfwSwapBuffers(m_glfwImplPtr->m_windowHandle);
    }
}
}  // namespace Canvas
}  // namespace MMM
