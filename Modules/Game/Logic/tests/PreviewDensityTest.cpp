#include "logic/PreviewDensity.h"

#include "log/colorful-log.h"

#include <array>
#include <cmath>
#include <vector>

namespace
{

/// @brief 使用小容差比较密度时间元数据。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-9;
}

/// @brief 验证空谱面不会生成无意义样本。
/// @return 行为符合预期时返回 true。
bool testEmptyChart()
{
    const auto density =
        MMM::Logic::buildPreviewDensitySnapshot({}, 120.0, 2.0, 0.25, 512);
    if ( !density.counts.empty() || density.maxCount != 0 ||
         !near(density.duration, 120.0) ) {
        XERROR("Preview density empty-chart behavior changed");
        return false;
    }
    return true;
}

/// @brief 验证两秒滑窗会在相邻采样点持续覆盖密集物件。
/// @return 行为符合预期时返回 true。
bool testFixedSlidingWindow()
{
    constexpr std::array<double, 5> TIMES{ 0.2, 0.8, 1.2, 2.8, 4.9 };
    const auto                      density =
        MMM::Logic::buildPreviewDensitySnapshot(TIMES, 5.0, 2.0, 1.0, 16);
    const std::vector<std::uint32_t> expected{ 3, 2, 1, 1, 1 };
    if ( density.counts != expected || density.maxCount != 3 ||
         !near(density.sampleInterval, 1.0) ||
         !near(density.windowDuration, 2.0) ) {
        XERROR("Preview density fixed-window counts changed");
        return false;
    }
    return true;
}

/// @brief 验证长谱面会限制样本数量并覆盖末尾物件。
/// @return 行为符合预期时返回 true。
bool testLongChartBinLimit()
{
    constexpr std::array<double, 2> TIMES{ 0.0, 599.9 };
    const auto                      density =
        MMM::Logic::buildPreviewDensitySnapshot(TIMES, 600.0, 2.0, 0.25, 64);
    if ( density.counts.size() != 64 || density.counts.front() != 1 ||
         density.counts.back() != 1 ||
         !near(density.sampleInterval, 600.0 / 64.0) ) {
        XERROR("Preview density long-chart bin limiting changed");
        return false;
    }
    return true;
}

/// @brief 验证物件末尾能够扩展无音轨谱面的密度时间轴。
/// @return 行为符合预期时返回 true。
bool testObjectTimeExtendsDuration()
{
    constexpr std::array<double, 1> TIMES{ 9.5 };
    const auto                      density =
        MMM::Logic::buildPreviewDensitySnapshot(TIMES, 0.0, 2.0, 0.5, 64);
    if ( !near(density.duration, 9.5) || density.counts.empty() ||
         density.counts.back() != 1 ) {
        XERROR("Preview density object-duration fallback changed");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行预览密度缓存纯逻辑测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testEmptyChart() && testFixedSlidingWindow() &&
                   testLongChartBinLimit() && testObjectTimeExtendsDuration()
               ? 0
               : 1;
}
