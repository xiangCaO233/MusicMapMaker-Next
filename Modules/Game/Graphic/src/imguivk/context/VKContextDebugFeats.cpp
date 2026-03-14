#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{
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

}  // namespace MMM::Graphic
