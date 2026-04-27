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
    if ( m_device ) (void)m_device.waitIdle();

    releaseResources();
    XDEBUG("VKOffScreenRenderer destroyed.");
}


/// @brief 录制gpu指令
void VKOffScreenRenderer::recordCmds(vk::CommandBuffer& cmdBuf,
                                     uint32_t           frameIndex)
{
    if ( frameIndex >= MAX_FRAMES_IN_FLIGHT ) {
        XERROR("VKOffScreenRenderer: frameIndex out of bounds!");
        return;
    }

    // 1. 设置清除颜色 (Alpha 为 0，确保画布背景透明)
    vk::ClearValue clearValue;
    clearValue.setColor(
        vk::ClearColorValue(std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f }));

    // 获取渲染数据
    const auto& vertices = getVertices();
    const auto& indices  = getIndices();

    if ( vertices.empty() || indices.empty() ) return;

    // --- 动态扩容检查 ---
    size_t neededCount = std::max(vertices.size(), indices.size());
    if ( neededCount > m_lastAllocatedCount ) {
        XWARN(
            "VKOffScreenRenderer: Buffer size insufficient ({} > {}), "
            "reallocating...",
            neededCount,
            m_lastAllocatedCount);

        // 1. 等待 GPU 完成当前工作
        (void)m_device.waitIdle();

        // 2. 增加 50% 冗余防止频繁扩容
        size_t newCount = static_cast<size_t>(neededCount * 1.5f);

        // 3. 释放并重新分配所有并发帧的资源
        m_vertexBuffers.clear();
        m_indexBuffers.clear();
        m_uniformBuffers.clear();

        size_t bufferSize = sizeof(Vertex::VKBasicVertex) * newCount;
        for ( int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
            m_vertexBuffers.push_back(std::make_unique<VKMemBuffer>(
                m_physicalDevice,
                m_device,
                bufferSize,
                vk::BufferUsageFlagBits::eVertexBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent));

            m_indexBuffers.push_back(std::make_unique<VKMemBuffer>(
                m_physicalDevice,
                m_device,
                bufferSize,
                vk::BufferUsageFlagBits::eIndexBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent));

            m_uniformBuffers.push_back(std::make_unique<VKMemBuffer>(
                m_physicalDevice,
                m_device,
                sizeof(float),
                vk::BufferUsageFlagBits::eUniformBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent));
        }

        m_lastAllocatedCount = newCount;
        XDEBUG("VKOffScreenRenderer: Reallocated buffers with capacity: {}",
               newCount);
    }

    uploadUniformBuffer2GPU();

    // 每一帧将 CPU 的顶点数据上传到当前帧的 GPU 缓冲区
    m_vertexBuffers[frameIndex]->uploadData(
        vertices.data(), vertices.size() * sizeof(Vertex::VKBasicVertex));
    if ( !indices.empty() ) {
        m_indexBuffers[frameIndex]->uploadData(
            indices.data(), indices.size() * sizeof(uint32_t));
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
        glm::mat4    ortho = glm::ortho(0.0f,
                                        (float)m_logicalWidth,
                                        0.0f - m_yOffset,
                                        (float)m_logicalHeight - m_yOffset,
                                        -1.0f,
                                        1.0f);
        vk::Viewport viewport(
            0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f);
        vk::Rect2D scissor({ 0, 0 }, { m_width, m_height });
        cmdBuf.setViewport(0, 1, &viewport);
        cmdBuf.setScissor(0, 1, &scissor);

        // 4. 绑定离屏专属管线
        cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                            m_mainBrushRenderPipeline->m_graphicsPipeline);

        // 4. 绑定当前帧的描述符集
        cmdBuf.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
            0,
            1,
            &m_offScreenDescriptorSets[frameIndex],
            0,
            nullptr);

        cmdBuf.pushConstants(
            m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
            vk::ShaderStageFlagBits::eVertex |
                vk::ShaderStageFlagBits::eFragment,
            0,
            sizeof(glm::mat4),
            &ortho);

        // 5. 绑定当前帧的顶点/索引缓冲区
        cmdBuf.bindVertexBuffers(
            0, m_vertexBuffers[frameIndex]->m_vkBuffer, { 0 });
        cmdBuf.bindIndexBuffer(
            m_indexBuffers[frameIndex]->m_vkBuffer, 0, vk::IndexType::eUint32);

        // 6. 解析 DrawCmds 进行批次渲染 (回调到 UI 实现层)
        onRecordDrawCmds(cmdBuf,
                         m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
                         m_mainBrushRenderPipeline->getDescriptorSetLayout(),
                         m_offScreenDescriptorSets[frameIndex],
                         frameIndex);
    }
    cmdBuf.endRenderPass();

    // 7. 发光特效：如果有 glowShader 且启用了光效，进行多轮渲染
    if ( m_glowBrushRenderPipeline && m_blurRenderPipeline &&
         m_compositeRenderPipeline ) {
        // --- 7.1. 渲染发光几何体到 m_glowFramebuffer ---
        rpBegin.setFramebuffer(m_glowFramebuffer);
        cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
        {
            glm::mat4    ortho = glm::ortho(0.0f,
                                            (float)m_logicalWidth,
                                            0.0f - m_yOffset,
                                            (float)m_logicalHeight - m_yOffset,
                                            -1.0f,
                                            1.0f);
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
                &m_offScreenDescriptorSets[frameIndex],
                0,
                nullptr);

            cmdBuf.pushConstants(
                m_glowBrushRenderPipeline->m_graphicsPipelineLayout,
                vk::ShaderStageFlagBits::eVertex |
                    vk::ShaderStageFlagBits::eFragment,
                0,
                sizeof(glm::mat4),
                &ortho);

            cmdBuf.bindVertexBuffers(
                0, m_vertexBuffers[frameIndex]->m_vkBuffer, { 0 });
            cmdBuf.bindIndexBuffer(m_indexBuffers[frameIndex]->m_vkBuffer,
                                   0,
                                   vk::IndexType::eUint32);

            // 绘制发光几何体
            onRecordGlowCmds(
                cmdBuf,
                m_glowBrushRenderPipeline->m_graphicsPipelineLayout,
                m_glowBrushRenderPipeline->getDescriptorSetLayout(),
                m_offScreenDescriptorSets[frameIndex],
                frameIndex);
        }
        cmdBuf.endRenderPass();

        // --- 7.2. Ping-Pong 模糊渲染 ---
        int passes = MMM::Config::SkinManager::instance().getGlowPasses();
        if ( passes > 0 ) {
            // 设置模糊专用的 RenderPass (不 Clear)
            rpBegin.setRenderPass(m_blurRenderPass->getRenderPass());

            bool ping = true;
            for ( int i = 0; i < passes; ++i ) {
                vk::Framebuffer currentFb =
                    ping ? m_pingFramebuffer : m_pongFramebuffer;
                vk::DescriptorSet currentSampleSet =
                    (i == 0) ? m_glowDescriptorSets[frameIndex]
                             : (ping ? m_pongDescriptorSets[frameIndex]
                                     : m_pingDescriptorSets[frameIndex]);

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
                    pc.dirAndPasses = glm::vec4(ping ? 1.0f / m_width : 0.0f,
                                                ping ? 0.0f : 1.0f / m_height,
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
                    ping ? m_pongDescriptorSets[frameIndex]
                         : m_pingDescriptorSets[frameIndex];
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
                float intensity =
                    MMM::Config::SkinManager::instance().getGlowIntensity();
                pc.dirAndPasses = glm::vec4(0.0f, 0.0f, 0.0f, intensity);

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
