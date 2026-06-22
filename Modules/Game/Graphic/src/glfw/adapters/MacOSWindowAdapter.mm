#if defined(__APPLE__)

#include "graphic/glfw/window/adapters/MacOSWindowAdapter.h"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

namespace MMM::Graphic
{
namespace
{
/// @brief Cocoa content view 上保存 macOS frame adapter 的关联键。
char MACOS_WINDOW_ADAPTER_ASSOCIATION_KEY;

/// @brief 从 Cocoa content view 读取 macOS frame adapter。
/// @param view Cocoa content view。
/// @return 关联的 adapter；未关联时返回 nullptr。
MacOSWindowAdapter* adapterForContentView(NSView* view)
{
    if ( !view ) {
        return nullptr;
    }

    NSValue* adapterValue = objc_getAssociatedObject(
        view, &MACOS_WINDOW_ADAPTER_ASSOCIATION_KEY);
    return adapterValue ? static_cast<MacOSWindowAdapter*>(
                              [adapterValue pointerValue])
                        : nullptr;
}

/// @brief Cocoa content view 的 mouseDownCanMoveWindow 实现。
/// @param self Cocoa content view。
/// @param selector Objective-C selector。
/// @return 当前鼠标按下命中拖动区域时返回 YES。
BOOL contentViewMouseDownCanMoveWindow(id self, SEL selector)
{
    (void)selector;
    auto* adapter = adapterForContentView((NSView*)self);
    return adapter && adapter->handleNativeMouseDownCanMoveWindow() ? YES : NO;
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
    return false;
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

void MacOSWindowAdapter::refreshFrameShape() {}

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
         !m_host.isFrameMaximized() ) {
        return false;
    }

    m_pressX = cursorX;
    m_pressY = cursorY;
    resetPendingFrameRequest();

    if ( isInsideDragArea(cursorX, cursorY) ) {
        m_pendingMove = true;
    }
    return false;
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
    return m_host.restoreFrameForClientMove(cursorX, cursorY);
}

void MacOSWindowAdapter::handleClientFocusChange(bool focused)
{
    (void)focused;
    resetPendingFrameRequest();
}

bool MacOSWindowAdapter::handleNativeMouseDownCanMoveWindow() const
{
    if ( !m_window || glfwGetWindowMonitor(m_window) != nullptr ||
         m_host.isFrameMaximized() ) {
        return false;
    }

    double cursorX = 0.0;
    double cursorY = 0.0;
    if ( !getCurrentNativeMouseDownPosition(cursorX, cursorY) ) {
        return false;
    }

    return isInsideDragArea(cursorX, cursorY);
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

    NSView* contentView = [nativeWindow contentView];
    if ( !contentView ) {
        return;
    }

    Class contentViewClass = object_getClass(contentView);
    if ( !contentViewClass ) {
        return;
    }

    class_replaceMethod(contentViewClass,
                        @selector(mouseDownCanMoveWindow),
                        reinterpret_cast<IMP>(contentViewMouseDownCanMoveWindow),
                        "c@:");
    objc_setAssociatedObject(contentView,
                             &MACOS_WINDOW_ADAPTER_ASSOCIATION_KEY,
                             [NSValue valueWithPointer:this],
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [nativeWindow setMovable:YES];
    [nativeWindow setMovableByWindowBackground:YES];
}

void MacOSWindowAdapter::removeNativeDragBridge()
{
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    if ( !nativeWindow ) {
        return;
    }

    NSView* contentView = [nativeWindow contentView];
    if ( !contentView ) {
        return;
    }

    objc_setAssociatedObject(contentView,
                             &MACOS_WINDOW_ADAPTER_ASSOCIATION_KEY,
                             nil,
                             OBJC_ASSOCIATION_ASSIGN);
}

bool MacOSWindowAdapter::getCurrentNativeMouseDownPosition(double& cursorX,
                                                           double& cursorY) const
{
    NSWindow* nativeWindow = m_window ? glfwGetCocoaWindow(m_window) : nil;
    if ( !nativeWindow ) {
        return false;
    }

    NSEvent* currentEvent = [NSApp currentEvent];
    if ( !currentEvent || [currentEvent window] != nativeWindow ||
         [currentEvent type] != NSEventTypeLeftMouseDown ) {
        return false;
    }

    NSView* contentView = [nativeWindow contentView];
    if ( !contentView ) {
        return false;
    }

    const NSPoint windowPoint = [currentEvent locationInWindow];
    const NSPoint viewPoint = [contentView convertPoint:windowPoint fromView:nil];
    const NSRect  bounds    = [contentView bounds];
    const CGFloat localX    = viewPoint.x - bounds.origin.x;
    const CGFloat localY    = viewPoint.y - bounds.origin.y;
    if ( localX < 0.0 || localY < 0.0 || localX > bounds.size.width ||
         localY > bounds.size.height ) {
        return false;
    }

    cursorX = static_cast<double>(localX);
    cursorY = static_cast<double>(bounds.size.height - localY);
    return true;
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
}

}  // namespace MMM::Graphic

#endif
