#ifdef _WIN32

#    include "graphic/glfw/window/adapters/Win32WindowAdapter.h"
#    include "event/core/EventBus.h"
#    include "log/colorful-log.h"
#    include <GLFW/glfw3.h>
#    define GLFW_EXPOSE_NATIVE_WIN32
#    include <GLFW/glfw3native.h>
#    include <commctrl.h>  // For DefSubclassProc
#    include <dwmapi.h>

namespace MMM::Graphic
{

Win32WindowAdapter::Win32WindowAdapter(GLFWwindow* window) : m_window(window)
{
    m_hwnd = glfwGetWin32Window(m_window);

    // 订阅拖拽区域更新事件
    Event::EventBus::instance().subscribe<Event::UpdateDragAreaEvent>(
        [this](Event::UpdateDragAreaEvent e) { this->onUpdateDragArea(e); });

    // 安装窗口子类过程
    SetWindowSubclass(m_hwnd, WindowProc, 0, (DWORD_PTR)this);

    // 方案 1：强制开启阴影（即使是无边框）
    const MARGINS shadow_margin = { 3, 3, 3, 3 };
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
        RemoveWindowSubclass(m_hwnd, WindowProc, 0);
    }
}

void Win32WindowAdapter::onUpdateDragArea(const Event::UpdateDragAreaEvent& e)
{
    // 更新拖拽区域缓存
    m_dragAreas = e.areas;
}

LRESULT CALLBACK Win32WindowAdapter::WindowProc(HWND hWnd, UINT uMsg,
                                                WPARAM wParam, LPARAM lParam,
                                                UINT_PTR  uIdSubclass,
                                                DWORD_PTR dwRefData)
{
    Win32WindowAdapter* adapter =
        reinterpret_cast<Win32WindowAdapter*>(dwRefData);

    if ( uMsg == WM_NCHITTEST ) {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
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