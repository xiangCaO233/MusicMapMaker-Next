#include "presentation/mobile/MobileCanvasNavigation.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include <cmath>

namespace
{
int failures{};  ///< 汇总所有独立手势场景，不因首个失败跳过后续路径。
/// @brief 断言用户可观察到的手势语义，不读取状态机私有成员。
void check(bool condition, const char* message)
{
    if ( !condition ) {
        XERROR("{}", message);
        ++failures;
    }
}
/// @brief 固定像素坐标断言容忍浮点归一化的舍入误差。
bool near(float a, float b)
{
    return std::abs(a - b) < .001f;
}
/// @brief 使用真实输入跟踪器建立双指手势，不直接伪造内部阶段。
/// @details 设备 ID 固定，手指 ID 不依赖当前数组索引。
/// 先平移再捏合，验证二者能在同一代际连续执行。
/// 全部输入只在内存中生成，不访问手机、文件或图形后端。
/// @note 单指等待阶段不产生相机命令，第二指加入时仍使用原视图基线。
/// 使用非零纵坐标和不相等的两点横坐标，排除默认零值偶然通过。
/// 第一段只平移，中点改变而指距不变，因此缩放必须保持一倍。
/// 第二段只改变指距，验证没有额外引入平移。
/// 回程检查绝对倍率，能发现每帧叠乘导致的漂移。
/// 导航结束在第一指抬起时提交，剩余手指仍属于相同原始代际。
/// 结束后的移动不能重启导航，也不能生成历史动作。
/// 三个边沿分别读取生产返回值，不用复制状态机条件推导期望值。
/// 像素容差只处理浮点舍入，不把错误位移放宽到整个触控半径。
/// 输入按时间顺序写入，测试不假定平台一定逐条渲染每次 Motion。
void testNavigation()
{
    using namespace MMM;
    using P = Graphic::TouchPhase;
    Graphic::MobileTouchTracker    tracker;
    Canvas::MobileCanvasNavigation navigation;
    const auto                     step = [&] {
        return navigation.update(tracker.snapshot(), true, 1000, 1000, 1, 10);
    };
    // 真实跟踪器负责身份与归一化坐标，导航器只解释其已发布结果。
    // 每次调用均发生在同一线程，不需要模拟锁或跨线程睡眠。
    tracker.update(P::Down, 1, 8, .2f, .4f, 0);
    check(!step().m_changed, "one finger does not navigate");
    tracker.update(P::Down, 1, 9, .4f, .4f, .03);
    check(!step().m_changed, "second finger arrival does not jump");
    // 两指同向同距移动；缩放必须保持一倍，不能用某一个触点作为平移中心。
    tracker.update(P::Move, 1, 8, .25f, .5f, .05);
    tracker.update(P::Move, 1, 9, .45f, .5f, .05);
    // 固定期望像素来自输入场景，不重复生产实现中的中点计算代码。
    auto delta = step();
    check(delta.m_changed && near(delta.m_x, 50) && near(delta.m_y, 100) &&
              near(delta.m_zoom, 1),
          "two-finger pan follows centroid");
    check(near(delta.m_anchorX, 300) && near(delta.m_anchorY, 400),
          "anchor belongs to previous centroid");
    // 中点保持不动时张开到两倍距离，只改变时间线缩放。
    tracker.update(P::Move, 1, 8, .15f, .5f, .08);
    tracker.update(P::Move, 1, 9, .55f, .5f, .08);
    delta = step();
    check(near(delta.m_zoom, 2) && near(delta.m_x, 0) && near(delta.m_y, 0),
          "pinch scales around stationary midpoint");
    // 回到原始距离应回到一倍，防止逐帧乘法产生累计误差。
    tracker.update(P::Move, 1, 8, .25f, .5f, .11);
    tracker.update(P::Move, 1, 9, .45f, .5f, .11);
    check(near(step().m_zoom, 1),
          "pinch return does not accumulate zoom drift");
    // 抬起前记录的倍率仍随结束结果交付，收尾不能退回配置初始倍率。
    tracker.update(P::Up, 1, 8, .25f, .5f, .13);
    check(step().m_finished, "first finger release commits navigation once");
    // 剩余一指移动不属于新的单指编辑，也不能继续推进双指导航。
    tracker.update(P::Move, 1, 9, .55f, .6f, .14);
    check(!step().m_changed && !navigation.active(),
          "remaining finger cannot navigate");
    tracker.update(P::Up, 1, 9, .55f, .6f, .15);
    delta = step();
    check(!delta.m_finished && !delta.m_undo && !delta.m_redo,
          "completed movement cannot become history tap");
}
/// @brief 多触点全部在一个 UI 帧内发生时仍只产生一次历史动作。
/// @details 二指撤销、三指重做只有全抬起才成立。
/// 不能凭当前触点数为零就丢失手势峰值，也不能在下一帧重复动作。
/// 计时来自输入事件，无测试 sleep 或真实时钟依赖。
/// @note 只在峰值为二或三时执行历史，单指轻点归普通编辑工具。
/// 中途只剩一根手指也必须等待，不能依据即时数量分类。
/// 二指和三指分别创建独立状态机，避免前一结果掩盖重复提交。
/// 同一结束快照连续读取两次，检测空帧重放造成的多次撤销。
/// 第三指加入后整个手势保持三指身份，不能在释放时退回二指撤销。
/// 事件间隔刻意短于轻触上限，不依赖主机 CPU 的执行速度。
/// 往返移动只向状态机交付最终快照，覆盖一帧内完整经过边界的情况。
/// 跟踪器保存的最大位移必须能否决最终位置看似静止的假轻触。
/// 测试不检查私有枚举，允许以后重构实现而保持相同用户行为。
/// 这里只检查意图，历史栈执行与权限由实际业务控制器回归验证。
void testHistory()
{
    using namespace MMM;
    using P = Graphic::TouchPhase;
    for ( int count : { 2, 3 } ) {
        Graphic::MobileTouchTracker    tracker;
        Canvas::MobileCanvasNavigation navigation;
        for ( int i = 0; i < count; ++i )
            tracker.update(P::Down, 1, i, .2f + i * .1f, .4f, .02 * i);
        // 只留最后一个触点时不能提前执行；后续释放也不退回单指逻辑。
        for ( int i = 0; i < count - 1; ++i )
            tracker.update(P::Up, 1, i, .2f + i * .1f, .4f, .08);
        auto delta =
            navigation.update(tracker.snapshot(), true, 1000, 1000, 1, 10);
        // 单指残留期允许普通 UI 继续绘制，但历史动作仍必须保持沉默。
        check(!delta.m_undo && !delta.m_redo,
              "history waits for every finger up");
        tracker.update(P::Up, 1, count - 1, .2f + (count - 1) * .1f, .4f, .1);
        delta = navigation.update(tracker.snapshot(), true, 1000, 1000, 1, 10);
        check(delta.m_undo == (count == 2) && delta.m_redo == (count == 3),
              "two taps undo and three taps redo");
        delta = navigation.update(tracker.snapshot(), true, 1000, 1000, 1, 10);
        check(!delta.m_undo && !delta.m_redo,
              "ended history gesture is consumed once");
    }
    // 往返抖动超出容差即是移动，不能只比较抬起位置与起点。
    Graphic::MobileTouchTracker    tracker;
    Canvas::MobileCanvasNavigation navigation;
    tracker.update(P::Down, 1, 1, .2f, .4f, 0);
    tracker.update(P::Down, 1, 2, .4f, .4f, .02);
    tracker.update(P::Move, 1, 1, .3f, .4f, .03);
    tracker.update(P::Move, 1, 1, .2f, .4f, .04);
    tracker.update(P::Up, 1, 1, .2f, .4f, .06);
    tracker.update(P::Up, 1, 2, .4f, .4f, .08);
    check(
        !navigation.update(tracker.snapshot(), true, 1000, 1000, 1, 10).m_undo,
        "out-and-back motion is not a tap");
}
/// @brief 系统取消、窗口外起点和无效尺寸不执行历史动作。
/// @details 这些条件都应终止意图，不能在下一帧恢复旧触点。
/// @note 起点不归本画布的代际在移动进入后仍保持拒绝状态。
/// 系统取消与正常抬起具有不同含义，绝不能通过归零触点触发撤销。
/// 同一跟踪器在取消后再次按下会建立新代际，不沿用旧计时。
/// 测试使用真实 cancel 接口，使峰值保留和取消标志同时参与判定。
/// 没有操作系统窗口，失焦的具体来源不影响这层业务契约。
/// 断言同时覆盖撤销、重做和几何变化，避免取消意图泄漏到另一通道。
void testCancellation()
{
    using namespace MMM;
    using P = Graphic::TouchPhase;
    Graphic::MobileTouchTracker    tracker;
    Canvas::MobileCanvasNavigation navigation;
    tracker.update(P::Down, 1, 1, .2f, .4f, 0);
    tracker.update(P::Down, 1, 2, .4f, .4f, .02);
    navigation.update(tracker.snapshot(), false, 1000, 1000, 1, 10);
    tracker.update(P::Up, 1, 1, .2f, .4f, .06);
    tracker.update(P::Up, 1, 2, .4f, .4f, .08);
    check(
        !navigation.update(tracker.snapshot(), true, 1000, 1000, 1, 10).m_undo,
        "outside start stays rejected after entering canvas");
    // 新代际重新归属后，系统中断同样不能变成二指撤销。
    tracker.update(P::Down, 1, 1, .2f, .4f, 1);
    tracker.update(P::Down, 1, 2, .4f, .4f, 1.02);
    navigation.update(tracker.snapshot(), true, 1000, 1000, 1, 10);
    // 取消保留峰值以检测误触；导航器必须优先尊重取消标志。
    tracker.cancel();
    const auto delta =
        navigation.update(tracker.snapshot(), true, 1000, 1000, 1, 10);
    check(!delta.m_undo && !delta.m_redo && !delta.m_changed,
          "system cancellation never edits history");
}
}  // namespace
/// @brief 固定事件序列覆盖导航、捏合、历史及系统接管，不启动原生窗口。
int main()
{
    // 每个场景都运行，即使前一个失败也保留后续诊断信息。
    testNavigation();
    // 历史分支使用独立输入流，导航回程不允许污染轻触分类。
    testHistory();
    testCancellation();
    return failures == 0 ? 0 : 1;
}
