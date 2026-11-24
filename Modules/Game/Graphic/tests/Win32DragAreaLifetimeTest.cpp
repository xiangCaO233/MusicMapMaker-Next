#include "graphic/glfw/window/adapters/Win32WindowAdapter.h"

#include "event/core/EventBus.h"
#include "event/ui/UpdateDragAreaEvent.h"
#include "log/colorful-log.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <chrono>
#include <future>
#include <memory>

namespace
{
/// @brief 通过真实 HWND 子类过程检查指定客户区位置的命中类型。
/// @param window 测试持有的隐藏 GLFW 窗口。
/// @param x 客户区横坐标。
/// @param y 客户区纵坐标。
/// @return 窗口过程返回的 WM_NCHITTEST 值。
LRESULT hitTest(GLFWwindow* window, int x, int y)
{
    const HWND hwnd = glfwGetWin32Window(window);
    POINT      point{ x, y };
    // Win32 要求屏幕坐标，窗口可能被放在主屏幕原点之外。
    ClientToScreen(hwnd, &point);
    // 同步发给本用例的 HWND，不需要进程外的输入注入或桌面焦点。
    return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
}

/// @brief 让旧订阅随事件快照保留至窗口重建之后，再检查当前窗口命中。
/// @details 在创建线程销毁适配器和 HWND；发布线程只负责事件总线回调。
/// 测试通过同一个事件类型登记门闩，以确定顺序而非等待调度碰撞。
/// @return 新窗口只采用重建后发布的拖拽区和排除区时返回真。
bool testInFlightDragAreas()
{
    using namespace MMM::Event;
    using MMM::Graphic::Win32WindowAdapter;
    using WindowPtr = std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)>;

    // 两个窗口都经 GLFW 创建，销毁路径与生产适配器使用相同的 HWND。
    WindowPtr window(glfwCreateWindow(480, 320, "Old", nullptr, nullptr),
                     glfwDestroyWindow);
    // 初始化失败时没有可测试的真实子类过程，必须报告失败。
    if ( !window ) return false;

    // 事件直接进入应用共用单例，避免测试自行实现分发器。
    auto& bus = EventBus::instance();
    // 第一个信号确认分发已经启动，第二个信号只负责解除测试门闩。
    std::promise<void> snapshotTaken;
    std::promise<void> releaseSnapshot;
    auto               snapshot = snapshotTaken.get_future();
    auto               release  = releaseSnapshot.get_future();
    const auto         blocker =
        bus.subscribe<UpdateDragAreaEvent>([&](const UpdateDragAreaEvent&) {
            // 该订阅先于适配器登记，分发执行到此处时已冻结全部订阅。
            snapshotTaken.set_value();
            release.wait();
        });

    // 适配器后于 GLFW 窗口构造，也必须早于窗口销毁。
    auto adapter = std::make_unique<Win32WindowAdapter>(window.get());
    UpdateDragAreaEvent obsolete;
    // 故意让旧区与新区不相交，使两次事件即使都执行也能区分归属。
    obsolete.areas.push_back({ 30, 30, 200, 30 });
    // 明确启动发布线程，窗口创建线程才能在回调停留时重建窗口。
    auto publishing =
        std::async(std::launch::async, [&] { bus.publish(obsolete); });
    // 超时代表夹具未到达预期边界，不能用最后的命中成功掩盖错误。
    const bool started =
        snapshot.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if ( started ) {
        // 保持旧发布挂起，先移除适配器的子类过程和订阅，再销毁 HWND。
        // 重建后拥有全新的状态，即使分配器复用旧地址也不能接收旧消息。
        adapter.reset();
        // 独立销毁旧 GLFW 窗口，新的构造在旧 HWND 寿命结束后才开始。
        window.reset();
        window.reset(glfwCreateWindow(480, 320, "Current", nullptr, nullptr));
        if ( window )
            adapter = std::make_unique<Win32WindowAdapter>(window.get());
    }
    // 无论是否超时都放行线程，保证最后一次旧回调出栈后才销毁门闩。
    // 门闩属于旧订阅快照；先注销可避免之后的正常发布进入测试拦截。
    bus.unsubscribe<UpdateDragAreaEvent>(blocker);
    releaseSnapshot.set_value();
    // 等待旧分发彻底完成，使检查新窗口前不存在异步写入。
    publishing.get();
    if ( !started || !window ) return false;

    // 区域内部且远离边框，旧事件如果被错误投递会把客户区变成标题栏。
    if ( hitTest(window.get(), 80, 40) != HTCLIENT ) return false;
    UpdateDragAreaEvent current;
    // 允许区和控件覆盖区来自同一个新事件，验证矩形对整体可见。
    current.areas.push_back({ 30, 100, 200, 30 });
    // 排除区完全落在允许区内，保证能验证排除优先级。
    current.blockedAreas.push_back({ 70, 100, 20, 30 });
    // 生产回调不需要窗口轮询即可消费本轮布局，命中结果应立即更新。
    bus.publish(current);
    // 命中全部由生产窗口过程决定：旧区无效，新区生效，控件排除优先。
    return hitTest(window.get(), 80, 40) == HTCLIENT &&
           hitTest(window.get(), 110, 110) == HTCAPTION &&
           hitTest(window.get(), 80, 110) == HTCLIENT;
}
}  // namespace

/// @brief 用原生 GLFW 窗口覆盖订阅析构和重建，无需渲染设备。
int main()
{
    if ( !glfwInit() ) {
        XERROR("Could not initialize GLFW for the Win32 drag area test");
        return 1;
    }
    // 隐藏窗口不抢焦点，也不创建图形上下文或发送系统级鼠标输入。
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    const bool passed = testInFlightDragAreas();
    // 函数返回时测试窗口和适配器已释放，最后才清理 GLFW 全局状态。
    glfwTerminate();
    if ( !passed )
        XERROR("In-flight drag event reached the wrong Win32 window");
    return passed ? 0 : 1;
}
