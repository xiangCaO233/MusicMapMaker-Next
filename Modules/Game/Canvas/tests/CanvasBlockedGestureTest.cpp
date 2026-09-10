#include "canvas/CanvasBlockedGesture.h"

namespace
{

/// @brief 验证按住鼠标进入批注区不会提前丢弃正在绘制的笔刷手势。
/// @return 尚未释放左键时不请求结束或清理状态。
///
/// 用例模拟手势已从画布建立、指针随后移动到批注浮层的连续帧。
constexpr bool testBlockedAreaPreservesHeldBrush()
{
    // 绘制手势从画布开始，但本帧左右键均未释放。
    const auto completion = MMM::Canvas::resolveBlockedCanvasGestureCompletion(
        MMM::Logic::EditTool::Draw, false, false, true, false, false);
    // 阻挡区只影响新输入命中，不能在按住期间提前终止既有手势。
    return completion.leftEnd ==
               MMM::Canvas::BlockedCanvasLeftGestureEnd::None &&
           !completion.clearLeftState && !completion.endErase;
}

/// @brief 验证从画布空白处开始的框选拖动可连续穿过批注区。
/// @return 仅真实活动框选绕过阻挡，批注区起手和物件拖拽仍被拦截。
///
/// 各负向组合每次只破坏一项前置条件，便于定位状态判断回归。
constexpr bool testBlockedAreaPassesActiveMarquee()
{
    // 第一项是唯一允许穿透的活动框选，后续逐一改变起点与拖动状态。
    return MMM::Canvas::shouldContinueMarqueeAcrossBlockedArea(
               MMM::Logic::EditTool::Marquee, true, true, false, false) &&
           !MMM::Canvas::shouldContinueMarqueeAcrossBlockedArea(
               // 手势不是从画布开始时不能借用已有框选穿透规则。
               MMM::Logic::EditTool::Marquee,
               true,
               false,
               false,
               false) &&
           !MMM::Canvas::shouldContinueMarqueeAcrossBlockedArea(
               MMM::Logic::EditTool::Marquee, true, true, true, false) &&
           !MMM::Canvas::shouldContinueMarqueeAcrossBlockedArea(
               MMM::Logic::EditTool::Marquee, true, true, false, true) &&
           !MMM::Canvas::shouldContinueMarqueeAcrossBlockedArea(
               // 未形成拖动时即使从画布空白开始也不应穿透。
               MMM::Logic::EditTool::Marquee,
               false,
               true,
               false,
               false);
}

/// @brief 验证在批注区释放左键会结束原画布笔刷并清理手势状态。
/// @return 绘制手势收到 Brush 结束动作。
constexpr bool testBlockedAreaFinishesReleasedBrush()
{
    // 左键释放发生在阻挡区内，仍需向画布发送 Brush 收尾动作。
    const auto completion = MMM::Canvas::resolveBlockedCanvasGestureCompletion(
        MMM::Logic::EditTool::Draw, true, false, true, false, false);
    // clearLeftState 与结束命令必须在同一释放帧出现。
    return completion.leftEnd ==
               MMM::Canvas::BlockedCanvasLeftGestureEnd::Brush &&
           completion.clearLeftState && !completion.endErase;
}

/// @brief 验证物件拖拽与右键擦除在批注区释放时仍会正常收尾。
/// @return 两种活动手势分别请求结束命令。
///
/// 左右键释放通过独立结果字段表达，不能互相吞掉收尾动作。
constexpr bool testBlockedAreaFinishesOtherReleasedGestures()
{
    // 物件拖拽以左键释放结束，优先级高于当前工具枚举。
    const auto dragCompletion =
        MMM::Canvas::resolveBlockedCanvasGestureCompletion(
            MMM::Logic::EditTool::Move, true, false, true, true, false);
    const auto eraseCompletion =
        // 擦除仅释放右键，不应伪造任何左键结束动作。
        MMM::Canvas::resolveBlockedCanvasGestureCompletion(
            MMM::Logic::EditTool::Draw, false, true, false, false, true);
    // 两种独立按键手势在同一用例中验证各自的释放通道。
    return dragCompletion.leftEnd ==
               MMM::Canvas::BlockedCanvasLeftGestureEnd::ObjectDrag &&
           dragCompletion.clearLeftState && eraseCompletion.endErase;
}

}  // namespace

/// @brief 覆盖画布手势进入批注阻挡区后的保持与释放行为。
/// @return 全部编译期断言通过时返回 0。
int main()
{
    // 编译期断言覆盖保持、穿透和三类释放收尾，无需模拟输入队列。
    static_assert(testBlockedAreaPreservesHeldBrush());
    static_assert(testBlockedAreaPassesActiveMarquee());
    static_assert(testBlockedAreaFinishesReleasedBrush());
    static_assert(testBlockedAreaFinishesOtherReleasedGestures());
    // 若状态机常量表达式发生回归，目标会在编译阶段失败。
    return 0;
}
