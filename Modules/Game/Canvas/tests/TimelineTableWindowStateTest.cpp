#include "canvas/TimelineTableWindowState.h"

#include <cmath>

namespace
{
/// @brief 判断两个浮点数是否足够接近。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 误差不超过测试容差时返回 true。
bool approximatelyEqual(float lhs, float rhs)
{
    return std::abs(lhs - rhs) <= 0.001F;
}

/// @brief 验证表格菜单项在关闭、未聚焦和已聚焦状态下的切换行为。
/// @return 所有状态迁移符合预期时返回 true。
bool testTimelineTableWindowActivation()
{
    using MMM::Canvas::resolveTimelineTableWindowActivation;

    const auto closed = resolveTimelineTableWindowActivation(false, false);
    if ( !closed.open || !closed.requestFocus || !closed.requestRecovery ) {
        return false;
    }

    const auto unfocused = resolveTimelineTableWindowActivation(true, false);
    if ( !unfocused.open || !unfocused.requestFocus ||
         !unfocused.requestRecovery ) {
        return false;
    }

    const auto focused = resolveTimelineTableWindowActivation(true, true);
    return !focused.open && !focused.requestFocus && !focused.requestRecovery;
}

/// @brief 验证菜单弹窗临时接管焦点时仍保留表格此前的聚焦状态。
/// @return 聚焦状态更新符合预期时返回 true。
bool testTimelineTableWindowFocusTracking()
{
    using MMM::Canvas::resolveTimelineTableWindowFocusedAndReachable;

    return resolveTimelineTableWindowFocusedAndReachable(
               false, true, true, false) &&
           resolveTimelineTableWindowFocusedAndReachable(
               true, true, false, true) &&
           !resolveTimelineTableWindowFocusedAndReachable(
               true, true, false, false) &&
           !resolveTimelineTableWindowFocusedAndReachable(
               true, false, true, true);
}

/// @brief 验证只有标题栏保留足够可点击区域时才视为窗口可访问。
/// @return 工作区交叠判定符合预期时返回 true。
bool testTimelineTableWindowReachability()
{
    using MMM::Canvas::isTimelineTableWindowReachable;
    using MMM::Canvas::TimelineTableWindowRect;

    constexpr TimelineTableWindowRect WORK_AREA{
        100.0F, 50.0F, 800.0F, 600.0F
    };
    constexpr float TITLE_BAR_HEIGHT = 28.0F;
    constexpr float MINIMUM_WIDTH    = 64.0F;

    if ( !isTimelineTableWindowReachable({ 200.0F, 100.0F, 500.0F, 300.0F },
                                         WORK_AREA,
                                         TITLE_BAR_HEIGHT,
                                         MINIMUM_WIDTH) ) {
        return false;
    }
    if ( isTimelineTableWindowReachable({ 950.0F, 100.0F, 500.0F, 300.0F },
                                        WORK_AREA,
                                        TITLE_BAR_HEIGHT,
                                        MINIMUM_WIDTH) ) {
        return false;
    }
    if ( isTimelineTableWindowReachable({ 200.0F, 5.0F, 500.0F, 300.0F },
                                        WORK_AREA,
                                        TITLE_BAR_HEIGHT,
                                        MINIMUM_WIDTH) ) {
        return false;
    }
    if ( isTimelineTableWindowReachable({ 870.0F, 100.0F, 500.0F, 300.0F },
                                        WORK_AREA,
                                        TITLE_BAR_HEIGHT,
                                        MINIMUM_WIDTH) ) {
        return false;
    }
    return isTimelineTableWindowReachable({ 830.0F, 40.0F, 500.0F, 300.0F },
                                          WORK_AREA,
                                          TITLE_BAR_HEIGHT,
                                          MINIMUM_WIDTH);
}

/// @brief 验证屏幕外窗口恢复时会限制尺寸并居中到主工作区。
/// @return 恢复后的窗口矩形符合预期时返回 true。
bool testTimelineTableWindowRecovery()
{
    using MMM::Canvas::recoverTimelineTableWindowRect;
    using MMM::Canvas::TimelineTableWindowRect;

    constexpr TimelineTableWindowRect WORK_AREA{
        100.0F, 50.0F, 800.0F, 600.0F
    };
    const auto oversized = recoverTimelineTableWindowRect(
        { 2000.0F, -900.0F, 1200.0F, 900.0F }, WORK_AREA, 20.0F);
    if ( !approximatelyEqual(oversized.x, 120.0F) ||
         !approximatelyEqual(oversized.y, 70.0F) ||
         !approximatelyEqual(oversized.width, 760.0F) ||
         !approximatelyEqual(oversized.height, 560.0F) ) {
        return false;
    }

    const auto regular = recoverTimelineTableWindowRect(
        { -800.0F, 900.0F, 400.0F, 300.0F }, WORK_AREA, 20.0F);
    return approximatelyEqual(regular.x, 300.0F) &&
           approximatelyEqual(regular.y, 200.0F) &&
           approximatelyEqual(regular.width, 400.0F) &&
           approximatelyEqual(regular.height, 300.0F);
}
}  // namespace

/// @brief 运行 Timeline 独立表格窗口状态与位置恢复回归测试。
/// @return 测试通过时返回 0。
int main()
{
    return testTimelineTableWindowActivation() &&
                   testTimelineTableWindowFocusTracking() &&
                   testTimelineTableWindowReachability() &&
                   testTimelineTableWindowRecovery()
               ? 0
               : 1;
}
