#include "colorful-log.h"
#include "skin/SkinConfig.h"
#include "translation/Translation.h"
#include "vulkan/vulkan.hpp"
#include <array>
#include <filesystem>
#include <optional>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_handles.hpp>
#include <vulkan/vulkan_structs.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// vk debug回调
VKAPI_ATTR VkBool32 VKAPI_PTR vkDebug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT             messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData);

int main(int argc, char* argv[])
{
    using namespace MMM;
    XLogger::init("MMM");

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
        return -1;
    }

    // 跨平台（自动处理 / 或 \）
    auto assetPath = rootDir / "assets";

    using namespace Translation;
    Translator::instance().loadLanguage(
        (assetPath / "lang" / "en_us.lua").generic_string());
    Translator::instance().loadLanguage(
        (assetPath / "lang" / "zh_cn.lua").generic_string());
    Translator::instance().switchLang("zh_cn");
    XINFO(TR("tips.welcom"));


    using namespace Config;
    // 载入皮肤配置
    SkinManager::instance().loadSkin(
        (assetPath / "skins" / "mmm-nightly" / "skin.lua").generic_string());
    auto backgroundColor = SkinManager::instance().getColor("background");
    XINFO("background color:[{},{},{},{}]",
          backgroundColor.r,
          backgroundColor.g,
          backgroundColor.b,
          backgroundColor.a);

    // 测试vulkan

    // 1.初始化GLFW
    if ( !glfwInit() ) {
        XERROR("glfw init failed");
        return -1;
    };
    XINFO("GLFW initialized successfully.");

    // 1.1设置GLFW为NOAPI来适配vulkan
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // 1.2检查 GLFW 的 Vulkan 是否被支持
    if ( !glfwVulkanSupported() ) {
        XERROR("Fatal: GLFW reports that Vulkan is not supported!");
        glfwTerminate();
        return -1;
    }
    XINFO("GLFW Vulkan is supported.");

    // 1.3获取 GLFW 需要的扩展
    uint32_t     glfwExtensionCount = 0;
    const char** glfwExtensions{ nullptr };
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if ( glfwExtensions == nullptr ) {
        XERROR("Fatal: Failed to get required GLFW extensions.");
        glfwTerminate();
        return -1;
    }

    // 1.4将 C 风格的字符串数组转换为更易于管理的 std::vector
    std::vector<const char*> requiredExtensions(
        glfwExtensions, glfwExtensions + glfwExtensionCount);
    XINFO("Required GLFW extensions:");
    for ( const auto& ext : requiredExtensions ) {
        XINFO("  - {}", ext);
    }
    // 启用vk的debug工具扩展
    requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // 1.5创建GLFW窗口
    GLFWwindow* glfwWindow =
        glfwCreateWindow(1920, 1080, "MMMTestVulkan", nullptr, nullptr);

    if ( !glfwWindow ) {
        XERROR("glfw窗口创建失败");
        glfwTerminate();
        return -1;
    }


    // 2.启用vk验证层
    const std::array<const char*, 1> validationLayers = {
        "VK_LAYER_KHRONOS_validation",
    };

    // 2.1检查请求的验证层是否可用
    uint32_t layerCount;
    auto     res = vk::enumerateInstanceLayerProperties(&layerCount, nullptr);
    // 先获取验证层的数量-会填入layerCount
    std::vector<vk::LayerProperties> availableLayers(layerCount);
    res = vk::enumerateInstanceLayerProperties(&layerCount,
                                               availableLayers.data());
    // 然后才是获取所有的验证层-会填入availableLayers.data()

    // 2.2检查请求到的验证层都能不能用
    bool allLayersAvailable = true;
    for ( const char* layerName : validationLayers ) {
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

    if ( !allLayersAvailable ) {
        XERROR("Fatal: Not all requested validation layers are available!");
        // 在这里终止程序
        return -1;
    } else {
        XINFO("Validation layers enabled.");
    }

    // 3.vk调试创建信息 (vk::DebugUtilsMessengerCreateInfoEXT)
    vk::DebugUtilsMessengerCreateInfoEXT vkDebugUtilcreateInfo{};
    vkDebugUtilcreateInfo
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
        .setPfnUserCallback(
            vk::PFN_DebugUtilsMessengerCallbackEXT(vkDebug_callback));


    // 4. 应用程序信息 (vk::ApplicationInfo)
    vk::ApplicationInfo appInfo{};
    appInfo.setPApplicationName("MMM")
        .setApplicationVersion(VK_MAKE_VERSION(1, 0, 0))
        .setApiVersion(VK_API_VERSION_1_4)
        .setEngineVersion(VK_MAKE_VERSION(1, 0, 0))
        .setPEngineName("No Engine");

    // 5.vk实例创建信息 (vk::InstanceCreateInfo)
    vk::InstanceCreateInfo instanceCreateInfo{};
    instanceCreateInfo.setPApplicationInfo(&appInfo)
        .setPEnabledLayerNames(validationLayers)
        .setPEnabledExtensionNames(requiredExtensions)
        .setPNext(&vkDebugUtilcreateInfo);

    // 创建vk实例 (vk::Instance)
    vk::Instance vkInstance = vk::createInstance(instanceCreateInfo);
    XINFO("VK Instance created.");

    // 5.1初始化vkDebugMessenger
    // 在全局或类成员中定义
    vk::DebugUtilsMessengerEXT        vkDebugMessenger;
    vk::detail::DispatchLoaderDynamic dldy;  // 动态加载器
    // 5.2. 初始化动态加载器（它会去寻找 vkCreateDebugUtilsMessengerEXT 的地址）
    dldy.init(vkInstance, vkGetInstanceProcAddr);
    // 5.3. 创建 Messenger 对象
    // 这里必须传入 dldy 否则会链接报错
    vkDebugMessenger = vkInstance.createDebugUtilsMessengerEXT(
        vkDebugUtilcreateInfo, nullptr, dldy);
    XINFO("Vulkan Debug Messenger 开启成功");


    // 6. 选择GPU设备
    // 6.1. 列出所有物理GPU设备
    std::vector<vk::PhysicalDevice> phyDevices =
        vkInstance.enumeratePhysicalDevices();
    for ( const auto& phyDevice : phyDevices ) {
        // 4.1 可以在这里检查GPU设备的功能并选择
        auto devFeature    = phyDevice.getFeatures();
        auto devProperties = phyDevice.getProperties();
        auto devFeatures   = phyDevice.getFeatures();
        // 如优先选择独立显卡
        // devProperties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
    }
    // 简单起见先选择第一个GPU设备
    vk::PhysicalDevice selectedPhyDevice = phyDevices[0];

    XINFO("Selected GPU:\n-- {}",
          std::string(selectedPhyDevice.getProperties().deviceName));

    // 6.2 vk逻辑设备创建信息
    vk::DeviceCreateInfo vkDeviceCreateInfo{};

    // 6.3 vk逻辑设备队列创建信息
    vk::DeviceQueueCreateInfo vkDeviceQueueCreateInfo{};
    float                     priorities{ 1.f };  // 优先级表

    // 6.4 查询图形队列族索引
    struct QueueFamilyIndex final {
        std::optional<uint32_t> graphicsQueue;
    };
    QueueFamilyIndex queueFamilyIndex{};

    auto phyDeviceQueueFamilyProperties =
        selectedPhyDevice.getQueueFamilyProperties();
    for ( size_t i{ 0 }; i < phyDeviceQueueFamilyProperties.size(); ++i ) {
        const auto& phyDeviceQueueFamilyProperty =
            phyDeviceQueueFamilyProperties[i];
        const auto& queueFlags = phyDeviceQueueFamilyProperty.queueFlags;

        if ( queueFlags & vk::QueueFlagBits::eGraphics ) {
            // 使用图形功能
            queueFamilyIndex.graphicsQueue = i;
            break;
        }
    }
    if ( !queueFamilyIndex.graphicsQueue.has_value() ) {
        XERROR("Graphic Queue Family not found");
        // 销毁调试消息器 (在 Instance 销毁之前)
        // 需要传入之前初始化的 dldy
        if ( vkDebugMessenger ) {
            vkInstance.destroyDebugUtilsMessengerEXT(
                vkDebugMessenger, nullptr, dldy);
            XINFO("VK Debug Messenger destroyed.");
        }

        // 销毁vk实例
        vkInstance.destroy();
        XINFO("VK Instance destroyed.");

        glfwTerminate();
        XINFO("GLFW Terminated.");
        return -1;
    }

    vkDeviceQueueCreateInfo
        // 优先级表
        .setPQueuePriorities(&priorities)
        .setQueueCount(1)
        .setQueueFamilyIndex(queueFamilyIndex.graphicsQueue.value());

    // 6.5将队列创建信息设置到逻辑设备创建信息中
    vkDeviceCreateInfo
        .setQueueCreateInfos(vkDeviceQueueCreateInfo)
        // 启用逻辑设备功能
        .setPEnabledFeatures({})
        // 启用逻辑设备扩展
        .setPEnabledExtensionNames({});

    // 6.6创建vk逻辑设备 (通过物理设备)
    vk::Device vkDevice = selectedPhyDevice.createDevice(vkDeviceCreateInfo);


    // 6.7获取逻辑设备队列
    vk::Queue graphicsQueue =
        vkDevice.getQueue(queueFamilyIndex.graphicsQueue.value(), 0);

    //

    // 进入glfw窗口循环
    while ( !glfwWindowShouldClose(glfwWindow) ) {
        // 渲染循环
        glfwPollEvents();
        glfwSwapBuffers(glfwWindow);
    }

    // 销毁调试消息器 (在 Instance 销毁之前)
    // 需要传入之前初始化的 dldy
    if ( vkDebugMessenger ) {
        vkInstance.destroyDebugUtilsMessengerEXT(
            vkDebugMessenger, nullptr, dldy);
        XINFO("VK Debug Messenger destroyed.");
    }

    // 在销毁实例之前，先销毁逻辑设备
    vkDevice.destroy();
    XINFO("VK Device destroyed.");
    // 销毁vk实例
    vkInstance.destroy();
    XINFO("VK Instance destroyed.");

    glfwTerminate();
    XINFO("GLFW Terminated.");
    XLogger::shutdown();
    return 0;
}

// vk debug回调
VKAPI_ATTR VkBool32 VKAPI_CALL vkDebug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT             messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
{
    // 1. 整理消息前缀（显示消息类型：常规/校验/性能）
    std::string typeHeader;
    if ( messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT )
        typeHeader = "[General]";
    else if ( messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT )
        typeHeader = "[Validation]";
    else if ( messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT )
        typeHeader = "[Performance]";

    // 2. 根据 Severity 等级分发到不同的日志宏
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
