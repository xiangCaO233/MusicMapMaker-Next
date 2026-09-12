#include "graphic/imguivk/VKContext.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/input/MMMInput.h"
#include "event/input/glfw/GLFWKeyEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/glfw/window/adapters/IWindowFrameAdapter.h"
#include "graphic/imguivk/VKRenderPass.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "graphic/system/SystemTheme.h"
#include "graphic/theme/ImGuiThemeRegistry.h"
#include "imgui.h"

#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"

#include <fmt/format.h>
#include <utility>

namespace MMM::Graphic
{
namespace
{
/// @brief 将物理设备类型格式化为短文本。
///
/// 返回值只用于启动诊断和日志，不参与设备排序。未知的未来枚举值统一收敛为
/// unknown，避免诊断格式依赖 Vulkan-Hpp 的流输出实现。
///
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

/// @brief 单个物理设备的可用性与优先级选择结果。
///
/// 该临时值只保存 Vulkan 非拥有句柄、队列索引及诊断文本。候选设备必须同时具备
/// 图形和呈现队列才算有效；独立显卡标志只决定有效候选之间的偏好。
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
 * @brief 获取进程内唯一的 Vulkan 图形上下文。
 *
 * 首次调用会同步构造上下文并完成不依赖窗口的 GLFW、Vulkan instance 初始化。
 * 构造函数不抛出异常，而是把首个失败原因保存在实例中；后续调用稳定返回同一
 * 错误，不会重复尝试创建半初始化的全局资源。
 *
 * @return 初始化成功时返回上下文引用，否则返回可记录的失败原因。
 * @warning 启动低频路径：首次调用会加载窗口系统与 Vulkan loader，不得从渲染
 * 循环、音频线程或并发工作线程调用。
 */
std::expected<std::reference_wrapper<VKContext>, std::string> VKContext::get()
{
    // 函数局部静态对象提供线程安全的一次性构造和进程退出时的确定析构顺序。
    static VKContext vkContext;
    // 保留失败实例而不是销毁重试，防止 GLFW/Vulkan 部分初始化反复产生不同状态。
    if ( vkContext.hasInitializationError() ) {
        return std::unexpected<std::string>(vkContext.m_initializationError);
    }
    return vkContext;
}

/// @brief 保存上下文初始化链中的首个失败原因。
///
/// 后续清理或派生步骤可能继续报告错误，但首因最能解释上下文为何不可用，因此
/// 一旦非空就不再覆盖。调用方随后通过 hasInitializationError 中止剩余阶段。
///
/// @param message 面向上层错误通道的稳定失败说明。
void VKContext::failInitialization(std::string message)
{
    if ( m_initializationError.empty() ) {
        m_initializationError = std::move(message);
    }
}

/// @brief 初始化不依赖具体窗口尺寸的 GLFW 与 Vulkan instance 资源。
///
/// 构造按依赖顺序完成 GLFW、instance 扩展、Debug layer、instance 和动态扩展
/// 分派器；surface、物理设备、逻辑设备及交换链留给 initVKWindowRess。任一步骤
/// 失败都会记录错误并提前返回，使 get() 暴露失败而非半可用上下文。
///
/// 末尾订阅快捷键和配置命令，但回调会再次检查 Application 模式，Bootstrap
/// 窗口不会触发皮肤、全屏或呈现模式变更。
///
/// @warning 进程启动低频路径：包含窗口系统初始化、Vulkan instance 创建和事件
/// 订阅，只能由单例首次构造执行。
VKContext::VKContext() : m_themeRegistry(std::make_unique<ImGuiThemeRegistry>())
{
    // GLFW 必须先成功，后续才能查询平台要求的 Vulkan surface 扩展。
    initGLFW();
    if ( hasInitializationError() ) return;

    // 扩展名容器必须在 InstanceCreateInfo 引用它之前完成填充。
    registerGLFWExtensions();
    if ( hasInitializationError() ) return;

    // Debug 构建把 debug utils 和 validation layer 作为必需启动能力；Release
    // 不请求这些开发环境依赖。
    if ( is_debug() ) {
        enableVKDebugExt();
        enableVKValidateLayer();
        if ( hasInitializationError() ) return;
    }

    // 应用信息与 instance 创建参数都是上下文成员，保证内部指针跨过创建调用。
    initVkAppInfo();

    initVkInstanceCreateInfo();
    // 在真正创建前记录完整请求，loader 返回失败时可与可用扩展列表交叉检查。
    collectVulkanInstanceCreateDiagnostics();

    // Vulkan-Hpp 配置为无异常模式，必须同时检查 result 并只在成功后发布句柄。
    auto instanceResult = vk::createInstance(m_vkInstanceCreateInfo);
    if ( instanceResult.result != vk::Result::eSuccess ) {
        addStartupDiagnostic(fmt::format("vkCreateInstance failed: {}",
                                         vk::to_string(instanceResult.result)));
        logStartupDiagnostics("vkCreateInstance failed.");
        releaseGLFW();
        failInitialization("Fatal: Failed to create Vulkan instance.");
        return;
    }
    m_vkInstance = instanceResult.value;
    addStartupDiagnostic("Vulkan instance created successfully.");
    XDEBUG("VK Instance created.");
    collectPhysicalDeviceDiagnostics(false);

    // Debug Utils 属于 instance 扩展，动态分派器只能在 instance
    // 创建后解析入口； 即使 Release 不建立
    // messenger，其余扩展调用也共享这份分派状态。
    m_vkDldy.init(m_vkInstance, vkGetInstanceProcAddr);
    XDEBUG("VK dldy initialized.");

    if ( is_debug() ) {
        // 创建参数已通过 instance pNext 捕获创建阶段消息；正式 messenger 接管
        // instance 生命周期剩余阶段。显式传入动态分派器以调用扩展入口。
        auto debugMessengerResult = m_vkInstance.createDebugUtilsMessengerEXT(
            m_vkDebugUtilCreateInfo, nullptr, m_vkDldy);
        if ( debugMessengerResult.result != vk::Result::eSuccess ) {
            addStartupDiagnostic(
                fmt::format("vkCreateDebugUtilsMessengerEXT failed: {}",
                            vk::to_string(debugMessengerResult.result)));
            logStartupDiagnostics("vkCreateDebugUtilsMessengerEXT failed.");
            release();
            releaseGLFW();
            failInitialization(
                "Fatal: Failed to create Vulkan debug messenger.");
            return;
        }
        m_vkDebugMessenger = debugMessengerResult.value;
        addStartupDiagnostic("Vulkan debug messenger created successfully.");
        XDEBUG("Vulkan Debug Messenger Initialize Successed");
    }

    // 此处刻意不选择物理设备：呈现队列能力依赖真实 surface，必须等原生窗口
    // 创建后再由 initVKWindowRess 判断。

    // 键盘订阅与上下文同生命周期。回调只在完整 Application 模式处理快捷键，
    // 启动同步窗口使用 Bootstrap 模式时不会读取尚未就绪的编辑器资源。
    m_glfwKeySubscription =
        MMM::Event::EventBus::instance().subscribe<MMM::Event::GLFWKeyEvent>(
            [this](const MMM::Event::GLFWKeyEvent& e) {
                if ( !m_windowResourcesInitialized ||
                     m_windowResourceMode !=
                         VKWindowResourceMode::Application ) {
                    return;
                }
                if ( e.key == MMM::Event::Input::Key::F7 &&
                     e.action == MMM::Event::Input::Action::Press ) {
                    // F7 按枚举的五个连续值循环，并通过统一配置命令触发实际
                    // swapchain 呈现模式更新，避免快捷键形成旁路状态。
                    auto& config =
                        MMM::Config::AppConfig::instance().getEditorConfig();
                    int currentLimit =
                        static_cast<int>(config.settings.frameLimit);
                    currentLimit = (currentLimit + 1) % 5;
                    config.settings.frameLimit =
                        static_cast<MMM::Config::FrameLimitPreference>(
                            currentLimit);

                    // 先发布完整配置快照，使包括本上下文在内的运行时监听者立即
                    // 应用新值，再持久化相同状态。
                    MMM::Event::EventBus::instance().publish(
                        MMM::Event::LogicCommandEvent(
                            MMM::Logic::CmdUpdateEditorConfig{ config }));

                    // 保存是用户快捷键动作的低频 I/O，不位于逐帧渲染路径。
                    MMM::Config::AppConfig::instance().save();

                    // 显示名称顺序必须与 FrameLimitPreference
                    // 数值顺序一致，才能 使用 currentLimit
                    // 同时索引配置和本地化文本。
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
                    // 全屏切换由 NativeWindow
                    // 保存和恢复窗口几何，本上下文只负责
                    // 标记交换链，并在切换后从 monitor 绑定状态生成反馈文本。
                    ToggleFullscreen();

                    bool isFullscreen =
                        (glfwGetWindowMonitor(
                             m_nativeWindow_ptr->getWindowHandle()) != nullptr);
                    showCenterNotification(
                        isFullscreen
                            ? TR("ui.settings.software.screen.fullscreen")
                                  .data()
                            : TR("ui.settings.software.screen.windowed")
                                  .data());
                }
            });

    // 配置命令订阅把所有入口汇合到同一呈现模式与主题应用流程。事件中携带的是
    // 已更新快照，避免回调重新读取可能处于保存过程中的配置对象。
    m_logicCommandSubscription =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::LogicCommandEvent>(
                [this](const MMM::Event::LogicCommandEvent& e) {
                    if ( !m_windowResourcesInitialized ||
                         m_windowResourceMode !=
                             VKWindowResourceMode::Application ) {
                        return;
                    }
                    if ( std::holds_alternative<
                             MMM::Logic::CmdUpdateEditorConfig>(e.command) ) {
                        const auto& cmd =
                            std::get<MMM::Logic::CmdUpdateEditorConfig>(
                                e.command);
                        // 呈现模式只在实际变化时同步重建；主题应用根据最新配置
                        // 更新 ImGui 样式和插件覆盖。
                        setFrameLimitPresentMode(
                            cmd.config.settings.frameLimit);
                        applyTheme();
                    }
                });
}

/// @brief 按 Vulkan 依赖顺序释放上下文，再终止进程级 GLFW。
/// @warning 进程退出路径：release 可能等待 GPU idle，析构期间不得再有窗口或
/// 渲染线程访问单例。
VKContext::~VKContext()
{
    release();

    // GLFW 光标和窗口系统最后退出，确保所有使用其创建 surface 的 Vulkan 对象
    // 已经销毁。
    releaseGLFW();
}

/// @brief 幂等释放窗口、ImGui、Vulkan device 与 instance 资源。
///
/// 函数既供显式关闭使用，也由析构调用。事件订阅始终优先取消；GPU 资源则按
/// “使用者先于被使用者”逆序销毁。初始化中途失败时所有句柄检查允许安全进入
/// 相同清理路径。
///
/// @warning 退出低频路径：逻辑设备存在时会执行 waitIdle，调用前必须停止提交新
/// 帧，且不得从渲染热路径调用。
void VKContext::release()
{
    // 先切断外部事件入口，即使此前已释放 GPU，重复调用也不会遗留订阅。
    if ( m_glfwKeySubscription != 0 ) {
        MMM::Event::EventBus::instance().unsubscribe<MMM::Event::GLFWKeyEvent>(
            m_glfwKeySubscription);
        m_glfwKeySubscription = 0;
    }
    if ( m_logicCommandSubscription != 0 ) {
        MMM::Event::EventBus::instance()
            .unsubscribe<MMM::Event::LogicCommandEvent>(
                m_logicCommandSubscription);
        m_logicCommandSubscription = 0;
    }

    // 订阅清理完成后再检查幂等标志，覆盖部分初始化和重复 release 两种情况。
    if ( m_isReleased ) return;

    if ( m_vkLogicalDevice ) {
        // 下方资源可能仍被已提交命令引用，统一 idle 后才能按所有权逆序销毁。
        (void)m_vkLogicalDevice.waitIdle();
    }

    XDEBUG("Starting VKContext explicit release...");

    // 软件光标纹理向 ImGui 注册 descriptor，必须在 ImGui Vulkan
    // 后端关闭前释放。
    if ( m_vkRenderer ) {
        m_vkRenderer->releaseCursorManager();
    }

    // ImGui Vulkan 后端借用渲染器 descriptor pool 和 device；GLFW
    // 后端借用窗口， 因此两者都要在渲染器、device 与 GLFW 之前关闭。
    if ( ImGui::GetCurrentContext() ) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        XDEBUG("ImGui Destroyed.");
    }

    // 渲染器析构释放 descriptor pool、同步对象和 command pool。
    m_vkRenderer.reset();

    if ( m_swapchain ) {
        // framebuffer 引用 render pass 与交换链 image view，必须先单独销毁。
        m_swapchain->destroyFramebuffers();
    }

    // render pass 不再被 framebuffer 使用后可释放，随后交换链清理 image view
    // 和 swapchain 句柄。
    m_vkRenderPass.reset();

    // 销毁交换链
    m_swapchain.reset();

    // 所有 device 子对象均已释放，逻辑设备现在可以安全销毁。
    if ( m_vkLogicalDevice ) {
        m_vkLogicalDevice.destroy();
        m_vkLogicalDevice = nullptr;
        XDEBUG("VK Logical Device destroyed.");
    }

    // surface 属于 instance，并且只在交换链与逻辑设备不再使用后销毁。
    if ( m_vkSurface ) {
        m_vkInstance.destroySurfaceKHR(m_vkSurface);
        m_vkSurface = nullptr;
        XDEBUG("VK Surface destroyed.");
    }

    // messenger 使用 instance 扩展入口销毁，必须早于其所属 instance。
    if ( is_debug() && m_vkDebugMessenger ) {
        m_vkInstance.destroyDebugUtilsMessengerEXT(
            m_vkDebugMessenger, nullptr, m_vkDldy);
        m_vkDebugMessenger = nullptr;
        XDEBUG("VK Debug Messenger destroyed.");
    }

    // instance 是 Vulkan 对象树根节点，放在资源释放链最后。
    if ( m_vkInstance ) {
        m_vkInstance.destroy();
        m_vkInstance = nullptr;
        XDEBUG("VK Instance destroyed.");
    }

    // 清除外部观察状态并封存幂等标志，后续调用不会尝试重新使用旧窗口指针。
    m_windowResourcesInitialized = false;
    m_nativeWindow_ptr           = nullptr;
    m_isReleased                 = true;
    XINFO("VKContext resources released.");
}

/// @brief 选择同时支持图形与当前 surface 呈现的物理设备和队列族。
///
/// 枚举顺序只用于建立第一个有效后备候选；遇到有效独立显卡立即优先选择。设备
/// 不要求图形和呈现位于同一队列族，后续逻辑设备和交换链会据索引选择共享模式。
/// surface 尚未建立的诊断场景把图形能力视为呈现后备，但正常窗口初始化会在
/// surface 创建后调用本函数并查询真实 present support。
///
/// @warning 窗口资源初始化路径：会枚举物理设备、队列族并查询 surface 支持，
/// 禁止从逐帧渲染路径调用。
void VKContext::imguiAutoSelect()
{
    // 无异常 Vulkan-Hpp 返回显式 result；空列表与枚举失败都无法继续创建
    // device。
    auto devicesResult = m_vkInstance.enumeratePhysicalDevices();
    if ( devicesResult.result != vk::Result::eSuccess ||
         devicesResult.value.empty() ) {
        addStartupDiagnostic(
            fmt::format("Physical device selection failed: result={}, count={}",
                        vk::to_string(devicesResult.result),
                        devicesResult.value.size()));
        logStartupDiagnostics("No usable Vulkan physical device.");
        failInitialization(
            "Fatal: No usable Vulkan physical device was found.");
        return;
    }

    DeviceSelection fallbackSelection{};
    DeviceSelection preferredSelection{};
    // 每个设备独立收集索引，防止前一候选的部分队列结果泄漏到下一候选。
    for ( const auto& device : devicesResult.value ) {
        DeviceSelection selection{};
        selection.device = device;

        const auto properties = device.getProperties();
        selection.deviceName  = properties.deviceName.data();
        selection.isDiscreteGpu =
            properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;

        // 只记录每种角色遇到的第一个队列族，当前渲染器每种角色只申请一条队列。
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

            // 正常窗口路径必须由 surface 查询确认呈现；无 surface
            // 只用于提前诊断，
            // 暂以图形族作为候选，不能替代最终窗口绑定后的选择。
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
            // 诊断同时记录两种角色是否存在，便于区分纯计算设备与窗口后端不支持。
            addStartupDiagnostic(fmt::format(
                "Rejected GPU \"{}\": graphicsQueue={}, presentQueue={}",
                selection.deviceName,
                selection.indices.graphicsQueueIndex.has_value(),
                selection.indices.presentQueueIndex.has_value()));
            continue;
        }

        // 首个有效设备保证集成显卡或软件实现仍能启动；独立显卡只覆盖性能偏好。
        if ( !fallbackSelection.isValid() ) {
            fallbackSelection = selection;
        }
        if ( selection.isDiscreteGpu ) {
            preferredSelection = selection;
            break;
        }
    }

    // 只有有效独立候选才覆盖后备，避免因设备类型偏好接受缺失队列的设备。
    const DeviceSelection selected =
        preferredSelection.isValid() ? preferredSelection : fallbackSelection;
    if ( !selected.isValid() ) {
        logStartupDiagnostics(
            "No Vulkan physical device has graphics and present queues.");
        failInitialization(
            "Fatal: No Vulkan physical device has graphics and present "
            "queues.");
        return;
    }

    // 候选验证完成后一次性发布设备与配套索引，后续逻辑设备创建看到一致状态。
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
 * @brief 为指定原生窗口初始化 surface、device、交换链、渲染器与 ImGui。
 *
 * 基础 instance 已由构造函数建立。本函数把上下文一次性绑定到一个 NativeWindow，
 * 并按依赖顺序创建其余窗口资源。相同窗口和模式的重复调用幂等成功，但不支持把
 * 已初始化上下文重新绑定到另一窗口，也不在原地把 Bootstrap 提升为 Application。
 *
 * @param native_window_ptr 提供 GLFW 窗口与平台窗框适配器的原生窗口。
 * @param w 初始 framebuffer 宽度（像素）。
 * @param h 初始 framebuffer 高度（像素）。
 * @param mode 启动最小资源或完整应用资源的初始化模式。
 * @return 全部窗口资源就绪或相同绑定已经就绪时返回 true，否则返回 false。
 * @warning 启动低频路径：创建 Vulkan surface/device/swapchain、同步对象和 ImGui
 * 后端，包含大量资源分配，禁止从渲染循环调用。
 */
bool VKContext::initVKWindowRess(NativeWindow* native_window_ptr, int w, int h,
                                 VKWindowResourceMode mode)
{
    // 基础 instance 初始化已经失败时不再触碰窗口，保留原始失败原因。
    if ( hasInitializationError() ) {
        return false;
    }

    if ( m_windowResourcesInitialized ) {
        // 上下文不实现资源树的热迁移；只有完全相同的窗口和模式可视为幂等调用。
        if ( native_window_ptr != m_nativeWindow_ptr ||
             mode != m_windowResourceMode ) {
            XERROR("Vulkan window resources cannot be rebound or promoted");
            return false;
        }
        return true;
    }

    // 先记录诊断请求，但在解引用前验证窗口封装和底层 GLFW 句柄。
    m_nativeWindow_ptr = native_window_ptr;
    addStartupDiagnostic(
        fmt::format("Initializing Vulkan window resources: {}x{}", w, h));

    if ( !native_window_ptr || !native_window_ptr->getWindowHandle() ) {
        addStartupDiagnostic("Native window handle is null.");
        logStartupDiagnostics("Native window handle is null.");
        failInitialization(
            "Failed to initialize Vulkan window resources: "
            "native window is null.");
        return false;
    }

    // GLFW 的 C API 根据当前平台创建对应 VkSurfaceKHR；成功后立即包装为
    // Vulkan-Hpp 句柄并纳入上下文释放顺序。
    VkSurfaceKHR   surface;
    const VkResult surfaceResult = glfwCreateWindowSurface(
        m_vkInstance, native_window_ptr->getWindowHandle(), nullptr, &surface);
    if ( surfaceResult != VK_SUCCESS ) {
        addStartupDiagnostic(
            fmt::format("glfwCreateWindowSurface failed: {}",
                        vk::to_string(static_cast<vk::Result>(surfaceResult))));
        collectLastGLFWErrorDiagnostic("glfwCreateWindowSurface failed.");
        logStartupDiagnostics("glfwCreateWindowSurface failed.");
        failInitialization("Failed to create window surface!");
        return false;
    }

    // 从此处开始物理设备选择可以查询真实窗口呈现能力。
    m_vkSurface = surface;
    // MoltenVK 会在创建 Surface 时安装实际的 CAMetalLayer，必须在此后重新
    // 应用 macOS 圆角裁剪和原生外扩散阴影。
    if ( auto* frameAdapter = native_window_ptr->getWindowFrameAdapter() ) {
        frameAdapter->refreshFrameShape();
    }
    addStartupDiagnostic("Vulkan window surface created successfully.");
    XDEBUG("Vulkan Surface created.");
    collectPhysicalDeviceDiagnostics(true);

    // 设备候选必须同时满足图形与当前 surface 的呈现要求。
    imguiAutoSelect();
    if ( hasInitializationError() ) {
        return false;
    }
    collectSelectedSurfaceDiagnostics(w, h);

    // 在首次交换链创建前确定呈现模式，避免启动完成后为同一配置立即重建一次。
    updateGlobalPresentMode(
        Config::AppConfig::instance().getEditorSettings().frameLimit);

    // 逻辑设备依据刚选定的队列族和必需 device 扩展创建。
    initLogicDevice();
    if ( hasInitializationError() ) {
        return false;
    }

    // 交换链拥有呈现图像视图，实际 extent 会按 surface 能力钳制传入尺寸。
    m_swapchain = std::make_unique<VKSwapchain>(m_vkPhysicalDevice,
                                                m_vkLogicalDevice,
                                                m_vkSurface,
                                                m_queueFamilyIndices,
                                                w,
                                                h);
    // 最终 render pass 的颜色附件在结束时转换为 present
    // 布局，与交换链格式绑定。
    m_vkRenderPass = std::make_unique<VKRenderPass>(
        m_vkLogicalDevice, *m_swapchain, vk::ImageLayout::ePresentSrcKHR);

    // framebuffer 同时引用 render pass 与每张交换链 image
    // view，必须在二者之后。
    m_swapchain->createFramebuffers(*m_vkRenderPass);

    // 渲染器借用上下文、交换链、render pass 和队列，所有者仍是当前 VKContext。
    m_vkRenderer = std::make_unique<VKRenderer>(*this,
                                                m_vkPhysicalDevice,
                                                m_vkLogicalDevice,
                                                *m_swapchain,
                                                *m_vkRenderPass,
                                                m_LogicDeviceGraphicsQueue,
                                                m_LogicDevicePresentQueue);

    if ( mode == VKWindowResourceMode::Application ) {
        // 软件光标依赖皮肤贴图，启动期资源同步完成前不能创建。
        m_vkRenderer->initCursorManager(m_vkPhysicalDevice, m_vkLogicalDevice);
    }

    // ImGui 最后初始化，因为其 Vulkan 后端需要已经存在的 device、队列、render
    // pass 和渲染器 descriptor pool。
    imguiVulkanInit(native_window_ptr->getWindowHandle(), mode);
    // 仅在完整初始化链返回后发布就绪标志，事件回调不会观察到半初始化资源。
    m_windowResourceMode         = mode;
    m_windowResourcesInitialized = true;
    return true;
}

/// @brief 按窗口当前 framebuffer 尺寸重建全部交换链尺寸相关资源。
///
/// 最小化窗口可能报告零尺寸，此时阻塞等待 GLFW 事件直到恢复；随后等待 GPU
/// idle， 重建交换链图像视图与
/// framebuffer，并通知渲染器更新按图像索引的同步对象。 device、render pass
/// 和主渲染器本身保持不变。
///
/// @param window_context 用于查询恢复后 framebuffer 尺寸的 GLFW 窗口。
/// @param width 已知 framebuffer 宽度；为零时进入最小化等待。
/// @param height 已知 framebuffer 高度；为零时进入最小化等待。
/// @warning 不可中断的低频路径：可能等待窗口恢复并执行 device waitIdle，只能在
/// 交换链失效或窗口尺寸变化后调用。
void VKContext::recreateSwapchain(GLFWwindow* window_context, int width,
                                  int height)
{
    // Vulkan 不允许创建零 extent 交换链；等待事件可休眠线程而不是忙等轮询。
    while ( width == 0 || height == 0 ) {
        glfwGetFramebufferSize(window_context, &width, &height);
        glfwWaitEvents();
    }

    // 旧 framebuffer、image view 和同步对象仍可能被在途提交引用。
    (void)m_vkLogicalDevice.waitIdle();

    // VKSwapchain 内部保留旧句柄供驱动迁移资源，并只替换图像相关对象；逻辑设备
    // 与 render pass 的附件契约没有变化。
    m_swapchain->recreate(
        m_vkPhysicalDevice, m_vkSurface, m_queueFamilyIndices, width, height);

    // 新 image view 需要重新与既有 render pass 组合成一一对应的 framebuffer。
    m_swapchain->createFramebuffers(*m_vkRenderPass);

    // 图像数量可能随呈现模式或 surface 能力变化，渲染器必须重建按图像索引的
    // render-finished semaphore 集合。
    m_vkRenderer->onSwapchainChanged();

    XDEBUG("Swapchain recreation finished.");
}

/// @brief 根据帧率限制策略更新呈现模式并请求下一帧重建交换链。
/// @param frameLimit 新的帧率限制偏好。
/// @warning 低频设置路径：模式实际变化时执行 device
/// waitIdle；只标记交换链，实际 重建由渲染流程在安全边界完成。
void VKContext::setFrameLimitPresentMode(
    Config::FrameLimitPreference frameLimit)
{
    // 把用户偏好收敛为当前 surface 实际支持的模式，等效结果无需重建。
    auto nextPresentMode = selectPresentMode(frameLimit);
    if ( VKSwapchain::s_globalPresentMode == nextPresentMode ) {
        return;
    }

    // 当前交换链仍在使用旧呈现模式，先等待已提交帧完成再发布重建请求。
    (void)m_vkLogicalDevice.waitIdle();

    // 全局偏好会由 VKSwapchain::createInternal 写入下一份创建信息。
    VKSwapchain::s_globalPresentMode = nextPresentMode;

    // 启动早期交换链尚不存在时只保存偏好，首次创建会直接采用它。
    if ( m_swapchain ) {
        m_swapchain->markDirty();
    }
}

/// @brief 把帧率限制偏好映射到当前 surface 可用的 Vulkan 呈现模式。
///
/// VSync 始终选择规范保证支持的 FIFO。其他倍率由应用自己的帧调度控制，交换链
/// 优先 Immediate 以避免额外同步；不可用时尝试低延迟 Mailbox，最终安全回退
/// FIFO。尚未选择设备或 surface 时同样返回 FIFO。
///
/// @param frameLimit 用户配置的帧率限制偏好。
/// @return 当前设备与 surface 可用的最合适呈现模式。
vk::PresentModeKHR VKContext::selectPresentMode(
    Config::FrameLimitPreference frameLimit) const
{
    if ( frameLimit == Config::FrameLimitPreference::VSync ) {
        return vk::PresentModeKHR::eFifo;
    }

    // 只缓存选择所需的两种可选能力，不在该低频设置路径保留完整枚举结果。
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

    // 非 VSync 策略优先无队列的 Immediate；Mailbox
    // 是保持完整帧的次优低延迟模式。
    if ( supportsImmediate ) {
        return vk::PresentModeKHR::eImmediate;
    }
    if ( supportsMailbox ) {
        return vk::PresentModeKHR::eMailbox;
    }
    return vk::PresentModeKHR::eFifo;
}

/// @brief 在首次交换链创建前更新全局呈现模式，不触发重建或等待。
/// @param frameLimit 启动配置中的帧率限制偏好。
void VKContext::updateGlobalPresentMode(Config::FrameLimitPreference frameLimit)
{
    VKSwapchain::s_globalPresentMode = selectPresentMode(frameLimit);
}

/// @brief 切换原生窗口全屏状态并请求交换链适配新的 framebuffer 尺寸。
/// @warning 用户交互低频路径：NativeWindow 会操作平台窗口，实际 Vulkan 重建延迟
/// 到渲染器安全边界。
void VKContext::ToggleFullscreen()
{
    m_nativeWindow_ptr->ToggleFullscreen();
    // 全屏切换可能同时改变像素尺寸、缩放和 surface 能力，旧交换链不能继续沿用。
    m_swapchain->markDirty();
}

/// @brief 更新中央通知文本及其基于 GLFW 单调时间的过期点。
/// @param message 下一帧开始显示的通知文本。
/// @param durationSeconds 从当前时刻起保持可见的秒数。
/// @warning UI 交互路径：只更新内存状态，不创建窗口资源或执行阻塞操作。
void VKContext::showCenterNotification(const std::string& message,
                                       float              durationSeconds)
{
    m_notificationMessage = message;
    m_notificationExpireTime =
        glfwGetTime() + static_cast<double>(durationSeconds);
}

/// @brief 在主视口中央绘制当前未过期通知，并在最后 0.3 秒淡出。
///
/// 使用无输入、无保存状态的临时 ImGui 窗口承载文本，并在窗口范围外扩绘制半透明
/// 背板。函数始终保持 Begin/End 配对，即使 Begin 返回 false。
///
/// @warning 热路径：渲染循环每帧调用；禁止加入文件系统访问、阻塞等待、完整遍历
/// 或需要异常处理的操作。
void VKContext::drawCenterNotification()
{
    // 过期或空文本直接退出，常态下不建立 ImGui 窗口也不分配绘制命令。
    double currentTime = glfwGetTime();
    if ( currentTime >= m_notificationExpireTime ||
         m_notificationMessage.empty() ) {
        return;
    }

    // 可见期大部分保持不透明，只在末尾短窗口线性衰减，避免长时间降低可读性。
    float timeLeft = static_cast<float>(m_notificationExpireTime - currentTime);
    float alpha    = 1.0f;
    if ( timeLeft < 0.3f ) {
        alpha = timeLeft / 0.3f;
    }

    // 锚点设为主视口中心，pivot 取 0.5 让自动尺寸窗口围绕中心展开。
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);

    // 禁止输入、导航、焦点和 ini 持久化，通知只贡献当前帧视觉内容。
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs;

    if ( ImGui::Begin("##CenterNotification", nullptr, flags) ) {
        // ImGui 窗口本身透明，使用其 draw list 在文本边界外绘制自定义背板。
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2      minPos   = ImGui::GetWindowPos();
        ImVec2      maxPos   = ImVec2(minPos.x + ImGui::GetWindowWidth(),
                                      minPos.y + ImGui::GetWindowHeight());

        // 背景和边框共享淡出 Alpha，但保留不同基准透明度以维持层次。
        ImU32 bgColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.07f, 0.07f, 0.11f, 0.85f * alpha));
        ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.25f, 0.45f, 0.95f, 0.6f * alpha));

        // 向文本窗口四周外扩固定像素，避免额外修改全局 ImGui WindowPadding。
        float  paddingX = 24.0f;
        float  paddingY = 16.0f;
        ImVec2 bgMin    = ImVec2(minPos.x - paddingX, minPos.y - paddingY);
        ImVec2 bgMax    = ImVec2(maxPos.x + paddingX, maxPos.y + paddingY);

        drawList->AddRectFilled(bgMin, bgMax, bgColor, 12.0f);
        drawList->AddRect(bgMin, bgMax, borderColor, 12.0f, 0, 1.5f);

        // 文本颜色只在局部 style 栈生效，绘制后立即恢复调用方主题状态。
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 1.0f, alpha));
        ImGui::TextUnformatted(m_notificationMessage.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

}  // namespace MMM::Graphic
