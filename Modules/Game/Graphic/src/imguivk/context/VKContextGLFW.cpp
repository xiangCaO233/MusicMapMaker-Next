#include "graphic/glfw/GLFWHeader.h"
#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{
/**
 * @brief 初始化 GLFW 上下文
 */
void VKContext::initGLFW()
{
    // 1.初始化GLFW
    if ( !glfwInit() ) {
        // !此处可能退出
        throw std::runtime_error("GLFW init failed");
    };
    XDEBUG("GLFW initialized successfully.");

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
    XDEBUG("GLFW Vulkan is supported.");
}

/**
 * @brief 注册 GLFW 所需的 Vulkan 扩展
 */
void VKContext::registerGLFWExtensions()
{
    // 1.3获取 GLFW 需要的扩展
    uint32_t     glfwExtensionCount = 0;
    const char** glfwExtensions{ nullptr };
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if ( glfwExtensions == nullptr ) {
        const char* description;
        int         code = glfwGetError(&description);
        if ( description ) {
            // 打印出具体的错误原因
            XCRITICAL("GLFW Error {} : {}", code, description);
        }
        releaseGLFW();
        // !此处可能退出
        throw std::runtime_error(
            "Fatal: Failed to get required GLFW extensions.");
    }

    XDEBUG("Required GLFW extensions:");
    // 1.4将 C 风格的字符串数组放入注册的s_vkExtensions(std::vector)
    for ( int i{ 0 }; i < glfwExtensionCount; ++i ) {
        auto glfwExtension = glfwExtensions[i];
        m_vkExtensions.push_back(glfwExtension);
        XDEBUG("  - {}", glfwExtension);
    }

#ifdef __APPLE__
    m_vkExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    XDEBUG("  - {}", VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    m_vkExtensions.push_back(
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    XDEBUG("  - {}", VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#endif  // __APPLE__

    // 启用vk的debug工具扩展
    // requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
}

/**
 * @brief 释放 GLFW 资源
 */
void VKContext::releaseGLFW()
{
    glfwTerminate();
    XDEBUG("GLFW Terminated.");
}

}  // namespace MMM::Graphic
