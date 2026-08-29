#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace MMM::Logic
{

/// @brief 预览窗口全谱物件密度的固定时长滑动窗口快照。
struct PreviewDensitySnapshot {
    /// @brief 各采样时间窗口内的物件数量，按时间升序排列。
    std::vector<std::uint32_t> counts;

    /// @brief 密度时间轴覆盖的谱面总时长，单位秒。
    double duration{ 0.0 };

    /// @brief 相邻密度采样中心的时间间隔，单位秒。
    double sampleInterval{ 0.0 };

    /// @brief 每个密度样本使用的固定滑动窗口时长，单位秒。
    double windowDuration{ 2.0 };

    /// @brief 当前快照中的最大窗口物件数，用于 UI 归一化条宽。
    std::uint32_t maxCount{ 0 };

    /// @brief 清空密度样本并恢复默认元数据，同时保留向量容量。
    /// @warning 快照热路径：只清空已分配容器并重置常量字段，不释放容量。
    void clear()
    {
        counts.clear();
        duration       = 0.0;
        sampleInterval = 0.0;
        windowDuration = 2.0;
        maxCount       = 0;
    }
};

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
    double windowDuration = 2.0, double preferredSampleInterval = 0.25,
    std::size_t maxBinCount = 512);

}  // namespace MMM::Logic
