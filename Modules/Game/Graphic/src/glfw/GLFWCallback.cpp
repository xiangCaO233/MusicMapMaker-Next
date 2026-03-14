#include "graphic/glfw/GLFWHeader.h"
#include "log/colorful-log.h"

namespace MMM
{
namespace Graphic
{
void glfw_error_callback(int error, const char* description)
{
    XERROR("GLFW Error {}: {}", error, description);
}
}  // namespace Graphic

}  // namespace MMM
