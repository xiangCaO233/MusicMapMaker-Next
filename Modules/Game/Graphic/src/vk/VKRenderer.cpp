#include "graphic/vk/VKRenderer.h"
#include "log/colorful-log.h"
#include "graphic/vk/mesh/VKVertex.h"

namespace MMM
{
namespace Graphic
{
// 顶点信息
static std::array<VKVertex, 3> vertices{
    VKVertex{ .pos   = { .x = 0.f, .y = -.5f },
              .color = { .r = 1.f, .g = 0.f, .b = 0.f, .a = .33f } },
    VKVertex{ .pos   = { .x = .5f, .y = .5f },
              .color = { .r = 0.f, .g = 1.f, .b = 0.f, .a = .66f } },
    VKVertex{ .pos   = { .x = -0.5f, .y = 0.5f },
              .color = { .r = 0.0f, .g = 0.0f, .b = 1.0f, .a = 1.0f } },

};

/**
 * @brief 构造函数，初始化渲染所需的同步对象和命令资源
 *
 * @param vkPhysicalDevice 物理设备引用 (用于创建内存缓冲区)
 * @param logicalDevice 逻辑设备引用
 * @param swapchain 交换链引用
 * @param pipeline 渲染管线引用
 * @param renderPass 渲染流程引用
 * @param logicDeviceGraphicsQueue 图形队列引用
 * @param logicDevicePresentQueue 呈现队列引用
 */
VKRenderer::VKRenderer(vk::PhysicalDevice& vkPhysicalDevice,
                       vk::Device& logicalDevice, VKSwapchain& swapchain,
                       VKRenderPipeline& pipeline, VKRenderPass& renderPass,
                       vk::Queue& logicDeviceGraphicsQueue,
                       vk::Queue& logicDevicePresentQueue)
    : m_vkLogicalDevice(logicalDevice)
    , m_vkRenderPass(renderPass)
    , m_vkSwapChain(swapchain)
    , m_vkRenderPipeline(pipeline)
    , m_LogicDeviceGraphicsQueue(logicDeviceGraphicsQueue)
    , m_LogicDevicePresentQueue(logicDevicePresentQueue)
{
    m_avalableImageBufferCount = swapchain.m_vkImageBuffers.size();
    XINFO("Avalable Image Buffer Count:{}.", m_avalableImageBufferCount);
    // XINFO("Max In Flight Frame Count set to:{}.", MAX_FRAMES_IN_FLIGHT);

    // 创建命令池
    vk::CommandPoolCreateInfo commandPoolCreateInfo;
    commandPoolCreateInfo
        // 可以随时重置
        .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);
    m_vkCommandPool = logicalDevice.createCommandPool(commandPoolCreateInfo);
    XINFO("Created VK Command Pool.");

    // 为每一个图像缓冲分配一个命令缓冲区
    vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
    commandBufferAllocateInfo
        // 要从哪个命令池分配
        .setCommandPool(m_vkCommandPool)
        // 分配图像缓冲个数个缓冲区
        .setCommandBufferCount(m_avalableImageBufferCount)
        // 主要: 可直接上gpu执行
        // 次要: 需要在主要的CommandBuffer上执行
        // 这里分配主要的
        .setLevel(vk::CommandBufferLevel::ePrimary);
    m_vkCommandBuffers =
        logicalDevice.allocateCommandBuffers(commandBufferAllocateInfo);
    XINFO("Allocated VK Command Buffers.");

    // 创建信号量和同步栅
    m_imageAvailableSems.resize(m_avalableImageBufferCount);
    m_renderFinishedSems.resize(m_avalableImageBufferCount);
    m_cmdAvailableFences.resize(m_avalableImageBufferCount);

    for ( size_t i{ 0 }; i < m_avalableImageBufferCount; ++i ) {
        // 创建信号量
        vk::SemaphoreCreateInfo semaphoreCreateInfo;
        m_imageAvailableSems[i] =
            logicalDevice.createSemaphore(semaphoreCreateInfo);
        m_renderFinishedSems[i] =
            logicalDevice.createSemaphore(semaphoreCreateInfo);
        XINFO("Created image Semaphores For ImageBuffer{}.", i);

        // 创建同步栅
        vk::FenceCreateInfo fenceCreateInfo;
        // 初始化为 Signaled，让第一帧可以直接通过 wait
        fenceCreateInfo.setFlags(vk::FenceCreateFlagBits::eSignaled);
        m_cmdAvailableFences[i] = logicalDevice.createFence(fenceCreateInfo);
        XINFO("Created cmd Sync Fence For ImageBuffer{}.", i);
    }

    // 创建VK内存缓冲区
    m_vkMemBuffer = std::make_unique<VKMemBuffer>(
        vkPhysicalDevice,
        logicalDevice,
        sizeof(vertices),
        vk::BufferUsageFlagBits::eVertexBuffer,
        // 主机可访问 + 可协同工作
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent);

    // 在这里上传一次数据
    void* data =
        logicalDevice.mapMemory(m_vkMemBuffer->m_vkDevMem, 0, sizeof(vertices));
    memcpy(data, vertices.data(), sizeof(vertices));
    logicalDevice.unmapMemory(m_vkMemBuffer->m_vkDevMem);

    XINFO("Uploaded vertex data to buffer.");
}

VKRenderer::~VKRenderer()
{
    // 等待设备空闲，确保不再使用任何资源
    m_vkLogicalDevice.waitIdle();

    // 释放VK内存缓冲区
    m_vkMemBuffer.reset();

    for ( auto& cmdAvailableFence : m_cmdAvailableFences ) {
        m_vkLogicalDevice.destroyFence(cmdAvailableFence);
    }
    XINFO("Destroyed cmd Sync Fences.");

    for ( auto& imageAvailableSem : m_imageAvailableSems ) {
        m_vkLogicalDevice.destroySemaphore(imageAvailableSem);
    }
    for ( auto& renderFinishedSem : m_renderFinishedSems ) {
        m_vkLogicalDevice.destroySemaphore(renderFinishedSem);
    }
    XINFO("Destroyed All image Semaphores.");

    // for ( auto& vkCommandBuffer : m_vkCommandBuffers ) {
    //     m_vkLogicalDevice.freeCommandBuffers(m_vkCommandPool,
    //     vkCommandBuffer);
    // }
    // XINFO("Destroyed VK Command Buffers.");

    m_vkLogicalDevice.destroyCommandPool(m_vkCommandPool);
    XINFO("Destroyed VK Command Pool.");
}

/**
 * @brief 执行单帧渲染
 *
 * 包含等待 Fence、获取图像、录制命令、提交队列、呈现图像等步骤。
 */
void VKRenderer::render()
{
    // 等待cmd完成
    auto waitResult = m_vkLogicalDevice.waitForFences(
        m_cmdAvailableFences[m_currentFrameIndex],
        true,
        std::numeric_limits<uint64_t>::max());
    if ( waitResult != vk::Result::eSuccess ) {
        XWARN("VK Device WaitForFences failed");
    }

    // 恢复fence
    m_vkLogicalDevice.resetFences(m_cmdAvailableFences[m_currentFrameIndex]);

    // 请求下一个可绘制的图像 - 查到的同时发出图像可用信号量
    auto imageResult = m_vkLogicalDevice.acquireNextImageKHR(
        m_vkSwapChain.m_swapchain,
        std::numeric_limits<uint64_t>::max(),
        m_imageAvailableSems[m_currentFrameIndex]);
    if ( imageResult.result != vk::Result::eSuccess ) {
        XWARN("acquire ImageKHR failed");
    }
    // 获取到实际查询到的可绘制的图像下标
    auto imageIndex = imageResult.value;

    // 重置命令缓冲
    auto& currentCmdBuffer = m_vkCommandBuffers[m_currentFrameIndex];
    currentCmdBuffer.reset();

    // 准备开始输入命令
    vk::CommandBufferBeginInfo commandBufferBeginInfo;
    commandBufferBeginInfo
        // 设置用法
        // 只提交一次,提交完就不用了
        .setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    // 生命周期覆盖整个渲染流程
    // .setFlags(vk::CommandBufferUsageFlagBits::eRenderPassContinue);
    // 无限复用
    // .setFlags(vk::CommandBufferUsageFlagBits::eSimultaneousUse);

    // 渲染区域
    vk::Rect2D renderArea;
    auto       swapchainCreateInfo = m_vkSwapChain.info();
    // clearmask - 类似opengl的清屏颜色
    vk::ClearValue      clearValue;
    vk::ClearColorValue clearColorValue(std::array{ .23f, .23f, .23f, 1.f });
    clearValue.setColor(clearColorValue);

    // 命令录制
    currentCmdBuffer.begin(commandBufferBeginInfo);
    {
        renderArea = { { 0,
                         0,
                         swapchainCreateInfo.imageExtent.width,
                         swapchainCreateInfo.imageExtent.height } };

        // 绑定渲染管线
        currentCmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                      m_vkRenderPipeline.m_graphicsPipeline);

        // 开始渲染流程
        vk::RenderPassBeginInfo renderPassBeginInfo;
        renderPassBeginInfo
            // 设置渲染流程
            .setRenderPass(m_vkRenderPass.m_graphicRenderPass)
            // 设置渲染区域
            .setRenderArea(renderArea)
            // 设置要绘制到哪个帧缓冲上(上面查到了索引直接用)
            .setFramebuffer(
                m_vkSwapChain.m_vkImageBuffers[imageIndex].vk_frameBuffer)
            // clearmask - 类似opengl的清屏颜色
            .setClearValues(clearValue);

        // 真 - 命令录制
        currentCmdBuffer.beginRenderPass(renderPassBeginInfo, {});
        {
            // 绑定顶点缓冲区
            currentCmdBuffer.bindVertexBuffers(
                // 多个buffer时要绑定第几个
                0,
                // 可以传入多个buffer
                m_vkMemBuffer->m_vkBuffer,
                // 偏移量
                { 0 });
            currentCmdBuffer.draw(
                // 绘制几个顶点
                3
                // 绘制几个实例
                ,
                1
                // 从第几个顶点开始
                ,
                0
                // 从第几个实例开始
                ,
                0);
        }
        // 结束渲染流程
        currentCmdBuffer.endRenderPass();
    }
    currentCmdBuffer.end();  // 结束命令录制

    // 准备等待的阶段掩码
    // 这表示：在流水线的“颜色附件输出”阶段等待信号量
    // 也就是说，可以在图像还没准备好时就开始执行顶点着色器，
    // 但必须等到图像准备好了才能写入颜色。
    vk::PipelineStageFlags waitStages[] = {
        vk::PipelineStageFlagBits::eColorAttachmentOutput
    };
    // 发送命令到gpu执行绘制 - 通过图形渲染队列
    vk::SubmitInfo submitInfo;
    submitInfo
        // 设置命令缓冲区
        .setCommandBuffers(currentCmdBuffer)
        // 等待信号量
        .setWaitSemaphores(m_imageAvailableSems[m_currentFrameIndex])
        // 设置等待的阶段掩码
        .setWaitDstStageMask(waitStages)
        // 发出信号量
        .setSignalSemaphores(m_renderFinishedSems[imageIndex]);
    m_LogicDeviceGraphicsQueue.submit(
        submitInfo, m_cmdAvailableFences[m_currentFrameIndex]);

    // 呈现
    vk::PresentInfoKHR presentInfo;
    presentInfo
        // 呈现哪张图像?上面拿到的
        .setImageIndices(imageIndex)
        // 设置交换链
        .setSwapchains(m_vkSwapChain.m_swapchain)
        // 等待信号量
        .setWaitSemaphores(m_renderFinishedSems[imageIndex]);
    auto presentResult = m_LogicDevicePresentQueue.presentKHR(presentInfo);
    if ( presentResult != vk::Result::eSuccess ) {
        XWARN("Present failed");
    }

    // 并发帧数步进
    ++m_currentFrameIndex %= MAX_FRAMES_IN_FLIGHT;
}

}  // namespace Graphic

}  // namespace MMM
