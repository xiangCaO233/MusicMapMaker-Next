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

/// 提示边界使用 ImGui 画布局部坐标，不包含窗口位置或滚动偏移。调用方应在
/// 同一坐标系中绘制边框，不能把结果作为谱面时间或轨道坐标持久化。

/// @brief 从当前可见命中框解析批注实际指向的物件几何。
/// @param item 当前悬浮的批注。
/// @param hitboxes 当前主画布已生成的可见物件命中框。
/// @param padding 目标物件四周追加的视觉留白。
/// @param minimumExtent 提示框横纵方向允许的最小尺寸。
/// @return 目标可见时返回合并后的提示边界；时间戳批注或目标不可见时为空。
///
/// 同一目标可对应多个渲染命中框，例如长条的头部与主体。函数只合并当前
/// 可见快照中的匹配框，不尝试补全离屏部分，因此返回值仅服务本帧提示。
/// @warning UI 热路径：仅在悬浮批注详情卡片时扫描当前可见命中框，不得访问
/// ECS 或文件系统。
[[nodiscard]] inline std::optional<AnnotationTargetHintBounds>
findAnnotationTargetHintBounds(const Common::Render::AnnotationRenderItem& item,
                               std::span<const Common::Render::Hitbox> hitboxes,
                               float padding       = 5.0F,
                               float minimumExtent = 32.0F)
{
    // 时间戳批注没有物件目标；丢失标记还阻止实体编号复用后的误匹配。
    if ( item.targetMissing || item.targetEntity == entt::null ||
         item.targetKind == ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP ) {
        return std::nullopt;
    }

    const bool targetIsAudioSample =
        item.targetKind == ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE;
    // 同一实体可能产生头部、长条和控制点等多个命中框，需要合并为一个
    // 连续提示区域；首个有效框负责初始化，后续框再扩展四条边界。
    bool  found  = false;
    float left   = 0.0F;
    float top    = 0.0F;
    float right  = 0.0F;
    float bottom = 0.0F;
    for ( const auto& hitbox : hitboxes ) {
        // 玩家物件同时接受正式物件和草稿物件；自动采样必须严格匹配
        // AudioSample，防止 entt 编号碰巧相同的其它渲染项参与合并。
        const bool kindMatches =
            targetIsAudioSample
                ? hitbox.kind == Logic::ChartObjectKind::AudioSample
                : (hitbox.kind == Logic::ChartObjectKind::PlayerNote ||
                   hitbox.kind == Logic::ChartObjectKind::DraftNote);
        if ( hitbox.entity != item.targetEntity || !kindMatches ||
             hitbox.w < 0.0F || hitbox.h < 0.0F ||
             (item.targetSubIndex >= 0 &&
              hitbox.subIndex != item.targetSubIndex) ) {
            // subIndex 为负时表示整个父物件，允许合并其全部可见部件。
            continue;
        }
        // 负尺寸代表无效几何，不能参与边界归并；零尺寸仍可由最小尺寸扩张。
        const float hitboxRight  = hitbox.x + hitbox.w;
        const float hitboxBottom = hitbox.y + hitbox.h;
        if ( !found ) {
            // 首个匹配项建立初始包围盒，避免用零值污染全为负坐标的轨道。
            left   = hitbox.x;
            top    = hitbox.y;
            right  = hitboxRight;
            bottom = hitboxBottom;
            found  = true;
        } else {
            // Polyline 同一节点的头部与长条段共享 subIndex，需覆盖全部部件。
            left   = std::min(left, hitbox.x);
            top    = std::min(top, hitbox.y);
            right  = std::max(right, hitboxRight);
            bottom = std::max(bottom, hitboxBottom);
        }
    }
    // 目标当前不在可见命中框中时不绘制提示，不能访问 ECS 反查离屏几何。
    if ( !found ) return std::nullopt;

    // 外部参数先限制为非负值，保持提示框不会反向缩进或翻转。
    const float safePadding       = std::max(0.0F, padding);
    const float safeMinimumExtent = std::max(0.0F, minimumExtent);
    // 先从未经扩张的联合边界计算中心，确保最小尺寸兜底不会产生偏移。
    const float centerX = (left + right) * 0.5F;
    const float centerY = (top + bottom) * 0.5F;
    const float hintWidth =
        // 先扩充实际包围盒，再应用最小尺寸，细小点状物件仍清晰可见。
        std::max(right - left + safePadding * 2.0F, safeMinimumExtent);
    const float hintHeight =
        std::max(bottom - top + safePadding * 2.0F, safeMinimumExtent);
    return AnnotationTargetHintBounds{
        // 围绕原始包围盒中心对称扩张，不改变提示与目标的视觉中心。
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
///
/// PLAYER_OBJECT 的负轨道映射到草稿区；AUDIO_SAMPLE 的轨道编号在玩家区
/// 之后连续映射到 BGM 区。任何无法解析的地址都必须保持 fallbackX。
/// @warning UI 热路径：每张可见详情卡片调用一次，只执行常量级投影查询。
[[nodiscard]] inline float annotationConnectorSourceX(
    const Common::Render::AnnotationRenderItem& item,
    const Logic::CanvasLaneProjection& projection, float fallbackX)
{
    // 时间戳与丢失目标均从批注栏自身起线，避免暗示不存在的轨道关联。
    if ( item.targetMissing ||
         item.targetKind == ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP ) {
        return fallbackX;
    }

    std::optional<Logic::CanvasLaneBounds> bounds;
    if ( item.targetKind ==
         ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT ) {
        // 负轨道编码草稿区：-1 为第一条草稿轨，最小合法值由轨道数决定。
        const auto minimumDraftTrack =
            -static_cast<std::int32_t>(projection.draftLaneCount);
        if ( item.track < minimumDraftTrack ||
             item.track >=
                 static_cast<std::int32_t>(projection.playerLaneCount) ) {
            // 越界轨道不可夹取到最近轨道，否则连线会错误指向其它物件。
            return fallbackX;
        }
        // 统一地址转换同时处理负草稿轨与非负玩家轨的坐标投影。
        const auto address = Logic::CanvasLaneAddress::fromAbsoluteTrack(
            item.track, projection.playerLaneCount, projection.draftLaneCount);
        // bounds 可能因相应区域被隐藏而为空，此时最终统一回退。
        bounds = projection.bounds(address);
    } else if ( item.targetKind ==
                ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE ) {
        // 自动采样先使用玩家轨编号空间，超过玩家轨数后连续映射到 BGM 轨。
        const auto track = static_cast<std::uint32_t>(item.track);
        if ( track < projection.playerLaneCount ) {
            // 玩家轨内的采样与玩家音符共用相同横向中心。
            bounds =
                projection.bounds({ Logic::CanvasLaneKind::Player, track });
        } else {
            // 减去玩家轨数量后才是 BGM 区内部的零基轨道编号。
            bounds = projection.bounds({ Logic::CanvasLaneKind::Bgm,
                                         track - projection.playerLaneCount });
        }
    }
    // 未知目标种类或投影中不存在该轨道时使用调用方提供的安全起点。
    return bounds ? (bounds->leftX + bounds->rightX) * 0.5F : fallbackX;
}

}  // namespace MMM::Canvas
