#include "graphic/imguivk/VKRenderer.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{

void VKRenderer::initCursorManager(vk::PhysicalDevice& vkPhysicalDevice,
                                   vk::Device&         logicalDevice)
{
    // 创建光标管理器
    m_cursorManager =
        std::make_unique<CursorManager>(vkPhysicalDevice,
                                        logicalDevice,
                                        m_vkCommandPool,
                                        m_LogicDeviceGraphicsQueue);
}

void VKRenderer::releaseCursorManager()
{
    // 释放光标管理器
    m_cursorManager.reset();
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
        m_vkLogicalDevice.createCommandPool(commandPoolCreateInfo).value;
    XDEBUG("Created VK Command Pool.");
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
        m_vkLogicalDevice.allocateCommandBuffers(commandBufferAllocateInfo)
            .value;

    XDEBUG("Allocated VK Command Buffers.");
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
            m_vkLogicalDevice.createSemaphore(semaphoreCreateInfo).value;
        m_renderFinishedSems[i] =
            m_vkLogicalDevice.createSemaphore(semaphoreCreateInfo).value;
        XDEBUG("Created image Semaphores For ImageBuffer{}.", i);

        // 创建同步栅
        vk::FenceCreateInfo fenceCreateInfo;
        // 初始化为 Signaled，让第一帧可以直接通过 wait
        fenceCreateInfo.setFlags(vk::FenceCreateFlagBits::eSignaled);
        m_cmdAvailableFences[i] =
            m_vkLogicalDevice.createFence(fenceCreateInfo).value;
        XDEBUG("Created cmd Sync Fence For ImageBuffer{}.", i);
    }
}

/**
 * @brief 创建描述符池
 */
void VKRenderer::createDescriptPool()
{
    // 定义 ImGui 需要的各种描述符类型的大小
    // 这些数量通常给得比较从容，以防 ImGui 插件或大量贴图使用
    std::array<vk::DescriptorPoolSize, 11> poolSizes = {
        { { vk::DescriptorType::eSampler, 1000 },
          { vk::DescriptorType::eCombinedImageSampler, 1000 },
          { vk::DescriptorType::eSampledImage, 1000 },
          { vk::DescriptorType::eStorageImage, 1000 },
          { vk::DescriptorType::eUniformTexelBuffer, 1000 },
          { vk::DescriptorType::eStorageTexelBuffer, 1000 },
          { vk::DescriptorType::eUniformBuffer, 1000 },
          { vk::DescriptorType::eStorageBuffer, 1000 },
          { vk::DescriptorType::eUniformBufferDynamic, 1000 },
          { vk::DescriptorType::eStorageBufferDynamic, 1000 },
          { vk::DescriptorType::eInputAttachment, 1000 } }
    };

    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setFlags(vk::DescriptorPoolCreateFlagBits::
                      eFreeDescriptorSet)  // 允许 ImGui 动态增删贴图
        .setMaxSets(1000 * poolSizes.size())
        .setPoolSizes(poolSizes);

    m_vkDescriptorPool = m_vkLogicalDevice.createDescriptorPool(poolInfo).value;
    XDEBUG("Created Global Descriptor Pool for ImGui.");

    // 创建画笔纹理共享布局
    vk::DescriptorSetLayoutBinding binding0(
        0,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment,
        nullptr);
    vk::DescriptorSetLayoutCreateInfo layoutInfo({}, binding0);
    m_brushTextureLayout =
        m_vkLogicalDevice.createDescriptorSetLayout(layoutInfo).value;
    XDEBUG("Created Shared Brush Texture Layout.");
}

}  // namespace MMM::Graphic
