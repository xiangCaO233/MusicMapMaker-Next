#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/TimelineAction.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMM::Logic
{

/// @brief 获取当前会话可用于轨道镜像的轨道数量。
int getMirrorTrackCount(const SessionContext& ctx)
{
    if ( ctx.currentBeatmap &&
         ctx.currentBeatmap->m_baseMapMetadata.track_count > 0 ) {
        return ctx.currentBeatmap->m_baseMapMetadata.track_count;
    }
    return ctx.trackCount;
}

/// @brief 对单个 NoteComponent 应用轨道镜像变换。
void mirrorNoteComponent(NoteComponent& note, int trackCount)
{
    if ( trackCount <= 0 ) return;

    note.m_trackIndex = (trackCount - 1) - note.m_trackIndex;
    if ( note.m_type == ::MMM::NoteType::FLICK ) {
        note.m_dtrack = -note.m_dtrack;
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( auto& sub : note.m_subNotes ) {
            sub.trackIndex = (trackCount - 1) - sub.trackIndex;
            if ( sub.type == ::MMM::NoteType::FLICK ) {
                sub.dtrack = -sub.dtrack;
            }
        }
    }
}

/// @brief 将调色盘命令颜色转换为 NoteColorOverrides。
NoteColorOverrides makeNoteColorOverrides(
    const std::array<glm::vec4, NOTE_COLOR_SLOT_COUNT>& colors)
{
    NoteColorOverrides overrides;
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteColorOverride(overrides, slot, colors[i]);
    }
    return overrides;
}

/// @brief 确保整体替换创建出的音符实体拥有基础辅助组件。
/// @param registry 目标 ECS 注册表。
/// @param entity 目标音符实体。
void ensureReplacementNoteAuxiliaryComponents(entt::registry& registry,
                                              entt::entity    entity)
{
    if ( !registry.all_of<TransformComponent>(entity) ) {
        registry.emplace<TransformComponent>(entity);
    }
    if ( !registry.all_of<InteractionComponent>(entity) ) {
        registry.emplace<InteractionComponent>(entity);
    }
}

/// @brief 标记整体替换后需要重建音符排序和统计缓存。
/// @param ctx 当前会话上下文。
void markReplacementNoteOrderDirty(SessionContext& ctx)
{
    ctx.isNoteOrderDirty = true;
    ctx.isNoteStatsDirty = true;
}

/// @brief 将折线子物件点击目标解析到父折线实体。
entt::entity resolveNoteColorTargetEntity(SessionContext& ctx,
                                          entt::entity    entity)
{
    if ( entity == entt::null || !ctx.noteRegistry.valid(entity) ||
         !ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
        return entt::null;
    }

    const auto& note = ctx.noteRegistry.get<NoteComponent>(entity);
    if ( note.m_isSubNote && note.m_parentPolyline != entt::null &&
         ctx.noteRegistry.valid(note.m_parentPolyline) &&
         ctx.noteRegistry.all_of<NoteComponent>(note.m_parentPolyline) ) {
        return note.m_parentPolyline;
    }
    return entity;
}

/// @brief 判断两个可选颜色是否完全相同。
bool isSameOptionalColor(const std::optional<glm::vec4>& lhs,
                         const std::optional<glm::vec4>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    if ( !lhs.has_value() ) return true;
    return lhs->r == rhs->r && lhs->g == rhs->g && lhs->b == rhs->b &&
           lhs->a == rhs->a;
}

/// @brief 判断两个音符配色覆写缓存是否完全相同。
bool isSameNoteColorOverrides(const NoteColorOverrides& lhs,
                              const NoteColorOverrides& rhs)
{
    return isSameOptionalColor(lhs.tap, rhs.tap) &&
           isSameOptionalColor(lhs.head, rhs.head) &&
           isSameOptionalColor(lhs.hold, rhs.hold) &&
           isSameOptionalColor(lhs.end, rhs.end) &&
           isSameOptionalColor(lhs.flickArrow, rhs.flickArrow) &&
           isSameOptionalColor(lhs.node, rhs.node);
}

/// @brief 为待删除折线父实体批量追加其子物件删除条目。
/// @param ctx 当前会话上下文。
/// @param deletedEntities 已经加入删除操作的实体集合。
/// @param entries 待追加的批量 note action 条目。
/// @warning 逻辑热路径低频分支：删除命令执行时最多完整扫描一次 note ECS，禁止按
/// 父折线数量重复扫描。
void appendDeletedPolylineChildren(
    SessionContext&                         ctx,
    const std::unordered_set<entt::entity>& deletedEntities,
    std::vector<BatchNoteAction::Entry>&    entries)
{
    if ( deletedEntities.empty() ) return;

    std::unordered_set<entt::entity> polylineParents;
    for ( auto entity : deletedEntities ) {
        if ( !ctx.noteRegistry.valid(entity) ||
             !ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
            continue;
        }

        const auto& note = ctx.noteRegistry.get<NoteComponent>(entity);
        if ( note.m_type == ::MMM::NoteType::POLYLINE &&
             !note.m_subNotes.empty() ) {
            polylineParents.insert(entity);
        }
    }
    if ( polylineParents.empty() ) return;

    std::unordered_set<entt::entity> existingEntries;
    existingEntries.reserve(entries.size());
    for ( const auto& entry : entries ) {
        if ( entry.entity != entt::null ) {
            existingEntries.insert(entry.entity);
        }
    }

    auto noteView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto entity : noteView ) {
        const auto& note = noteView.get<NoteComponent>(entity);
        if ( note.m_isSubNote &&
             polylineParents.find(note.m_parentPolyline) !=
                 polylineParents.end() &&
             existingEntries.insert(entity).second ) {
            entries.push_back({ entity, note, std::nullopt });
        }
    }
}

/// @brief 收集当前 Timeline Registry 中的全部事件并按时间排序。
std::vector<TimelineComponent> collectSortedTimelineComponents(
    SessionContext& ctx)
{
    std::vector<TimelineComponent> timelines;
    auto view = ctx.timelineRegistry.view<TimelineComponent>();
    for ( auto entity : view ) {
        timelines.push_back(view.get<TimelineComponent>(entity));
    }

    std::stable_sort(
        timelines.begin(),
        timelines.end(),
        [](const auto& lhs, const auto& rhs) {
            if ( std::abs(lhs.m_timestamp - rhs.m_timestamp) > 1e-9 ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            return static_cast<int>(lhs.m_effect) <
                   static_cast<int>(rhs.m_effect);
        });
    return timelines;
}

/// @brief 将指定 Timeline 列表整体写回当前会话。
void replaceTimelineComponents(SessionContext&                       ctx,
                               const std::vector<TimelineComponent>& timelines)
{
    std::vector<entt::entity> entities;
    auto view = ctx.timelineRegistry.view<TimelineComponent>();
    for ( auto entity : view ) {
        entities.push_back(entity);
    }
    for ( auto entity : entities ) {
        ctx.timelineRegistry.destroy(entity);
    }

    for ( const auto& timeline : timelines ) {
        auto entity = ctx.timelineRegistry.create();
        ctx.timelineRegistry.emplace<TimelineComponent>(entity, timeline);
    }

    ctx.m_needsTimingsSync = true;
    ctx.isBpmEventsDirty   = true;
    ctx.isTransformDirty   = true;
    ctx.isNoteStatsDirty   = true;
    if ( auto* cache =
             ctx.timelineRegistry.ctx().find<System::ScrollCache>() ) {
        cache->isDirty = true;
    }
}

/// @brief 归一化批量替换后的 Timeline 列表。
std::vector<TimelineComponent> normalizeReplacementTimelines(
    std::vector<TimelineComponent> timelines)
{
    std::erase_if(timelines, [](const auto& timeline) {
        return !std::isfinite(timeline.m_timestamp) ||
               !std::isfinite(timeline.m_value) ||
               (timeline.m_effect == ::MMM::TimingEffect::BPM &&
                timeline.m_value <= 0.0);
    });

    std::stable_sort(
        timelines.begin(),
        timelines.end(),
        [](const auto& lhs, const auto& rhs) {
            if ( std::abs(lhs.m_timestamp - rhs.m_timestamp) > 1e-9 ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            return static_cast<int>(lhs.m_effect) <
                   static_cast<int>(rhs.m_effect);
        });

    std::vector<TimelineComponent> normalized;
    normalized.reserve(timelines.size());
    for ( const auto& timeline : timelines ) {
        if ( timeline.m_effect == ::MMM::TimingEffect::BPM ) {
            auto duplicateIt = std::find_if(
                normalized.begin(),
                normalized.end(),
                [&](const auto& existing) {
                    return existing.m_effect == ::MMM::TimingEffect::BPM &&
                           std::abs(existing.m_timestamp -
                                    timeline.m_timestamp) < 1e-6;
                });
            if ( duplicateIt != normalized.end() ) {
                *duplicateIt = timeline;
                continue;
            }
        }
        normalized.push_back(timeline);
    }
    return normalized;
}

/// @brief 收集当前会话中可作为整体替换快照的物件组件。
/// @param ctx 当前会话上下文。
/// @return 非子物件的物件组件列表。
std::vector<NoteComponent> collectEditableNoteComponents(SessionContext& ctx)
{
    std::vector<NoteComponent> notes;
    auto                       view = ctx.noteRegistry.view<NoteComponent>();
    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;
        notes.push_back(note);
    }

    std::stable_sort(
        notes.begin(), notes.end(), [](const auto& lhs, const auto& rhs) {
            if ( std::abs(lhs.m_timestamp - rhs.m_timestamp) > 1e-9 ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            if ( lhs.m_trackIndex != rhs.m_trackIndex ) {
                return lhs.m_trackIndex < rhs.m_trackIndex;
            }
            return static_cast<int>(lhs.m_type) < static_cast<int>(rhs.m_type);
        });
    return notes;
}

/// @brief 从谱面 Note 构建 ECS 音符组件。
/// @param note 来源谱面物件。
/// @return 对应的 ECS 组件。
NoteComponent makeNoteComponentFromBeatmapNote(const ::MMM::Note& note)
{
    NoteComponent component;
    component.m_type       = note.m_type;
    component.m_timestamp  = note.m_timestamp / 1000.0;
    component.m_trackIndex = static_cast<int>(note.m_track);
    component.m_metadata   = note.m_metadata;

    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        component.m_duration =
            static_cast<const ::MMM::Hold&>(note).m_duration / 1000.0;
    } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
        component.m_dtrack = static_cast<const ::MMM::Flick&>(note).m_dtrack;
    }

    loadNoteColorOverridesFromMetadata(component);
    return component;
}

/// @brief 从谱面折线子物件构建 ECS 子物件组件。
/// @param note 来源谱面子物件。
/// @return 对应的 ECS 折线子物件。
NoteComponent::SubNote makeSubNoteComponentFromBeatmapNote(
    const ::MMM::Note& note)
{
    NoteComponent::SubNote subNote;
    subNote.type       = note.m_type;
    subNote.timestamp  = note.m_timestamp / 1000.0;
    subNote.duration   = 0.0;
    subNote.trackIndex = static_cast<int>(note.m_track);
    subNote.dtrack     = 0;
    subNote.metadata   = note.m_metadata;

    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        subNote.duration =
            static_cast<const ::MMM::Hold&>(note).m_duration / 1000.0;
    } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
        subNote.dtrack = static_cast<const ::MMM::Flick&>(note).m_dtrack;
    }

    loadNoteColorOverridesFromMetadata(subNote);
    return subNote;
}

/// @brief 收集谱面中已经被 Polyline 引用的子物件地址。
/// @param beatMap 来源谱面。
/// @return Polyline 子物件地址集合。
std::unordered_set<const ::MMM::Note*> collectBeatmapPolylineSubNotePointers(
    const ::MMM::BeatMap& beatMap)
{
    std::unordered_set<const ::MMM::Note*> subNotePointers;
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            subNotePointers.insert(&subNoteRef.get());
        }
    }
    return subNotePointers;
}

/// @brief 从谱面数据构建可整体替换到当前会话的物件组件列表。
/// @param beatMap 来源谱面。
/// @return 非子物件的物件组件列表。
std::vector<NoteComponent> makeNoteComponentsFromBeatMap(
    const ::MMM::BeatMap& beatMap)
{
    std::vector<NoteComponent> notes;
    notes.reserve(
        beatMap.m_noteData.notes.size() + beatMap.m_noteData.holds.size() +
        beatMap.m_noteData.flicks.size() + beatMap.m_noteData.polylines.size());

    const auto subNotePointers = collectBeatmapPolylineSubNotePointers(beatMap);
    for ( const auto& note : beatMap.m_noteData.notes ) {
        if ( note.m_isSubNote || subNotePointers.contains(&note) ) continue;
        notes.push_back(makeNoteComponentFromBeatmapNote(note));
    }
    for ( const auto& hold : beatMap.m_noteData.holds ) {
        if ( hold.m_isSubNote || subNotePointers.contains(&hold) ) continue;
        notes.push_back(makeNoteComponentFromBeatmapNote(hold));
    }
    for ( const auto& flick : beatMap.m_noteData.flicks ) {
        if ( flick.m_isSubNote || subNotePointers.contains(&flick) ) continue;
        notes.push_back(makeNoteComponentFromBeatmapNote(flick));
    }
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        auto component   = makeNoteComponentFromBeatmapNote(polyline);
        component.m_type = ::MMM::NoteType::POLYLINE;
        component.m_subNotes.clear();
        component.m_subNotes.reserve(polyline.m_subNotes.size());
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            component.m_subNotes.push_back(
                makeSubNoteComponentFromBeatmapNote(subNoteRef.get()));
        }
        if ( !component.m_subNotes.empty() ) {
            component.m_timestamp  = component.m_subNotes.front().timestamp;
            component.m_trackIndex = component.m_subNotes.front().trackIndex;
        }
        notes.push_back(std::move(component));
    }

    std::stable_sort(
        notes.begin(), notes.end(), [](const auto& lhs, const auto& rhs) {
            if ( std::abs(lhs.m_timestamp - rhs.m_timestamp) > 1e-9 ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            if ( lhs.m_trackIndex != rhs.m_trackIndex ) {
                return lhs.m_trackIndex < rhs.m_trackIndex;
            }
            return static_cast<int>(lhs.m_type) < static_cast<int>(rhs.m_type);
        });
    return notes;
}

/// @brief 从谱面 Timing 构建可替换到当前会话的 Timeline 组件。
/// @param beatMap 来源谱面。
/// @return Timeline 组件列表。
std::vector<TimelineComponent> makeTimelineComponentsFromBeatMap(
    const ::MMM::BeatMap& beatMap)
{
    std::vector<TimelineComponent> timelines;
    timelines.reserve(beatMap.m_timings.size());
    for ( const auto& timing : beatMap.m_timings ) {
        TimelineComponent timeline;
        timeline.m_timestamp = timing.m_timestamp / 1000.0;
        timeline.m_effect    = timing.m_timingEffect;
        timeline.m_value     = timing.m_timingEffectParameter;
        if ( timeline.m_effect == ::MMM::TimingEffect::BPM &&
             !(timeline.m_value > 0.0) ) {
            timeline.m_value = timing.m_bpm;
        }
        timeline.m_metadata = timing.m_metadata;
        timelines.push_back(std::move(timeline));
    }
    return normalizeReplacementTimelines(std::move(timelines));
}

/// @brief 整体重建当前会话的物件 ECS。
/// @param ctx 当前会话上下文。
/// @param notes 替换后的非子物件组件列表。
void replaceNoteComponents(SessionContext&                   ctx,
                           const std::vector<NoteComponent>& notes)
{
    ctx.noteRegistry.clear();
    for ( const auto& note : notes ) {
        auto entity = ctx.noteRegistry.create();
        ctx.noteRegistry.emplace<NoteComponent>(entity, note);
        ensureReplacementNoteAuxiliaryComponents(ctx.noteRegistry, entity);

        if ( note.m_type != ::MMM::NoteType::POLYLINE ) continue;

        for ( std::size_t index = 0; index < note.m_subNotes.size(); ++index ) {
            const auto&   sub = note.m_subNotes[index];
            NoteComponent subComponent;
            subComponent.m_type           = sub.type;
            subComponent.m_timestamp      = sub.timestamp;
            subComponent.m_duration       = sub.duration;
            subComponent.m_trackIndex     = sub.trackIndex;
            subComponent.m_dtrack         = sub.dtrack;
            subComponent.m_isSubNote      = true;
            subComponent.m_parentPolyline = entity;
            subComponent.m_subIndex       = static_cast<int>(index);
            subComponent.m_metadata       = sub.metadata;
            subComponent.m_customColors   = sub.customColors;

            auto subEntity = ctx.noteRegistry.create();
            ctx.noteRegistry.emplace<NoteComponent>(subEntity, subComponent);
            ensureReplacementNoteAuxiliaryComponents(ctx.noteRegistry,
                                                     subEntity);
        }
    }

    ctx.hoveredEntity       = entt::null;
    ctx.draggedEntity       = entt::null;
    ctx.draggedPart         = HoverPart::None;
    ctx.draggedSubIndex     = -1;
    ctx.isDragging          = false;
    ctx.isSelecting         = false;
    ctx.hasMarqueeSelection = false;
    ctx.marqueeBoxes.clear();
    ctx.m_needsNotesSync = true;
    SessionUtils::markHitEventsDirty(ctx);
    markReplacementNoteOrderDirty(ctx);
}

/// @brief 谱面元数据替换快照。
struct BeatmapMetadataSnapshot {
    /// @brief 基础谱面元数据。
    ::MMM::BaseMapMeta baseMeta;

    /// @brief 扩展谱面元数据。
    ::MMM::MapMetadata mapMetadata;
};

/// @brief 从当前谱面构建元数据快照。
/// @param beatMap 来源谱面。
/// @return 当前元数据快照。
BeatmapMetadataSnapshot makeMetadataSnapshot(const ::MMM::BeatMap& beatMap)
{
    return BeatmapMetadataSnapshot{
        .baseMeta    = beatMap.m_baseMapMetadata,
        .mapMetadata = beatMap.m_metadata,
    };
}

/// @brief 构建元数据替换后的快照，保留当前谱面的文件和资源路径。
/// @param current 当前谱面。
/// @param source 来源谱面。
/// @return 替换后的元数据快照。
BeatmapMetadataSnapshot makeReplacementMetadataSnapshot(
    const ::MMM::BeatMap& current, const ::MMM::BeatMap& source)
{
    auto base            = source.m_baseMapMetadata;
    base.map_path        = current.m_baseMapMetadata.map_path;
    base.main_audio_path = current.m_baseMapMetadata.main_audio_path;
    base.main_cover_path = current.m_baseMapMetadata.main_cover_path;
    base.cover_path      = current.m_baseMapMetadata.cover_path;
    return BeatmapMetadataSnapshot{
        .baseMeta    = std::move(base),
        .mapMetadata = source.m_metadata,
    };
}

/// @brief 将元数据快照应用到当前谱面。
/// @param ctx 当前会话上下文。
/// @param snapshot 元数据快照。
void applyMetadataSnapshot(SessionContext&                ctx,
                           const BeatmapMetadataSnapshot& snapshot)
{
    if ( !ctx.currentBeatmap ) return;

    ctx.currentBeatmap->m_baseMapMetadata = snapshot.baseMeta;
    ctx.currentBeatmap->m_metadata        = snapshot.mapMetadata;
    if ( snapshot.baseMeta.track_count > 0 ) {
        ctx.trackCount = snapshot.baseMeta.track_count;
    }
    ctx.isTransformDirty = true;
    ctx.isNoteStatsDirty = true;
}

/// @brief 从其他谱面替换当前谱面部分数据的可撤销动作。
class ReplaceBeatmapDataAction : public IEditorAction
{
public:
    /// @brief 构造数据替换动作。
    ReplaceBeatmapDataAction(bool replaceObjects, bool replaceTimelines,
                             bool                           replaceMetadata,
                             std::vector<NoteComponent>     beforeNotes,
                             std::vector<NoteComponent>     afterNotes,
                             std::vector<TimelineComponent> beforeTimelines,
                             std::vector<TimelineComponent> afterTimelines,
                             BeatmapMetadataSnapshot        beforeMetadata,
                             BeatmapMetadataSnapshot        afterMetadata,
                             double                         beforePreferenceBpm,
                             double                         afterPreferenceBpm)
        : m_replaceObjects(replaceObjects)
        , m_replaceTimelines(replaceTimelines)
        , m_replaceMetadata(replaceMetadata)
        , m_beforeNotes(std::move(beforeNotes))
        , m_afterNotes(std::move(afterNotes))
        , m_beforeTimelines(std::move(beforeTimelines))
        , m_afterTimelines(std::move(afterTimelines))
        , m_beforeMetadata(std::move(beforeMetadata))
        , m_afterMetadata(std::move(afterMetadata))
        , m_beforePreferenceBpm(beforePreferenceBpm)
        , m_afterPreferenceBpm(afterPreferenceBpm)
    {
    }

    /// @brief 执行替换。
    /// @param ctx 当前会话上下文。
    void execute(SessionContext& ctx) override { apply(ctx, true); }

    /// @brief 撤销替换。
    /// @param ctx 当前会话上下文。
    void undo(SessionContext& ctx) override { apply(ctx, false); }

    /// @brief 重做替换。
    /// @param ctx 当前会话上下文。
    void redo(SessionContext& ctx) override { execute(ctx); }

    /// @brief 获取动作名称。
    std::string getName() const override { return "Replace Beatmap Data"; }

private:
    /// @brief 应用指定方向的替换快照。
    /// @param ctx 当前会话上下文。
    /// @param forward 是否应用替换后的快照。
    void apply(SessionContext& ctx, bool forward)
    {
        if ( m_replaceObjects ) {
            replaceNoteComponents(ctx, forward ? m_afterNotes : m_beforeNotes);
        }
        if ( m_replaceTimelines ) {
            replaceTimelineComponents(
                ctx, forward ? m_afterTimelines : m_beforeTimelines);
            if ( ctx.currentBeatmap && !m_replaceMetadata ) {
                ctx.currentBeatmap->m_baseMapMetadata.preference_bpm =
                    forward ? m_afterPreferenceBpm : m_beforePreferenceBpm;
            }
        }
        if ( m_replaceMetadata ) {
            applyMetadataSnapshot(ctx,
                                  forward ? m_afterMetadata : m_beforeMetadata);
        }
    }

    /// @brief 是否替换物件数据。
    bool m_replaceObjects{ false };

    /// @brief 是否替换时间线数据。
    bool m_replaceTimelines{ false };

    /// @brief 是否替换谱面元数据。
    bool m_replaceMetadata{ false };

    /// @brief 替换前物件组件。
    std::vector<NoteComponent> m_beforeNotes;

    /// @brief 替换后物件组件。
    std::vector<NoteComponent> m_afterNotes;

    /// @brief 替换前 Timeline 组件。
    std::vector<TimelineComponent> m_beforeTimelines;

    /// @brief 替换后 Timeline 组件。
    std::vector<TimelineComponent> m_afterTimelines;

    /// @brief 替换前元数据。
    BeatmapMetadataSnapshot m_beforeMetadata;

    /// @brief 替换后元数据。
    BeatmapMetadataSnapshot m_afterMetadata;

    /// @brief 替换前首选 BPM。
    double m_beforePreferenceBpm{ 120.0 };

    /// @brief 替换后首选 BPM。
    double m_afterPreferenceBpm{ 120.0 };
};

/// @brief 批量替换 Timeline 的可撤销动作。
class ReplaceTimelinesAction : public IEditorAction
{
public:
    /// @brief 构造批量替换 Timeline 动作。
    /// @param before 替换前的 Timeline 列表。
    /// @param after 替换后的 Timeline 列表。
    /// @param beforePreferenceBpm 替换前的首选 BPM。
    /// @param afterPreferenceBpm 替换后的首选 BPM。
    ReplaceTimelinesAction(std::vector<TimelineComponent> before,
                           std::vector<TimelineComponent> after,
                           double                         beforePreferenceBpm,
                           double                         afterPreferenceBpm)
        : m_before(std::move(before))
        , m_after(std::move(after))
        , m_beforePreferenceBpm(beforePreferenceBpm)
        , m_afterPreferenceBpm(afterPreferenceBpm)
    {
    }

    /// @brief 执行批量替换。
    /// @param ctx 会话上下文引用。
    void execute(SessionContext& ctx) override
    {
        replaceTimelineComponents(ctx, m_after);
        if ( ctx.currentBeatmap ) {
            ctx.currentBeatmap->m_baseMapMetadata.preference_bpm =
                m_afterPreferenceBpm;
        }
    }

    /// @brief 撤销批量替换。
    /// @param ctx 会话上下文引用。
    void undo(SessionContext& ctx) override
    {
        replaceTimelineComponents(ctx, m_before);
        if ( ctx.currentBeatmap ) {
            ctx.currentBeatmap->m_baseMapMetadata.preference_bpm =
                m_beforePreferenceBpm;
        }
    }

    /// @brief 重做批量替换。
    /// @param ctx 会话上下文引用。
    void redo(SessionContext& ctx) override { execute(ctx); }

    /// @brief 获取动作名称。
    std::string getName() const override { return "Replace Timings"; }

private:
    /// @brief 替换前的 Timeline 列表。
    std::vector<TimelineComponent> m_before;

    /// @brief 替换后的 Timeline 列表。
    std::vector<TimelineComponent> m_after;

    /// @brief 替换前的谱面首选 BPM。
    double m_beforePreferenceBpm{ 120.0 };

    /// @brief 替换后的谱面首选 BPM。
    double m_afterPreferenceBpm{ 120.0 };
};

// --- Editing Handlers ---

void ActionController::handleCommand(const CmdUndo& cmd)
{
    m_ctx.actionStack.undo(m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

void ActionController::handleCommand(const CmdRedo& cmd)
{
    m_ctx.actionStack.redo(m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

void ActionController::handleCommand(const CmdCopy& cmd)
{
    m_ctx.clipboard.clear();
    auto view = m_ctx.noteRegistry.view<NoteComponent, InteractionComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            const auto& note = view.get<NoteComponent>(entity);
            m_ctx.clipboard.push_back({ note });
        }
    }
    EditorEngine::instance().setClipboard(m_ctx.clipboard, &m_ctx, false);
    XINFO("Copied {} items to clipboard", m_ctx.clipboard.size());
    m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                          TR("ui.status.category.clipboard"),
                                          TR("ui.status.clipboard.copied"),
                                          m_ctx.clipboard.size(),
                                          TR("ui.status.info.items"));
}

void ActionController::handleCommand(const CmdCut& cmd)
{
    handleCommand(CmdCopy{});
    auto view = m_ctx.noteRegistry.view<InteractionComponent>();
    for ( auto entity : view ) {
        auto& ic = m_ctx.noteRegistry.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            ic.isCut = true;
        }
    }
    EditorEngine::instance().setClipboard(m_ctx.clipboard, &m_ctx, true);
    m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                          TR("ui.status.category.clipboard"),
                                          TR("ui.status.clipboard.cut"),
                                          m_ctx.clipboard.size(),
                                          TR("ui.status.info.items"));
}

void ActionController::handleCommand(const CmdDeleteSelected& cmd)
{
    std::vector<BatchNoteAction::Entry> entries;

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            entries.push_back(
                { entity, view.get<NoteComponent>(entity), std::nullopt });
        }
    }

    // 如果没有任何选中的，但有悬停的，也删除悬停的 (符合习惯)
    if ( entries.empty() && m_ctx.hoveredEntity != entt::null ) {
        if ( m_ctx.noteRegistry.valid(m_ctx.hoveredEntity) &&
             m_ctx.noteRegistry.all_of<NoteComponent>(m_ctx.hoveredEntity) ) {
            entries.push_back(
                { m_ctx.hoveredEntity,
                  m_ctx.noteRegistry.get<NoteComponent>(m_ctx.hoveredEntity),
                  std::nullopt });
        }
    }

    // 收集所有被删除实体的 ID（用于后续查找子物件）
    std::unordered_set<entt::entity> deletedEntities;
    for ( const auto& entry : entries ) {
        deletedEntities.insert(entry.entity);
    }

    // 同时删除被删除折线下所有子物件实体，防止孤儿子实体残留
    appendDeletedPolylineChildren(m_ctx, deletedEntities, entries);

    if ( !entries.empty() ) {
        size_t count  = entries.size();
        auto   action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                          "Delete Selected");
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        XINFO("Deleted {} selected/hovered items", count);
    }
}

void ActionController::handleCommand(const CmdMirrorSelected& cmd)
{
    if ( !m_ctx.currentBeatmap ) return;
    int trackCount = getMirrorTrackCount(m_ctx);

    std::vector<BatchNoteAction::Entry> entries;
    std::unordered_set<entt::entity>    toMirror;

    // 1. 收集所有选中的物件
    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            toMirror.insert(entity);

            // 如果是 Polyline，收集其所有子物件实体
            const auto& nc = view.get<NoteComponent>(entity);
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) {
                for ( auto subEnt : m_ctx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == entity ) {
                        toMirror.insert(subEnt);
                    }
                }
            }
        }
    }

    // 2. 执行镜像逻辑
    for ( auto entity : toMirror ) {
        if ( !m_ctx.noteRegistry.valid(entity) ||
             !m_ctx.noteRegistry.all_of<NoteComponent>(entity) )
            continue;

        const auto& oldNote = m_ctx.noteRegistry.get<NoteComponent>(entity);
        auto        newNote = oldNote;

        mirrorNoteComponent(newNote, trackCount);

        entries.push_back({ entity, oldNote, newNote });
    }

    if ( !entries.empty() ) {
        size_t count  = entries.size();
        auto   action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                          "Mirror Selected");
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        XINFO("Mirrored {} items (including sub-notes)", count);

        m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                              TR("ui.status.category.action"),
                                              TR("ui.edit.mirror"),
                                              count,
                                              TR("ui.status.info.items"));
    }
}

void ActionController::handleCommand(const CmdApplyNoteColorToSelection& cmd)
{
    std::vector<BatchNoteAction::Entry> entries;

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( !ic.isSelected ) continue;

        const auto& oldNote = view.get<NoteComponent>(entity);
        if ( oldNote.m_isSubNote ) continue;

        auto newNote = oldNote;
        setNoteColorOverride(newNote, cmd.slot, cmd.color);
        entries.push_back({ entity, oldNote, newNote });
    }

    if ( entries.empty() ) return;

    auto actionName =
        cmd.color.has_value() ? "Set Note Color" : "Clear Note Color";
    auto action =
        std::make_unique<BatchNoteAction>(std::move(entries), actionName);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage =
        cmd.color.has_value() ? "Note color applied" : "Note color cleared";
}

void ActionController::handleCommand(const CmdApplyNotePaletteToSelection& cmd)
{
    std::vector<BatchNoteAction::Entry> entries;
    auto colors = makeNoteColorOverrides(cmd.colors);

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( !ic.isSelected ) continue;

        const auto& oldNote = view.get<NoteComponent>(entity);
        if ( oldNote.m_isSubNote ) continue;

        auto newNote = oldNote;
        applyNoteColorOverrides(newNote, colors);
        entries.push_back({ entity, oldNote, newNote });
    }

    if ( entries.empty() ) return;

    auto action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                    "Set Note Palette");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "Note palette applied";
}

void ActionController::handleCommand(const CmdApplyBrushPaletteToEntity& cmd)
{
    entt::entity target = resolveNoteColorTargetEntity(m_ctx, cmd.entity);
    if ( target == entt::null ) return;

    const auto& colors = m_ctx.brushState.customColors;
    if ( !hasAnyNoteColorOverride(colors) ) return;

    const auto& oldNote = m_ctx.noteRegistry.get<NoteComponent>(target);
    auto        newNote = oldNote;
    applyNoteColorOverrides(newNote, colors);
    if ( isSameNoteColorOverrides(oldNote.m_customColors,
                                  newNote.m_customColors) )
        return;

    std::vector<BatchNoteAction::Entry> entries;
    entries.push_back({ target, oldNote, newNote });

    auto action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                    "Set Note Palette");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "Note palette applied";
}

void ActionController::handleCommand(const CmdClearNoteColorOverrides& cmd)
{
    entt::entity target = resolveNoteColorTargetEntity(m_ctx, cmd.entity);
    if ( target == entt::null ) return;

    const auto&        oldNote = m_ctx.noteRegistry.get<NoteComponent>(target);
    auto               newNote = oldNote;
    NoteColorOverrides emptyColors;
    applyNoteColorOverrides(newNote, emptyColors);
    if ( isSameNoteColorOverrides(oldNote.m_customColors,
                                  newNote.m_customColors) )
        return;

    std::vector<BatchNoteAction::Entry> entries;
    entries.push_back({ target, oldNote, newNote });

    auto action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                    "Clear Note Palette");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "Note palette cleared";
}

void ActionController::handleCommand(const CmdPaste& cmd)
{
    auto clipboard = EditorEngine::instance().getClipboard();
    if ( clipboard.empty() ) {
        clipboard = m_ctx.clipboard;
    }
    if ( clipboard.empty() ) return;

    // 计算基准点 (目前取所有选中音符的最小时间)
    double minTime = clipboard[0].note.m_timestamp;
    for ( const auto& item : clipboard ) {
        minTime = std::min(minTime, item.note.m_timestamp);
    }

    std::vector<BatchNoteAction::Entry> entries;
    /// @brief 本次粘贴预先分配的新实体 ID 列表，用于动作执行后选中新物件。
    std::vector<entt::entity> pastedEntities;
    pastedEntities.reserve(clipboard.size());
    /// @brief 是否在粘贴完成后只保留新粘贴物件为选中状态。
    const bool selectPastedObjects = cmd.m_selectPastedObjects;

    // 1. 如果之前有 Cut，需要删除那些 Cut 的物件
    auto view       = m_ctx.noteRegistry.view<InteractionComponent>();
    bool isLocalCut = EditorEngine::instance().isClipboardCutFrom(&m_ctx);
    if ( isLocalCut ) {
        for ( auto entity : view ) {
            auto& ic = m_ctx.noteRegistry.get<InteractionComponent>(entity);
            if ( ic.isCut ) {
                if ( !m_ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                    continue;
                }

                auto oldNote = m_ctx.noteRegistry.get<NoteComponent>(entity);
                entries.push_back({ entity, oldNote, std::nullopt });

                // 同时删除 Polyline 的子物件实体
                if ( oldNote.m_type == ::MMM::NoteType::POLYLINE &&
                     !oldNote.m_subNotes.empty() ) {
                    for ( auto subEnt :
                          m_ctx.noteRegistry.view<NoteComponent>() ) {
                        const auto& subNC =
                            m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                        if ( subNC.m_isSubNote &&
                             subNC.m_parentPolyline == entity ) {
                            entries.push_back({ subEnt, subNC, std::nullopt });
                        }
                    }
                }
            }
        }
    } else {
        EditorEngine::instance().consumeCrossSessionCutClipboard(&m_ctx);
        for ( auto entity : view ) {
            m_ctx.noteRegistry.get<InteractionComponent>(entity).isCut = false;
        }
    }

    // 2. 粘贴到当前视觉时间 (判定线)
    double pasteTime = m_ctx.visualTime;

    // 尝试获取鼠标悬停处的时间作为基准 (如果有)
    // 注意：这里为了简化直接使用视觉时间。如果需要鼠标对齐，需要 UI 传入坐标。

    double timeOffset = pasteTime - minTime;

    int mirrorTrackCount = cmd.m_mirrored ? getMirrorTrackCount(m_ctx) : 0;
    for ( const auto& item : clipboard ) {
        auto newNote        = item.note;
        newNote.m_timestamp = item.note.m_timestamp + timeOffset;

        // 折线物件：同步偏移所有子物件的时间戳
        if ( newNote.m_type == ::MMM::NoteType::POLYLINE ) {
            for ( auto& sub : newNote.m_subNotes ) {
                sub.timestamp += timeOffset;
            }
        }

        if ( cmd.m_mirrored ) {
            mirrorNoteComponent(newNote, mirrorTrackCount);
        }

        /// @brief 为新粘贴物件预分配实体，避免执行后再从撤销栈动作反查实体。
        entt::entity pastedEntity = m_ctx.noteRegistry.create();
        pastedEntities.push_back(pastedEntity);
        entries.push_back({ pastedEntity, std::nullopt, newNote });
    }

    auto action = std::make_unique<BatchNoteAction>(
        std::move(entries), cmd.m_mirrored ? "Mirror Paste" : "Paste");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);

    if ( selectPastedObjects ) {
        m_ctx.isSelecting         = false;
        m_ctx.hasMarqueeSelection = false;
        m_ctx.marqueeIsAdditive   = false;
        m_ctx.marqueeBoxes.clear();
        // 先清空所有旧选择，再只选中本次粘贴创建出的实体。
        for ( auto entity : m_ctx.noteRegistry.view<InteractionComponent>() ) {
            m_ctx.noteRegistry.get<InteractionComponent>(entity).isSelected =
                false;
        }

        for ( auto entity : pastedEntities ) {
            if ( !m_ctx.noteRegistry.valid(entity) ||
                 !m_ctx.noteRegistry.all_of<InteractionComponent>(entity) )
                continue;

            m_ctx.noteRegistry.get<InteractionComponent>(entity).isSelected =
                true;
        }
    }

    // 清除剪切状态
    for ( auto entity : view ) {
        m_ctx.noteRegistry.get<InteractionComponent>(entity).isCut = false;
    }
    if ( isLocalCut ) {
        EditorEngine::instance().markCutClipboardConsumed();
    }
}

// --- Timeline Handlers ---

void ActionController::handleCommand(const CmdUpdateTimelineEvent& cmd)
{
    if ( m_ctx.timelineRegistry.valid(cmd.entity) ) {
        auto oldTl = m_ctx.timelineRegistry.get<TimelineComponent>(cmd.entity);
        auto newTl = oldTl;
        newTl.m_timestamp = cmd.newTime;
        newTl.m_value     = cmd.newValue;

        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Update, cmd.entity, oldTl, newTl);
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        m_ctx.isBpmEventsDirty = true;
    }
}

void ActionController::handleCommand(const CmdDeleteTimelineEvent& cmd)
{
    if ( m_ctx.timelineRegistry.valid(cmd.entity) ) {
        auto oldTl  = m_ctx.timelineRegistry.get<TimelineComponent>(cmd.entity);
        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Delete, cmd.entity, oldTl, std::nullopt);
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        m_ctx.isBpmEventsDirty = true;
    }
}

void ActionController::handleCommand(const CmdCreateTimelineEvent& cmd)
{
    TimelineComponent newTl{ cmd.time, cmd.type, cmd.value };
    auto              action = std::make_unique<TimelineAction>(
        TimelineAction::Type::Create, entt::null, std::nullopt, newTl);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

void ActionController::handleCommand(const CmdCreateTimelineEvents& cmd)
{
    if ( cmd.events.empty() ) {
        return;
    }

    std::vector<BatchTimelineAction::Entry> entries;
    entries.reserve(cmd.events.size());
    for ( const auto& event : cmd.events ) {
        entries.push_back(
            { entt::null,
              std::nullopt,
              TimelineComponent{ event.time, event.type, event.value } });
    }

    auto action =
        std::make_unique<BatchTimelineAction>(std::move(entries), "Paste");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

void ActionController::handleCommand(const CmdReplaceBeatmapTimings& cmd)
{
    if ( !m_ctx.currentBeatmap ) {
        return;
    }

    const std::vector<TimelineComponent> before =
        collectSortedTimelineComponents(m_ctx);
    std::vector<TimelineComponent> after;
    if ( cmd.keepNonBpmTimings ) {
        for ( const auto& timeline : before ) {
            if ( timeline.m_effect != ::MMM::TimingEffect::BPM ) {
                after.push_back(timeline);
            }
        }
    }

    double afterPreferenceBpm =
        m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm > 0.0
            ? m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : 120.0;
    bool hasBpm = false;
    for ( const auto& timing : cmd.timings ) {
        if ( timing.m_timingEffect != ::MMM::TimingEffect::BPM ) {
            continue;
        }

        double bpm = timing.m_timingEffectParameter > 0.0
                         ? timing.m_timingEffectParameter
                         : timing.m_bpm;
        if ( !(bpm > 0.0) || !std::isfinite(bpm) ||
             !std::isfinite(timing.m_timestamp) ) {
            continue;
        }

        bpm = std::clamp(bpm, 1.0, 999.0);
        TimelineComponent timeline;
        timeline.m_timestamp = timing.m_timestamp / 1000.0;
        timeline.m_effect    = ::MMM::TimingEffect::BPM;
        timeline.m_value     = bpm;
        timeline.m_metadata  = timing.m_metadata;
        after.push_back(timeline);
        if ( !hasBpm ) {
            afterPreferenceBpm = bpm;
            hasBpm             = true;
        }
    }

    if ( !hasBpm ) {
        return;
    }

    after = normalizeReplacementTimelines(std::move(after));
    const double beforePreferenceBpm =
        m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm > 0.0
            ? m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : afterPreferenceBpm;

    auto action = std::make_unique<ReplaceTimelinesAction>(
        before, after, beforePreferenceBpm, afterPreferenceBpm);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

void ActionController::handleCommand(const CmdReplaceBeatmapData& cmd)
{
    if ( !m_ctx.currentBeatmap || !cmd.sourceBeatmap ) {
        return;
    }
    if ( !cmd.replaceObjects && !cmd.replaceTimelines &&
         !cmd.replaceMetadata ) {
        return;
    }

    const auto normalizeBpm = [](double bpm) {
        return bpm > 0.0 && std::isfinite(bpm) ? bpm : 120.0;
    };

    std::vector<NoteComponent> beforeNotes;
    std::vector<NoteComponent> afterNotes;
    if ( cmd.replaceObjects ) {
        beforeNotes = collectEditableNoteComponents(m_ctx);
        afterNotes  = makeNoteComponentsFromBeatMap(*cmd.sourceBeatmap);
    }

    std::vector<TimelineComponent> beforeTimelines;
    std::vector<TimelineComponent> afterTimelines;
    if ( cmd.replaceTimelines ) {
        beforeTimelines = collectSortedTimelineComponents(m_ctx);
        afterTimelines  = makeTimelineComponentsFromBeatMap(*cmd.sourceBeatmap);
    }

    BeatmapMetadataSnapshot beforeMetadata =
        makeMetadataSnapshot(*m_ctx.currentBeatmap);
    BeatmapMetadataSnapshot afterMetadata = makeReplacementMetadataSnapshot(
        *m_ctx.currentBeatmap, *cmd.sourceBeatmap);

    const double beforePreferenceBpm =
        normalizeBpm(m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm);
    const double afterPreferenceBpm =
        normalizeBpm(cmd.sourceBeatmap->m_baseMapMetadata.preference_bpm);

    auto action =
        std::make_unique<ReplaceBeatmapDataAction>(cmd.replaceObjects,
                                                   cmd.replaceTimelines,
                                                   cmd.replaceMetadata,
                                                   std::move(beforeNotes),
                                                   std::move(afterNotes),
                                                   std::move(beforeTimelines),
                                                   std::move(afterTimelines),
                                                   std::move(beforeMetadata),
                                                   std::move(afterMetadata),
                                                   beforePreferenceBpm,
                                                   afterPreferenceBpm);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);

    m_ctx.lastActionMessage =
        fmt::format("{} {}", TR("ui.status.category.action"), "数据来源替换");
}

void ActionController::handleCommand(const CmdAlignSelectedToCommonBeats& cmd)
{
    if ( !m_ctx.currentBeatmap ) return;

    // Helper function to extract common beat divisors from the skin
    auto getCommonDivisorsFromSkin = []() -> std::vector<int> {
        return MMM::Config::SkinManager::instance().getCommonDivisors();
    };

    std::vector<int> commonDivisors = getCommonDivisorsFromSkin();

    // Gather BPM/Timing events
    auto tlView = m_ctx.timelineRegistry.view<const TimelineComponent>();
    std::vector<const TimelineComponent*> bpmEvents;
    for ( auto entity : tlView ) {
        const auto& tc = tlView.get<const TimelineComponent>(entity);
        if ( tc.m_effect == ::MMM::TimingEffect::BPM ) {
            bpmEvents.push_back(&tc);
        }
    }
    std::stable_sort(
        bpmEvents.begin(), bpmEvents.end(), [](const auto* a, const auto* b) {
            return a->m_timestamp < b->m_timestamp;
        });

    auto getAlignedTime = [&](double rawTime) -> double {
        if ( bpmEvents.empty() ) return rawTime;

        /// @brief 首个 BPM 前是否允许按绘制出的前置分拍线对齐。
        const bool allowBeforeFirstTiming =
            m_ctx.lastConfig.visual.drawBeatLinesBeforeFirstTiming;
        if ( rawTime < bpmEvents[0]->m_timestamp && !allowBeforeFirstTiming )
            return rawTime;

        double bestSnappedTime  = rawTime;
        double minWeightedError = std::numeric_limits<double>::max();

        // Find the timing event containing rawTime
        const TimelineComponent* currentBPM = nullptr;
        double                   bpmTime    = 0.0;
        double                   bpmVal     = 120.0;
        double nextBpmTime = std::numeric_limits<double>::infinity();

        if ( rawTime < bpmEvents.front()->m_timestamp ) {
            currentBPM  = bpmEvents.front();
            bpmTime     = currentBPM->m_timestamp;
            bpmVal      = currentBPM->m_value;
            nextBpmTime = bpmEvents.size() > 1
                              ? bpmEvents[1]->m_timestamp
                              : std::numeric_limits<double>::infinity();
        } else {
            for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                double tBpm  = bpmEvents[i]->m_timestamp;
                double tNext = (i + 1 < bpmEvents.size())
                                   ? bpmEvents[i + 1]->m_timestamp
                                   : std::numeric_limits<double>::infinity();
                if ( rawTime >= tBpm && rawTime < tNext ) {
                    currentBPM  = bpmEvents[i];
                    bpmTime     = tBpm;
                    bpmVal      = currentBPM->m_value;
                    nextBpmTime = tNext;
                    break;
                }
            }
        }

        if ( !currentBPM ) {
            currentBPM  = bpmEvents.back();
            bpmTime     = currentBPM->m_timestamp;
            bpmVal      = currentBPM->m_value;
            nextBpmTime = std::numeric_limits<double>::infinity();
        }

        double bVal = bpmVal;
        if ( bVal <= 0.0 ) {
            bVal = 120.0;
            if ( auto session = EditorEngine::instance().getActiveSession() ) {
                if ( auto beatmap = session->getContext().currentBeatmap ) {
                    if ( beatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
                        bVal = beatmap->m_baseMapMetadata.preference_bpm;
                    }
                }
            }
        }

        double beatDuration = 60.0 / bVal;

        for ( int divisor : commonDivisors ) {
            if ( divisor <= 0 ) continue;
            double stepDuration    = beatDuration / divisor;
            double relativeTime    = rawTime - bpmTime;
            double stepCount       = std::round(relativeTime / stepDuration);
            double nearestStepTime = bpmTime + stepCount * stepDuration;

            if ( nearestStepTime > nextBpmTime ) nearestStepTime = nextBpmTime;

            double error         = std::abs(rawTime - nearestStepTime);
            double weightedError = error * static_cast<double>(divisor);

            if ( weightedError < minWeightedError ) {
                minWeightedError = weightedError;
                bestSnappedTime  = nearestStepTime;
            }
        }

        return bestSnappedTime;
    };

    auto alignNote = [&](NoteComponent& nc) {
        double alignedStart = getAlignedTime(nc.m_timestamp);
        double alignedEnd   = getAlignedTime(nc.m_timestamp + nc.m_duration);
        nc.m_timestamp      = alignedStart;
        nc.m_duration       = std::max(0.0, alignedEnd - alignedStart);
    };

    std::unordered_set<entt::entity> toAlign;
    auto                             noteView =
        m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : noteView ) {
        const auto& ic = noteView.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            toAlign.insert(entity);
        }
    }

    // 闭包扩展：若 parent 在 toAlign 中，则其所有 subNotes 必须都在 toAlign
    // 中； 若任一 subNote 在 toAlign 中，则其 parent 及其所有 sibling subNotes
    // 必须都在 toAlign 中。
    bool expanded = true;
    while ( expanded ) {
        expanded = false;
        std::vector<entt::entity> currentToAlign(toAlign.begin(),
                                                 toAlign.end());
        for ( auto entity : currentToAlign ) {
            if ( !m_ctx.noteRegistry.valid(entity) ||
                 !m_ctx.noteRegistry.all_of<NoteComponent>(entity) )
                continue;

            const auto& nc = m_ctx.noteRegistry.get<NoteComponent>(entity);
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) {
                for ( auto subEnt : m_ctx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == entity ) {
                        if ( toAlign.insert(subEnt).second ) {
                            expanded = true;
                        }
                    }
                }
            } else if ( nc.m_isSubNote && nc.m_parentPolyline != entt::null ) {
                if ( toAlign.insert(nc.m_parentPolyline).second ) {
                    expanded = true;
                }
            }
        }
    }

    if ( toAlign.empty() ) return;

    std::vector<BatchNoteAction::Entry>             entries;
    std::unordered_map<entt::entity, NoteComponent> originalNotes;
    for ( auto entity : toAlign ) {
        if ( m_ctx.noteRegistry.valid(entity) &&
             m_ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
            originalNotes[entity] =
                m_ctx.noteRegistry.get<NoteComponent>(entity);
        }
    }

    std::unordered_map<entt::entity, NoteComponent> newNotes = originalNotes;

    // Align non-polyline notes and child subnotes
    for ( auto& [entity, newNote] : newNotes ) {
        if ( newNote.m_type != ::MMM::NoteType::POLYLINE ) {
            bool shouldAlign = false;
            if ( m_ctx.noteRegistry.all_of<InteractionComponent>(entity) &&
                 m_ctx.noteRegistry.get<InteractionComponent>(entity)
                     .isSelected ) {
                shouldAlign = true;
            } else if ( newNote.m_isSubNote &&
                        newNote.m_parentPolyline != entt::null ) {
                if ( toAlign.count(newNote.m_parentPolyline) ) {
                    shouldAlign = true;
                }
            } else {
                shouldAlign = true;
            }

            if ( shouldAlign ) {
                alignNote(newNote);
            }
        }
    }

    // Sync subnotes within parent polylines
    for ( auto& [entity, newNote] : newNotes ) {
        if ( newNote.m_type == ::MMM::NoteType::POLYLINE ) {
            struct ChildInfo {
                entt::entity entity;
                double       timestamp;
                int          originalSubIndex;
            };
            std::vector<ChildInfo> children;
            for ( const auto& [otherEnt, otherNote] : newNotes ) {
                if ( otherNote.m_isSubNote &&
                     otherNote.m_parentPolyline == entity ) {
                    children.push_back({ otherEnt,
                                         otherNote.m_timestamp,
                                         otherNote.m_subIndex });
                }
            }

            std::stable_sort(
                children.begin(),
                children.end(),
                [](const ChildInfo& a, const ChildInfo& b) {
                    if ( std::abs(a.timestamp - b.timestamp) < 1e-9 ) {
                        return a.originalSubIndex < b.originalSubIndex;
                    }
                    return a.timestamp < b.timestamp;
                });

            std::vector<NoteComponent::SubNote> newSubNotesList;
            newSubNotesList.reserve(newNote.m_subNotes.size());

            for ( size_t i = 0; i < children.size(); ++i ) {
                entt::entity childEnt = children[i].entity;
                int          oldIdx   = children[i].originalSubIndex;

                NoteComponent::SubNote updatedSub = newNote.m_subNotes[oldIdx];
                updatedSub.timestamp = newNotes[childEnt].m_timestamp;
                updatedSub.duration  = newNotes[childEnt].m_duration;

                newSubNotesList.push_back(updatedSub);
                newNotes[childEnt].m_subIndex = static_cast<int>(i);
            }

            newNote.m_subNotes = std::move(newSubNotesList);

            if ( !newNote.m_subNotes.empty() ) {
                newNote.m_timestamp  = newNote.m_subNotes.front().timestamp;
                newNote.m_trackIndex = newNote.m_subNotes.front().trackIndex;
            }
        }
    }

    // Generate BatchNoteAction entries
    for ( auto entity : toAlign ) {
        entries.push_back({ entity, originalNotes[entity], newNotes[entity] });
    }

    if ( !entries.empty() ) {
        size_t count  = entries.size();
        auto   action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                          "Align Selected");
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        XINFO("Aligned {} selected items to nearest common beat divisors",
              count);

        m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                              TR("ui.status.category.action"),
                                              TR("ui.tools.align_beats"),
                                              count,
                                              TR("ui.status.info.items"));
    }
}

}  // namespace MMM::Logic
