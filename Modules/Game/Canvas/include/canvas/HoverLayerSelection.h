#pragma once

#include "common/ChartObjectKind.h"

#include <algorithm>
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
    /// @brief 候选物件实际渲染几何的左边界。
    float x{ 0.0F };
    /// @brief 候选物件实际渲染几何的上边界。
    float y{ 0.0F };
    /// @brief 候选物件实际渲染几何的宽度。
    float width{ 0.0F };
    /// @brief 候选物件实际渲染几何的高度。
    float height{ 0.0F };
};

/// @brief 悬浮提示框在画布局部坐标中的边界。
struct HoverHintBounds {
    /// @brief 左边界。
    float left{ 0.0F };
    /// @brief 上边界。
    float top{ 0.0F };
    /// @brief 右边界。
    float right{ 0.0F };
    /// @brief 下边界。
    float bottom{ 0.0F };
};

/// @brief 按真实几何中心扩展悬浮提示框，并保证小物件仍清晰可见。
/// @param candidate 当前悬浮层候选。
/// @param padding 真实几何四周追加的视觉留白。
/// @param minimumExtent 提示框横纵方向允许的最小尺寸。
/// @return 保持物件中心不变的悬浮提示框边界。
/// @warning UI 热路径：每帧至多调用一次，只执行常量级算术。
[[nodiscard]] inline HoverHintBounds calculateHoverHintBounds(
    const HoverLayerCandidate& candidate, float padding = 5.0F,
    float minimumExtent = 32.0F)
{
    const float safePadding       = std::max(0.0F, padding);
    const float safeMinimumExtent = std::max(0.0F, minimumExtent);
    const float sourceWidth       = std::max(0.0F, candidate.width);
    const float sourceHeight      = std::max(0.0F, candidate.height);
    const float centerX           = candidate.x + sourceWidth * 0.5F;
    const float centerY           = candidate.y + sourceHeight * 0.5F;
    const float hintWidth =
        std::max(sourceWidth + safePadding * 2.0F, safeMinimumExtent);
    const float hintHeight =
        std::max(sourceHeight + safePadding * 2.0F, safeMinimumExtent);
    return {
        centerX - hintWidth * 0.5F,
        centerY - hintHeight * 0.5F,
        centerX + hintWidth * 0.5F,
        centerY + hintHeight * 0.5F,
    };
}

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
