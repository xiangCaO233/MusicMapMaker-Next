#pragma once

#include "graphic/vk/VKQueueFamilyDef.h"
#include "graphic/vk/VKRenderPass.h"
#include <vulkan/vulkan.hpp>

namespace MMM
{
namespace Graphic
{
class VKSwapchain final
{
public:
    VKSwapchain(vk::PhysicalDevice& vkPhysicalDevice,
                vk::Device& vkLogicalDevice, vk::SurfaceKHR& vkSurface,
                QueueFamilyIndices& queueFamilyIndices, int w, int h);
    VKSwapchain(VKSwapchain&&)                 = delete;
    VKSwapchain(const VKSwapchain&)            = delete;
    VKSwapchain& operator=(VKSwapchain&&)      = delete;
    VKSwapchain& operator=(const VKSwapchain&) = delete;
    ~VKSwapchain();

    /*
     * 获取vk交换链创建信息
     * */
    const vk::SwapchainCreateInfoKHR& info() const;

    /*
     * 创建vk帧缓冲 - 时序要求,必须在RenderPass创建后手动调用
     * */
    void createFramebuffers(VKRenderPass& renderPass);

    /*
     * 销毁vk帧缓冲 - 时序要求,必须在Swapchain释放前手动调用
     * */
    void destroyFramebuffers();

private:
    /*
     * vk逻辑设备引用
     * */
    vk::Device& m_vkLogicalDevice;

    /*
     * vk交换链
     * */
    vk::SwapchainKHR m_swapchain;

    /*
     * vk交换链创建信息
     * */
    vk::SwapchainCreateInfoKHR m_swapchainCreateInfo;

    struct ImageBuffer {
        // vk图像
        vk::Image vk_image;
        // vk图像视图(vulkan观看图像的转换封装)
        vk::ImageView vk_imageView;
        // vk帧缓冲
        vk::Framebuffer vk_frameBuffer;
    };

    /*
     * vk的图像缓冲-匹配创建交换链时选择的数量
     * */
    std::vector<ImageBuffer> m_vkImageBuffers;

    friend class VKRenderer;
};
}  // namespace Graphic

}  // namespace MMM
