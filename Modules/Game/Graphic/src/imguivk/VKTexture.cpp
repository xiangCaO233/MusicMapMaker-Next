#include "graphic/imguivk/VKTexture.h"
#include "log/colorful-log.h"

#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstring>
#include <mutex>

namespace MMM::Graphic
{
namespace
{
/// @brief 保护共享 VkDescriptorPool 的 allocate/free 调用。
/// @warning Vulkan 要求同一 descriptor pool
/// 的描述符分配和释放由调用端外部同步。
std::mutex& descriptorPoolMutationMutex()
{
    static std::mutex mutex;
    return mutex;
}
}  // namespace

// 构造函数 A：从文件
VKTexture::VKTexture(const std::filesystem::path& filePath,
                     vk::PhysicalDevice& physicalDevice, vk::Device& device,
                     vk::CommandPool commandPool, vk::Queue queue)
    : m_device(device)
{
    auto        u8Path = filePath.u8string();
    std::string utf8Path(reinterpret_cast<const char*>(u8Path.c_str()),
                         u8Path.size());

    int      texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(
        utf8Path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

    if ( !pixels ) {
        XCRITICAL("Failed to load texture file: {}", utf8Path);
        m_device = nullptr;
        return;
    }

    // 调用共通逻辑
    const bool initialized = initFromPixels(pixels,
                                            static_cast<uint32_t>(texWidth),
                                            static_cast<uint32_t>(texHeight),
                                            physicalDevice,
                                            commandPool,
                                            queue,
                                            VKTexturePixelFormat::Rgba8);

    stbi_image_free(pixels);
    if ( !initialized ) {
        releaseResources();
        XERROR("Failed to create texture resources for file: {}", utf8Path);
        return;
    }
    XDEBUG("Texture loaded from file: {}", utf8Path);
}

// 构造函数 B：从内存数据
VKTexture::VKTexture(const unsigned char* pixels, uint32_t width,
                     uint32_t height, vk::PhysicalDevice& physicalDevice,
                     vk::Device& device, vk::CommandPool commandPool,
                     vk::Queue queue, VKTexturePixelFormat pixelFormat)
    : m_device(device)
{
    // 直接调用共通逻辑
    if ( !initFromPixels(pixels,
                         width,
                         height,
                         physicalDevice,
                         commandPool,
                         queue,
                         pixelFormat) ) {
        releaseResources();
        XERROR("Failed to create texture from memory buffer [{}x{}]",
               width,
               height);
        return;
    }
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
    , m_valid(other.m_valid)
{
    other.m_device        = nullptr;
    other.m_image         = nullptr;
    other.m_memory        = nullptr;
    other.m_imageView     = nullptr;
    other.m_sampler       = nullptr;
    other.m_descriptorSet = nullptr;
    other.m_nativePool    = nullptr;
    other.m_valid         = false;
}

VKTexture& VKTexture::operator=(VKTexture&& other) noexcept
{
    if ( this != &other ) {
        releaseResources();
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
        m_valid         = other.m_valid;

        other.m_device        = nullptr;
        other.m_image         = nullptr;
        other.m_memory        = nullptr;
        other.m_imageView     = nullptr;
        other.m_sampler       = nullptr;
        other.m_descriptorSet = nullptr;
        other.m_nativePool    = nullptr;
        other.m_valid         = false;
    }
    return *this;
}

VKTexture::~VKTexture()
{
    releaseResources();
}

void VKTexture::releaseResources()
{
    if ( !m_device ) return;

    {
        std::unique_lock descriptorLock(m_descriptorMutex);
        std::lock_guard  poolLock(descriptorPoolMutationMutex());

        // 顺序非常重要：先从 ImGui 注销，再销毁资源
        if ( m_descriptorSet ) {
            ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)m_descriptorSet);
            m_descriptorSet = nullptr;
        }

        if ( m_nativePool ) {
            for ( auto& [layout, set] : m_nativeSets ) {
                (void)m_device.freeDescriptorSets(m_nativePool, set);
            }
            m_nativeSets.clear();
            m_nativePool = nullptr;
        }
    }

    if ( m_sampler ) m_device.destroySampler(m_sampler);
    if ( m_imageView ) m_device.destroyImageView(m_imageView);
    if ( m_image ) m_device.destroyImage(m_image);
    if ( m_memory ) m_device.freeMemory(m_memory);
    m_sampler   = nullptr;
    m_imageView = nullptr;
    m_image     = nullptr;
    m_memory    = nullptr;
    m_device    = nullptr;
    m_width     = 0;
    m_height    = 0;
    m_valid     = false;
}

// 【共通核心逻辑实现】
bool VKTexture::initFromPixels(const unsigned char* pixels, uint32_t width,
                               uint32_t height, vk::PhysicalDevice& physDevice,
                               vk::CommandPool pool, vk::Queue queue,
                               VKTexturePixelFormat pixelFormat)
{
    if ( !pixels || width == 0 || height == 0 ) {
        XERROR("Invalid texture pixel input [{}x{}]", width, height);
        return false;
    }

    m_width  = width;
    m_height = height;

    const bool isSingleChannel = pixelFormat == VKTexturePixelFormat::R8 ||
                                 pixelFormat == VKTexturePixelFormat::R8Red;
    const vk::Format imageFormat =
        isSingleChannel ? vk::Format::eR8Unorm : vk::Format::eR8G8B8A8Unorm;
    const uint32_t bytesPerPixel = isSingleChannel ? 1U : 4U;
    vk::DeviceSize imageSize =
        static_cast<vk::DeviceSize>(width) * height * bytesPerPixel;

    // 1. 创建 Staging Buffer 并上传
    vk::BufferCreateInfo stagingBufferInfo(
        {}, imageSize, vk::BufferUsageFlagBits::eTransferSrc);
    vk::Buffer stagingBuffer = m_device.createBuffer(stagingBufferInfo).value;

    vk::MemoryRequirements memReqs =
        m_device.getBufferMemoryRequirements(stagingBuffer);
    auto stagingMemoryType =
        findMemoryType(physDevice,
                       memReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eHostVisible |
                           vk::MemoryPropertyFlagBits::eHostCoherent);
    if ( !stagingMemoryType ) {
        m_device.destroyBuffer(stagingBuffer);
        return false;
    }
    vk::MemoryAllocateInfo allocInfo(memReqs.size, *stagingMemoryType);

    vk::DeviceMemory stagingMemory = m_device.allocateMemory(allocInfo).value;
    (void)m_device.bindBufferMemory(stagingBuffer, stagingMemory, 0);

    void* data = m_device.mapMemory(stagingMemory, 0, imageSize).value;
    memcpy(data, pixels, static_cast<size_t>(imageSize));
    m_device.unmapMemory(stagingMemory);

    // 2. 创建真正的 Image (Device Local)
    vk::ImageCreateInfo imageInfo(
        {},
        vk::ImageType::e2D,
        imageFormat,
        { m_width, m_height, 1 },
        1,
        1,
        vk::SampleCountFlagBits::e1,
        vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        vk::SharingMode::eExclusive);

    m_image = m_device.createImage(imageInfo).value;
    memReqs = m_device.getImageMemoryRequirements(m_image);

    auto imageMemoryType =
        findMemoryType(physDevice,
                       memReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eDeviceLocal);
    if ( !imageMemoryType ) {
        m_device.destroyBuffer(stagingBuffer);
        m_device.freeMemory(stagingMemory);
        m_device.destroyImage(m_image);
        m_image = nullptr;
        return false;
    }
    vk::MemoryAllocateInfo imgAllocInfo(memReqs.size, *imageMemoryType);
    m_memory = m_device.allocateMemory(imgAllocInfo).value;
    (void)m_device.bindImageMemory(m_image, m_memory, 0);

    // 3. 数据拷贝 (Undefined -> Dst -> ShaderRead)
    if ( !transitionImageLayout(pool,
                                queue,
                                vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eTransferDstOptimal) ) {
        m_device.destroyBuffer(stagingBuffer);
        m_device.freeMemory(stagingMemory);
        return false;
    }
    copyBufferToImage(pool, queue, stagingBuffer, m_width, m_height);
    if ( !transitionImageLayout(pool,
                                queue,
                                vk::ImageLayout::eTransferDstOptimal,
                                vk::ImageLayout::eShaderReadOnlyOptimal) ) {
        m_device.destroyBuffer(stagingBuffer);
        m_device.freeMemory(stagingMemory);
        return false;
    }

    // 清理临时资源
    m_device.destroyBuffer(stagingBuffer);
    m_device.freeMemory(stagingMemory);

    // 4. 创建 ImageView
    vk::ComponentMapping componentMapping{};
    if ( pixelFormat == VKTexturePixelFormat::R8 ) {
        componentMapping = vk::ComponentMapping(vk::ComponentSwizzle::eR,
                                                vk::ComponentSwizzle::eR,
                                                vk::ComponentSwizzle::eR,
                                                vk::ComponentSwizzle::eOne);
    } else if ( pixelFormat == VKTexturePixelFormat::R8Red ) {
        componentMapping = vk::ComponentMapping(vk::ComponentSwizzle::eR,
                                                vk::ComponentSwizzle::eZero,
                                                vk::ComponentSwizzle::eZero,
                                                vk::ComponentSwizzle::eOne);
    }

    vk::ImageViewCreateInfo viewInfo(
        {},
        m_image,
        vk::ImageViewType::e2D,
        imageFormat,
        componentMapping,
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
    m_valid   = static_cast<bool>(m_image) && static_cast<bool>(m_imageView) &&
              static_cast<bool>(m_sampler);
    return m_valid;
}

ImTextureID VKTexture::getImTextureID()
{
    if ( !isValid() ) return 0;

    {
        std::shared_lock descriptorLock(m_descriptorMutex);
        if ( m_descriptorSet ) {
            return reinterpret_cast<ImTextureID>(
                static_cast<VkDescriptorSet>(m_descriptorSet));
        }
    }

    std::unique_lock descriptorLock(m_descriptorMutex);
    if ( !m_descriptorSet ) {
        std::lock_guard poolLock(descriptorPoolMutationMutex());
        // ImGui_ImplVulkan_AddTexture 内部会从它持有的全局 DescriptorPool
        // 中分配一个 Set。
        m_descriptorSet = (vk::DescriptorSet)ImGui_ImplVulkan_AddTexture(
            (VkSampler)m_sampler,
            (VkImageView)m_imageView,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    return reinterpret_cast<ImTextureID>(
        static_cast<VkDescriptorSet>(m_descriptorSet));
}

vk::DescriptorSet VKTexture::getDescriptorSet() const
{
    std::shared_lock descriptorLock(m_descriptorMutex);
    return m_descriptorSet;
}

vk::DescriptorSet VKTexture::getNativeDescriptorSet(
    vk::DescriptorPool pool, vk::DescriptorSetLayout layout)
{
    if ( !isValid() ) return nullptr;

    VkDescriptorSetLayout lHandle = (VkDescriptorSetLayout)layout;

    {
        std::shared_lock descriptorLock(m_descriptorMutex);
        auto             it = m_nativeSets.find(lHandle);
        if ( it != m_nativeSets.end() && m_nativePool == pool ) {
            return it->second;
        }
    }

    std::unique_lock descriptorLock(m_descriptorMutex);
    auto             it = m_nativeSets.find(lHandle);
    if ( it != m_nativeSets.end() && m_nativePool == pool ) {
        return it->second;
    }

    // 如果 pool 变更了，逻辑上应该清空所有旧 pool 的 set
    if ( m_nativePool && m_nativePool != pool ) {
        std::lock_guard poolLock(descriptorPoolMutationMutex());
        for ( auto& [oldLayout, set] : m_nativeSets ) {
            (void)m_device.freeDescriptorSets(m_nativePool, set);
        }
        m_nativeSets.clear();
    }
    m_nativePool = pool;

    vk::DescriptorSetAllocateInfo allocInfo(pool, 1, &layout);
    vk::DescriptorSet             newSet;
    {
        std::lock_guard poolLock(descriptorPoolMutationMutex());
        newSet = m_device.allocateDescriptorSets(allocInfo).value[0];
    }

    vk::DescriptorImageInfo imageInfo(
        m_sampler, m_imageView, vk::ImageLayout::eShaderReadOnlyOptimal);

    vk::WriteDescriptorSet descriptorWrite(
        newSet, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo);

    m_device.updateDescriptorSets(descriptorWrite, nullptr);

    m_nativeSets[lHandle] = newSet;
    return newSet;
}

std::optional<uint32_t> VKTexture::findMemoryType(
    vk::PhysicalDevice& physDevice, uint32_t typeFilter,
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
    XERROR("Failed to find suitable Vulkan memory type");
    return std::nullopt;
}

bool VKTexture::transitionImageLayout(vk::CommandPool pool, vk::Queue queue,
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
        XERROR("Unsupported texture layout transition: {} -> {}",
               vk::to_string(oldLayout),
               vk::to_string(newLayout));
        m_device.freeCommandBuffers(pool, cmd);
        return false;
    }

    cmd.pipelineBarrier(
        sourceStage, destinationStage, {}, nullptr, nullptr, barrier);
    (void)cmd.end();

    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &cmd);
    (void)queue.submit(submitInfo, nullptr);
    (void)queue.waitIdle();
    m_device.freeCommandBuffers(pool, cmd);
    return true;
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
