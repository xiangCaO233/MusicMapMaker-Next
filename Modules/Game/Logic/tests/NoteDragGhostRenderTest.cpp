#include "logic/ecs/system/NoteRenderSystem.h"

#include "common/render/CanvasRenderTypes.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
/// @brief 测试主画布宽度。
constexpr float VIEWPORT_WIDTH = 800.0F;
/// @brief 测试主画布高度。
constexpr float VIEWPORT_HEIGHT = 600.0F;
/// @brief 玩家轨道数量。
constexpr std::int32_t PLAYER_TRACK_COUNT = 4;

/// @brief 比较两个画布坐标是否足够接近。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 误差小于测试容差时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 从指定绘制指令中提取 Note 纹理几何的最小横坐标。
/// @param snapshot 待检查快照。
/// @param commands 基础层或发光层绘制指令。
/// @param minX 成功时写入最小横坐标。
/// @return 找到 Note 纹理顶点时返回 true。
bool findMinimumNoteX(
    const MMM::Logic::RenderSnapshot&                      snapshot,
    const std::vector<MMM::Common::Render::CanvasDrawCmd>& commands,
    float&                                                 minX)
{
    minX       = std::numeric_limits<float>::max();
    bool found = false;
    for ( const auto& command : commands ) {
        const std::size_t end =
            static_cast<std::size_t>(command.indexOffset) + command.indexCount;
        for ( std::size_t index = command.indexOffset;
              index < end && index < snapshot.indices.size();
              ++index ) {
            const std::size_t vertexIndex =
                static_cast<std::size_t>(snapshot.indices[index]) +
                command.vertexOffset;
            if ( vertexIndex >= snapshot.vertices.size() ) continue;
            const auto& vertex = snapshot.vertices[vertexIndex];
            // 测试 UV 图中只有 Note 使用该区间，可排除轨道背景和调试几何。
            if ( vertex.uv.u < 0.24F || vertex.uv.u > 0.46F ||
                 vertex.uv.v < 0.34F || vertex.uv.v > 0.46F ) {
                continue;
            }
            minX  = std::min(minX, vertex.pos.x);
            found = true;
        }
    }
    return found;
}

/// @brief 验证跨入 BGM 区的拖动单键在基础层、发光层与命中盒中共用统一投影。
/// @return 虚影避开批注沟槽并落在第一条 BGM 轨道时返回 true。
bool testDraggedTapUsesBgmLaneBounds()
{
    entt::registry noteRegistry;
    entt::registry sampleRegistry;
    entt::registry timelineRegistry;

    // 固定 BPM 让音符在判定线附近稳定生成渲染与命中数据。
    const auto bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig config;
    config.visual.trackLayout.left  = 0.1F;
    config.visual.trackLayout.right = 0.5F;
    config.visual.noteScaleX        = 0.8F;
    config.visual.noteScaleY        = 1.0F;
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    config.visual.previewConfig.drawBeatLines   = false;
    config.visual.previewConfig.drawTimingLines = false;
    config.settings.enableBmsEditing            = true;

    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);

    // 拖动更新期间普通 Tap 仍位于 Note registry，但轨道已编码为首条 BGM
    // 绝对轨。
    const auto noteEntity = noteRegistry.create();
    noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::NOTE,
            .m_timestamp  = 0.0,
            .m_trackIndex = PLAYER_TRACK_COUNT,
        });
    noteRegistry.emplace<MMM::Logic::TransformComponent>(noteEntity);
    noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity,
        MMM::Logic::InteractionComponent{
            .isSelected = true,
            .isDragging = true,
        });
    const std::vector<entt::entity> sortedNotes{ noteEntity };
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);

    MMM::Logic::RenderSnapshot snapshot;
    snapshot.hasBeatmap         = true;
    snapshot.acceptsInteraction = true;
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note),
        glm::vec4{ 0.25F, 0.35F, 0.2F, 0.1F });

    MMM::Logic::System::NoteRenderSystem::generateSnapshot(
        noteRegistry,
        sampleRegistry,
        {},
        {},
        timelineRegistry,
        {},
        &snapshot,
        "Basic2DCanvas",
        0.0,
        VIEWPORT_WIDTH,
        VIEWPORT_HEIGHT,
        VIEWPORT_HEIGHT * config.visual.judgeline_pos,
        PLAYER_TRACK_COUNT,
        1,
        PLAYER_TRACK_COUNT,
        config,
        VIEWPORT_HEIGHT);

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        VIEWPORT_WIDTH,
        PLAYER_TRACK_COUNT,
        1,
        config.visual.trackLayout.left,
        config.visual.trackLayout.right,
        0.0F,
        true,
        true,
        false,
        PLAYER_TRACK_COUNT,
        true);
    const auto bgmBounds =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Bgm, 0U });
    if ( !bgmBounds ) {
        XERROR("Dragged Note ghost test could not resolve the first BGM lane");
        return false;
    }

    const float noteWidth =
        projection.player.singleTrackWidth * config.visual.noteScaleX;
    const float expectedHeadX =
        bgmBounds->leftX +
        (projection.player.singleTrackWidth - noteWidth) * 0.5F;

    float baseMinX = 0.0F;
    float glowMinX = 0.0F;
    if ( !findMinimumNoteX(snapshot, snapshot.cmds, baseMinX) ||
         !near(baseMinX, expectedHeadX) ||
         !findMinimumNoteX(snapshot, snapshot.glowCmds, glowMinX) ||
         !near(glowMinX, expectedHeadX) ) {
        XERROR(
            "Dragged Note body/glow X mismatch: base={}, glow={}, expected={}",
            baseMinX,
            glowMinX,
            expectedHeadX);
        return false;
    }

    for ( const auto& hitbox : snapshot.hitboxes ) {
        if ( hitbox.entity != noteEntity ||
             hitbox.part != MMM::Logic::HoverPart::Head ) {
            continue;
        }
        if ( !near(hitbox.x, expectedHeadX) ||
             hitbox.x < projection.annotationRightX ) {
            XERROR(
                "Dragged Note ghost remained in the annotation gutter: {} != "
                "{}",
                hitbox.x,
                expectedHeadX);
            return false;
        }
        return true;
    }

    XERROR("Dragged Note ghost did not publish a head hitbox");
    return false;
}
}  // namespace

/// @brief 运行拖动单键虚影渲染回归测试。
/// @return 测试通过时返回 0。
int main()
{
    return testDraggedTapUsesBgmLaneBounds() ? 0 : 1;
}
