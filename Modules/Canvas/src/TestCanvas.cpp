#include "GLFW/glfw3.h"
#include "colorful-log.h"
#include <CanvasDefs.h>
#include <TestCanvas.h>
#include <vulkan/vulkan.hpp>


namespace Canvas
{
TestCanvas::TestCanvas(int w, int h)
{
    auto res = glfwInit();
    if ( res == GLFW_FALSE )
    {
        XERROR("GLFW init failed!");
    }
    else
    {
        XINFO("GLFW init successed!");
        m_glfwWindowContext = glfwCreateWindow(w, h, "", nullptr, nullptr);
    }
}
TestCanvas::~TestCanvas()
{
    glfwTerminate();
}
}  // namespace Canvas
