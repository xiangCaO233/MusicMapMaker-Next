#pragma once

#include "logic/ecs/components/NoteComponent.h"
#include "logic/session/EditorAction.h"
#include <algorithm>
#include <entt/entt.hpp>
#include <optional>

namespace MMM::Logic
{

/// @brief 单个音符操作 (增/删/改)。
class NoteAction : public IEditorAction
{
public:
    /// @brief 操作类型
    enum class Type { Update, Create, Delete };

    /// @brief 构造函数
    /// @param type 操作类型
    /// @param entity 关联的实体
    /// @param before 变更前数据
    /// @param after 变更后数据
    NoteAction(Type type, entt::entity entity,
               std::optional<NoteComponent> before,
               std::optional<NoteComponent> after)
        : m_type(type), m_entity(entity), m_before(before), m_after(after)
    {
        if ( m_type == Type::Create && m_after ) {
            m_after->m_collaborationId.clear();
            for ( auto& subNote : m_after->m_subNotes ) {
                subNote.collaborationId.clear();
            }
        }
    }

    void        execute(SessionContext& ctx) override;
    void        undo(SessionContext& ctx) override;
    void        redo(SessionContext& ctx) override;
    std::string getName() const override;
    /// @brief 草稿专属操作不发布谱面变更，其余操作修改主谱面物件。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        const bool hasFormalBefore = m_before && !m_before->m_isDraft;
        const bool hasFormalAfter  = m_after && !m_after->m_isDraft;
        if ( !hasFormalBefore && !hasFormalAfter ) {
            return ::MMM::BeatmapMutationFlags::None;
        }
        return ::MMM::BeatmapMutationFlags::Objects;
    }

private:
    Type                         m_type;    ///< 操作类型
    entt::entity                 m_entity;  ///< 实体 ID
    std::optional<NoteComponent> m_before;  ///< 变更前数据
    std::optional<NoteComponent> m_after;   ///< 变更后数据
    /// @brief 首次执行前的草稿轨道数量。
    std::optional<std::int32_t> m_beforeDraftTrackCount;
    /// @brief 为容纳变更后草稿物件计算出的轨道数量。
    std::optional<std::int32_t> m_afterDraftTrackCount;
};

/// @brief 批量音符操作 (用于粘贴/批量删除)，提高处理效率并合并撤销历史。
class BatchNoteAction : public IEditorAction
{
public:
    /// @brief 单个条目的记录
    struct Entry {
        entt::entity                 entity;  ///< 实体 ID
        std::optional<NoteComponent> before;  ///< 变更前数据 (null 表示新建)
        std::optional<NoteComponent> after;   ///< 变更后数据 (null 表示删除)
        /// @brief 变更前的选中状态；为空时不覆盖交互组件。
        std::optional<bool> beforeSelected;
        /// @brief 变更后的选中状态；为空时不覆盖交互组件。
        std::optional<bool> afterSelected;
    };

    /// @brief 构造函数
    /// @param entries 批量操作条目列表
    /// @param name 操作描述名称
    /// @param mutationFlags 动作对协作谱面造成的精确变更类别。
    BatchNoteAction(std::vector<Entry>          entries,
                    std::string                 name = "Batch Note Action",
                    ::MMM::BeatmapMutationFlags mutationFlags =
                        ::MMM::BeatmapMutationFlags::Objects)
        : m_entries(std::move(entries))
        , m_name(std::move(name))
        , m_mutationFlags(mutationFlags)
    {
        for ( auto& entry : m_entries ) {
            if ( entry.before || !entry.after ) continue;
            entry.after->m_collaborationId.clear();
            for ( auto& subNote : entry.after->m_subNotes ) {
                subNote.collaborationId.clear();
            }
        }
    }

    void        execute(SessionContext& ctx) override;
    void        undo(SessionContext& ctx) override;
    void        redo(SessionContext& ctx) override;
    std::string getName() const override;
    /// @brief 返回该批量动作声明的精确谱面变更类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        const bool hasFormalNote = std::any_of(
            m_entries.begin(), m_entries.end(), [](const Entry& entry) {
                return (entry.before && !entry.before->m_isDraft) ||
                       (entry.after && !entry.after->m_isDraft);
            });
        if ( !hasFormalNote ) return ::MMM::BeatmapMutationFlags::None;
        return m_mutationFlags;
    }

private:
    std::vector<Entry> m_entries;  ///< 条目列表
    std::string        m_name;     ///< 操作名称
    /// @brief 首次执行前的草稿轨道数量。
    std::optional<std::int32_t> m_beforeDraftTrackCount;
    /// @brief 为容纳批量变更后草稿物件计算出的轨道数量。
    std::optional<std::int32_t> m_afterDraftTrackCount;
    /// @brief 该动作对协作同步声明的精确变更类别。
    ::MMM::BeatmapMutationFlags m_mutationFlags{
        ::MMM::BeatmapMutationFlags::Objects
    };
};

}  // namespace MMM::Logic
