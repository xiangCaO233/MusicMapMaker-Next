#include "graphic/imguivk/VKRenderer.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "graphic/imguivk/mem/VKUniforms.h"
#include "graphic/imguivk/mesh/VKVertex.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"
#include <chrono>

namespace MMM::Graphic
{

/**
 * @brief 构造函数，初始化渲染所需的同步对象和命令资源
 *
 * @param vkPhysicalDevice 物理设备引用 (用于创建内存缓冲区)
 * @param logicalDevice 逻辑设备引用
 * @param swapchain 交换链引用
 * @param pipeline 渲染管线引用
 * @param renderPass 渲染流程引用
 * @param logicDeviceGraphicsQueue 图形队列引用
 * @param logicDevicePresentQueue 呈现队列引用
 */
VKRenderer::VKRenderer(vk::PhysicalDevice& vkPhysicalDevice,
                       vk::Device& logicalDevice, VKSwapchain& swapchain,
                       VKRenderPipeline& pipeline, VKRenderPass& renderPass,
                       vk::Queue& logicDeviceGraphicsQueue,
                       vk::Queue& logicDevicePresentQueue)
    : m_vkLogicalDevice(logicalDevice)
    , m_vkRenderPass(renderPass)
    , m_vkSwapChain(swapchain)
    , m_vkRenderPipeline(pipeline)
    , m_LogicDeviceGraphicsQueue(logicDeviceGraphicsQueue)
    , m_LogicDevicePresentQueue(logicDevicePresentQueue)
{
    m_avalableImageBufferCount = swapchain.m_vkImageBuffers.size();
    XINFO("Available Image Buffer Count:{}.", m_avalableImageBufferCount);

    XINFO("Max In Flight Frame Count set to:{}.", MAX_FRAMES_IN_FLIGHT);

    // 创建命令池
    createCommandPool();

    // 创建命令缓冲区
    allocateCommandBuffers();

    // 创建信号量和栅栏
    createSemsWithFences();

    // 创建内存缓冲区
    createMemBuffers(vkPhysicalDevice);

    // 上传顶点数据到gpu
    uploadVertexBuffer2GPU();

    // 创建描述符池
    createDescriptPool();

    // 创建描述符集
    createDescriptSets();

    // 映射uniform描述符集
    mapUniformBuffer2DescriptorSet();
}

VKRenderer::~VKRenderer()
{
    // 等待设备空闲，确保不再使用任何资源
    m_vkLogicalDevice.waitIdle();

    // 释放描述符池
    // 描述符集会随描述符池一同销毁，不必再手动销毁
    m_vkLogicalDevice.destroyDescriptorPool(m_vkDescriptorPool);
    XINFO("Destroyed Descriptor Pool.");

    // 释放VK内存缓冲区
    m_vkHostMemBuffer.reset();
    m_vkGPUMemBuffer.reset();

    for ( auto& cmdAvailableFence : m_cmdAvailableFences ) {
        m_vkLogicalDevice.destroyFence(cmdAvailableFence);
    }
    XINFO("Destroyed cmd Sync Fences.");

    for ( auto& imageAvailableSem : m_imageAvailableSems ) {
        m_vkLogicalDevice.destroySemaphore(imageAvailableSem);
    }
    for ( auto& renderFinishedSem : m_renderFinishedSems ) {
        m_vkLogicalDevice.destroySemaphore(renderFinishedSem);
    }
    XINFO("Destroyed All image Semaphores.");

    // 分配的命令缓冲区会自动随pool一起释放
    // for ( auto& vkCommandBuffer : m_vkCommandBuffers ) {
    //     m_vkLogicalDevice.freeCommandBuffers(m_vkCommandPool,
    //     vkCommandBuffer);
    // }
    // XINFO("Destroyed VK Command Buffers.");

    m_vkLogicalDevice.destroyCommandPool(m_vkCommandPool);
    XINFO("Destroyed VK Command Pool.");
}

/**
 * @brief 执行单帧渲染
 *
 * 包含等待 Fence、获取图像、录制命令、提交队列、呈现图像等步骤。
 */
void VKRenderer::render(NativeWindow& window)
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
    static std::array clear_color{ .23f, .23f, .23f, 1.f };
    // --- 1. ImGui 准备新帧 ---
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // 2. Show a simple window that we create ourselves. We use a Begin/End pair
    // to create a named window.
    {
        ImGuiIO&     io      = ImGui::GetIO();
        static float f       = 0.0f;
        static int   counter = 0;

        ImGui::Begin("Hello, world!");  // Create a window called "Hello,
                                        // world!" and append into it.

        ImGui::Text("This is some useful text.");  // Display some text (you can
                                                   // use a format strings too)

        ImGui::SliderFloat(
            "float",
            &f,
            0.0f,
            1.0f);  // Edit 1 float using a slider from 0.0f to 1.0f
        ImGui::ColorEdit4(
            "clear color",
            clear_color.data());  // Edit 4 floats representing a color

        if ( ImGui::Button(
                 "Button") )  // Buttons return true when clicked (most widgets
                              // return true when edited/activated)
            counter++;
        ImGui::SameLine();
        ImGui::Text("counter = %d", counter);

        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                    1000.0f / io.Framerate,
                    io.Framerate);
        ImGui::End();
    }

    ImGui::ShowDemoWindow();  // 测试用，显示ImGui默认演示窗口
    ImGui::Render();          // 生成绘制数据

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
    vk::ClearColorValue clearColorValue(clear_color);
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

void VKRenderer::triggerRecreate(NativeWindow& window)
{
    int w, h;
    window.getFramebufferSize(w, h);
    // 这里需要调用 context 的 recreateSwapchain
    // 你可以通过回调或者单例模式来调用
    MMM::Graphic::VKContext::get().value().get().recreateSwapchain(
        window.getWindowHandle(), w, h);
    window.resetFramebufferResized();
}

/**
 * @brief 传输数据到GPU
 */
void VKRenderer::uploadBuffer2GPU(
    vk::CommandBuffer&                  cmdBuffer,
    const std::unique_ptr<VKMemBuffer>& hostBuffer,
    const std::unique_ptr<VKMemBuffer>& gpuBuffer) const
{
    vk::CommandBufferBeginInfo uploadCmdBufBeginInfo;

    // 只用一次
    uploadCmdBufBeginInfo.setFlags(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    cmdBuffer.begin(uploadCmdBufBeginInfo);
    {
        vk::BufferCopy bufferCopyRegion;
        bufferCopyRegion
            // 如名
            .setSize(hostBuffer->m_bufSize)
            .setSrcOffset(0)
            .setDstOffset(0);
        cmdBuffer.copyBuffer(
            // 起始缓冲区
            hostBuffer->m_vkBuffer,
            // 目标缓冲区
            gpuBuffer->m_vkBuffer,
            // 拷贝区域
            bufferCopyRegion);
    }
    cmdBuffer.end();

    // 上传信息 - 无需信号量
    vk::SubmitInfo submitInfo;
    submitInfo.setCommandBuffers(cmdBuffer);

    // 上传命令
    m_LogicDeviceGraphicsQueue.submit(submitInfo);

    // 等待上传完毕
    m_vkLogicalDevice.waitIdle();
}

/**
 * @brief 上传顶点缓冲区到GPU
 */
void VKRenderer::uploadVertexBuffer2GPU() const
{
    // 为每一个图像缓冲分配一个命令缓冲区
    vk::CommandBufferAllocateInfo commandBufferAllocateInfo;

    // 写入数据到主机内存缓冲区
    void* data = m_vkLogicalDevice.mapMemory(
        m_vkHostMemBuffer->m_vkDevMem, 0, sizeof(s_vertices));
    memcpy(data, s_vertices.data(), sizeof(s_vertices));
    m_vkLogicalDevice.unmapMemory(m_vkHostMemBuffer->m_vkDevMem);

    // 传输主机内存缓冲区到GPU内存缓冲区
    // 需要专门分配一个命令缓冲区用于上传
    commandBufferAllocateInfo
        // 要从哪个命令池分配
        .setCommandPool(m_vkCommandPool)
        // 分配1个缓冲区
        .setCommandBufferCount(1)
        .setLevel(vk::CommandBufferLevel::ePrimary);
    vk::CommandBuffer uploadMemCmdBuffer =
        m_vkLogicalDevice.allocateCommandBuffers(commandBufferAllocateInfo)[0];

    // 传输到对应gpu缓冲区
    uploadBuffer2GPU(uploadMemCmdBuffer, m_vkHostMemBuffer, m_vkGPUMemBuffer);

    // 销毁缓冲区
    m_vkLogicalDevice.freeCommandBuffers(m_vkCommandPool, uploadMemCmdBuffer);

    XINFO("Uploaded vertex data to gpu buffer.");
}

/**
 * @brief 上传uniform缓冲区到GPU
 */
void VKRenderer::uploadUniformBuffer2GPU(const uint32_t current_image_index)
{
    // 获取当前image对应的缓冲区
    auto& current_frame_host_uniform_buffer =
        m_vkHostUniformMemBuffers[current_image_index];
    auto& current_frame_gpu_uniform_buffer =
        m_vkGPUUniformMemBuffers[current_image_index];
    auto& uniformUploadCmdBuffer =
        m_uniformUploadCmdBuffers[current_image_index];

    // 映射获取主机uniform缓冲区内存地址
    auto data_ptr = m_vkLogicalDevice.mapMemory(
        current_frame_host_uniform_buffer->m_vkDevMem,
        0,
        current_frame_host_uniform_buffer->m_bufSize);

    // 写入uniform到主机uniform缓冲区
    memcpy(data_ptr, &m_testCurrentTime, sizeof(m_testCurrentTime));

    // 结束映射
    m_vkLogicalDevice.unmapMemory(
        current_frame_host_uniform_buffer->m_vkDevMem);

    // 传输到gpu对应位置
    uploadBuffer2GPU(uniformUploadCmdBuffer,
                     current_frame_host_uniform_buffer,
                     current_frame_gpu_uniform_buffer);

    // XINFO("Uploaded uniform data to gpu buffer.");
}

}  // namespace MMM::Graphic
