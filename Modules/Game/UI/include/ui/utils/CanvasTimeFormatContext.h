#pragma once

#include <vector>

namespace MMM::UI::Utils
{

/// @brief 不依赖画布渲染快照的时间格式化 BPM 节点。
struct CanvasTimeFormatBpmPoint {
    /// @brief 节点起始时间，单位秒。
    double time{ 0.0 };

    /// @brief 节点 BPM。
    double bpm{ 120.0 };
};

/// @brief 独立窗口格式化谱面时间所需的轻量上下文。
struct CanvasTimeFormatContext {
    /// @brief 按时间升序排列且已经去重的 BPM 节点。
    std::vector<CanvasTimeFormatBpmPoint> bpmPoints;

    /// @brief 当前分拍数。
    int beatDivisor{ 4 };
};

}  // namespace MMM::UI::Utils
