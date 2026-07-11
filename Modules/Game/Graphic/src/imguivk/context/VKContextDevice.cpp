#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <fmt/format.h>
#include <set>
#include <string_view>

namespace MMM::Graphic
{
namespace
{
/// @brief 检查 device 扩展列表中是否存在指定扩展。
/// @param extensions Vulkan-Hpp device 扩展列表。
/// @param extensionName 需要查找的扩展名。
/// @return 找到扩展时返回 true。
bool hasDeviceExtension(const std::vector<vk::ExtensionProperties>& extensions,
                        const char* extensionName)
{
    return std::any_of(
        extensions.begin(),
        extensions.end(),
        [extensionName](const vk::ExtensionProperties& extension) {
            return std::string_view(extension.extensionName) == extensionName;
        });
}
}  // namespace

/**
 * @brief 初始化逻辑设备
 */
void VKContext::initLogicDevice()
{
    // vk逻辑设备队列优先级表
    std::array vkDeviceQueuePriorities{ 1.f };

    // 唯一队列族集合(set自动去重)
    const std::set uniqueQueueFamilies = {
        m_queueFamilyIndices.graphicsQueueIndex.value(),
        m_queueFamilyIndices.presentQueueIndex.value()
    };

    // 1.为每个队列族索引创建对应的队列创建信息
    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;

    for ( const uint32_t& queueFamily : uniqueQueueFamilies ) {
        vk::DeviceQueueCreateInfo queueInfo{};
        queueInfo.setQueueFamilyIndex(queueFamily)
            .setQueueCount(1)
            .setQueuePriorities(vkDeviceQueuePriorities);
        queueCreateInfos.push_back(queueInfo);
    }

    // 2.准备逻辑设备需要启用的扩展
    std::vector<const char*> deviceExtensions = {
        // 交换链扩展
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    // --- macOS 适配开始 ---
    // 获取当前物理设备支持的所有扩展
    auto availableDeviceExtensionsResult =
        m_vkPhysicalDevice.enumerateDeviceExtensionProperties();
    if ( availableDeviceExtensionsResult.result != vk::Result::eSuccess ) {
        addStartupDiagnostic(fmt::format(
            "Failed to enumerate selected GPU device extensions: {}",
            vk::to_string(availableDeviceExtensionsResult.result)));
        logStartupDiagnostics(
            "Failed to enumerate selected GPU device extensions.");
        failInitialization(
            "Fatal: Failed to enumerate Vulkan device extensions.");
        return;
    }
    auto& availableDeviceExtensions = availableDeviceExtensionsResult.value;
    addStartupDiagnostic(
        fmt::format("Selected GPU device extensions reported: {}",
                    availableDeviceExtensions.size()));

    if ( !hasDeviceExtension(availableDeviceExtensions,
                             VK_KHR_SWAPCHAIN_EXTENSION_NAME) ) {
        addStartupDiagnostic(
            fmt::format("Required device extension missing: {}",
                        VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        logStartupDiagnostics(
            "Required Vulkan swapchain device extension is "
            "missing.");
        failInitialization(
            "Fatal: Required Vulkan swapchain device extension is missing.");
        return;
    }

    // 检查是否存在 portability_subset
    for ( const auto& ext : availableDeviceExtensions ) {
        if ( std::string_view(ext.extensionName) ==
             "VK_KHR_portability_subset" ) {
            deviceExtensions.push_back("VK_KHR_portability_subset");
            break;
        }
    }
    // --- macOS 适配结束 ---
    std::vector<uint32_t> queueFamilyList(uniqueQueueFamilies.begin(),
                                          uniqueQueueFamilies.end());
    collectLogicalDeviceCreateDiagnostics(deviceExtensions, queueFamilyList);

    // 3.初始化队列创建信息
    vk::DeviceCreateInfo vkDeviceCreateInfo;

    // 4.初始化逻辑设备创建信息
    vkDeviceCreateInfo
        // 将队列创建信息设置到逻辑设备创建信息中
        .setQueueCreateInfos(queueCreateInfos)
        // 启用逻辑设备功能(几何着色器等功能在这里启用)
        .setPEnabledFeatures({})
        // 启用逻辑设备扩展
        .setPEnabledExtensionNames(deviceExtensions);

    // 5.创建vk逻辑设备 (通过物理设备)
    auto deviceResult = m_vkPhysicalDevice.createDevice(vkDeviceCreateInfo);
    if ( deviceResult.result != vk::Result::eSuccess ) {
        addStartupDiagnostic(fmt::format("vkCreateDevice failed: {}",
                                         vk::to_string(deviceResult.result)));
        logStartupDiagnostics("vkCreateDevice failed.");
        failInitialization("Fatal: Failed to create Vulkan logical device.");
        return;
    }
    m_vkLogicalDevice = deviceResult.value;
    addStartupDiagnostic("Vulkan logical device created successfully.");
    XDEBUG("VK Logic Device Initialized.");

    // 6.获取图形队列族句柄
    m_LogicDeviceGraphicsQueue = m_vkLogicalDevice.getQueue(
        m_queueFamilyIndices.graphicsQueueIndex.value(), 0);
    XDEBUG("Graphics Queue handle retrieved.");

    // 7.获取呈现队列族句柄
    m_LogicDevicePresentQueue = m_vkLogicalDevice.getQueue(
        m_queueFamilyIndices.presentQueueIndex.value(), 0);
    XDEBUG("Present Queue handle retrieved.");
}


}  // namespace MMM::Graphic
