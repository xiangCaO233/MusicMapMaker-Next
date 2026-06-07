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
/// @param edge 无边框窗口边缘缩放方向。
/// @return X11 EWMH 方向值。
long toX11MoveResizeDirection(WindowFrameResizeEdge edge)
{
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
    return X11_WM_MOVERESIZE_MOVE;
}

/// @brief 向 X11 根窗口发送 _NET_WM_MOVERESIZE 消息。
/// @param window GLFW 窗口句柄。
/// @param rootCursorX 鼠标在根窗口中的 X 坐标。
/// @param rootCursorY 鼠标在根窗口中的 Y 坐标。
/// @param direction 移动或缩放方向。
/// @return 发送成功时返回 true。
bool sendX11MoveResizeMessage(GLFWwindow* window, int rootCursorX,
                              int rootCursorY, long direction)
{
    if ( !window || glfwGetPlatform() != GLFW_PLATFORM_X11 ) {
        return false;
    }

    Display* display = glfwGetX11Display();
    Window   xWindow = glfwGetX11Window(window);
    if ( !display || xWindow == 0 ) {
        return false;
    }

    Atom moveResizeAtom = XInternAtom(display, "_NET_WM_MOVERESIZE", False);
    if ( moveResizeAtom == None ) {
        return false;
    }

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

    XUngrabPointer(display, CurrentTime);
    const int sent =
        XSendEvent(display,
                   XDefaultRootWindow(display),
                   False,
                   SubstructureRedirectMask | SubstructureNotifyMask,
                   &event);
    XFlush(display);
    return sent != 0;
}

#    if defined(MMM_ENABLE_X11_FRAME_SHAPE)
/// @brief X11 无边框窗口圆角基础半径。
constexpr int X11_FRAME_CORNER_RADIUS = 10;
#    endif
}  // namespace

X11WindowAdapter::X11WindowAdapter(IWindowFrameHost& host)
    : m_host(host), m_window(host.getFrameWindowHandle())
{
    m_dragAreaSubscription =
        Event::EventBus::instance().subscribe<Event::UpdateDragAreaEvent>(
            [this](const Event::UpdateDragAreaEvent& event) {
                this->onUpdateDragArea(event);
            });
}

X11WindowAdapter::~X11WindowAdapter()
{
    if ( m_dragAreaSubscription != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::UpdateDragAreaEvent>(
            m_dragAreaSubscription);
    }
}

bool X11WindowAdapter::requestMove()
{
    if ( !m_window || glfwGetPlatform() != GLFW_PLATFORM_X11 ||
         glfwGetWindowMonitor(m_window) != nullptr ) {
        return false;
    }

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
        int normalX      = 0;
        int normalY      = 0;
        int normalWidth  = 0;
        int normalHeight = 0;
        m_host.getNormalFramePlacement(
            normalX, normalY, normalWidth, normalHeight);
        (void)normalX;
        (void)normalY;

        const int   restoreWidth  = std::max(1, normalWidth);
        const int   restoreHeight = std::max(1, normalHeight);
        const float cursorRatioX =
            windowWidth > 0 ? std::clamp(static_cast<float>(cursorX) /
                                             static_cast<float>(windowWidth),
                                         0.0f,
                                         1.0f)
                            : 0.5f;
        const int restoreX =
            rootCursorX -
            static_cast<int>(static_cast<float>(restoreWidth) * cursorRatioX);
        const int restoreY =
            rootCursorY -
            std::clamp(static_cast<int>(cursorY), 0, restoreHeight - 1);

        glfwRestoreWindow(m_window);
        glfwSetWindowSize(m_window, restoreWidth, restoreHeight);
        glfwSetWindowPos(m_window, restoreX, restoreY);
        m_host.setNormalFramePlacement(
            restoreX, restoreY, restoreWidth, restoreHeight);
        refreshFrameShape();
    }

    return sendX11MoveResizeMessage(
        m_window, rootCursorX, rootCursorY, X11_WM_MOVERESIZE_MOVE);
}

bool X11WindowAdapter::requestResize(WindowFrameResizeEdge edge)
{
    if ( !m_window || glfwGetPlatform() != GLFW_PLATFORM_X11 ||
         glfwGetWindowMonitor(m_window) != nullptr ||
         glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE ) {
        return false;
    }

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

bool X11WindowAdapter::supportsClientFrameRequests() const
{
    return m_window && glfwGetPlatform() == GLFW_PLATFORM_X11;
}

bool X11WindowAdapter::usesClientFrameOverlay() const
{
    return m_window && glfwGetPlatform() == GLFW_PLATFORM_X11;
}

void X11WindowAdapter::refreshFrameShape()
{
#    if defined(MMM_ENABLE_X11_FRAME_SHAPE)
    if ( !m_window || glfwGetPlatform() != GLFW_PLATFORM_X11 ) {
        return;
    }

    Display* display = glfwGetX11Display();
    Window   window  = glfwGetX11Window(m_window);
    if ( !display || window == 0 ) {
        return;
    }

    if ( glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE ||
         glfwGetWindowMonitor(m_window) != nullptr ) {
        XShapeCombineMask(display, window, ShapeBounding, 0, 0, None, ShapeSet);
        XFlush(display);
        return;
    }

    int width  = 0;
    int height = 0;
    glfwGetWindowSize(m_window, &width, &height);
    if ( width <= 0 || height <= 0 ) {
        return;
    }

    const int radius =
        std::clamp(static_cast<int>(std::round(
                       static_cast<float>(X11_FRAME_CORNER_RADIUS) *
                       Config::AppConfig::instance().getWindowContentScale())),
                   0,
                   std::min(width, height) / 2);
    if ( radius <= 0 ) {
        XShapeCombineMask(display, window, ShapeBounding, 0, 0, None, ShapeSet);
        XFlush(display);
        return;
    }

    Region region = XCreateRegion();
    if ( !region ) {
        return;
    }

    XRectangle middle{
        0,
        static_cast<short>(radius),
        static_cast<unsigned short>(width),
        static_cast<unsigned short>(std::max(0, height - radius * 2)),
    };
    XUnionRectWithRegion(&middle, region, region);

    const int radiusSquared = radius * radius;
    for ( int y = 0; y < radius; ++y ) {
        const int dy = radius - y;
        const int inset =
            radius - static_cast<int>(std::sqrt(static_cast<float>(
                         std::max(0, radiusSquared - dy * dy))));
        const int  stripWidth = std::max(0, width - inset * 2);
        XRectangle top{
            static_cast<short>(inset),
            static_cast<short>(y),
            static_cast<unsigned short>(stripWidth),
            1,
        };
        XUnionRectWithRegion(&top, region, region);

        XRectangle bottom{
            static_cast<short>(inset),
            static_cast<short>(height - y - 1),
            static_cast<unsigned short>(stripWidth),
            1,
        };
        XUnionRectWithRegion(&bottom, region, region);
    }

    XShapeCombineRegion(display, window, ShapeBounding, 0, 0, region, ShapeSet);
    XDestroyRegion(region);
    XFlush(display);
#    endif
}

bool X11WindowAdapter::handleClientMouseButton(int button, int action,
                                               double cursorX, double cursorY)
{
    if ( button != GLFW_MOUSE_BUTTON_LEFT ) {
        return false;
    }

    if ( action == GLFW_RELEASE ) {
        resetPendingFrameRequest();
        return false;
    }

    if ( action != GLFW_PRESS || !supportsClientFrameRequests() ||
         glfwGetWindowMonitor(m_window) != nullptr ) {
        return false;
    }

    m_pressX = cursorX;
    m_pressY = cursorY;
    resetPendingFrameRequest();

    const bool isMaximized =
        glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE;

    if ( auto edge = resolveResizeEdge(cursorX, cursorY) ) {
        m_skipNextMaximizedDragPress = false;
        if ( requestResize(*edge) ) {
            return true;
        }

        m_pendingResize     = true;
        m_pendingResizeEdge = *edge;
        return false;
    }

    if ( isInsideDragArea(cursorX, cursorY) ) {
        if ( m_skipNextMaximizedDragPress && isMaximized ) {
            m_skipNextMaximizedDragPress = false;
            return false;
        }
        m_skipNextMaximizedDragPress = false;
        m_pendingMove                = true;
    } else {
        m_skipNextMaximizedDragPress = false;
    }
    return false;
}

bool X11WindowAdapter::handleClientCursorPos(double cursorX, double cursorY)
{
    if ( !supportsClientFrameRequests() ||
         glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS ) {
        resetPendingFrameRequest();
        return false;
    }

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

    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const double dx        = cursorX - m_pressX;
    const double dy        = cursorY - m_pressY;
    const double threshold = std::max(
        2.0, static_cast<double>(X11_FRAME_MOVE_START_THRESHOLD * dpiScale));
    if ( dx * dx + dy * dy < threshold * threshold ) {
        return false;
    }

    if ( requestMove() ) {
        resetPendingFrameRequest();
        return true;
    }

    return false;
}

void X11WindowAdapter::handleClientFocusChange(bool focused)
{
    resetPendingFrameRequest();
    if ( !m_window ) {
        m_skipNextMaximizedDragPress = false;
        return;
    }

    const bool isMaximized =
        glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE;
    if ( !focused ) {
        m_skipNextMaximizedDragPress = isMaximized;
        return;
    }

    m_skipNextMaximizedDragPress = m_skipNextMaximizedDragPress || isMaximized;
}

void X11WindowAdapter::onUpdateDragArea(const Event::UpdateDragAreaEvent& event)
{
    m_dragAreas = event.areas;
}

std::optional<WindowFrameResizeEdge> X11WindowAdapter::resolveResizeEdge(
    double cursorX, double cursorY) const
{
    if ( !m_window ||
         glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE ) {
        return std::nullopt;
    }

    int width  = 0;
    int height = 0;
    glfwGetWindowSize(m_window, &width, &height);
    if ( width <= 0 || height <= 0 ) {
        return std::nullopt;
    }

    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const double thickness = std::max(
        5.0, static_cast<double>(X11_FRAME_RESIZE_HIT_THICKNESS * dpiScale));
    const bool left   = cursorX <= thickness;
    const bool right  = cursorX >= static_cast<double>(width) - thickness;
    const bool top    = cursorY <= thickness;
    const bool bottom = cursorY >= static_cast<double>(height) - thickness;

    if ( top && left ) return WindowFrameResizeEdge::TopLeft;
    if ( top && right ) return WindowFrameResizeEdge::TopRight;
    if ( bottom && left ) return WindowFrameResizeEdge::BottomLeft;
    if ( bottom && right ) return WindowFrameResizeEdge::BottomRight;
    if ( left ) return WindowFrameResizeEdge::Left;
    if ( right ) return WindowFrameResizeEdge::Right;
    if ( top ) return WindowFrameResizeEdge::Top;
    if ( bottom ) return WindowFrameResizeEdge::Bottom;

    return std::nullopt;
}

bool X11WindowAdapter::isInsideDragArea(double cursorX, double cursorY) const
{
    for ( const auto& area : m_dragAreas ) {
        if ( area.w <= 0.0f || area.h <= 0.0f ) {
            continue;
        }

        if ( cursorX >= static_cast<double>(area.x) &&
             cursorX <= static_cast<double>(area.x + area.w) &&
             cursorY >= static_cast<double>(area.y) &&
             cursorY <= static_cast<double>(area.y + area.h) ) {
            return true;
        }
    }
    return false;
}

void X11WindowAdapter::resetPendingFrameRequest()
{
    m_pendingMove   = false;
    m_pendingResize = false;
}

}  // namespace MMM::Graphic

#endif
