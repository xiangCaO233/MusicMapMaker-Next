#pragma once

#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/EditorAction.h"
#include <entt/entt.hpp>
#include <optional>
#include <string>
#include <vector>

namespace MMM::Logic
{

/// @brief 时间线事件操作 (增/删/改)，例如修改 BPM。
class TimelineAction : public IEditorAction
{
public:
    /// @brief 操作类型
    enum class Type { Create, Delete, Update };

    /// @brief 构造函数
    /// @param type 操作类型
    /// @param entity 关联的 ECS 实体
    /// @param before 变更前的数据 (Update/Delete 需提供)
    /// @param after 变更后的数据 (Update/Create 需提供)
    TimelineAction(Type type, entt::entity entity,
                   std::optional<TimelineComponent> before,
                   std::optional<TimelineComponent> after)
        : m_type(type), m_entity(entity), m_before(before), m_after(after)
    {
    }

    void        execute(SessionContext& ctx) override;
    void        undo(SessionContext& ctx) override;
    void        redo(SessionContext& ctx) override;
    std::string getName() const override;
    /// @brief Timeline 操作始终修改 Timing 数据。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return ::MMM::BeatmapMutationFlags::Timelines;
    }

private:
    Type                             m_type;    ///< 操作类型
    entt::entity                     m_entity;  ///< 实体 ID
    std::optional<TimelineComponent> m_before;  ///< 变更前数据
    std::optional<TimelineComponent> m_after;   ///< 变更后数据
};

/// @brief 批量时间线事件操作，用于粘贴等需要合并撤销历史的场景。
class BatchTimelineAction : public IEditorAction
{
public:
    /// @brief 单个 Timeline 批量操作条目。
    struct Entry {
        /// @brief Timeline 实体。
        entt::entity entity{ entt::null };

        /// @brief 变更前数据；为空表示新建。
        std::optional<TimelineComponent> before;

        /// @brief 变更后数据；为空表示删除。
        std::optional<TimelineComponent> after;
    };

    /// @brief 构造函数。
    /// @param entries 批量 Timeline 操作条目。
    /// @param name 操作名称。
    BatchTimelineAction(std::vector<Entry> entries,
                        std::string        name = "Batch Timeline Action")
        : m_entries(std::move(entries)), m_name(std::move(name))
    {
    }

    void        execute(SessionContext& ctx) override;
    void        undo(SessionContext& ctx) override;
    void        redo(SessionContext& ctx) override;
    std::string getName() const override;
    /// @brief 批量 Timeline 操作始终修改 Timing 数据。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return ::MMM::BeatmapMutationFlags::Timelines;
    }

private:
    /// @brief 批量操作条目。
    std::vector<Entry> m_entries;

    /// @brief 操作名称。
    std::string m_name;
};

}  // namespace MMM::Logic
