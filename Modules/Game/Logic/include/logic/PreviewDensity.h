#pragma once

#include "common/render/ScrollRenderData.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace MMM::Logic
{

/// @brief 预览密度快照兼容别名。
using PreviewDensitySnapshot = Common::Render::PreviewDensitySnapshot;

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
