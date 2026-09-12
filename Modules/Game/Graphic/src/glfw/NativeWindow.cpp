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
///
/// 视频模式宽高覆盖显示器完整像素区域，不扣除任务栏或 Dock；只在工作区 API 不可
/// 用或用于计算窗口与显示器重叠时使用。
///
/// @param monitor GLFW 显示器句柄。
/// @param bounds 输出显示器尺寸。
/// @return 查询成功时返回 true。
bool queryMonitorVideoBounds(GLFWmonitor*            monitor,
                             MonitorPlacementBounds& bounds)
{
    // GLFW 可能在显示器热插拔期间返回空句柄。
    if ( !monitor ) {
        return false;
    }

    // 显示器位置处于虚拟桌面坐标，可包含负值。
    int monitorX = 0;
    int monitorY = 0;
    glfwGetMonitorPos(monitor, &monitorX, &monitorY);

    // Video mode 由 GLFW 拥有，句柄只在当前监视器状态下借用。
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    if ( !mode ) {
        return false;
    }

    // 只有位置和模式都取得成功后才一次性发布完整 bounds。
    bounds.m_x      = monitorX;
    bounds.m_y      = monitorY;
    bounds.m_width  = mode->width;
    bounds.m_height = mode->height;
    return true;
}

/// @brief 查询显示器工作区边界，失败时回落到完整视频模式边界。
///
/// 工作区排除系统面板，适合居中、最大化和尺寸钳制。macOS 路径刻意避开
/// GLFW
/// workarea 查询，由 Cocoa 专用 helper 处理可见
/// frame；此处回落到视频模式边界。
///
/// @param monitor GLFW 显示器句柄。
/// @param bounds 输出显示器尺寸。
/// @return 查询成功时返回 true。
bool queryMonitorWorkAreaBounds(GLFWmonitor*            monitor,
                                MonitorPlacementBounds& bounds)
{
    // 所有平台先验证监视器句柄，避免向 GLFW 传递空对象。
    if ( !monitor ) {
        return false;
    }

#if defined(__APPLE__)
    // GLFW 在 macOS 启动早期可能没有可用 NSScreen，使用视频模式作为保守后备。
    return queryMonitorVideoBounds(monitor, bounds);
#else
    // X11/Win32 的 workarea 坐标同样位于虚拟桌面空间。
    int workAreaX      = 0;
    int workAreaY      = 0;
    int workAreaWidth  = 0;
    int workAreaHeight = 0;
    glfwGetMonitorWorkarea(
        monitor, &workAreaX, &workAreaY, &workAreaWidth, &workAreaHeight);

    // 未知桌面环境可能不提供有效工作区，退回完整显示器仍能恢复窗口。
    if ( workAreaWidth <= 0 || workAreaHeight <= 0 ) {
        return queryMonitorVideoBounds(monitor, bounds);
    }

    // 输出只在获得正宽高后更新，调用者不会观察到半有效结构。
    bounds.m_x      = workAreaX;
    bounds.m_y      = workAreaY;
    bounds.m_width  = workAreaWidth;
    bounds.m_height = workAreaHeight;
    return true;
#endif
}

/// @brief 查询主显示器上可用于窗口位置恢复判断的尺寸。
///
/// 历史 placement
/// 只用于识别“误保存了最大化尺寸”，采用主显示器作为稳定比较
///
/// 基准，不改变窗口当前所在监视器。
///
/// @param bounds 输出显示器尺寸。
/// @return 查询成功时返回 true。
/// @warning 低频窗口恢复路径：仅在应用项目窗口状态时执行；macOS 上避免
/// glfwGetMonitorWorkarea 触发 Cocoa 无 NSScreen 的平台错误。
bool queryPrimaryMonitorPlacementBounds(MonitorPlacementBounds& bounds)
{
    return queryMonitorWorkAreaBounds(glfwGetPrimaryMonitor(), bounds);
}

/// @brief 判断当前窗口系统是否允许应用主动读取和设置窗口位置。
///
/// Wayland 顶层窗口位置由 compositor 决定，GLFW 的 get/setWindowPos
/// 不具备可靠
/// 语义；其他受支持 backend 可使用程序化 placement。
///
/// @return X11、Windows 和 macOS 返回 true；Wayland 返回 false。
bool supportsProgrammaticWindowPosition()
{
#if defined(__linux__)
    return glfwGetPlatform() != GLFW_PLATFORM_WAYLAND;
#else
    return true;
#endif
}

/// @brief 计算两个一维区间重叠长度。
///
/// 输入按半开区间 [min,max) 处理，结果钳制到零，供二维面积计算复用。
///
/// @param firstMin 第一个区间起点。
/// @param firstMax 第一个区间终点。
/// @param secondMin 第二个区间起点。
/// @param secondMax 第二个区间终点。
/// @return 重叠长度；不重叠时返回 0。
int intervalOverlap(int firstMin, int firstMax, int secondMin, int secondMax)
{
    // 先取右端最小值与左端最大值，负差表示不相交。
    return std::max(
        0, std::min(firstMax, secondMax) - std::max(firstMin, secondMin));
}

/// @brief 根据窗口与显示器的重叠面积选择当前显示器。
///
/// 使用完整视频模式边界计算每个显示器与窗口矩形的交集面积，选择最大者。窗口位于
///
/// 所有显示器之外或枚举失败时回退主显示器，保证后续居中仍有候选。
///
/// @param window GLFW 窗口句柄。
/// @return 最匹配的显示器；无法解析时返回主显示器。
/// @warning 低频窗口放置路径：会枚举全部显示器和查询视频模式，不用于每帧循环。
GLFWmonitor* findBestMonitorForWindow(GLFWwindow* window)
{
    // 空窗口没有位置矩形，主显示器是唯一稳定后备。
    if ( !window ) {
        return glfwGetPrimaryMonitor();
    }

    // GLFW 位置与显示器位置都位于同一虚拟桌面坐标系。
    int windowX      = 0;
    int windowY      = 0;
    int windowWidth  = 0;
    int windowHeight = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    glfwGetWindowSize(window, &windowWidth, &windowHeight);

    // monitors 数组由 GLFW 拥有，仅在本次事件处理期间借用。
    int           monitorCount = 0;
    GLFWmonitor** monitors     = glfwGetMonitors(&monitorCount);
    GLFWmonitor*  bestMonitor  = nullptr;
    // -1 让第一个有效监视器即使零重叠也成为确定候选。
    int bestArea = -1;

    for ( int i = 0; monitors && i < monitorCount; ++i ) {
        MonitorPlacementBounds monitorBounds;
        if ( !queryMonitorVideoBounds(monitors[i], monitorBounds) ) {
            continue;
        }

        // X/Y 重叠独立计算，任一为零则面积为零。
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
        // 相同面积保留枚举顺序中的第一个显示器，结果保持稳定。
        if ( overlapArea > bestArea ) {
            bestArea    = overlapArea;
            bestMonitor = monitors[i];
        }
    }

    // 热插拔或无有效模式时再次查询主显示器作为最终后备。
    return bestMonitor ? bestMonitor : glfwGetPrimaryMonitor();
}

/// @brief 广播窗口最大化状态变化给自绘标题栏。
///
/// hasStateChange 标记该事件为通知而非命令，NativeWindow
/// 自身的命令订阅会忽略，
/// 避免发布状态后再次触发 toggle。
///
/// @param isMaximized 当前窗口是否最大化。
void publishWindowMaximizedState(bool isMaximized)
{
    // 事件只携带最终状态，UI 不需要再次查询平台窗口属性。
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
///
/// showCmd 在最小化过渡期间仍可保留恢复目标，与 IsZoomed 的即时可见状态互补。
///
/// @param placement Win32 窗口布局信息。
/// @return 当前 show command 是最大化时返回 true。
bool win32PlacementShowsMaximized(const WINDOWPLACEMENT& placement)
{
    return placement.showCmd == SW_SHOWMAXIMIZED;
}

/// @brief 清除 Win32 最小化后恢复最大化的系统 hint。
///
/// 仅当 WINDOWPLACEMENT 查询成功且目标 flag 存在时回写，保留其他系统
/// flags。
/// 用于用户明确还原普通窗口后消除过期恢复意图。
///
/// @param hwnd Win32 窗口句柄。
/// @warning 低频窗口状态路径：同步调用 Win32 placement API。
void clearWin32RestoreToMaximizedFlag(HWND hwnd)
{
    // HWND 可能在关闭流程已失效，空值保持无操作。
    if ( !hwnd ) {
        return;
    }

    // Win32 要求 length 字段在 GetWindowPlacement 前初始化。
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(WINDOWPLACEMENT);
    if ( !GetWindowPlacement(hwnd, &placement) ||
         (placement.flags & RESTORE_TO_MAXIMIZED_FLAG) == 0 ) {
        return;
    }

    // 只清目标 bit，不能覆盖系统保存的其他 placement 信息。
    placement.flags &= ~RESTORE_TO_MAXIMIZED_FLAG;
    SetWindowPlacement(hwnd, &placement);
}

/// @brief 判断 Win32 窗口当前是否明确处于最大化状态。
///
/// 优先使用 IsZoomed；最小化或恢复过渡中再读取
/// WINDOWPLACEMENT.showCmd，容忍消息
/// 顺序暂时让即时状态与恢复目标不一致。
///
/// @param hwnd Win32 窗口句柄。
/// @return 当前窗口为最大化时返回 true。
bool win32PlacementWantsMaximized(HWND hwnd)
{
    // 无句柄时不能保留最大化意图。
    if ( !hwnd ) {
        return false;
    }

    // 常见已最大化路径无需读取完整 placement。
    if ( IsZoomed(hwnd) ) {
        return true;
    }

    // 即时状态未最大化时检查系统保存的展示命令。
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(WINDOWPLACEMENT);
    if ( !GetWindowPlacement(hwnd, &placement) ) {
        return false;
    }

    return win32PlacementShowsMaximized(placement);
}

/// @brief 写入 Win32 最小化后恢复最大化的跨模块窗口属性。
///
/// NativeWindow 与 Win32WindowAdapter 通过同名 HWND property
/// 共享恢复意图；属性
/// 值只作存在性标志，不转移任何 HANDLE 所有权。
///
/// @param window GLFW 窗口句柄。
/// @param restoreMaximized 任务栏恢复时是否应最大化。
void setWin32RestoreMaximizedProperty(GLFWwindow* window, bool restoreMaximized)
{
    // GLFW 窗口或其原生映射不存在时无法持久化 property。
    if ( !window ) {
        return;
    }

    HWND hwnd = glfwGetWin32Window(window);
    if ( !hwnd ) {
        return;
    }

    if ( restoreMaximized ) {
        // 常量句柄 1 表示 true，读取方只检查属性是否存在。
        SetPropW(hwnd, RESTORE_MAXIMIZED_PROP, reinterpret_cast<HANDLE>(1));
    } else {
        RemovePropW(hwnd, RESTORE_MAXIMIZED_PROP);
    }
}

/// @brief 判断 GLFW 窗口是否带有恢复最大化的 Win32 属性。
///
/// 属性跨越 NativeWindow 与 subclass 消息回调，补充普通 C++
/// 成员在嵌套消息中的
/// 可见状态。
///
/// @param window GLFW 窗口句柄。
/// @return 存在恢复最大化属性时返回 true。
bool hasWin32RestoreMaximizedProperty(GLFWwindow* window)
{
    // 空 GLFWwindow 不能解析 HWND。
    if ( !window ) {
        return false;
    }

    HWND hwnd = glfwGetWin32Window(window);
    return hwnd && GetPropW(hwnd, RESTORE_MAXIMIZED_PROP) != nullptr;
}

/// @brief 判断 Win32 最小化/恢复流程是否应保留之前的最大化状态。
///
/// 同时合并缓存状态、应用最近请求、HWND property 和系统
/// placement，任一来源表明
/// 最大化都采用保守保留，避免任务栏/Alt+Tab
/// 恢复为普通窗口。
///
/// @param window GLFW 窗口句柄。
/// @param lastRequestedMaximized 应用最近一次请求的最大化状态。
/// @param cachedRestoreMaximized 之前缓存的最小化恢复状态。
/// @return 任务栏或 Alt+Tab 恢复时应回到最大化则返回 true。
bool shouldPreserveWin32MaximizedRestore(GLFWwindow* window,
                                         bool        lastRequestedMaximized,
                                         bool        cachedRestoreMaximized)
{
    // 只有有效原生 HWND 才能可靠判断 Windows 恢复语义。
    if ( !window ) {
        return false;
    }

    HWND hwnd = glfwGetWin32Window(window);
    if ( !hwnd ) {
        return false;
    }

    // 四个来源取或以容忍 WM_SYSCOMMAND、WM_SIZE 和 GLFW callback 的顺序差异。
    return cachedRestoreMaximized || lastRequestedMaximized ||
           hasWin32RestoreMaximizedProperty(window) ||
           win32PlacementWantsMaximized(hwnd);
}

/// @brief 最小化 Win32 窗口，同时保留系统恢复目标。
///
/// 需要恢复最大化时通过 WINDOWPLACEMENT 同时设置 restore flag 和
/// minimized
/// showCmd；查询失败或普通窗口则清 flag 并使用 ShowWindow
/// 最小化。
///
/// @param hwnd Win32 窗口句柄。
/// @param restoreMaximized 下一次任务栏恢复时是否应最大化。
void minimizeWin32Window(HWND hwnd, bool restoreMaximized)
{
    // 空句柄允许上层销毁路径安全调用。
    if ( !hwnd ) {
        return;
    }

    if ( restoreMaximized ) {
        // 使用系统 placement
        // 能让任务栏恢复沿用最大化目标，不只依赖应用回调补偿。
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(WINDOWPLACEMENT);
        if ( GetWindowPlacement(hwnd, &placement) ) {
            // 保留其他 flags，并显式切换当前展示状态为最小化。
            placement.flags |= RESTORE_TO_MAXIMIZED_FLAG;
            placement.showCmd = SW_SHOWMINIMIZED;
            SetWindowPlacement(hwnd, &placement);
            return;
        }
    }

    // 普通窗口必须移除历史 flag，否则系统可能错误最大化恢复。
    clearWin32RestoreToMaximizedFlag(hwnd);
    ShowWindow(hwnd, SW_MINIMIZE);
}

/// @brief 在 Win32 上最小化窗口，并在最小化前持久化最大化还原意图。
///
/// 先解析 HWND；无法取得时仍回退 GLFW 标准最小化。有效 HWND
/// 路径把综合判定写入
/// 共享 property，再选择带或不带 restore flag 的 Win32
/// 最小化。
///
/// @param window GLFW 窗口句柄。
/// @param lastRequestedMaximized 最近一次请求的最大化状态。
void iconifyWin32WindowPreservingMaximize(GLFWwindow* window,
                                          bool        lastRequestedMaximized)
{
    // 事件可能在窗口关闭附近到达，空句柄直接返回。
    if ( !window ) {
        return;
    }

    HWND hwnd = glfwGetWin32Window(window);
    if ( !hwnd ) {
        // 原生映射失败时保留基本最小化能力，但无法保证 Windows 专属恢复语义。
        glfwIconifyWindow(window);
        return;
    }

    // 最小化前冻结本轮恢复意图，并同时提供给 adapter subclass。
    const bool restoreMaximized = shouldPreserveWin32MaximizedRestore(
        window, lastRequestedMaximized, false);
    setWin32RestoreMaximizedProperty(window, restoreMaximized);
    minimizeWin32Window(hwnd, restoreMaximized);
}
#endif
}  // namespace

/// @brief 创建无系统装饰 GLFW 窗口并安装 DPI、frame 与输入事件桥接。
///
/// 构造按窗口角色配置 hint、创建窗口、校正平台缩放与初始居中、记录普通
/// placement，
/// 再为正式应用窗口安装平台 frame adapter 和 EventBus
/// 原生命令订阅。所有 GLFW
/// callback 通过 window user pointer
/// 回到当前对象，并把输入转换为项目事件。
///
/// @param w 期望的初始逻辑宽度。
/// @param h 期望的初始逻辑高度。
/// @param wtitle 传给 GLFW 的 UTF-8 窗口标题。
/// @param windowMode Application 启用自绘 frame/软件光标，Startup
/// 保持资源准备行为。
/// @warning
/// 应用启动低频路径：创建窗口、读取图标、查询显示器并注册回调，必须在
///
/// GLFW 初始化后的窗口线程执行。
NativeWindow::NativeWindow(int w, int h, const char* wtitle,
                           NativeWindowMode windowMode)
{
    // 记录能力错误但继续创建窗口，让上层初始化流程统一处理缺少 Vulkan 的结果。
    if ( !glfwVulkanSupported() ) {
        XERROR("GLFW: Vulkan Not Supported");
    }

    // 启动日志保存实际 GLFW 版本和运行 backend，便于区分 X11/Wayland 行为。
    XINFO("GLFW Version: {}", glfwGetVersionString());
    XINFO("GLFW Platform code: {}", glfwGetPlatform());

    // 所有模式使用无装饰透明窗口；仅正式应用窗口允许用户改变尺寸。
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(
        GLFW_RESIZABLE,
        windowMode == NativeWindowMode::Application ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
#if defined(_WIN32) || defined(__linux__)
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif

#ifdef __linux__
    // Wayland app-id 与 X11 class name 对齐 desktop
    // 文件，供桌面环境分组和图标匹配。
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "MusicMapMaker-Next");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "MusicMapMaker-Next");
#endif
    // 不共享 OpenGL context；Vulkan surface 由后续图形上下文创建。
    m_windowHandle = glfwCreateWindow(w, h, wtitle, nullptr, nullptr);

    if ( m_windowHandle ) {
#if defined(__APPLE__)
        // macOS 首击桥接只在 Cocoa window 已创建后安装。
        enableMacOSFirstMouse(m_windowHandle);
#endif
        // 图标加载依赖实际窗口句柄，Wayland 路径会按平台能力跳过。
        reloadWindowIcon();
    }

    // 窗口创建成功后检测 GLFW 已自动应用的 framebuffer scale，避免重复 DPI
    // 放大。
    if ( m_windowHandle ) {
        // content scale 描述平台原生 DPI，window/framebuffer 比例描述 backend
        // 自动缩放。
        float xscale, yscale;
        glfwGetWindowContentScale(m_windowHandle, &xscale, &yscale);

        int actualW, actualH;
        glfwGetWindowSize(m_windowHandle, &actualW, &actualH);

#if defined(_WIN32) || defined(__linux__)
        // framebuffer/window 比值可识别 Wayland 等 backend
        // 已经完成的逻辑像素缩放。
        int fbW, fbH;
        glfwGetFramebufferSize(m_windowHandle, &fbW, &fbH);
        float fbScaleX =
            (actualW > 0) ? static_cast<float>(fbW) / actualW : 1.0f;

        // 当 content scale 大于 backend 已应用比例时，仅补足缺失部分，避免高
        // DPI 环境创建出视觉上过小的启动窗口。
        if ( xscale > fbScaleX ) {
            // 使用 X/Y 原生比例分别计算目标尺寸，支持非统一平台缩放报告。
            int scaledW = static_cast<int>(w * xscale);
            int scaledH = static_cast<int>(h * yscale);
            // 只放大尚未达到目标的窗口，不覆盖 backend 已选择的更大合法尺寸。
            if ( actualW < scaledW ) {
                glfwSetWindowSize(m_windowHandle, scaledW, scaledH);
                glfwGetWindowSize(m_windowHandle, &actualW, &actualH);
                glfwGetFramebufferSize(m_windowHandle, &fbW, &fbH);
                // 尺寸调整后重新读取 framebuffer
                // 比例，配置必须使用最终窗口状态。
                fbScaleX =
                    (actualW > 0) ? static_cast<float>(fbW) / actualW : 1.0f;
            }
        }
#else
        // 其他平台仍查询 framebuffer ratio，用于分离 native 与手动 UI scale。
        int fbW, fbH;
        glfwGetFramebufferSize(m_windowHandle, &fbW, &fbH);
        float fbScaleX =
            (actualW > 0) ? static_cast<float>(fbW) / actualW : 1.0f;
#endif

        // nativeContentScale 用于字体资源选择；uiScale 只表示 backend
        // 尚未自动应用的 补偿部分，两者职责不能混为一个倍率。
        Config::AppConfig::instance().setNativeContentScale(xscale);
        Config::AppConfig::instance().setUIScale(xscale / fbScaleX);

        XINFO("Detected scales: native={}, fb_auto={}, ui_manual={}",
              xscale,
              fbScaleX,
              xscale / fbScaleX);

        // 初始窗口优先在主显示器可见区域居中，失败时不阻止后续启动。
        GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
#if defined(__APPLE__)
        // Cocoa helper 使用 visibleFrame 扣除 Dock/menu bar；失败才回退通用
        // GLFW 路径。
        if ( centerMacOSWindowInVisibleFrame(m_windowHandle, w, h) ) {
            glfwGetWindowSize(m_windowHandle, &actualW, &actualH);
        } else if ( supportsProgrammaticWindowPosition() && monitor && mode ) {
#else
        // Wayland 禁止程序设置顶层坐标，因此仅 X11/Win32 等 backend 调用
        // setWindowPos。
        if ( supportsProgrammaticWindowPosition() && monitor && mode ) {
#endif
            // 通用回退使用完整视频模式居中，坐标相对主显示器原点。
            int xPos = (mode->width - actualW) / 2;
            int yPos = (mode->height - actualH) / 2;
            glfwSetWindowPos(m_windowHandle, xPos, yPos);
        }
        if ( monitor && mode ) {
            // 主显示器刷新率提供给逻辑/渲染帧率上限，不依赖窗口当前所在屏幕。
            Config::AppConfig::instance().setDeviceRefreshRate(
                mode->refreshRate);
            XINFO("Detected primary monitor refresh rate: {} Hz",
                  mode->refreshRate);
        }
    }

    // 在任何最大化操作前建立普通窗口恢复矩形；不支持位置的平台会安全跳过。
    rememberCurrentWindowPlacement();
    if ( m_windowHandle ) {
#if defined(__APPLE__)
        // macOS 使用应用层模拟最大化，初始状态明确为普通窗口并主动通知标题栏。
        m_lastRequestedMaximized = false;
        publishWindowMaximizedState(false);
#else
        // 其他平台从 GLFW 实际属性初始化状态，避免覆盖窗口管理器启动策略。
        m_lastRequestedMaximized =
            glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED) == GLFW_TRUE;
#endif
    }

    // Startup 资源窗口不安装主 frame adapter，避免 Win32 style 修改在首帧前短暂
    // 显示系统标题栏，也不订阅应用级窗口命令。
    if ( windowMode == NativeWindowMode::Application ) {
        // adapter 需要已经创建的 GLFW/native
        // handle，各平台分支至多选择一个实现。
#if defined(_WIN32)
        m_windowFrameAdapter =
            std::make_unique<Win32WindowAdapter>(m_windowHandle);
#endif
#if defined(__APPLE__)
        m_windowFrameAdapter = std::make_unique<MacOSWindowAdapter>(*this);
#endif
#if defined(MMM_ENABLE_X11_FRAME_INTERACTION)
        // Linux 构建可能同时支持 Wayland 与 X11，运行时只在实际 X11 backend
        // 安装。
        if ( glfwGetPlatform() == GLFW_PLATFORM_X11 ) {
            m_windowFrameAdapter = std::make_unique<X11WindowAdapter>(*this);
        }
#endif
    }

    // 正式应用由自绘软件光标接管，启动窗口仍显示系统光标以保持基本反馈。
    glfwSetInputMode(m_windowHandle,
                     GLFW_CURSOR,
                     windowMode == NativeWindowMode::Application
                         ? GLFW_CURSOR_HIDDEN
                         : GLFW_CURSOR_NORMAL);
#ifdef __linux__
    // 保持对现代 GLFW raw mouse 能力的兼容探测；当前窗口不启用 passthrough。
    if ( glfwRawMouseMotionSupported() ) {
        // 探测本身不改变 input mode。
    }
// Wayland 下显式允许 compositor 处理系统组合键。
#    if defined(GLFW_KEYBOARD_SHORTCUTS_INHIBIT)
    glfwSetInputMode(
        m_windowHandle, GLFW_KEYBOARD_SHORTCUTS_INHIBIT, GLFW_FALSE);
#    endif
#endif


    // 所有静态 GLFW callback 通过 user pointer
    // 借用当前对象；析构销毁窗口后不再回调。
    glfwSetWindowUserPointer(m_windowHandle, this);
    glfwSetFramebufferSizeCallback(m_windowHandle,
                                   &NativeWindow::framebufferResizeCallback);
    glfwSetWindowPosCallback(m_windowHandle, &NativeWindow::windowPosCallback);
    glfwSetWindowSizeCallback(m_windowHandle,
                              &NativeWindow::windowSizeCallback);
    glfwSetWindowIconifyCallback(m_windowHandle,
                                 &NativeWindow::GLFW_IconifyCallback);
    // Focus 变化先通知 frame adapter 清理平台拖拽候选，再发布只读状态事件给
    // UI。
    glfwSetWindowFocusCallback(m_windowHandle, [](GLFWwindow* w, int focused) {
        // user pointer 在 callback 注册前已设置，关闭附近仍防御空对象。
        auto app = reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(w));
        if ( app && app->m_windowFrameAdapter ) {
            app->m_windowFrameAdapter->handleClientFocusChange(focused ==
                                                               GLFW_TRUE);
        }
        // hasStateChange 防止 NativeWindow 的命令订阅把通知反向执行为窗口操作。
        Event::EventBus::instance().publish(Event::GLFWNativeEvent{
            .type           = Event::NativeEventType::GLFW_WINDOW_FOCUS_CHANGED,
            .hasStateChange = true,
            .isFocused      = focused == GLFW_TRUE,
        });
    });
    // 键盘与文件拖放使用独立静态翻译入口。
    glfwSetKeyCallback(m_windowHandle, GLFW_KeyCallback);
    glfwSetDropCallback(m_windowHandle, GLFW_DropCallback);

    // 跨显示器移动可能改变 content scale；回调只更新配置、frame
    // 外观和资源重载脏 通知，不在 GLFW 回调中直接重建字体或交换链。
    glfwSetWindowContentScaleCallback(
        m_windowHandle, [](GLFWwindow* w, float xscale, float yscale) {
            // 当前 UI 缩放只使用横向 framebuffer ratio，保持与启动检测一致。
            int winW, winH, fbW, fbH;
            glfwGetWindowSize(w, &winW, &winH);
            glfwGetFramebufferSize(w, &fbW, &fbH);
            float fbScaleX = (winW > 0) ? static_cast<float>(fbW) / winW : 1.0f;

            // 将平台原生 DPI 与 backend 自动 framebuffer 缩放拆分保存。
            Config::AppConfig::instance().setNativeContentScale(xscale);
            Config::AppConfig::instance().setUIScale(xscale / fbScaleX);

            XINFO("Content scale changed: native={}, ui={}",
                  xscale,
                  xscale / fbScaleX);

            // 圆角/阴影可能依赖内容缩放，adapter 存在时低频刷新。
            auto app =
                reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(w));
            if ( app ) {
                app->refreshWindowFrameShape();
            }

            // 复用 resize 状态事件通知 UI
            // 资源失效，真正重载由外层安全阶段完成。
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type           = Event::NativeEventType::GLFW_WINDOW_RESIZED,
                .hasStateChange = true });
        });

    // 系统级最大化/还原回调同步应用状态、普通 placement 和自绘标题栏通知。
    glfwSetWindowMaximizeCallback(
        m_windowHandle, [](GLFWwindow* w, int maximized) {
            // 所有成员访问均需有效 user pointer；事件通知仍可在下方按 GLFW
            // 状态发布。
            auto app =
                reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(w));
            if ( app ) {
#if defined(__APPLE__)
                // Cocoa 无边框窗口由 m_emulatedMaximized 作为权威状态，不采用
                // GLFW callback 参数覆盖应用层模拟结果。
                (void)maximized;
                app->m_lastRequestedMaximized = app->m_emulatedMaximized;
                app->refreshWindowFrameShape();
                publishWindowMaximizedState(app->m_emulatedMaximized);
                return;
#else
                // 原生平台 callback 参数代表窗口管理器确认的最终最大化状态。
                const bool isMaximized   = maximized == GLFW_TRUE;
                // 收到原生状态后退出任何旧的应用层模拟标记。
                app->m_emulatedMaximized = false;
                if ( isMaximized ) {
                    // 最大化确认后记录用户意图，并消费 Win32 最小化恢复 property。
                    app->m_lastRequestedMaximized = true;
#    ifdef _WIN32
                    setWin32RestoreMaximizedProperty(w, false);
#    endif
                } else {
#    ifdef _WIN32
                    // Windows 从任务栏恢复时可能短暂报告 restored；综合属性决定是否
                    // 仍需把逻辑状态视为最大化。
                    const bool keepMaximizedRestore =
                        shouldPreserveWin32MaximizedRestore(
                            w, false, app->m_restoreMaximizedAfterIconify);
#    else
                    // 非 Windows 原生还原没有额外恢复 property。
                    constexpr bool keepMaximizedRestore = false;
#    endif
                    if ( !keepMaximizedRestore ) {
                        // 真实还原才更新普通窗口 placement，过渡状态不能污染恢复矩形。
                        app->m_lastRequestedMaximized = false;
                        app->rememberCurrentWindowPlacement();
#    ifdef _WIN32
                        setWin32RestoreMaximizedProperty(w, false);
                        clearWin32RestoreToMaximizedFlag(glfwGetWin32Window(w));
#    endif
                    } else {
                        // 保留最大化意图，后续 iconify/subclass 流程会重新应用状态。
                        app->m_lastRequestedMaximized = true;
                    }
                }
                // 最大化改变平台圆角/阴影或 X11 shape，状态落定后统一刷新。
                app->refreshWindowFrameShape();
#endif
            }

            // 广播回调参数供 UI 更新按钮；macOS 分支已在上方提前发布并 return。
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE,
                .hasStateChange = true,
                .isMaximized    = (maximized == GLFW_TRUE) });
        });

    // 鼠标按键回调先转换统一事件，再让 frame adapter 抢占标题栏/边缘交互。
    glfwSetMouseButtonCallback(
        m_windowHandle, [](GLFWwindow* w, int button, int action, int mods) {
            // adapter 与应用事件总线共享同一 NativeWindow user pointer。
            auto app =
                reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(w));

            // DTO 在栈上构造，不跨 callback 保存 GLFW 原始指针。
            MMM::Event::GLFWMouseButtonEvent e;

            // Translator 把 GLFW 数值映射为项目跨平台枚举。
            e.button = MMM::Event::Translator::GLFW::GetMouseButton(button);
            e.action = MMM::Event::Translator::GLFW::GetAction(action);
            e.mods   = MMM::Event::Translator::GLFW::GetMods(mods);

            // GLFW 按键 callback 不携带位置，读取同一时刻客户区 cursor 坐标。
            double xpos, ypos;
            glfwGetCursorPos(w, &xpos, &ypos);
            e.pos = { static_cast<float>(xpos), static_cast<float>(ypos) };

            // 平台 frame 行为具有优先级，成功接管时不再把同一 press 发送给业务
            // UI。
            bool frameInteractionStarted = false;
            if ( app && app->m_windowFrameAdapter ) {
                frameInteractionStarted =
                    app->m_windowFrameAdapter->handleClientMouseButton(
                        button, action, xpos, ypos);
            }

            if ( !frameInteractionStarted ) {
                // 未命中标题栏/边缘的事件进入常规输入总线。
                MMM::Event::EventBus::instance().publish(e);
            }
        });

    // Cursor callback 维护跨事件 delta，并允许 adapter
    // 在窗口拖动/缩放时消费事件。
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

            // 首次事件以当前位置初始化历史，避免从静态零点产生巨大 delta。
            if ( s_firstMouse ) {
                s_lastMouseX = xpos;
                s_lastMouseY = ypos;
                s_firstMouse = false;
            }

            // 位置与 delta 统一窄化为事件系统使用的 float 坐标。
            MMM::Event::GLFWMouseMoveEvent e;
            e.pos = { static_cast<float>(xpos), static_cast<float>(ypos) };

            // Delta 相对上一个 cursor event，而非渲染帧；适用于连续交互。
            e.delta = { static_cast<float>(xpos - s_lastMouseX),
                        static_cast<float>(ypos - s_lastMouseY) };

            // 无论 adapter 是否消费，都更新历史位置，下一次 delta 保持连续。
            s_lastMouseX = xpos;
            s_lastMouseY = ypos;

            if ( !frameInteractionStarted ) {
                // frame 拖动/缩放期间不向业务 UI 泄漏同一 cursor motion。
                MMM::Event::EventBus::instance().publish(e);
            }
        });

    // Scroll 不参与 frame adapter，直接携带偏移和发生位置发布。
    glfwSetScrollCallback(
        m_windowHandle, [](GLFWwindow* w, double xoffset, double yoffset) {
            MMM::Event::GLFWMouseScrollEvent e;
            e.offset = { static_cast<float>(xoffset),
                         static_cast<float>(yoffset) };
            // GLFW scroll callback 不带 cursor 坐标，主动读取客户区当前位置。
            double xpos, ypos;
            glfwGetCursorPos(w, &xpos, &ypos);
            e.pos = { static_cast<float>(xpos), static_cast<float>(ypos) };

            MMM::Event::EventBus::instance().publish(e);
        });

    if ( windowMode == NativeWindowMode::Application ) {
        // 正式窗口订阅 UI 发出的命令事件；Startup 窗口不响应主应用控制按钮。
        m_nativeEventSubscription =
            Event::EventBus::instance().subscribe<Event::GLFWNativeEvent>(
                [this](const Event::GLFWNativeEvent& e) {
                    // 状态通知由窗口回调发布，不能再次解释成命令形成反馈循环。
                    if ( e.hasStateChange ) return;

                    // 每种命令只调用低频
                    // GLFW/平台窗口操作，不在事件回调中等待。
                    switch ( e.type ) {
                    case Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE: {
                        // 统一入口处理 macOS 模拟最大化与其他平台原生最大化。
                        toggleMaximized();
                        break;
                    }
                    case Event::NativeEventType::GLFW_ICONFY_WINDOW: {
#ifdef _WIN32
                        // Windows 额外保留任务栏恢复后的最大化意图。
                        iconifyWin32WindowPreservingMaximize(
                            m_windowHandle, m_lastRequestedMaximized);
#elif defined(__APPLE__)
                // 优先使用 Cocoa miniaturize 维持无边框窗口平台行为。
                if ( !miniaturizeMacOSWindow(m_windowHandle) ) {
                    glfwIconifyWindow(m_windowHandle);
                }
#else
        // X11/Wayland 走 GLFW 标准最小化请求。
        glfwIconifyWindow(m_windowHandle);
#endif
                        XINFO("Window iconified.");
                        break;
                    }
                    case Event::NativeEventType::GLFW_CLOSE_WINDOW: {
                        // 只设置 should-close
                        // 标志，主循环负责有序退出和资源释放。
                        glfwSetWindowShouldClose(m_windowHandle, GLFW_TRUE);
                        break;
                    }
                    case Event::NativeEventType::GLFW_WINDOW_RESIZED:
                    case Event::NativeEventType::
                        GLFW_WINDOW_CONTENT_SCALE_CHANGED:
                    case Event::NativeEventType::GLFW_WINDOW_FOCUS_CHANGED:
                        // 这些类型只作为状态通知存在，即使缺失标志也不执行窗口命令。
                        break;
                    }
                });
    }

    // adapter 与回调全部建立后应用一次当前普通/最大化状态对应的外形。
    refreshWindowFrameShape();
}

/// @brief 从当前资源目录重新加载主窗口图标。
///
/// 图标路径由 AppPaths 解析，文件完整读入内存后由 stb_image 解码为
/// RGBA8，并只在
/// glfwSetWindowIcon 调用期间借用像素。Wayland 顶层图标交给
/// desktop 文件/compositor。
///
/// @warning 启动或资源重载低频路径：包含文件 I/O、vector
/// 分配与图片解码，禁止从
/// 每帧渲染/UI update 调用。
void NativeWindow::reloadWindowIcon()
{
    // 没有窗口句柄时无法设置平台图标。
    if ( !m_windowHandle ) return;
#if defined(__linux__)
    // Wayland 协议不允许客户端直接设置顶层图标，避免调用无效 GLFW 路径。
    if ( glfwGetPlatform() == GLFW_PLATFORM_WAYLAND ) return;
#endif

    // AppPaths 指向当前已安装/同步资源目录，不依赖进程工作目录。
    const std::filesystem::path iconPath =
        Config::AppPaths::windowIconFilePath();
    // 二进制读取保留 PNG 等压缩资源原始字节。
    std::ifstream iconFile(iconPath, std::ios::binary);
    if ( !iconFile ) {
        XDEBUG("Window icon is not available yet: {}",
               Config::pathToUtf8(iconPath));
        return;
    }

    // 文件在低频路径完整读入，stb_image 需要连续内存缓冲。
    std::vector<unsigned char> iconBytes{ std::istreambuf_iterator<char>(
                                              iconFile),
                                          std::istreambuf_iterator<char>() };
    int                        width    = 0;
    int                        height   = 0;
    int                        channels = 0;
    // 强制输出四通道，GLFWimage 要求每像素 RGBA 8-bit 顺序。
    unsigned char* pixels =
        stbi_load_from_memory(iconBytes.data(),
                              static_cast<int>(iconBytes.size()),
                              &width,
                              &height,
                              &channels,
                              4);
    if ( !pixels ) {
        XWARN("Failed to decode window icon: {}", Config::pathToUtf8(iconPath));
        return;
    }

    // GLFW 在调用期间复制图标数据，随后即可释放 stb_image 缓冲。
    GLFWimage image{ .width = width, .height = height, .pixels = pixels };
    glfwSetWindowIcon(m_windowHandle, 1, &image);
    stbi_image_free(pixels);
}

/// @brief 按逻辑尺寸调整窗口并在当前显示器工作区居中。
///
/// Wayland 通过 hide/resize/show 请求 compositor 重新放置；macOS 优先使用
/// Cocoa
/// visibleFrame；X11/Win32 等平台按 DPI
/// 自动缩放缺口修正尺寸，在当前最佳显示器
///
/// 工作区内钳制并居中。所有成功路径标记交换链需要重建。
///
/// @param width 目标逻辑宽度，必须大于零。
/// @param height 目标逻辑高度，必须大于零。
/// @warning
/// 启动/显式窗口调整低频路径：会重新映射或修改原生窗口矩形，不得在每帧
///
/// 调用。
void NativeWindow::resizeAndCenter(int width, int height)
{
    // 拒绝无效 extent，避免 GLFW 和交换链收到零尺寸请求。
    if ( !m_windowHandle || width <= 0 || height <= 0 ) return;

    if ( !supportsProgrammaticWindowPosition() ) {
        // Wayland 不报告/接受可靠位置，暂时屏蔽 placement callback
        // 防止保存伪坐标。
        m_ignoreWindowPlacementCallbacks = true;
        const bool wasVisible =
            glfwGetWindowAttrib(m_windowHandle, GLFW_VISIBLE) == GLFW_TRUE;
        // Wayland 不允许客户端设置顶层窗口坐标。重新映射窗口，让合成器按新
        // 尺寸重新执行窗口放置策略，避免从启动小窗左上角向右下方扩张。
        // 仅恢复原可见状态，隐藏的启动窗口保持隐藏。
        if ( wasVisible ) glfwHideWindow(m_windowHandle);
        glfwSetWindowSize(m_windowHandle, width, height);
        if ( wasVisible ) glfwShowWindow(m_windowHandle);
        m_ignoreWindowPlacementCallbacks = false;
        // 重新映射后的 surface extent 已立即变化，清零时间点让下一帧直接重建。
        m_lastResizeTime = std::chrono::steady_clock::time_point{};
        m_resizePending.store(true, std::memory_order_relaxed);
        refreshWindowFrameShape();
        return;
    }

#if defined(__APPLE__)
    // Cocoa visibleFrame 能正确排除 Dock/menu bar 并处理屏幕坐标方向。
    if ( centerMacOSWindowInVisibleFrame(m_windowHandle, width, height) ) {
        // 专用 helper 已设置最终 frame，随后缓存普通 placement
        // 并请求交换链更新。
        rememberCurrentWindowPlacement();
        m_lastResizeTime = std::chrono::steady_clock::now();
        m_resizePending.store(true, std::memory_order_relaxed);
        refreshWindowFrameShape();
        return;
    }
#endif

    // 其他平台从调用方逻辑尺寸开始，必要时补足 GLFW 未自动应用的 DPI 缩放。
    int targetWidth  = width;
    int targetHeight = height;
#if defined(_WIN32) || defined(__linux__)
    // content scale 与 framebuffer/window 比值共同决定是否需要手工放大。
    float xScale = 1.0f;
    float yScale = 1.0f;
    glfwGetWindowContentScale(m_windowHandle, &xScale, &yScale);

    int currentWidth      = 0;
    int currentHeight     = 0;
    int framebufferWidth  = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(m_windowHandle, &currentWidth, &currentHeight);
    glfwGetFramebufferSize(
        m_windowHandle, &framebufferWidth, &framebufferHeight);
    const float framebufferScaleX = currentWidth > 0
                                        ? static_cast<float>(framebufferWidth) /
                                              static_cast<float>(currentWidth)
                                        : 1.0f;
    // 仅在平台 DPI 大于 backend 自动 scale 时应用手动尺寸补偿。
    if ( xScale > framebufferScaleX ) {
        targetWidth  = static_cast<int>(static_cast<float>(width) * xScale);
        targetHeight = static_cast<int>(static_cast<float>(height) * yScale);
    }
#endif

    // 使用窗口当前重叠面积最大的显示器，而不是无条件跳回主显示器。
    MonitorPlacementBounds bounds;
    if ( queryMonitorWorkAreaBounds(findBestMonitorForWindow(m_windowHandle),
                                    bounds) ) {
        // 目标尺寸钳制到工作区，保证居中后仍完全可见。
        targetWidth  = std::min(targetWidth, bounds.m_width);
        targetHeight = std::min(targetHeight, bounds.m_height);
        const int targetX =
            bounds.m_x + std::max((bounds.m_width - targetWidth) / 2, 0);
        const int targetY =
            bounds.m_y + std::max((bounds.m_height - targetHeight) / 2, 0);
        // applyWindowPlacement 统一维护普通 placement 和 resize 脏位。
        applyWindowPlacement(
            targetX, targetY, targetWidth, targetHeight, false);
        return;
    }

    // 显示器查询失败时以当前窗口中心为锚，不凭空选择绝对坐标。
    int currentX = 0;
    int currentY = 0;
    glfwGetWindowPos(m_windowHandle, &currentX, &currentY);
    glfwGetWindowSize(m_windowHandle, &width, &height);
    applyWindowPlacement(currentX + (width - targetWidth) / 2,
                         currentY + (height - targetHeight) / 2,
                         targetWidth,
                         targetHeight,
                         false);
}

/// @brief 注销原生命令订阅、销毁 frame adapter，再释放 GLFW 窗口。
///
/// adapter 可能持有原生窗口句柄和 EventBus 回调，必须早于 glfwDestroyWindow
/// 析构。 GLFW 全局终止由更高层生命周期负责。
NativeWindow::~NativeWindow()
{
    // Startup 模式没有订阅，零 ID 路径保持幂等。
    if ( m_nativeEventSubscription != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::GLFWNativeEvent>(
            m_nativeEventSubscription);
        m_nativeEventSubscription = 0;
    }
    // 平台 adapter 析构会撤销 subclass、关联事件或 drag-area 订阅。
    m_windowFrameAdapter.reset();
    if ( m_windowHandle ) {
        // 销毁窗口同时取消其全部 GLFW callback，user pointer 不再被访问。
        glfwDestroyWindow(m_windowHandle);
    }
}

/// @brief 导出可持久化的普通窗口矩形和当前最大化状态。
///
/// 默认值保证窗口句柄缺失时仍返回合法配置。当前处于最大化时，位置/尺寸改用缓存
///
/// 的 normal placement，避免把工作区大小写回下次启动配置。
///
/// @param x 输出普通窗口左上角 X。
/// @param y 输出普通窗口左上角 Y。
/// @param width 输出普通窗口宽度。
/// @param height 输出普通窗口高度。
/// @param maximized 输出当前逻辑最大化状态。
void NativeWindow::getWindowPlacement(int& x, int& y, int& width, int& height,
                                      bool& maximized) const
{
    // 先写安全默认值，所有早退路径都满足输出参数后置条件。
    x         = 100;
    y         = 100;
    width     = 1280;
    height    = 720;
    maximized = false;

    if ( !m_windowHandle ) {
        return;
    }

    // 普通状态优先读取实际 GLFW 矩形，随后按最大化语义决定是否替换。
    glfwGetWindowPos(m_windowHandle, &x, &y);
    glfwGetWindowSize(m_windowHandle, &width, &height);
    maximized = isWindowMaximized();

    if ( maximized ) {
        // 持久化最大化 flag 与普通恢复矩形，而不是当前铺满工作区的 frame。
        x      = m_normalWindowPos[0];
        y      = m_normalWindowPos[1];
        width  = m_normalWindowSize[0];
        height = m_normalWindowSize[1];
    }
}

/// @brief 应用持久化 placement，并按平台恢复普通或最大化状态。
///
/// 对疑似误保存为工作区大小的最大化尺寸，改用当前缓存 normal
/// placement。先恢复
/// 原生最大化、设置普通矩形，再按请求进入 macOS
/// 模拟最大化或平台原生最大化，
/// 从而保持可靠的还原目标。
///
/// @param x 持久化普通窗口 X。
/// @param y 持久化普通窗口 Y。
/// @param width 持久化普通窗口宽度。
/// @param height 持久化普通窗口高度。
/// @param maximized 是否在普通矩形建立后进入最大化。
/// @warning 启动/设置应用低频路径：连续修改窗口状态和矩形，并请求交换链重建。
void NativeWindow::applyWindowPlacement(int x, int y, int width, int height,
                                        bool maximized)
{
    // 无窗口或非法尺寸不改变现有 placement 状态。
    if ( !m_windowHandle || width <= 0 || height <= 0 ) {
        return;
    }

    // restore* 始终表示将来退出最大化时使用的普通窗口矩形。
    int restoreX      = x;
    int restoreY      = y;
    int restoreWidth  = width;
    int restoreHeight = height;
    // 旧配置可能误把最大化工作区矩形当普通矩形，检测后保留当前安全缓存。
    if ( maximized && isLikelyMaximizedPlacement(width, height) ) {
        restoreX      = m_normalWindowPos[0];
        restoreY      = m_normalWindowPos[1];
        restoreWidth  = m_normalWindowSize[0];
        restoreHeight = m_normalWindowSize[1];
    }

    // 在任何系统状态切换前先建立确定的 normal placement。
    rememberWindowPlacement(restoreX, restoreY, restoreWidth, restoreHeight);
    m_emulatedMaximized = false;

    // 最近请求用于 Win32 iconify 回调跨过中间 restored 状态。
    m_lastRequestedMaximized = maximized;
#ifdef _WIN32
    if ( !maximized ) {
        m_restoreMaximizedAfterIconify = false;
        setWin32RestoreMaximizedProperty(m_windowHandle, false);
        clearWin32RestoreToMaximizedFlag(glfwGetWin32Window(m_windowHandle));
    }
#endif

    // 必须先退出旧原生最大化，后续 setWindowPos/Size 才真正落到普通 frame。
    if ( glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED) == GLFW_TRUE ) {
        glfwRestoreWindow(m_windowHandle);
    }

    // callback 屏蔽防止程序化中间矩形覆盖刚缓存的 normal placement。
    setWindowPlacementWithoutRemembering(
        restoreX, restoreY, restoreWidth, restoreHeight);

    if ( maximized ) {
#if defined(__APPLE__)
        // macOS 无边框最大化优先使用 visibleFrame 模拟，失败才尝试 GLFW
        // 原生路径。
        if ( applyEmulatedMaximizedPlacement() ) {
            publishWindowMaximizedState(true);
        } else {
            glfwMaximizeWindow(m_windowHandle);
        }
#else
        // X11/Win32 使用窗口管理器原生最大化以保留 Snap/任务栏语义。
        glfwMaximizeWindow(m_windowHandle);
#endif
        // 最大化 callback 不应改变 normal frame，结束时再次固定原恢复矩形。
        rememberWindowPlacement(
            restoreX, restoreY, restoreWidth, restoreHeight);
    }
    // 最终状态决定圆角、阴影或 X11 shape。
    refreshWindowFrameShape();
}

/// @brief 在当前普通与最大化状态之间切换，并维护跨平台还原矩形。
///
/// 还原路径清理最近请求、iconify 恢复和 Win32
/// property；最大化路径先保存当前普通
/// placement，再进入 macOS
/// 模拟或平台原生最大化。两条路径都刷新自绘 frame 外观。
///
/// @warning
/// 用户窗口命令低频路径：会修改原生窗口状态/矩形并发布状态事件，不在
///
/// 每帧调用。
void NativeWindow::toggleMaximized()
{
    // 没有 GLFWwindow 时无法切换。
    if ( !m_windowHandle ) {
        return;
    }

    // 统一查询包含 macOS 模拟状态和其他平台 GLFW 属性。
    const bool maximized = isWindowMaximized();
    if ( maximized ) {
        // 用户明确还原后不应在随后 iconify/restore 中再次强制最大化。
        m_lastRequestedMaximized       = false;
        m_restoreMaximizedAfterIconify = false;
#ifdef _WIN32
        setWin32RestoreMaximizedProperty(m_windowHandle, false);
#endif
#if defined(__APPLE__)
        // 模拟最大化没有可调用的 GLFW restore 状态，直接恢复缓存普通矩形。
        m_emulatedMaximized = false;
        setWindowPlacementWithoutRemembering(m_normalWindowPos[0],
                                             m_normalWindowPos[1],
                                             m_normalWindowSize[0],
                                             m_normalWindowSize[1]);
        publishWindowMaximizedState(false);
#else
        // 原生平台交给窗口管理器恢复其 normal placement。
        glfwRestoreWindow(m_windowHandle);
#endif
#ifdef _WIN32
        clearWin32RestoreToMaximizedFlag(glfwGetWin32Window(m_windowHandle));
#endif
        XINFO("Window restored.");
        // 状态切换后移除最大化专用圆角/阴影配置。
        refreshWindowFrameShape();
        return;
    }

    // 进入最大化前捕获当前普通 frame，后续回调不允许覆盖。
    rememberCurrentWindowPlacement();
    const int restoreX      = m_normalWindowPos[0];
    const int restoreY      = m_normalWindowPos[1];
    const int restoreWidth  = m_normalWindowSize[0];
    const int restoreHeight = m_normalWindowSize[1];
    // 先记录意图，Win32 中间消息即使报告 restored 也能保留恢复目标。
    m_lastRequestedMaximized = true;
#if defined(__APPLE__)
    // Cocoa visibleFrame 模拟成功时主动发布状态，失败交给 GLFW callback 通知。
    if ( applyEmulatedMaximizedPlacement() ) {
        publishWindowMaximizedState(true);
    } else {
        glfwMaximizeWindow(m_windowHandle);
    }
#else
    glfwMaximizeWindow(m_windowHandle);
#endif
    // 再次写回快照，防御同步平台 callback 在 maximize 调用内触发位置/尺寸回调。
    rememberWindowPlacement(restoreX, restoreY, restoreWidth, restoreHeight);
    XINFO("Window maximized.");
    refreshWindowFrameShape();
}

/// @brief 获取当前平台 frame adapter 的非 owning 指针。
/// @return Application 模式且平台有实现时返回 adapter，否则返回 nullptr。
IWindowFrameAdapter* NativeWindow::getWindowFrameAdapter() const
{
    return m_windowFrameAdapter.get();
}

/// @brief 向 frame adapter 暴露宿主持有的 GLFW 主窗口。
/// @return 当前 GLFWwindow 非 owning 句柄。
GLFWwindow* NativeWindow::getFrameWindowHandle() const
{
    return m_windowHandle;
}

/// @brief 返回缓存的普通窗口还原矩形。
///
/// 输出不查询当前最大化/全屏 frame，专供平台 adapter 在标题栏拖动恢复时使用。
///
/// @param x 输出普通窗口 X。
/// @param y 输出普通窗口 Y。
/// @param width 输出普通窗口宽度。
/// @param height 输出普通窗口高度。
void NativeWindow::getNormalFramePlacement(int& x, int& y, int& width,
                                           int& height) const
{
    x      = m_normalWindowPos[0];
    y      = m_normalWindowPos[1];
    width  = m_normalWindowSize[0];
    height = m_normalWindowSize[1];
}

/// @brief 由平台 adapter 更新普通窗口还原矩形。
/// @param x 普通窗口 X。
/// @param y 普通窗口 Y。
/// @param width 正宽度。
/// @param height 正高度。
void NativeWindow::setNormalFramePlacement(int x, int y, int width, int height)
{
    rememberWindowPlacement(x, y, width, height);
}

/// @brief 向 frame adapter 返回统一的原生/模拟最大化状态。
/// @return 当前窗口在逻辑上最大化时返回 true。
bool NativeWindow::isFrameMaximized() const
{
    return isWindowMaximized();
}

/// @brief 为最大化标题栏拖动恢复普通窗口，并维持指针横向相对位置。
///
/// 将客户区 cursor 转换为根坐标，根据 normal width 的横向比例计算新
/// X，并保留
///
/// 纵向按下偏移。随后清理各平台最大化状态、设置普通矩形、发布还原通知并刷新
///
/// frame 外观。
///
/// @param cursorX 最大化客户区中的指针 X。
/// @param cursorY 最大化客户区中的指针 Y。
/// @return 窗口原先最大化且成功应用普通矩形时返回 true。
/// @warning 标题栏拖动低频分支：会同步修改窗口矩形；只在移动超过 adapter
/// 阈值后
/// 执行一次。
bool NativeWindow::restoreFrameForClientMove(double cursorX, double cursorY)
{
    // 仅最大化窗口具备“拖出还原”语义。
    if ( !m_windowHandle || !isWindowMaximized() ) {
        return false;
    }

    // GLFW cursor 为客户区坐标，与窗口虚拟桌面位置组合得到根坐标。
    int windowX      = 0;
    int windowY      = 0;
    int windowWidth  = 0;
    int windowHeight = 0;
    glfwGetWindowPos(m_windowHandle, &windowX, &windowY);
    glfwGetWindowSize(m_windowHandle, &windowWidth, &windowHeight);
    if ( windowWidth <= 0 || windowHeight <= 0 ) {
        return false;
    }

    // normal size 至少钳制为 1，避免异常缓存导致除零或非法 GLFW extent。
    const int rootCursorX   = windowX + static_cast<int>(cursorX);
    const int rootCursorY   = windowY + static_cast<int>(cursorY);
    const int restoreWidth  = std::max(1, m_normalWindowSize[0]);
    const int restoreHeight = std::max(1, m_normalWindowSize[1]);
    // 横向比例保持用户抓住的标题栏位置，恢复后窗口不会跳到指针单侧。
    const float cursorRatioX = std::clamp(
        static_cast<float>(cursorX) / static_cast<float>(windowWidth),
        0.0f,
        1.0f);
    // 纵向直接保留客户区偏移，并限制到恢复窗口高度以内。
    const int restoreX =
        rootCursorX -
        static_cast<int>(static_cast<float>(restoreWidth) * cursorRatioX);
    const int restoreY =
        rootCursorY -
        std::clamp(static_cast<int>(cursorY), 0, restoreHeight - 1);

    // 用户拖出表示明确取消最大化，所有跨最小化恢复标志都必须同步清除。
    m_lastRequestedMaximized       = false;
    m_restoreMaximizedAfterIconify = false;
    m_emulatedMaximized            = false;
#ifdef _WIN32
    setWin32RestoreMaximizedProperty(m_windowHandle, false);
    clearWin32RestoreToMaximizedFlag(glfwGetWin32Window(m_windowHandle));
#endif
#if !defined(__APPLE__)
    // 原生平台先解除 GLFW maximized 属性，再写入明确普通 frame。
    if ( glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED) == GLFW_TRUE ) {
        glfwRestoreWindow(m_windowHandle);
    }
#endif
    // 设置过程中屏蔽 callback，随后显式保存最终 normal placement。
    setWindowPlacementWithoutRemembering(
        restoreX, restoreY, restoreWidth, restoreHeight);
    rememberWindowPlacement(restoreX, restoreY, restoreWidth, restoreHeight);
    // macOS 模拟路径没有 GLFW maximize callback，统一主动通知标题栏。
    publishWindowMaximizedState(false);
    refreshWindowFrameShape();
    return true;
}

/// @brief 记录 framebuffer extent 变化并启动交换链重建消抖。
/// @param window 发生变化的 GLFW 窗口。
/// @param w 新 framebuffer 宽度；脏位记录不依赖具体数值。
/// @param h 新 framebuffer 高度；脏位记录不依赖具体数值。
/// @warning GLFW resize 回调热路径：只更新时间点和 relaxed
/// 原子脏位，禁止直接
/// waitIdle 或重建交换链。
void NativeWindow::framebufferResizeCallback(GLFWwindow* window, int w, int h)
{
    // 尺寸由真正重建阶段重新查询，回调参数仅用于 GLFW 签名。
    auto app =
        reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(window));
    // user pointer 由构造在 callback 注册前设置。
    app->m_lastResizeTime = std::chrono::steady_clock::now();
    app->m_resizePending.store(true, std::memory_order_relaxed);
}

/// @brief 在普通窗口移动后更新可还原 placement 位置。
/// @param window 发生移动的 GLFW 窗口。
/// @param x 新虚拟桌面 X。
/// @param y 新虚拟桌面 Y。
/// @warning GLFW 窗口事件路径：只在普通、非程序化状态下查询尺寸并写固定成员。
void NativeWindow::windowPosCallback(GLFWwindow* window, int x, int y)
{
    // 程序化 placement、最大化、最小化和 fullscreen 回调不能污染 normal rect。
    auto app =
        reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(window));
    if ( !app || !app->canRememberCurrentWindowPlacement() ) {
        return;
    }

    // 位置回调不携带尺寸，从同一窗口读取当前客户区大小组成完整矩形。
    int width  = 0;
    int height = 0;
    glfwGetWindowSize(window, &width, &height);
    app->rememberWindowPlacement(x, y, width, height);
}

/// @brief 在普通窗口缩放后更新可还原 placement 尺寸并刷新 frame 外观。
/// @param window 发生尺寸变化的 GLFW 窗口。
/// @param width 新窗口宽度。
/// @param height 新窗口高度。
/// @warning GLFW 窗口事件路径：只在可记忆状态下查询位置、写缓存并低频刷新外形。
void NativeWindow::windowSizeCallback(GLFWwindow* window, int width, int height)
{
    // 与 position callback 使用同一资格判断，保持 normal rect 两部分一致。
    auto app =
        reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(window));
    if ( !app || !app->canRememberCurrentWindowPlacement() ) {
        return;
    }

    // 尺寸回调不携带位置，从 GLFW 读取当前虚拟桌面坐标。
    int x = 0;
    int y = 0;
    glfwGetWindowPos(window, &x, &y);
    app->rememberWindowPlacement(x, y, width, height);
    app->refreshWindowFrameShape();
}

/// @brief 判断当前回调是否允许写入普通窗口还原矩形。
///
/// Wayland 不支持可靠位置；程序化设置、模拟最大化、全屏和最小化均跳过。非
/// macOS
/// 平台还额外排除 GLFW 原生最大化，macOS 则由 m_emulatedMaximized
/// 表达该状态。
///
/// @return 当前实际为用户操纵的普通可定位窗口时返回 true。
bool NativeWindow::canRememberCurrentWindowPlacement() const
{
    // 不可定位 backend 的坐标不是可持久化用户状态。
    if ( !supportsProgrammaticWindowPosition() ) return false;

#if defined(__APPLE__)
    // macOS 无边框最大化由应用模拟，不依赖 GLFW_MAXIMIZED 属性。
    return !m_ignoreWindowPlacementCallbacks && !m_emulatedMaximized &&
           m_windowHandle && glfwGetWindowMonitor(m_windowHandle) == nullptr &&
           glfwGetWindowAttrib(m_windowHandle, GLFW_ICONIFIED) != GLFW_TRUE;
#else
    // X11/Win32 同时排除应用模拟标志和窗口管理器原生最大化属性。
    return !m_ignoreWindowPlacementCallbacks && !m_emulatedMaximized &&
           m_windowHandle && glfwGetWindowMonitor(m_windowHandle) == nullptr &&
           glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED) != GLFW_TRUE &&
           glfwGetWindowAttrib(m_windowHandle, GLFW_ICONIFIED) != GLFW_TRUE;
#endif
}

/// @brief 合并平台原生属性与 macOS 应用层标志判断逻辑最大化状态。
/// @return 当前窗口应按最大化处理时返回 true。
/// @warning 轻量状态查询路径：只读成员并至多调用一次 GLFW attribute。
bool NativeWindow::isWindowMaximized() const
{
#if defined(__APPLE__)
    // Cocoa visibleFrame 模拟是本项目 macOS 最大化的权威状态。
    return m_emulatedMaximized;
#else
    // 保留 emulated 分支便于公共状态机，即使当前非 macOS 通常为 false。
    return m_emulatedMaximized ||
           (m_windowHandle &&
            glfwGetWindowAttrib(m_windowHandle, GLFW_MAXIMIZED) == GLFW_TRUE);
#endif
}

/// @brief 判断历史普通尺寸是否接近主显示器工作区，疑似保存了最大化矩形。
///
/// 宽或高任一接近工作区边界即判定，容忍窗口边框和平台 rounding 的固定像素误差。
///
/// @param width 历史配置宽度。
/// @param height 历史配置高度。
/// @return 尺寸有效且贴近工作区任一维度时返回 true。
/// @warning 启动 placement 路径：会查询主显示器边界，不用于窗口事件热路径。
bool NativeWindow::isLikelyMaximizedPlacement(int width, int height) const
{
    // 无窗口或非正尺寸不具备可信历史 placement。
    if ( !m_windowHandle || width <= 0 || height <= 0 ) {
        return false;
    }

    // 工作区查询失败时不猜测，直接接受调用方保存矩形。
    MonitorPlacementBounds bounds;
    if ( !queryPrimaryMonitorPlacementBounds(bounds) ) return false;

    // 单维达到边界也可能来自最大化或贴边保存，优先保护现有 normal rect。
    return width >= bounds.m_width - MAXIMIZED_PLACEMENT_TOLERANCE ||
           height >= bounds.m_height - MAXIMIZED_PLACEMENT_TOLERANCE;
}

/// @brief 从当前 GLFW 普通窗口状态抓取并缓存完整还原矩形。
/// @warning 低频窗口状态路径：只有 canRemember 为 true 时查询位置和尺寸。
void NativeWindow::rememberCurrentWindowPlacement()
{
    // 资格判断统一屏蔽最大化/全屏/最小化及程序化中间 callback。
    if ( !canRememberCurrentWindowPlacement() ) {
        return;
    }

    // 两次 GLFW 查询在同一事件线程连续执行，随后一次性写入缓存。
    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
    glfwGetWindowPos(m_windowHandle, &x, &y);
    glfwGetWindowSize(m_windowHandle, &width, &height);
    rememberWindowPlacement(x, y, width, height);
}

/// @brief 设置程序化窗口矩形，同时阻止同步 GLFW 回调覆盖 normal placement。
///
/// 设置完成后立即记录 resize 时间和 relaxed
/// 脏位，交换链在外层消抖阶段重建。
/// 调用方负责在需要时显式
/// rememberWindowPlacement。
///
/// @param x 目标虚拟桌面 X。
/// @param y 目标虚拟桌面 Y。
/// @param width 正目标宽度。
/// @param height 正目标高度。
/// @warning 低频窗口状态路径：同步调用 setWindowPos/Size，可派生平台消息。
void NativeWindow::setWindowPlacementWithoutRemembering(int x, int y, int width,
                                                        int height)
{
    // 非法 extent 不进入回调屏蔽状态。
    if ( !m_windowHandle || width <= 0 || height <= 0 ) {
        return;
    }

    // GLFW setter 可能同步触发 position/size callback，标志必须覆盖两次调用。
    m_ignoreWindowPlacementCallbacks = true;
    glfwSetWindowPos(m_windowHandle, x, y);
    glfwSetWindowSize(m_windowHandle, width, height);
    m_ignoreWindowPlacementCallbacks = false;
    // 完成后恢复用户回调记忆，并把新 extent 交给交换链消抖逻辑。
    m_lastResizeTime = std::chrono::steady_clock::now();
    m_resizePending.store(true, std::memory_order_relaxed);
}

/// @brief 在 macOS 为无边框窗口应用工作区大小的模拟最大化 frame。
///
/// 优先调用 Cocoa visibleFrame helper；失败时使用 GLFW
/// 当前最佳显示器工作区回退。
/// 只有成功设置矩形才保持
/// m_emulatedMaximized=true，并请求交换链重建。
///
/// @return macOS 成功应用可见工作区矩形时返回 true；其他平台固定返回 false。
/// @warning 低频最大化路径：会修改原生窗口 frame 和 resize 脏位。
bool NativeWindow::applyEmulatedMaximizedPlacement()
{
#if defined(__APPLE__)
    // 无 GLFW 窗口时不能建立模拟状态。
    if ( !m_windowHandle ) {
        return false;
    }

    // 在调用 helper 前先置状态，屏蔽同步 position/size callback 写入 normal
    // rect。
    m_emulatedMaximized = true;
    if ( applyMacOSVisibleWindowFrame(m_windowHandle) ) {
        // Cocoa helper 成功后显式通知交换链处理新的 framebuffer extent。
        m_lastResizeTime = std::chrono::steady_clock::now();
        m_resizePending.store(true, std::memory_order_relaxed);
        return true;
    }

    // Cocoa 路径不可用时，用 GLFW 工作区在当前最佳显示器上构造同等矩形。
    MonitorPlacementBounds bounds;
    if ( queryMonitorWorkAreaBounds(findBestMonitorForWindow(m_windowHandle),
                                    bounds) ) {
        // setter 自身屏蔽 placement callback，并设置 resize 脏位。
        setWindowPlacementWithoutRemembering(
            bounds.m_x, bounds.m_y, bounds.m_width, bounds.m_height);
        return true;
    }

    // 两种定位方式都失败时回滚逻辑状态，允许调用方尝试 GLFW 原生 maximize。
    m_emulatedMaximized = false;
    return false;
#else
    // 非 macOS 平台不使用应用层模拟最大化。
    return false;
#endif
}

/// @brief 把当前窗口状态转发给平台 frame adapter 刷新外形。
/// @warning 低频状态变化路径：adapter 可能调用 DWM、XShape 或 Core Animation
/// API。
void NativeWindow::refreshWindowFrameShape()
{
    // Startup 或不支持的平台没有 adapter，保持无操作。
    if ( m_windowFrameAdapter ) {
        m_windowFrameAdapter->refreshFrameShape();
    }
}

/// @brief 写入经过验证的普通窗口还原矩形缓存。
///
/// 位置可位于负虚拟桌面坐标；只拒绝非正尺寸。函数不查询或修改 GLFW 窗口。
///
/// @param x 普通窗口 X。
/// @param y 普通窗口 Y。
/// @param width 正宽度。
/// @param height 正高度。
void NativeWindow::rememberWindowPlacement(int x, int y, int width, int height)
{
    // 无效尺寸不能覆盖最后一个可恢复矩形。
    if ( width <= 0 || height <= 0 ) {
        return;
    }

    // 四个字段作为同一事件线程内的矩形快照连续写入。
    m_normalWindowPos[0]  = x;
    m_normalWindowPos[1]  = y;
    m_normalWindowSize[0] = width;
    m_normalWindowSize[1] = height;
}

/// @brief 处理窗口最小化/恢复事件，并在任务栏恢复时保留最大化状态。
///
/// 最小化时先发布失焦状态，并按平台捕获“此前最大化”意图；恢复时若意图仍存在且
///
/// 当前不是最大化，则重新应用 macOS
/// 模拟或平台原生最大化。最后消费一次性恢复标志
/// 并刷新 frame 外观。
///
/// @param iconified GLFW 最小化状态。
/// @warning 低频 GLFW 状态回调：可能调用 maximize 或原生 property
/// API，不得阻塞。
void NativeWindow::handleWindowIconify(int iconified)
{
    // 窗口关闭附近的回调允许安全忽略。
    if ( !m_windowHandle ) {
        return;
    }

    if ( iconified == GLFW_TRUE ) {
        // 最小化等价于失去可交互焦点，主动通知 UI 清理输入状态。
        Event::EventBus::instance().publish(Event::GLFWNativeEvent{
            .type           = Event::NativeEventType::GLFW_WINDOW_FOCUS_CHANGED,
            .hasStateChange = true,
            .isFocused      = false,
        });
#ifdef _WIN32
        // 合并 GLFW 请求、adapter property 和系统 placement，抵抗 Win32
        // 消息顺序差异。
        m_restoreMaximizedAfterIconify =
            shouldPreserveWin32MaximizedRestore(m_windowHandle,
                                                m_lastRequestedMaximized,
                                                m_restoreMaximizedAfterIconify);
        // 在最小化期间继续把最近意图视为最大化，避免中间 restored callback
        // 清除。
        m_lastRequestedMaximized =
            m_lastRequestedMaximized || m_restoreMaximizedAfterIconify;
        setWin32RestoreMaximizedProperty(m_windowHandle,
                                         m_restoreMaximizedAfterIconify);
#else
        // 其他平台只需结合应用请求和当前统一最大化状态。
        m_restoreMaximizedAfterIconify =
            m_lastRequestedMaximized || isWindowMaximized();
#endif
        return;
    }

#ifdef _WIN32
    // 恢复阶段再次读取 Win32 property/placement，捕获最小化期间产生的状态变化。
    m_restoreMaximizedAfterIconify =
        shouldPreserveWin32MaximizedRestore(m_windowHandle,
                                            m_lastRequestedMaximized,
                                            m_restoreMaximizedAfterIconify);
#endif
    // 只有系统尚未自行恢复最大化时才主动补偿，避免重复 maximize。
    if ( m_restoreMaximizedAfterIconify && !isWindowMaximized() ) {
        m_lastRequestedMaximized = true;
#if defined(__APPLE__)
        // macOS 优先恢复应用层 visibleFrame 最大化。
        if ( applyEmulatedMaximizedPlacement() ) {
            publishWindowMaximizedState(true);
        } else {
            glfwMaximizeWindow(m_windowHandle);
        }
#else
        // X11/Win32 交给窗口管理器最大化，状态通知由对应 callback 发布。
        glfwMaximizeWindow(m_windowHandle);
#endif
    }
    // 恢复意图仅覆盖一次 iconify 生命周期，处理后清除。
    m_restoreMaximizedAfterIconify = false;
    refreshWindowFrameShape();
}

/// @brief 将 GLFW 键盘回调参数转换为项目统一按键事件并发布。
///
/// Translator 负责 key/action/mods 映射；非 Release
/// 动作再根据统一键值和修饰键推导
/// codepoint，Release
/// 明确写零，避免消费者把释放事件当文本输入。
///
/// @param w 事件窗口；当前转换不需要访问 user pointer。
/// @param key GLFW key code。
/// @param scancode 平台扫描码，原样保留。
/// @param action GLFW press/repeat/release。
/// @param mods GLFW 修饰键 bit mask。
/// @warning 高频输入回调：仅栈上 DTO 转换和一次 EventBus publish。
void NativeWindow::GLFW_KeyCallback(GLFWwindow* w, int key, int scancode,
                                    int action, int mods)
{
    // 窗口参数属于统一 GLFW callback 签名，事件本身不绑定具体窗口对象。
    MMM::Event::GLFWKeyEvent e;

    // 先把平台枚举归一化，后续业务层不直接依赖 GLFW 常量。
    e.key      = MMM::Event::Translator::GLFW::GetKey(key);
    e.action   = MMM::Event::Translator::GLFW::GetAction(action);
    e.mods     = MMM::Event::Translator::GLFW::GetMods(mods);
    e.scancode = scancode;

    // Release 不产生文本；Press/Repeat 可按统一键位推导 codepoint。
    if ( e.action != MMM::Event::Input::Action::Release ) {
        e.codepoint = MMM::Event::Translator::ResolveCodepoint(e.key, e.mods);
    } else {
        e.codepoint = 0;
    }

    // DTO 已完全脱离 GLFW 参数生命周期，可以同步发布给输入系统。
    MMM::Event::EventBus::instance().publish(e);
}


/// @brief 把 GLFW 文件拖放路径和发生位置打包为项目事件。
///
/// 每个 UTF-8 路径复制进事件 vector，随后读取客户区 cursor
/// 坐标；事件发布后不再
/// 依赖 GLFW 提供的 const char** 生命周期。
///
/// @param w 接收拖放的窗口。
/// @param count paths 中的元素数量。
/// @param paths 仅在 callback 期间有效的 UTF-8 路径数组。
/// @warning 低频用户输入路径：按文件数分配字符串并写日志，不属于每帧热路径。
void NativeWindow::GLFW_DropCallback(GLFWwindow* w, int count,
                                     const char** paths)
{
    // 日志保留本次 OS 拖放文件数量，便于诊断平台路径编码。
    XINFO("GLFW Drop Callback triggered! count: {}", count);
    MMM::Event::GLFWDropEvent e;
    for ( int i = 0; i < count; ++i ) {
        // emplace_back 在 GLFW 缓冲失效前取得每条路径所有权。
        XINFO("  Dropped path[{}]: {}", i, paths[i]);
        e.paths.emplace_back(paths[i]);
    }
    // GLFW drop callback 不附带坐标，读取投放完成时客户区 cursor 位置。
    double xpos, ypos;
    glfwGetCursorPos(w, &xpos, &ypos);
    e.pos = { static_cast<float>(xpos), static_cast<float>(ypos) };

    MMM::Event::EventBus::instance().publish(e);
}

/// @brief GLFW 窗口最小化/恢复回调入口。
///
/// 静态函数只解析 user
/// pointer，再把平台状态机交给实例方法，窗口销毁附近允许
/// pointer 为空。
///
/// @param window GLFW 窗口句柄。
/// @param iconified GLFW 最小化状态。
void NativeWindow::GLFW_IconifyCallback(GLFWwindow* window, int iconified)
{
    // user pointer 在 NativeWindow 构造中于 callback 注册前设置。
    auto app =
        reinterpret_cast<NativeWindow*>(glfwGetWindowUserPointer(window));
    if ( app ) {
        app->handleWindowIconify(iconified);
    }
}

/// @brief 查询 GLFW 是否已收到窗口关闭请求。
/// @return 窗口存在且 should-close flag 为真时返回 true。
/// @warning 主循环热路径：只读取 GLFW 窗口标志，不分配或阻塞。
bool NativeWindow::shouldClose() const
{
    return m_windowHandle && glfwWindowShouldClose(m_windowHandle);
}

/// @brief 非阻塞轮询并派发当前线程全部 GLFW 窗口事件。
///
/// 回调会同步发布项目输入/窗口事件；函数不等待新事件到达。
///
/// @warning 主循环热路径：每帧调用 glfwPollEvents，禁止替换为阻塞式
/// waitEvents。
void NativeWindow::pollEvents() const
{
    // 处理操作系统已排队的鼠标、键盘、窗口和关闭事件。
    glfwPollEvents();
}

/// @brief 获取当前 GLFW 主窗口的非 owning 句柄。
/// @return 创建失败或已无窗口时为 nullptr。
GLFWwindow* NativeWindow::getWindowHandle() const
{
    return m_windowHandle;
}

/// @brief 查询交换链应匹配的 framebuffer 物理像素尺寸。
/// @param width 输出 framebuffer 宽度。
/// @param height 输出 framebuffer 高度。
/// @warning 渲染准备路径：只调用 GLFW 查询，不执行交换链重建或等待。
void NativeWindow::getFramebufferSize(int& width, int& height) const
{
    // framebuffer 尺寸而非 window 逻辑尺寸决定 Vulkan surface extent。
    glfwGetFramebufferSize(m_windowHandle, &width, &height);
}

/// @brief 在主显示器独占全屏与先前普通窗口矩形之间切换。
///
/// 进入前保存当前 normal placement 到独立 fullscreen
/// backup，查询主显示器模式后
/// 切换 monitor；退出时解除 monitor 并恢复
/// backup 矩形。最终刷新平台 frame 外观。
///
/// @warning 用户窗口命令低频路径：会改变 GLFW monitor 绑定并触发
/// framebuffer
/// resize，禁止在每帧循环调用。
void NativeWindow::ToggleFullscreen()
{
    // monitor 非空表示 GLFW 当前处于 fullscreen 模式。
    if ( glfwGetWindowMonitor(m_windowHandle) ) {
        // 解除 monitor 时同时提交进入全屏前保存的普通位置、尺寸和不限制刷新率。
        glfwSetWindowMonitor(m_windowHandle,
                             nullptr,
                             m_backupPos[0],
                             m_backupPos[1],
                             m_backupSize[0],
                             m_backupSize[1],
                             0);
        // 退出后的矩形重新成为后续最大化还原和持久化基线。
        rememberWindowPlacement(
            m_backupPos[0], m_backupPos[1], m_backupSize[0], m_backupSize[1]);
        XINFO("Restored window to {}x{} at ({},{})",
              m_backupSize[0],
              m_backupSize[1],
              m_backupPos[0],
              m_backupPos[1]);
    } else {
        // 只在当前状态可记忆时刷新 normal rect，再复制到 fullscreen 专用
        // backup。
        rememberCurrentWindowPlacement();
        m_backupPos[0]  = m_normalWindowPos[0];
        m_backupPos[1]  = m_normalWindowPos[1];
        m_backupSize[0] = m_normalWindowSize[0];
        m_backupSize[1] = m_normalWindowSize[1];

        // 全屏目标固定为主显示器当前视频模式。
        GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if ( !monitor || !mode ) {
            // 保留现有窗口模式和 backup，不提交不完整 monitor 切换。
            XERROR("Failed to enter fullscreen mode: no primary monitor.");
            return;
        }
        // monitor 坐标从 (0,0) 开始，尺寸与刷新率严格使用所选视频模式。
        glfwSetWindowMonitor(m_windowHandle,
                             monitor,
                             0,
                             0,
                             mode->width,
                             mode->height,
                             mode->refreshRate);
        XINFO("Entered fullscreen mode.");
    }
    // 全屏不应保留普通窗口圆角/阴影，退出后需恢复。
    refreshWindowFrameShape();
}

}  // namespace MMM::Graphic
