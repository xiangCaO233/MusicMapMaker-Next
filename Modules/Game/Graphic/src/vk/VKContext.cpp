#include "graphic/vk/VKContext.h"
#include "log/colorful-log.h"

#include <GLFW/glfw3.h>
#include <filesystem>
#include <fstream>
#include <ios>
#include <set>

namespace MMM
{
namespace Graphic
{

/**
 * @brief 获取 VKContext 单例实例
 *
 * 如果实例尚未创建，将尝试进行初始化。
 *
 * @return std::expected<std::reference_wrapper<VKContext>, std::string>
 *         成功返回上下文引用，失败返回错误信息字符串
 */
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

/**
 * @brief 私有构造函数，执行基础 Vulkan 初始化
 */
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
        XINFO("Vulkan Debug Messenger Initialize Successed");
    }

    // 初始化窗口表面和
    // 后续显卡设备初始化
    // 放在窗口相关资源初始化函数中
}

/**
 * @brief 析构函数，负责清理所有 Vulkan 资源
 */
VKContext::~VKContext()
{
    // 销毁渲染器
    m_vkRenderer.reset();

    // 清理渲染管线
    m_vkRenderPipeline.reset();

    // 清理着色器程序
    m_vkShaders.clear();

    if ( m_swapchain ) {
        // 手动销毁交换链中的帧缓冲
        m_swapchain->destroyFramebuffers();
    }

    // RenderPass 是通过 Device 创建和销毁的
    // 在销毁 Device 前销毁 RenderPass
    m_vkRenderPass.reset();

    // 销毁交换链
    m_swapchain.reset();

    // 销毁逻辑设备
    if ( m_vkLogicalDevice ) {
        m_vkLogicalDevice.destroy();
        XINFO("VK Logical Device destroyed.");
    }

    // 销毁vk表面
    if ( m_vkSurface ) m_vkInstance.destroySurfaceKHR(m_vkSurface);
    XINFO("VK Surface destroyed.");

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

/**
 * @brief 初始化 Vulkan 应用程序信息
 */
void VKContext::initVkAppInfo()
{
    m_vkAppInfo.setPApplicationName("MMM")
        .setApplicationVersion(VK_MAKE_VERSION(1, 0, 0))
        .setApiVersion(VK_API_VERSION_1_4)
        .setEngineVersion(VK_MAKE_VERSION(1, 0, 0))
        .setPEngineName("No Engine");
}

/**
 * @brief 初始化 Vulkan 实例创建信息
 */
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

/**
 * @brief 启用 Vulkan Debug 扩展
 */
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

/**
 * @brief 启用并检查 Validation Layer
 */
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

/**
 * @brief Vulkan Debug 回调函数
 *
 * 用于接收并处理 Validation Layer 发出的调试信息。
 *
 * @param messageSeverity 消息严重等级
 * @param messageTypes 消息类型
 * @param pCallbackData 回调数据（包含错误信息）
 * @param pUserData 用户自定义数据指针
 * @return VKAPI_ATTR VkBool32 是否中断 Vulkan 调用
 */
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

/**
 * @brief 初始化 Vulkan 窗体相关资源
 *
 * 包括 Surface, Swapchain, RenderPass, Pipeline 等依赖窗口尺寸的资源。
 *
 * @param window_ctx GLFW 窗口句柄
 * @param w 窗口宽度
 * @param h 窗口高度
 */
void VKContext::initVKWindowRess(GLFWwindow* window_ctx, int w, int h)
{
    // 初始化vk表面句柄
    initSurface(window_ctx);

    // 挑选物理显卡
    pickPhysicalDevice();

    // 查询图形队列族索引
    queryQueueFamilyIndices();

    // 初始化逻辑设备
    initLogicDevice();

    // 创建交换链
    m_swapchain = std::make_unique<VKSwapchain>(m_vkPhysicalDevice,
                                                m_vkLogicalDevice,
                                                m_vkSurface,
                                                m_queueFamilyIndices,
                                                w,
                                                h);
    // 创建着色器程序
    createShader();

    // 创建渲染流程
    m_vkRenderPass =
        std::make_unique<VKRenderPass>(m_vkLogicalDevice, *m_swapchain);

    // 创建帧缓冲
    m_swapchain->createFramebuffers(*m_vkRenderPass);

    // 创建图形渲染管线
    m_vkRenderPipeline =
        std::make_unique<VKRenderPipeline>(m_vkLogicalDevice,
                                           *m_vkShaders["testShader"],
                                           *m_vkRenderPass,
                                           *m_swapchain,
                                           w,
                                           h);

    // 创建渲染器
    m_vkRenderer = std::make_unique<VKRenderer>(m_vkPhysicalDevice,
                                                m_vkLogicalDevice,
                                                *m_swapchain,
                                                *m_vkRenderPipeline,
                                                *m_vkRenderPass,
                                                m_LogicDeviceGraphicsQueue,
                                                m_LogicDevicePresentQueue);
}

/**
 * @brief 初始化 Vulkan 表面
 * @param window_handle GLFW 窗口句柄
 */
void VKContext::initSurface(GLFWwindow* window_handle)
{
    // C 风格的 Surface 创建（GLFW 提供的快捷函数）
    VkSurfaceKHR surface;
    if ( glfwCreateWindowSurface(
             m_vkInstance, window_handle, nullptr, &surface) != VK_SUCCESS ) {
        throw std::runtime_error("Failed to create window surface!");
    }

    // 转换为 vk::SurfaceKHR
    m_vkSurface = surface;
    XINFO("Vulkan Surface created.");
}

/**
 * @brief 挑选合适的物理设备
 */
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

/**
 * @brief 查询物理设备的队列族索引
 */
void VKContext::queryQueueFamilyIndices()
{
    // 1.获取物理显卡队列族属性
    auto phyDeviceQueueFamilyProperties =
        m_vkPhysicalDevice.getQueueFamilyProperties();

    // 2.查询到支持的显卡队列族索引并保存
    for ( size_t i{ 0 }; i < phyDeviceQueueFamilyProperties.size(); ++i ) {
        const auto& phyDeviceQueueFamilyProperty =
            phyDeviceQueueFamilyProperties[i];
        const auto& queueFlags = phyDeviceQueueFamilyProperty.queueFlags;

        // 检查是否支持图形功能
        if ( queueFlags & vk::QueueFlagBits::eGraphics ) {
            m_queueFamilyIndices.graphicsQueueIndex = i;
        }

        // 检查是否支持在该 Surface 上显示
        if ( m_vkPhysicalDevice.getSurfaceSupportKHR(i, m_vkSurface) ) {
            m_queueFamilyIndices.presentQueueIndex = i;
        }

        if ( m_queueFamilyIndices ) {
            // 找到了同时支持两者的（或者分别找到了）
            break;
        }
    }

    // 3.没查到的话及时销毁已创建的资源并抛出异常
    if ( !m_queueFamilyIndices ) {
        // 销毁调试消息器 (在 Instance 销毁之前)
        // 需要传入之前初始化的 dldy
        if ( is_debug() && m_vkDebugMessenger ) {
            m_vkInstance.destroyDebugUtilsMessengerEXT(
                m_vkDebugMessenger, nullptr, m_vkDldy);
            XINFO("VK Debug Messenger destroyed.");
        }

        // 销毁vk表面
        if ( m_vkSurface ) m_vkInstance.destroySurfaceKHR(m_vkSurface);
        XINFO("VK Surface destroyed.");

        // 销毁vk实例
        m_vkInstance.destroy();
        XINFO("VK Instance destroyed.");

        // 释放GLFW
        releaseGLFW();
        XINFO("GLFW Terminated.");

        throw std::runtime_error("Graphic Queue Family not found");
    }
}

/**
 * @brief 初始化逻辑设备
 */
void VKContext::initLogicDevice()
{
    // vk逻辑设备队列优先级表
    std::array<float, 1> vkDeviceQueuePriorities{ 1.f };

    // 唯一队列族集合(set自动去重)
    std::set<uint32_t> uniqueQueueFamilies = {
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
    const std::array<const char*, 1> deviceExtensions = {
        // 交换链扩展
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

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

std::string readFile(std::string path)
{
    // 读取文件内容
    std::ifstream fs;
    fs.open(path, std::ios::binary | std::ios::ate);
    if ( !fs.is_open() ) {
        XERROR("Fatal: Could not open File[{}]!", path);
        return {};
    }
    auto        sourceSize = fs.tellg();
    std::string source;
    source.resize(sourceSize);
    fs.seekg(0);
    fs.read(source.data(), sourceSize);
    fs.close();
    return source;
}

/**
 * @brief 加载并创建 Shader 模块
 */
void VKContext::createShader()
{
    // 读取源码初始化shader
    // 假设 assets 肯定在运行目录上n级
    // 而 build 目录通常在 root/build/Modules/Main/ 下 (深度为 3 或 4)
    auto rootDir = std::filesystem::current_path();
    // 向上查找直到找到 assets 文件夹
    while ( !std::filesystem::exists(rootDir / "assets") &&
            rootDir.has_parent_path() ) {
        rootDir = rootDir.parent_path();
    }

    if ( !std::filesystem::exists(rootDir / "assets") ) {
        XERROR("Fatal: Could not find assets directory!");
        return;
    }

    // 跨平台（自动处理 / 或 \）
    auto assetPath = rootDir / "assets";
    auto testVertexShaderSourcePath =
        assetPath / "shaders" / "testVertexShader.spv";
    auto testFragmentShaderSourcePath =
        assetPath / "shaders" / "testFragmentShader.spv";

    // 读取文件内容
    std::string testVertexShaderSource =
        readFile(testVertexShaderSourcePath.generic_string());
    std::string testFragmentShaderSource =
        readFile(testFragmentShaderSourcePath.generic_string());

    // 创建着色器
    auto test_vkShader = std::make_unique<VKShader>(
        m_vkLogicalDevice, testVertexShaderSource, testFragmentShaderSource);
    m_vkShaders.emplace("testShader", std::move(test_vkShader));
}

}  // namespace Graphic

}  // namespace MMM
