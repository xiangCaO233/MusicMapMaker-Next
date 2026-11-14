#pragma once
#include "common/ChartObjectKind.h"
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <vector>

namespace MMM::Logic
{
/// @brief 显式删除的领域身份，不借用选择集或悬浮对象。
/// @details 实体包含 EnTT 代数；不同 Registry 的同号对象必须带上领域。
/// 这里只表达整物件删除，折线局部断开继续使用节点命令。
struct ObjectDeletionTarget {
    /// @brief 空句柄没有替代目标，不允许回退悬浮或选择。
    entt::entity    m_entity{ entt::null };  ///< 本次看到的完整实体句柄。
    ChartObjectKind m_kind{ ChartObjectKind::PlayerNote };  ///< 句柄所属领域。
    /// @brief 去重比较同时包含领域和带代数句柄，不混淆不同 Registry。
    bool operator==(const ObjectDeletionTarget&) const = default;
};
/// @brief 一次明确目标的删除请求，空目标也不能退化为删除当前选区。
/// @details 以谱面实例隔离排队期间的切谱，不携带 UI 窗口或平台类型。
/// UI 只在释放时转移目标；逻辑线程复核领域、可编辑性及实体代数。
struct ObjectDeletion {
    /// @brief 实例标识只在进程内排队使用，不作为持久化或协作协议身份。
    std::uintptr_t m_beatmapInstanceId{};  ///< 与渲染快照相同的进程内实例身份。
    std::vector<ObjectDeletionTarget> m_targets;  ///< 一笔累计的显式根目标。
};
}  // namespace MMM::Logic
