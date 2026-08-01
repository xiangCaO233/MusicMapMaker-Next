#include "graphic/glfw/GLFWHeader.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "log/colorful-log.h"

#include <fmt/format.h>

#ifdef __APPLE__
#    include <cstdlib>
#    include <filesystem>
#    include <limits.h>
#    include <mach-o/dyld.h>
#endif

namespace MMM::Graphic
{

#ifdef __APPLE__
namespace
{
/// @brief 让 Vulkan loader 只使用应用束内的 MoltenVK 驱动清单。
/// @return 找到并配置应用束 ICD 清单时返回 true。
bool configureMacOSBundledVulkanDriver()
{
    char     executablePathBuffer[PATH_MAX];
    uint32_t executablePathSize = sizeof(executablePathBuffer);
    if ( _NSGetExecutablePath(executablePathBuffer, &executablePathSize) !=
         0 ) {
        return false;
    }

    std::filesystem::path driverManifest(executablePathBuffer);
    driverManifest = driverManifest.parent_path().parent_path() / "Resources" /
                     "vulkan" / "icd.d" / "MoltenVK_icd.json";

    std::error_code fileError;
    if ( !std::filesystem::is_regular_file(driverManifest, fileError) ||
         fileError ) {
        return false;
    }

    const std::string driverManifestPath = driverManifest.string();
    if ( setenv("VK_DRIVER_FILES", driverManifestPath.c_str(), 1) != 0 ) {
        return false;
    }

    XDEBUG("Using bundled Vulkan driver manifest: {}", driverManifestPath);
    return true;
}
}  // namespace
#endif

/**
 * @brief 初始化 GLFW 上下文
 */
void VKContext::initGLFW()
{
    glfwSetErrorCallback(glfw_error_callback);

#ifdef __APPLE__
    // macOS 开发构建的 Vulkan loader 可能位于 Homebrew 或 SDK 目录；
    // 显式复用已链接入口，避免 GLFW 按标准动态库名二次查找失败。
    configureMacOSBundledVulkanDriver();
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
#endif

    // 1.初始化GLFW
    if ( !glfwInit() ) {
        collectLastGLFWErrorDiagnostic("glfwInit failed.");
        logStartupDiagnostics("glfwInit failed.");
        failInitialization("GLFW init failed");
        return;
    };
    XDEBUG("GLFW initialized successfully.");
    collectGLFWDiagnostics();

    // 1.1设置GLFW为NOAPI来适配vulkan
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // 1.2检查 GLFW 的 Vulkan 是否被支持
    if ( !glfwVulkanSupported() ) {
        collectLastGLFWErrorDiagnostic("glfwVulkanSupported failed.");
        collectVulkanLoaderDiagnostics();
        logStartupDiagnostics("GLFW reports that Vulkan is not supported.");
        // 释放GLFW
        releaseGLFW();
        failInitialization("Fatal: GLFW reports that Vulkan is not supported!");
        return;
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
        collectLastGLFWErrorDiagnostic(
            "glfwGetRequiredInstanceExtensions failed.");
        addStartupDiagnostic(
            fmt::format("GLFW required Vulkan extension "
                        "count: {}",
                        glfwExtensionCount));
        collectVulkanLoaderDiagnostics();
        logStartupDiagnostics(
            "glfwGetRequiredInstanceExtensions returned null.");
        releaseGLFW();
        failInitialization(
            "Fatal: Failed to get required GLFW "
            "extensions for window surface creation.");
        return;
    }

    XDEBUG("Required GLFW extensions:");
    // 1.4将 C 风格的字符串数组放入注册的s_vkExtensions(std::vector)
    for ( int i{ 0 }; i < glfwExtensionCount; ++i ) {
        auto glfwExtension = glfwExtensions[i];
        m_vkExtensions.push_back(glfwExtension);
        XDEBUG("  - {}", glfwExtension);
    }
    addStartupDiagnostic(
        fmt::format("GLFW required Vulkan extensions: {}", glfwExtensionCount));
    for ( int i{ 0 }; i < glfwExtensionCount; ++i ) {
        addStartupDiagnostic(
            fmt::format("GLFW required extension: {}", glfwExtensions[i]));
    }
    collectVulkanLoaderDiagnostics();

#ifdef __APPLE__
    m_vkExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    XDEBUG("  - {}", VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    m_vkExtensions.push_back(
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    XDEBUG("  - {}", VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#endif  // __APPLE__

    // debug 工具扩展由验证层配置流程统一处理。
}

/**
 * @brief 释放 GLFW 资源
 */
void VKContext::releaseGLFW()
{
    VKRenderer::releaseGlfwCursorResources();
    glfwTerminate();
    XDEBUG("GLFW Terminated.");
}

}  // namespace MMM::Graphic
