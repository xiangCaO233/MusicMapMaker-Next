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
    // 默认 AnnotationRenderItem 表示独立时间戳目标。
    const MMM::Common::Render::AnnotationRenderItem item;
    // 不设置 targetEntity，保留时间戳目标的标准无实体状态。
    // 放入一个可见命中框，确认函数不会误用当前悬浮的任意物件。
    const std::vector<MMM::Common::Render::Hitbox> hitboxes{
        // 实体编号刻意有效，确保被排除的原因确实是目标类型而非空实体。
        { static_cast<entt::entity>(1),
          // Head 是普通玩家物件最常见的可见部件。
          MMM::Common::Render::HoverPart::Head,
          // -1 表示非折线子节点。
          -1,
          // 几何参数给出非零有效矩形，排除无效尺寸分支干扰。
          10.0F,
          20.0F,
          40.0F,
          20.0F },
    };
    // 时间戳目标没有实体关联，因此结果必须为空。
    return !MMM::Canvas::findAnnotationTargetHintBounds(item, hitboxes);
}

/// @brief 验证 Polyline 子物件只合并自身索引对应的命中框。
/// @return 提示边界未包含同一父物件的其它子物件时返回 true。
bool testPolylineSubTargetUsesMatchingHitboxes()
{
    // 三个命中框共享父实体，其中首个属于另一个折线节点。
    // 这同时覆盖实体匹配与子索引匹配两层筛选条件。
    const auto entity = static_cast<entt::entity>(7);
    MMM::Common::Render::AnnotationRenderItem item;
    // 指定 PLAYER_OBJECT 后，函数应接受 PlayerNote 与 DraftNote 命中框。
    item.targetKind   = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
    item.targetEntity = entity;
    // subIndex=2 要求只合并后两个框，排除 subIndex=1 的长条段。
    item.targetSubIndex = 2;
    const std::vector<MMM::Common::Render::Hitbox> hitboxes{
        // 此框与目标实体一致，但子索引 1 必须被过滤。
        { entity,
          MMM::Common::Render::HoverPart::HoldBody,
          1,
          20.0F,
          50.0F,
          30.0F,
          15.0F },
        // subIndex=2 的头部建立目标联合边界的上半部分。
        { entity,
          MMM::Common::Render::HoverPart::Head,
          2,
          100.0F,
          40.0F,
          8.0F,
          12.0F },
        // 同一节点的 HoldBody 向下扩展联合边界。
        { entity,
          MMM::Common::Render::HoverPart::HoldBody,
          2,
          104.0F,
          52.0F,
          4.0F,
          18.0F },
    };
    // 默认 5 像素留白和 32 像素最小尺寸共同决定最终范围。
    const auto bounds =
        MMM::Canvas::findAnnotationTargetHintBounds(item, hitboxes);
    // 结果还需满足 32 像素最小宽度，因此窄头部会围绕中心对称扩张。
    // 纵向联合范围由 y=40 的头部延伸到 y=70 的长条底边。
    return bounds && near(bounds->left, 88.0F) && near(bounds->top, 35.0F) &&
           // 被排除的 x=20 命中框若参与合并，left 将明显小于 88。
           near(bounds->right, 120.0F) && near(bounds->bottom, 75.0F);
}

/// @brief 验证自动采样提示会合并主体与偏移句柄。
/// @return 合并边界同时覆盖两处几何并追加留白时返回 true。
bool testAudioSampleTargetMergesVisibleParts()
{
    // 自动采样的锚点与偏移句柄属于同一实体、同一种对象类型。
    // 两种 HoverPart 不应影响合并，只由对象类型和实体编号决定归属。
    const auto entity = static_cast<entt::entity>(9);
    MMM::Common::Render::AnnotationRenderItem item;
    // AUDIO_SAMPLE 分支应拒绝同实体编号的玩家或草稿对象命中框。
    item.targetKind   = MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE;
    item.targetEntity = entity;
    // 不限制 subIndex，使同一采样实体的锚点和偏移句柄全部参与合并。
    // 两个框纵向分离，测试合并结果必须覆盖中间空白和两端留白。
    const std::vector<MMM::Common::Render::Hitbox> hitboxes{
        // 锚点给出联合边界的左右极值和下边界。
        { entity,
          MMM::Common::Render::HoverPart::SampleAnchor,
          -1,
          50.0F,
          80.0F,
          40.0F,
          20.0F,
          MMM::Logic::ChartObjectKind::AudioSample },
        // 偏移句柄给出联合边界的上边界，且仍属于 AudioSample。
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
    // 高度由两个可见部件的联合范围决定，不受最小尺寸兜底影响。
    // 横向联合范围由较宽的采样锚点决定，偏移句柄只扩展上边界。
    return bounds && near(bounds->left, 45.0F) && near(bounds->top, 25.0F) &&
           // 原始联合范围为 [50, 90] x [30, 100]，四边各扩张 5。
           near(bounds->right, 95.0F) && near(bounds->bottom, 105.0F);
}

/// @brief 验证已丢失的批注目标不会绑定到复用的实体编号。
/// @return 标记丢失后没有提示边界时返回 true。
bool testMissingTargetHasNoHint()
{
    MMM::Common::Render::AnnotationRenderItem item;
    // 目标种类本身有效，用于隔离 targetMissing 的优先级行为。
    item.targetKind   = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
    item.targetEntity = static_cast<entt::entity>(3);
    // 即使可见列表存在相同实体编号，显式丢失标记也必须优先终止匹配。
    item.targetMissing = true;
    // targetMissing 是持久语义，优先于当前帧命中框里是否出现同号实体。
    const std::vector<MMM::Common::Render::Hitbox> hitboxes{
        // 相同编号的有效玩家物件模拟实体池复用后的新对象。
        { static_cast<entt::entity>(3),
          MMM::Common::Render::HoverPart::Head,
          -1,
          0.0F,
          0.0F,
          20.0F,
          20.0F },
    };
    // 该场景模拟 entt 销毁后编号被其它物件复用，不能产生错误高亮。
    return !MMM::Canvas::findAnnotationTargetHintBounds(item, hitboxes);
}

/// @brief 验证草稿物件使用负轨道投影连到草稿区并支持悬浮提示。
/// @return 连线起点位于目标草稿轨中心且提示边界命中草稿物件时返回 true。
bool testDraftTargetUsesDraftLaneProjection()
{
    // 负轨道 -2 表示从玩家区向左数第二条草稿轨。
    // 连线算法必须保留负轨语义，不能先转换成无符号整数。
    const auto entity = static_cast<entt::entity>(13);
    MMM::Common::Render::AnnotationRenderItem item;
    // DraftNote 仍使用 PLAYER_OBJECT 批注目标种类，区别只存在于轨道与命中框。
    item.targetKind   = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
    item.targetEntity = entity;
    item.track        = -2;
    // 未设置 targetMissing，确保测试进入正常轨道投影而非提前回退。

    // 投影启用三条草稿轨，使 -2 落在有效范围内。
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        // 画布宽度为 1000，四条玩家轨，草稿区宽度比例为 0.1。
        1000.0F,
        4,
        0,
        0.1F,
        0.5F,
        0.0F,
        true,
        false,
        true,
        3,
        true);
    const float sourceX =
        MMM::Canvas::annotationConnectorSourceX(item, projection, 512.0F);
    // fallbackX 故意与预期草稿轨中心不同，避免错误回退也能通过测试。

    // 命中框以计算出的草稿轨中心构造，验证草稿对象类型也可被提示函数接受。
    const std::vector<MMM::Common::Render::Hitbox> hitboxes{
        // 以 sourceX 为中心的 40 像素框便于直接验证左右边界。
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
    // 同时验证连线投影与悬浮边界，保证两条视觉提示落在同一草稿轨。
    // 提示框中心应与 sourceX 一致，避免连线和高亮分别指向不同区域。
    return near(sourceX, -50.0F) && bounds && near(bounds->left, -75.0F) &&
           // 40 像素命中框加两侧 5 像素留白得到 50 像素提示宽度。
           near(bounds->right, -25.0F);
}

/// @brief 验证无效负轨不会被夹到最左草稿轨。
/// @return 超出草稿区范围时回退到批注栏中心坐标。
bool testInvalidDraftTrackUsesAnnotationGutterFallback()
{
    // 投影只有三条草稿轨，-5 明确越过合法负轨范围。
    // 边界检查应发生在 CanvasLaneAddress 转换之前。
    MMM::Common::Render::AnnotationRenderItem item;
    // PLAYER_OBJECT 使负轨道进入正式/草稿统一地址转换分支。
    item.targetKind = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT;
    item.track      = -5;
    // 本测试只关心连线起点，无需构造任何可见命中框。
    // 512 是批注栏中心的显著值，便于识别是否错误投影到负坐标草稿区。
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        // 与有效草稿轨测试保持相同投影，仅改变待解析的 track。
        1000.0F,
        4,
        0,
        0.1F,
        0.5F,
        0.0F,
        true,
        false,
        true,
        3,
        true);
    // 越界值不得被 clamp 到最左轨，必须保持调用方给定的批注栏中心。
    return near(
        // 返回值应逐字等于调用方提供的 fallbackX，而非某条轨道中心。
        MMM::Canvas::annotationConnectorSourceX(item, projection, 512.0F),
        512.0F);
}

}  // namespace

/// @brief 覆盖批注悬浮时解析连线目标几何的规则。
/// @return 全部断言通过时返回 0。
int main()
{
    // 独立覆盖无目标、折线节点、自动采样、实体复用、草稿轨和越界轨道。
    // 任一规则回归都会使测试进程返回非零，供 CTest 或直接执行捕获。
    // 测试仅构造轻量渲染数据，不依赖 EditorEngine 或图形设备初始化。
    return testTimestampAnnotationHasNoTargetHint() &&
                   testPolylineSubTargetUsesMatchingHitboxes() &&
                   testAudioSampleTargetMergesVisibleParts() &&
                   testMissingTargetHasNoHint() &&
                   testDraftTargetUsesDraftLaneProjection() &&
                   testInvalidDraftTrackUsesAnnotationGutterFallback()
               ? 0
               : 1;
}
