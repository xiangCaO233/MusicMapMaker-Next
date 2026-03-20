#include "graphic/imguivk/VKTexture.h"
#include "log/colorful-log.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace MMM::Graphic
{

VKTexture::VKTexture(const std::string&  filePath,
                     vk::PhysicalDevice& physicalDevice, vk::Device& device,
                     vk::CommandPool commandPool, vk::Queue queue)
    : m_device(device)
{
    // 1. 创建 Image 并上传数据
    createTextureImage(filePath, physicalDevice, commandPool, queue);

    // 2. 创建访问 Image 的视图
    createTextureImageView();

    // 3. 创建采样器
    createTextureSampler();

    XINFO("Texture loaded and registered to ImGui: {}", filePath);
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

void VKTexture::createTextureImage(const std::string&  filePath,
                                   vk::PhysicalDevice& physDevice,
                                   vk::CommandPool pool, vk::Queue queue)
{
    int texWidth, texHeight, texChannels;
    // 强制加载为 RGBA 格式
    stbi_uc* pixels = stbi_load(
        filePath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    vk::DeviceSize imageSize = texWidth * texHeight * 4;

    if ( !pixels ) {
        XCRITICAL("Failed to load texture image: {}", filePath);
        throw std::runtime_error("failed to load texture image!");
    }

    m_width  = static_cast<uint32_t>(texWidth);
    m_height = static_cast<uint32_t>(texHeight);

    // --- 创建 Staging Buffer ---
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
    stbi_image_free(pixels);

    // --- 创建目的 Image ---
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

    memReqs   = m_device.getImageMemoryRequirements(m_image);
    allocInfo = vk::MemoryAllocateInfo(
        memReqs.size,
        findMemoryType(physDevice,
                       memReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eDeviceLocal));
    m_memory = m_device.allocateMemory(allocInfo);
    m_device.bindImageMemory(m_image, m_memory, 0);

    // --- 数据传输过程 ---
    // 1. 转为传输接收布局
    transitionImageLayout(pool,
                          queue,
                          vk::ImageLayout::eUndefined,
                          vk::ImageLayout::eTransferDstOptimal);
    // 2. 拷贝
    copyBufferToImage(pool, queue, stagingBuffer, m_width, m_height);
    // 3. 转为 Shader 只读布局 (ImGui需要)
    transitionImageLayout(pool,
                          queue,
                          vk::ImageLayout::eTransferDstOptimal,
                          vk::ImageLayout::eShaderReadOnlyOptimal);

    // 清理临时 Buffer
    m_device.destroyBuffer(stagingBuffer);
    m_device.freeMemory(stagingMemory);
}

void VKTexture::createTextureImageView()
{
    vk::ImageViewCreateInfo viewInfo(
        {},
        m_image,
        vk::ImageViewType::e2D,
        vk::Format::eR8G8B8A8Unorm,
        {},
        { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });
    m_imageView = m_device.createImageView(viewInfo);
}

void VKTexture::createTextureSampler()
{
    vk::SamplerCreateInfo samplerInfo({},
                                      vk::Filter::eLinear,
                                      vk::Filter::eLinear,
                                      vk::SamplerMipmapMode::eLinear,
                                      vk::SamplerAddressMode::eRepeat,
                                      vk::SamplerAddressMode::eRepeat,
                                      vk::SamplerAddressMode::eRepeat,
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
