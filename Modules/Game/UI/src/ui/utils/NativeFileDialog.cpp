#include "ui/utils/NativeFileDialog.h"

#include "log/colorful-log.h"

#include <GLFW/glfw3.h>
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

namespace MMM::UI::NativeFileDialog
{
namespace
{

/// @brief 全部原生选择器共享的 GLFW 主窗口观察指针。
/// @warning 仅由 UI 线程绑定和读取，生命周期由 GameLoop 保证。
GLFWwindow* g_mainWindow = nullptr;

/// @brief 在执行原生对话框调用前临时切换到系统普通光标。
/// @param window GLFW 主窗口句柄，可为空。
/// @return 对话框打开前的 GLFW 光标模式。
[[nodiscard]] int showSystemCursor(GLFWwindow* window)
{
    if ( !window ) return GLFW_CURSOR_NORMAL;
    const int previousMode = glfwGetInputMode(window, GLFW_CURSOR);
    if ( previousMode != GLFW_CURSOR_NORMAL ) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    return previousMode;
}

/// @brief 原生对话框关闭后恢复此前的 GLFW 光标模式。
/// @param window GLFW 主窗口句柄，可为空。
/// @param previousMode 对话框打开前的 GLFW 光标模式。
void restoreSystemCursor(GLFWwindow* window, int previousMode)
{
    if ( !window || previousMode == GLFW_CURSOR_NORMAL ) return;
    glfwSetInputMode(window, GLFW_CURSOR, previousMode);
}

#if defined(_WIN32)
/// @brief 判断窗口是否属于当前应用进程。
/// @param window Win32 窗口句柄。
/// @return 窗口有效且属于当前进程时返回 true。
[[nodiscard]] bool isCurrentProcessWindow(HWND window)
{
    if ( !window || !IsWindow(window) ) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

/// @brief 将当前进程窗口归一到其顶层根窗口。
/// @param window 候选 Win32 窗口句柄。
/// @return 当前进程的顶层窗口；无效或属于其他进程时返回 nullptr。
[[nodiscard]] HWND rootCurrentProcessWindow(HWND window)
{
    if ( !isCurrentProcessWindow(window) ) return nullptr;
    if ( HWND rootWindow = GetAncestor(window, GA_ROOT) ) {
        return rootWindow;
    }
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
    if ( HWND ownerWindow = rootCurrentProcessWindow(activeWindow) ) {
        return ownerWindow;
    }
    if ( HWND ownerWindow = rootCurrentProcessWindow(foregroundWindow) ) {
        return ownerWindow;
    }
    return rootCurrentProcessWindow(fallbackWindow);
}

/// @brief 在创建 IFileDialog 前把 owner 恢复并提升为当前活动窗口。
/// @param window 文件对话框 owner。
/// @warning 用户触发的低频平台操作；不使用 TOPMOST，避免压住其他应用。
void activateWin32DialogOwner(HWND window)
{
    if ( !window ) return;
    if ( IsIconic(window) ) {
        ShowWindow(window, SW_RESTORE);
    }
    BringWindowToTop(window);
    SetActiveWindow(window);
    SetWindowPos(
        window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    if ( !SetForegroundWindow(window) && GetForegroundWindow() != window ) {
        XWARN("Windows 拒绝将原生文件选择器父窗口切换到前台");
    }
}
#endif

/// @brief 获取当前平台的原生文件选择器父窗口。
/// @return 可用父窗口；转换失败时返回值初始化句柄。
[[nodiscard]] nfdwindowhandle_t resolveParentWindow()
{
    nfdwindowhandle_t parentWindow{};
    if ( !g_mainWindow ) return parentWindow;

#if defined(__linux__)
    (void)NFD_SetDisplayPropertiesFromGLFW();
#endif
    if ( !NFD_GetNativeWindowFromGLFWWindow(g_mainWindow, &parentWindow) ) {
        XWARN("无法取得原生文件选择器父窗口，将使用系统默认 owner");
        return {};
    }

#if defined(_WIN32)
    const auto fallbackWindow =
        parentWindow.type == NFD_WINDOW_HANDLE_TYPE_WINDOWS
            ? static_cast<HWND>(parentWindow.handle)
            : nullptr;
    const HWND ownerWindow = selectWin32DialogOwner(
        GetActiveWindow(), GetForegroundWindow(), fallbackWindow);
    if ( !ownerWindow ) {
        XWARN("无法取得当前 Windows 活动窗口，将使用系统默认 owner");
        return {};
    }
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
    args.parentWindow            = resolveParentWindow();
    const int previousCursorMode = showSystemCursor(g_mainWindow);
#if defined(_WIN32)
    if ( args.parentWindow.type == NFD_WINDOW_HANDLE_TYPE_WINDOWS ) {
        activateWin32DialogOwner(static_cast<HWND>(args.parentWindow.handle));
    }
#endif
    const nfdresult_t result = std::forward<Callback>(callback)(args);
    restoreSystemCursor(g_mainWindow, previousCursorMode);
    return result;
}

}  // namespace

void bindMainWindow(GLFWwindow* window)
{
    g_mainWindow = window;
}

nfdresult_t openFile(nfdu8char_t** outPath, const nfdu8filteritem_t* filters,
                     nfdfiltersize_t    filterCount,
                     const nfdu8char_t* defaultPath)
{
    nfdopendialogu8args_t args{};
    args.filterList  = filters;
    args.filterCount = filterCount;
    args.defaultPath = defaultPath;
    return runDialog(args, [outPath](const nfdopendialogu8args_t& dialogArgs) {
        return NFD_OpenDialogU8_With(outPath, &dialogArgs);
    });
}

nfdresult_t saveFile(nfdu8char_t** outPath, const nfdu8filteritem_t* filters,
                     nfdfiltersize_t    filterCount,
                     const nfdu8char_t* defaultPath,
                     const nfdu8char_t* defaultName)
{
    nfdsavedialogu8args_t args{};
    args.filterList  = filters;
    args.filterCount = filterCount;
    args.defaultPath = defaultPath;
    args.defaultName = defaultName;
    return runDialog(args, [outPath](const nfdsavedialogu8args_t& dialogArgs) {
        return NFD_SaveDialogU8_With(outPath, &dialogArgs);
    });
}

nfdresult_t pickFolder(nfdu8char_t** outPath, const nfdu8char_t* defaultPath)
{
    nfdpickfolderu8args_t args{};
    args.defaultPath = defaultPath;
    return runDialog(args, [outPath](const nfdpickfolderu8args_t& dialogArgs) {
        return NFD_PickFolderU8_With(outPath, &dialogArgs);
    });
}

}  // namespace MMM::UI::NativeFileDialog
