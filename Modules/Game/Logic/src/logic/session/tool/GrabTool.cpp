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
    m_isPolylineSubDrag = false;

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
            isPrimarySelected =
                reg.get<InteractionComponent>(cmd.entity).isSelected;
        }

        if ( isPrimarySelected ) {
            // 模式 A: 拖动整个选中组
            auto view = reg.view<InteractionComponent, NoteComponent>();
            for ( auto entity : view ) {
                if ( view.get<InteractionComponent>(entity).isSelected ) {
                    m_initialStates[entity] = { view.get<NoteComponent>(
                        entity) };
                    reg.get<InteractionComponent>(entity).isDragging = true;
                }
            }

            // 额外收集所有选中 Polyline 的子物件实体
            // (确保它们跟随移动并能正确提交 Action)
            for ( auto entity : reg.view<NoteComponent>() ) {
                const auto& nc = reg.get<NoteComponent>(entity);
                if ( nc.m_isSubNote &&
                     m_initialStates.count(nc.m_parentPolyline) ) {
                    m_initialStates[entity] = { nc };
                    if ( reg.all_of<InteractionComponent>(entity) ) {
                        reg.get<InteractionComponent>(entity).isDragging = true;
                    }
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

                // 如果是 Polyline，也收集其子物件
                if ( note->m_type == ::MMM::NoteType::POLYLINE ) {
                    for ( auto subEnt : reg.view<NoteComponent>() ) {
                        const auto& subNC = reg.get<NoteComponent>(subEnt);
                        if ( subNC.m_isSubNote &&
                             subNC.m_parentPolyline == cmd.entity ) {
                            m_initialStates[subEnt] = { subNC };
                            if ( reg.all_of<InteractionComponent>(subEnt) ) {
                                reg.get<InteractionComponent>(subEnt)
                                    .isDragging = true;
                            }
                        }
                    }
                }
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
        const auto* mainCamera =
            SessionUtils::findMainCanvasCamera(ctx.cameras);
        float mainViewportHeight =
            mainCamera ? mainCamera->viewportHeight : it->second.viewportHeight;
        float mainEffectiveH = (ctx.lastConfig.visual.trackLayout.bottom -
                                ctx.lastConfig.visual.trackLayout.top) *
                               mainViewportHeight;
        float ty = ctx.lastConfig.visual.previewConfig.margin.top;
        float by = it->second.viewportHeight -
                   ctx.lastConfig.visual.previewConfig.margin.bottom;
        float previewDrawH = by - ty;
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
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
        bpmEvents.begin(), bpmEvents.end(), [](const auto* a, const auto* b) {
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
    if ( snap.isSnapped && !cmd.isCtrlDown ) {
        targetTime = snap.snappedTime;
    }

    float leftX =
        it->second.viewportWidth * ctx.lastConfig.visual.trackLayout.left;
    float rightX =
        it->second.viewportWidth * ctx.lastConfig.visual.trackLayout.right;
    float trackAreaW   = rightX - leftX;
    float singleTrackW = trackAreaW / static_cast<float>(ctx.trackCount);
    int   targetTrack =
        static_cast<int>(std::floor((cmd.mouseX - leftX) / singleTrackW));
    targetTrack = std::clamp(targetTrack, 0, ctx.trackCount - 1);

    // --- 2. 计算参考点的初始位置 ---
    // 以鼠标抓取的那个点作为参考，计算位移增量
    const auto& primaryInitialState = m_initialStates[ctx.draggedEntity];
    double      refInitialTime      = primaryInitialState.note.m_timestamp;
    int         refInitialTrack     = primaryInitialState.note.m_trackIndex;

    if ( ctx.draggedPart == HoverPart::PolylineNode &&
         ctx.draggedSubIndex >= 0 &&
         ctx.draggedSubIndex <
             (int)primaryInitialState.note.m_subNotes.size() ) {
        refInitialTime =
            primaryInitialState.note.m_subNotes[ctx.draggedSubIndex].timestamp;
        refInitialTrack =
            primaryInitialState.note.m_subNotes[ctx.draggedSubIndex].trackIndex;
    }

    double deltaT     = targetTime - refInitialTime;
    int    deltaTrack = targetTrack - refInitialTrack;

    // --- 3. 限制增量，确保所有物件合法 ---
    // 注意：如果是拖拽特定部件且没有多选，则可能进入单点编辑模式
    bool isMultiDrag = (ctx.draggedPart == HoverPart::None ||
                        ctx.draggedPart == HoverPart::Head ||
                        ctx.draggedPart == HoverPart::HoldBody ||
                        ctx.draggedPart == HoverPart::PolylineNode);

    // 如果点击的是特定部件 (如 HoldEnd)，且该物件未被选中，则强制进入单点编辑
    bool isPrimarySelected = false;
    if ( ctx.noteRegistry.all_of<InteractionComponent>(ctx.draggedEntity) ) {
        isPrimarySelected =
            ctx.noteRegistry.get<InteractionComponent>(ctx.draggedEntity)
                .isSelected;
    }

    // 对于未选中的 Polyline，所有部分的拖拽均视为整体移动（多选模式）
    // 确保拖拽头部、节点、尾端等任意部位时，折线及其所有子物件整体跟随移动
    if ( !isPrimarySelected ) {
        auto* draggedNote =
            ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
        if ( draggedNote && draggedNote->m_type == ::MMM::NoteType::POLYLINE ) {
            isMultiDrag = true;
        }
    }

    if ( !isPrimarySelected ) {
        if ( ctx.draggedPart == HoverPart::HoldEnd ||
             ctx.draggedPart == HoverPart::FlickArrow ) {
            // 仅非 Polyline 实体支持单节点编辑（调节长条长度或滑键方向）
            auto* draggedNote =
                ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
            if ( !draggedNote ||
                 draggedNote->m_type != ::MMM::NoteType::POLYLINE ) {
                isMultiDrag = false;
            }
        }
    }

    // --- 折线内部子段拖拽检测 ---
    bool isPolylineSubDrag = false;
    if ( !isPrimarySelected && ctx.draggedPart == HoverPart::HoldBody ) {
        auto* draggedNote =
            ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
        if ( draggedNote && draggedNote->m_type == ::MMM::NoteType::POLYLINE &&
             ctx.draggedSubIndex > 0 &&
             ctx.draggedSubIndex < (int)(draggedNote->m_subNotes.size()) ) {
            isPolylineSubDrag = true;
        }
    }
    m_isPolylineSubDrag = isPolylineSubDrag;

    if ( isPolylineSubDrag ) {
        auto* note = ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
        if ( !note ) return;

        const auto& initState    = m_initialStates[ctx.draggedEntity];
        const auto& initSubNotes = initState.note.m_subNotes;
        int         subIdx       = ctx.draggedSubIndex;
        const auto& initSub      = initSubNotes[subIdx];

        if ( initSub.type == ::MMM::NoteType::HOLD ) {
            int localDelta = targetTrack - initSub.trackIndex;

            for ( size_t j = subIdx; j < initSubNotes.size(); ++j ) {
                int newTrack = initSubNotes[j].trackIndex + localDelta;
                if ( newTrack < 0 ) localDelta = -initSubNotes[j].trackIndex;
                if ( newTrack >= ctx.trackCount )
                    localDelta =
                        ctx.trackCount - 1 - initSubNotes[j].trackIndex;
                if ( initSubNotes[j].type == ::MMM::NoteType::FLICK ) {
                    int endTrack = newTrack + initSubNotes[j].dtrack;
                    if ( endTrack < 0 )
                        localDelta = -(initSubNotes[j].trackIndex +
                                       initSubNotes[j].dtrack);
                    if ( endTrack >= ctx.trackCount )
                        localDelta = ctx.trackCount - 1 -
                                     (initSubNotes[j].trackIndex +
                                      initSubNotes[j].dtrack);
                }
            }
            if ( subIdx > 0 &&
                 initSubNotes[subIdx - 1].type == ::MMM::NoteType::FLICK ) {
                int prevEnd = initSubNotes[subIdx - 1].trackIndex +
                              initSubNotes[subIdx - 1].dtrack + localDelta;
                if ( prevEnd < 0 )
                    localDelta = -(initSubNotes[subIdx - 1].trackIndex +
                                   initSubNotes[subIdx - 1].dtrack);
                if ( prevEnd >= ctx.trackCount )
                    localDelta = ctx.trackCount - 1 -
                                 (initSubNotes[subIdx - 1].trackIndex +
                                  initSubNotes[subIdx - 1].dtrack);
            }

            for ( size_t j = subIdx; j < note->m_subNotes.size(); ++j ) {
                note->m_subNotes[j].trackIndex =
                    initSubNotes[j].trackIndex + localDelta;
            }
            if ( subIdx > 0 &&
                 note->m_subNotes[subIdx - 1].type == ::MMM::NoteType::FLICK ) {
                note->m_subNotes[subIdx - 1].dtrack =
                    initSubNotes[subIdx - 1].dtrack + localDelta;
            }
        } else if ( initSub.type == ::MMM::NoteType::FLICK ) {
            double localDelta = targetTime - initSub.timestamp;

            if ( subIdx > 0 &&
                 initSubNotes[subIdx - 1].type == ::MMM::NoteType::HOLD ) {
                double minDt = -initSubNotes[subIdx - 1].duration;
                if ( localDelta < minDt ) localDelta = minDt;
            }
            for ( size_t j = subIdx; j < initSubNotes.size(); ++j ) {
                if ( initSubNotes[j].timestamp + localDelta < 0.0 )
                    localDelta = -initSubNotes[j].timestamp;
            }

            for ( size_t j = subIdx; j < note->m_subNotes.size(); ++j ) {
                note->m_subNotes[j].timestamp =
                    initSubNotes[j].timestamp + localDelta;
            }
            if ( subIdx > 0 &&
                 note->m_subNotes[subIdx - 1].type == ::MMM::NoteType::HOLD ) {
                note->m_subNotes[subIdx - 1].duration =
                    initSubNotes[subIdx - 1].duration + localDelta;
            }
        }

        syncPolylineSubEntities(ctx, ctx.draggedEntity, *note);

        if ( auto* trans = ctx.noteRegistry.try_get<TransformComponent>(
                 ctx.draggedEntity) ) {
            float sTrackW = (it->second.viewportWidth *
                             (ctx.lastConfig.visual.trackLayout.right -
                              ctx.lastConfig.visual.trackLayout.left)) /
                            static_cast<float>(ctx.trackCount);
            float lx = it->second.viewportWidth *
                       ctx.lastConfig.visual.trackLayout.left;
            trans->m_pos.x = lx + note->m_trackIndex * sTrackW;
        }
    } else if ( isMultiDrag ) {
        // 预检查增量限制
        for ( const auto& [entity, state] : m_initialStates ) {
            auto check =
                [&](::MMM::NoteType type, double t, int track, int dtrack) {
                    if ( t + deltaT < 0.0 ) deltaT = -t;

                    int trackL = track;
                    int trackR = track;
                    if ( type == ::MMM::NoteType::FLICK ) {
                        trackL = std::min(track, track + dtrack);
                        trackR = std::max(track, track + dtrack);
                    }

                    if ( trackL + deltaTrack < 0 ) deltaTrack = -trackL;
                    if ( trackR + deltaTrack >= ctx.trackCount )
                        deltaTrack = ctx.trackCount - 1 - trackR;
                };

            const auto& n = state.note;
            check(n.m_type, n.m_timestamp, n.m_trackIndex, n.m_dtrack);
            for ( const auto& sub : n.m_subNotes ) {
                check(sub.type, sub.timestamp, sub.trackIndex, sub.dtrack);
            }
        }

        // 应用变更
        for ( auto& [entity, state] : m_initialStates ) {
            if ( auto* note =
                     ctx.noteRegistry.try_get<NoteComponent>(entity) ) {
                note->m_timestamp  = state.note.m_timestamp + deltaT;
                note->m_trackIndex = state.note.m_trackIndex + deltaTrack;

                // 同步子物件 (Polylines 内部向量)
                for ( size_t i = 0; i < note->m_subNotes.size(); ++i ) {
                    note->m_subNotes[i].timestamp =
                        state.note.m_subNotes[i].timestamp + deltaT;
                    note->m_subNotes[i].trackIndex =
                        state.note.m_subNotes[i].trackIndex + deltaTrack;
                }

                // 更新 Transform
                if ( auto* trans = ctx.noteRegistry.try_get<TransformComponent>(
                         entity) ) {
                    trans->m_pos.x = leftX + note->m_trackIndex * singleTrackW;
                }
            }
        }
    } else {
        // 单个物件特殊部位拖拽 (维持原逻辑，并增加 PolylineNode 支持)
        if ( auto* note =
                 ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity) ) {
            if ( note->m_type == ::MMM::NoteType::HOLD &&
                 ctx.draggedPart == HoverPart::HoldEnd ) {
                note->m_duration =
                    std::max(0.0, targetTime - note->m_timestamp);
            } else if ( note->m_type == ::MMM::NoteType::FLICK &&
                        ctx.draggedPart == HoverPart::FlickArrow ) {
                note->m_dtrack = targetTrack - note->m_trackIndex;
                if ( note->m_dtrack == 0 ) {
                    note->m_dtrack =
                        (cmd.mouseX > leftX +
                                          note->m_trackIndex * singleTrackW +
                                          singleTrackW / 2.0f)
                            ? 1
                            : -1;
                }
                note->m_dtrack =
                    std::clamp(note->m_dtrack,
                               -note->m_trackIndex,
                               ctx.trackCount - 1 - note->m_trackIndex);
            } else if ( ctx.draggedPart == HoverPart::PolylineNode &&
                        ctx.draggedSubIndex >= 0 &&
                        ctx.draggedSubIndex < (int)note->m_subNotes.size() ) {
                // 拖拽单个 Polyline 节点
                auto& sub      = note->m_subNotes[ctx.draggedSubIndex];
                sub.timestamp  = std::max(0.0, targetTime);
                sub.trackIndex = targetTrack;

                // 同步更新实际的子物件实体 (如果是单点拖拽)
                auto subView = ctx.noteRegistry.view<NoteComponent>();
                for ( auto subEnt : subView ) {
                    auto& subNC = subView.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == ctx.draggedEntity &&
                         subNC.m_subIndex == ctx.draggedSubIndex ) {
                        subNC.m_timestamp  = sub.timestamp;
                        subNC.m_trackIndex = sub.trackIndex;
                        break;
                    }
                }

                // 注意：这里可能需要同步父 note 的基础位置，如果该节点是 Head
                if ( ctx.draggedSubIndex == 0 ) {
                    note->m_timestamp  = sub.timestamp;
                    note->m_trackIndex = sub.trackIndex;
                }
            }
        }
    }
}

void GrabTool::handleEndDrag(SessionContext& ctx, const CmdEndDrag& cmd)
{
    if ( ctx.draggedEntity == entt::null ) return;

    if ( m_isPolylineSubDrag && tryPolylineSubDragMerge(ctx) ) {
        m_isPolylineSubDrag = false;
        return;
    }

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

void GrabTool::syncPolylineSubEntities(SessionContext& ctx, entt::entity parent,
                                       const NoteComponent& note)
{
    auto subView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto subEnt : subView ) {
        auto& subNC = subView.get<NoteComponent>(subEnt);
        if ( !subNC.m_isSubNote || subNC.m_parentPolyline != parent ) continue;
        if ( subNC.m_subIndex < 0 ||
             subNC.m_subIndex >= (int)note.m_subNotes.size() )
            continue;

        const auto& sub      = note.m_subNotes[subNC.m_subIndex];
        subNC.m_type         = sub.type;
        subNC.m_timestamp    = sub.timestamp;
        subNC.m_duration     = sub.duration;
        subNC.m_trackIndex   = sub.trackIndex;
        subNC.m_dtrack       = sub.dtrack;
        subNC.m_metadata     = sub.metadata;
        subNC.m_customColors = sub.customColors;
    }
}

bool GrabTool::tryPolylineSubDragMerge(SessionContext& ctx)
{
    if ( ctx.draggedEntity == entt::null ) return false;

    auto* note = ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
    if ( !note || note->m_type != ::MMM::NoteType::POLYLINE ) return false;

    int subIdx  = ctx.draggedSubIndex;
    int lastIdx = (int)note->m_subNotes.size() - 1;
    if ( subIdx < 2 || subIdx >= lastIdx ) return false;

    const auto& initState    = m_initialStates[ctx.draggedEntity];
    const auto& initSubNotes = initState.note.m_subNotes;
    auto&       subNotes     = note->m_subNotes;
    const auto& initDragged  = initSubNotes[subIdx];

    bool mergeSubHold  = (initDragged.type == ::MMM::NoteType::HOLD &&
                         subNotes[subIdx - 1].dtrack == 0);
    bool mergeSubFlick = (initDragged.type == ::MMM::NoteType::FLICK &&
                          std::abs(subNotes[subIdx - 1].duration) < 0.001);

    if ( !mergeSubHold && !mergeSubFlick ) return false;

    NoteComponent parentBefore = initState.note;
    NoteComponent parentAfter  = *note;

    auto& newSubNotes = parentAfter.m_subNotes;

    if ( mergeSubHold ) {
        NoteComponent::SubNote merged;
        merged.type      = ::MMM::NoteType::HOLD;
        merged.timestamp = newSubNotes[subIdx - 2].timestamp;
        merged.duration =
            (newSubNotes[subIdx].timestamp + newSubNotes[subIdx].duration) -
            newSubNotes[subIdx - 2].timestamp;
        merged.trackIndex       = newSubNotes[subIdx - 2].trackIndex;
        merged.dtrack           = 0;
        merged.metadata         = newSubNotes[subIdx - 2].metadata;
        merged.customColors     = newSubNotes[subIdx - 2].customColors;
        newSubNotes[subIdx - 2] = merged;
    } else {
        NoteComponent::SubNote merged;
        merged.type       = ::MMM::NoteType::FLICK;
        merged.timestamp  = newSubNotes[subIdx - 2].timestamp;
        merged.trackIndex = newSubNotes[subIdx - 2].trackIndex;
        merged.dtrack =
            (newSubNotes[subIdx].trackIndex + newSubNotes[subIdx].dtrack) -
            newSubNotes[subIdx - 2].trackIndex;
        merged.duration         = 0.0;
        merged.metadata         = newSubNotes[subIdx - 2].metadata;
        merged.customColors     = newSubNotes[subIdx - 2].customColors;
        newSubNotes[subIdx - 2] = merged;
    }
    newSubNotes.erase(newSubNotes.begin() + subIdx - 1,
                      newSubNotes.begin() + subIdx + 1);

    std::vector<BatchNoteAction::Entry> entries;
    entries.push_back({ ctx.draggedEntity, parentBefore, parentAfter });

    auto subView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto subEnt : subView ) {
        auto& subNC = subView.get<NoteComponent>(subEnt);
        if ( !subNC.m_isSubNote || subNC.m_parentPolyline != ctx.draggedEntity )
            continue;

        int           oldIdx = subNC.m_subIndex;
        auto          it     = m_initialStates.find(subEnt);
        NoteComponent initNC =
            (it != m_initialStates.end()) ? it->second.note : subNC;

        if ( oldIdx == subIdx - 1 || oldIdx == subIdx ) {
            entries.push_back({ subEnt, initNC, std::nullopt });
        } else if ( oldIdx == subIdx - 2 ) {
            const auto&   merged = newSubNotes[subIdx - 2];
            NoteComponent after  = initNC;
            after.m_type         = merged.type;
            after.m_timestamp    = merged.timestamp;
            after.m_duration     = merged.duration;
            after.m_trackIndex   = merged.trackIndex;
            after.m_dtrack       = merged.dtrack;
            after.m_metadata     = merged.metadata;
            after.m_customColors = merged.customColors;
            entries.push_back({ subEnt, initNC, after });
        } else if ( oldIdx > subIdx ) {
            NoteComponent after = initNC;
            after.m_subIndex    = oldIdx - 2;
            entries.push_back({ subEnt, initNC, after });
        } else {
            entries.push_back({ subEnt, initNC, subNC });
        }
    }

    if ( !entries.empty() ) {
        auto action = std::make_unique<BatchNoteAction>(
            std::move(entries), "Polyline Sub-Drag Merge");
        ctx.actionStack.pushAndExecute(std::move(action), ctx);
    }

    SessionUtils::rebuildHitEvents(ctx);

    ctx.draggedEntity   = entt::null;
    ctx.dragInitialNote = std::nullopt;
    m_initialStates.clear();

    return true;
}

}  // namespace MMM::Logic
