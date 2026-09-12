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
///
/// Vulkan 扩展名存放在固定长度字符数组中，以 string_view 比较可避免初始化阶段
/// 为每个候选扩展创建临时字符串；调用方保证 extensionName 指向以空字符结尾的
/// Vulkan 扩展常量。
///
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

/// @brief 为已选中的物理设备创建逻辑设备及图形、呈现队列。
///
/// 队列族索引由物理设备选择阶段保证完整。本函数合并重复的队列族，只为每个族
/// 请求一条队列；随后验证交换链扩展，按设备能力追加 portability subset，并创建
/// 不启用额外可选 feature 的逻辑设备。
///
/// @warning 窗口资源启动路径：会枚举设备扩展并创建 Vulkan device，只能在物理
/// 设备与 surface 选定后调用，禁止进入渲染循环。
void VKContext::initLogicDevice()
{
    // 每个队列族只申请一条最高优先级队列；数组必须存活到 createDevice 返回，
    // 因为各个 DeviceQueueCreateInfo 都持有它的数据指针。
    std::array vkDeviceQueuePriorities{ 1.f };

    // 图形与呈现可能落在同一队列族，set 去重可避免提交两份相同族索引的创建
    // 信息；该重复在 Vulkan device 创建契约中是不允许的。
    const std::set uniqueQueueFamilies = {
        m_queueFamilyIndices.graphicsQueueIndex.value(),
        m_queueFamilyIndices.presentQueueIndex.value()
    };

    // vector 持有最终传给 DeviceCreateInfo 的完整队列描述，其生命周期覆盖设备
    // 创建调用。
    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;

    // 每个唯一队列族只取索引 0 的一条队列；渲染器在外部同步图形和呈现提交，
    // 当前架构不需要额外队列。
    for ( const uint32_t& queueFamily : uniqueQueueFamilies ) {
        vk::DeviceQueueCreateInfo queueInfo{};
        queueInfo.setQueueFamilyIndex(queueFamily)
            .setQueueCount(1)
            .setQueuePriorities(vkDeviceQueuePriorities);
        queueCreateInfos.push_back(queueInfo);
    }

    // 窗口呈现无条件依赖交换链扩展，即使 loader 枚举到了设备，也不能假设该
    // device 扩展必然存在。
    std::vector<const char*> deviceExtensions = {
        // 交换链扩展
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    // 对选中的设备执行一次完整扩展枚举，既验证必需交换链能力，也为 MoltenVK
    // 的 portability subset 做能力驱动的可选启用。
    auto availableDeviceExtensionsResult =
        m_vkPhysicalDevice.enumerateDeviceExtensionProperties();
    if ( availableDeviceExtensionsResult.result != vk::Result::eSuccess ) {
        // 枚举失败时没有可信的能力集合，不能继续猜测扩展支持并创建 device。
        addStartupDiagnostic(fmt::format(
            "Failed to enumerate selected GPU device extensions: {}",
            vk::to_string(availableDeviceExtensionsResult.result)));
        logStartupDiagnostics(
            "Failed to enumerate selected GPU device extensions.");
        failInitialization(
            "Fatal: Failed to enumerate Vulkan device extensions.");
        return;
    }
    // 引用只在 result 容器的当前作用域内使用，避免复制可能较长的属性列表。
    auto& availableDeviceExtensions = availableDeviceExtensionsResult.value;
    addStartupDiagnostic(
        fmt::format("Selected GPU device extensions reported: {}",
                    availableDeviceExtensions.size()));

    if ( !hasDeviceExtension(availableDeviceExtensions,
                             VK_KHR_SWAPCHAIN_EXTENSION_NAME) ) {
        // surface 创建成功并不代表 device 支持交换链；缺少扩展时要在创建逻辑
        // 设备前报告明确原因，避免把失败延迟到 swapchain 初始化。
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

    // portability subset 是设备扩展而非 instance 扩展，只能在设备确实报告后
    // 加入列表；原生 Vulkan 驱动通常不会提供它。
    for ( const auto& ext : availableDeviceExtensions ) {
        if ( std::string_view(ext.extensionName) ==
             "VK_KHR_portability_subset" ) {
            deviceExtensions.push_back("VK_KHR_portability_subset");
            break;
        }
    }
    // 诊断层只需要稳定的索引值，不应依赖 set 的内部表示或创建信息指针。
    std::vector<uint32_t> queueFamilyList(uniqueQueueFamilies.begin(),
                                          uniqueQueueFamilies.end());
    collectLogicalDeviceCreateDiagnostics(deviceExtensions, queueFamilyList);

    // DeviceCreateInfo 引用上方两个 vector；它不会在 createDevice 返回后继续
    // 持有这些临时容器。
    vk::DeviceCreateInfo vkDeviceCreateInfo;

    // 当前渲染路径只使用核心能力，因此显式传入空 feature 集；新增可选能力时
    // 必须先在物理设备选择阶段验证，不能只在此处盲目启用。
    vkDeviceCreateInfo.setQueueCreateInfos(queueCreateInfos)
        .setPEnabledFeatures({})
        .setPEnabledExtensionNames(deviceExtensions);

    // Vulkan-Hpp 无异常模式通过 result/value 返回失败；只有成功时才发布 device
    // 句柄，保证 release 路径不会销毁半初始化对象。
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

    // getQueue 不创建新资源，只取回建 device 时已经申请的队列索引 0。
    m_LogicDeviceGraphicsQueue = m_vkLogicalDevice.getQueue(
        m_queueFamilyIndices.graphicsQueueIndex.value(), 0);
    XDEBUG("Graphics Queue handle retrieved.");

    // 当图形与呈现共享队列族时两个句柄可相同；保留两个角色字段可让渲染器
    // 无需关心队列族是否合并。
    m_LogicDevicePresentQueue = m_vkLogicalDevice.getQueue(
        m_queueFamilyIndices.presentQueueIndex.value(), 0);
    XDEBUG("Present Queue handle retrieved.");
}

}  // namespace MMM::Graphic
