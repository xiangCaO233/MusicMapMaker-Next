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

    // 从关闭状态激活时，应同时打开、聚焦并请求检查历史窗口位置。
    const auto closed = resolveTimelineTableWindowActivation(false, false);
    // 三个输出字段必须作为同一动作出现，不能只打开而遗漏恢复或聚焦。
    if ( !closed.open || !closed.requestFocus || !closed.requestRecovery ) {
        return false;
    }

    // 已打开但失焦的窗口不能被菜单直接关闭，而应恢复到用户可见位置。
    const auto unfocused = resolveTimelineTableWindowActivation(true, false);
    // 该分支覆盖窗口可能落在其它显示器或被其它停靠页遮挡的情况。
    if ( !unfocused.open || !unfocused.requestFocus ||
         !unfocused.requestRecovery ) {
        return false;
    }

    // 只有已打开、聚焦且可达的窗口再次激活时才解释为切换关闭。
    const auto focused = resolveTimelineTableWindowActivation(true, true);
    // 关闭结果应把两个一次性请求一并清零。
    // 由此保证下一次重新打开时必须重新生成恢复与聚焦请求。
    return !focused.open && !focused.requestFocus && !focused.requestRecovery;
}

/// @brief 验证菜单弹窗临时接管焦点时仍保留表格此前的聚焦状态。
/// @return 聚焦状态更新符合预期时返回 true。
bool testTimelineTableWindowFocusTracking()
{
    using MMM::Canvas::resolveTimelineTableWindowFocusedAndReachable;

    // 四个布尔组合按优先级覆盖 reachable、focused、popupOpen 和 previous。
    // 当前窗口聚焦且可达时建立有效状态。
    return resolveTimelineTableWindowFocusedAndReachable(
               false, true, true, false) &&
           // 菜单弹窗临时夺取焦点时，应延续上一帧的有效状态。
           resolveTimelineTableWindowFocusedAndReachable(
               true, true, false, true) &&
           // 没有弹窗的普通失焦应立即清除有效状态。
           !resolveTimelineTableWindowFocusedAndReachable(
               true, true, false, false) &&
           // 不可达状态优先于焦点与弹窗，避免屏幕外窗口被当作可关闭。
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
        // 工作区从 (100, 50) 开始，右下边界为 (900, 650)。
        100.0F,
        50.0F,
        800.0F,
        600.0F
    };
    // 标题栏高 28 像素，至少需要露出 64 像素宽度才能拖回窗口。
    constexpr float TITLE_BAR_HEIGHT = 28.0F;
    constexpr float MINIMUM_WIDTH    = 64.0F;
    // 高度判据由标题栏推导，测试无需额外传入正文可见高度。

    // 完全位于工作区内的窗口显然可访问，作为正向基线。
    if ( !isTimelineTableWindowReachable({ 200.0F, 100.0F, 500.0F, 300.0F },
                                         WORK_AREA,
                                         TITLE_BAR_HEIGHT,
                                         MINIMUM_WIDTH) ) {
        return false;
    }
    // 正向基线失败时提前返回，后续负向用例无需继续执行。
    // 左边界位于工作区右侧，横向完全没有交叠。
    if ( isTimelineTableWindowReachable({ 950.0F, 100.0F, 500.0F, 300.0F },
                                        WORK_AREA,
                                        TITLE_BAR_HEIGHT,
                                        MINIMUM_WIDTH) ) {
        return false;
    }
    // 完全横向离屏必须比“露出不足”更早被归类为不可达。
    // 窗口正文可能与工作区相交，但标题栏位于工作区上方时仍不可操作。
    if ( isTimelineTableWindowReachable({ 200.0F, 5.0F, 500.0F, 300.0F },
                                        WORK_AREA,
                                        TITLE_BAR_HEIGHT,
                                        MINIMUM_WIDTH) ) {
        return false;
    }
    // 标题栏高度交叠不足时，即使正文区域可见也无法可靠拖动窗口。
    // 右侧仅露出 30 像素，小于规定的 64 像素可点击宽度。
    if ( isTimelineTableWindowReachable({ 870.0F, 100.0F, 500.0F, 300.0F },
                                        WORK_AREA,
                                        TITLE_BAR_HEIGHT,
                                        MINIMUM_WIDTH) ) {
        return false;
    }
    // 临界附近的横向用例保证最小可见宽度不是简单的任意相交判断。
    // 最后一项同时跨越上边界和右边界，但标题栏仍有足够交叠面积。
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
        // 与可达性测试复用相同工作区，便于手工核对恢复后的中心坐标。
        100.0F,
        50.0F,
        800.0F,
        600.0F
    };
    // 超大窗口在四边各留 20 像素后，最大尺寸应收敛到 760 x 560。
    const auto oversized = recoverTimelineTableWindowRect(
        { 2000.0F, -900.0F, 1200.0F, 900.0F }, WORK_AREA, 20.0F);
    // 原位置完全离屏，不应参与恢复后的坐标计算。
    if ( !approximatelyEqual(oversized.x, 120.0F) ||
         !approximatelyEqual(oversized.y, 70.0F) ||
         !approximatelyEqual(oversized.width, 760.0F) ||
         !approximatelyEqual(oversized.height, 560.0F) ) {
        return false;
    }
    // 四个字段一起验证边距扣除后的尺寸和工作区内居中位置。

    // 尺寸已适合工作区的窗口只需居中，不应被无意义放大或缩小。
    const auto regular = recoverTimelineTableWindowRect(
        { -800.0F, 900.0F, 400.0F, 300.0F }, WORK_AREA, 20.0F);
    // 400 x 300 在该工作区居中后的左上角应为 (300, 200)。
    // 宽高断言还保证恢复操作没有把正常窗口强制扩展到可用上限。
    // 坐标使用近似比较，避免不同编译器的浮点折叠差异影响结果。
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
    // 激活、焦点、可达性和恢复是辅助窗口状态机的四个独立职责。
    // 依次组合执行，任一失败都通过非零退出码报告。
    return testTimelineTableWindowActivation() &&
                   testTimelineTableWindowFocusTracking() &&
                   testTimelineTableWindowReachability() &&
                   testTimelineTableWindowRecovery()
               ? 0
               : 1;
}
