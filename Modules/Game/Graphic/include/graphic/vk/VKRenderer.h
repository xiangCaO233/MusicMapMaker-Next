#pragma once

#include "graphic/vk/VKRenderPipeline.h"
#include "graphic/vk/VKSwapchain.h"
#include "vulkan/vulkan.hpp"

namespace MMM
{
namespace Graphic
{

class VKRenderer final
{

public:
    VKRenderer(vk::Device& logicalDevice, VKSwapchain& swapchain,
               VKRenderPipeline& pipeline, VKRenderPass& renderPass,
               vk::Queue& logicDeviceGraphicsQueue,
               vk::Queue& logicDevicePresentQueue);
    VKRenderer(VKRenderer&&)                 = delete;
    VKRenderer(const VKRenderer&)            = delete;
    VKRenderer& operator=(VKRenderer&&)      = delete;
    VKRenderer& operator=(const VKRenderer&) = delete;
    ~VKRenderer();

    /*
     * 执行渲染
     * */
    void render();

private:
    /*
     * vk逻辑设备引用
     * */
    vk::Device& m_vkLogicalDevice;

    /*
     * vk渲染流程引用
     * */
    VKRenderPass& m_vkRenderPass;

    /*
     * vk交换链引用
     * */
    VKSwapchain& m_vkSwapChain;

    /*
     * vk渲染管线引用
     * */
    VKRenderPipeline& m_vkRenderPipeline;

    /*
     * 逻辑设备图形队列引用
     * */
    vk::Queue& m_LogicDeviceGraphicsQueue;

    /*
     * 逻辑设备呈现队列引用
     * */
    vk::Queue& m_LogicDevicePresentQueue;

    /*
     * vk命令池对象
     * */
    vk::CommandPool m_vkCommandPool;

    /*
     * vk命令缓冲区对象
     * */
    std::vector<vk::CommandBuffer> m_vkCommandBuffers;

    /*
     * 图像可用信号量
     * */
    std::vector<vk::Semaphore> m_imageAvailableSems;

    /*
     * 图像绘制完成信号量
     * */
    std::vector<vk::Semaphore> m_imageFinishedSems;

    /*
     * 同步栅
     * */
    std::vector<vk::Fence> m_cmdAvailableFences;

    /*
     * 可用图像缓冲数量
     * */
    size_t m_avalableImageBufferCount;

    /*
     * 最大并发帧数，通常 2 就够了（双重缓冲逻辑），3 也可以
     * */
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    /*
     * 当前正在渲染的图像缓冲索引
     * */
    size_t m_currentFrameIndex{ 0 };
};

}  // namespace Graphic

}  // namespace MMM
