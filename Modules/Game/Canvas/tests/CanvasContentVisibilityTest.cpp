#include "canvas/CanvasContentVisibility.h"

namespace
{

/// @brief 验证欢迎占位页不会显示谱面状态或鼠标悬浮检视。
/// @return 两类谱面专属信息均被隐藏时返回 true。
constexpr bool testPlaceholderHidesBeatmapDetails()
{
    return !MMM::Canvas::shouldShowBeatmapDetails(false) &&
           !MMM::Canvas::shouldShowCanvasHoverInspection(
               false, false, true, true, false);
}

/// @brief 验证真实谱面在正常悬停时仍显示检视信息。
/// @return 谱面状态和悬浮检视均可见时返回 true。
constexpr bool testBeatmapKeepsInspectionVisible()
{
    return MMM::Canvas::shouldShowBeatmapDetails(true) &&
           MMM::Canvas::shouldShowCanvasHoverInspection(
               true, false, true, true, false);
}

/// @brief 验证遮挡、未悬停或播放状态会隐藏悬浮检视。
/// @return 所有限制条件均生效时返回 true。
constexpr bool testHoverInspectionRespectsInteractionState()
{
    return !MMM::Canvas::shouldShowCanvasHoverInspection(
               true, true, true, true, false) &&
           !MMM::Canvas::shouldShowCanvasHoverInspection(
               true, false, false, true, false) &&
           !MMM::Canvas::shouldShowCanvasHoverInspection(
               true, false, true, false, false) &&
           !MMM::Canvas::shouldShowCanvasHoverInspection(
               true, false, true, true, true);
}

}  // namespace

/// @brief 覆盖真实谱面与欢迎占位页的画布信息可见性。
/// @return 所有可见性规则满足时返回 0。
int main()
{
    static_assert(testPlaceholderHidesBeatmapDetails());
    static_assert(testBeatmapKeepsInspectionVisible());
    static_assert(testHoverInspectionRespectsInteractionState());
    return 0;
}
