#pragma once

#include "imgui_impl_vulkan.h"
#include <filesystem>
#include <imgui.h>
#include <shared_mutex>
#include <string>
#include <unordered_map>
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

    VKTexture(VKTexture&& other) noexcept;
    VKTexture& operator=(VKTexture&& other) noexcept;
    VKTexture(const VKTexture&)            = delete;
    VKTexture& operator=(const VKTexture&) = delete;
    ~VKTexture();

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

private:
    /// @brief 从内存像素初始化 Vulkan 纹理资源。
    void initFromPixels(const unsigned char* pixels, uint32_t width,
                        uint32_t height, vk::PhysicalDevice& physDevice,
                        vk::CommandPool pool, vk::Queue queue,
                        VKTexturePixelFormat pixelFormat);

    uint32_t findMemoryType(vk::PhysicalDevice& physDevice, uint32_t typeFilter,
                            vk::MemoryPropertyFlags properties);

    void transitionImageLayout(vk::CommandPool pool, vk::Queue queue,
                               vk::ImageLayout oldLayout,
                               vk::ImageLayout newLayout);

    void copyBufferToImage(vk::CommandPool pool, vk::Queue queue,
                           vk::Buffer buffer, uint32_t width, uint32_t height);

    /// @brief 释放纹理持有的 Vulkan 资源与描述符。
    /// @warning 资源销毁路径：descriptor set 释放需要与同一 descriptor pool
    /// 的 allocate/free 外部同步。
    void releaseResources();

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
};

}  // namespace MMM::Graphic
