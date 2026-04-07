#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "config/skin/SkinConfig.h"
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
void VKOffScreenRenderer::recordCmds(vk::CommandBuffer& cmdBuf)
{
    // 1. 设置清除颜色 (Alpha 为 0，确保画布背景透明)
    vk::ClearValue clearValue;
    clearValue.setColor(
        vk::ClearColorValue(std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f }));

    // 获取渲染数据
    const auto& vertices = getVertices();
    const auto& indices  = getIndices();

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
        cmdBuf.bindIndexBuffer(
            m_indexBuffer->m_vkBuffer, 0, vk::IndexType::eUint32);

        // 6. 解析 DrawCmds 进行批次渲染 (回调到 UI 实现层)
        onRecordDrawCmds(cmdBuf,
                         m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
                         m_offScreenDescriptorSet);
    }
    cmdBuf.endRenderPass();

    // 7. 发光特效：如果有 glowShader 且启用了光效，进行多轮渲染
    if ( m_glowBrushRenderPipeline && m_blurRenderPipeline &&
         m_compositeRenderPipeline ) {
        // --- 7.1. 渲染发光几何体到 m_glowFramebuffer ---
        rpBegin.setFramebuffer(m_glowFramebuffer);
        cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
        {
            vk::Viewport viewport(
                0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f);
            vk::Rect2D scissor({ 0, 0 }, { m_width, m_height });
            cmdBuf.setViewport(0, 1, &viewport);
            cmdBuf.setScissor(0, 1, &scissor);

            cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                m_glowBrushRenderPipeline->m_graphicsPipeline);

            cmdBuf.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics,
                m_glowBrushRenderPipeline->m_graphicsPipelineLayout,
                0,
                1,
                &m_offScreenDescriptorSet,
                0,
                nullptr);

            glm::mat4 ortho = glm::ortho(
                0.0f, (float)m_width, 0.0f, (float)m_height, -1.0f, 1.0f);
            cmdBuf.pushConstants(
                m_glowBrushRenderPipeline->m_graphicsPipelineLayout,
                vk::ShaderStageFlagBits::eVertex |
                    vk::ShaderStageFlagBits::eFragment,
                0,
                sizeof(glm::mat4),
                &ortho);

            cmdBuf.bindVertexBuffers(0, m_vertexBuffer->m_vkBuffer, { 0 });
            cmdBuf.bindIndexBuffer(
                m_indexBuffer->m_vkBuffer, 0, vk::IndexType::eUint32);

            // 绘制发光几何体
            onRecordGlowCmds(
                cmdBuf,
                m_glowBrushRenderPipeline->m_graphicsPipelineLayout,
                m_offScreenDescriptorSet);
        }
        cmdBuf.endRenderPass();

        // 切换 m_glowImage 到 ShaderReadOnlyOptimal
        vk::ImageMemoryBarrier barrier(
            {},
            {},
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            m_glowImage,
            { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });
        barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmdBuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::PipelineStageFlagBits::eFragmentShader,
            {},
            nullptr,
            nullptr,
            barrier);

        // --- 7.2. Ping-Pong 模糊渲染 ---
        int passes = MMM::Config::SkinManager::instance().getGlowPasses();
        if ( passes > 0 ) {
            bool ping = true;
            for ( int i = 0; i < passes; ++i ) {
                vk::Framebuffer currentFb =
                    ping ? m_pingFramebuffer : m_pongFramebuffer;
                vk::DescriptorSet currentSampleSet =
                    (i == 0)
                        ? m_glowDescriptorSet
                        : (ping ? m_pongDescriptorSet : m_pingDescriptorSet);

                rpBegin.setFramebuffer(currentFb);
                cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
                {
                    vk::Viewport viewport(0.0f,
                                          0.0f,
                                          (float)m_width,
                                          (float)m_height,
                                          0.0f,
                                          1.0f);
                    vk::Rect2D   scissor({ 0, 0 }, { m_width, m_height });
                    cmdBuf.setViewport(0, 1, &viewport);
                    cmdBuf.setScissor(0, 1, &scissor);

                    cmdBuf.bindPipeline(
                        vk::PipelineBindPoint::eGraphics,
                        m_blurRenderPipeline->m_graphicsPipeline);
                    cmdBuf.bindDescriptorSets(
                        vk::PipelineBindPoint::eGraphics,
                        m_blurRenderPipeline->m_graphicsPipelineLayout,
                        0,
                        1,
                        &currentSampleSet,
                        0,
                        nullptr);

                    struct BlurPC {
                        glm::vec4 dirAndPasses;
                        glm::vec4 pad1, pad2, pad3;
                    } pc;
                    pc.dirAndPasses = glm::vec4(ping ? 3.0f / m_width : 0.0f,
                                                ping ? 0.0f : 3.0f / m_height,
                                                0.0f,
                                                0.0f);

                    cmdBuf.pushConstants(
                        m_blurRenderPipeline->m_graphicsPipelineLayout,
                        vk::ShaderStageFlagBits::eVertex |
                            vk::ShaderStageFlagBits::eFragment,
                        0,
                        sizeof(BlurPC),
                        &pc);

                    cmdBuf.draw(3, 1, 0, 0);  // Full screen quad
                }
                cmdBuf.endRenderPass();

                ping = !ping;
            }

            // --- 7.3. 叠加最终结果到主画布 (m_image) ---
            rpBegin.setRenderPass(m_compositeRenderPass->getRenderPass());
            rpBegin.setFramebuffer(m_framebuffer);  // render to main image
            cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
            {
                vk::Viewport viewport(
                    0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f);
                vk::Rect2D scissor({ 0, 0 }, { m_width, m_height });
                cmdBuf.setViewport(0, 1, &viewport);
                cmdBuf.setScissor(0, 1, &scissor);

                cmdBuf.bindPipeline(
                    vk::PipelineBindPoint::eGraphics,
                    m_compositeRenderPipeline->m_graphicsPipeline);

                vk::DescriptorSet finalSampleSet =
                    ping ? m_pongDescriptorSet : m_pingDescriptorSet;
                cmdBuf.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    m_compositeRenderPipeline->m_graphicsPipelineLayout,
                    0,
                    1,
                    &finalSampleSet,
                    0,
                    nullptr);

                struct BlurPC {
                    glm::vec4 dirAndPasses;
                    glm::vec4 pad1, pad2, pad3;
                } pc;
                pc.dirAndPasses = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);

                cmdBuf.pushConstants(
                    m_compositeRenderPipeline->m_graphicsPipelineLayout,
                    vk::ShaderStageFlagBits::eVertex |
                        vk::ShaderStageFlagBits::eFragment,
                    0,
                    sizeof(BlurPC),
                    &pc);

                cmdBuf.draw(3, 1, 0, 0);  // Full screen quad
            }
            cmdBuf.endRenderPass();
        }
    }
}

}  // namespace MMM::Graphic
