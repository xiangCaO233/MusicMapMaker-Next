#pragma once

#include "vk/VKRenderPass.h"
#include "vk/VKShader.h"
#include "vk/VKSwapchain.h"

namespace MMM
{
namespace Graphic
{
class VKRenderPipeline final
{
public:
    VKRenderPipeline(vk::Device& logicalDevice, VKShader& shader,
                     VKRenderPass& renderPass, VKSwapchain& swapchain, int w,
                     int h);
    VKRenderPipeline(VKRenderPipeline&&)                 = delete;
    VKRenderPipeline(const VKRenderPipeline&)            = delete;
    VKRenderPipeline& operator=(VKRenderPipeline&&)      = delete;
    VKRenderPipeline& operator=(const VKRenderPipeline&) = delete;
    ~VKRenderPipeline();

private:
    /*
     * vk逻辑设备引用
     * */
    vk::Device& m_logicalDevice;

    /*
     * vk图形渲染管线
     * */
    vk::Pipeline m_graphicsPipeline;

    /*
     * vk图形渲染管线布局
     * */
    vk::PipelineLayout m_graphicsPipelineLayout;

    friend class VKRenderer;
};


}  // namespace Graphic
}  // namespace MMM
