#include "canvas/CanvasBlockedGesture.h"

namespace
{

/// @brief 验证按住鼠标进入批注区不会提前丢弃正在绘制的笔刷手势。
/// @return 尚未释放左键时不请求结束或清理状态。
constexpr bool testBlockedAreaPreservesHeldBrush()
{
    const auto completion = MMM::Canvas::resolveBlockedCanvasGestureCompletion(
        MMM::Logic::EditTool::Draw, false, false, true, false, false);
    return completion.leftEnd ==
               MMM::Canvas::BlockedCanvasLeftGestureEnd::None &&
           !completion.clearLeftState && !completion.endErase;
}

/// @brief 验证从画布空白处开始的框选拖动可连续穿过批注区。
/// @return 仅真实活动框选绕过阻挡，批注区起手和物件拖拽仍被拦截。
constexpr bool testBlockedAreaPassesActiveMarquee()
{
    return MMM::Canvas::shouldContinueMarqueeAcrossBlockedArea(
               MMM::Logic::EditTool::Marquee, true, true, false, false) &&
           !MMM::Canvas::shouldContinueMarqueeAcrossBlockedArea(
               MMM::Logic::EditTool::Marquee, true, false, false, false) &&
           !MMM::Canvas::shouldContinueMarqueeAcrossBlockedArea(
               MMM::Logic::EditTool::Marquee, true, true, true, false) &&
           !MMM::Canvas::shouldContinueMarqueeAcrossBlockedArea(
               MMM::Logic::EditTool::Marquee, true, true, false, true) &&
           !MMM::Canvas::shouldContinueMarqueeAcrossBlockedArea(
               MMM::Logic::EditTool::Marquee, false, true, false, false);
}

/// @brief 验证在批注区释放左键会结束原画布笔刷并清理手势状态。
/// @return 绘制手势收到 Brush 结束动作。
constexpr bool testBlockedAreaFinishesReleasedBrush()
{
    const auto completion = MMM::Canvas::resolveBlockedCanvasGestureCompletion(
        MMM::Logic::EditTool::Draw, true, false, true, false, false);
    return completion.leftEnd ==
               MMM::Canvas::BlockedCanvasLeftGestureEnd::Brush &&
           completion.clearLeftState && !completion.endErase;
}

/// @brief 验证物件拖拽与右键擦除在批注区释放时仍会正常收尾。
/// @return 两种活动手势分别请求结束命令。
constexpr bool testBlockedAreaFinishesOtherReleasedGestures()
{
    const auto dragCompletion =
        MMM::Canvas::resolveBlockedCanvasGestureCompletion(
            MMM::Logic::EditTool::Move, true, false, true, true, false);
    const auto eraseCompletion =
        MMM::Canvas::resolveBlockedCanvasGestureCompletion(
            MMM::Logic::EditTool::Draw, false, true, false, false, true);
    return dragCompletion.leftEnd ==
               MMM::Canvas::BlockedCanvasLeftGestureEnd::ObjectDrag &&
           dragCompletion.clearLeftState && eraseCompletion.endErase;
}

}  // namespace

/// @brief 覆盖画布手势进入批注阻挡区后的保持与释放行为。
/// @return 全部编译期断言通过时返回 0。
int main()
{
    static_assert(testBlockedAreaPreservesHeldBrush());
    static_assert(testBlockedAreaPassesActiveMarquee());
    static_assert(testBlockedAreaFinishesReleasedBrush());
    static_assert(testBlockedAreaFinishesOtherReleasedGestures());
    return 0;
}
