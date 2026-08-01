#include "graphic/glfw/GLFWHeader.h"
#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <fmt/format.h>
#include <string_view>
#include <utility>

namespace MMM::Graphic
{
namespace
{
/// @brief 将布尔值格式化为诊断日志中的 yes/no。
/// @param value 需要格式化的布尔值。
/// @return yes 或 no 文本。
constexpr const char* yesNo(bool value)
{
    return value ? "yes" : "no";
}

/// @brief 将 Vulkan 版本号格式化为 major.minor.patch。
/// @param version Vulkan 编码版本号。
/// @return 可读版本号文本。
std::string formatVulkanVersion(uint32_t version)
{
    return fmt::format("{}.{}.{}",
                       VK_VERSION_MAJOR(version),
                       VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

/// @brief 将 VkResult 格式化为数值诊断文本。
/// @param result Vulkan 结果码。
/// @return 可读结果码文本。
std::string formatVkResult(VkResult result)
{
    return fmt::format("{} ({})",
                       vk::to_string(static_cast<vk::Result>(result)),
                       static_cast<int>(result));
}

/// @brief 将物理设备类型格式化为短文本。
/// @param type Vulkan 物理设备类型。
/// @return 设备类型文本。
const char* physicalDeviceTypeName(VkPhysicalDeviceType type)
{
    switch ( type ) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "other";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "unknown";
    }
}

/// @brief 将队列能力位格式化为短文本。
/// @param flags Vulkan 队列能力位。
/// @return 队列能力文本。
std::string queueFlagsText(VkQueueFlags flags)
{
    std::string result;
    const auto  append = [&result](std::string_view flag) {
        if ( !result.empty() ) {
            result += "|";
        }
        result += flag;
    };

    if ( (flags & VK_QUEUE_GRAPHICS_BIT) != 0 ) append("graphics");
    if ( (flags & VK_QUEUE_COMPUTE_BIT) != 0 ) append("compute");
    if ( (flags & VK_QUEUE_TRANSFER_BIT) != 0 ) append("transfer");
    if ( (flags & VK_QUEUE_SPARSE_BINDING_BIT) != 0 ) append("sparse");
    if ( (flags & VK_QUEUE_PROTECTED_BIT) != 0 ) append("protected");

    if ( result.empty() ) {
        return "none";
    }
    return result;
}

/// @brief 将 Vulkan present mode 格式化为短文本。
/// @param mode Vulkan 呈现模式。
/// @return 呈现模式文本。
const char* presentModeName(VkPresentModeKHR mode)
{
    switch ( mode ) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default: return "other";
    }
}

/// @brief 检查扩展列表中是否存在指定扩展。
/// @param extensions Vulkan 扩展属性列表。
/// @param extensionName 需要查找的扩展名。
/// @return 找到扩展时返回 true。
bool hasExtension(const std::vector<VkExtensionProperties>& extensions,
                  const char*                               extensionName)
{
    return std::any_of(extensions.begin(),
                       extensions.end(),
                       [extensionName](const VkExtensionProperties& extension) {
                           return std::string_view(extension.extensionName) ==
                                  extensionName;
                       });
}

/// @brief 检查扩展列表中是否存在指定扩展。
/// @param extensions Vulkan-Hpp 扩展属性列表。
/// @param extensionName 需要查找的扩展名。
/// @return 找到扩展时返回 true。
bool hasExtension(const std::vector<vk::ExtensionProperties>& extensions,
                  const char*                                 extensionName)
{
    return std::any_of(
        extensions.begin(),
        extensions.end(),
        [extensionName](const vk::ExtensionProperties& extension) {
            return std::string_view(extension.extensionName) == extensionName;
        });
}

}  // namespace

void VKContext::addStartupDiagnostic(std::string line)
{
    m_startupDiagnosticLines.push_back(std::move(line));
}

void VKContext::collectGLFWDiagnostics()
{
    int major    = 0;
    int minor    = 0;
    int revision = 0;
    glfwGetVersion(&major, &minor, &revision);

    addStartupDiagnostic(fmt::format("GLFW version: {}.{}.{} ({})",
                                     major,
                                     minor,
                                     revision,
                                     glfwGetVersionString()));
    addStartupDiagnostic(
        fmt::format("GLFW platform code: {}", glfwGetPlatform()));

    const bool vulkanSupported = glfwVulkanSupported() == GLFW_TRUE;
    addStartupDiagnostic(fmt::format("GLFW Vulkan minimally supported: {}",
                                     yesNo(vulkanSupported)));
}

void VKContext::collectLastGLFWErrorDiagnostic(const char* context)
{
    const char* description = nullptr;
    const int   code        = glfwGetError(&description);
    if ( description ) {
        addStartupDiagnostic(
            fmt::format("{} GLFW error {}: {}", context, code, description));
        return;
    }

    addStartupDiagnostic(fmt::format(
        "{} GLFW did not report a concrete error. Last error code: {}",
        context,
        code));
}

void VKContext::collectVulkanLoaderDiagnostics()
{
#ifdef VK_HEADER_VERSION_COMPLETE
    addStartupDiagnostic(
        fmt::format("Vulkan header version: {} (VK_HEADER_VERSION={})",
                    formatVulkanVersion(VK_HEADER_VERSION_COMPLETE),
                    VK_HEADER_VERSION));
#else
    addStartupDiagnostic(
        fmt::format("Vulkan header patch version: {}", VK_HEADER_VERSION));
#endif

    uint32_t loaderVersion = VK_API_VERSION_1_0;
    auto     enumerateInstanceVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if ( enumerateInstanceVersion ) {
        const VkResult result = enumerateInstanceVersion(&loaderVersion);
        addStartupDiagnostic(fmt::format(
            "Vulkan loader instance API version query: result={}, version={}",
            formatVkResult(result),
            formatVulkanVersion(loaderVersion)));
        if ( result == VK_SUCCESS ) {
            XINFO("Vulkan loader API version: {}",
                  formatVulkanVersion(loaderVersion));
        }
    } else {
        addStartupDiagnostic(
            "Vulkan loader does not export "
            "vkEnumerateInstanceVersion; assuming 1.0.x.");
    }

    uint32_t extensionCount = 0;
    VkResult result         = vkEnumerateInstanceExtensionProperties(
        nullptr, &extensionCount, nullptr);
    if ( result != VK_SUCCESS ) {
        addStartupDiagnostic(
            fmt::format("Failed to enumerate Vulkan instance extensions: {}",
                        formatVkResult(result)));
        return;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    if ( extensionCount > 0 ) {
        result = vkEnumerateInstanceExtensionProperties(
            nullptr, &extensionCount, extensions.data());
        if ( result != VK_SUCCESS ) {
            addStartupDiagnostic(
                fmt::format("Failed to read Vulkan instance extension list: {}",
                            formatVkResult(result)));
            return;
        }
    }

    addStartupDiagnostic(
        fmt::format("Vulkan instance extensions reported: {}", extensionCount));
    for ( const auto& extension : extensions ) {
        addStartupDiagnostic(fmt::format("Instance extension: {} spec={}",
                                         extension.extensionName,
                                         extension.specVersion));
    }

    const bool hasSurface =
        hasExtension(extensions, VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(_WIN32)
    const bool hasPlatformSurface =
        hasExtension(extensions, "VK_KHR_win32_surface");
    constexpr const char* platformSurfaceName = "VK_KHR_win32_surface";
#elif defined(__APPLE__)
    const bool hasPlatformSurface =
        hasExtension(extensions, "VK_EXT_metal_surface");
    constexpr const char* platformSurfaceName = "VK_EXT_metal_surface";
#elif defined(__linux__)
    const bool hasPlatformSurface =
        hasExtension(extensions, "VK_KHR_xcb_surface") ||
        hasExtension(extensions, "VK_KHR_xlib_surface") ||
        hasExtension(extensions, "VK_KHR_wayland_surface");
    constexpr const char* platformSurfaceName =
        "VK_KHR_xcb_surface/VK_KHR_xlib_surface/VK_KHR_wayland_surface";
#else
    const bool            hasPlatformSurface  = false;
    constexpr const char* platformSurfaceName = "platform surface";
#endif
    addStartupDiagnostic(
        fmt::format("Vulkan WSI extension status: {}={}, {}={}",
                    VK_KHR_SURFACE_EXTENSION_NAME,
                    yesNo(hasSurface),
                    platformSurfaceName,
                    yesNo(hasPlatformSurface)));

    uint32_t layerCount = 0;
    result = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    if ( result != VK_SUCCESS ) {
        addStartupDiagnostic(
            fmt::format("Failed to enumerate Vulkan instance layers: {}",
                        formatVkResult(result)));
        return;
    }

    std::vector<VkLayerProperties> layers(layerCount);
    if ( layerCount > 0 ) {
        result = vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        if ( result != VK_SUCCESS ) {
            addStartupDiagnostic(
                fmt::format("Failed to read Vulkan instance layer list: {}",
                            formatVkResult(result)));
            return;
        }
    }

    addStartupDiagnostic(
        fmt::format("Vulkan instance layers reported: {}", layerCount));
    for ( const auto& layer : layers ) {
        addStartupDiagnostic(
            fmt::format("Instance layer: {} spec={} impl={} desc={}",
                        layer.layerName,
                        formatVulkanVersion(layer.specVersion),
                        layer.implementationVersion,
                        layer.description));
    }
}

void VKContext::collectVulkanInstanceCreateDiagnostics()
{
    addStartupDiagnostic(
        fmt::format("Requested Vulkan application API version: {}",
                    formatVulkanVersion(m_vkAppInfo.apiVersion)));
    addStartupDiagnostic(fmt::format("Requested Vulkan instance extensions: {}",
                                     m_vkExtensions.size()));
    for ( const char* extension : m_vkExtensions ) {
        addStartupDiagnostic(fmt::format("Requested instance extension: {}",
                                         extension ? extension : "<null>"));
    }

    if ( is_debug() ) {
        for ( const char* layer : m_vkValidationLayers ) {
            addStartupDiagnostic(
                fmt::format("Requested validation layer: {}", layer));
        }
    }
}

void VKContext::collectPhysicalDeviceDiagnostics(bool includeSurfaceSupport)
{
    if ( !m_vkInstance ) {
        addStartupDiagnostic(
            "Physical device diagnostics skipped: Vulkan instance is null.");
        return;
    }

    auto devicesResult = m_vkInstance.enumeratePhysicalDevices();
    if ( devicesResult.result != vk::Result::eSuccess ) {
        addStartupDiagnostic(
            fmt::format("Failed to enumerate physical devices: {}",
                        vk::to_string(devicesResult.result)));
        return;
    }

    const auto& devices = devicesResult.value;
    addStartupDiagnostic(
        fmt::format("Vulkan physical devices reported: {}", devices.size()));
    XINFO("Vulkan physical devices detected: {}", devices.size());

    for ( size_t deviceIndex = 0; deviceIndex < devices.size();
          ++deviceIndex ) {
        const VkPhysicalDevice rawDevice =
            static_cast<VkPhysicalDevice>(devices[deviceIndex]);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(rawDevice, &properties);

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(rawDevice, &features);

        addStartupDiagnostic(
            fmt::format("GPU[{}]: name=\"{}\", type={}, api={}, driverRaw={} "
                        "(0x{:08X}), vendor=0x{:04X}, device=0x{:04X}",
                        deviceIndex,
                        properties.deviceName,
                        physicalDeviceTypeName(properties.deviceType),
                        formatVulkanVersion(properties.apiVersion),
                        properties.driverVersion,
                        properties.driverVersion,
                        properties.vendorID,
                        properties.deviceID));
        addStartupDiagnostic(fmt::format(
            "GPU[{}] features: geometryShader={}, samplerAnisotropy={}, "
            "wideLines={}, fillModeNonSolid={}, sampleRateShading={}",
            deviceIndex,
            yesNo(features.geometryShader == VK_TRUE),
            yesNo(features.samplerAnisotropy == VK_TRUE),
            yesNo(features.wideLines == VK_TRUE),
            yesNo(features.fillModeNonSolid == VK_TRUE),
            yesNo(features.sampleRateShading == VK_TRUE)));

        auto extensionsResult =
            devices[deviceIndex].enumerateDeviceExtensionProperties();
        if ( extensionsResult.result == vk::Result::eSuccess ) {
            const auto& deviceExtensions = extensionsResult.value;
            addStartupDiagnostic(fmt::format(
                "GPU[{}] device extensions: count={}, {}={}, "
                "VK_KHR_portability_subset={}, {}={}",
                deviceIndex,
                deviceExtensions.size(),
                VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                yesNo(hasExtension(deviceExtensions,
                                   VK_KHR_SWAPCHAIN_EXTENSION_NAME)),
                yesNo(hasExtension(deviceExtensions,
                                   "VK_KHR_portability_subset")),
                VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
                yesNo(hasExtension(deviceExtensions,
                                   VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME))));

            const bool supportsDriverProperties =
                properties.apiVersion >= VK_API_VERSION_1_2 ||
                hasExtension(deviceExtensions,
                             VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
            if ( supportsDriverProperties ) {
                auto getProperties2 =
                    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                        vkGetInstanceProcAddr(
                            m_vkInstance, "vkGetPhysicalDeviceProperties2"));
                if ( !getProperties2 ) {
                    getProperties2 =
                        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                            vkGetInstanceProcAddr(
                                m_vkInstance,
                                "vkGetPhysicalDeviceProperties2KHR"));
                }

                if ( getProperties2 ) {
                    VkPhysicalDeviceDriverProperties driverProperties{};
                    driverProperties.sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
                    VkPhysicalDeviceProperties2 properties2{};
                    properties2.sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                    properties2.pNext = &driverProperties;
                    getProperties2(rawDevice, &properties2);

                    addStartupDiagnostic(fmt::format(
                        "GPU[{}] driver: id={}, name=\"{}\", info=\"{}\", "
                        "conformance={}.{}.{}.{}",
                        deviceIndex,
                        static_cast<int>(driverProperties.driverID),
                        driverProperties.driverName,
                        driverProperties.driverInfo,
                        static_cast<int>(
                            driverProperties.conformanceVersion.major),
                        static_cast<int>(
                            driverProperties.conformanceVersion.minor),
                        static_cast<int>(
                            driverProperties.conformanceVersion.subminor),
                        static_cast<int>(
                            driverProperties.conformanceVersion.patch)));
                } else {
                    addStartupDiagnostic(fmt::format(
                        "GPU[{}] driver properties supported, but "
                        "vkGetPhysicalDeviceProperties2 is unavailable.",
                        deviceIndex));
                }
            } else {
                addStartupDiagnostic(fmt::format(
                    "GPU[{}] driver properties extension is not supported.",
                    deviceIndex));
            }
        } else {
            addStartupDiagnostic(
                fmt::format("GPU[{}] failed to enumerate device extensions: {}",
                            deviceIndex,
                            vk::to_string(extensionsResult.result)));
        }

        const auto queueFamilies =
            devices[deviceIndex].getQueueFamilyProperties();
        addStartupDiagnostic(fmt::format(
            "GPU[{}] queue families: {}", deviceIndex, queueFamilies.size()));
        for ( uint32_t queueIndex = 0;
              queueIndex < static_cast<uint32_t>(queueFamilies.size());
              ++queueIndex ) {
            std::string presentText = "not-queried";
            if ( includeSurfaceSupport && m_vkSurface ) {
                auto presentResult = devices[deviceIndex].getSurfaceSupportKHR(
                    queueIndex, m_vkSurface);
                presentText = presentResult.result == vk::Result::eSuccess
                                  ? yesNo(presentResult.value == VK_TRUE)
                                  : vk::to_string(presentResult.result);
            }

            addStartupDiagnostic(
                fmt::format("GPU[{}] queue[{}]: count={}, flags={}, present={}",
                            deviceIndex,
                            queueIndex,
                            queueFamilies[queueIndex].queueCount,
                            queueFlagsText(static_cast<VkQueueFlags>(
                                queueFamilies[queueIndex].queueFlags)),
                            presentText));
        }
    }
}

void VKContext::collectSelectedSurfaceDiagnostics(int width, int height)
{
    if ( !m_vkPhysicalDevice || !m_vkSurface ) {
        addStartupDiagnostic(
            "Selected surface diagnostics skipped: device or surface is null.");
        return;
    }

    addStartupDiagnostic(
        fmt::format("Requested framebuffer extent: {}x{}", width, height));

    VkSurfaceCapabilitiesKHR capabilities{};
    VkResult                 result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        static_cast<VkPhysicalDevice>(m_vkPhysicalDevice),
        static_cast<VkSurfaceKHR>(m_vkSurface),
        &capabilities);
    if ( result == VK_SUCCESS ) {
        addStartupDiagnostic(fmt::format(
            "Surface capabilities: minImages={}, maxImages={}, "
            "currentExtent={}x{}, minExtent={}x{}, maxExtent={}x{}, "
            "currentTransform=0x{:X}",
            capabilities.minImageCount,
            capabilities.maxImageCount,
            capabilities.currentExtent.width,
            capabilities.currentExtent.height,
            capabilities.minImageExtent.width,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.width,
            capabilities.maxImageExtent.height,
            static_cast<uint32_t>(capabilities.currentTransform)));
    } else {
        addStartupDiagnostic(
            fmt::format("Failed to query surface capabilities: {}",
                        formatVkResult(result)));
    }

    uint32_t formatCount = 0;
    result               = vkGetPhysicalDeviceSurfaceFormatsKHR(
        static_cast<VkPhysicalDevice>(m_vkPhysicalDevice),
        static_cast<VkSurfaceKHR>(m_vkSurface),
        &formatCount,
        nullptr);
    if ( result == VK_SUCCESS ) {
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        if ( formatCount > 0 ) {
            result = vkGetPhysicalDeviceSurfaceFormatsKHR(
                static_cast<VkPhysicalDevice>(m_vkPhysicalDevice),
                static_cast<VkSurfaceKHR>(m_vkSurface),
                &formatCount,
                formats.data());
        }
        if ( result == VK_SUCCESS ) {
            addStartupDiagnostic(
                fmt::format("Surface formats reported: {}", formatCount));
            for ( size_t index = 0; index < formats.size(); ++index ) {
                addStartupDiagnostic(fmt::format(
                    "Surface format[{}]: format={}, colorSpace={}",
                    index,
                    vk::to_string(
                        static_cast<vk::Format>(formats[index].format)),
                    static_cast<int>(formats[index].colorSpace)));
            }
        }
    }
    if ( result != VK_SUCCESS ) {
        addStartupDiagnostic(fmt::format("Failed to query surface formats: {}",
                                         formatVkResult(result)));
    }

    uint32_t presentModeCount = 0;
    result                    = vkGetPhysicalDeviceSurfacePresentModesKHR(
        static_cast<VkPhysicalDevice>(m_vkPhysicalDevice),
        static_cast<VkSurfaceKHR>(m_vkSurface),
        &presentModeCount,
        nullptr);
    if ( result == VK_SUCCESS ) {
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        if ( presentModeCount > 0 ) {
            result = vkGetPhysicalDeviceSurfacePresentModesKHR(
                static_cast<VkPhysicalDevice>(m_vkPhysicalDevice),
                static_cast<VkSurfaceKHR>(m_vkSurface),
                &presentModeCount,
                presentModes.data());
        }
        if ( result == VK_SUCCESS ) {
            addStartupDiagnostic(fmt::format(
                "Surface present modes reported: {}", presentModeCount));
            for ( size_t index = 0; index < presentModes.size(); ++index ) {
                addStartupDiagnostic(
                    fmt::format("Surface presentMode[{}]: {}",
                                index,
                                presentModeName(presentModes[index])));
            }
            XINFO(
                "Selected Vulkan surface support: formats={}, presentModes={}",
                formatCount,
                presentModeCount);
        }
    }
    if ( result != VK_SUCCESS ) {
        addStartupDiagnostic(
            fmt::format("Failed to query surface present modes: {}",
                        formatVkResult(result)));
    }
}

void VKContext::collectLogicalDeviceCreateDiagnostics(
    const std::vector<const char*>& deviceExtensions,
    const std::vector<uint32_t>&    queueFamilies)
{
    addStartupDiagnostic(fmt::format(
        "Requested logical-device queue families: {}", queueFamilies.size()));
    for ( uint32_t queueFamily : queueFamilies ) {
        addStartupDiagnostic(
            fmt::format("Requested queue family: {}", queueFamily));
    }

    addStartupDiagnostic(fmt::format("Requested device extensions: {}",
                                     deviceExtensions.size()));
    for ( const char* extension : deviceExtensions ) {
        addStartupDiagnostic(fmt::format("Requested device extension: {}",
                                         extension ? extension : "<null>"));
    }
}

void VKContext::logStartupDiagnostics(const char* reason)
{
    if ( m_startupDiagnosticsPrinted ) {
        XERROR(
            "Graphics startup diagnostics already printed. Latest reason: {}",
            reason ? reason : "<unknown>");
        return;
    }

    m_startupDiagnosticsPrinted = true;
    XERROR("================ Graphics Startup Diagnostics ================");
    XERROR("Reason: {}", reason ? reason : "<unknown>");
    if ( m_startupDiagnosticLines.empty() ) {
        XERROR("No startup diagnostic lines were collected.");
    }

    for ( size_t index = 0; index < m_startupDiagnosticLines.size(); ++index ) {
        XERROR("[{:03}] {}", index, m_startupDiagnosticLines[index]);
    }
    XERROR("================ End Graphics Startup Diagnostics ============");
}

}  // namespace MMM::Graphic
