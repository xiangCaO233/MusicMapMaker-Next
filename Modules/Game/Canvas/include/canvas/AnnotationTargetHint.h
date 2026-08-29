#pragma once

#include "common/render/AnnotationRenderData.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "logic/session/CanvasCamera.h"

#include <algorithm>
#include <optional>
#include <span>

namespace MMM::Canvas
{

/// @brief 批注目标提示框在画布局部坐标中的边界。
struct AnnotationTargetHintBounds {
    /// @brief 左边界。
    float left{ 0.0F };
    /// @brief 上边界。
    float top{ 0.0F };
    /// @brief 右边界。
    float right{ 0.0F };
    /// @brief 下边界。
    float bottom{ 0.0F };
};

/// @brief 从当前可见命中框解析批注实际指向的物件几何。
/// @param item 当前悬浮的批注。
/// @param hitboxes 当前主画布已生成的可见物件命中框。
/// @param padding 目标物件四周追加的视觉留白。
/// @param minimumExtent 提示框横纵方向允许的最小尺寸。
/// @return 目标可见时返回合并后的提示边界；时间戳批注或目标不可见时为空。
/// @warning UI 热路径：仅在悬浮批注详情卡片时扫描当前可见命中框，不得访问
/// ECS 或文件系统。
[[nodiscard]] inline std::optional<AnnotationTargetHintBounds>
findAnnotationTargetHintBounds(const Common::Render::AnnotationRenderItem& item,
                               std::span<const Common::Render::Hitbox> hitboxes,
                               float padding       = 5.0F,
                               float minimumExtent = 32.0F)
{
    if ( item.targetMissing || item.targetEntity == entt::null ||
         item.targetKind == ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP ) {
        return std::nullopt;
    }

    const bool targetIsAudioSample =
        item.targetKind == ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE;
    bool  found  = false;
    float left   = 0.0F;
    float top    = 0.0F;
    float right  = 0.0F;
    float bottom = 0.0F;
    for ( const auto& hitbox : hitboxes ) {
        const bool kindMatches =
            targetIsAudioSample
                ? hitbox.kind == Logic::ChartObjectKind::AudioSample
                : (hitbox.kind == Logic::ChartObjectKind::PlayerNote ||
                   hitbox.kind == Logic::ChartObjectKind::DraftNote);
        if ( hitbox.entity != item.targetEntity || !kindMatches ||
             hitbox.w < 0.0F || hitbox.h < 0.0F ||
             (item.targetSubIndex >= 0 &&
              hitbox.subIndex != item.targetSubIndex) ) {
            continue;
        }
        const float hitboxRight  = hitbox.x + hitbox.w;
        const float hitboxBottom = hitbox.y + hitbox.h;
        if ( !found ) {
            left   = hitbox.x;
            top    = hitbox.y;
            right  = hitboxRight;
            bottom = hitboxBottom;
            found  = true;
        } else {
            left   = std::min(left, hitbox.x);
            top    = std::min(top, hitbox.y);
            right  = std::max(right, hitboxRight);
            bottom = std::max(bottom, hitboxBottom);
        }
    }
    if ( !found ) return std::nullopt;

    const float safePadding       = std::max(0.0F, padding);
    const float safeMinimumExtent = std::max(0.0F, minimumExtent);
    const float centerX           = (left + right) * 0.5F;
    const float centerY           = (top + bottom) * 0.5F;
    const float hintWidth =
        std::max(right - left + safePadding * 2.0F, safeMinimumExtent);
    const float hintHeight =
        std::max(bottom - top + safePadding * 2.0F, safeMinimumExtent);
    return AnnotationTargetHintBounds{
        centerX - hintWidth * 0.5F,
        centerY - hintHeight * 0.5F,
        centerX + hintWidth * 0.5F,
        centerY + hintHeight * 0.5F,
    };
}

/// @brief 计算批注连线在目标物件轨道上的起点横坐标。
/// @param item 批注展示数据。
/// @param projection 当前画布横向投影。
/// @param fallbackX 时间戳、丢失目标或无效轨道使用的批注栏中心。
/// @return 正式物件、草稿物件或自动采样所在轨道的中心横坐标。
/// @warning UI 热路径：每张可见详情卡片调用一次，只执行常量级投影查询。
[[nodiscard]] inline float annotationConnectorSourceX(
    const Common::Render::AnnotationRenderItem& item,
    const Logic::CanvasLaneProjection& projection, float fallbackX)
{
    if ( item.targetMissing ||
         item.targetKind == ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP ) {
        return fallbackX;
    }

    std::optional<Logic::CanvasLaneBounds> bounds;
    if ( item.targetKind ==
         ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT ) {
        const auto minimumDraftTrack =
            -static_cast<std::int32_t>(projection.draftLaneCount);
        if ( item.track < minimumDraftTrack ||
             item.track >=
                 static_cast<std::int32_t>(projection.playerLaneCount) ) {
            return fallbackX;
        }
        const auto address = Logic::CanvasLaneAddress::fromAbsoluteTrack(
            item.track, projection.playerLaneCount, projection.draftLaneCount);
        bounds = projection.bounds(address);
    } else if ( item.targetKind ==
                ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE ) {
        const auto track = static_cast<std::uint32_t>(item.track);
        if ( track < projection.playerLaneCount ) {
            bounds =
                projection.bounds({ Logic::CanvasLaneKind::Player, track });
        } else {
            bounds = projection.bounds({ Logic::CanvasLaneKind::Bgm,
                                         track - projection.playerLaneCount });
        }
    }
    return bounds ? (bounds->leftX + bounds->rightX) * 0.5F : fallbackX;
}

}  // namespace MMM::Canvas
