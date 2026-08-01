#include "graphic/CursorManager.h"
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

/// @brief 重新加载渲染器内部持有的皮肤纹理。
/// @warning 低频资源重载路径：皮肤热切换时调用，会等待设备空闲并替换
/// 软件光标等渲染器自持有纹理，禁止放入每帧渲染路径。
void VKRenderer::reloadSkinTextures()
{
    (void)m_vkLogicalDevice.waitIdle();
    if ( !m_cursorManager ) {
        initCursorManager(m_vkPhysicalDevice, m_vkLogicalDevice);
        return;
    }

    m_cursorManager->reloadSkinTextures(m_vkPhysicalDevice,
                                        m_vkLogicalDevice,
                                        m_vkCommandPool,
                                        m_LogicDeviceGraphicsQueue);
}

/// @brief 确保离屏录制任务槽数量足够。
/// @param taskCount 当前帧需要的任务槽数量。
/// @warning 渲染热路径低频分支：只有可渲染视图数量增加时才创建 Vulkan command
/// pool。
void VKRenderer::ensureOffscreenRecordSlots(size_t taskCount)
{
    if ( taskCount <= m_offscreenRecordSlots.size() ) {
        return;
    }

    const size_t oldSize = m_offscreenRecordSlots.size();
    m_offscreenRecordSlots.resize(taskCount);

    for ( size_t slotIndex = oldSize; slotIndex < taskCount; ++slotIndex ) {
        vk::CommandPoolCreateInfo commandPoolCreateInfo;
        commandPoolCreateInfo.setFlags(
            vk::CommandPoolCreateFlagBits::eResetCommandBuffer);
        m_offscreenRecordSlots[slotIndex].commandPool =
            m_vkLogicalDevice.createCommandPool(commandPoolCreateInfo).value;

        vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
        commandBufferAllocateInfo
            .setCommandPool(m_offscreenRecordSlots[slotIndex].commandPool)
            .setCommandBufferCount(MAX_FRAMES_IN_FLIGHT)
            .setLevel(vk::CommandBufferLevel::ePrimary);
        m_offscreenRecordSlots[slotIndex].commandBuffers =
            m_vkLogicalDevice.allocateCommandBuffers(commandBufferAllocateInfo)
                .value;
    }

    XDEBUG("Prepared {} offscreen record command slot(s).", taskCount);
}

/// @brief 释放离屏命令录制任务槽资源。
/// @warning 不可中断操作：只能在 GPU idle 后的渲染器销毁路径调用。
void VKRenderer::releaseOffscreenRecordResources()
{
    m_offscreenRecordTasks.clear();
    m_frameSubmitCommandBuffers.clear();

    for ( auto& slot : m_offscreenRecordSlots ) {
        if ( slot.commandPool ) {
            m_vkLogicalDevice.destroyCommandPool(slot.commandPool);
            slot.commandPool = VK_NULL_HANDLE;
        }
        slot.commandBuffers.clear();
    }
    m_offscreenRecordSlots.clear();
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
    // 为每一帧分配一个命令缓冲区
    vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
    commandBufferAllocateInfo
        // 要从哪个命令池分配
        .setCommandPool(m_vkCommandPool)
        // 分配并发帧数个缓冲区
        .setCommandBufferCount(MAX_FRAMES_IN_FLIGHT)
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
    m_imageAvailableSems.resize(MAX_FRAMES_IN_FLIGHT);
    m_renderFinishedSems.resize(m_avalableImageBufferCount);
    m_cmdAvailableFences.resize(MAX_FRAMES_IN_FLIGHT);

    vk::SemaphoreCreateInfo semaphoreCreateInfo;

    for ( size_t i{ 0 }; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
        // 创建图像可用信号量 (按并发帧数)
        m_imageAvailableSems[i] =
            m_vkLogicalDevice.createSemaphore(semaphoreCreateInfo).value;
        XDEBUG("Created Image Available Semaphore For FrameInFlight {}.", i);

        // 创建同步栅 (按并发帧数)
        vk::FenceCreateInfo fenceCreateInfo;
        // 初始化为 Signaled，让第一帧可以直接通过 wait
        fenceCreateInfo.setFlags(vk::FenceCreateFlagBits::eSignaled);
        m_cmdAvailableFences[i] =
            m_vkLogicalDevice.createFence(fenceCreateInfo).value;
        XDEBUG("Created cmd Sync Fence For FrameInFlight {}.", i);
    }

    for ( size_t i{ 0 }; i < m_avalableImageBufferCount; ++i ) {
        // 创建渲染完成信号量 (按交换链图像数)
        m_renderFinishedSems[i] =
            m_vkLogicalDevice.createSemaphore(semaphoreCreateInfo).value;
        XDEBUG("Created Render Finished Semaphore For Swapchain Image {}.", i);
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
