#pragma once

#include "graphic/vk/VKRenderPass.h"
#include "graphic/vk/VKShader.h"
#include "graphic/vk/VKSwapchain.h"

namespace MMM
{
namespace Graphic
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
                     VKRenderPass& renderPass, VKSwapchain& swapchain, int w,
                     int h);

    // 禁用拷贝和移动
    VKRenderPipeline(VKRenderPipeline&&)                 = delete;
    VKRenderPipeline(const VKRenderPipeline&)            = delete;
    VKRenderPipeline& operator=(VKRenderPipeline&&)      = delete;
    VKRenderPipeline& operator=(const VKRenderPipeline&) = delete;

    ~VKRenderPipeline();

private:
    /// @brief 逻辑设备引用
    vk::Device& m_logicalDevice;

    /// @brief Vulkan 图形管线句柄
    vk::Pipeline m_graphicsPipeline;

    /// @brief Vulkan Descriptor Sets布局句柄 (描述 Uniform等资源布局)
    vk::DescriptorSetLayout m_descriptorSetLayout;

    /// @brief Vulkan 管线布局句柄 (描述 Push Constants 和 Descriptor Sets)
    vk::PipelineLayout m_graphicsPipelineLayout;

    // 允许 Renderer 访问内部 Pipeline 句柄进行绑定
    friend class VKRenderer;
};


}  // namespace Graphic
}  // namespace MMM
