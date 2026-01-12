#pragma once

#include <vulkan/vulkan.hpp>

namespace MMM
{
namespace Graphic
{
class VKSwapchain;

class VKRenderPass final
{
public:
    VKRenderPass(vk::Device& logicalDevice, VKSwapchain& swapchain);
    VKRenderPass(VKRenderPass&&)                 = delete;
    VKRenderPass(const VKRenderPass&)            = delete;
    VKRenderPass& operator=(VKRenderPass&&)      = delete;
    VKRenderPass& operator=(const VKRenderPass&) = delete;
    ~VKRenderPass();

private:
    /*
     * vk逻辑设备引用
     * */
    vk::Device& m_logicalDevice;

    /*
     * vk图形渲染流程
     * */
    vk::RenderPass m_graphicRenderPass;

    friend class VKRenderer;
    friend class VKSwapchain;
    friend class VKRenderPipeline;
};


}  // namespace Graphic

}  // namespace MMM
