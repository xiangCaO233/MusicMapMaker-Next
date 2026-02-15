#pragma once

#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
{
class VKSwapchain;

/**
 * @brief Vulkan 渲染流程封装类 (Render Pass)
 *
 * 描述了帧缓冲区附件的格式、加载/存储操作以及子流程依赖关系。
 * 是创建 Graphics Pipeline 和 Framebuffer 的必要依赖。
 */
class VKRenderPass final
{
public:
    /**
     * @brief 构造函数
     * @param logicalDevice Vulkan 逻辑设备引用
     * @param swapchain 交换链引用 (用于获取图像格式)
     */
    VKRenderPass(vk::Device& logicalDevice, VKSwapchain& swapchain);

    // 禁用拷贝和移动
    VKRenderPass(VKRenderPass&&)                 = delete;
    VKRenderPass(const VKRenderPass&)            = delete;
    VKRenderPass& operator=(VKRenderPass&&)      = delete;
    VKRenderPass& operator=(const VKRenderPass&) = delete;

    ~VKRenderPass();

private:
    /// @brief 逻辑设备引用，用于销毁资源
    vk::Device& m_logicalDevice;

    /// @brief Vulkan 渲染流程句柄
    vk::RenderPass m_graphicRenderPass;

    // 允许 Renderer, Swapchain, Pipeline 访问内部 RenderPass 句柄
    friend class VKRenderer;
    friend class VKSwapchain;
    friend class VKRenderPipeline;
};


} // namespace MMM::Graphic


