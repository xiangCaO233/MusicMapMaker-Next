#if defined(__APPLE__)

#include "graphic/glfw/window/adapters/MacOSWindowAdapter.h"
#include "event/ui/GLFWNativeEvent.h"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <objc/runtime.h>

namespace MMM::Graphic
{
namespace
{
/// @brief macOS 普通窗口的原生内容裁剪圆角半径。
constexpr CGFloat MACOS_WINDOW_CORNER_RADIUS = 12.0;

/// @brief Cocoa 窗口上保存原始左键按下事件的关联键。
char MACOS_MOUSE_DOWN_EVENT_ASSOCIATION_KEY;

/// @brief 获取当前正在派发的左键按下事件。
/// @param nativeWindow 目标 Cocoa 窗口。
/// @return 属于目标窗口的左键按下事件；当前不是按下阶段时返回 nil。
NSEvent* currentLeftMouseDownEvent(NSWindow* nativeWindow)
{
    if ( !nativeWindow ) {
        return nil;
    }

    NSEvent* event = [NSApp currentEvent];
    if ( !event || [event window] != nativeWindow ||
         [event type] != NSEventTypeLeftMouseDown ) {
        return nil;
    }
    return event;
}

/// @brief 缓存供 AppKit 原生窗口拖动使用的初始左键事件。
/// @param nativeWindow 目标 Cocoa 窗口。
/// @param event 原始左键按下事件；传入 nil 时清空缓存。
void cacheMouseDownEvent(NSWindow* nativeWindow, NSEvent* event)
{
    if ( !nativeWindow ) {
        return;
    }

    objc_setAssociatedObject(nativeWindow,
                             &MACOS_MOUSE_DOWN_EVENT_ASSOCIATION_KEY,
                             event,
                             event ? OBJC_ASSOCIATION_RETAIN_NONATOMIC
                                   : OBJC_ASSOCIATION_ASSIGN);
}

/// @brief 对承载窗口画面的 Cocoa 视图层应用圆角裁剪。
/// @param view 需要裁剪的视图。
/// @param cornerRadius 当前窗口状态对应的圆角半径。
/// @param shouldClip 普通窗口状态下为 true。
void applyRoundedContentLayer(NSView* view, CGFloat cornerRadius,
                              bool shouldClip)
{
    if ( !view ) {
        return;
    }

    [view setWantsLayer:YES];
    CALayer* layer = [view layer];
    if ( !layer ) {
        return;
    }

    [layer setCornerRadius:cornerRadius];
    [layer setMasksToBounds:shouldClip ? YES : NO];
    [layer setAllowsEdgeAntialiasing:YES];
}
}  // namespace

MacOSWindowAdapter::MacOSWindowAdapter(IWindowFrameHost& host)
    : m_host(host), m_window(host.getFrameWindowHandle())
{
    m_dragAreaSubscription =
        Event::EventBus::instance().subscribe<Event::UpdateDragAreaEvent>(
            [this](const Event::UpdateDragAreaEvent& event) {
                this->onUpdateDragArea(event);
            });
    installNativeDragBridge();
}

MacOSWindowAdapter::~MacOSWindowAdapter()
{
    removeNativeDragBridge();
    if ( m_dragAreaSubscription != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::UpdateDragAreaEvent>(
            m_dragAreaSubscription);
    }
}

bool MacOSWindowAdapter::requestMove()
{
    if ( !m_window || glfwGetWindowMonitor(m_window) != nullptr ||
         m_host.isFrameMaximized() ) {
        return false;
    }

    NSWindow* nativeWindow = glfwGetCocoaWindow(m_window);
    if ( !nativeWindow ) {
        return false;
    }

    NSEvent* mouseDownEvent = currentLeftMouseDownEvent(nativeWindow);
    if ( !mouseDownEvent ) {
        mouseDownEvent = objc_getAssociatedObject(
            nativeWindow, &MACOS_MOUSE_DOWN_EVENT_ASSOCIATION_KEY);
    }
    if ( !mouseDownEvent || [mouseDownEvent window] != nativeWindow ||
         [mouseDownEvent type] != NSEventTypeLeftMouseDown ) {
        return false;
    }

    [nativeWindow performWindowDragWithEvent:mouseDownEvent];
    cacheMouseDownEvent(nativeWindow, nil);
    return true;
}

bool MacOSWindowAdapter::requestResize(WindowFrameResizeEdge edge)
{
    (void)edge;
    return false;
}

bool MacOSWindowAdapter::supportsClientFrameRequests() const
{
    return false;
}

bool MacOSWindowAdapter::usesClientFrameOverlay() const
{
    return false;
}

void MacOSWindowAdapter::refreshFrameShape()
{
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    if ( !nativeWindow ) {
        return;
    }

    const bool useWindowEffects =
        glfwGetWindowMonitor(m_window) == nullptr && !m_host.isFrameMaximized();
    NSView* contentView = [nativeWindow contentView];

    [nativeWindow setOpaque:NO];
    [nativeWindow setBackgroundColor:[NSColor clearColor]];
    [nativeWindow setHasShadow:useWindowEffects ? YES : NO];

    if ( contentView ) {
        const CGFloat cornerRadius =
            useWindowEffects ? MACOS_WINDOW_CORNER_RADIUS : 0.0;
        applyRoundedContentLayer(
            contentView, cornerRadius, useWindowEffects);
        applyRoundedContentLayer(
            [contentView superview], cornerRadius, useWindowEffects);
    }

    [nativeWindow invalidateShadow];
}

bool MacOSWindowAdapter::handleClientMouseButton(int button, int action,
                                                 double cursorX, double cursorY)
{
    if ( button != GLFW_MOUSE_BUTTON_LEFT ) {
        return false;
    }

    if ( action == GLFW_RELEASE ) {
        resetPendingFrameRequest();
        return false;
    }

    if ( action != GLFW_PRESS || !m_window ||
         glfwGetWindowMonitor(m_window) != nullptr ||
         !isInsideDragArea(cursorX, cursorY) ) {
        return false;
    }

    NSWindow* nativeWindow = glfwGetCocoaWindow(m_window);
    NSEvent*  mouseDownEvent = currentLeftMouseDownEvent(nativeWindow);
    if ( mouseDownEvent ) {
        cacheMouseDownEvent(nativeWindow, mouseDownEvent);
    }

    if ( mouseDownEvent && [mouseDownEvent clickCount] >= 2 ) {
        resetPendingFrameRequest();
        Event::EventBus::instance().publish(Event::GLFWNativeEvent{
            .type =
                Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE });
        return true;
    }

    if ( !m_host.isFrameMaximized() ) {
        return requestMove();
    }

    m_pressX = cursorX;
    m_pressY = cursorY;
    resetPendingFrameRequest();
    cacheMouseDownEvent(nativeWindow, mouseDownEvent);
    m_pendingMove = true;
    return true;
}

bool MacOSWindowAdapter::handleClientCursorPos(double cursorX, double cursorY)
{
    if ( !m_window ||
         glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS ) {
        resetPendingFrameRequest();
        return false;
    }

    if ( !m_pendingMove || !m_host.isFrameMaximized() ) {
        return false;
    }

    const double dx        = cursorX - m_pressX;
    const double dy        = cursorY - m_pressY;
    constexpr double threshold = 4.0;
    if ( dx * dx + dy * dy < threshold * threshold ) {
        return false;
    }

    m_pendingMove = false;
    if ( !m_host.restoreFrameForClientMove(cursorX, cursorY) ) {
        resetPendingFrameRequest();
        return false;
    }
    return requestMove();
}

void MacOSWindowAdapter::handleClientFocusChange(bool focused)
{
    (void)focused;
    resetPendingFrameRequest();
}

void MacOSWindowAdapter::onUpdateDragArea(
    const Event::UpdateDragAreaEvent& event)
{
    m_dragAreas        = event.areas;
    m_blockedDragAreas = event.blockedAreas;
}

void MacOSWindowAdapter::installNativeDragBridge()
{
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    if ( !nativeWindow ) {
        return;
    }

    [nativeWindow setMovable:YES];
    [nativeWindow setMovableByWindowBackground:NO];
    refreshFrameShape();
}

void MacOSWindowAdapter::removeNativeDragBridge()
{
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    if ( !nativeWindow ) {
        return;
    }

    cacheMouseDownEvent(nativeWindow, nil);
}

bool MacOSWindowAdapter::isInsideDragArea(double cursorX, double cursorY) const
{
    for ( const auto& area : m_blockedDragAreas ) {
        if ( area.w <= 0.0f || area.h <= 0.0f ) {
            continue;
        }

        if ( cursorX >= static_cast<double>(area.x) &&
             cursorX <= static_cast<double>(area.x + area.w) &&
             cursorY >= static_cast<double>(area.y) &&
             cursorY <= static_cast<double>(area.y + area.h) ) {
            return false;
        }
    }

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

void MacOSWindowAdapter::resetPendingFrameRequest()
{
    m_pendingMove = false;
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    cacheMouseDownEvent(nativeWindow, nil);
}

}  // namespace MMM::Graphic

#endif
