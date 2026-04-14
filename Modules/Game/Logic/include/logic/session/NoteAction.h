#pragma once

#include "logic/ecs/components/NoteComponent.h"
#include "logic/session/EditorAction.h"
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
    }

    void execute(SessionContext& ctx) override;
    void undo(SessionContext& ctx) override;
    void redo(SessionContext& ctx) override;

private:
    Type                         m_type;    ///< 操作类型
    entt::entity                 m_entity;  ///< 实体 ID
    std::optional<NoteComponent> m_before;  ///< 变更前数据
    std::optional<NoteComponent> m_after;   ///< 变更后数据
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
    };

    /// @brief 构造函数
    /// @param entries 批量操作条目列表
    BatchNoteAction(std::vector<Entry> entries) : m_entries(std::move(entries))
    {
    }

    void execute(SessionContext& ctx) override;
    void undo(SessionContext& ctx) override;
    void redo(SessionContext& ctx) override;

private:
    std::vector<Entry> m_entries;  ///< 条目列表
};

}  // namespace MMM::Logic
