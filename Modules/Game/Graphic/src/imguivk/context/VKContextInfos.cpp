#include "graphic/imguivk/VKContext.h"

namespace MMM::Graphic
{
/// @brief 填充 Vulkan instance 使用的应用与引擎版本信息。
///
/// 该结构保存在 VKContext 内，字符串均为静态字面量，因此其指针在 instance
/// 创建前始终有效。声明 Vulkan 1.4 也使 loader 能按项目使用的 API
/// 版本校验能力。
void VKContext::initVkAppInfo()
{
    m_vkAppInfo.setPApplicationName("MMM")
        .setApplicationVersion(VK_MAKE_VERSION(1, 0, 0))
        .setApiVersion(VK_API_VERSION_1_4)
        .setEngineVersion(VK_MAKE_VERSION(1, 0, 0))
        .setPEngineName("No Engine");
}

/// @brief 汇总应用信息、平台扩展与可选验证层，生成 instance 创建参数。
///
/// m_vkInstanceCreateInfo 只引用上下文成员保存的数据，调用方必须先完成 GLFW
/// 扩展注册以及 Debug 配置，并在这些容器保持不变期间创建 Vulkan instance。
void VKContext::initVkInstanceCreateInfo()
{
    // 基础路径只启用窗口系统要求的扩展；macOS 的标志与 portability 扩展必须
    // 成对出现，否则 MoltenVK 物理设备不会出现在 instance 的枚举结果中。
    m_vkInstanceCreateInfo
        .setPApplicationInfo(&m_vkAppInfo)
        // 启用的扩展
        .setPEnabledExtensionNames(m_vkExtensions)
#ifdef __APPLE__
        .setFlags(vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR)
#endif  //__APPLE__
        ;

    if ( is_debug() ) {
        // Debug messenger 创建信息通过 pNext 同时覆盖 instance 创建期间产生的
        // 验证消息；正式 messenger 会在 instance 创建成功后单独建立。
        m_vkInstanceCreateInfo.setPEnabledLayerNames(m_vkValidationLayers);
        m_vkInstanceCreateInfo.setPNext(&m_vkDebugUtilCreateInfo);
    }
}

}  // namespace MMM::Graphic
