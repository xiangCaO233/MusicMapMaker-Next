#include "logic/session/tool/GrabTool.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/EditorAction.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace MMM::Logic
{

namespace
{
/// @brief 折线子段拖拽结束时用于判断零长度 Hold 的容差。
constexpr double POLYLINE_SUB_DRAG_ZERO_DURATION = 1e-5;

/// @brief 带原始子实体索引的折线子段，用于清理后回写子实体。
struct CleanPolylineSubSegment {
    /// @brief 折线子段数据。
    NoteComponent::SubNote sub;
    /// @brief 该清理后子段优先复用的原始子实体索引。
    int sourceIndex{ -1 };
};

/// @brief 判断折线子段是否已经退化为应删除的零值段。
/// @param sub 待检测子段。
/// @return 需要清理时返回 true。
bool isDegeneratePolylineSubSegment(const NoteComponent::SubNote& sub)
{
    if ( sub.type == ::MMM::NoteType::HOLD ) {
        return sub.duration < POLYLINE_SUB_DRAG_ZERO_DURATION;
    }
    if ( sub.type == ::MMM::NoteType::FLICK ) {
        return sub.dtrack == 0;
    }
    return false;
}

/// @brief 判断两个相邻折线子段是否可在零值清理后合并。
/// @param lhs 前一个子段。
/// @param rhs 后一个子段。
/// @return 可合并时返回 true。
bool canMergeAdjacentPolylineSubSegments(const NoteComponent::SubNote& lhs,
                                         const NoteComponent::SubNote& rhs)
{
    if ( lhs.type != rhs.type ) return false;
    return lhs.type == ::MMM::NoteType::HOLD ||
           lhs.type == ::MMM::NoteType::FLICK;
}

/// @brief 将后一个同类折线子段合并进前一个子段。
/// @param target 保留并扩展的子段。
/// @param source 被合并的子段。
void mergeAdjacentPolylineSubSegments(NoteComponent::SubNote&       target,
                                      const NoteComponent::SubNote& source)
{
    if ( target.type == ::MMM::NoteType::HOLD ) {
        double mergedEnd = source.timestamp + source.duration;
        target.duration  = std::max(0.0, mergedEnd - target.timestamp);
    } else if ( target.type == ::MMM::NoteType::FLICK ) {
        int mergedEndTrack = source.trackIndex + source.dtrack;
        target.dtrack      = mergedEndTrack - target.trackIndex;
    }
}

/// @brief 取颜色覆盖中的单个槽位。
/// @param colors 颜色覆盖集合。
/// @param slot 目标颜色槽位。
/// @return 对应槽位的颜色；未设置时为空。
std::optional<glm::vec4> getPolylineDragColorOverride(
    const NoteColorOverrides& colors, NoteColorSlot slot)
{
    return getNoteColorOverride(colors, slot);
}

/// @brief 合并子段和父折线的颜色覆盖，优先保留子段颜色。
/// @param childColors 保留下来的唯一子段颜色。
/// @param parentColors 被降级的父折线颜色。
/// @return 子段缺失槽位由父折线补齐后的颜色覆盖集合。
NoteColorOverrides mergeStandalonePolylineColors(
    const NoteColorOverrides& childColors,
    const NoteColorOverrides& parentColors)
{
    NoteColorOverrides merged = childColors;
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        if ( getPolylineDragColorOverride(merged, slot).has_value() ) {
            continue;
        }
        setNoteColorOverride(
            merged, slot, getPolylineDragColorOverride(parentColors, slot));
    }
    return merged;
}

/// @brief 清理折线子段中的零值段，并合并相邻同类段。
/// @param subNotes 拖拽结束后的折线子段列表。
/// @param changed 输出是否发生了清理或合并。
/// @return 清理后的子段与其复用的原始子实体索引。
std::vector<CleanPolylineSubSegment> cleanPolylineSubSegments(
    const std::vector<NoteComponent::SubNote>& subNotes, bool& changed)
{
    std::vector<CleanPolylineSubSegment> segments;
    segments.reserve(subNotes.size());
    for ( std::size_t i = 0; i < subNotes.size(); ++i ) {
        segments.push_back({ subNotes[i], static_cast<int>(i) });
    }

    changed          = false;
    bool passChanged = true;
    while ( passChanged ) {
        passChanged = false;

        for ( std::size_t i = 0; i < segments.size(); ) {
            if ( isDegeneratePolylineSubSegment(segments[i].sub) ) {
                segments.erase(segments.begin() +
                               static_cast<std::ptrdiff_t>(i));
                passChanged = true;
                changed     = true;
                continue;
            }
            ++i;
        }

        for ( std::size_t i = 0; i + 1 < segments.size(); ) {
            if ( canMergeAdjacentPolylineSubSegments(segments[i].sub,
                                                     segments[i + 1].sub) ) {
                mergeAdjacentPolylineSubSegments(segments[i].sub,
                                                 segments[i + 1].sub);
                segments.erase(segments.begin() +
                               static_cast<std::ptrdiff_t>(i + 1));
                passChanged = true;
                changed     = true;
                continue;
            }
            ++i;
        }
    }

    return segments;
}

/// @brief 将单个折线子段转为独立音符组件。
/// @param target 被降级的父音符组件。
/// @param sub 保留下来的唯一子段。
void applyStandaloneSubSegment(NoteComponent&                target,
                               const NoteComponent::SubNote& sub)
{
    const NoteColorOverrides parentColors = target.m_customColors;

    target.m_type           = sub.type;
    target.m_timestamp      = sub.timestamp;
    target.m_duration       = sub.duration;
    target.m_trackIndex     = sub.trackIndex;
    target.m_dtrack         = sub.dtrack;
    target.m_isSubNote      = false;
    target.m_parentPolyline = entt::null;
    target.m_subIndex       = -1;
    target.m_metadata       = sub.metadata;
    applyNoteColorOverrides(
        target, mergeStandalonePolylineColors(sub.customColors, parentColors));
    target.m_subNotes.clear();
}

/// @brief 将清理后的子段结果应用到父音符组件。
/// @param parentAfter 即将写入 Action 的父音符结果。
/// @param segments 清理后的子段列表。
void applyCleanedPolylineSubSegments(
    NoteComponent&                              parentAfter,
    const std::vector<CleanPolylineSubSegment>& segments)
{
    parentAfter.m_isSubNote      = false;
    parentAfter.m_parentPolyline = entt::null;
    parentAfter.m_subIndex       = -1;

    if ( segments.empty() ) {
        parentAfter.m_type     = ::MMM::NoteType::NOTE;
        parentAfter.m_duration = 0.0;
        parentAfter.m_dtrack   = 0;
        parentAfter.m_subNotes.clear();
        return;
    }

    if ( segments.size() == 1 ) {
        applyStandaloneSubSegment(parentAfter, segments.front().sub);
        return;
    }

    parentAfter.m_type       = ::MMM::NoteType::POLYLINE;
    parentAfter.m_timestamp  = segments.front().sub.timestamp;
    parentAfter.m_trackIndex = segments.front().sub.trackIndex;
    parentAfter.m_duration   = 0.0;
    parentAfter.m_dtrack     = 0;
    parentAfter.m_subNotes.clear();
    parentAfter.m_subNotes.reserve(segments.size());
    for ( const auto& segment : segments ) {
        parentAfter.m_subNotes.push_back(segment.sub);
    }
}

/// @brief 清除当前拖拽涉及实体的拖拽态。
/// @param ctx 会话上下文。
/// @param entities 拖拽开始时记录的实体集合。
template<typename InitialStateMap>
void clearDraggingFlags(SessionContext& ctx, const InitialStateMap& entities)
{
    for ( const auto& [entity, state] : entities ) {
        (void)state;
        if ( ctx.noteRegistry.valid(entity) &&
             ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
            ctx.noteRegistry.get<InteractionComponent>(entity).isDragging =
                false;
        }
    }
}
}  // namespace

void GrabTool::handleStartDrag(SessionContext& ctx, const CmdStartDrag& cmd)
{
    m_isPolylineSubDrag        = false;
    m_hasLastAppliedDragTarget = false;
    ctx.dragRenderPinnedEntities.clear();

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

        ctx.dragRenderPinnedEntities.reserve(m_initialStates.size());
        for ( const auto& [entity, state] : m_initialStates ) {
            (void)state;
            ctx.dragRenderPinnedEntities.push_back(entity);
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
        float ty             = ctx.lastConfig.visual.previewConfig.margin.top;
        float by           = it->second.viewportHeight -
                             ctx.lastConfig.visual.previewConfig.margin.bottom;
        float previewDrawH = by - ty;
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
    }

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;

    double currentAbsY = cache->getAbsY(ctx.animateTime);
    double targetAbsY =
        currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
    double targetTime = cache->getTime(targetAbsY);

    // 磁吸处理
    SessionUtils::ensureBpmEvents(ctx);
    const auto& bpmEvents = ctx.bpmEvents;

    auto snap = SessionUtils::getSnapResult(
        targetTime,
        cmd.mouseY,
        it->second,
        ctx.lastConfig,
        bpmEvents,
        ctx.timelineRegistry,
        ctx.animateTime,
        ctx.cameras,
        ctx.currentBeatmap
            ? ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : 120.0);
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

    if ( isPolylineSubDrag || isMultiDrag ) {
        constexpr double TARGET_TIME_EPSILON = 1e-7;
        if ( m_hasLastAppliedDragTarget &&
             std::abs(m_lastAppliedDragTargetTime - targetTime) <=
                 TARGET_TIME_EPSILON &&
             m_lastAppliedDragTargetTrack == targetTrack ) {
            return;
        }
        m_hasLastAppliedDragTarget   = true;
        m_lastAppliedDragTargetTime  = targetTime;
        m_lastAppliedDragTargetTrack = targetTrack;
    }

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
            float sTrackW  = (it->second.viewportWidth *
                              (ctx.lastConfig.visual.trackLayout.right -
                               ctx.lastConfig.visual.trackLayout.left)) /
                             static_cast<float>(ctx.trackCount);
            float lx       = it->second.viewportWidth *
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
        m_isPolylineSubDrag        = false;
        m_hasLastAppliedDragTarget = false;
        ctx.dragRenderPinnedEntities.clear();
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

    ctx.draggedEntity          = entt::null;
    ctx.dragInitialNote        = std::nullopt;
    m_hasLastAppliedDragTarget = false;
    ctx.dragRenderPinnedEntities.clear();
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

    int subIdx = ctx.draggedSubIndex;
    if ( subIdx <= 0 || subIdx >= static_cast<int>(note->m_subNotes.size()) ) {
        return false;
    }

    auto initIt = m_initialStates.find(ctx.draggedEntity);
    if ( initIt == m_initialStates.end() ) return false;

    bool cleanedChanged = false;
    auto cleanedSegments =
        cleanPolylineSubSegments(note->m_subNotes, cleanedChanged);
    if ( !cleanedChanged ) return false;

    NoteComponent parentBefore = initIt->second.note;
    NoteComponent parentAfter  = *note;
    applyCleanedPolylineSubSegments(parentAfter, cleanedSegments);

    struct ChildRecord {
        entt::entity  entity{ entt::null };
        int           oldSubIndex{ -1 };
        NoteComponent before;
        bool          kept{ false };
    };

    std::vector<BatchNoteAction::Entry> entries;
    entries.push_back({ ctx.draggedEntity, parentBefore, parentAfter });

    std::vector<ChildRecord> children;
    auto                     subView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto subEnt : subView ) {
        const auto& subNC = subView.get<NoteComponent>(subEnt);
        if ( !subNC.m_isSubNote || subNC.m_parentPolyline != ctx.draggedEntity )
            continue;

        auto          initSubIt = m_initialStates.find(subEnt);
        NoteComponent before    = (initSubIt != m_initialStates.end())
                                      ? initSubIt->second.note
                                      : subNC;
        children.push_back(
            { subEnt, before.m_subIndex, std::move(before), false });
    }

    std::stable_sort(children.begin(),
                     children.end(),
                     [](const ChildRecord& lhs, const ChildRecord& rhs) {
                         return lhs.oldSubIndex < rhs.oldSubIndex;
                     });

    if ( parentAfter.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( std::size_t newIndex = 0; newIndex < cleanedSegments.size();
              ++newIndex ) {
            auto childIt =
                std::find_if(children.begin(),
                             children.end(),
                             [&](const ChildRecord& child) {
                                 return child.oldSubIndex ==
                                        cleanedSegments[newIndex].sourceIndex;
                             });

            NoteComponent after =
                makeNoteComponentFromSubNote(parentAfter.m_subNotes[newIndex],
                                             true,
                                             ctx.draggedEntity,
                                             static_cast<int>(newIndex));

            if ( childIt != children.end() ) {
                childIt->kept = true;
                entries.push_back({ childIt->entity, childIt->before, after });
            } else {
                entt::entity newChild = ctx.noteRegistry.create();
                entries.push_back({ newChild, std::nullopt, after });
            }
        }
    }

    for ( const auto& child : children ) {
        if ( !child.kept ) {
            entries.push_back({ child.entity, child.before, std::nullopt });
        }
    }

    if ( !entries.empty() ) {
        clearDraggingFlags(ctx, m_initialStates);
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
