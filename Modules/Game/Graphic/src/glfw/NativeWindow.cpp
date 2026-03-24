#include "graphic/glfw/window/NativeWindow.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWKeyEvent.h"
#include "event/input/glfw/GLFWMouseEvent.h"
#include "event/input/translators/GLFWTranslator.h"
#include "event/input/translators/UniversalCodepoint.h"
#include "log/colorful-log.h"

#ifdef WIN32
#    define GLFW_EXPOSE_NATIVE_WIN32
#    include <GLFW/glfw3native.h>
#    include <dwmapi.h>
// 窗口过程拦截器
LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
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

        // 如果你需要像原标题栏一样拖动窗口，可以自定义一块区域返回 HTCAPTION
        // if (pt.y < 30) return HTCAPTION;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}
#endif

namespace MMM::Graphic
{
double NativeWindow::s_lastMouseX{ 0. };
double NativeWindow::s_lastMouseY{ 0. };
bool   NativeWindow::s_firstMouse{ true };

NativeWindow::NativeWindow(int w, int h, const char* wtitle)
{
    if ( !glfwVulkanSupported() ) {
        XERROR("GLFW: Vulkan Not Supported");
    }
    // 隐藏系统标题栏
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);

    // 不再需要初始化 ImGui 的辅助窗口
    m_windowHandle = glfwCreateWindow(w, h, wtitle, nullptr, nullptr);

    // 在 glfwCreateWindow 之后调用
#ifdef WIN32
    HWND hwnd = glfwGetWin32Window(m_windowHandle);

    SetWindowSubclass(hwnd, WindowProc, 0, 0);

    // 方案 1：强制开启阴影（即使是无边框）
    const MARGINS shadow_margin = { 3, 3, 3, 3 };
    DwmExtendFrameIntoClientArea(hwnd, &shadow_margin);

    // 方案 2：如果是 Windows 11，甚至可以设置圆角
    DWORD count = DWMWCP_ROUND;
    DwmSetWindowAttribute(
        hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &count, sizeof(count));
#endif

    // 隐藏系统原生光标
    glfwSetInputMode(m_windowHandle, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);


    // 设置用户指针，方便回调函数访问类成员
    glfwSetWindowUserPointer(m_windowHandle, this);
    glfwSetFramebufferSizeCallback(m_windowHandle,
                                   &NativeWindow::framebufferResizeCallback);
    glfwSetKeyCallback(m_windowHandle, GLFW_KeyCallback);
    // 1. 鼠标点击事件封装
    glfwSetMouseButtonCallback(
        m_windowHandle, [](GLFWwindow* w, int button, int action, int mods) {
            MMM::Event::GLFWMouseButtonEvent e;

            // 转换平台特定的参数
            e.button = MMM::Event::Translator::GLFW::GetMouseButton(button);
            e.action = MMM::Event::Translator::GLFW::GetAction(action);
            e.mods   = MMM::Event::Translator::GLFW::GetMods(mods);

            // GLFW 按钮回调不带坐标，需要主动获取
            double xpos, ypos;
            glfwGetCursorPos(w, &xpos, &ypos);
            e.pos = { static_cast<float>(xpos), static_cast<float>(ypos) };

            MMM::Event::EventBus::instance().publish(e);
        });

    // 2. 鼠标移动事件封装 (含 Delta 计算)
    glfwSetCursorPosCallback(
        m_windowHandle, [](GLFWwindow* w, double xpos, double ypos) {
            if ( s_firstMouse ) {
                s_lastMouseX = xpos;
                s_lastMouseY = ypos;
                s_firstMouse = false;
            }

            MMM::Event::GLFWMouseMoveEvent e;
            e.pos = { static_cast<float>(xpos), static_cast<float>(ypos) };

            // 计算相对上一帧的增量 (对于自由视角摄像机极为重要)
            e.delta = { static_cast<float>(xpos - s_lastMouseX),
                        static_cast<float>(ypos - s_lastMouseY) };

            // 更新缓存
            s_lastMouseX = xpos;
            s_lastMouseY = ypos;

            // 鼠标移动通常不需要修饰键，如果需要，可以调用 glfwGetKey 判定
            MMM::Event::EventBus::instance().publish(e);
        });

    // 3. 鼠标滚轮事件封装
    glfwSetScrollCallback(
        m_windowHandle, [](GLFWwindow* w, double xoffset, double yoffset) {
            MMM::Event::GLFWMouseScrollEvent e;
            e.offset = { static_cast<float>(xoffset),
                         static_cast<float>(yoffset) };
            // 获取滚轮发生时的坐标
            double xpos, ypos;
            glfwGetCursorPos(w, &xpos, &ypos);
            e.pos = { static_cast<float>(xpos), static_cast<float>(ypos) };

            MMM::Event::EventBus::instance().publish(e);
        });
}

NativeWindow::~NativeWindow()
{
    if ( m_windowHandle ) {
        glfwDestroyWindow(m_windowHandle);
    }
}

void NativeWindow::framebufferResizeCallback(GLFWwindow* window, int w, int h)
{
    auto app =
        reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(window));
    app->m_lastResizeTime = std::chrono::steady_clock::now();
    app->m_resizePending  = true;
}

void NativeWindow::GLFW_KeyCallback(GLFWwindow* w, int key, int scancode,
                                    int action, int mods)
{
    MMM::Event::GLFWKeyEvent e;

    // 1. 平台相关的转换 (利用 GLFWTranslator)
    e.key      = MMM::Event::Translator::GLFW::GetKey(key);
    e.action   = MMM::Event::Translator::GLFW::GetAction(action);
    e.mods     = MMM::Event::Translator::GLFW::GetMods(mods);
    e.scancode = scancode;

    // 2. 跨平台统一的字符推导
    if ( e.action != MMM::Event::Input::Action::Release ) {
        e.codepoint = MMM::Event::Translator::ResolveCodepoint(e.key, e.mods);
    } else {
        e.codepoint = 0;
    }

    // 3. 发布归一化后的事件
    MMM::Event::EventBus::instance().publish(e);
}


bool NativeWindow::shouldClose() const
{
    return m_windowHandle && glfwWindowShouldClose(m_windowHandle);
}

void NativeWindow::pollEvents() const
{
    glfwPollEvents();  // 处理操作系统的鼠标、键盘、关闭等事件
}

GLFWwindow* NativeWindow::getWindowHandle() const
{
    return m_windowHandle;
}

void NativeWindow::getFramebufferSize(int& width, int& height) const
{
    glfwGetFramebufferSize(m_windowHandle, &width, &height);
}

/**
 * @brief 全屏
 */
void NativeWindow::ToggleFullscreen()
{
    if ( glfwGetWindowMonitor(m_windowHandle) ) {
        // --- 退出全屏，恢复窗口模式 ---
        glfwSetWindowMonitor(m_windowHandle,
                             nullptr,
                             m_backupPos[0],
                             m_backupPos[1],
                             m_backupSize[0],
                             m_backupSize[1],
                             0);
        XINFO("Restored window to {}x{} at ({},{})",
              m_backupSize[0],
              m_backupSize[1],
              m_backupPos[0],
              m_backupPos[1]);
    } else {
        // --- 进入全屏前，备份当前窗口状态 ---
        glfwGetWindowPos(m_windowHandle, &m_backupPos[0], &m_backupPos[1]);
        glfwGetWindowSize(m_windowHandle, &m_backupSize[0], &m_backupSize[1]);

        // 执行全屏切换
        GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode    = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(m_windowHandle,
                             monitor,
                             0,
                             0,
                             mode->width,
                             mode->height,
                             mode->refreshRate);
        XINFO("Entered fullscreen mode.");
    }
}

}  // namespace MMM::Graphic
