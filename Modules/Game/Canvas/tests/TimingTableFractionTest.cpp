#include "canvas/TimingTableFraction.h"

#include <cmath>

namespace
{

/// @brief 使用严格容差比较连续拍位。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-12;
}

/// @brief 验证最高分母边界不会被规整到下一拍。
bool testMaximumDenominatorBoundary()
{
    const auto fit = MMM::Canvas::fitTimingTableFraction(8.0 + 1919.0 / 1920.0);
    return fit.beatIndex == 8 && fit.numerator == 1919 &&
           fit.denominator == 1920 && near(fit.fraction, 1919.0 / 1920.0);
}

/// @brief 验证 1/1920 最小分拍和可约分分拍均精确保留。
bool testMaximumGridAndReduction()
{
    const auto minimum =
        MMM::Canvas::fitTimingTableFraction(3.0 + 1.0 / 1920.0);
    const auto reduced =
        MMM::Canvas::fitTimingTableFraction(2.0 + 1000.0 / 1920.0);
    return minimum.beatIndex == 3 && minimum.numerator == 1 &&
           minimum.denominator == 1920 && reduced.beatIndex == 2 &&
           reduced.numerator == 25 && reduced.denominator == 48;
}

/// @brief 验证仅使用 MC 固定分拍候选，其他分拍落到 1/1920 网格。
bool testFixedMalodySubdivisionCandidates()
{
    const auto thirds = MMM::Canvas::fitTimingTableFraction(4.0 + 1.0 / 3.0);
    const auto denominator288 =
        MMM::Canvas::fitTimingTableFraction(5.0 + 287.0 / 288.0);
    const auto nonCandidate =
        MMM::Canvas::fitTimingTableFraction(6.0 + 1.0 / 7.0);
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
    return testMaximumDenominatorBoundary() && testMaximumGridAndReduction() &&
                   testFixedMalodySubdivisionCandidates()
               ? 0
               : 1;
}
