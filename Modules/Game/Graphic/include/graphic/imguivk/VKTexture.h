#pragma once

#include "imgui_impl_vulkan.h"
#include <cstddef>
#include <filesystem>
#include <imgui.h>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
{

/// @brief 从内存创建纹理时使用的像素格式。
enum class VKTexturePixelFormat {
    Rgba8,  ///< RGBA8 UNORM，每像素 4 字节。
    R8,     ///< R8 UNORM，每像素 1 字节，采样时 R 通道映射到 RGB。
    R8Red   ///< R8 UNORM，每像素 1 字节，采样时只映射到 R 通道。
};

/// @brief Vulkan 纹理资源，封装图像、采样器以及 ImGui/原生管线描述符。
class VKTexture
{
public:
    /// @brief 从文件加载 RGBA8 纹理。
    VKTexture(const std::filesystem::path& filePath,
              vk::PhysicalDevice& physicalDevice, vk::Device& device,
              vk::CommandPool commandPool, vk::Queue queue);

    /// @brief 从内存像素加载纹理。
    /// @param pixels 像素数据起始地址。
    /// @param width 纹理宽度。
    /// @param height 纹理高度。
    /// @param physicalDevice Vulkan 物理设备。
    /// @param device Vulkan 逻辑设备。
    /// @param commandPool 上传命令池。
    /// @param queue 上传队列。
    /// @param pixelFormat 像素格式，默认 RGBA8。
    VKTexture(const unsigned char* pixels, uint32_t width, uint32_t height,
              vk::PhysicalDevice& physicalDevice, vk::Device& device,
              vk::CommandPool commandPool, vk::Queue queue,
              VKTexturePixelFormat pixelFormat = VKTexturePixelFormat::Rgba8);

    /// @brief 转移纹理及其持久映射的分帧上传资源。
    VKTexture(VKTexture&& other) noexcept;

    /// @brief 释放当前资源后接管另一个纹理的全部 Vulkan 资源。
    /// @warning 低频资源生命周期路径：赋值前必须保证当前纹理不再被 GPU 使用。
    VKTexture& operator=(VKTexture&& other) noexcept;

    /// @brief 禁止复制 Vulkan 资源所有权。
    VKTexture(const VKTexture&) = delete;

    /// @brief 禁止复制赋值 Vulkan 资源所有权。
    VKTexture& operator=(const VKTexture&) = delete;

    /// @brief 释放纹理、描述符和持久映射上传资源。
    /// @warning 低频资源生命周期路径：析构前必须保证资源不再被 GPU 使用。
    ~VKTexture();

    /// @brief 为 RGBA8 动态帧创建持久映射的分帧上传缓冲。
    /// @param physicalDevice 用于选择 Host Visible/Coherent
    /// 内存类型的物理设备。
    /// @param frameSlots 与渲染器并发帧数量一致的上传槽位数。
    /// @return 所有槽位创建并映射成功时返回 true。
    /// @warning 低频资源准备路径：可能分配 Vulkan
    /// Buffer/Memory；调用前必须确保旧 streaming
    /// 槽位不再被在途命令使用，禁止放入每帧无条件路径。
    bool prepareStreamingUpload(vk::PhysicalDevice& physicalDevice,
                                uint32_t            frameSlots = 2);

    /// @brief 将一帧 RGBA8 像素复制到持久 staging 槽位并记录图像上传命令。
    /// @param commandBuffer 当前帧正在录制、且位于 RenderPass 外的命令缓冲。
    /// @param frameIndex 当前并发帧对应的 staging 槽位索引。
    /// @param pixels 连续 RGBA8 像素数据。
    /// @param byteCount 像素字节数，必须严格等于 width * height * 4。
    /// @return 参数有效且上传命令已记录时返回 true。
    /// @warning 渲染命令录制热路径：会执行一次完整帧 memcpy 和固定数量 Vulkan
    /// 命令记录；调用方必须保证该 frameIndex 槽位的上一帧 Fence 已完成，且同一
    /// 纹理不会被并发录制；禁止加入分配、submit、waitIdle、文件访问或锁等待。
    bool recordStreamingUpload(vk::CommandBuffer& commandBuffer,
                               uint32_t frameIndex, const unsigned char* pixels,
                               std::size_t byteCount);

    /// @brief 获取 ImGui 可使用的纹理 ID，首次调用时注册描述符。
    /// @warning 资源准备路径：首次调用会从 ImGui descriptor pool
    /// 分配描述符，必须与其他 descriptor pool allocate/free 外部同步。
    ImTextureID getImTextureID();

    // 暴露句柄供 DescriptorSet 更新使用
    vk::ImageView getImageView() const { return m_imageView; }
    vk::Sampler   getSampler() const { return m_sampler; }
    /// @brief 获取已缓存的 ImGui 描述符集。
    vk::DescriptorSet getDescriptorSet() const;

    /**
     * @brief 获取适配本项目原生管线的描述符集 (CombinedImageSampler)
     * @warning 渲染命令录制热路径：缓存命中仅读取句柄；首次分配会同步
     * descriptor pool，禁止在每条 draw command 中制造新 layout 或新 pool。
     */
    vk::DescriptorSet getNativeDescriptorSet(vk::DescriptorPool      pool,
                                             vk::DescriptorSetLayout layout);

    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    /// @brief 判断纹理是否成功创建了 Vulkan 资源。
    /// @return 纹理图像、视图和采样器均可用时返回 true。
    bool isValid() const { return m_valid; }

private:
    /// @brief 从内存像素初始化 Vulkan 纹理资源。
    bool initFromPixels(const unsigned char* pixels, uint32_t width,
                        uint32_t height, vk::PhysicalDevice& physDevice,
                        vk::CommandPool pool, vk::Queue queue,
                        VKTexturePixelFormat pixelFormat);

    std::optional<uint32_t> findMemoryType(vk::PhysicalDevice&     physDevice,
                                           uint32_t                typeFilter,
                                           vk::MemoryPropertyFlags properties);

    bool transitionImageLayout(vk::CommandPool pool, vk::Queue queue,
                               vk::ImageLayout oldLayout,
                               vk::ImageLayout newLayout);

    void copyBufferToImage(vk::CommandPool pool, vk::Queue queue,
                           vk::Buffer buffer, uint32_t width, uint32_t height);

    /// @brief 释放纹理持有的 Vulkan 资源与描述符。
    /// @warning 资源销毁路径：descriptor set 释放需要与同一 descriptor pool
    /// 的 allocate/free 外部同步。
    void releaseResources();

    /// @brief 解除映射并释放所有 streaming staging 槽位。
    /// @warning 低频资源销毁路径：调用方必须保证对应槽位不再被 GPU 使用。
    void releaseStreamingUploadResources();

    /// @brief 单个并发帧持有的持久 streaming staging 资源。
    struct StreamingUploadSlot {
        vk::Buffer m_buffer{ nullptr };  ///< Host 写入、Transfer 读取的缓冲。
        vk::DeviceMemory m_memory{ nullptr };  ///< staging 缓冲绑定的内存。
        void* m_mappedPixels{ nullptr };       ///< 生命周期内持续映射的地址。
    };

private:
    vk::Device        m_device{ nullptr };
    vk::Image         m_image{ nullptr };
    vk::DeviceMemory  m_memory{ nullptr };
    vk::ImageView     m_imageView{ nullptr };
    vk::Sampler       m_sampler{ nullptr };
    vk::DescriptorSet m_descriptorSet{ nullptr };

    // 原生管线用的描述符集映射 (Layout -> DescriptorSet)
    std::unordered_map<VkDescriptorSetLayout, vk::DescriptorSet> m_nativeSets;
    vk::DescriptorPool m_nativePool{ nullptr };

    /// @brief 保护 ImGui 与原生 descriptor 缓存，允许缓存命中并发读取。
    mutable std::shared_mutex m_descriptorMutex;

    uint32_t m_width{ 0 };
    uint32_t m_height{ 0 };
    /// @brief 当前图像使用的像素格式，用于限制 streaming 路径只接收 RGBA8。
    VKTexturePixelFormat m_pixelFormat{ VKTexturePixelFormat::Rgba8 };
    /// @brief 每个并发帧独占的 streaming staging 资源。
    std::vector<StreamingUploadSlot> m_streamingUploadSlots;
    /// @brief 单帧 streaming 上传必须提供的 RGBA8 字节数。
    std::size_t m_streamingUploadByteCount{ 0 };
    bool        m_valid{ false };
};

}  // namespace MMM::Graphic
