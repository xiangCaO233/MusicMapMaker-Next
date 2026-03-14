#include "graphic/imguivk/VKContext.h"

namespace MMM::Graphic
{
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
        .setPEnabledExtensionNames(m_vkExtensions)
#ifdef __APPLE__
        .setFlags(vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR)
#endif  //__APPLE__
        ;

    if ( is_debug() ) {
        // 启用的Layer
        m_vkInstanceCreateInfo.setPEnabledLayerNames(m_vkValidationLayers);
        // 启用的Next指针(这里启用的调试信息)
        m_vkInstanceCreateInfo.setPNext(&m_vkDebugUtilCreateInfo);
    }
}

}  // namespace MMM::Graphic
