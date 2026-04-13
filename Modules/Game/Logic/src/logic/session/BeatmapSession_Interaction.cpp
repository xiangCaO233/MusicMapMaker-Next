#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/NoteAction.h"
#include "mmm/beatmap/BeatMap.h"

namespace MMM::Logic
{

// --- Interaction Handlers ---

void BeatmapSession::handleCommand(const CmdSetHoveredEntity& cmd)
{
    if ( m_hoveredEntity != cmd.entity && m_hoveredEntity != entt::null ) {
        if ( m_noteRegistry.valid(m_hoveredEntity) && m_noteRegistry.all_of<InteractionComponent>(m_hoveredEntity) ) {
            m_noteRegistry.get<InteractionComponent>(m_hoveredEntity).isHovered = false;
            m_noteRegistry.get<InteractionComponent>(m_hoveredEntity).hoveredPart = static_cast<uint8_t>(HoverPart::None);
        }
    }

    m_hoveredEntity = cmd.entity;
    m_hoveredPart   = cmd.part;

    if ( m_hoveredEntity != entt::null && m_noteRegistry.valid(m_hoveredEntity) ) {
        if ( !m_noteRegistry.all_of<InteractionComponent>(m_hoveredEntity) ) {
            m_noteRegistry.emplace<InteractionComponent>(m_hoveredEntity);
        }
        auto& ic = m_noteRegistry.get<InteractionComponent>(m_hoveredEntity);
        ic.isHovered = true;
        ic.hoveredPart = cmd.part;
    }
}

void BeatmapSession::handleCommand(const CmdSelectEntity& cmd)
{
    // 如果清空其他选中，则也清空当前可能的框选留存
    if ( cmd.clearOthers ) {
        m_hasMarqueeSelection = false;
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

        if ( auto* note = m_noteRegistry.try_get<NoteComponent>(cmd.entity) ) {
            m_dragInitialNote = *note;
        }
    }
}

void BeatmapSession::handleCommand(const CmdUpdateDrag& cmd)
{
    if ( m_draggedEntity != entt::null &&
         m_noteRegistry.valid(m_draggedEntity) ) {
        auto it = m_cameras.find(cmd.cameraId);
        if ( it != m_cameras.end() ) {
            float judgmentLineY =
                it->second.viewportHeight * m_lastConfig.visual.judgeline_pos;
            auto* cache = m_timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                double currentAbsY = cache->getAbsY(m_visualTime);
                double targetAbsY  = currentAbsY + (judgmentLineY - cmd.mouseY);
                double targetTime  = cache->getTime(targetAbsY);

                std::vector<const TimelineComponent*> bpmEvents;
                auto                                  tlView =
                    m_timelineRegistry.view<const TimelineComponent>();
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

                auto snap = getSnapResult(targetTime,
                                          cmd.mouseY,
                                          it->second,
                                          m_lastConfig,
                                          bpmEvents);
                if ( snap.isSnapped ) {
                    targetTime = snap.snappedTime;
                }

                if ( auto* note = m_noteRegistry.try_get<NoteComponent>(
                         m_draggedEntity) ) {
                    note->m_timestamp  = targetTime;
                    float leftX        = it->second.viewportWidth *
                                         m_lastConfig.visual.trackLayout.left;
                    float rightX       = it->second.viewportWidth *
                                         m_lastConfig.visual.trackLayout.right;
                    float trackAreaW   = rightX - leftX;
                    float noteW        = trackAreaW / m_trackCount;
                    int   track        = static_cast<int>(std::round(
                        (cmd.mouseX - leftX - noteW / 2.0f) / noteW));
                    track              = std::clamp(track, 0, m_trackCount - 1);
                    note->m_trackIndex = track;

                    if ( auto* trans =
                             m_noteRegistry.try_get<TransformComponent>(
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

        // 提交撤销操作
        if ( m_dragInitialNote.has_value() ) {
            auto* currentNote =
                m_noteRegistry.try_get<NoteComponent>(m_draggedEntity);
            if ( currentNote ) {
                auto action =
                    std::make_unique<NoteAction>(NoteAction::Type::Update,
                                                 m_draggedEntity,
                                                 *m_dragInitialNote,
                                                 *currentNote);
                m_actionStack.pushAndExecute(std::move(action), *this);
            }
        }

        rebuildHitEvents();
    }
    m_draggedEntity   = entt::null;
    m_dragInitialNote = std::nullopt;
}

void BeatmapSession::handleCommand(const CmdSetMousePosition& cmd)
{
    bool canUpdate = false;
    if ( cmd.isHovering ) {
        if ( !m_isDragging || m_mouseCameraId == cmd.cameraId ||
             m_mouseCameraId == "" ) {
            m_mouseCameraId   = cmd.cameraId;
            m_isMouseInCanvas = true;
            canUpdate         = true;
        }
    } else if ( m_isDragging && m_mouseCameraId == cmd.cameraId ) {
        // 如果正在往外拖拽，依然允许更新坐标以便主画布跟随
        canUpdate = true;
    }

    if ( canUpdate ) {
        m_lastMousePos = { cmd.mouseX, cmd.mouseY };
        
        // 如果是从预览区发起的，或者正在预览区拖动，更新全局拖拽状态
        if ( cmd.cameraId == "Preview" ) {
             m_isDragging = cmd.isDragging;
        }

        // 预览区边缘滚动
        if ( cmd.cameraId == "Preview" ) {
            auto it = m_cameras.find(cmd.cameraId);
            if ( it != m_cameras.end() ) {
                float margin = 20.0f;
                float dist   = 0.0f;
                if ( cmd.mouseY < margin )
                    dist = margin - cmd.mouseY;
                else if ( cmd.mouseY > it->second.viewportHeight - margin )
                    dist = (it->second.viewportHeight - margin) - cmd.mouseY;

                float sensitivity           = m_lastConfig.visual.previewConfig
                                        .edgeScrollSensitivity;
                m_previewEdgeScrollVelocity =
                    static_cast<double>(dist) * sensitivity;
            }
        }
    } else {
        m_previewEdgeScrollVelocity = 0.0;
    }

    if ( !cmd.isHovering && !m_isDragging ) {
        if ( m_mouseCameraId == cmd.cameraId ) {
            m_mouseCameraId             = "";
            m_isMouseInCanvas           = false;
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

void BeatmapSession::handleCommand(const CmdStartMarquee& cmd)
{
    m_isSelecting         = true;
    m_hasMarqueeSelection = true;
    m_selectionCameraId   = cmd.cameraId;
    auto* cache         = m_timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        auto it = m_cameras.find(cmd.cameraId);
        if ( it != m_cameras.end() ) {
            float judgmentLineY =
                it->second.viewportHeight * m_lastConfig.visual.judgeline_pos;

            float renderScaleY = 1.0f;
            if ( cmd.cameraId == "Preview" ) {
                auto  itMain             = m_cameras.find("Basic2DCanvas");
                float mainViewportHeight = itMain != m_cameras.end()
                                               ? itMain->second.viewportHeight
                                               : it->second.viewportHeight;

                float mainEffectiveH = (m_lastConfig.visual.trackLayout.bottom -
                                        m_lastConfig.visual.trackLayout.top) *
                                       mainViewportHeight;
                float ty = m_lastConfig.visual.previewConfig.margin.top;
                float by = it->second.viewportHeight -
                           m_lastConfig.visual.previewConfig.margin.bottom;
                float previewDrawH = by - ty;

                renderScaleY = previewDrawH / (mainEffectiveH *
                                               m_lastConfig.visual.previewConfig.areaRatio);
            } else {
                renderScaleY = m_lastConfig.visual.noteScaleY;
            }

            double currentAbsY   = cache->getAbsY(m_visualTime);
            double targetAbsY    = currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
            m_selectionStartTime = cache->getTime(targetAbsY);
            m_selectionEndTime   = m_selectionStartTime;

            float leftX =
                it->second.viewportWidth * m_lastConfig.visual.trackLayout.left;
            float rightX     = it->second.viewportWidth *
                               m_lastConfig.visual.trackLayout.right;
            float trackAreaW = rightX - leftX;
            m_selectionStartTrack =
                (cmd.mouseX - leftX) / (trackAreaW / m_trackCount);
            m_selectionEndTrack = m_selectionStartTrack;
        }
    }
}

void BeatmapSession::handleCommand(const CmdUpdateMarquee& cmd)
{
    if ( !m_isSelecting ) return;
    auto* cache = m_timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        auto it = m_cameras.find(m_selectionCameraId);
        if ( it != m_cameras.end() ) {
            float judgmentLineY =
                it->second.viewportHeight * m_lastConfig.visual.judgeline_pos;

            float renderScaleY = 1.0f;
            if ( m_selectionCameraId == "Preview" ) {
                auto  itMain             = m_cameras.find("Basic2DCanvas");
                float mainViewportHeight = itMain != m_cameras.end()
                                               ? itMain->second.viewportHeight
                                               : it->second.viewportHeight;

                float mainEffectiveH = (m_lastConfig.visual.trackLayout.bottom -
                                        m_lastConfig.visual.trackLayout.top) *
                                       mainViewportHeight;
                float ty = m_lastConfig.visual.previewConfig.margin.top;
                float by = it->second.viewportHeight -
                           m_lastConfig.visual.previewConfig.margin.bottom;
                float previewDrawH = by - ty;

                renderScaleY = previewDrawH / (mainEffectiveH *
                                               m_lastConfig.visual.previewConfig.areaRatio);
            } else {
                renderScaleY = m_lastConfig.visual.noteScaleY;
            }

            double currentAbsY = cache->getAbsY(m_visualTime);
            double targetAbsY  = currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
            m_selectionEndTime = cache->getTime(targetAbsY);

            float leftX =
                it->second.viewportWidth * m_lastConfig.visual.trackLayout.left;
            float rightX     = it->second.viewportWidth *
                               m_lastConfig.visual.trackLayout.right;
            float trackAreaW = rightX - leftX;
            m_selectionEndTrack =
                (cmd.mouseX - leftX) / (trackAreaW / m_trackCount);
        }
    }
}

void BeatmapSession::handleCommand(const CmdEndMarquee& cmd)
{
    m_isSelecting = false;
}

}  // namespace MMM::Logic
