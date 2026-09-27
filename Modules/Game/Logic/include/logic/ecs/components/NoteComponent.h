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

    /// @brief 触发时间，统一使用秒；领域对象的毫秒换算在载入与保存边界完成。
    double m_timestamp{ 0.0 };

    /// @brief 持续时间 (如果是 Hold 类型)
    double m_duration{ 0.0 };

    /// @brief 所属轨道索引；负值表示当前谱面左侧草稿区，非负值用于正式谱面。
    int m_trackIndex{ 0 };

    /// @brief 滑动轨道偏移 (如果是 Flick 类型)
    int m_dtrack{ 0 };

    /// @brief 是否为折线内部子物件（如果是，则在标准渲染流程中跳过）
    bool m_isSubNote{ false };

    /// @brief 是否属于当前谱面草稿轨而非正式内容。
    bool m_isDraft{ false };

    /// @brief 子物件所属的 Polyline 父实体，仅在当前会话注册表中有效。
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
    /// @brief 折线根物件的子节点数据，子实体通过父句柄与序号关联到此列表。
    std::vector<SubNote> m_subNotes;

    /// @brief 保存、协作及同谱面草稿同步中稳定的逻辑物件标识，不等同于 entt
    /// 实体句柄。
    std::string m_collaborationId;
};

}  // namespace MMM::Logic
