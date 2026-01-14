#pragma once

#include <expected>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_handles.hpp>
#include <vulkan/vulkan_structs.hpp>

namespace MMM
{
namespace Graphic
{

static constexpr bool is_debug()
{
#ifdef BUILD_TYPE_DEBUG
    return BUILD_TYPE_DEBUG;
#else
    return false;
#endif
}

/*
 * 图形Vulkan上下文
 * */
class VKContext
{
public:
    // 单例&&立即初始化模式
    static std::expected<std::reference_wrapper<VKContext>, std::string> get();

    VKContext(VKContext&&)                 = delete;
    VKContext(const VKContext&)            = delete;
    VKContext& operator=(VKContext&&)      = delete;
    VKContext& operator=(const VKContext&) = delete;

private:
    VKContext();
    ~VKContext();

    // GLFW相关
private:
    /*
     * 初始化GLFW窗口上下文
     * */
    void initGLFW();

    /*
     * 注册GLFW窗口扩展
     * */
    void registerGLFWExtentions();

    /*
     * 释放GLFW
     * */
    void releaseGLFW();

    // VK相关
private:
    /*
     * vk应用程序信息
     * */
    vk::ApplicationInfo m_vkAppInfo;

    /*
     * vk实例创建信息
     * */
    vk::InstanceCreateInfo m_vkInstanceCreateInfo;

    /*
     * vk动态加载器
     * */
    vk::detail::DispatchLoaderDynamic m_vkDldy;

    /*
     * vk实例
     * */
    vk::Instance m_vkInstance;

    /*
     * vk设备
     * */
    vk::PhysicalDevice m_vkPhysicalDevice;

    /*
     * 初始化vk应用程序信息
     * */
    void initVkAppInfo();

    /*
     * 初始化vk实例创建信息
     * */
    void initVkInstanceCreateInfo();

    /*
     * vk扩展列表
     * */
    using VKExtensions = std::vector<const char*>;
    VKExtensions m_vkExtensions;

    // vk debug回调
    static VKAPI_ATTR VkBool32 VKAPI_PTR
    vkDebug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                     VkDebugUtilsMessageTypeFlagsEXT        messageTypes,
                     const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                     void*                                       pUserData);

    /*
     * 启用vkdebug扩展
     * */
    void enableVKDebugExt();

    /*
     * vk调试创建信息
     * */
    vk::DebugUtilsMessengerCreateInfoEXT m_vkDebugUtilCreateInfo;

    /*
     * vkdebug调试信息工具
     * */
    vk::DebugUtilsMessengerEXT m_vkDebugMessenger;

    /*
     * vk验证层列表
     * */
    const std::array<const char*, 1> m_vkValidationLayers{
        "VK_LAYER_KHRONOS_validation",
    };

    /*
     * 启用vk验证层
     * */
    void enableVKValidateLayer();
};



}  // namespace Graphic

}  // namespace MMM
