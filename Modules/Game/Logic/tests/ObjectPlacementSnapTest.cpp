#include "config/EditorSettings.h"
#include "log/colorful-log.h"
#include "logic/session/SessionUtils.h"

#include <cmath>
#include <limits>

namespace
{

/// @brief 使用小容差比较磁吸时间。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个时间足够接近时返回 true。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-9;
}

/// @brief 验证关闭物件放置磁吸时保留原始位置。
/// @return 未产生磁吸结果时返回 true。
bool testDisabledPlacementSnap()
{
    MMM::Config::EditorSettings settings;
    const auto result = MMM::Logic::SessionUtils::calculateObjectPlacementSnap(
        0.14, 0.0, std::numeric_limits<double>::infinity(), 120.0, settings);
    if ( result.isSnapped ) {
        XERROR("Disabled object placement snap produced a candidate");
        return false;
    }
    return true;
}

/// @brief 验证常用分拍默认选择只包含约定集合。
/// @return 2、3、4、6、8、12、16、24 被选中且其它分母未选中时返回 true。
bool testDefaultCommonBeatDivisorSelection()
{
    const MMM::Config::EditorSettings settings;
    for ( int divisor = MMM::Config::COMMON_BEAT_DIVISOR_MIN;
          divisor <= MMM::Config::COMMON_BEAT_DIVISOR_MAX;
          ++divisor ) {
        const bool expected = divisor == 2 || divisor == 3 || divisor == 4 ||
                              divisor == 6 || divisor == 8 || divisor == 12 ||
                              divisor == 16 || divisor == 24;
        if ( MMM::Config::isCommonBeatDivisorEnabled(
                 settings.commonBeatDivisorMask, divisor) != expected ) {
            XERROR("Unexpected default common beat divisor: {}", divisor);
            return false;
        }
    }
    return true;
}

/// @brief 验证当前分拍模式仅使用 beatDivisor 对齐。
/// @return 时间和分拍标记均符合 1/4 网格时返回 true。
bool testCurrentBeatDivisorSnap()
{
    MMM::Config::EditorSettings settings;
    settings.objectPlacementSnap = true;
    settings.beatDivisor         = 4;
    const auto result = MMM::Logic::SessionUtils::calculateObjectPlacementSnap(
        0.14, 0.0, std::numeric_limits<double>::infinity(), 120.0, settings);
    if ( !result.isSnapped || !near(result.snappedTime, 0.125) ||
         result.numerator != 1 || result.denominator != 4 ) {
        XERROR("Current beat divisor snap did not select the 1/4 line");
        return false;
    }
    return true;
}

/// @brief 验证常用分拍模式忽略当前分拍策略并使用用户选择。
/// @return 仅选择 1/3 时吸附到 1/3 网格返回 true。
bool testSelectedCommonBeatDivisorSnap()
{
    MMM::Config::EditorSettings settings;
    settings.objectPlacementSnap = true;
    settings.objectPlacementSnapMode =
        MMM::Config::ObjectPlacementSnapMode::CommonBeatDivisors;
    settings.beatDivisor           = 4;
    settings.commonBeatDivisorMask = 0U;
    MMM::Config::setCommonBeatDivisorEnabled(
        settings.commonBeatDivisorMask, 3, true);

    const auto result = MMM::Logic::SessionUtils::calculateObjectPlacementSnap(
        0.14, 0.0, std::numeric_limits<double>::infinity(), 120.0, settings);
    if ( !result.isSnapped || !near(result.snappedTime, 1.0 / 6.0) ||
         result.numerator != 1 || result.denominator != 3 ) {
        XERROR("Selected common beat divisor did not override current grid");
        return false;
    }
    return true;
}

/// @brief 验证向下取整会在所选常用网格中选取最近的更早拍线。
/// @return 1/3 与 1/5 网格共同启用时选择 1/3 返回 true。
bool testCommonBeatDivisorFloorSnap()
{
    MMM::Config::EditorSettings settings;
    settings.objectPlacementSnap = true;
    settings.objectPlacementSnapMode =
        MMM::Config::ObjectPlacementSnapMode::CommonBeatDivisors;
    settings.snapFloor             = true;
    settings.commonBeatDivisorMask = 0U;
    MMM::Config::setCommonBeatDivisorEnabled(
        settings.commonBeatDivisorMask, 3, true);
    MMM::Config::setCommonBeatDivisorEnabled(
        settings.commonBeatDivisorMask, 5, true);

    const auto result = MMM::Logic::SessionUtils::calculateObjectPlacementSnap(
        0.19, 0.0, std::numeric_limits<double>::infinity(), 120.0, settings);
    if ( !result.isSnapped || !near(result.snappedTime, 1.0 / 6.0) ||
         result.numerator != 1 || result.denominator != 3 ) {
        XERROR("Common beat divisor floor snap selected the wrong line");
        return false;
    }
    return true;
}

/// @brief 验证常用分拍全部取消后不会产生候选。
/// @return 未产生磁吸结果时返回 true。
bool testEmptyCommonBeatDivisorSelection()
{
    MMM::Config::EditorSettings settings;
    settings.objectPlacementSnap = true;
    settings.objectPlacementSnapMode =
        MMM::Config::ObjectPlacementSnapMode::CommonBeatDivisors;
    settings.commonBeatDivisorMask = 0U;
    const auto result = MMM::Logic::SessionUtils::calculateObjectPlacementSnap(
        0.14, 0.0, std::numeric_limits<double>::infinity(), 120.0, settings);
    if ( result.isSnapped ) {
        XERROR("Empty common beat divisor selection produced a candidate");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行物件放置磁吸模式测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testDisabledPlacementSnap() &&
                   testDefaultCommonBeatDivisorSelection() &&
                   testCurrentBeatDivisorSnap() &&
                   testSelectedCommonBeatDivisorSnap() &&
                   testCommonBeatDivisorFloorSnap() &&
                   testEmptyCommonBeatDivisorSelection()
               ? 0
               : 1;
}
