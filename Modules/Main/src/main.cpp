#include "colorful-log.h"

#include "skin/SkinConfig.h"
#include "translation/Translation.h"
#include <filesystem>
#include <vulkan/vulkan.hpp>
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

    const std::vector<const char*> validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };

    // 2.检查请求的验证层是否可用
    uint32_t layerCount;
    auto     res = vk::enumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<vk::LayerProperties> availableLayers(layerCount);
    res = vk::enumerateInstanceLayerProperties(&layerCount,
                                               availableLayers.data());

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

    // 4. 填充应用程序信息 (vk::ApplicationInfo)
    vk::ApplicationInfo appInfo;
    appInfo.sType              = vk::StructureType::eApplicationInfo;
    appInfo.pApplicationName   = "MMM";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName        = "No Engine";
    appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_2;  // 或者更高版本

    // 创建vk实例
    vk::InstanceCreateInfo instanceCreateInfo;
    instanceCreateInfo.sType = vk::StructureType::eInstanceCreateInfo;

    glfwTerminate();
    XLogger::shutdown();
    return 0;
}
