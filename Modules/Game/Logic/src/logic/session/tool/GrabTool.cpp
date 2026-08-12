#include "logic/session/tool/GrabTool.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectResourceService.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/EditorAction.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
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
/// @param inheritsParentSound 子段是否仍包含原折线头部。
void applyStandaloneSubSegment(NoteComponent&                target,
                               const NoteComponent::SubNote& sub,
                               bool inheritsParentSound)
{
    const NoteColorOverrides parentColors        = target.m_customColors;
    const auto               parentSampleBinding = target.m_sampleBinding;

    target.m_type           = sub.type;
    target.m_timestamp      = sub.timestamp;
    target.m_duration       = sub.duration;
    target.m_trackIndex     = sub.trackIndex;
    target.m_dtrack         = sub.dtrack;
    target.m_isSubNote      = false;
    target.m_parentPolyline = entt::null;
    target.m_subIndex       = -1;
    target.m_metadata       = sub.metadata;
    target.m_sampleBinding  = sub.sampleBinding;
    if ( !target.m_sampleBinding && inheritsParentSound ) {
        target.m_sampleBinding = parentSampleBinding;
    }
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
        applyStandaloneSubSegment(parentAfter,
                                  segments.front().sub,
                                  segments.front().sourceIndex == 0);
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

/// @brief 清除当前拖拽涉及自动采样实体的拖拽态。
/// @param ctx 会话上下文。
/// @param entities 自动采样初始状态集合。
template<typename InitialStateMap>
void clearSampleDraggingFlags(SessionContext&        ctx,
                              const InitialStateMap& entities)
{
    for ( const auto& [entity, state] : entities ) {
        (void)state;
        if ( ctx.sampleRegistry.valid(entity) &&
             ctx.sampleRegistry.all_of<InteractionComponent>(entity) ) {
            ctx.sampleRegistry.get<InteractionComponent>(entity).isDragging =
                false;
        }
    }
}

/// @brief 查询实体当前是否被选中。
/// @warning 拖动开始低频路径：只读取单个实体的交互组件。
bool isEntitySelected(const entt::registry& registry, entt::entity entity)
{
    const auto* interaction =
        registry.try_get<const InteractionComponent>(entity);
    return interaction && interaction->isSelected;
}

/// @brief 统一轨道拖动中的鼠标目标。
struct UnifiedDragTarget {
    /// @brief 目标锚点或实际触发时间，单位秒。
    double time{ 0.0 };
    /// @brief 草稿/玩家/BGM 共用的有符号轨道，负值表示草稿轨。
    std::int32_t absoluteTrack{ 0 };
    /// @brief 玩家轨道投影左边界。
    float leftX{ 0.0F };
    /// @brief 单轨逻辑宽度。
    float singleTrackWidth{ 0.0F };
};

/// @brief 将当前鼠标位置换算为统一画布时间与绝对轨道。
/// @param ctx 会话上下文。
/// @param cmd 拖动更新命令。
/// @return 缺少相机或滚动缓存时返回空。
/// @warning 拖动热路径：只做缓存查询、二分吸附与常量坐标运算。
std::optional<UnifiedDragTarget> calculateUnifiedDragTarget(
    SessionContext& ctx, const CmdUpdateDrag& cmd)
{
    const auto cameraIterator = ctx.cameras.find(cmd.cameraId);
    if ( cameraIterator == ctx.cameras.end() ) return std::nullopt;
    const auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return std::nullopt;

    const auto& camera = cameraIterator->second;
    const float judgmentLineY =
        camera.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    float renderScaleY = 1.0F;
    if ( cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas" ) {
        const auto* mainCamera =
            SessionUtils::findMainCanvasCamera(ctx.cameras);
        const float mainViewportHeight =
            mainCamera ? mainCamera->viewportHeight : camera.viewportHeight;
        const float mainEffectiveHeight =
            (ctx.lastConfig.visual.trackLayout.bottom -
             ctx.lastConfig.visual.trackLayout.top) *
            mainViewportHeight;
        const float previewTop = ctx.lastConfig.visual.previewConfig.margin.top;
        const float previewBottom =
            camera.viewportHeight -
            ctx.lastConfig.visual.previewConfig.margin.bottom;
        const float denominator =
            mainEffectiveHeight * ctx.lastConfig.visual.previewConfig.areaRatio;
        if ( std::abs(denominator) > 1e-6F ) {
            renderScaleY = (previewBottom - previewTop) / denominator;
        }
    }
    if ( std::abs(renderScaleY) < 1e-6F ) return std::nullopt;
    const double currentAbsY = cache->getAbsY(ctx.animateTime);
    const double targetAbsY =
        currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
    double targetTime = cache->getTime(targetAbsY);

    SessionUtils::ensureBpmEvents(ctx);
    const auto snap = SessionUtils::getSnapResult(
        targetTime,
        cmd.mouseY,
        camera,
        ctx.lastConfig,
        ctx.bpmEvents,
        ctx.timelineRegistry,
        ctx.animateTime,
        ctx.cameras,
        ctx.currentBeatmap
            ? ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : 120.0);
    if ( snap.isSnapped && !cmd.isCtrlDown ) {
        targetTime = snap.snappedTime;
    }

    const bool isMainCanvas = SessionUtils::isMainCanvasCameraId(cmd.cameraId);
    const auto projection   = calculateCanvasLaneProjection(
        camera.viewportWidth,
        ctx.trackCount,
        ctx.bgmTrackCount,
        ctx.lastConfig.visual.trackLayout.left,
        ctx.lastConfig.visual.trackLayout.right,
        isMainCanvas ? camera.horizontalOffsetX : 0.0F,
        isMainCanvas,
        isMainCanvas && ctx.lastConfig.settings.enableBmsEditing);
    if ( !projection.valid ) return std::nullopt;

    CanvasLaneAddress address;
    if ( isMainCanvas ) {
        const auto lane = projection.laneAt(cmd.mouseX);
        if ( lane ) {
            address = *lane;
        } else if ( cmd.mouseX < projection.player.leftX ) {
            address = { CanvasLaneKind::Draft, 0 };
        } else if ( projection.bgmLaneCount > 0 ) {
            address = { CanvasLaneKind::Bgm,
                        projection.bgmLaneCount - std::uint32_t{ 1 } };
        } else {
            address = { CanvasLaneKind::Player,
                        projection.playerLaneCount - std::uint32_t{ 1 } };
        }
    } else {
        address = {
            CanvasLaneKind::Player,
            static_cast<std::uint32_t>(
                projection.player.trackAt(cmd.mouseX, ctx.trackCount)),
        };
    }

    return UnifiedDragTarget{
        .time             = targetTime,
        .absoluteTrack    = address.absoluteTrack(projection.playerLaneCount),
        .leftX            = projection.player.leftX,
        .singleTrackWidth = projection.player.singleTrackWidth,
    };
}

/// @brief 判断音符及其折线子物件是否完整位于玩家轨道区。
/// @param note 待检查音符。
/// @param trackCount 玩家轨道数。
/// @return 所有轨道和 Flick 终点都合法时返回 true。
bool noteFitsPlayerArea(const NoteComponent& note, std::int32_t trackCount)
{
    if ( trackCount <= 0 ) return false;
    const auto fits = [trackCount](::MMM::NoteType type,
                                   std::int32_t    track,
                                   std::int32_t    dtrack) {
        if ( track < 0 || track >= trackCount ) return false;
        if ( type != ::MMM::NoteType::FLICK ) return true;
        const auto endTrack = track + dtrack;
        return endTrack >= 0 && endTrack < trackCount;
    };
    if ( !fits(note.m_type, note.m_trackIndex, note.m_dtrack) ) return false;
    return std::all_of(note.m_subNotes.begin(),
                       note.m_subNotes.end(),
                       [&](const NoteComponent::SubNote& sub) {
                           return fits(sub.type, sub.trackIndex, sub.dtrack);
                       });
}

/// @brief 判断草稿物件是否完整位于左侧 K 条草稿轨中。
bool noteFitsDraftArea(const NoteComponent& note, std::int32_t trackCount)
{
    if ( trackCount <= 0 ) return false;
    const auto fits = [trackCount](::MMM::NoteType type,
                                   std::int32_t    track,
                                   std::int32_t    dtrack) {
        if ( track < -trackCount || track >= 0 ) return false;
        if ( type != ::MMM::NoteType::FLICK ) return true;
        const auto endTrack = track + dtrack;
        return endTrack >= -trackCount && endTrack < 0;
    };
    if ( !fits(note.m_type, note.m_trackIndex, note.m_dtrack) ) return false;
    return std::all_of(note.m_subNotes.begin(),
                       note.m_subNotes.end(),
                       [&](const NoteComponent::SubNote& sub) {
                           return fits(sub.type, sub.trackIndex, sub.dtrack);
                       });
}

/// @brief 判断拖动可变字段是否保持不变。
/// @warning 释放低频路径：仅遍历单个 Polyline 的局部子物件。
bool sameDraggedNoteState(const NoteComponent& lhs, const NoteComponent& rhs)
{
    if ( lhs.m_timestamp != rhs.m_timestamp ||
         lhs.m_trackIndex != rhs.m_trackIndex ||
         lhs.m_duration != rhs.m_duration || lhs.m_dtrack != rhs.m_dtrack ||
         lhs.m_isDraft != rhs.m_isDraft ||
         lhs.m_subNotes.size() != rhs.m_subNotes.size() ) {
        return false;
    }
    for ( std::size_t index = 0; index < lhs.m_subNotes.size(); ++index ) {
        const auto& left  = lhs.m_subNotes[index];
        const auto& right = rhs.m_subNotes[index];
        if ( left.timestamp != right.timestamp ||
             left.trackIndex != right.trackIndex ||
             left.duration != right.duration || left.dtrack != right.dtrack ) {
            return false;
        }
    }
    return true;
}

/// @brief 判断自动采样拖动可变字段是否保持不变。
bool sameDraggedSampleState(const SampleComponent& lhs,
                            const SampleComponent& rhs)
{
    return lhs.m_timestamp == rhs.m_timestamp &&
           lhs.m_offsetMs == rhs.m_offsetMs && lhs.m_track == rhs.m_track;
}

/// @brief 按项目资源表解析谱面物件保存的音频引用。
/// @param ctx 会话上下文。
/// @param reference 资源 ID 或兼容路径。
/// @return 匹配项目资源；无项目或引用失效时返回空。
const ::MMM::AudioResource* resolveReferencedAudioResource(
    const SessionContext& ctx, const std::string& reference)
{
    const auto* project = ctx.collaborationProject
                              ? ctx.collaborationProject.get()
                              : EditorEngine::instance().getCurrentProject();
    if ( !project || reference.empty() ) return nullptr;
    const std::filesystem::path beatmapPath =
        ctx.currentBeatmap ? ctx.currentBeatmap->m_baseMapMetadata.map_path
                           : std::filesystem::path{};
    return ProjectResourceService::findAudioResourceForReference(
        *project, beatmapPath, reference);
}

/// @brief 清除会话级拖拽手势状态。
/// @param ctx 会话上下文。
void clearSessionDragState(SessionContext& ctx)
{
    ctx.isDragging      = false;
    ctx.draggedPart     = HoverPart::None;
    ctx.draggedSubIndex = -1;
    ctx.dragCameraId.clear();
}
}  // namespace

std::optional<NoteComponent> makePlayerNoteFromSample(
    const SampleComponent& sample, std::int32_t targetTrack,
    const ::MMM::AudioResource* resource)
{
    if ( sample.m_offsetMs != 0 || targetTrack < 0 ) {
        return std::nullopt;
    }

    NoteComponent note;
    note.m_type       = ::MMM::NoteType::NOTE;
    note.m_timestamp  = sample.m_timestamp;
    note.m_trackIndex = targetTrack;
    if ( sample.m_audioResourceId.empty() ) {
        return note;
    }
    if ( !resource || resource->m_type != ::MMM::AudioTrackType::Effect ) {
        return std::nullopt;
    }
    note.m_sampleBinding =
        ::MMM::AudioSampleBinding{ sample.m_audioResourceId, sample.m_volume };
    return note;
}

std::optional<SampleComponent> makeAudioSampleFromPlayerNote(
    const NoteComponent& note, std::uint32_t targetTrack,
    const ::MMM::AudioResource* resource)
{
    if ( note.m_isSubNote || note.m_type != ::MMM::NoteType::NOTE ||
         note.m_duration != 0.0 || note.m_dtrack != 0 ||
         !note.m_subNotes.empty() ) {
        return std::nullopt;
    }

    SampleComponent sample;
    sample.m_timestamp = note.m_timestamp;
    sample.m_offsetMs  = 0;
    sample.m_track     = targetTrack;
    if ( !note.m_sampleBinding ||
         note.m_sampleBinding->m_audioResourceId.empty() ) {
        return sample;
    }
    if ( !resource || resource->m_type != ::MMM::AudioTrackType::Effect ) {
        return std::nullopt;
    }
    sample.m_audioResourceId = note.m_sampleBinding->m_audioResourceId;
    sample.m_volume          = note.m_sampleBinding->m_volume;
    return sample;
}

/// @brief 开始物件移动或局部编辑拖拽。
/// @param ctx 会话上下文。
/// @param cmd 拖拽开始命令。
void GrabTool::handleStartDrag(SessionContext& ctx, const CmdStartDrag& cmd)
{
    m_isPolylineSubDrag        = false;
    m_usesUnifiedObjectDrag    = false;
    m_isSampleOffsetDrag       = false;
    m_hasLastAppliedDragTarget = false;
    m_initialStates.clear();
    m_initialSampleStates.clear();
    ctx.dragRenderPinnedEntities.clear();
    ctx.dragSampleRenderPinnedEntities.clear();

    if ( cmd.entity == entt::null ) return;
    if ( cmd.kind == ChartObjectKind::PlayerNote ||
         cmd.kind == ChartObjectKind::DraftNote ) {
        const auto* note =
            ctx.noteRegistry.try_get<const NoteComponent>(cmd.entity);
        if ( !note ||
             !SessionUtils::isNoteEditable(*note, ctx.lastConfig.settings) ) {
            return;
        }
    }

    ctx.draggedEntity     = cmd.entity;
    ctx.draggedObjectKind = cmd.kind;
    ctx.dragCameraId      = cmd.cameraId;
    ctx.draggedPart       = static_cast<HoverPart>(ctx.hoveredPart);
    ctx.draggedSubIndex   = ctx.hoveredSubIndex;
    if ( cmd.kind == ChartObjectKind::DraftNote ) {
        m_usesUnifiedObjectDrag = true;
    }

    if ( cmd.kind == ChartObjectKind::AudioSample ) {
        auto& registry = ctx.sampleRegistry;
        if ( !registry.valid(cmd.entity) ||
             !registry.all_of<SampleComponent>(cmd.entity) ) {
            return;
        }

        m_usesUnifiedObjectDrag    = true;
        m_isSampleOffsetDrag       = ctx.draggedPart == HoverPart::SampleOffset;
        const bool primarySelected = isEntitySelected(registry, cmd.entity);
        const bool collectSelectedGroup =
            primarySelected && !m_isSampleOffsetDrag;

        if ( collectSelectedGroup ) {
            auto sampleView =
                registry.view<InteractionComponent, SampleComponent>();
            for ( const auto entity : sampleView ) {
                const auto& interaction =
                    sampleView.get<InteractionComponent>(entity);
                if ( !interaction.isSelected ) continue;
                m_initialSampleStates.emplace(
                    entity,
                    SampleInitialState{
                        sampleView.get<SampleComponent>(entity),
                        true,
                    });
                registry.get<InteractionComponent>(entity).isDragging = true;
            }

            auto noteView =
                ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
            for ( const auto entity : noteView ) {
                const auto& interaction =
                    noteView.get<InteractionComponent>(entity);
                const auto& note = noteView.get<NoteComponent>(entity);
                if ( !interaction.isSelected || note.m_isSubNote ||
                     !SessionUtils::isNoteEditable(note,
                                                   ctx.lastConfig.settings) ) {
                    continue;
                }
                m_initialStates.emplace(entity, InitialState{ note, true });
                ctx.noteRegistry.get<InteractionComponent>(entity).isDragging =
                    true;
            }
            for ( const auto entity : ctx.noteRegistry.view<NoteComponent>() ) {
                const auto& note = ctx.noteRegistry.get<NoteComponent>(entity);
                if ( !note.m_isSubNote ||
                     !m_initialStates.contains(note.m_parentPolyline) ) {
                    continue;
                }
                m_initialStates.emplace(
                    entity,
                    InitialState{
                        note,
                        isEntitySelected(ctx.noteRegistry, entity),
                    });
                if ( ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
                    ctx.noteRegistry.get<InteractionComponent>(entity)
                        .isDragging = true;
                }
            }
        } else {
            const auto& sample = registry.get<SampleComponent>(cmd.entity);
            m_initialSampleStates.emplace(
                cmd.entity, SampleInitialState{ sample, primarySelected });
            if ( !registry.all_of<InteractionComponent>(cmd.entity) ) {
                registry.emplace<InteractionComponent>(cmd.entity);
            }
            registry.get<InteractionComponent>(cmd.entity).isDragging = true;
        }

        const auto initialIterator = m_initialSampleStates.find(cmd.entity);
        if ( initialIterator == m_initialSampleStates.end() ) return;
        ctx.dragInitialSample = initialIterator->second.sample;
        ctx.dragInitialNote.reset();
        ctx.isDragging = true;
    } else if ( ctx.noteRegistry.valid(cmd.entity) &&
                ctx.noteRegistry.all_of<NoteComponent>(cmd.entity) ) {
        auto&      registry        = ctx.noteRegistry;
        const bool primarySelected = isEntitySelected(registry, cmd.entity);

        if ( primarySelected ) {
            // 模式 A: 拖动整个选中组
            auto view = registry.view<InteractionComponent, NoteComponent>();
            for ( auto entity : view ) {
                const auto& note = view.get<NoteComponent>(entity);
                if ( view.get<InteractionComponent>(entity).isSelected &&
                     SessionUtils::isNoteEditable(note,
                                                  ctx.lastConfig.settings) ) {
                    m_initialStates[entity] = {
                        note,
                        true,
                    };
                    registry.get<InteractionComponent>(entity).isDragging =
                        true;
                }
            }

            // 额外收集所有选中 Polyline 的子物件实体
            // (确保它们跟随移动并能正确提交 Action)
            for ( auto entity : registry.view<NoteComponent>() ) {
                const auto& nc = registry.get<NoteComponent>(entity);
                if ( nc.m_isSubNote &&
                     m_initialStates.count(nc.m_parentPolyline) ) {
                    m_initialStates[entity] = {
                        nc,
                        isEntitySelected(registry, entity),
                    };
                    if ( registry.all_of<InteractionComponent>(entity) ) {
                        registry.get<InteractionComponent>(entity).isDragging =
                            true;
                    }
                }
            }

            const bool groupCompatiblePart =
                ctx.draggedPart == HoverPart::None ||
                ctx.draggedPart == HoverPart::Head ||
                ctx.draggedPart == HoverPart::HoldBody ||
                ctx.draggedPart == HoverPart::PolylineNode;
            if ( groupCompatiblePart ) {
                auto sampleView =
                    ctx.sampleRegistry
                        .view<InteractionComponent, SampleComponent>();
                for ( const auto entity : sampleView ) {
                    const auto& interaction =
                        sampleView.get<InteractionComponent>(entity);
                    if ( !interaction.isSelected ) continue;
                    m_initialSampleStates.emplace(
                        entity,
                        SampleInitialState{
                            sampleView.get<SampleComponent>(entity),
                            true,
                        });
                    ctx.sampleRegistry.get<InteractionComponent>(entity)
                        .isDragging = true;
                }
                m_usesUnifiedObjectDrag = !m_initialSampleStates.empty();
            }
        } else {
            // 模式 B: 只拖动当前物件
            if ( auto* note = registry.try_get<NoteComponent>(cmd.entity) ) {
                m_initialStates[cmd.entity] = { *note, false };
                if ( !registry.all_of<InteractionComponent>(cmd.entity) ) {
                    registry.emplace<InteractionComponent>(cmd.entity);
                }
                registry.get<InteractionComponent>(cmd.entity).isDragging =
                    true;

                // 如果是 Polyline，也收集其子物件
                if ( note->m_type == ::MMM::NoteType::POLYLINE ) {
                    for ( auto subEnt : registry.view<NoteComponent>() ) {
                        const auto& subNC = registry.get<NoteComponent>(subEnt);
                        if ( subNC.m_isSubNote &&
                             subNC.m_parentPolyline == cmd.entity ) {
                            m_initialStates[subEnt] = {
                                subNC,
                                isEntitySelected(registry, subEnt),
                            };
                            if ( registry.all_of<InteractionComponent>(
                                     subEnt) ) {
                                registry.get<InteractionComponent>(subEnt)
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
            ctx.dragInitialSample.reset();
            ctx.isDragging = true;
        }
    } else {
        ctx.draggedEntity = entt::null;
        return;
    }

    ctx.dragRenderPinnedEntities.reserve(m_initialStates.size());
    for ( const auto& [entity, state] : m_initialStates ) {
        (void)state;
        ctx.dragRenderPinnedEntities.push_back(entity);
    }
    ctx.dragSampleRenderPinnedEntities.reserve(m_initialSampleStates.size());
    for ( const auto& [entity, state] : m_initialSampleStates ) {
        (void)state;
        ctx.dragSampleRenderPinnedEntities.push_back(entity);
    }
    auto* pinned = ctx.sampleRegistry.ctx().find<DragRenderPinnedEntities>();
    if ( !pinned ) {
        pinned = &ctx.sampleRegistry.ctx().emplace<DragRenderPinnedEntities>();
    }
    pinned->entities = &ctx.dragSampleRenderPinnedEntities;
}

bool GrabTool::handleUnifiedDragUpdate(SessionContext&      ctx,
                                       const CmdUpdateDrag& cmd)
{
    if ( !m_usesUnifiedObjectDrag &&
         (ctx.draggedObjectKind == ChartObjectKind::PlayerNote ||
          ctx.draggedObjectKind == ChartObjectKind::DraftNote) ) {
        if ( !SessionUtils::isMainCanvasCameraId(cmd.cameraId) ) return false;
        const auto cameraIterator = ctx.cameras.find(cmd.cameraId);
        if ( cameraIterator == ctx.cameras.end() ) return false;
        const auto playerProjection = calculatePlayerTrackProjection(
            cameraIterator->second.viewportWidth,
            ctx.trackCount,
            ctx.lastConfig.visual.trackLayout.left,
            ctx.lastConfig.visual.trackLayout.right,
            cameraIterator->second.horizontalOffsetX);
        if ( !playerProjection.valid ||
             (cmd.mouseX >= playerProjection.leftX &&
              cmd.mouseX < playerProjection.rightX) ) {
            return false;
        }
    }

    const auto target = calculateUnifiedDragTarget(ctx, cmd);
    if ( !target ) {
        return m_usesUnifiedObjectDrag ||
               ctx.draggedObjectKind == ChartObjectKind::AudioSample;
    }

    if ( m_isSampleOffsetDrag ) {
        const auto initialIterator =
            m_initialSampleStates.find(ctx.draggedEntity);
        auto* sample =
            ctx.sampleRegistry.try_get<SampleComponent>(ctx.draggedEntity);
        if ( initialIterator == m_initialSampleStates.end() || !sample ) {
            return true;
        }

        const long double offsetMs =
            (static_cast<long double>(target->time) -
             static_cast<long double>(
                 initialIterator->second.sample.m_timestamp)) *
            1000.0L;
        constexpr auto minimumOffset = std::numeric_limits<std::int64_t>::min();
        constexpr auto maximumOffset = std::numeric_limits<std::int64_t>::max();
        if ( offsetMs <= static_cast<long double>(minimumOffset) ) {
            sample->m_offsetMs = minimumOffset;
        } else if ( offsetMs >= static_cast<long double>(maximumOffset) ) {
            sample->m_offsetMs = maximumOffset;
        } else {
            sample->m_offsetMs =
                static_cast<std::int64_t>(std::round(offsetMs));
        }
        m_hasLastAppliedDragTarget   = true;
        m_lastAppliedDragTargetTime  = target->time;
        m_lastAppliedDragTargetTrack = static_cast<int>(target->absoluteTrack);
        return true;
    }

    const bool targetIsBgm =
        target->absoluteTrack >=
        static_cast<std::int32_t>(std::max(0, ctx.trackCount));
    if ( !m_usesUnifiedObjectDrag &&
         ctx.draggedObjectKind == ChartObjectKind::PlayerNote &&
         !targetIsBgm ) {
        return false;
    }
    m_usesUnifiedObjectDrag = true;

    double       primaryTime  = 0.0;
    std::int64_t primaryTrack = 0;
    if ( ctx.draggedObjectKind == ChartObjectKind::AudioSample ) {
        const auto initialIterator =
            m_initialSampleStates.find(ctx.draggedEntity);
        if ( initialIterator == m_initialSampleStates.end() ) return true;
        primaryTime  = initialIterator->second.sample.m_timestamp;
        primaryTrack = initialIterator->second.sample.m_track;
    } else {
        const auto initialIterator = m_initialStates.find(ctx.draggedEntity);
        if ( initialIterator == m_initialStates.end() ) return true;
        primaryTime  = initialIterator->second.note.m_timestamp;
        primaryTrack = initialIterator->second.note.m_trackIndex;
    }

    double       deltaTime = target->time - primaryTime;
    std::int64_t deltaTrack =
        static_cast<std::int64_t>(target->absoluteTrack) - primaryTrack;
    if ( ctx.lastConfig.settings.disableVerticalObjectDrag ) {
        deltaTime = 0.0;
    }

    double       minimumTimestamp = std::numeric_limits<double>::infinity();
    std::int64_t minimumTrack     = std::numeric_limits<std::int64_t>::max();
    std::int64_t maximumTrack     = std::numeric_limits<std::int64_t>::min();
    const auto   includeTrack     = [&](std::int64_t track) {
        minimumTrack = std::min(minimumTrack, track);
        maximumTrack = std::max(maximumTrack, track);
    };
    const auto includeNote = [&](const NoteComponent& note) {
        minimumTimestamp = std::min(minimumTimestamp, note.m_timestamp);
        includeTrack(note.m_trackIndex);
        if ( note.m_type == ::MMM::NoteType::FLICK ) {
            includeTrack(static_cast<std::int64_t>(note.m_trackIndex) +
                         note.m_dtrack);
        }
        for ( const auto& sub : note.m_subNotes ) {
            minimumTimestamp = std::min(minimumTimestamp, sub.timestamp);
            includeTrack(sub.trackIndex);
            if ( sub.type == ::MMM::NoteType::FLICK ) {
                includeTrack(static_cast<std::int64_t>(sub.trackIndex) +
                             sub.dtrack);
            }
        }
    };

    for ( const auto& [entity, state] : m_initialStates ) {
        (void)entity;
        includeNote(state.note);
    }
    for ( const auto& [entity, state] : m_initialSampleStates ) {
        (void)entity;
        minimumTimestamp = std::min(minimumTimestamp, state.sample.m_timestamp);
        includeTrack(state.sample.m_track);
    }
    if ( std::isfinite(minimumTimestamp) &&
         minimumTimestamp + deltaTime < 0.0 ) {
        deltaTime = -minimumTimestamp;
    }

    const std::int64_t maximumAccessibleTrack =
        static_cast<std::int64_t>(std::max(0, ctx.trackCount)) +
        static_cast<std::int64_t>(std::max(0, ctx.bgmTrackCount));
    const std::int64_t minimumAccessibleTrack =
        m_initialSampleStates.empty()
            ? -static_cast<std::int64_t>(std::max(0, ctx.trackCount))
            : 0;
    if ( minimumTrack != std::numeric_limits<std::int64_t>::max() &&
         maximumTrack != std::numeric_limits<std::int64_t>::min() ) {
        const std::int64_t minimumDelta = minimumAccessibleTrack - minimumTrack;
        const std::int64_t maximumDelta = maximumAccessibleTrack - maximumTrack;
        if ( minimumDelta <= maximumDelta ) {
            deltaTrack = std::clamp(deltaTrack, minimumDelta, maximumDelta);
        } else {
            deltaTrack = 0;
        }
    }

    constexpr double TARGET_TIME_EPSILON = 1e-7;
    const auto       appliedTargetTrack  = static_cast<int>(
        std::clamp<std::int64_t>(primaryTrack + deltaTrack,
                                 std::numeric_limits<int>::min(),
                                 std::numeric_limits<int>::max()));
    const double appliedTargetTime = primaryTime + deltaTime;
    if ( m_hasLastAppliedDragTarget &&
         std::abs(m_lastAppliedDragTargetTime - appliedTargetTime) <=
             TARGET_TIME_EPSILON &&
         m_lastAppliedDragTargetTrack == appliedTargetTrack ) {
        return true;
    }
    m_hasLastAppliedDragTarget   = true;
    m_lastAppliedDragTargetTime  = appliedTargetTime;
    m_lastAppliedDragTargetTrack = appliedTargetTrack;

    for ( const auto& [entity, state] : m_initialStates ) {
        auto* note = ctx.noteRegistry.try_get<NoteComponent>(entity);
        if ( !note ) continue;
        note->m_timestamp = state.note.m_timestamp + deltaTime;
        const auto movedTrack =
            static_cast<std::int64_t>(state.note.m_trackIndex) + deltaTrack;
        note->m_trackIndex = static_cast<int>(
            std::clamp<std::int64_t>(movedTrack,
                                     std::numeric_limits<int>::min(),
                                     std::numeric_limits<int>::max()));
        note->m_isDraft = note->m_trackIndex < 0;
        for ( std::size_t index = 0; index < note->m_subNotes.size() &&
                                     index < state.note.m_subNotes.size();
              ++index ) {
            note->m_subNotes[index].timestamp =
                state.note.m_subNotes[index].timestamp + deltaTime;
            const auto movedSubTrack =
                static_cast<std::int64_t>(
                    state.note.m_subNotes[index].trackIndex) +
                deltaTrack;
            note->m_subNotes[index].trackIndex = static_cast<int>(
                std::clamp<std::int64_t>(movedSubTrack,
                                         std::numeric_limits<int>::min(),
                                         std::numeric_limits<int>::max()));
        }
        if ( note->m_type == ::MMM::NoteType::POLYLINE && !note->m_isSubNote ) {
            syncPolylineSubEntities(ctx, entity, *note);
        }
        if ( auto* transform =
                 ctx.noteRegistry.try_get<TransformComponent>(entity) ) {
            transform->m_pos.x =
                target->leftX + static_cast<float>(note->m_trackIndex) *
                                    target->singleTrackWidth;
        }
    }

    for ( const auto& [entity, state] : m_initialSampleStates ) {
        auto* sample = ctx.sampleRegistry.try_get<SampleComponent>(entity);
        if ( !sample ) continue;
        sample->m_timestamp = state.sample.m_timestamp + deltaTime;
        const auto movedTrack =
            static_cast<std::int64_t>(state.sample.m_track) + deltaTrack;
        sample->m_track =
            static_cast<std::uint32_t>(std::max<std::int64_t>(0, movedTrack));
    }
    return true;
}

void GrabTool::handleUpdateDrag(SessionContext& ctx, const CmdUpdateDrag& cmd)
{
    if ( ctx.draggedEntity == entt::null ) return;
    if ( handleUnifiedDragUpdate(ctx, cmd) ) return;
    if ( m_initialStates.empty() ) return;
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

    const auto projection = calculatePlayerTrackProjection(
        it->second.viewportWidth,
        ctx.trackCount,
        ctx.lastConfig.visual.trackLayout.left,
        ctx.lastConfig.visual.trackLayout.right,
        SessionUtils::isMainCanvasCameraId(cmd.cameraId)
            ? it->second.horizontalOffsetX
            : 0.0F);
    const float leftX        = projection.leftX;
    const float singleTrackW = projection.singleTrackWidth;
    int         targetTrack  = projection.trackAt(cmd.mouseX, ctx.trackCount);

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

    const bool movesWholeObjects = isMultiDrag && !isPolylineSubDrag;
    if ( movesWholeObjects &&
         ctx.lastConfig.settings.disableVerticalObjectDrag ) {
        deltaT = 0.0;
    }

    if ( isPolylineSubDrag || isMultiDrag ) {
        constexpr double TARGET_TIME_EPSILON = 1e-7;
        const double     appliedTargetTime =
            movesWholeObjects ? refInitialTime + deltaT : targetTime;
        if ( m_hasLastAppliedDragTarget &&
             std::abs(m_lastAppliedDragTargetTime - appliedTargetTime) <=
                 TARGET_TIME_EPSILON &&
             m_lastAppliedDragTargetTrack == targetTrack ) {
            return;
        }
        m_hasLastAppliedDragTarget   = true;
        m_lastAppliedDragTargetTime  = appliedTargetTime;
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
            trans->m_pos.x = leftX + note->m_trackIndex * singleTrackW;
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

void GrabTool::finishUnifiedDrag(SessionContext& ctx)
{
    struct NoteToSampleConversion {
        entt::entity    source{ entt::null };
        SampleComponent target;
        bool            selected{ false };
    };
    struct SampleToNoteConversion {
        entt::entity  source{ entt::null };
        NoteComponent target;
        bool          selected{ false };
    };

    std::vector<NoteToSampleConversion> noteConversions;
    std::vector<SampleToNoteConversion> sampleConversions;
    std::unordered_set<entt::entity>    convertedNotes;
    std::unordered_set<entt::entity>    convertedSamples;
    std::string                         rejectionReason;

    for ( const auto& [entity, state] : m_initialStates ) {
        const auto* current =
            ctx.noteRegistry.try_get<const NoteComponent>(entity);
        if ( !current || current->m_isSubNote ) continue;

        if ( current->m_trackIndex < 0 ) {
            if ( !noteFitsDraftArea(*current, ctx.trackCount) ) {
                rejectionReason =
                    "草稿物件拖动结果超出左侧草稿轨道区，已取消本次操作";
                break;
            }
            continue;
        }

        if ( current->m_trackIndex < ctx.trackCount ) {
            if ( !noteFitsPlayerArea(*current, ctx.trackCount) ) {
                rejectionReason =
                    "玩家物件拖动结果超出玩家轨道区，已取消本次操作";
                break;
            }
            continue;
        }

        const std::string reference =
            current->m_sampleBinding
                ? current->m_sampleBinding->m_audioResourceId
                : std::string{};
        const auto* resource  = resolveReferencedAudioResource(ctx, reference);
        const auto  converted = makeAudioSampleFromPlayerNote(
            *current,
            static_cast<std::uint32_t>(current->m_trackIndex),
            resource);
        if ( !converted ) {
            rejectionReason =
                "只有未绑定音频或绑定 Effect 的普通 Tap 才能拖入 BGM 轨道区";
            break;
        }
        noteConversions.push_back({ entity, *converted, state.selected });
        convertedNotes.insert(entity);
    }

    if ( rejectionReason.empty() ) {
        for ( const auto& [entity, state] : m_initialSampleStates ) {
            const auto* current =
                ctx.sampleRegistry.try_get<const SampleComponent>(entity);
            if ( !current ||
                 current->m_track >=
                     static_cast<std::uint32_t>(std::max(0, ctx.trackCount)) ) {
                continue;
            }

            const auto* resource =
                resolveReferencedAudioResource(ctx, current->m_audioResourceId);
            const auto converted = makePlayerNoteFromSample(
                *current,
                static_cast<std::int32_t>(current->m_track),
                resource);
            if ( !converted ) {
                rejectionReason =
                    "自动采样仅能在空资源或 Effect 且 offset 为 0 "
                    "时拖入玩家轨道区";
                break;
            }
            sampleConversions.push_back({ entity, *converted, state.selected });
            convertedSamples.insert(entity);
        }
    }

    const auto restoreInitialStates = [&]() {
        for ( const auto& [entity, state] : m_initialStates ) {
            if ( ctx.noteRegistry.valid(entity) &&
                 ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                ctx.noteRegistry.replace<NoteComponent>(entity, state.note);
            }
        }
        for ( const auto& [entity, state] : m_initialSampleStates ) {
            if ( ctx.sampleRegistry.valid(entity) &&
                 ctx.sampleRegistry.all_of<SampleComponent>(entity) ) {
                ctx.sampleRegistry.replace<SampleComponent>(entity,
                                                            state.sample);
            }
        }
        ctx.isTransformDirty = true;
    };

    clearDraggingFlags(ctx, m_initialStates);
    clearSampleDraggingFlags(ctx, m_initialSampleStates);

    if ( !rejectionReason.empty() ) {
        restoreInitialStates();
        ctx.lastActionMessage = std::move(rejectionReason);
    } else {
        std::vector<BatchNoteAction::Entry>   noteEntries;
        std::vector<BatchSampleAction::Entry> sampleEntries;
        noteEntries.reserve(m_initialStates.size() + sampleConversions.size());
        sampleEntries.reserve(m_initialSampleStates.size() +
                              noteConversions.size());

        for ( const auto& [entity, state] : m_initialStates ) {
            const auto* current =
                ctx.noteRegistry.try_get<const NoteComponent>(entity);
            if ( !current ) continue;
            if ( convertedNotes.contains(entity) ) {
                noteEntries.push_back({
                    .entity         = entity,
                    .before         = state.note,
                    .after          = std::nullopt,
                    .beforeSelected = state.selected,
                });
            } else if ( !sameDraggedNoteState(state.note, *current) ) {
                noteEntries.push_back({
                    .entity = entity,
                    .before = state.note,
                    .after  = *current,
                });
            }
        }

        for ( const auto& [entity, state] : m_initialSampleStates ) {
            const auto* current =
                ctx.sampleRegistry.try_get<const SampleComponent>(entity);
            if ( !current ) continue;
            if ( convertedSamples.contains(entity) ) {
                sampleEntries.push_back({
                    .entity         = entity,
                    .before         = state.sample,
                    .after          = std::nullopt,
                    .beforeSelected = state.selected,
                });
            } else if ( !sameDraggedSampleState(state.sample, *current) ) {
                sampleEntries.push_back({
                    .entity = entity,
                    .before = state.sample,
                    .after  = *current,
                });
            }
        }

        for ( const auto& conversion : noteConversions ) {
            const auto targetEntity = ctx.sampleRegistry.create();
            sampleEntries.push_back({
                .entity        = targetEntity,
                .before        = std::nullopt,
                .after         = conversion.target,
                .afterSelected = conversion.selected,
            });
        }
        for ( const auto& conversion : sampleConversions ) {
            const auto targetEntity = ctx.noteRegistry.create();
            noteEntries.push_back({
                .entity        = targetEntity,
                .before        = std::nullopt,
                .after         = conversion.target,
                .afterSelected = conversion.selected,
            });
        }

        std::vector<std::unique_ptr<IEditorAction>> actions;
        if ( !noteEntries.empty() ) {
            actions.push_back(std::make_unique<BatchNoteAction>(
                std::move(noteEntries), "统一画布物件移动"));
        }
        if ( sampleEntries.size() == 1 && sampleEntries.front().before &&
             sampleEntries.front().after && actions.empty() ) {
            auto entry = std::move(sampleEntries.front());
            actions.push_back(
                std::make_unique<SampleAction>(SampleAction::Type::Update,
                                               entry.entity,
                                               std::move(entry.before),
                                               std::move(entry.after)));
        } else if ( !sampleEntries.empty() ) {
            actions.push_back(std::make_unique<BatchSampleAction>(
                std::move(sampleEntries), "统一画布自动采样移动"));
        }

        if ( actions.size() == 1 ) {
            ctx.actionStack.pushAndExecute(std::move(actions.front()), ctx);
        } else if ( actions.size() > 1 ) {
            ctx.actionStack.pushAndExecute(
                std::make_unique<CompositeEditorAction>(
                    std::move(actions), "玩家物件与纯采样跨区转换"),
                ctx);
        }
    }

    SessionUtils::rebuildHitEvents(ctx);
    clearSessionDragState(ctx);
    ctx.draggedEntity     = entt::null;
    ctx.draggedObjectKind = ChartObjectKind::PlayerNote;
    ctx.dragInitialNote.reset();
    ctx.dragInitialSample.reset();
    ctx.dragRenderPinnedEntities.clear();
    ctx.dragSampleRenderPinnedEntities.clear();
    m_initialStates.clear();
    m_initialSampleStates.clear();
    m_usesUnifiedObjectDrag    = false;
    m_isSampleOffsetDrag       = false;
    m_hasLastAppliedDragTarget = false;
}

/// @brief 结束物件移动或局部编辑拖拽并提交动作。
/// @param ctx 会话上下文。
/// @param cmd 拖拽结束命令。
void GrabTool::handleEndDrag(SessionContext& ctx, const CmdEndDrag& cmd)
{
    (void)cmd;
    if ( ctx.draggedEntity == entt::null ) return;

    if ( m_usesUnifiedObjectDrag ||
         ctx.draggedObjectKind == ChartObjectKind::AudioSample ) {
        finishUnifiedDrag(ctx);
        return;
    }

    if ( m_isPolylineSubDrag && tryPolylineSubDragMerge(ctx) ) {
        m_isPolylineSubDrag        = false;
        m_hasLastAppliedDragTarget = false;
        clearSessionDragState(ctx);
        ctx.dragRenderPinnedEntities.clear();
        ctx.dragSampleRenderPinnedEntities.clear();
        ctx.draggedObjectKind = ChartObjectKind::PlayerNote;
        ctx.dragInitialSample.reset();
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

    clearSessionDragState(ctx);
    ctx.draggedEntity     = entt::null;
    ctx.draggedObjectKind = ChartObjectKind::PlayerNote;
    ctx.dragInitialNote   = std::nullopt;
    ctx.dragInitialSample.reset();
    m_hasLastAppliedDragTarget = false;
    ctx.dragRenderPinnedEntities.clear();
    ctx.dragSampleRenderPinnedEntities.clear();
    m_initialStates.clear();
    m_initialSampleStates.clear();
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

        const auto& sub       = note.m_subNotes[subNC.m_subIndex];
        subNC.m_type          = sub.type;
        subNC.m_timestamp     = sub.timestamp;
        subNC.m_duration      = sub.duration;
        subNC.m_trackIndex    = sub.trackIndex;
        subNC.m_dtrack        = sub.dtrack;
        subNC.m_isDraft       = note.m_isDraft;
        subNC.m_metadata      = sub.metadata;
        subNC.m_sampleBinding = sub.sampleBinding;
        subNC.m_customColors  = sub.customColors;
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
            after.m_isDraft = parentAfter.m_isDraft;

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
