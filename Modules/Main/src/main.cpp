#include "colorful-log.h"
#include "skin/SkinConfig.h"
#include "translation/Translation.h"
#include <array>
#include <filesystem>
#include <optional>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_handles.hpp>
#include <vulkan/vulkan_structs.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

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

    // 1.1检查 GLFW 的 Vulkan 是否被支持
    if ( !glfwVulkanSupported() ) {
        XERROR("Fatal: GLFW reports that Vulkan is not supported!");
        glfwTerminate();
        return -1;
    }
    XINFO("GLFW Vulkan is supported.");

    // 1.2获取 GLFW 需要的扩展
    uint32_t     glfwExtensionCount = 0;
    const char** glfwExtensions;
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if ( glfwExtensions == nullptr ) {
        XERROR("Fatal: Failed to get required GLFW extensions.");
        glfwTerminate();
        return -1;
    }

    // 1.3将 C 风格的字符串数组转换为更易于管理的 std::vector
    std::vector<const char*> requiredExtensions(
        glfwExtensions, glfwExtensions + glfwExtensionCount);
    XINFO("Required GLFW extensions:");
    for ( const auto& ext : requiredExtensions ) {
        XINFO("  - {}", ext);
    }

    // 2.启用vk验证层
    const std::array<const char*, 1> validationLayers = {
        "VK_LAYER_KHRONOS_validation"
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

    // 3. 应用程序信息 (vk::ApplicationInfo)
    vk::ApplicationInfo appInfo;
    appInfo.setPApplicationName("MMM")
        .setApplicationVersion(VK_MAKE_VERSION(1, 0, 0))
        .setApiVersion(VK_API_VERSION_1_4)
        .setEngineVersion(VK_MAKE_VERSION(1, 0, 0))
        .setPEngineName("No Engine");

    // 4.vk实例创建信息 (vk::InstanceCreateInfo)
    vk::InstanceCreateInfo instanceCreateInfo;
    instanceCreateInfo.setPApplicationInfo(&appInfo).setPEnabledLayerNames(
        validationLayers);

    // 创建vk实例 (vk::Instance)
    vk::Instance vkInstance = vk::createInstance(instanceCreateInfo);
    XINFO("VK Instance created.");

    // 4. 选择物理GPU设备
    // 4.1. 列出所有物理GPU设备
    std::vector<vk::PhysicalDevice> phyDevices =
        vkInstance.enumeratePhysicalDevices();
    for ( const auto& phyDevice : phyDevices ) {
        // 4.1 可以在这里检查GPU设备的功能并选择
        auto devFeature = phyDevice.getFeatures();
    }
    // 默认选择第一个GPU设备
    vk::PhysicalDevice selectedPhyDevice = phyDevices[0];
    XINFO("Selected GPU:\n-- {}",
          std::string(selectedPhyDevice.getProperties().deviceName));

    // 5. vk逻辑设备创建信息
    vk::DeviceCreateInfo vkDeviceCreateInfo;
    // 6. vk逻辑设备队列创建信息
    vk::DeviceQueueCreateInfo vkDeviceQueueCreateInfo;
    float                     priorities{ 1.f };  // 优先级表
    struct QueueFamilyIndicies final {
        std::optional<uint32_t> graphicsQueue;
    };
    QueueFamilyIndicies queueFamilyIndicies;
    // 6.1 查询QueueFamilyIndicies
    auto phyDeviceQueueFamilyProperties =
        selectedPhyDevice.getQueueFamilyProperties();
    for ( size_t i{ 0 }; i < phyDeviceQueueFamilyProperties.size(); ++i ) {
        const auto& phyDeviceQueueFamilyProperty =
            phyDeviceQueueFamilyProperties[i];
        auto& queueFlags = phyDeviceQueueFamilyProperty.queueFlags;
        if ( queueFlags | vk::QueueFlagBits::eGraphics ) {
            // 得是图形显卡
            queueFamilyIndicies.graphicsQueue = i;
            break;
        }
    }
    vkDeviceQueueCreateInfo.setPQueuePriorities(&priorities)
        .setQueueCount(1)
        .setQueueFamilyIndex(queueFamilyIndicies.graphicsQueue.value());

    // 将队列创建信息设置到逻辑设备创建信息中
    vkDeviceCreateInfo.setQueueCreateInfos(vkDeviceQueueCreateInfo);
    // 6.2创建vk逻辑设备 (通过物理设备)
    vk::Device vkDevice = selectedPhyDevice.createDevice(vkDeviceCreateInfo);

    // 6.3获取逻辑设备队列
    vk::Queue graphicsQueue =
        vkDevice.getQueue(queueFamilyIndicies.graphicsQueue.value(), 0);

    //


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
