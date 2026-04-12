#include "graphic/glfw/window/NativeWindow.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/input/glfw/GLFWKeyEvent.h"
#include "event/input/glfw/GLFWMouseEvent.h"
#include "event/input/translators/GLFWTranslator.h"
#include "event/input/translators/UniversalCodepoint.h"
#include "event/ui/GLFWNativeEvent.h"
#include "log/colorful-log.h"
#include <GLFW/glfw3.h>

#ifdef _WIN32
#    include "graphic/glfw/window/adapters/Win32WindowAdapter.h"
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

    XINFO("GLFW Version: {}", glfwGetVersionString());
    XINFO("GLFW Platform code: {}", glfwGetPlatform());

    // 隐藏系统标题栏
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
#ifdef _WIN32
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif

    // 不再需要初始化 ImGui 的辅助窗口
    m_windowHandle = glfwCreateWindow(w, h, wtitle, nullptr, nullptr);

    // 窗口启动时居中
    if ( m_windowHandle ) {
        float xscale, yscale;
        glfwGetWindowContentScale(m_windowHandle, &xscale, &yscale);

        int actualW, actualH;
        glfwGetWindowSize(m_windowHandle, &actualW, &actualH);

#ifdef _WIN32
        // Windows 下如果内容缩放大于 1.0 且窗口尚未缩放，则手动调整
        if ( xscale > 1.0f || yscale > 1.0f ) {
            int scaledW = static_cast<int>(w * xscale);
            int scaledH = static_cast<int>(h * yscale);
            if ( actualW < scaledW ) {
                glfwSetWindowSize(m_windowHandle, scaledW, scaledH);
                actualW = scaledW;
                actualH = scaledH;
            }
        }
#endif

        GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode    = glfwGetVideoMode(monitor);
        if ( monitor && mode ) {
            int xPos = (mode->width - actualW) / 2;
            int yPos = (mode->height - actualH) / 2;
            glfwSetWindowPos(m_windowHandle, xPos, yPos);
        }
    }

    // 在 glfwCreateWindow 之后调用
#ifdef _WIN32
    m_win32Adapter = std::make_unique<Win32WindowAdapter>(m_windowHandle);
#endif

    // 隐藏系统原生光标
    glfwSetInputMode(m_windowHandle, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);


    // 设置用户指针，方便回调函数访问类成员
    glfwSetWindowUserPointer(m_windowHandle, this);
    glfwSetFramebufferSizeCallback(m_windowHandle,
                                   &NativeWindow::framebufferResizeCallback);
    glfwSetKeyCallback(m_windowHandle, GLFW_KeyCallback);
    glfwSetDropCallback(m_windowHandle, GLFW_DropCallback);

    // 窗口最大化/还原回调 (处理系统级别的状态变更)
    glfwSetWindowMaximizeCallback(
        m_windowHandle, [](GLFWwindow* w, int maximized) {
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE,
                .hasStateChange = true,
                .isMaximized    = (maximized == GLFW_TRUE) });
        });

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

    Event::EventBus::instance().subscribe<Event::GLFWNativeEvent>(
        [&](Event::GLFWNativeEvent e) {
            if ( e.hasStateChange ) return;  // 忽略仅用于状态通知的事件

            switch ( e.type ) {
            case Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE: {
                // 获取当前最大化状态
                int maximized =
                    glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED);
                if ( maximized ) {
                    glfwRestoreWindow(m_windowHandle);
                    XINFO("Window restored.");
                } else {
                    glfwMaximizeWindow(m_windowHandle);
                    XINFO("Window maximized.");
                }
                break;
            }
            case Event::NativeEventType::GLFW_ICONFY_WINDOW: {
                // 最小化窗口
                glfwIconifyWindow(m_windowHandle);
                XINFO("Window iconified.");
                break;
            }
            case Event::NativeEventType::GLFW_CLOSE_WINDOW: {
                glfwSetWindowShouldClose(m_windowHandle, GLFW_TRUE);
                break;
            }
            }
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


void NativeWindow::GLFW_DropCallback(GLFWwindow* w, int count,
                                     const char** paths)
{
    XINFO("GLFW Drop Callback triggered! count: {}", count);
    MMM::Event::GLFWDropEvent e;
    for ( int i = 0; i < count; ++i ) {
        XINFO("  Dropped path[{}]: {}", i, paths[i]);
        e.paths.emplace_back(paths[i]);
    }
    double xpos, ypos;
    glfwGetCursorPos(w, &xpos, &ypos);
    e.pos = { static_cast<float>(xpos), static_cast<float>(ypos) };

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
