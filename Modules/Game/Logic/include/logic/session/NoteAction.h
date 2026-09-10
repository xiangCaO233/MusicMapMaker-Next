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
    /// @note 前后快照按值保存，不借用外部组件的可变存储。
    NoteAction(Type type, entt::entity entity,
               std::optional<NoteComponent> before,
               std::optional<NoteComponent> after)
        : m_type(type), m_entity(entity), m_before(before), m_after(after)
    {
        if ( m_type == Type::Create && m_after ) {
            // 新建可能来自复制，不能沿用来源物件的协作身份。
            // 这里只清理动作持有的快照，不改动调用方的原始数据。
            m_after->m_collaborationId.clear();
            for ( auto& subNote : m_after->m_subNotes ) {
                // 折线子节点也有独立身份，需要与根物件一起重新分配。
                subNote.collaborationId.clear();
            }
        }
    }

    /// @brief 首次应用音符变化，建立后续撤销所需的状态。
    void execute(SessionContext& ctx) override;
    /// @brief 恢复变化前的音符及相关草稿轨道状态。
    void undo(SessionContext& ctx) override;
    /// @brief 重新应用变化后的音符状态。
    void redo(SessionContext& ctx) override;
    /// @brief 获取该操作类型的可读名称。
    std::string getName() const override;
    /// @brief 草稿专属操作不发布谱面变更，其余操作修改主谱面物件。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        const bool hasFormalBefore = m_before && !m_before->m_isDraft;
        // 前后任一侧属于正式谱面就必须通知，包括正式物件删除或转为草稿。
        const bool hasFormalAfter = m_after && !m_after->m_isDraft;
        if ( !hasFormalBefore && !hasFormalAfter ) {
            // 纯草稿由项目草稿同步处理，不触发主谱面物件类别通知。
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
    /// @note entries 转交动作持有，调用方后续编辑原列表不会改变撤销快照。
    BatchNoteAction(std::vector<Entry>          entries,
                    std::string                 name = "Batch Note Action",
                    ::MMM::BeatmapMutationFlags mutationFlags =
                        ::MMM::BeatmapMutationFlags::Objects)
        : m_entries(std::move(entries))
        , m_name(std::move(name))
        , m_mutationFlags(mutationFlags)
    {
        for ( auto& entry : m_entries ) {
            // 只有无 before 且有 after 的条目是新建，更新必须保留原身份。
            if ( entry.before || !entry.after ) continue;
            entry.after->m_collaborationId.clear();
            for ( auto& subNote : entry.after->m_subNotes ) {
                // 粘贴整条折线时，子节点也不能与来源折线共享协作 ID。
                subNote.collaborationId.clear();
            }
        }
    }

    /// @brief 应用本批音符变化，作为单个撤销历史条目处理。
    void execute(SessionContext& ctx) override;
    /// @brief 恢复本批条目的 before 快照和已记录的选中状态。
    void undo(SessionContext& ctx) override;
    /// @brief 恢复本批条目的 after 快照和已记录的选中状态。
    void redo(SessionContext& ctx) override;
    /// @brief 获取调用方指定的批量操作名称。
    std::string getName() const override;
    /// @brief 返回该批量动作声明的精确谱面变更类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        const bool hasFormalNote = std::any_of(
            // 前态也参与判断，保证仅含正式物件删除的批次仍会发出通知。
            // 混合批次只要包含正式物件变化，就不能按纯草稿忽略。
            m_entries.begin(),
            m_entries.end(),
            [](const Entry& entry) {
                return (entry.before && !entry.before->m_isDraft) ||
                       (entry.after && !entry.after->m_isDraft);
            });
        if ( !hasFormalNote ) return ::MMM::BeatmapMutationFlags::None;
        // 保留调用方声明的精确类别，避免扩大为完整物件变更。
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

/// @brief 改变当前谱面独占草稿区的持久化轨道数量。
class DraftTrackCountAction : public IEditorAction
{
public:
    /// @brief 构造草稿轨道数量操作。
    /// @param beforeDraftTrackCount 原持久化草稿轨道数量。
    /// @param afterDraftTrackCount 新持久化草稿轨道数量。
    DraftTrackCountAction(std::int32_t beforeDraftTrackCount,
                          std::int32_t afterDraftTrackCount)
        : m_beforeDraftTrackCount(beforeDraftTrackCount)
        , m_afterDraftTrackCount(afterDraftTrackCount)
    {
    }

    /// @brief 应用新的草稿轨道数量。
    void execute(SessionContext& ctx) override;
    /// @brief 恢复原草稿轨道数量。
    void undo(SessionContext& ctx) override;
    /// @brief 重新应用新的草稿轨道数量。
    void redo(SessionContext& ctx) override;
    /// @brief 获取增加或删除草稿轨道的本地化操作名称。
    std::string getName() const override;
    /// @brief 草稿轨数只更新项目侧车草稿，不属于 BeatMap 正式内容变更。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return ::MMM::BeatmapMutationFlags::None;
    }

private:
    std::int32_t m_beforeDraftTrackCount{ 1 };  ///< 原草稿轨道数量。
    std::int32_t m_afterDraftTrackCount{ 1 };   ///< 新草稿轨道数量。
};

}  // namespace MMM::Logic
