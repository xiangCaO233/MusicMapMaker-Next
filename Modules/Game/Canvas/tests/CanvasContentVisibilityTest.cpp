#include "ui/utils/CanvasContentVisibility.h"

namespace
{

/// @brief 验证欢迎占位页不会显示谱面状态或鼠标悬浮检视。
/// @return 两类谱面专属信息均被隐藏时返回 true。
constexpr bool testPlaceholderHidesBeatmapDetails()
{
    // hasBeatmap=false 直接隐藏状态栏谱面详情。
    return !MMM::UI::Utils::shouldShowBeatmapDetails(false) &&
           // 即使其余交互条件都允许，占位页也不能出现物件检视。
           !MMM::UI::Utils::shouldShowCanvasHoverInspection(
               false, false, true, true, false);
}

/// @brief 验证真实谱面在正常悬停时仍显示检视信息。
/// @return 谱面状态和悬浮检视均可见时返回 true。
constexpr bool testBeatmapKeepsInspectionVisible()
{
    // 真实谱面且画布悬停、窗口聚焦、未播放时应完整显示检视。
    return MMM::UI::Utils::shouldShowBeatmapDetails(true) &&
           MMM::UI::Utils::shouldShowCanvasHoverInspection(
               true, false, true, true, false);
}

/// @brief 验证遮挡、未悬停或播放状态会隐藏悬浮检视。
/// @return 所有限制条件均生效时返回 true。
constexpr bool testHoverInspectionRespectsInteractionState()
{
    // 依次隔离弹窗遮挡、未悬停、未聚焦与播放四个禁止条件。
    return !MMM::UI::Utils::shouldShowCanvasHoverInspection(
               true, true, true, true, false) &&
           !MMM::UI::Utils::shouldShowCanvasHoverInspection(
               true, false, false, true, false) &&
           !MMM::UI::Utils::shouldShowCanvasHoverInspection(
               true, false, true, false, false) &&
           !MMM::UI::Utils::shouldShowCanvasHoverInspection(
               true, false, true, true, true);
}

}  // namespace

/// @brief 覆盖真实谱面与欢迎占位页的画布信息可见性。
/// @return 所有可见性规则满足时返回 0。
///
/// 使用编译期断言固定纯布尔策略，避免测试依赖实际窗口状态。
int main()
{
    // constexpr 用例在编译期固定可见性真值表，不依赖 ImGui 上下文。
    static_assert(testPlaceholderHidesBeatmapDetails());
    static_assert(testBeatmapKeepsInspectionVisible());
    static_assert(testHoverInspectionRespectsInteractionState());
    // 所有断言通过即可返回成功，不需要运行期状态。
    return 0;
}
