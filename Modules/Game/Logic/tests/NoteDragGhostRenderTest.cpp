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
#include <utility>
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

/// @brief 从指定绘制层提取一个测试纹理的横向顶点范围。
/// @param snapshot 待检查渲染快照。
/// @param commands 基础层或发光层绘制指令。
/// @param textureU 测试纹理的起始 U 坐标。
/// @param minX 成功时写入最小横坐标。
/// @param maxX 成功时写入最大横坐标。
/// @return 找到目标纹理顶点时返回 true。
bool findTextureXRange(
    const MMM::Logic::RenderSnapshot&                      snapshot,
    const std::vector<MMM::Common::Render::CanvasDrawCmd>& commands,
    float textureU, float& minX, float& maxX)
{
    minX       = std::numeric_limits<float>::max();
    maxX       = std::numeric_limits<float>::lowest();
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
            if ( vertex.uv.u < textureU - 1e-4F ||
                 vertex.uv.u > textureU + 0.011F ) {
                continue;
            }
            minX  = std::min(minX, vertex.pos.x);
            maxX  = std::max(maxX, vertex.pos.x);
            found = true;
        }
    }
    return found;
}

/// @brief 为跨 Draft/Player 边界的单个拖动物件生成主画布快照。
/// @param note 待渲染 Flick 或 Polyline。
/// @param snapshot 输出快照。
/// @param projection 输出统一轨道投影。
/// @param noteEntity 输出物件实体。
/// @param brush 可选笔刷预览状态；为空时仅渲染实体。
/// @param includeBgm 是否启用一条独立 BGM 轨道。
/// @warning 测试夹具只在单线程 CTest 中运行；局部 Registry
/// 在快照生成后即可释放。
void renderCrossRegionGhost(
    MMM::Logic::NoteComponent note, MMM::Logic::RenderSnapshot& snapshot,
    MMM::Logic::CanvasLaneProjection& projection, entt::entity& noteEntity,
    const MMM::Logic::RenderSnapshot::BrushSnapshot* brush      = nullptr,
    bool                                             includeBgm = false)
{
    entt::registry noteRegistry;
    entt::registry sampleRegistry;
    entt::registry timelineRegistry;

    // 固定 BPM 与判定线，确保所有端点同时处于可见和可拾取范围。
    const auto bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig config;
    config.visual.trackLayout.left             = 0.1F;
    config.visual.trackLayout.right            = 0.5F;
    config.visual.trackLayout.draftLanes.left  = -0.21F;
    config.visual.trackLayout.draftLanes.width = 0.06F;
    config.visual.trackLayout.bgmLanes.left    = 0.7F;
    config.visual.trackLayout.bgmLanes.width   = 0.05F;
    config.visual.noteScaleX                   = 0.8F;
    config.visual.noteScaleY                   = 1.0F;
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    config.visual.previewConfig.drawBeatLines   = false;
    config.visual.previewConfig.drawTimingLines = false;
    config.settings.enableDraftLanes            = true;
    config.settings.enableBmsEditing            = includeBgm;

    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);

    note.m_isDraft = note.m_trackIndex < 0;
    noteEntity     = noteRegistry.create();
    noteRegistry.emplace<MMM::Logic::NoteComponent>(noteEntity,
                                                    std::move(note));
    noteRegistry.emplace<MMM::Logic::TransformComponent>(noteEntity);
    noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity,
        MMM::Logic::InteractionComponent{
            .isSelected = true,
            .isDragging = true,
        });
    const std::vector<entt::entity> sortedNotes{ noteEntity };
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);

    snapshot.hasBeatmap         = true;
    snapshot.acceptsInteraction = true;
    if ( brush ) snapshot.brush = *brush;
    const auto addTexture = [&](MMM::Logic::TextureID id, float u) {
        snapshot.uvMap.emplace(static_cast<std::uint32_t>(id),
                               glm::vec4{ u, 0.2F, 0.01F, 0.01F });
    };
    addTexture(MMM::Logic::TextureID::None, 0.0F);
    addTexture(MMM::Logic::TextureID::Note, 0.2F);
    addTexture(MMM::Logic::TextureID::HoldBodyHorizontal, 0.4F);
    addTexture(MMM::Logic::TextureID::HoldBodyVertical, 0.5F);
    addTexture(MMM::Logic::TextureID::Node, 0.6F);
    addTexture(MMM::Logic::TextureID::HoldEnd, 0.7F);
    addTexture(MMM::Logic::TextureID::FlickArrowRight, 0.8F);
    addTexture(MMM::Logic::TextureID::FlickArrowLeft, 0.9F);

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
        includeBgm ? 1 : 0,
        PLAYER_TRACK_COUNT,
        config,
        VIEWPORT_HEIGHT);
    projection =
        MMM::Logic::calculateCanvasLaneProjection(VIEWPORT_WIDTH,
                                                  PLAYER_TRACK_COUNT,
                                                  includeBgm ? 1 : 0,
                                                  config.visual.trackLayout,
                                                  0.0F,
                                                  true,
                                                  includeBgm,
                                                  true,
                                                  PLAYER_TRACK_COUNT,
                                                  true);
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
    config.visual.trackLayout.left           = 0.1F;
    config.visual.trackLayout.right          = 0.5F;
    config.visual.trackLayout.bgmLanes.left  = 0.7F;
    config.visual.trackLayout.bgmLanes.width = 0.05F;
    config.visual.noteScaleX                 = 0.8F;
    config.visual.noteScaleY                 = 1.0F;
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

    const auto projection =
        MMM::Logic::calculateCanvasLaneProjection(VIEWPORT_WIDTH,
                                                  PLAYER_TRACK_COUNT,
                                                  1,
                                                  config.visual.trackLayout,
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

    const float noteWidth = projection.bgmLaneWidth * config.visual.noteScaleX;
    const float expectedHeadX =
        bgmBounds->leftX + (projection.bgmLaneWidth - noteWidth) * 0.5F;

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

/// @brief 验证 Draft 根节点指向 Player 终点的 Flick 使用两侧真实轨道几何。
/// @return 箭头、连接体、发光与命中盒均跨越独立间隙时返回 true。
bool testCrossRegionFlickUsesEndpointProjection()
{
    MMM::Logic::NoteComponent flick{
        .m_type       = MMM::NoteType::FLICK,
        .m_timestamp  = 0.0,
        .m_trackIndex = -1,
        .m_dtrack     = 1,
    };
    MMM::Logic::RenderSnapshot       snapshot;
    MMM::Logic::CanvasLaneProjection projection;
    entt::entity                     entity{ entt::null };
    renderCrossRegionGhost(std::move(flick), snapshot, projection, entity);

    const auto rootLane =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Draft, 4U });
    const auto endpointLane =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Player, 0U });
    if ( !rootLane || !endpointLane ) {
        XERROR(
            "Cross-region Flick projection did not expose both endpoint lanes");
        return false;
    }

    constexpr float expectedBodyX   = 48.0F;
    constexpr float expectedBodyW   = 72.0F;
    constexpr float expectedArrowX  = 88.0F;
    constexpr float expectedArrowX2 = 152.0F;
    bool            foundBody       = false;
    bool            foundArrow      = false;
    for ( const auto& hitbox : snapshot.hitboxes ) {
        if ( hitbox.entity != entity ) continue;
        if ( hitbox.part == MMM::Logic::HoverPart::HoldBody ) {
            foundBody =
                near(hitbox.x, expectedBodyX) && near(hitbox.w, expectedBodyW);
        } else if ( hitbox.part == MMM::Logic::HoverPart::FlickArrow ) {
            foundArrow = near(hitbox.x, expectedArrowX) &&
                         near(hitbox.w, expectedArrowX2 - expectedArrowX);
        }
    }

    float      baseArrowMin = 0.0F;
    float      baseArrowMax = 0.0F;
    float      glowArrowMin = 0.0F;
    float      glowArrowMax = 0.0F;
    float      baseBodyMin  = 0.0F;
    float      baseBodyMax  = 0.0F;
    float      glowBodyMin  = 0.0F;
    float      glowBodyMax  = 0.0F;
    const bool baseArrow    = findTextureXRange(
        snapshot, snapshot.cmds, 0.8F, baseArrowMin, baseArrowMax);
    const bool glowArrow = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.8F, glowArrowMin, glowArrowMax);
    const bool baseBody = findTextureXRange(
        snapshot, snapshot.cmds, 0.4F, baseBodyMin, baseBodyMax);
    const bool glowBody = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.4F, glowBodyMin, glowBodyMax);
    if ( !foundBody || !foundArrow || !baseArrow || !glowArrow || !baseBody ||
         !glowBody || !near(baseArrowMin, expectedArrowX) ||
         !near(baseArrowMax, expectedArrowX2) ||
         !near(glowArrowMin, expectedArrowX) ||
         !near(glowArrowMax, expectedArrowX2) ||
         !near(baseBodyMin, expectedBodyX) ||
         !near(baseBodyMax, expectedBodyX + expectedBodyW) ||
         !near(glowBodyMin, expectedBodyX) ||
         !near(glowBodyMax, expectedBodyX + expectedBodyW) ) {
        XERROR(
            "Cross-region Flick endpoint geometry diverged from Player lane");
        return false;
    }
    return true;
}

/// @brief 验证 Player 根节点指向独立 BGM 轨道的 Flick 终点投影。
/// @return 箭头按 BGM 轨宽缩放且连接体跨越批注间隙时返回 true。
bool testPlayerToBgmFlickUsesEndpointProjection()
{
    MMM::Logic::NoteComponent flick{
        .m_type       = MMM::NoteType::FLICK,
        .m_timestamp  = 0.0,
        .m_trackIndex = 3,
        .m_dtrack     = 1,
    };
    MMM::Logic::RenderSnapshot       snapshot;
    MMM::Logic::CanvasLaneProjection projection;
    entt::entity                     entity{ entt::null };
    renderCrossRegionGhost(
        std::move(flick), snapshot, projection, entity, nullptr, true);

    constexpr float expectedBodyX   = 360.0F;
    constexpr float expectedBodyMax = 580.0F;
    constexpr float expectedArrowX  = 564.0F;
    constexpr float expectedArrowX2 = 596.0F;
    bool            foundBody       = false;
    bool            foundArrow      = false;
    for ( const auto& hitbox : snapshot.hitboxes ) {
        if ( hitbox.entity != entity ) continue;
        if ( hitbox.part == MMM::Logic::HoverPart::HoldBody ) {
            foundBody = near(hitbox.x, expectedBodyX) &&
                        near(hitbox.w, expectedBodyMax - expectedBodyX);
        } else if ( hitbox.part == MMM::Logic::HoverPart::FlickArrow ) {
            foundArrow = near(hitbox.x, expectedArrowX) &&
                         near(hitbox.w, expectedArrowX2 - expectedArrowX);
        }
    }

    float      baseBodyMin  = 0.0F;
    float      baseBodyMax  = 0.0F;
    float      glowBodyMin  = 0.0F;
    float      glowBodyMax  = 0.0F;
    float      baseArrowMin = 0.0F;
    float      baseArrowMax = 0.0F;
    float      glowArrowMin = 0.0F;
    float      glowArrowMax = 0.0F;
    const bool baseBody     = findTextureXRange(
        snapshot, snapshot.cmds, 0.4F, baseBodyMin, baseBodyMax);
    const bool glowBody = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.4F, glowBodyMin, glowBodyMax);
    const bool baseArrow = findTextureXRange(
        snapshot, snapshot.cmds, 0.8F, baseArrowMin, baseArrowMax);
    const bool glowArrow = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.8F, glowArrowMin, glowArrowMax);
    if ( !foundBody || !foundArrow || !baseBody || !glowBody || !baseArrow ||
         !glowArrow || !near(baseBodyMin, expectedBodyX) ||
         !near(baseBodyMax, expectedBodyMax) ||
         !near(glowBodyMin, expectedBodyX) ||
         !near(glowBodyMax, expectedBodyMax) ||
         !near(baseArrowMin, expectedArrowX) ||
         !near(baseArrowMax, expectedArrowX2) ||
         !near(glowArrowMin, expectedArrowX) ||
         !near(glowArrowMax, expectedArrowX2) ) {
        XERROR("Player-to-BGM Flick endpoint ignored independent BGM geometry");
        return false;
    }
    return true;
}

/// @brief 验证跨 Draft/Player 的 Polyline 节点与过渡段逐端点投影。
/// @return 两侧节点宽度及斜向连接包围盒均匹配真实轨道时返回 true。
bool testCrossRegionPolylineUsesPerNodeProjection()
{
    MMM::Logic::NoteComponent polyline{
        .m_type       = MMM::NoteType::POLYLINE,
        .m_timestamp  = 0.0,
        .m_trackIndex = -1,
    };
    polyline.m_subNotes = {
        MMM::Logic::NoteComponent::SubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 0.0, .trackIndex = -1 },
        MMM::Logic::NoteComponent::SubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 0.1, .trackIndex = 0 },
        MMM::Logic::NoteComponent::SubNote{ .type       = MMM::NoteType::FLICK,
                                            .timestamp  = 0.2,
                                            .trackIndex = -1,
                                            .dtrack     = 1 },
    };

    MMM::Logic::RenderSnapshot       snapshot;
    MMM::Logic::CanvasLaneProjection projection;
    entt::entity                     entity{ entt::null };
    renderCrossRegionGhost(std::move(polyline), snapshot, projection, entity);

    constexpr float draftNodeX      = 28.8F;
    constexpr float draftNodeW      = 38.4F;
    constexpr float playerNodeX     = 88.0F;
    constexpr float playerNodeW     = 64.0F;
    constexpr float transitionW     = 123.2F;
    bool            foundDraft      = false;
    bool            foundPlayer     = false;
    bool            foundBody       = false;
    bool            foundFlickBody  = false;
    bool            foundFlickArrow = false;
    for ( const auto& hitbox : snapshot.hitboxes ) {
        if ( hitbox.entity != entity ) continue;
        if ( hitbox.part == MMM::Logic::HoverPart::PolylineNode &&
             hitbox.subIndex == 0 ) {
            foundDraft =
                near(hitbox.x, draftNodeX) && near(hitbox.w, draftNodeW);
        } else if ( hitbox.part == MMM::Logic::HoverPart::PolylineNode &&
                    hitbox.subIndex == 1 ) {
            foundPlayer =
                near(hitbox.x, playerNodeX) && near(hitbox.w, playerNodeW);
        } else if ( hitbox.part == MMM::Logic::HoverPart::HoldBody &&
                    hitbox.subIndex == 0 ) {
            foundBody =
                near(hitbox.x, draftNodeX) && near(hitbox.w, transitionW);
        } else if ( hitbox.part == MMM::Logic::HoverPart::HoldBody &&
                    hitbox.subIndex == 2 ) {
            foundFlickBody = near(hitbox.x, 48.0F) && near(hitbox.w, 72.0F);
        } else if ( hitbox.part == MMM::Logic::HoverPart::FlickArrow &&
                    hitbox.subIndex == 2 ) {
            foundFlickArrow =
                near(hitbox.x, playerNodeX) && near(hitbox.w, playerNodeW);
        }
    }

    float      baseHeadMin       = 0.0F;
    float      baseHeadMax       = 0.0F;
    float      glowHeadMin       = 0.0F;
    float      glowHeadMax       = 0.0F;
    float      baseNodeMin       = 0.0F;
    float      baseNodeMax       = 0.0F;
    float      glowNodeMin       = 0.0F;
    float      glowNodeMax       = 0.0F;
    float      baseArrowMin      = 0.0F;
    float      baseArrowMax      = 0.0F;
    float      glowArrowMin      = 0.0F;
    float      glowArrowMax      = 0.0F;
    float      baseFlickBodyMin  = 0.0F;
    float      baseFlickBodyMax  = 0.0F;
    float      glowFlickBodyMin  = 0.0F;
    float      glowFlickBodyMax  = 0.0F;
    float      baseTransitionMin = 0.0F;
    float      baseTransitionMax = 0.0F;
    float      glowTransitionMin = 0.0F;
    float      glowTransitionMax = 0.0F;
    const bool baseHead          = findTextureXRange(
        snapshot, snapshot.cmds, 0.2F, baseHeadMin, baseHeadMax);
    const bool glowHead = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.2F, glowHeadMin, glowHeadMax);
    const bool baseNodes = findTextureXRange(
        snapshot, snapshot.cmds, 0.6F, baseNodeMin, baseNodeMax);
    const bool glowNodes = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.6F, glowNodeMin, glowNodeMax);
    const bool baseArrow = findTextureXRange(
        snapshot, snapshot.cmds, 0.8F, baseArrowMin, baseArrowMax);
    const bool glowArrow = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.8F, glowArrowMin, glowArrowMax);
    const bool baseFlickBody = findTextureXRange(
        snapshot, snapshot.cmds, 0.4F, baseFlickBodyMin, baseFlickBodyMax);
    const bool glowFlickBody = findTextureXRange(
        snapshot, snapshot.glowCmds, 0.4F, glowFlickBodyMin, glowFlickBodyMax);
    const bool baseTransition = findTextureXRange(
        snapshot, snapshot.cmds, 0.5F, baseTransitionMin, baseTransitionMax);
    const bool glowTransition = findTextureXRange(snapshot,
                                                  snapshot.glowCmds,
                                                  0.5F,
                                                  glowTransitionMin,
                                                  glowTransitionMax);
    if ( !foundDraft || !foundPlayer || !foundBody || !foundFlickBody ||
         !foundFlickArrow || !baseHead || !glowHead || !baseNodes ||
         !glowNodes || !baseArrow || !glowArrow || !baseFlickBody ||
         !glowFlickBody || !baseTransition || !glowTransition ||
         !near(baseHeadMin, draftNodeX) ||
         !near(baseHeadMax, draftNodeX + draftNodeW) ||
         !near(glowHeadMin, draftNodeX) ||
         !near(glowHeadMax, draftNodeX + draftNodeW) ||
         !near(baseNodeMin, draftNodeX) ||
         !near(baseNodeMax, playerNodeX + playerNodeW) ||
         !near(glowNodeMin, draftNodeX) ||
         !near(glowNodeMax, playerNodeX + playerNodeW) ||
         !near(baseArrowMin, playerNodeX) ||
         !near(baseArrowMax, playerNodeX + playerNodeW) ||
         !near(glowArrowMin, playerNodeX) ||
         !near(glowArrowMax, playerNodeX + playerNodeW) ||
         !near(baseFlickBodyMin, 48.0F) || !near(baseFlickBodyMax, 120.0F) ||
         !near(glowFlickBodyMin, 48.0F) || !near(glowFlickBodyMax, 120.0F) ||
         !near(baseTransitionMin, draftNodeX) ||
         !near(baseTransitionMax, playerNodeX + playerNodeW) ||
         !near(glowTransitionMin, draftNodeX) ||
         !near(glowTransitionMax, playerNodeX + playerNodeW) ) {
        XERROR(
            "Cross-region Polyline geometry mismatch: draft={}, player={}, "
            "body={}, base={} [{}, {}], glow={} [{}, {}]",
            foundDraft,
            foundPlayer,
            foundBody,
            baseNodes,
            baseNodeMin,
            baseNodeMax,
            glowNodes,
            glowNodeMin,
            glowNodeMax);
        return false;
    }
    return true;
}

/// @brief 验证 Flick 与 Polyline 笔刷预览同样逐端点使用统一轨道投影。
/// @return 预览箭头、横向连接体和折线过渡均跨越 Draft/Player 间隙时返回 true。
bool testCrossRegionBrushPreviewUsesEndpointProjection()
{
    const MMM::Logic::NoteComponent dummyNote{
        .m_type       = MMM::NoteType::NOTE,
        .m_timestamp  = 8.0,
        .m_trackIndex = 3,
    };

    // Flick 预览不生成命中盒，因此直接检查纹理顶点范围。
    MMM::Logic::RenderSnapshot::BrushSnapshot flickBrush;
    flickBrush.isActive = true;
    flickBrush.time     = 0.0;
    flickBrush.track    = -1;
    flickBrush.dtrack   = 1;
    flickBrush.type     = MMM::NoteType::FLICK;
    MMM::Logic::RenderSnapshot       flickSnapshot;
    MMM::Logic::CanvasLaneProjection flickProjection;
    entt::entity                     flickEntity{ entt::null };
    renderCrossRegionGhost(
        dummyNote, flickSnapshot, flickProjection, flickEntity, &flickBrush);

    float      flickBodyMin  = 0.0F;
    float      flickBodyMax  = 0.0F;
    float      flickArrowMin = 0.0F;
    float      flickArrowMax = 0.0F;
    const bool flickBody     = findTextureXRange(
        flickSnapshot, flickSnapshot.cmds, 0.4F, flickBodyMin, flickBodyMax);
    const bool flickArrow = findTextureXRange(
        flickSnapshot, flickSnapshot.cmds, 0.8F, flickArrowMin, flickArrowMax);
    if ( !flickBody || !flickArrow || !near(flickBodyMin, 48.0F) ||
         !near(flickBodyMax, 120.0F) || !near(flickArrowMin, 88.0F) ||
         !near(flickArrowMax, 152.0F) ) {
        XERROR("Cross-region Flick brush preview did not use endpoint lane");
        return false;
    }

    // Polyline 预览同时覆盖独立宽度节点、斜向过渡和内嵌 Flick 终点。
    MMM::Logic::RenderSnapshot::BrushSnapshot polylineBrush;
    polylineBrush.isActive         = true;
    polylineBrush.time             = 0.0;
    polylineBrush.track            = -1;
    polylineBrush.type             = MMM::NoteType::POLYLINE;
    polylineBrush.polylineSegments = {
        MMM::Common::Render::PolylineSubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 0.0, .trackIndex = -1 },
        MMM::Common::Render::PolylineSubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 0.1, .trackIndex = 0 },
        MMM::Common::Render::PolylineSubNote{ .type      = MMM::NoteType::FLICK,
                                              .timestamp = 0.2,
                                              .trackIndex = -1,
                                              .dtrack     = 1 },
    };
    MMM::Logic::RenderSnapshot       polylineSnapshot;
    MMM::Logic::CanvasLaneProjection polylineProjection;
    entt::entity                     polylineEntity{ entt::null };
    renderCrossRegionGhost(dummyNote,
                           polylineSnapshot,
                           polylineProjection,
                           polylineEntity,
                           &polylineBrush);

    float      transitionMin = 0.0F;
    float      transitionMax = 0.0F;
    float      bodyMin       = 0.0F;
    float      bodyMax       = 0.0F;
    float      arrowMin      = 0.0F;
    float      arrowMax      = 0.0F;
    const bool transition    = findTextureXRange(polylineSnapshot,
                                              polylineSnapshot.cmds,
                                              0.5F,
                                              transitionMin,
                                              transitionMax);
    const bool body          = findTextureXRange(
        polylineSnapshot, polylineSnapshot.cmds, 0.4F, bodyMin, bodyMax);
    const bool arrow = findTextureXRange(
        polylineSnapshot, polylineSnapshot.cmds, 0.8F, arrowMin, arrowMax);
    if ( !transition || !body || !arrow || !near(transitionMin, 28.8F) ||
         !near(transitionMax, 152.0F) || !near(bodyMin, 48.0F) ||
         !near(bodyMax, 120.0F) || !near(arrowMin, 88.0F) ||
         !near(arrowMax, 152.0F) ) {
        XERROR("Cross-region Polyline brush preview used root lane width");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行拖动单键虚影渲染回归测试。
/// @return 测试通过时返回 0。
int main()
{
    const bool tapPassed      = testDraggedTapUsesBgmLaneBounds();
    const bool flickPassed    = testCrossRegionFlickUsesEndpointProjection();
    const bool bgmPassed      = testPlayerToBgmFlickUsesEndpointProjection();
    const bool polylinePassed = testCrossRegionPolylineUsesPerNodeProjection();
    const bool brushPassed =
        testCrossRegionBrushPreviewUsesEndpointProjection();
    return tapPassed && flickPassed && bgmPassed && polylinePassed &&
                   brushPassed
               ? 0
               : 1;
}
