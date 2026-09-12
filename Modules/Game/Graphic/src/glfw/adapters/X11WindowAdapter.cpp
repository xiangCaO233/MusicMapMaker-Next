#if defined(MMM_ENABLE_X11_FRAME_INTERACTION)

#    include "graphic/glfw/window/adapters/X11WindowAdapter.h"
#    include "config/AppConfig.h"
#    include <GLFW/glfw3.h>
#    define GLFW_EXPOSE_NATIVE_X11
#    include <GLFW/glfw3native.h>
#    include <algorithm>
#    include <cmath>

#    if defined(MMM_ENABLE_X11_FRAME_SHAPE)
#        include <X11/extensions/shape.h>
#    endif

namespace MMM::Graphic
{
namespace
{
/// @brief X11 EWMH 无边框窗口移动消息方向。
constexpr long X11_WM_MOVERESIZE_MOVE = 8;

/// @brief X11 EWMH 消息中的鼠标左键编号。
constexpr long X11_WM_MOVERESIZE_LEFT_BUTTON = 1;

/// @brief X11 EWMH 消息来源：普通应用。
constexpr long X11_WM_MOVERESIZE_SOURCE_APPLICATION = 1;

/// @brief X11 无装饰窗口边缘缩放热区基础宽度。
constexpr float X11_FRAME_RESIZE_HIT_THICKNESS = 8.0f;

/// @brief 标题栏拖动启动前允许的鼠标移动阈值。
constexpr float X11_FRAME_MOVE_START_THRESHOLD = 3.0f;

/// @brief 将窗口缩放方向转换为 _NET_WM_MOVERESIZE 使用的方向值。
///
/// EWMH 对八个边角使用 0 到 7 的固定编码；本地枚举顺序不是协议契约，因此在
/// 此处显式映射。未知值回退到移动操作，避免把未定义整数发送给窗口管理器。
///
/// @param edge 无边框窗口边缘缩放方向。
/// @return X11 EWMH 方向值。
long toX11MoveResizeDirection(WindowFrameResizeEdge edge)
{
    // 协议编码按左上开始顺时针排列，不能用 static_cast 依赖枚举底层值。
    switch ( edge ) {
    case WindowFrameResizeEdge::TopLeft: return 0;
    case WindowFrameResizeEdge::Top: return 1;
    case WindowFrameResizeEdge::TopRight: return 2;
    case WindowFrameResizeEdge::Right: return 3;
    case WindowFrameResizeEdge::BottomRight: return 4;
    case WindowFrameResizeEdge::Bottom: return 5;
    case WindowFrameResizeEdge::BottomLeft: return 6;
    case WindowFrameResizeEdge::Left: return 7;
    }
    // 防御未来扩展枚举时遗漏分支；移动是窗口管理器能够安全解释的已知操作。
    return X11_WM_MOVERESIZE_MOVE;
}

/// @brief 向 X11 根窗口发送 _NET_WM_MOVERESIZE 消息。
///
/// 消息把根窗口坐标、交互方向、鼠标按键和来源编码交给 EWMH 窗口管理器，实际
/// 移动/缩放由窗口管理器接管。发送前释放 pointer grab，避免客户窗口继续截获
/// 本次拖拽。
///
/// @param window GLFW 窗口句柄。
/// @param rootCursorX 鼠标在根窗口中的 X 坐标。
/// @param rootCursorY 鼠标在根窗口中的 Y 坐标。
/// @param direction 移动或缩放方向。
/// @return 发送成功时返回 true。
/// @warning 鼠标交互回调路径：只查询原生句柄并发送一条 X11 消息，不分配项目
/// 资源或等待窗口管理器完成操作。
bool sendX11MoveResizeMessage(GLFWwindow* window, int rootCursorX,
                              int rootCursorY, long direction)
{
    // 原生 X11 API 只能用于 GLFW 当前实际选择的 X11 backend；Wayland 等平台由
    // 其他适配策略处理。
    if ( !window || glfwGetPlatform() != GLFW_PLATFORM_X11 ) {
        return false;
    }

    // GLFW 同时提供 Display 与 Window，任一无效都不能构造 ClientMessage。
    Display* display = glfwGetX11Display();
    Window   xWindow = glfwGetX11Window(window);
    if ( !display || xWindow == 0 ) {
        return false;
    }

    // False 允许在 atom 尚未缓存时由 X server 创建标准 EWMH atom。
    Atom moveResizeAtom = XInternAtom(display, "_NET_WM_MOVERESIZE", False);
    if ( moveResizeAtom == None ) {
        return false;
    }

    // 零初始化整个 union，防止未使用字段携带栈垃圾传给 X server。
    XEvent event{};
    event.xclient.type         = ClientMessage;
    event.xclient.window       = xWindow;
    event.xclient.message_type = moveResizeAtom;
    event.xclient.format       = 32;
    event.xclient.data.l[0]    = rootCursorX;
    event.xclient.data.l[1]    = rootCursorY;
    event.xclient.data.l[2]    = direction;
    event.xclient.data.l[3]    = X11_WM_MOVERESIZE_LEFT_BUTTON;
    event.xclient.data.l[4]    = X11_WM_MOVERESIZE_SOURCE_APPLICATION;

    // 窗口管理器需要取得 pointer grab 才能继续原生拖拽，先释放
    // GLFW/客户区抓取。
    XUngrabPointer(display, CurrentTime);
    const int sent =
        XSendEvent(display,
                   XDefaultRootWindow(display),
                   False,
                   SubstructureRedirectMask | SubstructureNotifyMask,
                   &event);
    // XSendEvent 只把请求排入客户端输出缓冲；flush 让 WM 能立即收到交互请求。
    XFlush(display);
    return sent != 0;
}

#    if defined(MMM_ENABLE_X11_FRAME_SHAPE)
/// @brief X11 无边框窗口圆角基础半径。
constexpr int X11_FRAME_CORNER_RADIUS = 10;
#    endif
}  // namespace

/// @brief 绑定主窗口并订阅 UI 发布的标题栏拖拽区域更新。
///
/// host 与 GLFWwindow 均为非 owning 引用，生命周期由 NativeWindow 覆盖适配器。
/// EventBus 订阅捕获 this，因此析构前必须用保存的订阅 ID 注销。
///
/// @param host 提供原生窗口句柄和普通窗口 placement 的宿主。
X11WindowAdapter::X11WindowAdapter(IWindowFrameHost& host)
    : m_host(host), m_window(host.getFrameWindowHandle())
{
    // UI 可能在布局变化后多次发布区域，回调只覆盖本地缓存，不触发 X11 操作。
    m_dragAreaSubscription =
        Event::EventBus::instance().subscribe<Event::UpdateDragAreaEvent>(
            [this](const Event::UpdateDragAreaEvent& event) {
                this->onUpdateDragArea(event);
            });
}

/// @brief 注销拖拽区域订阅并结束适配器生命周期。
///
/// GLFW 窗口由宿主销毁，本类只断开可能继续回调 this 的 EventBus 连接。
X11WindowAdapter::~X11WindowAdapter()
{
    // 零值表示订阅从未成功建立或已无可注销句柄。
    if ( m_dragAreaSubscription != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::UpdateDragAreaEvent>(
            m_dragAreaSubscription);
    }
}

/// @brief 请求窗口管理器从当前指针位置开始移动普通顶层窗口。
///
/// 最大化窗口先恢复到宿主保存的 normal placement，并保持指针在窗口宽度中的
/// 相对横向位置，随后发送 EWMH move 请求。全屏窗口和非 X11 backend 不处理。
///
/// @return EWMH ClientMessage 成功排入 X server 时返回 true。
/// @warning 鼠标交互路径：最大化恢复分支会同步调用 GLFW 尺寸/位置更新，但不做
/// 固定时长等待或文件访问。
bool X11WindowAdapter::requestMove()
{
    // monitor 非空表示 GLFW fullscreen，不能把它当作普通无装饰顶层窗口移动。
    if ( !m_window || glfwGetPlatform() != GLFW_PLATFORM_X11 ||
         glfwGetWindowMonitor(m_window) != nullptr ) {
        return false;
    }

    // GLFW cursor 坐标位于客户区，EWMH 协议要求根窗口坐标，需与窗口位置相加。
    int    windowX      = 0;
    int    windowY      = 0;
    int    windowWidth  = 0;
    int    windowHeight = 0;
    double cursorX      = 0.0;
    double cursorY      = 0.0;
    glfwGetWindowPos(m_window, &windowX, &windowY);
    glfwGetWindowSize(m_window, &windowWidth, &windowHeight);
    glfwGetCursorPos(m_window, &cursorX, &cursorY);

    const int rootCursorX = windowX + static_cast<int>(cursorX);
    const int rootCursorY = windowY + static_cast<int>(cursorY);

    if ( glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE ) {
        // 从宿主缓存读取最后一个普通 placement；位置会按当前指针重新计算，原始
        // normalX/normalY 只为完成统一接口读取。
        int normalX      = 0;
        int normalY      = 0;
        int normalWidth  = 0;
        int normalHeight = 0;
        m_host.getNormalFramePlacement(
            normalX, normalY, normalWidth, normalHeight);
        (void)normalX;
        (void)normalY;

        // 对缺失或异常的历史尺寸做最小合法钳制，避免传入 GLFW 的宽高为零。
        const int restoreWidth  = std::max(1, normalWidth);
        const int restoreHeight = std::max(1, normalHeight);
        // 维持指针在标题栏中的相对横向比例，可避免恢复后窗口突然跳到指针一侧。
        const float cursorRatioX =
            windowWidth > 0 ? std::clamp(static_cast<float>(cursorX) /
                                             static_cast<float>(windowWidth),
                                         0.0f,
                                         1.0f)
                            : 0.5f;
        // 纵向位置直接保留指针在恢复窗口中的客户区偏移，并限制在有效高度内。
        const int restoreX =
            rootCursorX -
            static_cast<int>(static_cast<float>(restoreWidth) * cursorRatioX);
        const int restoreY =
            rootCursorY -
            std::clamp(static_cast<int>(cursorY), 0, restoreHeight - 1);

        // 先解除 maximized 状态，再写入明确的 size/position，最后刷新 shape
        // mask。
        glfwRestoreWindow(m_window);
        glfwSetWindowSize(m_window, restoreWidth, restoreHeight);
        glfwSetWindowPos(m_window, restoreX, restoreY);
        m_host.setNormalFramePlacement(
            restoreX, restoreY, restoreWidth, restoreHeight);
        refreshFrameShape();
    }

    // 无论是否经过恢复，WM 都从按下时换算出的根坐标开始接管移动。
    return sendX11MoveResizeMessage(
        m_window, rootCursorX, rootCursorY, X11_WM_MOVERESIZE_MOVE);
}

/// @brief 请求窗口管理器从当前指针位置开始指定边角的原生缩放。
///
/// 仅普通、非最大化的 X11 顶层窗口允许缩放。函数把客户区 cursor 坐标转换为根
/// 窗口坐标，再使用显式 EWMH 方向编码发送请求。
///
/// @param edge 用户命中的窗口边或角。
/// @return EWMH resize 消息成功发送时返回 true。
/// @warning 鼠标交互回调路径：只执行 GLFW 状态查询和单条 X11 消息发送。
bool X11WindowAdapter::requestResize(WindowFrameResizeEdge edge)
{
    // 全屏与最大化状态由窗口管理器控制尺寸，不允许客户区边缘覆盖其策略。
    if ( !m_window || glfwGetPlatform() != GLFW_PLATFORM_X11 ||
         glfwGetWindowMonitor(m_window) != nullptr ||
         glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE ) {
        return false;
    }

    // EWMH 与 GLFW 使用不同坐标空间，窗口原点和客户区 cursor 必须组合。
    int    windowX = 0;
    int    windowY = 0;
    double cursorX = 0.0;
    double cursorY = 0.0;
    glfwGetWindowPos(m_window, &windowX, &windowY);
    glfwGetCursorPos(m_window, &cursorX, &cursorY);

    return sendX11MoveResizeMessage(m_window,
                                    windowX + static_cast<int>(cursorX),
                                    windowY + static_cast<int>(cursorY),
                                    toX11MoveResizeDirection(edge));
}

/// @brief 检查当前窗口能否使用 X11 客户区 frame 请求。
/// @return 已绑定窗口且 GLFW 当前 backend 为 X11 时返回 true。
/// @warning UI 查询热路径：仅读取句柄和 GLFW backend，不执行原生消息发送。
bool X11WindowAdapter::supportsClientFrameRequests() const
{
    return m_window && glfwGetPlatform() == GLFW_PLATFORM_X11;
}

/// @brief 判断 X11 无装饰窗口是否需要 UI 绘制边框覆盖层。
/// @return 当前使用有效 X11 窗口时返回 true。
/// @warning UI 查询热路径：不得在此触发 shape 更新或窗口管理器交互。
bool X11WindowAdapter::usesClientFrameOverlay() const
{
    return m_window && glfwGetPlatform() == GLFW_PLATFORM_X11;
}

/// @brief 根据当前尺寸、最大化状态与 DPI 刷新 X11 Shape 圆角边界。
///
/// 最大化/全屏窗口恢复矩形边界；普通窗口用中央矩形和上下逐行条带合成圆角
/// region。 若构建未启用 X11 Shape 扩展，函数保持为空操作。
///
/// @warning 低频窗口状态路径：会创建临时 X11 Region 并 flush
/// Display，只应在尺寸 或窗口状态变化后调用，不能放入每帧渲染循环。
void X11WindowAdapter::refreshFrameShape()
{
#    if defined(MMM_ENABLE_X11_FRAME_SHAPE)
    // 即使编译期具备 XShape，运行时 backend 仍可能是 Wayland，必须再次守卫。
    if ( !m_window || glfwGetPlatform() != GLFW_PLATFORM_X11 ) {
        return;
    }

    // 原生 Display/Window 由 GLFW 拥有，本函数只在调用期间借用。
    Display* display = glfwGetX11Display();
    Window   window  = glfwGetX11Window(m_window);
    if ( !display || window == 0 ) {
        return;
    }

    // 最大化和 fullscreen 应覆盖整个矩形，移除自定义 bounding
    // mask，避免屏幕边缘 出现透明缺角。
    if ( glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE ||
         glfwGetWindowMonitor(m_window) != nullptr ) {
        XShapeCombineMask(display, window, ShapeBounding, 0, 0, None, ShapeSet);
        XFlush(display);
        return;
    }

    // XRectangle 使用有限宽度字段；这里只接受 GLFW 报告的正尺寸窗口。
    int width  = 0;
    int height = 0;
    glfwGetWindowSize(m_window, &width, &height);
    if ( width <= 0 || height <= 0 ) {
        return;
    }

    // 基础半径按内容缩放系数调整，并限制到短边一半，保证条带宽度非负。
    const int radius =
        std::clamp(static_cast<int>(std::round(
                       static_cast<float>(X11_FRAME_CORNER_RADIUS) *
                       Config::AppConfig::instance().getWindowContentScale())),
                   0,
                   std::min(width, height) / 2);
    if ( radius <= 0 ) {
        // DPI 或尺寸使半径退化时显式清除旧 mask，不能保留上一次较大窗口的圆角。
        XShapeCombineMask(display, window, ShapeBounding, 0, 0, None, ShapeSet);
        XFlush(display);
        return;
    }

    // Region 是本函数唯一拥有的 X11 临时资源，所有成功路径末尾必须销毁。
    Region region = XCreateRegion();
    if ( !region ) {
        return;
    }

    // 中央矩形覆盖除上下圆角带之外的完整宽度，避免逐像素构造主体区域。
    XRectangle middle{
        0,
        static_cast<short>(radius),
        static_cast<unsigned short>(width),
        static_cast<unsigned short>(std::max(0, height - radius * 2)),
    };
    XUnionRectWithRegion(&middle, region, region);

    // 对圆的每条扫描线求水平 inset，再把对称的顶部和底部 1px 条带并入 region。
    const int radiusSquared = radius * radius;
    for ( int y = 0; y < radius; ++y ) {
        // dy 从圆心向边缘递减，sqrt 计算当前扫描线可见弦长对应的内缩量。
        const int dy = radius - y;
        const int inset =
            radius - static_cast<int>(std::sqrt(static_cast<float>(
                         std::max(0, radiusSquared - dy * dy))));
        const int stripWidth = std::max(0, width - inset * 2);
        // width 已钳制，转换为 XRectangle 的 unsigned short 前不会产生负数。
        XRectangle top{
            static_cast<short>(inset),
            static_cast<short>(y),
            static_cast<unsigned short>(stripWidth),
            1,
        };
        XUnionRectWithRegion(&top, region, region);

        // 底部条带与顶部关于窗口水平中线对称。
        XRectangle bottom{
            static_cast<short>(inset),
            static_cast<short>(height - y - 1),
            static_cast<unsigned short>(stripWidth),
            1,
        };
        XUnionRectWithRegion(&bottom, region, region);
    }

    // ShapeSet 替换而不是叠加旧 mask，因此 resize 后不会残留旧尺寸的区域。
    XShapeCombineRegion(display, window, ShapeBounding, 0, 0, region, ShapeSet);
    // server 已复制 region 内容，本地对象随后即可销毁。
    XDestroyRegion(region);
    // flush 使边界变化及时到达 compositor/window manager。
    XFlush(display);
#    endif
}

/// @brief 处理客户区鼠标按键并建立待移动或待缩放状态。
///
/// 左键按下时边缘 resize 优先于标题栏 move。窗口管理器若立即接受 resize 请求，
/// 返回 true 让上层停止普通 UI 处理；发送失败时保留 pending 状态，允许 cursor
/// 回调在指针移动后重试。释放左键总会清除挂起请求。
///
/// @param button GLFW 鼠标按键编号。
/// @param action GLFW_PRESS、GLFW_RELEASE 等动作。
/// @param cursorX 按键发生时的客户区 X 坐标。
/// @param cursorY 按键发生时的客户区 Y 坐标。
/// @return 已成功把窗口交互交给窗口管理器时返回 true。
/// @warning 输入回调热路径：只更新缓存状态并至多发送一次 X11 请求，禁止分配、
/// 阻塞等待或遍历窗口外的全局对象。
bool X11WindowAdapter::handleClientMouseButton(int button, int action,
                                               double cursorX, double cursorY)
{
    // 只有左键参与 EWMH moveresize，其他按键继续由 UI 正常消费。
    if ( button != GLFW_MOUSE_BUTTON_LEFT ) {
        return false;
    }

    if ( action == GLFW_RELEASE ) {
        // 无论 WM 是否已接管，释放时都终止本地重试状态，避免下次移动误触发。
        resetPendingFrameRequest();
        return false;
    }

    // repeat/未知动作、非 X11 backend 和 fullscreen 窗口都不建立客户区 frame
    // 状态。
    if ( action != GLFW_PRESS || !supportsClientFrameRequests() ||
         glfwGetWindowMonitor(m_window) != nullptr ) {
        return false;
    }

    // 保存按下原点用于标题栏移动阈值；新按下会覆盖任何旧的未完成请求。
    m_pressX = cursorX;
    m_pressY = cursorY;
    resetPendingFrameRequest();

    // 最大化状态同时影响拖拽恢复保护和 resize 可用性。
    const bool isMaximized =
        glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE;

    // 边角 hit zone 优先，避免标题栏区域延伸到窗口上边缘时抢走 resize。
    if ( auto edge = resolveResizeEdge(cursorX, cursorY) ) {
        m_skipNextMaximizedDragPress = false;
        if ( requestResize(*edge) ) {
            return true;
        }

        // 某些 WM 可能只在指针产生位移后接受请求，保留方向供 cursor 回调重试。
        m_pendingResize     = true;
        m_pendingResizeEdge = *edge;
        return false;
    }

    if ( isInsideDragArea(cursorX, cursorY) ) {
        // 焦点切回最大化窗口产生的首个 press
        // 可能不是用户拖拽，消费一次保护标志。
        if ( m_skipNextMaximizedDragPress && isMaximized ) {
            m_skipNextMaximizedDragPress = false;
            return false;
        }
        m_skipNextMaximizedDragPress = false;
        // Move 延迟到越过 DPI 缩放阈值后再请求，允许标题栏单击继续传给 UI。
        m_pendingMove = true;
    } else {
        m_skipNextMaximizedDragPress = false;
    }
    return false;
}

/// @brief 在左键保持期间推进挂起的客户区移动或缩放请求。
///
/// Resize 请求可立即重试；Move 请求必须先超过按 DPI 缩放的距离阈值，区分点击和
/// 拖拽。任一请求被窗口管理器接受后立即清除本地 pending 状态。
///
/// @param cursorX 当前客户区 X 坐标。
/// @param cursorY 当前客户区 Y 坐标。
/// @return 本次回调成功启动窗口管理器交互时返回 true。
/// @warning 高频输入路径：鼠标移动时可连续调用，只允许常数时间计算和至多一次
/// X11 消息发送，不得创建 Region 或执行等待。
bool X11WindowAdapter::handleClientCursorPos(double cursorX, double cursorY)
{
    // 平台失效或左键已释放时，挂起状态不再有合法完成条件。
    if ( !supportsClientFrameRequests() ||
         glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS ) {
        resetPendingFrameRequest();
        return false;
    }

    // Resize 不需要移动阈值，边缘 press 已明确表达尺寸调整意图。
    if ( m_pendingResize ) {
        if ( requestResize(m_pendingResizeEdge) ) {
            resetPendingFrameRequest();
            return true;
        }
        return false;
    }

    if ( !m_pendingMove ) {
        return false;
    }

    // 物理感受上的拖动阈值随内容缩放增大，并保留至少 2px 的稳定下限。
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const double dx        = cursorX - m_pressX;
    const double dy        = cursorY - m_pressY;
    const double threshold = std::max(
        2.0, static_cast<double>(X11_FRAME_MOVE_START_THRESHOLD * dpiScale));
    // 比较平方距离避免每次 cursor event 求平方根。
    if ( dx * dx + dy * dy < threshold * threshold ) {
        return false;
    }

    // WM 接管后本地不再重发；失败则保留 pendingMove，后续移动仍可重试。
    if ( requestMove() ) {
        resetPendingFrameRequest();
        return true;
    }

    return false;
}

/// @brief 在焦点切换时复位交互，并维护最大化窗口的首击保护状态。
///
/// 失焦期间可能发生鼠标释放而应用收不到对应事件，因此必须无条件清除 pending。
/// 对最大化窗口记录保护标志，防止重新获得焦点的同一次按下被解释为拖拽恢复。
///
/// @param focused 窗口当前是否获得输入焦点。
/// @warning 低频窗口事件路径：只更新本地布尔状态和查询一次 GLFW 属性。
void X11WindowAdapter::handleClientFocusChange(bool focused)
{
    // pending move/resize
    // 不允许跨焦点边界继续，否则根坐标和按钮状态都可能过期。
    resetPendingFrameRequest();
    if ( !m_window ) {
        m_skipNextMaximizedDragPress = false;
        return;
    }

    const bool isMaximized =
        glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE;
    if ( !focused ) {
        // 仅最大化窗口需要首击保护；普通窗口重新聚焦后可正常建立拖拽。
        m_skipNextMaximizedDragPress = isMaximized;
        return;
    }

    // 获得焦点时保留失焦阶段设置的标志，也覆盖焦点事件前已最大化的情况。
    m_skipNextMaximizedDragPress = m_skipNextMaximizedDragPress || isMaximized;
}

/// @brief 用 UI 最新上报的可拖拽区域与排除区域替换本地命中缓存。
/// @param event 包含逻辑客户区坐标下的允许区域和阻止区域。
/// @warning UI 事件路径：复制区域 vector，发布频率应限制在布局实际变化时，而非
/// 每个鼠标移动事件。
void X11WindowAdapter::onUpdateDragArea(const Event::UpdateDragAreaEvent& event)
{
    // 两组区域必须来自同一事件快照，blocked 在命中判断中拥有更高优先级。
    m_dragAreas        = event.areas;
    m_blockedDragAreas = event.blockedAreas;
}

/// @brief 将当前客户区坐标解析为无装饰窗口的八向 resize 命中结果。
///
/// 命中厚度由基础像素值和当前内容缩放共同决定，并设 5px 下限。角落组合优先于
/// 单边，最大化窗口完全禁用 resize hit zone。
///
/// @param cursorX 当前客户区 X 坐标。
/// @param cursorY 当前客户区 Y 坐标。
/// @return 命中的边角；窗口无效、尺寸无效或位于内部区域时返回 std::nullopt。
/// @warning 高频输入路径：只执行常数时间 GLFW 查询与算术，不得引入容器遍历、
/// 原生消息发送或阻塞。
std::optional<WindowFrameResizeEdge> X11WindowAdapter::resolveResizeEdge(
    double cursorX, double cursorY) const
{
    // 最大化窗口的边缘属于桌面工作区边界，客户区不应覆盖 WM 的系统行为。
    if ( !m_window ||
         glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE ) {
        return std::nullopt;
    }

    // GLFW 客户区尺寸与 cursor 坐标处于同一坐标空间，可直接比较 hit thickness。
    int width  = 0;
    int height = 0;
    glfwGetWindowSize(m_window, &width, &height);
    if ( width <= 0 || height <= 0 ) {
        return std::nullopt;
    }

    // 使用 AppConfig 的内容缩放保持高 DPI 下相近的物理命中宽度。
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const double thickness = std::max(
        5.0, static_cast<double>(X11_FRAME_RESIZE_HIT_THICKNESS * dpiScale));
    // 坐标允许略微落在边界外，<=/>= 判定仍能在拖拽刚开始时保持边缘命中。
    const bool left   = cursorX <= thickness;
    const bool right  = cursorX >= static_cast<double>(width) - thickness;
    const bool top    = cursorY <= thickness;
    const bool bottom = cursorY >= static_cast<double>(height) - thickness;

    // 先返回四角，避免同时满足 top/left 时被降级为单边缩放。
    if ( top && left ) return WindowFrameResizeEdge::TopLeft;
    if ( top && right ) return WindowFrameResizeEdge::TopRight;
    if ( bottom && left ) return WindowFrameResizeEdge::BottomLeft;
    if ( bottom && right ) return WindowFrameResizeEdge::BottomRight;
    if ( left ) return WindowFrameResizeEdge::Left;
    if ( right ) return WindowFrameResizeEdge::Right;
    if ( top ) return WindowFrameResizeEdge::Top;
    if ( bottom ) return WindowFrameResizeEdge::Bottom;

    // 内部客户区不启动 resize，随后可继续尝试标题栏 drag area 命中。
    return std::nullopt;
}

/// @brief 判断客户区坐标是否落在可拖拽标题栏且未被交互控件阻挡。
///
/// blocked 区域优先于 drag 区域，用于从标题栏中扣除按钮、标签等可交互控件。
/// 非正尺寸矩形被视为无效并跳过，边界点按闭区间处理。
///
/// @param cursorX 当前客户区 X 坐标。
/// @param cursorY 当前客户区 Y 坐标。
/// @return 命中有效 drag 区域且未命中任何 blocked 区域时返回 true。
/// @warning 输入热路径：遍历 UI 预先缓存的小型区域列表；禁止在此重新计算布局或
/// 访问文件系统。
bool X11WindowAdapter::isInsideDragArea(double cursorX, double cursorY) const
{
    // 排除区优先检查，允许它覆盖任意数量的上层 drag area。
    for ( const auto& area : m_blockedDragAreas ) {
        // 忽略布局尚未完成或折叠控件产生的退化矩形。
        if ( area.w <= 0.0f || area.h <= 0.0f ) {
            continue;
        }

        // Event 坐标为 float，统一提升到 double 与 GLFW cursor 精度比较。
        if ( cursorX >= static_cast<double>(area.x) &&
             cursorX <= static_cast<double>(area.x + area.w) &&
             cursorY >= static_cast<double>(area.y) &&
             cursorY <= static_cast<double>(area.y + area.h) ) {
            return false;
        }
    }

    // 只有未被阻挡的坐标才进入允许区域扫描。
    for ( const auto& area : m_dragAreas ) {
        if ( area.w <= 0.0f || area.h <= 0.0f ) {
            continue;
        }

        // 任一允许矩形命中即可，不要求多个标题栏片段合并成连续区域。
        if ( cursorX >= static_cast<double>(area.x) &&
             cursorX <= static_cast<double>(area.x + area.w) &&
             cursorY >= static_cast<double>(area.y) &&
             cursorY <= static_cast<double>(area.y + area.h) ) {
            return true;
        }
    }
    // 没有区域快照或仅命中退化矩形时保持普通客户区交互。
    return false;
}

/// @brief 清除尚未交给窗口管理器的本地 move/resize 请求。
/// @warning 输入热路径：仅写入两个布尔标志，可在释放、失焦和请求成功时调用。
void X11WindowAdapter::resetPendingFrameRequest()
{
    // resize edge 与 press 坐标无需重置，只有对应 pending 标志为 true
    // 时才会读取。
    m_pendingMove   = false;
    m_pendingResize = false;
}

}  // namespace MMM::Graphic

#endif
