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
    auto noteView =
        registry.view<const TransformComponent, const NoteComponent>();

    const auto** cachePtr = registry.ctx().find<const ScrollCache*>();
    if ( !cachePtr || !(*cachePtr) ) return;
    const auto* cache       = *cachePtr;
    double      currentAbsY = cache->getAbsY(currentTime);

    // 1. 获取基准纹理 (Note) 的比例与信息
    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return;
    float baseAspect = itBase->second.z / itBase->second.w;

    // 2. 计算基准尺寸 (画布空间)
    float noteW = singleTrackW * config.visual.noteScaleX;
    float noteH = noteW / baseAspect;

    // 3. 获取皮肤配色方案
    auto& skin   = Config::SkinManager::instance();
    auto  toVec4 = [](const Config::Color& c) {
        return glm::vec4(c.r, c.g, c.b, c.a);
    };
    auto color_tap   = skin.getColor("note_tap");
    auto color_hold  = skin.getColor("note_hold");
    auto color_node  = skin.getColor("note_node");
    auto color_arrow = skin.getColor("note_flick_arrow");

    glm::vec4 colorTap = { color_tap.r, color_tap.g, color_tap.b, color_tap.a };
    glm::vec4 colorHold = {
        color_hold.r, color_hold.g, color_hold.b, color_hold.a
    };
    glm::vec4 colorNode = {
        color_node.r, color_node.g, color_node.b, color_node.a
    };
    glm::vec4 colorArrow = {
        color_arrow.r, color_arrow.g, color_arrow.b, color_arrow.a
    };

    // 4. 生成精确碰撞盒 (分两遍确保优先级：Body 在下，Head 在上)
    std::unordered_set<entt::entity> visibleEntities;

    // Pass 1: Body and Transition Hitboxes (Lower Priority)
    for ( auto entity : noteView ) {
        const auto& transform = noteView.get<const TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);

        double noteAbsY = cache->getAbsY(note.m_timestamp);
        float  minY     = static_cast<float>(noteAbsY - currentAbsY);
        float  visualH  = transform.m_size.y * renderScaleY;
        float  screenY  = judgmentLineY - (minY * renderScaleY);

        if ( screenY - visualH - noteH > bottomY || screenY + noteH < topY )
            continue;

        if ( note.m_isSubNote ) {
            if ( note.m_parentPolyline != entt::null ) {
                visibleEntities.insert(note.m_parentPolyline);
                int         subIndex = -1;
                const auto* parentNote =
                    registry.try_get<NoteComponent>(note.m_parentPolyline);
                if ( parentNote ) {
                    for ( size_t i = 0; i < parentNote->m_subNotes.size();
                          ++i ) {
                        if ( std::abs(parentNote->m_subNotes[i].timestamp -
                                      note.m_timestamp) < 1e-4 ) {
                            subIndex = static_cast<int>(i);
                            break;
                        }
                    }
                }

                // 子物件自身 Body Hitbox
                if ( note.m_type == ::MMM::NoteType::FLICK &&
                     note.m_dtrack != 0 ) {
                    float startTrack =
                        std::min((float)note.m_trackIndex,
                                 (float)note.m_trackIndex + note.m_dtrack);
                    float drawW = std::abs(note.m_dtrack) * singleTrackW;
                    float bodyX =
                        leftX + startTrack * singleTrackW + singleTrackW * 0.5f;
                    snapshot->hitboxes.push_back({ note.m_parentPolyline,
                                                   HoverPart::HoldBody,
                                                   subIndex,
                                                   bodyX,
                                                   screenY - noteH * 0.5f,
                                                   drawW,
                                                   noteH });
                } else if ( note.m_type == ::MMM::NoteType::HOLD &&
                            visualH > noteH * 0.1f ) {
                    float headX = leftX + note.m_trackIndex * singleTrackW +
                                  (singleTrackW - noteW) * 0.5f;
                    snapshot->hitboxes.push_back(
                        { note.m_parentPolyline,
                          HoverPart::HoldBody,
                          subIndex,
                          headX,
                          screenY - visualH + noteH * 0.5f,
                          noteW,
                          visualH - noteH });
                }
            }
        } else {
            visibleEntities.insert(entity);
            if ( note.m_type == ::MMM::NoteType::FLICK && note.m_dtrack != 0 ) {
                float startTrack =
                    std::min((float)note.m_trackIndex,
                             (float)note.m_trackIndex + note.m_dtrack);
                float drawW = std::abs(note.m_dtrack) * singleTrackW;
                float bodyX =
                    leftX + startTrack * singleTrackW + singleTrackW * 0.5f;
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               -1,
                                               bodyX,
                                               screenY - noteH * 0.5f,
                                               drawW,
                                               noteH });
            } else if ( note.m_type == ::MMM::NoteType::HOLD &&
                        visualH > noteH * 0.1f ) {
                float headX = leftX + note.m_trackIndex * singleTrackW +
                              (singleTrackW - noteW) * 0.5f;
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               -1,
                                               headX,
                                               screenY - visualH + noteH * 0.5f,
                                               noteW,
                                               visualH - noteH });
            } else if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
                // Polyline Hitboxes are now generated inside renderPolyline
                // during the base pass
            }
        }
    }

    // Pass 2: Head, Node, Arrow, and End Hitboxes (Higher Priority)
    for ( auto entity : noteView ) {
        const auto& transform = noteView.get<const TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);
        double      noteAbsY  = cache->getAbsY(note.m_timestamp);
        float       screenY =
            judgmentLineY -
            (static_cast<float>(noteAbsY - currentAbsY) * renderScaleY);
        float visualH = transform.m_size.y * renderScaleY;
        if ( screenY - visualH - noteH > bottomY || screenY + noteH < topY )
            continue;

        float headX = leftX + note.m_trackIndex * singleTrackW +
                      (singleTrackW - noteW) * 0.5f;

        if ( note.m_isSubNote ) {
            // Polyline sub-note hitboxes are now handled inside renderPolyline
            // for the parent entity
        } else {
            if ( note.m_type != ::MMM::NoteType::POLYLINE ) {
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::Head,
                                               -1,
                                               headX,
                                               screenY - noteH * 0.5f,
                                               noteW,
                                               noteH });
                if ( note.m_type == ::MMM::NoteType::FLICK &&
                     note.m_dtrack != 0 ) {
                    float arrowX =
                        leftX +
                        (note.m_trackIndex + note.m_dtrack) * singleTrackW +
                        (singleTrackW - noteW) * 0.5f;
                    snapshot->hitboxes.push_back({ entity,
                                                   HoverPart::FlickArrow,
                                                   -1,
                                                   arrowX,
                                                   screenY - noteH * 0.5f,
                                                   noteW,
                                                   noteH });
                } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
                    snapshot->hitboxes.push_back(
                        { entity,
                          HoverPart::HoldEnd,
                          -1,
                          headX,
                          screenY - visualH - noteH * 0.5f,
                          noteW,
                          noteH });
                }
            }
        }
    }

    // 5. 排序渲染
    std::vector<entt::entity> sortedEntities(visibleEntities.begin(),
                                             visibleEntities.end());
    std::sort(sortedEntities.begin(),
              sortedEntities.end(),
              [&registry, cache](entt::entity a, entt::entity b) {
                  const auto &nA = registry.get<NoteComponent>(a),
                             &nB = registry.get<NoteComponent>(b);
                  return (std::abs(nA.m_timestamp - nB.m_timestamp) > 1e-6)
                             ? (nA.m_timestamp > nB.m_timestamp)
                             : (a > b);
              });

    // 6. 基础层渲染
    for ( auto entity : sortedEntities ) {
        const auto& transform = registry.get<TransformComponent>(entity);
        const auto& note      = registry.get<NoteComponent>(entity);
        double      noteAbsY  = cache->getAbsY(note.m_timestamp);
        float       screenY =
            judgmentLineY -
            (static_cast<float>(noteAbsY - currentAbsY) * renderScaleY);
        float visualH = transform.m_size.y * renderScaleY;
        float trackX  = leftX + note.m_trackIndex * singleTrackW;

        if ( note.m_type == ::MMM::NoteType::NOTE )
            NoteRenderSystem::renderTap(batcher,
                                        note,
                                        config,
                                        trackX + (singleTrackW - noteW) * 0.5f,
                                        screenY,
                                        noteW,
                                        noteH,
                                        baseAspect,
                                        colorTap);
        else if ( note.m_type == ::MMM::NoteType::HOLD )
            NoteRenderSystem::renderHold(batcher,
                                         note,
                                         config,
                                         snapshot,
                                         trackX + (singleTrackW - noteW) * 0.5f,
                                         screenY,
                                         noteW,
                                         noteH,
                                         visualH,
                                         singleTrackW,
                                         colorHold);
        else if ( note.m_type == ::MMM::NoteType::FLICK )
            NoteRenderSystem::renderFlick(
                batcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - noteW) * 0.5f,
                screenY,
                noteW,
                noteH,
                singleTrackW,
                colorHold,
                colorArrow);
        else if ( note.m_type == ::MMM::NoteType::POLYLINE )
            NoteRenderSystem::renderPolyline(registry,
                                             batcher,
                                             note,
                                             config,
                                             snapshot,
                                             currentTime,
                                             judgmentLineY,
                                             leftX,
                                             rightX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             colorHold,
                                             colorNode,
                                             colorArrow,
                                             entity,
                                             true);
    }
    batcher.flush();

    // 7. 发光层渲染
    Batcher glowBatcher(snapshot, &snapshot->glowCmds);
    for ( auto entity : sortedEntities ) {
        const auto* ic = registry.try_get<InteractionComponent>(entity);
        if ( !ic || !ic->isHovered ) continue;
        const auto& transform = registry.get<TransformComponent>(entity);
        const auto& note      = registry.get<NoteComponent>(entity);
        double      noteAbsY  = cache->getAbsY(note.m_timestamp);
        float       screenY =
            judgmentLineY -
            (static_cast<float>(noteAbsY - currentAbsY) * renderScaleY);
        float     visualH  = transform.m_size.y * renderScaleY;
        float     trackX   = leftX + note.m_trackIndex * singleTrackW;
        HoverPart glowPart = static_cast<HoverPart>(ic->hoveredPart);
        int       glowIdx  = ic->hoveredSubIndex;

        if ( note.m_type == ::MMM::NoteType::NOTE )
            NoteRenderSystem::renderTap(glowBatcher,
                                        note,
                                        config,
                                        trackX + (singleTrackW - noteW) * 0.5f,
                                        screenY,
                                        noteW,
                                        noteH,
                                        baseAspect,
                                        colorTap);
        else if ( note.m_type == ::MMM::NoteType::HOLD )
            NoteRenderSystem::renderHold(glowBatcher,
                                         note,
                                         config,
                                         snapshot,
                                         trackX + (singleTrackW - noteW) * 0.5f,
                                         screenY,
                                         noteW,
                                         noteH,
                                         visualH,
                                         singleTrackW,
                                         colorHold,
                                         glowPart);
        else if ( note.m_type == ::MMM::NoteType::FLICK )
            NoteRenderSystem::renderFlick(
                glowBatcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - noteW) * 0.5f,
                screenY,
                noteW,
                noteH,
                singleTrackW,
                colorHold,
                colorArrow,
                glowPart);
        else if ( note.m_type == ::MMM::NoteType::POLYLINE )
            NoteRenderSystem::renderPolyline(registry,
                                             glowBatcher,
                                             note,
                                             config,
                                             snapshot,
                                             currentTime,
                                             judgmentLineY,
                                             leftX,
                                             rightX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             colorHold,
                                             colorNode,
                                             colorArrow,
                                             entity,
                                             false,
                                             glowPart,
                                             glowIdx);
    }
    glowBatcher.flush();
}

}  // namespace MMM::Logic::System
