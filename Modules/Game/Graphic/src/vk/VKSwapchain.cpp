#include "vk/VKSwapchain.h"
#include "colorful-log.h"
#include <set>

namespace MMM
{
namespace Graphic
{

VKSwapchain::VKSwapchain(vk::PhysicalDevice& vkPhysicalDevice,
                         vk::Device& vkLogicalDevice, vk::SurfaceKHR& vkSurface,
                         QueueFamilyIndices& queueFamilyIndices, int w, int h)
    : m_vkLogicalDevice(vkLogicalDevice)
{
    // 无需查询的配置
    m_swapchainCreateInfo
        // 裁切
        .setClipped(true)
        // 图像数组层数 - 3d图像可能需要多层
        .setImageArrayLayers(1)
        // 图像使用方法: 一般颜色附件
        // 允许gpu往上绘制像素
        .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
        // 设置窗口表面
        .setSurface(vkSurface)
        // 与窗口表面融混策略
        .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque);
    ;

    // 需要查询的配置
    struct SwapChainInfo {
        // 图像尺寸
        vk::Extent2D imageExtent{};
        // 图像数量
        uint32_t imageCount{};
        // 表面格式
        vk::SurfaceFormatKHR surfaceFormat{};
        // 贴上图像前的可选变换
        vk::SurfaceTransformFlagBitsKHR surfaceTransformFlags;
        // 呈现模式
        vk::PresentModeKHR presentMode;
    };

    SwapChainInfo swapchainInfo;

    // 查询物理设备支持的表面格式
    std::vector<vk::SurfaceFormatKHR> supported_surfaceFormats =
        vkPhysicalDevice.getSurfaceFormatsKHR(vkSurface);
    // 默认选第一个
    swapchainInfo.surfaceFormat = supported_surfaceFormats[0];
    for ( const auto& surfaceFormat : supported_surfaceFormats ) {
        // 选个通用srgb色彩空间的格式
        if ( surfaceFormat.format == vk::Format::eR8G8B8A8Srgb &&
             surfaceFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear ) {
            swapchainInfo.surfaceFormat = surfaceFormat;
        }
    }
    // 查询物理设备表面支持
    vk::SurfaceCapabilitiesKHR surfaceSupportedCapabilities =
        vkPhysicalDevice.getSurfaceCapabilitiesKHR(vkSurface);

    // 图像数量期望为2个形成双缓冲绘制
    swapchainInfo.imageCount =
        // 限制在物理设置支持的数量上下限之间
        std::clamp<uint32_t>(
            2,
            surfaceSupportedCapabilities.minImageCount,
            (surfaceSupportedCapabilities.maxImageCount > 0
                 ? surfaceSupportedCapabilities.maxImageCount
                 : (2 > surfaceSupportedCapabilities.minImageCount
                        ? 2
                        : surfaceSupportedCapabilities.minImageCount)));

    // 图像尺寸期望为创建的窗口尺寸
    swapchainInfo.imageExtent.width =
        // 限制在物理设置支持的分辨率上下限之间
        std::clamp<uint32_t>(w,
                             surfaceSupportedCapabilities.minImageExtent.width,
                             surfaceSupportedCapabilities.maxImageExtent.width);
    swapchainInfo.imageExtent.height = std::clamp<uint32_t>(
        h,
        surfaceSupportedCapabilities.minImageExtent.height,
        surfaceSupportedCapabilities.maxImageExtent.height);

    // 图像变换期望为物理设备默认的
    swapchainInfo.surfaceTransformFlags =
        surfaceSupportedCapabilities.currentTransform;

    // 查询物理设备支持的呈现模式
    std::vector<vk::PresentModeKHR> supported_presentModes =
        vkPhysicalDevice.getSurfacePresentModesKHR(vkSurface);
    // 默认用fifo(必定支持-基本等于垂直同步?)
    swapchainInfo.presentMode = vk::PresentModeKHR::eFifo;
    // 下面这两个几乎必然造成撕裂
    // 松弛fifo模式
    // swapchainInfo.presentMode = vk::PresentModeKHR::eFifoRelaxed;
    // 立即模式
    // swapchainInfo.presentMode = vk::PresentModeKHR::eImmediate;
    for ( const auto& presentMode : supported_presentModes ) {
        // 最优选mailbox模式
        // 直接取当前时刻gpu产出的最新的图像用于绘制(刷新率高且不撕裂)
        if ( presentMode == vk::PresentModeKHR::eMailbox ) {
            swapchainInfo.presentMode = vk::PresentModeKHR::eMailbox;
            break;
        }
    }

    // 命令队列索引去重
    std::set<uint32_t> queueFamilyIndexSet{
        queueFamilyIndices.graphicsQueueIndex.value(),
        queueFamilyIndices.presentQueueIndex.value()
    };
    std::vector<uint32_t> resQueueFamilyIndices;
    for ( uint32_t queueFamilyIndex : queueFamilyIndexSet ) {
        resQueueFamilyIndices.push_back(queueFamilyIndex);
    }

    // 继续填充查询到的交换链创建信息
    m_swapchainCreateInfo
        // 刚查到的支持的色彩空间
        .setImageColorSpace(swapchainInfo.surfaceFormat.colorSpace)
        // 刚查到的支持的表面格式中的图像格式
        .setImageFormat(swapchainInfo.surfaceFormat.format)
        // 刚查到的图像尺寸(或者说分辨率?)
        .setImageExtent(swapchainInfo.imageExtent)
        // 刚查到的图像缓冲数量
        .setMinImageCount(swapchainInfo.imageCount)
        // 刚查到的呈现模式
        .setPresentMode(swapchainInfo.presentMode)
        // 刚查到的变换模式
        .setPreTransform(swapchainInfo.surfaceTransformFlags)
        // 命令队列索引集合
        .setQueueFamilyIndices(resQueueFamilyIndices)
        // 若队列不是同一个，则需要sharing,否则独占
        .setImageSharingMode(resQueueFamilyIndices.size() == 1
                                 ? vk::SharingMode::eExclusive
                                 : vk::SharingMode::eConcurrent);

    // 用逻辑设备创建交换链
    m_swapchain = vkLogicalDevice.createSwapchainKHR(m_swapchainCreateInfo);
    XINFO("SwapChain Created");

    // 创建vk的图像缓冲
    // 获取到交换链中的图像
    std::vector<vk::Image> swapchain_images =
        vkLogicalDevice.getSwapchainImagesKHR(m_swapchain);
    m_vkImageBuffers.reserve(swapchain_images.size());
    for ( const auto& swapchain_image : swapchain_images ) {
        // 色彩组件映射
        vk::ComponentMapping imageComponentMapping{};
        // 如交换R颜色组件和A颜色组件
        // imageComponentMapping.setR(vk::ComponentSwizzle::eA);

        // 纹理资源范围
        vk::ImageSubresourceRange imageSubresourceRange;
        imageSubresourceRange
            // 不使用mipmap
            .setBaseMipLevel(0)
            // 需要的mipmap等级数量(不使用的话就是一个)
            .setLevelCount(1)
            // 3d图像可能会使用到这个
            .setBaseArrayLayer(0)
            // 3d图像的话设置层数,2d的话就一层
            .setLayerCount(1)
            // 作为色彩资源
            .setAspectMask(vk::ImageAspectFlagBits::eColor);

        // 图像视图创建信息
        vk::ImageViewCreateInfo imageViewCreateInfo;
        imageViewCreateInfo
            // 毫无疑问设置的图像
            .setImage(swapchain_image)
            // 观察方式(二维)
            .setViewType(vk::ImageViewType::e2D)
            // 色彩组件映射
            .setComponents(imageComponentMapping)
            // 图像格式
            .setFormat(swapchainInfo.surfaceFormat.format)
            // 纹理资源范围
            .setSubresourceRange(imageSubresourceRange);


        // 创建图像缓冲
        m_vkImageBuffers.push_back(
            { .vk_image = swapchain_image,
              .vk_imageView =
                  vkLogicalDevice.createImageView(imageViewCreateInfo),
              .vk_frameBuffer = {} });
    }
    XINFO("Successfully Created [{}] ImageBuffers", m_vkImageBuffers.size());
}

VKSwapchain::~VKSwapchain()
{
    // 用逻辑设备销毁创建的图像缓冲中的图像视图
    for ( const auto& imageBuffer : m_vkImageBuffers ) {
        m_vkLogicalDevice.destroyImageView(imageBuffer.vk_imageView);
    }
    XINFO("ImageView all destroyed.");

    // 用逻辑设备销毁交换链
    m_vkLogicalDevice.destroySwapchainKHR(m_swapchain);
    XINFO("SwapChain destroyed.");
}

/*
 * 获取vk交换链创建信息
 * */
const vk::SwapchainCreateInfoKHR& VKSwapchain::info() const
{
    return m_swapchainCreateInfo;
}

/*
 * 创建vk帧缓冲 - 时序要求,必须在后面手动调用
 * */
void VKSwapchain::createFramebuffers(VKRenderPass& renderPass)
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
            .setRenderPass(renderPass.m_graphicRenderPass)
            // 设置layers - 非3d图像绘制只能拿一个
            .setLayers(1);
        imageBuffer.vk_frameBuffer =
            m_vkLogicalDevice.createFramebuffer(framebufferCreateInfo);
    }
    XINFO("Successfully Created [{}] FrameBuffers", m_vkImageBuffers.size());
}

/*
 * 销毁vk帧缓冲 - 时序要求,必须在Swapchain释放前手动调用
 * */
void VKSwapchain::destroyFramebuffers()
{
    for ( auto& imageBuffer : m_vkImageBuffers ) {
        m_vkLogicalDevice.destroyFramebuffer(imageBuffer.vk_frameBuffer);
        imageBuffer.vk_frameBuffer = nullptr;
    }
    XINFO("FrameBuffers all destroyed.");
}

}  // namespace Graphic

}  // namespace MMM
