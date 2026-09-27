#include "graphic/imguivk/VKSwapchain.h"
#include "graphic/imguivk/VKQueueFamilyDef.h"
#include "graphic/imguivk/VKRenderPass.h"
#include "log/colorful-log.h"
#include <set>

namespace MMM::Graphic
{

// 默认使用 FIFO，等价于垂直同步且 Vulkan 必定支持。
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
    // Framebuffer 引用 image view，而 image view 又引用交换链图像，必须按依赖
    // 逆序销毁，最后才释放交换链句柄。
    destroyFramebuffers();
    cleanupImageViews();
    if ( m_swapchain ) {
        m_vkLogicalDevice.destroySwapchainKHR(m_swapchain);
    }

    XDEBUG("SwapChain destroyed.");
}

/// @brief 创建交换链、取得其中的图像并为每张图像建立颜色视图。
/// @param oldSwapchain 重建时仍然有效的旧交换链；首次创建传空句柄。
/// @warning 启动或交换链重建路径：调用方必须先停止使用旧 framebuffer 和 view，
/// 并保证逻辑设备在整个创建过程中保持有效。
void VKSwapchain::createInternal(vk::PhysicalDevice& vkPhysicalDevice,
                                 vk::SurfaceKHR&     vkSurface,
                                 QueueFamilyIndices& queueFamilyIndices, int w,
                                 int h, vk::SwapchainKHR oldSwapchain)
{
    // 颜色附件仅用于最终呈现，旧交换链交给驱动复用兼容资源；裁剪被遮挡区域可
    // 避免实现保留应用永远不会观察到的像素。
    m_swapchainCreateInfo.setClipped(true)
        .setImageArrayLayers(1)
        .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
        .setSurface(vkSurface)
        .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
        .setOldSwapchain(oldSwapchain);  // 【关键】设置旧交换链以优化重建

    // surface 格式属于物理设备与窗口系统的组合能力，每次重建都重新取得当前
    // 列表。优先 UNORM 以匹配现有着色器和 ImGui 输出的颜色约定。
    auto formatsResult = vkPhysicalDevice.getSurfaceFormatsKHR(vkSurface);
    std::vector<vk::SurfaceFormatKHR> supported_surfaceFormats =
        formatsResult.value;
    vk::SurfaceFormatKHR chosenFormat = supported_surfaceFormats[0];
    for ( const auto& sf : supported_surfaceFormats ) {
        // 将 Srgb 改为 Unorm
        if ( sf.format == vk::Format::eR8G8B8A8Unorm ||
             sf.format == vk::Format::eB8G8R8A8Unorm ) {
            chosenFormat = sf;
            break;
        }
    }

    // 能力同时约束图像数量和 extent，窗口请求尺寸不能直接写入创建信息。
    auto capsResult = vkPhysicalDevice.getSurfaceCapabilitiesKHR(vkSurface);
    vk::SurfaceCapabilitiesKHR caps = capsResult.value;

    // 确定图像数量
    // 推荐做法：min + 1。
    // [优化] 如果是 FIFO (垂直同步) 模式，使用 minImageCount (通常是 2)
    // 以开启双重缓冲，减少 1 帧延迟。 如果是 Mailbox 或其他模式，使用
    // minImageCount + 1 (通常是 3) 以保证平滑。
    uint32_t imageCount = (s_globalPresentMode == vk::PresentModeKHR::eFifo)
                              ? caps.minImageCount
                              : caps.minImageCount + 1;

    if ( caps.maxImageCount > 0 && imageCount > caps.maxImageCount ) {
        imageCount = caps.maxImageCount;
    }

    XDEBUG("Swapchain image count: requested {}, min {}, max {}",
           imageCount,
           caps.minImageCount,
           caps.maxImageCount);

    // 当前 GLFW 路径传入 framebuffer 像素尺寸，并将其钳制到 surface 能力范围。
    vk::Extent2D extent;
    extent.width = std::clamp<uint32_t>(
        w, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp<uint32_t>(
        h, caps.minImageExtent.height, caps.maxImageExtent.height);

    // 图形与呈现队列族不同时使用 concurrent 共享，避免每帧显式转移图像所有权；
    // 相同时保留 exclusive，以获得更低的驱动管理开销。
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

    m_swapchain =
        m_vkLogicalDevice.createSwapchainKHR(m_swapchainCreateInfo).value;
    XDEBUG("SwapChain Created (Extent: {}x{})", extent.width, extent.height);

    // 交换链拥有 Image，封装只拥有与其配套的 ImageView 和后建的 Framebuffer。
    // 新创建前 cleanupImageViews 已清空容器，因此 reserve 不会保留旧条目。
    auto imagesResult = m_vkLogicalDevice.getSwapchainImagesKHR(m_swapchain);
    std::vector<vk::Image> swapchain_images = imagesResult.value;
    m_vkImageBuffers.reserve(swapchain_images.size());

    for ( const auto& img : swapchain_images ) {
        vk::ImageViewCreateInfo viewInfo;
        viewInfo.setImage(img)
            .setViewType(vk::ImageViewType::e2D)
            .setFormat(chosenFormat.format)
            .setSubresourceRange(
                { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });

        m_vkImageBuffers.push_back(
            { .vk_image     = img,
              .vk_imageView = m_vkLogicalDevice.createImageView(viewInfo).value,
              .vk_frameBuffer = nullptr });
    }
    XDEBUG("Successfully Created [{}] ImageBuffers", m_vkImageBuffers.size());
}

/// @brief 销毁当前交换链图像对应的全部 ImageView 并清空缓存。
/// @warning Framebuffer 必须已先销毁；本函数不销毁交换链本身，以便重建时把旧
/// 句柄传入驱动。
void VKSwapchain::cleanupImageViews()
{
    for ( const auto& imageBuffer : m_vkImageBuffers ) {
        if ( imageBuffer.vk_imageView ) {
            m_vkLogicalDevice.destroyImageView(imageBuffer.vk_imageView);
        }
    }
    m_vkImageBuffers.clear();
    XDEBUG("ImageView all destroyed.");
}


/**
 * @brief 高效重建交换链
 */
void VKSwapchain::recreate(vk::PhysicalDevice& vkPhysicalDevice,
                           vk::SurfaceKHR&     vkSurface,
                           QueueFamilyIndices& queueFamilyIndices, int w, int h)
{
    // 保留旧句柄直到新交换链成功创建，使驱动能够迁移内部呈现资源。
    vk::SwapchainKHR oldSwapchain = m_swapchain;

    // framebuffer 和 view 都依赖旧图像，必须在建立新一组包装资源前释放。
    destroyFramebuffers();
    cleanupImageViews();

    // 3. 重新创建内部资源
    createInternal(
        vkPhysicalDevice, vkSurface, queueFamilyIndices, w, h, oldSwapchain);

    // createInternal 成功后成员已指向新句柄，局部变量成为唯一待销毁的旧句柄。
    if ( oldSwapchain ) {
        m_vkLogicalDevice.destroySwapchainKHR(oldSwapchain);
    }

    m_needsRecreate.store(false, std::memory_order_relaxed);
    XDEBUG("Swapchain creation completed.");
}

/**
 * @brief 获取交换链创建信息
 * @return 包含图像格式、尺寸、呈现模式等字段的交换链创建信息引用
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
    // 每个交换链 image view 对应一个 framebuffer；附件顺序必须与 render pass
    // 的单颜色附件声明一致，尺寸则沿用实际创建成功的交换链 extent。
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
            m_vkLogicalDevice.createFramebuffer(framebufferCreateInfo).value;
    }
    XDEBUG("Successfully Created [{}] FrameBuffers", m_vkImageBuffers.size());
}

/**
 * @brief 销毁帧缓冲区
 *
 * @note 时序要求：必须在 Swapchain 析构之前手动调用（通常在 Context
 * 析构中）， 或者在重建 Swapchain 时调用。
 */
void VKSwapchain::destroyFramebuffers()
{
    // Vulkan 允许销毁空句柄，但这里仍把成员复位，防止重建或析构路径重复持有
    // 已释放资源的表象。
    for ( auto& imageBuffer : m_vkImageBuffers ) {
        m_vkLogicalDevice.destroyFramebuffer(imageBuffer.vk_frameBuffer);
        imageBuffer.vk_frameBuffer = nullptr;
    }
    XDEBUG("FrameBuffers all destroyed.");
}

}  // namespace MMM::Graphic
