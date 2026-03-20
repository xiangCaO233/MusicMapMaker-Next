#pragma once

#include "imgui_impl_vulkan.h"
#include <imgui.h>
#include <string>
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
{

/**
 * @brief Vulkan 纹理封装类
 *
 * 负责从文件加载图片、创建 Vulkan Image 资源、上传数据，
 * 并将其注册到 ImGui 的描述符池中。
 */
class VKTexture
{
public:
    VKTexture(const std::string& filePath, vk::PhysicalDevice& physicalDevice,
              vk::Device& device, vk::CommandPool commandPool, vk::Queue queue);
    // 允许移动
    VKTexture(VKTexture&& other) noexcept;
    VKTexture& operator=(VKTexture&& other) noexcept;
    // 禁用拷贝
    VKTexture(const VKTexture&)            = delete;
    VKTexture& operator=(const VKTexture&) = delete;
    ~VKTexture();

    /// @brief 获取 ImGui 可用的纹理 ID
    ImTextureID getImTextureID()
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

    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }

private:
    void createTextureImage(const std::string&  filePath,
                            vk::PhysicalDevice& physDevice,
                            vk::CommandPool pool, vk::Queue queue);
    void createTextureImageView();
    void createTextureSampler();

    // 辅助工具：寻找内存类型
    uint32_t findMemoryType(vk::PhysicalDevice& physDevice, uint32_t typeFilter,
                            vk::MemoryPropertyFlags properties);

    // 辅助工具：执行单次命令
    void transitionImageLayout(vk::CommandPool pool, vk::Queue queue,
                               vk::ImageLayout oldLayout,
                               vk::ImageLayout newLayout);
    void copyBufferToImage(vk::CommandPool pool, vk::Queue queue,
                           vk::Buffer buffer, uint32_t width, uint32_t height);

private:
    vk::Device       m_device{ nullptr };
    vk::Image        m_image{ nullptr };
    vk::DeviceMemory m_memory{ nullptr };
    vk::ImageView    m_imageView{ nullptr };
    vk::Sampler      m_sampler{ nullptr };

    // ImGui 相关的描述符集句柄
    vk::DescriptorSet m_descriptorSet{ nullptr };

    uint32_t m_width{ 0 };
    uint32_t m_height{ 0 };
};
}  // namespace MMM::Graphic
