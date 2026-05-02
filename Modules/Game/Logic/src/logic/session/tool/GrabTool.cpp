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

        m_initialStates.clear();
        auto& reg = ctx.noteRegistry;

        // 检查 Primary 是否被选中
        bool isPrimarySelected = false;
        if ( reg.all_of<InteractionComponent>(cmd.entity) ) {
            isPrimarySelected = reg.get<InteractionComponent>(cmd.entity).isSelected;
        }

        if ( isPrimarySelected ) {
            // 模式 A: 拖动整个选中组
            auto view = reg.view<InteractionComponent, NoteComponent>();
            for ( auto entity : view ) {
                if ( view.get<InteractionComponent>(entity).isSelected ) {
                    m_initialStates[entity] = { view.get<NoteComponent>(entity) };
                    reg.get<InteractionComponent>(entity).isDragging = true;
                }
            }
        } else {
            // 模式 B: 只拖动当前物件
            if ( auto* note = reg.try_get<NoteComponent>(cmd.entity) ) {
                m_initialStates[cmd.entity] = { *note };
                if ( !reg.all_of<InteractionComponent>(cmd.entity) ) {
                    reg.emplace<InteractionComponent>(cmd.entity);
                }
                reg.get<InteractionComponent>(cmd.entity).isDragging = true;
            }
        }

        // 兼容旧代码 (保留主拖拽物件的初始备份)
        if ( m_initialStates.count(cmd.entity) ) {
            ctx.dragInitialNote = m_initialStates[cmd.entity].note;
        }
    }
}

void GrabTool::handleUpdateDrag(SessionContext& ctx, const CmdUpdateDrag& cmd)
{
    if ( ctx.draggedEntity == entt::null || m_initialStates.empty() ) return;
    if ( m_initialStates.find(ctx.draggedEntity) == m_initialStates.end() )
        return;

    auto it = ctx.cameras.find(cmd.cameraId);
    if ( it == ctx.cameras.end() ) return;

    // --- 1. 计算鼠标指向的目标位置 (Time, Track) ---
    float judgmentLineY =
        it->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" ) {
        auto  itMain             = ctx.cameras.find("Basic2DCanvas");
        float mainViewportHeight = itMain != ctx.cameras.end()
                                       ? itMain->second.viewportHeight
                                       : it->second.viewportHeight;
        float mainEffectiveH = (ctx.lastConfig.visual.trackLayout.bottom -
                                ctx.lastConfig.visual.trackLayout.top) *
                               mainViewportHeight;
        float ty           = ctx.lastConfig.visual.previewConfig.margin.top;
        float by           = it->second.viewportHeight -
                   ctx.lastConfig.visual.previewConfig.margin.bottom;
        float previewDrawH = by - ty;
        renderScaleY       = previewDrawH /
                       (mainEffectiveH *
                        ctx.lastConfig.visual.previewConfig.areaRatio);
    }

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;

    double currentAbsY = cache->getAbsY(ctx.visualTime);
    double targetAbsY =
        currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
    double targetTime = cache->getTime(targetAbsY);

    // 磁吸处理
    std::vector<const TimelineComponent*> bpmEvents;
    auto tlView = ctx.timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        const auto& tl = tlView.get<const TimelineComponent>(entity);
        if ( tl.m_effect == ::MMM::TimingEffect::BPM ) bpmEvents.push_back(&tl);
    }
    std::stable_sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const auto* a, const auto* b) { return a->m_timestamp < b->m_timestamp; });

    auto snap = SessionUtils::getSnapResult(targetTime,
                                            cmd.mouseY,
                                            it->second,
                                            ctx.lastConfig,
                                            bpmEvents,
                                            ctx.timelineRegistry,
                                            ctx.visualTime,
                                            ctx.cameras);
    if ( snap.isSnapped && !cmd.isCtrlDown ) {
        targetTime = snap.snappedTime;
    }

    float leftX =
        it->second.viewportWidth * ctx.lastConfig.visual.trackLayout.left;
    float rightX =
        it->second.viewportWidth * ctx.lastConfig.visual.trackLayout.right;
    float trackAreaW   = rightX - leftX;
    float singleTrackW = trackAreaW / static_cast<float>(ctx.trackCount);
    int   targetTrack  = static_cast<int>(
        std::floor((cmd.mouseX - leftX) / singleTrackW));
    targetTrack = std::clamp(targetTrack, 0, ctx.trackCount - 1);

    // --- 2. 计算 Primary 物件的增量 ---
    const auto& primaryInitial = m_initialStates[ctx.draggedEntity].note;
    double      deltaT         = targetTime - primaryInitial.m_timestamp;
    int         deltaTrack     = targetTrack - primaryInitial.m_trackIndex;

    // --- 3. 限制增量，确保所有物件合法 ---
    // 注意：如果是拖拽特殊部位 (HoldEnd, FlickArrow)，不执行多选拖拽
    bool isMultiDrag = (ctx.draggedPart == HoverPart::None ||
                        ctx.draggedPart == HoverPart::Head ||
                        ctx.draggedPart == HoverPart::HoldBody);

    if ( isMultiDrag && m_initialStates.size() > 1 ) {
        for ( const auto& [entity, state] : m_initialStates ) {
            const auto& n = state.note;
            // 时间限制 (timestamp >= 0)
            if ( n.m_timestamp + deltaT < 0.0 ) {
                deltaT = -n.m_timestamp;
            }
            // 轨道限制 (考虑 Flick 的范围)
            int trackL = n.m_trackIndex;
            int trackR = n.m_trackIndex;
            if ( n.m_type == ::MMM::NoteType::FLICK ) {
                if ( n.m_dtrack > 0 )
                    trackR += n.m_dtrack;
                else
                    trackL += n.m_dtrack;
            }
            if ( trackL + deltaTrack < 0 ) {
                deltaTrack = -trackL;
            }
            if ( trackR + deltaTrack >= ctx.trackCount ) {
                deltaTrack = ctx.trackCount - 1 - trackR;
            }
        }
    }

    // --- 4. 应用变更 ---
    if ( isMultiDrag ) {
        for ( auto& [entity, state] : m_initialStates ) {
            if ( auto* note = ctx.noteRegistry.try_get<NoteComponent>(entity) ) {
                note->m_timestamp  = state.note.m_timestamp + deltaT;
                note->m_trackIndex = state.note.m_trackIndex + deltaTrack;

                // 同步 Transform (简易同步)
                if ( auto* trans = ctx.noteRegistry.try_get<TransformComponent>(entity) ) {
                    trans->m_pos.x = leftX + note->m_trackIndex * singleTrackW;
                }
            }
        }
    } else {
        // 单个物件特殊部位拖拽 (维持原逻辑)
        if ( auto* note = ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity) ) {
            if ( note->m_type == ::MMM::NoteType::HOLD &&
                 ctx.draggedPart == HoverPart::HoldEnd ) {
                note->m_duration = std::max(0.0, targetTime - note->m_timestamp);
            } else if ( note->m_type == ::MMM::NoteType::FLICK &&
                        ctx.draggedPart == HoverPart::FlickArrow ) {
                note->m_dtrack = targetTrack - note->m_trackIndex;
                if ( note->m_dtrack == 0 ) {
                    note->m_dtrack = (cmd.mouseX > leftX + note->m_trackIndex * singleTrackW +
                                                       singleTrackW / 2.0f)
                                         ? 1
                                         : -1;
                }
                note->m_dtrack = std::clamp(note->m_dtrack, -note->m_trackIndex,
                                            ctx.trackCount - 1 - note->m_trackIndex);
            }
        }
    }
}

void GrabTool::handleEndDrag(SessionContext& ctx, const CmdEndDrag& cmd)
{
    if ( ctx.draggedEntity == entt::null ) return;

    std::vector<BatchNoteAction::Entry> entries;

    for ( auto& [entity, state] : m_initialStates ) {
        if ( ctx.noteRegistry.valid(entity) ) {
            if ( ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
                ctx.noteRegistry.get<InteractionComponent>(entity).isDragging =
                    false;
            }

            auto* currentNote = ctx.noteRegistry.try_get<NoteComponent>(entity);
            if ( currentNote ) {
                // 提交变更
                entries.push_back({ entity, state.note, *currentNote });
            }
        }
    }

    if ( !entries.empty() ) {
        auto action = std::make_unique<BatchNoteAction>(std::move(entries));
        ctx.actionStack.pushAndExecute(std::move(action), ctx);
    }

    SessionUtils::rebuildHitEvents(ctx);

    ctx.draggedEntity   = entt::null;
    ctx.dragInitialNote = std::nullopt;
    m_initialStates.clear();
}

}  // namespace MMM::Logic
