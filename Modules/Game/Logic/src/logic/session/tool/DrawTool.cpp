#include "logic/session/tool/DrawTool.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <unordered_set>

namespace MMM::Logic
{

void DrawTool::handleStartBrush(SessionContext& ctx, const CmdStartBrush& cmd)
{
    auto itCamera = ctx.cameras.find(cmd.cameraId);
    if ( itCamera == ctx.cameras.end() ) return;

    // 计算轨道边界
    float leftX =
        itCamera->second.viewportWidth * ctx.lastConfig.visual.trackLayout.left;
    float rightX = itCamera->second.viewportWidth *
                   ctx.lastConfig.visual.trackLayout.right;
    if ( cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas" ) {
        leftX  = ctx.lastConfig.visual.previewConfig.margin.left;
        rightX = itCamera->second.viewportWidth -
                 ctx.lastConfig.visual.previewConfig.margin.right;
    }

    // 在轨道外点击，忽略并防止进入绘制状态
    if ( cmd.mouseX < leftX || cmd.mouseX > rightX ) {
        return;
    }

    ctx.brushState.isActive = true;
    ctx.brushState.duration = 0.0;
    ctx.brushState.dtrack   = 0;

    // 获取 BPM 事件供磁吸使用
    std::vector<const TimelineComponent*> bpmEvents;
    auto tlView = ctx.timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        const auto& tl = tlView.get<const TimelineComponent>(entity);
        if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
            bpmEvents.push_back(&tl);
        }
    }
    std::stable_sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const TimelineComponent* a, const TimelineComponent* b) {
            return a->m_timestamp < b->m_timestamp;
        });

    // 计算逻辑时间
    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) {
        ctx.brushState.isActive = false;
        return;
    }

    float judgmentLineY =
        itCamera->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    double currentAbsY = cache->getAbsY(ctx.visualTime);
    double deltaY      = (judgmentLineY - cmd.mouseY);

    // 处理预览区缩放
    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas" ) {
        auto  itMain             = ctx.cameras.find("Basic2DCanvas");
        float mainViewportHeight = itMain != ctx.cameras.end()
                                       ? itMain->second.viewportHeight
                                       : itCamera->second.viewportHeight;
        float mainEffectiveH     = (ctx.lastConfig.visual.trackLayout.bottom -
                                    ctx.lastConfig.visual.trackLayout.top) *
                                   mainViewportHeight;
        float previewDrawH =
            itCamera->second.viewportHeight -
            (ctx.lastConfig.visual.previewConfig.margin.top +
             ctx.lastConfig.visual.previewConfig.margin.bottom);
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
    }
    deltaY /= renderScaleY;

    double rawTime = cache->getTime(currentAbsY + deltaY);

    auto snap = SessionUtils::getSnapResult(rawTime,
                                            cmd.mouseY,
                                            itCamera->second,
                                            ctx.lastConfig,
                                            bpmEvents,
                                            ctx.timelineRegistry,
                                            ctx.visualTime,
                                            ctx.cameras);

    ctx.brushState.time = snap.isSnapped ? snap.snappedTime : rawTime;
    float trackAreaW   = rightX - leftX;
    float singleTrackW = trackAreaW / static_cast<float>(ctx.trackCount);
    int   track =
        static_cast<int>(std::floor((cmd.mouseX - leftX) / singleTrackW));
    ctx.brushState.track = std::clamp(track, 0, ctx.trackCount - 1);

    if ( cmd.isShiftDown ) {
        ctx.brushState.type          = ::MMM::NoteType::NOTE;  // 初始为 Note
        ctx.brushState.holdStartTime = ctx.brushState.time;
        ctx.brushState.startTrack    = ctx.brushState.track;
        ctx.brushState.startMouseY   = cmd.mouseY;
    } else {
        ctx.brushState.type = ::MMM::NoteType::NOTE;
    }
}

void DrawTool::handleUpdateBrush(SessionContext& ctx, const CmdUpdateBrush& cmd)
{
    if ( !ctx.brushState.isActive ) return;

    // 同 StartBrush 的逻辑
    auto itCamera = ctx.cameras.find(cmd.cameraId);
    if ( itCamera == ctx.cameras.end() ) return;

    std::vector<const TimelineComponent*> bpmEvents;
    auto tlView = ctx.timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        const auto& tl = tlView.get<const TimelineComponent>(entity);
        if ( tl.m_effect == ::MMM::TimingEffect::BPM ) bpmEvents.push_back(&tl);
    }
    std::stable_sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const TimelineComponent* a, const TimelineComponent* b) {
            return a->m_timestamp < b->m_timestamp;
        });

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;

    float judgmentLineY =
        itCamera->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    double currentAbsY = cache->getAbsY(ctx.visualTime);
    double deltaY      = (judgmentLineY - cmd.mouseY);

    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas" ) {
        auto  itMain             = ctx.cameras.find("Basic2DCanvas");
        float mainViewportHeight = itMain != ctx.cameras.end()
                                       ? itMain->second.viewportHeight
                                       : itCamera->second.viewportHeight;
        float mainEffectiveH     = (ctx.lastConfig.visual.trackLayout.bottom -
                                    ctx.lastConfig.visual.trackLayout.top) *
                                   mainViewportHeight;
        float previewDrawH =
            itCamera->second.viewportHeight -
            (ctx.lastConfig.visual.previewConfig.margin.top +
             ctx.lastConfig.visual.previewConfig.margin.bottom);
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
    }
    deltaY /= renderScaleY;

    double rawTime = cache->getTime(currentAbsY + deltaY);
    auto   snap    = SessionUtils::getSnapResult(rawTime,
                                                 cmd.mouseY,
                                                 itCamera->second,
                                                 ctx.lastConfig,
                                                 bpmEvents,
                                                 ctx.timelineRegistry,
                                                 ctx.visualTime,
                                                 ctx.cameras);

    double currentPosTime =
        (snap.isSnapped && !cmd.isCtrlDown) ? snap.snappedTime : rawTime;

    float leftX =
        itCamera->second.viewportWidth * ctx.lastConfig.visual.trackLayout.left;
    float rightX = itCamera->second.viewportWidth *
                   ctx.lastConfig.visual.trackLayout.right;
    if ( cmd.cameraId == "Preview" ) {
        leftX  = ctx.lastConfig.visual.previewConfig.margin.left;
        rightX = itCamera->second.viewportWidth -
                 ctx.lastConfig.visual.previewConfig.margin.right;
    }
    float trackAreaW   = rightX - leftX;
    float singleTrackW = trackAreaW / static_cast<float>(ctx.trackCount);
    int   currentTrack =
        static_cast<int>(std::floor((cmd.mouseX - leftX) / singleTrackW));
    currentTrack = std::clamp(currentTrack, 0, ctx.trackCount - 1);

    if ( cmd.isShiftDown ) {
        if ( ctx.brushState.type == ::MMM::NoteType::NOTE &&
             ctx.brushState.polylineSegments.empty() &&
             ctx.brushState.duration == 0.0 && ctx.brushState.dtrack == 0 ) {
            // 刚按下 Shift 或处于 Note 状态，锁定初始状态
            if ( ctx.brushState.holdStartTime == 0.0 ) {
                ctx.brushState.holdStartTime      = currentPosTime;
                ctx.brushState.startTrack         = currentTrack;
                ctx.brushState.startMouseY        = cmd.mouseY;
                ctx.brushState.segmentStartMouseY = cmd.mouseY;
            }
        }

        float threshold = ctx.lastConfig.visual.snapThreshold;

        // --- Polyline 状态机 ---
        if ( ctx.brushState.polylineSegments.empty() ) {
            // [Phase 1] 决定初始物件 (HOLD or FLICK)
            float diffY = std::abs(cmd.mouseY - ctx.brushState.startMouseY);
            bool timeChanged = std::abs(currentPosTime - ctx.brushState.holdStartTime) > 1e-5;

            // 如果时间未改变（停留在同一拍）且垂直拖拽极小，则判断为普通音符或滑键
            if ( !timeChanged && diffY <= 5.0f ) {
                int dtrack = currentTrack - ctx.brushState.startTrack;
                if ( dtrack != 0 && (ctx.brushState.startTrack + dtrack >= 0) &&
                     (ctx.brushState.startTrack + dtrack < ctx.trackCount) ) {
                    ctx.brushState.type   = ::MMM::NoteType::FLICK;
                    ctx.brushState.dtrack = dtrack;
                } else {
                    ctx.brushState.type   = ::MMM::NoteType::NOTE;
                    ctx.brushState.dtrack = 0;
                }
                ctx.brushState.duration = 0.0;
            } else {
                ctx.brushState.type     = ::MMM::NoteType::HOLD;
                ctx.brushState.duration = std::max(
                    0.0, currentPosTime - ctx.brushState.holdStartTime);
                ctx.brushState.dtrack = 0;
            }

            // 检查是否需要转化为 Polyline (状态转换点)
            if ( ctx.brushState.type == ::MMM::NoteType::HOLD &&
                 currentTrack != ctx.brushState.startTrack ) {
                // HOLD 偏离轨道 -> 开始 Polyline: [HOLD, FLICK]
                NoteComponent::SubNote s1{ ::MMM::NoteType::HOLD,
                                           ctx.brushState.holdStartTime,
                                           ctx.brushState.duration,
                                           ctx.brushState.startTrack,
                                           0 };
                NoteComponent::SubNote s2{ ::MMM::NoteType::FLICK,
                                           s1.timestamp + s1.duration,
                                           0.0,
                                           s1.trackIndex,
                                           currentTrack - s1.trackIndex };
                ctx.brushState.polylineSegments.push_back(s1);
                ctx.brushState.polylineSegments.push_back(s2);
                ctx.brushState.segmentStartMouseY = cmd.mouseY;
                ctx.brushState.type               = ::MMM::NoteType::POLYLINE;
            } else if ( ctx.brushState.type == ::MMM::NoteType::FLICK &&
                        (timeChanged || diffY > 5.0f) ) {
                // FLICK 垂直位移 -> 开始 Polyline: [FLICK, HOLD]
                NoteComponent::SubNote s1{ ::MMM::NoteType::FLICK,
                                           ctx.brushState.holdStartTime,
                                           0.0,
                                           ctx.brushState.startTrack,
                                           ctx.brushState.dtrack };
                NoteComponent::SubNote s2{
                    ::MMM::NoteType::HOLD,
                    s1.timestamp,
                    std::max(0.0, currentPosTime - s1.timestamp),
                    s1.trackIndex + s1.dtrack,
                    0
                };
                ctx.brushState.polylineSegments.push_back(s1);
                ctx.brushState.polylineSegments.push_back(s2);
                ctx.brushState.segmentStartMouseY = cmd.mouseY;
                ctx.brushState.type               = ::MMM::NoteType::POLYLINE;
            }

            if ( ctx.brushState.type != ::MMM::NoteType::POLYLINE ) {
                ctx.brushState.track = ctx.brushState.startTrack;
                ctx.brushState.time  = ctx.brushState.holdStartTime;
            }
        } else {
            // [Phase 2] Polyline 动态增长与退化
            auto& last = ctx.brushState.polylineSegments.back();
            if ( last.type == ::MMM::NoteType::HOLD ) {
                if ( currentPosTime <= last.timestamp ) {
                    // 回退该 Hold 段
                    ctx.brushState.polylineSegments.pop_back();
                    if ( ctx.brushState.polylineSegments.size() == 1 ) {
                        auto s = ctx.brushState.polylineSegments[0];
                        ctx.brushState.type          = s.type;
                        ctx.brushState.holdStartTime = s.timestamp;
                        ctx.brushState.startTrack    = s.trackIndex;
                        ctx.brushState.duration      = s.duration;
                        ctx.brushState.dtrack        = s.dtrack;
                        ctx.brushState.polylineSegments.clear();
                    }
                    return;
                }

                last.duration = std::max(0.0, currentPosTime - last.timestamp);
                if ( currentTrack != last.trackIndex ) {
                    // 开启新的 Flick
                    ctx.brushState.polylineSegments.push_back(
                        { ::MMM::NoteType::FLICK,
                          last.timestamp + last.duration,
                          0.0,
                          last.trackIndex,
                          currentTrack - last.trackIndex });
                    ctx.brushState.segmentStartMouseY = cmd.mouseY;
                }
            } else if ( last.type == ::MMM::NoteType::FLICK ) {
                int targetTrack = last.trackIndex + last.dtrack;
                if ( currentTrack != targetTrack ) {
                    // 正在横移：更新 dtrack，并重置垂直参考点以防止意外触发
                    // Hold
                    last.dtrack = currentTrack - last.trackIndex;
                    ctx.brushState.segmentStartMouseY = cmd.mouseY;

                    // 退化检查: 如果 Flick 回到了起始轨道
                    if ( last.dtrack == 0 ) {
                        ctx.brushState.polylineSegments.pop_back();
                        if ( ctx.brushState.polylineSegments.size() == 1 ) {
                            auto s = ctx.brushState.polylineSegments[0];
                            ctx.brushState.type          = s.type;
                            ctx.brushState.holdStartTime = s.timestamp;
                            ctx.brushState.startTrack    = s.trackIndex;
                            ctx.brushState.duration      = s.duration;
                            ctx.brushState.dtrack        = s.dtrack;
                            ctx.brushState.polylineSegments.clear();
                        }
                    }
                } else {
                    // 轨道稳定：检查垂直移动
                    float diffYLocal = std::abs(
                        cmd.mouseY - ctx.brushState.segmentStartMouseY);
                    bool timeChangedLocal = std::abs(currentPosTime - last.timestamp) > 1e-5;

                    if ( timeChangedLocal || diffYLocal > 5.0f ) {
                        // 开启新的 Hold
                        ctx.brushState.polylineSegments.push_back(
                            { ::MMM::NoteType::HOLD,
                              last.timestamp,
                              0.0,
                              last.trackIndex + last.dtrack,
                              0 });
                        ctx.brushState.segmentStartMouseY = cmd.mouseY;
                    }
                }
            }
        }
    } else {
        // 切换回 Note，重置锁定状态
        ctx.brushState.type          = ::MMM::NoteType::NOTE;
        ctx.brushState.time          = currentPosTime;
        ctx.brushState.track         = currentTrack;
        ctx.brushState.duration      = 0.0;
        ctx.brushState.dtrack        = 0;
        ctx.brushState.holdStartTime = 0.0;
        ctx.brushState.polylineSegments.clear();
    }
}

void DrawTool::handleEndBrush(SessionContext& ctx, const CmdEndBrush& cmd)
{
    if ( !ctx.brushState.isActive ) return;

    // 创建正式音符
    NoteComponent note;
    note.m_timestamp  = ctx.brushState.time;
    note.m_duration   = ctx.brushState.duration;
    note.m_trackIndex = ctx.brushState.track;
    note.m_dtrack     = ctx.brushState.dtrack;
    note.m_type       = ctx.brushState.type;

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        note.m_subNotes = ctx.brushState.polylineSegments;
        if ( !note.m_subNotes.empty() ) {
            note.m_timestamp  = note.m_subNotes.front().timestamp;
            note.m_trackIndex = note.m_subNotes.front().trackIndex;
        }
    }

    auto action = std::make_unique<NoteAction>(
        NoteAction::Type::Create, entt::null, std::nullopt, note);
    ctx.actionStack.pushAndExecute(std::move(action), ctx);

    // 重置状态
    ctx.brushState.polylineSegments.clear();
    ctx.brushState.holdStartTime = 0.0;
    ctx.brushState.duration      = 0.0;
    ctx.brushState.dtrack        = 0;
    ctx.brushState.isActive      = false;
}

void DrawTool::handleStartErase(SessionContext& ctx, const CmdStartErase& cmd)
{
    ctx.eraserState.isActive = true;
    ctx.eraserState.targetEntities.clear();
    if ( ctx.hoveredEntity != entt::null ) {
        ctx.eraserState.targetEntities.insert(ctx.hoveredEntity);
    }
}

void DrawTool::handleUpdateErase(SessionContext& ctx, const CmdUpdateErase& cmd)
{
    if ( !ctx.eraserState.isActive ) return;

    // 每帧只标记当前鼠标正下方的物件，移开就取消
    ctx.eraserState.targetEntities.clear();
    if ( ctx.hoveredEntity != entt::null ) {
        ctx.eraserState.targetEntities.insert(ctx.hoveredEntity);
    }
}

void DrawTool::handleEndErase(SessionContext& ctx, const CmdEndErase& cmd)
{
    if ( !ctx.eraserState.isActive ) return;

    if ( !ctx.eraserState.targetEntities.empty() ) {
        std::vector<BatchNoteAction::Entry> entries;
        bool                                targetIsSelected = false;

        // 1. 检查擦除目标中是否有物件处于选中状态
        for ( auto entity : ctx.eraserState.targetEntities ) {
            if ( ctx.noteRegistry.valid(entity) &&
                 ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
                if ( ctx.noteRegistry.get<InteractionComponent>(entity)
                         .isSelected ) {
                    targetIsSelected = true;
                    break;
                }
            }
        }

        if ( targetIsSelected ) {
            // 场景 A: 擦除的目标中包含选中物件 -> 删除所有选中的物件 +
            // 擦除的目标物件
            std::unordered_set<entt::entity> toDelete;

            // 添加所有选中的
            auto view =
                ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
            for ( auto entity : view ) {
                if ( view.get<InteractionComponent>(entity).isSelected ) {
                    toDelete.insert(entity);
                }
            }

            // 添加所有本次擦除目标的（包括未选中的）
            for ( auto entity : ctx.eraserState.targetEntities ) {
                if ( ctx.noteRegistry.valid(entity) &&
                     ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                    toDelete.insert(entity);
                }
            }

            for ( auto entity : toDelete ) {
                entries.push_back({ entity,
                                    ctx.noteRegistry.get<NoteComponent>(entity),
                                    std::nullopt });
            }
            XINFO("Eraser: Deleting all {} items (selected + targets)",
                  entries.size());
        } else {
            // 场景 B: 擦除的目标全都是未选中物件 -> 只删除这些目标物件
            for ( auto entity : ctx.eraserState.targetEntities ) {
                if ( ctx.noteRegistry.valid(entity) &&
                     ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                    entries.push_back(
                        { entity,
                          ctx.noteRegistry.get<NoteComponent>(entity),
                          std::nullopt });
                }
            }
        }

        if ( !entries.empty() ) {
            // 同时删除被擦除折线下所有子物件实体，防止孤儿子实体残留
            std::unordered_set<entt::entity> erasedEntities;
            for ( const auto& entry : entries ) {
                if ( entry.entity != entt::null &&
                     ctx.noteRegistry.valid(entry.entity) ) {
                    erasedEntities.insert(entry.entity);
                }
            }
            for ( entt::entity parentEntity : erasedEntities ) {
                if ( ctx.noteRegistry.all_of<NoteComponent>(parentEntity) ) {
                    const auto& nc =
                        ctx.noteRegistry.get<NoteComponent>(parentEntity);
                    if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                         !nc.m_subNotes.empty() ) {
                        for ( auto subEnt :
                              ctx.noteRegistry.view<NoteComponent>() ) {
                            const auto& subNC =
                                ctx.noteRegistry.get<NoteComponent>(subEnt);
                            if ( subNC.m_isSubNote &&
                                 subNC.m_parentPolyline == parentEntity ) {
                                entries.push_back(
                                    { subEnt, subNC, std::nullopt });
                            }
                        }
                    }
                }
            }

            auto action = std::make_unique<BatchNoteAction>(std::move(entries));
            ctx.actionStack.pushAndExecute(std::move(action), ctx);
        }
    }

    ctx.eraserState.isActive = false;
    ctx.eraserState.targetEntities.clear();
}

}  // namespace MMM::Logic
