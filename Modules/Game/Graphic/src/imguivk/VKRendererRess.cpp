#include "graphic/CursorManager.h"
#include "graphic/imguivk/VKRenderer.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{

/// @brief 使用渲染器已有上传资源创建软件光标管理器。
/// @param vkPhysicalDevice 供光标纹理选择内存类型的物理设备。
/// @param logicalDevice 创建光标图像及描述符的逻辑设备。
/// @warning 启动或皮肤资源重载路径：会加载纹理并创建 Vulkan 资源。
void VKRenderer::initCursorManager(vk::PhysicalDevice& vkPhysicalDevice,
                                   vk::Device&         logicalDevice)
{
    // 光标与主渲染共用 command pool 和图形队列，生命周期不得超过渲染器。
    m_cursorManager =
        std::make_unique<CursorManager>(vkPhysicalDevice,
                                        logicalDevice,
                                        m_vkCommandPool,
                                        m_LogicDeviceGraphicsQueue);
}

/// @brief 释放渲染器持有的软件光标纹理与状态。
/// @warning 低频清理路径：调用方必须先保证 GPU 不再引用光标纹理。
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

    // resize 只发生在槽位需求达到新高水位时；已有槽位及其命令缓冲保持不变，
    // 从而避免每帧重复创建 Vulkan 资源。
    const size_t oldSize = m_offscreenRecordSlots.size();
    m_offscreenRecordSlots.resize(taskCount);

    for ( size_t slotIndex = oldSize; slotIndex < taskCount; ++slotIndex ) {
        // 每个并行任务使用独立 command pool，满足 Vulkan
        // 对命令池外部同步的要求。
        vk::CommandPoolCreateInfo commandPoolCreateInfo;
        commandPoolCreateInfo.setFlags(
            vk::CommandPoolCreateFlagBits::eResetCommandBuffer);
        m_offscreenRecordSlots[slotIndex].commandPool =
            m_vkLogicalDevice.createCommandPool(commandPoolCreateInfo).value;

        // 同一任务槽按并发帧分配命令缓冲，只有当前帧索引对应的缓冲参与录制。
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
    // 先清除借用 hook 与 command buffer 的临时列表，避免清理后保留悬空观察值。
    m_offscreenRecordTasks.clear();
    m_frameSubmitCommandBuffers.clear();

    // command buffer 随所属 pool 隐式释放，随后清空句柄容器。
    for ( auto& slot : m_offscreenRecordSlots ) {
        if ( slot.commandPool ) {
            m_vkLogicalDevice.destroyCommandPool(slot.commandPool);
            slot.commandPool = VK_NULL_HANDLE;
        }
        slot.commandBuffers.clear();
    }
    m_offscreenRecordSlots.clear();
}

/// @brief 创建主渲染命令缓冲所属的可重置命令池。
///
/// 该池只服务主线程的窗口渲染命令；并行离屏录制使用各自独立的 pool，避免多个
/// 线程同时重置或分配同一命令池中的资源。
///
/// @warning 渲染器初始化路径：逻辑设备和图形队列族必须已完成选择。
void VKRenderer::createCommandPool()
{
    // 创建信息当前沿用 Vulkan-Hpp 零初始化的队列族索引；若设备选择允许图形族
    // 非零，此处必须同步显式设置，不能仅依赖逻辑设备已创建目标队列。reset 标志
    // 允许逐帧复用各自的 primary command buffer。
    vk::CommandPoolCreateInfo commandPoolCreateInfo;
    commandPoolCreateInfo
        // 可以随时重置
        .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);
    m_vkCommandPool =
        m_vkLogicalDevice.createCommandPool(commandPoolCreateInfo).value;
    XDEBUG("Created VK Command Pool.");
}

/// @brief 按最大并发帧数从主命令池分配 primary command buffer。
/// @warning 渲染器初始化路径：只分配一次，逐帧路径负责重置而非重新申请。
void VKRenderer::allocateCommandBuffers()
{
    // 每个 in-flight frame 独占一份主命令缓冲，CPU 不会覆写仍由 GPU 执行的槽。
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

/// @brief 创建 acquire、submit 与 present 之间使用的信号量和 CPU fence。
///
/// image-available 与 fence 按并发帧索引组织，render-finished 按交换链图像索引
/// 组织；这种区分与 render() 中的 acquire 结果及帧轮转保持一致。
///
/// @warning 渲染器初始化路径：创建 Vulkan 同步对象，不得逐帧重复调用。
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
        // 初始 signaled 让第一个使用该帧槽的 CPU wait 立即通过；提交前会
        // reset。
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

/// @brief 创建 ImGui 全局描述符池与原生 Brush 共享纹理布局。
///
/// pool 覆盖 ImGui 后端可能使用的全部 descriptor
/// 类型，并允许单独释放集合；Brush 布局则固定为 set 0 / binding 0 的 combined
/// image sampler。
///
/// @warning 渲染器初始化路径：会批量预留 descriptor 容量。
void VKRenderer::createDescriptPool()
{
    // 每类保留相同的宽裕额度，避免插件和运行时纹理增加时频繁更换全局 pool。
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

    // 原生 Brush 管线和纹理描述符必须共享完全相同的 binding 契约；布局由渲染器
    // 统一拥有，使用它创建的管线不得自行销毁该句柄。
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
