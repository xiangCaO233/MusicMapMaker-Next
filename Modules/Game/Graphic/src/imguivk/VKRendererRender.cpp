#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#endif

#include "config/AppConfig.h"
#include "graphic/CursorManager.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/IGraphicUserHook.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderPass.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ice/thread/ThreadPool.hpp>
#include <latch>

#ifdef _WIN32
#    include "graphic/glfw/window/adapters/Win32WindowAdapter.h"
#    define GLFW_EXPOSE_NATIVE_WIN32
#    include <GLFW/glfw3native.h>
#    include <dwmapi.h>

#    ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#        define DWMWA_WINDOW_CORNER_PREFERENCE 33
#    endif
#    ifndef DWMWCP_ROUND
#        define DWMWCP_ROUND 2
#    endif
#endif

namespace MMM::Graphic
{
namespace
{
/// @brief 渲染性能统计使用的单调时钟。
using RenderProfileClock = std::chrono::steady_clock;

/// @brief 渲染性能统计日志输出间隔。
constexpr auto RENDER_PROFILE_LOG_INTERVAL = std::chrono::seconds(2);

/// @brief 单个渲染阶段在统计窗口内的累计耗时。
///
/// 只保存总和与最大值，不记录逐帧样本，令关闭日志时零开销、开启日志时也只有
/// 常量空间。平均值在输出边界用完整帧数现算。
struct RenderStageStat {
    /// @brief 阶段累计耗时，单位为毫秒。
    double totalMs{ 0.0 };

    /// @brief 阶段单帧最大耗时，单位为毫秒。
    double maxMs{ 0.0 };

    /// @brief 追加一次阶段耗时。
    /// @param elapsedMs 本帧该阶段耗时，单位为毫秒。
    /// @warning 渲染热路径：每帧执行，只能做常量时间浮点累加。
    void add(double elapsedMs)
    {
        // 同一阶段每个完整帧只追加一次，因此总和可直接除以窗口帧数。
        totalMs += elapsedMs;
        maxMs = std::max(maxMs, elapsedMs);
    }

    /// @brief 获取统计窗口内的平均耗时。
    /// @param frameCount 统计窗口内累计的完整帧数。
    /// @return 平均耗时，单位为毫秒。
    double average(uint64_t frameCount) const
    {
        // 防御空窗口，避免配置刚开启或重置后产生除零和无意义日志。
        return frameCount == 0 ? 0.0
                               : totalMs / static_cast<double>(frameCount);
    }

    /// @brief 清空阶段统计数据。
    void reset()
    {
        // 统计对象跨帧复用，只清标量，不释放内存或访问系统时钟。
        totalMs = 0.0;
        maxMs   = 0.0;
    }
};

/// @brief 渲染主循环的分阶段累计性能统计。
///
/// 所有字段严格对应 render 中的一对时间点。只有成功走到帧尾的帧才增加
/// frameCount；交换链重建或 acquire 失败的早退不会用不完整时间点污染平均值。
struct RenderProfileAccumulator {
    /// @brief 当前统计窗口的起始时间。
    RenderProfileClock::time_point windowStart{ RenderProfileClock::now() };

    /// @brief 当前统计窗口内累计的完整帧数。
    uint64_t frameCount{ 0 };

    /// @brief 单帧总耗时统计。
    RenderStageStat total;

    /// @brief 等待和重置 Fence 的耗时统计。
    RenderStageStat fence;

    /// @brief acquireNextImageKHR 的耗时统计。
    RenderStageStat acquire;

    /// @brief GLFW 事件轮询的耗时统计。
    RenderStageStat pollEvents;

    /// @brief ImGui 新帧准备和光标模式同步的耗时统计。
    RenderStageStat newFrame;

    /// @brief 图形钩子资源准备的耗时统计。
    RenderStageStat prepareResources;

    /// @brief UI 更新和 ImGui 绘制列表生成前逻辑的耗时统计。
    RenderStageStat updateUi;

    /// @brief ImGui::Render 的耗时统计。
    RenderStageStat imguiRender;

    /// @brief 命令缓冲开始录制前准备的耗时统计。
    RenderStageStat commandSetup;

    /// @brief 离屏画布命令录制的耗时统计。
    RenderStageStat offscreenRecord;

    /// @brief 主 RenderPass 与 ImGui Vulkan 绘制命令录制的耗时统计。
    RenderStageStat mainRecord;

    /// @brief 图形队列提交的耗时统计。
    RenderStageStat submit;

    /// @brief presentKHR 的耗时统计。
    RenderStageStat present;

    /// @brief ImGui 多视口平台窗口更新和渲染的耗时统计。
    RenderStageStat platformWindows;

    /// @brief 清空统计窗口。
    /// @param nextStart 下一个统计窗口的起始时间。
    void reset(RenderProfileClock::time_point nextStart)
    {
        // 新窗口从调用方给出的同一时间点开始，避免逐字段 reset 期间的时间偏移。
        windowStart = nextStart;
        frameCount  = 0;
        total.reset();
        fence.reset();
        acquire.reset();
        pollEvents.reset();
        newFrame.reset();
        prepareResources.reset();
        updateUi.reset();
        imguiRender.reset();
        commandSetup.reset();
        offscreenRecord.reset();
        mainRecord.reset();
        submit.reset();
        present.reset();
        platformWindows.reset();
    }

    /// @brief 到达统计间隔后输出一次累计结果。
    ///
    /// 输出拆为总览和两行阶段明细，避免单条日志过长被后端截断。所有平均值共用
    /// frameCount，最大值则保留统计窗口内最慢一帧，便于区分持续负载与偶发尖峰。
    /// 未达到间隔时不执行格式化或日志调用。
    ///
    /// @param now 当前时间。
    /// @warning 渲染热路径：每帧只做时间间隔判断；到达间隔后才写日志。
    void logIfReady(RenderProfileClock::time_point now)
    {
        // steady_clock 不受系统时间校准影响，适合跨多个渲染帧累计间隔。
        const double elapsedSeconds =
            std::chrono::duration<double>(now - windowStart).count();
        const double logIntervalSeconds =
            std::chrono::duration<double>(RENDER_PROFILE_LOG_INTERVAL).count();
        if ( elapsedSeconds < logIntervalSeconds || frameCount == 0 ) {
            return;
        }

        // 极小下限仅防御异常零间隔，不改变正常两秒统计窗口的 FPS。
        const double averageFps = static_cast<double>(frameCount) /
                                  std::max(elapsedSeconds, 0.000001);
        XINFO(
            "RenderProfile {:.2f}s frames={} fps={:.1f} "
            "total(avg/max)={:.3f}/{:.3f}ms",
            elapsedSeconds,
            frameCount,
            averageFps,
            total.average(frameCount),
            total.maxMs);
        XINFO(
            "RenderStages avg/max ms: fence {:.3f}/{:.3f}, acquire "
            "{:.3f}/{:.3f}, poll {:.3f}/{:.3f}, newFrame {:.3f}/{:.3f}, "
            "prepare {:.3f}/{:.3f}, updateUI {:.3f}/{:.3f}",
            fence.average(frameCount),
            fence.maxMs,
            acquire.average(frameCount),
            acquire.maxMs,
            pollEvents.average(frameCount),
            pollEvents.maxMs,
            newFrame.average(frameCount),
            newFrame.maxMs,
            prepareResources.average(frameCount),
            prepareResources.maxMs,
            updateUi.average(frameCount),
            updateUi.maxMs);
        XINFO(
            "RenderStages avg/max ms: imguiRender {:.3f}/{:.3f}, cmdSetup "
            "{:.3f}/{:.3f}, offscreen {:.3f}/{:.3f}, mainPass {:.3f}/{:.3f}, "
            "submit {:.3f}/{:.3f}, present {:.3f}/{:.3f}, platform "
            "{:.3f}/{:.3f}",
            imguiRender.average(frameCount),
            imguiRender.maxMs,
            commandSetup.average(frameCount),
            commandSetup.maxMs,
            offscreenRecord.average(frameCount),
            offscreenRecord.maxMs,
            mainRecord.average(frameCount),
            mainRecord.maxMs,
            submit.average(frameCount),
            submit.maxMs,
            present.average(frameCount),
            present.maxMs,
            platformWindows.average(frameCount),
            platformWindows.maxMs);

        // 输出完成后从当前帧尾开启下一窗口，当前帧不会重复计入。
        reset(now);
    }
};

/// @brief 计算两个时间点之间的毫秒差。
///
/// 使用 double 毫秒保留亚毫秒阶段精度；时间点均来自同一 steady_clock。
///
/// @param begin 起始时间点。
/// @param end 结束时间点。
/// @return 时间差，单位为毫秒。
double elapsedMilliseconds(RenderProfileClock::time_point begin,
                           RenderProfileClock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

/// @brief 在启用渲染性能日志时读取当前时间点。
/// @param enabled 是否启用渲染性能日志。
/// @return 启用时返回当前时间点，关闭时返回空时间点。
/// @warning 渲染热路径：每个阶段边界调用；关闭日志时不得调用系统时钟。
RenderProfileClock::time_point profileTimePoint(bool enabled)
{
    return enabled ? RenderProfileClock::now()
                   : RenderProfileClock::time_point{};
}

/// @brief 判断当前 ImGui 光标是否应使用原生窗口缩放光标。
///
/// 软件光标适合普通指针，但窗口边缘缩放需要平台光标与原生 hit-test 反馈一致，
/// 因此四种方向缩放光标临时回退 GLFW。
///
/// @param cursor 当前 ImGui 光标类型。
/// @return 是缩放光标时返回 true。
bool isNativeResizeCursor(ImGuiMouseCursor cursor)
{
    return cursor == ImGuiMouseCursor_ResizeNS ||
           cursor == ImGuiMouseCursor_ResizeEW ||
           cursor == ImGuiMouseCursor_ResizeNESW ||
           cursor == ImGuiMouseCursor_ResizeNWSE;
}

constexpr size_t IMGUI_MOUSE_CURSOR_COUNT =
    static_cast<size_t>(ImGuiMouseCursor_COUNT);
/// @brief 按 ImGuiMouseCursor 枚举索引保存惰性创建的 GLFW 光标。
using GlfwStandardCursorCache =
    std::array<GLFWcursor*, IMGUI_MOUSE_CURSOR_COUNT>;

/// @brief 获取由当前 GLFW 生命周期拥有的标准光标缓存。
///
/// 函数局部静态数组只保存非拥有句柄，统一由 releaseGlfwCursorResources 在
/// glfwTerminate 前销毁；渲染器实例销毁重建时无需反复创建系统光标。
///
/// @return 标准光标句柄数组。
/// @warning 渲染热路径：每帧可能访问；不得在此加入分配或 GLFW 调用。
GlfwStandardCursorCache& glfwStandardCursorCache()
{
    static GlfwStandardCursorCache cursors{};
    return cursors;
}

/// @brief 获取对应 ImGui 光标类型的 GLFW 标准光标。
///
/// 有效枚举按需创建一次并缓存在稳定槽位。编译期检测不同 GLFW 版本提供的扩展
/// 光标常量，缺少专用形状时保持 arrow 后备，避免引入版本条件到调用方。
///
/// @param cursor 当前 ImGui 光标类型。
/// @return GLFW 标准光标句柄，不支持时返回 nullptr。
/// @warning 渲染热路径低频分支：首次遇到某个光标类型时创建 GLFW cursor。
GLFWcursor* getGlfwStandardCursor(ImGuiMouseCursor cursor)
{
    auto& cursors = glfwStandardCursorCache();

    // ImGui 的 None 为负值，COUNT 是哨兵；两者都不能作为数组索引。
    if ( cursor < 0 || cursor >= ImGuiMouseCursor_COUNT ) {
        return nullptr;
    }
    if ( cursors[static_cast<size_t>(cursor)] ) {
        return cursors[static_cast<size_t>(cursor)];
    }

    // 所有未显式映射或旧 GLFW 不支持的枚举都安全回退箭头。
    int glfwCursor = GLFW_ARROW_CURSOR;
    switch ( cursor ) {
    case ImGuiMouseCursor_TextInput: glfwCursor = GLFW_IBEAM_CURSOR; break;
    case ImGuiMouseCursor_Hand:
#if defined(GLFW_POINTING_HAND_CURSOR)
        glfwCursor = GLFW_POINTING_HAND_CURSOR;
#elif defined(GLFW_HAND_CURSOR)
        glfwCursor = GLFW_HAND_CURSOR;
#endif
        break;
    case ImGuiMouseCursor_ResizeNS:
#if defined(GLFW_RESIZE_NS_CURSOR)
        glfwCursor = GLFW_RESIZE_NS_CURSOR;
#endif
        break;
    case ImGuiMouseCursor_ResizeEW:
#if defined(GLFW_RESIZE_EW_CURSOR)
        glfwCursor = GLFW_RESIZE_EW_CURSOR;
#endif
        break;
    case ImGuiMouseCursor_ResizeNESW:
#if defined(GLFW_RESIZE_NESW_CURSOR)
        glfwCursor = GLFW_RESIZE_NESW_CURSOR;
#endif
        break;
    case ImGuiMouseCursor_ResizeNWSE:
#if defined(GLFW_RESIZE_NWSE_CURSOR)
        glfwCursor = GLFW_RESIZE_NWSE_CURSOR;
#endif
        break;
    case ImGuiMouseCursor_ResizeAll:
#if defined(GLFW_RESIZE_ALL_CURSOR)
        glfwCursor = GLFW_RESIZE_ALL_CURSOR;
#endif
        break;
    case ImGuiMouseCursor_NotAllowed:
#if defined(GLFW_NOT_ALLOWED_CURSOR)
        glfwCursor = GLFW_NOT_ALLOWED_CURSOR;
#endif
        break;
    default: break;
    }

    // GLFW 创建失败会缓存空值，下帧仍可重试，调用方可接受 nullptr
    // 恢复默认光标。
    cursors[static_cast<size_t>(cursor)] = glfwCreateStandardCursor(glfwCursor);
    return cursors[static_cast<size_t>(cursor)];
}

/// @brief GLFW 主窗口光标状态缓存。
///
/// 缓存只用于抑制每帧重复 glfwSetInputMode/glfwSetCursor；窗口改变时必须同时
/// 失效 mode 与 cursor，不能把旧窗口状态套用到新句柄。
struct GlfwCursorState {
    /// @brief 当前缓存所属窗口。
    GLFWwindow* window{ nullptr };

    /// @brief 当前 GLFW 光标模式。
    int mode{ -1 };

    /// @brief 当前 GLFW 标准光标类型。
    ImGuiMouseCursor cursor{ ImGuiMouseCursor_COUNT };
};

/// @brief GLFW 主窗口可激活状态缓存。
///
/// 只保存上一帧的“聚焦且可见”合成状态，用于检测 false 到 true 的边沿；不负责
/// 持有窗口或改变焦点。
struct MainWindowFocusState {
    /// @brief 当前缓存所属 GLFW 主窗口。
    GLFWwindow* window{ nullptr };

    /// @brief 上一帧主窗口是否拥有焦点且未最小化。
    bool active{ false };
};

/// @brief 获取 GLFW 主窗口光标状态缓存。
/// @return 可修改的光标状态。
/// @warning 渲染热路径：每帧访问；不得在此加入分配或 GLFW 调用。
GlfwCursorState& glfwCursorState()
{
    static GlfwCursorState state;
    return state;
}

/// @brief 获取 GLFW 主窗口焦点状态缓存。
/// @return 可修改的焦点状态。
/// @warning 渲染热路径：每帧访问；不得在此加入分配或 GLFW 调用。
MainWindowFocusState& mainWindowFocusState()
{
    static MainWindowFocusState state;
    return state;
}

/// @brief 对 GLFW 主窗口应用光标模式，并跳过重复状态写入。
///
/// 隐藏模式不设置具体标准光标，并清除缓存枚举；恢复 normal 后即使 ImGui 枚举
/// 与隐藏前相同也会重新绑定句柄。空窗口保持缓存不变，等待有效句柄重新同步。
///
/// @param window GLFW 窗口句柄。
/// @param cursorMode GLFW 光标模式。
/// @param cursor 当前 ImGui 光标类型。
/// @warning 渲染热路径：每帧调用；只有状态变化时才触发 GLFW API。
void applyGlfwCursorMode(GLFWwindow* window, int cursorMode,
                         ImGuiMouseCursor cursor)
{
    auto& state = glfwCursorState();

    if ( !window ) {
        return;
    }

    // 新窗口的真实 GLFW 状态未知，所有缓存字段都回到哨兵值强制首次写入。
    if ( state.window != window ) {
        state.window = window;
        state.mode   = -1;
        state.cursor = ImGuiMouseCursor_COUNT;
    }

    // 输入模式通常跨帧稳定，只有软件/原生光标切换时调用平台 API。
    if ( state.mode != cursorMode ) {
        glfwSetInputMode(window, GLFW_CURSOR, cursorMode);
        state.mode = cursorMode;
    }

    // GLFW 隐藏/禁用模式忽略具体 cursor 句柄，失效枚举供未来 normal 模式重绑。
    if ( cursorMode != GLFW_CURSOR_NORMAL ) {
        state.cursor = ImGuiMouseCursor_COUNT;
        return;
    }

    // 模式已为 normal 时只在 ImGui 请求形状变化后切换标准光标。
    if ( state.cursor != cursor ) {
        glfwSetCursor(window, getGlfwStandardCursor(cursor));
        state.cursor = cursor;
    }
}

/// @brief 隐藏 GLFW 原生光标。
/// @param window GLFW 窗口句柄。
/// @warning 渲染热路径：软件光标模式下每帧调用；内部会过滤重复写入。
void hideNativeCursor(GLFWwindow* window)
{
    applyGlfwCursorMode(window, GLFW_CURSOR_HIDDEN, ImGuiMouseCursor_None);
}

/// @brief 对 GLFW 主窗口应用当前 ImGui 光标。
///
/// ImGui None 或范围外值表示当前不应显示系统指针，统一进入隐藏路径；其余枚举
/// 通过惰性标准光标缓存映射。
///
/// @param window GLFW 窗口句柄。
/// @param cursor 当前 ImGui 光标类型。
/// @warning 渲染热路径：每帧最多一次；内部会过滤重复 GLFW 状态写入。
void applyNativeCursor(GLFWwindow* window, ImGuiMouseCursor cursor)
{
    if ( cursor < 0 || cursor >= ImGuiMouseCursor_COUNT ) {
        hideNativeCursor(window);
        return;
    }

    applyGlfwCursorMode(window, GLFW_CURSOR_NORMAL, cursor);
}

/// @brief 消费主窗口从非激活态到可见聚焦态的激活边沿。
///
/// 首次观察某个窗口时，如果它已经激活，也产生一次边沿以同步整个多视口窗口组。
/// Windows 额外排除 iconified，因为任务栏最小化期间焦点属性可能短暂保持 true。
///
/// @param window GLFW 主窗口句柄。
/// @return 本帧主窗口刚获得焦点且未最小化时返回 true。
/// @warning 渲染热路径：每帧调用；只读取 GLFW 窗口属性和更新静态缓存。
bool consumeMainWindowActivation(GLFWwindow* window)
{
    auto& state = mainWindowFocusState();

    // 空句柄同时清除旧窗口状态，下一次有效窗口会按首次观察处理。
    if ( !window ) {
        state.window = nullptr;
        state.active = false;
        return false;
    }

    const bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
    bool       active  = focused;
#ifdef _WIN32
    // Windows 10 在任务栏最小化期间可能短暂保留焦点，不能把该状态当作恢复边沿。
    active = active && glfwGetWindowAttrib(window, GLFW_ICONIFIED) != GLFW_TRUE;
#endif
    // 窗口身份改变时没有可比较的上一帧，直接把当前激活态作为初始边沿结果。
    if ( state.window != window ) {
        state.window = window;
        state.active = active;
        return active;
    }

    // 只有非激活到激活的转换触发提窗，持续聚焦不会每帧修改平台 Z 序。
    const bool activated = active && !state.active;
    state.active         = active;
    return activated;
}

#ifdef _WIN32
/// @brief HWND 属性名，与 Win32WindowAdapter 协同保留最小化前最大化状态。
constexpr const wchar_t* RESTORE_MAXIMIZED_PROP =
    L"MMMRestoreMaximizedAfterMinimize";

/// @brief 判断 Win32 窗口最小化前是否处于最大化状态。
///
/// 自定义窗框适配器用窗口属性保存自己的恢复意图；若属性不存在，再检查系统当前
/// zoom 状态和 WINDOWPLACEMENT 标志，覆盖任务栏及系统菜单触发的最小化路径。
///
/// @param hwnd Win32 窗口句柄。
/// @return 任务栏恢复时应恢复到最大化状态则返回 true。
/// @warning 低频平台查询：只在主窗口被任务栏/系统激活时调用。
bool shouldRestoreWin32WindowToMaximized(HWND hwnd)
{
    if ( !hwnd ) {
        return false;
    }

    // 适配器属性优先，因为最小化后 IsZoomed 可能不再反映最小化前状态。
    if ( GetPropW(hwnd, RESTORE_MAXIMIZED_PROP) != nullptr ) {
        return true;
    }

    if ( IsZoomed(hwnd) ) {
        return true;
    }

    // WPF_RESTORETOMAXIMIZED 在部分 SDK 路径中没有便捷枚举，保留规范位值查询。
    constexpr UINT  restoreToMaximizedFlag = 0x0002;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(WINDOWPLACEMENT);
    if ( !GetWindowPlacement(hwnd, &placement) ) {
        return false;
    }
    return placement.showCmd == SW_SHOWMAXIMIZED ||
           (placement.flags & restoreToMaximizedFlag) != 0;
}
#endif

/// @brief 将 GLFW 窗口恢复并提升到当前窗口组顶层。
///
/// Windows 直接操作 HWND，区分恢复最大化与普通恢复，并允许只提升 Z 序而不抢
/// 焦点；其他平台通过 GLFW 恢复和聚焦，没有无激活提窗的可移植接口。
///
/// @param window GLFW 窗口句柄。
/// @param activate 是否请求系统前台焦点。
/// @warning 低频平台操作：只在主窗口被任务栏/系统激活时调用。
void raiseGlfwWindowToTop(GLFWwindow* window, bool activate)
{
    if ( !window ) {
        return;
    }

#ifdef _WIN32
    // 平台句柄缺失时不能安全调用 User32；GLFW 窗口本身有效也不保证桥接成功。
    HWND hwnd = glfwGetWin32Window(window);
    if ( !hwnd ) {
        return;
    }

    // 先恢复最小化窗口，再设置 Z 序；否则 SetWindowPos 不会使其重新可见。
    if ( IsIconic(hwnd) ) {
        const bool restoreMaximized = shouldRestoreWin32WindowToMaximized(hwnd);
        ShowWindow(hwnd, restoreMaximized ? SW_SHOWMAXIMIZED : SW_RESTORE);
    }
#else
    if ( glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE ) {
        glfwRestoreWindow(window);
    }

#endif

#ifdef _WIN32
    // 提窗不应改变 ImGui 平台窗口已经计算好的位置和尺寸。
    UINT flags = SWP_NOMOVE | SWP_NOSIZE;
    if ( !activate ) {
        flags |= SWP_NOACTIVATE;
    }
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, flags);
    if ( activate ) {
        SetForegroundWindow(hwnd);
    }
#else
    (void)activate;
    glfwFocusWindow(window);
#endif
}

/// @brief 主窗口被系统激活时同步提升所有 ImGui 多视口窗口。
///
/// 多视口工具窗是独立原生窗口，主窗口从任务栏恢复时系统不一定同步提升它们。
/// 本函数只在激活边沿遍历平台视口：Windows 先激活主窗，再无激活提升工具窗；
/// 其他平台先聚焦工具窗，最后重新聚焦主窗，保证键盘焦点归属正确。
///
/// @param mainWindow GLFW 主窗口句柄。
/// @warning 低频平台操作：只在主窗口焦点激活边沿触发，避免每帧提窗。
void raiseImGuiViewportGroup(GLFWwindow* mainWindow)
{
    if ( !mainWindow ) {
        return;
    }

#ifdef _WIN32
    // 主窗口仍最小化时不提升任何从属视口，避免工具窗单独出现在其他应用之上。
    HWND mainHwnd = glfwGetWin32Window(mainWindow);
    if ( !mainHwnd || IsIconic(mainHwnd) ) {
        return;
    }

    // 在 Windows 下提窗顺序很关键：主窗口先拿回前台焦点，随后独立
    // 独立 ImGui 视口只提升 Z 序而不抢焦点，避免浮动工具窗落在其他程序后面。
    raiseGlfwWindowToTop(mainWindow, true);
#endif

    // PlatformHandle 在 GLFW 后端中指向 GLFWwindow；空视口和主视口都跳过。
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    for ( int i = 0; i < platformIo.Viewports.Size; ++i ) {
        ImGuiViewport* viewport = platformIo.Viewports[i];
        if ( !viewport || !viewport->PlatformHandle ) {
            continue;
        }

        GLFWwindow* viewportWindow =
            static_cast<GLFWwindow*>(viewport->PlatformHandle);
        if ( !viewportWindow || viewportWindow == mainWindow ) {
            continue;
        }

#ifdef _WIN32
        raiseGlfwWindowToTop(viewportWindow, false);
#else
        raiseGlfwWindowToTop(viewportWindow, true);
#endif
    }

#ifndef _WIN32
    // 非 Windows 分支逐个 focus 会改变键盘焦点，最后恢复到主编辑窗口。
    raiseGlfwWindowToTop(mainWindow, true);
#endif
}
}  // namespace

/// @brief 在 GLFW 终止前销毁并清空进程级原生光标缓存。
///
/// 标准光标句柄属于 GLFW 生命周期而非某个 VKRenderer 实例；统一释放后还要清空
/// 窗口、模式和焦点缓存，防止下次初始化比较到已经失效的句柄状态。
/// 该函数不关闭 GLFW，也不处理 ImGui 软件光标纹理，两类资源由各自所有者按更早
/// 的生命周期阶段释放。
///
/// @warning 低频 GLFW 生命周期路径：只能在主线程且 GLFW 仍已初始化时调用。
void VKRenderer::releaseGlfwCursorResources()
{
    auto& cursors = glfwStandardCursorCache();
    // 逐槽复位让重复调用保持幂等，也允许部分光标从未成功创建。
    for ( GLFWcursor*& cursor : cursors ) {
        if ( cursor ) {
            glfwDestroyCursor(cursor);
            cursor = nullptr;
        }
    }

    glfwCursorState()      = {};
    mainWindowFocusState() = {};
}

// clang-format off
/**
 * @brief 推进输入、UI、离屏录制、主交换链提交与多视口呈现的一帧。
 *
 * 帧槽 fence 防止 CPU 覆写仍在执行的主命令缓冲；acquire 成功后才轮询输入并生成
 * ImGui 数据。离屏任务可在独立 command pool 中并行录制，但提交顺序始终位于
 * 主 render pass 之前。present 后轮转并发帧索引，并更新 ImGui 平台窗口。
 *
 * @param window 主窗口及其 framebuffer/事件接口。
 * @param graphicUserHooks 本帧按给定顺序准备资源、更新 UI 和录制离屏任务的观察
 * 指针列表；调用方保证整个函数期间对象存活。
 */
/// @warning 热路径：主线程每帧执行；Fence/Acquire/Present 不可中断。
/// 禁止在此加入文件系统访问、完整 ECS 遍历、完整排序、异常处理或常态堆分配。
// clang-format on
void VKRenderer::render(NativeWindow&                window,
                        std::span<IGraphicUserHook*> graphicUserHooks)
{
    // 统计器跨帧保存固定标量；配置关闭时 profileTimePoint 不读取时钟，所有 add
    // 分支也在帧尾跳过。
    static RenderProfileAccumulator profile;
    static bool                     lastRenderProfileLoggingEnabled = false;
    const bool                      renderProfileLoggingEnabled =
        Config::AppConfig::instance().getEditorSettings().renderProfileLogging;
    // 从关闭切到开启时丢弃旧窗口，避免停用期间的墙钟时间拉低平均 FPS。
    if ( renderProfileLoggingEnabled && !lastRenderProfileLoggingEnabled ) {
        profile.reset(RenderProfileClock::now());
    }
    lastRenderProfileLoggingEnabled = renderProfileLoggingEnabled;
    // 配置关闭时保留旧标量不会产生输出；再次开启会在上方立刻整体 reset。
    const auto frameProfileStart =
        profileTimePoint(renderProfileLoggingEnabled);

    // NativeWindow 在尺寸回调中只设置消抖状态；到达安全帧边界后转为交换链脏位。
    if ( window.shouldRecreate() ) {
        m_vkSwapChain.markDirty();
    }

    // 重建优先于任何 fence/acquire
    // 操作，避免继续从已经与窗口不匹配的交换链取图。
    if ( m_vkSwapChain.needsRecreate() ) {
        // 最小化返回零 framebuffer，保持脏位并跳过本帧，等待恢复后的有效尺寸。
        int w, h;
        window.getFramebufferSize(w, h);
        if ( w == 0 || h == 0 ) return;

        // triggerRecreate 内部等待设备并在 VKSwapchain 成功后清除脏位，本帧不再
        // acquire 新交换链图像。
        triggerRecreate(window);
        return;
    }

    // 当前 in-flight 槽的 fence 由上次使用该槽的 queue submit 发出；无限等待是
    // 复用其主命令缓冲和 image-available semaphore 前的必需同步点。
    const auto fenceStart = profileTimePoint(renderProfileLoggingEnabled);
    auto       waitResult = m_vkLogicalDevice.waitForFences(
        m_cmdAvailableFences[m_currentFrameIndex],
        true,
        std::numeric_limits<uint64_t>::max());
    if ( waitResult != vk::Result::eSuccess ) {
        // 当前既有控制流只记录异常结果并继续
        // reset/acquire；诊断注释不改变该错误
        // 策略，后续若需中止帧应作为独立行为修复并补充故障测试。
        XWARN("VK Device WaitForFences failed");
    }

    // 提交前把 fence 恢复未触发状态，本帧 queue submit 将再次绑定并最终触发它。
    (void)m_vkLogicalDevice.resetFences(
        m_cmdAvailableFences[m_currentFrameIndex]);
    const auto fenceEnd = profileTimePoint(renderProfileLoggingEnabled);

    // 在 UI 更新前 acquire 可把 FIFO 垂直同步等待放在帧首；返回时
    // image-available semaphore 会在目标图像真正可写后触发。
    const auto acquireStart = profileTimePoint(renderProfileLoggingEnabled);
    vk::ResultValue<uint32_t> imageResult =
        m_vkLogicalDevice.acquireNextImageKHR(
            m_vkSwapChain.m_swapchain,
            std::numeric_limits<uint64_t>::max(),
            m_imageAvailableSems[m_currentFrameIndex]);
    const auto acquireEnd = profileTimePoint(renderProfileLoggingEnabled);

    if ( imageResult.result == vk::Result::eErrorOutOfDateKHR ) {
        // surface 已失配时立即同步重建，当前 acquire 没有可提交图像。
        triggerRecreate(window);
        return;
    } else if ( imageResult.result != vk::Result::eSuccess &&
                imageResult.result != vk::Result::eSuboptimalKHR ) {
        XWARN("acquire ImageKHR failed: {}",
              static_cast<int>(imageResult.result));
        return;
    }

    // eSuboptimalKHR 仍提供合法图像，本帧继续绘制，present 结果会标记后续重建。
    uint32_t imageIndex = imageResult.value;

    // 驱动索引必须落在当前交换链缓存中；越界时不能访问 framebuffer 或
    // semaphore。
    if ( imageIndex >= m_vkSwapChain.m_vkImageBuffers.size() ) {
        XERROR("Invalid image index acquired: {}", imageIndex);
        return;
    }

    // acquire 阻塞解除后再轮询事件，使 UI 使用尽可能接近呈现时刻的输入状态。
    const auto pollStart = profileTimePoint(renderProfileLoggingEnabled);
    window.pollEvents();
    const auto pollEnd = profileTimePoint(renderProfileLoggingEnabled);

    // 后端顺序先更新 Vulkan/GLFW 状态，再由 ImGui::NewFrame 建立当前 UI 帧。
    const auto newFrameStart = profileTimePoint(renderProfileLoggingEnabled);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // 每帧借用当前编辑器配置，不复制完整 DTO；软件光标还要求管理器已经初始化。
    auto&      editorCfg = Config::AppConfig::instance().getEditorConfig();
    const bool softwareCursorAvailable =
        m_cursorManager &&
        editorCfg.settings.cursorStyle == Config::CursorStyle::Software;
    if ( softwareCursorAvailable ) {
        // 先让 ImGui 不绘制默认鼠标，稍后 CursorManager 在 UI draw list
        // 中追加皮肤光标。
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
    const auto newFrameEnd = profileTimePoint(renderProfileLoggingEnabled);

    // 钩子可在 UI 前处理低频脏资源；调用顺序与传入 span
    // 一致，空指针契约由调用方 维护（离屏任务收集处仍防御空项）。
    const auto prepareStart = profileTimePoint(renderProfileLoggingEnabled);
    for ( auto& graphicUserHook : graphicUserHooks ) {
        graphicUserHook->onPrepareResources(m_vkPhysicalDevice,
                                            m_vkLogicalDevice,
                                            m_vkSwapChain,
                                            m_vkCommandPool,
                                            m_LogicDeviceGraphicsQueue);
    }
    const auto prepareEnd = profileTimePoint(renderProfileLoggingEnabled);

    // 所有 UI 钩子先更新 ImGui 控件，再统一追加中央通知和软件光标覆盖层。
    const auto updateUiStart = profileTimePoint(renderProfileLoggingEnabled);
    for ( auto& graphicUserHook : graphicUserHooks ) {
        graphicUserHook->onUpdateUI();
    }

    // 通知属于主视口 UI，必须位于 ImGui::Render 生成 draw data 之前。
    m_vkContext.drawCenterNotification();

    // 读取 UI 命中测试最终选择的光标；窗口缩放形状即使软件模式也交给系统绘制。
    const ImGuiMouseCursor currentMouseCursor = ImGui::GetMouseCursor();
    const bool             useNativeResizeCursor =
        softwareCursorAvailable && isNativeResizeCursor(currentMouseCursor);

    if ( useNativeResizeCursor ) {
        // 原生缩放光标与平台非客户区 hit-test 对齐，避免皮肤光标方向滞后。
        applyNativeCursor(window.getWindowHandle(), currentMouseCursor);
    } else if ( softwareCursorAvailable ) {
        // 普通软件光标模式隐藏系统指针，防止一帧出现双光标。
        hideNativeCursor(window.getWindowHandle());
    } else {
        // 软件资源不可用或配置选择系统样式时完整跟随 ImGui 光标枚举。
        applyNativeCursor(window.getWindowHandle(), currentMouseCursor);
    }

    // Resize 状态不绘制软件光标；其他状态把粒子寿命覆盖和鼠标位置写入 draw
    // list。
    if ( softwareCursorAvailable && !useNativeResizeCursor ) {
        m_cursorManager->UpdateAndDraw(m_cursorSmokeLifeOverride);
    }
    const auto updateUiEnd = profileTimePoint(renderProfileLoggingEnabled);

    const auto imguiRenderStart = profileTimePoint(renderProfileLoggingEnabled);
    // 冻结本帧 UI 并生成随后录入主 render pass 的顶点、索引和命令列表。
    ImGui::Render();
    const auto imguiRenderEnd = profileTimePoint(renderProfileLoggingEnabled);

    // 任务列表跨帧复用容量；每个 hook 把内部任务数量展开为稳定的 hook/index
    // 对。
    m_offscreenRecordTasks.clear();
    for ( auto& graphicUserHook : graphicUserHooks ) {
        if ( !graphicUserHook ) {
            continue;
        }

        // 数量在当前帧收集后视为固定，录制期间 hook 不得改变索引含义。
        const uint32_t taskCount =
            graphicUserHook->getOffscreenRecordTaskCount();
        for ( uint32_t taskIndex = 0; taskIndex < taskCount; ++taskIndex ) {
            m_offscreenRecordTasks.push_back({ graphicUserHook, taskIndex });
        }
    }

    const auto commandSetupStart =
        profileTimePoint(renderProfileLoggingEnabled);

    // fence 已确认该帧槽空闲，现在可重置并重新录制其主 command buffer。
    auto& currentCmdBuffer = m_vkCommandBuffers[m_currentFrameIndex];
    (void)currentCmdBuffer.reset();

    // 主命令缓冲每帧只提交一次，OneTimeSubmit 允许驱动优化内部跟踪。
    vk::CommandBufferBeginInfo commandBufferBeginInfo;
    commandBufferBeginInfo
        // 设置用法
        // 只提交一次,提交完就不用了
        .setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    // 复制创建信息使本帧 extent 在录制过程中稳定，不重复查询交换链成员。
    auto swapchainCreateInfo = m_vkSwapChain.info();
    // 清除值顺序与 render pass 附件契约一致：颜色在索引 0，深度模板在索引 1。
    std::array<vk::ClearValue, 2> clearValues;
    vk::ClearColorValue           clearColorValue(s_clear_color);
    clearValues[0].setColor(clearColorValue);
    clearValues[1].setDepthStencil({ 1.0f, 0 });

    // 只有线程池存在且任务多于一个时并行；单任务走主缓冲可避免调度与 latch
    // 成本。
    auto* offscreenRecordThreadPool =
        MMM::Runtime::AppThreadPool::instance().get();
    const bool useParallelOffscreenRecord =
        offscreenRecordThreadPool && m_offscreenRecordTasks.size() > 1;
    if ( useParallelOffscreenRecord ) {
        // 槽位只在任务数量达到新高水位时扩容，每个槽拥有独立 command pool。
        ensureOffscreenRecordSlots(m_offscreenRecordTasks.size());
    }

    // 主缓冲可与工作线程中的离屏缓冲并行 begin，因为它们不共享 command pool。
    (void)currentCmdBuffer.begin(commandBufferBeginInfo);
    const auto commandSetupEnd = profileTimePoint(renderProfileLoggingEnabled);

    // recordFrameIndex 同时选择主帧槽和每个离屏槽对应缓冲，生命周期覆盖 latch。
    const auto offscreenStart = profileTimePoint(renderProfileLoggingEnabled);
    const uint32_t recordFrameIndex =
        static_cast<uint32_t>(m_currentFrameIndex);
    if ( useParallelOffscreenRecord ) {
        // latch 计数与已展开任务一一对应，主线程在所有 command buffer end
        // 后继续。
        std::latch offscreenLatch(
            static_cast<std::ptrdiff_t>(m_offscreenRecordTasks.size()));
        for ( size_t taskSlot = 0; taskSlot < m_offscreenRecordTasks.size();
              ++taskSlot ) {
            // 任务描述与 command buffer 句柄按值捕获；只有 latch
            // 以引用跨线程共享。
            const OffscreenRecordTask task = m_offscreenRecordTasks[taskSlot];
            vk::CommandBuffer taskCmd = m_offscreenRecordSlots[taskSlot]
                                            .commandBuffers[recordFrameIndex];
            offscreenRecordThreadPool->enqueue_void(
                [task, taskCmd, recordFrameIndex, &offscreenLatch]() mutable {
                    // 每个工作任务独占槽位 pool/command
                    // buffer，无需在此加互斥锁。
                    (void)taskCmd.reset();
                    vk::CommandBufferBeginInfo taskBeginInfo;
                    taskBeginInfo.setFlags(
                        vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
                    (void)taskCmd.begin(taskBeginInfo);
                    if ( task.hook ) {
                        // hook 只能录制自己的离屏目标，不得开始主交换链 render
                        // pass。
                        task.hook->onRecordOffscreenTask(
                            taskCmd, recordFrameIndex, task.taskIndex);
                    }
                    (void)taskCmd.end();
                    // command buffer 完成 end
                    // 后才通知主线程，可立即纳入提交数组。
                    offscreenLatch.count_down();
                });
        }
        // 固定等待当前帧已提交的有限任务，不用超时窗口猜测命令是否录制完成。
        offscreenLatch.wait();
    } else {
        // 串行路径把离屏 pass 直接录入主缓冲，天然保持任务顺序且无需额外提交。
        for ( const auto& task : m_offscreenRecordTasks ) {
            if ( task.hook ) {
                task.hook->onRecordOffscreenTask(
                    currentCmdBuffer, recordFrameIndex, task.taskIndex);
            }
        }
    }
    const auto offscreenEnd = profileTimePoint(renderProfileLoggingEnabled);

    const auto mainRecordStart = profileTimePoint(renderProfileLoggingEnabled);
    {
        // 主 render area 始终覆盖交换链实际
        // extent，不使用可能已被钳制前的窗口请求值。
        vk::Rect2D renderArea;
        renderArea = { { 0,
                         0,
                         swapchainCreateInfo.imageExtent.width,
                         swapchainCreateInfo.imageExtent.height } };

        // 主 pass 绑定 acquire 返回图像对应的
        // framebuffer，并在开始时应用清除值。
        vk::RenderPassBeginInfo renderPassBeginInfo;
        renderPassBeginInfo.setRenderPass(m_vkRenderPass.getRenderPass())
            .setRenderArea(renderArea)
            .setFramebuffer(
                m_vkSwapChain.m_vkImageBuffers[imageIndex].vk_frameBuffer)
            .setClearValues(clearValues);

        // ImGui 作为主 pass
        // 当前唯一绘制内容写入交换链颜色附件；离屏任务已在此前
        // 完成录制，提交顺序保证其输出可供 UI 纹理采样。
        currentCmdBuffer.beginRenderPass(renderPassBeginInfo, {});
        {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                            currentCmdBuffer);
        }
        // endRenderPass 完成附件最终布局转换，随后结束整份主 command buffer。
        currentCmdBuffer.endRenderPass();
    }
    (void)currentCmdBuffer.end();
    const auto mainRecordEnd = profileTimePoint(renderProfileLoggingEnabled);

    // 提交数组跨帧复用容量。并行录制时先按任务顺序加入全部离屏缓冲，最后加入
    // 主缓冲；串行路径的离屏命令已经包含在主缓冲中。
    m_frameSubmitCommandBuffers.clear();
    if ( useParallelOffscreenRecord ) {
        for ( size_t taskSlot = 0; taskSlot < m_offscreenRecordTasks.size();
              ++taskSlot ) {
            m_frameSubmitCommandBuffers.push_back(
                m_offscreenRecordSlots[taskSlot]
                    .commandBuffers[recordFrameIndex]);
        }
    }
    // vector 只保存本帧借用句柄，queue submit 返回后仍可清空；命令资源由各自
    // pool 持有，并由 fence 约束下一次重置时机。
    m_frameSubmitCommandBuffers.push_back(currentCmdBuffer);

    // acquire semaphore 只需阻止交换链颜色附件写入；更早的离屏命令可先执行，
    // 从而与呈现图像等待重叠。数组长度与单个 wait semaphore 一致。
    vk::PipelineStageFlags waitStages[] = {
        vk::PipelineStageFlagBits::eColorAttachmentOutput
    };
    // 一次 queue submit 保留 command buffer 数组顺序，等待 acquire 后在目标图像
    // 写入，并以 imageIndex 对应 semaphore 通知 present。
    vk::SubmitInfo submitInfo;
    submitInfo.setCommandBuffers(m_frameSubmitCommandBuffers)
        .setWaitSemaphores(m_imageAvailableSems[m_currentFrameIndex])
        .setWaitDstStageMask(waitStages)
        .setSignalSemaphores(m_renderFinishedSems[imageIndex]);
    const auto submitStart = profileTimePoint(renderProfileLoggingEnabled);
    (void)m_LogicDeviceGraphicsQueue.submit(
        submitInfo, m_cmdAvailableFences[m_currentFrameIndex]);
    const auto submitEnd = profileTimePoint(renderProfileLoggingEnabled);

    // present 绑定本次 acquire 的同一交换链与图像索引，并等待渲染完成
    // semaphore。
    vk::PresentInfoKHR presentInfo;
    presentInfo.setImageIndices(imageIndex)
        .setSwapchains(m_vkSwapChain.m_swapchain)
        .setWaitSemaphores(m_renderFinishedSems[imageIndex]);

    const auto presentStart = profileTimePoint(renderProfileLoggingEnabled);
    vk::Result presentResult =
        m_LogicDevicePresentQueue.presentKHR(presentInfo);
    const auto presentEnd = profileTimePoint(renderProfileLoggingEnabled);

    if ( presentResult == vk::Result::eErrorOutOfDateKHR ||
         presentResult == vk::Result::eSuboptimalKHR ) {
        // 当前提交已经完成，延迟到下一帧入口按统一安全路径重建交换链。
        m_vkSwapChain.markDirty();
    } else if ( presentResult != vk::Result::eSuccess ) {
        XWARN("Present failed: {}", static_cast<int>(presentResult));
    }

    // 只有成功走到提交/呈现后的完整帧才轮转槽位；早退仍保留当前 fence 语义。
    ++m_currentFrameIndex %= MAX_FRAMES_IN_FLIGHT;

    // 平台窗口由 ImGui 后端拥有独立交换链；在主窗口提交后更新，激活边沿还会
    // 恢复整组原生窗口 Z 序。
    ImGuiIO&    io               = ImGui::GetIO();
    GLFWwindow* mainWindowHandle = window.getWindowHandle();
    const bool  mainWindowActivated =
        consumeMainWindowActivation(mainWindowHandle);
    const auto platformStart = profileTimePoint(renderProfileLoggingEnabled);
    if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) {
        // UpdatePlatformWindows
        // 先创建、移动或销毁原生视口，随后才能遍历有效句柄。
        ImGui::UpdatePlatformWindows();
        if ( mainWindowActivated ) {
            raiseImGuiViewportGroup(mainWindowHandle);
        }
#ifdef _WIN32
        // ImGui 可能动态新建平台窗口，每帧对现存 HWND 幂等应用任务栏分组、DWM
        // 阴影和圆角，保持浮动视口与主窗口一致。
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        for ( int i = 0; i < platform_io.Viewports.Size; ++i ) {
            ImGuiViewport* vp = platform_io.Viewports[i];
            if ( vp->PlatformHandle ) {
                // GLFW 后端把 PlatformHandle 暴露为 GLFWwindow，再桥接到原生
                // HWND。
                GLFWwindow* glfwWin =
                    static_cast<GLFWwindow*>(vp->PlatformHandle);
                HWND hwnd = glfwGetWin32Window(glfwWin);
                if ( hwnd ) {
                    // 关联主 HWND 使任务栏把浮动工具窗视为同一应用窗口组。
                    HWND mainHwnd = glfwGetWin32Window(mainWindowHandle);
                    Win32WindowAdapter::associateTaskbarGroupWindow(hwnd,
                                                                    mainHwnd);
                    // 一像素 client frame 让 DWM
                    // 生成原生外阴影，不侵占可见内容。
                    const MARGINS shadow_margin = { 1, 1, 1, 1 };
                    DwmExtendFrameIntoClientArea(hwnd, &shadow_margin);

                    // 显式请求系统圆角；旧系统会忽略未知属性而不改变渲染内容。
                    DWORD cornerPreference = DWMWCP_ROUND;
                    DwmSetWindowAttribute(hwnd,
                                          DWMWA_WINDOW_CORNER_PREFERENCE,
                                          &cornerPreference,
                                          sizeof(cornerPreference));
                }
            }
        }
#endif
        // 由 ImGui GLFW/Vulkan 后端分别录制并呈现所有非主视口。
        ImGui::RenderPlatformWindowsDefault();
    }
    const auto platformEnd = profileTimePoint(renderProfileLoggingEnabled);
    // 帧总耗时包含多视口平台更新；主交换链 present 与平台窗口 present 分列在
    // present、platformWindows 两个阶段，避免把二者混作同一阻塞点。
    const auto frameProfileEnd = profileTimePoint(renderProfileLoggingEnabled);

    if ( renderProfileLoggingEnabled ) {
        // 仅完整帧在此统一提交所有阶段耗时，保证每个 stat 分母相同。
        ++profile.frameCount;
        profile.total.add(
            elapsedMilliseconds(frameProfileStart, frameProfileEnd));
        profile.fence.add(elapsedMilliseconds(fenceStart, fenceEnd));
        profile.acquire.add(elapsedMilliseconds(acquireStart, acquireEnd));
        profile.pollEvents.add(elapsedMilliseconds(pollStart, pollEnd));
        profile.newFrame.add(elapsedMilliseconds(newFrameStart, newFrameEnd));
        profile.prepareResources.add(
            elapsedMilliseconds(prepareStart, prepareEnd));
        profile.updateUi.add(elapsedMilliseconds(updateUiStart, updateUiEnd));
        profile.imguiRender.add(
            elapsedMilliseconds(imguiRenderStart, imguiRenderEnd));
        profile.commandSetup.add(
            elapsedMilliseconds(commandSetupStart, commandSetupEnd));
        profile.offscreenRecord.add(
            elapsedMilliseconds(offscreenStart, offscreenEnd));
        profile.mainRecord.add(
            elapsedMilliseconds(mainRecordStart, mainRecordEnd));
        profile.submit.add(elapsedMilliseconds(submitStart, submitEnd));
        profile.present.add(elapsedMilliseconds(presentStart, presentEnd));
        profile.platformWindows.add(
            elapsedMilliseconds(platformStart, platformEnd));
        // 两秒窗口到期时才写日志；未到期只保留常量时间累加状态。
        profile.logIfReady(frameProfileEnd);
    }
}

}  // namespace MMM::Graphic
