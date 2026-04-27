#include "graphic/imguivk/VKContext.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/input/MMMInput.h"
#include "event/input/glfw/GLFWKeyEvent.h"
#include "event/logic/LogicCommandEvent.h"

#include "graphic/glfw/window/NativeWindow.h"
#include "imgui_impl_glfw.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{
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

    // 创建vk实例
    m_vkInstance = vk::createInstance(m_vkInstanceCreateInfo).value;
    XDEBUG("VK Instance created.");

    // 初始化vk动态加载器(要在创建vkInstance后)
    // (它会去找到扩展函数 如 vkCreateDebugUtilsMessengerEXT的地址)
    m_vkDldy.init(m_vkInstance, vkGetInstanceProcAddr);
    XDEBUG("VK dldy initialized.");

    if ( is_debug() ) {
        // debug模式初始化vk调试信息工具
        // 创建 Messenger 对象
        // 这里必须传入 dldy 否则会链接报错
        m_vkDebugMessenger = m_vkInstance
                                 .createDebugUtilsMessengerEXT(
                                     m_vkDebugUtilCreateInfo, nullptr, m_vkDldy)
                                 .value;
        XDEBUG("Vulkan Debug Messenger Initialize Successed");
    }

    // 初始化窗口表面和
    // 后续显卡设备初始化
    // 放在窗口相关资源初始化函数中

    MMM::Event::EventBus::instance().subscribe<MMM::Event::GLFWKeyEvent>(
        [&](MMM::Event::GLFWKeyEvent e) {
            if ( e.key == MMM::Event::Input::Key::F7 &&
                 e.action == MMM::Event::Input::Action::Press ) {
                /// @brief F7 切换垂直同步状态
                auto& config =
                    MMM::Config::AppConfig::instance().getEditorConfig();
                config.settings.vsync = !config.settings.vsync;

                // 发布配置更新事件，触发所有监听者（包括本类中的
                // LogicCommandEvent 监听器）
                MMM::Event::EventBus::instance().publish(
                    MMM::Event::LogicCommandEvent(
                        MMM::Logic::CmdUpdateEditorConfig{ config }));

                // 持久化配置
                MMM::Config::AppConfig::instance().save();

                XDEBUG("VSync toggled by shortcut: {}",
                       config.settings.vsync ? "ON" : "OFF");
            }
            if ( e.key == MMM::Event::Input::Key::F11 &&
                 e.action == MMM::Event::Input::Action::Press ) {
                /// @brief F11 切换全屏
                ToggleFullscreen();
            }
        });

    MMM::Event::EventBus::instance().subscribe<MMM::Event::LogicCommandEvent>(
        [&](MMM::Event::LogicCommandEvent e) {
            if ( std::holds_alternative<MMM::Logic::CmdUpdateEditorConfig>(
                     e.command) ) {
                auto& cmd =
                    std::get<MMM::Logic::CmdUpdateEditorConfig>(e.command);
                setVSync(cmd.config.settings.vsync);
                applyTheme();
            }
        });
}

VKContext::~VKContext()
{
    if ( m_vkLogicalDevice ) {
        (void)m_vkLogicalDevice.waitIdle();
    }

    // 在关闭 ImGui之前必须先释放渲染器中imgui对纹理的引用！
    m_vkRenderer->releaseCursorManager();

    // 在销毁 DescriptorPool 和 Device 之前必须先关闭 ImGui！
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    XDEBUG("ImGui Destroyed.");

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
        XDEBUG("VK Logical Device destroyed.");
    }

    // 销毁vk表面
    if ( m_vkSurface ) m_vkInstance.destroySurfaceKHR(m_vkSurface);
    XDEBUG("VK Surface destroyed.");

    // 销毁可能的vk调试信息工具实例
    if ( is_debug() && m_vkDebugMessenger ) {
        m_vkInstance.destroyDebugUtilsMessengerEXT(
            m_vkDebugMessenger, nullptr, m_vkDldy);
        XDEBUG("VK Debug Messenger destroyed.");
    }
    // 销毁vk实例
    m_vkInstance.destroy();
    XDEBUG("VK Instance destroyed.");

    // 释放GLFW上下文
    releaseGLFW();
}

/**
 * @brief 使用imgui自动选择物理设备和队列族
 */
void VKContext::imguiAutoSelect()
{
    // Select Physical Device (GPU)
    m_vkPhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(m_vkInstance);
    IM_ASSERT(m_vkPhysicalDevice != VK_NULL_HANDLE);

    // Select graphics queue family
    auto queueFamily =
        ImGui_ImplVulkanH_SelectQueueFamilyIndex(m_vkPhysicalDevice);
    IM_ASSERT(queueFamily != (uint32_t)-1);

    m_queueFamilyIndices.graphicsQueueIndex = queueFamily;
    m_queueFamilyIndices.presentQueueIndex  = queueFamily;
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

    // 初始化vk表面句柄
    // C 风格的 Surface 创建（GLFW 提供的快捷函数）
    VkSurfaceKHR surface;
    if ( glfwCreateWindowSurface(m_vkInstance,
                                 native_window_ptr->getWindowHandle(),
                                 nullptr,
                                 &surface) != VK_SUCCESS ) {
        throw std::runtime_error("Failed to create window surface!");
    }

    // 转换为 vk::SurfaceKHR
    m_vkSurface = surface;
    XDEBUG("Vulkan Surface created.");

    // 使用imgui自动选择物理设备和队列族
    imguiAutoSelect();

    // 在创建交换链之前，根据配置预设全局呈现模式，避免启动后再次重建
    updateGlobalPresentMode(
        Config::AppConfig::instance().getEditorSettings().vsync);

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
    // m_vkRenderer->onSwapchainChanged();

    XDEBUG("Swapchain recreation finished.");
}

/**
 * @brief 切换垂直同步
 */
void VKContext::setVSync(bool enabled)
{
    // 1. 等待设备空闲，因为要修改交换链
    (void)m_vkLogicalDevice.waitIdle();

    // 2. 修改交换链配置类里的 PresentMode 偏好
    updateGlobalPresentMode(enabled);

    // 3. 标记需要重建
    if ( m_swapchain ) {
        m_swapchain->markDirty();
    }
}

/**
 * @brief 仅更新全局呈现模式参数，不触发重建
 */
void VKContext::updateGlobalPresentMode(bool enabled)
{
    if ( enabled ) {
        VKSwapchain::s_globalPresentMode = vk::PresentModeKHR::eFifo;
    } else {
        // fallback 为立即模式
        VKSwapchain::s_globalPresentMode = vk::PresentModeKHR::eImmediate;
        // 查询物理设备支持的呈现模式
        if ( m_vkPhysicalDevice && m_vkSurface ) {
            std::vector<vk::PresentModeKHR> supported_presentModes =
                m_vkPhysicalDevice.getSurfacePresentModesKHR(m_vkSurface).value;
            for ( const auto& presentMode : supported_presentModes ) {
                // 无限帧数优选mailbox模式
                // 直接取当前时刻gpu产出的最新的图像用于绘制(刷新率高且不撕裂)
                if ( presentMode == vk::PresentModeKHR::eMailbox ) {
                    VKSwapchain::s_globalPresentMode =
                        vk::PresentModeKHR::eMailbox;
                    break;
                }
            }
        }
    }
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

}  // namespace MMM::Graphic
