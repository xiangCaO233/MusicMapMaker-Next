#pragma once

#include "graphic/imguivk/VKContext.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace MMM::Graphic
{
/// @brief 图形启动诊断及 Vulkan 调试消息器的私有所有者。
/// @details 采集函数借用已创建的实例、设备或表面，不改变其生命周期。
/// 消息器属于实例，调用方须在销毁实例及其动态分派器前调用清理入口。
struct VKContext::StartupDiagnostics {
    /// @brief 追加一行启动阶段图形诊断信息。
    /// @param line 诊断信息文本。
    void addStartupDiagnostic(std::string line);

    /// @brief 采集 GLFW 版本、平台和 Vulkan loader 可用性。
    void collectGLFWDiagnostics();

    /// @brief 采集 GLFW 最近一次错误信息。
    /// @param context 触发 GLFW 错误查询的上下文。
    void collectLastGLFWErrorDiagnostic(const char* context);

    /// @brief 采集 Vulkan loader 版本、instance 扩展和 layer 信息。
    void collectVulkanLoaderDiagnostics();

    /// @brief 采集 Vulkan instance 创建请求信息。
    void collectVulkanInstanceCreateDiagnostics(
        const vk::ApplicationInfo&      appInfo,
        const std::vector<const char*>& extensions);

    /// @brief 采集 Vulkan instance 创建后的物理设备基础信息。
    /// @param includeSurfaceSupport 是否同时查询窗口 surface 呈现支持。
    void collectPhysicalDeviceDiagnostics(vk::Instance   instance,
                                          vk::SurfaceKHR surface,
                                          bool           includeSurfaceSupport);

    /// @brief 采集选中物理设备的窗口 surface 支持信息。
    /// @param width 请求的窗口宽度。
    /// @param height 请求的窗口高度。
    void collectSelectedSurfaceDiagnostics(vk::PhysicalDevice physicalDevice,
                                           vk::SurfaceKHR surface, int width,
                                           int height);

    /// @brief 采集逻辑设备创建请求信息。
    /// @param deviceExtensions 请求启用的 device 扩展。
    /// @param queueFamilies 请求创建队列的队列族索引。
    void collectLogicalDeviceCreateDiagnostics(
        const std::vector<const char*>& deviceExtensions,
        const std::vector<uint32_t>&    queueFamilies);

    /// @brief 打印启动阶段已经采集到的诊断快照。
    /// @param reason 导致打印诊断信息的失败原因。
    void logStartupDiagnostics(const char* reason);

    /**
     * @brief 启用 Vulkan Debug 扩展
     */
    void enableVKDebugExt(std::vector<const char*>& extensions);

    /// @brief 返回构建时是否启用了 Vulkan 验证层及对应调试消息机制。
    /// @return 启用验证层时为 true，否则为 false。
    static bool validationLayersEnabled();

    /**
     * @brief 启用并检查 Validation Layer
     */
    bool checkValidationLayers();


    /// @brief 让实例创建期间也能使用本组件保存的验证回调配置。
    void configureInstance(vk::InstanceCreateInfo& info) const;

    /// @brief 创建与给定实例同生命周期的验证消息器。
    vk::Result createMessenger(
        vk::Instance                             instance,
        const vk::detail::DispatchLoaderDynamic& dispatch);

    /// @brief 在实例销毁前回收验证消息器，允许无消息器的失败路径。
    void destroyMessenger(vk::Instance                             instance,
                          const vk::detail::DispatchLoaderDynamic& dispatch);

private:
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

    /// @brief 启动阶段图形诊断信息缓存。
    std::vector<std::string> m_startupDiagnosticLines;

    /// @brief 启动诊断信息是否已因失败打印过。
    bool m_startupDiagnosticsPrinted{ false };

    /// @brief Vulkan Debug Utils 创建信息
    vk::DebugUtilsMessengerCreateInfoEXT m_vkDebugUtilCreateInfo{};

    /// @brief Vulkan Debug Messenger 句柄
    vk::DebugUtilsMessengerEXT m_vkDebugMessenger{};

    /// @brief 请求启用的 Validation Layer 列表
    const std::array<const char*, 1> m_vkValidationLayers{
        "VK_LAYER_KHRONOS_validation",
    };
};
}  // namespace MMM::Graphic
