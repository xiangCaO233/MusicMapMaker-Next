#include "graphic/imguivk/VKTexture.h"
#include "log/colorful-log.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace MMM::Graphic
{

// 构造函数 A：从文件
VKTexture::VKTexture(const std::string&  filePath,
                     vk::PhysicalDevice& physicalDevice, vk::Device& device,
                     vk::CommandPool commandPool, vk::Queue queue)
    : m_device(device)
{
    int      texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(
        filePath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

    if ( !pixels ) {
        XCRITICAL("Failed to load texture file: {}", filePath);
        throw std::runtime_error("failed to load texture image!");
    }

    // 调用共通逻辑
    initFromPixels(pixels,
                   static_cast<uint32_t>(texWidth),
                   static_cast<uint32_t>(texHeight),
                   physicalDevice,
                   commandPool,
                   queue);

    stbi_image_free(pixels);
    XINFO("Texture loaded from file: {}", filePath);
}

// 构造函数 B：从内存数据
VKTexture::VKTexture(const unsigned char* pixels, uint32_t width,
                     uint32_t height, vk::PhysicalDevice& physicalDevice,
                     vk::Device& device, vk::CommandPool commandPool,
                     vk::Queue queue)
    : m_device(device)
{
    // 直接调用共通逻辑
    initFromPixels(pixels, width, height, physicalDevice, commandPool, queue);
    XINFO("Texture created from memory buffer [{}x{}]", width, height);
}

VKTexture::~VKTexture()
{
    if ( !m_device ) return;

    // 顺序非常重要：先从 ImGui 注销，再销毁资源
    if ( m_descriptorSet ) {
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)m_descriptorSet);
    }

    m_device.destroySampler(m_sampler);
    m_device.destroyImageView(m_imageView);
    m_device.destroyImage(m_image);
    m_device.freeMemory(m_memory);
}

// 【共通核心逻辑实现】
void VKTexture::initFromPixels(const unsigned char* pixels, uint32_t width,
                               uint32_t height, vk::PhysicalDevice& physDevice,
                               vk::CommandPool pool, vk::Queue queue)
{
    m_width                  = width;
    m_height                 = height;
    vk::DeviceSize imageSize = width * height * 4;

    // 1. 创建 Staging Buffer 并上传
    vk::BufferCreateInfo stagingBufferInfo(
        {}, imageSize, vk::BufferUsageFlagBits::eTransferSrc);
    vk::Buffer stagingBuffer = m_device.createBuffer(stagingBufferInfo);

    vk::MemoryRequirements memReqs =
        m_device.getBufferMemoryRequirements(stagingBuffer);
    vk::MemoryAllocateInfo allocInfo(
        memReqs.size,
        findMemoryType(physDevice,
                       memReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eHostVisible |
                           vk::MemoryPropertyFlagBits::eHostCoherent));

    vk::DeviceMemory stagingMemory = m_device.allocateMemory(allocInfo);
    m_device.bindBufferMemory(stagingBuffer, stagingMemory, 0);

    void* data = m_device.mapMemory(stagingMemory, 0, imageSize);
    memcpy(data, pixels, static_cast<size_t>(imageSize));
    m_device.unmapMemory(stagingMemory);

    // 2. 创建真正的 Image (Device Local)
    vk::ImageCreateInfo imageInfo(
        {},
        vk::ImageType::e2D,
        vk::Format::eR8G8B8A8Unorm,
        { m_width, m_height, 1 },
        1,
        1,
        vk::SampleCountFlagBits::e1,
        vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        vk::SharingMode::eExclusive);

    m_image = m_device.createImage(imageInfo);
    memReqs = m_device.getImageMemoryRequirements(m_image);

    vk::MemoryAllocateInfo imgAllocInfo(
        memReqs.size,
        findMemoryType(physDevice,
                       memReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eDeviceLocal));
    m_memory = m_device.allocateMemory(imgAllocInfo);
    m_device.bindImageMemory(m_image, m_memory, 0);

    // 3. 数据拷贝 (Undefined -> Dst -> ShaderRead)
    transitionImageLayout(pool,
                          queue,
                          vk::ImageLayout::eUndefined,
                          vk::ImageLayout::eTransferDstOptimal);
    copyBufferToImage(pool, queue, stagingBuffer, m_width, m_height);
    transitionImageLayout(pool,
                          queue,
                          vk::ImageLayout::eTransferDstOptimal,
                          vk::ImageLayout::eShaderReadOnlyOptimal);

    // 清理临时资源
    m_device.destroyBuffer(stagingBuffer);
    m_device.freeMemory(stagingMemory);

    // 4. 创建 ImageView
    vk::ImageViewCreateInfo viewInfo(
        {},
        m_image,
        vk::ImageViewType::e2D,
        vk::Format::eR8G8B8A8Unorm,
        {},
        { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });
    m_imageView = m_device.createImageView(viewInfo);

    // 5. 创建 Sampler
    vk::SamplerCreateInfo samplerInfo({},
                                      vk::Filter::eLinear,
                                      vk::Filter::eLinear,
                                      vk::SamplerMipmapMode::eLinear,
                                      vk::SamplerAddressMode::eClampToEdge,
                                      vk::SamplerAddressMode::eClampToEdge,
                                      vk::SamplerAddressMode::eClampToEdge,
                                      0.0f,
                                      VK_FALSE,
                                      1.0f,
                                      VK_FALSE,
                                      vk::CompareOp::eAlways,
                                      0.0f,
                                      0.0f,
                                      vk::BorderColor::eIntOpaqueBlack,
                                      VK_FALSE);
    m_sampler = m_device.createSampler(samplerInfo);
}

uint32_t VKTexture::findMemoryType(vk::PhysicalDevice&     physDevice,
                                   uint32_t                typeFilter,
                                   vk::MemoryPropertyFlags properties)
{
    vk::PhysicalDeviceMemoryProperties memProperties =
        physDevice.getMemoryProperties();
    for ( uint32_t i = 0; i < memProperties.memoryTypeCount; i++ ) {
        if ( (typeFilter & (1 << i)) &&
             (memProperties.memoryTypes[i].propertyFlags & properties) ==
                 properties ) {
            return i;
        }
    }
    throw std::runtime_error("failed to find suitable memory type!");
}

void VKTexture::transitionImageLayout(vk::CommandPool pool, vk::Queue queue,
                                      vk::ImageLayout oldLayout,
                                      vk::ImageLayout newLayout)
{
    vk::CommandBufferAllocateInfo allocInfo(
        pool, vk::CommandBufferLevel::ePrimary, 1);
    vk::CommandBuffer cmd = m_device.allocateCommandBuffers(allocInfo)[0];

    cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });

    vk::ImageMemoryBarrier barrier(
        {},
        {},
        oldLayout,
        newLayout,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        m_image,
        { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });

    vk::PipelineStageFlags sourceStage;
    vk::PipelineStageFlags destinationStage;

    if ( oldLayout == vk::ImageLayout::eUndefined &&
         newLayout == vk::ImageLayout::eTransferDstOptimal ) {
        barrier.setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
        sourceStage      = vk::PipelineStageFlagBits::eTopOfPipe;
        destinationStage = vk::PipelineStageFlagBits::eTransfer;
    } else if ( oldLayout == vk::ImageLayout::eTransferDstOptimal &&
                newLayout == vk::ImageLayout::eShaderReadOnlyOptimal ) {
        barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite);
        barrier.setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        sourceStage      = vk::PipelineStageFlagBits::eTransfer;
        destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
    } else {
        throw std::invalid_argument("unsupported layout transition!");
    }

    cmd.pipelineBarrier(
        sourceStage, destinationStage, {}, nullptr, nullptr, barrier);
    cmd.end();

    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &cmd);
    queue.submit(submitInfo, nullptr);
    queue.waitIdle();
    m_device.freeCommandBuffers(pool, cmd);
}

void VKTexture::copyBufferToImage(vk::CommandPool pool, vk::Queue queue,
                                  vk::Buffer buffer, uint32_t width,
                                  uint32_t height)
{
    vk::CommandBufferAllocateInfo allocInfo(
        pool, vk::CommandBufferLevel::ePrimary, 1);
    vk::CommandBuffer cmd = m_device.allocateCommandBuffers(allocInfo)[0];

    cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
    vk::BufferImageCopy region(0,
                               0,
                               0,
                               { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                               { 0, 0, 0 },
                               { width, height, 1 });
    cmd.copyBufferToImage(
        buffer, m_image, vk::ImageLayout::eTransferDstOptimal, region);
    cmd.end();

    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &cmd);
    queue.submit(submitInfo, nullptr);
    queue.waitIdle();
    m_device.freeCommandBuffers(pool, cmd);
}

}  // namespace MMM::Graphic
