#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"
#include "ui/IRenderableView.h"
#include "ui/UIManager.h"

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
    // 检查窗口是否完成了缩放操作（消抖）
    if ( window.shouldRecreate() ) {
        m_vkSwapChain.markDirty();
    }

    // 只判断标志位（极快的布尔值检查）
    if ( m_vkSwapChain.needsRecreate() ) {
        // 额外检查：如果是最小化，宽和高为 0，此时不应重建，直接跳过
        int w, h;
        window.getFramebufferSize(w, h);
        if ( w == 0 || h == 0 ) return;

        triggerRecreate(
            window);  // 内部重建完后记得调用 m_vkSwapChain.checkAndResetDirty()
        return;
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

    // 在录制Imgui的UI前先检查需不需要重建帧缓冲 ★★★
    for ( auto* uiManager : uiManagers ) {
        // 在 UIManager 获取到所有注册可再渲染View
        for ( auto renderableView : uiManager->getRenderableViews() ) {
            // 如果离屏渲染器需要重建（初次或改名）
            if ( renderableView->needReCreateFrameBuffer() ) {
                renderableView->reCreateFrameBuffer(m_vkPhysicalDevice,
                                                    m_vkLogicalDevice,
                                                    m_vkSwapChain,
                                                    m_vkCommandPool,
                                                    m_LogicDeviceGraphicsQueue);
            }
        }
    }

    // 录制所有Imgui的ui布局
    for ( UI::UIManager* uiManager : uiManagers ) {
        uiManager->updateAllUIs();
    }

    if ( m_cursorManager ) {
        m_cursorManager->UpdateAndDraw();
    }

    ImGui::Render();  // 生成imgui绘制顶点数据

    // 请求下一个可绘制的图像 - 查到的同时发出图像可用信号量
    auto imageResult = m_vkLogicalDevice.acquireNextImageKHR(
        m_vkSwapChain.m_swapchain,
        std::numeric_limits<uint64_t>::max(),
        m_imageAvailableSems[m_currentFrameIndex]);

    // if ( imageResult.result == vk::Result::eErrorOutOfDateKHR ) {
    //     // 先结束 ImGui 帧
    //     ImGui::EndFrame();
    //     // 标记需要重建
    //     triggerRecreate(window);
    //     return;
    // }

    if ( imageResult.result != vk::Result::eSuccess ) {
        XWARN("acquire ImageKHR failed");
    }
    // 获取到实际查询到的可绘制的图像下标
    auto imageIndex = imageResult.value;

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
    std::array<vk::ClearValue, 2> clearValues;
    vk::ClearColorValue           clearColorValue(s_clear_color);
    clearValues[0].setColor(clearColorValue);
    clearValues[1].setDepthStencil({ 1.0f, 0 });

    // 命令录制
    currentCmdBuffer.begin(commandBufferBeginInfo);
    // 在主 RenderPass 开始前，执行离屏渲染录制
    for ( auto* uiManager : uiManagers ) {
        // 在 UIManager 获取到所有注册可再渲染View
        for ( auto renderableView : uiManager->getRenderableViews() ) {
            // 录制离屏绘制指令到当前命令缓冲
            renderableView->recordCmds(currentCmdBuffer, renderableView);
        }
    }
    {
        vk::Rect2D renderArea;
        renderArea = { { 0,
                         0,
                         swapchainCreateInfo.imageExtent.width,
                         swapchainCreateInfo.imageExtent.height } };

        // 开始主屏幕渲染流程
        vk::RenderPassBeginInfo renderPassBeginInfo;
        renderPassBeginInfo
            // 设置渲染流程
            .setRenderPass(m_vkRenderPass.getRenderPass())
            // 设置渲染区域
            .setRenderArea(renderArea)
            // 设置要绘制到哪个帧缓冲上(上面查到了索引直接用)
            .setFramebuffer(
                m_vkSwapChain.m_vkImageBuffers[imageIndex].vk_frameBuffer)
            // clearmask - 类似opengl的清屏颜色
            .setClearValues(clearValues);

        // 真 - 命令录制
        currentCmdBuffer.beginRenderPass(renderPassBeginInfo, {});
        {
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
        if ( presentResult == vk::Result::eSuboptimalKHR ) {
            // triggerRecreate(window);
            XWARN("Size Missed");
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
