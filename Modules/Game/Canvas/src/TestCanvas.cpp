#include "TestCanvas.h"
#include "CanvasDefs.h"
#include "colorful-log.h"

#include <vulkan/vulkan.hpp>

#include <GLFW/glfw3.h>

namespace MMM
{
namespace Canvas
{
// 定义内部实现类，持有 GLFW上下文和GLFWwindow
struct TestCanvas::ContextImpl {
    class WindowContext
    {
    public:
        WindowContext()
        {
            if ( !glfwInit() ) {
                XERROR("GLFW Init Failed");
                return;
            }
            glfwSetErrorCallback([](int error, const char* description) {
                XERROR("GLFW Error %d: %s", error, description);
            });

            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        }
        ~WindowContext() { glfwTerminate(); }
    };
    WindowContext m_context;
    GLFWwindow*   m_windowHandle = nullptr;

    ContextImpl(int w, int h)
    {
        m_windowHandle =
            glfwCreateWindow(w, h, "MusicMapMaker", nullptr, nullptr);
        if ( !m_windowHandle ) {
            glfwTerminate();
            throw std::runtime_error("Failed to create GLFW window");
        }

        if ( !glfwVulkanSupported() ) {
            glfwDestroyWindow(m_windowHandle);
            glfwTerminate();
            throw std::runtime_error("GLFW: Vulkan Not Supported");
        }

    }
    ~ContextImpl()
    {
    }
};

TestCanvas::TestCanvas(int w, int h)
    : m_CtxImplPtr(std::make_unique<ContextImpl>(w, h))
{

}

TestCanvas::~TestCanvas() {}

bool TestCanvas::shouldClose()
{
    return m_CtxImplPtr->m_windowHandle &&
           glfwWindowShouldClose(m_CtxImplPtr->m_windowHandle);
}

// 具体的 GLFW 操作封装在这
void TestCanvas::update()
{
    if ( m_CtxImplPtr->m_windowHandle ) {
        glfwPollEvents();
        glfwSwapBuffers(m_CtxImplPtr->m_windowHandle);
    }
}
}  // namespace Canvas
}  // namespace MMM
