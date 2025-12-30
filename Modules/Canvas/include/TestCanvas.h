#include "CanvasDefs.h"
#include <GLFW/glfw3.h>
#include <imgui.h>

namespace Canvas
{
class TestCanvas
{
public:
    TestCanvas(int w, int h);
    ~TestCanvas();

private:
    GLFWwindow* m_glfwWindowContext;
};
}  // namespace Canvas
