#include "graphic/imguivk/VKContext.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/input/MMMInput.h"
#include "event/input/glfw/GLFWKeyEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"

#include "graphic/glfw/window/NativeWindow.h"
#include "imgui_impl_glfw.h"
#include "log/colorful-log.h"

#include <fmt/format.h>

namespace MMM::Graphic
{
namespace
{
/// @brief 将物理设备类型格式化为短文本。
/// @param type Vulkan-Hpp 物理设备类型。
/// @return 设备类型文本。
const char* physicalDeviceTypeText(vk::PhysicalDeviceType type)
{
    switch ( type ) {
    case vk::PhysicalDeviceType::eDiscreteGpu: return "discrete-gpu";
    case vk::PhysicalDeviceType::eIntegratedGpu: return "integrated-gpu";
    case vk::PhysicalDeviceType::eVirtualGpu: return "virtual-gpu";
    case vk::PhysicalDeviceType::eCpu: return "cpu";
    case vk::PhysicalDeviceType::eOther: return "other";
    default: return "unknown";
    }
}

/// @brief 物理设备与队列族选择结果。
struct DeviceSelection final {
    /// @brief 物理设备句柄。
    vk::PhysicalDevice device{};

    /// @brief 队列族索引。
    QueueFamilyIndices indices{};

    /// @brief 设备名称。
    std::string deviceName;

    /// @brief 是否为独立显卡。
    bool isDiscreteGpu{ false };

    /// @brief 检查是否找到了可用设备和队列族。
    /// @return 找到可用设备和队列族时返回 true。
    bool isValid() const { return device && indices; }
};
}  // namespace

/**
 * @brief 获取 VKContext 单例实例
 *
 * 如果实例尚未创建，将尝试进行初始化。
 *
 * @return std::expected<std::reference_wrapper<VKContext>, std::string>
 *         成功返回上下文引用，失败返回错误信息字符串
 */
std::expected<std::reference_wrapper<VKContext>, std::string> VKContext::get()
{
    try {
        // 尝试初始化VK上下文
        static VKContext vkContext;
        return vkContext;
    } catch ( const std::runtime_error& e ) {
        // 初始化失败返回what错误
        return std::unexpected<std::string>(e.what());
    }
}

VKContext::VKContext()
{
    // 初始化GLFW
    initGLFW();
    // 注册GLFW的VK扩展
    registerGLFWExtensions();

    // Debug模式启用VK调试工具
    if ( is_debug() ) {
        enableVKDebugExt();
        enableVKValidateLayer();
    }

    // 初始化vk应用程序信息
    initVkAppInfo();

    // 初始化vk实例创建信息
    initVkInstanceCreateInfo();
    collectVulkanInstanceCreateDiagnostics();

    // 创建vk实例
    auto instanceResult = vk::createInstance(m_vkInstanceCreateInfo);
    if ( instanceResult.result != vk::Result::eSuccess ) {
        addStartupDiagnostic(fmt::format("vkCreateInstance failed: {}",
                                         vk::to_string(instanceResult.result)));
        logStartupDiagnostics("vkCreateInstance failed.");
        releaseGLFW();
        throw std::runtime_error("Fatal: Failed to create Vulkan instance.");
    }
    m_vkInstance = instanceResult.value;
    addStartupDiagnostic("Vulkan instance created successfully.");
    XDEBUG("VK Instance created.");
    collectPhysicalDeviceDiagnostics(false);

    // 初始化vk动态加载器(要在创建vkInstance后)
    // (它会去找到扩展函数 如 vkCreateDebugUtilsMessengerEXT的地址)
    m_vkDldy.init(m_vkInstance, vkGetInstanceProcAddr);
    XDEBUG("VK dldy initialized.");

    if ( is_debug() ) {
        // debug模式初始化vk调试信息工具
        // 创建 Messenger 对象
        // 这里必须传入 dldy 否则会链接报错
        auto debugMessengerResult = m_vkInstance.createDebugUtilsMessengerEXT(
            m_vkDebugUtilCreateInfo, nullptr, m_vkDldy);
        if ( debugMessengerResult.result != vk::Result::eSuccess ) {
            addStartupDiagnostic(
                fmt::format("vkCreateDebugUtilsMessengerEXT failed: {}",
                            vk::to_string(debugMessengerResult.result)));
            logStartupDiagnostics("vkCreateDebugUtilsMessengerEXT failed.");
            release();
            releaseGLFW();
            throw std::runtime_error(
                "Fatal: Failed to create Vulkan debug messenger.");
        }
        m_vkDebugMessenger = debugMessengerResult.value;
        addStartupDiagnostic("Vulkan debug messenger created successfully.");
        XDEBUG("Vulkan Debug Messenger Initialize Successed");
    }

    // 初始化窗口表面和
    // 后续显卡设备初始化
    // 放在窗口相关资源初始化函数中

    MMM::Event::EventBus::instance().subscribe<MMM::Event::GLFWKeyEvent>(
        [&](MMM::Event::GLFWKeyEvent e) {
            if ( e.key == MMM::Event::Input::Key::F7 &&
                 e.action == MMM::Event::Input::Action::Press ) {
                /// @brief F7 循环切换帧率限制模式
                auto& config =
                    MMM::Config::AppConfig::instance().getEditorConfig();
                int currentLimit = static_cast<int>(config.settings.frameLimit);
                currentLimit     = (currentLimit + 1) % 5;
                config.settings.frameLimit =
                    static_cast<MMM::Config::FrameLimitPreference>(
                        currentLimit);

                // 发布配置更新事件，触发所有监听者（包括本类中的
                // LogicCommandEvent 监听器）
                MMM::Event::EventBus::instance().publish(
                    MMM::Event::LogicCommandEvent(
                        MMM::Logic::CmdUpdateEditorConfig{ config }));

                // 持久化配置
                MMM::Config::AppConfig::instance().save();

                const char* displayNames[] = {
                    TR("ui.settings.software.framelimit.vsync").data(),
                    TR("ui.settings.software.framelimit.2x").data(),
                    TR("ui.settings.software.framelimit.4x").data(),
                    TR("ui.settings.software.framelimit.8x").data(),
                    TR("ui.settings.software.framelimit.unlimited").data()
                };
                std::string title =
                    TR("ui.settings.software.framelimit").data();
                std::string notificationMsg =
                    title + ": " + displayNames[currentLimit];
                showCenterNotification(notificationMsg);

                XDEBUG("Frame Limit mode toggled by shortcut: {}",
                       notificationMsg);
            }
            if ( e.key == MMM::Event::Input::Key::F11 &&
                 e.action == MMM::Event::Input::Action::Press ) {
                /// @brief F11 切换全屏
                ToggleFullscreen();

                bool isFullscreen =
                    (glfwGetWindowMonitor(
                         m_nativeWindow_ptr->getWindowHandle()) != nullptr);
                showCenterNotification(
                    isFullscreen
                        ? TR("ui.settings.software.screen.fullscreen").data()
                        : TR("ui.settings.software.screen.windowed").data());
            }
        });

    MMM::Event::EventBus::instance().subscribe<MMM::Event::LogicCommandEvent>(
        [&](MMM::Event::LogicCommandEvent e) {
            if ( std::holds_alternative<MMM::Logic::CmdUpdateEditorConfig>(
                     e.command) ) {
                auto& cmd =
                    std::get<MMM::Logic::CmdUpdateEditorConfig>(e.command);
                setFrameLimitPresentMode(cmd.config.settings.frameLimit);
                applyTheme();
            }
        });
}

VKContext::~VKContext()
{
    release();

    // 最后释放 GLFW，确保所有窗口和上下文都已安全销毁
    releaseGLFW();
}

void VKContext::release()
{
    if ( m_isReleased ) return;

    if ( m_vkLogicalDevice ) {
        (void)m_vkLogicalDevice.waitIdle();
    }

    XDEBUG("Starting VKContext explicit release...");

    // 在关闭 ImGui 之前必须先释放渲染器中 imgui 对纹理的引用！
    if ( m_vkRenderer ) {
        m_vkRenderer->releaseCursorManager();
    }

    // 在销毁 DescriptorPool 和 Device 之前必须先关闭 ImGui！
    // 检查 ImGui 上下文是否存在
    if ( ImGui::GetCurrentContext() ) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        XDEBUG("ImGui Destroyed.");
    }

    // 销毁渲染器
    m_vkRenderer.reset();

    if ( m_swapchain ) {
        // 手动销毁交换链中的帧缓冲
        m_swapchain->destroyFramebuffers();
    }

    // RenderPass 是通过 Device 创建和销毁的
    // 在销毁 Device 前销毁 RenderPass
    m_vkRenderPass.reset();

    // 销毁交换链
    m_swapchain.reset();

    // 销毁逻辑设备
    if ( m_vkLogicalDevice ) {
        m_vkLogicalDevice.destroy();
        m_vkLogicalDevice = nullptr;
        XDEBUG("VK Logical Device destroyed.");
    }

    // 销毁 vk 表面
    if ( m_vkSurface ) {
        m_vkInstance.destroySurfaceKHR(m_vkSurface);
        m_vkSurface = nullptr;
        XDEBUG("VK Surface destroyed.");
    }

    // 销毁可能的 vk 调试信息工具实例
    if ( is_debug() && m_vkDebugMessenger ) {
        m_vkInstance.destroyDebugUtilsMessengerEXT(
            m_vkDebugMessenger, nullptr, m_vkDldy);
        m_vkDebugMessenger = nullptr;
        XDEBUG("VK Debug Messenger destroyed.");
    }

    // 销毁 vk 实例
    if ( m_vkInstance ) {
        m_vkInstance.destroy();
        m_vkInstance = nullptr;
        XDEBUG("VK Instance destroyed.");
    }

    m_isReleased = true;
    XINFO("VKContext resources released.");
}

/**
 * @brief 使用imgui自动选择物理设备和队列族
 */
void VKContext::imguiAutoSelect()
{
    auto devicesResult = m_vkInstance.enumeratePhysicalDevices();
    if ( devicesResult.result != vk::Result::eSuccess ||
         devicesResult.value.empty() ) {
        addStartupDiagnostic(
            fmt::format("Physical device selection failed: result={}, count={}",
                        vk::to_string(devicesResult.result),
                        devicesResult.value.size()));
        logStartupDiagnostics("No usable Vulkan physical device.");
        throw std::runtime_error(
            "Fatal: No usable Vulkan physical device was found.");
    }

    DeviceSelection fallbackSelection{};
    DeviceSelection preferredSelection{};
    for ( const auto& device : devicesResult.value ) {
        DeviceSelection selection{};
        selection.device = device;

        const auto properties = device.getProperties();
        selection.deviceName  = properties.deviceName.data();
        selection.isDiscreteGpu =
            properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;

        const auto queueFamilies = device.getQueueFamilyProperties();
        for ( uint32_t queueIndex = 0;
              queueIndex < static_cast<uint32_t>(queueFamilies.size());
              ++queueIndex ) {
            const bool supportsGraphics =
                (queueFamilies[queueIndex].queueFlags &
                 vk::QueueFlagBits::eGraphics) == vk::QueueFlagBits::eGraphics;
            if ( supportsGraphics &&
                 !selection.indices.graphicsQueueIndex.has_value() ) {
                selection.indices.graphicsQueueIndex = queueIndex;
            }

            bool supportsPresent = !m_vkSurface && supportsGraphics;
            if ( m_vkSurface ) {
                auto presentResult =
                    device.getSurfaceSupportKHR(queueIndex, m_vkSurface);
                supportsPresent =
                    presentResult.result == vk::Result::eSuccess &&
                    presentResult.value == VK_TRUE;
            }
            if ( supportsPresent &&
                 !selection.indices.presentQueueIndex.has_value() ) {
                selection.indices.presentQueueIndex = queueIndex;
            }
        }

        if ( !selection.isValid() ) {
            addStartupDiagnostic(fmt::format(
                "Rejected GPU \"{}\": graphicsQueue={}, presentQueue={}",
                selection.deviceName,
                selection.indices.graphicsQueueIndex.has_value(),
                selection.indices.presentQueueIndex.has_value()));
            continue;
        }

        if ( !fallbackSelection.isValid() ) {
            fallbackSelection = selection;
        }
        if ( selection.isDiscreteGpu ) {
            preferredSelection = selection;
            break;
        }
    }

    const DeviceSelection selected =
        preferredSelection.isValid() ? preferredSelection : fallbackSelection;
    if ( !selected.isValid() ) {
        logStartupDiagnostics(
            "No Vulkan physical device has graphics and present queues.");
        throw std::runtime_error(
            "Fatal: No Vulkan physical device has graphics and present "
            "queues.");
    }

    m_vkPhysicalDevice   = selected.device;
    m_queueFamilyIndices = selected.indices;

    const auto properties = m_vkPhysicalDevice.getProperties();
    addStartupDiagnostic(fmt::format(
        "Selected GPU: \"{}\", type={}, graphicsQueue={}, presentQueue={}",
        properties.deviceName.data(),
        physicalDeviceTypeText(properties.deviceType),
        m_queueFamilyIndices.graphicsQueueIndex.value(),
        m_queueFamilyIndices.presentQueueIndex.value()));
    XINFO("Selected Vulkan GPU: {} ({})",
          properties.deviceName.data(),
          physicalDeviceTypeText(properties.deviceType));
}

/**
 * @brief 初始化 Vulkan 窗体相关资源
 *
 * 包括 Surface, Swapchain, RenderPass, Pipeline 等依赖窗口尺寸的资源。
 *
 * @param window_ctx GLFW 窗口句柄
 * @param w 窗口宽度
 * @param h 窗口高度
 */
void VKContext::initVKWindowRess(NativeWindow* native_window_ptr, int w, int h)
{
    m_nativeWindow_ptr = native_window_ptr;
    addStartupDiagnostic(
        fmt::format("Initializing Vulkan window resources: {}x{}", w, h));

    if ( !native_window_ptr || !native_window_ptr->getWindowHandle() ) {
        addStartupDiagnostic("Native window handle is null.");
        logStartupDiagnostics("Native window handle is null.");
        throw std::runtime_error(
            "Failed to initialize Vulkan window resources: "
            "native window is null.");
    }

    // 初始化vk表面句柄
    // C 风格的 Surface 创建（GLFW 提供的快捷函数）
    VkSurfaceKHR   surface;
    const VkResult surfaceResult = glfwCreateWindowSurface(
        m_vkInstance, native_window_ptr->getWindowHandle(), nullptr, &surface);
    if ( surfaceResult != VK_SUCCESS ) {
        addStartupDiagnostic(
            fmt::format("glfwCreateWindowSurface failed: {}",
                        vk::to_string(static_cast<vk::Result>(surfaceResult))));
        collectLastGLFWErrorDiagnostic("glfwCreateWindowSurface failed.");
        logStartupDiagnostics("glfwCreateWindowSurface failed.");
        throw std::runtime_error("Failed to create window surface!");
    }

    // 转换为 vk::SurfaceKHR
    m_vkSurface = surface;
    addStartupDiagnostic("Vulkan window surface created successfully.");
    XDEBUG("Vulkan Surface created.");
    collectPhysicalDeviceDiagnostics(true);

    // 使用imgui自动选择物理设备和队列族
    imguiAutoSelect();
    collectSelectedSurfaceDiagnostics(w, h);

    // 在创建交换链之前，根据配置预设全局呈现模式，避免启动后再次重建
    updateGlobalPresentMode(
        Config::AppConfig::instance().getEditorSettings().frameLimit);

    // 初始化逻辑设备
    initLogicDevice();

    // 创建交换链
    m_swapchain = std::make_unique<VKSwapchain>(m_vkPhysicalDevice,
                                                m_vkLogicalDevice,
                                                m_vkSurface,
                                                m_queueFamilyIndices,
                                                w,
                                                h);



    // 创建最终呈现的渲染流程
    m_vkRenderPass = std::make_unique<VKRenderPass>(
        m_vkLogicalDevice, *m_swapchain, vk::ImageLayout::ePresentSrcKHR);

    // 创建帧缓冲
    m_swapchain->createFramebuffers(*m_vkRenderPass);

    // 创建渲染器
    m_vkRenderer = std::make_unique<VKRenderer>(m_vkPhysicalDevice,
                                                m_vkLogicalDevice,
                                                *m_swapchain,
                                                *m_vkRenderPass,
                                                m_LogicDeviceGraphicsQueue,
                                                m_LogicDevicePresentQueue);

    // 初始化渲染器中的光标管理器
    m_vkRenderer->initCursorManager(m_vkPhysicalDevice, m_vkLogicalDevice);

    // 初始化 ImGui
    imguiVulkanInit(native_window_ptr->getWindowHandle());
    applyTheme();
}

/**
 * @brief 重建交换链
 */
void VKContext::recreateSwapchain(GLFWwindow* window_context, int width,
                                  int height)
{
    // 1. 最小化处理
    while ( width == 0 || height == 0 ) {
        glfwGetFramebufferSize(window_context, &width, &height);
        glfwWaitEvents();
    }

    // 2. 等待设备空闲
    (void)m_vkLogicalDevice.waitIdle();

    // 3. 【只清理尺寸相关的资源】
    // 不需要销毁 Device! 不需要销毁 Renderer!

    // 4. 【重建交换链】
    // 传入旧的 swapchain 句柄可以加速创建 (oldSwapchain)
    m_swapchain->recreate(
        m_vkPhysicalDevice, m_vkSurface, m_queueFamilyIndices, width, height);

    // 5. 【重建帧缓冲】
    // 既然 RenderPass 没变，直接用原来的 RenderPass 重新绑定到新的 Swapchain
    // 图像上
    m_swapchain->createFramebuffers(*m_vkRenderPass);

    // 6. 【通知渲染器更新】
    // 后续如果渲染器内部存了 imageCount 之类的缓存，更新它
    m_vkRenderer->onSwapchainChanged();

    XDEBUG("Swapchain recreation finished.");
}

/// @brief 根据帧率限制策略切换交换链呈现模式。
void VKContext::setFrameLimitPresentMode(
    Config::FrameLimitPreference frameLimit)
{
    auto nextPresentMode = selectPresentMode(frameLimit);
    if ( VKSwapchain::s_globalPresentMode == nextPresentMode ) {
        return;
    }

    // 1. 等待设备空闲，因为要修改交换链
    (void)m_vkLogicalDevice.waitIdle();

    // 2. 修改交换链配置类里的 PresentMode 偏好
    VKSwapchain::s_globalPresentMode = nextPresentMode;

    // 3. 标记需要重建
    if ( m_swapchain ) {
        m_swapchain->markDirty();
    }
}

/// @brief 根据帧率限制策略选择当前设备支持的呈现模式。
vk::PresentModeKHR VKContext::selectPresentMode(
    Config::FrameLimitPreference frameLimit) const
{
    if ( frameLimit == Config::FrameLimitPreference::VSync ) {
        return vk::PresentModeKHR::eFifo;
    }

    bool supportsImmediate = false;
    bool supportsMailbox   = false;
    if ( m_vkPhysicalDevice && m_vkSurface ) {
        std::vector<vk::PresentModeKHR> supportedPresentModes =
            m_vkPhysicalDevice.getSurfacePresentModesKHR(m_vkSurface).value;
        for ( const auto& presentMode : supportedPresentModes ) {
            if ( presentMode == vk::PresentModeKHR::eImmediate ) {
                supportsImmediate = true;
            } else if ( presentMode == vk::PresentModeKHR::eMailbox ) {
                supportsMailbox = true;
            }
        }
    }

    if ( supportsImmediate ) {
        return vk::PresentModeKHR::eImmediate;
    }
    if ( supportsMailbox ) {
        return vk::PresentModeKHR::eMailbox;
    }
    return vk::PresentModeKHR::eFifo;
}

/// @brief 仅更新全局呈现模式参数，不触发重建。
void VKContext::updateGlobalPresentMode(Config::FrameLimitPreference frameLimit)
{
    VKSwapchain::s_globalPresentMode = selectPresentMode(frameLimit);
}

/**
 * @brief 全屏
 */
void VKContext::ToggleFullscreen()
{
    m_nativeWindow_ptr->ToggleFullscreen();
    // 因为全屏会导致窗口尺寸剧变，必须标记交换链需要重建
    m_swapchain->markDirty();
}

/**
 * @brief 显示在窗口正中央的临时通知/Tooltip
 */
void VKContext::showCenterNotification(const std::string& message,
                                       float              durationSeconds)
{
    m_notificationMessage = message;
    m_notificationExpireTime =
        glfwGetTime() + static_cast<double>(durationSeconds);
}

/**
 * @brief 绘制临时中央通知 (每帧渲染)
 */
void VKContext::drawCenterNotification()
{
    double currentTime = glfwGetTime();
    if ( currentTime >= m_notificationExpireTime ||
         m_notificationMessage.empty() ) {
        return;
    }

    // 计算剩余时间实现淡出效果
    float timeLeft = static_cast<float>(m_notificationExpireTime - currentTime);
    float alpha    = 1.0f;
    if ( timeLeft < 0.3f ) {
        alpha = timeLeft / 0.3f;  // 最后 0.3 秒进行渐隐淡出
    }

    // 设置无边框、无背景、不可交互的窗口
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);  // 窗口底层完全透明

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs;

    if ( ImGui::Begin("##CenterNotification", nullptr, flags) ) {
        // 自定义绘制一个圆角的高端黑色毛玻璃质感背板
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2      minPos   = ImGui::GetWindowPos();
        ImVec2      maxPos   = ImVec2(minPos.x + ImGui::GetWindowWidth(),
                                      minPos.y + ImGui::GetWindowHeight());

        // 绘制毛玻璃/半透明背板 (深色磨砂)
        ImU32 bgColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.07f, 0.07f, 0.11f, 0.85f * alpha));
        ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
            0.25f, 0.45f, 0.95f, 0.6f * alpha));  // 极富科技感的幽蓝色边框

        // 稍微往外扩一点作为背板
        float  paddingX = 24.0f;
        float  paddingY = 16.0f;
        ImVec2 bgMin    = ImVec2(minPos.x - paddingX, minPos.y - paddingY);
        ImVec2 bgMax    = ImVec2(maxPos.x + paddingX, maxPos.y + paddingY);

        drawList->AddRectFilled(bgMin, bgMax, bgColor, 12.0f);  // 12px 大圆角
        drawList->AddRect(
            bgMin, bgMax, borderColor, 12.0f, 0, 1.5f);  // 幽蓝外边框

        // 渲染文本，使其发光/带有高饱和颜色
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 1.0f, alpha));
        ImGui::TextUnformatted(m_notificationMessage.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

}  // namespace MMM::Graphic
