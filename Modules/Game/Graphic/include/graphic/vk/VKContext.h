#pragma once

#include "VKShader.h"
#include "graphic/vk/VKQueueFamilyDef.h"
#include "graphic/vk/VKRenderPass.h"
#include "graphic/vk/VKRenderPipeline.h"
#include "graphic/vk/VKRenderer.h"
#include "graphic/vk/VKSwapchain.h"
#include <array>
#include <expected>
#include <memory>
#include <unordered_map>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_handles.hpp>
#include <vulkan/vulkan_structs.hpp>

struct GLFWwindow;

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
class VKContext final
{
public:
    // 单例&&立即初始化模式
    static std::expected<std::reference_wrapper<VKContext>, std::string> get();

    VKContext(VKContext&&)                 = delete;
    VKContext(const VKContext&)            = delete;
    VKContext& operator=(VKContext&&)      = delete;
    VKContext& operator=(const VKContext&) = delete;

    /*
     * 初始化VK窗体相关资源
     * */
    void initVKWindowRess(GLFWwindow* window_ctx, int w, int h);

    /*
     * 获取渲染器
     * */
    inline VKRenderer& getRenderer() { return *m_vkRenderer; }

private:
    VKContext();
    ~VKContext();

    // GLFW相关
private:
    /*
     * 初始化GLFW上下文
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
    vk::ApplicationInfo m_vkAppInfo{};

    /*
     * vk实例创建信息
     * */
    vk::InstanceCreateInfo m_vkInstanceCreateInfo{};

    /*
     * vk动态加载器
     * */
    vk::detail::DispatchLoaderDynamic m_vkDldy{};

    /*
     * vk实例
     * */
    vk::Instance m_vkInstance{};

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
    VKExtensions m_vkExtensions{};

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
    vk::DebugUtilsMessengerCreateInfoEXT m_vkDebugUtilCreateInfo{};

    /*
     * vkdebug调试信息工具
     * */
    vk::DebugUtilsMessengerEXT m_vkDebugMessenger{};

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

    /*
     * vk表面句柄
     * */
    vk::SurfaceKHR m_vkSurface{};

    /*
     * 初始化vk表面句柄
     * */
    void initSurface(GLFWwindow* window_handle);

    /*
     * vk物理设备
     * */
    vk::PhysicalDevice m_vkPhysicalDevice{};

    /*
     * 挑选vk物理设备
     * */
    void pickPhysicalDevice();

    /*
     * vk逻辑设备图形队列族索引
     * */
    QueueFamilyIndices m_queueFamilyIndices{};

    /*
     * 查询图形队列族索引
     * */
    void queryQueueFamilyIndices();

    /*
     * vk逻辑设备
     * */
    vk::Device m_vkLogicalDevice{};

    /*
     * 初始化vk逻辑设备
     * */
    void initLogicDevice();

    /*
     * 逻辑设备图形队列
     * */
    vk::Queue m_LogicDeviceGraphicsQueue;

    /*
     * 逻辑设备呈现队列
     * */
    vk::Queue m_LogicDevicePresentQueue;

    /*
     * vk交换链
     * */
    std::unique_ptr<VKSwapchain> m_swapchain{ nullptr };

    /*
     * vk着色器表
     * */
    std::unordered_map<std::string, std::unique_ptr<VKShader>> m_vkShaders;

    /*
     * 创建vk着色器
     * */
    void createShader();

    /*
     * vk渲染流程
     * */
    std::unique_ptr<VKRenderPass> m_vkRenderPass{ nullptr };

    /*
     * vk渲染管线
     * */
    std::unique_ptr<VKRenderPipeline> m_vkRenderPipeline{ nullptr };

    /*
     * vk渲染器
     * */
    std::unique_ptr<VKRenderer> m_vkRenderer{ nullptr };
};

}  // namespace Graphic

}  // namespace MMM
