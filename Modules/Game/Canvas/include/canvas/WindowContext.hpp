#ifndef MMM_WINDOWCONTEXT_H
#define MMM_WINDOWCONTEXT_H

#include <GLFW/glfw3.h>

namespace MMM
{
class WindowContext
{
public:
    WindowContext() { glfwInit(); }
    ~WindowContext() { glfwTerminate(); }
};
}  // namespace MMM
#endif  // !MMM_WINDOWCONTEXT_H
