#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "config/skin/SkinConfig.h"
#include "graphic/imguivk/VKTexture.h"
#include "log/colorful-log.h"
#include "vulkan/vulkan.hpp"
#include <glm/ext.hpp>

namespace MMM::Graphic
{

/// @brief 选择预先创建且布局兼容的主画布混合管线。
///
/// 普通 Alpha 与加法管线共享 descriptor set layout、push constant 范围和顶点
/// 格式，切换时无需重新绑定纹理或几何缓冲。调用方保证两条管线已经创建成功。
///
/// @param cmdBuf 当前帧正在录制的离屏命令缓冲。
/// @param additive 为 true 时选择预乘加法混合，否则选择常规透明混合。
/// @warning 录制热路径仅绑定句柄，不进行资源分配或 GPU 同步。
void VKOffScreenRenderer::bindMainBlendPipeline(vk::CommandBuffer& cmdBuf,
                                                bool               additive)
{
    // 只选择已有 owning pointer，不复制共享所有权或创建 Vulkan 对象。
    const auto& pipeline =
        additive ? m_additiveBrushRenderPipeline : m_mainBrushRenderPipeline;
    cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                        pipeline->m_graphicsPipeline);
}
/// @brief 创建尚未绑定 Vulkan device 和 framebuffer 的离屏渲染器基类。
///
/// 实际资源由 reCreateFrameBuffer 在派生视图取得尺寸与 shader 路径后建立，因此
/// 默认构造保持所有句柄为空，recordCmds 会据 resourcesReady 安全跳过。
VKOffScreenRenderer::VKOffScreenRenderer() {}

/// @brief 等待离屏工作完成并按资源依赖顺序释放全部 Vulkan 对象。
/// @warning 析构低频路径：device 已绑定时执行 waitIdle，调用方必须停止提交新的
/// 离屏命令。
VKOffScreenRenderer::~VKOffScreenRenderer()
{
    // releaseResources 销毁 framebuffer、image、pipeline 与
    // descriptor；它们可能 仍被在途命令引用，因此有 device 时先建立完整 idle
    // 边界。
    if ( m_device ) (void)m_device.waitIdle();

    releaseResources();
    XDEBUG("VKOffScreenRenderer destroyed.");
}
/// @brief 把普通画布、可选发光后处理及最终覆盖层录入当前帧命令缓冲。
///
/// 普通层始终先清空透明目标并绘制当前几何。存在发光命令时，几何先写入低分辨率
/// glow 目标，再经 ping-pong 全屏模糊，最后加法合成回主图像；覆盖层在所有后
/// 处理之后追加。空几何仍执行一次清除 pass，防止沿用上一帧图像。
///
/// 顶点、索引、descriptor 和 uniform 都按 frameIndex 分槽。容量不足时现有实现会
/// 等待 device idle 并一次性扩展所有帧槽，这必须保持为低频高水位路径。
///
/// 本函数不 begin/end 外层 command buffer，也不自行 submit；主渲染器负责把串行
/// 录制内容或独立离屏 command buffer 按正确顺序提交。每个内部 render pass 都在
/// 返回前闭合，调用方可以随后继续录制主交换链 pass。
///
/// @param cmdBuf 当前帧独占且已 begin 的 primary command buffer。
/// @param frameIndex 当前并发帧槽索引。
/// @warning 热路径：每帧离屏命令录制时执行；扩容分支会 waitIdle 并阻塞
/// GPU，必须保持为容量不足时的低频路径。禁止文件 I/O、异常和常态分配。
void VKOffScreenRenderer::recordCmds(vk::CommandBuffer& cmdBuf,
                                     uint32_t           frameIndex)
{
    // 所有 per-frame 容器固定按 MAX_FRAMES_IN_FLIGHT
    // 建立，越界索引没有安全后备。
    if ( frameIndex >= MAX_FRAMES_IN_FLIGHT ) {
        XERROR("VKOffScreenRenderer: frameIndex out of bounds!");
        return;
    }

    // 录制前一次性验证本帧会解引用的核心资源：device、目标 framebuffer、主
    // render pass、两种混合管线，以及四组 per-frame 容器。Glow 资源在真正需要
    // 该分支时另行检查，使禁用发光的基础离屏画布仍可工作。
    const bool resourcesReady =
        m_device && m_framebuffer && m_offScreenRenderPass &&
        m_mainBrushRenderPipeline && m_mainBrushRenderPipeline->isValid() &&
        m_additiveBrushRenderPipeline &&
        m_additiveBrushRenderPipeline->isValid() &&
        frameIndex < m_vertexBuffers.size() &&
        frameIndex < m_indexBuffers.size() &&
        frameIndex < m_uniformBuffers.size() &&
        frameIndex < m_offScreenDescriptorSets.size();
    if ( !resourcesReady ) {
        // 初始化或重建尚未完成时保持命令缓冲不变，主渲染器仍可提交其他任务。
        return;
    }

    // 上传命令必须位于第一个读取这些资源的 render pass 之前；派生类只能记录
    // 命令，不能在此提交或等待队列。
    // 上传回调与下方几何上传共享 frameIndex，所有临时资源都必须落入同一并发帧
    // 槽，不能借用上一帧仍可能在 GPU 侧读取的 staging 或 descriptor 状态。
    onRecordResourceUploads(cmdBuf, frameIndex);

    // 离屏纹理会由 ImGui 与后处理继续合成，清除 Alpha 必须为零以保留透明背景。
    vk::ClearValue clearValue;
    clearValue.setColor(
        vk::ClearColorValue(std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f }));

    // 借用派生类已生成的帧快照，不复制可能很大的顶点与索引数组。
    const auto& vertices            = getVertices();
    const auto& indices             = getIndices();
    const bool  hasDrawableGeometry = !vertices.empty() && !indices.empty();

    if ( !hasDrawableGeometry ) {
        // 空帧仍需进入并结束 render pass，利用 loadOp clear
        // 清除上一帧残留像素并 完成目标图像的布局转换。
        // 此路径不上传空 vector，也不进入容量检查，避免依赖 data() 与零字节传输
        // 在不同缓冲后端上的处理差异。
        vk::RenderPassBeginInfo rpBegin;
        rpBegin.setRenderPass(m_offScreenRenderPass->getRenderPass())
            .setFramebuffer(m_framebuffer)
            .setRenderArea(vk::Rect2D({ 0, 0 }, { m_width, m_height }))
            .setClearValues(clearValue);
        cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
        cmdBuf.endRenderPass();
        return;
    }

    // 顶点与索引共用一个元素容量高水位，取较大者保证两个上传都不会越界。
    size_t neededCount = std::max(vertices.size(), indices.size());
    if ( neededCount > m_lastAllocatedCount ) {
        XWARN(
            "VKOffScreenRenderer: Buffer size insufficient ({} > {}), "
            "reallocating...",
            neededCount,
            m_lastAllocatedCount);

        // 所有帧槽缓冲可能仍被 GPU 使用，清空 owning pointers 前必须全设备
        // idle。
        // 这里选择一次性同步而不是逐缓冲追踪 fence，是因为扩容会同时替换全部
        // 帧槽；正常帧依靠容量高水位完全绕过这条同步路径。
        (void)m_device.waitIdle();

        // 预留 50% 增长空间，连续小幅增加几何时不会每帧重新创建缓冲。
        size_t newCount = static_cast<size_t>(neededCount * 1.5f);

        // 三类 per-frame 缓冲作为一组替换；descriptor 绑定由资源创建流程管理，
        // 本扩容分支只替换缓冲所有者，不在热路径重建 descriptor pool。
        m_vertexBuffers.clear();
        m_indexBuffers.clear();
        m_uniformBuffers.clear();

        // 当前实现以顶点跨度统一分配顶点和索引缓冲，索引缓冲因此可能留有额外
        // 字节，但容量一定不小于 uint32_t 索引需求。
        size_t bufferSize = sizeof(Vertex::VKBasicVertex) * newCount;
        for ( int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
            // HostVisible + HostCoherent 支持每帧 CPU 直接上传，无需显式
            // flush。
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

            // 每帧独立 uniform 缓冲避免 CPU 更新覆盖仍在执行的另一帧数据。
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

    // uniform 内容在任何本帧 draw 前上传；具体 descriptor 是否引用该缓冲由资源
    // 初始化阶段的绑定布局决定。
    uploadUniformBuffer2GPU();

    // 几何快照复制到当前空闲帧槽，上传字节数只覆盖本帧实际元素。
    m_vertexBuffers[frameIndex]->uploadData(
        vertices.data(), vertices.size() * sizeof(Vertex::VKBasicVertex));
    m_indexBuffers[frameIndex]->uploadData(indices.data(),
                                           indices.size() * sizeof(uint32_t));

    // 主离屏 pass 覆盖完整物理 framebuffer，逻辑坐标通过下方正交矩阵映射。
    // RenderPass 的 final layout 允许结束后把主图像作为 ImGui 或后处理采样源。
    vk::RenderPassBeginInfo rpBegin;
    rpBegin.setRenderPass(m_offScreenRenderPass->getRenderPass())
        .setFramebuffer(m_framebuffer)
        .setRenderArea(vk::Rect2D({ 0, 0 }, { m_width, m_height }))
        .setClearValues(clearValue);

    cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
    {
        // 管线把 viewport/scissor
        // 声明为动态状态，每次录制都按当前物理尺寸写入。
        // 正交投影仍使用逻辑尺寸，并把亚帧 yOffset 纳入上下边界。
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

        // 首个批次从常规透明管线开始；派生类可通过 bindMainBlendPipeline 在兼容
        // 管线间切换。
        cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                            m_mainBrushRenderPipeline->m_graphicsPipeline);

        // descriptor 指向当前帧 uniform
        // 与默认白纹理，布局由两条主画布管线共享。
        cmdBuf.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
            0,
            1,
            &m_offScreenDescriptorSets[frameIndex],
            0,
            nullptr);

        // 变换矩阵以 push constant 同时提供顶点和片元阶段，不依赖 uniform
        // 更新。
        cmdBuf.pushConstants(
            m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
            vk::ShaderStageFlagBits::eVertex |
                vk::ShaderStageFlagBits::eFragment,
            0,
            sizeof(glm::mat4),
            &ortho);

        // 所有批次共享本帧整份几何缓冲，通过各 DrawCmd 的 index offset
        // 选择范围。
        // 顶点偏移固定为零，索引类型与 getIndices 的 uint32_t 元素保持一致。
        cmdBuf.bindVertexBuffers(
            0, m_vertexBuffers[frameIndex]->m_vkBuffer, { 0 });
        cmdBuf.bindIndexBuffer(
            m_indexBuffers[frameIndex]->m_vkBuffer, 0, vk::IndexType::eUint32);

        // 主目标使用标准 framebuffer 比例，零覆盖值让 getPhysicalScissor
        // 自行读取 当前目标/逻辑尺寸比。
        m_scissorScaleX = 0.0f;
        m_scissorScaleY = 0.0f;
        // 派生实现只遍历已生成批次并录制 draw，不重新生成或排序逻辑对象。
        onRecordDrawCmds(cmdBuf,
                         m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
                         m_mainBrushRenderPipeline->getDescriptorSetLayout(),
                         m_offScreenDescriptorSets[frameIndex],
                         frameIndex);
    }
    cmdBuf.endRenderPass();

    // 主 pass 结束后，离屏颜色附件已经包含不发光的最终底图。后续效果只能在该
    // 结果上追加，任何可选分支失败都不得再次清除或覆盖已经录制的普通内容。
    // Glow 配置每帧只读取一次，保证几何、模糊次数与最终强度来自同一份皮肤状态。
    const int glowPasses = MMM::Config::SkinManager::instance().getGlowPasses();
    const float glowIntensity =
        MMM::Config::SkinManager::instance().getGlowIntensity();
    if ( hasGlowDrawCmds() && glowPasses > 0 && glowIntensity > 0.0f &&
         m_glowBrushRenderPipeline && m_blurRenderPipeline &&
         m_compositeRenderPipeline && m_glowBrushRenderPipeline->isValid() &&
         m_blurRenderPipeline->isValid() &&
         m_compositeRenderPipeline->isValid() && m_glowWidth > 0 &&
         m_glowHeight > 0 && m_logicalWidth > 0 && m_logicalHeight > 0 ) {
        // 分支同时验证命令、用户配置、三条管线和尺寸，任一资源未就绪都只跳过
        // 可选效果，不影响已经完成的普通画布内容。
        // Framebuffer、descriptor 与 render pass
        // 由同一重建流程成组建立，此处依赖
        // 该内部不变量，不在每帧逐句柄重复验证。
        const vk::Rect2D glowRenderArea({ 0, 0 },
                                        { m_glowWidth, m_glowHeight });
        const vk::Rect2D mainRenderArea({ 0, 0 }, { m_width, m_height });
        // 第一阶段把发光批次单独写入透明 glow 目标，避免普通几何参与模糊。
        // 复用离屏 render pass 表明 glow 图像格式与主离屏颜色附件兼容。
        rpBegin.setRenderPass(m_offScreenRenderPass->getRenderPass())
            .setFramebuffer(m_glowFramebuffer)
            .setRenderArea(glowRenderArea)
            .setClearValues(clearValue);
        cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
        {
            // 逻辑投影与主画布一致，但 viewport/scissor 使用较小的 glow
            // 物理尺寸。
            glm::mat4    ortho = glm::ortho(0.0f,
                                            (float)m_logicalWidth,
                                            0.0f - m_yOffset,
                                            (float)m_logicalHeight - m_yOffset,
                                            -1.0f,
                                            1.0f);
            vk::Viewport viewport(0.0f,
                                  0.0f,
                                  (float)m_glowWidth,
                                  (float)m_glowHeight,
                                  0.0f,
                                  1.0f);
            vk::Rect2D   scissor({ 0, 0 }, { m_glowWidth, m_glowHeight });
            cmdBuf.setViewport(0, 1, &viewport);
            cmdBuf.setScissor(0, 1, &scissor);

            // Glow Brush
            // 管线使用适合遮罩累积的混合状态，descriptor/几何布局仍与
            // 主画布兼容。
            cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                m_glowBrushRenderPipeline->m_graphicsPipeline);

            // 发光几何继续采样当前帧原始纹理，不读取后处理 ping-pong 目标。
            cmdBuf.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics,
                m_glowBrushRenderPipeline->m_graphicsPipelineLayout,
                0,
                1,
                &m_offScreenDescriptorSets[frameIndex],
                0,
                nullptr);

            // 同一逻辑坐标投影保证发光遮罩与主画布几何精确对齐。
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

            // 裁剪命令来自逻辑坐标，录制 Glow 批次期间临时切换为 glow
            // 目标比例。
            // X/Y 独立计算可覆盖非等比 framebuffer 缩放，而不是假设统一 DPI。
            m_scissorScaleX = static_cast<float>(m_glowWidth) /
                              static_cast<float>(m_logicalWidth);
            m_scissorScaleY = static_cast<float>(m_glowHeight) /
                              static_cast<float>(m_logicalHeight);

            // 派生类只录制被标记为发光的 DrawCmd，避免重复绘制普通批次。
            onRecordGlowCmds(
                cmdBuf,
                m_glowBrushRenderPipeline->m_graphicsPipelineLayout,
                m_glowBrushRenderPipeline->getDescriptorSetLayout(),
                m_offScreenDescriptorSets[frameIndex],
                frameIndex);
            // 回调结束立即清除覆盖，后续主尺寸 pass 恢复标准 framebuffer 比例。
            m_scissorScaleX = 0.0f;
            m_scissorScaleY = 0.0f;
        }
        cmdBuf.endRenderPass();

        // 第二阶段交替写
        // ping/pong，上一轮输出始终作为下一轮采样输入，避免在同一
        // 图像上同时读写。
        // 每个 pass 的附件布局转换由 render pass 依赖承担；循环内只交换固定
        // framebuffer 与 descriptor 句柄，不创建 barrier 或临时 Vulkan 对象。
        int passes = glowPasses;
        if ( passes > 0 ) {
            // Blur render pass 不需要保留目标旧内容，全屏三角形会写满
            // renderArea； pass 结束后目标转为下一轮可采样布局。
            rpBegin.setRenderPass(m_blurRenderPass->getRenderPass())
                .setRenderArea(glowRenderArea)
                .setClearValues(clearValue);

            // ping=true 表示本轮写 ping：首轮采样
            // glow，后续采样上轮另一侧结果。
            bool ping = true;
            for ( int i = 0; i < passes; ++i ) {
                // framebuffer 是本轮输出，descriptor set
                // 必须指向不同的输入图像。
                vk::Framebuffer currentFb =
                    ping ? m_pingFramebuffer : m_pongFramebuffer;
                vk::DescriptorSet currentSampleSet =
                    (i == 0) ? m_glowDescriptorSets[frameIndex]
                             : (ping ? m_pongDescriptorSets[frameIndex]
                                     : m_pingDescriptorSets[frameIndex]);
                // 首轮输入固定为原始 glow 遮罩；之后写 ping 时读 pong、写 pong
                // 时读 ping，永不从 currentFb 自身采样。

                // 每轮独立 render pass
                // 负责前一输出转为可采样布局和本轮输出布局。
                rpBegin.setFramebuffer(currentFb);
                cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
                {
                    vk::Viewport viewport(0.0f,
                                          0.0f,
                                          (float)m_glowWidth,
                                          (float)m_glowHeight,
                                          0.0f,
                                          1.0f);
                    vk::Rect2D scissor({ 0, 0 }, { m_glowWidth, m_glowHeight });
                    cmdBuf.setViewport(0, 1, &viewport);
                    cmdBuf.setScissor(0, 1, &scissor);

                    // 全屏管线不使用顶点缓冲，通过 gl_VertexIndex
                    // 生成覆盖三角形。
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

                    // Push constant 保持四个 vec4 的着色器
                    // ABI；当前只有第一向量承载 采样方向，其余 padding
                    // 明确保留布局空间。
                    struct BlurPC {
                        /// @brief xy 保存单个纹素采样步长。
                        glm::vec4 dirAndPasses;
                        /// @brief 保留到着色器声明的固定 push constant 大小。
                        glm::vec4 pad1, pad2, pad3;
                    } pc;
                    // Ping 轮执行水平一个 texel 步长，Pong 轮执行垂直步长。
                    pc.dirAndPasses =
                        glm::vec4(ping ? 1.0f / m_glowWidth : 0.0f,
                                  ping ? 0.0f : 1.0f / m_glowHeight,
                                  0.0f,
                                  0.0f);

                    cmdBuf.pushConstants(
                        m_blurRenderPipeline->m_graphicsPipelineLayout,
                        vk::ShaderStageFlagBits::eVertex |
                            vk::ShaderStageFlagBits::eFragment,
                        0,
                        sizeof(BlurPC),
                        &pc);

                    // 三顶点全屏三角形避免四边形接缝和索引缓冲绑定。
                    cmdBuf.draw(3, 1, 0, 0);
                }
                cmdBuf.endRenderPass();

                // 翻转后 ping
                // 表示下一轮输出侧，也用于循环结束推导最终结果所在侧。
                ping = !ping;
            }

            // 第三阶段以 load 语义回到主
            // framebuffer，把最终模糊纹理加法合成在普通 画布之上。
            // 该 pass 只绘制最终全屏结果，不再次提交普通或 glow 几何批次。
            rpBegin.setRenderPass(m_compositeRenderPass->getRenderPass())
                .setFramebuffer(m_framebuffer)
                .setRenderArea(mainRenderArea)
                .setClearValues(clearValue);
            cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
            {
                vk::Viewport viewport(
                    0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f);
                vk::Rect2D scissor({ 0, 0 }, { m_width, m_height });
                cmdBuf.setViewport(0, 1, &viewport);
                cmdBuf.setScissor(0, 1, &scissor);

                // Composite 管线使用带源 Alpha
                // 的加法混合，不清除已存在的主内容。
                cmdBuf.bindPipeline(
                    vk::PipelineBindPoint::eGraphics,
                    m_compositeRenderPipeline->m_graphicsPipeline);

                // 循环末尾 ping
                // 已翻转，最终写入位于“下一轮将作为输入”的另一侧。
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

                // 复用相同 push constant ABI，w 分量在 composite shader
                // 中解释为 用户配置的发光强度。
                struct BlurPC {
                    /// @brief composite shader 从 w 分量读取最终发光强度。
                    glm::vec4 dirAndPasses;
                    /// @brief 与 blur 管线共享 push constant 范围的填充字段。
                    glm::vec4 pad1, pad2, pad3;
                } pc;
                // 局部值在 pushConstants 调用完成前有效，不跨命令录制保存指针。
                float intensity = glowIntensity;
                pc.dirAndPasses = glm::vec4(0.0f, 0.0f, 0.0f, intensity);

                cmdBuf.pushConstants(
                    m_compositeRenderPipeline->m_graphicsPipelineLayout,
                    vk::ShaderStageFlagBits::eVertex |
                        vk::ShaderStageFlagBits::eFragment,
                    0,
                    sizeof(BlurPC),
                    &pc);

                // 全屏三角形把低分辨率模糊结果缩放覆盖整个主画布。
                cmdBuf.draw(3, 1, 0, 0);
            }
            cmdBuf.endRenderPass();
        }
    }

    // 覆盖层用于重叠检测等必须位于发光之上的视觉结果；使用 composite pass 保留
    // 已有主颜色，不重新清屏。
    // 即使 glow 因配置关闭或资源未就绪而跳过，覆盖层仍独立判断并追加到普通层；
    // 两个可选阶段之间不存在控制流依赖。
    if ( hasOverlayDrawCmds() && m_compositeRenderPass &&
         m_mainBrushRenderPipeline && m_width > 0 && m_height > 0 &&
         m_logicalWidth > 0 && m_logicalHeight > 0 ) {
        // 覆盖层回到主物理尺寸和逻辑投影，资源未就绪时只跳过可选层。
        const vk::Rect2D overlayRenderArea({ 0, 0 }, { m_width, m_height });
        rpBegin.setRenderPass(m_compositeRenderPass->getRenderPass())
            .setFramebuffer(m_framebuffer)
            .setRenderArea(overlayRenderArea)
            .setClearValues(clearValue);
        // composite render pass 的 loadOp 保留主附件；clearValue 仅满足统一的
        // begin 信息结构，不会在本阶段擦除普通层或已经合成的 glow 像素。
        cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
        {
            // 与普通层使用相同投影，确保诊断轮廓精确覆盖目标几何。
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

            // 覆盖批次复用主透明管线与 descriptor 布局，不需要独立资源集合。
            cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                m_mainBrushRenderPipeline->m_graphicsPipeline);

            // 当前帧默认 descriptor 与普通层一致，派生 DrawCmd
            // 仍可按批次切换纹理。
            cmdBuf.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics,
                m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
                0,
                1,
                &m_offScreenDescriptorSets[frameIndex],
                0,
                nullptr);

            // 推送与主层相同的逻辑到物理变换，覆盖层不继承 glow 缩放。
            cmdBuf.pushConstants(
                m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
                vk::ShaderStageFlagBits::eVertex |
                    vk::ShaderStageFlagBits::eFragment,
                0,
                sizeof(glm::mat4),
                &ortho);

            // 几何继续引用本帧上传的统一顶点/索引快照，覆盖命令只选择不同范围。
            cmdBuf.bindVertexBuffers(
                0, m_vertexBuffers[frameIndex]->m_vkBuffer, { 0 });
            cmdBuf.bindIndexBuffer(m_indexBuffers[frameIndex]->m_vkBuffer,
                                   0,
                                   vk::IndexType::eUint32);

            // 显式恢复标准裁剪比例，防御前一 glow 回调异常遗留的临时缩放状态。
            m_scissorScaleX = 0.0f;
            m_scissorScaleY = 0.0f;
            // 回调只录制已准备覆盖批次；顺序位于 composite
            // 后是本函数的核心保证。
            // 回调返回后不再允许写入此 pass，随后立即闭合 render pass，让主
            // 渲染器可以把最终离屏图像作为只读纹理继续使用。
            onRecordOverlayCmds(
                cmdBuf,
                m_mainBrushRenderPipeline->m_graphicsPipelineLayout,
                m_mainBrushRenderPipeline->getDescriptorSetLayout(),
                m_offScreenDescriptorSets[frameIndex],
                frameIndex);
        }
        cmdBuf.endRenderPass();
    }
}

}  // namespace MMM::Graphic
