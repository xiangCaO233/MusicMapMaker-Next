#include "canvas/TimingTableFraction.h"

#include <cmath>

namespace
{

/// @brief 使用严格容差比较连续拍位。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两值误差小于浮点拟合容差时返回 true。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-12;
}

/// @brief 验证最高分母边界不会被规整到下一拍。
bool testMaximumDenominatorBoundary()
{
    // 使用 1920 网格的最后一个分点，验证边界不会进位到第九拍。
    const auto fit = MMM::Canvas::fitTimingTableFraction(8.0 + 1919.0 / 1920.0);
    // 除整数拍号外，同时检查未约分分数和小数表示。
    // fraction 的严格比较通过 near 处理二进制浮点表示误差。
    return fit.beatIndex == 8 && fit.numerator == 1919 &&
           fit.denominator == 1920 && near(fit.fraction, 1919.0 / 1920.0);
}

/// @brief 验证 1/1920 最小分拍和可约分分拍均精确保留。
bool testMaximumGridAndReduction()
{
    // 1/1920 是支持的最细分拍，必须保持原始最大分母。
    const auto minimum =
        MMM::Canvas::fitTimingTableFraction(3.0 + 1.0 / 1920.0);
    const auto reduced =
        // 1000/1920 应约分为 25/48，但连续拍位置仍保持一致。
        MMM::Canvas::fitTimingTableFraction(2.0 + 1000.0 / 1920.0);
    // 两个结果放在同一测试中，确保最大精度与最大公约数约分可以共存。
    return minimum.beatIndex == 3 && minimum.numerator == 1 &&
           minimum.denominator == 1920 && reduced.beatIndex == 2 &&
           reduced.numerator == 25 && reduced.denominator == 48;
}

/// @brief 验证仅使用 MC 固定分拍候选，其他分拍落到 1/1920 网格。
bool testFixedMalodySubdivisionCandidates()
{
    // 1/3 和 287/288 都属于 MC 固定候选，应优先得到精确表达。
    const auto thirds = MMM::Canvas::fitTimingTableFraction(4.0 + 1.0 / 3.0);
    const auto denominator288 =
        MMM::Canvas::fitTimingTableFraction(5.0 + 287.0 / 288.0);
    // 287/288 位于候选分母上界附近，用于检查不会提前进位到下一拍。
    const auto nonCandidate =
        // 1/7 不在候选表中，应量化到最接近的 1/1920 网格并约分。
        MMM::Canvas::fitTimingTableFraction(6.0 + 1.0 / 7.0);
    // 量化后的 137/960 是 1/7 在统一细网格上的最接近可约分结果。
    return thirds.beatIndex == 4 && thirds.numerator == 1 &&
           thirds.denominator == 3 && denominator288.beatIndex == 5 &&
           denominator288.numerator == 287 &&
           denominator288.denominator == 288 && nonCandidate.beatIndex == 6 &&
           nonCandidate.numerator == 137 && nonCandidate.denominator == 960;
}

}  // namespace

/// @brief 运行时间点表格分拍精度测试。
/// @return 全部分拍拟合测试通过时返回 0。
int main()
{
    // 三组场景分别覆盖上边界、最细网格、约分和非候选量化。
    return testMaximumDenominatorBoundary() && testMaximumGridAndReduction() &&
                   testFixedMalodySubdivisionCandidates()
               ? 0
               : 1;
}
