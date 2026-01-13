#pragma once

#include <string>
#include <vector>

#include "glfw/GLFWHeader.h"

namespace MMM
{
namespace Graphic
{
class VKExtensions
{
public:
    VKExtensions();
    ~VKExtensions();

    VKExtensions(VKExtensions&&)                 = delete;
    VKExtensions(const VKExtensions&)            = delete;
    VKExtensions& operator=(VKExtensions&&)      = delete;
    VKExtensions& operator=(const VKExtensions&) = delete;

private:
    std::vector<std::string> m_extentionNames;
};
}  // namespace Graphic

}  // namespace MMM
