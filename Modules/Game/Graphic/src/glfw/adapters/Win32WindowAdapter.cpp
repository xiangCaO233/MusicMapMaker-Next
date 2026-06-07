#ifdef _WIN32

#    include "graphic/glfw/window/adapters/Win32WindowAdapter.h"
#    include "event/core/EventBus.h"
#    include "log/colorful-log.h"
#    include <GLFW/glfw3.h>
#    define GLFW_EXPOSE_NATIVE_WIN32
#    include <GLFW/glfw3native.h>
#    include <commctrl.h>  // For DefSubclassProc
#    include <dwmapi.h>
#    include <windowsx.h>

namespace MMM::Graphic
{
namespace
{

/// @brief HWND 属性名，用于跨 Win32/GLFW 恢复路径保留最小化前最大化状态。
constexpr const wchar_t* RESTORE_MAXIMIZED_PROP =
    L"MMMRestoreMaximizedAfterMinimize";

/// @brief Private message used to apply maximized restore after Win32 finishes
/// its own minimized-to-restored transition.
constexpr UINT APPLY_MAXIMIZED_RESTORE_MESSAGE = WM_APP + 0x0312;

/// @brief Private message used to clear the short-lived Alt+Tab restore guard.
constexpr UINT CLEAR_RESTORE_IGNORE_MESSAGE = WM_APP + 0x0313;

/// @brief Win32 WINDOWPLACEMENT flag that asks minimized restore to maximize.
constexpr UINT RESTORE_TO_MAXIMIZED_FLAG = 0x0002;

/// @brief 判断 Win32 placement 是否直接表示当前窗口最大化。
/// @param placement Win32 窗口布局信息。
/// @return 当前 show command 是最大化时返回 true。
bool placementShowsMaximized(const WINDOWPLACEMENT& placement)
{
    return placement.showCmd == SW_SHOWMAXIMIZED;
}

/// @brief 清除 Win32 最小化后恢复最大化的系统 hint。
/// @param hWnd Win32 窗口句柄。
void clearRestoreToMaximizedFlag(HWND hWnd)
{
    if ( !hWnd ) {
        return;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(WINDOWPLACEMENT);
    if ( !GetWindowPlacement(hWnd, &placement) ||
         (placement.flags & RESTORE_TO_MAXIMIZED_FLAG) == 0 ) {
        return;
    }

    placement.flags &= ~RESTORE_TO_MAXIMIZED_FLAG;
    SetWindowPlacement(hWnd, &placement);
}

/// @brief 将布尔值写入 HWND 属性。
/// @param hWnd Win32 窗口句柄。
/// @param value 待写入状态。
void setRestoreMaximizedProperty(HWND hWnd, bool value)
{
    if ( !hWnd ) {
        return;
    }

    if ( value ) {
        SetPropW(hWnd, RESTORE_MAXIMIZED_PROP, reinterpret_cast<HANDLE>(1));
    } else {
        RemovePropW(hWnd, RESTORE_MAXIMIZED_PROP);
    }
}

}  // namespace

Win32WindowAdapter::Win32WindowAdapter(GLFWwindow* window) : m_window(window)
{
    m_hwnd               = glfwGetWin32Window(m_window);
    m_lastKnownMaximized = IsZoomed(m_hwnd) != FALSE;

    // 订阅拖拽区域更新事件
    Event::EventBus::instance().subscribe<Event::UpdateDragAreaEvent>(
        [this](Event::UpdateDragAreaEvent e) { this->onUpdateDragArea(e); });

    // 安装窗口子类过程
    SetWindowSubclass(m_hwnd, WindowProc, 0, (DWORD_PTR)this);

    // 启用调整大小和 Aero Snap
    LONG_PTR style = GetWindowLongPtr(m_hwnd, GWL_STYLE);
    style |= WS_THICKFRAME | WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    SetWindowLongPtr(m_hwnd, GWL_STYLE, style);
    SetWindowPos(m_hwnd,
                 nullptr,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    // 重新从模块资源加载窗口图标，并应用到 HWND 和 Window Class，解决无边框修改
    // style 后任务栏图标丢失问题
    HICON hIcon = LoadIcon(GetModuleHandle(nullptr), "IDI_ICON1");
    if ( !hIcon ) {
        hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
    }
    if ( hIcon ) {
        SendMessage(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessage(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        SetClassLongPtr(m_hwnd, GCLP_HICON, (LONG_PTR)hIcon);
        SetClassLongPtr(m_hwnd, GCLP_HICONSM, (LONG_PTR)hIcon);
    }

    // 方案 1：强制开启阴影（即使是无边框）
    const MARGINS shadow_margin = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &shadow_margin);

    // 方案 2：如果是 Windows 11，甚至可以设置圆角
    DWORD count = DWMWCP_ROUND;
    DwmSetWindowAttribute(
        m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &count, sizeof(count));

    XINFO("Win32WindowAdapter initialized");
}

Win32WindowAdapter::~Win32WindowAdapter()
{
    if ( m_hwnd ) {
        setRestoreMaximizedProperty(m_hwnd, false);
        RemoveWindowSubclass(m_hwnd, WindowProc, 0);
    }
}

bool Win32WindowAdapter::requestMove()
{
    return false;
}

bool Win32WindowAdapter::requestResize(WindowFrameResizeEdge edge)
{
    (void)edge;
    return false;
}

bool Win32WindowAdapter::supportsClientFrameRequests() const
{
    return false;
}

bool Win32WindowAdapter::usesClientFrameOverlay() const
{
    return false;
}

void Win32WindowAdapter::refreshFrameShape()
{
    if ( !m_hwnd ) {
        return;
    }

    DWORD count = DWMWCP_ROUND;
    DwmSetWindowAttribute(
        m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &count, sizeof(count));
}

bool Win32WindowAdapter::handleClientMouseButton(int button, int action,
                                                 double cursorX, double cursorY)
{
    (void)button;
    (void)action;
    (void)cursorX;
    (void)cursorY;
    return false;
}

bool Win32WindowAdapter::handleClientCursorPos(double cursorX, double cursorY)
{
    (void)cursorX;
    (void)cursorY;
    return false;
}

void Win32WindowAdapter::onUpdateDragArea(const Event::UpdateDragAreaEvent& e)
{
    // 更新拖拽区域缓存
    m_dragAreas = e.areas;
}

bool Win32WindowAdapter::windowPlacementWantsMaximized(HWND hWnd) const
{
    if ( !hWnd ) {
        return false;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(WINDOWPLACEMENT);
    if ( !GetWindowPlacement(hWnd, &placement) ) {
        return IsZoomed(hWnd) != FALSE;
    }

    return IsZoomed(hWnd) != FALSE || placementShowsMaximized(placement);
}

void Win32WindowAdapter::rememberRestoreStateBeforeMinimize(HWND hWnd)
{
    m_restoreMaximizedAfterMinimize =
        m_lastKnownMaximized || windowPlacementWantsMaximized(hWnd);
    if ( !m_restoreMaximizedAfterMinimize ) {
        clearRestoreToMaximizedFlag(hWnd);
    }
    setRestoreMaximizedProperty(hWnd, m_restoreMaximizedAfterMinimize);
}

bool Win32WindowAdapter::hasRestoreMaximizedProperty(HWND hWnd) const
{
    return hWnd && GetPropW(hWnd, RESTORE_MAXIMIZED_PROP) != nullptr;
}

void Win32WindowAdapter::restoreMaximizedAfterMinimize(HWND hWnd)
{
    if ( !hWnd || m_applyingMaximizedRestore ) {
        return;
    }

    if ( !hasRestoreMaximizedProperty(hWnd) ) {
        m_restoreMaximizedAfterMinimize = false;
        return;
    }

    m_restoreMaximizedAfterMinimize = true;
    m_lastKnownMaximized            = true;
    if ( IsIconic(hWnd) ) {
        return;
    }

    if ( IsZoomed(hWnd) ) {
        m_restoreMaximizedAfterMinimize = false;
        setRestoreMaximizedProperty(hWnd, false);
        return;
    }

    if ( m_maximizedRestorePosted ) {
        return;
    }

    m_maximizedRestorePosted = true;
    PostMessageW(hWnd, APPLY_MAXIMIZED_RESTORE_MESSAGE, 0, 0);
}

void Win32WindowAdapter::applyQueuedMaximizedRestore(HWND hWnd)
{
    m_maximizedRestorePosted = false;
    if ( !hWnd || m_applyingMaximizedRestore ) {
        return;
    }

    if ( !hasRestoreMaximizedProperty(hWnd) ) {
        m_restoreMaximizedAfterMinimize = false;
        return;
    }

    if ( IsIconic(hWnd) ) {
        return;
    }

    if ( IsZoomed(hWnd) ) {
        m_restoreMaximizedAfterMinimize = false;
        setRestoreMaximizedProperty(hWnd, false);
        return;
    }

    m_restoreMaximizedAfterMinimize = true;
    m_lastKnownMaximized            = true;
    m_applyingMaximizedRestore      = true;
    ShowWindow(hWnd, SW_SHOWMAXIMIZED);
    m_applyingMaximizedRestore = false;

    if ( IsZoomed(hWnd) ) {
        m_restoreMaximizedAfterMinimize = false;
        setRestoreMaximizedProperty(hWnd, false);
    }
}

LRESULT CALLBACK Win32WindowAdapter::WindowProc(HWND hWnd, UINT uMsg,
                                                WPARAM wParam, LPARAM lParam,
                                                UINT_PTR  uIdSubclass,
                                                DWORD_PTR dwRefData)
{
    Win32WindowAdapter* adapter =
        reinterpret_cast<Win32WindowAdapter*>(dwRefData);

    if ( adapter ) {
        switch ( uMsg ) {
        case APPLY_MAXIMIZED_RESTORE_MESSAGE:
            adapter->applyQueuedMaximizedRestore(hWnd);
            return 0;
        case CLEAR_RESTORE_IGNORE_MESSAGE:
            adapter->m_ignoreNextRestoreSysCommand = false;
            adapter->m_restoreIgnoreClearPosted    = false;
            return 0;
        case WM_SYSCOMMAND:
            if ( (wParam & 0xFFF0) == SC_RESTORE &&
                 adapter->m_ignoreNextRestoreSysCommand && IsZoomed(hWnd) &&
                 !IsIconic(hWnd) ) {
                adapter->m_ignoreNextRestoreSysCommand = false;
                return 0;
            }
            if ( (wParam & 0xFFF0) == SC_MINIMIZE ) {
                adapter->rememberRestoreStateBeforeMinimize(hWnd);
            } else if ( (wParam & 0xFFF0) == SC_RESTORE ) {
                adapter->restoreMaximizedAfterMinimize(hWnd);
            }
            break;
        case WM_SIZE:
            if ( wParam == SIZE_MAXIMIZED ) {
                const bool restoredMaximizedFromMinimize =
                    adapter->m_restoreMaximizedAfterMinimize ||
                    adapter->hasRestoreMaximizedProperty(hWnd);
                adapter->m_lastKnownMaximized            = true;
                adapter->m_restoreMaximizedAfterMinimize = false;
                setRestoreMaximizedProperty(hWnd, false);
                if ( restoredMaximizedFromMinimize ) {
                    adapter->m_ignoreNextRestoreSysCommand = true;
                    if ( !adapter->m_restoreIgnoreClearPosted ) {
                        adapter->m_restoreIgnoreClearPosted = true;
                        PostMessageW(hWnd, CLEAR_RESTORE_IGNORE_MESSAGE, 0, 0);
                    }
                }
            } else if ( wParam == SIZE_MINIMIZED ) {
                adapter->rememberRestoreStateBeforeMinimize(hWnd);
            } else if ( wParam == SIZE_RESTORED ) {
                adapter->restoreMaximizedAfterMinimize(hWnd);
                if ( !adapter->m_restoreMaximizedAfterMinimize &&
                     !adapter->hasRestoreMaximizedProperty(hWnd) &&
                     !IsZoomed(hWnd) ) {
                    adapter->m_lastKnownMaximized = false;
                    clearRestoreToMaximizedFlag(hWnd);
                }
            }
            break;
        case WM_ACTIVATE:
            if ( LOWORD(wParam) != WA_INACTIVE ) {
                adapter->restoreMaximizedAfterMinimize(hWnd);
            }
            break;
        default: break;
        }
    }

    if ( uMsg == WM_NCCALCSIZE && wParam == TRUE ) {
        LPNCCALCSIZE_PARAMS params =
            reinterpret_cast<LPNCCALCSIZE_PARAMS>(lParam);

        WINDOWPLACEMENT wp;
        wp.length = sizeof(WINDOWPLACEMENT);
        if ( GetWindowPlacement(hWnd, &wp) && wp.showCmd == SW_SHOWMAXIMIZED ) {
            // 当窗口最大化时，系统会给窗口一个超出屏幕范围的负偏移（通常是边框宽度）
            // 我们需要根据监视器的实际工作区调整它
            HMONITOR hMonitor =
                MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            if ( GetMonitorInfo(hMonitor, &mi) ) {
                params->rgrc[0] = mi.rcWork;
            }
        }
        return 0;
    }

    if ( uMsg == WM_NCHITTEST ) {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &pt);
        RECT rect;
        GetClientRect(hWnd, &rect);

        int  border = 8;  // 响应缩放的边缘宽度
        bool left   = pt.x < border;
        bool right  = pt.x > rect.right - border;
        bool top    = pt.y < border;
        bool bottom = pt.y > rect.bottom - border;

        if ( top && left ) return HTTOPLEFT;
        if ( top && right ) return HTTOPRIGHT;
        if ( bottom && left ) return HTBOTTOMLEFT;
        if ( bottom && right ) return HTBOTTOMRIGHT;
        if ( left ) return HTLEFT;
        if ( right ) return HTRIGHT;
        if ( top ) return HTTOP;
        if ( bottom ) return HTBOTTOM;

        // --- 核心修改：动态读取 UI 层上报的拖拽区域 ---
        if ( adapter ) {
            for ( const auto& area : adapter->m_dragAreas ) {
                if ( pt.x >= area.x && pt.x <= (area.x + area.w) &&
                     pt.y >= area.y && pt.y <= (area.y + area.h) ) {
                    return HTCAPTION;
                }
            }
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

}  // namespace MMM::Graphic

#endif
