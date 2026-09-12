#include "graphic/glfw/GLFWHeader.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "log/colorful-log.h"

#include <fmt/format.h>

#ifdef __APPLE__
#    include <cstdlib>
#    include <filesystem>
#    include <limits.h>
#    include <mach-o/dyld.h>
#endif

namespace MMM::Graphic
{

#ifdef __APPLE__
namespace
{
/// @brief 让 Vulkan loader 优先使用应用束内随程序发布的 MoltenVK 驱动清单。
///
/// 开发环境可能还能从 Homebrew 或 SDK 找到 ICD，但发布后的应用束不能依赖这些
/// 外部安装。这里在 GLFW 初始化 Vulkan loader
/// 前固定清单位置，使两条启动路径使用 同一份
/// MoltenVK。找不到随包清单时保留现有环境，让后续诊断报告真实 loader 状态。
///
/// @return 找到清单且成功写入当前进程环境时返回 true，否则返回 false。
bool configureMacOSBundledVulkanDriver()
{
    // _NSGetExecutablePath
    // 以实际可执行文件为锚点；当前固定缓冲不足时不尝试分配，
    // 因为失败后仍可交由系统 Vulkan loader 按默认规则继续探测。
    char     executablePathBuffer[PATH_MAX];
    uint32_t executablePathSize = sizeof(executablePathBuffer);
    if ( _NSGetExecutablePath(executablePathBuffer, &executablePathSize) !=
         0 ) {
        return false;
    }

    // macOS 应用束结构固定为 Contents/MacOS/<binary>，驱动清单位于相邻的
    // Contents/Resources/vulkan/icd.d，连续两个 parent_path 回到 Contents。
    std::filesystem::path driverManifest(executablePathBuffer);
    driverManifest = driverManifest.parent_path().parent_path() / "Resources" /
                     "vulkan" / "icd.d" / "MoltenVK_icd.json";

    // 使用 error_code 重载避免文件系统异常越过禁止异常的初始化边界。
    std::error_code fileError;
    if ( !std::filesystem::is_regular_file(driverManifest, fileError) ||
         fileError ) {
        return false;
    }

    // VK_DRIVER_FILES 明确覆盖 loader 的驱动选择，必须在 glfwInit 及任何 Vulkan
    // 枚举调用之前写入；字符串只需存活到 setenv 完成复制。
    const std::string driverManifestPath = driverManifest.string();
    if ( setenv("VK_DRIVER_FILES", driverManifestPath.c_str(), 1) != 0 ) {
        return false;
    }

    XDEBUG("Using bundled Vulkan driver manifest: {}", driverManifestPath);
    return true;
}
}  // namespace
#endif

/// @brief 初始化 GLFW，并确认当前平台能够通过 GLFW 使用 Vulkan。
///
/// 本函数建立进程级 GLFW 状态，设置无客户端 API 的窗口提示，并在失败时采集
/// GLFW/Vulkan loader 诊断。失败路径会立即终止 GLFW，调用方通过上下文错误状态
/// 停止后续 instance 初始化。
///
/// @warning 启动低频路径：会初始化平台窗口库并探测 Vulkan loader，禁止从渲染
/// 循环或并发窗口回调中调用。
void VKContext::initGLFW()
{
    // 错误回调必须先于 glfwInit 注册，才能记录初始化本身产生的平台错误。
    glfwSetErrorCallback(glfw_error_callback);

#ifdef __APPLE__
    // macOS 开发构建的 Vulkan loader 可能位于 Homebrew 或 SDK 目录；
    // 显式复用已链接入口，避免 GLFW 按标准动态库名二次查找失败。
    configureMacOSBundledVulkanDriver();
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
#endif

    // GLFW 是进程级资源；一旦初始化失败，统一通过 failInitialization 固化首个
    // 错误，避免构造函数继续创建依赖 GLFW 扩展列表的 Vulkan instance。
    if ( !glfwInit() ) {
        collectLastGLFWErrorDiagnostic("glfwInit failed.");
        logStartupDiagnostics("glfwInit failed.");
        failInitialization("GLFW init failed");
        return;
    };
    XDEBUG("GLFW initialized successfully.");
    collectGLFWDiagnostics();

    // 窗口不创建 OpenGL/OpenGL ES 上下文，后续呈现表面完全由 Vulkan 接管。
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // 该探测同时验证 loader 和平台 surface 支持；失败时补采 loader 清单，便于
    // 区分缺少驱动、缺少 instance 扩展与 GLFW 平台后端不可用。
    if ( !glfwVulkanSupported() ) {
        collectLastGLFWErrorDiagnostic("glfwVulkanSupported failed.");
        collectVulkanLoaderDiagnostics();
        logStartupDiagnostics("GLFW reports that Vulkan is not supported.");
        // 构造尚未进入 Vulkan instance 阶段，此处只需回收 GLFW 的进程级状态。
        releaseGLFW();
        failInitialization("Fatal: GLFW reports that Vulkan is not supported!");
        return;
    }
    XDEBUG("GLFW Vulkan is supported.");
}

/// @brief 将 GLFW 创建窗口 surface 所需的 instance 扩展注册到上下文。
///
/// GLFW 返回的扩展名由 GLFW 持有，本函数仅把稳定的名称指针复制进创建参数列表；
/// macOS 额外加入 portability enumeration 扩展，使 MoltenVK 设备能参与枚举。
///
/// @warning 启动低频路径：必须在 initGLFW 成功后、initVkInstanceCreateInfo
/// 前调用， 且同一上下文只能注册一次，避免向扩展列表重复追加名称。
void VKContext::registerGLFWExtensions()
{
    // 扩展集合取决于当前 GLFW 平台后端，例如 Win32、X11 或 Cocoa，因此不能在
    // 编译期写死；返回空指针表示 GLFW 无法给出可创建 surface 的完整契约。
    uint32_t     glfwExtensionCount = 0;
    const char** glfwExtensions{ nullptr };
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if ( glfwExtensions == nullptr ) {
        collectLastGLFWErrorDiagnostic(
            "glfwGetRequiredInstanceExtensions failed.");
        addStartupDiagnostic(
            fmt::format("GLFW required Vulkan extension "
                        "count: {}",
                        glfwExtensionCount));
        collectVulkanLoaderDiagnostics();
        logStartupDiagnostics(
            "glfwGetRequiredInstanceExtensions returned null.");
        releaseGLFW();
        failInitialization(
            "Fatal: Failed to get required GLFW "
            "extensions for window surface creation.");
        return;
    }

    XDEBUG("Required GLFW extensions:");
    // 保持 GLFW 给出的顺序写入上下文，名称指针会直接供 vk::InstanceCreateInfo
    // 使用；在 instance 创建完成前不得终止 GLFW 或改写其全局状态。
    for ( int i{ 0 }; i < glfwExtensionCount; ++i ) {
        auto glfwExtension = glfwExtensions[i];
        m_vkExtensions.push_back(glfwExtension);
        XDEBUG("  - {}", glfwExtension);
    }
    addStartupDiagnostic(
        fmt::format("GLFW required Vulkan extensions: {}", glfwExtensionCount));
    for ( int i{ 0 }; i < glfwExtensionCount; ++i ) {
        addStartupDiagnostic(
            fmt::format("GLFW required extension: {}", glfwExtensions[i]));
    }
    collectVulkanLoaderDiagnostics();

#ifdef __APPLE__
    // MoltenVK 将非原生 Vulkan 设备标记为 portability 实现；同时请求属性查询
    // 扩展，保证后续 portability 枚举链在 macOS loader 上可用。
    m_vkExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    XDEBUG("  - {}", VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    m_vkExtensions.push_back(
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    XDEBUG("  - {}", VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#endif  // __APPLE__

    // debug utils 是否加入列表由验证层流程统一决定，Release 不携带该可选依赖。
}

/// @brief 释放图形模块持有的 GLFW 进程级资源。
///
/// 软件光标由 GLFW 创建但缓存在渲染器静态状态中，必须先销毁光标，再终止 GLFW；
/// 该顺序同时覆盖完整退出和 Vulkan 探测失败后的早期清理。
///
/// @warning 退出或初始化失败的低频路径：调用前必须保证没有线程继续访问 GLFW
/// 窗口、光标或回调。
void VKContext::releaseGLFW()
{
    VKRenderer::releaseGlfwCursorResources();
    glfwTerminate();
    XDEBUG("GLFW Terminated.");
}

}  // namespace MMM::Graphic
