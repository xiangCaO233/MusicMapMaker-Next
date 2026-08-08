#include "canvas/CollaborationViewportProjection.h"

#include <cmath>
#include <limits>

namespace
{

/// @brief 比较投影结果与期望画布坐标。
/// @param value 待检查投影结果。
/// @param expected 期望 Y 坐标。
/// @return 结果存在且误差小于容差时返回 true。
bool near(const std::optional<float>& value, float expected)
{
    return value && std::abs(*value - expected) < 1e-4F;
}

/// @brief 比较反投影结果与期望视觉时间。
/// @param value 待检查反投影结果。
/// @param expected 期望视觉时间。
/// @return 结果存在且误差小于容差时返回 true。
bool near(const std::optional<double>& value, double expected)
{
    return value && std::abs(*value - expected) < 1e-9;
}

/// @brief 比较两个直接坐标值。
/// @param value 待检查坐标。
/// @param expected 期望坐标。
/// @return 误差小于容差时返回 true。
bool near(float value, float expected)
{
    return std::abs(value - expected) < 1e-4F;
}

/// @brief 验证本地完整可见范围始终覆盖整幅画布。
/// @return 顶边、判定线和底边锚点全部正确时返回 true。
bool testLocalViewportAnchors()
{
    constexpr double VISIBLE_START = 9.0;
    constexpr double VISUAL_TIME   = 10.0;
    constexpr double VISIBLE_END   = 15.0;
    constexpr float  JUDGMENT_Y    = 500.0F;
    constexpr float  HEIGHT        = 600.0F;
    return near(MMM::Canvas::projectCollaborationViewportTime(VISIBLE_START,
                                                              VISUAL_TIME,
                                                              VISIBLE_START,
                                                              VISIBLE_END,
                                                              JUDGMENT_Y,
                                                              HEIGHT),
                HEIGHT) &&
           near(MMM::Canvas::projectCollaborationViewportTime(VISUAL_TIME,
                                                              VISUAL_TIME,
                                                              VISIBLE_START,
                                                              VISIBLE_END,
                                                              JUDGMENT_Y,
                                                              HEIGHT),
                JUDGMENT_Y) &&
           near(MMM::Canvas::projectCollaborationViewportTime(VISIBLE_END,
                                                              VISUAL_TIME,
                                                              VISIBLE_START,
                                                              VISIBLE_END,
                                                              JUDGMENT_Y,
                                                              HEIGHT),
                0.0F);
}

/// @brief 验证上下半区分别使用判定线锚点进行比例投影。
/// @return 两侧中点不受固定滚动速度假设影响时返回 true。
bool testPiecewiseProjection()
{
    return near(MMM::Canvas::projectCollaborationViewportTime(
                    9.5, 10.0, 9.0, 15.0, 500.0F, 600.0F),
                550.0F) &&
           near(MMM::Canvas::projectCollaborationViewportTime(
                    12.5, 10.0, 9.0, 15.0, 500.0F, 600.0F),
                250.0F);
}

/// @brief 验证发布端使用轨道框边界而不是整幅画布边界。
/// @return 上下布局边距被排除且边界时间可投影回原坐标时返回 true。
bool testTrackViewportBoundaryRoundTrip()
{
    constexpr double VISIBLE_START  = 9.0;
    constexpr double VISUAL_TIME    = 10.0;
    constexpr double VISIBLE_END    = 15.0;
    constexpr float  JUDGMENT_Y     = 500.0F;
    constexpr float  HEIGHT         = 600.0F;
    constexpr float  TRACK_TOP_Y    = 60.0F;
    constexpr float  TRACK_BOTTOM_Y = 540.0F;

    const auto bottomTime =
        MMM::Canvas::unprojectCollaborationViewportTime(TRACK_BOTTOM_Y,
                                                        VISUAL_TIME,
                                                        VISIBLE_START,
                                                        VISIBLE_END,
                                                        JUDGMENT_Y,
                                                        HEIGHT);
    const auto topTime =
        MMM::Canvas::unprojectCollaborationViewportTime(TRACK_TOP_Y,
                                                        VISUAL_TIME,
                                                        VISIBLE_START,
                                                        VISIBLE_END,
                                                        JUDGMENT_Y,
                                                        HEIGHT);
    return near(bottomTime, 9.6) && near(topTime, 14.4) &&
           near(MMM::Canvas::projectCollaborationViewportTime(*bottomTime,
                                                              VISUAL_TIME,
                                                              VISIBLE_START,
                                                              VISIBLE_END,
                                                              JUDGMENT_Y,
                                                              HEIGHT),
                TRACK_BOTTOM_Y) &&
           near(MMM::Canvas::projectCollaborationViewportTime(*topTime,
                                                              VISUAL_TIME,
                                                              VISIBLE_START,
                                                              VISIBLE_END,
                                                              JUDGMENT_Y,
                                                              HEIGHT),
                TRACK_TOP_Y);
}

/// @brief 验证协作框横向范围只使用本地轨道投影。
/// @return 本地轨道边界保持不变时返回 true。
bool testHorizontalUsesLocalTrackProjection()
{
    const auto range = MMM::Canvas::projectCollaborationViewportHorizontalRange(
        300.0F, 700.0F, 1000.0F);
    return range && near(range->leftX, 300.0F) && near(range->rightX, 700.0F);
}

/// @brief 验证完全移出画布的远端视野停留为边缘提示，不回跳默认轨道区。
/// @return 左右两侧均保留固定最小宽度时返回 true。
bool testHorizontalOffscreenClamping()
{
    const auto left = MMM::Canvas::projectCollaborationViewportHorizontalRange(
        -900.0F, -500.0F, 1000.0F);
    const auto right = MMM::Canvas::projectCollaborationViewportHorizontalRange(
        1300.0F, 1700.0F, 1000.0F);
    return left && near(left->leftX, 2.0F) && near(left->rightX, 5.0F) &&
           right && near(right->leftX, 995.0F) && near(right->rightX, 998.0F);
}

/// @brief 验证同侧多人离屏箭头按稳定槽位展开且窄轨道会退回整幅画布。
/// @return 单人居中、多人展开和非法槽位均符合预期时返回 true。
bool testOffscreenIndicatorLayout()
{
    const auto single = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        300.0F, 700.0F, 1000.0F, 0, 1);
    const auto first = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        300.0F, 700.0F, 1000.0F, 0, 3);
    const auto middle = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        300.0F, 700.0F, 1000.0F, 1, 3);
    const auto last = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        300.0F, 700.0F, 1000.0F, 2, 3);
    const auto narrowFirst = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        490.0F, 510.0F, 1000.0F, 0, 3);
    const auto narrowLast = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        490.0F, 510.0F, 1000.0F, 2, 3);

    return near(single, 500.0F) && near(first, 310.0F) &&
           near(middle, 500.0F) && near(last, 690.0F) &&
           near(narrowFirst, 10.0F) && near(narrowLast, 990.0F) &&
           !MMM::Canvas::layoutCollaborationViewportIndicatorX(
               300.0F, 700.0F, 1000.0F, 3, 3);
}

/// @brief 验证反向时间边界和非法输入不会产生错误坐标。
/// @return 反向锚点正确且 NaN 被拒绝时返回 true。
bool testReverseAndInvalidRanges()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    return near(MMM::Canvas::projectCollaborationViewportTime(
                    11.0, 10.0, 11.0, 5.0, 500.0F, 600.0F),
                600.0F) &&
           near(MMM::Canvas::projectCollaborationViewportTime(
                    5.0, 10.0, 11.0, 5.0, 500.0F, 600.0F),
                0.0F) &&
           !MMM::Canvas::projectCollaborationViewportTime(
               nan, 10.0, 9.0, 15.0, 500.0F, 600.0F);
}

}  // namespace

/// @brief 覆盖协作视野在缺少 ScrollSegment 时的锚点投影。
/// @return 所有检查通过时返回 0。
int main()
{
    return testLocalViewportAnchors() && testPiecewiseProjection() &&
                   testTrackViewportBoundaryRoundTrip() &&
                   testHorizontalUsesLocalTrackProjection() &&
                   testHorizontalOffscreenClamping() &&
                   testOffscreenIndicatorLayout() &&
                   testReverseAndInvalidRanges()
               ? 0
               : 1;
}
