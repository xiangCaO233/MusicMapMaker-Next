#include "ui/utils/NativeFileDialog.h"

#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <GLFW/glfw3.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#    define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#    define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(__linux__)
#    define GLFW_EXPOSE_NATIVE_WAYLAND
#    define GLFW_EXPOSE_NATIVE_X11
#endif
#include <nfd_glfw3.h>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#endif

/// @file NativeFileDialog.cpp
/// @brief nativefiledialog-extended 的默认目录、父窗口和 GLFW 光标状态适配。
/// @details 所有公开入口都通过 runDialog 使用一致的 owner 与光标策略；路径
/// 在 C++ 文件系统边界转换为 UTF-8，取消和错误码原样返回调用方。

namespace MMM::UI::NativeFileDialog
{
namespace
{

/// @brief 全部原生选择器共享的 GLFW 主窗口观察指针。
/// @warning 仅由 UI 线程绑定和读取，生命周期由 GameLoop 保证。
GLFWwindow* g_mainWindow = nullptr;

/// @brief 将文件选择器默认目录规范化为现存的绝对路径。
/// @param defaultPath 调用方提供的 UTF-8 历史目录，可为空或为相对路径。
/// @return 可传给 NFD 的 UTF-8 绝对目录；无法解析时返回空字符串。
[[nodiscard]] std::string resolveDefaultPath(const nfdu8char_t* defaultPath)
{
    // 可执行文件目录是空路径、相对路径和失效路径的统一锚点。
    const std::filesystem::path executableDirectory =
        Config::AppPaths::executableDirectoryPath();
    std::filesystem::path resolvedPath;
    if ( defaultPath && defaultPath[0] != '\0' ) {
        // NFD UTF-8 字符串先通过项目路径辅助函数转换为平台路径。
        resolvedPath = Config::utf8ToPath(defaultPath);
    }

    if ( resolvedPath.empty() || resolvedPath == "." ) {
        // 空值与单点目录都表示使用应用程序所在目录。
        resolvedPath = executableDirectory;
    } else if ( resolvedPath.is_relative() ) {
        // 其他相对历史目录也以可执行文件目录解析，不依赖进程 cwd。
        resolvedPath = executableDirectory / resolvedPath;
    }

    // error_code 重载维持项目无异常错误边界。
    std::error_code directoryError;
    if ( resolvedPath.empty() ||
         !std::filesystem::is_directory(resolvedPath, directoryError) ||
         directoryError ) {
        // 不存在、非目录或查询失败时回退到已知应用目录。
        resolvedPath = executableDirectory;
    }
    // NFD 只接受有效绝对目录；连回退目录都不可用时不给默认值。
    if ( resolvedPath.empty() || !resolvedPath.is_absolute() ) return {};
    // 词法归一化清理点段，不解析可能失效的符号链接。
    return Config::pathToUtf8(resolvedPath.lexically_normal());
}

/// @brief 在执行原生对话框调用前临时切换到系统普通光标。
/// @param window GLFW 主窗口句柄，可为空。
/// @return 对话框打开前的 GLFW 光标模式。
[[nodiscard]] int showSystemCursor(GLFWwindow* window)
{
    // 测试或启动阶段没有 GLFW 窗口时按普通模式处理。
    if ( !window ) return GLFW_CURSOR_NORMAL;
    // 保存模式以便模态对话框关闭后无损恢复捕获状态。
    const int previousMode = glfwGetInputMode(window, GLFW_CURSOR);
    if ( previousMode != GLFW_CURSOR_NORMAL ) {
        // 原生对话框交互期间必须显示系统指针并释放禁用捕获。
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    return previousMode;
}

/// @brief 原生对话框关闭后恢复此前的 GLFW 光标模式。
/// @param window GLFW 主窗口句柄，可为空。
/// @param previousMode 对话框打开前的 GLFW 光标模式。
void restoreSystemCursor(GLFWwindow* window, int previousMode)
{
    // 无窗口或原本就是普通光标时无需调用平台后端。
    if ( !window || previousMode == GLFW_CURSOR_NORMAL ) return;
    // 恢复隐藏、禁用等调用前模式，使画布输入状态连续。
    glfwSetInputMode(window, GLFW_CURSOR, previousMode);
}

#if defined(_WIN32)
/// @brief 判断窗口是否属于当前应用进程。
/// @param window Win32 窗口句柄。
/// @return 窗口有效且属于当前进程时返回 true。
[[nodiscard]] bool isCurrentProcessWindow(HWND window)
{
    // 无效 HWND 不传给进程 ID 查询。
    if ( !window || !IsWindow(window) ) return false;
    // owner 只能选择本进程窗口，避免激活其他应用的前台窗口。
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

/// @brief 将当前进程窗口归一到其顶层根窗口。
/// @param window 候选 Win32 窗口句柄。
/// @return 当前进程的顶层窗口；无效或属于其他进程时返回 nullptr。
[[nodiscard]] HWND rootCurrentProcessWindow(HWND window)
{
    // 先验证输入和进程归属，再追踪顶层根窗口。
    if ( !isCurrentProcessWindow(window) ) return nullptr;
    if ( HWND rootWindow = GetAncestor(window, GA_ROOT) ) {
        // ImGui 多视口子窗口归一到对应平台顶层窗口。
        return rootWindow;
    }
    // GetAncestor 无结果时保留已验证的窗口本身。
    return window;
}

/// @brief 选择当前应用内最适合作为文件对话框 owner 的活动顶层窗口。
/// @param activeWindow 当前线程活动窗口。
/// @param foregroundWindow 系统前台窗口。
/// @param fallbackWindow GLFW 主窗口转换出的回退句柄。
/// @return 当前活动 ImGui 视口或主窗口句柄。
[[nodiscard]] HWND selectWin32DialogOwner(HWND activeWindow,
                                          HWND foregroundWindow,
                                          HWND fallbackWindow)
{
    // 当前线程活动窗口最能反映用户触发对话框的 ImGui 视口。
    if ( HWND ownerWindow = rootCurrentProcessWindow(activeWindow) ) {
        return ownerWindow;
    }
    // 活动窗口不可用时尝试系统前台窗口，但仍要求属于本进程。
    if ( HWND ownerWindow = rootCurrentProcessWindow(foregroundWindow) ) {
        return ownerWindow;
    }
    // 最后回退 GLFW 主窗口，保持单视口环境可用。
    return rootCurrentProcessWindow(fallbackWindow);
}

/// @brief 在创建 IFileDialog 前把 owner 恢复并提升为当前活动窗口。
/// @param window 文件对话框 owner。
/// @warning 用户触发的低频平台操作；不使用 TOPMOST，避免压住其他应用。
void activateWin32DialogOwner(HWND window)
{
    // 缺少 owner 时让 NFD 使用系统默认所有者。
    if ( !window ) return;
    if ( IsIconic(window) ) {
        // 最小化窗口先恢复，否则文件对话框可能出现在不可见层级。
        ShowWindow(window, SW_RESTORE);
    }
    // 提升并激活 owner，但不设置永久 TOPMOST 属性。
    BringWindowToTop(window);
    SetActiveWindow(window);
    SetWindowPos(
        window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    if ( !SetForegroundWindow(window) && GetForegroundWindow() != window ) {
        // Windows 可能按前台锁定策略拒绝切换，NFD 仍可继续打开。
        XWARN("Windows 拒绝将原生文件选择器父窗口切换到前台");
    }
}
#endif

/// @brief 获取当前平台的原生文件选择器父窗口。
/// @return 可用父窗口；转换失败时返回值初始化句柄。
[[nodiscard]] nfdwindowhandle_t resolveParentWindow()
{
    // 值初始化句柄表示 NFD 自行选择系统默认 owner。
    nfdwindowhandle_t parentWindow{};
    if ( !g_mainWindow ) return parentWindow;

#if defined(__linux__)
    // Linux 下先从 GLFW 当前后端同步 X11 或 Wayland 显示属性。
    (void)NFD_SetDisplayPropertiesFromGLFW();
#endif
    // 跨平台辅助函数把 GLFW 主窗口转换为 NFD 原生句柄联合体。
    if ( !NFD_GetNativeWindowFromGLFWWindow(g_mainWindow, &parentWindow) ) {
        XWARN("无法取得原生文件选择器父窗口，将使用系统默认 owner");
        return {};
    }

#if defined(_WIN32)
    // GLFW 转换句柄仅作为活动/前台窗口均不可用时的回退。
    const auto fallbackWindow =
        parentWindow.type == NFD_WINDOW_HANDLE_TYPE_WINDOWS
            ? static_cast<HWND>(parentWindow.handle)
            : nullptr;
    // 多视口下优先当前活动平台窗口，使对话框覆盖触发它的视口。
    const HWND ownerWindow = selectWin32DialogOwner(
        GetActiveWindow(), GetForegroundWindow(), fallbackWindow);
    if ( !ownerWindow ) {
        // 没有本进程窗口时禁止借用其他应用 owner。
        XWARN("无法取得当前 Windows 活动窗口，将使用系统默认 owner");
        return {};
    }
    // 用选择后的顶层 HWND 覆盖最初的 GLFW 主窗口句柄。
    parentWindow.type   = NFD_WINDOW_HANDLE_TYPE_WINDOWS;
    parentWindow.handle = ownerWindow;
#endif
    return parentWindow;
}

/// @brief 使用统一 owner、前台激活与光标恢复策略运行 NFD With 接口。
/// @tparam Args NFD 参数结构类型。
/// @tparam Callback 对应 NFD With 调用。
/// @param args 待填充 parentWindow 的参数。
/// @param callback 实际 NFD 调用。
/// @return NFD 对话框结果。
/// @warning 用户触发的低频阻塞路径；只允许在 UI 线程调用。
template<typename Args, typename Callback>
[[nodiscard]] nfdresult_t runDialog(Args& args, Callback&& callback)
{
    // 所有 NFD With 参数结构都具有相同 parentWindow 字段。
    args.parentWindow = resolveParentWindow();
    // 在阻塞平台对话框前保存并切换 GLFW 光标模式。
    const int previousCursorMode = showSystemCursor(g_mainWindow);
#if defined(_WIN32)
    if ( args.parentWindow.type == NFD_WINDOW_HANDLE_TYPE_WINDOWS ) {
        // Windows 在创建 COM 对话框前主动激活 owner，降低窗口落到后台概率。
        activateWin32DialogOwner(static_cast<HWND>(args.parentWindow.handle));
    }
#endif
    // callback 选择具体 Open、Save 或 PickFolder With API。
    const nfdresult_t result = std::forward<Callback>(callback)(args);
    // 无论成功、取消还是错误都恢复进入对话框前的光标模式。
    restoreSystemCursor(g_mainWindow, previousCursorMode);
    return result;
}

}  // namespace

/// @brief 绑定原生文件选择器使用的 GLFW 主窗口观察指针。
/// @param window 生命周期由 GameLoop 管理的窗口，可传 nullptr 解除绑定。
/// @warning 仅允许 UI 线程在窗口创建或销毁阶段调用。
void bindMainWindow(GLFWwindow* window)
{
    // 不取得 GLFWwindow 所有权，也不注册销毁回调。
    g_mainWindow = window;
}

/// @brief 打开单文件原生选择器。
/// @param outPath 成功时接收 NFD 分配的 UTF-8 路径。
/// @param filters 可选扩展名过滤器数组。
/// @param filterCount 过滤器数量。
/// @param defaultPath 可选默认目录，支持相对可执行文件路径。
/// @return NFD_OKAY、NFD_CANCEL 或 NFD_ERROR；路径释放责任遵循 NFD API。
/// @warning 用户触发的低频阻塞调用，只能在 UI 线程执行。
nfdresult_t openFile(nfdu8char_t** outPath, const nfdu8filteritem_t* filters,
                     nfdfiltersize_t    filterCount,
                     const nfdu8char_t* defaultPath)
{
    // 局部字符串生命周期覆盖阻塞式 NFD 调用。
    const std::string resolvedDefaultPath = resolveDefaultPath(defaultPath);
    // 值初始化保证未显式设置的 NFD 参数使用零值默认行为。
    nfdopendialogu8args_t args{};
    args.filterList  = filters;
    args.filterCount = filterCount;
    // 空解析结果使用 nullptr，让 NFD 选择系统默认目录。
    args.defaultPath =
        resolvedDefaultPath.empty() ? nullptr : resolvedDefaultPath.c_str();
    // Lambda 只捕获输出指针，并把统一参数结构转发给 Open With API。
    return runDialog(args, [outPath](const nfdopendialogu8args_t& dialogArgs) {
        return NFD_OpenDialogU8_With(outPath, &dialogArgs);
    });
}

/// @brief 打开保存文件原生选择器。
/// @param outPath 成功时接收 NFD 分配的 UTF-8 路径。
/// @param filters 可选扩展名过滤器数组。
/// @param filterCount 过滤器数量。
/// @param defaultPath 可选默认目录。
/// @param defaultName 可选默认文件名，不包含目录。
/// @return NFD 对话框结果；成功路径由调用方按 NFD 规则释放。
/// @warning 用户触发的低频阻塞调用，只能在 UI 线程执行。
nfdresult_t saveFile(nfdu8char_t** outPath, const nfdu8filteritem_t* filters,
                     nfdfiltersize_t    filterCount,
                     const nfdu8char_t* defaultPath,
                     const nfdu8char_t* defaultName)
{
    // 默认目录规范化与打开文件入口保持一致。
    const std::string     resolvedDefaultPath = resolveDefaultPath(defaultPath);
    nfdsavedialogu8args_t args{};
    args.filterList  = filters;
    args.filterCount = filterCount;
    args.defaultPath =
        resolvedDefaultPath.empty() ? nullptr : resolvedDefaultPath.c_str();
    // 默认文件名由调用方提供，NFD 负责展示和用户编辑。
    args.defaultName = defaultName;
    // 统一 owner 与光标策略后调用 Save With API。
    return runDialog(args, [outPath](const nfdsavedialogu8args_t& dialogArgs) {
        return NFD_SaveDialogU8_With(outPath, &dialogArgs);
    });
}

/// @brief 打开文件夹原生选择器。
/// @param outPath 成功时接收 NFD 分配的 UTF-8 目录路径。
/// @param defaultPath 可选默认目录。
/// @return NFD 对话框结果；成功路径由调用方按 NFD 规则释放。
/// @warning 用户触发的低频阻塞调用，只能在 UI 线程执行。
nfdresult_t pickFolder(nfdu8char_t** outPath, const nfdu8char_t* defaultPath)
{
    // 文件夹选择不需要扩展名过滤器或默认文件名。
    const std::string     resolvedDefaultPath = resolveDefaultPath(defaultPath);
    nfdpickfolderu8args_t args{};
    args.defaultPath =
        resolvedDefaultPath.empty() ? nullptr : resolvedDefaultPath.c_str();
    // 统一 owner 与光标策略后调用 PickFolder With API。
    return runDialog(args, [outPath](const nfdpickfolderu8args_t& dialogArgs) {
        return NFD_PickFolderU8_With(outPath, &dialogArgs);
    });
}

}  // namespace MMM::UI::NativeFileDialog
