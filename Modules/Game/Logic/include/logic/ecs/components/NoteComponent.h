#pragma once

#include "mmm/note/Note.h"
#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

namespace MMM::Logic
{

/// @brief 音符局部自定义颜色缓存；为空时使用当前皮肤默认颜色。
struct NoteColorOverrides {
    /// @brief 普通 Note 本体颜色，对应 note_tap。
    std::optional<glm::vec4> tap;

    /// @brief Hold/Flick/Polyline 头部颜色，对应 note_head。
    std::optional<glm::vec4> head;

    /// @brief Hold/Flick/Polyline 连接体颜色，对应 note_hold。
    std::optional<glm::vec4> hold;

    /// @brief Hold/Flick/Polyline 尾部颜色，对应 note_end。
    std::optional<glm::vec4> end;

    /// @brief Flick/Polyline 滑键箭头颜色，对应 note_flick_arrow。
    std::optional<glm::vec4> flickArrow;

    /// @brief Polyline 中间节点颜色，对应 note_node。
    std::optional<glm::vec4> node;
};

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

    /// @brief 物件绑定的自定义音效资源标识；为空时使用内置打击音效。
    std::string m_boundSound;

    /// @brief 自定义音符配色缓存；保存时同步写入 m_metadata。
    NoteColorOverrides m_customColors;

    /// @brief 折线子物件定义 (如果是 Polyline 类型)
    struct SubNote {
        ::MMM::NoteType     type;
        double              timestamp;
        double              duration;
        int                 trackIndex;
        int                 dtrack;
        ::MMM::NoteMetadata metadata;
        /// @brief 子物件绑定的自定义音效资源标识。
        std::string boundSound;
        /// @brief 子物件自定义颜色缓存；为空时继承皮肤默认色。
        NoteColorOverrides customColors;
    };
    std::vector<SubNote> m_subNotes;
};

}  // namespace MMM::Logic
