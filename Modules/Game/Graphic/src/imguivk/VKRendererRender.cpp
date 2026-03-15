#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "graphic/imguivk/mem/VKUniforms.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"
#include "ui/UIManager.h"
#include <chrono>

namespace MMM::Graphic
{
/**
 * @brief 执行单帧渲染
 *
 * 包含等待 Fence、获取图像、录制命令、提交队列、呈现图像等步骤。
 */
void VKRenderer::render(NativeWindow&               window,
                        std::vector<UI::UIManager*> uiManagers)
{
    // 1. 主动检查 GLFW 报告的当前尺寸
    int currentWidth, currentHeight;
    window.getFramebufferSize(currentWidth, currentHeight);

    // 2. 检查当前交换链的 Extent 是否与窗口一致
    auto& extent = m_vkSwapChain.info().imageExtent;

    // 如果 flag 被触发，或者尺寸实际对不上，就强制重建
    if ( window.isFramebufferResized() || currentWidth != extent.width ||
         currentHeight != extent.height ) {
        triggerRecreate(window);
        return;  // 重建后这一帧直接跳过，下一帧再画
    }

    // 等待cmd完成
    auto waitResult = m_vkLogicalDevice.waitForFences(
        m_cmdAvailableFences[m_currentFrameIndex],
        true,
        std::numeric_limits<uint64_t>::max());
    if ( waitResult != vk::Result::eSuccess ) {
        XWARN("VK Device WaitForFences failed");
    }

    // 恢复fence
    m_vkLogicalDevice.resetFences(m_cmdAvailableFences[m_currentFrameIndex]);

    // --- 1. ImGui 准备新帧 ---
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    for ( UI::UIManager* uiManager : uiManagers ) {
        uiManager->updateAllUIs();
    }

    ImGui::Render();  // 生成绘制数据

    // 请求下一个可绘制的图像 - 查到的同时发出图像可用信号量
    auto imageResult = m_vkLogicalDevice.acquireNextImageKHR(
        m_vkSwapChain.m_swapchain,
        std::numeric_limits<uint64_t>::max(),
        m_imageAvailableSems[m_currentFrameIndex]);
    if ( imageResult.result == vk::Result::eErrorOutOfDateKHR ) {

        // 标记需要重建
        triggerRecreate(window);

        return;
    }
    if ( imageResult.result != vk::Result::eSuccess ) {
        XWARN("acquire ImageKHR failed");
    }
    // 获取到实际查询到的可绘制的图像下标
    auto imageIndex = imageResult.value;

    // 获取当前时间
    static auto start_time =
        std::chrono::high_resolution_clock::now().time_since_epoch();
    auto since_start =
        std::chrono::high_resolution_clock::now().time_since_epoch() -
        start_time;
    auto current_time_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(since_start);
    m_testCurrentTime.time =
        static_cast<float>(current_time_ms.count()) / 1000.f;
    // XINFO("current time: {}", m_testCurrentTime.time);
    // 上传uniform数据
    uploadUniformBuffer2GPU(imageIndex);

    // 重置命令缓冲
    auto& currentCmdBuffer = m_vkCommandBuffers[m_currentFrameIndex];
    currentCmdBuffer.reset();

    // 准备开始输入命令
    vk::CommandBufferBeginInfo commandBufferBeginInfo;
    commandBufferBeginInfo
        // 设置用法
        // 只提交一次,提交完就不用了
        .setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    // 生命周期覆盖整个渲染流程
    // .setFlags(vk::CommandBufferUsageFlagBits::eRenderPassContinue);
    // 无限复用
    // .setFlags(vk::CommandBufferUsageFlagBits::eSimultaneousUse);

    // 渲染区域
    auto swapchainCreateInfo = m_vkSwapChain.info();
    // clearmask - 类似opengl的清屏颜色
    vk::ClearValue      clearValue;
    vk::ClearColorValue clearColorValue(s_clear_color);
    clearValue.setColor(clearColorValue);

    // 命令录制
    currentCmdBuffer.begin(commandBufferBeginInfo);
    {
        vk::Rect2D renderArea;
        renderArea = { { 0,
                         0,
                         swapchainCreateInfo.imageExtent.width,
                         swapchainCreateInfo.imageExtent.height } };

        // 绑定渲染管线
        currentCmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                      m_vkRenderPipeline.m_graphicsPipeline);

        // 绑定描述符集
        currentCmdBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            m_vkRenderPipeline.m_graphicsPipelineLayout,
            0,
            m_vkDescriptorSets[imageIndex],
            {});

        // 开始渲染流程
        vk::RenderPassBeginInfo renderPassBeginInfo;
        renderPassBeginInfo
            // 设置渲染流程
            .setRenderPass(m_vkRenderPass.m_graphicRenderPass)
            // 设置渲染区域
            .setRenderArea(renderArea)
            // 设置要绘制到哪个帧缓冲上(上面查到了索引直接用)
            .setFramebuffer(
                m_vkSwapChain.m_vkImageBuffers[imageIndex].vk_frameBuffer)
            // clearmask - 类似opengl的清屏颜色
            .setClearValues(clearValue);

        // 真 - 命令录制
        currentCmdBuffer.beginRenderPass(renderPassBeginInfo, {});
        {
            // 绑定顶点缓冲区
            currentCmdBuffer.bindVertexBuffers(
                // 多个buffer时要绑定第几个
                0,
                // 使用GPU内存缓冲区
                // 可以传入多个buffer
                m_vkGPUMemBuffer->m_vkBuffer,
                // 偏移量
                { 0 });
            currentCmdBuffer.draw(
                // 绘制几个顶点
                3
                // 绘制几个实例
                ,
                1
                // 从第几个顶点开始
                ,
                0
                // 从第几个实例开始
                ,
                0);
            // 在此处绘制 ImGui (在 3D 之后画，从而覆盖在上面)
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                            currentCmdBuffer);
        }
        // 结束渲染流程
        currentCmdBuffer.endRenderPass();
    }
    currentCmdBuffer.end();  // 结束命令录制

    // 准备等待的阶段掩码
    // 这表示：在流水线的“颜色附件输出”阶段等待信号量
    // 也就是说，可以在图像还没准备好时就开始执行顶点着色器，
    // 但必须等到图像准备好了才能写入颜色。
    vk::PipelineStageFlags waitStages[] = {
        vk::PipelineStageFlagBits::eColorAttachmentOutput
    };
    // 发送命令到gpu执行绘制 - 通过图形渲染队列
    vk::SubmitInfo submitInfo;
    submitInfo
        // 设置命令缓冲区
        .setCommandBuffers(currentCmdBuffer)
        // 等待信号量
        .setWaitSemaphores(m_imageAvailableSems[m_currentFrameIndex])
        // 设置等待的阶段掩码
        .setWaitDstStageMask(waitStages)
        // 发出信号量
        .setSignalSemaphores(m_renderFinishedSems[imageIndex]);
    m_LogicDeviceGraphicsQueue.submit(
        submitInfo, m_cmdAvailableFences[m_currentFrameIndex]);

    // 呈现
    vk::PresentInfoKHR presentInfo;
    presentInfo
        // 呈现哪张图像?上面拿到的
        .setImageIndices(imageIndex)
        // 设置交换链
        .setSwapchains(m_vkSwapChain.m_swapchain)
        // 等待信号量
        .setWaitSemaphores(m_renderFinishedSems[imageIndex]);
    auto presentResult = m_LogicDevicePresentQueue.presentKHR(presentInfo);

    if ( presentResult != vk::Result::eSuccess ) {
        if ( presentResult == vk::Result::eSuboptimalKHR ||
             window.isFramebufferResized() ) {
            triggerRecreate(window);
        } else {
            XWARN("Present failed");
        }
    }

    // 并发帧数步进
    ++m_currentFrameIndex %= MAX_FRAMES_IN_FLIGHT;

    // 更新并渲染所有的多视口 (Viewports)
    ImGuiIO& io = ImGui::GetIO();
    if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

}  // namespace MMM::Graphic
