#include "logic/PreviewDensity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MMM::Logic
{

/// @brief 从已排序物件时间生成固定时长滑动窗口密度样本。
/// @param sortedObjectTimes 已过滤为有限非负值并按升序排列的物件时间。
/// @param totalDuration 谱面或主音轨总时长，单位秒。
/// @param windowDuration 固定滑动窗口时长，单位秒。
/// @param preferredSampleInterval 优先采样间隔，单位秒。
/// @param maxBinCount 最大样本数量，限制快照复制和 UI 绘制成本。
/// @return 可直接写入渲染快照的密度数据。
/// @warning 逻辑低频缓存重建路径：只允许在物件脏标记或总时长变化时调用；
/// 使用线性双指针统计，禁止放入每帧无条件路径。
PreviewDensitySnapshot buildPreviewDensitySnapshot(
    std::span<const double> sortedObjectTimes, double totalDuration,
    double windowDuration, double preferredSampleInterval,
    std::size_t maxBinCount)
{
    PreviewDensitySnapshot result;
    result.windowDuration =
        std::isfinite(windowDuration) && windowDuration > 0.0 ? windowDuration
                                                              : 2.0;

    double effectiveDuration =
        std::isfinite(totalDuration) && totalDuration > 0.0 ? totalDuration
                                                            : 0.0;
    if ( !sortedObjectTimes.empty() &&
         std::isfinite(sortedObjectTimes.back()) ) {
        effectiveDuration =
            std::max(effectiveDuration, sortedObjectTimes.back());
    }
    result.duration = effectiveDuration;

    if ( effectiveDuration <= 0.0 || sortedObjectTimes.empty() ||
         maxBinCount == 0 ) {
        return result;
    }

    const double preferredInterval =
        std::isfinite(preferredSampleInterval) && preferredSampleInterval > 0.0
            ? preferredSampleInterval
            : 0.25;
    const double requestedBinCount =
        std::ceil(effectiveDuration / preferredInterval);
    const std::size_t binCount = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(
            std::min(requestedBinCount, static_cast<double>(maxBinCount))));
    result.sampleInterval = effectiveDuration / static_cast<double>(binCount);
    result.counts.resize(binCount, 0);

    const double halfWindow = result.windowDuration * 0.5;
    const double latestWindowStart =
        std::max(0.0, effectiveDuration - result.windowDuration);
    std::size_t leftIndex  = 0;
    std::size_t rightIndex = 0;

    for ( std::size_t bin = 0; bin < binCount; ++bin ) {
        const double center =
            (static_cast<double>(bin) + 0.5) * result.sampleInterval;
        const double windowStart =
            std::clamp(center - halfWindow, 0.0, latestWindowStart);
        const double windowEnd =
            std::min(effectiveDuration, windowStart + result.windowDuration);

        while ( leftIndex < sortedObjectTimes.size() &&
                sortedObjectTimes[leftIndex] < windowStart ) {
            ++leftIndex;
        }
        rightIndex = std::max(rightIndex, leftIndex);
        while ( rightIndex < sortedObjectTimes.size() &&
                sortedObjectTimes[rightIndex] <= windowEnd ) {
            ++rightIndex;
        }

        const std::size_t count        = rightIndex - leftIndex;
        const auto        clampedCount = static_cast<std::uint32_t>(
            std::min(count,
                     static_cast<std::size_t>(
                         std::numeric_limits<std::uint32_t>::max())));
        result.counts[bin] = clampedCount;
        result.maxCount    = std::max(result.maxCount, clampedCount);
    }

    // 极长谱面在样本间隔大于固定窗口时，以每个窗口峰值补齐对应时间桶，
    // 避免稀疏物件恰好落在采样中心之间而完全消失。
    if ( result.sampleInterval > result.windowDuration ) {
        std::size_t peakLeft = 0;
        for ( std::size_t peakRight = 0; peakRight < sortedObjectTimes.size();
              ++peakRight ) {
            const double windowStart =
                sortedObjectTimes[peakRight] - result.windowDuration;
            while ( peakLeft < peakRight &&
                    sortedObjectTimes[peakLeft] < windowStart ) {
                ++peakLeft;
            }

            const double center =
                std::clamp(sortedObjectTimes[peakRight] - halfWindow,
                           0.0,
                           effectiveDuration);
            const std::size_t bin = std::min(
                binCount - 1,
                static_cast<std::size_t>(center / result.sampleInterval));
            const std::size_t count        = peakRight - peakLeft + 1;
            const auto        clampedCount = static_cast<std::uint32_t>(
                std::min(count,
                         static_cast<std::size_t>(
                             std::numeric_limits<std::uint32_t>::max())));
            result.counts[bin] = std::max(result.counts[bin], clampedCount);
            result.maxCount    = std::max(result.maxCount, clampedCount);
        }
    }

    return result;
}

}  // namespace MMM::Logic
