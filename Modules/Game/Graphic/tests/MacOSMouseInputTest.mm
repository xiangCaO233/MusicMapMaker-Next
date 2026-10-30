#include "graphic/glfw/window/adapters/MacOSWindowUtils.h"
#define GLFW_EXPOSE_NATIVE_COCOA
#import <AppKit/AppKit.h>
#include <GLFW/glfw3native.h>
#include <backends/imgui_impl_glfw.h>
#include <cstdio>
#include <imgui.h>
#include <imgui_internal.h>
#include <initializer_list>

namespace
{
/// @file MacOSMouseInputTest.mm
/// @brief 经 AppKit 和 GLFW 真实派发，检查快速移动后的关闭按钮。
///
/// 单独向 ImGuiIO 注入按键会绕过发生故障的 Cocoa/GLFW 回调链。
/// 本例只给自己的原生窗口投递事件，不调用全局输入或读取用户配置。
/// 浮窗和 Dock 节点叉号分别覆盖客户区/桌面坐标、短按/跨帧释放。
/// 断言 open 状态和原始边沿数，悬浮高亮不能代替实际关闭成功。
/// 不建立 Vulkan device；生成的 DrawData 只用于完成 UI 状态更新。
/// 无辅助功能或截屏授权要求，可使用原生 Mac Runner 的桌面会话。
/// 每个场景重建 ImGui context，字体、ini 和输入历史不跨场景共享。
/// 全部循环有固定帧数上限，不用墙钟延迟掩盖事件排序错误。
///
/// @brief 当前场景实际收到的按键边沿数量；位置同步不可重复生成按键。
int edgeCount = 0;
/// @brief 当前场景从有效 Cocoa 按键事件同步位置的次数。
int positionCount = 0;

/// @brief 与 NativeWindow 相同，用户按键回调先同步位置，再返回后端排入边沿。
/// @param window GLFW 正常派发本次原生事件所关联的窗口。
/// @note 原始边沿只计数，不在测试中手工再次调用 ImGui 按键回调。
void onMouseButton(GLFWwindow* window, int, int, int)
{
    // 与生产回调共享同一位置入口，不能在测试里预先排入正确坐标。
    // 边沿只在后端原回调中提交，计数可以捕捉重复模拟点击。
    double x = 0, y = 0;
    if ( MMM::Graphic::syncMacOSMouseButtonPosition(window, x, y) )
        ++positionCount;
    ++edgeCount;
}

/// @brief 将逻辑客户区点转换为 NSEvent 所需的窗口坐标，投递真实按键事件。
/// @param pressed 按下或释放；不直接修改 ImGui 的 MouseDown 数组。
/// @param window 当前场景拥有的原生窗口。
/// @param point 相对内容视图左上角的目标逻辑点。
/// @note 采用实际内容视图的翻转属性，Retina 像素倍率不参与换算。
void postButton(GLFWwindow* window, ImVec2 point, bool pressed)
{
    NSWindow*    native = glfwGetCocoaWindow(window);
    NSView*      view   = native.contentView;
    const NSRect bounds = view.bounds;
    // 原生位置采用窗口坐标，而测试目标采用内容区左上角逻辑点。
    // 先尊重视图翻转，再转回窗口坐标，避免 Retina 或标题栏偏移混入。
    // 不能直接用 ImGui 的桌面坐标构造 NSEvent。
    NSPoint local = NSMakePoint(
        NSMinX(bounds) + point.x,
        view.flipped ? NSMinY(bounds) + point.y : NSMaxY(bounds) - point.y);
    const NSPoint location = [view convertPoint:local toView:nil];
    NSEvent*      event =
        [NSEvent mouseEventWithType:pressed ? NSEventTypeLeftMouseDown
                                            : NSEventTypeLeftMouseUp
                           location:location
                      modifierFlags:0
                          timestamp:0
                       windowNumber:native.windowNumber
                            context:nil
                        eventNumber:1
                         clickCount:1
                           pressure:pressed ? 1 : 0];
    // 从正常队尾进入 NSApplication，必须由 glfwPollEvents 调用内容视图。
    // 直接调用 mouseDown: 无法验证 NSApp.currentEvent 的生产取值路径。
    [NSApp postEvent:event atStart:NO];
}

/// @brief 提交浮动窗口或真实 DockSpace 中的可关闭窗口，不替换原生叉号。
/// @param open 跨帧持久保存的关闭状态，不能每帧重新初始化为 true。
/// @param docked 是否创建停靠宿主并将测试窗口放入同一个 Dock 节点。
/// @param parkPointer 是否固定先前 UI 位置，以暴露按键先于新位置入队的错误。
/// @note 正式后端 NewFrame 与平台窗口更新顺序和应用一致。
void frame(bool& open, bool docked, bool parkPointer = false)
{
    glfwPollEvents();
    ImGui_ImplGlfw_NewFrame();
    if ( parkPointer ) {
        // 固定按下前的 UI 位置，模拟尚未收到最新位置的上一帧快照。
        // 不预先向目标点注入位置；同步必须由真实 Cocoa 按键回调完成。
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::GetIO().AddMousePosEvent(viewport->Pos.x + 450,
                                        viewport->Pos.y + 400);
    }
    ImGui::NewFrame();
    auto*   viewport = ImGui::GetMainViewport();
    ImGuiID dock     = 0;
    if ( docked ) {
        // 宿主先于子窗口提交，避免无关的 DockSpace 生命周期影响关闭结果。
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos({ viewport->Pos.x + 20, viewport->Pos.y + 20 });
        ImGui::SetNextWindowSize({ 360, 240 });
        ImGui::Begin(
            "host",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        // 相同种子跨帧保持 Dock 节点身份，叉号目标不能随帧重新分配。
        // 不手工调用 CloseButton，必须验证真实停靠窗口提交的按钮。
        dock = ImGui::GetID("dock");
        ImGui::DockSpace(dock);
        ImGui::End();
    }
    // 关闭状态跨帧保留；若每帧重置 open，会把成功关闭误判为仍可见。
    // 不再提交目标时 ImGui 自行回收可见状态，测试不手工隐藏窗口。
    if ( open ) {
        ImGui::SetNextWindowViewport(viewport->ID);
        if ( docked )
            ImGui::SetNextWindowDockID(dock, ImGuiCond_Always);
        else {
            ImGui::SetNextWindowPos(
                { viewport->Pos.x + 20, viewport->Pos.y + 20 });
            ImGui::SetNextWindowSize({ 300, 200 });
        }
        ImGui::Begin("close target", &open, ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("native close button regression");
        ImGui::End();
    }
    // 只完成 UI 状态和主视口定位，不提交任何 GPU 渲染或系统截图请求。
    ImGui::Render();
    if ( ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable )
        ImGui::UpdatePlatformWindows();
}

/// @brief 检查一个关闭手势，同时验证每个 Cocoa 边沿只交付一次。
/// @param window 全部场景共享且仍然存活的原生客户区。
/// @param viewports 是否启用与应用默认相同的桌面坐标。
/// @param docked 是否点击 Dock 节点右侧叉号，而非普通浮动窗口叉号。
/// @param quick 按下与释放是否在同一批原生事件中提交。
/// @return 已关闭且位置同步及边沿均恰好两次时为真。
/// @note 指针在最后一帧仍停留在控件外，不能让后端提前轮询目标位置。
/// @note 短按使用默认输入队列节流，不通过关闭节流让场景通过。
bool scenario(GLFWwindow* window, bool viewports, bool docked, bool quick)
{
    edgeCount = positionCount = 0;
    glfwSetMouseButtonCallback(window, onMouseButton);
    // GLFW callback 先安装用户回调，再让后端接管并链式调用，顺序与应用一致。
    // 每轮独立的 context 防止前一轮 ActiveId 或输入队列影响结果。
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    // 禁止套用磁盘上的停靠位置，否则目标几何会依赖 Runner 的历史状态。
    // 本例固定内部控件几何，但不假设原生窗口位于特定屏幕坐标。
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if ( viewports ) {
        // 没有次级窗口渲染；这里只需要主窗口的屏幕坐标能力。
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    }
    ImGui_ImplGlfw_InitForVulkan(window, true);
    // 没有 Vulkan renderer 上传字体，仍须建立图集供标准控件计算标题栏。
    // 只使用内置字体，避免资源下载、字体替换或用户 DPI 配置改变几何。
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    // 建立可见窗口及稳定几何，不靠 sleep 等待任意墙钟时长。
    NSWindow* native = glfwGetCocoaWindow(window);
    [NSApp activateIgnoringOtherApps:YES];
    [native makeKeyAndOrderFront:nil];
    glfwSetCursorPos(window, 450, 400);
    bool open = true;
    for ( int i = 0; i < 4; ++i ) frame(open, docked, i == 3);
    auto* target = ImGui::FindWindowByName("close target");
    // Dock 节点尚未建立时记录失败，不能退回浮窗位置让用例假通过。
    // 结果在销毁资源前取得，不能把销毁窗口当成叉号关闭成功。
    bool valid =
        target && (!docked || (target->DockNode && target->DockNode->TabBar));
    if ( valid ) {
        // 从实际窗口/标签栏取得叉号中心，不能把测试固定到某个屏幕分辨率。
        const float font  = ImGui::GetFontSize();
        const auto& style = ImGui::GetStyle();
        ImVec2      point;
        if ( docked ) {
            const auto* node = target->DockNode;
            point = { node->Pos.x + node->Size.x - style.FramePadding.x -
                          font * .5f,
                      node->TabBar->BarRect.GetCenter().y };
        } else
            point = { target->Pos.x + target->Size.x - style.FramePadding.x -
                          font * .5f,
                      target->Pos.y + target->TitleBarHeight * .5f };
        const ImVec2 origin = ImGui::GetMainViewport()->Pos;
        point               = { point.x - origin.x, point.y - origin.y };
        // 系统指针刚到目标时立即按下，不能提前推进一帧替被测代码同步位置。
        glfwSetCursorPos(window, point.x, point.y);
        postButton(window, point, true);
        if ( quick ) postButton(window, point, false);
        frame(open, docked);
        if ( !quick ) postButton(window, point, false);
        // 同批按下/松开可能被默认节流拆为两帧，保留后续帧消费完整手势。
        // 不补发遗漏边沿；原生事件未到达也必须失败，不能靠重试掩盖。
        for ( int i = 0; i < 6; ++i ) frame(open, docked);
        // 每条原生边沿都应同步一次位置；位置修复不应新增或重复按键事件。
        // 关闭状态和派发计数必须同时通过，不能用其中之一代替整个手势。
        valid = !open && edgeCount == 2 && positionCount == 2;
    }
    std::fprintf(stderr,
                 "MacOSMouseInputTest: viewports=%d docked=%d quick=%d open=%d "
                 "edges=%d positions=%d result=%d\n",
                 viewports,
                 docked,
                 quick,
                 open,
                 edgeCount,
                 positionCount,
                 valid);
    // 后端依然拥有自己的 callbacks；退出时按生产顺序配对回收。
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    return valid;
}
}  // namespace

/// @brief 仅在 macOS 运行真实原生事件测试，八组手势均不得丢失或重复。
int main()
{
    if ( !glfwInit() ) return 1;
    // 八个场景共享唯一客户区；短时间反复销毁/创建 NSWindow 会让系统队列
    // 尚未完成窗口切换。复用窗口让本例只验证输入排序，不测窗口管理器时序。
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    GLFWwindow* window =
        glfwCreateWindow(640, 480, "MMM input regression", nullptr, nullptr);
    if ( !window ) {
        glfwTerminate();
        return 1;
    }
    MMM::Graphic::enableMacOSFirstMouse(window);
    bool ok = true;
    for ( bool viewports : { false, true } )
        for ( bool docked : { false, true } )
            for ( bool quick : { false, true } )
                ok = scenario(window, viewports, docked, quick) && ok;
    // 场景后端均已关闭，最后再销毁共享客户区，保持所有 callback 句柄有效。
    // GLFW 生命周期仅配对一次，避免窗口切换时序成为测试的隐含前置条件。
    glfwDestroyWindow(window);
    glfwTerminate();
    return ok ? 0 : 1;
}
