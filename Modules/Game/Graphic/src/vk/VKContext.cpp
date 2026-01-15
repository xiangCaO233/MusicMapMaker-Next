#include "vk/VKContext.h"
#include "colorful-log.h"

#include <expected>
#include <stdexcept>

namespace MMM
{
namespace Graphic
{

// 获取实例
std::expected<std::reference_wrapper<VKContext>, std::string> VKContext::get()
{
    try {
        // 尝试初始化VK上下文
        static VKContext vkContext;
        return vkContext;
    } catch ( const std::runtime_error& e ) {
        // 初始化失败返回what错误
        return std::unexpected<std::string>(e.what());
    }
}

VKContext::VKContext()
{
    // 初始化GLFW
    initGLFW();
    // 注册GLFW的VK扩展
    registerGLFWExtentions();

    // Debug模式启用VK调试工具
    if ( is_debug() ) {
        enableVKDebugExt();
        enableVKValidateLayer();
    }

    // 初始化vk应用程序信息
    initVkAppInfo();

    // 初始化vk实例创建信息
    initVkInstanceCreateInfo();

    // 创建vk实例
    m_vkInstance = vk::createInstance(m_vkInstanceCreateInfo);
    XINFO("VK Instance created.");

    // 初始化vk动态加载器(要在创建vkInstance后)
    // (它会去找到扩展函数 如 vkCreateDebugUtilsMessengerEXT的地址)
    m_vkDldy.init(m_vkInstance, vkGetInstanceProcAddr);
    XINFO("VK dldy initialized.");

    if ( is_debug() ) {
        // debug模式初始化vk调试信息工具
        // 创建 Messenger 对象
        // 这里必须传入 dldy 否则会链接报错
        m_vkDebugMessenger = m_vkInstance.createDebugUtilsMessengerEXT(
            m_vkDebugUtilCreateInfo, nullptr, m_vkDldy);
        XINFO("Vulkan Debug Messenger 开启成功");
    }

    // 挑选物理显卡
    pickPhysicalDevice();

    // 查询图形队列族索引
    queryQueueFamilyIndices();

    // 初始化逻辑设备
    initLogicDevice();
}

VKContext::~VKContext()
{
    // 1. 销毁逻辑设备
    if ( m_vkLogicalDevice ) {
        m_vkLogicalDevice.destroy();
        XINFO("VK Logical Device destroyed.");
    }

    // 销毁可能的vk调试信息工具实例
    if ( is_debug() && m_vkDebugMessenger ) {
        m_vkInstance.destroyDebugUtilsMessengerEXT(
            m_vkDebugMessenger, nullptr, m_vkDldy);
        XINFO("VK Debug Messenger destroyed.");
    }
    // 销毁vk实例
    m_vkInstance.destroy();
    XINFO("VK Instance destroyed.");

    // 释放GLFW上下文
    releaseGLFW();
}

/*
 * 初始化vk应用程序信息
 * */
void VKContext::initVkAppInfo()
{
    m_vkAppInfo.setPApplicationName("MMM")
        .setApplicationVersion(VK_MAKE_VERSION(1, 0, 0))
        .setApiVersion(VK_API_VERSION_1_4)
        .setEngineVersion(VK_MAKE_VERSION(1, 0, 0))
        .setPEngineName("No Engine");
}

/*
 * 初始化vk实例创建信息
 * */
void VKContext::initVkInstanceCreateInfo()
{
    m_vkInstanceCreateInfo
        .setPApplicationInfo(&m_vkAppInfo)
        // 启用的扩展
        .setPEnabledExtensionNames(m_vkExtensions);

    if ( is_debug() ) {
        // 启用的Layer
        m_vkInstanceCreateInfo.setPEnabledLayerNames(m_vkValidationLayers);
        // 启用的Next指针(这里启用的调试信息)
        m_vkInstanceCreateInfo.setPNext(&m_vkDebugUtilCreateInfo);
    }
}

/*
 * 启用vkdebug扩展
 * */
void VKContext::enableVKDebugExt()
{
    // 1.启用vk的debug工具扩展
    m_vkExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    m_vkDebugUtilCreateInfo
        // 设置严重等级
        .setMessageSeverity(
            // 接收错误
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
            // 接收警告
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning
            // 接收信息
            // | vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo
            // 接收详细调试信息（非常吵，建议开发时开启）
            // | vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose
            )
        // 设置消息类型
        .setMessageType(
            // 常规类型
            vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
            // 验证层类型
            vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
            // 性能类型
            vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance)
        // 设置回调函数指针
        .setPfnUserCallback(vk::PFN_DebugUtilsMessengerCallbackEXT(
            &VKContext::vkDebug_callback));
}

/*
 * 启用vk验证层
 * */
void VKContext::enableVKValidateLayer()
{
    // 1.检查请求的验证层是否可用
    uint32_t layerCount;
    auto     res = vk::enumerateInstanceLayerProperties(&layerCount, nullptr);

    // 1.1先获取验证层的数量-会填入layerCount
    std::vector<vk::LayerProperties> availableLayers(layerCount);
    // 然后才是获取所有的验证层-会填入availableLayers.data()
    res = vk::enumerateInstanceLayerProperties(&layerCount,
                                               availableLayers.data());

    // 2.检查请求到的验证层都能不能用
    bool allLayersAvailable = true;
    for ( const char* layerName : m_vkValidationLayers ) {
        bool layerFound = false;
        for ( const auto& layerProperties : availableLayers ) {
            if ( strcmp(layerName, layerProperties.layerName) == 0 ) {
                layerFound = true;
                break;
            }
        }
        if ( !layerFound ) {
            XERROR("Validation layer requested, but not available: {}",
                   layerName);
            allLayersAvailable = false;
        }
    }

    // 3.层检查不通过及时释放已初始化的资源并抛出异常
    if ( !allLayersAvailable ) {
        releaseGLFW();
        // !此处可能退出
        throw std::runtime_error(
            "Fatal: Not all requested validation layers are available!");
    } else {
        XINFO("Validation layers enabled.");
    }
}

// vk debug回调
VKAPI_ATTR VkBool32 VKAPI_CALL VKContext::vkDebug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT             messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
{
    // 1.整理消息前缀（显示消息类型：常规/校验/性能）
    std::string typeHeader;
    if ( messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT )
        typeHeader = "[General]";
    else if ( messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT )
        typeHeader = "[Validation]";
    else if ( messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT )
        typeHeader = "[Performance]";

    // 2.根据 Severity 等级分发到不同的日志宏
    if ( messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ) {
        XERROR("{} {}", typeHeader, pCallbackData->pMessage);
    } else if ( messageSeverity &
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ) {
        XWARN("{} {}", typeHeader, pCallbackData->pMessage);
    } else if ( messageSeverity &
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT ) {
        XINFO("{} {}", typeHeader, pCallbackData->pMessage);
    } else if ( messageSeverity &
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT ) {
        // 对应 Debug 或 Trace 级别
        XDEBUG("{} {}", typeHeader, pCallbackData->pMessage);
    }

    // 返回 VK_FALSE 表示不中断触发该消息的 Vulkan 调用
    // 如果返回 VK_TRUE，则该 API 调用会返回 VK_ERROR_VALIDATION_FAILED_EXT
    // 并可能中断程序
    return VK_FALSE;
}

/*
 * 挑选vk物理设备
 * */
void VKContext::pickPhysicalDevice()
{
    // 1.列出所有物理GPU设备
    std::vector<vk::PhysicalDevice> phyDevices =
        m_vkInstance.enumeratePhysicalDevices();
    for ( const auto& phyDevice : phyDevices ) {
        // 1.1 可以在这里检查GPU设备的功能并选择
        auto devFeature    = phyDevice.getFeatures();
        auto devProperties = phyDevice.getProperties();
        auto devFeatures   = phyDevice.getFeatures();
        // 如优先选择独立显卡
        // devProperties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
    }

    // 2.简单起见先选择第一个GPU设备
    m_vkPhysicalDevice = phyDevices[0];

    XINFO("Selected GPU:\n-- {}",
          std::string(m_vkPhysicalDevice.getProperties().deviceName));

    /*
     * 挑选不到心仪设备记得释放资源并抛出异常
     * // 销毁调试消息器 (在 Instance 销毁之前)
     * // 需要传入之前初始化的 dldy
     * if ( is_debug() && m_vkDebugMessenger ) {
     *     m_vkInstance.destroyDebugUtilsMessengerEXT(
     *         m_vkDebugMessenger, nullptr, m_vkDldy);
     *     XINFO("VK Debug Messenger destroyed.");
     * }
     *
     * // 销毁vk实例
     * m_vkInstance.destroy();
     * XINFO("VK Instance destroyed.");
     *
     * // 释放GLFW
     * releaseGLFW();
     * XINFO("GLFW Terminated.");
     *
     * throw std::runtime_error("Graphic Queue Family not found");
     *
     * */
}

/*
 * 查询图形队列族索引
 * */
void VKContext::queryQueueFamilyIndices()
{
    // 1.获取物理显卡队列族属性
    auto phyDeviceQueueFamilyProperties =
        m_vkPhysicalDevice.getQueueFamilyProperties();

    // 2.查询到支持图形的显卡队列族索引并保存
    for ( size_t i{ 0 }; i < phyDeviceQueueFamilyProperties.size(); ++i ) {
        const auto& phyDeviceQueueFamilyProperty =
            phyDeviceQueueFamilyProperties[i];
        const auto& queueFlags = phyDeviceQueueFamilyProperty.queueFlags;

        if ( queueFlags & vk::QueueFlagBits::eGraphics ) {
            // 使用图形功能
            m_queueFamilyIndices.graphicsQueue = i;
            break;
        }
    }

    // 3.没查到的话及时销毁已创建的资源并抛出异常
    if ( !m_queueFamilyIndices.graphicsQueue.has_value() ) {
        // 销毁调试消息器 (在 Instance 销毁之前)
        // 需要传入之前初始化的 dldy
        if ( is_debug() && m_vkDebugMessenger ) {
            m_vkInstance.destroyDebugUtilsMessengerEXT(
                m_vkDebugMessenger, nullptr, m_vkDldy);
            XINFO("VK Debug Messenger destroyed.");
        }

        // 销毁vk实例
        m_vkInstance.destroy();
        XINFO("VK Instance destroyed.");

        // 释放GLFW
        releaseGLFW();
        XINFO("GLFW Terminated.");

        throw std::runtime_error("Graphic Queue Family not found");
    }
}

/*
 * 初始化vk逻辑设备
 * */
void VKContext::initLogicDevice()
{
    // 1.初始化队列创建信息
    m_vkDeviceQueueCreateInfo
        // 优先级表
        .setQueuePriorities(m_vkDeviceQueuePriorities)
        // 图形队列族索引
        .setQueueFamilyIndex(m_queueFamilyIndices.graphicsQueue.value());

    // 2.初始化逻辑设备创建信息
    m_vkDeviceCreateInfo
        // 将队列创建信息设置到逻辑设备创建信息中
        .setQueueCreateInfos(m_vkDeviceQueueCreateInfo)
        // 启用逻辑设备功能
        .setPEnabledFeatures({})
        // 启用逻辑设备扩展
        .setPEnabledExtensionNames({});

    // 3.创建vk逻辑设备 (通过物理设备)
    m_vkLogicalDevice = m_vkPhysicalDevice.createDevice(m_vkDeviceCreateInfo);
    XINFO("VK Logic Device Initialized.");

    // 4.获取队列句柄
    m_LogicDeviceGraphicsQueue = m_vkLogicalDevice.getQueue(
        m_queueFamilyIndices.graphicsQueue.value(), 0);
    XINFO("Graphics Queue handle retrieved.");
}

}  // namespace Graphic

}  // namespace MMM
