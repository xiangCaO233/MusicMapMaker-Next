#include "logic/session/SessionUtils.h"

#include "audio/AudioManager.h"
#include "common/VideoFrameDecoder.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "mmm/beatmap/BeatMap.h"
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

/// @brief 将谱面载入会话上下文并加载对应主音轨。
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

    namespace Utf8 = Config;

    ctx.noteRegistry.clear();
    ctx.timelineRegistry.clear();
    ctx.actionStack.clear();
    ctx.sortedNoteEntities.clear();
    ctx.sortedNoteMaxEndPrefix.clear();
    ctx.previewDensityObjectTimes.clear();
    ctx.previewDensityCache.clear();
    ctx.lastCameraSnapshotTimes.clear();
    ctx.isNoteOrderDirty      = true;
    ctx.isNotePruneDirty      = false;
    ctx.isNoteStatsDirty      = true;
    ctx.isPreviewDensityDirty = true;
    ctx.loadedMainAudioPath.clear();
    ctx.mainAudioTotalTime = 0.0;
    Audio::AudioManager::instance().stop();

    ctx.isPlaying               = false;
    ctx.isMainAudioSyncFollower = false;
    ctx.restartPlaybackAfterFinishPending.store(false,
                                                std::memory_order_relaxed);
    ctx.currentTime = 0.0;
    ctx.animateTime =
        ctx.currentTime + ctx.lastConfig.visual.getEffectiveVisualOffset();
    ctx.animateTimeTarget          = ctx.animateTime;
    ctx.animateTimeAnimationActive = false;
    ctx.animatedTimelineZoom       = ctx.lastConfig.visual.timelineZoom;
    ctx.animatedTimelineZoomTarget = ctx.animatedTimelineZoom;
    ctx.animatedTimelineZoomAnimationActive = false;
    ctx.currentBeatmap                      = beatmap;

    if ( !beatmap ) {
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

    // 加载音频
    std::filesystem::path audioPath = resolveMainAudioPath(ctx, project);
    std::error_code       audioFilesystemError;
    if ( !beatmap->m_baseMapMetadata.main_audio_path.empty() &&
         std::filesystem::is_regular_file(audioPath, audioFilesystemError) &&
         !audioFilesystemError ) {
        // 查找对应的 AudioResource 配置
        AudioTrackConfig config;
        if ( project ) {
            for ( const auto& res : project->m_audioResources ) {
                if ( res.m_id ==
                         Utf8::pathToUtf8(beatmap->m_baseMapMetadata
                                              .main_audio_path.filename()) ||
                     res.m_path ==
                         Utf8::pathToUtf8(
                             beatmap->m_baseMapMetadata.main_audio_path) ) {
                    config = res.m_config;
                    break;
                }
            }
        }
        const std::string audioPathUtf8 = Utf8::pathToUtf8(audioPath);
        auto&             audio         = Audio::AudioManager::instance();
        if ( audio.getLoadedBGMPath() == audioPathUtf8 ) {
            ctx.loadedMainAudioPath = audioPathUtf8;
            ctx.mainAudioTotalTime  = audio.getTotalTime();
        } else if ( audio.loadBGM(audioPathUtf8, config) ) {
            ctx.loadedMainAudioPath = audioPathUtf8;
            ctx.mainAudioTotalTime  = audio.getTotalTime();
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
        nc.m_metadata   = note.m_metadata;
        nc.m_boundSound = note.m_boundSound;
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
        nc.m_metadata   = hold.m_metadata;
        nc.m_boundSound = hold.m_boundSound;
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
        nc.m_metadata   = flick.m_metadata;
        nc.m_boundSound = flick.m_boundSound;
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
        comp.m_metadata   = polyline.m_metadata;
        comp.m_boundSound = polyline.m_boundSound;
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
            sn.metadata   = subNote.m_metadata;
            sn.boundSound = subNote.m_boundSound;
            loadNoteColorOverridesFromMetadata(sn);
            comp.m_subNotes.push_back(sn);
        }

        ctx.noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
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
                                  note.m_boundSound });
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
                                  hold.m_boundSound });
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
                                  flick.m_boundSound });
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

            ctx.hitEvents.push_back(
                { subNote.m_timestamp / 1000.0,
                  subNote.m_type,
                  role,
                  span,
                  static_cast<int>(subNote.m_track),
                  trackOffset,
                  duration,
                  true,
                  !subNote.m_boundSound.empty()
                      ? subNote.m_boundSound
                      : (role == HitRole::Head ? polyline.m_boundSound
                                               : std::string{}) });
        }
    }
    std::sort(ctx.hitEvents.begin(), ctx.hitEvents.end());
    ctx.isHitEventsDirty = false;

    XINFO(
        "Loaded new BeatMap with {} notes, {} holds, {} flicks, {} polylines "
        "and {} timings.",
        beatmap->m_noteData.notes.size(),
        beatmap->m_noteData.holds.size(),
        beatmap->m_noteData.flicks.size(),
        beatmap->m_noteData.polylines.size(),
        beatmap->m_timings.size());

    ctx.m_needsTimingsSync = false;
    ctx.m_needsNotesSync   = false;
    ctx.isNoteOrderDirty   = true;
    ctx.isNotePruneDirty   = false;
    ctx.isNoteStatsDirty   = true;
}


void SessionUtils::syncBeatmap(SessionContext& ctx)
{
    if ( !ctx.currentBeatmap ) return;
    if ( !ctx.m_needsTimingsSync && !ctx.m_needsNotesSync ) return;

    std::vector<Timing>                       newTimings;
    std::vector<std::reference_wrapper<Note>> newAllNotes;
    NoteData                                  newNoteData;

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
            if ( nc.m_isSubNote ) continue;
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
                n.m_boundSound = syncedNote.m_boundSound;
                newNoteData.notes.push_back(std::move(n));
                newAllNotes.push_back(newNoteData.notes.back());
            } else if ( nc.m_type == ::MMM::NoteType::HOLD ) {
                Hold h;
                h.m_type       = ::MMM::NoteType::HOLD;
                h.m_timestamp  = syncedNote.m_timestamp * 1000.0;
                h.m_track      = static_cast<uint32_t>(syncedNote.m_trackIndex);
                h.m_duration   = syncedNote.m_duration * 1000.0;
                h.m_metadata   = syncedNote.m_metadata;
                h.m_boundSound = syncedNote.m_boundSound;
                newNoteData.holds.push_back(std::move(h));
                newAllNotes.push_back(newNoteData.holds.back());
            } else if ( nc.m_type == ::MMM::NoteType::FLICK ) {
                Flick f;
                f.m_type       = ::MMM::NoteType::FLICK;
                f.m_timestamp  = syncedNote.m_timestamp * 1000.0;
                f.m_track      = static_cast<uint32_t>(syncedNote.m_trackIndex);
                f.m_dtrack     = syncedNote.m_dtrack;
                f.m_metadata   = syncedNote.m_metadata;
                f.m_boundSound = syncedNote.m_boundSound;
                newNoteData.flicks.push_back(std::move(f));
                newAllNotes.push_back(newNoteData.flicks.back());
            }
        }

        for ( auto entity : noteView ) {
            const auto& nc = noteView.get<NoteComponent>(entity);
            if ( nc.m_type != ::MMM::NoteType::POLYLINE ) continue;

            NoteComponent syncedPolyline = nc;
            if ( hasAnyNoteColorOverride(syncedPolyline.m_customColors) ) {
                writeNoteColorOverridesToMetadata(syncedPolyline);
            }

            Polyline p;
            p.m_type       = ::MMM::NoteType::POLYLINE;
            p.m_timestamp  = syncedPolyline.m_timestamp * 1000.0;
            p.m_track      = static_cast<uint32_t>(syncedPolyline.m_trackIndex);
            p.m_metadata   = syncedPolyline.m_metadata;
            p.m_boundSound = syncedPolyline.m_boundSound;

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
                    n.m_metadata   = syncedSubNote.metadata;
                    n.m_boundSound = syncedSubNote.boundSound;
                    newNoteData.notes.push_back(std::move(n));
                    auto& ref = newNoteData.notes.back();
                    p.m_subNotes.push_back(ref);
                    newAllNotes.push_back(ref);
                } else if ( syncedSubNote.type == ::MMM::NoteType::HOLD ) {
                    Hold h;
                    h.m_type      = ::MMM::NoteType::HOLD;
                    h.m_timestamp = syncedSubNote.timestamp * 1000.0;
                    h.m_track = static_cast<uint32_t>(syncedSubNote.trackIndex);
                    h.m_duration   = syncedSubNote.duration * 1000.0;
                    h.m_metadata   = syncedSubNote.metadata;
                    h.m_boundSound = syncedSubNote.boundSound;
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
                    f.m_dtrack     = syncedSubNote.dtrack;
                    f.m_metadata   = syncedSubNote.metadata;
                    f.m_boundSound = syncedSubNote.boundSound;
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
    }

    ctx.m_needsTimingsSync = false;
    ctx.m_needsNotesSync   = false;
}

}  // namespace MMM::Logic
