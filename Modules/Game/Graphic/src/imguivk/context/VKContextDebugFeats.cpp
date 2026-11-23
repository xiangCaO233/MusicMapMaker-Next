#include "common/MessageBox.h"
#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"

#include <fmt/format.h>

namespace MMM::Graphic
{
/// @brief 启用 Debug Utils 扩展并准备 instance 和 messenger 共用的回调配置。
/// @details 实例创建参数通过 pNext 借用本配置，因此创建期间也能接收诊断。
/// 独立 messenger 在实例及动态分派器就绪后创建，覆盖实例存活期间的后续调用。
/// @note 两处复用同一配置，避免启动和正常运行使用不同的诊断筛选条件。
void VKContext::enableVKDebugExt()
{
    // instance 创建时引用调试配置，扩展必须同时列入待启用的实例扩展。
    // 此函数只组装参数，不依赖尚未建立的 instance 级动态分派入口。
    m_vkExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // 在 Vulkan 层过滤严重级别，避免大量正常诊断进入回调和日志队列。
    // 警告也能定位驱动及配置问题，因此它与错误同样保留。
    m_vkDebugUtilCreateInfo
        .setMessageSeverity(vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
        // 类型遮罩与严重级别分别匹配；验证规则之外，仍需保留通用和性能警告。
        // 性能告警在 Vulkan 层已有分类，因此无需根据消息正文猜测是否记录。
        .setMessageType(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                        vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance)
        // 使用静态函数，避免回调从 pUserData 借用可能已经开始销毁的上下文。
        .setPfnUserCallback(vk::PFN_DebugUtilsMessengerCallbackEXT(
            &VKContext::vkDebug_callback));
}

/// @brief 检查构建选项要求的全部 Vulkan Validation Layer 是否可用。
///
/// 任一 layer 缺失都会终止初始化，因为后续 instance 创建参数仍会请求完整列表；
/// 与其依赖 loader 返回不透明错误，这里先给出缺失名称与目标系统安装提示。
/// @note 校验层是实例级选项，此处不依赖物理设备枚举及设备扩展状态。
/// @warning 仅在初始化阶段枚举 loader 的配置，不能从 Vulkan 调试回调中查询。
void VKContext::enableVKValidateLayer()
{
    // Vulkan 的两阶段枚举先取得数量，再由调用方分配连续存储并读取属性。
    uint32_t layerCount;
    auto     res = vk::enumerateInstanceLayerProperties(&layerCount, nullptr);

    // 按 loader 报告的总数准备存储；可用列表可能比项目请求的必需集合更大。
    // 不用请求数量给枚举结果定长，否则额外安装的层可能挤掉需要的名称。
    std::vector<vk::LayerProperties> availableLayers(layerCount);
    // 填充连续快照供后续按名称匹配，枚举索引不作为层的稳定标识。
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
            // 收集所有缺项后统一终止，避免用户逐个补齐依赖时反复经历启动失败。
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
            "This build enables Vulkan validation layers, so they are "
            "required.\n"
            "Install VK_LAYER_KHRONOS_validation on the target system.";
        XERROR("Fatal: {}", msg);
        UI::showFatalError("MusicMapMaker - Vulkan Validation Layer Missing",
                           msg);

        releaseGLFW();
        failInitialization(
            "Fatal: Not all requested validation layers are available!");
        return;
    } else {
        XDEBUG("Validation layers enabled.");
    }
}

/// @brief 将 Vulkan 验证和驱动诊断交给应用日志。
/// @param messageSeverity 触发回调的严重级别，用于选择日志级别。
/// @param messageTypes 可组合的消息分类，用于选择日志前缀。
/// @param pCallbackData 回调期间借用的数据，返回后不得保留指针。
/// @param pUserData 当前日志回调未使用的应用上下文参数。
/// @return VK_FALSE，继续触发该诊断的 Vulkan 调用。
/// @warning 回调在引发诊断的线程内执行，可能从多个线程同时进入；禁止调用
/// Vulkan 命令、变更渲染状态或等待需要由调用线程完成的工作。
VKAPI_ATTR VkBool32 VKAPI_CALL VKContext::vkDebug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT             messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
{
    // 仅维护本次调用的分类文本，不让同时进入的回调共享可变格式缓冲。
    // 多种类型位可能同时存在，沿用固定优先级只选一个前缀。
    // 每条诊断只写一条日志，保留驱动原始正文，便于对照验证层消息。
    std::string typeHeader;
    if ( messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT )
        typeHeader = "[General]";
    else if ( messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT )
        typeHeader = "[Validation]";
    else if ( messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT )
        typeHeader = "[Performance]";

    // 日志宏在本次调用内读取消息正文，不能把借用的 pMessage 排队给其他代码。
    // 级别选择也覆盖低严重度，以便日后扩展筛选时仍保留分发语义。
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
        XDEBUG("{} {}", typeHeader, pCallbackData->pMessage);
    }

    // 验证层开发可使用的 VK_TRUE 不属于应用程序的诊断返回约定。
    // 此处始终由调用方依据 Vulkan 命令结果决定后续行为。
    return VK_FALSE;
}

}  // namespace MMM::Graphic
