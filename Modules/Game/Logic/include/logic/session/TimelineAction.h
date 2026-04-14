#pragma once

#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/EditorAction.h"
#include <entt/entt.hpp>
#include <optional>

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

    void execute(SessionContext& ctx) override;
    void undo(SessionContext& ctx) override;
    void redo(SessionContext& ctx) override;

private:
    Type                             m_type;    ///< 操作类型
    entt::entity                     m_entity;  ///< 实体 ID
    std::optional<TimelineComponent> m_before;  ///< 变更前数据
    std::optional<TimelineComponent> m_after;   ///< 变更后数据
};

}  // namespace MMM::Logic
