#pragma once

#include "graphic/glfw/GLFWHeader.h"
#include "graphic/imguivk/VKQueueFamilyDef.h"
#include "graphic/imguivk/VKRenderPass.h"
#include "graphic/imguivk/VKRenderPipeline.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "imgui_impl_vulkan.h"
#include <expected>
#include <memory>
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
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

    VKContext();
    VKContext(VKContext&&)                 = delete;
    VKContext(const VKContext&)            = delete;
    VKContext& operator=(VKContext&&)      = delete;
    VKContext& operator=(const VKContext&) = delete;
    ~VKContext();

    /**
     * @brief 初始化 Vulkan 窗体相关资源
     *
     * 包括 Surface, Swapchain, RenderPass, Pipeline 等依赖窗口尺寸的资源。
     *
     * @param window_ctx GLFW 窗口句柄
     * @param w 窗口宽度
     * @param h 窗口高度
     */
    void initVKWindowRess(GLFWwindow* window, int width, int height);

    /**
     * @brief 获取渲染器实例
     * @return VKRenderer& 渲染器引用
     */
    inline VKRenderer& getRenderer() { return *m_vkRenderer; }

    inline void setVSync(bool enabled)
    {
        // 1. 等待设备空闲，因为要修改交换链
        m_vkLogicalDevice.waitIdle();

        // 2. 修改交换链配置类里的 PresentMode 偏好
        if ( enabled ) {
            VKSwapchain::s_globalPresentMode = vk::PresentModeKHR::eFifo;
        } else {
            // fallback 为立即模式
            VKSwapchain::s_globalPresentMode = vk::PresentModeKHR::eImmediate;
            // 查询物理设备支持的呈现模式
            std::vector<vk::PresentModeKHR> supported_presentModes =
                m_vkPhysicalDevice.getSurfacePresentModesKHR(m_vkSurface);
            for ( const auto& presentMode : supported_presentModes ) {
                // 无限帧数优选mailbox模式
                // 直接取当前时刻gpu产出的最新的图像用于绘制(刷新率高且不撕裂)
                if ( presentMode == vk::PresentModeKHR::eMailbox ) {
                    VKSwapchain::s_globalPresentMode =
                        vk::PresentModeKHR::eMailbox;
                    break;
                }
            }
        }

        // 3. 标记需要重建
        m_swapchain->markDirty();
    }

private:
    /**
     * @brief 初始化 GLFW 上下文
     */
    static void initGLFW();

    /**
     * @brief 注册 GLFW 所需的 Vulkan 扩展
     */
    void registerGLFWExtensions();

    /**
     * @brief 释放 GLFW 资源
     */
    static void releaseGLFW();

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

    /// @brief 选定的 Vulkan 物理设备 (GPU)
    vk::PhysicalDevice m_vkPhysicalDevice{};

    /// @brief 队列族索引结构体
    QueueFamilyIndices m_queueFamilyIndices{};

    /// @brief 逻辑设备图形队列句柄
    vk::Queue m_LogicDeviceGraphicsQueue;

    /// @brief 逻辑设备呈现队列句柄
    vk::Queue m_LogicDevicePresentQueue;

    /**
     * @brief 使用imgui自动选择物理设备和队列族
     */
    void imguiAutoSelect();

    /// @brief Vulkan 逻辑设备句柄
    vk::Device m_vkLogicalDevice{};

    /**
     * @brief 初始化逻辑设备
     */
    void initLogicDevice();

    /// @brief Vulkan 表面句柄 (Window Surface)
    vk::SurfaceKHR m_vkSurface{};

    // =========================================================================
    // 渲染资源相关
    // =========================================================================

    /// @brief Vulkan 交换链封装对象
    std::unique_ptr<VKSwapchain> m_swapchain{ nullptr };

    /**
     * @brief 重建交换链
     */
    void recreateSwapchain(GLFWwindow* window_context, int width, int height);

    /// @brief Vulkan 渲染流程封装对象 (Render Pass)
    std::unique_ptr<VKRenderPass> m_vkRenderPass{ nullptr };

    /// @brief Vulkan 渲染器封装对象 (负责 Command Buffer 和 Draw Call)
    std::unique_ptr<VKRenderer> m_vkRenderer{ nullptr };

    // =========================================================================
    // imgui - 实现相关
    // =========================================================================

    /**
     * @brief 初始化 imgui Vulkan
     */
    void imguiVulkanInit(GLFWwindow* iwindow_handle);

    friend class VKRenderer;
};

}  // namespace MMM::Graphic
