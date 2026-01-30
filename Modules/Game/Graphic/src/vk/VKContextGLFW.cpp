#include "log/colorful-log.h"
#include "graphic/glfw/GLFWHeader.h"
#include "graphic/vk/VKContext.h"
#include <stdexcept>

namespace MMM
{
namespace Graphic
{
/*
 * 初始化GLFW窗口上下文
 * */
void VKContext::initGLFW()
{
    // 1.初始化GLFW
    if ( !glfwInit() ) {
        // !此处可能退出
        throw std::runtime_error("GLFW init failed");
    };
    XINFO("GLFW initialized successfully.");

    // 1.1设置GLFW为NOAPI来适配vulkan
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // 1.2检查 GLFW 的 Vulkan 是否被支持
    if ( !glfwVulkanSupported() ) {
        // 释放GLFW
        releaseGLFW();
        // !此处可能退出
        throw std::runtime_error(
            "Fatal: GLFW reports that Vulkan is not supported!");
    }
    XINFO("GLFW Vulkan is supported.");
}

/*
 * 注册GLFW窗口扩展
 * */
void VKContext::registerGLFWExtentions()
{
    // 1.3获取 GLFW 需要的扩展
    uint32_t     glfwExtensionCount = 0;
    const char** glfwExtensions{ nullptr };
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if ( glfwExtensions == nullptr ) {
        releaseGLFW();
        // !此处可能退出
        throw std::runtime_error(
            "Fatal: Failed to get required GLFW extensions.");
    }

    XINFO("Required GLFW extensions:");
    // 1.4将 C 风格的字符串数组放入注册的s_vkExtensions(std::vector)
    for ( int i{ 0 }; i < glfwExtensionCount; ++i ) {
        auto glfwExtension = glfwExtensions[i];
        m_vkExtensions.push_back(glfwExtension);
        XINFO("  - {}", glfwExtension);
    }

    // 启用vk的debug工具扩展
    // requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
}

/*
 * 释放GLFW
 * */
void VKContext::releaseGLFW()
{
    glfwTerminate();
    XINFO("GLFW Terminated.");
}

}  // namespace Graphic

}  // namespace MMM
