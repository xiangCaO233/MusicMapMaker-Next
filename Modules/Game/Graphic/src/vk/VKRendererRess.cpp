#include "graphic/vk/VKRenderer.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{

// 顶点信息
std::array<VKVertex, 3> VKRenderer::s_vertices{
    VKVertex{ .pos = { .x = 0.f, .y = -.5f },
              .color = { .r = 1.f, .g = 0.f, .b = 0.f, .a = .33f } },
    VKVertex{ .pos = { .x = .5f, .y = .5f },
              .color = { .r = 0.f, .g = 1.f, .b = 0.f, .a = .66f } },
    VKVertex{ .pos = { .x = -0.5f, .y = 0.5f },
              .color = { .r = 0.0f, .g = 0.0f, .b = 1.0f, .a = 1.0f } },

};

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
    // 为每一个图像缓冲分配一个用于上传Uniform数据的命令缓冲区
    m_uniformUploadCmdBuffers = m_vkLogicalDevice.allocateCommandBuffers(commandBufferAllocateInfo);

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
        sizeof(s_vertices),
        // 设置为传输起点
        vk::BufferUsageFlagBits::eTransferSrc,
        // 主机可访问 + 可协同工作
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent);

    // 创建VKGPU内存缓冲区
    m_vkGPUMemBuffer =
        std::make_unique<VKMemBuffer>(vkPhysicalDevice,
                                      m_vkLogicalDevice,
                                      sizeof(s_vertices),
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
void VKRenderer::mapUniformBuffer2DescriptorSet() const
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

}
