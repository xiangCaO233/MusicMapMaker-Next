#include "logic/session/InteractionController.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/NoteAction.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/DrawTool.h"
#include "logic/session/tool/GrabTool.h"
#include "logic/session/tool/MarqueeTool.h"
#include "mmm/beatmap/BeatMap.h"

namespace MMM::Logic
{

InteractionController::InteractionController(SessionContext& ctx) : m_ctx(ctx)
{
    m_tools[EditTool::Move]    = std::make_unique<GrabTool>();
    m_tools[EditTool::Marquee] = std::make_unique<MarqueeTool>();
    m_tools[EditTool::Draw]    = std::make_unique<DrawTool>();
}

// --- Interaction Handlers ---

void InteractionController::handleCommand(const CmdSetHoveredEntity& cmd)
{
    if ( m_ctx.hoveredEntity != cmd.entity &&
         m_ctx.hoveredEntity != entt::null ) {
        if ( m_ctx.noteRegistry.valid(m_ctx.hoveredEntity) &&
             m_ctx.noteRegistry.all_of<InteractionComponent>(
                 m_ctx.hoveredEntity) ) {
            m_ctx.noteRegistry.get<InteractionComponent>(m_ctx.hoveredEntity)
                .isHovered = false;
            m_ctx.noteRegistry.get<InteractionComponent>(m_ctx.hoveredEntity)
                .hoveredPart = static_cast<uint8_t>(HoverPart::None);
        }
    }

    m_ctx.hoveredEntity   = cmd.entity;
    m_ctx.hoveredPart     = cmd.part;
    m_ctx.hoveredSubIndex = cmd.subIndex;

    if ( m_ctx.hoveredEntity != entt::null &&
         m_ctx.noteRegistry.valid(m_ctx.hoveredEntity) ) {
        if ( !m_ctx.noteRegistry.all_of<InteractionComponent>(
                 m_ctx.hoveredEntity) ) {
            m_ctx.noteRegistry.emplace<InteractionComponent>(
                m_ctx.hoveredEntity);
        }
        auto& ic =
            m_ctx.noteRegistry.get<InteractionComponent>(m_ctx.hoveredEntity);
        ic.isHovered       = true;
        ic.hoveredPart     = cmd.part;
        ic.hoveredSubIndex = cmd.subIndex;
    }
}

void InteractionController::handleCommand(const CmdSelectEntity& cmd)
{
    // 只有在框选工具模式下才允许修改选中状态
    if ( m_ctx.currentTool != EditTool::Marquee ) return;

    // 如果清空其他选中，则也清空当前可能的框选留存
    if ( cmd.clearOthers ) {
        m_ctx.hasMarqueeSelection = false;
        auto view = m_ctx.noteRegistry.view<InteractionComponent>();
        for ( auto entity : view ) {
            m_ctx.noteRegistry.get<InteractionComponent>(entity).isSelected =
                false;
        }
    }
    if ( cmd.entity != entt::null ) {
        if ( !m_ctx.noteRegistry.all_of<InteractionComponent>(cmd.entity) ) {
            m_ctx.noteRegistry.emplace<InteractionComponent>(cmd.entity);
        }
        auto& ic = m_ctx.noteRegistry.get<InteractionComponent>(cmd.entity);
        ic.isSelected = !ic.isSelected;
    }
}

void InteractionController::handleCommand(const CmdSelectAll& cmd)
{
    auto view = m_ctx.noteRegistry.view<NoteComponent>();
    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        if ( !m_ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
            m_ctx.noteRegistry.emplace<InteractionComponent>(entity);
        }
        m_ctx.noteRegistry.get<InteractionComponent>(entity).isSelected = true;
    }
    m_ctx.hasMarqueeSelection = false;
}

void InteractionController::handleCommand(const CmdStartDrag& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleStartDrag(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdUpdateDrag& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateDrag(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdEndDrag& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndDrag(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdSetMousePosition& cmd)
{
    bool canUpdate = false;
    if ( cmd.isHovering ) {
        if ( !m_ctx.isDragging || m_ctx.mouseCameraId == cmd.cameraId ||
             m_ctx.mouseCameraId == "" ) {
            m_ctx.mouseCameraId   = cmd.cameraId;
            m_ctx.isMouseInCanvas = true;
            canUpdate             = true;
        }
    } else if ( m_ctx.isDragging && m_ctx.mouseCameraId == cmd.cameraId ) {
        // 如果正在往外拖拽，依然允许更新坐标以便主画布跟随
        canUpdate = true;
    }

    if ( canUpdate ) {
        m_ctx.lastMousePos = { cmd.mouseX, cmd.mouseY };

        // 如果是从预览区发起的，或者正在预览区拖动，更新全局拖拽状态
        if ( cmd.cameraId == "Preview" ) {
            m_ctx.isDragging = cmd.isDragging;
        }

        // 预览区边缘滚动
        if ( cmd.cameraId == "Preview" && cmd.isDragging ) {
            auto it = m_ctx.cameras.find(cmd.cameraId);
            if ( it != m_ctx.cameras.end() ) {
                float margin = 20.0f;
                float dist   = 0.0f;
                if ( cmd.mouseY < margin )
                    dist = margin - cmd.mouseY;
                else if ( cmd.mouseY > it->second.viewportHeight - margin )
                    dist = (it->second.viewportHeight - margin) - cmd.mouseY;

                float sensitivity =
                    m_ctx.lastConfig.visual.previewConfig.edgeScrollSensitivity;
                m_ctx.previewEdgeScrollVelocity =
                    static_cast<double>(dist) * sensitivity;
            }
        } else if ( cmd.cameraId == "Preview" ) {
            m_ctx.previewEdgeScrollVelocity = 0.0;
        }
    } else {
        m_ctx.previewEdgeScrollVelocity = 0.0;
    }

    if ( !cmd.isHovering && !m_ctx.isDragging ) {
        if ( m_ctx.mouseCameraId == cmd.cameraId ) {
            m_ctx.mouseCameraId             = "";
            m_ctx.isMouseInCanvas           = false;
            m_ctx.previewEdgeScrollVelocity = 0.0;
        }
    }
}

void InteractionController::handleCommand(const CmdUpdateTrackCount& cmd)
{
    m_ctx.trackCount = cmd.trackCount;
    if ( m_ctx.currentBeatmap ) {
        m_ctx.currentBeatmap->m_baseMapMetadata.track_count = cmd.trackCount;
    }
}

void InteractionController::handleCommand(const CmdChangeTool& cmd)
{
    m_ctx.currentTool = cmd.tool;
}

void InteractionController::handleCommand(const CmdStartMarquee& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleStartMarquee(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdUpdateMarquee& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateMarquee(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdEndMarquee& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndMarquee(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdRemoveMarqueeAt& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleRemoveMarqueeAt(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdStartBrush& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_ctx.isDragging   = true;
        m_ctx.dragCameraId = cmd.cameraId;
        m_tools[m_ctx.currentTool]->handleStartBrush(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdUpdateBrush& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateBrush(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdEndBrush& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndBrush(m_ctx, cmd);
    }
    m_ctx.isDragging = false;
}

void InteractionController::handleCommand(const CmdStartErase& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_ctx.isDragging   = true;
        m_ctx.dragCameraId = cmd.cameraId;
        m_tools[m_ctx.currentTool]->handleStartErase(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdUpdateErase& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateErase(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdEndErase& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndErase(m_ctx, cmd);
    }
    m_ctx.isDragging = false;
}



void InteractionController::updateMarqueeSelection(bool forceFullSync)
{
    if ( m_ctx.marqueeBoxes.empty() ) return;

    auto mode = m_ctx.lastConfig.settings.selectionMode;
    auto view = m_ctx.noteRegistry.view<NoteComponent>();

    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        bool isSelectedInAny = false;
        for ( const auto& box : m_ctx.marqueeBoxes ) {
            double minTime  = std::min(box.startTime, box.endTime);
            double maxTime  = std::max(box.startTime, box.endTime);
            float  minTrack = std::min(box.startTrack, box.endTrack);
            float  maxTrack = std::max(box.startTrack, box.endTrack);

            bool insideThis = false;
            if ( mode == Config::SelectionMode::Strict ) {
                double noteEnd = note.m_timestamp + note.m_duration;
                float  trackL  = static_cast<float>(note.m_trackIndex);
                float  trackR  = trackL + 1.0f;
                if ( note.m_type == ::MMM::NoteType::FLICK ) {
                    if ( note.m_dtrack > 0 )
                        trackR += note.m_dtrack;
                    else
                        trackL += note.m_dtrack;
                }

                if ( note.m_timestamp >= minTime && noteEnd <= maxTime &&
                     trackL >= minTrack && trackR <= maxTrack ) {
                    insideThis = true;
                }
            } else {
                double noteEnd = note.m_timestamp + note.m_duration;
                float  trackL  = static_cast<float>(note.m_trackIndex);
                float  trackR  = trackL + 1.0f;
                if ( note.m_type == ::MMM::NoteType::FLICK ) {
                    if ( note.m_dtrack > 0 )
                        trackR += note.m_dtrack;
                    else
                        trackL += note.m_dtrack;
                }

                bool timeOverlap = std::max(note.m_timestamp, minTime) <=
                                   std::min(noteEnd, maxTime);
                bool trackOverlap =
                    std::max(trackL, minTrack) <= std::min(trackR, maxTrack);
                if ( timeOverlap && trackOverlap ) {
                    insideThis = true;
                }
            }

            if ( insideThis ) {
                isSelectedInAny = true;
                break;
            }
        }

        if ( !m_ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
            m_ctx.noteRegistry.emplace<InteractionComponent>(entity);
        }

        auto& ic = m_ctx.noteRegistry.get<InteractionComponent>(entity);
        if ( m_ctx.marqueeIsAdditive && !forceFullSync ) {
            if ( isSelectedInAny ) {
                ic.isSelected = true;
            }
        } else {
            ic.isSelected = isSelectedInAny;
        }
    }
}

}  // namespace MMM::Logic
