#include "graphic/imguivk/VKTexture.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"

#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstring>
#include <limits>
#include <mutex>

namespace MMM::Graphic
{
namespace
{
/// @brief 保护共享 VkDescriptorPool 的 allocate/free 调用。
///
/// ImGui backend pool 和调用方传入的原生 pool 都可能被多个纹理对象同时访问。
/// Vulkan 对同一 descriptor pool 的分配/释放要求外部同步，因此用进程内唯一互斥
/// 量串行化所有缓存 miss 与销毁路径。
///
/// @return 当前进程中所有 VKTexture 共享的 descriptor pool 变更互斥量。
/// @warning Vulkan 要求同一 descriptor pool
/// 的描述符分配和释放由调用端外部同步。
std::mutex& descriptorPoolMutationMutex()
{
    // 函数局部静态对象由 C++ 保证线程安全初始化，并贯穿进程生命周期。
    static std::mutex mutex;
    return mutex;
}
}  // namespace

/// @brief 解码文件为 RGBA8 像素并同步创建 GPU 纹理资源。
///
/// 路径转换为 UTF-8 交给 stb_image，解码结果强制为四通道。initFromPixels
/// 在一次性 command buffer 中完成 staging
/// 上传和布局转换；无论初始化成功与否，CPU 像素 都在返回前由 stb_image 释放。
///
/// @param filePath 待解码的本地图片路径。
/// @param physicalDevice 用于选择 staging 与 image memory type 的物理设备。
/// @param device 创建并拥有纹理对象的逻辑设备。
/// @param commandPool 上传一次性 command buffer 的命令池。
/// @param queue 与 commandPool 队列族兼容的传输/图形队列。
/// @warning 低频资源加载路径：包含文件 I/O、图像解码、显存分配和 queue idle
/// 等待，禁止在渲染或 UI 每帧热路径构造。
VKTexture::VKTexture(const std::filesystem::path& filePath,
                     vk::PhysicalDevice& physicalDevice, vk::Device& device,
                     vk::CommandPool commandPool, vk::Queue queue)
    : m_device(device)
{
    // std::filesystem::path::u8string 保留非 ASCII 文件名，显式复制为 stb_image
    // 接受 的 char 字节序列。
    auto        u8Path = filePath.u8string();
    std::string utf8Path(reinterpret_cast<const char*>(u8Path.c_str()),
                         u8Path.size());

    // texChannels 返回源文件通道数，但输出由 STBI_rgb_alpha 固定为 RGBA8。
    int      texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(
        utf8Path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

    if ( !pixels ) {
        // 解码失败时对象保持 invalid，并清 device 句柄让析构跳过 Vulkan 销毁。
        XCRITICAL("Failed to load texture file: {}", utf8Path);
        m_device = nullptr;
        return;
    }

    // GPU 初始化只在 CPU 解码成功后执行，尺寸转换建立内部 uint32_t 契约。
    const bool initialized = initFromPixels(pixels,
                                            static_cast<uint32_t>(texWidth),
                                            static_cast<uint32_t>(texHeight),
                                            physicalDevice,
                                            commandPool,
                                            queue,
                                            VKTexturePixelFormat::Rgba8);

    // initFromPixels 在返回前已复制到 staging memory，不再引用 stb_image 缓冲。
    stbi_image_free(pixels);
    if ( !initialized ) {
        // 统一释放初始化中途可能已建立的 image/memory/view/sampler。
        releaseResources();
        XERROR("Failed to create texture resources for file: {}", utf8Path);
        return;
    }
    XDEBUG("Texture loaded from file: {}", utf8Path);
}

/// @brief 从调用方提供的连续像素缓冲同步创建 GPU 纹理。
///
/// 输入只在构造调用期间借用，返回前已复制到 staging buffer。pixelFormat 决定每
/// 像素字节数、Vulkan image format 和 R8 采样 swizzle。
///
/// @param pixels 连续像素起始地址。
/// @param width 纹理宽度，必须大于零。
/// @param height 纹理高度，必须大于零。
/// @param physicalDevice 用于选择 memory type 的物理设备。
/// @param device 创建资源的逻辑设备。
/// @param commandPool 一次性上传命令池。
/// @param queue 执行上传与布局转换的队列。
/// @param pixelFormat 输入数据及目标图像格式。
/// @warning 低频资源创建路径：分配 Vulkan 资源并多次等待 queue idle，不得每帧
/// 无条件调用。
VKTexture::VKTexture(const unsigned char* pixels, uint32_t width,
                     uint32_t height, vk::PhysicalDevice& physicalDevice,
                     vk::Device& device, vk::CommandPool commandPool,
                     vk::Queue queue, VKTexturePixelFormat pixelFormat)
    : m_device(device)
{
    // 两个构造入口共享完全相同的 Vulkan 创建/失败清理协议。
    if ( !initFromPixels(pixels,
                         width,
                         height,
                         physicalDevice,
                         commandPool,
                         queue,
                         pixelFormat) ) {
        // 初始化失败可能发生在 staging、image 或 layout
        // 阶段，统一回滚成员资源。
        releaseResources();
        XERROR("Failed to create texture from memory buffer [{}x{}]",
               width,
               height);
        return;
    }
    XDEBUG("Texture created from memory buffer [{}x{}]", width, height);
}

/// @brief 转移纹理、descriptor 缓存和 streaming staging 槽位所有权。
///
/// 移动后目标对象接管同一 VkDevice 下的全部资源，源对象被重置为可安全析构的
/// invalid 状态。外部必须保证没有线程同时访问 other 的 descriptor 缓存。
///
/// @param other 被接管的纹理对象。
/// @warning 低频资源生命周期路径：不会等待 GPU；调用方需保证资源所有权转移时
/// 没有并发录制或缓存查询。
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
    , m_pixelFormat(other.m_pixelFormat)
    , m_streamingUploadSlots(std::move(other.m_streamingUploadSlots))
    , m_streamingUploadByteCount(other.m_streamingUploadByteCount)
    , m_valid(other.m_valid)
{
    // 所有 owning Vulkan 句柄、descriptor
    // 映射和持久映射槽位必须作为一个整体转移。
    other.m_device        = nullptr;
    other.m_image         = nullptr;
    other.m_memory        = nullptr;
    other.m_imageView     = nullptr;
    other.m_sampler       = nullptr;
    other.m_descriptorSet = nullptr;
    // 清空源句柄防止其析构释放已经转移到目标对象的资源。
    other.m_nativePool  = nullptr;
    other.m_width       = 0;
    other.m_height      = 0;
    other.m_pixelFormat = VKTexturePixelFormat::Rgba8;
    // vector 已移动，显式清理源侧容量语义并恢复格式/有效性初始值。
    other.m_streamingUploadSlots.clear();
    other.m_streamingUploadByteCount = 0;
    other.m_valid                    = false;
}

/// @brief 释放当前资源后接管另一个纹理的完整 Vulkan 所有权。
///
/// 自移动保持无操作；正常移动先释放目标现有资源，再复制句柄/标量并移动容器，
/// 最后把源对象复位为 invalid。descriptor mutex
/// 本身不移动，各对象保留自己的锁。
///
/// @param other 被接管的纹理对象。
/// @return 当前对象引用。
/// @warning 低频资源生命周期路径：releaseResources 不等待 GPU，调用方必须保证
/// 当前纹理和 other 均没有在途使用或并发 descriptor 访问。
VKTexture& VKTexture::operator=(VKTexture&& other) noexcept
{
    // 自移动时保留现有资源，避免先释放后再读取同一对象。
    if ( this != &other ) {
        // 目标旧资源先按 descriptor/image 依赖顺序释放，再覆盖成员句柄。
        releaseResources();
        m_device                   = other.m_device;
        m_image                    = other.m_image;
        m_memory                   = other.m_memory;
        m_imageView                = other.m_imageView;
        m_sampler                  = other.m_sampler;
        m_descriptorSet            = other.m_descriptorSet;
        m_nativeSets               = std::move(other.m_nativeSets);
        m_nativePool               = other.m_nativePool;
        m_width                    = other.m_width;
        m_height                   = other.m_height;
        m_pixelFormat              = other.m_pixelFormat;
        m_streamingUploadSlots     = std::move(other.m_streamingUploadSlots);
        m_streamingUploadByteCount = other.m_streamingUploadByteCount;
        m_valid                    = other.m_valid;

        // 与移动构造保持相同源对象后置条件，确保其析构幂等。
        other.m_device        = nullptr;
        other.m_image         = nullptr;
        other.m_memory        = nullptr;
        other.m_imageView     = nullptr;
        other.m_sampler       = nullptr;
        other.m_descriptorSet = nullptr;
        other.m_nativePool    = nullptr;
        other.m_width         = 0;
        other.m_height        = 0;
        other.m_pixelFormat   = VKTexturePixelFormat::Rgba8;
        other.m_streamingUploadSlots.clear();
        other.m_streamingUploadByteCount = 0;
        other.m_valid                    = false;
    }
    return *this;
}

/// @brief 释放当前对象仍拥有的 descriptor、staging 和图像资源。
/// @warning 低频资源销毁路径：不等待 GPU，析构前必须保证没有在途命令引用纹理。
VKTexture::~VKTexture()
{
    releaseResources();
}

/// @brief 释放 ImGui/原生 descriptor、streaming staging 与纹理图像资源。
///
/// descriptor mutex 先串行化当前纹理缓存，进程级 pool mutex 再保护 Vulkan pool
/// allocate/free 外部同步。之后按 sampler/view/image/memory
/// 的引用逆序销毁，并把 对象恢复为 invalid 空状态。m_device 为空的 moved-from
/// 对象仅清理 CPU 容器。
///
/// @warning 低频资源销毁路径：不会等待 GPU；调用方必须保证 descriptor、image 和
/// staging buffer 已不再被任何在途 command buffer 使用。
void VKTexture::releaseResources()
{
    // moved-from 或文件解码失败对象没有可调用 destroy 的 device，但仍统一复位
    // streaming 容器统计。
    if ( !m_device ) {
        m_streamingUploadSlots.clear();
        m_streamingUploadByteCount = 0;
        return;
    }

    // 持久映射 staging 与 image 相互独立，先解除映射并释放所有帧槽。
    releaseStreamingUploadResources();

    {
        // 固定加锁顺序为对象 descriptor mutex -> 全局 pool mutex，所有分配/释放
        // 路径必须保持一致，避免锁顺序反转。
        std::unique_lock descriptorLock(m_descriptorMutex);
        std::lock_guard  poolLock(descriptorPoolMutationMutex());

        // ImGui backend descriptor 引用 sampler/image
        // view，必须先注销再销毁图像资源。
        if ( m_descriptorSet ) {
            ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)m_descriptorSet);
            m_descriptorSet = nullptr;
        }

        if ( m_nativePool ) {
            // nativeSets 全部由同一个 m_nativePool 分配；释放依赖调用方 pool
            // 支持 单独 free descriptor sets。
            for ( auto& [layout, set] : m_nativeSets ) {
                // layout 只作为缓存 key，不拥有 Vulkan layout 句柄。
                (void)m_device.freeDescriptorSets(m_nativePool, set);
            }
            m_nativeSets.clear();
            // vector/map 清空后 pool 仍由外部拥有，本类只丢弃非 owning 句柄。
            m_nativePool = nullptr;
        }
    }

    // image view 依赖 image，image 又依赖绑定 memory，按逆序逐项销毁有效句柄。
    if ( m_sampler ) m_device.destroySampler(m_sampler);
    if ( m_imageView ) m_device.destroyImageView(m_imageView);
    if ( m_image ) m_device.destroyImage(m_image);
    if ( m_memory ) m_device.freeMemory(m_memory);
    // 所有 Vulkan 句柄和元数据恢复构造前状态，使重复 release 与 moved-from
    // 析构安全。
    m_sampler     = nullptr;
    m_imageView   = nullptr;
    m_image       = nullptr;
    m_memory      = nullptr;
    m_device      = nullptr;
    m_width       = 0;
    m_height      = 0;
    m_pixelFormat = VKTexturePixelFormat::Rgba8;
    m_valid       = false;
}

/// @brief 解除映射并释放所有 streaming staging 槽位。
///
/// 每个槽位拥有一对 buffer/memory 和生命周期内持续有效的 mapped pointer。释放时
/// 先 unmap，再销毁 buffer，最后归还 memory；部分创建失败的槽位也允许逐项为空。
///
/// @warning 低频资源销毁路径：不等待 GPU，调用方必须保证对应 frame fence
/// 已完成。
void VKTexture::releaseStreamingUploadResources()
{
    // moved-from 对象没有有效 device，容器只包含已被移动走或空的槽位。
    if ( m_device ) {
        for ( auto& slot : m_streamingUploadSlots ) {
            // 只有成功 map 且 memory 仍有效时调用 unmapMemory。
            if ( slot.m_mappedPixels && slot.m_memory ) {
                m_device.unmapMemory(slot.m_memory);
            }
            // Buffer 在释放其绑定 memory 前销毁。
            if ( slot.m_buffer ) {
                m_device.destroyBuffer(slot.m_buffer);
            }
            if ( slot.m_memory ) {
                m_device.freeMemory(slot.m_memory);
            }
            // 逐槽归零便于中途失败清理保持相同后置条件。
            slot = {};
        }
    }
    // 容器和期望字节数一起复位，下一次 prepare 必须重新验证/创建。
    m_streamingUploadSlots.clear();
    m_streamingUploadByteCount = 0;
}

/// @brief 把连续 CPU 像素同步上传为单 mip、单 layer 的 sampled 2D 图像。
///
/// 创建流程为 host-visible staging buffer、device-local
/// image、两次布局转换和一次 buffer-to-image copy，最后建立带格式 swizzle 的
/// image view 与线性 clamp sampler。 部分失败由本函数清理局部
/// staging，成员资源交给调用构造函数统一 release。
///
/// @param pixels 调用期间有效的连续像素数据。
/// @param width 图像宽度，必须大于零。
/// @param height 图像高度，必须大于零。
/// @param physDevice 用于选择 host-visible 与 device-local memory type。
/// @param pool 分配一次性上传 command buffer 的命令池。
/// @param queue 与 pool 队列族兼容的传输/图形队列。
/// @param pixelFormat 输入字节布局和采样通道映射。
/// @return 完整 image/view/sampler 创建成功时返回 true。
/// @warning 低频同步上传路径：包含内存分配、map/memcpy 和多次 queue
/// idle，不得从 每帧渲染热路径调用。
bool VKTexture::initFromPixels(const unsigned char* pixels, uint32_t width,
                               uint32_t height, vk::PhysicalDevice& physDevice,
                               vk::CommandPool pool, vk::Queue queue,
                               VKTexturePixelFormat pixelFormat)
{
    // 空指针或零 extent 无法构造合法 Vulkan 图像，也不能计算有效上传字节数。
    if ( !pixels || width == 0 || height == 0 ) {
        XERROR("Invalid texture pixel input [{}x{}]", width, height);
        return false;
    }

    // 尺寸与格式先写入成员，后续 image view 和 streaming 能沿用同一元数据。
    m_width       = width;
    m_height      = height;
    m_pixelFormat = pixelFormat;

    // R8/R8Red 在内存中均为单字节，只在最终 view swizzle
    // 上区分彩色与红通道用途。
    const bool isSingleChannel = pixelFormat == VKTexturePixelFormat::R8 ||
                                 pixelFormat == VKTexturePixelFormat::R8Red;
    const vk::Format imageFormat =
        isSingleChannel ? vk::Format::eR8Unorm : vk::Format::eR8G8B8A8Unorm;
    const uint32_t bytesPerPixel = isSingleChannel ? 1U : 4U;
    // 有效尺寸来自 uint32_t，提升到 DeviceSize 后再相乘，避免 32 位中间值溢出。
    vk::DeviceSize imageSize =
        static_cast<vk::DeviceSize>(width) * height * bytesPerPixel;

    // Staging buffer 只作为 transfer source，CPU 写完后由一次性命令复制到 GPU
    // 图像。
    vk::BufferCreateInfo stagingBufferInfo(
        {}, imageSize, vk::BufferUsageFlagBits::eTransferSrc);
    vk::Buffer stagingBuffer = m_device.createBuffer(stagingBufferInfo).value;

    // Vulkan requirements 决定 allocation 大小和允许的 memory type mask。
    vk::MemoryRequirements memReqs =
        m_device.getBufferMemoryRequirements(stagingBuffer);
    auto stagingMemoryType =
        findMemoryType(physDevice,
                       memReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eHostVisible |
                           vk::MemoryPropertyFlagBits::eHostCoherent);
    if ( !stagingMemoryType ) {
        // image 尚未创建，此时只需销毁 staging buffer。
        m_device.destroyBuffer(stagingBuffer);
        return false;
    }
    vk::MemoryAllocateInfo allocInfo(memReqs.size, *stagingMemoryType);

    // HostCoherent 让 memcpy 后无需显式 flushMappedMemoryRanges。
    vk::DeviceMemory stagingMemory = m_device.allocateMemory(allocInfo).value;
    (void)m_device.bindBufferMemory(stagingBuffer, stagingMemory, 0);

    // pixels 只借用到 memcpy 完成，随后 staging memory 可立即解除映射。
    void* data = m_device.mapMemory(stagingMemory, 0, imageSize).value;
    memcpy(data, pixels, static_cast<size_t>(imageSize));
    m_device.unmapMemory(stagingMemory);

    // 目标 image 只由 transfer 写入和 fragment shader 采样，使用 optimal tiling
    // 与 device-local memory，不支持 CPU 直接映射。
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

    // 单 mip/单 layer 与后续 view 和 copy region 的子资源范围严格对应。
    m_image = m_device.createImage(imageInfo).value;
    memReqs = m_device.getImageMemoryRequirements(m_image);

    auto imageMemoryType =
        findMemoryType(physDevice,
                       memReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eDeviceLocal);
    if ( !imageMemoryType ) {
        // staging 已完整创建但尚未提交，失败时同时释放局部资源和未绑定目标
        // image。
        m_device.destroyBuffer(stagingBuffer);
        m_device.freeMemory(stagingMemory);
        m_device.destroyImage(m_image);
        m_image = nullptr;
        return false;
    }
    vk::MemoryAllocateInfo imgAllocInfo(memReqs.size, *imageMemoryType);
    // m_memory 与 m_image 一对一绑定，成员 releaseResources 按 image 后 memory
    // 销毁。
    m_memory = m_device.allocateMemory(imgAllocInfo).value;
    (void)m_device.bindImageMemory(m_image, m_memory, 0);

    // 第一次 barrier 丢弃 undefined 内容并允许 transfer write。
    if ( !transitionImageLayout(pool,
                                queue,
                                vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eTransferDstOptimal) ) {
        m_device.destroyBuffer(stagingBuffer);
        m_device.freeMemory(stagingMemory);
        return false;
    }
    // copy helper 返回时 queue 已空闲，staging 内容已经进入目标图像。
    copyBufferToImage(pool, queue, stagingBuffer, m_width, m_height);
    // 第二次 barrier 让 fragment shader 读取 transfer 产生的全部像素。
    if ( !transitionImageLayout(pool,
                                queue,
                                vk::ImageLayout::eTransferDstOptimal,
                                vk::ImageLayout::eShaderReadOnlyOptimal) ) {
        m_device.destroyBuffer(stagingBuffer);
        m_device.freeMemory(stagingMemory);
        return false;
    }

    // 两个同步 helper 均已等待 queue idle，局部 staging 可立即销毁。
    m_device.destroyBuffer(stagingBuffer);
    m_device.freeMemory(stagingMemory);

    // 默认 ComponentMapping 使用 identity；单通道格式根据业务含义覆写采样结果。
    vk::ComponentMapping componentMapping{};
    if ( pixelFormat == VKTexturePixelFormat::R8 ) {
        // 字形/遮罩纹理把 R 复制到 RGB，并固定 Alpha=1，采样结果呈灰度。
        componentMapping = vk::ComponentMapping(vk::ComponentSwizzle::eR,
                                                vk::ComponentSwizzle::eR,
                                                vk::ComponentSwizzle::eR,
                                                vk::ComponentSwizzle::eOne);
    } else if ( pixelFormat == VKTexturePixelFormat::R8Red ) {
        // 数据纹理仅保留红通道，G/B 清零且 Alpha 固定为 1。
        componentMapping = vk::ComponentMapping(vk::ComponentSwizzle::eR,
                                                vk::ComponentSwizzle::eZero,
                                                vk::ComponentSwizzle::eZero,
                                                vk::ComponentSwizzle::eOne);
    }

    // View 覆盖唯一颜色 mip/layer，并沿用 image 的实际 Vulkan format。
    vk::ImageViewCreateInfo viewInfo(
        {},
        m_image,
        vk::ImageViewType::e2D,
        imageFormat,
        componentMapping,
        { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });
    m_imageView = m_device.createImageView(viewInfo).value;

    // 线性过滤适合 UI 图片缩放，ClampToEdge 防止采样跨越纹理边缘回绕。
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
    // valid 只在三个对外使用的核心句柄全部存在后发布。
    m_sampler = m_device.createSampler(samplerInfo).value;
    m_valid   = static_cast<bool>(m_image) && static_cast<bool>(m_imageView) &&
                static_cast<bool>(m_sampler);
    return m_valid;
}

/// @brief 为 RGBA8 动态帧创建持久映射的分帧上传缓冲。
///
/// 每个 in-flight frame 独占一个 host-visible/coherent staging
/// buffer，生命周期内 保持映射，避免视频帧等连续更新时重复创建、map 和
/// unmap。槽位数量或纹理字节数 变化时整体替换旧资源，任一槽位失败则回滚本轮全部
/// staging。
///
/// @param physicalDevice 用于筛选 CPU 可写 memory type 的物理设备。
/// @param frameSlots 与外层渲染器并发帧数量一致的槽位数。
/// @return 输入与纹理格式有效且所有 staging 槽位准备成功时返回 true。
/// @warning 低频资源准备路径：会释放/分配/映射 Vulkan memory；调用前必须保证旧
/// 槽位对应 fence 已完成，禁止从每帧无条件路径调用。
bool VKTexture::prepareStreamingUpload(vk::PhysicalDevice& physicalDevice,
                                       uint32_t            frameSlots)
{
    // Streaming 协议只接受固定 RGBA8 帧；单通道纹理和未完成初始化对象不支持。
    if ( !isValid() || m_pixelFormat != VKTexturePixelFormat::Rgba8 ||
         frameSlots == 0 || m_width == 0 || m_height == 0 ) {
        return false;
    }

    // 使用 size_t 计算 CPU memcpy 字节数，并在乘法前逐级检查溢出。
    constexpr std::size_t RGBA8_BYTES_PER_PIXEL = 4;
    const std::size_t     width  = static_cast<std::size_t>(m_width);
    const std::size_t     height = static_cast<std::size_t>(m_height);
    // 先验证 height*4，再验证 width*(height*4)，避免检查表达式本身溢出。
    if ( height >
             std::numeric_limits<std::size_t>::max() / RGBA8_BYTES_PER_PIXEL ||
         width > std::numeric_limits<std::size_t>::max() /
                     (height * RGBA8_BYTES_PER_PIXEL) ) {
        XERROR("VKTexture streaming upload size overflow: {}x{}",
               m_width,
               m_height);
        return false;
    }
    const std::size_t byteCount = width * height * RGBA8_BYTES_PER_PIXEL;

    // 完全匹配时复用持久映射槽位，常态资源检查不触发任何 Vulkan 调用。
    if ( m_streamingUploadSlots.size() == frameSlots &&
         m_streamingUploadByteCount == byteCount ) {
        return true;
    }

    // 配置变化采用整体替换，避免混用不同大小或不同创建代次的帧槽。
    releaseStreamingUploadResources();
    m_streamingUploadByteCount = byteCount;
    m_streamingUploadSlots.reserve(frameSlots);

    // 所有槽位共享同一 buffer create info，仅用途为 transfer source。
    const vk::BufferCreateInfo bufferInfo(
        {},
        static_cast<vk::DeviceSize>(byteCount),
        vk::BufferUsageFlagBits::eTransferSrc);
    for ( uint32_t slotIndex = 0; slotIndex < frameSlots; ++slotIndex ) {
        // 先在 vector 中建立空槽，后续任一失败均可由统一释放函数识别部分资源。
        auto& slot = m_streamingUploadSlots.emplace_back();

        // no-exceptions Vulkan-Hpp 返回 result/value，失败时记录具体 VkResult。
        const auto bufferResult = m_device.createBuffer(bufferInfo);
        if ( bufferResult.result != vk::Result::eSuccess ) {
            XERROR("VKTexture failed to create streaming staging buffer: {}",
                   vk::to_string(bufferResult.result));
            // 回滚之前已经成功建立的槽位以及当前空槽。
            releaseStreamingUploadResources();
            return false;
        }
        slot.m_buffer = bufferResult.value;

        // 每个 buffer 的 requirements 决定 allocation 大小和允许 memory type。
        const vk::MemoryRequirements memoryRequirements =
            m_device.getBufferMemoryRequirements(slot.m_buffer);
        const auto memoryType =
            findMemoryType(physicalDevice,
                           memoryRequirements.memoryTypeBits,
                           vk::MemoryPropertyFlagBits::eHostVisible |
                               vk::MemoryPropertyFlagBits::eHostCoherent);
        if ( !memoryType ) {
            // 当前 slot 已有 buffer，统一回滚会按 buffer -> memory 顺序清理。
            releaseStreamingUploadResources();
            return false;
        }

        // HostCoherent 保证逐帧 memcpy 后无需显式 flush。
        const vk::MemoryAllocateInfo allocationInfo(memoryRequirements.size,
                                                    *memoryType);
        const auto memoryResult = m_device.allocateMemory(allocationInfo);
        if ( memoryResult.result != vk::Result::eSuccess ) {
            XERROR("VKTexture failed to allocate streaming staging memory: {}",
                   vk::to_string(memoryResult.result));
            releaseStreamingUploadResources();
            return false;
        }
        slot.m_memory = memoryResult.value;

        // buffer 与 allocation 一对一绑定且 offset 为零。
        const vk::Result bindResult =
            m_device.bindBufferMemory(slot.m_buffer, slot.m_memory, 0);
        if ( bindResult != vk::Result::eSuccess ) {
            XERROR("VKTexture failed to bind streaming staging memory: {}",
                   vk::to_string(bindResult));
            releaseStreamingUploadResources();
            return false;
        }

        // 一次 map 覆盖完整像素区，地址保存到 slot 并持续到资源释放。
        const auto mappedResult = m_device.mapMemory(
            slot.m_memory, 0, static_cast<vk::DeviceSize>(byteCount));
        if ( mappedResult.result != vk::Result::eSuccess ||
             !mappedResult.value ) {
            XERROR("VKTexture failed to map streaming staging memory: {}",
                   vk::to_string(mappedResult.result));
            releaseStreamingUploadResources();
            return false;
        }
        slot.m_mappedPixels = mappedResult.value;
    }

    // 所有槽位都具备 buffer、memory 和 mapped pointer 后才报告成功。
    return true;
}

/// @brief 将一帧 RGBA8 像素复制到持久 staging 槽位并记录图像上传命令。
///
/// 函数不 submit、不等待，只把 CPU 帧复制到当前 fence 已完成的 staging 槽，并在
/// 调用方 command buffer 中记录 ShaderRead -> TransferDst、copy、TransferDst ->
/// ShaderRead 的固定命令序列。
///
/// @param commandBuffer 当前帧已 begin 且位于 render pass 外的命令缓冲。
/// @param frameIndex 外层并发帧对应的独占 staging 槽索引。
/// @param pixels 连续 RGBA8 源数据。
/// @param byteCount 源数据大小，必须精确匹配纹理 width*height*4。
/// @return 参数/槽位有效且完整上传命令已记录时返回 true。
/// @warning 渲染录制热路径：执行整帧 memcpy 和固定 Vulkan 命令；调用方保证同一
/// 纹理不并发录制且 frameIndex 对应 fence 已完成。
bool VKTexture::recordStreamingUpload(vk::CommandBuffer&   commandBuffer,
                                      uint32_t             frameIndex,
                                      const unsigned char* pixels,
                                      std::size_t          byteCount)
{
    // 严格字节数检查防止部分帧遗留旧像素或 memcpy 越过持久映射范围。
    if ( !isValid() || m_pixelFormat != VKTexturePixelFormat::Rgba8 ||
         !pixels || byteCount == 0 || byteCount != m_streamingUploadByteCount ||
         frameIndex >= m_streamingUploadSlots.size() ) {
        return false;
    }

    // frameIndex 已验证，引用当前帧独占槽位，不复制任何 owning 资源。
    auto& slot = m_streamingUploadSlots[frameIndex];
    if ( !slot.m_buffer || !slot.m_memory || !slot.m_mappedPixels ) {
        return false;
    }

    // HostCoherent memory 让 CPU 写入对后续 transfer 可见，无需 flush mapped
    // range。
    std::memcpy(slot.m_mappedPixels, pixels, byteCount);

    // Streaming 图像固定只有一个颜色 mip/layer，两次 barrier 覆盖同一完整范围。
    const vk::ImageSubresourceRange imageRange(
        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
    // 前一帧 fragment shader 读取必须完成后，本帧 transfer 才能覆盖 image。
    const vk::ImageMemoryBarrier toTransfer(
        vk::AccessFlagBits::eShaderRead,
        vk::AccessFlagBits::eTransferWrite,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageLayout::eTransferDstOptimal,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        m_image,
        imageRange);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                  vk::PipelineStageFlagBits::eTransfer,
                                  {},
                                  nullptr,
                                  nullptr,
                                  toTransfer);

    // 紧密排列输入使用 rowLength/imageHeight=0，copy extent 覆盖整张纹理。
    const vk::BufferImageCopy copyRegion(
        0,
        0,
        0,
        { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        { 0, 0, 0 },
        { m_width, m_height, 1 });
    commandBuffer.copyBufferToImage(slot.m_buffer,
                                    m_image,
                                    vk::ImageLayout::eTransferDstOptimal,
                                    copyRegion);

    // copy 完成后发布 transfer write，恢复 descriptor 声明的
    // ShaderReadOnlyOptimal。
    const vk::ImageMemoryBarrier toShaderRead(
        vk::AccessFlagBits::eTransferWrite,
        vk::AccessFlagBits::eShaderRead,
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        m_image,
        imageRange);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                  vk::PipelineStageFlagBits::eFragmentShader,
                                  {},
                                  nullptr,
                                  nullptr,
                                  toShaderRead);
    // 命令只完成录制；实际执行和 frame fence 生命周期仍由调用方提交逻辑负责。
    return true;
}

/// @brief 获取或惰性创建 ImGui backend 可采样的纹理 descriptor。
///
/// 先用 shared lock 检查缓存命中；miss 后升级为 unique lock
/// 并再次检查，防止两个 线程同时分配。实际 AddTexture 还受全局 pool mutex
/// 保护，以满足 ImGui Vulkan descriptor pool 的外部同步要求。
///
/// @return 有效纹理的稳定 ImTextureID；纹理未初始化时返回零。
/// @warning 资源访问路径：缓存命中只读锁和句柄；首次调用会同步 descriptor pool
/// 并分配 set，应在绘制循环开始前预热。
ImTextureID VKTexture::getImTextureID()
{
    // invalid 对象没有 sampler/image view，不能向 backend 注册。
    if ( !isValid() ) return 0;

    {
        // 常态缓存命中允许多个读取者并行，不触碰 descriptor pool。
        std::shared_lock descriptorLock(m_descriptorMutex);
        if ( m_descriptorSet ) {
            return reinterpret_cast<ImTextureID>(
                static_cast<VkDescriptorSet>(m_descriptorSet));
        }
    }

    // miss 后独占当前纹理缓存；双重检查处理等待期间其他线程完成注册的情况。
    std::unique_lock descriptorLock(m_descriptorMutex);
    if ( !m_descriptorSet ) {
        std::lock_guard poolLock(descriptorPoolMutationMutex());
        // ImGui_ImplVulkan_AddTexture 从 backend 共享 pool 分配
        // set，全局互斥量覆盖 不同 VKTexture 实例之间的并发 allocate/free。
        m_descriptorSet = (vk::DescriptorSet)ImGui_ImplVulkan_AddTexture(
            (VkSampler)m_sampler,
            (VkImageView)m_imageView,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // ImGui Vulkan backend 约定 ImTextureID 直接编码 VkDescriptorSet 句柄。
    return reinterpret_cast<ImTextureID>(
        static_cast<VkDescriptorSet>(m_descriptorSet));
}

/// @brief 线程安全读取已经缓存的 ImGui descriptor set。
/// @return 尚未调用 getImTextureID 时为空，否则返回 backend 分配的集合。
/// @warning 高频读取路径：只获取 shared lock，不触发 descriptor 分配。
vk::DescriptorSet VKTexture::getDescriptorSet() const
{
    std::shared_lock descriptorLock(m_descriptorMutex);
    return m_descriptorSet;
}

/// @brief 获取或按 layout 惰性创建项目原生管线使用的纹理 descriptor set。
///
/// 缓存以 VkDescriptorSetLayout 句柄为 key，并限制所有集合来自同一个 pool。
/// 调用方切换 pool 时，先释放旧 pool 中所有缓存集合，再从新 pool 分配并写入
/// binding 0 的 combined image sampler。
///
/// @param pool 分配集合的外部 descriptor pool，必须支持单独释放 descriptor
/// set。
/// @param layout 声明 binding 0 为 combined image sampler 的兼容布局。
/// @return 缓存或新建的 descriptor set；纹理无效时返回空句柄。
/// @warning 渲染路径缓存命中只读锁；首次 layout/pool 会加独占锁并分配/释放
/// Vulkan descriptor，应在批次录制前稳定 layout 与 pool。
vk::DescriptorSet VKTexture::getNativeDescriptorSet(
    vk::DescriptorPool pool, vk::DescriptorSetLayout layout)
{
    // 无效纹理没有可写入 descriptor 的 image view/sampler。
    if ( !isValid() ) return nullptr;

    // 使用裸 Vulkan layout 句柄作为 unordered_map key，避免包装类型哈希差异。
    VkDescriptorSetLayout lHandle = (VkDescriptorSetLayout)layout;

    {
        // 常态命中允许多个渲染读取者并行返回稳定句柄。
        std::shared_lock descriptorLock(m_descriptorMutex);
        auto             it = m_nativeSets.find(lHandle);
        if ( it != m_nativeSets.end() && m_nativePool == pool ) {
            return it->second;
        }
    }

    // 缓存 miss 后升级为独占锁并再次检查，避免竞争线程重复分配。
    std::unique_lock descriptorLock(m_descriptorMutex);
    auto             it = m_nativeSets.find(lHandle);
    if ( it != m_nativeSets.end() && m_nativePool == pool ) {
        return it->second;
    }

    // 缓存集合不能跨 pool 释放或复用；pool 身份变化时整组淘汰旧 layout 映射。
    if ( m_nativePool && m_nativePool != pool ) {
        // pool allocate/free
        // 必须跨纹理实例外部同步，遵循对象锁后全局锁的固定顺序。
        std::lock_guard poolLock(descriptorPoolMutationMutex());
        for ( auto& [oldLayout, set] : m_nativeSets ) {
            // oldLayout 仅作 key；真正释放只需要旧 pool 与 set。
            (void)m_device.freeDescriptorSets(m_nativePool, set);
        }
        m_nativeSets.clear();
    }
    // 记录本轮 pool，即使 map 为空也约束下一次缓存命中来源。
    m_nativePool = pool;

    // 每个 layout 只分配一个纹理 set，缓存生命周期覆盖该 pool 的使用期。
    vk::DescriptorSetAllocateInfo allocInfo(pool, 1, &layout);
    vk::DescriptorSet             newSet;
    {
        // Vulkan 要求同一 pool 的 allocate/free 外部同步。
        std::lock_guard poolLock(descriptorPoolMutationMutex());
        newSet = m_device.allocateDescriptorSets(allocInfo).value[0];
    }

    // 纹理初始化结束时已处于 ShaderReadOnlyOptimal，descriptor layout
    // 与之匹配。
    vk::DescriptorImageInfo imageInfo(
        m_sampler, m_imageView, vk::ImageLayout::eShaderReadOnlyOptimal);

    // 原生画笔布局约定 binding 0 为单个 combined image sampler。
    vk::WriteDescriptorSet descriptorWrite(
        newSet, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo);

    // updateDescriptorSets 返回后已复制 imageInfo，不保存栈上指针。
    m_device.updateDescriptorSets(descriptorWrite, nullptr);

    // 最后发布到 map，保证其他 shared-lock 读取者只观察到完整初始化的 set。
    m_nativeSets[lHandle] = newSet;
    return newSet;
}

/// @brief 在物理设备允许的类型中查找包含全部请求属性的 memory type。
/// @param physDevice 提供 memory type 表的物理设备。
/// @param typeFilter Vulkan resource requirements 给出的候选 bit mask。
/// @param properties 调用方要求同时具备的 memory property flags。
/// @return 第一个满足候选位和全部属性的索引；不存在时返回 std::nullopt。
/// @warning 资源创建路径：遍历固定上限 memoryTypes，不分配或抛出异常。
std::optional<uint32_t> VKTexture::findMemoryType(
    vk::PhysicalDevice& physDevice, uint32_t typeFilter,
    vk::MemoryPropertyFlags properties)
{
    // memoryTypeCount 由 Vulkan
    // 规范限制为固定小规模，线性扫描仅发生在资源创建时。
    vk::PhysicalDeviceMemoryProperties memProperties =
        physDevice.getMemoryProperties();
    for ( uint32_t i = 0; i < memProperties.memoryTypeCount; i++ ) {
        // 必须既被 resource requirements 接受，也包含 properties 的每一个 bit。
        if ( (typeFilter & (1 << i)) &&
             (memProperties.memoryTypes[i].propertyFlags & properties) ==
                 properties ) {
            return i;
        }
    }
    // 返回 optional 让上层按当前已创建资源阶段执行无异常回滚。
    XERROR("Failed to find suitable Vulkan memory type");
    return std::nullopt;
}

/// @brief 用一次性命令同步转换静态纹理图像布局。
///
/// 当前只支持初始化所需的 Undefined -> TransferDst 和 TransferDst ->
/// ShaderRead 两种转换，并为每种组合设置精确 access/stage。提交后等待 queue
/// idle，保证后续 copy、view/descriptor 使用可立即继续。
///
/// @param pool 分配临时 primary command buffer 的命令池。
/// @param queue 执行布局转换并与 pool 队列族兼容的队列。
/// @param oldLayout 调用方保证的当前布局。
/// @param newLayout 下一初始化阶段要求的目标布局。
/// @return 支持该转换并完成同步提交时返回 true；未知组合返回 false。
/// @warning 低频静态纹理上传路径：内部 queue.waitIdle，禁止用于 streaming
/// 或每帧 图像更新。
bool VKTexture::transitionImageLayout(vk::CommandPool pool, vk::Queue queue,
                                      vk::ImageLayout oldLayout,
                                      vk::ImageLayout newLayout)
{
    // 临时 command buffer 只记录一个 image barrier，完成后立即归还 pool。
    vk::CommandBufferAllocateInfo allocInfo(
        pool, vk::CommandBufferLevel::ePrimary, 1);
    vk::CommandBuffer cmd = m_device.allocateCommandBuffers(allocInfo).value[0];

    // eOneTimeSubmit 表明命令不会重复提交。
    (void)cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });

    // 静态纹理固定为单颜色 mip/layer，barrier 覆盖完整图像。
    vk::ImageMemoryBarrier barrier(
        {},
        {},
        oldLayout,
        newLayout,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        m_image,
        { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });

    // access masks 与 pipeline stages 在受支持分支中成对设置。
    vk::PipelineStageFlags sourceStage;
    vk::PipelineStageFlags destinationStage;

    if ( oldLayout == vk::ImageLayout::eUndefined &&
         newLayout == vk::ImageLayout::eTransferDstOptimal ) {
        // Undefined 内容无需等待生产者，首次消费者是 transfer write。
        barrier.setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
        sourceStage      = vk::PipelineStageFlagBits::eTopOfPipe;
        destinationStage = vk::PipelineStageFlagBits::eTransfer;
    } else if ( oldLayout == vk::ImageLayout::eTransferDstOptimal &&
                newLayout == vk::ImageLayout::eShaderReadOnlyOptimal ) {
        // copy 的 transfer write 必须在 fragment shader 采样前可见。
        barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite);
        barrier.setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        sourceStage      = vk::PipelineStageFlagBits::eTransfer;
        destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
    } else {
        // 未知组合不提交不完整 barrier，并释放尚未在途的临时 command buffer。
        XERROR("Unsupported texture layout transition: {} -> {}",
               vk::to_string(oldLayout),
               vk::to_string(newLayout));
        m_device.freeCommandBuffers(pool, cmd);
        return false;
    }

    // 不需要额外 memory/buffer barrier，本次同步只涉及 m_image。
    cmd.pipelineBarrier(
        sourceStage, destinationStage, {}, nullptr, nullptr, barrier);
    (void)cmd.end();

    // 没有 semaphore/fence；queue idle 直接建立初始化阶段的完整同步边界。
    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &cmd);
    (void)queue.submit(submitInfo, nullptr);
    (void)queue.waitIdle();
    // 等待完成后 command buffer 不再在途，可以安全释放。
    m_device.freeCommandBuffers(pool, cmd);
    return true;
}

/// @brief 用一次性命令把 staging buffer 的紧密像素数据复制到纹理图像。
///
/// 调用方保证 m_image 已处于 TransferDstOptimal，buffer 至少包含 width*height
/// 对应像素。函数记录整张单 mip/单 layer copy，提交并等待队列空闲后释放命令
/// 缓冲。
///
/// @param pool 分配临时 primary command buffer 的命令池。
/// @param queue 执行 copy 且与 pool 队列族兼容的队列。
/// @param buffer 已填充的 transfer-source staging buffer。
/// @param width 复制区域宽度。
/// @param height 复制区域高度。
/// @warning 低频静态纹理上传路径：内部 queue.waitIdle；动态帧必须改用
/// recordStreamingUpload 的非提交录制协议。
void VKTexture::copyBufferToImage(vk::CommandPool pool, vk::Queue queue,
                                  vk::Buffer buffer, uint32_t width,
                                  uint32_t height)
{
    // 临时命令只提交一次，完成后归还调用方命令池。
    vk::CommandBufferAllocateInfo allocInfo(
        pool, vk::CommandBufferLevel::ePrimary, 1);
    vk::CommandBuffer cmd = m_device.allocateCommandBuffers(allocInfo).value[0];

    (void)cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
    // rowLength/imageHeight=0 表示紧密排列；子资源覆盖颜色 mip0 layer0。
    vk::BufferImageCopy region(0,
                               0,
                               0,
                               { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                               { 0, 0, 0 },
                               { width, height, 1 });
    // 布局已由 transitionImageLayout 建立，本函数不重复记录 barrier。
    cmd.copyBufferToImage(
        buffer, m_image, vk::ImageLayout::eTransferDstOptimal, region);
    (void)cmd.end();

    // 同步等待保证调用者返回后可释放 staging buffer。
    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &cmd);
    (void)queue.submit(submitInfo, nullptr);
    (void)queue.waitIdle();
    // queue idle 后 command buffer 生命周期结束。
    m_device.freeCommandBuffers(pool, cmd);
}

}  // namespace MMM::Graphic
