#include "graphic/vk/VKRenderer.h"
#include "graphic/vk/mem/VKUniforms.h"
#include "graphic/vk/mesh/VKVertex.h"
#include "log/colorful-log.h"
#include <chrono>

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

    createCommandPool();

    allocateCommandBuffers();

    createSemsWithFences();

    createMemBuffers(vkPhysicalDevice);

    uploadVertexBuffer2GPU();

    createDescriptPool();

    createDescriptSets();

    mapUniformBuffer2DescriptorSet();
}

VKRenderer::~VKRenderer()
{
    // 等待设备空闲，确保不再使用任何资源
    m_vkLogicalDevice.waitIdle();

    // 释放描述符池
    // 描述符集会随描述符池一同销毁，不必再手动销毁
    m_vkLogicalDevice.destroyDescriptorPool(m_vkDescriptorPool);
    XINFO("Destroyed Descriptor Pool.");

    // 释放VK内存缓冲区
    m_vkHostMemBuffer.reset();
    m_vkGPUMemBuffer.reset();

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

    // 分配的命令缓冲区会自动随pool一起释放
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

    // 获取当前时间
    static auto start_time =
        std::chrono::high_resolution_clock::now().time_since_epoch();
    auto since_start =
        std::chrono::high_resolution_clock::now().time_since_epoch() -
        start_time;
    auto current_time_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(since_start);
    m_testCurrentTime.time = float(current_time_ms.count()) / 1000.f;
    XINFO("current time: {}", m_testCurrentTime.time);
    // 上传uniform数据
    uploadUniformBuffer2GPU(imageIndex);

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

        // 绑定描述符集
        currentCmdBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            m_vkRenderPipeline.m_graphicsPipelineLayout,
            0,
            m_vkDescriptorSets[imageIndex],
            {});

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
                // 使用GPU内存缓冲区
                // 可以传入多个buffer
                m_vkGPUMemBuffer->m_vkBuffer,
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

/**
 * @brief 创建命令池
 */
void VKRenderer::createCommandPool()
{
    // 创建命令池
    vk::CommandPoolCreateInfo commandPoolCreateInfo;
    commandPoolCreateInfo
        // 可以随时重置
        .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);
    m_vkCommandPool =
        m_vkLogicalDevice.createCommandPool(commandPoolCreateInfo);
    XINFO("Created VK Command Pool.");
}

/**
 * @brief 分配命令缓冲区
 */
void VKRenderer::allocateCommandBuffers()
{
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
        m_vkLogicalDevice.allocateCommandBuffers(commandBufferAllocateInfo);

    // 创建上传Uniform数据的命令缓冲区
    m_uniformUploadCmdBuffers.resize(m_avalableImageBufferCount);
    for ( auto& uniformUploadCmdBuffer : m_uniformUploadCmdBuffers ) {
        // 为每一个图像缓冲分配一个用于上传Uniform数据的命令缓冲区
        vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
        commandBufferAllocateInfo
            // 要从哪个命令池分配
            .setCommandPool(m_vkCommandPool)
            // 分配1个缓冲区
            .setCommandBufferCount(1)
            .setLevel(vk::CommandBufferLevel::ePrimary);
        uniformUploadCmdBuffer = m_vkLogicalDevice.allocateCommandBuffers(
            commandBufferAllocateInfo)[0];
    }

    XINFO("Allocated VK Command Buffers.");
}

/**
 * @brief 创建信号量和栅栏
 */
void VKRenderer::createSemsWithFences()
{
    // 创建信号量和同步栅
    m_imageAvailableSems.resize(m_avalableImageBufferCount);
    m_renderFinishedSems.resize(m_avalableImageBufferCount);
    m_cmdAvailableFences.resize(m_avalableImageBufferCount);

    for ( size_t i{ 0 }; i < m_avalableImageBufferCount; ++i ) {
        // 创建信号量
        vk::SemaphoreCreateInfo semaphoreCreateInfo;
        m_imageAvailableSems[i] =
            m_vkLogicalDevice.createSemaphore(semaphoreCreateInfo);
        m_renderFinishedSems[i] =
            m_vkLogicalDevice.createSemaphore(semaphoreCreateInfo);
        XINFO("Created image Semaphores For ImageBuffer{}.", i);

        // 创建同步栅
        vk::FenceCreateInfo fenceCreateInfo;
        // 初始化为 Signaled，让第一帧可以直接通过 wait
        fenceCreateInfo.setFlags(vk::FenceCreateFlagBits::eSignaled);
        m_cmdAvailableFences[i] =
            m_vkLogicalDevice.createFence(fenceCreateInfo);
        XINFO("Created cmd Sync Fence For ImageBuffer{}.", i);
    }
}

/**
 * @brief 创建缓冲区
 *
 * @param vkPhysicalDevice 物理设备引用 (用于创建内存缓冲区)
 */
void VKRenderer::createMemBuffers(vk::PhysicalDevice& vkPhysicalDevice)
{
    // 创建VK主机内存缓冲区
    m_vkHostMemBuffer = std::make_unique<VKMemBuffer>(
        vkPhysicalDevice,
        m_vkLogicalDevice,
        sizeof(vertices),
        // 设置为传输起点
        vk::BufferUsageFlagBits::eTransferSrc,
        // 主机可访问 + 可协同工作
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent);

    // 创建VKGPU内存缓冲区
    m_vkGPUMemBuffer =
        std::make_unique<VKMemBuffer>(vkPhysicalDevice,
                                      m_vkLogicalDevice,
                                      sizeof(vertices),
                                      // 顶点缓冲区并设置为传输终点
                                      vk::BufferUsageFlagBits::eVertexBuffer |
                                          vk::BufferUsageFlagBits::eTransferDst,
                                      // 仅GPU设备本地可见
                                      vk::MemoryPropertyFlagBits::eDeviceLocal);

    // 创建主机Uniform缓冲区
    m_vkHostUniformMemBuffers.resize(m_avalableImageBufferCount);
    for ( auto& vkHostUniformMemBuffer : m_vkHostUniformMemBuffers ) {
        vkHostUniformMemBuffer = std::make_unique<VKMemBuffer>(
            vkPhysicalDevice,
            m_vkLogicalDevice,
            sizeof(Graphic::VKTestTimeUniform),
            // 设置为传输起点
            vk::BufferUsageFlagBits::eTransferSrc,
            // 主机可访问 + 可协同工作
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent);
    }

    // 创建GPUUniform缓冲区
    m_vkGPUUniformMemBuffers.resize(m_avalableImageBufferCount);
    for ( auto& vkGPUUniformMemBuffer : m_vkGPUUniformMemBuffers ) {
        vkGPUUniformMemBuffer = std::make_unique<VKMemBuffer>(
            vkPhysicalDevice,
            m_vkLogicalDevice,
            sizeof(Graphic::VKTestTimeUniform),
            // Uniform缓冲区并设置为传输终点
            vk::BufferUsageFlagBits::eUniformBuffer |
                vk::BufferUsageFlagBits::eTransferDst,
            // 仅GPU设备本地可见
            vk::MemoryPropertyFlagBits::eDeviceLocal);
    }
}

/**
 * @brief 传输数据到GPU
 */
void VKRenderer::uploadBuffer2GPU(vk::CommandBuffer&            cmdBuffer,
                                  std::unique_ptr<VKMemBuffer>& hostBuffer,
                                  std::unique_ptr<VKMemBuffer>& gpuBuffer)
{
    vk::CommandBufferBeginInfo uploadCmdBufBeginInfo;

    // 只用一次
    uploadCmdBufBeginInfo.setFlags(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    cmdBuffer.begin(uploadCmdBufBeginInfo);
    {
        vk::BufferCopy bufferCopyRegion;
        bufferCopyRegion
            // 如名
            .setSize(hostBuffer->m_bufSize)
            .setSrcOffset(0)
            .setDstOffset(0);
        cmdBuffer.copyBuffer(
            // 起始缓冲区
            hostBuffer->m_vkBuffer,
            // 目标缓冲区
            gpuBuffer->m_vkBuffer,
            // 拷贝区域
            bufferCopyRegion);
    }
    cmdBuffer.end();

    // 上传信息 - 无需信号量
    vk::SubmitInfo submitInfo;
    submitInfo.setCommandBuffers(cmdBuffer);

    // 上传命令
    m_LogicDeviceGraphicsQueue.submit(submitInfo);

    // 等待上传完毕
    m_vkLogicalDevice.waitIdle();
}

/**
 * @brief 上传顶点缓冲区到GPU
 */
void VKRenderer::uploadVertexBuffer2GPU()
{
    // 为每一个图像缓冲分配一个命令缓冲区
    vk::CommandBufferAllocateInfo commandBufferAllocateInfo;

    // 写入数据到主机内存缓冲区
    void* data = m_vkLogicalDevice.mapMemory(
        m_vkHostMemBuffer->m_vkDevMem, 0, sizeof(vertices));
    memcpy(data, vertices.data(), sizeof(vertices));
    m_vkLogicalDevice.unmapMemory(m_vkHostMemBuffer->m_vkDevMem);

    // 传输主机内存缓冲区到GPU内存缓冲区
    // 需要专门分配一个命令缓冲区用于上传
    commandBufferAllocateInfo
        // 要从哪个命令池分配
        .setCommandPool(m_vkCommandPool)
        // 分配1个缓冲区
        .setCommandBufferCount(1)
        .setLevel(vk::CommandBufferLevel::ePrimary);
    vk::CommandBuffer uploadMemCmdBuffer =
        m_vkLogicalDevice.allocateCommandBuffers(commandBufferAllocateInfo)[0];

    // 传输到对应gpu缓冲区
    uploadBuffer2GPU(uploadMemCmdBuffer, m_vkHostMemBuffer, m_vkGPUMemBuffer);

    // 销毁缓冲区
    m_vkLogicalDevice.freeCommandBuffers(m_vkCommandPool, uploadMemCmdBuffer);

    XINFO("Uploaded vertex data to gpu buffer.");
}

/**
 * @brief 创建描述符池
 */
void VKRenderer::createDescriptPool()
{
    // 描述符池创建信息
    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    // 描述符池大小
    vk::DescriptorPoolSize descriptorPoolSize;
    descriptorPoolSize
        // 描述符类型::Uniform类型 - 之后还有采样器类型等等
        .setType(vk::DescriptorType::eUniformBuffer)
        // 总共需要创建多少描述符集 (描述符集数量 * 对应的内容数量
        // (当前uniform就一个))
        .setDescriptorCount(m_avalableImageBufferCount * 1);
    descriptorPoolCreateInfo
        // 有多少帧就创建多少个描述符集
        .setMaxSets(m_avalableImageBufferCount)
        // 设置描述符池大小
        .setPoolSizes(descriptorPoolSize);

    // 创建描述符池
    m_vkDescriptorPool =
        m_vkLogicalDevice.createDescriptorPool(descriptorPoolCreateInfo);

    XINFO("Created Descriptor Pool.");
}

/**
 * @brief 创建描述符集列表
 */
void VKRenderer::createDescriptSets()
{
    // 描述符集分配信息
    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    // 复制多份setlayout用于一一对应描述符集
    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts(
        m_avalableImageBufferCount, m_vkRenderPipeline.m_descriptorSetLayout);
    descriptorSetAllocateInfo
        // 从哪个描述符池创建
        .setDescriptorPool(m_vkDescriptorPool)
        // 创建多少个描述符集
        .setDescriptorSetCount(m_avalableImageBufferCount)
        // 设置描述符集布局 - 需要一一对应
        .setSetLayouts(descriptorSetLayouts);

    // 分配描述符集
    m_vkDescriptorSets =
        m_vkLogicalDevice.allocateDescriptorSets(descriptorSetAllocateInfo);
    XINFO("Descriptor Sets Allocated.");
}

/**
 * @brief 映射uniformbuffer到对应描述符集
 */
void VKRenderer::mapUniformBuffer2DescriptorSet()
{
    for ( size_t i{ 0 }; i < m_vkDescriptorSets.size(); ++i ) {
        auto& descriptorSet           = m_vkDescriptorSets[i];
        auto& descriptorUniformBuffer = m_vkGPUUniformMemBuffers[i];

        // 描述符集写入信息
        vk::WriteDescriptorSet writeDescriptorSet;

        // 描述符缓冲区信息
        vk::DescriptorBufferInfo descriptorBufferInfo;
        descriptorBufferInfo
            .setBuffer(descriptorUniformBuffer->m_vkBuffer)
            // 无偏移量
            .setOffset(0)
            // 缓冲区范围 - 大小
            .setRange(descriptorUniformBuffer->m_bufSize);
        writeDescriptorSet
            // 描述符类型
            .setDescriptorType(vk::DescriptorType::eUniformBuffer)
            // 描述的buffer
            .setBufferInfo(descriptorBufferInfo)
            // 设置绑定号0
            .setDstBinding(0)
            // 设置关联的set
            .setDstSet(descriptorSet)
            // 如果是数组，设置为对应数组的实际对象的下标
            .setDstArrayElement(0)
            // 数组里的元素总数
            .setDescriptorCount(1);

        // 映射uniform缓冲区到描述符集
        m_vkLogicalDevice.updateDescriptorSets(writeDescriptorSet, {});
    }
}

/**
 * @brief 上传uniform缓冲区到GPU
 */
void VKRenderer::uploadUniformBuffer2GPU(int current_image_index)
{
    // 获取当前image对应的缓冲区
    auto& current_frame_host_uniform_buffer =
        m_vkHostUniformMemBuffers[current_image_index];
    auto& current_frame_gpu_uniform_buffer =
        m_vkGPUUniformMemBuffers[current_image_index];
    auto& uniformUploadCmdBuffer =
        m_uniformUploadCmdBuffers[current_image_index];

    // 映射获取主机uniform缓冲区内存地址
    auto data_ptr = m_vkLogicalDevice.mapMemory(
        current_frame_host_uniform_buffer->m_vkDevMem,
        0,
        current_frame_host_uniform_buffer->m_bufSize);

    // 写入uniform到主机uniform缓冲区
    memcpy(data_ptr, &m_testCurrentTime, sizeof(m_testCurrentTime));

    // 结束映射
    m_vkLogicalDevice.unmapMemory(
        current_frame_host_uniform_buffer->m_vkDevMem);

    // 传输到gpu对应位置
    uploadBuffer2GPU(uniformUploadCmdBuffer,
                     current_frame_host_uniform_buffer,
                     current_frame_gpu_uniform_buffer);

    // XINFO("Uploaded uniform data to gpu buffer.");
}

}  // namespace Graphic

}  // namespace MMM
