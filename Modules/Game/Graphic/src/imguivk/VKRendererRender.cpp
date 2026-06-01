#include "config/AppConfig.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/IGraphicUserHook.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ice/thread/ThreadPool.hpp>
#include <latch>

#ifdef _WIN32
#    define GLFW_EXPOSE_NATIVE_WIN32
#    include <GLFW/glfw3native.h>
#    include <dwmapi.h>

#    ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#        define DWMWA_WINDOW_CORNER_PREFERENCE 33
#    endif
#    ifndef DWMWCP_ROUND
#        define DWMWCP_ROUND 2
#    endif
#endif

namespace MMM::Graphic
{
namespace
{
/// @brief 渲染性能统计使用的单调时钟。
using RenderProfileClock = std::chrono::steady_clock;

/// @brief 渲染性能统计日志输出间隔。
constexpr auto RENDER_PROFILE_LOG_INTERVAL = std::chrono::seconds(2);

/// @brief 单个渲染阶段在统计窗口内的累计耗时。
struct RenderStageStat {
    /// @brief 阶段累计耗时，单位为毫秒。
    double totalMs{ 0.0 };

    /// @brief 阶段单帧最大耗时，单位为毫秒。
    double maxMs{ 0.0 };

    /// @brief 追加一次阶段耗时。
    /// @param elapsedMs 本帧该阶段耗时，单位为毫秒。
    /// @warning 渲染热路径：每帧执行，只能做常量时间浮点累加。
    void add(double elapsedMs)
    {
        totalMs += elapsedMs;
        maxMs = std::max(maxMs, elapsedMs);
    }

    /// @brief 获取统计窗口内的平均耗时。
    /// @param frameCount 统计窗口内累计的完整帧数。
    /// @return 平均耗时，单位为毫秒。
    double average(uint64_t frameCount) const
    {
        return frameCount == 0 ? 0.0
                               : totalMs / static_cast<double>(frameCount);
    }

    /// @brief 清空阶段统计数据。
    void reset()
    {
        totalMs = 0.0;
        maxMs   = 0.0;
    }
};

/// @brief 渲染主循环的分阶段累计性能统计。
struct RenderProfileAccumulator {
    /// @brief 当前统计窗口的起始时间。
    RenderProfileClock::time_point windowStart{ RenderProfileClock::now() };

    /// @brief 当前统计窗口内累计的完整帧数。
    uint64_t frameCount{ 0 };

    /// @brief 单帧总耗时统计。
    RenderStageStat total;

    /// @brief 等待和重置 Fence 的耗时统计。
    RenderStageStat fence;

    /// @brief acquireNextImageKHR 的耗时统计。
    RenderStageStat acquire;

    /// @brief GLFW 事件轮询的耗时统计。
    RenderStageStat pollEvents;

    /// @brief ImGui 新帧准备和光标模式同步的耗时统计。
    RenderStageStat newFrame;

    /// @brief 图形钩子资源准备的耗时统计。
    RenderStageStat prepareResources;

    /// @brief UI 更新和 ImGui 绘制列表生成前逻辑的耗时统计。
    RenderStageStat updateUi;

    /// @brief ImGui::Render 的耗时统计。
    RenderStageStat imguiRender;

    /// @brief 命令缓冲开始录制前准备的耗时统计。
    RenderStageStat commandSetup;

    /// @brief 离屏画布命令录制的耗时统计。
    RenderStageStat offscreenRecord;

    /// @brief 主 RenderPass 与 ImGui Vulkan 绘制命令录制的耗时统计。
    RenderStageStat mainRecord;

    /// @brief 图形队列提交的耗时统计。
    RenderStageStat submit;

    /// @brief presentKHR 的耗时统计。
    RenderStageStat present;

    /// @brief ImGui 多视口平台窗口更新和渲染的耗时统计。
    RenderStageStat platformWindows;

    /// @brief 清空统计窗口。
    /// @param nextStart 下一个统计窗口的起始时间。
    void reset(RenderProfileClock::time_point nextStart)
    {
        windowStart = nextStart;
        frameCount  = 0;
        total.reset();
        fence.reset();
        acquire.reset();
        pollEvents.reset();
        newFrame.reset();
        prepareResources.reset();
        updateUi.reset();
        imguiRender.reset();
        commandSetup.reset();
        offscreenRecord.reset();
        mainRecord.reset();
        submit.reset();
        present.reset();
        platformWindows.reset();
    }

    /// @brief 到达统计间隔后输出一次累计结果。
    /// @param now 当前时间。
    /// @warning 渲染热路径：每帧只做时间间隔判断；到达间隔后才写日志。
    void logIfReady(RenderProfileClock::time_point now)
    {
        const double elapsedSeconds =
            std::chrono::duration<double>(now - windowStart).count();
        const double logIntervalSeconds =
            std::chrono::duration<double>(RENDER_PROFILE_LOG_INTERVAL).count();
        if ( elapsedSeconds < logIntervalSeconds || frameCount == 0 ) {
            return;
        }

        const double averageFps = static_cast<double>(frameCount) /
                                  std::max(elapsedSeconds, 0.000001);
        XINFO(
            "RenderProfile {:.2f}s frames={} fps={:.1f} "
            "total(avg/max)={:.3f}/{:.3f}ms",
            elapsedSeconds,
            frameCount,
            averageFps,
            total.average(frameCount),
            total.maxMs);
        XINFO(
            "RenderStages avg/max ms: fence {:.3f}/{:.3f}, acquire "
            "{:.3f}/{:.3f}, poll {:.3f}/{:.3f}, newFrame {:.3f}/{:.3f}, "
            "prepare {:.3f}/{:.3f}, updateUI {:.3f}/{:.3f}",
            fence.average(frameCount),
            fence.maxMs,
            acquire.average(frameCount),
            acquire.maxMs,
            pollEvents.average(frameCount),
            pollEvents.maxMs,
            newFrame.average(frameCount),
            newFrame.maxMs,
            prepareResources.average(frameCount),
            prepareResources.maxMs,
            updateUi.average(frameCount),
            updateUi.maxMs);
        XINFO(
            "RenderStages avg/max ms: imguiRender {:.3f}/{:.3f}, cmdSetup "
            "{:.3f}/{:.3f}, offscreen {:.3f}/{:.3f}, mainPass {:.3f}/{:.3f}, "
            "submit {:.3f}/{:.3f}, present {:.3f}/{:.3f}, platform "
            "{:.3f}/{:.3f}",
            imguiRender.average(frameCount),
            imguiRender.maxMs,
            commandSetup.average(frameCount),
            commandSetup.maxMs,
            offscreenRecord.average(frameCount),
            offscreenRecord.maxMs,
            mainRecord.average(frameCount),
            mainRecord.maxMs,
            submit.average(frameCount),
            submit.maxMs,
            present.average(frameCount),
            present.maxMs,
            platformWindows.average(frameCount),
            platformWindows.maxMs);

        reset(now);
    }
};

/// @brief 计算两个时间点之间的毫秒差。
/// @param begin 起始时间点。
/// @param end 结束时间点。
/// @return 时间差，单位为毫秒。
double elapsedMilliseconds(RenderProfileClock::time_point begin,
                           RenderProfileClock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

/// @brief 在启用渲染性能日志时读取当前时间点。
/// @param enabled 是否启用渲染性能日志。
/// @return 启用时返回当前时间点，关闭时返回空时间点。
/// @warning 渲染热路径：每个阶段边界调用；关闭日志时不得调用系统时钟。
RenderProfileClock::time_point profileTimePoint(bool enabled)
{
    return enabled ? RenderProfileClock::now()
                   : RenderProfileClock::time_point{};
}
}  // namespace

// clang-format off
/**
 * @brief 执行单帧渲染
 *
 * 包含等待 Fence、获取图像、录制命令、提交队列、呈现图像等步骤。
 */
/// @warning 热路径：主线程每帧执行；Fence/Acquire/Present 不可中断。
/// 禁止在此加入文件系统访问、完整 ECS 遍历或完整排序。
// clang-format on
void VKRenderer::render(NativeWindow&                window,
                        std::span<IGraphicUserHook*> graphicUserHooks)
{
    static RenderProfileAccumulator profile;
    static bool                     lastRenderProfileLoggingEnabled = false;
    const bool                      renderProfileLoggingEnabled =
        Config::AppConfig::instance().getEditorSettings().renderProfileLogging;
    if ( renderProfileLoggingEnabled && !lastRenderProfileLoggingEnabled ) {
        profile.reset(RenderProfileClock::now());
    }
    lastRenderProfileLoggingEnabled = renderProfileLoggingEnabled;
    const auto frameProfileStart =
        profileTimePoint(renderProfileLoggingEnabled);

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
    const auto fenceStart = profileTimePoint(renderProfileLoggingEnabled);
    auto       waitResult = m_vkLogicalDevice.waitForFences(
        m_cmdAvailableFences[m_currentFrameIndex],
        true,
        std::numeric_limits<uint64_t>::max());
    if ( waitResult != vk::Result::eSuccess ) {
        XWARN("VK Device WaitForFences failed");
    }

    // 恢复fence
    (void)m_vkLogicalDevice.resetFences(
        m_cmdAvailableFences[m_currentFrameIndex]);
    const auto fenceEnd = profileTimePoint(renderProfileLoggingEnabled);

    // --- [优化] 在准备新帧之前获取图像 ---
    // 请求下一个可绘制的图像 - 查到的同时发出图像可用信号量
    // 在 FIFO (VSync) 模式下，这里是主要的阻塞点，会等待垂直同步
    const auto acquireStart = profileTimePoint(renderProfileLoggingEnabled);
    vk::ResultValue<uint32_t> imageResult =
        m_vkLogicalDevice.acquireNextImageKHR(
            m_vkSwapChain.m_swapchain,
            std::numeric_limits<uint64_t>::max(),
            m_imageAvailableSems[m_currentFrameIndex]);
    const auto acquireEnd = profileTimePoint(renderProfileLoggingEnabled);

    if ( imageResult.result == vk::Result::eErrorOutOfDateKHR ) {
        triggerRecreate(window);
        return;
    } else if ( imageResult.result != vk::Result::eSuccess &&
                imageResult.result != vk::Result::eSuboptimalKHR ) {
        XWARN("acquire ImageKHR failed: {}",
              static_cast<int>(imageResult.result));
        return;
    }

    // 获取到实际查询到的可绘制的图像下标
    uint32_t imageIndex = imageResult.value;

    // 安全检查：防止越界
    if ( imageIndex >= m_vkSwapChain.m_vkImageBuffers.size() ) {
        XERROR("Invalid image index acquired: {}", imageIndex);
        return;
    }

    // --- [关键优化] 在 VSync 阻塞解除后立即处理输入 ---
    // 这样能保证本帧使用的输入数据是最新鲜的
    const auto pollStart = profileTimePoint(renderProfileLoggingEnabled);
    window.pollEvents();
    const auto pollEnd = profileTimePoint(renderProfileLoggingEnabled);

    // --- 1. ImGui 准备新帧 ---
    const auto newFrameStart = profileTimePoint(renderProfileLoggingEnabled);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // 根据配置决定是否隐藏系统光标
    auto& editorCfg = Config::AppConfig::instance().getEditorConfig();
    if ( editorCfg.settings.cursorStyle == Config::CursorStyle::Software ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        glfwSetInputMode(
            window.getWindowHandle(), GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
    } else {
        glfwSetInputMode(
            window.getWindowHandle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    const auto newFrameEnd = profileTimePoint(renderProfileLoggingEnabled);

    // 准备所有资源
    const auto prepareStart = profileTimePoint(renderProfileLoggingEnabled);
    for ( auto& graphicUserHook : graphicUserHooks ) {
        graphicUserHook->onPrepareResources(m_vkPhysicalDevice,
                                            m_vkLogicalDevice,
                                            m_vkSwapChain,
                                            m_vkCommandPool,
                                            m_LogicDeviceGraphicsQueue);
    }
    const auto prepareEnd = profileTimePoint(renderProfileLoggingEnabled);

    // 录制所有ui
    const auto updateUiStart = profileTimePoint(renderProfileLoggingEnabled);
    for ( auto& graphicUserHook : graphicUserHooks ) {
        graphicUserHook->onUpdateUI();
    }

    // 绘制中央临时通知
    VKContext::get().value().get().drawCenterNotification();

    // 更新光标管理器
    if ( m_cursorManager &&
         editorCfg.settings.cursorStyle == Config::CursorStyle::Software ) {
        m_cursorManager->UpdateAndDraw(m_cursorSmokeLifeOverride);
    }
    const auto updateUiEnd = profileTimePoint(renderProfileLoggingEnabled);

    const auto imguiRenderStart = profileTimePoint(renderProfileLoggingEnabled);
    ImGui::Render();  // 生成imgui绘制顶点数据
    const auto imguiRenderEnd = profileTimePoint(renderProfileLoggingEnabled);

    m_offscreenRecordTasks.clear();
    for ( auto& graphicUserHook : graphicUserHooks ) {
        if ( !graphicUserHook ) {
            continue;
        }

        const uint32_t taskCount =
            graphicUserHook->getOffscreenRecordTaskCount();
        for ( uint32_t taskIndex = 0; taskIndex < taskCount; ++taskIndex ) {
            m_offscreenRecordTasks.push_back({ graphicUserHook, taskIndex });
        }
    }

    const auto commandSetupStart =
        profileTimePoint(renderProfileLoggingEnabled);

    // 重置命令缓冲
    auto& currentCmdBuffer = m_vkCommandBuffers[m_currentFrameIndex];
    (void)currentCmdBuffer.reset();

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

    auto* offscreenRecordThreadPool =
        MMM::Runtime::AppThreadPool::instance().get();
    const bool useParallelOffscreenRecord =
        offscreenRecordThreadPool && m_offscreenRecordTasks.size() > 1;
    if ( useParallelOffscreenRecord ) {
        ensureOffscreenRecordSlots(m_offscreenRecordTasks.size());
    }

    // 命令录制
    (void)currentCmdBuffer.begin(commandBufferBeginInfo);
    const auto commandSetupEnd = profileTimePoint(renderProfileLoggingEnabled);

    // 录制所有离屏渲染命令
    const auto offscreenStart = profileTimePoint(renderProfileLoggingEnabled);
    const uint32_t recordFrameIndex =
        static_cast<uint32_t>(m_currentFrameIndex);
    if ( useParallelOffscreenRecord ) {
        std::latch offscreenLatch(
            static_cast<std::ptrdiff_t>(m_offscreenRecordTasks.size()));
        for ( size_t taskSlot = 0; taskSlot < m_offscreenRecordTasks.size();
              ++taskSlot ) {
            const OffscreenRecordTask task = m_offscreenRecordTasks[taskSlot];
            vk::CommandBuffer taskCmd = m_offscreenRecordSlots[taskSlot]
                                            .commandBuffers[recordFrameIndex];
            offscreenRecordThreadPool->enqueue_void(
                [task, taskCmd, recordFrameIndex, &offscreenLatch]() mutable {
                    (void)taskCmd.reset();
                    vk::CommandBufferBeginInfo taskBeginInfo;
                    taskBeginInfo.setFlags(
                        vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
                    (void)taskCmd.begin(taskBeginInfo);
                    if ( task.hook ) {
                        task.hook->onRecordOffscreenTask(
                            taskCmd, recordFrameIndex, task.taskIndex);
                    }
                    (void)taskCmd.end();
                    offscreenLatch.count_down();
                });
        }
        offscreenLatch.wait();
    } else {
        for ( const auto& task : m_offscreenRecordTasks ) {
            if ( task.hook ) {
                task.hook->onRecordOffscreenTask(
                    currentCmdBuffer, recordFrameIndex, task.taskIndex);
            }
        }
    }
    const auto offscreenEnd = profileTimePoint(renderProfileLoggingEnabled);

    const auto mainRecordStart = profileTimePoint(renderProfileLoggingEnabled);
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
    (void)currentCmdBuffer.end();  // 结束命令录制
    const auto mainRecordEnd = profileTimePoint(renderProfileLoggingEnabled);

    m_frameSubmitCommandBuffers.clear();
    if ( useParallelOffscreenRecord ) {
        for ( size_t taskSlot = 0; taskSlot < m_offscreenRecordTasks.size();
              ++taskSlot ) {
            m_frameSubmitCommandBuffers.push_back(
                m_offscreenRecordSlots[taskSlot]
                    .commandBuffers[recordFrameIndex]);
        }
    }
    m_frameSubmitCommandBuffers.push_back(currentCmdBuffer);

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
        .setCommandBuffers(m_frameSubmitCommandBuffers)
        // 等待信号量
        .setWaitSemaphores(m_imageAvailableSems[m_currentFrameIndex])
        // 设置等待的阶段掩码
        .setWaitDstStageMask(waitStages)
        // 发出信号量
        .setSignalSemaphores(m_renderFinishedSems[imageIndex]);
    const auto submitStart = profileTimePoint(renderProfileLoggingEnabled);
    (void)m_LogicDeviceGraphicsQueue.submit(
        submitInfo, m_cmdAvailableFences[m_currentFrameIndex]);
    const auto submitEnd = profileTimePoint(renderProfileLoggingEnabled);

    // 呈现
    vk::PresentInfoKHR presentInfo;
    presentInfo
        // 呈现哪张图像?上面拿到的
        .setImageIndices(imageIndex)
        // 设置交换链
        .setSwapchains(m_vkSwapChain.m_swapchain)
        // 等待信号量
        .setWaitSemaphores(m_renderFinishedSems[imageIndex]);

    const auto presentStart = profileTimePoint(renderProfileLoggingEnabled);
    vk::Result presentResult =
        m_LogicDevicePresentQueue.presentKHR(presentInfo);
    const auto presentEnd = profileTimePoint(renderProfileLoggingEnabled);

    if ( presentResult == vk::Result::eErrorOutOfDateKHR ||
         presentResult == vk::Result::eSuboptimalKHR ) {
        m_vkSwapChain.markDirty();
    } else if ( presentResult != vk::Result::eSuccess ) {
        XWARN("Present failed: {}", static_cast<int>(presentResult));
    }

    // 并发帧数步进
    ++m_currentFrameIndex %= MAX_FRAMES_IN_FLIGHT;

    // 更新并渲染所有的多视口 (Viewports)
    ImGuiIO&   io            = ImGui::GetIO();
    const auto platformStart = profileTimePoint(renderProfileLoggingEnabled);
    if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) {
        ImGui::UpdatePlatformWindows();
#ifdef _WIN32
        // 对所有多视口平台窗口应用 Win32 DWM 圆角和阴影，确保视觉一致性
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        for ( int i = 0; i < platform_io.Viewports.Size; ++i ) {
            ImGuiViewport* vp = platform_io.Viewports[i];
            if ( vp->PlatformHandle ) {
                GLFWwindow* glfwWin =
                    static_cast<GLFWwindow*>(vp->PlatformHandle);
                HWND hwnd = glfwGetWin32Window(glfwWin);
                if ( hwnd ) {
                    const MARGINS shadow_margin = { 1, 1, 1, 1 };
                    DwmExtendFrameIntoClientArea(hwnd, &shadow_margin);

                    DWORD cornerPreference = DWMWCP_ROUND;
                    DwmSetWindowAttribute(hwnd,
                                          DWMWA_WINDOW_CORNER_PREFERENCE,
                                          &cornerPreference,
                                          sizeof(cornerPreference));
                }
            }
        }
#endif
        ImGui::RenderPlatformWindowsDefault();
    }
    const auto platformEnd     = profileTimePoint(renderProfileLoggingEnabled);
    const auto frameProfileEnd = profileTimePoint(renderProfileLoggingEnabled);

    if ( renderProfileLoggingEnabled ) {
        ++profile.frameCount;
        profile.total.add(
            elapsedMilliseconds(frameProfileStart, frameProfileEnd));
        profile.fence.add(elapsedMilliseconds(fenceStart, fenceEnd));
        profile.acquire.add(elapsedMilliseconds(acquireStart, acquireEnd));
        profile.pollEvents.add(elapsedMilliseconds(pollStart, pollEnd));
        profile.newFrame.add(elapsedMilliseconds(newFrameStart, newFrameEnd));
        profile.prepareResources.add(
            elapsedMilliseconds(prepareStart, prepareEnd));
        profile.updateUi.add(elapsedMilliseconds(updateUiStart, updateUiEnd));
        profile.imguiRender.add(
            elapsedMilliseconds(imguiRenderStart, imguiRenderEnd));
        profile.commandSetup.add(
            elapsedMilliseconds(commandSetupStart, commandSetupEnd));
        profile.offscreenRecord.add(
            elapsedMilliseconds(offscreenStart, offscreenEnd));
        profile.mainRecord.add(
            elapsedMilliseconds(mainRecordStart, mainRecordEnd));
        profile.submit.add(elapsedMilliseconds(submitStart, submitEnd));
        profile.present.add(elapsedMilliseconds(presentStart, presentEnd));
        profile.platformWindows.add(
            elapsedMilliseconds(platformStart, platformEnd));
        profile.logIfReady(frameProfileEnd);
    }
}

}  // namespace MMM::Graphic
