#pragma once

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace MMM
{
namespace Graphic
{
void glfw_error_callback(int error, const char* description);
}
}  // namespace MMM
