#include "graphic/imguivk/VKSwapchain.h"
#include "log/colorful-log.h"
#include <set>

namespace MMM::Graphic
{

// 默认用fifo(必定支持-等于垂直同步)
// 下面这两个几乎必然造成撕裂
// 松弛fifo模式
// swapchainInfo.presentMode = vk::PresentModeKHR::eFifoRelaxed;
// 立即模式 -无限制 (撕裂高手l)
// swapchainInfo.presentMode = vk::PresentModeKHR::eImmediate;
/// @brief 主窗口呈现模式默认垂直同步 - 更改后需重建交换链
vk::PresentModeKHR VKSwapchain::s_globalPresentMode{
    vk::PresentModeKHR::eFifo
};

/**
 * @brief 构造函数，创建交换链及其图像资源
 *
 * @param vkPhysicalDevice 物理设备引用 (查询表面支持能力)
 * @param vkLogicalDevice 逻辑设备引用
 * @param vkSurface 窗口表面句柄
 * @param queueFamilyIndices 队列族索引信息
 * @param w 期望的宽度
 * @param h 期望的高度
 */
VKSwapchain::VKSwapchain(vk::PhysicalDevice& vkPhysicalDevice,
                         vk::Device& vkLogicalDevice, vk::SurfaceKHR& vkSurface,
                         QueueFamilyIndices& queueFamilyIndices, int w, int h)
    : m_vkLogicalDevice(vkLogicalDevice)
{
    // 初始创建，oldSwapchain 为空
    createInternal(
        vkPhysicalDevice, vkSurface, queueFamilyIndices, w, h, nullptr);
}

VKSwapchain::~VKSwapchain()
{
    destroyFramebuffers();
    cleanupImageViews();
    if ( m_swapchain ) {
        m_vkLogicalDevice.destroySwapchainKHR(m_swapchain);
    }

    XINFO("SwapChain destroyed.");
}

// 提取出的公共初始化逻辑
void VKSwapchain::createInternal(vk::PhysicalDevice& vkPhysicalDevice,
                                 vk::SurfaceKHR&     vkSurface,
                                 QueueFamilyIndices& queueFamilyIndices, int w,
                                 int h, vk::SwapchainKHR oldSwapchain)
{
    // 1. 基础配置 (不随窗口变化的)
    m_swapchainCreateInfo.setClipped(true)
        .setImageArrayLayers(1)
        .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
        .setSurface(vkSurface)
        .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
        .setOldSwapchain(oldSwapchain);  // 【关键】设置旧交换链以优化重建

    // 2. 查询物理设备支持情况
    // 查询格式
    std::vector<vk::SurfaceFormatKHR> supported_surfaceFormats =
        vkPhysicalDevice.getSurfaceFormatsKHR(vkSurface);
    vk::SurfaceFormatKHR chosenFormat = supported_surfaceFormats[0];
    for ( const auto& sf : supported_surfaceFormats ) {
        if ( sf.format == vk::Format::eR8G8B8A8Srgb &&
             sf.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear ) {
            chosenFormat = sf;
            break;
        }
    }

    // 查询能力 (Extent, Count)
    vk::SurfaceCapabilitiesKHR caps =
        vkPhysicalDevice.getSurfaceCapabilitiesKHR(vkSurface);

    // 确定图像数量
    // 推荐做法：min + 1。如果没有上限限制，就用这个值。
    // 如果有上限限制，取 (min+1) 和 max 之间的较小值。
    uint32_t imageCount = caps.minImageCount + 1;
    if ( caps.maxImageCount > 0 && imageCount > caps.maxImageCount ) {
        imageCount = caps.maxImageCount;
    }

    XINFO("Swapchain image count: requested {}, min {}, max {}",
          imageCount,
          caps.minImageCount,
          caps.maxImageCount);

    // 确定尺寸
    vk::Extent2D extent;
    extent.width = std::clamp<uint32_t>(
        w, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp<uint32_t>(
        h, caps.minImageExtent.height, caps.maxImageExtent.height);

    // 3. 队列族处理
    std::set<uint32_t> queueIndices = {
        queueFamilyIndices.graphicsQueueIndex.value(),
        queueFamilyIndices.presentQueueIndex.value()
    };
    std::vector<uint32_t> queueIndicesVec(queueIndices.begin(),
                                          queueIndices.end());

    // 4. 填充并创建交换链
    m_swapchainCreateInfo.setImageColorSpace(chosenFormat.colorSpace)
        .setImageFormat(chosenFormat.format)
        .setImageExtent(extent)
        .setMinImageCount(imageCount)
        .setPresentMode(s_globalPresentMode)
        .setPreTransform(caps.currentTransform)
        .setQueueFamilyIndices(queueIndicesVec)
        .setImageSharingMode(queueIndicesVec.size() > 1
                                 ? vk::SharingMode::eConcurrent
                                 : vk::SharingMode::eExclusive);

    m_swapchain = m_vkLogicalDevice.createSwapchainKHR(m_swapchainCreateInfo);
    XINFO("SwapChain Created (Extent: {}x{})", extent.width, extent.height);

    // 5. 获取图像并创建 ImageView
    std::vector<vk::Image> swapchain_images =
        m_vkLogicalDevice.getSwapchainImagesKHR(m_swapchain);
    m_vkImageBuffers.reserve(swapchain_images.size());

    for ( const auto& img : swapchain_images ) {
        vk::ImageViewCreateInfo viewInfo;
        viewInfo.setImage(img)
            .setViewType(vk::ImageViewType::e2D)
            .setFormat(chosenFormat.format)
            .setSubresourceRange(
                { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });

        m_vkImageBuffers.push_back(
            { .vk_image       = img,
              .vk_imageView   = m_vkLogicalDevice.createImageView(viewInfo),
              .vk_frameBuffer = nullptr });
    }
    XINFO("Successfully Created [{}] ImageBuffers", m_vkImageBuffers.size());
}

// 清理 ImageView 的逻辑（不销毁 Swapchain 句柄，用于 recreate 过程中间）
void VKSwapchain::cleanupImageViews()
{
    for ( const auto& imageBuffer : m_vkImageBuffers ) {
        if ( imageBuffer.vk_imageView ) {
            m_vkLogicalDevice.destroyImageView(imageBuffer.vk_imageView);
        }
    }
    m_vkImageBuffers.clear();
    XINFO("ImageView all destroyed.");
}


/**
 * @brief 高效重建交换链
 */
void VKSwapchain::recreate(vk::PhysicalDevice& vkPhysicalDevice,
                           vk::SurfaceKHR&     vkSurface,
                           QueueFamilyIndices& queueFamilyIndices, int w, int h)
{
    // 1. 备份旧句柄
    vk::SwapchainKHR oldSwapchain = m_swapchain;

    // 2. 销毁依赖旧交换链的资源（注意：不能先销毁 oldSwapchain 句柄）
    destroyFramebuffers();
    cleanupImageViews();

    // 3. 重新创建内部资源
    createInternal(
        vkPhysicalDevice, vkSurface, queueFamilyIndices, w, h, oldSwapchain);

    // 4. 此时可以安全销毁旧句柄了
    if ( oldSwapchain ) {
        m_vkLogicalDevice.destroySwapchainKHR(oldSwapchain);
    }

    m_needsRecreate = false;
    XINFO("Swapchain creation completed.");
}

/**
 * @brief 获取交换链创建信息
 * @return const vk::SwapchainCreateInfoKHR&
 * 包含图像格式、尺寸、呈现模式等信息
 */
const vk::SwapchainCreateInfoKHR& VKSwapchain::info() const
{
    return m_swapchainCreateInfo;
}

/**
 * @brief 创建帧缓冲区 (Framebuffer)
 *
 * @note 时序要求：必须在 RenderPass 创建之后手动调用此函数，
 * 因为 Framebuffer 依赖于 RenderPass 的结构。
 *
 * @param renderPass 渲染流程引用
 */
void VKSwapchain::createFramebuffers(const VKRenderPass& renderPass)
{
    for ( auto& imageBuffer : m_vkImageBuffers ) {
        // 帧缓冲创建信息
        vk::FramebufferCreateInfo framebufferCreateInfo;
        framebufferCreateInfo
            // 附件设置为上面创建的imageview
            .setAttachments(imageBuffer.vk_imageView)
            // 尺寸
            .setWidth(m_swapchainCreateInfo.imageExtent.width)
            .setHeight(m_swapchainCreateInfo.imageExtent.height)
            // 这里需要知道renderpass
            .setRenderPass(renderPass.getRenderPass())
            // 设置layers - 非3d图像绘制只能拿一个
            .setLayers(1);
        imageBuffer.vk_frameBuffer =
            m_vkLogicalDevice.createFramebuffer(framebufferCreateInfo);
    }
    XINFO("Successfully Created [{}] FrameBuffers", m_vkImageBuffers.size());
}

/**
 * @brief 销毁帧缓冲区
 *
 * @note 时序要求：必须在 Swapchain 析构之前手动调用（通常在 Context
 * 析构中）， 或者在重建 Swapchain 时调用。
 */
void VKSwapchain::destroyFramebuffers()
{
    for ( auto& imageBuffer : m_vkImageBuffers ) {
        m_vkLogicalDevice.destroyFramebuffer(imageBuffer.vk_frameBuffer);
        imageBuffer.vk_frameBuffer = nullptr;
    }
    XINFO("FrameBuffers all destroyed.");
}

}  // namespace MMM::Graphic
