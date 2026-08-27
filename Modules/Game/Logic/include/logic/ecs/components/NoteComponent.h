#pragma once

#include "common/render/NoteRenderData.h"
#include "mmm/note/Note.h"
#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

namespace MMM::Logic
{

/// @brief 音符局部自定义颜色缓存兼容别名。
using NoteColorOverrides = Common::Render::NoteColorOverrides;

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

    /// @brief 是否属于项目级草稿轨而非当前正式谱面。
    bool m_isDraft{ false };

    /// @brief 如果是子物件，指向所属的 Polyline 父实体
    entt::entity m_parentPolyline{ entt::null };

    /// @brief 如果是子物件，记录其在父物件中的索引
    int m_subIndex{ -1 };

    /// @brief 原始元数据备份 (用于导出时保持结构一致性)
    ::MMM::NoteMetadata m_metadata;

    /// @brief 整个玩家物件的编辑器注释。
    std::string m_annotation;

    /// @brief 物件命中时触发的可选采样绑定；为空时使用内置打击音效。
    std::optional<::MMM::AudioSampleBinding> m_sampleBinding;

    /// @brief 自定义音符配色缓存；保存时同步写入 m_metadata。
    NoteColorOverrides m_customColors;

    /// @brief 折线子物件定义兼容别名。
    using SubNote = Common::Render::PolylineSubNote;
    std::vector<SubNote> m_subNotes;

    /// @brief 协作会话内稳定的逻辑物件标识。
    std::string m_collaborationId;
};

}  // namespace MMM::Logic
