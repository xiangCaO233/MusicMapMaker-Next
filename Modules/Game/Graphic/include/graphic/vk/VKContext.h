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

/**
 * @brief 检查当前是否为 Debug 模式
 * @return true 如果是 Debug 模式
 * @return false 如果是 Release 模式
 */
static constexpr bool is_debug()
{
#ifdef BUILD_TYPE_DEBUG
    return BUILD_TYPE_DEBUG;
#else
    return false;
#endif
}

/**
 * @brief Vulkan 图形上下文类
 *
 * 负责管理 Vulkan 的实例、设备、表面、交换链等核心资源的生命周期。
 * 采用单例模式设计。
 */
class VKContext final
{
public:
    /**
     * @brief 获取 VKContext 单例实例
     *
     * 如果实例尚未创建，将尝试进行初始化。
     *
     * @return std::expected<std::reference_wrapper<VKContext>, std::string>
     *         成功返回上下文引用，失败返回错误信息字符串
     */
    static std::expected<std::reference_wrapper<VKContext>, std::string> get();

    // 禁用拷贝和移动语义
    VKContext(VKContext&&)                 = delete;
    VKContext(const VKContext&)            = delete;
    VKContext& operator=(VKContext&&)      = delete;
    VKContext& operator=(const VKContext&) = delete;

    /**
     * @brief 初始化 Vulkan 窗体相关资源
     *
     * 包括 Surface, Swapchain, RenderPass, Pipeline 等依赖窗口尺寸的资源。
     *
     * @param window_ctx GLFW 窗口句柄
     * @param w 窗口宽度
     * @param h 窗口高度
     */
    void initVKWindowRess(GLFWwindow* window_ctx, int w, int h);

    /**
     * @brief 获取渲染器实例
     * @return VKRenderer& 渲染器引用
     */
    inline VKRenderer& getRenderer() { return *m_vkRenderer; }

private:
    /**
     * @brief 私有构造函数，执行基础 Vulkan 初始化
     */
    VKContext();

    /**
     * @brief 析构函数，负责清理所有 Vulkan 资源
     */
    ~VKContext();

    // =========================================================================
    // GLFW 相关
    // =========================================================================
private:
    /**
     * @brief 初始化 GLFW 上下文
     */
    void initGLFW();

    /**
     * @brief 注册 GLFW 所需的 Vulkan 扩展
     */
    void registerGLFWExtentions();

    /**
     * @brief 释放 GLFW 资源
     */
    void releaseGLFW();

    // =========================================================================
    // Vulkan 核心初始化相关
    // =========================================================================
private:
    /// @brief Vulkan 应用程序信息结构体
    vk::ApplicationInfo m_vkAppInfo{};

    /// @brief Vulkan 实例创建信息结构体
    vk::InstanceCreateInfo m_vkInstanceCreateInfo{};

    /// @brief Vulkan 动态函数加载器 (用于加载扩展函数)
    vk::detail::DispatchLoaderDynamic m_vkDldy{};

    /// @brief Vulkan 实例句柄
    vk::Instance m_vkInstance{};

    /**
     * @brief 初始化 Vulkan 应用程序信息
     */
    void initVkAppInfo();

    /**
     * @brief 初始化 Vulkan 实例创建信息
     */
    void initVkInstanceCreateInfo();

    /// @brief 启用的 Vulkan 扩展列表类型定义
    using VKExtensions = std::vector<const char*>;

    /// @brief 当前启用的 Vulkan 扩展列表
    VKExtensions m_vkExtensions{};

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
    static VKAPI_ATTR VkBool32 VKAPI_PTR
    vkDebug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                     VkDebugUtilsMessageTypeFlagsEXT        messageTypes,
                     const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                     void*                                       pUserData);

    /**
     * @brief 启用 Vulkan Debug 扩展
     */
    void enableVKDebugExt();

    /// @brief Vulkan Debug Utils 创建信息
    vk::DebugUtilsMessengerCreateInfoEXT m_vkDebugUtilCreateInfo{};

    /// @brief Vulkan Debug Messenger 句柄
    vk::DebugUtilsMessengerEXT m_vkDebugMessenger{};

    /// @brief 请求启用的 Validation Layer 列表
    const std::array<const char*, 1> m_vkValidationLayers{
        "VK_LAYER_KHRONOS_validation",
    };

    /**
     * @brief 启用并检查 Validation Layer
     */
    void enableVKValidateLayer();

    // =========================================================================
    // 物理设备与逻辑设备相关
    // =========================================================================

    /// @brief Vulkan 表面句柄 (Window Surface)
    vk::SurfaceKHR m_vkSurface{};

    /**
     * @brief 初始化 Vulkan 表面
     * @param window_handle GLFW 窗口句柄
     */
    void initSurface(GLFWwindow* window_handle);

    /// @brief 选定的 Vulkan 物理设备 (GPU)
    vk::PhysicalDevice m_vkPhysicalDevice{};

    /**
     * @brief 挑选合适的物理设备
     */
    void pickPhysicalDevice();

    /// @brief 队列族索引结构体
    QueueFamilyIndices m_queueFamilyIndices{};

    /**
     * @brief 查询物理设备的队列族索引
     */
    void queryQueueFamilyIndices();

    /// @brief Vulkan 逻辑设备句柄
    vk::Device m_vkLogicalDevice{};

    /**
     * @brief 初始化逻辑设备
     */
    void initLogicDevice();

    /// @brief 逻辑设备图形队列句柄
    vk::Queue m_LogicDeviceGraphicsQueue;

    /// @brief 逻辑设备呈现队列句柄
    vk::Queue m_LogicDevicePresentQueue;

    // =========================================================================
    // 渲染资源相关
    // =========================================================================

    /// @brief Vulkan 交换链封装对象
    std::unique_ptr<VKSwapchain> m_swapchain{ nullptr };

    /// @brief 编译好的 Shader 模块映射表 (Name -> Shader)
    std::unordered_map<std::string, std::unique_ptr<VKShader>> m_vkShaders;

    /**
     * @brief 加载并创建 Shader 模块
     */
    void createShader();

    /// @brief Vulkan 渲染流程封装对象 (Render Pass)
    std::unique_ptr<VKRenderPass> m_vkRenderPass{ nullptr };

    /// @brief Vulkan 渲染管线封装对象 (Graphics Pipeline)
    std::unique_ptr<VKRenderPipeline> m_vkRenderPipeline{ nullptr };

    /// @brief Vulkan 渲染器封装对象 (负责 Command Buffer 和 Draw Call)
    std::unique_ptr<VKRenderer> m_vkRenderer{ nullptr };
};

}  // namespace Graphic

}  // namespace MMM
