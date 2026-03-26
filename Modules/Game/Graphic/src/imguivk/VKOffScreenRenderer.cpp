#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/VKTexture.h"
#include "log/colorful-log.h"
#include "vulkan/vulkan.hpp"
#include <glm/ext.hpp>

namespace MMM::Graphic
{

VKOffScreenRenderer::VKOffScreenRenderer() {}

VKOffScreenRenderer::~VKOffScreenRenderer()
{
    // 1. 先等待设备空闲，防止正在渲染时销毁
    if ( m_device ) m_device.waitIdle();

    releaseResources();
    XINFO("VKOffScreenRenderer destroyed.");
}


/// @brief 录制gpu指令
void VKOffScreenRenderer::recordCmds(vk::CommandBuffer&   cmdBuf,
                                     UI::IRenderableView* renderable_view)
{
    // 1. 设置清除颜色 (Alpha 为 0，确保画布背景透明)
    vk::ClearValue clearValue;
    clearValue.setColor(
        vk::ClearColorValue(std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f }));

    // 获取渲染数据
    const auto& vertices = getVertices();
    const auto& indices  = getIndices();
    // const auto& drawCmds = brush.getCmds();

    if ( vertices.empty() || indices.empty() ) return;

    uploadUniformBuffer2GPU();

    // 每一帧将 CPU 的顶点数据上传到 GPU
    m_vertexBuffer->uploadData(vertices.data(),
                               vertices.size() * sizeof(Vertex::VKBasicVertex));
    if ( !indices.empty() ) {
        m_indexBuffer->uploadData(indices.data(),
                                  indices.size() * sizeof(uint32_t));
    }

    // 2. 开始渲染流程 (针对离屏 Framebuffer)
    vk::RenderPassBeginInfo rpBegin;
    rpBegin.setRenderPass(m_offScreenRenderPass->getRenderPass())
        .setFramebuffer(m_framebuffer)
        .setRenderArea(vk::Rect2D({ 0, 0 }, { m_width, m_height }))
        .setClearValues(clearValue);

    cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
    {
        // 3. 因为开启了动态状态，必须手动设置 Viewport 和 Scissor
        vk::Viewport viewport(
            0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f);
        vk::Rect2D scissor({ 0, 0 }, { m_width, m_height });
        cmdBuf.setViewport(0, 1, &viewport);
        cmdBuf.setScissor(0, 1, &scissor);

        // 4. 绑定离屏专属管线
        cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                            m_mainBrushRenderPipeline->m_graphicsPipeline);

        // 4. 绑定描述符集
        // 这里使用的 Layout 必须是创建该 Pipeline 时用的那个 Layout
        cmdBuf.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
            0,
            1,
            &m_offScreenDescriptorSet,
            0,
            nullptr);

        // ==========================================
        // ★ 发送 Push Constant：正交投影矩阵
        // ==========================================
        // 作用：将屏幕像素坐标 (0,0) 到 (width, height) 映射到 Vulkan
        // 设备标准坐标 (-1 到 1)
        glm::mat4 ortho = glm::ortho(
            0.0f, (float)m_width, 0.0f, (float)m_height, -1.0f, 1.0f);

        cmdBuf.pushConstants(
            m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
            vk::ShaderStageFlagBits::eVertex,
            0,
            sizeof(glm::mat4),
            &ortho);

        // 5. 绑定顶点缓冲区
        cmdBuf.bindVertexBuffers(0, m_vertexBuffer->m_vkBuffer, { 0 });

        // 修改最后的绘制命令：
        if ( !indices.empty() ) {
            cmdBuf.bindIndexBuffer(
                m_indexBuffer->m_vkBuffer, 0, vk::IndexType::eUint32);
            cmdBuf.drawIndexed(
                static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
        } else {
            cmdBuf.draw(static_cast<uint32_t>(vertices.size()), 1, 0, 0);
        }
    }
    cmdBuf.endRenderPass();
}

}  // namespace MMM::Graphic
