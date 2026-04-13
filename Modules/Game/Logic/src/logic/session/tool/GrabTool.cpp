#include "logic/session/tool/GrabTool.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/EditorAction.h"
#include "logic/session/NoteAction.h"
#include <algorithm>
#include <cmath>

namespace MMM::Logic
{

void GrabTool::handleStartDrag(BeatmapSession& session, const CmdStartDrag& cmd)
{
    if ( cmd.entity != entt::null &&
         session.m_noteRegistry.valid(cmd.entity) ) {
        session.m_draggedEntity = cmd.entity;
        session.m_dragCameraId  = cmd.cameraId;
        session.m_draggedPart   = static_cast<HoverPart>(session.m_hoveredPart);
        session.m_draggedSubIndex = session.m_hoveredSubIndex;

        if ( !session.m_noteRegistry.all_of<InteractionComponent>(
                 cmd.entity) ) {
            session.m_noteRegistry.emplace<InteractionComponent>(cmd.entity);
        }
        session.m_noteRegistry.get<InteractionComponent>(cmd.entity)
            .isDragging = true;

        if ( auto* note =
                 session.m_noteRegistry.try_get<NoteComponent>(cmd.entity) ) {
            session.m_dragInitialNote = *note;
        }
    }
}

void GrabTool::handleUpdateDrag(BeatmapSession&      session,
                                const CmdUpdateDrag& cmd)
{
    if ( session.m_draggedEntity != entt::null &&
         session.m_noteRegistry.valid(session.m_draggedEntity) ) {
        auto it = session.m_cameras.find(cmd.cameraId);
        if ( it != session.m_cameras.end() ) {
            float judgmentLineY = it->second.viewportHeight *
                                  session.m_lastConfig.visual.judgeline_pos;

            float renderScaleY = 1.0f;
            if ( cmd.cameraId == "Preview" ) {
                auto  itMain = session.m_cameras.find("Basic2DCanvas");
                float mainViewportHeight = itMain != session.m_cameras.end()
                                               ? itMain->second.viewportHeight
                                               : it->second.viewportHeight;

                float mainEffectiveH =
                    (session.m_lastConfig.visual.trackLayout.bottom -
                     session.m_lastConfig.visual.trackLayout.top) *
                    mainViewportHeight;
                float ty = session.m_lastConfig.visual.previewConfig.margin.top;
                float by =
                    it->second.viewportHeight -
                    session.m_lastConfig.visual.previewConfig.margin.bottom;
                float previewDrawH = by - ty;

                renderScaleY =
                    previewDrawH /
                    (mainEffectiveH *
                     session.m_lastConfig.visual.previewConfig.areaRatio);
            } else {
                renderScaleY = session.m_lastConfig.visual.noteScaleY;
            }

            auto* cache =
                session.m_timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                double currentAbsY = cache->getAbsY(session.m_visualTime);
                double targetAbsY =
                    currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
                double targetTime = cache->getTime(targetAbsY);

                std::vector<const TimelineComponent*> bpmEvents;
                auto                                  tlView =
                    session.m_timelineRegistry.view<const TimelineComponent>();
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

                auto snap = session.getSnapResult(targetTime,
                                                  cmd.mouseY,
                                                  it->second,
                                                  session.m_lastConfig,
                                                  bpmEvents);
                if ( snap.isSnapped ) {
                    targetTime = snap.snappedTime;
                }

                if ( auto* note = session.m_noteRegistry.try_get<NoteComponent>(
                         session.m_draggedEntity) ) {
                    if ( note->m_type == ::MMM::NoteType::HOLD &&
                         session.m_draggedPart == HoverPart::HoldEnd ) {
                        // 1. 拖拽 Hold 尾部：调整持续时间 (Duration)
                        double newDuration = targetTime - note->m_timestamp;
                        if ( newDuration < 0.0 ) newDuration = 0.0;
                        note->m_duration = newDuration;
                    } else if ( note->m_type == ::MMM::NoteType::FLICK &&
                                session.m_draggedPart ==
                                    HoverPart::FlickArrow ) {
                        // 2. 拖拽 Flick 箭头：调整偏移轨道数 (dtrack)
                        float leftX =
                            it->second.viewportWidth *
                            session.m_lastConfig.visual.trackLayout.left;
                        float rightX =
                            it->second.viewportWidth *
                            session.m_lastConfig.visual.trackLayout.right;
                        float trackAreaW = rightX - leftX;
                        float noteW      = trackAreaW / session.m_trackCount;

                        int targetTrack = static_cast<int>(std::round(
                            (cmd.mouseX - leftX - noteW / 2.0f) / noteW));
                        targetTrack     = std::clamp(
                            targetTrack, 0, session.m_trackCount - 1);

                        note->m_dtrack = targetTrack - note->m_trackIndex;
                    } else {
                        // 3. 拖拽音符头部或身体（或非 Hold/Flick
                        // 特殊部位）：移动整个音符
                        note->m_timestamp = targetTime;

                        float leftX =
                            it->second.viewportWidth *
                            session.m_lastConfig.visual.trackLayout.left;
                        float rightX =
                            it->second.viewportWidth *
                            session.m_lastConfig.visual.trackLayout.right;
                        float trackAreaW = rightX - leftX;
                        float noteW      = trackAreaW / session.m_trackCount;
                        int   track      = static_cast<int>(std::round(
                            (cmd.mouseX - leftX - noteW / 2.0f) / noteW));
                        track = std::clamp(track, 0, session.m_trackCount - 1);
                        note->m_trackIndex = track;

                        if ( auto* trans = session.m_noteRegistry
                                               .try_get<TransformComponent>(
                                                   session.m_draggedEntity) ) {
                            trans->m_pos.x = leftX + track * noteW;
                        }
                    }
                }
            }
        }
    }
}

void GrabTool::handleEndDrag(BeatmapSession& session, const CmdEndDrag& cmd)
{
    if ( session.m_draggedEntity != entt::null &&
         session.m_noteRegistry.valid(session.m_draggedEntity) ) {
        if ( session.m_noteRegistry.all_of<InteractionComponent>(
                 session.m_draggedEntity) ) {
            session.m_noteRegistry
                .get<InteractionComponent>(session.m_draggedEntity)
                .isDragging = false;
        }

        // 提交撤销操作
        if ( session.m_dragInitialNote.has_value() ) {
            auto* currentNote = session.m_noteRegistry.try_get<NoteComponent>(
                session.m_draggedEntity);
            if ( currentNote ) {
                auto action =
                    std::make_unique<NoteAction>(NoteAction::Type::Update,
                                                 session.m_draggedEntity,
                                                 *session.m_dragInitialNote,
                                                 *currentNote);
                session.m_actionStack.pushAndExecute(std::move(action),
                                                     session);
            }
        }

        session.rebuildHitEvents();
    }
    session.m_draggedEntity   = entt::null;
    session.m_dragInitialNote = std::nullopt;
}

}  // namespace MMM::Logic
