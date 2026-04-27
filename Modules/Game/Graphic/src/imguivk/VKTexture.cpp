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
    XDEBUG("Texture loaded from file: {}", filePath);
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
    XDEBUG("Texture created from memory buffer [{}x{}]", width, height);
}

VKTexture::VKTexture(VKTexture&& other) noexcept
    : m_device(other.m_device)
    , m_image(other.m_image)
    , m_memory(other.m_memory)
    , m_imageView(other.m_imageView)
    , m_sampler(other.m_sampler)
    , m_descriptorSet(other.m_descriptorSet)
    , m_nativeSets(std::move(other.m_nativeSets))
    , m_nativePool(other.m_nativePool)
    , m_width(other.m_width)
    , m_height(other.m_height)
{
    other.m_device        = nullptr;
    other.m_image         = nullptr;
    other.m_memory        = nullptr;
    other.m_imageView     = nullptr;
    other.m_sampler       = nullptr;
    other.m_descriptorSet = nullptr;
    other.m_nativePool    = nullptr;
}

VKTexture& VKTexture::operator=(VKTexture&& other) noexcept
{
    if ( this != &other ) {
        this->~VKTexture();
        m_device        = other.m_device;
        m_image         = other.m_image;
        m_memory        = other.m_memory;
        m_imageView     = other.m_imageView;
        m_sampler       = other.m_sampler;
        m_descriptorSet = other.m_descriptorSet;
        m_nativeSets    = std::move(other.m_nativeSets);
        m_nativePool    = other.m_nativePool;
        m_width         = other.m_width;
        m_height        = other.m_height;

        other.m_device        = nullptr;
        other.m_image         = nullptr;
        other.m_memory        = nullptr;
        other.m_imageView     = nullptr;
        other.m_sampler       = nullptr;
        other.m_descriptorSet = nullptr;
        other.m_nativePool    = nullptr;
    }
    return *this;
}

VKTexture::~VKTexture()
{
    if ( !m_device ) return;

    // 顺序非常重要：先从 ImGui 注销，再销毁资源
    if ( m_descriptorSet ) {
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)m_descriptorSet);
    }

    if ( m_nativePool ) {
        for ( auto& [layout, set] : m_nativeSets ) {
            (void)m_device.freeDescriptorSets(m_nativePool, set);
        }
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
    vk::Buffer stagingBuffer = m_device.createBuffer(stagingBufferInfo).value;

    vk::MemoryRequirements memReqs =
        m_device.getBufferMemoryRequirements(stagingBuffer);
    vk::MemoryAllocateInfo allocInfo(
        memReqs.size,
        findMemoryType(physDevice,
                       memReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eHostVisible |
                           vk::MemoryPropertyFlagBits::eHostCoherent));

    vk::DeviceMemory stagingMemory = m_device.allocateMemory(allocInfo).value;
    (void)m_device.bindBufferMemory(stagingBuffer, stagingMemory, 0);

    void* data = m_device.mapMemory(stagingMemory, 0, imageSize).value;
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

    m_image = m_device.createImage(imageInfo).value;
    memReqs = m_device.getImageMemoryRequirements(m_image);

    vk::MemoryAllocateInfo imgAllocInfo(
        memReqs.size,
        findMemoryType(physDevice,
                       memReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eDeviceLocal));
    m_memory = m_device.allocateMemory(imgAllocInfo).value;
    (void)m_device.bindImageMemory(m_image, m_memory, 0);

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
    m_imageView = m_device.createImageView(viewInfo).value;

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
    m_sampler = m_device.createSampler(samplerInfo).value;
}

vk::DescriptorSet VKTexture::getNativeDescriptorSet(
    vk::DescriptorPool pool, vk::DescriptorSetLayout layout)
{
    VkDescriptorSetLayout lHandle = (VkDescriptorSetLayout)layout;

    if ( m_nativeSets.count(lHandle) ) {
        return m_nativeSets[lHandle];
    }

    // 如果 pool 变更了，逻辑上应该清空所有旧 pool 的 set
    if ( m_nativePool && m_nativePool != pool ) {
        for ( auto& [oldLayout, set] : m_nativeSets ) {
            (void)m_device.freeDescriptorSets(m_nativePool, set);
        }
        m_nativeSets.clear();
    }
    m_nativePool = pool;

    vk::DescriptorSetAllocateInfo allocInfo(pool, 1, &layout);
    vk::DescriptorSet             newSet =
        m_device.allocateDescriptorSets(allocInfo).value[0];

    vk::DescriptorImageInfo imageInfo(
        m_sampler, m_imageView, vk::ImageLayout::eShaderReadOnlyOptimal);

    vk::WriteDescriptorSet descriptorWrite(
        newSet, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo);

    m_device.updateDescriptorSets(descriptorWrite, nullptr);

    m_nativeSets[lHandle] = newSet;
    return newSet;
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
    vk::CommandBuffer cmd = m_device.allocateCommandBuffers(allocInfo).value[0];

    (void)cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });

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
    (void)cmd.end();

    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &cmd);
    (void)queue.submit(submitInfo, nullptr);
    (void)queue.waitIdle();
    m_device.freeCommandBuffers(pool, cmd);
}

void VKTexture::copyBufferToImage(vk::CommandPool pool, vk::Queue queue,
                                  vk::Buffer buffer, uint32_t width,
                                  uint32_t height)
{
    vk::CommandBufferAllocateInfo allocInfo(
        pool, vk::CommandBufferLevel::ePrimary, 1);
    vk::CommandBuffer cmd = m_device.allocateCommandBuffers(allocInfo).value[0];

    (void)cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
    vk::BufferImageCopy region(0,
                               0,
                               0,
                               { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                               { 0, 0, 0 },
                               { width, height, 1 });
    cmd.copyBufferToImage(
        buffer, m_image, vk::ImageLayout::eTransferDstOptimal, region);
    (void)cmd.end();

    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &cmd);
    (void)queue.submit(submitInfo, nullptr);
    (void)queue.waitIdle();
    m_device.freeCommandBuffers(pool, cmd);
}

}  // namespace MMM::Graphic
