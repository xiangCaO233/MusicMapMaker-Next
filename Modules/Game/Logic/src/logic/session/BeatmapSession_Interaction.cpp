#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>

namespace MMM::Logic
{

void BeatmapSession::handleCommand(const CmdSetHoveredEntity& cmd)
{
    auto view = m_noteRegistry.view<InteractionComponent>();
    for ( auto entity : view ) {
        auto& ic           = m_noteRegistry.get<InteractionComponent>(entity);
        ic.isHovered       = false;
        ic.hoveredPart     = 0;
        ic.hoveredSubIndex = -1;
    }
    if ( cmd.entity != entt::null ) {
        if ( !m_noteRegistry.all_of<InteractionComponent>(cmd.entity) ) {
            m_noteRegistry.emplace<InteractionComponent>(cmd.entity);
        }
        auto& ic           = m_noteRegistry.get<InteractionComponent>(cmd.entity);
        ic.isHovered       = true;
        ic.hoveredPart     = cmd.part;
        ic.hoveredSubIndex = cmd.subIndex;
    }
}

void BeatmapSession::handleCommand(const CmdSelectEntity& cmd)
{
    if ( cmd.clearOthers ) {
        auto view = m_noteRegistry.view<InteractionComponent>();
        for ( auto entity : view ) {
            m_noteRegistry.get<InteractionComponent>(entity).isSelected = false;
        }
    }
    if ( cmd.entity != entt::null ) {
        if ( !m_noteRegistry.all_of<InteractionComponent>(cmd.entity) ) {
            m_noteRegistry.emplace<InteractionComponent>(cmd.entity);
        }
        auto& ic      = m_noteRegistry.get<InteractionComponent>(cmd.entity);
        ic.isSelected = !ic.isSelected;
    }
}

void BeatmapSession::handleCommand(const CmdStartDrag& cmd)
{
    if ( cmd.entity != entt::null && m_noteRegistry.valid(cmd.entity) ) {
        m_draggedEntity = cmd.entity;
        m_dragCameraId  = cmd.cameraId;
        if ( !m_noteRegistry.all_of<InteractionComponent>(cmd.entity) ) {
            m_noteRegistry.emplace<InteractionComponent>(cmd.entity);
        }
        m_noteRegistry.get<InteractionComponent>(cmd.entity).isDragging = true;
    }
}

void BeatmapSession::handleCommand(const CmdUpdateDrag& cmd)
{
    if ( m_draggedEntity != entt::null &&
         m_noteRegistry.valid(m_draggedEntity) ) {
        auto it = m_cameras.find(cmd.cameraId);
        if ( it != m_cameras.end() ) {
            float judgmentLineY = it->second.viewportHeight *
                                  m_lastConfig.visual.judgeline_pos;
            auto* cache = m_timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                double currentAbsY = cache->getAbsY(m_visualTime);
                double targetAbsY  = currentAbsY + (judgmentLineY - cmd.mouseY);
                double targetTime  = cache->getTime(targetAbsY);

                std::vector<const TimelineComponent*> bpmEvents;
                auto tlView = m_timelineRegistry.view<const TimelineComponent>();
                for ( auto entity : tlView ) {
                    const auto& tl = tlView.get<const TimelineComponent>(entity);
                    if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
                        bpmEvents.push_back(&tl);
                    }
                }
                std::stable_sort(bpmEvents.begin(),
                                 bpmEvents.end(),
                                 [](const auto* a, const auto* b) {
                                     return a->m_timestamp < b->m_timestamp;
                                 });

                auto snap = getSnapResult(
                    targetTime, cmd.mouseY, it->second, m_lastConfig, bpmEvents);
                if ( snap.isSnapped ) {
                    targetTime = snap.snappedTime;
                }

                if ( auto* note =
                         m_noteRegistry.try_get<NoteComponent>(m_draggedEntity) ) {
                    note->m_timestamp = targetTime;
                    float leftX       = it->second.viewportWidth *
                                  m_lastConfig.visual.trackLayout.left;
                    float rightX = it->second.viewportWidth *
                                   m_lastConfig.visual.trackLayout.right;
                    float trackAreaW = rightX - leftX;
                    float noteW      = trackAreaW / m_trackCount;
                    int   track      = static_cast<int>(std::round(
                        (cmd.mouseX - leftX - noteW / 2.0f) / noteW));
                    track            = std::clamp(track, 0, m_trackCount - 1);
                    note->m_trackIndex = track;

                    if ( auto* trans = m_noteRegistry.try_get<TransformComponent>(
                             m_draggedEntity) ) {
                        trans->m_pos.x = leftX + track * noteW;
                    }
                }
            }
        }
    }
}

void BeatmapSession::handleCommand(const CmdEndDrag& cmd)
{
    if ( m_draggedEntity != entt::null &&
         m_noteRegistry.valid(m_draggedEntity) ) {
        if ( m_noteRegistry.all_of<InteractionComponent>(m_draggedEntity) ) {
            m_noteRegistry.get<InteractionComponent>(m_draggedEntity)
                .isDragging = false;
        }
        rebuildHitEvents();
    }
    m_draggedEntity = entt::null;
}

void BeatmapSession::handleCommand(const CmdSetMousePosition& cmd)
{
    bool canUpdate = false;
    if ( cmd.isHovering ) {
        if ( !m_isDragging || m_mouseCameraId == cmd.cameraId ||
             m_mouseCameraId == "" ) {
            canUpdate = true;
        }
    } else if ( m_isDragging && m_mouseCameraId == cmd.cameraId ) {
        canUpdate = true;
    }

    if ( canUpdate ) {
        m_mouseCameraId   = cmd.cameraId;
        m_mouseX          = cmd.mouseX;
        m_mouseY          = cmd.mouseY;
        m_isMouseInCanvas = cmd.isHovering;
        m_isDragging      = cmd.isDragging;

        if ( m_mouseCameraId == "Preview" ) {
            auto* cache = m_timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                auto itP = m_cameras.find("Preview");
                if ( itP != m_cameras.end() ) {
                    const auto& camera        = itP->second;
                    float       judgmentLineY = camera.viewportHeight *
                                  m_lastConfig.visual.judgeline_pos;
                    double currentAbsY = cache->getAbsY(m_visualTime);
                    double deltaY      = (judgmentLineY - m_mouseY);

                    float mainHeight = 1000.0f;
                    auto  itMain     = m_cameras.find("Basic2DCanvas");
                    if ( itMain != m_cameras.end() ) {
                        mainHeight = itMain->second.viewportHeight;
                    }

                    float mainEffectiveH = (m_lastConfig.visual.trackLayout.bottom -
                                            m_lastConfig.visual.trackLayout.top) *
                                           mainHeight;
                    float previewDrawH = camera.viewportHeight -
                                         (m_lastConfig.visual.previewConfig.margin
                                              .top +
                                          m_lastConfig.visual.previewConfig.margin
                                              .bottom);
                    float renderScaleY =
                        previewDrawH /
                        (mainEffectiveH *
                         m_lastConfig.visual.previewConfig.areaRatio);

                    if ( std::abs(renderScaleY) > 0.0001f ) {
                        deltaY /= renderScaleY;
                    }

                    double targetAbsY  = currentAbsY + deltaY;
                    m_previewHoverTime = cache->getTime(targetAbsY);

                    float topM      = m_lastConfig.visual.previewConfig.margin.top;
                    float bottomM   = m_lastConfig.visual.previewConfig.margin.bottom;
                    float viewH     = camera.viewportHeight;
                    float threshold = 60.0f;

                    m_previewEdgeScrollVelocity = 0.0;
                    if ( m_isDragging ) {
                        float sensitivity =
                            0.1f *
                            m_lastConfig.visual.previewConfig.edgeScrollSensitivity;
                        if ( m_mouseY < topM + threshold ) {
                            float dist = (topM + threshold) - m_mouseY;
                            m_previewEdgeScrollVelocity =
                                static_cast<double>(dist) * sensitivity;
                        } else if ( m_mouseY > viewH - bottomM - threshold ) {
                            float dist = m_mouseY - (viewH - bottomM - threshold);
                            m_previewEdgeScrollVelocity =
                                -static_cast<double>(dist) * sensitivity;
                        }
                    }
                }
            }
        } else {
            m_previewEdgeScrollVelocity = 0.0;
        }

        if ( !cmd.isHovering && !m_isDragging ) {
            m_mouseCameraId             = "";
            m_previewEdgeScrollVelocity = 0.0;
        }
    }
}

void BeatmapSession::handleCommand(const CmdUpdateTrackCount& cmd)
{
    m_trackCount = cmd.trackCount;
    if ( m_currentBeatmap ) {
        m_currentBeatmap->m_baseMapMetadata.track_count = cmd.trackCount;
    }
}

void BeatmapSession::handleCommand(const CmdChangeTool& cmd)
{
    m_currentTool = cmd.tool;
}

} // namespace MMM::Logic
