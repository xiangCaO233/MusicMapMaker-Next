#pragma once

#include <cstdint>
#include <vector>

namespace MMM::Common::Render
{

/// @brief BPM 分段的起点、规范化节奏和段首前已完成的拍数。
/// @details 段首拍数按上一段独立舍入后累计，以保持 BPM 变化处的既有编号。
struct BeatTimePoint {
    /// @brief 时间线节点，以秒为单位，允许位于零时刻之前。
    double m_time;
    /// @brief 用于界面换算的正 BPM，已归一化。
    double m_bpm;
    /// @brief 从首节点到本节点的累计拍数，不含本段。
    std::int64_t m_completedBeats;
};

/// @brief 在同一 BPM 修订内保持不变的拍号查询索引。
/// @note 节点严格升序并已去重；查询直接二分节点，无需读取滚动速度表。
struct BeatTimeIndex {
    /// @brief 跨视口共享的 BPM 节点及累计值。
    std::vector<BeatTimePoint> m_points;
};

}  // namespace MMM::Common::Render
