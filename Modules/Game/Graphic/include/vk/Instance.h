#pragma once

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_handles.hpp>
#include <vulkan/vulkan_structs.hpp>

#include "glfw/GLFWHeader.h"
#include "vk/VKExtensions.h"

namespace MMM
{
namespace Graphic
{

/*
 * 图形Vulkan上下文
 * */
class VKContext
{
public:
    VKContext();
    ~VKContext();

    VKContext(VKContext&&)                 = delete;
    VKContext(const VKContext&)            = delete;
    VKContext& operator=(VKContext&&)      = delete;
    VKContext& operator=(const VKContext&) = delete;

private:
    /*
     * glfw是否已初始化
     * */
    static bool         s_glfwInitialized;
    static vk::Instance s_vkInstance;
    static VKExtensions s_vkExtensions;
};



}  // namespace Graphic

}  // namespace MMM
