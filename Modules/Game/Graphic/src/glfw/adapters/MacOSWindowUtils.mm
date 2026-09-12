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
/// @brief 非最大化启动窗口占屏幕可见工作区的最大比例。
///
/// 预留的 10% 边界让用户仍能看到桌面和其他窗口，也避免标题栏紧贴菜单栏或
/// Dock；只限制上限，不放大本来更小的请求尺寸。
constexpr CGFloat STARTUP_VISIBLE_FRAME_RATIO = 0.9;

/// @brief 获取窗口所在屏幕，失败时回落到主屏幕。
///
/// 新建窗口在首次显示前可能尚未绑定 NSScreen，此时使用 mainScreen 仍能得到
/// 合理的初始可见工作区。两个来源都为空时由调用方保持原窗口状态。
///
/// @param nativeWindow macOS 原生窗口。
/// @return 可用屏幕；无法解析时返回 nil。
NSScreen* resolveWindowScreen(NSWindow* nativeWindow)
{
    // 优先使用窗口当前屏幕，保证多显示器环境中的最大化和居中不会跳到主屏幕。
    NSScreen* screen = nativeWindow ? [nativeWindow screen] : nil;
    if ( !screen ) {
        screen = [NSScreen mainScreen];
    }
    return screen;
}

/// @brief macOS 内容视图 first mouse 响应实现。
///
/// GLFW 的内容视图类通过 Objective-C runtime 安装此 IMP。函数不读取对象、选择器
/// 或事件，仅改变 AppKit 对非活跃窗口首击的接收决策。
///
/// @param self Objective-C 接收对象。
/// @param selector Objective-C 选择器。
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
///
/// GLFW 没有暴露与自定义窗框按钮完全对应的 Cocoa 操作，因此适配器直接取得
/// NSWindow。调用只提交 AppKit 最小化请求，不等待动画完成。
///
/// @param window GLFW 窗口句柄。
/// @return 成功请求最小化时返回 true。
bool miniaturizeMacOSWindow(GLFWwindow* window)
{
    // 所有桥接函数都把空 GLFW/NSWindow 视为无操作失败，避免向 Objective-C
    // 消息链传入不完整窗口并让上层误判操作已完成。
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
///
/// visibleFrame 已扣除菜单栏和 Dock，与覆盖整个 frame 的系统全屏不同；该操作
/// 用作项目无边框窗口的“最大化”状态，并立即刷新屏幕内容。
///
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

    // 屏幕选择与居中路径共用相同回退规则，保持多显示器行为一致。
    NSScreen* screen = resolveWindowScreen(nativeWindow);
    if ( !screen ) {
        return false;
    }

    // 禁用动画使 GLFW 尺寸回调尽快收到最终几何，交换链只需针对最终尺寸重建。
    [nativeWindow setFrame:[screen visibleFrame] display:YES animate:NO];
    return true;
}

/// @brief 将 macOS 原生窗口以留边尺寸居中到当前屏幕可见工作区。
///
/// 请求尺寸按 Cocoa 点解释，并分别钳制到可见工作区的固定比例；随后基于同一
/// visibleFrame 计算中心，因此能正确处理副屏幕的非零甚至负坐标原点。
///
/// @param window GLFW 窗口句柄。
/// @param requestedWidth 请求的内容宽度。
/// @param requestedHeight 请求的内容高度。
/// @return 成功设置窗口矩形时返回 true。
bool centerMacOSWindowInVisibleFrame(GLFWwindow* window, int requestedWidth,
                                     int requestedHeight)
{
    // 非正尺寸不是可恢复的窗口几何，交由调用方选择平台后备行为。
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

    // 宽高独立钳制，既保留请求的较小维度，也保证超大窗口四周都有可见余量。
    const NSRect visibleFrame = [screen visibleFrame];
    const CGFloat targetWidth =
        std::min<CGFloat>(static_cast<CGFloat>(requestedWidth),
                          visibleFrame.size.width * STARTUP_VISIBLE_FRAME_RATIO);
    const CGFloat targetHeight =
        std::min<CGFloat>(static_cast<CGFloat>(requestedHeight),
                          visibleFrame.size.height * STARTUP_VISIBLE_FRAME_RATIO);
    // Cocoa 使用左下原点；直接在 visibleFrame 坐标系内计算居中位置，不与 GLFW
    // 的窗口坐标约定混算。
    const CGFloat targetX =
        visibleFrame.origin.x + (visibleFrame.size.width - targetWidth) * 0.5;
    const CGFloat targetY =
        visibleFrame.origin.y + (visibleFrame.size.height - targetHeight) * 0.5;

    // 一次提交最终 frame，避免先 resize 再 move 产生两轮 framebuffer 重建请求。
    [nativeWindow setFrame:NSMakeRect(targetX, targetY, targetWidth, targetHeight)
                   display:YES
                   animate:NO];
    return true;
}

/// @brief 允许 macOS 非活跃窗口的第一次鼠标点击直接交给 GLFW 内容视图。
///
/// AppKit 默认可能只用首击激活窗口，编辑器则要求该点击同时完成画布操作。这里在
/// GLFW 内容视图的运行时类上替换 acceptsFirstMouse:，影响该进程中同类的全部
/// 内容视图，并开启 mouse-moved 事件供悬停反馈使用。
///
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

    // 方法安装在实际内容视图类上，而不是假设 GLFW 使用某个公开 Cocoa 子类名。
    NSView* contentView = [nativeWindow contentView];
    if ( !contentView ) {
        return false;
    }

    // object_getClass 返回对象的动态类，兼容 GLFW 未来更换内部 NSView 子类。
    Class contentViewClass = object_getClass(contentView);
    if ( !contentViewClass ) {
        return false;
    }

    // 类型编码与 BOOL acceptsFirstMouse:(NSEvent*) 签名匹配；替换是幂等的，多个
    // 窗口重复调用仍安装相同函数实现。
    class_replaceMethod(contentViewClass,
                        @selector(acceptsFirstMouse:),
                        reinterpret_cast<IMP>(acceptFirstMouse),
                        "c@:@");
    [nativeWindow setAcceptsMouseMovedEvents:YES];
    return true;
}

}  // namespace MMM::Graphic
