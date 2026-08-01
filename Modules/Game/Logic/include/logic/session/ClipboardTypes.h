#pragma once

#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/components/TimelineComponent.h"

#include <cstdint>
#include <vector>

namespace MMM::Logic
{

/// @brief 音符剪贴板条目，保留物件值和复制时的分拍位置。
struct ClipboardItem {
    NoteComponent       note;              ///< 复制的音符组件数据。
    double              startBeat{ 0.0 };  ///< 复制时的起始分拍位置。
    double              endBeat{ 0.0 };    ///< 复制时的结束分拍位置。
    std::vector<double> subStartBeats;     ///< 折线子物件起始分拍位置。
    std::vector<double> subEndBeats;       ///< 折线子物件结束分拍位置。
    bool hasBeatPositions{ false };  ///< 是否已记录可用于按分拍粘贴的位置。
};

/// @brief 自动采样剪贴板条目。
struct SampleClipboardItem {
    /// @brief 复制的自动采样组件数据；其中 m_track 已归一化为 BGM 相对索引。
    SampleComponent sample;

    /// @brief 相对玩家轨道区右边界的 BGM 轨道索引。
    std::uint32_t bgmLane{ 0 };

    /// @brief 复制瞬间采样锚点对应的连续 beat 位置。
    double startBeat{ 0.0 };

    /// @brief 是否已记录可用于按分拍粘贴的位置。
    bool hasBeatPosition{ false };
};

/// @brief Timeline 事件剪贴板条目。
struct TimelineClipboardItem {
    /// @brief 复制的 Timeline 组件数据。
    TimelineComponent timeline;

    /// @brief 相对剪贴板锚点时间，单位秒。
    double relativeTime{ 0.0 };

    /// @brief 相对剪贴板锚点的连续 beat 偏移。
    double relativeBeat{ 0.0 };

    /// @brief 是否已记录可用于按分拍粘贴的位置。
    bool hasBeatPosition{ false };
};

}  // namespace MMM::Logic
