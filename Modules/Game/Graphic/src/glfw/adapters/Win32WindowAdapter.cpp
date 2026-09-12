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

/// @brief HWND 属性名，用于标记独立 ImGui 视口所属的任务栏窗口组。
constexpr const wchar_t* TASKBAR_WINDOW_GROUP_PROP = L"MMMTaskbarWindowGroup";

/// @brief Win32 完成最小化到恢复态切换后用于应用最大化恢复的私有消息。
constexpr UINT APPLY_MAXIMIZED_RESTORE_MESSAGE = WM_APP + 0x0312;

/// @brief 用于清理短生命周期 Alt+Tab 恢复保护的私有消息。
constexpr UINT CLEAR_RESTORE_IGNORE_MESSAGE = WM_APP + 0x0313;

/// @brief 要求 Win32 最小化恢复时回到最大化状态的 WINDOWPLACEMENT 标志。
constexpr UINT RESTORE_TO_MAXIMIZED_FLAG = 0x0002;

/// @brief 判断 Win32 placement 是否直接表示当前窗口最大化。
///
/// WINDOWPLACEMENT 的 showCmd 能保留最小化前的展示意图，因此与 IsZoomed 的
/// 即时状态分开判断，供恢复流程组合使用。
///
/// @param placement Win32 窗口布局信息。
/// @return 当前 show command 是最大化时返回 true。
bool placementShowsMaximized(const WINDOWPLACEMENT& placement)
{
    return placement.showCmd == SW_SHOWMAXIMIZED;
}

/// @brief 清除 Win32 最小化后恢复最大化的系统 hint。
///
/// RESTORE_TO_MAXIMIZED_FLAG 是 WINDOWPLACEMENT 的系统恢复提示。只有查询成功且
/// 标志确实存在时才回写，避免无意义的 placement 更新触发额外窗口消息。
///
/// @param hWnd Win32 窗口句柄。
/// @warning 低频窗口状态路径：会同步调用
/// Get/SetWindowPlacement，不用于渲染循环。
void clearRestoreToMaximizedFlag(HWND hWnd)
{
    // 空句柄可能出现在销毁阶段，直接保持幂等返回。
    if ( !hWnd ) {
        return;
    }

    // Win32 要求调用者填写结构体长度，零初始化其余字段避免读取未定义内容。
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(WINDOWPLACEMENT);
    if ( !GetWindowPlacement(hWnd, &placement) ||
         (placement.flags & RESTORE_TO_MAXIMIZED_FLAG) == 0 ) {
        return;
    }

    // 只移除目标 bit，保留系统可能附加的其他 placement flags。
    placement.flags &= ~RESTORE_TO_MAXIMIZED_FLAG;
    SetWindowPlacement(hWnd, &placement);
}

/// @brief 将布尔值写入 HWND 属性。
///
/// 窗口属性用于跨同步 Win32 消息保存“最小化前应恢复最大化”的状态，不依赖 C++
/// 对象成员在消息嵌套期间的更新顺序。false 时删除属性而不是存储空值。
///
/// @param hWnd Win32 窗口句柄。
/// @param value 待写入状态。
void setRestoreMaximizedProperty(HWND hWnd, bool value)
{
    // 属性 API 只接受有效 HWND；销毁期间清理调用允许安全跳过。
    if ( !hWnd ) {
        return;
    }

    if ( value ) {
        // 属性值只充当存在性标志，非 owning HANDLE 值 1 不需要释放。
        SetPropW(hWnd, RESTORE_MAXIMIZED_PROP, reinterpret_cast<HANDLE>(1));
    } else {
        // RemovePropW 同时清除跨消息保存的恢复意图。
        RemovePropW(hWnd, RESTORE_MAXIMIZED_PROP);
    }
}

/// @brief 无激活地最小化属于指定任务栏窗口组的独立平台窗口。
///
/// EnumThreadWindows 会访问同一 UI 线程的所有顶层窗口；属性值必须精确匹配主
/// HWND 才视为 ImGui 平台窗口组成员。主窗口、自身已最小化或不可见窗口跳过。
///
/// @param window 当前枚举到的线程顶层窗口。
/// @param groupParam 任务栏窗口组主窗口句柄。
/// @return 固定返回 TRUE 以继续枚举。
BOOL CALLBACK minimizeTaskbarGroupMember(HWND window, LPARAM groupParam)
{
    // groupParam 由 minimizeTaskbarWindowGroup 传入，不拥有主窗口句柄。
    HWND mainWindow = reinterpret_cast<HWND>(groupParam);
    if ( window == mainWindow || !IsWindowVisible(window) ||
         IsIconic(window) ) {
        return TRUE;
    }

    // 只最小化主动登记的 viewport，避免影响同线程的工具窗或系统辅助窗口。
    HANDLE associatedGroup = GetPropW(window, TASKBAR_WINDOW_GROUP_PROP);
    if ( associatedGroup == reinterpret_cast<HANDLE>(mainWindow) ) {
        // 不激活目标可保持任务栏操作的焦点语义由主窗口控制。
        ShowWindow(window, SW_SHOWMINNOACTIVE);
    }
    return TRUE;
}

/// @brief 最小化与主窗口关联的全部独立平台窗口。
///
/// 关联通过 TASKBAR_WINDOW_GROUP_PROP 建立，枚举范围限制在主窗口所属 UI 线程，
/// 不扫描进程外窗口，也不改变未登记窗口状态。
///
/// @param mainWindow 任务栏窗口组主窗口句柄。
/// @warning 低频 Win32 消息路径：仅在主窗口收到最小化命令时枚举 UI 线程窗口。
void minimizeTaskbarWindowGroup(HWND mainWindow)
{
    // 窗口可能在 WM_DESTROY 附近已失效，空句柄不进入枚举。
    if ( !mainWindow ) {
        return;
    }

    // ImGui viewport 与主窗口在同一 GUI 线程创建，因此线程枚举足以覆盖组成员。
    const DWORD windowThread = GetWindowThreadProcessId(mainWindow, nullptr);
    if ( windowThread != 0 ) {
        EnumThreadWindows(windowThread,
                          minimizeTaskbarGroupMember,
                          reinterpret_cast<LPARAM>(mainWindow));
    }
}

}  // namespace

/// @brief 为 GLFW 主窗口安装 Win32 子类过程并配置无边框系统能力。
///
/// 构造取得 HWND、订阅 UI 拖拽区域、恢复可缩放/最小化/最大化 style，并向 DWM
/// 请求阴影与圆角。GLFWwindow 和 HWND 均由 NativeWindow 拥有，本适配器不销毁。
///
/// @param window 已创建且使用 Win32 backend 的 GLFW 主窗口。
/// @warning 窗口初始化低频路径：会修改 HWND style、class icon 和 DWM 属性，只能
/// 在窗口创建线程调用。
Win32WindowAdapter::Win32WindowAdapter(GLFWwindow* window) : m_window(window)
{
    // 原生 HWND 是后续 subclass 和 DWM 操作的稳定身份。
    m_hwnd = glfwGetWin32Window(m_window);
    // 保存构造时状态，为最小化消息可能缺失前序 WM_SIZE 的情况提供基线。
    m_lastKnownMaximized = IsZoomed(m_hwnd) != FALSE;

    // UI 布局通过事件发布标题栏允许区和控件排除区，供 WM_NCHITTEST 实时查询。
    Event::EventBus::instance().subscribe<Event::UpdateDragAreaEvent>(
        [this](Event::UpdateDragAreaEvent e) { this->onUpdateDragArea(e); });

    // 子类过程保留 GLFW 原始 WndProc，并通过 DefSubclassProc 继续默认消息链。
    SetWindowSubclass(m_hwnd, WindowProc, 0, (DWORD_PTR)this);

    // 即使视觉上无边框，也保留系统 resize/caption style 以启用 Aero Snap 和系统
    // 最大化、最小化行为；客户区尺寸由 WM_NCCALCSIZE 修正。
    LONG_PTR style = GetWindowLongPtr(m_hwnd, GWL_STYLE);
    style |= WS_THICKFRAME | WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    SetWindowLongPtr(m_hwnd, GWL_STYLE, style);
    // SWP_FRAMECHANGED 通知系统立即按新 style 重新计算 non-client frame。
    SetWindowPos(m_hwnd,
                 nullptr,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    // 修改 style 后重新把模块资源图标同时应用到实例与 class，保持任务栏、标题
    // 切换器和小图标一致。先尝试命名资源，再兼容数值 ID 1。
    HICON hIcon = LoadIcon(GetModuleHandle(nullptr), "IDI_ICON1");
    if ( !hIcon ) {
        hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
    }
    if ( hIcon ) {
        // LoadIcon 返回共享资源句柄，不由本适配器 DestroyIcon。
        SendMessage(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessage(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        SetClassLongPtr(m_hwnd, GCLP_HICON, (LONG_PTR)hIcon);
        SetClassLongPtr(m_hwnd, GCLP_HICONSM, (LONG_PTR)hIcon);
    }

    // 1px frame extension 让 DWM 为无边框窗口保留系统阴影合成。
    const MARGINS shadow_margin = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &shadow_margin);

    // 请求系统圆角；不自行绘制 mask，失败时保持平台默认外观。
    DWORD count = DWMWCP_ROUND;
    DwmSetWindowAttribute(
        m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &count, sizeof(count));

    XINFO("Win32WindowAdapter initialized");
}

/// @brief 清除 HWND 恢复属性并移除安装的窗口子类过程。
///
/// 销毁不负责 GLFWwindow/HWND，也不撤销系统 style；只断开会回调当前 this 的
/// subclass 关联，并删除临时最大化恢复标志。
Win32WindowAdapter::~Win32WindowAdapter()
{
    // HWND 可能已由宿主销毁，空句柄时不调用 Win32 属性或 subclass API。
    if ( m_hwnd ) {
        setRestoreMaximizedProperty(m_hwnd, false);
        RemoveWindowSubclass(m_hwnd, WindowProc, 0);
    }
}

/// @brief 表明 Win32 移动由 WM_NCHITTEST 的 HTCAPTION 结果驱动。
/// @return 固定返回 false，调用方应让系统 non-client 消息链处理。
bool Win32WindowAdapter::requestMove()
{
    return false;
}

/// @brief 表明 Win32 缩放由 WM_NCHITTEST 的 HT* 边角结果驱动。
/// @param edge 未使用；具体方向由 WindowProc 根据指针位置解析。
/// @return 固定返回 false，不从 GLFW 鼠标回调主动发起系统 resize。
bool Win32WindowAdapter::requestResize(WindowFrameResizeEdge edge)
{
    (void)edge;
    return false;
}

/// @brief 查询是否需要客户区主动发送移动/缩放请求。
/// @return Win32 固定返回 false，系统 hit-test 已提供原生交互。
bool Win32WindowAdapter::supportsClientFrameRequests() const
{
    return false;
}

/// @brief 查询 UI 是否需要额外绘制客户区 frame 覆盖层。
/// @return Win32 固定返回 false，阴影与边框外观交给 DWM。
bool Win32WindowAdapter::usesClientFrameOverlay() const
{
    return false;
}

/// @brief 重新向 DWM 应用无边框窗口圆角偏好。
///
/// 状态切换或 frame 重建后系统可能丢失视觉属性，调用方可在低频窗口事件后刷新。
///
/// @warning 低频窗口外观路径：同步调用 DwmSetWindowAttribute，不用于每帧绘制。
void Win32WindowAdapter::refreshFrameShape()
{
    // 销毁阶段或原生窗口尚未就绪时保持无操作。
    if ( !m_hwnd ) {
        return;
    }

    // DWM 不支持该属性时返回失败也不影响窗口基本交互，沿用系统默认边角。
    DWORD count = DWMWCP_ROUND;
    DwmSetWindowAttribute(
        m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &count, sizeof(count));
}

/// @brief 将独立 ImGui viewport 归入主窗口的 owner 与任务栏状态组。
///
/// Owner 关系保证主窗口最小化/激活语义，私有 HWND 属性供线程枚举时精准识别组
/// 成员。属性已经匹配时快速返回，避免渲染路径反复写 non-client frame。
///
/// @param window 要关联的独立平台窗口。
/// @param mainWindow 作为 owner 和任务栏组身份的主窗口。
/// @warning 渲染热路径可能重复调用；常态只做句柄/属性查询，只有归属变化时写入
/// owner、刷新 frame 并设置属性。
void Win32WindowAdapter::associateTaskbarGroupWindow(HWND window,
                                                     HWND mainWindow)
{
    // 主窗口本身不登记为自己的成员，任一空句柄也没有合法关联语义。
    if ( !window || !mainWindow || window == mainWindow ) {
        return;
    }

    // 属性值直接编码非 owning HWND 身份，只用于等值比较。
    HANDLE expectedGroup = reinterpret_cast<HANDLE>(mainWindow);
    if ( GetPropW(window, TASKBAR_WINDOW_GROUP_PROP) == expectedGroup ) {
        return;
    }

    if ( GetWindow(window, GW_OWNER) != mainWindow ) {
        // SetWindowLongPtr 返回零既可能是合法旧值，也可能失败，需配合 LastError
        // 判定。
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previousOwner = SetWindowLongPtrW(
            window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(mainWindow));
        const DWORD ownerError = GetLastError();
        if ( previousOwner == 0 && ownerError != ERROR_SUCCESS ) {
            XWARN("Failed to associate Win32 viewport owner: error={}",
                  ownerError);
            return;
        }

        // Owner 更新后用 FRAMECHANGED 刷新任务栏/non-client
        // 状态，但不激活或移动窗口。
        SetWindowPos(window,
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED);
    }

    // 属性写入失败只影响后续组最小化识别，不撤销已经成功建立的 owner 关系。
    if ( !SetPropW(window, TASKBAR_WINDOW_GROUP_PROP, expectedGroup) ) {
        XWARN("Failed to mark Win32 taskbar window group: error={}",
              GetLastError());
    }
}

/// @brief Win32 的 GLFW 鼠标按键回调不处理客户区 frame 请求。
/// @return 固定返回 false，让 UI 与原生 WindowProc 继续处理事件。
bool Win32WindowAdapter::handleClientMouseButton(int button, int action,
                                                 double cursorX, double cursorY)
{
    // 参数属于跨平台接口；Win32 方向判定集中在
    // WM_NCHITTEST，避免两套状态机竞争。
    (void)button;
    (void)action;
    (void)cursorX;
    (void)cursorY;
    return false;
}

/// @brief Win32 的 GLFW cursor 回调不推进客户区 frame 状态。
/// @return 固定返回 false，系统在 non-client 命中后自行跟踪拖拽。
bool Win32WindowAdapter::handleClientCursorPos(double cursorX, double cursorY)
{
    // 保留统一适配器接口，但不缓存指针坐标。
    (void)cursorX;
    (void)cursorY;
    return false;
}

/// @brief 用 UI 最新发布的标题栏允许区和控件排除区替换命中缓存。
/// @param e 同一布局快照中的 drag/blocked 矩形集合。
/// @warning UI 事件路径：复制 vector，事件应只在布局区域变化时发布。
void Win32WindowAdapter::onUpdateDragArea(const Event::UpdateDragAreaEvent& e)
{
    // WindowProc 始终先检查 blocked，再检查 drag，使按钮等交互控件优先。
    m_dragAreas        = e.areas;
    m_blockedDragAreas = e.blockedAreas;
}

/// @brief 结合即时缩放状态与 WINDOWPLACEMENT 判断最大化意图。
///
/// IsZoomed 描述当前可见状态；placement.showCmd 在最小化或过渡期间仍可能保留
/// SW_SHOWMAXIMIZED。两者取或可覆盖任务栏恢复的消息时序差异。
///
/// @param hWnd 要检查的窗口。
/// @return 当前状态或保存的展示命令表示最大化时返回 true。
bool Win32WindowAdapter::windowPlacementWantsMaximized(HWND hWnd) const
{
    // 空句柄没有可恢复状态。
    if ( !hWnd ) {
        return false;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(WINDOWPLACEMENT);
    if ( !GetWindowPlacement(hWnd, &placement) ) {
        // placement 查询失败时仍保留 IsZoomed 的即时判定作为保守后备。
        return IsZoomed(hWnd) != FALSE;
    }

    return IsZoomed(hWnd) != FALSE || placementShowsMaximized(placement);
}

/// @brief 在主窗口进入最小化前保存是否应恢复到最大化。
///
/// 状态同时写入成员与 HWND 属性，确保同步嵌套消息和延迟私有消息读取同一恢复
/// 意图；普通窗口则顺带清除可能残留的系统 restore-to-maximized hint。
///
/// @param hWnd 即将最小化的主窗口。
void Win32WindowAdapter::rememberRestoreStateBeforeMinimize(HWND hWnd)
{
    // 使用最近 WM_SIZE 状态和实时 placement 的并集，容忍不同 Windows 消息顺序。
    m_restoreMaximizedAfterMinimize =
        m_lastKnownMaximized || windowPlacementWantsMaximized(hWnd);
    if ( !m_restoreMaximizedAfterMinimize ) {
        // 普通窗口必须去掉系统 hint，否则任务栏恢复可能意外进入最大化。
        clearRestoreToMaximizedFlag(hWnd);
    }
    setRestoreMaximizedProperty(hWnd, m_restoreMaximizedAfterMinimize);
}

/// @brief 查询 HWND 上是否仍存在最大化恢复标记。
/// @param hWnd 待检查窗口。
/// @return 句柄有效且属性存在时返回 true。
bool Win32WindowAdapter::hasRestoreMaximizedProperty(HWND hWnd) const
{
    return hWnd && GetPropW(hWnd, RESTORE_MAXIMIZED_PROP) != nullptr;
}

/// @brief 在最小化结束后排队恢复原最大化状态。
///
/// 函数只确认恢复意图和当前窗口状态，不在当前系统消息栈中直接 ShowWindow。
/// 需要恢复时投递私有消息，让普通 restore 流程先完成，避免同步递归和中间尺寸
/// 覆盖最大化 placement。
///
/// @param hWnd 正在从最小化恢复的主窗口。
/// @warning Win32 消息路径：只投递非阻塞消息，不等待窗口状态变化。
void Win32WindowAdapter::restoreMaximizedAfterMinimize(HWND hWnd)
{
    // applyQueuedMaximizedRestore 内的同步 ShowWindow
    // 可能触发嵌套消息，递归时跳过。
    if ( !hWnd || m_applyingMaximizedRestore ) {
        return;
    }

    // HWND 属性是恢复意图的权威跨消息标记；缺失时同步清除成员镜像。
    if ( !hasRestoreMaximizedProperty(hWnd) ) {
        m_restoreMaximizedAfterMinimize = false;
        return;
    }

    // 在过渡阶段先维持“最后已知最大化”，防止随后 MINIMIZE 消息丢失原始状态。
    m_restoreMaximizedAfterMinimize = true;
    m_lastKnownMaximized            = true;
    if ( IsIconic(hWnd) ) {
        // 窗口仍最小化时等待后续 SIZE_RESTORED/ACTIVATE 再尝试，不提前
        // ShowWindow。
        return;
    }

    if ( IsZoomed(hWnd) ) {
        // 系统已正确恢复为最大化，消费属性并结束补偿流程。
        m_restoreMaximizedAfterMinimize = false;
        setRestoreMaximizedProperty(hWnd, false);
        return;
    }

    // 同一恢复周期最多保留一条私有消息，避免 WM_SIZE 与 WM_ACTIVATE 重复排队。
    if ( m_maximizedRestorePosted ) {
        return;
    }

    // PostMessage 异步返回，让当前系统 restore 消息先退出。
    m_maximizedRestorePosted = true;
    PostMessageW(hWnd, APPLY_MAXIMIZED_RESTORE_MESSAGE, 0, 0);
}

/// @brief 处理已排队的最大化恢复并消费对应 HWND 属性。
///
/// 私有消息到达时再次验证窗口、恢复标记、最小化和最大化状态；只有仍处于普通
/// 恢复态时才用 ShowWindow(SW_SHOWMAXIMIZED) 应用补偿。
///
/// @param hWnd 接收私有恢复消息的主窗口。
/// @warning Win32 消息路径：ShowWindow 可同步派生
/// WM_SIZE，使用成员重入标志保护。
void Win32WindowAdapter::applyQueuedMaximizedRestore(HWND hWnd)
{
    // 消息已经出队，先清 posted 标志，后续状态变化才允许再次排队。
    m_maximizedRestorePosted = false;
    if ( !hWnd || m_applyingMaximizedRestore ) {
        return;
    }

    // 用户或系统可能在消息排队期间取消恢复意图，届时不再强制最大化。
    if ( !hasRestoreMaximizedProperty(hWnd) ) {
        m_restoreMaximizedAfterMinimize = false;
        return;
    }

    if ( IsIconic(hWnd) ) {
        // 再次最小化时保留 HWND 属性，等待下一次真实恢复事件重新排队。
        return;
    }

    if ( IsZoomed(hWnd) ) {
        // 系统抢先完成最大化时只需清理状态，无需重复 ShowWindow。
        m_restoreMaximizedAfterMinimize = false;
        setRestoreMaximizedProperty(hWnd, false);
        return;
    }

    // 在同步 ShowWindow 期间保持恢复意图，嵌套 WM_SIZE 可识别此次最大化来源。
    m_restoreMaximizedAfterMinimize = true;
    m_lastKnownMaximized            = true;
    m_applyingMaximizedRestore      = true;
    ShowWindow(hWnd, SW_SHOWMAXIMIZED);
    m_applyingMaximizedRestore = false;

    // 仅在系统确认 IsZoomed 后消费标志；失败时保留状态供后续消息再次处理。
    if ( IsZoomed(hWnd) ) {
        m_restoreMaximizedAfterMinimize = false;
        setRestoreMaximizedProperty(hWnd, false);
    }
}

/// @brief 拦截主窗口 non-client、最小化恢复与任务栏组同步消息。
///
/// 子类过程优先处理两个私有恢复消息，再维护 WM_SYSCOMMAND/WM_SIZE/WM_ACTIVATE
/// 状态机；WM_NCCALCSIZE 修正最大化工作区，WM_NCHITTEST 把客户区边角和 UI
/// 标题栏区域映射为系统 hit-test。未消费的消息全部传给 DefSubclassProc。
///
/// @param hWnd 当前消息目标窗口。
/// @param uMsg Win32 消息编号。
/// @param wParam 消息相关整型参数。
/// @param lParam 消息相关指针或坐标参数。
/// @param uIdSubclass 安装子类过程时使用的 ID；当前实现固定为零。
/// @param dwRefData 安装时保存的 Win32WindowAdapter 指针。
/// @return 已处理消息的结果，或下一个 subclass/default 过程的返回值。
/// @warning 高频系统消息路径：WM_NCHITTEST 可随指针移动频繁触发，只允许小型区域
/// 缓存遍历；不得引入分配、文件访问或阻塞等待。
LRESULT CALLBACK Win32WindowAdapter::WindowProc(HWND hWnd, UINT uMsg,
                                                WPARAM wParam, LPARAM lParam,
                                                UINT_PTR  uIdSubclass,
                                                DWORD_PTR dwRefData)
{
    // dwRefData 生命周期由构造/析构中的 Set/RemoveWindowSubclass 包围。
    Win32WindowAdapter* adapter =
        reinterpret_cast<Win32WindowAdapter*>(dwRefData);

    if ( adapter ) {
        // 恢复状态机只访问当前适配器成员；没有 adapter 时仍可执行通用
        // hit-test。
        switch ( uMsg ) {
        case APPLY_MAXIMIZED_RESTORE_MESSAGE:
            // 延迟到系统普通 restore 之后再应用最大化。
            adapter->applyQueuedMaximizedRestore(hWnd);
            return 0;
        case CLEAR_RESTORE_IGNORE_MESSAGE:
            // 忽略标志只覆盖短暂消息窗口，私有消息到达即恢复正常 SC_RESTORE
            // 处理。
            adapter->m_ignoreNextRestoreSysCommand = false;
            adapter->m_restoreIgnoreClearPosted    = false;
            return 0;
        case WM_SYSCOMMAND:
            // Alt+Tab 恢复最大化窗口可能紧接一条多余
            // SC_RESTORE，只消费明确受保护
            // 且窗口仍最大化的那一次，避免把窗口降为普通状态。
            if ( (wParam & 0xFFF0) == SC_RESTORE &&
                 adapter->m_ignoreNextRestoreSysCommand && IsZoomed(hWnd) &&
                 !IsIconic(hWnd) ) {
                adapter->m_ignoreNextRestoreSysCommand = false;
                return 0;
            }
            if ( (wParam & 0xFFF0) == SC_MINIMIZE ) {
                // 在系统改变 placement 前保存主窗口意图，并同步最小化 viewport
                // 组。
                adapter->rememberRestoreStateBeforeMinimize(hWnd);
                minimizeTaskbarWindowGroup(hWnd);
            } else if ( (wParam & 0xFFF0) == SC_RESTORE ) {
                adapter->restoreMaximizedAfterMinimize(hWnd);
            }
            break;
        case WM_SIZE:
            if ( wParam == SIZE_MAXIMIZED ) {
                // 记录本次最大化是否来自最小化恢复，只有该来源才需要暂时忽略后续
                // 冗余 SC_RESTORE。
                const bool restoredMaximizedFromMinimize =
                    adapter->m_restoreMaximizedAfterMinimize ||
                    adapter->hasRestoreMaximizedProperty(hWnd);
                adapter->m_lastKnownMaximized            = true;
                adapter->m_restoreMaximizedAfterMinimize = false;
                setRestoreMaximizedProperty(hWnd, false);
                if ( restoredMaximizedFromMinimize ) {
                    adapter->m_ignoreNextRestoreSysCommand = true;
                    // 多个 WM_SIZE_MAXIMIZED
                    // 只排一条清理消息，缩短保护标志生命期。
                    if ( !adapter->m_restoreIgnoreClearPosted ) {
                        adapter->m_restoreIgnoreClearPosted = true;
                        PostMessageW(hWnd, CLEAR_RESTORE_IGNORE_MESSAGE, 0, 0);
                    }
                }
            } else if ( wParam == SIZE_MINIMIZED ) {
                // WM_SIZE 是对 WM_SYSCOMMAND
                // 路径的补充，保证程序化最小化也同步组。
                adapter->rememberRestoreStateBeforeMinimize(hWnd);
                minimizeTaskbarWindowGroup(hWnd);
            } else if ( wParam == SIZE_RESTORED ) {
                // 先尝试完成最大化补偿；确认为普通恢复后才更新最近状态并清系统
                // hint。
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
            // 任务栏/Alt+Tab 激活可能早于最终
            // WM_SIZE，获得焦点时再尝试一次补偿。
            if ( LOWORD(wParam) != WA_INACTIVE ) {
                adapter->restoreMaximizedAfterMinimize(hWnd);
            }
            break;
        default: break;
        }
    }

    // 无边框窗口仍保留 WS_CAPTION/WS_THICKFRAME，必须接管 non-client 计算才能让
    // 客户区覆盖视觉 frame；wParam=TRUE 时 lParam 才是 NCCALCSIZE_PARAMS。
    if ( uMsg == WM_NCCALCSIZE && wParam == TRUE ) {
        LPNCCALCSIZE_PARAMS params =
            reinterpret_cast<LPNCCALCSIZE_PARAMS>(lParam);

        // 最大化时 Windows 默认 rect 会向显示器外扩出边框宽度，改用最近监视器的
        // work area 可避开任务栏并消除负偏移。
        WINDOWPLACEMENT wp;
        wp.length = sizeof(WINDOWPLACEMENT);
        if ( GetWindowPlacement(hWnd, &wp) && wp.showCmd == SW_SHOWMAXIMIZED ) {
            HMONITOR hMonitor =
                MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            if ( GetMonitorInfo(hMonitor, &mi) ) {
                // 只覆盖建议的新客户区矩形，其余 NCCALCSIZE
                // 参数保持系统提供值。
                params->rgrc[0] = mi.rcWork;
            }
        }
        return 0;
    }

    // 返回系统 HT* 值让 Windows 自行实现 resize、Snap 和标题栏拖动，不在 GLFW
    // cursor 回调中维护另一套 Win32 交互状态。
    if ( uMsg == WM_NCHITTEST ) {
        // 消息携带屏幕坐标，UI drag areas 使用客户区坐标，先完成坐标转换。
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &pt);
        RECT rect;
        GetClientRect(hWnd, &rect);

        // 固定客户区边缘宽度优先于 UI 标题栏命中，保证四边与四角均可缩放。
        int  border = 8;
        bool left   = pt.x < border;
        bool right  = pt.x > rect.right - border;
        bool top    = pt.y < border;
        bool bottom = pt.y > rect.bottom - border;

        // 四角必须先于单边返回，否则同时满足 top/left 时会丢失对角缩放。
        if ( top && left ) return HTTOPLEFT;
        if ( top && right ) return HTTOPRIGHT;
        if ( bottom && left ) return HTBOTTOMLEFT;
        if ( bottom && right ) return HTBOTTOMRIGHT;
        if ( left ) return HTLEFT;
        if ( right ) return HTRIGHT;
        if ( top ) return HTTOP;
        if ( bottom ) return HTBOTTOM;

        // UI 上报的 blocked
        // 区域优先，用于从标题栏拖动范围扣除按钮和标签交互区。
        if ( adapter ) {
            for ( const auto& area : adapter->m_blockedDragAreas ) {
                // 忽略布局未完成或折叠控件产生的退化矩形。
                if ( area.w <= 0.0f || area.h <= 0.0f ) {
                    continue;
                }

                // 返回 HTCLIENT 让原始 UI 输入链继续接收鼠标事件。
                if ( pt.x >= area.x && pt.x <= (area.x + area.w) &&
                     pt.y >= area.y && pt.y <= (area.y + area.h) ) {
                    return HTCLIENT;
                }
            }

            // 只有未命中任何 blocked 区域时才扫描允许拖拽的标题栏片段。
            for ( const auto& area : adapter->m_dragAreas ) {
                if ( area.w <= 0.0f || area.h <= 0.0f ) {
                    continue;
                }

                // HTCAPTION 把后续拖动、双击最大化和 Snap 行为交给系统处理。
                if ( pt.x >= area.x && pt.x <= (area.x + area.w) &&
                     pt.y >= area.y && pt.y <= (area.y + area.h) ) {
                    return HTCAPTION;
                }
            }
        }
    }
    // 未消费消息继续沿 subclass 链传递，保留 GLFW 和系统默认窗口行为。
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

}  // namespace MMM::Graphic

#endif
