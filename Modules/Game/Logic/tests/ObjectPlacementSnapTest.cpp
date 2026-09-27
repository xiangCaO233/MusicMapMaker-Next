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
/// @note 时间以秒比较，容差仅用于浮点分拍运算，不代表用户配置的吸附范围。
bool near(double lhs, double rhs)
{
    // 不采用相对误差，所有期望位置都处于同一秒级范围。
    return std::abs(lhs - rhs) < 1e-9;
}

/// @brief 验证关闭物件放置磁吸时不产生候选。
/// @return 未产生磁吸结果时返回 true。
/// @note 原始位置由调用方保留，此处不要求空结果的 snappedTime 等于输入。
bool testDisabledPlacementSnap()
{
    // 不显式覆盖开关，同时约束 EditorSettings 的默认吸附状态为关闭。
    MMM::Config::EditorSettings settings;
    // 正常 BPM 与有限输入排除输入非法导致空结果的干扰。
    // 无穷右边界表示最后一个 BPM 段，本例不测试下一时间点截断。
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
/// @note 这里验证默认配置集合，不验证所有这些网格的实际吸附计算。
bool testDefaultCommonBeatDivisorSelection()
{
    const MMM::Config::EditorSettings settings;
    // 默认选中集合独立于总吸附开关，关闭吸附也不应丢失用户的分拍偏好。
    // 遍历完整受支持范围，同时检查遗漏默认项和意外启用的额外项。
    for ( int divisor = MMM::Config::COMMON_BEAT_DIVISOR_MIN;
          divisor <= MMM::Config::COMMON_BEAT_DIVISOR_MAX;
          ++divisor ) {
        const bool expected = divisor == 2 || divisor == 3 || divisor == 4 ||
                              divisor == 6 || divisor == 8 || divisor == 12 ||
                              divisor == 16 || divisor == 24;
        // 通过公共掩码查询接口读取，避免测试依赖位移编码的内部细节。
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
/// @note 分拍分母描述一拍内部的位置，不直接表示一秒的分数。
bool testCurrentBeatDivisorSnap()
{
    MMM::Config::EditorSettings settings;
    settings.objectPlacementSnap = true;
    settings.beatDivisor         = 4;
    // 默认模式应使用当前分拍；不显式改模式，保留对默认选择的回归覆盖。
    // 120 BPM 每拍 0.5 秒，四分之一拍的位置因而是 0.125 秒。
    // 输入 0.14 更靠近第一条分拍线，不处于等距候选的决胜边界。
    const auto result = MMM::Logic::SessionUtils::calculateObjectPlacementSnap(
        0.14, 0.0, std::numeric_limits<double>::infinity(), 120.0, settings);
    // 时间与最简分数必须一起正确，避免画面已吸附但分拍标签仍错误。
    if ( !result.isSnapped || !near(result.snappedTime, 0.125) ||
         result.numerator != 1 || result.denominator != 4 ) {
        XERROR("Current beat divisor snap did not select the 1/4 line");
        return false;
    }
    return true;
}

/// @brief 验证常用分拍模式忽略当前分拍策略并使用用户选择。
/// @return 仅选择 1/3 时吸附到 1/3 网格返回 true。
/// @note 当前网格故意保留为四分拍，用不同结果检测模式是否真正切换。
bool testSelectedCommonBeatDivisorSnap()
{
    MMM::Config::EditorSettings settings;
    settings.objectPlacementSnap = true;
    settings.objectPlacementSnapMode =
        MMM::Config::ObjectPlacementSnapMode::CommonBeatDivisors;
    settings.beatDivisor = 4;
    // 先清掉默认多分母组合，保证候选仅来自用户显式开启的三分拍。
    settings.commonBeatDivisorMask = 0U;
    MMM::Config::setCommonBeatDivisorEnabled(
        settings.commonBeatDivisorMask, 3, true);

    // 用与当前分拍用例相同的输入，只改变模式和集合以隔离选择策略差异。
    // 三分之一拍等于 0.5/3 秒；不能误把更近的四分拍候选混入竞争。
    const auto result = MMM::Logic::SessionUtils::calculateObjectPlacementSnap(
        0.14, 0.0, std::numeric_limits<double>::infinity(), 120.0, settings);
    // 即使当前 beatDivisor 仍是 4，结果分母也必须报告实际采用的 3。
    if ( !result.isSnapped || !near(result.snappedTime, 1.0 / 6.0) ||
         result.numerator != 1 || result.denominator != 3 ) {
        XERROR("Selected common beat divisor did not override current grid");
        return false;
    }
    return true;
}

/// @brief 验证向下取整会在所选常用网格中选取最近的更早拍线。
/// @return 1/3 与 1/5 网格共同启用时选择 1/3 返回 true。
/// @note 向下取整应分别作用于每种网格，再从它们的合法候选中选最近者。
bool testCommonBeatDivisorFloorSnap()
{
    MMM::Config::EditorSettings settings;
    settings.objectPlacementSnap = true;
    settings.objectPlacementSnapMode =
        MMM::Config::ObjectPlacementSnapMode::CommonBeatDivisors;
    settings.snapFloor = true;
    // 使用两个互不整除的分母，避免候选恰好重合而掩盖多方案比较错误。
    settings.commonBeatDivisorMask = 0U;
    MMM::Config::setCommonBeatDivisorEnabled(
        settings.commonBeatDivisorMask, 3, true);
    MMM::Config::setCommonBeatDivisorEnabled(
        settings.commonBeatDivisorMask, 5, true);

    // 三分拍向下候选为 1/6 秒，五分拍向下候选为 0.1 秒。
    // 五分拍的 0.2 秒虽然更近，但位于输入之后，必须被向下规则排除。
    // 因而该例也能发现将 snapFloor 错误实现成普通四舍五入的情况。
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
/// @note 本例总开关保持启用，空候选必须来自没有选中任何分母。
bool testEmptyCommonBeatDivisorSelection()
{
    MMM::Config::EditorSettings settings;
    settings.objectPlacementSnap = true;
    settings.objectPlacementSnapMode =
        MMM::Config::ObjectPlacementSnapMode::CommonBeatDivisors;
    settings.commonBeatDivisorMask = 0U;
    // 不允许悄悄回退到当前 beatDivisor，否则用户无法通过清空组合取消网格。
    // 沿用其它成功用例的时间和 BPM，避免把空集合与时间边界效应混在一起。
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
/// @note 覆盖模式与分拍集合，不覆盖非有限输入、负时间或 BPM 段末截断。
/// @note 本文件验证时间计算，不证明画布坐标投影或鼠标命中逻辑正确。
/// @note 没有构造等距网格，因此不对同距离候选的优先级作验收结论。
int main()
{
    // 所有查询只依赖内存设置，不启动会话、音频或渲染窗口。
    // 每个用例使用独立设置，避免掩码和开关修改泄漏到后续用例。
    // 保留短路执行：失败用例先输出具体原因，再返回非零退出码。
    return testDisabledPlacementSnap() &&
                   testDefaultCommonBeatDivisorSelection() &&
                   testCurrentBeatDivisorSnap() &&
                   testSelectedCommonBeatDivisorSnap() &&
                   testCommonBeatDivisorFloorSnap() &&
                   testEmptyCommonBeatDivisorSelection()
               ? 0
               : 1;
}
