#pragma once

#include "graphic/vk/VKQueueFamilyDef.h"
#include "graphic/vk/VKRenderPass.h"
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
{

/**
 * @brief Vulkan 交换链管理类
 *
 * 负责管理显示缓冲区（Swapchain
 * Images）、图像视图（ImageViews）和帧缓冲区（Framebuffers）。
 * 它是连接渲染管线和显示表面的桥梁。
 */
class VKSwapchain final
{
public:
    /**
     * @brief 构造函数，创建交换链及其图像资源
     *
     * @param vkPhysicalDevice 物理设备引用 (查询表面支持能力)
     * @param vkLogicalDevice 逻辑设备引用
     * @param vkSurface 窗口表面句柄
     * @param queueFamilyIndices 队列族索引信息
     * @param w 期望的宽度
     * @param h 期望的高度
     */
    VKSwapchain(vk::PhysicalDevice& vkPhysicalDevice,
                vk::Device&         vkLogicalDevice, vk::SurfaceKHR& vkSurface,
                QueueFamilyIndices& queueFamilyIndices, int          w, int h);

    // 禁用拷贝和移动
    VKSwapchain(VKSwapchain&&) = delete;

    VKSwapchain(const VKSwapchain&) = delete;

    VKSwapchain& operator=(VKSwapchain&&) = delete;

    VKSwapchain& operator=(const VKSwapchain&) = delete;

    ~VKSwapchain();

    /**
     * @brief 获取交换链创建信息
     * @return const vk::SwapchainCreateInfoKHR&
     * 包含图像格式、尺寸、呈现模式等信息
     */
    const vk::SwapchainCreateInfoKHR& info() const;

    /**
     * @brief 创建帧缓冲区 (Framebuffer)
     *
     * @note 时序要求：必须在 RenderPass 创建之后手动调用此函数，
     * 因为 Framebuffer 依赖于 RenderPass 的结构。
     *
     * @param renderPass 渲染流程引用
     */
    void createFramebuffers(const VKRenderPass& renderPass);

    /**
     * @brief 销毁帧缓冲区
     *
     * @note 时序要求：必须在 Swapchain 析构之前手动调用（通常在 Context
     * 析构中）， 或者在重建 Swapchain 时调用。
     */
    void destroyFramebuffers();

private:
    /// @brief 逻辑设备引用
    vk::Device& m_vkLogicalDevice;

    /// @brief Vulkan 交换链句柄
    vk::SwapchainKHR m_swapchain;

    /// @brief 缓存的交换链创建信息
    vk::SwapchainCreateInfoKHR m_swapchainCreateInfo;

    /**
     * @brief 内部图像缓冲结构体
     * 包含每一帧所需的图像资源
     */
    struct ImageBuffer
    {
        /// @brief Swapchain 原生图像句柄
        vk::Image vk_image;

        /// @brief 图像视图句柄 (描述如何访问图像)
        vk::ImageView vk_imageView;

        /// @brief 帧缓冲区句柄 (绑定到 RenderPass)
        vk::Framebuffer vk_frameBuffer;
    };

    /**
     * @brief 图像缓冲列表
     * 数量通常为 2 (双重缓冲) 或 3 (三重缓冲)，取决于 Swapchain 创建时的配置。
     */
    std::vector<ImageBuffer> m_vkImageBuffers;

    // 允许 Renderer 直接访问内部的 ImageBuffers
    friend class VKRenderer;
};
} // namespace MMM::Graphic


