#pragma once

#include <entt/entity/entity.hpp>

#include <vector>

namespace MMM::Logic
{

/// @brief 供渲染系统从 ECS 上下文借用当前手势的实体列表。
/// @details 同一逻辑线程在生成快照前更新该视图，渲染候选查询只读取它。
/// 列表的生命周期由手势或渲染调用者维持，注册表上下文不拥有该列表。
struct DragRenderPinnedEntities {
    /// @brief 需要绕过静态可见性索引补充渲染的实体，空指针表示没有拖动。
    const std::vector<entt::entity>* entities{ nullptr };
};

}  // namespace MMM::Logic
