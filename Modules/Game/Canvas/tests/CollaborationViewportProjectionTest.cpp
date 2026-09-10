#include "canvas/CollaborationViewportProjection.h"

#include <cmath>
#include <limits>

namespace
{

/// @brief 比较投影结果与期望画布坐标。
/// @param value 待检查投影结果。
/// @param expected 期望 Y 坐标。
/// @return 结果存在且误差小于容差时返回 true。
///
/// optional 为空也视为失败，从而同时验证输入被接受和数值正确。
bool near(const std::optional<float>& value, float expected)
{
    return value && std::abs(*value - expected) < 1e-4F;
}

/// @brief 比较反投影结果与期望视觉时间。
/// @param value 待检查反投影结果。
/// @param expected 期望视觉时间。
/// @return 结果存在且误差小于容差时返回 true。
///
/// 时间使用更严格容差，避免往返测试掩盖分段选择错误。
bool near(const std::optional<double>& value, double expected)
{
    return value && std::abs(*value - expected) < 1e-9;
}

/// @brief 比较两个直接坐标值。
/// @param value 待检查坐标。
/// @param expected 期望坐标。
/// @return 误差小于容差时返回 true。
///
/// 此重载用于已经确认 optional 存在后的横向边界字段。
bool near(float value, float expected)
{
    return std::abs(value - expected) < 1e-4F;
}

/// @brief 验证本地完整可见范围始终覆盖整幅画布。
/// @return 顶边、判定线和底边锚点全部正确时返回 true。
///
/// 用例使用不对称时间跨度，防止实现退化为忽略判定线的单段映射。
bool testLocalViewportAnchors()
{
    // 下方仅覆盖 1 秒，上方覆盖 5 秒，刻意构造非等速的两段时间轴。
    constexpr double VISIBLE_START = 9.0;
    constexpr double VISUAL_TIME   = 10.0;
    constexpr double VISIBLE_END   = 15.0;
    constexpr float  JUDGMENT_Y    = 500.0F;
    constexpr float  HEIGHT        = 600.0F;
    // 判定线靠近底部，明确排除默认位于画布中央的隐含假设。
    // 三个锚点必须精确落在底边、判定线与顶边，不能使用全高统一比例。
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
///
/// 两个采样点分别位于判定线上下，且各自恰好处于局部时间区间中点。
bool testPiecewiseProjection()
{
    // 下半区时间中点位于 550，上半区时间中点位于 250。
    // 两个结果分别使用各自区间长度，验证判定线确实是分段锚点。
    return near(MMM::Canvas::projectCollaborationViewportTime(
                    9.5, 10.0, 9.0, 15.0, 500.0F, 600.0F),
                550.0F) &&
           near(MMM::Canvas::projectCollaborationViewportTime(
                    12.5, 10.0, 9.0, 15.0, 500.0F, 600.0F),
                250.0F);
}

/// @brief 验证发布端使用轨道框边界而不是整幅画布边界。
/// @return 上下布局边距被排除且边界时间可投影回原坐标时返回 true。
///
/// 轨道框上下各留出 60 像素，往返结果可直接揭示是否误用了画布边缘。
bool testTrackViewportBoundaryRoundTrip()
{
    // 使用与锚点测试相同的非等速时间范围。
    constexpr double VISIBLE_START  = 9.0;
    constexpr double VISUAL_TIME    = 10.0;
    constexpr double VISIBLE_END    = 15.0;
    constexpr float  JUDGMENT_Y     = 500.0F;
    constexpr float  HEIGHT         = 600.0F;
    constexpr float  TRACK_TOP_Y    = 60.0F;
    constexpr float  TRACK_BOTTOM_Y = 540.0F;

    // 先把轨道区实际上下边界反投影成要发布给协作者的时间范围。
    const auto bottomTime =
        // 轨道底边位于判定线下方，应使用 visibleStart 到 visualTime 区间。
        MMM::Canvas::unprojectCollaborationViewportTime(TRACK_BOTTOM_Y,
                                                        VISUAL_TIME,
                                                        VISIBLE_START,
                                                        VISIBLE_END,
                                                        JUDGMENT_Y,
                                                        HEIGHT);
    const auto topTime =
        // 轨道顶边位于判定线上方，应使用 visualTime 到 visibleEnd 区间。
        MMM::Canvas::unprojectCollaborationViewportTime(TRACK_TOP_Y,
                                                        VISUAL_TIME,
                                                        VISIBLE_START,
                                                        VISIBLE_END,
                                                        JUDGMENT_Y,
                                                        HEIGHT);
    // 再正向投影回本地坐标，验证两个方向在各自分段内互逆。
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
///
/// 输入远离两侧裁剪区，用于验证常规路径不会重新计算默认轨道宽度。
bool testHorizontalUsesLocalTrackProjection()
{
    // 轨道范围完全位于画布内，不应触发边缘裁剪或最小宽度扩张。
    const auto range = MMM::Canvas::projectCollaborationViewportHorizontalRange(
        300.0F, 700.0F, 1000.0F);
    return range && near(range->leftX, 300.0F) && near(range->rightX, 700.0F);
}

/// @brief 验证完全移出画布的远端视野停留为边缘提示，不回跳默认轨道区。
/// @return 左右两侧均保留固定最小宽度时返回 true。
///
/// 左右镜像输入确保两个离屏分支使用一致的安全边距与提示宽度。
bool testHorizontalOffscreenClamping()
{
    // 两个范围分别完全位于画布左侧和右侧。
    const auto left = MMM::Canvas::projectCollaborationViewportHorizontalRange(
        // 范围右边界仍小于零，确认完全离开左侧。
        -900.0F,
        -500.0F,
        1000.0F);
    const auto right = MMM::Canvas::projectCollaborationViewportHorizontalRange(
        // 范围左边界超过画布宽度，确认完全离开右侧。
        1300.0F,
        1700.0F,
        1000.0F);
    return left && near(left->leftX, 2.0F) && near(left->rightX, 5.0F) &&
           right && near(right->leftX, 995.0F) && near(right->rightX, 998.0F);
}

/// @brief 验证同侧多人离屏箭头按稳定槽位展开且窄轨道会退回整幅画布。
/// @return 单人居中、多人展开和非法槽位均符合预期时返回 true。
///
/// 测试不涉及参与者排序，只验证调用方已经提供稳定索引后的几何布局。
bool testOffscreenIndicatorLayout()
{
    // 单人槽位应位于轨道中心，多人槽位使用轨道内缩后的首、中、尾位置。
    const auto single = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        // 单槽位不需要展开，使用内容中心 500。
        300.0F,
        700.0F,
        1000.0F,
        0,
        1);
    const auto first = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        300.0F, 700.0F, 1000.0F, 0, 3);
    const auto middle = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        300.0F, 700.0F, 1000.0F, 1, 3);
    const auto last = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        300.0F, 700.0F, 1000.0F, 2, 3);
    const auto narrowFirst = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        // 回退后首槽位位于左安全边距 10。
        490.0F,
        510.0F,
        1000.0F,
        0,
        3);
    const auto narrowLast = MMM::Canvas::layoutCollaborationViewportIndicatorX(
        // 回退后末槽位位于右安全边距 990。
        490.0F,
        510.0F,
        1000.0F,
        2,
        3);

    // 最后一项传入 slotIndex==slotCount，必须拒绝越界槽位。
    return near(single, 500.0F) && near(first, 310.0F) &&
           near(middle, 500.0F) && near(last, 690.0F) &&
           near(narrowFirst, 10.0F) && near(narrowLast, 990.0F) &&
           !MMM::Canvas::layoutCollaborationViewportIndicatorX(
               300.0F, 700.0F, 1000.0F, 3, 3);
}

/// @brief 验证反向时间边界和非法输入不会产生错误坐标。
/// @return 反向锚点正确且 NaN 被拒绝时返回 true。
///
/// 反向区间仍保留相同三个视觉锚点，而 NaN 必须在插值前被拒绝。
bool testReverseAndInvalidRanges()
{
    // 反向边界模拟时间随画布方向递减的滚动布局。
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // visibleStart=11 仍映射到底边，visibleEnd=5 仍映射到顶边。
    return near(MMM::Canvas::projectCollaborationViewportTime(
                    11.0, 10.0, 11.0, 5.0, 500.0F, 600.0F),
                600.0F) &&
           near(MMM::Canvas::projectCollaborationViewportTime(
                    5.0, 10.0, 11.0, 5.0, 500.0F, 600.0F),
                0.0F) &&
           !MMM::Canvas::projectCollaborationViewportTime(
               // 任一非有限时间都不能生成可绘制坐标。
               nan,
               10.0,
               9.0,
               15.0,
               500.0F,
               600.0F);
}

}  // namespace

/// @brief 覆盖协作视野在缺少 ScrollSegment 时的锚点投影。
/// @return 所有检查通过时返回 0。
///
/// 这些回归用例验证纯几何回退，不需要构造 ScrollCache 或渲染快照。
int main()
{
    // 锚点、分段、往返、横向裁剪、箭头槽位与反向范围分别独立覆盖。
    // 全部函数为纯计算，退出码可稳定用于所有平台的构建验证。
    return testLocalViewportAnchors() && testPiecewiseProjection() &&
                   testTrackViewportBoundaryRoundTrip() &&
                   testHorizontalUsesLocalTrackProjection() &&
                   testHorizontalOffscreenClamping() &&
                   testOffscreenIndicatorLayout() &&
                   testReverseAndInvalidRanges()
               ? 0
               : 1;
}
// 画布安全边距为 2，离屏提示宽度为 3，因此得到 [2,5] 与 [995,998]。
// 20 像素窄轨道无法容纳三个槽位，预期回退到画布安全边缘。
