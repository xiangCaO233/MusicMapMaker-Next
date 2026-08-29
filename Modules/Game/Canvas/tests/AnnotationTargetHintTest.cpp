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
    const MMM::Logic::AnnotationRenderItem item;
    const std::vector<MMM::Logic::Hitbox>  hitboxes{
        { static_cast<entt::entity>(1),
          MMM::Logic::HoverPart::Head,
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
    const auto                       entity = static_cast<entt::entity>(7);
    MMM::Logic::AnnotationRenderItem item;
    item.targetKind     = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
    item.targetEntity   = entity;
    item.targetSubIndex = 2;
    const std::vector<MMM::Logic::Hitbox> hitboxes{
        { entity,
          MMM::Logic::HoverPart::HoldBody,
          1,
          20.0F,
          50.0F,
          30.0F,
          15.0F },
        { entity, MMM::Logic::HoverPart::Head, 2, 100.0F, 40.0F, 8.0F, 12.0F },
        { entity,
          MMM::Logic::HoverPart::HoldBody,
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
    const auto                       entity = static_cast<entt::entity>(9);
    MMM::Logic::AnnotationRenderItem item;
    item.targetKind   = MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE;
    item.targetEntity = entity;
    const std::vector<MMM::Logic::Hitbox> hitboxes{
        { entity,
          MMM::Logic::HoverPart::SampleAnchor,
          -1,
          50.0F,
          80.0F,
          40.0F,
          20.0F,
          MMM::Logic::ChartObjectKind::AudioSample },
        { entity,
          MMM::Logic::HoverPart::SampleOffset,
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
    MMM::Logic::AnnotationRenderItem item;
    item.targetKind    = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
    item.targetEntity  = static_cast<entt::entity>(3);
    item.targetMissing = true;
    const std::vector<MMM::Logic::Hitbox> hitboxes{
        { static_cast<entt::entity>(3),
          MMM::Logic::HoverPart::Head,
          -1,
          0.0F,
          0.0F,
          20.0F,
          20.0F },
    };
    return !MMM::Canvas::findAnnotationTargetHintBounds(item, hitboxes);
}

}  // namespace

/// @brief 覆盖批注悬浮时解析连线目标几何的规则。
/// @return 全部断言通过时返回 0。
int main()
{
    return testTimestampAnnotationHasNoTargetHint() &&
                   testPolylineSubTargetUsesMatchingHitboxes() &&
                   testAudioSampleTargetMergesVisibleParts() &&
                   testMissingTargetHasNoHint()
               ? 0
               : 1;
}
