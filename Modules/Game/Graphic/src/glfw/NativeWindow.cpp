#include "graphic/glfw/window/NativeWindow.h"
#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/input/glfw/GLFWKeyEvent.h"
#include "event/input/glfw/GLFWMouseEvent.h"
#include "event/input/translators/GLFWTranslator.h"
#include "event/input/translators/UniversalCodepoint.h"
#include "event/ui/GLFWNativeEvent.h"
#include "graphic/glfw/window/adapters/IWindowFrameAdapter.h"
#include "log/colorful-log.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stb_image.h>
#include <vector>

#ifdef _WIN32
#    include "graphic/glfw/window/adapters/Win32WindowAdapter.h"
#    define GLFW_EXPOSE_NATIVE_WIN32
#    include <GLFW/glfw3native.h>
#    include <windows.h>
#endif

#if defined(MMM_ENABLE_X11_FRAME_INTERACTION)
#    include "graphic/glfw/window/adapters/X11WindowAdapter.h"
#endif

#if defined(__APPLE__)
#    include "graphic/glfw/window/adapters/MacOSWindowAdapter.h"
#    include "graphic/glfw/window/adapters/MacOSWindowUtils.h"
#endif

namespace MMM::Graphic
{
double NativeWindow::s_lastMouseX{ 0. };
double NativeWindow::s_lastMouseY{ 0. };
bool   NativeWindow::s_firstMouse{ true };

namespace
{
/// @brief 判断历史尺寸是否贴近显示器工作区时允许的像素误差。
constexpr int MAXIMIZED_PLACEMENT_TOLERANCE = 8;

/// @brief 用于判断保存窗口尺寸是否贴近显示器边界的尺寸信息。
struct MonitorPlacementBounds {
    /// @brief 显示器可用区域左上角 X 坐标。
    int m_x{ 0 };

    /// @brief 显示器可用区域左上角 Y 坐标。
    int m_y{ 0 };

    /// @brief 可用于窗口最大化尺寸判断的显示器宽度。
    int m_width{ 0 };

    /// @brief 可用于窗口最大化尺寸判断的显示器高度。
    int m_height{ 0 };
};

/// @brief 查询显示器完整视频模式边界。
/// @param monitor GLFW 显示器句柄。
/// @param bounds 输出显示器尺寸。
/// @return 查询成功时返回 true。
bool queryMonitorVideoBounds(GLFWmonitor*            monitor,
                             MonitorPlacementBounds& bounds)
{
    if ( !monitor ) {
        return false;
    }

    int monitorX = 0;
    int monitorY = 0;
    glfwGetMonitorPos(monitor, &monitorX, &monitorY);

    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    if ( !mode ) {
        return false;
    }

    bounds.m_x      = monitorX;
    bounds.m_y      = monitorY;
    bounds.m_width  = mode->width;
    bounds.m_height = mode->height;
    return true;
}

/// @brief 查询显示器工作区边界，失败时回落到完整视频模式边界。
/// @param monitor GLFW 显示器句柄。
/// @param bounds 输出显示器尺寸。
/// @return 查询成功时返回 true。
bool queryMonitorWorkAreaBounds(GLFWmonitor*            monitor,
                                MonitorPlacementBounds& bounds)
{
    if ( !monitor ) {
        return false;
    }

#if defined(__APPLE__)
    return queryMonitorVideoBounds(monitor, bounds);
#else
    int workAreaX      = 0;
    int workAreaY      = 0;
    int workAreaWidth  = 0;
    int workAreaHeight = 0;
    glfwGetMonitorWorkarea(
        monitor, &workAreaX, &workAreaY, &workAreaWidth, &workAreaHeight);

    if ( workAreaWidth <= 0 || workAreaHeight <= 0 ) {
        return queryMonitorVideoBounds(monitor, bounds);
    }

    bounds.m_x      = workAreaX;
    bounds.m_y      = workAreaY;
    bounds.m_width  = workAreaWidth;
    bounds.m_height = workAreaHeight;
    return true;
#endif
}

/// @brief 查询主显示器上可用于窗口位置恢复判断的尺寸。
/// @param bounds 输出显示器尺寸。
/// @return 查询成功时返回 true。
/// @warning 低频窗口恢复路径：仅在应用项目窗口状态时执行；macOS 上避免
/// glfwGetMonitorWorkarea 触发 Cocoa 无 NSScreen 的平台错误。
bool queryPrimaryMonitorPlacementBounds(MonitorPlacementBounds& bounds)
{
    return queryMonitorWorkAreaBounds(glfwGetPrimaryMonitor(), bounds);
}

/// @brief 计算两个一维区间重叠长度。
/// @param firstMin 第一个区间起点。
/// @param firstMax 第一个区间终点。
/// @param secondMin 第二个区间起点。
/// @param secondMax 第二个区间终点。
/// @return 重叠长度；不重叠时返回 0。
int intervalOverlap(int firstMin, int firstMax, int secondMin, int secondMax)
{
    return std::max(
        0, std::min(firstMax, secondMax) - std::max(firstMin, secondMin));
}

/// @brief 根据窗口与显示器的重叠面积选择当前显示器。
/// @param window GLFW 窗口句柄。
/// @return 最匹配的显示器；无法解析时返回主显示器。
GLFWmonitor* findBestMonitorForWindow(GLFWwindow* window)
{
    if ( !window ) {
        return glfwGetPrimaryMonitor();
    }

    int windowX      = 0;
    int windowY      = 0;
    int windowWidth  = 0;
    int windowHeight = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    glfwGetWindowSize(window, &windowWidth, &windowHeight);

    int           monitorCount = 0;
    GLFWmonitor** monitors     = glfwGetMonitors(&monitorCount);
    GLFWmonitor*  bestMonitor  = nullptr;
    int           bestArea     = -1;

    for ( int i = 0; monitors && i < monitorCount; ++i ) {
        MonitorPlacementBounds monitorBounds;
        if ( !queryMonitorVideoBounds(monitors[i], monitorBounds) ) {
            continue;
        }

        const int overlapX =
            intervalOverlap(windowX,
                            windowX + windowWidth,
                            monitorBounds.m_x,
                            monitorBounds.m_x + monitorBounds.m_width);
        const int overlapY =
            intervalOverlap(windowY,
                            windowY + windowHeight,
                            monitorBounds.m_y,
                            monitorBounds.m_y + monitorBounds.m_height);
        const int overlapArea = overlapX * overlapY;
        if ( overlapArea > bestArea ) {
            bestArea    = overlapArea;
            bestMonitor = monitors[i];
        }
    }

    return bestMonitor ? bestMonitor : glfwGetPrimaryMonitor();
}

/// @brief 广播窗口最大化状态变化给自绘标题栏。
/// @param isMaximized 当前窗口是否最大化。
void publishWindowMaximizedState(bool isMaximized)
{
    Event::EventBus::instance().publish(Event::GLFWNativeEvent{
        .type           = Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE,
        .hasStateChange = true,
        .isMaximized    = isMaximized });
}

#ifdef _WIN32
/// @brief HWND 属性名，与 Win32WindowAdapter/renderer
/// 协同保留最小化前最大化状态。
constexpr const wchar_t* RESTORE_MAXIMIZED_PROP =
    L"MMMRestoreMaximizedAfterMinimize";

/// @brief 让最小化窗口在下一次激活时恢复为最大化的 Win32 WINDOWPLACEMENT 标志。
constexpr UINT RESTORE_TO_MAXIMIZED_FLAG = 0x0002;

/// @brief 判断 Win32 placement 是否直接表示当前窗口最大化。
/// @param placement Win32 窗口布局信息。
/// @return 当前 show command 是最大化时返回 true。
bool win32PlacementShowsMaximized(const WINDOWPLACEMENT& placement)
{
    return placement.showCmd == SW_SHOWMAXIMIZED;
}

/// @brief 清除 Win32 最小化后恢复最大化的系统 hint。
/// @param hwnd Win32 窗口句柄。
void clearWin32RestoreToMaximizedFlag(HWND hwnd)
{
    if ( !hwnd ) {
        return;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(WINDOWPLACEMENT);
    if ( !GetWindowPlacement(hwnd, &placement) ||
         (placement.flags & RESTORE_TO_MAXIMIZED_FLAG) == 0 ) {
        return;
    }

    placement.flags &= ~RESTORE_TO_MAXIMIZED_FLAG;
    SetWindowPlacement(hwnd, &placement);
}

/// @brief 判断 Win32 窗口当前是否明确处于最大化状态。
/// @param hwnd Win32 窗口句柄。
/// @return 当前窗口为最大化时返回 true。
bool win32PlacementWantsMaximized(HWND hwnd)
{
    if ( !hwnd ) {
        return false;
    }

    if ( IsZoomed(hwnd) ) {
        return true;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(WINDOWPLACEMENT);
    if ( !GetWindowPlacement(hwnd, &placement) ) {
        return false;
    }

    return win32PlacementShowsMaximized(placement);
}

/// @brief 写入 Win32 最小化后恢复最大化的跨模块窗口属性。
/// @param window GLFW 窗口句柄。
/// @param restoreMaximized 任务栏恢复时是否应最大化。
void setWin32RestoreMaximizedProperty(GLFWwindow* window, bool restoreMaximized)
{
    if ( !window ) {
        return;
    }

    HWND hwnd = glfwGetWin32Window(window);
    if ( !hwnd ) {
        return;
    }

    if ( restoreMaximized ) {
        SetPropW(hwnd, RESTORE_MAXIMIZED_PROP, reinterpret_cast<HANDLE>(1));
    } else {
        RemovePropW(hwnd, RESTORE_MAXIMIZED_PROP);
    }
}

/// @brief 判断 GLFW 窗口是否带有恢复最大化的 Win32 属性。
/// @param window GLFW 窗口句柄。
/// @return 存在恢复最大化属性时返回 true。
bool hasWin32RestoreMaximizedProperty(GLFWwindow* window)
{
    if ( !window ) {
        return false;
    }

    HWND hwnd = glfwGetWin32Window(window);
    return hwnd && GetPropW(hwnd, RESTORE_MAXIMIZED_PROP) != nullptr;
}

/// @brief 判断 Win32 最小化/恢复流程是否应保留之前的最大化状态。
/// @param window GLFW 窗口句柄。
/// @param lastRequestedMaximized 应用最近一次请求的最大化状态。
/// @param cachedRestoreMaximized 之前缓存的最小化恢复状态。
/// @return 任务栏或 Alt+Tab 恢复时应回到最大化则返回 true。
bool shouldPreserveWin32MaximizedRestore(GLFWwindow* window,
                                         bool        lastRequestedMaximized,
                                         bool        cachedRestoreMaximized)
{
    if ( !window ) {
        return false;
    }

    HWND hwnd = glfwGetWin32Window(window);
    if ( !hwnd ) {
        return false;
    }

    return cachedRestoreMaximized || lastRequestedMaximized ||
           hasWin32RestoreMaximizedProperty(window) ||
           win32PlacementWantsMaximized(hwnd);
}

/// @brief 最小化 Win32 窗口，同时保留系统恢复目标。
/// @param hwnd Win32 窗口句柄。
/// @param restoreMaximized 下一次任务栏恢复时是否应最大化。
void minimizeWin32Window(HWND hwnd, bool restoreMaximized)
{
    if ( !hwnd ) {
        return;
    }

    if ( restoreMaximized ) {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(WINDOWPLACEMENT);
        if ( GetWindowPlacement(hwnd, &placement) ) {
            placement.flags |= RESTORE_TO_MAXIMIZED_FLAG;
            placement.showCmd = SW_SHOWMINIMIZED;
            SetWindowPlacement(hwnd, &placement);
            return;
        }
    }

    clearWin32RestoreToMaximizedFlag(hwnd);
    ShowWindow(hwnd, SW_MINIMIZE);
}

/// @brief 在 Win32 上最小化窗口，并在最小化前持久化最大化还原意图。
/// @param window GLFW 窗口句柄。
/// @param lastRequestedMaximized 最近一次请求的最大化状态。
void iconifyWin32WindowPreservingMaximize(GLFWwindow* window,
                                          bool        lastRequestedMaximized)
{
    if ( !window ) {
        return;
    }

    HWND hwnd = glfwGetWin32Window(window);
    if ( !hwnd ) {
        glfwIconifyWindow(window);
        return;
    }

    const bool restoreMaximized = shouldPreserveWin32MaximizedRestore(
        window, lastRequestedMaximized, false);
    setWin32RestoreMaximizedProperty(window, restoreMaximized);
    minimizeWin32Window(hwnd, restoreMaximized);
}
#endif
}  // namespace

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
#if defined(_WIN32) || defined(__linux__)
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif

    // 不再需要初始化 ImGui 的辅助窗口
#ifdef __linux__
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "MusicMapMaker-Next");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "MusicMapMaker-Next");
#endif
    m_windowHandle = glfwCreateWindow(w, h, wtitle, nullptr, nullptr);

    // 设置窗口图标
    if ( m_windowHandle ) {
#if defined(__APPLE__)
        enableMacOSFirstMouse(m_windowHandle);
#endif
        /// @brief 用户 .config/mmm 资源包中的窗口图标路径。
        const std::filesystem::path iconPath =
            Config::AppPaths::windowIconFilePath();
        /// @brief 以二进制方式读取窗口图标，避免 Windows 中文路径被 C fopen
        /// 误解。
        std::ifstream iconFile(iconPath, std::ios::binary);
        if ( iconFile ) {
            /// @brief 窗口图标原始文件字节。
            std::vector<unsigned char> iconBytes{
                std::istreambuf_iterator<char>(iconFile),
                std::istreambuf_iterator<char>()
            };
            /// @brief 窗口图标宽度。
            int width = 0;
            /// @brief 窗口图标高度。
            int height = 0;
            /// @brief 窗口图标原始通道数量。
            int channels = 0;
            /// @brief 解码后的 RGBA 图标像素。
            unsigned char* pixels =
                stbi_load_from_memory(iconBytes.data(),
                                      static_cast<int>(iconBytes.size()),
                                      &width,
                                      &height,
                                      &channels,
                                      4);
            if ( pixels ) {
                GLFWimage images[1];
                images[0].width  = width;
                images[0].height = height;
                images[0].pixels = pixels;
                glfwSetWindowIcon(m_windowHandle, 1, images);
                stbi_image_free(pixels);
            } else {
                XWARN("Failed to decode window icon: {}",
                      Config::pathToUtf8(iconPath));
            }
        } else {
            XWARN("Failed to open window icon: {}",
                  Config::pathToUtf8(iconPath));
        }
    }

    // 窗口启动时居中
    if ( m_windowHandle ) {
        float xscale, yscale;
        glfwGetWindowContentScale(m_windowHandle, &xscale, &yscale);

        int actualW, actualH;
        glfwGetWindowSize(m_windowHandle, &actualW, &actualH);

#if defined(_WIN32) || defined(__linux__)
        // 获取 framebuffer 尺寸以检测系统是否已经自动处理了逻辑像素缩放，例如
        // Wayland 环境。
        int fbW, fbH;
        glfwGetFramebufferSize(m_windowHandle, &fbW, &fbH);
        float fbScaleX =
            (actualW > 0) ? static_cast<float>(fbW) / actualW : 1.0f;

        // 如果内容缩放大于系统已处理的缩放 (fbScaleX)，说明在 X11 或某些
        // Windows 环境下， 我们需要手动调整窗口尺寸以匹配预期的逻辑大小
        if ( xscale > fbScaleX ) {
            int scaledW = static_cast<int>(w * xscale);
            int scaledH = static_cast<int>(h * yscale);
            if ( actualW < scaledW ) {
                glfwSetWindowSize(m_windowHandle, scaledW, scaledH);
                glfwGetWindowSize(m_windowHandle, &actualW, &actualH);
                glfwGetFramebufferSize(m_windowHandle, &fbW, &fbH);
                fbScaleX =
                    (actualW > 0) ? static_cast<float>(fbW) / actualW : 1.0f;
            }
        }
#else
        int fbW, fbH;
        glfwGetFramebufferSize(m_windowHandle, &fbW, &fbH);
        float fbScaleX =
            (actualW > 0) ? static_cast<float>(fbW) / actualW : 1.0f;
#endif

        // 同步缩放到全局配置
        // nativeContentScale 是原生 DPI (用于字体加载)
        // uiScale 是我们需要手动补偿的比例 (xscale / fbScaleX)
        Config::AppConfig::instance().setNativeContentScale(xscale);
        Config::AppConfig::instance().setUIScale(xscale / fbScaleX);

        XINFO("Detected scales: native={}, fb_auto={}, ui_manual={}",
              xscale,
              fbScaleX,
              xscale / fbScaleX);

        GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
#if defined(__APPLE__)
        if ( centerMacOSWindowInVisibleFrame(m_windowHandle, w, h) ) {
            glfwGetWindowSize(m_windowHandle, &actualW, &actualH);
        } else if ( monitor && mode ) {
#else
        if ( monitor && mode ) {
#endif
            int xPos = (mode->width - actualW) / 2;
            int yPos = (mode->height - actualH) / 2;
            glfwSetWindowPos(m_windowHandle, xPos, yPos);
        }
        if ( monitor && mode ) {
            // 写入屏幕刷新率到全局配置，供逻辑线程等限制最高帧率使用
            Config::AppConfig::instance().setDeviceRefreshRate(
                mode->refreshRate);
            XINFO("Detected primary monitor refresh rate: {} Hz",
                  mode->refreshRate);
        }
    }

    rememberCurrentWindowPlacement();
    if ( m_windowHandle ) {
#if defined(__APPLE__)
        m_lastRequestedMaximized = false;
        publishWindowMaximizedState(false);
#else
        m_lastRequestedMaximized =
            glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED) == GLFW_TRUE;
#endif
    }

    // 在 glfwCreateWindow 之后调用
#if defined(_WIN32)
    m_windowFrameAdapter = std::make_unique<Win32WindowAdapter>(m_windowHandle);
#endif
#if defined(__APPLE__)
    m_windowFrameAdapter = std::make_unique<MacOSWindowAdapter>(*this);
#endif
#if defined(MMM_ENABLE_X11_FRAME_INTERACTION)
    if ( glfwGetPlatform() == GLFW_PLATFORM_X11 ) {
        m_windowFrameAdapter = std::make_unique<X11WindowAdapter>(*this);
    }
#endif

    // 隐藏系统原生光标
    glfwSetInputMode(m_windowHandle, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
#ifdef __linux__
    // 确保不禁止系统快捷键 (针对 Wayland)
    if ( glfwRawMouseMotionSupported() ) {  // 只是为了检查是否是现代 GLFW
        // GLFW_MOUSE_PASSTHROUGH 也可以考虑，但这里不是需要的
    }
// 显式确保不拦截系统组合键
#    if defined(GLFW_KEYBOARD_SHORTCUTS_INHIBIT)
    glfwSetInputMode(
        m_windowHandle, GLFW_KEYBOARD_SHORTCUTS_INHIBIT, GLFW_FALSE);
#    endif
#endif


    // 设置用户指针，方便回调函数访问类成员
    glfwSetWindowUserPointer(m_windowHandle, this);
    glfwSetFramebufferSizeCallback(m_windowHandle,
                                   &NativeWindow::framebufferResizeCallback);
    glfwSetWindowPosCallback(m_windowHandle, &NativeWindow::windowPosCallback);
    glfwSetWindowSizeCallback(m_windowHandle,
                              &NativeWindow::windowSizeCallback);
    glfwSetWindowIconifyCallback(m_windowHandle,
                                 &NativeWindow::GLFW_IconifyCallback);
    glfwSetWindowFocusCallback(m_windowHandle, [](GLFWwindow* w, int focused) {
        auto app = reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(w));
        if ( app && app->m_windowFrameAdapter ) {
            app->m_windowFrameAdapter->handleClientFocusChange(focused ==
                                                               GLFW_TRUE);
        }
    });
    glfwSetKeyCallback(m_windowHandle, GLFW_KeyCallback);
    glfwSetDropCallback(m_windowHandle, GLFW_DropCallback);

    // 监听内容缩放变化 (跨显示器移动)
    glfwSetWindowContentScaleCallback(
        m_windowHandle, [](GLFWwindow* w, float xscale, float yscale) {
            int winW, winH, fbW, fbH;
            glfwGetWindowSize(w, &winW, &winH);
            glfwGetFramebufferSize(w, &fbW, &fbH);
            float fbScaleX = (winW > 0) ? static_cast<float>(fbW) / winW : 1.0f;

            Config::AppConfig::instance().setNativeContentScale(xscale);
            Config::AppConfig::instance().setUIScale(xscale / fbScaleX);

            XINFO("Content scale changed: native={}, ui={}",
                  xscale,
                  xscale / fbScaleX);

            auto app =
                reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(w));
            if ( app ) {
                app->refreshWindowFrameShape();
            }

            // 发布事件，通知 UI 重载资源
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type           = Event::NativeEventType::GLFW_WINDOW_RESIZED,
                .hasStateChange = true });
        });

    // 窗口最大化/还原回调 (处理系统级别的状态变更)
    glfwSetWindowMaximizeCallback(
        m_windowHandle, [](GLFWwindow* w, int maximized) {
            auto app =
                reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(w));
            if ( app ) {
#if defined(__APPLE__)
                (void)maximized;
                app->m_lastRequestedMaximized = app->m_emulatedMaximized;
                app->refreshWindowFrameShape();
                publishWindowMaximizedState(app->m_emulatedMaximized);
                return;
#else
                const bool isMaximized   = maximized == GLFW_TRUE;
                app->m_emulatedMaximized = false;
                if ( isMaximized ) {
                    app->m_lastRequestedMaximized = true;
#    ifdef _WIN32
                    setWin32RestoreMaximizedProperty(w, false);
#    endif
                } else {
#    ifdef _WIN32
                    const bool keepMaximizedRestore =
                        shouldPreserveWin32MaximizedRestore(
                            w, false, app->m_restoreMaximizedAfterIconify);
#    else
                    constexpr bool keepMaximizedRestore = false;
#    endif
                    if ( !keepMaximizedRestore ) {
                        app->m_lastRequestedMaximized = false;
                        app->rememberCurrentWindowPlacement();
#    ifdef _WIN32
                        setWin32RestoreMaximizedProperty(w, false);
                        clearWin32RestoreToMaximizedFlag(glfwGetWin32Window(w));
#    endif
                    } else {
                        app->m_lastRequestedMaximized = true;
                    }
                }
                app->refreshWindowFrameShape();
#endif
            }

            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE,
                .hasStateChange = true,
                .isMaximized    = (maximized == GLFW_TRUE) });
        });

    // 1. 鼠标点击事件封装
    glfwSetMouseButtonCallback(
        m_windowHandle, [](GLFWwindow* w, int button, int action, int mods) {
            auto app =
                reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(w));

            MMM::Event::GLFWMouseButtonEvent e;

            // 转换平台特定的参数
            e.button = MMM::Event::Translator::GLFW::GetMouseButton(button);
            e.action = MMM::Event::Translator::GLFW::GetAction(action);
            e.mods   = MMM::Event::Translator::GLFW::GetMods(mods);

            // GLFW 按钮回调不带坐标，需要主动获取
            double xpos, ypos;
            glfwGetCursorPos(w, &xpos, &ypos);
            e.pos = { static_cast<float>(xpos), static_cast<float>(ypos) };

            bool frameInteractionStarted = false;
            if ( app && app->m_windowFrameAdapter ) {
                frameInteractionStarted =
                    app->m_windowFrameAdapter->handleClientMouseButton(
                        button, action, xpos, ypos);
            }

            if ( !frameInteractionStarted ) {
                MMM::Event::EventBus::instance().publish(e);
            }
        });

    // 2. 鼠标移动事件封装 (含 Delta 计算)
    glfwSetCursorPosCallback(
        m_windowHandle, [](GLFWwindow* w, double xpos, double ypos) {
            auto app =
                reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(w));
            bool frameInteractionStarted = false;
            if ( app && app->m_windowFrameAdapter ) {
                frameInteractionStarted =
                    app->m_windowFrameAdapter->handleClientCursorPos(xpos,
                                                                     ypos);
            }

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

            if ( !frameInteractionStarted ) {
                // 鼠标移动通常不需要修饰键，如果需要，可以调用 glfwGetKey 判定
                MMM::Event::EventBus::instance().publish(e);
            }
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
                toggleMaximized();
                break;
            }
            case Event::NativeEventType::GLFW_ICONFY_WINDOW: {
#ifdef _WIN32
                iconifyWin32WindowPreservingMaximize(m_windowHandle,
                                                     m_lastRequestedMaximized);
#elif defined(__APPLE__)
                if ( !miniaturizeMacOSWindow(m_windowHandle) ) {
                    glfwIconifyWindow(m_windowHandle);
                }
#else
            glfwIconifyWindow(m_windowHandle);
#endif
                XINFO("Window iconified.");
                break;
            }
            case Event::NativeEventType::GLFW_CLOSE_WINDOW: {
                glfwSetWindowShouldClose(m_windowHandle, GLFW_TRUE);
                break;
            }
            case Event::NativeEventType::GLFW_WINDOW_RESIZED:
            case Event::NativeEventType::GLFW_WINDOW_CONTENT_SCALE_CHANGED:
                break;
            }
        });

    refreshWindowFrameShape();
}

NativeWindow::~NativeWindow()
{
    m_windowFrameAdapter.reset();
    if ( m_windowHandle ) {
        glfwDestroyWindow(m_windowHandle);
    }
}

void NativeWindow::getWindowPlacement(int& x, int& y, int& width, int& height,
                                      bool& maximized) const
{
    x         = 100;
    y         = 100;
    width     = 1280;
    height    = 720;
    maximized = false;

    if ( !m_windowHandle ) {
        return;
    }

    glfwGetWindowPos(m_windowHandle, &x, &y);
    glfwGetWindowSize(m_windowHandle, &width, &height);
    maximized = isWindowMaximized();

    if ( maximized ) {
        x      = m_normalWindowPos[0];
        y      = m_normalWindowPos[1];
        width  = m_normalWindowSize[0];
        height = m_normalWindowSize[1];
    }
}

void NativeWindow::applyWindowPlacement(int x, int y, int width, int height,
                                        bool maximized)
{
    if ( !m_windowHandle || width <= 0 || height <= 0 ) {
        return;
    }

    int restoreX      = x;
    int restoreY      = y;
    int restoreWidth  = width;
    int restoreHeight = height;
    if ( maximized && isLikelyMaximizedPlacement(width, height) ) {
        restoreX      = m_normalWindowPos[0];
        restoreY      = m_normalWindowPos[1];
        restoreWidth  = m_normalWindowSize[0];
        restoreHeight = m_normalWindowSize[1];
    }

    rememberWindowPlacement(restoreX, restoreY, restoreWidth, restoreHeight);
    m_emulatedMaximized = false;

    m_lastRequestedMaximized = maximized;
#ifdef _WIN32
    if ( !maximized ) {
        m_restoreMaximizedAfterIconify = false;
        setWin32RestoreMaximizedProperty(m_windowHandle, false);
        clearWin32RestoreToMaximizedFlag(glfwGetWin32Window(m_windowHandle));
    }
#endif

    if ( glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED) == GLFW_TRUE ) {
        glfwRestoreWindow(m_windowHandle);
    }

    setWindowPlacementWithoutRemembering(
        restoreX, restoreY, restoreWidth, restoreHeight);

    if ( maximized ) {
#if defined(__APPLE__)
        if ( applyEmulatedMaximizedPlacement() ) {
            publishWindowMaximizedState(true);
        } else {
            glfwMaximizeWindow(m_windowHandle);
        }
#else
        glfwMaximizeWindow(m_windowHandle);
#endif
        rememberWindowPlacement(
            restoreX, restoreY, restoreWidth, restoreHeight);
    }
    refreshWindowFrameShape();
}

void NativeWindow::toggleMaximized()
{
    if ( !m_windowHandle ) {
        return;
    }

    const bool maximized = isWindowMaximized();
    if ( maximized ) {
        m_lastRequestedMaximized       = false;
        m_restoreMaximizedAfterIconify = false;
#ifdef _WIN32
        setWin32RestoreMaximizedProperty(m_windowHandle, false);
#endif
#if defined(__APPLE__)
        m_emulatedMaximized = false;
        setWindowPlacementWithoutRemembering(m_normalWindowPos[0],
                                             m_normalWindowPos[1],
                                             m_normalWindowSize[0],
                                             m_normalWindowSize[1]);
        publishWindowMaximizedState(false);
#else
        glfwRestoreWindow(m_windowHandle);
#endif
#ifdef _WIN32
        clearWin32RestoreToMaximizedFlag(glfwGetWin32Window(m_windowHandle));
#endif
        XINFO("Window restored.");
        refreshWindowFrameShape();
        return;
    }

    rememberCurrentWindowPlacement();
    const int restoreX       = m_normalWindowPos[0];
    const int restoreY       = m_normalWindowPos[1];
    const int restoreWidth   = m_normalWindowSize[0];
    const int restoreHeight  = m_normalWindowSize[1];
    m_lastRequestedMaximized = true;
#if defined(__APPLE__)
    if ( applyEmulatedMaximizedPlacement() ) {
        publishWindowMaximizedState(true);
    } else {
        glfwMaximizeWindow(m_windowHandle);
    }
#else
    glfwMaximizeWindow(m_windowHandle);
#endif
    rememberWindowPlacement(restoreX, restoreY, restoreWidth, restoreHeight);
    XINFO("Window maximized.");
    refreshWindowFrameShape();
}

IWindowFrameAdapter* NativeWindow::getWindowFrameAdapter() const
{
    return m_windowFrameAdapter.get();
}

GLFWwindow* NativeWindow::getFrameWindowHandle() const
{
    return m_windowHandle;
}

void NativeWindow::getNormalFramePlacement(int& x, int& y, int& width,
                                           int& height) const
{
    x      = m_normalWindowPos[0];
    y      = m_normalWindowPos[1];
    width  = m_normalWindowSize[0];
    height = m_normalWindowSize[1];
}

void NativeWindow::setNormalFramePlacement(int x, int y, int width, int height)
{
    rememberWindowPlacement(x, y, width, height);
}

bool NativeWindow::isFrameMaximized() const
{
    return isWindowMaximized();
}

bool NativeWindow::restoreFrameForClientMove(double cursorX, double cursorY)
{
    if ( !m_windowHandle || !isWindowMaximized() ) {
        return false;
    }

    int windowX      = 0;
    int windowY      = 0;
    int windowWidth  = 0;
    int windowHeight = 0;
    glfwGetWindowPos(m_windowHandle, &windowX, &windowY);
    glfwGetWindowSize(m_windowHandle, &windowWidth, &windowHeight);
    if ( windowWidth <= 0 || windowHeight <= 0 ) {
        return false;
    }

    const int   rootCursorX   = windowX + static_cast<int>(cursorX);
    const int   rootCursorY   = windowY + static_cast<int>(cursorY);
    const int   restoreWidth  = std::max(1, m_normalWindowSize[0]);
    const int   restoreHeight = std::max(1, m_normalWindowSize[1]);
    const float cursorRatioX  = std::clamp(
        static_cast<float>(cursorX) / static_cast<float>(windowWidth),
        0.0f,
        1.0f);
    const int restoreX =
        rootCursorX -
        static_cast<int>(static_cast<float>(restoreWidth) * cursorRatioX);
    const int restoreY =
        rootCursorY -
        std::clamp(static_cast<int>(cursorY), 0, restoreHeight - 1);

    m_lastRequestedMaximized       = false;
    m_restoreMaximizedAfterIconify = false;
    m_emulatedMaximized            = false;
#ifdef _WIN32
    setWin32RestoreMaximizedProperty(m_windowHandle, false);
    clearWin32RestoreToMaximizedFlag(glfwGetWin32Window(m_windowHandle));
#endif
#if !defined(__APPLE__)
    if ( glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED) == GLFW_TRUE ) {
        glfwRestoreWindow(m_windowHandle);
    }
#endif
    setWindowPlacementWithoutRemembering(
        restoreX, restoreY, restoreWidth, restoreHeight);
    rememberWindowPlacement(restoreX, restoreY, restoreWidth, restoreHeight);
    publishWindowMaximizedState(false);
    refreshWindowFrameShape();
    return true;
}

void NativeWindow::framebufferResizeCallback(GLFWwindow* window, int w, int h)
{
    auto app =
        reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(window));
    app->m_lastResizeTime = std::chrono::steady_clock::now();
    app->m_resizePending.store(true, std::memory_order_relaxed);
}

void NativeWindow::windowPosCallback(GLFWwindow* window, int x, int y)
{
    auto app =
        reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(window));
    if ( !app || !app->canRememberCurrentWindowPlacement() ) {
        return;
    }

    int width  = 0;
    int height = 0;
    glfwGetWindowSize(window, &width, &height);
    app->rememberWindowPlacement(x, y, width, height);
}

void NativeWindow::windowSizeCallback(GLFWwindow* window, int width, int height)
{
    auto app =
        reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(window));
    if ( !app || !app->canRememberCurrentWindowPlacement() ) {
        return;
    }

    int x = 0;
    int y = 0;
    glfwGetWindowPos(window, &x, &y);
    app->rememberWindowPlacement(x, y, width, height);
    app->refreshWindowFrameShape();
}

bool NativeWindow::canRememberCurrentWindowPlacement() const
{
#if defined(__APPLE__)
    return !m_ignoreWindowPlacementCallbacks && !m_emulatedMaximized &&
           m_windowHandle && glfwGetWindowMonitor(m_windowHandle) == nullptr &&
           glfwGetWindowAttrib(m_windowHandle, GLFW_ICONIFIED) != GLFW_TRUE;
#else
    return !m_ignoreWindowPlacementCallbacks && !m_emulatedMaximized &&
           m_windowHandle && glfwGetWindowMonitor(m_windowHandle) == nullptr &&
           glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED) != GLFW_TRUE &&
           glfwGetWindowAttrib(m_windowHandle, GLFW_ICONIFIED) != GLFW_TRUE;
#endif
}

bool NativeWindow::isWindowMaximized() const
{
#if defined(__APPLE__)
    return m_emulatedMaximized;
#else
    return m_emulatedMaximized ||
           (m_windowHandle &&
            glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED) == GLFW_TRUE);
#endif
}

bool NativeWindow::isLikelyMaximizedPlacement(int width, int height) const
{
    if ( !m_windowHandle || width <= 0 || height <= 0 ) {
        return false;
    }

    MonitorPlacementBounds bounds;
    if ( !queryPrimaryMonitorPlacementBounds(bounds) ) return false;

    return width >= bounds.m_width - MAXIMIZED_PLACEMENT_TOLERANCE ||
           height >= bounds.m_height - MAXIMIZED_PLACEMENT_TOLERANCE;
}

void NativeWindow::rememberCurrentWindowPlacement()
{
    if ( !canRememberCurrentWindowPlacement() ) {
        return;
    }

    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
    glfwGetWindowPos(m_windowHandle, &x, &y);
    glfwGetWindowSize(m_windowHandle, &width, &height);
    rememberWindowPlacement(x, y, width, height);
}

void NativeWindow::setWindowPlacementWithoutRemembering(int x, int y, int width,
                                                        int height)
{
    if ( !m_windowHandle || width <= 0 || height <= 0 ) {
        return;
    }

    m_ignoreWindowPlacementCallbacks = true;
    glfwSetWindowPos(m_windowHandle, x, y);
    glfwSetWindowSize(m_windowHandle, width, height);
    m_ignoreWindowPlacementCallbacks = false;
    m_lastResizeTime                 = std::chrono::steady_clock::now();
    m_resizePending.store(true, std::memory_order_relaxed);
}

bool NativeWindow::applyEmulatedMaximizedPlacement()
{
#if defined(__APPLE__)
    if ( !m_windowHandle ) {
        return false;
    }

    m_emulatedMaximized = true;
    if ( applyMacOSVisibleWindowFrame(m_windowHandle) ) {
        m_lastResizeTime = std::chrono::steady_clock::now();
        m_resizePending.store(true, std::memory_order_relaxed);
        return true;
    }

    MonitorPlacementBounds bounds;
    if ( queryMonitorWorkAreaBounds(findBestMonitorForWindow(m_windowHandle),
                                    bounds) ) {
        setWindowPlacementWithoutRemembering(
            bounds.m_x, bounds.m_y, bounds.m_width, bounds.m_height);
        return true;
    }

    m_emulatedMaximized = false;
    return false;
#else
    return false;
#endif
}

void NativeWindow::refreshWindowFrameShape()
{
    if ( m_windowFrameAdapter ) {
        m_windowFrameAdapter->refreshFrameShape();
    }
}

void NativeWindow::rememberWindowPlacement(int x, int y, int width, int height)
{
    if ( width <= 0 || height <= 0 ) {
        return;
    }

    m_normalWindowPos[0]  = x;
    m_normalWindowPos[1]  = y;
    m_normalWindowSize[0] = width;
    m_normalWindowSize[1] = height;
}

/// @brief 处理窗口最小化/恢复事件，并在任务栏恢复时保留最大化状态。
/// @param iconified GLFW 最小化状态。
void NativeWindow::handleWindowIconify(int iconified)
{
    if ( !m_windowHandle ) {
        return;
    }

    if ( iconified == GLFW_TRUE ) {
#ifdef _WIN32
        m_restoreMaximizedAfterIconify =
            shouldPreserveWin32MaximizedRestore(m_windowHandle,
                                                m_lastRequestedMaximized,
                                                m_restoreMaximizedAfterIconify);
        m_lastRequestedMaximized =
            m_lastRequestedMaximized || m_restoreMaximizedAfterIconify;
        setWin32RestoreMaximizedProperty(m_windowHandle,
                                         m_restoreMaximizedAfterIconify);
#else
        m_restoreMaximizedAfterIconify =
            m_lastRequestedMaximized || isWindowMaximized();
#endif
        return;
    }

#ifdef _WIN32
    m_restoreMaximizedAfterIconify =
        shouldPreserveWin32MaximizedRestore(m_windowHandle,
                                            m_lastRequestedMaximized,
                                            m_restoreMaximizedAfterIconify);
#endif
    if ( m_restoreMaximizedAfterIconify && !isWindowMaximized() ) {
        m_lastRequestedMaximized = true;
#if defined(__APPLE__)
        if ( applyEmulatedMaximizedPlacement() ) {
            publishWindowMaximizedState(true);
        } else {
            glfwMaximizeWindow(m_windowHandle);
        }
#else
        glfwMaximizeWindow(m_windowHandle);
#endif
    }
    m_restoreMaximizedAfterIconify = false;
    refreshWindowFrameShape();
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

/// @brief GLFW 窗口最小化/恢复回调入口。
/// @param window GLFW 窗口句柄。
/// @param iconified GLFW 最小化状态。
void NativeWindow::GLFW_IconifyCallback(GLFWwindow* window, int iconified)
{
    auto app =
        reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(window));
    if ( app ) {
        app->handleWindowIconify(iconified);
    }
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
        rememberWindowPlacement(
            m_backupPos[0], m_backupPos[1], m_backupSize[0], m_backupSize[1]);
        XINFO("Restored window to {}x{} at ({},{})",
              m_backupSize[0],
              m_backupSize[1],
              m_backupPos[0],
              m_backupPos[1]);
    } else {
        // --- 进入全屏前，备份当前窗口状态 ---
        rememberCurrentWindowPlacement();
        m_backupPos[0]  = m_normalWindowPos[0];
        m_backupPos[1]  = m_normalWindowPos[1];
        m_backupSize[0] = m_normalWindowSize[0];
        m_backupSize[1] = m_normalWindowSize[1];

        // 执行全屏切换
        GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if ( !monitor || !mode ) {
            XERROR("Failed to enter fullscreen mode: no primary monitor.");
            return;
        }
        glfwSetWindowMonitor(m_windowHandle,
                             monitor,
                             0,
                             0,
                             mode->width,
                             mode->height,
                             mode->refreshRate);
        XINFO("Entered fullscreen mode.");
    }
    refreshWindowFrameShape();
}

}  // namespace MMM::Graphic
