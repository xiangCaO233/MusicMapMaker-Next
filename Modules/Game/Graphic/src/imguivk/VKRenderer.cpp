#include "graphic/imguivk/VKRenderer.h"
#include "event/core/EventBus.h"
#include "event/ui/ClearColorUpdateEvent.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{

std::array<float, 4> VKRenderer::s_clear_color{ .23f, .23f, .23f, 1.f };

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
                       VKRenderPass& renderPass,
                       vk::Queue&    logicDeviceGraphicsQueue,
                       vk::Queue&    logicDevicePresentQueue)
    : m_vkPhysicalDevice(vkPhysicalDevice)
    , m_vkLogicalDevice(logicalDevice)
    , m_vkRenderPass(renderPass)
    , m_vkSwapChain(swapchain)
    , m_LogicDeviceGraphicsQueue(logicDeviceGraphicsQueue)
    , m_LogicDevicePresentQueue(logicDevicePresentQueue)
{
    m_avalableImageBufferCount = swapchain.m_vkImageBuffers.size();
    XDEBUG("Available Image Buffer Count:{}.", m_avalableImageBufferCount);

    XDEBUG("Max In Flight Frame Count set to:{}.", MAX_FRAMES_IN_FLIGHT);

    // 创建命令池
    createCommandPool();

    // 创建命令缓冲区
    allocateCommandBuffers();

    // 创建信号量和栅栏
    createSemsWithFences();

    // 创建描述符池
    createDescriptPool();

    // 订阅clearcolor更新事件
    Event::EventBus::instance().subscribe<Event::ClearColorUpdateEvent>(
        [&](Event::ClearColorUpdateEvent e) {
            s_clear_color = e.clear_color_value;
        });
}

VKRenderer::~VKRenderer()
{
    // 等待设备空闲，确保不再使用任何资源
    (void)m_vkLogicalDevice.waitIdle();

    // 释放描述符池
    // 描述符集会随描述符池一同销毁，不必再手动销毁
    m_vkLogicalDevice.destroyDescriptorPool(m_vkDescriptorPool);
    XDEBUG("Destroyed Descriptor Pool.");

    m_vkLogicalDevice.destroyDescriptorSetLayout(m_brushTextureLayout);
    XDEBUG("Destroyed Brush Texture Layout.");

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

    // 分配的命令缓冲区会自动随pool一起释放
    // for ( auto& vkCommandBuffer : m_vkCommandBuffers ) {
    //     m_vkLogicalDevice.freeCommandBuffers(m_vkCommandPool,
    //     vkCommandBuffer);
    // }
    // XINFO("Destroyed VK Command Buffers.");

    m_vkLogicalDevice.destroyCommandPool(m_vkCommandPool);
    XDEBUG("Destroyed VK Command Pool.");
}

void VKRenderer::triggerRecreate(NativeWindow& window)
{
    int w, h;
    window.getFramebufferSize(w, h);
    // 这里需要调用 context 的 recreateSwapchain
    // 你可以通过回调或者单例模式来调用
    MMM::Graphic::VKContext::get().value().get().recreateSwapchain(
        window.getWindowHandle(), w, h);
}

}  // namespace MMM::Graphic
