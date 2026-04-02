#pragma once

#include "mmm/note/Note.h"

namespace MMM::Logic
{

/**
 * @brief 谱面音符组件，用于 ECS 逻辑计算
 */
struct NoteComponent {
    /// @brief 物件类型
    ::MMM::NoteType m_type{ ::MMM::NoteType::NOTE };

    /// @brief 触发时间 (毫秒或秒，这里统一使用秒)
    double m_timestamp{ 0.0 };

    /// @brief 持续时间 (如果是 Hold 类型)
    double m_duration{ 0.0 };

    /// @brief 所属轨道索引
    int m_trackIndex{ 0 };
};

}  // namespace MMM::Logic