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
/// @note 输入是离散时间点，不解释物件类型或长条持续时间；采样含义由调用方决定。
/// @details counts 保存窗口内数量而非每秒速率，maxCount 用于显示归一化。
/// @pre maxBinCount 应为调用方可承担的快照容量，函数不会额外设定全局内存上限。
/// @warning 逻辑低频缓存重建路径：只允许在物件脏标记或总时长变化时调用；
/// 使用线性双指针统计，禁止放入每帧无条件路径。
PreviewDensitySnapshot buildPreviewDensitySnapshot(
    std::span<const double> sortedObjectTimes, double totalDuration,
    double windowDuration, double preferredSampleInterval,
    std::size_t maxBinCount)
{
    PreviewDensitySnapshot result;
    // 输入以 span 借用，只读统计不会排序或修改调用方的缓存。
    result.windowDuration =
        // 无效窗口回退到两秒，不能将零或负窗口带入左右边界计算。
        std::isfinite(windowDuration) && windowDuration > 0.0 ? windowDuration
                                                              : 2.0;

    double effectiveDuration =
        // 音频总长不可用时仍可用最后一个物件时间确定统计范围。
        std::isfinite(totalDuration) && totalDuration > 0.0 ? totalDuration
                                                            : 0.0;
    if ( !sortedObjectTimes.empty() &&
         std::isfinite(sortedObjectTimes.back()) ) {
        // 物件可以晚于主音轨结束，统计范围不能截掉谱尾的有效物件。
        effectiveDuration =
            std::max(effectiveDuration, sortedObjectTimes.back());
    }
    result.duration = effectiveDuration;
    // 即使没有可生成的样本，也保留规范化窗口和总时长供消费者判断空状态。

    if ( effectiveDuration <= 0.0 || sortedObjectTimes.empty() ||
         maxBinCount == 0 ) {
        // 禁止分配样本或缺少有效统计范围时直接返回，不制造虚假的零密度桶。
        return result;
    }

    const double preferredInterval =
        // 间隔只是显示采样的偏好，实际间隔由桶数上限和有效时长共同决定。
        std::isfinite(preferredSampleInterval) && preferredSampleInterval > 0.0
            ? preferredSampleInterval
            : 0.25;
    const double requestedBinCount =
        // 向上取整覆盖完整时长，短于一个偏好间隔的谱面也需要一个样本。
        std::ceil(effectiveDuration / preferredInterval);
    const std::size_t binCount = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(
            std::min(requestedBinCount, static_cast<double>(maxBinCount))));
    result.sampleInterval = effectiveDuration / static_cast<double>(binCount);
    // 均匀重新划分全长，末桶不留下单独的短尾段。
    result.counts.resize(binCount, 0);
    // 每个桶从零初始化，空窗口和后续未被峰值补充的桶保持明确的零值。

    const double halfWindow = result.windowDuration * 0.5;
    // 统计窗口宽度与桶间隔相互独立，降低显示分辨率不应改变密度统计时长。
    const double latestWindowStart =
        // 靠近末尾时平移整段窗口而非只裁去右半边，减少边缘密度低估。
        std::max(0.0, effectiveDuration - result.windowDuration);
    std::size_t leftIndex  = 0;
    std::size_t rightIndex = 0;
    // 窗口随桶中心单调前移，两个索引无需回退即可线性遍历物件时间。

    for ( std::size_t bin = 0; bin < binCount; ++bin ) {
        const double center =
            // 用桶中心作为常规采样位置，避免将每个样本偏向桶的左边界。
            (static_cast<double>(bin) + 0.5) * result.sampleInterval;
        const double windowStart =
            // 首尾窗口贴住谱面边界；总长小于窗口时只统计现有全长。
            std::clamp(center - halfWindow, 0.0, latestWindowStart);
        const double windowEnd =
            std::min(effectiveDuration, windowStart + result.windowDuration);

        while ( leftIndex < sortedObjectTimes.size() &&
                sortedObjectTimes[leftIndex] < windowStart ) {
            // 左边界相等的物件保留，统计窗口使用闭区间。
            ++leftIndex;
        }
        rightIndex = std::max(rightIndex, leftIndex);
        // 大间隔可能跨过一整段数据，右游标至少追到左游标以保持合法计数。
        while ( rightIndex < sortedObjectTimes.size() &&
                sortedObjectTimes[rightIndex] <= windowEnd ) {
            // 右游标停在首个超出窗口的物件，重复时间点仍各自计数。
            ++rightIndex;
        }

        const std::size_t count = rightIndex - leftIndex;
        // 内部使用容器宽度计数，写入快照的固定宽度字段前做饱和转换。
        const auto clampedCount = static_cast<std::uint32_t>(
            std::min(count,
                     static_cast<std::size_t>(
                         std::numeric_limits<std::uint32_t>::max())));
        result.counts[bin] = clampedCount;
        result.maxCount    = std::max(result.maxCount, clampedCount);
        // 最大值随已发布的饱和计数更新，显示范围与桶中实际值保持一致。
    }

    // 极长谱面在样本间隔大于固定窗口时，以每个窗口峰值补齐对应时间桶，
    // 避免稀疏物件恰好落在采样中心之间而完全消失。
    if ( result.sampleInterval > result.windowDuration ) {
        // 仅在常规窗口之间存在空隙时补峰值，普通密集采样无需再次扫描。
        std::size_t peakLeft = 0;
        for ( std::size_t peakRight = 0; peakRight < sortedObjectTimes.size();
              ++peakRight ) {
            // 以每个物件作为窗口右端，可覆盖那些完全落在常规采样空隙中的簇。
            const double windowStart =
                sortedObjectTimes[peakRight] - result.windowDuration;
            while ( peakLeft < peakRight &&
                    sortedObjectTimes[peakLeft] < windowStart ) {
                // 保留当前右端物件自身，非空簇的峰值至少为一。
                ++peakLeft;
            }

            const double center =
                // 将该候选窗口中心投到显示桶，不改变输出的均匀采样布局。
                std::clamp(sortedObjectTimes[peakRight] - halfWindow,
                           0.0,
                           effectiveDuration);
            const std::size_t bin = std::min(
                // 中心可能恰好等于总时长，此时商为 binCount，需归入最后一桶。
                binCount - 1,
                static_cast<std::size_t>(center / result.sampleInterval));
            const std::size_t count = peakRight - peakLeft + 1;
            // 峰值窗口索引两端均包含，区别于前面右端排他的窗口计数。
            const auto clampedCount = static_cast<std::uint32_t>(
                std::min(count,
                         static_cast<std::size_t>(
                             std::numeric_limits<std::uint32_t>::max())));
            result.counts[bin] = std::max(result.counts[bin], clampedCount);
            // 取最大而非累加，重叠候选窗口不能重复累计同一簇的密度。
            result.maxCount = std::max(result.maxCount, clampedCount);
        }
    }

    // 常规扫描和稀疏补峰共用同一最大值，消费者无需再遍历桶求峰值。
    return result;
}

}  // namespace MMM::Logic
