#include "logic/session/tool/DrawTool.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace MMM::Logic
{

namespace
{
/// @brief 最小允许放置物件的时间戳，单位秒。
constexpr double MIN_PLACEABLE_NOTE_TIME = 0.0;

/// @brief 判断绘制工具是否允许在指定时间创建物件。
/// @param time 待检查时间戳，单位秒。
/// @return 时间有限且不早于 0 秒时返回 true。
bool isPlaceableNoteTime(double time)
{
    return std::isfinite(time) && time >= MIN_PLACEABLE_NOTE_TIME;
}

/// @brief 判断即将创建的物件及其子物件是否都位于可放置时间范围内。
/// @param note 待检查物件。
/// @return 所有时间戳均有效且不为负时返回 true。
bool isPlaceableNote(const NoteComponent& note)
{
    if ( !isPlaceableNoteTime(note.m_timestamp) ) {
        return false;
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( const auto& subNote : note.m_subNotes ) {
            if ( !isPlaceableNoteTime(subNote.timestamp) ) {
                return false;
            }
        }
    }

    return true;
}

/// @brief 清理当前绘制笔刷状态。
/// @param ctx 会话上下文引用。
void resetBrushState(SessionContext& ctx)
{
    ctx.brushState.polylineSegments.clear();
    ctx.brushState.holdStartTime = -1.0;
    ctx.brushState.duration      = 0.0;
    ctx.brushState.dtrack        = 0;
    ctx.brushState.isActive      = false;
}

/// @brief 判断批量操作条目中是否已经包含指定实体。
bool hasBatchEntryForEntity(const std::vector<BatchNoteAction::Entry>& entries,
                            entt::entity                               entity)
{
    for ( const auto& entry : entries ) {
        if ( entry.entity == entity ) {
            return true;
        }
    }
    return false;
}

/// @brief 将指定折线父实体的所有子物件删除条目追加到批量操作中。
/// @param ctx 会话上下文引用。
/// @param parentEntity 折线父实体。
/// @param entries 待追加的批量操作条目列表。
/// @warning 逻辑热路径低频分支：只在删除/合并折线时执行一次完整 note
/// ECS 遍历，禁止在画笔移动更新中调用。
void appendPolylineChildDeleteEntries(
    SessionContext& ctx, entt::entity parentEntity,
    std::vector<BatchNoteAction::Entry>& entries)
{
    auto subNoteView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto subEnt : subNoteView ) {
        const auto& subNC = subNoteView.get<NoteComponent>(subEnt);
        if ( subNC.m_isSubNote && subNC.m_parentPolyline == parentEntity &&
             !hasBatchEntryForEntity(entries, subEnt) ) {
            entries.push_back({ subEnt, subNC, std::nullopt });
        }
    }
}

/// @brief 将多个折线父实体的子物件删除条目通过一次 ECS 扫描追加到批量操作中。
/// @param ctx 会话上下文引用。
/// @param parentEntities 需要删除子物件的折线父实体集合。
/// @param entries 待追加的批量操作条目列表。
/// @warning 逻辑热路径低频分支：橡皮擦结束时最多执行一次完整 note ECS
/// 遍历，禁止在擦除拖动更新中调用。
void appendPolylineChildDeleteEntries(
    SessionContext& ctx, const std::unordered_set<entt::entity>& parentEntities,
    std::vector<BatchNoteAction::Entry>& entries)
{
    if ( parentEntities.empty() ) return;

    std::unordered_set<entt::entity> existingEntries;
    existingEntries.reserve(entries.size());
    for ( const auto& entry : entries ) {
        if ( entry.entity != entt::null ) {
            existingEntries.insert(entry.entity);
        }
    }

    auto subNoteView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto subEnt : subNoteView ) {
        const auto& subNC = subNoteView.get<NoteComponent>(subEnt);
        if ( subNC.m_isSubNote &&
             parentEntities.find(subNC.m_parentPolyline) !=
                 parentEntities.end() &&
             existingEntries.insert(subEnt).second ) {
            entries.push_back({ subEnt, subNC, std::nullopt });
        }
    }
}
}  // namespace

void DrawTool::handleStartBrush(SessionContext& ctx, const CmdStartBrush& cmd)
{
    auto itCamera = ctx.cameras.find(cmd.cameraId);
    if ( itCamera == ctx.cameras.end() ) return;

    // 计算轨道边界
    const auto projection =
        calculatePlayerTrackProjection(itCamera->second.viewportWidth,
                                       ctx.trackCount,
                                       ctx.lastConfig.visual.trackLayout.left,
                                       ctx.lastConfig.visual.trackLayout.right,
                                       itCamera->second.horizontalOffsetX);
    float leftX  = projection.leftX;
    float rightX = projection.rightX;
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
    SessionUtils::ensureBpmEvents(ctx);
    const auto& bpmEvents = ctx.bpmEvents;

    // 计算逻辑时间
    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) {
        ctx.brushState.isActive = false;
        return;
    }

    float judgmentLineY =
        itCamera->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    double currentAbsY = cache->getAbsY(ctx.animateTime);
    double deltaY      = (judgmentLineY - cmd.mouseY);

    // 处理预览区缩放
    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas" ) {
        const auto* mainCamera =
            SessionUtils::findMainCanvasCamera(ctx.cameras);
        float mainViewportHeight = mainCamera ? mainCamera->viewportHeight
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

    auto snap = SessionUtils::getSnapResult(
        rawTime,
        cmd.mouseY,
        itCamera->second,
        ctx.lastConfig,
        bpmEvents,
        ctx.timelineRegistry,
        ctx.animateTime,
        ctx.cameras,
        ctx.currentBeatmap
            ? ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : 120.0);

    ctx.brushState.time = snap.isSnapped ? snap.snappedTime : rawTime;
    if ( !isPlaceableNoteTime(ctx.brushState.time) ) {
        resetBrushState(ctx);
        return;
    }

    const float singleTrackW =
        (rightX - leftX) / static_cast<float>(ctx.trackCount);
    const int track =
        static_cast<int>(std::floor((cmd.mouseX - leftX) / singleTrackW));
    ctx.brushState.track = std::clamp(track, 0, ctx.trackCount - 1);

    bool isResuming = false;
    if ( cmd.isShiftDown && ctx.hoveredEntity != entt::null &&
         ctx.noteRegistry.valid(ctx.hoveredEntity) ) {
        const auto& note =
            ctx.noteRegistry.get<NoteComponent>(ctx.hoveredEntity);
        entt::entity targetEntity = ctx.hoveredEntity;
        if ( note.m_isSubNote && note.m_parentPolyline != entt::null ) {
            targetEntity = note.m_parentPolyline;
        }

        if ( ctx.noteRegistry.valid(targetEntity) ) {
            const auto& parentNote =
                ctx.noteRegistry.get<NoteComponent>(targetEntity);
            if ( parentNote.m_type == ::MMM::NoteType::POLYLINE &&
                 !parentNote.m_subNotes.empty() ) {
                int lastIdx =
                    static_cast<int>(parentNote.m_subNotes.size() - 1);
                if ( ctx.hoveredSubIndex == lastIdx ) {
                    // 检查是否悬停在能够继续延伸的部分：Node、Body、HoldEnd
                    // 或 FlickArrow。
                    if ( ctx.hoveredPart ==
                             static_cast<uint8_t>(HoverPart::PolylineNode) ||
                         ctx.hoveredPart ==
                             static_cast<uint8_t>(HoverPart::HoldEnd) ||
                         ctx.hoveredPart ==
                             static_cast<uint8_t>(HoverPart::FlickArrow) ||
                         ctx.hoveredPart ==
                             static_cast<uint8_t>(HoverPart::HoldBody) ) {
                        // 进入恢复编辑模式
                        ctx.brushState.isActive = true;
                        ctx.brushState.type     = ::MMM::NoteType::POLYLINE;
                        ctx.brushState.polylineSegments = parentNote.m_subNotes;

                        // 如果最后一段是 Hold，则根据当前点击位置缩短/伸长它
                        if ( ctx.brushState.polylineSegments.back().type ==
                             ::MMM::NoteType::HOLD ) {
                            double snapTime =
                                snap.isSnapped ? snap.snappedTime : rawTime;
                            auto& lastSeg =
                                ctx.brushState.polylineSegments.back();
                            lastSeg.duration =
                                std::max(0.0, snapTime - lastSeg.timestamp);
                        }

                        // 恢复笔刷状态的各个参考值，以便 handleUpdateBrush
                        // 能够平滑接续
                        ctx.brushState.holdStartTime = parentNote.m_timestamp;
                        ctx.brushState.startTrack    = parentNote.m_trackIndex;

                        // 计算起始 Y 坐标，用于退化到普通音符时的参考
                        double startAbsY =
                            cache->getAbsY(parentNote.m_timestamp);
                        ctx.brushState.startMouseY =
                            judgmentLineY -
                            static_cast<float>(startAbsY - currentAbsY) *
                                renderScaleY;

                        // 当前鼠标 Y 为新段参考点
                        ctx.brushState.segmentStartMouseY = cmd.mouseY;

                        // 删除原物件及其子物件实体 (通过
                        // BatchNoteAction，以便整体撤销)
                        std::vector<BatchNoteAction::Entry> deleteEntries;
                        deleteEntries.push_back(
                            { targetEntity, parentNote, std::nullopt });

                        appendPolylineChildDeleteEntries(
                            ctx, targetEntity, deleteEntries);

                        auto deleteAction = std::make_unique<BatchNoteAction>(
                            std::move(deleteEntries), "Resume Polyline Edit");
                        ctx.actionStack.pushAndExecute(std::move(deleteAction),
                                                       ctx);

                        // 设置拖拽状态
                        ctx.isDragging   = true;
                        ctx.dragCameraId = cmd.cameraId;
                        isResuming       = true;
                        XINFO("Resuming Polyline edit for entity {}",
                              static_cast<uint32_t>(targetEntity));
                    }
                }
            } else if ( !parentNote.m_isSubNote &&
                        (parentNote.m_type == ::MMM::NoteType::NOTE ||
                         parentNote.m_type == ::MMM::NoteType::HOLD ||
                         parentNote.m_type == ::MMM::NoteType::FLICK) ) {
                // 将普通的 Note/Hold/Flick 转换为包含单个 subNote 的
                // Polyline，并恢复编辑
                ctx.brushState.isActive = true;
                ctx.brushState.type     = ::MMM::NoteType::POLYLINE;

                NoteComponent::SubNote s;
                s.type                          = parentNote.m_type;
                s.timestamp                     = parentNote.m_timestamp;
                s.duration                      = parentNote.m_duration;
                s.trackIndex                    = parentNote.m_trackIndex;
                s.dtrack                        = parentNote.m_dtrack;
                s.metadata                      = parentNote.m_metadata;
                s.boundSound                    = parentNote.m_boundSound;
                s.customColors                  = parentNote.m_customColors;
                ctx.brushState.polylineSegments = { s };

                // 如果是 Hold，则根据当前点击位置缩短/伸长它
                if ( parentNote.m_type == ::MMM::NoteType::HOLD ) {
                    double snapTime =
                        snap.isSnapped ? snap.snappedTime : rawTime;
                    ctx.brushState.polylineSegments.back().duration =
                        std::max(0.0, snapTime - parentNote.m_timestamp);
                }

                ctx.brushState.holdStartTime = parentNote.m_timestamp;
                ctx.brushState.startTrack    = parentNote.m_trackIndex;

                double startAbsY = cache->getAbsY(parentNote.m_timestamp);
                ctx.brushState.startMouseY =
                    judgmentLineY -
                    static_cast<float>(startAbsY - currentAbsY) * renderScaleY;

                ctx.brushState.segmentStartMouseY = cmd.mouseY;

                std::vector<BatchNoteAction::Entry> deleteEntries;
                deleteEntries.push_back(
                    { targetEntity, parentNote, std::nullopt });

                auto deleteAction = std::make_unique<BatchNoteAction>(
                    std::move(deleteEntries), "Convert Note to Polyline");
                ctx.actionStack.pushAndExecute(std::move(deleteAction), ctx);

                ctx.isDragging   = true;
                ctx.dragCameraId = cmd.cameraId;
                isResuming       = true;
                XINFO(
                    "Converting ordinary note (type {}) to Polyline and "
                    "resuming edit for entity {}",
                    static_cast<int>(parentNote.m_type),
                    static_cast<uint32_t>(targetEntity));
            }
        }
    }

    if ( !isResuming ) {
        if ( cmd.isShiftDown ) {
            ctx.brushState.type = ::MMM::NoteType::NOTE;  // 初始为 Note
            ctx.brushState.holdStartTime = ctx.brushState.time;
            ctx.brushState.startTrack    = ctx.brushState.track;
            ctx.brushState.startMouseY   = cmd.mouseY;
        } else {
            ctx.brushState.type = ::MMM::NoteType::NOTE;
        }
    }
}

void DrawTool::handleUpdateBrush(SessionContext& ctx, const CmdUpdateBrush& cmd)
{
    if ( !ctx.brushState.isActive ) return;

    // 同 StartBrush 的逻辑
    auto itCamera = ctx.cameras.find(cmd.cameraId);
    if ( itCamera == ctx.cameras.end() ) return;

    SessionUtils::ensureBpmEvents(ctx);
    const auto& bpmEvents = ctx.bpmEvents;

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;

    float judgmentLineY =
        itCamera->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    double currentAbsY = cache->getAbsY(ctx.animateTime);
    double deltaY      = (judgmentLineY - cmd.mouseY);

    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas" ) {
        const auto* mainCamera =
            SessionUtils::findMainCanvasCamera(ctx.cameras);
        float mainViewportHeight = mainCamera ? mainCamera->viewportHeight
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
    auto   snap    = SessionUtils::getSnapResult(
        rawTime,
        cmd.mouseY,
        itCamera->second,
        ctx.lastConfig,
        bpmEvents,
        ctx.timelineRegistry,
        ctx.animateTime,
        ctx.cameras,
        ctx.currentBeatmap
            ? ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : 120.0);

    double currentPosTime =
        (snap.isSnapped && !cmd.isCtrlDown) ? snap.snappedTime : rawTime;
    if ( !std::isfinite(currentPosTime) ) {
        resetBrushState(ctx);
        return;
    }
    currentPosTime = std::max(MIN_PLACEABLE_NOTE_TIME, currentPosTime);

    const auto projection =
        calculatePlayerTrackProjection(itCamera->second.viewportWidth,
                                       ctx.trackCount,
                                       ctx.lastConfig.visual.trackLayout.left,
                                       ctx.lastConfig.visual.trackLayout.right,
                                       itCamera->second.horizontalOffsetX);
    float leftX  = projection.leftX;
    float rightX = projection.rightX;
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
            if ( ctx.brushState.holdStartTime < 0.0 ) {
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
            bool  timeChanged =
                std::abs(currentPosTime - ctx.brushState.holdStartTime) > 1e-5;

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
                // HOLD 偏离轨道 -> 开始 Polyline
                if ( ctx.brushState.duration > 1e-5 ) {
                    // 只有长度大于 0 才添加 Hold 段
                    NoteComponent::SubNote s1{ ::MMM::NoteType::HOLD,
                                               ctx.brushState.holdStartTime,
                                               ctx.brushState.duration,
                                               ctx.brushState.startTrack,
                                               0 };
                    ctx.brushState.polylineSegments.push_back(s1);
                }

                NoteComponent::SubNote s2{
                    ::MMM::NoteType::FLICK,
                    ctx.brushState.holdStartTime + ctx.brushState.duration,
                    0.0,
                    ctx.brushState.startTrack,
                    currentTrack - ctx.brushState.startTrack
                };
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
                    // 正在横移：更新
                    // dtrack，并重置垂直参考点以防止意外触发长按。
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
                    bool timeChangedLocal =
                        std::abs(currentPosTime - last.timestamp) > 1e-5;

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
            } else if ( last.type == ::MMM::NoteType::NOTE ) {
                float diffYLocal =
                    std::abs(cmd.mouseY - ctx.brushState.segmentStartMouseY);
                bool timeChangedLocal =
                    std::abs(currentPosTime - last.timestamp) > 1e-5;

                if ( currentTrack != last.trackIndex ) {
                    // 改变轨道 -> 变成 Flick 段
                    last.type   = ::MMM::NoteType::FLICK;
                    last.dtrack = currentTrack - last.trackIndex;
                    ctx.brushState.segmentStartMouseY = cmd.mouseY;
                } else if ( timeChangedLocal || diffYLocal > 5.0f ) {
                    // 垂直拖动 -> 变成 Hold 段
                    last.type = ::MMM::NoteType::HOLD;
                    last.duration =
                        std::max(0.0, currentPosTime - last.timestamp);
                    ctx.brushState.segmentStartMouseY = cmd.mouseY;
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
        ctx.brushState.holdStartTime = -1.0;
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
    applyNoteColorOverrides(note, ctx.brushState.customColors);

    // 折线尾部结合所需的删除条目列表 (声明在外部以便后续使用)
    std::vector<BatchNoteAction::Entry> mergeDeleteEntries;

    bool isMergeableType = (note.m_type == ::MMM::NoteType::POLYLINE ||
                            note.m_type == ::MMM::NoteType::HOLD ||
                            note.m_type == ::MMM::NoteType::FLICK);

    if ( isMergeableType ) {
        std::vector<NoteComponent::SubNote> segments;
        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            segments = ctx.brushState.polylineSegments;
        } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
            segments.push_back({ ::MMM::NoteType::HOLD,
                                 note.m_timestamp,
                                 note.m_duration,
                                 note.m_trackIndex,
                                 0 });
        } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
            segments.push_back({ ::MMM::NoteType::FLICK,
                                 note.m_timestamp,
                                 0.0,
                                 note.m_trackIndex,
                                 note.m_dtrack });
        }

        // [深度清洗与递归简化] - 仅针对折线类型进行零值清洗与合并，普通 Hold /
        // Flick 无需深度清洗
        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            bool changed = true;
            while ( changed ) {
                changed = false;

                // 1. 过滤所有“零值”段：0长度Hold 或 0位移Flick
                auto it = std::remove_if(
                    segments.begin(), segments.end(), [](const auto& s) {
                        if ( s.type == ::MMM::NoteType::HOLD )
                            return s.duration < 1e-5;
                        if ( s.type == ::MMM::NoteType::FLICK )
                            return s.dtrack == 0;
                        return false;
                    });
                if ( it != segments.end() ) {
                    segments.erase(it, segments.end());
                    changed = true;
                }

                // 2. 合并连续的同类型物件
                if ( segments.size() > 1 ) {
                    for ( size_t i = 0; i < segments.size() - 1; ) {
                        auto& curr = segments[i];
                        auto& next = segments[i + 1];

                        if ( curr.type == next.type ) {
                            if ( curr.type == ::MMM::NoteType::HOLD ) {
                                // 合并长条持续时间
                                curr.duration += next.duration;
                                segments.erase(segments.begin() + i + 1);
                                changed = true;
                                continue;  // 继续检查合并后的段
                            } else if ( curr.type == ::MMM::NoteType::FLICK ) {
                                // 合并滑键位移量
                                curr.dtrack += next.dtrack;
                                segments.erase(segments.begin() + i + 1);
                                changed = true;
                                continue;
                            }
                        }
                        i++;
                    }
                }
            }
        }

        // 3. 尾部结合检测 (清洗后仍有 >=1 段时执行)
        //    时间容差 3ms，轨道必须一致
        constexpr double MERGE_TIME_TOLERANCE = 0.003;

        if ( segments.size() >= 1 ) {
            // 计算尾部的时间和轨道
            const auto& lastSeg   = segments.back();
            double      tailTime  = 0.0;
            int         tailTrack = 0;

            if ( lastSeg.type == ::MMM::NoteType::HOLD ) {
                // subHold: 尾部 = 起始时间 + 持续时间, 轨道不变
                tailTime  = lastSeg.timestamp + lastSeg.duration;
                tailTrack = lastSeg.trackIndex;
            } else if ( lastSeg.type == ::MMM::NoteType::FLICK ) {
                // subFlick: 尾部 = 时间戳, 轨道 = 起始轨道 + dtrack
                tailTime  = lastSeg.timestamp;
                tailTrack = lastSeg.trackIndex + lastSeg.dtrack;
            }

            // 在注册表中搜索尾部位置附近的物件
            auto noteView = ctx.noteRegistry.view<NoteComponent>();
            for ( auto entity : noteView ) {
                const auto& nc = noteView.get<NoteComponent>(entity);
                if ( nc.m_isSubNote ) continue;  // 跳过子物件

                // ===== 检测普通 Note（移除） =====
                if ( nc.m_type == ::MMM::NoteType::NOTE ) {
                    if ( nc.m_trackIndex == tailTrack &&
                         std::abs(nc.m_timestamp - tailTime) <=
                             MERGE_TIME_TOLERANCE ) {
                        // 移除与尾部重叠的普通 Note
                        mergeDeleteEntries.push_back(
                            { entity, nc, std::nullopt });
                        XINFO(
                            "Note merge: removing Note at t={:.3f} "
                            "track={}",
                            nc.m_timestamp,
                            nc.m_trackIndex);
                    }
                    continue;
                }

                // ===== 【1】结合普通物件 (Flick / Hold) =====
                if ( nc.m_type == ::MMM::NoteType::FLICK ) {
                    // 目标是一个独立的 Flick
                    if ( nc.m_trackIndex == tailTrack &&
                         std::abs(nc.m_timestamp - tailTime) <=
                             MERGE_TIME_TOLERANCE ) {
                        if ( lastSeg.type == ::MMM::NoteType::HOLD ) {
                            // 【1-1 不同类型】末尾 subHold + 目标 Flick
                            // → 将 Flick 作为最后一个 seg 加入
                            NoteComponent::SubNote flickSeg;
                            flickSeg.type         = ::MMM::NoteType::FLICK;
                            flickSeg.timestamp    = tailTime;  // 修复微小时间差
                            flickSeg.duration     = 0.0;
                            flickSeg.trackIndex   = tailTrack;
                            flickSeg.dtrack       = nc.m_dtrack;
                            flickSeg.metadata     = nc.m_metadata;
                            flickSeg.boundSound   = nc.m_boundSound;
                            flickSeg.customColors = nc.m_customColors;
                            segments.push_back(flickSeg);

                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            XINFO(
                                "Note merge [1-1]: appended Flick from "
                                "entity {} as new seg",
                                static_cast<uint32_t>(entity));
                        } else if ( lastSeg.type == ::MMM::NoteType::FLICK ) {
                            // 【1-2 相同类型】末尾 subFlick + 目标 Flick
                            // → 延长最后 subFlick 的 dtrack
                            // 以当前 subFlick 的时间为准，将 Flick
                            // 的终点作为新终点
                            int flickEnd = nc.m_trackIndex + nc.m_dtrack;
                            segments.back().dtrack =
                                flickEnd - segments.back().trackIndex;

                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            XINFO(
                                "Note merge [1-2]: extended last "
                                "subFlick to Flick end track={}",
                                flickEnd);
                        }
                        continue;
                    }
                }

                if ( nc.m_type == ::MMM::NoteType::HOLD ) {
                    // 目标是一个独立的 Hold
                    if ( nc.m_trackIndex == tailTrack &&
                         std::abs(nc.m_timestamp - tailTime) <=
                             MERGE_TIME_TOLERANCE ) {
                        if ( lastSeg.type == ::MMM::NoteType::FLICK ) {
                            // 【1-1 不同类型】末尾 subFlick + 目标 Hold
                            // → 将 Hold 作为最后一个 seg 加入
                            NoteComponent::SubNote holdSeg;
                            holdSeg.type         = ::MMM::NoteType::HOLD;
                            holdSeg.timestamp    = tailTime;  // 修复微小时间差
                            holdSeg.duration     = nc.m_duration;
                            holdSeg.trackIndex   = tailTrack;
                            holdSeg.dtrack       = 0;
                            holdSeg.metadata     = nc.m_metadata;
                            holdSeg.boundSound   = nc.m_boundSound;
                            holdSeg.customColors = nc.m_customColors;
                            segments.push_back(holdSeg);

                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            XINFO(
                                "Note merge [1-1]: appended Hold from "
                                "entity {} as new seg",
                                static_cast<uint32_t>(entity));
                        } else if ( lastSeg.type == ::MMM::NoteType::HOLD ) {
                            // 【1-2 相同类型】末尾 subHold + 目标 Hold
                            // → 直接延长最后 subHold 的持续时间
                            double holdEnd = nc.m_timestamp + nc.m_duration;
                            segments.back().duration =
                                holdEnd - segments.back().timestamp;

                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            XINFO(
                                "Note merge [1-2]: extended last "
                                "subHold to Hold end t={:.3f}",
                                holdEnd);
                        }
                        continue;
                    }
                }

                // ===== 【2】结合另一个折线 =====
                if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                     !nc.m_subNotes.empty() ) {
                    const auto& targetFirst = nc.m_subNotes.front();
                    // 目标折线头部的时间和轨道
                    double targetHeadTime  = targetFirst.timestamp;
                    int    targetHeadTrack = targetFirst.trackIndex;

                    if ( targetHeadTrack == tailTrack &&
                         std::abs(targetHeadTime - tailTime) <=
                             MERGE_TIME_TOLERANCE ) {
                        if ( lastSeg.type != targetFirst.type ) {
                            // 【2-1 不同类型】直接追加目标折线的所有 seg
                            for ( size_t si = 0; si < nc.m_subNotes.size();
                                  ++si ) {
                                auto seg = nc.m_subNotes[si];
                                // 修复首段连接处的微小时间差
                                if ( si == 0 ) {
                                    seg.timestamp = tailTime;
                                }
                                segments.push_back(seg);
                            }

                            // 删除目标折线及其子物件实体
                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            appendPolylineChildDeleteEntries(
                                ctx, entity, mergeDeleteEntries);
                            XINFO(
                                "Note merge [2-1]: appended {} segs "
                                "from Polyline entity {}",
                                nc.m_subNotes.size(),
                                static_cast<uint32_t>(entity));
                        } else {
                            // 【2-2 相同类型】延长尾段，追加剩余 seg
                            if ( targetFirst.type == ::MMM::NoteType::FLICK ) {
                                // 两个 subFlick 合并：
                                // 将目标首个 subFlick 的终点替换到用户绘制的
                                // subFlick 的终点
                                int targetFirstEnd =
                                    targetFirst.trackIndex + targetFirst.dtrack;
                                segments.back().dtrack =
                                    targetFirstEnd - segments.back().trackIndex;
                                // 以用户绘制的 subFlick 时间为准（不修改时间）
                            } else if ( targetFirst.type ==
                                        ::MMM::NoteType::HOLD ) {
                                // 两个 subHold 合并：
                                // 将目标首个 subHold 的结束时间作为新结束时间
                                double targetFirstEnd = targetFirst.timestamp +
                                                        targetFirst.duration;
                                segments.back().duration =
                                    targetFirstEnd - segments.back().timestamp;
                            }

                            // 追加目标折线除首段外的所有 seg
                            for ( size_t si = 1; si < nc.m_subNotes.size();
                                  ++si ) {
                                segments.push_back(nc.m_subNotes[si]);
                            }

                            // 删除目标折线及其子物件实体
                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            appendPolylineChildDeleteEntries(
                                ctx, entity, mergeDeleteEntries);
                            XINFO(
                                "Note merge [2-2]: extended tail and "
                                "appended {} remaining segs from Polyline "
                                "entity {}",
                                nc.m_subNotes.size() - 1,
                                static_cast<uint32_t>(entity));
                        }
                        continue;
                    }
                }
            }
        }

        // 3.5. 移除折线路径上的物件 (如果设置开启)
        if ( ctx.lastConfig.settings.removeObjectsOnPolylinePath ) {
            struct Node {
                int    track;
                double time;
            };
            std::vector<Node> nodes;
            if ( !segments.empty() ) {
                nodes.push_back({ segments.front().trackIndex,
                                  segments.front().timestamp });
                for ( const auto& s : segments ) {
                    if ( s.type == ::MMM::NoteType::HOLD ) {
                        nodes.push_back(
                            { s.trackIndex, s.timestamp + s.duration });
                    } else if ( s.type == ::MMM::NoteType::FLICK ) {
                        nodes.push_back(
                            { s.trackIndex + s.dtrack, s.timestamp });
                    }
                }
            }

            std::unordered_set<entt::entity> alreadyQueued;
            for ( const auto& entry : mergeDeleteEntries ) {
                if ( entry.entity != entt::null ) {
                    alreadyQueued.insert(entry.entity);
                }
            }

            auto noteView = ctx.noteRegistry.view<NoteComponent>();
            for ( auto entity : noteView ) {
                const auto& nc = noteView.get<NoteComponent>(entity);
                if ( nc.m_isSubNote ) continue;

                if ( nc.m_type == ::MMM::NoteType::NOTE ||
                     nc.m_type == ::MMM::NoteType::HOLD ||
                     nc.m_type == ::MMM::NoteType::FLICK ) {

                    bool match = false;
                    for ( const auto& node : nodes ) {
                        if ( nc.m_trackIndex == node.track &&
                             std::abs(nc.m_timestamp - node.time) <= 0.003 ) {
                            match = true;
                            break;
                        }
                    }

                    if ( match ) {
                        if ( alreadyQueued.find(entity) ==
                             alreadyQueued.end() ) {
                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            alreadyQueued.insert(entity);
                            XINFO(
                                "Polyline path clean: removing object {} at "
                                "t={:.3f} "
                                "track={}",
                                static_cast<uint32_t>(entity),
                                nc.m_timestamp,
                                nc.m_trackIndex);
                        }
                    }
                }
            }
        }

        // 3.8 对合并/清洗后的最终段再次进行深度清洗与递归简化
        {
            bool changed = true;
            while ( changed ) {
                changed = false;

                // 1. 过滤所有“零值”段：0长度Hold 或 0位移Flick
                auto it = std::remove_if(
                    segments.begin(), segments.end(), [](const auto& s) {
                        if ( s.type == ::MMM::NoteType::HOLD )
                            return s.duration < 1e-5;
                        if ( s.type == ::MMM::NoteType::FLICK )
                            return s.dtrack == 0;
                        return false;
                    });
                if ( it != segments.end() ) {
                    segments.erase(it, segments.end());
                    changed = true;
                }

                // 2. 合并连续的同类型物件
                if ( segments.size() > 1 ) {
                    for ( size_t i = 0; i < segments.size() - 1; ) {
                        auto& curr = segments[i];
                        auto& next = segments[i + 1];

                        if ( curr.type == next.type ) {
                            if ( curr.type == ::MMM::NoteType::HOLD ) {
                                // 合并长条持续时间
                                curr.duration += next.duration;
                                segments.erase(segments.begin() + i + 1);
                                changed = true;
                                continue;  // 继续检查合并后的段
                            } else if ( curr.type == ::MMM::NoteType::FLICK ) {
                                // 合并滑键位移量
                                curr.dtrack += next.dtrack;
                                segments.erase(segments.begin() + i + 1);
                                changed = true;
                                continue;
                            }
                        }
                        i++;
                    }
                }
            }
        }

        // 4. 根据最终清洗与合并结果进行降级或重构
        if ( segments.empty() ) {
            note.m_type     = ::MMM::NoteType::NOTE;
            note.m_duration = 0.0;
            note.m_dtrack   = 0;
            note.m_subNotes.clear();
        } else if ( segments.size() == 1 ) {
            auto s            = segments[0];
            note.m_type       = s.type;
            note.m_timestamp  = s.timestamp;
            note.m_duration   = s.duration;
            note.m_trackIndex = s.trackIndex;
            note.m_dtrack     = s.dtrack;
            note.m_subNotes.clear();
        } else {
            note.m_subNotes = segments;
            if ( !note.m_subNotes.empty() ) {
                note.m_timestamp  = note.m_subNotes.front().timestamp;
                note.m_trackIndex = note.m_subNotes.front().trackIndex;
                note.m_type       = ::MMM::NoteType::POLYLINE;
            }
        }
    }

    if ( !isPlaceableNote(note) ) {
        XWARN("DrawTool: blocked note placement before 0s (time={:.3f})",
              note.m_timestamp);
        resetBrushState(ctx);
        return;
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        // 创建折线父实体及所有子物件实体
        entt::entity parentEnt = ctx.noteRegistry.create();
        mergeDeleteEntries.push_back({ parentEnt, std::nullopt, note });

        for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
            const auto&   s     = note.m_subNotes[i];
            NoteComponent subNC = makeNoteComponentFromSubNote(
                s, true, parentEnt, static_cast<int>(i));

            entt::entity subEnt = ctx.noteRegistry.create();
            mergeDeleteEntries.push_back({ subEnt, std::nullopt, subNC });
        }

        auto action = std::make_unique<BatchNoteAction>(
            std::move(mergeDeleteEntries), "Polyline Create");
        ctx.actionStack.pushAndExecute(std::move(action), ctx);
    } else {
        // 非折线降级物件 (NOTE / HOLD / FLICK)
        if ( !mergeDeleteEntries.empty() ) {
            mergeDeleteEntries.push_back(
                { ctx.noteRegistry.create(), std::nullopt, note });
            auto action = std::make_unique<BatchNoteAction>(
                std::move(mergeDeleteEntries), "Note Create & Merge");
            ctx.actionStack.pushAndExecute(std::move(action), ctx);
        } else {
            auto action = std::make_unique<NoteAction>(
                NoteAction::Type::Create, entt::null, std::nullopt, note);
            ctx.actionStack.pushAndExecute(std::move(action), ctx);
        }
    }

    // 重置状态
    resetBrushState(ctx);
}

void DrawTool::handleStartErase(SessionContext& ctx, const CmdStartErase& cmd)
{
    ctx.eraserState.isActive    = true;
    ctx.eraserState.isShiftDown = cmd.isShiftDown;
    ctx.eraserState.targetEntities.clear();
    if ( ctx.hoveredEntity != entt::null ) {
        entt::entity target = ctx.hoveredEntity;
        // Shift 模式：如果悬停在 Polyline 的子物件上，解析到父 Polyline 实体
        if ( cmd.isShiftDown && ctx.noteRegistry.valid(ctx.hoveredEntity) &&
             ctx.noteRegistry.all_of<NoteComponent>(ctx.hoveredEntity) ) {
            const auto& nc =
                ctx.noteRegistry.get<NoteComponent>(ctx.hoveredEntity);
            if ( nc.m_isSubNote && nc.m_parentPolyline != entt::null ) {
                target = nc.m_parentPolyline;
            }
        }
        ctx.eraserState.targetEntities.insert(target);
    }
}

void DrawTool::handleUpdateErase(SessionContext& ctx, const CmdUpdateErase& cmd)
{
    if ( !ctx.eraserState.isActive ) return;

    ctx.eraserState.isShiftDown = cmd.isShiftDown;

    // 每帧只标记当前鼠标正下方的物件，移开就取消
    ctx.eraserState.targetEntities.clear();
    if ( ctx.hoveredEntity != entt::null ) {
        entt::entity target = ctx.hoveredEntity;
        // Shift 模式：如果悬停在 Polyline 的子物件上，解析到父 Polyline 实体
        if ( cmd.isShiftDown && ctx.noteRegistry.valid(ctx.hoveredEntity) &&
             ctx.noteRegistry.all_of<NoteComponent>(ctx.hoveredEntity) ) {
            const auto& nc =
                ctx.noteRegistry.get<NoteComponent>(ctx.hoveredEntity);
            if ( nc.m_isSubNote && nc.m_parentPolyline != entt::null ) {
                target = nc.m_parentPolyline;
            }
        }
        ctx.eraserState.targetEntities.insert(target);
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
                const auto& nc = ctx.noteRegistry.get<NoteComponent>(entity);
                bool        handled = false;

                // 特殊逻辑：如果是
                // Polyline，且当前悬停在它上面，则执行“分裂/缩减”而非“直接全部删除”
                if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                     !nc.m_subNotes.empty() &&
                     ctx.eraserState.targetEntities.count(entity) &&
                     !ctx.eraserState.isShiftDown ) {
                    int k = ctx.hoveredSubIndex;
                    if ( entity == ctx.hoveredEntity && k >= 0 &&
                         k < static_cast<int>(nc.m_subNotes.size()) ) {

                        // 1. 收集并删除该 Polyline 的所有旧子物件实体
                        appendPolylineChildDeleteEntries(ctx, entity, entries);

                        // 2. 删除原 Polyline 实体
                        entries.push_back({ entity, nc, std::nullopt });

                        // 3. 计算左右半段
                        if ( k > 0 ) {
                            auto subNotes = nc.m_subNotes;
                            std::vector<NoteComponent::SubNote> L(
                                subNotes.begin(), subNotes.begin() + k);
                            std::vector<NoteComponent::SubNote> R(
                                subNotes.begin() + k + 1, subNotes.end());

                            auto processPart = [&](const std::vector<
                                                       NoteComponent::SubNote>&
                                                        part,
                                                   bool inheritsParentSound) {
                                if ( part.empty() ) return;
                                if ( part.size() == 1 ) {
                                    auto          s = part[0];
                                    NoteComponent nextNC =
                                        makeNoteComponentFromSubNote(
                                            s, false, entt::null, -1);
                                    if ( nextNC.m_boundSound.empty() &&
                                         inheritsParentSound ) {
                                        nextNC.m_boundSound = nc.m_boundSound;
                                    }
                                    if ( !hasAnyNoteColorOverride(
                                             nextNC.m_customColors) &&
                                         hasAnyNoteColorOverride(
                                             nc.m_customColors) ) {
                                        nextNC.m_customColors =
                                            nc.m_customColors;
                                        writeNoteColorOverridesToMetadata(
                                            nextNC);
                                    }

                                    entt::entity newEnt =
                                        ctx.noteRegistry.create();
                                    entries.push_back(
                                        { newEnt, std::nullopt, nextNC });
                                } else {
                                    NoteComponent nextNC;
                                    nextNC.m_type = ::MMM::NoteType::POLYLINE;
                                    nextNC.m_timestamp = part.front().timestamp;
                                    nextNC.m_trackIndex =
                                        part.front().trackIndex;
                                    nextNC.m_duration = 0.0;
                                    nextNC.m_dtrack   = 0;
                                    nextNC.m_metadata = nc.m_metadata;
                                    if ( inheritsParentSound ) {
                                        nextNC.m_boundSound = nc.m_boundSound;
                                    }
                                    nextNC.m_customColors   = nc.m_customColors;
                                    nextNC.m_isSubNote      = false;
                                    nextNC.m_parentPolyline = entt::null;
                                    nextNC.m_subIndex       = -1;
                                    nextNC.m_subNotes       = part;

                                    entt::entity parentEnt =
                                        ctx.noteRegistry.create();
                                    entries.push_back(
                                        { parentEnt, std::nullopt, nextNC });

                                    for ( size_t i = 0; i < part.size(); ++i ) {
                                        const auto&   s = part[i];
                                        NoteComponent subNC =
                                            makeNoteComponentFromSubNote(
                                                s,
                                                true,
                                                parentEnt,
                                                static_cast<int>(i));

                                        entt::entity subEnt =
                                            ctx.noteRegistry.create();
                                        entries.push_back(
                                            { subEnt, std::nullopt, subNC });
                                    }
                                }
                            };

                            processPart(L, true);
                            processPart(R, false);
                        }

                        handled = true;
                    }
                }

                if ( !handled ) {
                    entries.push_back({ entity, nc, std::nullopt });
                }
            }
            XINFO("Eraser: Processing all {} items (selected + targets)",
                  entries.size());
        } else {
            // 场景 B: 擦除的目标全都是未选中物件 ->
            // 只处理这些目标物件（支持缩减/分裂）
            for ( auto entity : ctx.eraserState.targetEntities ) {
                if ( ctx.noteRegistry.valid(entity) &&
                     ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                    const auto& nc =
                        ctx.noteRegistry.get<NoteComponent>(entity);
                    bool handled = false;

                    if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                         !nc.m_subNotes.empty() &&
                         !ctx.eraserState.isShiftDown ) {
                        int k = ctx.hoveredSubIndex;
                        if ( entity == ctx.hoveredEntity && k >= 0 &&
                             k < static_cast<int>(nc.m_subNotes.size()) ) {

                            // 1. 收集并删除该 Polyline 的所有旧子物件实体
                            appendPolylineChildDeleteEntries(
                                ctx, entity, entries);

                            // 2. 删除原 Polyline 实体
                            entries.push_back({ entity, nc, std::nullopt });

                            // 3. 计算左右半段
                            if ( k > 0 ) {
                                auto subNotes = nc.m_subNotes;
                                std::vector<NoteComponent::SubNote> L(
                                    subNotes.begin(), subNotes.begin() + k);
                                std::vector<NoteComponent::SubNote> R(
                                    subNotes.begin() + k + 1, subNotes.end());

                                auto processPart = [&](const std::vector<
                                                           NoteComponent::
                                                               SubNote>& part,
                                                       bool
                                                           inheritsParentSound) {
                                    if ( part.empty() ) return;
                                    if ( part.size() == 1 ) {
                                        auto          s = part[0];
                                        NoteComponent nextNC =
                                            makeNoteComponentFromSubNote(
                                                s, false, entt::null, -1);
                                        if ( nextNC.m_boundSound.empty() &&
                                             inheritsParentSound ) {
                                            nextNC.m_boundSound =
                                                nc.m_boundSound;
                                        }
                                        if ( !hasAnyNoteColorOverride(
                                                 nextNC.m_customColors) &&
                                             hasAnyNoteColorOverride(
                                                 nc.m_customColors) ) {
                                            nextNC.m_customColors =
                                                nc.m_customColors;
                                            writeNoteColorOverridesToMetadata(
                                                nextNC);
                                        }

                                        entt::entity newEnt =
                                            ctx.noteRegistry.create();
                                        entries.push_back(
                                            { newEnt, std::nullopt, nextNC });
                                    } else {
                                        NoteComponent nextNC;
                                        nextNC.m_type =
                                            ::MMM::NoteType::POLYLINE;
                                        nextNC.m_timestamp =
                                            part.front().timestamp;
                                        nextNC.m_trackIndex =
                                            part.front().trackIndex;
                                        nextNC.m_duration = 0.0;
                                        nextNC.m_dtrack   = 0;
                                        nextNC.m_metadata = nc.m_metadata;
                                        if ( inheritsParentSound ) {
                                            nextNC.m_boundSound =
                                                nc.m_boundSound;
                                        }
                                        nextNC.m_customColors =
                                            nc.m_customColors;
                                        nextNC.m_isSubNote      = false;
                                        nextNC.m_parentPolyline = entt::null;
                                        nextNC.m_subIndex       = -1;
                                        nextNC.m_subNotes       = part;

                                        entt::entity parentEnt =
                                            ctx.noteRegistry.create();
                                        entries.push_back({ parentEnt,
                                                            std::nullopt,
                                                            nextNC });

                                        for ( size_t i = 0; i < part.size();
                                              ++i ) {
                                            const auto&   s = part[i];
                                            NoteComponent subNC =
                                                makeNoteComponentFromSubNote(
                                                    s,
                                                    true,
                                                    parentEnt,
                                                    static_cast<int>(i));

                                            entt::entity subEnt =
                                                ctx.noteRegistry.create();
                                            entries.push_back({ subEnt,
                                                                std::nullopt,
                                                                subNC });
                                        }
                                    }
                                };

                                processPart(L, true);
                                processPart(R, false);
                            }

                            handled = true;
                        }
                    }

                    if ( !handled ) {
                        entries.push_back(
                            { entity,
                              ctx.noteRegistry.get<NoteComponent>(entity),
                              std::nullopt });
                    }
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
            appendPolylineChildDeleteEntries(ctx, erasedEntities, entries);

            auto action = std::make_unique<BatchNoteAction>(std::move(entries));
            ctx.actionStack.pushAndExecute(std::move(action), ctx);
        }
    }

    ctx.eraserState.isActive    = false;
    ctx.eraserState.isShiftDown = false;
    ctx.eraserState.targetEntities.clear();
}

}  // namespace MMM::Logic
