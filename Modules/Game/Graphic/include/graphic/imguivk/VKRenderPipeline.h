#pragma once

#include "graphic/imguivk/VKRenderPass.h"
#include "graphic/imguivk/VKShader.h"

namespace MMM::Graphic
{

/**
 * @brief Vulkan 图形渲染管线封装类 (Graphics Pipeline)
 *
 * 封装了从顶点输入、着色器阶段、光栅化到颜色混合的所有固定管线状态。
 * 一旦创建，管线状态不可变（Immutable）。
 */
class VKRenderPipeline final
{
public:
    /**
     * @brief 构造函数，创建图形管线
     *
     * @param logicalDevice 逻辑设备引用
     * @param shader 着色器管理器引用 (提供 Shader Stages)
     * @param renderPass 渲染流程引用 (提供附件格式兼容性)
     * @param swapchain 交换链引用
     * @param w 视口宽度
     * @param h 视口高度
     */
    VKRenderPipeline(vk::Device& logicalDevice, VKShader& shader,
                     VKRenderPass& renderPass, VKSwapchain& swapchain,
                     bool is2DCanvas, int w = 0, int h = 0,
                     bool additiveBlend = false, bool blendEnable = true,
                     vk::DescriptorSetLayout sharedLayout = VK_NULL_HANDLE);

    // 禁用拷贝和移动
    VKRenderPipeline(VKRenderPipeline&&) = delete;

    VKRenderPipeline(const VKRenderPipeline&) = delete;

    VKRenderPipeline& operator=(VKRenderPipeline&&) = delete;

    VKRenderPipeline& operator=(const VKRenderPipeline&) = delete;

    ~VKRenderPipeline();

    inline vk::DescriptorSetLayout getDescriptorSetLayout() const
    {
        return m_descriptorSetLayout;
    }

    inline vk::PipelineLayout getPipelineLayout() const
    {
        return m_graphicsPipelineLayout;
    }

private:
    /// @brief 逻辑设备引用
    vk::Device& m_logicalDevice;

    /// @brief Vulkan 图形管线句柄
    vk::Pipeline m_graphicsPipeline;

    /// @brief Vulkan Descriptor Sets布局句柄 (描述 Uniform等资源布局)
    vk::DescriptorSetLayout m_descriptorSetLayout;

    /// @brief 是否拥有该布局 (是否需要在析构时销毁)
    bool m_ownDescriptorSetLayout{ true };

    /// @brief Vulkan 管线布局句柄 (描述 Push Constants 和 Descriptor Sets)
    vk::PipelineLayout m_graphicsPipelineLayout;
    friend class VKOffScreenRenderer;
};


}  // namespace MMM::Graphic
