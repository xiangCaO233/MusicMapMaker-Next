#include "canvas/AnnotationTargetHint.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{

/// @brief 使用小容差比较批注目标提示边界坐标。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个坐标足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 验证时间戳批注不会误高亮普通悬浮物件。
/// @return 时间戳批注始终没有物件提示边界时返回 true。
bool testTimestampAnnotationHasNoTargetHint()
{
    const MMM::Common::Render::AnnotationRenderItem item;
    const std::vector<MMM::Common::Render::Hitbox>  hitboxes{
        { static_cast<entt::entity>(1),
          MMM::Common::Render::HoverPart::Head,
          -1,
          10.0F,
          20.0F,
          40.0F,
          20.0F },
    };
    return !MMM::Canvas::findAnnotationTargetHintBounds(item, hitboxes);
}

/// @brief 验证 Polyline 子物件只合并自身索引对应的命中框。
/// @return 提示边界未包含同一父物件的其它子物件时返回 true。
bool testPolylineSubTargetUsesMatchingHitboxes()
{
    const auto entity = static_cast<entt::entity>(7);
    MMM::Common::Render::AnnotationRenderItem item;
    item.targetKind     = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
    item.targetEntity   = entity;
    item.targetSubIndex = 2;
    const std::vector<MMM::Common::Render::Hitbox> hitboxes{
        { entity,
          MMM::Common::Render::HoverPart::HoldBody,
          1,
          20.0F,
          50.0F,
          30.0F,
          15.0F },
        { entity,
          MMM::Common::Render::HoverPart::Head,
          2,
          100.0F,
          40.0F,
          8.0F,
          12.0F },
        { entity,
          MMM::Common::Render::HoverPart::HoldBody,
          2,
          104.0F,
          52.0F,
          4.0F,
          18.0F },
    };
    const auto bounds =
        MMM::Canvas::findAnnotationTargetHintBounds(item, hitboxes);
    return bounds && near(bounds->left, 88.0F) && near(bounds->top, 35.0F) &&
           near(bounds->right, 120.0F) && near(bounds->bottom, 75.0F);
}

/// @brief 验证自动采样提示会合并主体与偏移句柄。
/// @return 合并边界同时覆盖两处几何并追加留白时返回 true。
bool testAudioSampleTargetMergesVisibleParts()
{
    const auto entity = static_cast<entt::entity>(9);
    MMM::Common::Render::AnnotationRenderItem item;
    item.targetKind   = MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE;
    item.targetEntity = entity;
    const std::vector<MMM::Common::Render::Hitbox> hitboxes{
        { entity,
          MMM::Common::Render::HoverPart::SampleAnchor,
          -1,
          50.0F,
          80.0F,
          40.0F,
          20.0F,
          MMM::Logic::ChartObjectKind::AudioSample },
        { entity,
          MMM::Common::Render::HoverPart::SampleOffset,
          -1,
          65.0F,
          30.0F,
          10.0F,
          10.0F,
          MMM::Logic::ChartObjectKind::AudioSample },
    };
    const auto bounds =
        MMM::Canvas::findAnnotationTargetHintBounds(item, hitboxes);
    return bounds && near(bounds->left, 45.0F) && near(bounds->top, 25.0F) &&
           near(bounds->right, 95.0F) && near(bounds->bottom, 105.0F);
}

/// @brief 验证已丢失的批注目标不会绑定到复用的实体编号。
/// @return 标记丢失后没有提示边界时返回 true。
bool testMissingTargetHasNoHint()
{
    MMM::Common::Render::AnnotationRenderItem item;
    item.targetKind    = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
    item.targetEntity  = static_cast<entt::entity>(3);
    item.targetMissing = true;
    const std::vector<MMM::Common::Render::Hitbox> hitboxes{
        { static_cast<entt::entity>(3),
          MMM::Common::Render::HoverPart::Head,
          -1,
          0.0F,
          0.0F,
          20.0F,
          20.0F },
    };
    return !MMM::Canvas::findAnnotationTargetHintBounds(item, hitboxes);
}

/// @brief 验证草稿物件使用负轨道投影连到草稿区并支持悬浮提示。
/// @return 连线起点位于目标草稿轨中心且提示边界命中草稿物件时返回 true。
bool testDraftTargetUsesDraftLaneProjection()
{
    const auto entity = static_cast<entt::entity>(13);
    MMM::Common::Render::AnnotationRenderItem item;
    item.targetKind   = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
    item.targetEntity = entity;
    item.track        = -2;

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 0, 0.1F, 0.5F, 0.0F, true, false, true, 3, true);
    const float sourceX =
        MMM::Canvas::annotationConnectorSourceX(item, projection, 512.0F);

    const std::vector<MMM::Common::Render::Hitbox> hitboxes{
        { entity,
          MMM::Common::Render::HoverPart::Head,
          -1,
          sourceX - 20.0F,
          70.0F,
          40.0F,
          20.0F,
          MMM::Logic::ChartObjectKind::DraftNote },
    };
    const auto bounds =
        MMM::Canvas::findAnnotationTargetHintBounds(item, hitboxes);
    return near(sourceX, -50.0F) && bounds && near(bounds->left, -75.0F) &&
           near(bounds->right, -25.0F);
}

/// @brief 验证无效负轨不会被夹到最左草稿轨。
/// @return 超出草稿区范围时回退到批注栏中心坐标。
bool testInvalidDraftTrackUsesAnnotationGutterFallback()
{
    MMM::Common::Render::AnnotationRenderItem item;
    item.targetKind       = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
    item.track            = -5;
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 0, 0.1F, 0.5F, 0.0F, true, false, true, 3, true);
    return near(
        MMM::Canvas::annotationConnectorSourceX(item, projection, 512.0F),
        512.0F);
}

}  // namespace

/// @brief 覆盖批注悬浮时解析连线目标几何的规则。
/// @return 全部断言通过时返回 0。
int main()
{
    return testTimestampAnnotationHasNoTargetHint() &&
                   testPolylineSubTargetUsesMatchingHitboxes() &&
                   testAudioSampleTargetMergesVisibleParts() &&
                   testMissingTargetHasNoHint() &&
                   testDraftTargetUsesDraftLaneProjection() &&
                   testInvalidDraftTrackUsesAnnotationGutterFallback()
               ? 0
               : 1;
}
