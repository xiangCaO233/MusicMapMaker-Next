#include "config/CreatorIdentity.h"
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
#include "logic/session/NoteIdentity.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SelectionState.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/TimelineAction.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/BpmNormalization.h"
#include "runtime/AppThreadPool.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <ice/thread/ThreadPool.hpp>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
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
void mirrorNoteComponent(NoteComponent& note, int playerTrackCount,
                         int draftTrackCount)
{
    const auto trackCount = note.m_isDraft ? draftTrackCount : playerTrackCount;
    if ( trackCount <= 0 ) return;

    const auto mirrorTrack = [trackCount, isDraft = note.m_isDraft](int track) {
        return isDraft ? -trackCount - 1 - track : trackCount - 1 - track;
    };
    note.m_trackIndex = mirrorTrack(note.m_trackIndex);
    if ( note.m_type == ::MMM::NoteType::FLICK ) {
        note.m_dtrack = -note.m_dtrack;
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( auto& sub : note.m_subNotes ) {
            sub.trackIndex = mirrorTrack(sub.trackIndex);
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
        return ::MMM::normalizeBpmValue(
            ctx.currentBeatmap->m_baseMapMetadata.preference_bpm);
    }
    return ::MMM::DEFAULT_NORMALIZED_BPM;
}

/// @brief 按原值方向规整 BPM，保证分拍换算使用安全数值。
/// @param bpm 待规整 BPM。
/// @param fallbackBpm 原值为 NaN 时使用的回退 BPM。
/// @return 位于安全计算范围内的 BPM。
double sanitizeClipboardBpm(double bpm, double fallbackBpm)
{
    return ::MMM::normalizeBpmValue(bpm, fallbackBpm);
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

/// @brief 更新整体替换涉及音符实体的基础辅助组件。
/// @param registry 目标 ECS 注册表。
/// @param entity 目标音符实体。
/// @param preserveInteraction 是否保留实体已有的本地交互状态。
void ensureReplacementNoteAuxiliaryComponents(entt::registry& registry,
                                              entt::entity    entity,
                                              bool preserveInteraction)
{
    registry.emplace_or_replace<TransformComponent>(entity);
    if ( !preserveInteraction ||
         !registry.all_of<InteractionComponent>(entity) ) {
        registry.emplace_or_replace<InteractionComponent>(entity);
    }
}

/// @brief 标记整体替换后需要重建音符排序和统计缓存。
/// @param ctx 当前会话上下文。
void markReplacementNoteOrderDirty(SessionContext& ctx)
{
    ctx.sortedNoteEntities.clear();
    ctx.sortedNoteMaxEndPrefix.clear();
    ctx.previewDensityObjectTimes.clear();
    ctx.isNoteOrderDirty             = true;
    ctx.isNotePruneDirty             = false;
    ctx.isNoteStatsDirty             = true;
    ctx.isPreviewDensityDirty        = true;
    ctx.isAnnotationRenderCacheDirty = true;
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

/// @brief 判断两个可选命中采样绑定是否完全相同。
bool isSameSampleBinding(const std::optional<::MMM::AudioSampleBinding>& lhs,
                         const std::optional<::MMM::AudioSampleBinding>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    return !lhs.has_value() ||
           (lhs->m_audioResourceId == rhs->m_audioResourceId &&
            lhs->m_volume == rhs->m_volume);
}

/// @brief 判断两个折线子物件组件是否完全相同。
bool isSameSubNoteComponent(const NoteComponent::SubNote& lhs,
                            const NoteComponent::SubNote& rhs)
{
    return lhs.type == rhs.type && lhs.timestamp == rhs.timestamp &&
           lhs.duration == rhs.duration && lhs.trackIndex == rhs.trackIndex &&
           lhs.dtrack == rhs.dtrack &&
           lhs.metadata.note_properties == rhs.metadata.note_properties &&
           lhs.annotation == rhs.annotation &&
           isSameSampleBinding(lhs.sampleBinding, rhs.sampleBinding) &&
           isSameNoteColorOverrides(lhs.customColors, rhs.customColors);
}

/// @brief 判断两个根音符组件是否可在权威同步后保留同一实体身份。
bool isSameRootNoteComponent(const NoteComponent& lhs, const NoteComponent& rhs)
{
    return !lhs.m_isSubNote && !rhs.m_isSubNote && lhs.m_type == rhs.m_type &&
           lhs.m_timestamp == rhs.m_timestamp &&
           lhs.m_duration == rhs.m_duration &&
           lhs.m_trackIndex == rhs.m_trackIndex &&
           lhs.m_dtrack == rhs.m_dtrack &&
           lhs.m_metadata.note_properties == rhs.m_metadata.note_properties &&
           isSameSampleBinding(lhs.m_sampleBinding, rhs.m_sampleBinding) &&
           isSameNoteColorOverrides(lhs.m_customColors, rhs.m_customColors) &&
           lhs.m_subNotes.size() == rhs.m_subNotes.size() &&
           std::equal(lhs.m_subNotes.begin(),
                      lhs.m_subNotes.end(),
                      rhs.m_subNotes.begin(),
                      isSameSubNoteComponent);
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

/// @brief 判断 Timeline 数值是否满足可编辑域约束。
/// @param effect Timeline 类型。
/// @param value 待写入数值。
/// @return 数值有限，且 BPM 不小于零时返回 true。
bool isValidTimelineValue(::MMM::TimingEffect effect, double value)
{
    // 非 BPM 特效允许负值；BPM 的统一底线仅排除负数。
    return std::isfinite(value) &&
           (effect != ::MMM::TimingEffect::BPM || value >= 0.0);
}

/// @brief 归一化批量替换后的 Timeline 列表。
std::vector<TimelineComponent> normalizeReplacementTimelines(
    std::vector<TimelineComponent> timelines)
{
    std::erase_if(timelines, [](const auto& timeline) {
        // 批量替换同样经过统一数值约束，防止绕过普通编辑命令。
        return !std::isfinite(timeline.m_timestamp) ||
               !isValidTimelineValue(timeline.m_effect, timeline.m_value);
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
        if ( note.m_isSubNote || note.m_isDraft ) continue;
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
    component.m_type            = note.m_type;
    component.m_timestamp       = note.m_timestamp / 1000.0;
    component.m_trackIndex      = static_cast<int>(note.m_track);
    component.m_metadata        = note.m_metadata;
    component.m_annotation      = note.m_annotation;
    component.m_sampleBinding   = note.getSampleBinding();
    component.m_collaborationId = note.m_collaborationId;

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
    subNote.type            = note.m_type;
    subNote.timestamp       = note.m_timestamp / 1000.0;
    subNote.duration        = 0.0;
    subNote.trackIndex      = static_cast<int>(note.m_track);
    subNote.dtrack          = 0;
    subNote.metadata        = note.m_metadata;
    subNote.annotation      = note.m_annotation;
    subNote.sampleBinding   = note.getSampleBinding();
    subNote.collaborationId = note.m_collaborationId;

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

/// @brief 从完整谱面中只构建指定稳定标识对应的根物件组件。
/// @param beatMap 后台已经物化的最新可见谱面。
/// @param identities 本轮发生增删改的根物件稳定标识。
/// @return 最新谱面中仍存在的目标根物件组件。
std::vector<NoteComponent> makeChangedNoteComponentsFromBeatMap(
    const ::MMM::BeatMap&                  beatMap,
    const std::unordered_set<std::string>& identities)
{
    std::vector<NoteComponent> notes;
    notes.reserve(identities.size());
    const auto appendRoot = [&](const ::MMM::Note& note) {
        if ( note.m_isSubNote ||
             !identities.contains(note.m_collaborationId) ) {
            return;
        }
        notes.push_back(makeNoteComponentFromBeatmapNote(note));
    };
    for ( const auto& note : beatMap.m_noteData.notes ) appendRoot(note);
    for ( const auto& hold : beatMap.m_noteData.holds ) appendRoot(hold);
    for ( const auto& flick : beatMap.m_noteData.flicks ) appendRoot(flick);
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        if ( !identities.contains(polyline.m_collaborationId) ) continue;
        auto component   = makeNoteComponentFromBeatmapNote(polyline);
        component.m_type = ::MMM::NoteType::POLYLINE;
        component.m_subNotes.reserve(polyline.m_subNotes.size());
        for ( const auto& subNote : polyline.m_subNotes ) {
            component.m_subNotes.push_back(
                makeSubNoteComponentFromBeatmapNote(subNote.get()));
        }
        if ( !component.m_subNotes.empty() ) {
            component.m_timestamp  = component.m_subNotes.front().timestamp;
            component.m_trackIndex = component.m_subNotes.front().trackIndex;
        }
        notes.push_back(std::move(component));
    }
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
        if ( timeline.m_effect == ::MMM::TimingEffect::BPM ) {
            timeline.m_value =
                ::MMM::normalizeBpmValue(timeline.m_value, timing.m_bpm);
        }
        timeline.m_metadata = timing.m_metadata;
        timelines.push_back(std::move(timeline));
    }
    return normalizeReplacementTimelines(std::move(timelines));
}

/// @brief 按协作逻辑标识合并权威物件，并保留仍存在物件的 ECS 实体。
/// @param ctx 当前会话上下文。
/// @param notes 替换后的非子物件组件列表。
/// @param preserveInteraction 是否保留仍存在实体的本地交互状态。
/// @warning 低频权威同步路径：会完整扫描一次音符 Registry 并整理交互缓存，
/// 禁止从每帧更新路径调用。
void replaceNoteComponents(SessionContext&                   ctx,
                           const std::vector<NoteComponent>& notes,
                           bool preserveInteraction)
{
    if ( !preserveInteraction ) {
        clearChartObjectSelectionIndex(ctx, ChartObjectKind::PlayerNote);
    }

    struct ExistingNote {
        /// @brief 同步前的实体。
        entt::entity entity{ entt::null };
        /// @brief 同步前的组件快照。
        NoteComponent component;
    };

    std::vector<ExistingNote> existing;
    const auto view = ctx.noteRegistry.view<const NoteComponent>();
    existing.reserve(view.size());
    std::unordered_map<std::string, std::size_t> identityIndex;
    identityIndex.reserve(view.size());
    for ( const auto entity : view ) {
        const auto& component = view.get<const NoteComponent>(entity);
        if ( component.m_isDraft ) continue;
        existing.push_back({ entity, component });
        const auto& identity = existing.back().component.m_collaborationId;
        if ( !identity.empty() ) {
            identityIndex.try_emplace(identity, existing.size() - 1U);
        }
    }

    std::unordered_set<entt::entity> retained;
    retained.reserve(existing.size());
    const auto findByIdentity = [&](std::string_view identity,
                                    bool expectedSubNote) -> entt::entity {
        if ( identity.empty() ) return entt::null;
        const auto found = identityIndex.find(std::string(identity));
        if ( found == identityIndex.end() ) return entt::null;
        const auto& candidate = existing[found->second];
        if ( candidate.component.m_isSubNote != expectedSubNote ||
             retained.contains(candidate.entity) ) {
            return entt::null;
        }
        return candidate.entity;
    };
    const auto findLegacyRoot = [&](const NoteComponent& desired) {
        const auto found = std::find_if(
            existing.begin(), existing.end(), [&](const ExistingNote& entry) {
                return !entry.component.m_isSubNote &&
                       !retained.contains(entry.entity) &&
                       isSameRootNoteComponent(entry.component, desired);
            });
        return found == existing.end() ? entt::null : found->entity;
    };

    for ( const auto& note : notes ) {
        NoteComponent applied = note;
        auto          entity = findByIdentity(applied.m_collaborationId, false);
        if ( entity == entt::null && applied.m_collaborationId.empty() ) {
            entity = findLegacyRoot(applied);
        }
        const bool retainedExistingEntity = entity != entt::null;
        if ( entity == entt::null ) {
            entity = ctx.noteRegistry.create();
        } else if ( applied.m_collaborationId.empty() ) {
            const auto& previous = ctx.noteRegistry.get<NoteComponent>(entity);
            applied.m_collaborationId = previous.m_collaborationId;
            if ( applied.m_subNotes.size() == previous.m_subNotes.size() ) {
                for ( std::size_t index = 0; index < applied.m_subNotes.size();
                      ++index ) {
                    if ( applied.m_subNotes[index].collaborationId.empty() ) {
                        applied.m_subNotes[index].collaborationId =
                            previous.m_subNotes[index].collaborationId;
                    }
                }
            }
        }
        retained.insert(entity);
        ctx.noteRegistry.emplace_or_replace<NoteComponent>(entity, applied);
        ensureReplacementNoteAuxiliaryComponents(
            ctx.noteRegistry,
            entity,
            preserveInteraction && retainedExistingEntity);

        if ( applied.m_type != ::MMM::NoteType::POLYLINE ) continue;

        for ( std::size_t index = 0; index < applied.m_subNotes.size();
              ++index ) {
            const auto&   sub = applied.m_subNotes[index];
            NoteComponent subComponent;
            subComponent.m_type            = sub.type;
            subComponent.m_timestamp       = sub.timestamp;
            subComponent.m_duration        = sub.duration;
            subComponent.m_trackIndex      = sub.trackIndex;
            subComponent.m_dtrack          = sub.dtrack;
            subComponent.m_isSubNote       = true;
            subComponent.m_parentPolyline  = entity;
            subComponent.m_subIndex        = static_cast<int>(index);
            subComponent.m_metadata        = sub.metadata;
            subComponent.m_annotation      = sub.annotation;
            subComponent.m_sampleBinding   = sub.sampleBinding;
            subComponent.m_customColors    = sub.customColors;
            subComponent.m_collaborationId = sub.collaborationId;

            auto subEntity =
                findByIdentity(subComponent.m_collaborationId, true);
            const bool retainedExistingSubEntity = subEntity != entt::null;
            if ( subEntity == entt::null ) {
                subEntity = ctx.noteRegistry.create();
            }
            retained.insert(subEntity);
            ctx.noteRegistry.emplace_or_replace<NoteComponent>(subEntity,
                                                               subComponent);
            ensureReplacementNoteAuxiliaryComponents(
                ctx.noteRegistry,
                subEntity,
                preserveInteraction && retainedExistingSubEntity);
        }
    }

    for ( const auto& entry : existing ) {
        if ( !retained.contains(entry.entity) &&
             ctx.noteRegistry.valid(entry.entity) ) {
            ctx.noteRegistry.destroy(entry.entity);
        }
    }

    if ( preserveInteraction ) {
        std::erase_if(ctx.selectedNoteEntities, [&](entt::entity entity) {
            if ( ctx.noteRegistry.valid(entity) &&
                 ctx.noteRegistry.all_of<NoteComponent>(entity) &&
                 ctx.noteRegistry.get<const NoteComponent>(entity).m_isDraft ) {
                return false;
            }
            return !retained.contains(entity) ||
                   !ctx.noteRegistry.valid(entity) ||
                   !ctx.noteRegistry.all_of<InteractionComponent>(entity) ||
                   !ctx.noteRegistry.get<const InteractionComponent>(entity)
                        .isSelected;
        });
        if ( ctx.hoveredObjectKind == ChartObjectKind::PlayerNote &&
             (ctx.hoveredEntity == entt::null ||
              !retained.contains(ctx.hoveredEntity) ||
              !ctx.noteRegistry.valid(ctx.hoveredEntity)) ) {
            ctx.hoveredEntity     = entt::null;
            ctx.hoveredObjectKind = ChartObjectKind::PlayerNote;
            ctx.hoveredPart       = static_cast<std::int32_t>(HoverPart::None);
            ctx.hoveredSubIndex   = -1;
        }
        if ( !ctx.marqueeBoxes.empty() ) {
            ctx.isMarqueeSelectionDirty = true;
        }
    } else {
        ctx.hoveredEntity       = entt::null;
        ctx.hoveredObjectKind   = ChartObjectKind::PlayerNote;
        ctx.hoveredPart         = static_cast<std::int32_t>(HoverPart::None);
        ctx.hoveredSubIndex     = -1;
        ctx.isSelecting         = false;
        ctx.hasMarqueeSelection = false;
        ctx.isMarqueeSelectionDirty = false;
        ctx.marqueeBoxes.clear();
    }
    ctx.draggedEntity     = entt::null;
    ctx.draggedObjectKind = ChartObjectKind::PlayerNote;
    ctx.draggedPart       = HoverPart::None;
    ctx.draggedSubIndex   = -1;
    ctx.dragInitialNote.reset();
    ctx.dragInitialSample.reset();
    ctx.dragRenderPinnedEntities.clear();
    ctx.isDragging = false;
    if ( ctx.brushState.isActive && !ctx.brushState.createsAudioSample ) {
        ctx.brushState.isActive = false;
        ctx.brushState.polylineSegments.clear();
        ctx.brushState.activeAudioResourceId.clear();
        ctx.brushState.activeSampleBinding.reset();
        ctx.brushState.holdStartTime = -1.0;
        ctx.brushState.duration      = 0.0;
        ctx.brushState.dtrack        = 0;
    }
    if ( ctx.eraserState.isActive &&
         ctx.eraserState.targetObjectKind == ChartObjectKind::PlayerNote ) {
        ctx.eraserState.isActive = false;
        ctx.eraserState.targetEntities.clear();
    }
    ctx.m_needsNotesSync = true;
    SessionUtils::markHitEventsDirty(ctx);
    markReplacementNoteOrderDirty(ctx);
}

/// @brief 按后台差量只更新指定稳定标识对应的 ECS 物件。
/// @param ctx 当前会话上下文。
/// @param source 后台已经物化的最新可见谱面。
/// @param changedIdentities 本轮增删改的根物件稳定标识。
/// @warning 远端物件提交路径：会扫描 Registry 建立目标实体索引，但只复制、
/// 创建或销毁实际变化的物件，禁止从每帧更新路径调用。
void applyIncrementalNoteComponents(
    SessionContext& ctx, const ::MMM::BeatMap& source,
    const std::vector<std::string>& changedIdentities)
{
    std::unordered_set<std::string> identities(changedIdentities.begin(),
                                               changedIdentities.end());
    if ( identities.empty() ) return;

    struct ExistingNote {
        /// @brief 同步前的实体。
        entt::entity entity{ entt::null };
        /// @brief 同步前的目标组件快照。
        NoteComponent component;
    };

    struct OwnedCacheMutation {
        /// @brief 差量对应的实体。
        entt::entity entity{ entt::null };
        /// @brief 应用远端差量前的组件。
        std::optional<NoteComponent> before;
        /// @brief 应用远端差量后的组件。
        std::optional<NoteComponent> after;
    };

    std::vector<ExistingNote>                     existing;
    std::unordered_map<std::string, entt::entity> existingByIdentity;
    std::unordered_set<entt::entity>              targetRoots;
    const auto view = ctx.noteRegistry.view<const NoteComponent>();
    for ( const auto entity : view ) {
        const auto& note = view.get<const NoteComponent>(entity);
        if ( note.m_isDraft || note.m_isSubNote ||
             !identities.contains(note.m_collaborationId) ) {
            continue;
        }
        existing.push_back({ entity, note });
        targetRoots.insert(entity);
        if ( !note.m_collaborationId.empty() ) {
            existingByIdentity.try_emplace(note.m_collaborationId, entity);
        }
    }
    for ( const auto entity : view ) {
        const auto& note = view.get<const NoteComponent>(entity);
        if ( note.m_isDraft || !note.m_isSubNote ||
             !targetRoots.contains(note.m_parentPolyline) ) {
            continue;
        }
        existing.push_back({ entity, note });
        if ( !note.m_collaborationId.empty() ) {
            existingByIdentity.try_emplace(note.m_collaborationId, entity);
        }
    }

    std::vector<OwnedCacheMutation>               cacheMutations;
    std::unordered_map<entt::entity, std::size_t> cacheMutationByEntity;
    cacheMutations.reserve(existing.size() + identities.size());
    cacheMutationByEntity.reserve(existing.size() + identities.size());
    for ( const auto& entry : existing ) {
        cacheMutationByEntity.emplace(entry.entity, cacheMutations.size());
        cacheMutations.push_back({
            .entity = entry.entity,
            .before = entry.component,
        });
    }
    const auto recordAfter = [&](entt::entity         entity,
                                 const NoteComponent& component) {
        const auto found = cacheMutationByEntity.find(entity);
        if ( found != cacheMutationByEntity.end() ) {
            cacheMutations[found->second].after = component;
            return;
        }
        cacheMutationByEntity.emplace(entity, cacheMutations.size());
        cacheMutations.push_back({
            .entity = entity,
            .after  = component,
        });
    };

    std::unordered_set<entt::entity> retained;
    retained.reserve(existing.size() + identities.size());
    const auto acquireEntity = [&](std::string_view identity,
                                   bool expectedSubNote) -> entt::entity {
        if ( identity.empty() ) return entt::null;
        const auto found = existingByIdentity.find(std::string(identity));
        if ( found == existingByIdentity.end() ||
             retained.contains(found->second) ||
             !ctx.noteRegistry.valid(found->second) ) {
            return entt::null;
        }
        const auto& current =
            ctx.noteRegistry.get<const NoteComponent>(found->second);
        return current.m_isSubNote == expectedSubNote ? found->second
                                                      : entt::null;
    };

    const auto desiredNotes =
        makeChangedNoteComponentsFromBeatMap(source, identities);
    for ( const auto& desired : desiredNotes ) {
        auto       root = acquireEntity(desired.m_collaborationId, false);
        const bool retainedRoot = root != entt::null;
        if ( root == entt::null ) root = ctx.noteRegistry.create();
        retained.insert(root);
        ctx.noteRegistry.emplace_or_replace<NoteComponent>(root, desired);
        recordAfter(root, desired);
        ensureReplacementNoteAuxiliaryComponents(
            ctx.noteRegistry, root, retainedRoot);

        if ( desired.m_type != ::MMM::NoteType::POLYLINE ) continue;
        for ( std::size_t index = 0; index < desired.m_subNotes.size();
              ++index ) {
            const auto&   sub = desired.m_subNotes[index];
            NoteComponent child;
            child.m_type            = sub.type;
            child.m_timestamp       = sub.timestamp;
            child.m_duration        = sub.duration;
            child.m_trackIndex      = sub.trackIndex;
            child.m_dtrack          = sub.dtrack;
            child.m_isSubNote       = true;
            child.m_parentPolyline  = root;
            child.m_subIndex        = static_cast<int>(index);
            child.m_metadata        = sub.metadata;
            child.m_annotation      = sub.annotation;
            child.m_sampleBinding   = sub.sampleBinding;
            child.m_customColors    = sub.customColors;
            child.m_collaborationId = sub.collaborationId;

            auto childEntity = acquireEntity(child.m_collaborationId, true);
            const bool retainedChild = childEntity != entt::null;
            if ( childEntity == entt::null ) {
                childEntity = ctx.noteRegistry.create();
            }
            retained.insert(childEntity);
            ctx.noteRegistry.emplace_or_replace<NoteComponent>(childEntity,
                                                               child);
            recordAfter(childEntity, child);
            ensureReplacementNoteAuxiliaryComponents(
                ctx.noteRegistry, childEntity, retainedChild);
        }
    }

    std::unordered_set<entt::entity> removed;
    for ( const auto& entry : existing ) {
        if ( retained.contains(entry.entity) ||
             !ctx.noteRegistry.valid(entry.entity) ) {
            continue;
        }
        removed.insert(entry.entity);
        ctx.noteRegistry.destroy(entry.entity);
    }
    std::erase_if(ctx.selectedNoteEntities, [&](entt::entity entity) {
        return removed.contains(entity);
    });
    if ( removed.contains(ctx.hoveredEntity) ) {
        ctx.hoveredEntity     = entt::null;
        ctx.hoveredObjectKind = ChartObjectKind::PlayerNote;
        ctx.hoveredPart       = static_cast<std::int32_t>(HoverPart::None);
        ctx.hoveredSubIndex   = -1;
    }
    if ( !ctx.marqueeBoxes.empty() ) ctx.isMarqueeSelectionDirty = true;

    ctx.m_needsNotesSync = true;
    std::vector<SessionUtils::NoteCacheMutationView> cacheMutationViews;
    cacheMutationViews.reserve(cacheMutations.size());
    for ( const auto& mutation : cacheMutations ) {
        cacheMutationViews.push_back({
            .entity = mutation.entity,
            .before = mutation.before ? &*mutation.before : nullptr,
            .after  = mutation.after ? &*mutation.after : nullptr,
        });
    }
    if ( !SessionUtils::applyNoteCacheMutationsIncrementally(
             ctx, cacheMutationViews) ) {
        SessionUtils::markHitEventsDirty(ctx);
        markReplacementNoteOrderDirty(ctx);
    }
}

/// @brief 在线程池释放已经被最新权威状态换出的旧领域物件容器。
/// @param retiredBeatmap 已不再由逻辑线程读取、内部持有旧物件容器的谱面。
/// @warning 联机物件提交路径每次调用一次；这里保留 shared_ptr 是为了保证旧
/// 容器跨线程释放期间的生命周期。若直接在逻辑线程析构，大谱面的逐元素释放
/// 仍会造成可见掉帧，目前没有不转移所有权且能安全后台释放的替代方案。
void retireBeatmapObjectStorage(std::shared_ptr<::MMM::BeatMap> retiredBeatmap)
{
    if ( !retiredBeatmap ) return;
    auto release = [beatmap = std::move(retiredBeatmap)]() mutable {
        beatmap->m_allNotes.clear();
        beatmap->m_noteData = {};
    };
    auto* threadPool = Runtime::AppThreadPool::instance().get();
    if ( threadPool ) {
        static_cast<void>(threadPool->enqueue(std::move(release)));
    } else {
        release();
    }
}

/// @brief 收集当前会话中全部自动采样组件。
/// @param ctx 当前会话上下文。
/// @return 按时间和轨道稳定排序的自动采样快照。
std::vector<SampleComponent> collectSampleComponents(SessionContext& ctx)
{
    std::vector<SampleComponent> samples;
    const auto view = ctx.sampleRegistry.view<const SampleComponent>();
    samples.reserve(view.size());
    for ( const auto entity : view ) {
        samples.push_back(view.get<const SampleComponent>(entity));
    }
    std::stable_sort(
        samples.begin(), samples.end(), [](const auto& lhs, const auto& rhs) {
            if ( lhs.m_timestamp != rhs.m_timestamp ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            if ( lhs.m_track != rhs.m_track ) return lhs.m_track < rhs.m_track;
            return lhs.m_audioResourceId < rhs.m_audioResourceId;
        });
    return samples;
}

/// @brief 从谱面领域对象构建自动采样组件快照。
/// @param beatMap 来源谱面。
/// @return 可直接替换到 ECS 的自动采样组件。
std::vector<SampleComponent> makeSampleComponentsFromBeatMap(
    const ::MMM::BeatMap& beatMap)
{
    std::vector<SampleComponent> samples;
    samples.reserve(beatMap.m_audioSamples.size());
    for ( const auto& sample : beatMap.m_audioSamples ) {
        samples.push_back(SampleComponent::fromAudioSample(sample));
    }
    return samples;
}

/// @brief 整体重建当前会话的自动采样 ECS。
/// @param ctx 当前会话上下文。
/// @param samples 替换后的自动采样组件列表。
void replaceSampleComponents(SessionContext&                     ctx,
                             const std::vector<SampleComponent>& samples)
{
    clearChartObjectSelectionIndex(ctx, ChartObjectKind::AudioSample);
    ctx.sampleRegistry.clear();
    for ( const auto& sample : samples ) {
        const auto entity = ctx.sampleRegistry.create();
        ctx.sampleRegistry.emplace<SampleComponent>(entity, sample);
        ctx.sampleRegistry.emplace<InteractionComponent>(entity);
    }
    ctx.hoveredEntity = entt::null;
    ctx.draggedEntity = entt::null;
    ctx.dragInitialSample.reset();
    ctx.isSampleOrderDirty               = true;
    ctx.isSamplePruneDirty               = false;
    ctx.isAnnotationRenderCacheDirty     = true;
    ctx.m_needsSamplesSync               = true;
    ctx.isPreviewDensityDirty            = true;
    ctx.isAudioTimelineDescriptorDirty   = true;
    ctx.isAudioTimelineActivationPending = true;
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
    /// @param replaceAnnotations 是否按稳定标识替换玩家物件注释。
    /// @param preserveInteraction 是否保留仍存在物件的本地交互状态。
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
        bool replaceAudioSamples, bool replaceAnnotations,
        bool preserveInteraction, std::vector<NoteComponent> beforeNotes,
        std::vector<NoteComponent>                       afterNotes,
        std::vector<TimelineComponent>                   beforeTimelines,
        std::vector<TimelineComponent>                   afterTimelines,
        std::vector<SampleComponent>                     beforeSamples,
        std::vector<SampleComponent>                     afterSamples,
        std::vector<::MMM::BeatmapAnnotation>            beforeAnnotations,
        std::vector<::MMM::BeatmapAnnotation>            afterAnnotations,
        BeatmapMetadataSnapshot                          beforeMetadata,
        BeatmapMetadataSnapshot                          afterMetadata,
        std::vector<TrackCountAction::SampleTrackChange> sampleTrackChanges,
        double beforePreferenceBpm, double afterPreferenceBpm)
        : m_replaceObjects(replaceObjects)
        , m_replaceTimelines(replaceTimelines)
        , m_replaceMetadata(replaceMetadata)
        , m_replaceAudioSamples(replaceAudioSamples)
        , m_replaceAnnotations(replaceAnnotations)
        , m_preserveInteraction(preserveInteraction)
        , m_beforeNotes(std::move(beforeNotes))
        , m_afterNotes(std::move(afterNotes))
        , m_beforeTimelines(std::move(beforeTimelines))
        , m_afterTimelines(std::move(afterTimelines))
        , m_beforeSamples(std::move(beforeSamples))
        , m_afterSamples(std::move(afterSamples))
        , m_beforeAnnotations(std::move(beforeAnnotations))
        , m_afterAnnotations(std::move(afterAnnotations))
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

    /// @brief 返回本次替换实际覆盖的全部谱面数据类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        auto flags = ::MMM::BeatmapMutationFlags::None;
        if ( m_replaceObjects ) flags |= ::MMM::BeatmapMutationFlags::Objects;
        if ( m_replaceTimelines ) {
            flags |= ::MMM::BeatmapMutationFlags::Timelines;
        }
        if ( m_replaceAudioSamples || !m_sampleTrackChanges.empty() ) {
            flags |= ::MMM::BeatmapMutationFlags::AudioSamples;
        }
        if ( m_replaceMetadata ) {
            flags |= ::MMM::BeatmapMutationFlags::Metadata;
        }
        if ( m_replaceAnnotations ) {
            flags |= ::MMM::BeatmapMutationFlags::Annotations;
        }
        return flags;
    }

private:
    /// @brief 应用指定方向的替换快照。
    /// @param ctx 当前会话上下文。
    /// @param forward 是否应用替换后的快照。
    void apply(SessionContext& ctx, bool forward)
    {
        if ( m_replaceObjects ) {
            replaceNoteComponents(ctx,
                                  forward ? m_afterNotes : m_beforeNotes,
                                  m_preserveInteraction);
        }
        if ( m_replaceTimelines ) {
            replaceTimelineComponents(
                ctx, forward ? m_afterTimelines : m_beforeTimelines);
            if ( ctx.currentBeatmap && !m_replaceMetadata ) {
                ctx.currentBeatmap->m_baseMapMetadata.preference_bpm =
                    forward ? m_afterPreferenceBpm : m_beforePreferenceBpm;
            }
        }
        if ( m_replaceAudioSamples ) {
            replaceSampleComponents(ctx,
                                    forward ? m_afterSamples : m_beforeSamples);
        }
        if ( m_replaceAnnotations && ctx.currentBeatmap ) {
            ctx.currentBeatmap->m_annotations =
                forward ? m_afterAnnotations : m_beforeAnnotations;
            ctx.isAnnotationRenderCacheDirty = true;
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

    /// @brief 是否替换自动采样对象。
    bool m_replaceAudioSamples{ false };

    /// @brief 是否按稳定标识替换玩家物件注释。
    bool m_replaceAnnotations{ false };

    /// @brief 是否保留稳定身份仍存在物件的本地交互状态。
    bool m_preserveInteraction{ false };

    /// @brief 替换前物件组件。
    std::vector<NoteComponent> m_beforeNotes;

    /// @brief 替换后物件组件。
    std::vector<NoteComponent> m_afterNotes;

    /// @brief 替换前 Timeline 组件。
    std::vector<TimelineComponent> m_beforeTimelines;

    /// @brief 替换后 Timeline 组件。
    std::vector<TimelineComponent> m_afterTimelines;

    /// @brief 替换前自动采样组件。
    std::vector<SampleComponent> m_beforeSamples;

    /// @brief 替换后自动采样组件。
    std::vector<SampleComponent> m_afterSamples;

    /// @brief 替换前批注。
    std::vector<::MMM::BeatmapAnnotation> m_beforeAnnotations;

    /// @brief 替换后批注。
    std::vector<::MMM::BeatmapAnnotation> m_afterAnnotations;

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

/// @brief 按稳定 ID 增删改单条谱面批注的可撤销动作。
class BeatmapAnnotationAction : public IEditorAction
{
public:
    /// @brief 构造按稳定 ID 定位的单条批注动作。
    /// @param before 修改前批注；新增时为空。
    /// @param after 修改后批注；删除时为空。
    /// @param name 动作名称。
    BeatmapAnnotationAction(std::optional<::MMM::BeatmapAnnotation> before,
                            std::optional<::MMM::BeatmapAnnotation> after,
                            std::string                             name)
        : m_before(std::move(before))
        , m_after(std::move(after))
        , m_name(std::move(name))
    {
        m_annotationId =
            m_after ? m_after->m_id : (m_before ? m_before->m_id : "");
    }

    /// @brief 应用修改后的批注列表。
    /// @param ctx 当前会话上下文。
    void execute(SessionContext& ctx) override { apply(ctx, m_after); }

    /// @brief 恢复修改前的批注列表。
    /// @param ctx 当前会话上下文。
    void undo(SessionContext& ctx) override { apply(ctx, m_before); }

    /// @brief 重新应用修改后的批注列表。
    /// @param ctx 当前会话上下文。
    void redo(SessionContext& ctx) override { execute(ctx); }

    /// @brief 返回动作名称。
    std::string getName() const override { return m_name; }

    /// @brief 返回批注数据类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return ::MMM::BeatmapMutationFlags::Annotations;
    }

private:
    /// @brief 按稳定 ID 写回或删除单条批注。
    /// @param ctx 当前会话上下文。
    /// @param annotation 待应用批注；空值表示删除。
    void apply(SessionContext&                                ctx,
               const std::optional<::MMM::BeatmapAnnotation>& annotation)
    {
        if ( !ctx.currentBeatmap ) return;
        auto&      annotations = ctx.currentBeatmap->m_annotations;
        const auto found       = std::find_if(
            annotations.begin(), annotations.end(), [&](auto& item) {
                return item.m_id == m_annotationId;
            });
        if ( annotation ) {
            if ( found == annotations.end() ) {
                annotations.push_back(*annotation);
            } else {
                *found = *annotation;
            }
        } else if ( found != annotations.end() ) {
            annotations.erase(found);
        }
        ctx.isAnnotationRenderCacheDirty = true;
    }

    /// @brief 本动作唯一影响的批注稳定 ID。
    std::string m_annotationId;

    /// @brief 修改前单条批注。
    std::optional<::MMM::BeatmapAnnotation> m_before;

    /// @brief 修改后单条批注。
    std::optional<::MMM::BeatmapAnnotation> m_after;

    /// @brief 动作名称。
    std::string m_name;
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

    /// @brief Timing 替换同时更新从 BPM 推导的首选 BPM 元数据。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return ::MMM::BeatmapMutationFlags::Timelines |
               ::MMM::BeatmapMutationFlags::Metadata;
    }

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
        if ( (m_ctx.hoveredObjectKind == ChartObjectKind::PlayerNote ||
              m_ctx.hoveredObjectKind == ChartObjectKind::DraftNote) &&
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

        mirrorNoteComponent(newNote, trackCount, m_ctx.draftTrackCount);

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
    auto noteClipboard   = EditorEngine::instance().getClipboard(&m_ctx);
    auto sampleClipboard = EditorEngine::instance().getSampleClipboard(&m_ctx);
    auto timelineClipboard =
        EditorEngine::instance().getTimelineClipboard(&m_ctx);
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
                mirrorNoteComponent(
                    newNote, mirrorTrackCount, m_ctx.draftTrackCount);
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
                const auto* note =
                    m_ctx.noteRegistry.try_get<const NoteComponent>(entity);
                setChartObjectSelected(m_ctx,
                                       note && note->m_isDraft
                                           ? ChartObjectKind::DraftNote
                                           : ChartObjectKind::PlayerNote,
                                       entity,
                                       true);
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
            // 粘贴创建也必须遵循非负 BPM 约束，零值仍可保留。
            if ( !std::isfinite(targetTime) ||
                 !isValidTimelineValue(newTimeline.m_effect,
                                       newTimeline.m_value) ) {
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
        // 单项编辑是表格与弹窗的最终入口，非法 BPM 不得进入撤销栈。
        if ( !isValidTimelineValue(oldTl.m_effect, cmd.newValue) ) return;
        auto newTl        = oldTl;
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
        // 批量编辑逐项过滤负 BPM，避免一项非法值污染整个动作。
        if ( !isValidTimelineValue(oldTimeline.m_effect, update.newValue) ) {
            continue;
        }
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

void ActionController::handleCommand(const CmdUpdateBpmWithKeepSpeedSv& cmd)
{
    auto& registry = m_ctx.timelineRegistry;
    if ( !registry.valid(cmd.bpmEntity) ||
         !registry.all_of<TimelineComponent>(cmd.bpmEntity) ) {
        return;
    }

    const auto bpmBefore = registry.get<TimelineComponent>(cmd.bpmEntity);
    if ( bpmBefore.m_effect != ::MMM::TimingEffect::BPM ) {
        return;
    }
    // 保速联动仍是 BPM 修改入口，负数不得连带生成或修改 SV。
    if ( !isValidTimelineValue(::MMM::TimingEffect::BPM, cmd.newBpm) ||
         !isValidTimelineValue(::MMM::TimingEffect::SCROLL, cmd.scrollValue) ) {
        return;
    }

    auto bpmAfter        = bpmBefore;
    bpmAfter.m_timestamp = cmd.newTime;
    bpmAfter.m_value     = cmd.newBpm;
    if ( std::abs(bpmAfter.m_timestamp - bpmBefore.m_timestamp) > 1e-6 ) {
        clearMalodyTimingBeatMetadata(bpmAfter.m_metadata);
    }

    std::vector<BatchTimelineAction::Entry> entries;
    entries.reserve(2U);
    entries.push_back({ cmd.bpmEntity, bpmBefore, std::move(bpmAfter) });

    entt::entity companionScrollEntity = entt::null;
    const auto   timelines = registry.view<const TimelineComponent>();
    for ( const auto entity : timelines ) {
        const auto& timeline = timelines.get<const TimelineComponent>(entity);
        if ( timeline.m_effect == ::MMM::TimingEffect::SCROLL &&
             std::abs(timeline.m_timestamp - cmd.newTime) <= 1e-6 &&
             (companionScrollEntity == entt::null ||
              entt::to_integral(entity) >
                  entt::to_integral(companionScrollEntity)) ) {
            companionScrollEntity = entity;
        }
    }

    if ( companionScrollEntity != entt::null ) {
        const auto scrollBefore =
            registry.get<TimelineComponent>(companionScrollEntity);
        auto scrollAfter        = scrollBefore;
        scrollAfter.m_timestamp = cmd.newTime;
        scrollAfter.m_value     = cmd.scrollValue;
        if ( std::abs(scrollAfter.m_timestamp - scrollBefore.m_timestamp) >
             1e-12 ) {
            clearMalodyTimingBeatMetadata(scrollAfter.m_metadata);
        }
        entries.push_back(
            { companionScrollEntity, scrollBefore, std::move(scrollAfter) });
    } else {
        TimelineComponent newScroll{ cmd.newTime,
                                     ::MMM::TimingEffect::SCROLL,
                                     cmd.scrollValue };
        entries.push_back({ entt::null, std::nullopt, std::move(newScroll) });
    }

    auto action = std::make_unique<BatchTimelineAction>(std::move(entries),
                                                        "BPM Keep Speed SV");
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
    // 所有单项创建入口统一拒绝负 BPM 与非有限数值。
    if ( !isValidTimelineValue(cmd.type, cmd.value) ) return;
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
        // 批量创建只跳过非法项，合法的零 BPM 与其他事件继续提交。
        if ( !isValidTimelineValue(event.type, event.value) ) continue;
        entries.push_back(
            { entt::null,
              std::nullopt,
              TimelineComponent{
                  event.time, event.type, event.value, event.metadata } });
    }

    if ( entries.empty() ) return;
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

        bpm = ::MMM::normalizeBpmValue(bpm);
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

void ActionController::handleCommand(const CmdSetNoteAnnotation& cmd)
{
    if ( cmd.annotation.size() > ::MMM::MAX_NOTE_ANNOTATION_BYTES ||
         cmd.entity == entt::null || !m_ctx.noteRegistry.valid(cmd.entity) ||
         !m_ctx.noteRegistry.all_of<NoteComponent>(cmd.entity) ) {
        m_ctx.lastActionMessage = "注释目标无效或内容超过 8192 字节";
        return;
    }

    entt::entity rootEntity = cmd.entity;
    std::int32_t subIndex   = cmd.subIndex;
    const auto&  requested  = m_ctx.noteRegistry.get<NoteComponent>(cmd.entity);
    if ( requested.m_isSubNote ) {
        rootEntity = requested.m_parentPolyline;
        if ( subIndex < 0 ) subIndex = requested.m_subIndex;
    }
    if ( rootEntity == entt::null || !m_ctx.noteRegistry.valid(rootEntity) ||
         !m_ctx.noteRegistry.all_of<NoteComponent>(rootEntity) ) {
        m_ctx.lastActionMessage = "注释目标已经失效";
        return;
    }

    const auto beforeRoot = m_ctx.noteRegistry.get<NoteComponent>(rootEntity);
    auto       afterRoot  = beforeRoot;
    if ( subIndex < 0 ) {
        if ( beforeRoot.m_annotation == cmd.annotation ) return;
        afterRoot.m_annotation = cmd.annotation;
    } else {
        const auto index = static_cast<std::size_t>(subIndex);
        if ( beforeRoot.m_type != ::MMM::NoteType::POLYLINE ||
             index >= beforeRoot.m_subNotes.size() ) {
            m_ctx.lastActionMessage = "折线子音符注释目标已经失效";
            return;
        }
        if ( beforeRoot.m_subNotes[index].annotation == cmd.annotation ) return;
        afterRoot.m_subNotes[index].annotation = cmd.annotation;
    }

    std::vector<BatchNoteAction::Entry> entries;
    entries.push_back({
        .entity = rootEntity,
        .before = beforeRoot,
        .after  = afterRoot,
    });

    if ( subIndex >= 0 ) {
        const auto view = m_ctx.noteRegistry.view<const NoteComponent>();
        for ( const auto entity : view ) {
            const auto& note = view.get<const NoteComponent>(entity);
            if ( !note.m_isSubNote || note.m_parentPolyline != rootEntity ||
                 note.m_subIndex != subIndex ) {
                continue;
            }
            auto afterChild         = note;
            afterChild.m_annotation = cmd.annotation;
            entries.push_back({
                .entity = entity,
                .before = note,
                .after  = std::move(afterChild),
            });
            break;
        }
    }

    auto action = std::make_unique<BatchNoteAction>(
        std::move(entries),
        "Set Note Annotation",
        ::MMM::BeatmapMutationFlags::Annotations);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "已更新物件注释";
}

void ActionController::handleCommand(const CmdUpsertBeatmapAnnotation& cmd)
{
    if ( !m_ctx.currentBeatmap || cmd.content.empty() ||
         cmd.content.size() > ::MMM::MAX_BEATMAP_ANNOTATION_CONTENT_BYTES ) {
        m_ctx.lastActionMessage = "批注正文不能为空且不能超过 8192 字节";
        return;
    }

    std::optional<::MMM::BeatmapAnnotation> before;
    std::optional<::MMM::BeatmapAnnotation> after;
    if ( !cmd.annotationId.empty() ) {
        const auto found =
            std::find_if(m_ctx.currentBeatmap->m_annotations.begin(),
                         m_ctx.currentBeatmap->m_annotations.end(),
                         [&](const auto& annotation) {
                             return annotation.m_id == cmd.annotationId;
                         });
        if ( found == m_ctx.currentBeatmap->m_annotations.end() ) {
            m_ctx.lastActionMessage = "要修改的批注已经不存在";
            return;
        }
        if ( found->m_content == cmd.content ) return;
        before           = *found;
        after            = *found;
        after->m_content = cmd.content;
    } else {
        const std::string author = Config::normalizeCreatorIdentity(cmd.author);
        if ( author.empty() ) {
            m_ctx.lastActionMessage = "未设置默认 Creator，无法添加批注";
            return;
        }
        if ( m_ctx.currentBeatmap->m_annotations.size() >=
             ::MMM::MAX_BEATMAP_ANNOTATION_COUNT ) {
            m_ctx.lastActionMessage = "当前谱面批注数量已达到上限";
            return;
        }

        ::MMM::BeatmapAnnotation annotation;
        annotation.m_id         = makeNoteCollaborationId();
        annotation.m_targetKind = cmd.targetKind;
        annotation.m_author     = author;
        annotation.m_content    = cmd.content;

        if ( cmd.targetKind == ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP ) {
            if ( !std::isfinite(cmd.timestamp) || cmd.timestamp < 0.0 ) {
                m_ctx.lastActionMessage = "批注时间戳无效";
                return;
            }
            annotation.m_timestamp = cmd.timestamp * 1000.0;
        } else if ( cmd.targetKind ==
                    ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE ) {
            if ( cmd.objectKind != ChartObjectKind::AudioSample ||
                 cmd.entity == entt::null ||
                 !m_ctx.sampleRegistry.valid(cmd.entity) ||
                 !m_ctx.sampleRegistry.all_of<SampleComponent>(cmd.entity) ) {
                m_ctx.lastActionMessage = "自动采样批注目标已经失效";
                return;
            }
            auto& sample =
                m_ctx.sampleRegistry.get<SampleComponent>(cmd.entity);
            ensureSampleCollaborationIdentity(sample);
            annotation.m_targetId    = sample.m_collaborationId;
            annotation.m_timestamp   = sample.effectiveTime() * 1000.0;
            m_ctx.m_needsSamplesSync = true;
        } else if ( cmd.targetKind ==
                    ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT ) {
            if ( cmd.objectKind != ChartObjectKind::PlayerNote ||
                 cmd.entity == entt::null ||
                 !m_ctx.noteRegistry.valid(cmd.entity) ||
                 !m_ctx.noteRegistry.all_of<NoteComponent>(cmd.entity) ) {
                m_ctx.lastActionMessage = "玩家物件批注目标已经失效";
                return;
            }

            entt::entity rootEntity = cmd.entity;
            std::int32_t subIndex   = cmd.subIndex;
            const auto&  requested =
                m_ctx.noteRegistry.get<const NoteComponent>(cmd.entity);
            if ( requested.m_isSubNote ) {
                rootEntity = requested.m_parentPolyline;
                if ( subIndex < 0 ) subIndex = requested.m_subIndex;
            }
            if ( rootEntity == entt::null ||
                 !m_ctx.noteRegistry.valid(rootEntity) ||
                 !m_ctx.noteRegistry.all_of<NoteComponent>(rootEntity) ) {
                m_ctx.lastActionMessage = "玩家物件批注目标已经失效";
                return;
            }
            auto& root = m_ctx.noteRegistry.get<NoteComponent>(rootEntity);
            ensureNoteCollaborationIdentity(root);
            if ( subIndex >= 0 ) {
                const auto index = static_cast<std::size_t>(subIndex);
                if ( root.m_type != ::MMM::NoteType::POLYLINE ||
                     index >= root.m_subNotes.size() ) {
                    m_ctx.lastActionMessage = "折线子物件批注目标已经失效";
                    return;
                }
                const auto& sub        = root.m_subNotes[index];
                annotation.m_targetId  = sub.collaborationId;
                annotation.m_timestamp = sub.timestamp * 1000.0;
            } else {
                annotation.m_targetId  = root.m_collaborationId;
                annotation.m_timestamp = root.m_timestamp * 1000.0;
            }
            m_ctx.m_needsNotesSync = true;
        } else {
            m_ctx.lastActionMessage = "批注目标类型无效";
            return;
        }
        after = std::move(annotation);
    }

    auto action = std::make_unique<BeatmapAnnotationAction>(
        std::move(before), std::move(after), "Edit Beatmap Annotation");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "已保存谱面批注";
}

void ActionController::handleCommand(const CmdRemoveBeatmapAnnotation& cmd)
{
    if ( !m_ctx.currentBeatmap || cmd.annotationId.empty() ) return;
    const auto found =
        std::find_if(m_ctx.currentBeatmap->m_annotations.begin(),
                     m_ctx.currentBeatmap->m_annotations.end(),
                     [&](const auto& annotation) {
                         return annotation.m_id == cmd.annotationId;
                     });
    if ( found == m_ctx.currentBeatmap->m_annotations.end() ) {
        m_ctx.lastActionMessage = "要删除的批注已经不存在";
        return;
    }
    auto action = std::make_unique<BeatmapAnnotationAction>(
        *found, std::nullopt, "Remove Beatmap Annotation");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "已删除谱面批注";
}

void ActionController::handleCommand(const CmdReplaceBeatmapData& cmd)
{
    // 元数据编辑窗口仍会在 UI 线程同步读取 ECS；整体替换必须与该读取共用
    // SessionRegistry 锁，避免远端提交清空 Registry 时使 EnTT View 失效。
    std::lock_guard<std::recursive_mutex> sessionLock(
        EditorEngine::instance().getSessionMutex());
    if ( !m_ctx.currentBeatmap || !cmd.sourceBeatmap ) {
        return;
    }
    if ( !cmd.replaceObjects && !cmd.replaceTimelines && !cmd.replaceMetadata &&
         !cmd.replaceAudioSamples && !cmd.replaceAnnotations ) {
        return;
    }

    const bool appliesPreparedObjectDelta =
        cmd.authoritativeRemote && cmd.replaceObjects &&
        !cmd.replaceTimelines && !cmd.replaceMetadata &&
        !cmd.replaceAudioSamples && !cmd.replaceAnnotations &&
        cmd.objectEncodingBaselinePrepared &&
        cmd.objectDeltaIdentities.has_value();
    if ( appliesPreparedObjectDelta ) {
        applyIncrementalNoteComponents(
            m_ctx, *cmd.sourceBeatmap, *cmd.objectDeltaIdentities);
        if ( m_ctx.currentBeatmap.get() != cmd.sourceBeatmap.get() ) {
            std::swap(m_ctx.currentBeatmap->m_noteData,
                      cmd.sourceBeatmap->m_noteData);
            std::swap(m_ctx.currentBeatmap->m_allNotes,
                      cmd.sourceBeatmap->m_allNotes);
            retireBeatmapObjectStorage(cmd.sourceBeatmap);
        }
        m_ctx.m_needsNotesSync = false;
        m_ctx.actionStack.markDirty();
        m_ctx.lastActionMessage = fmt::format(
            "{} {}", TR("ui.status.category.action"), "联机物件增量更新");
        return;
    }

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

    std::vector<SampleComponent> beforeSamples;
    std::vector<SampleComponent> afterSamples;
    if ( cmd.replaceAudioSamples ) {
        beforeSamples = collectSampleComponents(m_ctx);
        afterSamples  = makeSampleComponentsFromBeatMap(*cmd.sourceBeatmap);
    }

    std::vector<::MMM::BeatmapAnnotation> beforeAnnotations;
    std::vector<::MMM::BeatmapAnnotation> afterAnnotations;
    if ( cmd.replaceAnnotations ) {
        beforeAnnotations = m_ctx.currentBeatmap->m_annotations;
        afterAnnotations  = cmd.sourceBeatmap->m_annotations;
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

    const double beforePreferenceBpm = ::MMM::normalizeBpmValue(
        m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm);
    const double afterPreferenceBpm = ::MMM::normalizeBpmValue(
        cmd.sourceBeatmap->m_baseMapMetadata.preference_bpm);

    auto action = std::make_unique<ReplaceBeatmapDataAction>(
        cmd.replaceObjects,
        cmd.replaceTimelines,
        cmd.replaceMetadata,
        cmd.replaceAudioSamples,
        cmd.replaceAnnotations,
        cmd.authoritativeRemote,
        std::move(beforeNotes),
        std::move(afterNotes),
        std::move(beforeTimelines),
        std::move(afterTimelines),
        std::move(beforeSamples),
        std::move(afterSamples),
        std::move(beforeAnnotations),
        std::move(afterAnnotations),
        std::move(beforeMetadata),
        std::move(afterMetadata),
        std::move(sampleTrackChanges),
        beforePreferenceBpm,
        afterPreferenceBpm);
    if ( cmd.authoritativeRemote ) {
        const bool preservesNoteHistory =
            (cmd.replaceObjects || cmd.replaceAnnotations) &&
            !cmd.replaceTimelines && !cmd.replaceMetadata &&
            !cmd.replaceAudioSamples;
        if ( !preservesNoteHistory ) {
            m_ctx.actionStack.clear();
        }
        action->execute(m_ctx);
        m_ctx.actionStack.markDirty();
    } else {
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    }

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

        const double fallbackBpm =
            m_ctx.currentBeatmap
                ? m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
                : ::MMM::DEFAULT_NORMALIZED_BPM;
        const double bVal = ::MMM::normalizeBpmValue(bpmVal, fallbackBpm);

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

    const auto alignTimeRange = [&](double& timestamp, double& duration) {
        const double alignedStart = getAlignedTime(timestamp);
        const double alignedEnd   = getAlignedTime(timestamp + duration);
        timestamp                 = alignedStart;
        duration                  = std::max(0.0, alignedEnd - alignedStart);
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

    // 对齐普通音符和实际存在的折线子实体。
    for ( auto& [entity, newNote] : newNotes ) {
        if ( newNote.m_type != ::MMM::NoteType::POLYLINE ) {
            (void)entity;
            alignTimeRange(newNote.m_timestamp, newNote.m_duration);
        }
    }

    // 父折线自身持有完整子节点数据。独立子实体可能因导入来源而缺失，
    // 因此必须以父折线为基准对齐，不能只用找到的子实体重建列表。
    for ( auto& [entity, newNote] : newNotes ) {
        if ( newNote.m_type == ::MMM::NoteType::POLYLINE ) {
            struct AlignedSubNote {
                NoteComponent::SubNote note;
                entt::entity           childEntity{ entt::null };
                std::size_t            originalSubIndex{ 0U };
            };

            std::vector<entt::entity> childEntities(
                newNote.m_subNotes.size(),
                static_cast<entt::entity>(entt::null));
            for ( const auto& [otherEnt, otherNote] : newNotes ) {
                if ( otherNote.m_isSubNote &&
                     otherNote.m_parentPolyline == entity &&
                     otherNote.m_subIndex >= 0 ) {
                    const auto subIndex =
                        static_cast<std::size_t>(otherNote.m_subIndex);
                    if ( subIndex < childEntities.size() &&
                         childEntities[subIndex] == entt::null ) {
                        childEntities[subIndex] = otherEnt;
                    }
                }
            }

            std::vector<AlignedSubNote> alignedSubNotes;
            alignedSubNotes.reserve(newNote.m_subNotes.size());
            for ( std::size_t index = 0U; index < newNote.m_subNotes.size();
                  ++index ) {
                auto       alignedSubNote = newNote.m_subNotes[index];
                const auto childEntity    = childEntities[index];
                if ( childEntity != entt::null ) {
                    const auto& childNote    = newNotes.at(childEntity);
                    alignedSubNote.timestamp = childNote.m_timestamp;
                    alignedSubNote.duration  = childNote.m_duration;
                } else {
                    alignTimeRange(alignedSubNote.timestamp,
                                   alignedSubNote.duration);
                }
                alignedSubNotes.push_back(
                    { std::move(alignedSubNote), childEntity, index });
            }

            std::stable_sort(
                alignedSubNotes.begin(),
                alignedSubNotes.end(),
                [](const AlignedSubNote& lhs, const AlignedSubNote& rhs) {
                    if ( std::abs(lhs.note.timestamp - rhs.note.timestamp) <
                         1e-9 ) {
                        return lhs.originalSubIndex < rhs.originalSubIndex;
                    }
                    return lhs.note.timestamp < rhs.note.timestamp;
                });

            std::vector<NoteComponent::SubNote> newSubNotesList;
            newSubNotesList.reserve(newNote.m_subNotes.size());
            for ( std::size_t index = 0U; index < alignedSubNotes.size();
                  ++index ) {
                auto& alignedSubNote = alignedSubNotes[index];
                newSubNotesList.push_back(alignedSubNote.note);
                if ( alignedSubNote.childEntity != entt::null ) {
                    auto& childNote = newNotes.at(alignedSubNote.childEntity);
                    childNote.m_timestamp = alignedSubNote.note.timestamp;
                    childNote.m_duration  = alignedSubNote.note.duration;
                    childNote.m_subIndex  = static_cast<int>(index);
                }
            }

            newNote.m_subNotes = std::move(newSubNotesList);

            if ( !newNote.m_subNotes.empty() ) {
                newNote.m_timestamp  = newNote.m_subNotes.front().timestamp;
                newNote.m_trackIndex = newNote.m_subNotes.front().trackIndex;
            }
        }
    }

    // 生成 BatchNoteAction 条目。
    entries.reserve(originalNotes.size());
    for ( const auto& [entity, originalNote] : originalNotes ) {
        entries.push_back({ entity, originalNote, newNotes.at(entity) });
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
