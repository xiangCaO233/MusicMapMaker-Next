#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"
#include <set>

namespace MMM::Graphic
{
/**
 * @brief 初始化逻辑设备
 */
void VKContext::initLogicDevice()
{
    // MY m_vkLogicalDevice Createing

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
    auto availableDeviceExtensions =
        m_vkPhysicalDevice.enumerateDeviceExtensionProperties();

    // 检查是否存在 portability_subset
    for ( const auto& ext : availableDeviceExtensions ) {
        if ( std::string_view(ext.extensionName) ==
             "VK_KHR_portability_subset" ) {
            deviceExtensions.push_back("VK_KHR_portability_subset");
            break;
        }
    }
    // --- macOS 适配结束 ---

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
    m_vkLogicalDevice = m_vkPhysicalDevice.createDevice(vkDeviceCreateInfo);
    XINFO("VK Logic Device Initialized.");

    // 6.获取图形队列族句柄
    m_LogicDeviceGraphicsQueue = m_vkLogicalDevice.getQueue(
        m_queueFamilyIndices.graphicsQueueIndex.value(), 0);
    XINFO("Graphics Queue handle retrieved.");

    // 7.获取呈现队列族句柄
    m_LogicDevicePresentQueue = m_vkLogicalDevice.getQueue(
        m_queueFamilyIndices.presentQueueIndex.value(), 0);
    XINFO("Present Queue handle retrieved.");
}


}  // namespace MMM::Graphic
