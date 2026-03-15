#pragma once

#include "graphic/imguivk/mem/VKMemBuffer.h"
#include "vulkan/vulkan.hpp"
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

    void recordCmds(vk::CommandBuffer&   cmdBuf,
                    UI::IRenderableView* renderable_view);

private:
    // --- 1. 物理资源 (独占) ---
    vk::Image        m_image;        // 画布纹理
    vk::DeviceMemory m_imageMemory;  // 纹理内存
    vk::ImageView    m_imageView;    // 纹理视图
    vk::Framebuffer  m_framebuffer;  // 绑定到此纹理的帧缓冲

    // --- 2. 几何资源 (独占) ---
    // 存 Brush 的顶点
    std::unique_ptr<VKMemBuffer> m_vertexBuffer;

    // --- 3. UI 集成句柄 (独占) ---
    vk::DescriptorSet m_imguiDescriptor;  // ImGui 用的贴图 ID

    // --- 4. 引用/外部注入 (非所有权) ---
    vk::Device     m_device;            // 逻辑设备引用
    vk::RenderPass m_sharedRenderPass;  // 外部传入的公共渲染流程
    vk::Pipeline   m_sharedPipeline;    // 外部传入的画笔管线
};

}  // namespace MMM::Graphic
