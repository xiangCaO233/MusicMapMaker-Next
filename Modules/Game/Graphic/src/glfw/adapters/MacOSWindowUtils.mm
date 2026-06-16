#include "graphic/glfw/window/adapters/MacOSWindowUtils.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>
#include <algorithm>
#include <objc/runtime.h>

namespace MMM::Graphic
{
namespace
{
/// @brief 非最大化启动窗口占屏幕可见区域的最大比例。
constexpr CGFloat STARTUP_VISIBLE_FRAME_RATIO = 0.9;

/// @brief 获取窗口所在屏幕，失败时回落到主屏幕。
/// @param nativeWindow macOS 原生窗口。
/// @return 可用屏幕；无法解析时返回 nil。
NSScreen* resolveWindowScreen(NSWindow* nativeWindow)
{
    NSScreen* screen = nativeWindow ? [nativeWindow screen] : nil;
    if ( !screen ) {
        screen = [NSScreen mainScreen];
    }
    return screen;
}

/// @brief macOS 内容视图 first mouse 响应实现。
/// @param self Objective-C 接收对象。
/// @param selector Objective-C selector。
/// @param event 鼠标事件。
/// @return 始终允许非活跃窗口首击进入内容视图。
BOOL acceptFirstMouse(id self, SEL selector, NSEvent* event)
{
    (void)self;
    (void)selector;
    (void)event;
    return YES;
}
}  // namespace

/// @brief 将 macOS 原生窗口最小化到 Dock。
/// @param window GLFW 窗口句柄。
/// @return 成功请求最小化时返回 true。
bool miniaturizeMacOSWindow(GLFWwindow* window)
{
    if ( !window ) {
        return false;
    }

    NSWindow* nativeWindow = glfwGetCocoaWindow(window);
    if ( !nativeWindow ) {
        return false;
    }

    [nativeWindow miniaturize:nil];
    return true;
}

/// @brief 将 macOS 原生窗口调整到当前屏幕的可见工作区。
/// @param window GLFW 窗口句柄。
/// @return 成功应用可见工作区窗口矩形时返回 true。
bool applyMacOSVisibleWindowFrame(GLFWwindow* window)
{
    if ( !window ) {
        return false;
    }

    NSWindow* nativeWindow = glfwGetCocoaWindow(window);
    if ( !nativeWindow ) {
        return false;
    }

    NSScreen* screen = resolveWindowScreen(nativeWindow);
    if ( !screen ) {
        return false;
    }

    [nativeWindow setFrame:[screen visibleFrame] display:YES animate:NO];
    return true;
}

/// @brief 将 macOS 原生窗口以留边尺寸居中到当前屏幕可见工作区。
/// @param window GLFW 窗口句柄。
/// @param requestedWidth 请求的内容宽度。
/// @param requestedHeight 请求的内容高度。
/// @return 成功设置窗口矩形时返回 true。
bool centerMacOSWindowInVisibleFrame(GLFWwindow* window, int requestedWidth,
                                     int requestedHeight)
{
    if ( !window || requestedWidth <= 0 || requestedHeight <= 0 ) {
        return false;
    }

    NSWindow* nativeWindow = glfwGetCocoaWindow(window);
    if ( !nativeWindow ) {
        return false;
    }

    NSScreen* screen = resolveWindowScreen(nativeWindow);
    if ( !screen ) {
        return false;
    }

    const NSRect visibleFrame = [screen visibleFrame];
    const CGFloat targetWidth =
        std::min<CGFloat>(static_cast<CGFloat>(requestedWidth),
                          visibleFrame.size.width * STARTUP_VISIBLE_FRAME_RATIO);
    const CGFloat targetHeight =
        std::min<CGFloat>(static_cast<CGFloat>(requestedHeight),
                          visibleFrame.size.height * STARTUP_VISIBLE_FRAME_RATIO);
    const CGFloat targetX =
        visibleFrame.origin.x + (visibleFrame.size.width - targetWidth) * 0.5;
    const CGFloat targetY =
        visibleFrame.origin.y + (visibleFrame.size.height - targetHeight) * 0.5;

    [nativeWindow setFrame:NSMakeRect(targetX, targetY, targetWidth, targetHeight)
                   display:YES
                   animate:NO];
    return true;
}

/// @brief 允许 macOS 非活跃窗口的第一次鼠标点击直接交给 GLFW 内容视图。
/// @param window GLFW 窗口句柄。
/// @return 成功启用 first mouse 支持时返回 true。
bool enableMacOSFirstMouse(GLFWwindow* window)
{
    if ( !window ) {
        return false;
    }

    NSWindow* nativeWindow = glfwGetCocoaWindow(window);
    if ( !nativeWindow ) {
        return false;
    }

    NSView* contentView = [nativeWindow contentView];
    if ( !contentView ) {
        return false;
    }

    Class contentViewClass = object_getClass(contentView);
    if ( !contentViewClass ) {
        return false;
    }

    class_replaceMethod(contentViewClass,
                        @selector(acceptsFirstMouse:),
                        reinterpret_cast<IMP>(acceptFirstMouse),
                        "c@:@");
    [nativeWindow setAcceptsMouseMovedEvents:YES];
    return true;
}

}  // namespace MMM::Graphic
