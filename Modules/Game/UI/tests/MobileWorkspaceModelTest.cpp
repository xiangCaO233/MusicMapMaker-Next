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

/// @brief 同一组断言镜像验证两侧抽屉，避免右侧手势沿用左侧方向。
/// @param side 本次检查的把手和抽屉方向。
/// @details 测试连续拖动、纵向误触、业务内容归属和标题反向收起。
/// 不检查绘制像素，模型需要保证的是输入层级及可立即读取的展开量。
/// @return 任一归属、实时展开量或最终状态不符合契约时返回 false。
bool verifyDrawerGestures(MMM::UI::MobileDrawer side)
{
    using namespace MMM::UI;
    MobileWorkspaceModel model;
    model.resize(1280, 720, 1);
    const auto handle =
        side == MobileDrawer::Left ? model.leftHandle() : model.rightHandle();
    const float x      = handle.m_x + handle.m_width * .5f;
    float       y      = handle.m_y + handle.m_height * .5f;
    const float inward = side == MobileDrawer::Left ? 1.0f : -1.0f;
    // 80 像素显著超过触摸抖动区，必须被识别为纵向意图。
    // 释放仍保留同一位置，避免由回跳坐标掩盖误触判断。
    // 纵向划过把手不能在松手时变成一次轻点打开。
    // 方向锁定后释放只结束移动，不触发抽屉打开动作。
    // 后续通过新位置的横向拖动同时验证两种语义不会互相覆盖。
    model.update({ x, y, true, true, false }, .016f, false);
    model.update({ x, y + 80, false, true, false }, .016f, false);
    model.update({ x, y + 80, false, false, true }, .016f, false);
    settle(model);
    if ( model.drawer() != MobileDrawer::None || model.blocksCanvasInput() )
        return false;
    // 纵向拖动现在移动把手；后续横向操作必须在新位置继续生效。
    const auto moved =
        side == MobileDrawer::Left ? model.leftHandle() : model.rightHandle();
    if ( std::abs(moved.m_y - handle.m_y - 80) > .01f ) return false;
    y = moved.m_y + moved.m_height * .5f;
    // 对侧把手的归一化中心独立，不应受到本次拖动影响。
    const auto peer =
        side == MobileDrawer::Left ? model.rightHandle() : model.leftHandle();
    if ( std::abs(peer.m_y - handle.m_y) > .01f ) return false;
    // 确认移动帧立即更新展开比例，没有等松手或动画结束才变化。
    // 镜像方向只影响位移符号，比例必须对两侧相同。
    model.update({ x, y, true, true, false }, .016f, false);
    model.update({ x + inward * 160, y, false, true, false }, .016f, false);
    // 160 像素足以越过展开阈值，但尚未到全开，能够检测延迟更新。
    // 允许小幅浮点误差，不把测试绑定到某一种计算舍入顺序。
    if ( std::abs(model.fraction() - 160 / model.drawerWidth()) > .001f )
        return false;
    model.update({ x + inward * 160, y, false, false, true }, .016f, false);
    settle(model);
    if ( model.drawer() != side || model.fraction() < .99f ) return false;
    // 内容区开始的水平移动不能接管为关闭手势，文件选择仍归业务视图。
    // 故意让终点离开内容区，确保判定只依据起点。
    const auto  rect     = model.drawerRect();
    const float contentX = rect.m_x + rect.m_width * .5f;
    // 内容坐标低于 72dp 标题，不能触发专属标题拖动。
    // 中间没有额外按下沿，因此滑出内容后仍属于原业务触摸。
    model.update({ contentX, 200, true, true, false }, .016f, false);
    model.update(
        { contentX - inward * 200, 200, false, false, true }, .016f, false);
    settle(model);
    if ( model.drawer() != side || model.fraction() < .99f ) return false;
    // 标题只微移后释放应回弹打开，不因手指抖动意外关闭内容。
    // 取拖动区中央坐标，避开独立收起按钮的排除区。
    const auto  grip = model.drawerGrip();
    const float gx   = grip.m_x + grip.m_width * .5f;
    const float gy   = grip.m_y + grip.m_height * .5f;
    model.update({ gx, gy, true, true, false }, .016f, false);
    model.update({ gx - inward * 16, gy, false, false, true }, .016f, false);
    // 16 像素已经超过抖动锁定区，实际检查的是短拖动回弹而不是轻点。
    settle(model);
    if ( model.drawer() != side || model.fraction() < .99f ) return false;
    // 明确反向拖过四分之一宽度才收起，连续移动时仍实时跟随触点。
    // 关闭当帧保留画布保护，收敛后才允许下一次编辑。
    model.update({ gx, gy, true, true, false }, .016f, false);
    model.update({ gx - inward * 200, gy, false, true, false }, .016f, false);
    // 200 像素尚未拉到屏幕边界，比例断言可发现只在终点刷新布局的问题。
    if ( model.fraction() > .6f || !model.blocksCanvasInput() ) return false;
    model.update({ gx - inward * 200, gy, false, false, true }, .016f, false);
    settle(model);
    return model.drawer() == MobileDrawer::None && !model.blocksCanvasInput();
}

/// @brief 验证中断不会遗留捕获，业务模态和旋转也不会伪造点击。
/// @details 这些场景没有可靠释放沿，不能用正常松手流程代替取消。
/// 每个中断后重新开始普通画布触摸，确认模型已释放手势所有权。
bool verifyGestureCancellation()
{
    using namespace MMM::UI;
    MobileWorkspaceModel model;
    model.resize(1280, 720, 1);
    // 返回触点取消后不能永久拦住画布，也不能弹出退出确认。
    model.update({ 2, 200, true, true, false }, .016f, false);
    // 下一次空采样模拟平台取消，没有人工补造一个正常释放事件。
    model.update({}, .016f, false);
    settle(model);
    if ( model.blocksCanvasInput() || model.exitConfirmation() ) return false;
    // 右侧向外移动只是一种无效返回，不能使用绝对位移判定退出。
    // 下一次反向向内滑才产生退出意图。
    model.update({ 1278, 200, true, true, false }, .016f, false);
    model.update({ 1350, 200, false, false, true }, .016f, false);
    // 终点故意越过视口，指针捕获流可能包含此类坐标，仍须检查方向。
    settle(model);
    if ( model.exitConfirmation() ) return false;
    model.update({ 1278, 200, true, true, false }, .016f, false);
    model.update({ 1170, 200, false, false, true }, .016f, false);
    if ( !model.exitConfirmation() ) return false;
    model.finishExitConfirmation(false);
    settle(model);
    // 高层模态中途接管时恢复拖动前的收起目标。
    // 关闭模态后不能拿旧坐标继续展开。
    const auto  handle = model.leftHandle();
    const float x      = handle.m_x + handle.m_width * .5f;
    const float y      = handle.m_y + handle.m_height * .5f;
    model.update({ x, y, true, true, false }, .016f, false);
    model.update({ x + 160, y, false, true, false }, .016f, false);
    model.update({ x + 160, y, false, true, false }, .016f, true);
    // 模态在已有可见展开量后出现，只有真正撤销目标才能通过后续断言。
    // 模态解除后的释放沿不应把此前的半开状态重新提交为打开。
    model.update({ x + 160, y, false, false, true }, .016f, false);
    settle(model);
    if ( model.drawer() != MobileDrawer::None || model.blocksCanvasInput() )
        return false;
    // 旋转同时改变坐标系和拖动分母，必须取消旧手势并回到原目标。
    model.update({ x, y, true, true, false }, .016f, false);
    model.update({ x + 160, y, false, true, false }, .016f, false);
    model.resize(720, 1280, 1);
    // 不使用旧方向坐标补释放，验证旋转入口本身已经清理捕获状态。
    settle(model);
    if ( model.drawer() != MobileDrawer::None || model.blocksCanvasInput() )
        return false;
    // 显式按钮与返回均能关闭抽屉，不需要等待一个额外的触摸释放。
    // 重新从模型取得竖屏把手，避免用横屏常量绕过几何更新。
    const auto  right = model.rightHandle();
    const float rx    = right.m_x + right.m_width * .5f;
    const float ry    = right.m_y + right.m_height * .5f;
    model.update({ rx, ry, true, true, false }, .016f, false);
    model.update({ rx, ry, false, false, true }, .016f, false);
    settle(model);
    // 非模态弹层也可以超出抽屉几何边界，点击应归弹层而不是背景遮罩。
    model.update({ 10, 200, true, true, false }, .016f, true);
    model.update({ 10, 200, false, false, true }, .016f, true);
    // 空帧推进后仍保持展开，确认没有遗留待执行的遮罩关闭意图。
    settle(model);
    if ( model.drawer() != MobileDrawer::Right ) return false;
    model.closeDrawer();
    // 显式收起没有触摸采样参数，动画及保护状态都必须由模型自行收尾。
    settle(model);
    return model.drawer() == MobileDrawer::None && !model.exitConfirmation();
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
    check(verifyDrawerGestures(MobileDrawer::Left),
          "left drawer gesture ownership");
    check(verifyDrawerGestures(MobileDrawer::Right),
          "right drawer gesture ownership");
    check(verifyGestureCancellation(),
          "cancelled and interrupted gestures release input");
    // 模拟新进程读取独立客户端值，恢复后仍保持抽屉收起。
    MobileWorkspaceModel restored;
    // 使用全新模型，避免前一手势的释放保护或动画状态参与恢复结果。
    restored.restoreHandleCenters(.2f, .8f);
    restored.resize(1280, 720, 1);
    check(restored.drawer() == MobileDrawer::None &&
              restored.leftHandle().m_y < restored.rightHandle().m_y,
          "restart restores independent handles without opening drawer");
    // 旋转只重新映射像素位置，不把归一化中心重置到屏幕正中。
    restored.resize(720, 1280, 1);
    // 先恢复后提供屏幕尺寸也必须有效，模拟配置早于平台窗口准备完成。
    // 只比较布局状态，不将打开抽屉的运行中手势写入持久化。
    check(restored.leftHandleCenter() == .2f &&
              restored.rightHandleCenter() == .8f &&
              restored.rightHandle().m_y + restored.rightHandle().m_height <=
                  1280,
          "rotation preserves restored positions inside screen");
    return failures == 0 ? 0 : 1;
}
