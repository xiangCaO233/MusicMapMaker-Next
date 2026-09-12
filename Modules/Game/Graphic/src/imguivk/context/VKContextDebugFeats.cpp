#include "common/MessageBox.h"
#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"

#include <fmt/format.h>

namespace MMM::Graphic
{
/// @brief 注册 Vulkan Debug Utils 扩展并配置验证消息回调。
///
/// 这里只准备 instance 创建参数；messenger 本身在 instance 创建成功且动态分派器
/// 可用后建立。当前仅接收 warning 与 error，避免一般信息淹没启动诊断。
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
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
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

/// @brief 检查 Debug 构建要求的全部 Vulkan Validation Layer 是否可用。
///
/// 任一 layer 缺失都会终止初始化，因为后续 instance 创建参数仍会请求完整列表；
/// 与其依赖 loader 返回不透明错误，这里先给出缺失名称与安装 Vulkan SDK 的提示。
void VKContext::enableVKValidateLayer()
{
    // Vulkan 的两阶段枚举先取得数量，再由调用方分配连续存储并读取属性。
    uint32_t layerCount;
    auto     res = vk::enumerateInstanceLayerProperties(&layerCount, nullptr);

    // 1.1先获取验证层的数量-会填入layerCount
    std::vector<vk::LayerProperties> availableLayers(layerCount);
    // 然后才是获取所有的验证层-会填入availableLayers.data()
    res = vk::enumerateInstanceLayerProperties(&layerCount,
                                               availableLayers.data());

    // 按名称逐项匹配而不依赖枚举顺序，确保配置中的每个必需 layer 都存在。
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
            addStartupDiagnostic(
                fmt::format("Validation layer missing: {}", layerName));
            allLayersAvailable = false;
        }
    }

    // 此时尚未创建 Vulkan instance，失败路径只需关闭 GLFW 并冻结初始化错误。
    if ( !allLayersAvailable ) {
        logStartupDiagnostics("Requested Vulkan validation layer is missing.");
        std::string msg =
            "Vulkan validation layers are missing.\n"
            "Since you are running a Debug build, these layers are required.\n"
            "Please download and install the Vulkan SDK to enable debugging "
            "features.";
        XERROR("Fatal: {}", msg);
        UI::showFatalError("MusicMapMaker - Vulkan SDK Missing", msg);

        releaseGLFW();
        failInitialization(
            "Fatal: Not all requested validation layers are available!");
        return;
    } else {
        XDEBUG("Validation layers enabled.");
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
        XDEBUG("{} {}", typeHeader, pCallbackData->pMessage);
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

}  // namespace MMM::Graphic
