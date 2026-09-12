#include "graphic/imguivk/VKRenderer.h"
#include "event/core/EventBus.h"
#include "event/ui/ClearColorUpdateEvent.h"
#include "graphic/CursorManager.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "graphic/imguivk/mem/VKMemBuffer.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{

// 在首个配置事件到达前使用中性不透明背景；事件订阅只替换四个标量值。
std::array<float, 4> VKRenderer::s_clear_color{ .23f, .23f, .23f, 1.f };

/**
 * @brief 构造函数，初始化渲染所需的同步对象和命令资源
 *
 * @param context 创建并拥有当前渲染器的 Vulkan 上下文
 * @param vkPhysicalDevice 物理设备引用 (用于创建内存缓冲区)
 * @param logicalDevice 逻辑设备引用
 * @param swapchain 交换链引用
 * @param renderPass 渲染流程引用
 * @param logicDeviceGraphicsQueue 图形队列引用
 * @param logicDevicePresentQueue 呈现队列引用
 */
VKRenderer::VKRenderer(VKContext& context, vk::PhysicalDevice& vkPhysicalDevice,
                       vk::Device& logicalDevice, VKSwapchain& swapchain,
                       VKRenderPass& renderPass,
                       vk::Queue&    logicDeviceGraphicsQueue,
                       vk::Queue&    logicDevicePresentQueue)
    : m_vkContext(context)
    , m_vkPhysicalDevice(vkPhysicalDevice)
    , m_vkLogicalDevice(logicalDevice)
    , m_vkRenderPass(renderPass)
    , m_vkSwapChain(swapchain)
    , m_LogicDeviceGraphicsQueue(logicDeviceGraphicsQueue)
    , m_LogicDevicePresentQueue(logicDevicePresentQueue)
{
    // 交换链图像数决定按图像索引使用的 render-finished semaphore 数量；并发帧
    // 资源则始终按 MAX_FRAMES_IN_FLIGHT 分配，两种索引不能混用。
    m_avalableImageBufferCount = swapchain.m_vkImageBuffers.size();
    XDEBUG("Available Image Buffer Count:{}.", m_avalableImageBufferCount);

    XDEBUG("Max In Flight Frame Count set to:{}.", MAX_FRAMES_IN_FLIGHT);

    // 初始化顺序遵循资源依赖：先建立命令池和缓冲，再创建同步对象及全局
    // descriptor pool。任一资源的引用都不会早于其所有者创建。
    createCommandPool();

    // 创建命令缓冲区
    allocateCommandBuffers();

    // 创建信号量和栅栏
    createSemsWithFences();

    // 创建描述符池
    createDescriptPool();

    // 清屏色由 UI 配置事件低频更新；回调不捕获 this，且订阅会在析构开始时取消，
    // 避免设备销毁期间仍有外部事件访问渲染状态。
    m_clearColorSubscription =
        Event::EventBus::instance().subscribe<Event::ClearColorUpdateEvent>(
            [](const Event::ClearColorUpdateEvent& e) {
                s_clear_color = e.clear_color_value;
            });
}

VKRenderer::~VKRenderer()
{
    // 先断开事件来源，之后析构过程不再接受任何渲染状态更新。
    if ( m_clearColorSubscription != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::ClearColorUpdateEvent>(
            m_clearColorSubscription);
        m_clearColorSubscription = 0;
    }

    // GPU 必须停止引用命令池、同步对象和描述符；这是析构低频同步点，不允许
    // 移入逐帧路径。
    (void)m_vkLogicalDevice.waitIdle();

    // 描述符集由 pool 隐式释放；共享 Brush layout 独立拥有，随后显式销毁。
    m_vkLogicalDevice.destroyDescriptorPool(m_vkDescriptorPool);
    XDEBUG("Destroyed Descriptor Pool.");

    m_vkLogicalDevice.destroyDescriptorSetLayout(m_brushTextureLayout);
    XDEBUG("Destroyed Brush Texture Layout.");

    // Fence 与 semaphore 均由渲染器拥有，device idle 后可按容器逐项释放。
    for ( auto& cmdAvailableFence : m_cmdAvailableFences ) {
        m_vkLogicalDevice.destroyFence(cmdAvailableFence);
    }
    XDEBUG("Destroyed cmd Sync Fences.");

    for ( auto& imageAvailableSem : m_imageAvailableSems ) {
        m_vkLogicalDevice.destroySemaphore(imageAvailableSem);
    }
    for ( auto& renderFinishedSem : m_renderFinishedSems ) {
        m_vkLogicalDevice.destroySemaphore(renderFinishedSem);
    }
    XDEBUG("Destroyed All image Semaphores.");

    // 离屏任务槽各自拥有 command pool，必须在主命令池之前释放。
    releaseOffscreenRecordResources();

    // 主命令缓冲区随 pool 自动释放，无需也不能再单独逐项销毁。

    m_vkLogicalDevice.destroyCommandPool(m_vkCommandPool);
    XDEBUG("Destroyed VK Command Pool.");
}

/// @brief 使用窗口当前 framebuffer 像素尺寸触发交换链重建。
/// @param window 提供窗口句柄与最新 framebuffer 尺寸的原生窗口。
/// @warning 低频重建路径：内部可能等待最小化窗口恢复及设备空闲，禁止在正常
/// 逐帧分支无条件调用。
void VKRenderer::triggerRecreate(NativeWindow& window)
{
    int w, h;
    window.getFramebufferSize(w, h);
    m_vkContext.recreateSwapchain(window.getWindowHandle(), w, h);
}

/// @brief 在交换链重建后同步图像数量及按图像索引使用的完成信号量。
///
/// 调用方已经等待 device idle，因此可直接销毁旧 semaphore。图像可用 semaphore
/// 和 fence 按并发帧索引分配，不受交换链图像数量变化影响。
///
/// @warning 交换链低频重建路径：包含 Vulkan 资源销毁与创建。
void VKRenderer::onSwapchainChanged()
{
    // 更新图像数量缓存
    m_avalableImageBufferCount = m_vkSwapChain.m_vkImageBuffers.size();
    XDEBUG("Swapchain changed, new image count: {}",
           m_avalableImageBufferCount);

    // present 等待的 semaphore 与 acquire 返回的图像索引绑定，必须和新交换链
    // 图像数量保持一一对应。
    for ( auto& sem : m_renderFinishedSems ) {
        m_vkLogicalDevice.destroySemaphore(sem);
    }
    m_renderFinishedSems.clear();
    m_renderFinishedSems.resize(m_avalableImageBufferCount);

    vk::SemaphoreCreateInfo semaphoreCreateInfo;
    for ( size_t i = 0; i < m_avalableImageBufferCount; ++i ) {
        m_renderFinishedSems[i] =
            m_vkLogicalDevice.createSemaphore(semaphoreCreateInfo).value;
    }
    XDEBUG("Recreated {} Render Finished Semaphores.",
           m_avalableImageBufferCount);
}

}  // namespace MMM::Graphic
