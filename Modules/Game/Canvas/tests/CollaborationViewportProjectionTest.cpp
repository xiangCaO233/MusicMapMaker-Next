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
                   testReverseAndInvalidRanges()
               ? 0
               : 1;
}
