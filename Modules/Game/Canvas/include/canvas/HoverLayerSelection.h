#pragma once

#include "common/ChartObjectKind.h"

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <vector>

namespace MMM::Canvas
{

/// @brief 主画布指针位置下可切换的单个物件候选。
struct HoverLayerCandidate {
    /// @brief 候选实体。
    entt::entity entity{ entt::null };
    /// @brief 实体所在的独立 ECS 注册表。
    Logic::ChartObjectKind kind{ Logic::ChartObjectKind::PlayerNote };
    /// @brief 当前优先命中的物件部位。
    std::uint8_t part{ 0 };
    /// @brief Polyline 等复合物件的子部位索引。
    int subIndex{ -1 };
};

/// @brief 追加悬浮物件候选，并合并同一自动采样的主体与偏移句柄。
/// @param candidates 已按渲染顶层优先级排列的候选列表。
/// @param candidate 待追加候选；同一自动采样已有候选时保留先到的顶层部位。
/// @return 成功追加新候选时返回 true，同一自动采样已有候选时返回 false。
/// @warning UI 热路径：每个命中框调用；仅扫描当前指针下的少量候选，不得访问
/// ECS。
inline bool appendHoverLayerCandidate(
    std::vector<HoverLayerCandidate>& candidates,
    const HoverLayerCandidate&        candidate)
{
    if ( candidate.kind == Logic::ChartObjectKind::AudioSample ) {
        for ( const auto& existing : candidates ) {
            if ( existing.entity == candidate.entity &&
                 existing.kind == candidate.kind ) {
                return false;
            }
        }
    }
    candidates.push_back(candidate);
    return true;
}

}  // namespace MMM::Canvas
