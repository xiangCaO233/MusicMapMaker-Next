#include "ui/workspace/MobileWorkspaceModel.h"
#include "log/colorful-log.h"
#include <cmath>

namespace
{
/// @brief 推进非阻塞动画，不引入墙钟等待或依赖图形驱动。
/// @param model 当前测试独占的模型，无外部订阅和图形资源。
/// @details 合成一秒时间足够收敛，不把动画实现的具体速度当作接口。
/// 测试关心最终输入层级，允许以后调整视觉缓动曲线。
void settle(MMM::UI::MobileWorkspaceModel& model)
{
    // 使用合成帧而非墙钟等待，断言不受测试机速度和 GPU 状态影响。
    for ( int frame = 0; frame < 60; ++frame )
        model.update({}, 1.0f / 60.0f, false);
}
}  // namespace

/// @brief 验证启动收起、把手归属、遮罩关闭以及返回确认的状态边界。
/// @details 只测试纯模型；真实窗口层级另外在 Android 模拟器验收。
/// 所有触点使用工作区坐标，不依赖模拟器窗口在桌面上的摆放。
/// 每个手势都区分按下和释放，防止测试跳过关键的背景阻挡阶段。
/// @return 失败断言数非零时返回失败，不使用异常作为断言通道。
int main()
{
    using namespace MMM::UI;
    // 保留首个失败之后的检查，单次运行可以展示不同状态边界的回归。
    int  failures = 0;
    auto check    = [&](bool valid, const char* message) {
        if ( !valid ) {
            ++failures;
            XERROR("Mobile workspace: {}", message);
        }
    };
    MobileWorkspaceModel model;
    // 独立于 ImGui 创建模型，输入归属的核心不需要平台窗口支持。
    model.resize(1280, 720, 1);
    check(model.drawer() == MobileDrawer::None && !model.blocksCanvasInput(),
          "startup is collapsed");
    // 把手提供完整手指命中面积，但避开操作系统最外侧返回区。
    const auto handle = model.leftHandle();
    check(handle.m_width >= 48 && handle.m_height >= 48 && handle.m_x >= 12,
          "touch target and back edge are separate");
    const float x = handle.m_x + handle.m_width * .5f;
    const float y = handle.m_y + handle.m_height * .5f;
    model.update({ x, y, true, true, false }, 1.0f / 60, false);
    // 在释放前就检查保护，防止第一帧已经往背景画布落下物件。
    check(model.blocksCanvasInput(), "handle down blocks canvas immediately");
    // 零位移释放代表轻点，与后续直接跟随手指的拖动语义分开。
    model.update({ x, y, false, false, true }, 1.0f / 60, false);
    settle(model);
    // 收敛后检查目标，而非依赖某个中间帧的浮点舍入结果。
    check(model.drawer() == MobileDrawer::Left && model.fraction() > .99f,
          "tap opens left drawer");
    // 左抽屉宽度小于视口，选取确定落在其外侧的坐标进行关闭。
    // 遮罩按下必须先关闭两级抽屉，该次按下与松开不能落到画布。
    model.update({ 1100, 300, true, true, false }, 1.0f / 60, false);
    check(model.blocksCanvasInput(), "outside down is consumed");
    model.update({ 1100, 300, false, false, true }, 1.0f / 60, false);
    // 松手帧需要继续阻挡，下一帧才可建立新的画布手势。
    check(model.blocksCanvasInput(), "outside release is consumed");
    // 收起动画没有隐藏的同步等待，继续喂入空帧即可回到初始状态。
    settle(model);
    check(model.drawer() == MobileDrawer::None && !model.blocksCanvasInput(),
          "closed drawer releases next gesture");
    // 原始画布区域不是抽屉触发区，普通绘制不能意外拉开抽屉。
    model.update({ 640, 300, true, true, false }, 1.0f / 60, false);
    // 起点位于画布中央，即使水平移动也不能被当成边缘手势。
    model.update({ 800, 300, false, false, true }, 1.0f / 60, false);
    check(model.drawer() == MobileDrawer::None,
          "canvas drag does not open drawer");
    model.update({}, 1.0f / 60, false);
    // 返回手势必须从外缘开始，不复用普通画布拖动的中间坐标。
    // 三键导航环境的边缘回退与原生返回请求使用相同的确认状态。
    model.update({ 2, 200, true, true, false }, 1.0f / 60, false);
    model.update({ 110, 200, false, false, true }, 1.0f / 60, false);
    // 只检查出现确认，而不是用退出请求代替确认窗口状态。
    check(model.exitConfirmation() && !model.takeExitRequest(),
          "edge back asks instead of exiting");
    model.finishExitConfirmation(false);
    // 取消只结束确认状态，绝不能留下下一帧可消费的退出意图。
    settle(model);
    check(!model.exitConfirmation() && !model.takeExitRequest(),
          "cancel returns to editor");
    model.requestBack();
    // 直接返回请求模拟系统回调，边缘手势和平台入口必须共用结果。
    // 正常关闭通道只消费一次确认，避免重复发布原生关闭事件。
    model.finishExitConfirmation(true);
    check(model.takeExitRequest() && !model.takeExitRequest(),
          "confirmed exit is consumed once");
    settle(model);
    // 高层业务模态出现时，把手不能越过其输入所有权。
    model.update({ x, y, true, true, false }, 1.0f / 60, true);
    // 被业务模态遮住时，相同把手坐标也不能取得手势所有权。
    check(model.drawer() == MobileDrawer::None, "business modal has priority");
    model.resize(320, 720, 1);
    // 小屏展开宽度保留外侧遮罩，否则只能依赖系统返回才能关闭。
    check(model.drawerWidth() < 320 && model.drawerWidth() > 240,
          "portrait keeps outside dismissal area");
    // 触点丢失按取消处理，不能因为位移为零而误识别成点击。
    MobileWorkspaceModel cancelled;
    // 单独实例避免前一个测试的释放保护掩盖取消行为。
    cancelled.resize(1280, 720, 1);
    cancelled.update({ x, y, true, true, false }, 1.0f / 60, false);
    cancelled.update({}, 1.0f / 60, false);
    settle(cancelled);
    check(cancelled.drawer() == MobileDrawer::None,
          "cancelled touch does not tap open");
    // 与左抽屉相同的返回层级也适用于右侧开关抽屉。
    const auto right = cancelled.rightHandle();
    // 镜像坐标从模型获取，不在测试中复制右把手的布局公式。
    const float rx = right.m_x + right.m_width * .5f;
    cancelled.update({ rx, y, true, true, false }, 1.0f / 60, false);
    cancelled.update({ rx, y, false, false, true }, 1.0f / 60, false);
    settle(cancelled);
    check(cancelled.drawer() == MobileDrawer::Right,
          "right handle opens controls");
    cancelled.requestBack();
    // 先消费抽屉层级，不能第一次返回就跨过它询问退出。
    settle(cancelled);
    check(cancelled.drawer() == MobileDrawer::None &&
              !cancelled.exitConfirmation(),
          "back closes drawer before asking exit");
    // 测试进程正常返回，不通过终止进程或实际关闭 Activity 验证模型。
    // 真实退出按钮和 JNI 桥接留给模拟器交互验证。
    return failures == 0 ? 0 : 1;
}
