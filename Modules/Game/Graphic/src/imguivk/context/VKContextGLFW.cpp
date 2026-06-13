#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace MMM::Graphic
{
namespace
{
/// @brief 判断 Vulkan instance 扩展列表中是否存在指定扩展。
/// @param extensions Vulkan loader 返回的 instance 扩展列表。
/// @param extensionName 需要查找的扩展名。
/// @return 找到指定扩展时返回 true。
bool hasVulkanInstanceExtension(
    const std::vector<VkExtensionProperties>& extensions,
    const char*                               extensionName)
{
    return std::any_of(extensions.begin(),
                       extensions.end(),
                       [extensionName](const VkExtensionProperties& extension) {
                           return std::strcmp(extension.extensionName,
                                              extensionName) == 0;
                       });
}

/// @brief 打印 GLFW 最近一次错误，便于定位初始化失败原因。
/// @param context 当前检查 GLFW 错误的上下文描述。
void logLastGlfwError(const char* context)
{
    const char* description = nullptr;
    const int   code        = glfwGetError(&description);
    if ( description ) {
        XERROR("{} GLFW error {}: {}", context, code, description);
        return;
    }

    XERROR("{} GLFW did not report a concrete error. Last error code: {}",
           context,
           code);
}

/// @brief 记录 Vulkan loader 暴露的窗口表面相关扩展状态。
/// @param extensions Vulkan loader 返回的 instance 扩展列表。
void logVulkanSurfaceExtensionStatus(
    const std::vector<VkExtensionProperties>& extensions)
{
    constexpr const char* surfaceExtension = "VK_KHR_surface";

#if defined(_WIN32)
    constexpr const char* platformSurfaceExtension = "VK_KHR_win32_surface";
#elif defined(__APPLE__)
    constexpr const char* platformSurfaceExtension = "VK_EXT_metal_surface";
#elif defined(__linux__)
    constexpr const char* platformSurfaceExtension =
        "VK_KHR_xcb_surface / VK_KHR_xlib_surface / VK_KHR_wayland_surface";
#else
    constexpr const char* platformSurfaceExtension = "platform surface";
#endif

    const bool hasSurface =
        hasVulkanInstanceExtension(extensions, surfaceExtension);

#if defined(__linux__)
    const bool hasPlatformSurface =
        hasVulkanInstanceExtension(extensions, "VK_KHR_xcb_surface") ||
        hasVulkanInstanceExtension(extensions, "VK_KHR_xlib_surface") ||
        hasVulkanInstanceExtension(extensions, "VK_KHR_wayland_surface");
#else
    const bool hasPlatformSurface =
        hasVulkanInstanceExtension(extensions, platformSurfaceExtension);
#endif

    XERROR("Vulkan WSI extension status: {}={}, {}={}",
           surfaceExtension,
           hasSurface,
           platformSurfaceExtension,
           hasPlatformSurface);

    if ( hasSurface && hasPlatformSurface ) {
        return;
    }

#if defined(_WIN32)
    XERROR(
        "Windows Vulkan loader is missing a required window-surface "
        "extension. Update/reinstall the GPU driver or Vulkan Runtime, and "
        "check for an incorrect vulkan-1.dll near the executable.");
#else
    XERROR(
        "Vulkan loader is missing the platform window-surface extension "
        "required by GLFW.");
#endif
}

/// @brief 枚举并记录 Vulkan instance 扩展，用于诊断 GLFW surface 初始化失败。
void logVulkanInstanceExtensionsForDiagnostics()
{
    uint32_t extensionCount = 0;
    VkResult result         = vkEnumerateInstanceExtensionProperties(
        nullptr, &extensionCount, nullptr);
    if ( result != VK_SUCCESS ) {
        XERROR("Failed to enumerate Vulkan instance extensions. VkResult={}",
               static_cast<int>(result));
        return;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    if ( extensionCount > 0 ) {
        result = vkEnumerateInstanceExtensionProperties(
            nullptr, &extensionCount, extensions.data());
        if ( result != VK_SUCCESS ) {
            XERROR("Failed to read Vulkan instance extension list. VkResult={}",
                   static_cast<int>(result));
            return;
        }
    }

    XERROR("Vulkan loader reported {} instance extension(s):", extensionCount);
    for ( const auto& extension : extensions ) {
        XERROR("  - {}", extension.extensionName);
    }

    logVulkanSurfaceExtensionStatus(extensions);
}
}  // namespace

/**
 * @brief 初始化 GLFW 上下文
 */
void VKContext::initGLFW()
{
    glfwSetErrorCallback(glfw_error_callback);

    // 1.初始化GLFW
    if ( !glfwInit() ) {
        logLastGlfwError("glfwInit failed.");
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
        logLastGlfwError("glfwGetRequiredInstanceExtensions failed.");
        XERROR("GLFW required Vulkan extension count: {}", glfwExtensionCount);
        logVulkanInstanceExtensionsForDiagnostics();
        releaseGLFW();
        // !此处可能退出
        throw std::runtime_error(
            "Fatal: Failed to get required GLFW "
            "extensions for window surface creation.");
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
