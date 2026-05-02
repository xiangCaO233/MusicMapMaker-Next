#pragma once

#include "mmm/note/Note.h"
#include <entt/entity/entity.hpp>
#include <vector>

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

    /// @brief 滑动轨道偏移 (如果是 Flick 类型)
    int m_dtrack{ 0 };

    /// @brief 是否为折线内部子物件（如果是，则在标准渲染流程中跳过）
    bool m_isSubNote{ false };

    /// @brief 如果是子物件，指向所属的 Polyline 父实体
    entt::entity m_parentPolyline{ entt::null };

    /// @brief 如果是子物件，记录其在父物件中的索引
    int m_subIndex{ -1 };

    /// @brief 原始元数据备份 (用于导出时保持结构一致性)
    ::MMM::NoteMetadata m_metadata;

    /// @brief 折线子物件定义 (如果是 Polyline 类型)
    struct SubNote {
        ::MMM::NoteType     type;
        double              timestamp;
        double              duration;
        int                 trackIndex;
        int                 dtrack;
        ::MMM::NoteMetadata metadata;
    };
    std::vector<SubNote> m_subNotes;
};

}  // namespace MMM::Logic