#include "logic/session/SessionUtils.h"

#include "common/VideoFrameDecoder.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectDraftLaneService.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/NoteIdentity.h"
#include "logic/session/SelectionState.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include <algorithm>
#include <deque>
#include <limits>
#include <stb_image.h>
#include <system_error>

namespace MMM::Logic
{

/// @brief 根据背景类型探测并缓存原始尺寸。
/// @param ctx 目标会话上下文。
/// @param metadata 待探测的谱面基础元数据。
/// @param project 当前项目；为空时从谱面目录解析资源。
/// @warning 低频资源加载路径：会访问文件系统并打开图片或视频。
void SessionUtils::updateBackgroundSize(SessionContext&         ctx,
                                        const MMM::BaseMapMeta& metadata,
                                        const ::MMM::Project*   project)
{
    ctx.bgSize = glm::vec2(0.0f);
    if ( metadata.main_cover_path.empty() ) {
        return;
    }

    const std::filesystem::path backgroundPath =
        project ? project->m_projectRoot / metadata.main_cover_path
                : metadata.map_path.parent_path() / metadata.main_cover_path;
    std::error_code backgroundPathError;
    if ( !std::filesystem::is_regular_file(backgroundPath,
                                           backgroundPathError) ||
         backgroundPathError ) {
        return;
    }

    if ( metadata.cover_type == MMM::CoverType::VIDEO ) {
        const auto videoInfo = ::MMM::Utils::probeVideoInfo(backgroundPath);
        if ( videoInfo && videoInfo->width > 0 && videoInfo->height > 0 ) {
            ctx.bgSize = glm::vec2(static_cast<float>(videoInfo->width),
                                   static_cast<float>(videoInfo->height));
        }
        return;
    }

    int width          = 0;
    int height         = 0;
    int componentCount = 0;
    if ( stbi_info(Config::pathToUtf8(backgroundPath).c_str(),
                   &width,
                   &height,
                   &componentCount) ) {
        ctx.bgSize =
            glm::vec2(static_cast<float>(width), static_cast<float>(height));
    }
}

/// @brief 将谱面载入会话上下文并标记复合音频描述符待构建。
/// @param ctx 目标会话上下文。
/// @param beatmap 待载入谱面；为空时清空当前谱面状态。
/// @warning
/// 低频谱面加载路径：会访问文件系统、探测背景资源并重建 ECS，
/// 不允许放入每帧 update。
void SessionUtils::loadBeatmap(SessionContext&               ctx,
                               std::shared_ptr<MMM::BeatMap> beatmap)
{
    auto& mutex = EditorEngine::instance().getSessionMutex();
    std::lock_guard<std::recursive_mutex> lock(mutex);

    clearChartObjectSelectionIndex(ctx, ChartObjectKind::PlayerNote);
    clearChartObjectSelectionIndex(ctx, ChartObjectKind::AudioSample);
    ctx.noteRegistry.clear();
    ctx.sampleRegistry.clear();
    ctx.timelineRegistry.clear();
    ctx.actionStack.clear();
    ctx.sortedNoteEntities.clear();
    ctx.sortedNoteMaxEndPrefix.clear();
    ctx.sortedSampleEntities.clear();
    ctx.sortedSampleMaxEndPrefix.clear();
    ctx.previewDensityObjectTimes.clear();
    ctx.previewDensityCache.clear();
    ctx.lastCameraSnapshotTimes.clear();
    ctx.isNoteOrderDirty      = true;
    ctx.isNotePruneDirty      = false;
    ctx.isNoteStatsDirty      = true;
    ctx.isPreviewDensityDirty = true;
    ctx.isSampleOrderDirty    = true;
    ctx.isSamplePruneDirty    = false;
    ctx.hoveredEntity         = entt::null;
    ctx.hoveredObjectKind     = ChartObjectKind::PlayerNote;
    ctx.hoveredPart           = static_cast<std::int32_t>(HoverPart::None);
    ctx.hoveredSubIndex       = -1;
    ctx.draggedEntity         = entt::null;
    ctx.draggedObjectKind     = ChartObjectKind::PlayerNote;
    ctx.draggedPart           = HoverPart::None;
    ctx.draggedSubIndex       = -1;
    ctx.dragInitialNote.reset();
    ctx.dragInitialSample.reset();
    ctx.isDragging = false;
    ctx.eraserState.targetEntities.clear();
    ctx.eraserState.isActive         = false;
    ctx.eraserState.targetObjectKind = ChartObjectKind::PlayerNote;
    ctx.audioTimelineDescriptor      = {};
    ctx.m_draftLaneGroupId.clear();
    ctx.m_draftLaneGroupRevision = 0U;
    ctx.m_draftLaneBasePayload.clear();
    ctx.m_needsDraftNotesSync                    = false;
    ctx.audioTimelineTotalTime                   = 0.0;
    ctx.missingAudioTimelineClipCount            = 0U;
    ctx.isAudioTimelineDescriptorDirty           = true;
    ctx.isAudioTimelineActivationPending         = true;
    ctx.isAudioTimelineFingerprintPublishPending = true;

    ctx.isPlaying                         = false;
    ctx.isSeekScrubbing                   = false;
    ctx.isAudioTimelineSyncFollower       = false;
    ctx.restartPlaybackAfterFinishPending = false;
    ctx.currentTime                       = 0.0;
    ctx.animateTime =
        ctx.currentTime + ctx.lastConfig.visual.getEffectiveVisualOffset();
    ctx.animateTimeTarget          = ctx.animateTime;
    ctx.animateTimeAnimationActive = false;
    ctx.animatedTimelineZoom       = ctx.lastConfig.visual.timelineZoom;
    ctx.animatedTimelineZoomTarget = ctx.animatedTimelineZoom;
    ctx.animatedTimelineZoomAnimationActive = false;
    ctx.currentBeatmap                      = beatmap;

    if ( !beatmap ) {
        ctx.bgmTrackCount = 0;
        ctx.hitEvents.clear();
        ctx.nextHitIndex                = 0;
        ctx.nextPredictHitIndex         = 0;
        ctx.nextBoundSoundPrefetchIndex = 0;
        ctx.isHitEventsDirty            = false;
        return;
    }

    auto* project = EditorEngine::instance().getCurrentProject();
    SessionUtils::updateBackgroundSize(
        ctx, beatmap->m_baseMapMetadata, project);

    ctx.trackCount = beatmap->m_baseMapMetadata.track_count;
    if ( ctx.trackCount <= 0 ) ctx.trackCount = 12;  // 默认值
    ctx.bgmTrackCount = std::max(0, beatmap->m_baseMapMetadata.bgm_track_count);

    // 协作逻辑标识只存在于运行时模型中；载入普通谱面时在构建 ECS 前补齐，
    // 保证房主首次发布的快照与本地操作栈引用同一逻辑物件。
    for ( auto& note : beatmap->m_noteData.notes ) {
        ensureNoteCollaborationIdentity(note);
    }
    for ( auto& hold : beatmap->m_noteData.holds ) {
        ensureNoteCollaborationIdentity(hold);
    }
    for ( auto& flick : beatmap->m_noteData.flicks ) {
        ensureNoteCollaborationIdentity(flick);
    }
    for ( auto& polyline : beatmap->m_noteData.polylines ) {
        ensureNoteCollaborationIdentity(polyline);
        for ( auto& subNote : polyline.m_subNotes ) {
            ensureNoteCollaborationIdentity(subNote.get());
        }
    }

    // 清空缓存上下文，以确保重新构建
    if ( auto* cache =
             ctx.timelineRegistry.ctx().find<System::ScrollCache>() ) {
        cache->isDirty = true;
    }

    // 根据传入的 BeatMap 构建 ECS 实体
    // 1. 加载 Timing 点
    for ( const auto& timing : beatmap->m_timings ) {
        auto  entity = ctx.timelineRegistry.create();
        auto& tc     = ctx.timelineRegistry.emplace<TimelineComponent>(
            entity,
            timing.m_timestamp / 1000.0,  // 毫秒转秒
            timing.m_timingEffect,
            timing.m_timingEffectParameter);
        tc.m_metadata = timing.m_metadata;
    }

    // 用于追踪子物件，防止在折线之外重复绘制
    std::unordered_map<const ::MMM::Note*, entt::entity> noteToEntity;

    // 2. 加载普通音符 (Notes)
    for ( const auto& note : beatmap->m_noteData.notes ) {
        auto entity         = ctx.noteRegistry.create();
        noteToEntity[&note] = entity;

        int track = static_cast<int>(note.m_track);

        auto& nc = ctx.noteRegistry.emplace<NoteComponent>(
            entity,
            note.m_type,
            note.m_timestamp / 1000.0,  // 毫秒转秒
            0.0,
            track);
        nc.m_metadata        = note.m_metadata;
        nc.m_annotation      = note.m_annotation;
        nc.m_sampleBinding   = note.getSampleBinding();
        nc.m_collaborationId = note.m_collaborationId;
        loadNoteColorOverridesFromMetadata(nc);

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 3. 加载长键 (Holds)
    for ( const auto& hold : beatmap->m_noteData.holds ) {
        auto entity         = ctx.noteRegistry.create();
        noteToEntity[&hold] = entity;

        int track = static_cast<int>(hold.m_track);

        auto& nc = ctx.noteRegistry.emplace<NoteComponent>(
            entity,
            hold.m_type,
            hold.m_timestamp / 1000.0,  // 毫秒转秒
            hold.m_duration / 1000.0,   // 毫秒转秒
            track);
        nc.m_metadata        = hold.m_metadata;
        nc.m_annotation      = hold.m_annotation;
        nc.m_sampleBinding   = hold.getSampleBinding();
        nc.m_collaborationId = hold.m_collaborationId;
        loadNoteColorOverridesFromMetadata(nc);

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 4. 加载滑键 (Flicks)
    for ( const auto& flick : beatmap->m_noteData.flicks ) {
        auto entity          = ctx.noteRegistry.create();
        noteToEntity[&flick] = entity;

        int track = static_cast<int>(flick.m_track);

        auto& nc =
            ctx.noteRegistry.emplace<NoteComponent>(entity,
                                                    flick.m_type,
                                                    flick.m_timestamp / 1000.0,
                                                    0.0,
                                                    track,
                                                    flick.m_dtrack);
        nc.m_metadata        = flick.m_metadata;
        nc.m_annotation      = flick.m_annotation;
        nc.m_sampleBinding   = flick.getSampleBinding();
        nc.m_collaborationId = flick.m_collaborationId;
        loadNoteColorOverridesFromMetadata(nc);

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 5. 加载折线 (Polylines)
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        auto entity = ctx.noteRegistry.create();

        int track = static_cast<int>(polyline.m_track);

        auto& comp = ctx.noteRegistry.emplace<NoteComponent>(
            entity, polyline.m_type, polyline.m_timestamp / 1000.0, 0.0, track);
        comp.m_metadata        = polyline.m_metadata;
        comp.m_annotation      = polyline.m_annotation;
        comp.m_sampleBinding   = polyline.getSampleBinding();
        comp.m_collaborationId = polyline.m_collaborationId;
        loadNoteColorOverridesFromMetadata(comp);

        // 填充子物件并标记它们为 SubNote
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            const auto& subNote = subNoteRef.get();

            // 标记原始实体为 SubNote，防止独立绘制
            if ( auto it = noteToEntity.find(&subNote);
                 it != noteToEntity.end() ) {
                auto& subComp = ctx.noteRegistry.get<NoteComponent>(it->second);
                subComp.m_isSubNote      = true;
                subComp.m_parentPolyline = entity;
                subComp.m_subIndex = static_cast<int>(comp.m_subNotes.size());
            }

            NoteComponent::SubNote sn;
            sn.type       = subNote.m_type;
            sn.timestamp  = subNote.m_timestamp / 1000.0;
            sn.trackIndex = static_cast<int>(subNote.m_track);
            sn.duration   = 0.0;
            sn.dtrack     = 0;

            if ( subNote.m_type == ::MMM::NoteType::HOLD ) {
                const auto& h = static_cast<const ::MMM::Hold&>(subNote);
                sn.duration   = h.m_duration / 1000.0;
            } else if ( subNote.m_type == ::MMM::NoteType::FLICK ) {
                const auto& f = static_cast<const ::MMM::Flick&>(subNote);
                sn.dtrack     = f.m_dtrack;
            }
            sn.metadata        = subNote.m_metadata;
            sn.annotation      = subNote.m_annotation;
            sn.sampleBinding   = subNote.getSampleBinding();
            sn.collaborationId = subNote.m_collaborationId;
            loadNoteColorOverridesFromMetadata(sn);
            comp.m_subNotes.push_back(sn);
        }

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 6. 自动采样使用独立 Registry，避免进入判定、Combo、KPS 与 HitFX。
    for ( const auto& sample : beatmap->m_audioSamples ) {
        auto entity = ctx.sampleRegistry.create();
        ctx.sampleRegistry.emplace<SampleComponent>(
            entity, SampleComponent::fromAudioSample(sample));
        ctx.sampleRegistry.emplace<InteractionComponent>(entity);

        if ( sample.m_track >= static_cast<std::uint32_t>(ctx.trackCount) ) {
            const auto requiredBgmCount =
                sample.m_track - static_cast<std::uint32_t>(ctx.trackCount) + 1;
            const auto clampedRequiredBgmCount =
                static_cast<std::int32_t>(std::min<std::uint32_t>(
                    requiredBgmCount,
                    static_cast<std::uint32_t>(
                        std::numeric_limits<std::int32_t>::max())));
            ctx.bgmTrackCount =
                std::max(ctx.bgmTrackCount, clampedRequiredBgmCount);
        }
    }

    // 构建音效触发事件队列并排序
    ctx.hitEvents.clear();
    ctx.nextHitIndex                = 0;
    ctx.nextBoundSoundPrefetchIndex = 0;

    // 收集所有的 subNote 引用，避免它们被重复加入普通音符的播放队列
    std::unordered_set<const ::MMM::Note*> subNotesSet;
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            subNotesSet.insert(&subNoteRef.get());
        }
    }

    using HitRole = System::HitFXSystem::HitEvent::Role;

    for ( const auto& note : beatmap->m_noteData.notes ) {
        if ( subNotesSet.find(&note) != subNotesSet.end() ) continue;
        ctx.hitEvents.push_back({ note.m_timestamp / 1000.0,
                                  note.m_type,
                                  HitRole::None,
                                  1,
                                  static_cast<int>(note.m_track),
                                  0,
                                  0.0,
                                  false,
                                  note.getSampleBinding() });
    }
    for ( const auto& hold : beatmap->m_noteData.holds ) {
        if ( subNotesSet.find(&hold) != subNotesSet.end() ) continue;
        ctx.hitEvents.push_back({ hold.m_timestamp / 1000.0,
                                  hold.m_type,
                                  HitRole::None,
                                  1,
                                  static_cast<int>(hold.m_track),
                                  0,
                                  hold.m_duration / 1000.0,
                                  false,
                                  hold.getSampleBinding() });
    }
    for ( const auto& flick : beatmap->m_noteData.flicks ) {
        if ( subNotesSet.find(&flick) != subNotesSet.end() ) continue;
        int span = std::abs(flick.m_dtrack) + 1;
        ctx.hitEvents.push_back({ flick.m_timestamp / 1000.0,
                                  flick.m_type,
                                  HitRole::None,
                                  span,
                                  static_cast<int>(flick.m_track),
                                  flick.m_dtrack,
                                  0.0,
                                  false,
                                  flick.getSampleBinding() });
    }
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        // 对于 Polyline 本身不发声，由子物件发声
        size_t subNoteCount = polyline.m_subNotes.size();
        for ( size_t i = 0; i < subNoteCount; ++i ) {
            const auto& subNote = polyline.m_subNotes[i].get();

            HitRole role = HitRole::Internal;
            if ( i == 0 )
                role = HitRole::Head;
            else if ( i == subNoteCount - 1 )
                role = HitRole::Tail;

            int    span        = 1;
            int    trackOffset = 0;
            double duration    = 0.0;
            if ( subNote.m_type == ::MMM::NoteType::FLICK ) {
                const auto& f = static_cast<const ::MMM::Flick&>(subNote);
                span          = std::abs(f.m_dtrack) + 1;
                trackOffset   = f.m_dtrack;
            } else if ( subNote.m_type == ::MMM::NoteType::HOLD ) {
                const auto& h = static_cast<const ::MMM::Hold&>(subNote);
                duration      = h.m_duration / 1000.0;
            }

            auto sampleBinding = subNote.getSampleBinding();
            if ( !sampleBinding && role == HitRole::Head ) {
                sampleBinding = polyline.getSampleBinding();
            }
            ctx.hitEvents.push_back({ subNote.m_timestamp / 1000.0,
                                      subNote.m_type,
                                      role,
                                      span,
                                      static_cast<int>(subNote.m_track),
                                      trackOffset,
                                      duration,
                                      true,
                                      std::move(sampleBinding) });
        }
    }
    std::sort(ctx.hitEvents.begin(), ctx.hitEvents.end());
    ctx.isHitEventsDirty = false;

    XINFO(
        "Loaded new BeatMap with {} notes, {} holds, {} flicks, {} polylines "
        "and {} timings and {} audio samples.",
        beatmap->m_noteData.notes.size(),
        beatmap->m_noteData.holds.size(),
        beatmap->m_noteData.flicks.size(),
        beatmap->m_noteData.polylines.size(),
        beatmap->m_timings.size(),
        beatmap->m_audioSamples.size());

    ProjectDraftLaneService::load(ctx, project);

    ctx.m_needsTimingsSync = false;
    ctx.m_needsNotesSync   = false;
    ctx.m_needsSamplesSync = false;
    ctx.isNoteOrderDirty   = true;
    ctx.isNotePruneDirty   = false;
    ctx.isNoteStatsDirty   = true;
    ctx.isSampleOrderDirty = true;
    ctx.isSamplePruneDirty = false;
}

bool SessionUtils::syncCreatedNoteToBeatmap(SessionContext&      ctx,
                                            const NoteComponent& note)
{
    if ( !ctx.currentBeatmap || note.m_isDraft || note.m_isSubNote ||
         note.m_collaborationId.empty() ||
         (note.m_type != ::MMM::NoteType::NOTE &&
          note.m_type != ::MMM::NoteType::HOLD &&
          note.m_type != ::MMM::NoteType::FLICK) ) {
        return false;
    }

    NoteComponent synchronized = note;
    if ( hasAnyNoteColorOverride(synchronized.m_customColors) ) {
        writeNoteColorOverridesToMetadata(synchronized);
    }

    auto& mutex = EditorEngine::instance().getSessionMutex();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    ::MMM::Note*                          appended = nullptr;
    if ( synchronized.m_type == ::MMM::NoteType::NOTE ) {
        auto& target      = ctx.currentBeatmap->m_noteData.notes;
        auto& value       = target.emplace_back();
        value.m_type      = ::MMM::NoteType::NOTE;
        value.m_timestamp = synchronized.m_timestamp * 1000.0;
        value.m_track = static_cast<std::uint32_t>(synchronized.m_trackIndex);
        value.m_metadata        = synchronized.m_metadata;
        value.m_annotation      = synchronized.m_annotation;
        value.m_sampleBinding   = synchronized.m_sampleBinding;
        value.m_collaborationId = synchronized.m_collaborationId;
        appended                = &value;
    } else if ( synchronized.m_type == ::MMM::NoteType::HOLD ) {
        auto& target      = ctx.currentBeatmap->m_noteData.holds;
        auto& value       = target.emplace_back();
        value.m_type      = ::MMM::NoteType::HOLD;
        value.m_timestamp = synchronized.m_timestamp * 1000.0;
        value.m_track = static_cast<std::uint32_t>(synchronized.m_trackIndex);
        value.m_duration        = synchronized.m_duration * 1000.0;
        value.m_metadata        = synchronized.m_metadata;
        value.m_annotation      = synchronized.m_annotation;
        value.m_sampleBinding   = synchronized.m_sampleBinding;
        value.m_collaborationId = synchronized.m_collaborationId;
        appended                = &value;
    } else {
        auto& target      = ctx.currentBeatmap->m_noteData.flicks;
        auto& value       = target.emplace_back();
        value.m_type      = ::MMM::NoteType::FLICK;
        value.m_timestamp = synchronized.m_timestamp * 1000.0;
        value.m_track  = static_cast<std::uint32_t>(synchronized.m_trackIndex);
        value.m_dtrack = synchronized.m_dtrack;
        value.m_metadata        = synchronized.m_metadata;
        value.m_annotation      = synchronized.m_annotation;
        value.m_sampleBinding   = synchronized.m_sampleBinding;
        value.m_collaborationId = synchronized.m_collaborationId;
        appended                = &value;
    }

    const auto insertion = std::upper_bound(
        ctx.currentBeatmap->m_allNotes.begin(),
        ctx.currentBeatmap->m_allNotes.end(),
        appended->m_timestamp,
        [](double timestamp, const std::reference_wrapper<::MMM::Note>& value) {
            return timestamp < value.get().m_timestamp;
        });
    ctx.currentBeatmap->m_allNotes.insert(insertion, *appended);
    return true;
}


void SessionUtils::syncBeatmap(SessionContext& ctx)
{
    if ( !ctx.currentBeatmap ) return;
    if ( !ctx.m_needsTimingsSync && !ctx.m_needsNotesSync &&
         !ctx.m_needsSamplesSync ) {
        return;
    }

    std::vector<Timing>                       newTimings;
    std::vector<std::reference_wrapper<Note>> newAllNotes;
    NoteData                                  newNoteData;
    std::deque<AudioSampleEvent>              newAudioSamples;

    if ( ctx.m_needsTimingsSync ) {
        auto tlView = ctx.timelineRegistry.view<TimelineComponent>();
        std::vector<TimelineComponent> sortedTLs;
        for ( auto entity : tlView ) {
            sortedTLs.push_back(tlView.get<TimelineComponent>(entity));
        }
        std::sort(sortedTLs.begin(),
                  sortedTLs.end(),
                  [](const auto& a, const auto& b) {
                      return a.m_timestamp < b.m_timestamp;
                  });

        double currentBPM = 120.0;
        if ( ctx.currentBeatmap &&
             ctx.currentBeatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
            currentBPM = ctx.currentBeatmap->m_baseMapMetadata.preference_bpm;
        }
        for ( const auto& tc : sortedTLs ) {
            Timing timing;
            timing.m_timestamp             = tc.m_timestamp * 1000.0;
            timing.m_timingEffect          = tc.m_effect;
            timing.m_timingEffectParameter = tc.m_value;

            if ( tc.m_effect == ::MMM::TimingEffect::BPM ) {
                currentBPM           = tc.m_value;
                timing.m_bpm         = currentBPM;
                timing.m_beat_length = 60000.0 / std::max(0.1, timing.m_bpm);
            } else {
                timing.m_bpm         = currentBPM;
                timing.m_beat_length = tc.m_value;
            }
            timing.m_metadata = tc.m_metadata;
            newTimings.push_back(timing);
        }
    }

    if ( ctx.m_needsNotesSync ) {
        auto noteView = ctx.noteRegistry.view<NoteComponent>();

        for ( auto entity : noteView ) {
            const auto& nc = noteView.get<NoteComponent>(entity);
            if ( nc.m_isSubNote || nc.m_isDraft ) continue;
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) continue;

            NoteComponent syncedNote = nc;
            if ( hasAnyNoteColorOverride(syncedNote.m_customColors) ) {
                writeNoteColorOverridesToMetadata(syncedNote);
            }

            if ( nc.m_type == ::MMM::NoteType::NOTE ) {
                Note n;
                n.m_type       = ::MMM::NoteType::NOTE;
                n.m_timestamp  = syncedNote.m_timestamp * 1000.0;
                n.m_track      = static_cast<uint32_t>(syncedNote.m_trackIndex);
                n.m_metadata   = syncedNote.m_metadata;
                n.m_annotation = syncedNote.m_annotation;
                n.m_sampleBinding   = syncedNote.m_sampleBinding;
                n.m_collaborationId = syncedNote.m_collaborationId;
                newNoteData.notes.push_back(std::move(n));
                newAllNotes.push_back(newNoteData.notes.back());
            } else if ( nc.m_type == ::MMM::NoteType::HOLD ) {
                Hold h;
                h.m_type       = ::MMM::NoteType::HOLD;
                h.m_timestamp  = syncedNote.m_timestamp * 1000.0;
                h.m_track      = static_cast<uint32_t>(syncedNote.m_trackIndex);
                h.m_duration   = syncedNote.m_duration * 1000.0;
                h.m_metadata   = syncedNote.m_metadata;
                h.m_annotation = syncedNote.m_annotation;
                h.m_sampleBinding   = syncedNote.m_sampleBinding;
                h.m_collaborationId = syncedNote.m_collaborationId;
                newNoteData.holds.push_back(std::move(h));
                newAllNotes.push_back(newNoteData.holds.back());
            } else if ( nc.m_type == ::MMM::NoteType::FLICK ) {
                Flick f;
                f.m_type       = ::MMM::NoteType::FLICK;
                f.m_timestamp  = syncedNote.m_timestamp * 1000.0;
                f.m_track      = static_cast<uint32_t>(syncedNote.m_trackIndex);
                f.m_dtrack     = syncedNote.m_dtrack;
                f.m_metadata   = syncedNote.m_metadata;
                f.m_annotation = syncedNote.m_annotation;
                f.m_sampleBinding   = syncedNote.m_sampleBinding;
                f.m_collaborationId = syncedNote.m_collaborationId;
                newNoteData.flicks.push_back(std::move(f));
                newAllNotes.push_back(newNoteData.flicks.back());
            }
        }

        for ( auto entity : noteView ) {
            const auto& nc = noteView.get<NoteComponent>(entity);
            if ( nc.m_isDraft || nc.m_type != ::MMM::NoteType::POLYLINE ) {
                continue;
            }

            NoteComponent syncedPolyline = nc;
            if ( hasAnyNoteColorOverride(syncedPolyline.m_customColors) ) {
                writeNoteColorOverridesToMetadata(syncedPolyline);
            }

            Polyline p;
            p.m_type       = ::MMM::NoteType::POLYLINE;
            p.m_timestamp  = syncedPolyline.m_timestamp * 1000.0;
            p.m_track      = static_cast<uint32_t>(syncedPolyline.m_trackIndex);
            p.m_metadata   = syncedPolyline.m_metadata;
            p.m_annotation = syncedPolyline.m_annotation;
            p.m_sampleBinding   = syncedPolyline.m_sampleBinding;
            p.m_collaborationId = syncedPolyline.m_collaborationId;

            for ( const auto& sub_note : syncedPolyline.m_subNotes ) {
                NoteComponent::SubNote syncedSubNote = sub_note;
                if ( hasAnyNoteColorOverride(syncedSubNote.customColors) ) {
                    writeNoteColorOverridesToMetadata(syncedSubNote);
                }

                if ( syncedSubNote.type == ::MMM::NoteType::NOTE ) {
                    Note n;
                    n.m_type      = ::MMM::NoteType::NOTE;
                    n.m_timestamp = syncedSubNote.timestamp * 1000.0;
                    n.m_track = static_cast<uint32_t>(syncedSubNote.trackIndex);
                    n.m_isSubNote       = true;
                    n.m_metadata        = syncedSubNote.metadata;
                    n.m_annotation      = syncedSubNote.annotation;
                    n.m_sampleBinding   = syncedSubNote.sampleBinding;
                    n.m_collaborationId = syncedSubNote.collaborationId;
                    newNoteData.notes.push_back(std::move(n));
                    auto& ref = newNoteData.notes.back();
                    p.m_subNotes.push_back(ref);
                    newAllNotes.push_back(ref);
                } else if ( syncedSubNote.type == ::MMM::NoteType::HOLD ) {
                    Hold h;
                    h.m_type      = ::MMM::NoteType::HOLD;
                    h.m_timestamp = syncedSubNote.timestamp * 1000.0;
                    h.m_track = static_cast<uint32_t>(syncedSubNote.trackIndex);
                    h.m_duration        = syncedSubNote.duration * 1000.0;
                    h.m_isSubNote       = true;
                    h.m_metadata        = syncedSubNote.metadata;
                    h.m_annotation      = syncedSubNote.annotation;
                    h.m_sampleBinding   = syncedSubNote.sampleBinding;
                    h.m_collaborationId = syncedSubNote.collaborationId;
                    newNoteData.holds.push_back(std::move(h));
                    auto& ref = newNoteData.holds.back();
                    p.m_subNotes.push_back(ref);
                    p.m_subHolds.push_back(ref);
                    newAllNotes.push_back(ref);
                } else if ( syncedSubNote.type == ::MMM::NoteType::FLICK ) {
                    Flick f;
                    f.m_type      = ::MMM::NoteType::FLICK;
                    f.m_timestamp = syncedSubNote.timestamp * 1000.0;
                    f.m_track = static_cast<uint32_t>(syncedSubNote.trackIndex);
                    f.m_dtrack          = syncedSubNote.dtrack;
                    f.m_isSubNote       = true;
                    f.m_metadata        = syncedSubNote.metadata;
                    f.m_annotation      = syncedSubNote.annotation;
                    f.m_sampleBinding   = syncedSubNote.sampleBinding;
                    f.m_collaborationId = syncedSubNote.collaborationId;
                    newNoteData.flicks.push_back(std::move(f));
                    auto& ref = newNoteData.flicks.back();
                    p.m_subNotes.push_back(ref);
                    p.m_subFlicks.push_back(ref);
                    newAllNotes.push_back(ref);
                }
            }

            newNoteData.polylines.push_back(std::move(p));
            newAllNotes.push_back(newNoteData.polylines.back());
        }

        std::sort(newAllNotes.begin(),
                  newAllNotes.end(),
                  [](const std::reference_wrapper<Note>& a,
                     const std::reference_wrapper<Note>& b) {
                      return a.get().m_timestamp < b.get().m_timestamp;
                  });
    }

    if ( ctx.m_needsSamplesSync ) {
        auto sampleView = ctx.sampleRegistry.view<const SampleComponent>();
        std::vector<const SampleComponent*> sortedSamples;
        sortedSamples.reserve(sampleView.size());
        for ( auto entity : sampleView ) {
            sortedSamples.push_back(
                &sampleView.get<const SampleComponent>(entity));
        }
        std::sort(sortedSamples.begin(),
                  sortedSamples.end(),
                  [](const SampleComponent* lhs, const SampleComponent* rhs) {
                      if ( lhs->m_timestamp != rhs->m_timestamp ) {
                          return lhs->m_timestamp < rhs->m_timestamp;
                      }
                      if ( lhs->m_track != rhs->m_track ) {
                          return lhs->m_track < rhs->m_track;
                      }
                      if ( lhs->m_offsetMs != rhs->m_offsetMs ) {
                          return lhs->m_offsetMs < rhs->m_offsetMs;
                      }
                      return lhs->m_audioResourceId < rhs->m_audioResourceId;
                  });
        for ( const auto* sample : sortedSamples ) {
            newAudioSamples.push_back(sample->toAudioSample());
        }
    }

    {
        auto& mutex = EditorEngine::instance().getSessionMutex();
        std::lock_guard<std::recursive_mutex> lock(mutex);

        if ( ctx.m_needsTimingsSync ) {
            ctx.currentBeatmap->m_timings.swap(newTimings);
        }
        if ( ctx.m_needsNotesSync ) {
            ctx.currentBeatmap->m_allNotes.swap(newAllNotes);
            ctx.currentBeatmap->m_noteData.notes.swap(newNoteData.notes);
            ctx.currentBeatmap->m_noteData.holds.swap(newNoteData.holds);
            ctx.currentBeatmap->m_noteData.flicks.swap(newNoteData.flicks);
            ctx.currentBeatmap->m_noteData.polylines.swap(
                newNoteData.polylines);
        }
        if ( ctx.m_needsSamplesSync ) {
            ctx.currentBeatmap->m_audioSamples.swap(newAudioSamples);
            ctx.currentBeatmap->m_baseMapMetadata.bgm_track_count =
                std::max(0, ctx.bgmTrackCount);
        }
    }

    ctx.isAudioTimelineDescriptorDirty = true;
    ctx.m_needsTimingsSync             = false;
    ctx.m_needsNotesSync               = false;
    ctx.m_needsSamplesSync             = false;
}

}  // namespace MMM::Logic
