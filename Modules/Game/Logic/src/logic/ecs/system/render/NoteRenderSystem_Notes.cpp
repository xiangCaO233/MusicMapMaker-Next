#include "config/skin/SkinConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include <unordered_set>

namespace MMM::Logic::System
{

void NoteRenderSystem::renderNotes(
    entt::registry& registry, RenderSnapshot* snapshot,
    const std::string& cameraId, double currentTime, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config, Batcher& batcher,
    float leftX, float rightX, float topY, float bottomY, float singleTrackW,
    float renderScaleY)
{
    // 1. 准备上下文与颜色
    NoteRenderSystem::NoteRenderContext ctx =
        NoteRenderSystem::prepareNoteRenderContext(
            registry, snapshot, currentTime, singleTrackW, config);
    if ( !ctx.cache ) return;

    // 2. 生成碰撞盒并获取可见实体
    NoteRenderSystem::generateNoteHitboxes(registry,
                                           snapshot,
                                           ctx,
                                           judgmentLineY,
                                           leftX,
                                           topY,
                                           bottomY,
                                           singleTrackW,
                                           renderScaleY,
                                           config);

    // 3. 基础层渲染
    NoteRenderSystem::renderNoteBaseLayer(registry,
                                          snapshot,
                                          ctx,
                                          config,
                                          batcher,
                                          (float)currentTime,
                                          judgmentLineY,
                                          leftX,
                                          rightX,
                                          topY,
                                          bottomY,
                                          singleTrackW,
                                          renderScaleY);

    // 4. 发光层渲染
    NoteRenderSystem::renderNoteGlowLayer(registry,
                                          snapshot,
                                          ctx,
                                          config,
                                          (float)currentTime,
                                          judgmentLineY,
                                          leftX,
                                          rightX,
                                          topY,
                                          bottomY,
                                          singleTrackW,
                                          renderScaleY);
}

NoteRenderSystem::NoteRenderContext NoteRenderSystem::prepareNoteRenderContext(
    entt::registry& registry, RenderSnapshot* snapshot, double currentTime,
    float singleTrackW, const Config::EditorConfig& config)
{
    NoteRenderSystem::NoteRenderContext ctx{};

    const auto** cachePtr = registry.ctx().find<const ScrollCache*>();
    if ( !cachePtr || !(*cachePtr) ) return ctx;
    ctx.cache       = *cachePtr;
    ctx.currentAbsY = ctx.cache->getAbsY(currentTime);

    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return ctx;
    ctx.baseAspect = itBase->second.z / itBase->second.w;

    ctx.noteW = singleTrackW * config.visual.noteScaleX;
    ctx.noteH = ctx.noteW / ctx.baseAspect;

    auto& skin      = Config::SkinManager::instance();
    auto  color_tap = skin.getColor("note_tap");
    ctx.colorTap    = { color_tap.r, color_tap.g, color_tap.b, color_tap.a };

    auto color_hold = skin.getColor("note_hold");
    ctx.colorHold = { color_hold.r, color_hold.g, color_hold.b, color_hold.a };

    auto color_node = skin.getColor("note_node");
    ctx.colorNode = { color_node.r, color_node.g, color_node.b, color_node.a };

    auto color_arrow = skin.getColor("note_flick_arrow");
    ctx.colorArrow   = {
        color_arrow.r, color_arrow.g, color_arrow.b, color_arrow.a
    };

    return ctx;
}

void NoteRenderSystem::generateNoteHitboxes(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx, float judgmentLineY,
    float leftX, float topY, float bottomY, float singleTrackW,
    float renderScaleY, const Config::EditorConfig& config)
{
    auto noteView =
        registry.view<const TransformComponent, const NoteComponent>();

    // Pass 1: Body (Lower Priority)
    for ( auto entity : noteView ) {
        const auto& transform = noteView.get<const TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);

        double noteAbsY = ctx.cache->getAbsY(note.m_timestamp);
        float  screenY =
            judgmentLineY -
            static_cast<float>(noteAbsY - ctx.currentAbsY) * renderScaleY;
        float visualH = transform.m_size.y * renderScaleY;

        if ( screenY - visualH - ctx.noteH > bottomY ||
             screenY + ctx.noteH < topY )
            continue;

        if ( !note.m_isSubNote ) {
            if ( note.m_type == ::MMM::NoteType::FLICK && note.m_dtrack != 0 ) {
                float startTrack =
                    std::min((float)note.m_trackIndex,
                             (float)note.m_trackIndex + note.m_dtrack);
                float drawW = std::abs(note.m_dtrack) * singleTrackW;
                float bodyX =
                    leftX + startTrack * singleTrackW + singleTrackW * 0.5f;

                float drawH   = ctx.noteH;
                auto  itBodyH = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
                if ( itBodyH != snapshot->uvMap.end() ) {
                    drawH = ctx.noteH *
                            (itBodyH->second.w /
                             snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
                }

                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               -1,
                                               bodyX,
                                               screenY - drawH * 0.5f,
                                               drawW,
                                               drawH });
            } else if ( note.m_type == ::MMM::NoteType::HOLD &&
                        visualH > ctx.noteH * 0.1f ) {
                float headX = leftX + note.m_trackIndex * singleTrackW +
                              (singleTrackW - ctx.noteW) * 0.5f;
                // Hold Body 从 Head 中心向上拉到 End 中心 (visualH
                // 是两者的距离)
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               -1,
                                               headX,
                                               screenY - visualH,
                                               ctx.noteW,
                                               visualH });
            }
        }
    }

    // Pass 2: Head/End/Arrow (Higher Priority)
    for ( auto entity : noteView ) {
        const auto& transform = noteView.get<const TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);
        double      noteAbsY  = ctx.cache->getAbsY(note.m_timestamp);
        float       screenY =
            judgmentLineY -
            (static_cast<float>(noteAbsY - ctx.currentAbsY) * renderScaleY);
        float visualH = transform.m_size.y * renderScaleY;

        if ( screenY - visualH - ctx.noteH > bottomY ||
             screenY + ctx.noteH < topY )
            continue;

        float headX = leftX + note.m_trackIndex * singleTrackW +
                      (singleTrackW - ctx.noteW) * 0.5f;

        if ( !note.m_isSubNote && note.m_type != ::MMM::NoteType::POLYLINE ) {
            // 所有非 Polyline 音符的 Head
            snapshot->hitboxes.push_back({ entity,
                                           HoverPart::Head,
                                           -1,
                                           headX,
                                           screenY - ctx.noteH * 0.5f,
                                           ctx.noteW,
                                           ctx.noteH });

            if ( note.m_type == ::MMM::NoteType::FLICK && note.m_dtrack != 0 ) {
                TextureID arrowId = (note.m_dtrack < 0)
                                        ? TextureID::FlickArrowLeft
                                        : TextureID::FlickArrowRight;
                auto  it = snapshot->uvMap.find(static_cast<uint32_t>(arrowId));
                float arrowW = ctx.noteW;
                float arrowH = ctx.noteH;
                if ( it != snapshot->uvMap.end() ) {
                    float baseWRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).z;
                    float wRatio = it->second.z / baseWRatio;
                    arrowW       = ctx.noteW * wRatio;
                    arrowH       = arrowW * (it->second.w / it->second.z);
                }

                float arrowX =
                    leftX + (note.m_trackIndex + note.m_dtrack) * singleTrackW +
                    (singleTrackW - arrowW) * 0.5f;

                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::FlickArrow,
                                               -1,
                                               arrowX,
                                               screenY - arrowH * 0.5f,
                                               arrowW,
                                               arrowH });
            } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
                snapshot->hitboxes.push_back(
                    { entity,
                      HoverPart::HoldEnd,
                      -1,
                      headX,
                      screenY - visualH - ctx.noteH * 0.5f,
                      ctx.noteW,
                      ctx.noteH });
            }
        }
    }
}

void NoteRenderSystem::renderNoteBaseLayer(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig& config, Batcher& batcher, float currentTime,
    float judgmentLineY, float leftX, float rightX, float topY, float bottomY,
    float singleTrackW, float renderScaleY)
{
    auto                      noteView = registry.view<const NoteComponent>();
    std::vector<entt::entity> visibleEntities;
    for ( auto entity : noteView ) {
        const auto& note = noteView.get<const NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        double noteAbsY = ctx.cache->getAbsY(note.m_timestamp);
        float  screenY =
            judgmentLineY -
            (static_cast<float>(noteAbsY - ctx.currentAbsY) * renderScaleY);

        if ( note.m_type != ::MMM::NoteType::POLYLINE ) {
            const auto& transform =
                registry.get<const TransformComponent>(entity);
            float visualH = transform.m_size.y * renderScaleY;
            if ( screenY - visualH - ctx.noteH > bottomY ||
                 screenY + ctx.noteH < topY )
                continue;
        }

        visibleEntities.push_back(entity);
    }

    std::sort(visibleEntities.begin(),
              visibleEntities.end(),
              [&registry, &ctx](entt::entity a, entt::entity b) {
                  const auto &nA = registry.get<const NoteComponent>(a),
                             &nB = registry.get<const NoteComponent>(b);
                  return (std::abs(nA.m_timestamp - nB.m_timestamp) > 1e-6)
                             ? (nA.m_timestamp > nB.m_timestamp)
                             : (a > b);
              });

    for ( auto entity : visibleEntities ) {
        const auto& transform = registry.get<const TransformComponent>(entity);
        const auto& note      = registry.get<const NoteComponent>(entity);

        // 处理拖拽时的半透明预览效果
        float alphaMul = 1.0f;
        if ( auto* ic = registry.try_get<InteractionComponent>(entity) ) {
            if ( ic->isDragging ) {
                alphaMul = 0.5f;
            }
        }

        double noteAbsY = ctx.cache->getAbsY(note.m_timestamp);
        float  screenY =
            judgmentLineY -
            (static_cast<float>(noteAbsY - ctx.currentAbsY) * renderScaleY);
        float visualH = transform.m_size.y * renderScaleY;
        float trackX  = leftX + note.m_trackIndex * singleTrackW;

        // 应用 Alpha 倍率
        glm::vec4 curColorTap   = ctx.colorTap;
        glm::vec4 curColorHold  = ctx.colorHold;
        glm::vec4 curColorNode  = ctx.colorNode;
        glm::vec4 curColorArrow = ctx.colorArrow;
        curColorTap.a *= alphaMul;
        curColorHold.a *= alphaMul;
        curColorNode.a *= alphaMul;
        curColorArrow.a *= alphaMul;

        if ( note.m_type == ::MMM::NoteType::NOTE )
            NoteRenderSystem::renderTap(
                batcher,
                note,
                config,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                ctx.baseAspect,
                curColorTap);
        else if ( note.m_type == ::MMM::NoteType::HOLD )
            NoteRenderSystem::renderHold(
                batcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                visualH,
                singleTrackW,
                curColorHold);
        else if ( note.m_type == ::MMM::NoteType::FLICK )
            NoteRenderSystem::renderFlick(
                batcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                singleTrackW,
                curColorHold,
                curColorArrow);
        else if ( note.m_type == ::MMM::NoteType::POLYLINE )
            NoteRenderSystem::renderPolyline(registry,
                                             batcher,
                                             note,
                                             config,
                                             snapshot,
                                             (double)currentTime,
                                             judgmentLineY,
                                             leftX,
                                             rightX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             curColorHold,
                                             curColorNode,
                                             curColorArrow,
                                             entity,
                                             true);
    }
    batcher.flush();
}

void NoteRenderSystem::renderNoteGlowLayer(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig& config, float currentTime, float judgmentLineY,
    float leftX, float rightX, float topY, float bottomY, float singleTrackW,
    float renderScaleY)
{
    auto interactionView = registry.view<const InteractionComponent>();
    std::vector<entt::entity> hoveredEntities;
    for ( auto entity : interactionView ) {
        const auto& ic =
            interactionView.get<const InteractionComponent>(entity);
        if ( ic.isHovered ) hoveredEntities.push_back(entity);
    }

    if ( hoveredEntities.empty() ) return;

    std::sort(hoveredEntities.begin(),
              hoveredEntities.end(),
              [&registry, &ctx](entt::entity a, entt::entity b) {
                  const auto &nA = registry.get<const NoteComponent>(a),
                             &nB = registry.get<const NoteComponent>(b);
                  return (std::abs(nA.m_timestamp - nB.m_timestamp) > 1e-6)
                             ? (nA.m_timestamp > nB.m_timestamp)
                             : (a > b);
              });

    Batcher glowBatcher(snapshot, &snapshot->glowCmds);
    for ( auto entity : hoveredEntities ) {
        const auto& transform = registry.get<const TransformComponent>(entity);
        const auto& note      = registry.get<const NoteComponent>(entity);
        const auto& ic       = registry.get<const InteractionComponent>(entity);
        double      noteAbsY = ctx.cache->getAbsY(note.m_timestamp);
        float       screenY =
            judgmentLineY -
            (static_cast<float>(noteAbsY - ctx.currentAbsY) * renderScaleY);
        float     visualH  = transform.m_size.y * renderScaleY;
        float     trackX   = leftX + note.m_trackIndex * singleTrackW;
        HoverPart glowPart = static_cast<HoverPart>(ic.hoveredPart);
        int       glowIdx  = ic.hoveredSubIndex;

        if ( note.m_type == ::MMM::NoteType::NOTE )
            NoteRenderSystem::renderTap(
                glowBatcher,
                note,
                config,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                ctx.baseAspect,
                ctx.colorTap);
        else if ( note.m_type == ::MMM::NoteType::HOLD )
            NoteRenderSystem::renderHold(
                glowBatcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                visualH,
                singleTrackW,
                ctx.colorHold,
                glowPart);
        else if ( note.m_type == ::MMM::NoteType::FLICK )
            NoteRenderSystem::renderFlick(
                glowBatcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                singleTrackW,
                ctx.colorHold,
                ctx.colorArrow,
                glowPart);
        else if ( note.m_type == ::MMM::NoteType::POLYLINE )
            NoteRenderSystem::renderPolyline(registry,
                                             glowBatcher,
                                             note,
                                             config,
                                             snapshot,
                                             (double)currentTime,
                                             judgmentLineY,
                                             leftX,
                                             rightX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             ctx.colorHold,
                                             ctx.colorNode,
                                             ctx.colorArrow,
                                             entity,
                                             false,
                                             glowPart,
                                             glowIdx);
    }
    glowBatcher.flush();
}

}  // namespace MMM::Logic::System
