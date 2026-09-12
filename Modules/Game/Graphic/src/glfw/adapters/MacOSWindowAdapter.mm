#if defined(__APPLE__)

#include "graphic/glfw/window/adapters/MacOSWindowAdapter.h"
#include "event/ui/GLFWNativeEvent.h"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <objc/runtime.h>

namespace MMM::Graphic
{
namespace
{
/// @brief macOS 普通窗口的原生内容裁剪圆角半径。
constexpr CGFloat MACOS_WINDOW_CORNER_RADIUS = 12.0;

/// @brief macOS 无边框窗口缩放热区宽度。
constexpr double MACOS_FRAME_RESIZE_HIT_THICKNESS = 7.0;

/// @brief macOS 主窗口允许缩放到的最小宽度。
constexpr CGFloat MACOS_MIN_WINDOW_WIDTH = 640.0;

/// @brief macOS 主窗口允许缩放到的最小高度。
constexpr CGFloat MACOS_MIN_WINDOW_HEIGHT = 480.0;

/// @brief Cocoa 窗口上保存原始左键按下事件的关联键。
char MACOS_MOUSE_DOWN_EVENT_ASSOCIATION_KEY;

/// @brief 获取当前正在派发的左键按下事件。
///
/// AppKit 的 performWindowDragWithEvent: 要求原始 mouse-down NSEvent，不能仅凭 GLFW
/// 客户区坐标重新构造。函数只接受当前应用事件且必须属于目标 NSWindow。
///
/// @param nativeWindow 目标 Cocoa 窗口。
/// @return 属于目标窗口的左键按下事件；当前不是按下阶段时返回 nil。
NSEvent* currentLeftMouseDownEvent(NSWindow* nativeWindow)
{
    // GLFW 窗口尚未映射到 Cocoa 或已销毁时没有合法原生事件上下文。
    if ( !nativeWindow ) {
        return nil;
    }

    // currentEvent 可能是移动、按键或其他窗口事件，必须同时验证 window 和 type。
    NSEvent* event = [NSApp currentEvent];
    if ( !event || [event window] != nativeWindow ||
         [event type] != NSEventTypeLeftMouseDown ) {
        return nil;
    }
    // 返回值由 NSApplication 管理，本函数不转移所有权。
    return event;
}

/// @brief 缓存供 AppKit 原生窗口拖动使用的初始左键事件。
///
/// GLFW 回调到达时 AppKit currentEvent 可能在随后恢复最大化流程中变化，因此将
/// 原始 NSEvent 关联到 NSWindow。非空事件使用 nonatomic retain，清理时改为 assign
/// nil，不跨线程共享。
///
/// @param nativeWindow 目标 Cocoa 窗口。
/// @param event 原始左键按下事件；传入 nil 时清空缓存。
void cacheMouseDownEvent(NSWindow* nativeWindow, NSEvent* event)
{
    // 关联对象只能挂在有效 Objective-C 实例上，窗口销毁阶段允许安全跳过。
    if ( !nativeWindow ) {
        return;
    }

    // 静态键地址在进程内唯一，避免与 GLFW 或其他窗口扩展的关联键碰撞。
    objc_setAssociatedObject(nativeWindow,
                             &MACOS_MOUSE_DOWN_EVENT_ASSOCIATION_KEY,
                             event,
                             event ? OBJC_ASSOCIATION_RETAIN_NONATOMIC
                                   : OBJC_ASSOCIATION_ASSIGN);
}

/// @brief 对承载窗口画面的 Cocoa 视图层应用圆角裁剪。
///
/// contentView 与其 superview 都可能参与最终窗口合成，因此调用者会对两层应用
/// 相同半径和 masksToBounds。最大化/全屏时保留 layer，但关闭裁剪。
///
/// @param view 需要裁剪的视图。
/// @param cornerRadius 当前窗口状态对应的圆角半径。
/// @param shouldClip 普通窗口状态下为 true。
/// @warning 低频窗口外观路径：会启用 backing layer 并修改 CALayer 属性。
void applyRoundedContentLayer(NSView* view, CGFloat cornerRadius,
                              bool shouldClip)
{
    // superview 在部分 AppKit/GLFW 组合下可能不存在，空视图直接返回。
    if ( !view ) {
        return;
    }

    // wantsLayer 确保 cornerRadius 和 mask 属性有实际 CALayer 承载。
    [view setWantsLayer:YES];
    CALayer* layer = [view layer];
    if ( !layer ) {
        return;
    }

    // 半径与裁剪开关分开设置，恢复普通窗口时无需重新创建 layer。
    [layer setCornerRadius:cornerRadius];
    [layer setMasksToBounds:shouldClip ? YES : NO];
    [layer setAllowsEdgeAntialiasing:YES];
}
}  // namespace

/// @brief 绑定 GLFW/Cocoa 主窗口、订阅拖拽区域并安装原生拖动桥接。
///
/// host 和 GLFWwindow 为非 owning 引用，生命周期由 NativeWindow 覆盖适配器。
/// 析构时必须先清原生关联事件，再注销 EventBus 订阅。
///
/// @param host 提供主窗口句柄、最大化状态和拖动恢复能力的宿主。
MacOSWindowAdapter::MacOSWindowAdapter(IWindowFrameHost& host)
    : m_host(host), m_window(host.getFrameWindowHandle())
{
    // UI 布局事件只更新本地命中缓存，不在发布回调中直接调用 AppKit。
    m_dragAreaSubscription =
        Event::EventBus::instance().subscribe<Event::UpdateDragAreaEvent>(
            [this](const Event::UpdateDragAreaEvent& event) {
                this->onUpdateDragArea(event);
            });
    // 窗口已创建后配置 AppKit movable 属性和初始阴影/圆角。
    installNativeDragBridge();
}

/// @brief 移除 Cocoa 关联事件并注销拖拽区域订阅。
///
/// NSWindow 由 GLFW 管理，本适配器只撤销自身桥接状态，不关闭原生窗口。
MacOSWindowAdapter::~MacOSWindowAdapter()
{
    // 先清除 retained NSEvent，避免订阅注销期间仍持有一次旧鼠标事件。
    removeNativeDragBridge();
    if ( m_dragAreaSubscription != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::UpdateDragAreaEvent>(
            m_dragAreaSubscription);
    }
}

/// @brief 使用原始 AppKit mouse-down 事件启动系统窗口拖动。
///
/// 优先读取当前正在派发的事件；最大化恢复导致 currentEvent 变化时，回退到按键
/// 回调缓存的关联事件。全屏和最大化窗口必须先由其他路径恢复为普通窗口。
///
/// @return 已调用 performWindowDragWithEvent: 并消费缓存事件时返回 true。
/// @warning 输入回调路径：会进入 AppKit 原生窗口拖动处理，不执行固定等待或文件
/// 访问。
bool MacOSWindowAdapter::requestMove()
{
    // fullscreen 与最大化窗口不能直接按普通 frame 交给 AppKit 拖动。
    if ( !m_window || glfwGetWindowMonitor(m_window) != nullptr ||
         m_host.isFrameMaximized() ) {
        return false;
    }

    // Cocoa 句柄由 GLFW 拥有，只在本次调用内借用。
    NSWindow* nativeWindow = glfwGetCocoaWindow(m_window);
    if ( !nativeWindow ) {
        return false;
    }

    // 普通 press 路径通常能直接取得 currentEvent；恢复最大化路径使用关联缓存。
    NSEvent* mouseDownEvent = currentLeftMouseDownEvent(nativeWindow);
    if ( !mouseDownEvent ) {
        mouseDownEvent = objc_getAssociatedObject(
            nativeWindow, &MACOS_MOUSE_DOWN_EVENT_ASSOCIATION_KEY);
    }
    // 缓存仍需重新验证所属窗口和事件类型，防止沿用过期或其他窗口的事件。
    if ( !mouseDownEvent || [mouseDownEvent window] != nativeWindow ||
         [mouseDownEvent type] != NSEventTypeLeftMouseDown ) {
        return false;
    }

    // Window Server 接管后立即清缓存，使同一 press 不会重复启动拖动。
    [nativeWindow performWindowDragWithEvent:mouseDownEvent];
    cacheMouseDownEvent(nativeWindow, nil);
    return true;
}

/// @brief 捕获窗口 frame 与全局指针位置，开始客户端驱动的边角缩放。
///
/// AppKit 没有与 performWindowDragWithEvent: 对应的通用无边框 resize 入口，因此
/// 记录固定起点，后续 cursor 回调通过 updateActiveResize 计算绝对目标 frame。
///
/// @param edge 本次缩放锁定的边或角。
/// @return 起始窗口状态有效并成功进入 resizeActive 时返回 true。
/// @warning 输入回调路径：只查询 AppKit 状态并写入固定大小成员，不执行分配。
bool MacOSWindowAdapter::requestResize(WindowFrameResizeEdge edge)
{
    // fullscreen 或最大化窗口的 frame 由系统模式控制，不启动客户区 resize。
    if ( !m_window || glfwGetWindowMonitor(m_window) != nullptr ||
         m_host.isFrameMaximized() ) {
        return false;
    }

    NSWindow* nativeWindow = glfwGetCocoaWindow(m_window);
    if ( !nativeWindow ) {
        return false;
    }

    // frame 与 mouseLocation 都使用屏幕坐标，后续 delta 无需客户区坐标转换。
    const NSRect frame = [nativeWindow frame];
    if ( frame.size.width <= 0.0 || frame.size.height <= 0.0 ) {
        return false;
    }

    const NSPoint mouseLocation = [NSEvent mouseLocation];
    // Resize 与 pending move 互斥，新边缘 press 立即取消可能存在的标题栏候选。
    m_pendingMove               = false;
    m_resizeActive              = true;
    m_resizeEdge                = edge;
    m_resizeStartMouseX         = mouseLocation.x;
    m_resizeStartMouseY         = mouseLocation.y;
    m_resizeStartFrameX         = frame.origin.x;
    m_resizeStartFrameY         = frame.origin.y;
    m_resizeStartFrameWidth     = frame.size.width;
    // 固定保存起始 frame，所有后续更新都相对它计算，避免增量舍入误差累积。
    m_resizeStartFrameHeight    = frame.size.height;
    return true;
}

/// @brief 查询 macOS 是否具备客户区 frame 交互所需的主窗口。
/// @return GLFW 主窗口句柄有效时返回 true。
/// @warning UI 查询热路径：仅检查非 owning 指针，不访问 AppKit 对象。
bool MacOSWindowAdapter::supportsClientFrameRequests() const
{
    return m_window != nullptr;
}

/// @brief 查询 UI 是否需要额外绘制 frame 覆盖层。
/// @return macOS 固定返回 false，圆角和阴影由 AppKit/Core Animation 提供。
bool MacOSWindowAdapter::usesClientFrameOverlay() const
{
    return false;
}

/// @brief 按普通、最大化或全屏状态刷新透明背景、阴影和内容圆角。
///
/// 普通窗口启用原生阴影并同时裁剪 contentView 与其 superview；最大化/全屏状态
/// 关闭阴影和 mask，防止屏幕边缘出现透明圆角。
///
/// @warning 低频窗口状态路径：修改 NSWindow/CALayer 属性并使阴影失效，只应在
/// frame 状态变化后调用。
void MacOSWindowAdapter::refreshFrameShape()
{
    // GLFW 可能尚未创建 Cocoa 窗口或正在销毁，nil 时保持幂等退出。
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    if ( !nativeWindow ) {
        return;
    }

    // 系统模式窗口不应用客户区圆角/阴影，普通无边框窗口才启用视觉效果。
    const bool useWindowEffects =
        glfwGetWindowMonitor(m_window) == nullptr && !m_host.isFrameMaximized();
    NSView* contentView = [nativeWindow contentView];

    // 透明 NSWindow 背景让裁剪后的圆角区域显示桌面合成结果。
    [nativeWindow setOpaque:NO];
    [nativeWindow setBackgroundColor:[NSColor clearColor]];
    [nativeWindow setHasShadow:useWindowEffects ? YES : NO];

    if ( contentView ) {
        // 两层采用同一半径，避免 GLFW 包装 view 在 contentView 外保留矩形背景。
        const CGFloat cornerRadius =
            useWindowEffects ? MACOS_WINDOW_CORNER_RADIUS : 0.0;
        applyRoundedContentLayer(
            contentView, cornerRadius, useWindowEffects);
        applyRoundedContentLayer(
            [contentView superview], cornerRadius, useWindowEffects);
    }

    // 属性变化后请求 AppKit 重新计算当前窗口阴影轮廓。
    [nativeWindow invalidateShadow];
}

/// @brief 处理左键按下/释放，选择边缘缩放、标题栏拖动或双击最大化。
///
/// resize hit zone 优先于 drag area；标题栏双击发布统一最大化事件。普通窗口直接
/// 交给 AppKit 拖动，最大化窗口则保存 press 和原生事件，等待移动超过阈值后先
/// 恢复窗口再继续拖动。
///
/// @param button GLFW 鼠标按键编号。
/// @param action GLFW_PRESS 或 GLFW_RELEASE。
/// @param cursorX 事件发生时的客户区 X 坐标。
/// @param cursorY 事件发生时的客户区 Y 坐标。
/// @return 已启动 frame 行为或建立最大化拖动候选时返回 true。
/// @warning 输入回调热路径：只遍历缓存命中区域、发布单个事件或进入 AppKit 拖动；
/// 禁止文件访问和固定时长等待。
bool MacOSWindowAdapter::handleClientMouseButton(int button, int action,
                                                 double cursorX, double cursorY)
{
    // 非左键不参与窗口 frame 交互，继续交给 UI。
    if ( button != GLFW_MOUSE_BUTTON_LEFT ) {
        return false;
    }

    if ( action == GLFW_RELEASE ) {
        // 释放结束客户端 resize/pending move，并清除可能 retained 的 NSEvent。
        resetPendingFrameRequest();
        return false;
    }

    // 只从普通窗口的真实 press 建立状态；fullscreen 由系统模式管理。
    if ( action != GLFW_PRESS || !m_window ||
         glfwGetWindowMonitor(m_window) != nullptr ) {
        return false;
    }

    // 边缘和角落必须优先于标题栏区域，否则顶部 drag area 会遮蔽 resize。
    if ( auto edge = resolveResizeEdge(cursorX, cursorY) ) {
        return requestResize(*edge);
    }

    // 普通客户区点击不应被适配器消费。
    if ( !isInsideDragArea(cursorX, cursorY) ) {
        return false;
    }

    // 当前 AppKit mouse-down 事件是 performWindowDragWithEvent: 所需的原始输入。
    NSWindow* nativeWindow = glfwGetCocoaWindow(m_window);
    NSEvent*  mouseDownEvent = currentLeftMouseDownEvent(nativeWindow);
    if ( mouseDownEvent ) {
        // 先缓存，保证最大化恢复改变 currentEvent 后 requestMove 仍可取回。
        cacheMouseDownEvent(nativeWindow, mouseDownEvent);
    }

    if ( mouseDownEvent && [mouseDownEvent clickCount] >= 2 ) {
        // 双击使用跨平台原生事件入口，窗口状态切换仍由宿主统一执行。
        resetPendingFrameRequest();
        Event::EventBus::instance().publish(Event::GLFWNativeEvent{
            .type =
                Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE });
        return true;
    }

    // 普通窗口无需等待 cursor move，直接使用当前按下事件启动系统拖动。
    if ( !m_host.isFrameMaximized() ) {
        return requestMove();
    }

    // 最大化窗口需先区分点击与拖动，超过阈值后由宿主恢复到普通 placement。
    m_pressX = cursorX;
    m_pressY = cursorY;
    resetPendingFrameRequest();
    cacheMouseDownEvent(nativeWindow, mouseDownEvent);
    // reset 会清缓存，因此在其后重新关联本次原生事件。
    m_pendingMove = true;
    return true;
}

/// @brief 在左键移动期间推进活动 resize 或最大化窗口拖动恢复。
///
/// 活动 resize 每次按全局鼠标位置更新绝对 frame。最大化拖动候选超过 4px 阈值后
/// 先让宿主按 cursor 比例恢复普通窗口，再用缓存的原生 mouse-down 事件启动 AppKit
/// 拖动。
///
/// @param cursorX 当前客户区 X 坐标。
/// @param cursorY 当前客户区 Y 坐标。
/// @return 本次更新提交了 resize 或启动了原生 move 时返回 true。
/// @warning 高频输入路径：鼠标移动期间连续调用，只允许常数时间几何计算和单次
/// AppKit frame 更新，不得分配或阻塞等待。
bool MacOSWindowAdapter::handleClientCursorPos(double cursorX, double cursorY)
{
    // 窗口失效或左键已释放时取消所有候选，防止旧 NSEvent 跨交互复用。
    if ( !m_window ||
         glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS ) {
        resetPendingFrameRequest();
        return false;
    }

    // Resize 与 move 互斥，一旦激活就由专用几何更新路径接管。
    if ( m_resizeActive ) {
        return updateActiveResize();
    }

    // pendingMove 只用于最大化恢复；普通窗口已在 mouse-down 阶段直接 requestMove。
    if ( !m_pendingMove || !m_host.isFrameMaximized() ) {
        return false;
    }

    const double dx        = cursorX - m_pressX;
    const double dy        = cursorY - m_pressY;
    // 使用平方距离避免每个 cursor event 求平方根，小幅移动仍视为标题栏点击。
    constexpr double threshold = 4.0;
    if ( dx * dx + dy * dy < threshold * threshold ) {
        return false;
    }

    // 只尝试一次恢复，失败时 reset 清理事件，成功后 requestMove 消费缓存。
    m_pendingMove = false;
    if ( !m_host.restoreFrameForClientMove(cursorX, cursorY) ) {
        resetPendingFrameRequest();
        return false;
    }
    return requestMove();
}

/// @brief 焦点切换时取消未完成的客户区 frame 交互。
/// @param focused 当前焦点状态；无论获得或失去焦点都不沿用旧候选。
/// @warning 低频窗口事件路径：只清本地状态和关联 NSEvent。
void MacOSWindowAdapter::handleClientFocusChange(bool focused)
{
    // 跨焦点边界后鼠标按键和原生事件时序不再可靠，统一复位。
    (void)focused;
    resetPendingFrameRequest();
}

/// @brief 用 UI 最新布局快照替换标题栏允许区与控件排除区。
/// @param event 包含客户区逻辑坐标下的 drag/blocked 区域。
/// @warning UI 事件路径：复制 vector，发布频率应限制在布局实际变化时。
void MacOSWindowAdapter::onUpdateDragArea(
    const Event::UpdateDragAreaEvent& event)
{
    // 两组区域来自同一事件，命中函数保证 blocked 优先级高于 drag。
    m_dragAreas        = event.areas;
    m_blockedDragAreas = event.blockedAreas;
}

/// @brief 配置 Cocoa 窗口由显式事件拖动并应用初始 frame 外观。
///
/// 窗口保持 movable，但关闭 background 自动拖动，避免整个透明客户区都成为标题栏；
/// 真正拖动只由 requestMove 使用已验证 NSEvent 发起。
///
/// @warning 窗口初始化低频路径：修改 NSWindow 属性并刷新 Core Animation 外观。
void MacOSWindowAdapter::installNativeDragBridge()
{
    // GLFW 可能尚未提供 Cocoa window，nil 时不安装部分状态。
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    if ( !nativeWindow ) {
        return;
    }

    // movable 是 performWindowDragWithEvent: 的前提，background 拖动则必须关闭。
    [nativeWindow setMovable:YES];
    [nativeWindow setMovableByWindowBackground:NO];
    refreshFrameShape();
}

/// @brief 清除 Cocoa 窗口上为拖动暂存的原始 mouse-down 事件。
///
/// 适配器没有安装额外 Objective-C method swizzle 或 observer，因此拆除只需释放
/// 关联事件；NSWindow 的 movable 配置可由宿主窗口生命周期自然结束。
void MacOSWindowAdapter::removeNativeDragBridge()
{
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    if ( !nativeWindow ) {
        return;
    }

    // nil 会移除 retained 关联对象，防止事件生命周期超过适配器。
    cacheMouseDownEvent(nativeWindow, nil);
}

/// @brief 将客户区指针坐标解析为八向无边框 resize 边角。
///
/// 固定 7px 热区覆盖四边，角落组合优先于单边；最大化窗口禁用客户区 resize。
///
/// @param cursorX 当前客户区 X 坐标。
/// @param cursorY 当前客户区 Y 坐标。
/// @return 命中的缩放方向；窗口/尺寸无效或位于内部时返回 std::nullopt。
/// @warning 高频输入路径：仅常数时间 GLFW 查询和算术，不调用 AppKit frame 更新。
std::optional<WindowFrameResizeEdge> MacOSWindowAdapter::resolveResizeEdge(
    double cursorX, double cursorY) const
{
    // 最大化 frame 由宿主控制，边缘 press 应保留给系统或普通 UI。
    if ( !m_window || m_host.isFrameMaximized() ) {
        return std::nullopt;
    }

    // GLFW 窗口尺寸与 cursor 坐标共享客户区坐标系，可直接比较。
    int width  = 0;
    int height = 0;
    glfwGetWindowSize(m_window, &width, &height);
    if ( width <= 0 || height <= 0 ) {
        return std::nullopt;
    }

    // <=/>= 包含边界点，并容忍刚进入窗口时轻微超出客户区的坐标。
    const bool left = cursorX <= MACOS_FRAME_RESIZE_HIT_THICKNESS;
    const bool right =
        cursorX >= static_cast<double>(width) -
                       MACOS_FRAME_RESIZE_HIT_THICKNESS;
    const bool top = cursorY <= MACOS_FRAME_RESIZE_HIT_THICKNESS;
    const bool bottom =
        cursorY >= static_cast<double>(height) -
                       MACOS_FRAME_RESIZE_HIT_THICKNESS;

    // 四角先返回，避免同时满足两条边时退化为单轴缩放。
    if ( top && left ) return WindowFrameResizeEdge::TopLeft;
    if ( top && right ) return WindowFrameResizeEdge::TopRight;
    if ( bottom && left ) return WindowFrameResizeEdge::BottomLeft;
    if ( bottom && right ) return WindowFrameResizeEdge::BottomRight;
    if ( left ) return WindowFrameResizeEdge::Left;
    if ( right ) return WindowFrameResizeEdge::Right;
    if ( top ) return WindowFrameResizeEdge::Top;
    if ( bottom ) return WindowFrameResizeEdge::Bottom;
    // 内部区域随后可继续参与标题栏 drag area 命中。
    return std::nullopt;
}

/// @brief 根据缩放起点与当前全局鼠标位置提交新的 Cocoa window frame。
///
/// 尺寸以 requestResize 捕获的初始 frame 为基准，按活动边角独立更新宽高并钳制到
/// 项目最小尺寸和 AppKit min/max size。拖动左边或底边时同步移动 origin，保持对侧
/// 边缘固定。
///
/// @return 活动状态和原生窗口有效并提交 frame 时返回 true。
/// @warning 高频输入路径：每个 resize cursor event 调用一次 setFrame，禁止在此
/// 引入动画、等待或增量状态分配。
bool MacOSWindowAdapter::updateActiveResize()
{
    // resizeActive 是唯一授权修改 frame 的状态，窗口失效时保持当前成员供 reset 清理。
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    if ( !m_resizeActive || !nativeWindow ) {
        return false;
    }

    // NSEvent mouseLocation 与 NSWindow frame 都是屏幕坐标，macOS 原点位于左下。
    const NSPoint mouseLocation = [NSEvent mouseLocation];
    const CGFloat deltaX =
        static_cast<CGFloat>(mouseLocation.x - m_resizeStartMouseX);
    const CGFloat deltaY =
        static_cast<CGFloat>(mouseLocation.y - m_resizeStartMouseY);
    // 缓存成员使用 double 维持跨接口精度，在 Cocoa 运算边界转换为 CGFloat。
    const CGFloat startX = static_cast<CGFloat>(m_resizeStartFrameX);
    const CGFloat startY = static_cast<CGFloat>(m_resizeStartFrameY);
    const CGFloat startWidth =
        static_cast<CGFloat>(m_resizeStartFrameWidth);
    const CGFloat startHeight =
        static_cast<CGFloat>(m_resizeStartFrameHeight);

    // 项目最低尺寸与 AppKit 原生约束取更严格者，最大值至少容纳起始尺寸和最小值。
    const NSSize nativeMinSize = [nativeWindow minSize];
    const NSSize nativeMaxSize = [nativeWindow maxSize];
    const CGFloat minWidth =
        std::max(MACOS_MIN_WINDOW_WIDTH, nativeMinSize.width);
    const CGFloat minHeight =
        std::max(MACOS_MIN_WINDOW_HEIGHT, nativeMinSize.height);
    const CGFloat maxWidth =
        std::max({ minWidth, startWidth, nativeMaxSize.width });
    const CGFloat maxHeight =
        std::max({ minHeight, startHeight, nativeMaxSize.height });

    // 将八向枚举拆为四个正交边标志，角落自然同时更新两个轴。
    const bool resizeLeft =
        m_resizeEdge == WindowFrameResizeEdge::Left ||
        m_resizeEdge == WindowFrameResizeEdge::TopLeft ||
        m_resizeEdge == WindowFrameResizeEdge::BottomLeft;
    const bool resizeRight =
        m_resizeEdge == WindowFrameResizeEdge::Right ||
        m_resizeEdge == WindowFrameResizeEdge::TopRight ||
        m_resizeEdge == WindowFrameResizeEdge::BottomRight;
    const bool resizeTop =
        m_resizeEdge == WindowFrameResizeEdge::Top ||
        m_resizeEdge == WindowFrameResizeEdge::TopLeft ||
        m_resizeEdge == WindowFrameResizeEdge::TopRight;
    const bool resizeBottom =
        m_resizeEdge == WindowFrameResizeEdge::Bottom ||
        m_resizeEdge == WindowFrameResizeEdge::BottomLeft ||
        m_resizeEdge == WindowFrameResizeEdge::BottomRight;

    // 左边向右移动会减小宽度，右边向右移动会增大宽度。
    CGFloat targetWidth = startWidth;
    if ( resizeLeft ) {
        targetWidth =
            std::clamp(startWidth - deltaX, minWidth, maxWidth);
    } else if ( resizeRight ) {
        targetWidth =
            std::clamp(startWidth + deltaX, minWidth, maxWidth);
    }

    // Cocoa Y 轴向上：底边上移减小高度，顶边上移增大高度。
    CGFloat targetHeight = startHeight;
    if ( resizeBottom ) {
        targetHeight =
            std::clamp(startHeight - deltaY, minHeight, maxHeight);
    } else if ( resizeTop ) {
        targetHeight =
            std::clamp(startHeight + deltaY, minHeight, maxHeight);
    }

    // 从左/底缩放时根据钳制后的目标尺寸回算 origin，使相反边保持起始位置。
    NSRect targetFrame = NSMakeRect(
        resizeLeft ? startX + startWidth - targetWidth : startX,
        resizeBottom ? startY + startHeight - targetHeight : startY,
        targetWidth,
        targetHeight);
    // 禁用 display/animation，让连续 cursor 更新立即改变窗口几何且不积压动画。
    [nativeWindow setFrame:targetFrame display:NO animate:NO];
    return true;
}

/// @brief 判断客户区坐标是否命中允许拖动且未被控件阻挡的标题栏区域。
///
/// blocked 区域先于 drag 区域检查，用于从大标题栏矩形扣除按钮、标签等交互控件。
/// 非正尺寸区域忽略，边界点按闭区间命中。
///
/// @param cursorX 当前客户区 X 坐标。
/// @param cursorY 当前客户区 Y 坐标。
/// @return 命中有效 drag 区且未命中 blocked 区时返回 true。
/// @warning 输入热路径：只遍历 UI 缓存的小型矩形集合，不重新计算布局。
bool MacOSWindowAdapter::isInsideDragArea(double cursorX, double cursorY) const
{
    // 排除区拥有最高优先级，允许任何控件覆盖底层标题栏拖动面。
    for ( const auto& area : m_blockedDragAreas ) {
        // 布局尚未完成或隐藏控件可能产生退化矩形，不能参与命中。
        if ( area.w <= 0.0f || area.h <= 0.0f ) {
            continue;
        }

        // 区域坐标为 float，提升为 double 与 GLFW cursor 精度一致。
        if ( cursorX >= static_cast<double>(area.x) &&
             cursorX <= static_cast<double>(area.x + area.w) &&
             cursorY >= static_cast<double>(area.y) &&
             cursorY <= static_cast<double>(area.y + area.h) ) {
            return false;
        }
    }

    // 只有未命中 blocked 的点才扫描一个或多个允许标题栏片段。
    for ( const auto& area : m_dragAreas ) {
        if ( area.w <= 0.0f || area.h <= 0.0f ) {
            continue;
        }

        // 任一允许矩形命中即可启动标题栏逻辑，不要求区域彼此连续。
        if ( cursorX >= static_cast<double>(area.x) &&
             cursorX <= static_cast<double>(area.x + area.w) &&
             cursorY >= static_cast<double>(area.y) &&
             cursorY <= static_cast<double>(area.y + area.h) ) {
            return true;
        }
    }
    // 未收到布局快照或所有矩形无效均保持普通客户区行为。
    return false;
}

/// @brief 结束待移动/活动缩放并释放关联的原始 AppKit mouse-down 事件。
///
/// 按下坐标、起始 frame 和 resizeEdge 可保留旧值，因为只有两个活动标志控制读取；
/// 每次 release、失焦或失败路径调用都保持幂等。
///
/// @warning 输入回调路径：仅写入布尔状态和清理一个 Objective-C 关联对象。
void MacOSWindowAdapter::resetPendingFrameRequest()
{
    // Move 与 resize 共享同一左键生命周期，统一结束防止状态交叉。
    m_pendingMove  = false;
    m_resizeActive = false;
    // 窗口已销毁时 cacheMouseDownEvent 会安全忽略 nil。
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    cacheMouseDownEvent(nativeWindow, nil);
}

}  // namespace MMM::Graphic

#endif
