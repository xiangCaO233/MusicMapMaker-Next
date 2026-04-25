#pragma once

#include "imgui_impl_vulkan.h"
#include <imgui.h>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
{

class VKTexture
{
public:
    // 构造方式 A：从文件加载
    VKTexture(const std::string& filePath, vk::PhysicalDevice& physicalDevice,
              vk::Device& device, vk::CommandPool commandPool, vk::Queue queue);

    // 构造方式 B：直接从内存像素加载 (用于纯色或动态生成纹理)
    VKTexture(const unsigned char* pixels, uint32_t width, uint32_t height,
              vk::PhysicalDevice& physicalDevice, vk::Device& device,
              vk::CommandPool commandPool, vk::Queue queue);

    VKTexture(VKTexture&& other) noexcept;
    VKTexture& operator=(VKTexture&& other) noexcept;
    VKTexture(const VKTexture&)            = delete;
    VKTexture& operator=(const VKTexture&) = delete;
    ~VKTexture();

    inline ImTextureID getImTextureID()
    {
        if ( !m_descriptorSet ) {
            // 4. 注册到 ImGui
            // ImGui_ImplVulkan_AddTexture 内部会从它持有的全局 DescriptorPool
            // 中分配一个 Set
            m_descriptorSet = (vk::DescriptorSet)ImGui_ImplVulkan_AddTexture(
                (VkSampler)m_sampler,
                (VkImageView)m_imageView,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        return reinterpret_cast<ImTextureID>(
            static_cast<VkDescriptorSet>(m_descriptorSet));
    }


    // 暴露句柄供 DescriptorSet 更新使用
    vk::ImageView     getImageView() const { return m_imageView; }
    vk::Sampler       getSampler() const { return m_sampler; }
    vk::DescriptorSet getDescriptorSet() const { return m_descriptorSet; }

    /**
     * @brief 获取适配本项目原生管线的描述符集 (CombinedImageSampler)
     */
    vk::DescriptorSet getNativeDescriptorSet(vk::DescriptorPool      pool,
                                             vk::DescriptorSetLayout layout);

    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }

private:
    // 【共通核心逻辑】
    void initFromPixels(const unsigned char* pixels, uint32_t width,
                        uint32_t height, vk::PhysicalDevice& physDevice,
                        vk::CommandPool pool, vk::Queue queue);

    uint32_t findMemoryType(vk::PhysicalDevice& physDevice, uint32_t typeFilter,
                            vk::MemoryPropertyFlags properties);

    void transitionImageLayout(vk::CommandPool pool, vk::Queue queue,
                               vk::ImageLayout oldLayout,
                               vk::ImageLayout newLayout);

    void copyBufferToImage(vk::CommandPool pool, vk::Queue queue,
                           vk::Buffer buffer, uint32_t width, uint32_t height);

private:
    vk::Device        m_device{ nullptr };
    vk::Image         m_image{ nullptr };
    vk::DeviceMemory  m_memory{ nullptr };
    vk::ImageView     m_imageView{ nullptr };
    vk::Sampler       m_sampler{ nullptr };
    vk::DescriptorSet m_descriptorSet{ nullptr };

    // 原生管线用的描述符集映射 (Layout -> DescriptorSet)
    std::unordered_map<VkDescriptorSetLayout, vk::DescriptorSet> m_nativeSets;
    vk::DescriptorPool                                           m_nativePool{
        nullptr
    };

    uint32_t m_width{ 0 };
    uint32_t m_height{ 0 };
};

}  // namespace MMM::Graphic
