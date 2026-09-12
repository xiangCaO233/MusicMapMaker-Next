#include "graphic/glfw/GLFWHeader.h"
#include "graphic/imguivk/VKContext.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <fmt/format.h>
#include <string_view>
#include <utility>

namespace MMM::Graphic
{
namespace
{
/// @brief 将布尔值格式化为诊断日志中的 yes/no。
///
/// 固定英文令启动日志不依赖翻译资源，而这些诊断往往正用于定位资源加载前的
/// 失败。返回静态字面量，不产生临时字符串分配。
///
/// @param value 需要格式化的布尔值。
/// @return yes 或 no 文本。
constexpr const char* yesNo(bool value)
{
    return value ? "yes" : "no";
}

/// @brief 将 Vulkan 版本号格式化为 major.minor.patch。
///
/// Vulkan 把三个版本分量编码在 uint32_t 中，必须通过规范宏提取；这里不输出
/// variant 位，因为当前诊断只比较 loader、header 和设备 API 的兼容版本。
///
/// @param version Vulkan 编码版本号。
/// @return 可读版本号文本。
std::string formatVulkanVersion(uint32_t version)
{
    return fmt::format("{}.{}.{}",
                       VK_VERSION_MAJOR(version),
                       VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

/// @brief 将 VkResult 格式化为数值诊断文本。
///
/// 同时保留 Vulkan-Hpp 枚举名称和原始整数，既方便人工阅读，也能覆盖未来头文件
/// 尚未命名的新结果码。
///
/// @param result Vulkan 结果码。
/// @return 可读结果码文本。
std::string formatVkResult(VkResult result)
{
    return fmt::format("{} ({})",
                       vk::to_string(static_cast<vk::Result>(result)),
                       static_cast<int>(result));
}

/// @brief 将物理设备类型格式化为短文本。
///
/// 该文本只用于稳定日志格式，不参与真正的设备优先级决策；未知枚举值安全回落
/// 为 unknown。
///
/// @param type Vulkan 物理设备类型。
/// @return 设备类型文本。
const char* physicalDeviceTypeName(VkPhysicalDeviceType type)
{
    switch ( type ) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "other";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "unknown";
    }
}

/// @brief 将队列能力位格式化为短文本。
///
/// 一个队列族可能同时提供多种能力，输出以竖线连接全部已知标志。未识别的新标志
/// 不会造成失败；若没有任何已知位则返回 none，便于发现纯扩展队列。
///
/// @param flags Vulkan 队列能力位。
/// @return 队列能力文本。
std::string queueFlagsText(VkQueueFlags flags)
{
    std::string result;
    // 统一处理分隔符，保证首项前没有竖线且多能力顺序固定，方便比较两次日志。
    const auto append = [&result](std::string_view flag) {
        if ( !result.empty() ) {
            result += "|";
        }
        result += flag;
    };

    // 顺序按常见渲染用途排列，不依赖枚举位的数值顺序。
    if ( (flags & VK_QUEUE_GRAPHICS_BIT) != 0 ) append("graphics");
    if ( (flags & VK_QUEUE_COMPUTE_BIT) != 0 ) append("compute");
    if ( (flags & VK_QUEUE_TRANSFER_BIT) != 0 ) append("transfer");
    if ( (flags & VK_QUEUE_SPARSE_BINDING_BIT) != 0 ) append("sparse");
    if ( (flags & VK_QUEUE_PROTECTED_BIT) != 0 ) append("protected");

    if ( result.empty() ) {
        return "none";
    }
    return result;
}

/// @brief 将 Vulkan present mode 格式化为短文本。
///
/// 只给核心 KHR 模式稳定命名，平台或扩展模式统一显示 other；原始能力数量仍会
/// 单独记录，因此未知项不会被误认为查询失败。
///
/// @param mode Vulkan 呈现模式。
/// @return 呈现模式文本。
const char* presentModeName(VkPresentModeKHR mode)
{
    switch ( mode ) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default: return "other";
    }
}

/// @brief 检查扩展列表中是否存在指定扩展。
///
/// C API 属性使用固定字符数组，以 string_view 直接比较可避免为诊断查询中的每个
/// 条目构造 std::string。
///
/// @param extensions Vulkan 扩展属性列表。
/// @param extensionName 需要查找的扩展名。
/// @return 找到扩展时返回 true。
bool hasExtension(const std::vector<VkExtensionProperties>& extensions,
                  const char*                               extensionName)
{
    return std::any_of(extensions.begin(),
                       extensions.end(),
                       [extensionName](const VkExtensionProperties& extension) {
                           return std::string_view(extension.extensionName) ==
                                  extensionName;
                       });
}

/// @brief 检查扩展列表中是否存在指定扩展。
///
/// Vulkan-Hpp 枚举路径保留独立重载，使调用点无需把包装属性转换回 C 结构；比较
/// 语义与上方 C API 重载完全一致。
///
/// @param extensions Vulkan-Hpp 扩展属性列表。
/// @param extensionName 需要查找的扩展名。
/// @return 找到扩展时返回 true。
bool hasExtension(const std::vector<vk::ExtensionProperties>& extensions,
                  const char*                                 extensionName)
{
    return std::any_of(
        extensions.begin(),
        extensions.end(),
        [extensionName](const vk::ExtensionProperties& extension) {
            return std::string_view(extension.extensionName) == extensionName;
        });
}

}  // namespace

/// @brief 把一条有序诊断追加到当前启动快照。
///
/// 诊断只在启动低频路径收集，vector 分配不会进入渲染循环。调用顺序就是最终日志
/// 顺序，便于从 loader、instance、device 一直追踪到 surface 的依赖链。
/// 文本在入队时取得完整所有权，调用方可以安全传入 fmt 生成的临时字符串。
/// 本缓存没有并发保护，因为整个启动链固定由创建图形上下文的主线程执行。
///
/// @param line 已格式化且不依赖外部生命周期的诊断文本。
void VKContext::addStartupDiagnostic(std::string line)
{
    m_startupDiagnosticLines.push_back(std::move(line));
}

/// @brief 记录当前 GLFW 版本、平台后端与最小 Vulkan 支持状态。
///
/// 本函数在 glfwInit 成功后调用，因此可以安全查询运行库版本和已选择的平台。
/// glfwVulkanSupported 是独立快照，后续失败仍会补充更具体的 loader 扩展诊断。
///
/// @warning 启动诊断路径：调用 GLFW/Vulkan 探测 API，只能在 GLFW 已初始化且尚未
/// 终止时执行。
void VKContext::collectGLFWDiagnostics()
{
    // 同时记录数值版本与 GLFW
    // 自带完整版本串，后者通常包含构建平台和编译器信息。
    int major    = 0;
    int minor    = 0;
    int revision = 0;
    glfwGetVersion(&major, &minor, &revision);

    addStartupDiagnostic(fmt::format("GLFW version: {}.{}.{} ({})",
                                     major,
                                     minor,
                                     revision,
                                     glfwGetVersionString()));
    // 平台代码反映 GLFW 实际选择的 Win32、Cocoa、X11 或 Wayland 后端。
    addStartupDiagnostic(
        fmt::format("GLFW platform code: {}", glfwGetPlatform()));

    // 该结果只表示 GLFW 能找到基本 Vulkan 支持，不证明目标 GPU 支持当前
    // surface。
    const bool vulkanSupported = glfwVulkanSupported() == GLFW_TRUE;
    addStartupDiagnostic(fmt::format("GLFW Vulkan minimally supported: {}",
                                     yesNo(vulkanSupported)));
}

/// @brief 读取并记录 GLFW 线程局部的最近一次错误。
///
/// glfwGetError 会清除该线程保存的错误状态，因此应紧跟失败调用执行。即使 GLFW
/// 没提供描述，也记录调用上下文和数值码，避免诊断快照出现无法解释的空洞。
///
/// @param context 说明哪个 GLFW 操作触发本次查询的静态上下文文本。
void VKContext::collectLastGLFWErrorDiagnostic(const char* context)
{
    // description 由 GLFW 管理，只在本次格式化调用内读取，不跨函数保存指针。
    const char* description = nullptr;
    const int   code        = glfwGetError(&description);
    if ( description ) {
        addStartupDiagnostic(
            fmt::format("{} GLFW error {}: {}", context, code, description));
        return;
    }

    // 部分平台失败不会设置具体文本；保留 code=0 也能证明已经查询过错误槽。
    addStartupDiagnostic(fmt::format(
        "{} GLFW did not report a concrete error. Last error code: {}",
        context,
        code));
}

/// @brief 采集 Vulkan header、loader、instance 扩展与 layer 的完整启动快照。
///
/// 所有查询都在 instance 创建前通过 loader 全局入口完成。扩展和 layer 使用
/// Vulkan
/// 规定的两阶段枚举：先读数量，再分配目标数组并读取内容。任一枚举失败只终止
/// 本函数剩余诊断，不直接改变上下文初始化状态；真正的能力要求由创建流程判断。
///
/// 输出同时突出 WSI surface 能力，便于区分 loader
/// 可用但缺少窗口平台扩展的环境。 Debug layer 的具体必需集合仍由
/// enableVKValidateLayer 验证。
///
/// 两阶段枚举之间系统集合理论上可能变化；当前启动阶段没有并发安装驱动的正常
/// 场景，因此使用第一次返回的容量，并忠实记录第二次调用结果。驱动若返回
/// VK_INCOMPLETE，会作为失败进入快照，不把截断列表伪装成完整能力集合。
///
/// @warning 启动低频路径：包含多次 loader 枚举与动态内存分配，禁止从渲染循环
/// 调用。
void VKContext::collectVulkanLoaderDiagnostics()
{
#ifdef VK_HEADER_VERSION_COMPLETE
    // 完整 header 版本可直接分解；旧头仅提供 patch 宏时保留有限但真实的信息。
    addStartupDiagnostic(
        fmt::format("Vulkan header version: {} (VK_HEADER_VERSION={})",
                    formatVulkanVersion(VK_HEADER_VERSION_COMPLETE),
                    VK_HEADER_VERSION));
#else
    addStartupDiagnostic(
        fmt::format("Vulkan header patch version: {}", VK_HEADER_VERSION));
#endif

    // Vulkan 1.0 loader 可能不导出 vkEnumerateInstanceVersion，默认值必须先设为
    // 规范保证的 1.0，随后通过全局 proc address 兼容探测新入口。
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    auto     enumerateInstanceVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if ( enumerateInstanceVersion ) {
        // 即使入口存在也保留 VkResult，因为损坏的 loader 仍可能拒绝版本查询。
        const VkResult result = enumerateInstanceVersion(&loaderVersion);
        addStartupDiagnostic(fmt::format(
            "Vulkan loader instance API version query: result={}, version={}",
            formatVkResult(result),
            formatVulkanVersion(loaderVersion)));
        if ( result == VK_SUCCESS ) {
            XINFO("Vulkan loader API version: {}",
                  formatVulkanVersion(loaderVersion));
        }
    } else {
        // 缺少入口是合法的 Vulkan 1.0 行为，不应单独判定初始化失败。
        addStartupDiagnostic(
            "Vulkan loader does not export "
            "vkEnumerateInstanceVersion; assuming 1.0.x.");
    }

    // 第一阶段只查询数量；失败时没有可靠容量，不能继续分配或读取扩展数组。
    uint32_t extensionCount = 0;
    VkResult result         = vkEnumerateInstanceExtensionProperties(
        nullptr, &extensionCount, nullptr);
    if ( result != VK_SUCCESS ) {
        addStartupDiagnostic(
            fmt::format("Failed to enumerate Vulkan instance extensions: {}",
                        formatVkResult(result)));
        return;
    }

    // 以首阶段数量分配精确容量，零扩展时跳过第二阶段并记录空集合。
    std::vector<VkExtensionProperties> extensions(extensionCount);
    if ( extensionCount > 0 ) {
        result = vkEnumerateInstanceExtensionProperties(
            nullptr, &extensionCount, extensions.data());
        if ( result != VK_SUCCESS ) {
            addStartupDiagnostic(
                fmt::format("Failed to read Vulkan instance extension list: {}",
                            formatVkResult(result)));
            return;
        }
    }

    // 逐项记录名称和 spec 版本，既支持人工排障，也能与请求列表精确比对。
    addStartupDiagnostic(
        fmt::format("Vulkan instance extensions reported: {}", extensionCount));
    for ( const auto& extension : extensions ) {
        addStartupDiagnostic(fmt::format("Instance extension: {} spec={}",
                                         extension.extensionName,
                                         extension.specVersion));
    }

    // 所有窗口平台都必须同时具备通用 surface 和至少一种当前系统 WSI 扩展。
    const bool hasSurface =
        hasExtension(extensions, VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(_WIN32)
    const bool hasPlatformSurface =
        hasExtension(extensions, "VK_KHR_win32_surface");
    constexpr const char* platformSurfaceName = "VK_KHR_win32_surface";
#elif defined(__APPLE__)
    const bool hasPlatformSurface =
        hasExtension(extensions, "VK_EXT_metal_surface");
    constexpr const char* platformSurfaceName = "VK_EXT_metal_surface";
#elif defined(__linux__)
    // GLFW 在 Linux 可选择 XCB、Xlib 或 Wayland，任意一种存在即可继续由 GLFW
    // 返回实际所需扩展列表。
    const bool hasPlatformSurface =
        hasExtension(extensions, "VK_KHR_xcb_surface") ||
        hasExtension(extensions, "VK_KHR_xlib_surface") ||
        hasExtension(extensions, "VK_KHR_wayland_surface");
    constexpr const char* platformSurfaceName =
        "VK_KHR_xcb_surface/VK_KHR_xlib_surface/VK_KHR_wayland_surface";
#else
    const bool            hasPlatformSurface  = false;
    constexpr const char* platformSurfaceName = "platform surface";
#endif
    addStartupDiagnostic(
        fmt::format("Vulkan WSI extension status: {}={}, {}={}",
                    VK_KHR_SURFACE_EXTENSION_NAME,
                    yesNo(hasSurface),
                    platformSurfaceName,
                    yesNo(hasPlatformSurface)));

    // Layer 与扩展独立枚举；扩展成功不代表验证层或其他显式 layer 可用。
    uint32_t layerCount = 0;
    result = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    if ( result != VK_SUCCESS ) {
        addStartupDiagnostic(
            fmt::format("Failed to enumerate Vulkan instance layers: {}",
                        formatVkResult(result)));
        return;
    }

    // 按 Vulkan 两阶段协议读取完整属性，避免依赖固定上限。
    std::vector<VkLayerProperties> layers(layerCount);
    if ( layerCount > 0 ) {
        result = vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        if ( result != VK_SUCCESS ) {
            addStartupDiagnostic(
                fmt::format("Failed to read Vulkan instance layer list: {}",
                            formatVkResult(result)));
            return;
        }
    }

    // 描述、规范和实现版本共同标识实际安装 layer，定位 SDK 与 loader 混装问题。
    addStartupDiagnostic(
        fmt::format("Vulkan instance layers reported: {}", layerCount));
    for ( const auto& layer : layers ) {
        addStartupDiagnostic(
            fmt::format("Instance layer: {} spec={} impl={} desc={}",
                        layer.layerName,
                        formatVulkanVersion(layer.specVersion),
                        layer.implementationVersion,
                        layer.description));
    }
}

/// @brief 记录即将交给 vkCreateInstance 的 API、扩展与验证层请求。
///
/// 该快照与 loader 可用能力分开保存，失败日志可以直接比较“系统报告什么”和
/// “应用请求什么”。空扩展指针也会显式写为占位文本，而不是在诊断路径解引用。
///
/// @warning 启动诊断路径：必须在所有扩展和 layer 注册完成后、创建 instance 前
/// 调用。
void VKContext::collectVulkanInstanceCreateDiagnostics()
{
    // 应用 API 版本来自持久化的 ApplicationInfo，与实际创建参数完全一致。
    addStartupDiagnostic(
        fmt::format("Requested Vulkan application API version: {}",
                    formatVulkanVersion(m_vkAppInfo.apiVersion)));
    addStartupDiagnostic(fmt::format("Requested Vulkan instance extensions: {}",
                                     m_vkExtensions.size()));
    // 保持容器顺序，便于发现重复注册或平台扩展追加顺序变化。
    for ( const char* extension : m_vkExtensions ) {
        addStartupDiagnostic(fmt::format("Requested instance extension: {}",
                                         extension ? extension : "<null>"));
    }

    // Release 不请求 validation layer，因此不制造“未记录 layer”的歧义。
    if ( is_debug() ) {
        for ( const char* layer : m_vkValidationLayers ) {
            addStartupDiagnostic(
                fmt::format("Requested validation layer: {}", layer));
        }
    }
}

/// @brief 记录 instance 可见的全部物理设备、扩展、驱动与队列族能力。
///
/// 设备基础属性和 feature 使用 Vulkan 1.0 核心入口，驱动详细属性仅在 API 版本或
/// KHR 扩展证明支持时通过 properties2 链查询。队列族始终记录能力与数量；只有
/// includeSurfaceSupport 为 true 且 surface 有效时才额外查询实际呈现能力。
///
/// 本函数用于诊断候选全集，不执行设备排序，也不修改 m_vkPhysicalDevice。单个
/// 设备的可选扩展或驱动属性查询失败不会阻止继续记录其余设备。
///
/// 基础 feature 列表仅解释固定管线可能遇到的能力差异，不代表应用已经请求启用
/// 这些 feature。DeviceCreateInfo 的真实请求由逻辑设备诊断另行记录。驱动原始
/// 版本号由厂商编码，因此同时输出十进制与十六进制，不在此猜测厂商专用分量。
///
/// @param includeSurfaceSupport 是否为每个队列族查询当前窗口 surface 呈现支持。
/// @warning 启动低频路径：可能遍历全部 GPU、扩展和队列族并进行多次驱动调用，
/// 禁止从渲染循环调用。
void VKContext::collectPhysicalDeviceDiagnostics(bool includeSurfaceSupport)
{
    // instance 不存在时没有合法的物理设备查询入口；记录跳过原因而非静默返回。
    if ( !m_vkInstance ) {
        addStartupDiagnostic(
            "Physical device diagnostics skipped: Vulkan instance is null.");
        return;
    }

    // 无异常 Vulkan-Hpp 通过 result/value 返回，失败值下不得读取设备数组。
    auto devicesResult = m_vkInstance.enumeratePhysicalDevices();
    if ( devicesResult.result != vk::Result::eSuccess ) {
        addStartupDiagnostic(
            fmt::format("Failed to enumerate physical devices: {}",
                        vk::to_string(devicesResult.result)));
        return;
    }

    // 借用 result 容器避免复制句柄数组，引用只在本函数作用域内有效。
    const auto& devices = devicesResult.value;
    addStartupDiagnostic(
        fmt::format("Vulkan physical devices reported: {}", devices.size()));
    XINFO("Vulkan physical devices detected: {}", devices.size());

    // 索引稳定对应后续每条 GPU[...] 日志，便于把属性、扩展和队列族重新归组。
    for ( size_t deviceIndex = 0; deviceIndex < devices.size();
          ++deviceIndex ) {
        // 基础属性使用 C API 结构，能直接复用 driver properties pNext
        // 结构与数值 格式化工具，不改变 Vulkan-Hpp 持有的设备句柄所有权。
        const VkPhysicalDevice rawDevice =
            static_cast<VkPhysicalDevice>(devices[deviceIndex]);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(rawDevice, &properties);

        // 只摘取项目可能关注的固定管线能力，不把庞大 feature 表逐字段写入日志。
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(rawDevice, &features);

        addStartupDiagnostic(
            fmt::format("GPU[{}]: name=\"{}\", type={}, api={}, driverRaw={} "
                        "(0x{:08X}), vendor=0x{:04X}, device=0x{:04X}",
                        deviceIndex,
                        properties.deviceName,
                        physicalDeviceTypeName(properties.deviceType),
                        formatVulkanVersion(properties.apiVersion),
                        properties.driverVersion,
                        properties.driverVersion,
                        properties.vendorID,
                        properties.deviceID));
        addStartupDiagnostic(fmt::format(
            "GPU[{}] features: geometryShader={}, samplerAnisotropy={}, "
            "wideLines={}, fillModeNonSolid={}, sampleRateShading={}",
            deviceIndex,
            yesNo(features.geometryShader == VK_TRUE),
            yesNo(features.samplerAnisotropy == VK_TRUE),
            yesNo(features.wideLines == VK_TRUE),
            yesNo(features.fillModeNonSolid == VK_TRUE),
            yesNo(features.sampleRateShading == VK_TRUE)));

        // Device 扩展决定交换链、portability 和详细驱动信息是否可以启用。
        auto extensionsResult =
            devices[deviceIndex].enumerateDeviceExtensionProperties();
        if ( extensionsResult.result == vk::Result::eSuccess ) {
            // 扩展数量和三个关键能力压缩在同一行，其余名称无需为当前启动决策
            // 重复输出完整清单。
            const auto& deviceExtensions = extensionsResult.value;
            addStartupDiagnostic(fmt::format(
                "GPU[{}] device extensions: count={}, {}={}, "
                "VK_KHR_portability_subset={}, {}={}",
                deviceIndex,
                deviceExtensions.size(),
                VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                yesNo(hasExtension(deviceExtensions,
                                   VK_KHR_SWAPCHAIN_EXTENSION_NAME)),
                yesNo(hasExtension(deviceExtensions,
                                   "VK_KHR_portability_subset")),
                VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
                yesNo(hasExtension(deviceExtensions,
                                   VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME))));

            // DriverProperties 在 Vulkan 1.2 进入核心；旧 API 设备必须显式报告
            // KHR 扩展才能把结构挂入 properties2 查询链。
            const bool supportsDriverProperties =
                properties.apiVersion >= VK_API_VERSION_1_2 ||
                hasExtension(deviceExtensions,
                             VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
            if ( supportsDriverProperties ) {
                // 先解析核心函数名，再回退 KHR 后缀，兼容不同 API 版本的
                // loader。
                // 使用 instance 级 proc address 而非静态链接符号，使旧 loader
                // 在不 导出新入口时仍能完成其余基础诊断。
                auto getProperties2 =
                    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                        vkGetInstanceProcAddr(
                            m_vkInstance, "vkGetPhysicalDeviceProperties2"));
                if ( !getProperties2 ) {
                    getProperties2 =
                        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                            vkGetInstanceProcAddr(
                                m_vkInstance,
                                "vkGetPhysicalDeviceProperties2KHR"));
                }

                if ( getProperties2 ) {
                    // 所有 pNext 结构都先零初始化并填写
                    // sType；局部对象覆盖同步查询
                    // 调用，返回后立即格式化，不保留驱动字符串指针。
                    VkPhysicalDeviceDriverProperties driverProperties{};
                    driverProperties.sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
                    VkPhysicalDeviceProperties2 properties2{};
                    properties2.sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                    properties2.pNext = &driverProperties;
                    getProperties2(rawDevice, &properties2);

                    // conformance 四分量单独转为整数，避免 uint8_t 被 fmt
                    // 当字符输出。
                    addStartupDiagnostic(fmt::format(
                        "GPU[{}] driver: id={}, name=\"{}\", info=\"{}\", "
                        "conformance={}.{}.{}.{}",
                        deviceIndex,
                        static_cast<int>(driverProperties.driverID),
                        driverProperties.driverName,
                        driverProperties.driverInfo,
                        static_cast<int>(
                            driverProperties.conformanceVersion.major),
                        static_cast<int>(
                            driverProperties.conformanceVersion.minor),
                        static_cast<int>(
                            driverProperties.conformanceVersion.subminor),
                        static_cast<int>(
                            driverProperties.conformanceVersion.patch)));
                } else {
                    // 能力宣称与入口缺失同时记录，通常指向 loader/ICD
                    // 版本混配。
                    addStartupDiagnostic(fmt::format(
                        "GPU[{}] driver properties supported, but "
                        "vkGetPhysicalDeviceProperties2 is unavailable.",
                        deviceIndex));
                }
            } else {
                // 这是可接受的旧设备能力缺失，不影响基础属性和队列诊断。
                addStartupDiagnostic(fmt::format(
                    "GPU[{}] driver properties extension is not supported.",
                    deviceIndex));
            }
        } else {
            // 单设备扩展枚举失败不终止候选遍历，后续 GPU 仍可能完整可用。
            addStartupDiagnostic(
                fmt::format("GPU[{}] failed to enumerate device extensions: {}",
                            deviceIndex,
                            vk::to_string(extensionsResult.result)));
        }

        // 队列能力是设备选择失败最常见原因之一，始终记录每个族而非只记录首个
        // 图形族。
        const auto queueFamilies =
            devices[deviceIndex].getQueueFamilyProperties();
        addStartupDiagnostic(fmt::format(
            "GPU[{}] queue families: {}", deviceIndex, queueFamilies.size()));
        for ( uint32_t queueIndex = 0;
              queueIndex < static_cast<uint32_t>(queueFamilies.size());
              ++queueIndex ) {
            // 明确区分“未查询”“支持/不支持”和 Vulkan 查询错误三种状态。
            std::string presentText = "not-queried";
            if ( includeSurfaceSupport && m_vkSurface ) {
                // 呈现支持属于 device、queue family 与 surface 三者组合，不能由
                // graphics 标志推断。
                auto presentResult = devices[deviceIndex].getSurfaceSupportKHR(
                    queueIndex, m_vkSurface);
                presentText = presentResult.result == vk::Result::eSuccess
                                  ? yesNo(presentResult.value == VK_TRUE)
                                  : vk::to_string(presentResult.result);
            }

            // count 为可创建队列数量，flags 文本保留一个族的全部已知角色。
            addStartupDiagnostic(
                fmt::format("GPU[{}] queue[{}]: count={}, flags={}, present={}",
                            deviceIndex,
                            queueIndex,
                            queueFamilies[queueIndex].queueCount,
                            queueFlagsText(static_cast<VkQueueFlags>(
                                queueFamilies[queueIndex].queueFlags)),
                            presentText));
        }
    }
}

/// @brief 记录已选物理设备对当前 surface 的尺寸、格式与呈现模式支持。
///
/// 三组能力分别查询且各自记录失败，某一查询失败不会伪造其余组的结果。格式与
/// present mode 遵循 Vulkan 两阶段枚举，并完整列出返回顺序；请求 framebuffer
/// 尺寸也一并记录，便于解释交换链最终 extent 的钳制结果。
///
/// currentExtent 可能是规范定义的特殊未固定值，诊断保留其原始无符号数，不在
/// 这里代替交换链实现选择尺寸。format 与 colorSpace 也保持设备报告的组合关系，
/// 不拆开推断任意配对是否有效。
///
/// @param width 调用方请求的 framebuffer 宽度。
/// @param height 调用方请求的 framebuffer 高度。
/// @warning 窗口初始化低频路径：会同步调用 surface 查询并分配临时数组。
void VKContext::collectSelectedSurfaceDiagnostics(int width, int height)
{
    // 只有设备与 surface 同时存在时查询才有定义；缺失哪个不影响记录统一原因。
    if ( !m_vkPhysicalDevice || !m_vkSurface ) {
        addStartupDiagnostic(
            "Selected surface diagnostics skipped: device or surface is null.");
        return;
    }

    addStartupDiagnostic(
        fmt::format("Requested framebuffer extent: {}x{}", width, height));

    // capabilities 是单次固定大小查询，记录图像数量、extent 和当前预变换约束。
    VkSurfaceCapabilitiesKHR capabilities{};
    VkResult                 result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        static_cast<VkPhysicalDevice>(m_vkPhysicalDevice),
        static_cast<VkSurfaceKHR>(m_vkSurface),
        &capabilities);
    if ( result == VK_SUCCESS ) {
        // maxImages 为零表示没有上限，保留原始零值供交换链数量选择逻辑解释。
        addStartupDiagnostic(fmt::format(
            "Surface capabilities: minImages={}, maxImages={}, "
            "currentExtent={}x{}, minExtent={}x{}, maxExtent={}x{}, "
            "currentTransform=0x{:X}",
            capabilities.minImageCount,
            capabilities.maxImageCount,
            capabilities.currentExtent.width,
            capabilities.currentExtent.height,
            capabilities.minImageExtent.width,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.width,
            capabilities.maxImageExtent.height,
            static_cast<uint32_t>(capabilities.currentTransform)));
    } else {
        addStartupDiagnostic(
            fmt::format("Failed to query surface capabilities: {}",
                        formatVkResult(result)));
    }

    // Surface format 使用两阶段枚举；首阶段失败时跳过数组分配并在统一出口记录。
    uint32_t formatCount = 0;
    result               = vkGetPhysicalDeviceSurfaceFormatsKHR(
        static_cast<VkPhysicalDevice>(m_vkPhysicalDevice),
        static_cast<VkSurfaceKHR>(m_vkSurface),
        &formatCount,
        nullptr);
    if ( result == VK_SUCCESS ) {
        // 零数量是可记录状态，只有非零才需要第二次驱动调用。
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        if ( formatCount > 0 ) {
            // 第二阶段允许驱动回写实际数量；数组仍按首阶段容量分配，避免依赖固定
            // 上限或栈上大对象。
            result = vkGetPhysicalDeviceSurfaceFormatsKHR(
                static_cast<VkPhysicalDevice>(m_vkPhysicalDevice),
                static_cast<VkSurfaceKHR>(m_vkSurface),
                &formatCount,
                formats.data());
        }
        if ( result == VK_SUCCESS ) {
            // 格式转为 Vulkan-Hpp 名称，色彩空间保留整数以覆盖扩展枚举值。
            addStartupDiagnostic(
                fmt::format("Surface formats reported: {}", formatCount));
            for ( size_t index = 0; index < formats.size(); ++index ) {
                addStartupDiagnostic(fmt::format(
                    "Surface format[{}]: format={}, colorSpace={}",
                    index,
                    vk::to_string(
                        static_cast<vk::Format>(formats[index].format)),
                    static_cast<int>(formats[index].colorSpace)));
            }
        }
    }
    // result 始终表示格式查询链最后执行的一步，任一步失败都会进入此处。
    if ( result != VK_SUCCESS ) {
        addStartupDiagnostic(fmt::format("Failed to query surface formats: {}",
                                         formatVkResult(result)));
    }

    // 呈现模式独立于格式查询重新使用 result，确保前一失败不会阻止继续诊断。
    uint32_t presentModeCount = 0;
    result                    = vkGetPhysicalDeviceSurfacePresentModesKHR(
        static_cast<VkPhysicalDevice>(m_vkPhysicalDevice),
        static_cast<VkSurfaceKHR>(m_vkSurface),
        &presentModeCount,
        nullptr);
    if ( result == VK_SUCCESS ) {
        // 驱动可能在两阶段之间调整数量，格式化时使用第二阶段回写的 count。
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        if ( presentModeCount > 0 ) {
            // 与格式路径相同，查询错误只影响本组诊断，不覆盖已经记录的
            // capabilities 或 surface format 信息。
            result = vkGetPhysicalDeviceSurfacePresentModesKHR(
                static_cast<VkPhysicalDevice>(m_vkPhysicalDevice),
                static_cast<VkSurfaceKHR>(m_vkSurface),
                &presentModeCount,
                presentModes.data());
        }
        if ( result == VK_SUCCESS ) {
            // 完整枚举能解释 selectPresentMode 为何选择 Immediate、Mailbox 或
            // FIFO。
            addStartupDiagnostic(fmt::format(
                "Surface present modes reported: {}", presentModeCount));
            for ( size_t index = 0; index < presentModes.size(); ++index ) {
                addStartupDiagnostic(
                    fmt::format("Surface presentMode[{}]: {}",
                                index,
                                presentModeName(presentModes[index])));
            }
            // 成功摘要进入常规日志，详细逐项信息只保留在失败时打印的快照中。
            XINFO(
                "Selected Vulkan surface support: formats={}, presentModes={}",
                formatCount,
                presentModeCount);
        }
    }
    if ( result != VK_SUCCESS ) {
        addStartupDiagnostic(
            fmt::format("Failed to query surface present modes: {}",
                        formatVkResult(result)));
    }
}

/// @brief 记录即将用于 vkCreateDevice 的队列族与扩展请求。
///
/// 输入容器已由逻辑设备初始化流程去重并完成能力验证，本函数只按原顺序复制为
/// 自有诊断文本，不保存任何外部字符串指针或 vector 引用。
///
/// 队列族数量可能少于逻辑角色数量，因为图形和呈现可以共享同一族；这里记录的
/// 是实际传给 DeviceCreateInfo 的去重结果，而不是未合并的角色列表。
///
/// @param deviceExtensions 请求启用的 device 扩展名称。
/// @param queueFamilies 请求创建至少一条队列的队列族索引。
void VKContext::collectLogicalDeviceCreateDiagnostics(
    const std::vector<const char*>& deviceExtensions,
    const std::vector<uint32_t>&    queueFamilies)
{
    addStartupDiagnostic(fmt::format(
        "Requested logical-device queue families: {}", queueFamilies.size()));
    // 逐项记录可发现重复索引或错误族号，数量摘要本身不足以定位这类问题。
    for ( uint32_t queueFamily : queueFamilies ) {
        addStartupDiagnostic(
            fmt::format("Requested queue family: {}", queueFamily));
    }

    addStartupDiagnostic(fmt::format("Requested device extensions: {}",
                                     deviceExtensions.size()));
    // 空名称以占位文本输出，诊断路径不得因无效请求再次崩溃。
    for ( const char* extension : deviceExtensions ) {
        addStartupDiagnostic(fmt::format("Requested device extension: {}",
                                         extension ? extension : "<null>"));
    }
}

/// @brief 将累计的启动诊断快照一次性输出到错误日志。
///
/// 首次失败会冻结“已打印”状态并按采集顺序编号全部行。后续失败只输出最新原因，
/// 防止同一启动链重复倾倒数百行信息并掩盖最初故障；缓存仍保留到上下文销毁。
///
/// 快照使用项目日志宏而非标准输出，确保 GUI 启动失败也遵循统一日志落盘与级别
/// 处理。边界标题用于从其他并发启动日志中完整截取诊断段。
///
/// @param reason 触发本次输出的直接失败原因；空指针显示为 unknown。
/// @warning 仅供启动失败路径调用：包含完整 vector 遍历和大量日志 I/O，绝对禁止
/// 放入渲染或逻辑热路径。
void VKContext::logStartupDiagnostics(const char* reason)
{
    // 幂等保护只限制完整快照，仍保留后续调用的直接原因以辅助发现重复失败路径。
    if ( m_startupDiagnosticsPrinted ) {
        XERROR(
            "Graphics startup diagnostics already printed. Latest reason: {}",
            reason ? reason : "<unknown>");
        return;
    }

    // 在输出前置位，日志后端若触发重入也不会递归打印同一快照。
    m_startupDiagnosticsPrinted = true;
    XERROR("================ Graphics Startup Diagnostics ================");
    XERROR("Reason: {}", reason ? reason : "<unknown>");
    // 空快照也显式说明，避免把日志截断误判为收集逻辑遗漏。
    if ( m_startupDiagnosticLines.empty() ) {
        XERROR("No startup diagnostic lines were collected.");
    }

    // 固定三位索引保持长日志可检索，并准确反映诊断采集的因果顺序。
    for ( size_t index = 0; index < m_startupDiagnosticLines.size(); ++index ) {
        XERROR("[{:03}] {}", index, m_startupDiagnosticLines[index]);
    }
    XERROR("================ End Graphics Startup Diagnostics ============");
}

}  // namespace MMM::Graphic
