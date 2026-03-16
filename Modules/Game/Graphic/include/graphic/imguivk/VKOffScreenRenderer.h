#pragma once

#include "graphic/imguivk/VKRenderPipeline.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "graphic/imguivk/mem/VKMemBuffer.h"
#include "vulkan/vulkan.hpp"
#include <atomic>
#include <cstddef>
#include <memory>

namespace MMM::Graphic
{
namespace UI
{
class IRenderableView;
}

class VKOffScreenRenderer
{
public:
    VKOffScreenRenderer();
    VKOffScreenRenderer(VKOffScreenRenderer&&)                 = delete;
    VKOffScreenRenderer(const VKOffScreenRenderer&)            = delete;
    VKOffScreenRenderer& operator=(VKOffScreenRenderer&&)      = delete;
    VKOffScreenRenderer& operator=(const VKOffScreenRenderer&) = delete;
    ~VKOffScreenRenderer();

    vk::DescriptorSet getDescriptorSet() const { return m_imguiDescriptor; }

    /// @brief 录制gpu指令
    void recordCmds(vk::CommandBuffer&   cmdBuf,
                    UI::IRenderableView* renderable_view,
                    vk::DescriptorSet    descriptorSet);

    /// @brief 重建帧缓冲
    void reCreateFrameBuffer(vk::PhysicalDevice& phyDevice,
                             vk::Device& logicalDevice, VKSwapchain& swapchain,
                             VKShader& shader, size_t maxVertexCount = 1024);

    /// @brief 外部确认是否需要重建
    inline bool needReCreateFrameBuffer() { return m_need_reCreate.load(); }

protected:
    /// @brief 画布尺寸
    uint32_t m_width{ 0 };
    uint32_t m_height{ 0 };

    // UI 请求的目标尺寸
    uint32_t m_targetWidth{ 0 };
    uint32_t m_targetHeight{ 0 };

    // UI 只设置目标，不改实际尺寸
    void setTargetSize(uint32_t w, uint32_t h)
    {
        if ( w != m_targetWidth || h != m_targetHeight ) {
            m_targetWidth  = w;
            m_targetHeight = h;
            m_need_reCreate.store(true);
        }
    }


    /// @brief 是否需要重建
    std::atomic<bool> m_need_reCreate{ true };

private:
    // --- 1. 物理资源 (独占) ---
    vk::Image        m_image;        // 画布纹理
    vk::DeviceMemory m_imageMemory;  // 纹理内存
    vk::ImageView    m_imageView;    // 纹理视图
    vk::Framebuffer  m_framebuffer;  // 绑定到此纹理的帧缓冲
    vk::Sampler      m_sampler;      // 绑定到此纹理的采样器

    // --- 2. 几何资源 (独占) ---
    // 存 Brush 的顶点
    std::unique_ptr<VKMemBuffer> m_vertexBuffer;

    // --- 3. UI 集成句柄 (独占) ---
    vk::DescriptorSet m_imguiDescriptor{ VK_NULL_HANDLE };  // ImGui 用的贴图 ID

    // --- 4. 引用/外部注入 (非所有权) ---
    // 逻辑设备引用
    vk::Device m_device{ VK_NULL_HANDLE };

    // 离屏渲染流程
    std::unique_ptr<VKRenderPass> m_offScreenRenderPass{ nullptr };

    // 画笔管线
    std::unique_ptr<VKRenderPipeline> m_brushRenderPipeline{ nullptr };

    /// @brief 释放持有的资源
    void releaseResources();
};

}  // namespace MMM::Graphic
