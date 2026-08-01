#include "config/Utf8Path.h"
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
#include "logic/session/SampleAction.h"
#include "logic/session/SelectionState.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/TimelineAction.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
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

/// @brief 判断新建物件是否允许落在谱面时间线上。
/// @param note 待创建物件。
/// @return 物件和所有折线子物件的时间戳均非负且有限时返回 true。
bool isPlaceableCreatedNote(const NoteComponent& note)
{
    if ( !std::isfinite(note.m_timestamp) || note.m_timestamp < 0.0 ) {
        return false;
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( const auto& subNote : note.m_subNotes ) {
            if ( !std::isfinite(subNote.timestamp) ||
                 subNote.timestamp < 0.0 ) {
                return false;
            }
        }
    }

    return true;
}

/// @brief 判断新建自动采样锚点是否允许落在谱面时间线上。
/// @param sample 待创建自动采样。
/// @return 锚点时间非负且有限、音量合法时返回 true；资源可为空以表示静音草稿。
bool isPlaceableCreatedSample(const SampleComponent& sample)
{
    return std::isfinite(sample.m_timestamp) && sample.m_timestamp >= 0.0 &&
           std::isfinite(sample.m_volume);
}

/// @brief 移除 Malody timing 拍位缓存。
/// @param metadata 待修改的 Timing 元数据。
void clearMalodyTimingBeatMetadata(::MMM::TimingMetadata& metadata)
{
    auto sourceIt =
        metadata.timing_properties.find(::MMM::TimingMetadataType::MALODY);
    if ( sourceIt == metadata.timing_properties.end() ) {
        return;
    }

    sourceIt->second.erase("beat");
    if ( sourceIt->second.empty() ) {
        metadata.timing_properties.erase(sourceIt);
    }
}

/// @brief 复制粘贴分拍换算使用的 BPM 时间点。
struct ClipboardBeatTimelinePoint {
    double timestamp{ 0.0 };  ///< BPM 时间点，单位秒
    double bpm{ 120.0 };      ///< 当前段 BPM
    double beat{ 0.0 };       ///< 该时间点对应的连续拍数
};

/// @brief 复制粘贴分拍换算用的连续 BPM 时间线。
using ClipboardBeatTimeline = std::vector<ClipboardBeatTimelinePoint>;

/// @brief 获取复制粘贴分拍换算使用的默认 BPM。
/// @param ctx 当前会话上下文。
/// @return 有效首选 BPM，缺失时返回 120。
double getClipboardFallbackBpm(const SessionContext& ctx)
{
    if ( ctx.currentBeatmap ) {
        double bpm = ctx.currentBeatmap->m_baseMapMetadata.preference_bpm;
        if ( std::isfinite(bpm) && bpm > 0.0 ) return bpm;
    }
    return 120.0;
}

/// @brief 规整 BPM 值，保证分拍换算不会除以零或使用非数值。
double sanitizeClipboardBpm(double bpm, double fallbackBpm)
{
    if ( std::isfinite(bpm) && bpm > 0.0 ) return bpm;
    if ( std::isfinite(fallbackBpm) && fallbackBpm > 0.0 ) {
        return fallbackBpm;
    }
    return 120.0;
}

/// @brief 从当前 BPM 缓存构建可双向换算的连续 beat 时间线。
/// @param ctx 当前会话上下文。
/// @param fallbackBpm BPM 无效时使用的默认 BPM。
/// @return 按时间排序的 BPM/beat 锚点列表。
/// @warning 低频编辑路径：复制或按分拍粘贴时调用，允许在 BPM 脏时重建缓存。
ClipboardBeatTimeline buildClipboardBeatTimeline(SessionContext& ctx,
                                                 double          fallbackBpm)
{
    SessionUtils::ensureBpmEvents(ctx);

    ClipboardBeatTimeline timeline;
    timeline.reserve(ctx.bpmEvents.size());
    for ( const auto* event : ctx.bpmEvents ) {
        if ( !event || !std::isfinite(event->m_timestamp) ) continue;

        const double bpm = sanitizeClipboardBpm(event->m_value, fallbackBpm);
        if ( timeline.empty() ) {
            timeline.push_back({ event->m_timestamp, bpm, 0.0 });
            continue;
        }

        const auto&  previous = timeline.back();
        const double beat =
            previous.beat +
            (event->m_timestamp - previous.timestamp) * previous.bpm / 60.0;
        timeline.push_back({ event->m_timestamp, bpm, beat });
    }
    return timeline;
}

/// @brief 将秒时间转换为连续 beat 位置。
double clipboardTimeToBeat(const ClipboardBeatTimeline& timeline,
                           double timestamp, double fallbackBpm)
{
    if ( !std::isfinite(timestamp) ) return 0.0;

    const double bpm = sanitizeClipboardBpm(fallbackBpm, 120.0);
    if ( timeline.empty() ) {
        return timestamp * bpm / 60.0;
    }

    auto it = std::upper_bound(
        timeline.begin(),
        timeline.end(),
        timestamp,
        [](double value, const ClipboardBeatTimelinePoint& point) {
            return value < point.timestamp;
        });
    const auto& point = it == timeline.begin() ? timeline.front() : *(it - 1);
    return point.beat + (timestamp - point.timestamp) * point.bpm / 60.0;
}

/// @brief 将连续 beat 位置转换为秒时间。
double clipboardBeatToTime(const ClipboardBeatTimeline& timeline, double beat,
                           double fallbackBpm)
{
    if ( !std::isfinite(beat) ) return 0.0;

    const double bpm = sanitizeClipboardBpm(fallbackBpm, 120.0);
    if ( timeline.empty() ) {
        return beat * 60.0 / bpm;
    }

    auto it = std::upper_bound(
        timeline.begin(),
        timeline.end(),
        beat,
        [](double value, const ClipboardBeatTimelinePoint& point) {
            return value < point.beat;
        });
    const auto& point = it == timeline.begin() ? timeline.front() : *(it - 1);
    return point.timestamp + (beat - point.beat) * 60.0 / point.bpm;
}

/// @brief 为剪贴板条目记录复制瞬间的 beat 位置。
void populateClipboardBeatPositions(ClipboardItem&               item,
                                    const ClipboardBeatTimeline& timeline,
                                    double                       fallbackBpm)
{
    item.startBeat =
        clipboardTimeToBeat(timeline, item.note.m_timestamp, fallbackBpm);
    item.endBeat = clipboardTimeToBeat(
        timeline, item.note.m_timestamp + item.note.m_duration, fallbackBpm);
    item.subStartBeats.clear();
    item.subEndBeats.clear();

    if ( item.note.m_type == ::MMM::NoteType::POLYLINE ) {
        item.subStartBeats.reserve(item.note.m_subNotes.size());
        item.subEndBeats.reserve(item.note.m_subNotes.size());
        for ( const auto& sub : item.note.m_subNotes ) {
            item.subStartBeats.push_back(
                clipboardTimeToBeat(timeline, sub.timestamp, fallbackBpm));
            item.subEndBeats.push_back(clipboardTimeToBeat(
                timeline, sub.timestamp + sub.duration, fallbackBpm));
        }
    }

    item.hasBeatPositions = true;
}

/// @brief 为自动采样剪贴板条目记录复制瞬间的 beat 锚点。
/// @param item 待写入拍位的自动采样条目。
/// @param timeline 复制来源的连续 BPM 时间线。
/// @param fallbackBpm BPM 缺失时的默认值。
void populateSampleClipboardBeatPosition(SampleClipboardItem&         item,
                                         const ClipboardBeatTimeline& timeline,
                                         double fallbackBpm)
{
    item.startBeat =
        clipboardTimeToBeat(timeline, item.sample.m_timestamp, fallbackBpm);
    item.hasBeatPosition = true;
}

/// @brief 按 beat 偏移将剪贴板条目落到新的粘贴时间。
void applyBeatPastePosition(NoteComponent& note, const ClipboardItem& item,
                            const ClipboardBeatTimeline& timeline,
                            double fallbackBpm, double pasteBeat,
                            double minBeat)
{
    const double startBeat = pasteBeat + item.startBeat - minBeat;
    const double endBeat   = pasteBeat + item.endBeat - minBeat;
    note.m_timestamp = clipboardBeatToTime(timeline, startBeat, fallbackBpm);
    const double endTime = clipboardBeatToTime(timeline, endBeat, fallbackBpm);
    note.m_duration      = std::max(0.0, endTime - note.m_timestamp);

    if ( note.m_type != ::MMM::NoteType::POLYLINE ||
         item.subStartBeats.size() != note.m_subNotes.size() ||
         item.subEndBeats.size() != note.m_subNotes.size() ) {
        return;
    }

    for ( std::size_t i = 0; i < note.m_subNotes.size(); ++i ) {
        auto&        sub          = note.m_subNotes[i];
        const double subStartBeat = pasteBeat + item.subStartBeats[i] - minBeat;
        const double subEndBeat   = pasteBeat + item.subEndBeats[i] - minBeat;
        sub.timestamp =
            clipboardBeatToTime(timeline, subStartBeat, fallbackBpm);
        const double subEndTime =
            clipboardBeatToTime(timeline, subEndBeat, fallbackBpm);
        sub.duration = std::max(0.0, subEndTime - sub.timestamp);
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
    component.m_type          = note.m_type;
    component.m_timestamp     = note.m_timestamp / 1000.0;
    component.m_trackIndex    = static_cast<int>(note.m_track);
    component.m_metadata      = note.m_metadata;
    component.m_sampleBinding = note.getSampleBinding();

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
    subNote.type          = note.m_type;
    subNote.timestamp     = note.m_timestamp / 1000.0;
    subNote.duration      = 0.0;
    subNote.trackIndex    = static_cast<int>(note.m_track);
    subNote.dtrack        = 0;
    subNote.metadata      = note.m_metadata;
    subNote.sampleBinding = note.getSampleBinding();

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
    clearChartObjectSelectionIndex(ctx, ChartObjectKind::PlayerNote);
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
            subComponent.m_sampleBinding  = sub.sampleBinding;
            subComponent.m_customColors   = sub.customColors;

            auto subEntity = ctx.noteRegistry.create();
            ctx.noteRegistry.emplace<NoteComponent>(subEntity, subComponent);
            ensureReplacementNoteAuxiliaryComponents(ctx.noteRegistry,
                                                     subEntity);
        }
    }

    ctx.hoveredEntity     = entt::null;
    ctx.hoveredObjectKind = ChartObjectKind::PlayerNote;
    ctx.draggedEntity     = entt::null;
    ctx.draggedObjectKind = ChartObjectKind::PlayerNote;
    ctx.draggedPart       = HoverPart::None;
    ctx.draggedSubIndex   = -1;
    ctx.dragInitialNote.reset();
    ctx.dragInitialSample.reset();
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

/// @brief 构建元数据替换后的快照，保留当前谱面的文件、资源路径及背景语义。
/// @param current 当前谱面。
/// @param source 来源谱面。
/// @return 替换后的元数据快照。
BeatmapMetadataSnapshot makeReplacementMetadataSnapshot(
    const ::MMM::BeatMap& current, const ::MMM::BeatMap& source)
{
    auto base     = source.m_baseMapMetadata;
    base.map_path = current.m_baseMapMetadata.map_path;
    base.main_audio_path.clear();
    base.song_file_hint  = current.m_baseMapMetadata.song_file_hint;
    base.bgm_track_count = current.m_baseMapMetadata.bgm_track_count;
    base.main_cover_path = current.m_baseMapMetadata.main_cover_path;
    base.cover_path      = current.m_baseMapMetadata.cover_path;
    base.cover_type      = current.m_baseMapMetadata.cover_type;
    base.video_starttime = current.m_baseMapMetadata.video_starttime;
    base.bgxoffset       = current.m_baseMapMetadata.bgxoffset;
    base.bgyoffset       = current.m_baseMapMetadata.bgyoffset;

    auto mapMetadata = source.m_metadata;
    if ( auto osuIt =
             mapMetadata.map_properties.find(::MMM::MapMetadataType::OSU);
         osuIt != mapMetadata.map_properties.end() ) {
        // 原始 OSU 属性也必须与保留的资源一致，避免元数据编辑器反向覆盖。
        osuIt->second["General::AudioFilename"] =
            Config::pathToUtf8(base.song_file_hint);
        osuIt->second["Events::background"] =
            base.cover_type == ::MMM::CoverType::VIDEO
                ? fmt::format("Video,{},\"{}\"",
                              base.video_starttime,
                              Config::pathToUtf8(base.main_cover_path))
                : fmt::format("0,0,\"{}\",{},{}",
                              Config::pathToUtf8(base.main_cover_path),
                              base.bgxoffset,
                              base.bgyoffset);
    }
    return BeatmapMetadataSnapshot{
        .baseMeta    = std::move(base),
        .mapMetadata = std::move(mapMetadata),
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
    ctx.trackCount    = std::max(1, snapshot.baseMeta.track_count);
    ctx.bgmTrackCount = std::max(0, snapshot.baseMeta.bgm_track_count);
    ctx.currentBeatmap->m_baseMapMetadata.track_count     = ctx.trackCount;
    ctx.currentBeatmap->m_baseMapMetadata.bgm_track_count = ctx.bgmTrackCount;
    ctx.isTransformDirty                                  = true;
    ctx.isNoteStatsDirty                                  = true;
}

/// @brief 从其他谱面替换当前谱面部分数据的可撤销动作。
class ReplaceBeatmapDataAction : public IEditorAction
{
public:
    /// @brief 构造数据替换动作。
    /// @param replaceObjects 是否替换玩家物件。
    /// @param replaceTimelines 是否替换 Timing。
    /// @param replaceMetadata 是否替换谱面元数据。
    /// @param beforeNotes 替换前的玩家物件快照。
    /// @param afterNotes 替换后的玩家物件快照。
    /// @param beforeTimelines 替换前的 Timeline 快照。
    /// @param afterTimelines 替换后的 Timeline 快照。
    /// @param beforeMetadata 替换前的元数据快照。
    /// @param afterMetadata 替换后的元数据快照。
    /// @param sampleTrackChanges 玩家轨道数变化对应的自动采样轨道迁移表。
    /// @param beforePreferenceBpm 替换前的首选 BPM。
    /// @param afterPreferenceBpm 替换后的首选 BPM。
    ReplaceBeatmapDataAction(
        bool replaceObjects, bool replaceTimelines, bool replaceMetadata,
        std::vector<NoteComponent>                       beforeNotes,
        std::vector<NoteComponent>                       afterNotes,
        std::vector<TimelineComponent>                   beforeTimelines,
        std::vector<TimelineComponent>                   afterTimelines,
        BeatmapMetadataSnapshot                          beforeMetadata,
        BeatmapMetadataSnapshot                          afterMetadata,
        std::vector<TrackCountAction::SampleTrackChange> sampleTrackChanges,
        double beforePreferenceBpm, double afterPreferenceBpm)
        : m_replaceObjects(replaceObjects)
        , m_replaceTimelines(replaceTimelines)
        , m_replaceMetadata(replaceMetadata)
        , m_beforeNotes(std::move(beforeNotes))
        , m_afterNotes(std::move(afterNotes))
        , m_beforeTimelines(std::move(beforeTimelines))
        , m_afterTimelines(std::move(afterTimelines))
        , m_beforeMetadata(std::move(beforeMetadata))
        , m_afterMetadata(std::move(afterMetadata))
        , m_sampleTrackChanges(std::move(sampleTrackChanges))
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
            for ( const auto& change : m_sampleTrackChanges ) {
                if ( !ctx.sampleRegistry.valid(change.entity) ||
                     !ctx.sampleRegistry.all_of<SampleComponent>(
                         change.entity) ) {
                    continue;
                }
                ctx.sampleRegistry.get<SampleComponent>(change.entity).m_track =
                    forward ? change.afterTrack : change.beforeTrack;
            }
            ctx.m_needsSamplesSync = true;
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

    /// @brief 玩家轨道数变化时自动采样绝对轨道的双向迁移表。
    std::vector<TrackCountAction::SampleTrackChange> m_sampleTrackChanges;

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

// --- 编辑命令处理 ---

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
    m_ctx.sampleClipboard.clear();
    const double fallbackBpm  = getClipboardFallbackBpm(m_ctx);
    auto         beatTimeline = buildClipboardBeatTimeline(m_ctx, fallbackBpm);
    auto view = m_ctx.noteRegistry.view<NoteComponent, InteractionComponent>();
    for ( auto entity : view ) {
        const auto& ic   = view.get<InteractionComponent>(entity);
        const auto& note = view.get<NoteComponent>(entity);
        if ( ic.isSelected &&
             SessionUtils::isNoteEditable(note, m_ctx.lastConfig.settings) ) {
            ClipboardItem item;
            item.note = note;
            populateClipboardBeatPositions(item, beatTimeline, fallbackBpm);
            m_ctx.clipboard.push_back(std::move(item));
        }
    }
    const std::uint32_t playerTrackCount =
        static_cast<std::uint32_t>(std::max(m_ctx.trackCount, 0));
    auto sampleView =
        m_ctx.sampleRegistry.view<SampleComponent, InteractionComponent>();
    for ( auto entity : sampleView ) {
        const auto& interaction = sampleView.get<InteractionComponent>(entity);
        if ( !interaction.isSelected ) continue;

        SampleClipboardItem item;
        item.sample         = sampleView.get<SampleComponent>(entity);
        item.bgmLane        = item.sample.m_track >= playerTrackCount
                                  ? item.sample.m_track - playerTrackCount
                                  : 0U;
        item.sample.m_track = item.bgmLane;
        populateSampleClipboardBeatPosition(item, beatTimeline, fallbackBpm);
        m_ctx.sampleClipboard.push_back(std::move(item));
    }

    EditorEngine::instance().setChartObjectClipboard(
        m_ctx.clipboard, m_ctx.sampleClipboard, &m_ctx, false);
    const std::size_t itemCount =
        m_ctx.clipboard.size() + m_ctx.sampleClipboard.size();
    XINFO("Copied {} chart objects to clipboard", itemCount);
    m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                          TR("ui.status.category.clipboard"),
                                          TR("ui.status.clipboard.copied"),
                                          itemCount,
                                          TR("ui.status.info.items"));
}

void ActionController::handleCommand(const CmdCut& cmd)
{
    handleCommand(CmdCopy{});
    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        auto&       ic   = m_ctx.noteRegistry.get<InteractionComponent>(entity);
        const auto& note = view.get<NoteComponent>(entity);
        if ( ic.isSelected &&
             SessionUtils::isNoteEditable(note, m_ctx.lastConfig.settings) ) {
            ic.isCut = true;
        }
    }
    auto sampleView = m_ctx.sampleRegistry.view<InteractionComponent>();
    for ( auto entity : sampleView ) {
        auto& interaction =
            m_ctx.sampleRegistry.get<InteractionComponent>(entity);
        if ( interaction.isSelected ) {
            interaction.isCut = true;
        }
    }
    EditorEngine::instance().setChartObjectClipboard(
        m_ctx.clipboard, m_ctx.sampleClipboard, &m_ctx, true);
    const std::size_t itemCount =
        m_ctx.clipboard.size() + m_ctx.sampleClipboard.size();
    m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                          TR("ui.status.category.clipboard"),
                                          TR("ui.status.clipboard.cut"),
                                          itemCount,
                                          TR("ui.status.info.items"));
}

void ActionController::handleCommand(const CmdDeleteSelected& cmd)
{
    std::vector<BatchNoteAction::Entry>   entries;
    std::vector<BatchSampleAction::Entry> sampleEntries;

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic   = view.get<InteractionComponent>(entity);
        const auto& note = view.get<NoteComponent>(entity);
        if ( ic.isSelected &&
             SessionUtils::isNoteEditable(note, m_ctx.lastConfig.settings) ) {
            entries.push_back({ entity, note, std::nullopt });
        }
    }

    auto sampleView =
        m_ctx.sampleRegistry.view<InteractionComponent, SampleComponent>();
    for ( auto entity : sampleView ) {
        const auto& interaction = sampleView.get<InteractionComponent>(entity);
        if ( interaction.isSelected ) {
            sampleEntries.push_back({
                entity,
                sampleView.get<SampleComponent>(entity),
                std::nullopt,
            });
        }
    }

    // 如果没有任何选中的，但有悬停的，也删除悬停的 (符合习惯)
    if ( entries.empty() && sampleEntries.empty() &&
         m_ctx.hoveredEntity != entt::null ) {
        if ( m_ctx.hoveredObjectKind == ChartObjectKind::PlayerNote &&
             m_ctx.noteRegistry.valid(m_ctx.hoveredEntity) &&
             m_ctx.noteRegistry.all_of<NoteComponent>(m_ctx.hoveredEntity) &&
             SessionUtils::isNoteEditable(
                 m_ctx.noteRegistry.get<const NoteComponent>(
                     m_ctx.hoveredEntity),
                 m_ctx.lastConfig.settings) ) {
            entries.push_back(
                { m_ctx.hoveredEntity,
                  m_ctx.noteRegistry.get<NoteComponent>(m_ctx.hoveredEntity),
                  std::nullopt });
        } else if ( m_ctx.hoveredObjectKind == ChartObjectKind::AudioSample &&
                    m_ctx.sampleRegistry.valid(m_ctx.hoveredEntity) &&
                    m_ctx.sampleRegistry.all_of<SampleComponent>(
                        m_ctx.hoveredEntity) ) {
            sampleEntries.push_back({
                m_ctx.hoveredEntity,
                m_ctx.sampleRegistry.get<SampleComponent>(m_ctx.hoveredEntity),
                std::nullopt,
            });
        }
    }

    // 收集所有被删除实体的 ID（用于后续查找子物件）
    std::unordered_set<entt::entity> deletedEntities;
    for ( const auto& entry : entries ) {
        deletedEntities.insert(entry.entity);
    }

    // 同时删除被删除折线下所有子物件实体，防止孤儿子实体残留
    appendDeletedPolylineChildren(m_ctx, deletedEntities, entries);

    const size_t count = entries.size() + sampleEntries.size();
    if ( count > 0 ) {
        std::vector<std::unique_ptr<IEditorAction>> actions;
        actions.reserve(2);
        if ( !entries.empty() ) {
            actions.push_back(std::make_unique<BatchNoteAction>(
                std::move(entries), "Delete Selected"));
        }
        if ( !sampleEntries.empty() ) {
            actions.push_back(std::make_unique<BatchSampleAction>(
                std::move(sampleEntries), "删除已选自动采样"));
        }

        std::unique_ptr<IEditorAction> action;
        if ( actions.size() == 1 ) {
            action = std::move(actions.front());
        } else {
            action = std::make_unique<CompositeEditorAction>(
                std::move(actions), "删除已选谱面物件");
        }
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
        const auto& nc = view.get<NoteComponent>(entity);
        if ( ic.isSelected &&
             SessionUtils::isNoteEditable(nc, m_ctx.lastConfig.settings) ) {
            toMirror.insert(entity);

            // 如果是 Polyline，收集其所有子物件实体
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
        if ( oldNote.m_isSubNote || !SessionUtils::isNoteEditable(
                                        oldNote, m_ctx.lastConfig.settings) ) {
            continue;
        }

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
        if ( oldNote.m_isSubNote || !SessionUtils::isNoteEditable(
                                        oldNote, m_ctx.lastConfig.settings) ) {
            continue;
        }

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
    if ( !SessionUtils::isNoteEditable(oldNote, m_ctx.lastConfig.settings) ) {
        return;
    }
    auto newNote = oldNote;
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

    const auto& oldNote = m_ctx.noteRegistry.get<NoteComponent>(target);
    if ( !SessionUtils::isNoteEditable(oldNote, m_ctx.lastConfig.settings) ) {
        return;
    }
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

/// @brief 粘贴剪贴板中的物件，并拒绝会落到负时间的创建结果。
/// @param cmd 粘贴指令。
/// @warning 低频编辑路径：用户触发粘贴时执行，可能批量创建实体。
void ActionController::handleCommand(const CmdPaste& cmd)
{
    auto noteClipboard     = EditorEngine::instance().getClipboard();
    auto sampleClipboard   = EditorEngine::instance().getSampleClipboard();
    auto timelineClipboard = EditorEngine::instance().getTimelineClipboard();
    if ( noteClipboard.empty() && sampleClipboard.empty() &&
         timelineClipboard.empty() ) {
        noteClipboard   = m_ctx.clipboard;
        sampleClipboard = m_ctx.sampleClipboard;
    }
    std::erase_if(noteClipboard, [&](const auto& item) {
        return !SessionUtils::isNoteEditable(item.note,
                                             m_ctx.lastConfig.settings);
    });
    if ( noteClipboard.empty() && sampleClipboard.empty() &&
         timelineClipboard.empty() ) {
        return;
    }

    // 先预计算粘贴目标，确保不会在负时间创建新物件。
    double pasteTime = m_ctx.animateTime;

    if ( !noteClipboard.empty() || !sampleClipboard.empty() ) {
        // Note 与 Sample 共用同一个最早锚点，保持混合编排的相对时间。
        double minTime = std::numeric_limits<double>::infinity();
        double minBeat = std::numeric_limits<double>::infinity();
        for ( const auto& item : noteClipboard ) {
            minTime = std::min(minTime, item.note.m_timestamp);
            minBeat = std::min(minBeat, item.startBeat);
        }
        for ( const auto& item : sampleClipboard ) {
            minTime = std::min(minTime, item.sample.m_timestamp);
            minBeat = std::min(minBeat, item.startBeat);
        }

        const double timeOffset = pasteTime - minTime;
        const bool   pasteByBeat =
            m_ctx.lastConfig.settings.copyPasteTimeBasis ==
                Config::CopyPasteTimeBasis::Beat &&
            std::all_of(
                noteClipboard.begin(),
                noteClipboard.end(),
                [](const auto& item) { return item.hasBeatPositions; }) &&
            std::all_of(sampleClipboard.begin(),
                        sampleClipboard.end(),
                        [](const auto& item) { return item.hasBeatPosition; });
        const double pasteFallbackBpm = getClipboardFallbackBpm(m_ctx);
        auto         pasteBeatTimeline =
            pasteByBeat ? buildClipboardBeatTimeline(m_ctx, pasteFallbackBpm)
                        : ClipboardBeatTimeline{};
        const double pasteBeat =
            pasteByBeat ? clipboardTimeToBeat(
                              pasteBeatTimeline, pasteTime, pasteFallbackBpm)
                        : 0.0;

        int mirrorTrackCount = cmd.m_mirrored ? getMirrorTrackCount(m_ctx) : 0;
        std::vector<NoteComponent> notesToPaste;
        notesToPaste.reserve(noteClipboard.size());
        for ( const auto& item : noteClipboard ) {
            auto newNote = item.note;
            if ( pasteByBeat ) {
                applyBeatPastePosition(newNote,
                                       item,
                                       pasteBeatTimeline,
                                       pasteFallbackBpm,
                                       pasteBeat,
                                       minBeat);
            } else {
                newNote.m_timestamp = item.note.m_timestamp + timeOffset;

                // 折线物件：同步偏移所有子物件的时间戳
                if ( newNote.m_type == ::MMM::NoteType::POLYLINE ) {
                    for ( auto& sub : newNote.m_subNotes ) {
                        sub.timestamp += timeOffset;
                    }
                }
            }

            if ( cmd.m_mirrored ) {
                mirrorNoteComponent(newNote, mirrorTrackCount);
            }

            if ( !isPlaceableCreatedNote(newNote) ) {
                XWARN("Paste blocked before 0s: target time={:.3f}",
                      newNote.m_timestamp);
                return;
            }

            notesToPaste.push_back(newNote);
        }

        const std::uint32_t playerTrackCount =
            static_cast<std::uint32_t>(std::max(m_ctx.trackCount, 0));
        std::vector<SampleComponent> samplesToPaste;
        samplesToPaste.reserve(sampleClipboard.size());
        for ( const auto& item : sampleClipboard ) {
            auto newSample = item.sample;
            if ( pasteByBeat ) {
                newSample.m_timestamp =
                    clipboardBeatToTime(pasteBeatTimeline,
                                        pasteBeat + item.startBeat - minBeat,
                                        pasteFallbackBpm);
            } else {
                newSample.m_timestamp = item.sample.m_timestamp + timeOffset;
            }

            const std::uint64_t targetTrack =
                static_cast<std::uint64_t>(playerTrackCount) + item.bgmLane;
            if ( targetTrack >
                 static_cast<std::uint64_t>(
                     std::numeric_limits<std::uint32_t>::max()) ) {
                XWARN("Paste blocked: BGM lane {} exceeds track range",
                      item.bgmLane);
                return;
            }
            newSample.m_track = static_cast<std::uint32_t>(targetTrack);
            if ( !isPlaceableCreatedSample(newSample) ) {
                XWARN("Sample paste blocked before 0s: target time={:.3f}",
                      newSample.m_timestamp);
                return;
            }
            samplesToPaste.push_back(std::move(newSample));
        }

        std::vector<BatchNoteAction::Entry>   noteEntries;
        std::vector<BatchSampleAction::Entry> sampleEntries;
        /// @brief 本次粘贴预先分配的新 Note 实体，用于执行后选中新物件。
        std::vector<entt::entity> pastedNoteEntities;
        /// @brief 本次粘贴预先分配的新 Sample 实体，用于执行后选中新物件。
        std::vector<entt::entity> pastedSampleEntities;
        pastedNoteEntities.reserve(noteClipboard.size());
        pastedSampleEntities.reserve(sampleClipboard.size());
        const bool selectPastedObjects = cmd.m_selectPastedObjects;

        // 如果之前有 Cut，需要删除那些 Cut 的物件。
        auto noteView   = m_ctx.noteRegistry.view<InteractionComponent>();
        auto sampleView = m_ctx.sampleRegistry.view<InteractionComponent>();
        bool isLocalCut = EditorEngine::instance().isClipboardCutFrom(&m_ctx);
        if ( isLocalCut ) {
            std::unordered_set<entt::entity> deletedNoteEntities;
            for ( auto entity : noteView ) {
                auto& ic = m_ctx.noteRegistry.get<InteractionComponent>(entity);
                if ( ic.isCut ) {
                    if ( !m_ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                        continue;
                    }

                    auto oldNote =
                        m_ctx.noteRegistry.get<NoteComponent>(entity);
                    if ( !SessionUtils::isNoteEditable(
                             oldNote, m_ctx.lastConfig.settings) ) {
                        continue;
                    }
                    noteEntries.push_back({
                        .entity         = entity,
                        .before         = oldNote,
                        .after          = std::nullopt,
                        .beforeSelected = ic.isSelected,
                    });
                    deletedNoteEntities.insert(entity);
                }
            }
            appendDeletedPolylineChildren(
                m_ctx, deletedNoteEntities, noteEntries);

            for ( auto entity : sampleView ) {
                auto& interaction =
                    m_ctx.sampleRegistry.get<InteractionComponent>(entity);
                if ( interaction.isCut &&
                     m_ctx.sampleRegistry.all_of<SampleComponent>(entity) ) {
                    sampleEntries.push_back({
                        .entity = entity,
                        .before =
                            m_ctx.sampleRegistry.get<SampleComponent>(entity),
                        .after          = std::nullopt,
                        .beforeSelected = interaction.isSelected,
                    });
                }
            }
        } else {
            EditorEngine::instance().consumeCrossSessionCutClipboard(&m_ctx);
        }

        for ( const auto& newNote : notesToPaste ) {
            // 为新粘贴物件预分配实体，避免执行后再从撤销栈动作反查实体。
            entt::entity pastedEntity = m_ctx.noteRegistry.create();
            pastedNoteEntities.push_back(pastedEntity);
            noteEntries.push_back({
                .entity        = pastedEntity,
                .before        = std::nullopt,
                .after         = newNote,
                .afterSelected = selectPastedObjects,
            });
        }
        for ( const auto& newSample : samplesToPaste ) {
            entt::entity pastedEntity = m_ctx.sampleRegistry.create();
            pastedSampleEntities.push_back(pastedEntity);
            sampleEntries.push_back({
                .entity        = pastedEntity,
                .before        = std::nullopt,
                .after         = newSample,
                .afterSelected = selectPastedObjects,
            });
        }

        // 动作执行前清除所有临时剪切标记，避免实体销毁后访问失效 View。
        for ( auto entity : noteView ) {
            m_ctx.noteRegistry.get<InteractionComponent>(entity).isCut = false;
        }
        for ( auto entity : sampleView ) {
            m_ctx.sampleRegistry.get<InteractionComponent>(entity).isCut =
                false;
        }

        std::vector<std::unique_ptr<IEditorAction>> actions;
        actions.reserve(2);
        if ( !noteEntries.empty() ) {
            actions.push_back(std::make_unique<BatchNoteAction>(
                std::move(noteEntries),
                cmd.m_mirrored ? "Mirror Paste" : "Paste"));
        }
        if ( !sampleEntries.empty() ) {
            actions.push_back(std::make_unique<BatchSampleAction>(
                std::move(sampleEntries), "粘贴自动采样"));
        }
        std::unique_ptr<IEditorAction> action;
        if ( actions.size() == 1 ) {
            action = std::move(actions.front());
        } else {
            action = std::make_unique<CompositeEditorAction>(
                std::move(actions),
                cmd.m_mirrored ? "镜像粘贴谱面物件" : "粘贴谱面物件");
        }
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);

        if ( selectPastedObjects ) {
            m_ctx.isSelecting         = false;
            m_ctx.hasMarqueeSelection = false;
            m_ctx.marqueeIsAdditive   = false;
            m_ctx.marqueeBoxes.clear();
            // 先清空所有旧选择，再只选中本次粘贴创建出的实体。
            clearChartObjectSelection(m_ctx);

            for ( auto entity : pastedNoteEntities ) {
                setChartObjectSelected(
                    m_ctx, ChartObjectKind::PlayerNote, entity, true);
            }
            for ( auto entity : pastedSampleEntities ) {
                setChartObjectSelected(
                    m_ctx, ChartObjectKind::AudioSample, entity, true);
            }
        }

        if ( isLocalCut ) {
            EditorEngine::instance().markCutClipboardConsumed();
        }
    }

    if ( !timelineClipboard.empty() ) {
        const bool pasteByBeat =
            m_ctx.lastConfig.settings.copyPasteTimeBasis ==
                Config::CopyPasteTimeBasis::Beat &&
            std::all_of(timelineClipboard.begin(),
                        timelineClipboard.end(),
                        [](const auto& item) { return item.hasBeatPosition; });
        const double pasteFallbackBpm = getClipboardFallbackBpm(m_ctx);
        auto         pasteBeatTimeline =
            pasteByBeat ? buildClipboardBeatTimeline(m_ctx, pasteFallbackBpm)
                        : ClipboardBeatTimeline{};
        const double pasteBeat =
            pasteByBeat ? clipboardTimeToBeat(
                              pasteBeatTimeline, pasteTime, pasteFallbackBpm)
                        : 0.0;

        std::vector<BatchTimelineAction::Entry> timelineEntries;
        timelineEntries.reserve(timelineClipboard.size());
        for ( const auto& item : timelineClipboard ) {
            auto   newTimeline = item.timeline;
            double targetTime  = pasteTime + item.relativeTime;
            if ( pasteByBeat ) {
                targetTime = clipboardBeatToTime(pasteBeatTimeline,
                                                 pasteBeat + item.relativeBeat,
                                                 pasteFallbackBpm);
            }
            if ( !std::isfinite(targetTime) ||
                 !std::isfinite(newTimeline.m_value) ) {
                continue;
            }
            if ( newTimeline.m_effect == ::MMM::TimingEffect::BPM &&
                 newTimeline.m_value <= 0.0 ) {
                continue;
            }

            newTimeline.m_timestamp = std::max(0.0, targetTime);
            timelineEntries.push_back(
                { entt::null, std::nullopt, newTimeline });
        }

        if ( !timelineEntries.empty() ) {
            auto action = std::make_unique<BatchTimelineAction>(
                std::move(timelineEntries), "Paste");
            m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
            m_ctx.isBpmEventsDirty = true;
        }
    }
}

// --- 时间线命令处理 ---

void ActionController::handleCommand(const CmdUpdateTimelineEvent& cmd)
{
    if ( m_ctx.timelineRegistry.valid(cmd.entity) ) {
        auto oldTl = m_ctx.timelineRegistry.get<TimelineComponent>(cmd.entity);
        auto newTl = oldTl;
        newTl.m_timestamp = cmd.newTime;
        newTl.m_value     = cmd.newValue;
        if ( cmd.metadataOverride ) {
            newTl.m_metadata = *cmd.metadataOverride;
        } else if ( std::abs(newTl.m_timestamp - oldTl.m_timestamp) > 1e-6 ) {
            clearMalodyTimingBeatMetadata(newTl.m_metadata);
        }

        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Update, cmd.entity, oldTl, newTl);
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        m_ctx.isBpmEventsDirty = true;
    }
}

void ActionController::handleCommand(const CmdUpdateTimelineEvents& cmd)
{
    if ( cmd.events.empty() ) {
        return;
    }

    std::vector<BatchTimelineAction::Entry> entries;
    entries.reserve(cmd.events.size());
    for ( const auto& update : cmd.events ) {
        if ( !m_ctx.timelineRegistry.valid(update.entity) ||
             !m_ctx.timelineRegistry.all_of<TimelineComponent>(
                 update.entity) ) {
            continue;
        }

        const auto oldTimeline =
            m_ctx.timelineRegistry.get<TimelineComponent>(update.entity);
        auto newTimeline        = oldTimeline;
        newTimeline.m_timestamp = update.newTime;
        newTimeline.m_value     = update.newValue;
        if ( update.metadataOverride ) {
            newTimeline.m_metadata = *update.metadataOverride;
        } else if ( std::abs(newTimeline.m_timestamp -
                             oldTimeline.m_timestamp) > 1e-6 ) {
            clearMalodyTimingBeatMetadata(newTimeline.m_metadata);
        }

        const bool coreFieldsChanged =
            std::abs(newTimeline.m_timestamp - oldTimeline.m_timestamp) >
                1e-12 ||
            std::abs(newTimeline.m_value - oldTimeline.m_value) > 1e-12;
        if ( !coreFieldsChanged && !update.metadataOverride ) {
            continue;
        }
        entries.push_back(
            { update.entity, oldTimeline, std::move(newTimeline) });
    }

    if ( entries.empty() ) {
        return;
    }
    auto action = std::make_unique<BatchTimelineAction>(
        std::move(entries), "Batch Timeline Update");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.isBpmEventsDirty = true;
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
              TimelineComponent{
                  event.time, event.type, event.value, event.metadata } });
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
    const std::int32_t beforeTrackCount = std::max(1, m_ctx.trackCount);
    const std::int32_t afterTrackCount =
        cmd.replaceMetadata
            ? std::max(1, cmd.sourceBeatmap->m_baseMapMetadata.track_count)
            : beforeTrackCount;
    const std::int32_t persistentBgmTrackCount =
        std::max(0, m_ctx.bgmTrackCount);
    beforeMetadata.baseMeta.track_count     = beforeTrackCount;
    beforeMetadata.baseMeta.bgm_track_count = persistentBgmTrackCount;
    afterMetadata.baseMeta.track_count      = afterTrackCount;
    afterMetadata.baseMeta.bgm_track_count  = persistentBgmTrackCount;

    std::vector<TrackCountAction::SampleTrackChange> sampleTrackChanges;
    if ( cmd.replaceMetadata && beforeTrackCount != afterTrackCount ) {
        const auto sampleView =
            m_ctx.sampleRegistry.view<const SampleComponent>();
        sampleTrackChanges.reserve(sampleView.size());
        for ( const auto entity : sampleView ) {
            const auto& sample = sampleView.get<const SampleComponent>(entity);
            const std::uint32_t bgmLane =
                sample.m_track >= static_cast<std::uint32_t>(beforeTrackCount)
                    ? sample.m_track -
                          static_cast<std::uint32_t>(beforeTrackCount)
                    : 0U;
            const std::uint64_t afterTrack =
                static_cast<std::uint64_t>(afterTrackCount) + bgmLane;
            if ( afterTrack > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::uint32_t>::max()) ) {
                m_ctx.lastActionMessage =
                    "替换谱面元数据会导致自动采样轨道索引溢出";
                return;
            }
            sampleTrackChanges.push_back({
                .entity      = entity,
                .beforeTrack = sample.m_track,
                .afterTrack  = static_cast<std::uint32_t>(afterTrack),
            });
        }
    }

    const double beforePreferenceBpm =
        normalizeBpm(m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm);
    const double afterPreferenceBpm =
        normalizeBpm(cmd.sourceBeatmap->m_baseMapMetadata.preference_bpm);

    auto action = std::make_unique<ReplaceBeatmapDataAction>(
        cmd.replaceObjects,
        cmd.replaceTimelines,
        cmd.replaceMetadata,
        std::move(beforeNotes),
        std::move(afterNotes),
        std::move(beforeTimelines),
        std::move(afterTimelines),
        std::move(beforeMetadata),
        std::move(afterMetadata),
        std::move(sampleTrackChanges),
        beforePreferenceBpm,
        afterPreferenceBpm);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);

    m_ctx.lastActionMessage =
        fmt::format("{} {}", TR("ui.status.category.action"), "数据来源替换");
}

void ActionController::handleCommand(const CmdAlignSelectedToCommonBeats& cmd)
{
    if ( !m_ctx.currentBeatmap ) return;

    // 从皮肤配置中读取常用分拍。
    auto getCommonDivisorsFromSkin = []() -> std::vector<int> {
        return MMM::Config::SkinManager::instance().getCommonDivisors();
    };

    std::vector<int> commonDivisors = getCommonDivisorsFromSkin();

    // 收集 BPM/Timing 事件。
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

        // 查找包含 rawTime 的 Timing 事件。
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
            if ( m_ctx.currentBeatmap &&
                 m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm >
                     0.0 ) {
                bVal = m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm;
            }
        }
        if ( bVal <= 0.0 ) bVal = 120.0;

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
        const auto& ic   = noteView.get<InteractionComponent>(entity);
        const auto& note = noteView.get<NoteComponent>(entity);
        if ( ic.isSelected &&
             SessionUtils::isNoteEditable(note, m_ctx.lastConfig.settings) ) {
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

    // 对齐非折线音符和子音符。
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

    // 同步父折线中的子音符顺序。
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

    // 生成 BatchNoteAction 条目。
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
