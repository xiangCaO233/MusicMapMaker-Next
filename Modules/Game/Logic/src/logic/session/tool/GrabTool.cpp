#include "logic/session/tool/GrabTool.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/EditorAction.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <cmath>

namespace MMM::Logic
{

void GrabTool::handleStartDrag(SessionContext& ctx, const CmdStartDrag& cmd)
{
    if ( cmd.entity != entt::null && ctx.noteRegistry.valid(cmd.entity) ) {
        ctx.draggedEntity   = cmd.entity;
        ctx.dragCameraId    = cmd.cameraId;
        ctx.draggedPart     = static_cast<HoverPart>(ctx.hoveredPart);
        ctx.draggedSubIndex = ctx.hoveredSubIndex;

        if ( !ctx.noteRegistry.all_of<InteractionComponent>(cmd.entity) ) {
            ctx.noteRegistry.emplace<InteractionComponent>(cmd.entity);
        }
        ctx.noteRegistry.get<InteractionComponent>(cmd.entity).isDragging =
            true;

        if ( auto* note =
                 ctx.noteRegistry.try_get<NoteComponent>(cmd.entity) ) {
            ctx.dragInitialNote = *note;
        }
    }
}

void GrabTool::handleUpdateDrag(SessionContext& ctx, const CmdUpdateDrag& cmd)
{
    if ( ctx.draggedEntity != entt::null &&
         ctx.noteRegistry.valid(ctx.draggedEntity) ) {
        auto it = ctx.cameras.find(cmd.cameraId);
        if ( it != ctx.cameras.end() ) {
            float judgmentLineY =
                it->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;

            float renderScaleY = 1.0f;
            if ( cmd.cameraId == "Preview" ) {
                auto  itMain             = ctx.cameras.find("Basic2DCanvas");
                float mainViewportHeight = itMain != ctx.cameras.end()
                                               ? itMain->second.viewportHeight
                                               : it->second.viewportHeight;

                float mainEffectiveH =
                    (ctx.lastConfig.visual.trackLayout.bottom -
                     ctx.lastConfig.visual.trackLayout.top) *
                    mainViewportHeight;
                float ty = ctx.lastConfig.visual.previewConfig.margin.top;
                float by = it->second.viewportHeight -
                           ctx.lastConfig.visual.previewConfig.margin.bottom;
                float previewDrawH = by - ty;

                renderScaleY = previewDrawH /
                               (mainEffectiveH *
                                ctx.lastConfig.visual.previewConfig.areaRatio);
            } else {
                renderScaleY = ctx.lastConfig.visual.noteScaleY;
            }

            auto* cache =
                ctx.timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                double currentAbsY = cache->getAbsY(ctx.visualTime);
                double targetAbsY =
                    currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
                double targetTime = cache->getTime(targetAbsY);

                std::vector<const TimelineComponent*> bpmEvents;
                auto                                  tlView =
                    ctx.timelineRegistry.view<const TimelineComponent>();
                for ( auto entity : tlView ) {
                    const auto& tl =
                        tlView.get<const TimelineComponent>(entity);
                    if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
                        bpmEvents.push_back(&tl);
                    }
                }
                std::stable_sort(bpmEvents.begin(),
                                 bpmEvents.end(),
                                 [](const auto* a, const auto* b) {
                                     return a->m_timestamp < b->m_timestamp;
                                 });

                auto snap = SessionUtils::getSnapResult(targetTime,
                                                        cmd.mouseY,
                                                        it->second,
                                                        ctx.lastConfig,
                                                        bpmEvents,
                                                        ctx.timelineRegistry,
                                                        ctx.visualTime,
                                                        ctx.cameras);
                if ( snap.isSnapped ) {
                    targetTime = snap.snappedTime;
                }

                if ( auto* note = ctx.noteRegistry.try_get<NoteComponent>(
                         ctx.draggedEntity) ) {
                    if ( note->m_type == ::MMM::NoteType::HOLD &&
                         ctx.draggedPart == HoverPart::HoldEnd ) {
                        // 1. 拖拽 Hold 尾部：调整持续时间 (Duration)
                        double newDuration = targetTime - note->m_timestamp;
                        note->m_duration   = std::max(0.0, newDuration);
                    } else if ( note->m_type == ::MMM::NoteType::FLICK &&
                                ctx.draggedPart == HoverPart::FlickArrow ) {
                        // 2. 拖拽 Flick 箭头：调整偏移轨道数 (dtrack)
                        float leftX  = it->second.viewportWidth *
                                       ctx.lastConfig.visual.trackLayout.left;
                        float rightX = it->second.viewportWidth *
                                       ctx.lastConfig.visual.trackLayout.right;
                        float trackAreaW = rightX - leftX;
                        float noteW      = trackAreaW / ctx.trackCount;

                        int targetTrack = static_cast<int>(std::round(
                            (cmd.mouseX - leftX - noteW / 2.0f) / noteW));
                        targetTrack =
                            std::clamp(targetTrack, 0, ctx.trackCount - 1);

                        note->m_dtrack = targetTrack - note->m_trackIndex;
                        if ( note->m_dtrack == 0 ) {
                            float centerX = leftX + note->m_trackIndex * noteW +
                                            noteW / 2.0f;
                            if ( cmd.mouseX > centerX ) {
                                note->m_dtrack = 1;
                            } else {
                                note->m_dtrack = -1;
                            }
                            // 确保不越界
                            if ( note->m_trackIndex + note->m_dtrack >=
                                 ctx.trackCount ) {
                                note->m_dtrack = -1;
                            } else if ( note->m_trackIndex + note->m_dtrack <
                                        0 ) {
                                note->m_dtrack = 1;
                            }
                        }
                    } else {
                        // 3. 拖拽音符头部或身体（或非 Hold/Flick
                        // 特殊部位）：移动整个音符
                        note->m_timestamp = std::max(0.0, targetTime);

                        float leftX  = it->second.viewportWidth *
                                       ctx.lastConfig.visual.trackLayout.left;
                        float rightX = it->second.viewportWidth *
                                       ctx.lastConfig.visual.trackLayout.right;
                        float trackAreaW = rightX - leftX;
                        float noteW      = trackAreaW / ctx.trackCount;
                        int   track      = static_cast<int>(std::round(
                            (cmd.mouseX - leftX - noteW / 2.0f) / noteW));
                        track = std::clamp(track, 0, ctx.trackCount - 1);

                        if ( note->m_type == ::MMM::NoteType::FLICK ) {
                            int targetEndTrack = track + note->m_dtrack;
                            if ( targetEndTrack < 0 ) {
                                track = -note->m_dtrack;
                            } else if ( targetEndTrack >= ctx.trackCount ) {
                                track = ctx.trackCount - 1 - note->m_dtrack;
                            }
                        }

                        note->m_trackIndex = track;

                        if ( auto* trans =
                                 ctx.noteRegistry.try_get<TransformComponent>(
                                     ctx.draggedEntity) ) {
                            trans->m_pos.x = leftX + track * noteW;
                        }
                    }
                }
            }
        }
    }
}

void GrabTool::handleEndDrag(SessionContext& ctx, const CmdEndDrag& cmd)
{
    if ( ctx.draggedEntity != entt::null &&
         ctx.noteRegistry.valid(ctx.draggedEntity) ) {
        if ( ctx.noteRegistry.all_of<InteractionComponent>(
                 ctx.draggedEntity) ) {
            ctx.noteRegistry.get<InteractionComponent>(ctx.draggedEntity)
                .isDragging = false;
        }

        // 提交撤销操作
        if ( ctx.dragInitialNote.has_value() ) {
            auto* currentNote =
                ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
            if ( currentNote ) {
                auto action =
                    std::make_unique<NoteAction>(NoteAction::Type::Update,
                                                 ctx.draggedEntity,
                                                 *ctx.dragInitialNote,
                                                 *currentNote);
                ctx.actionStack.pushAndExecute(std::move(action), ctx);
            }
        }

        SessionUtils::rebuildHitEvents(ctx);
    }
    ctx.draggedEntity   = entt::null;
    ctx.dragInitialNote = std::nullopt;
}

}  // namespace MMM::Logic
