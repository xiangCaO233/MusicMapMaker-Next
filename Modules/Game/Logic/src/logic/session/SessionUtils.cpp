#include "logic/session/SessionUtils.h"
#include "audio/AudioManager.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "mmm/timing/BpmNormalization.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace MMM::Logic::SessionUtils
{

namespace
{
/// @brief 单个顶层音符对状态栏统计的贡献。
struct NoteStatisticsContribution {
    /// @brief 可计数物件数量。
    std::size_t noteCount{ 0U };
    /// @brief 最大连击数量。
    std::size_t maxCombo{ 0U };
};

/// @brief 计算 Hold 区间内的四分之一拍连击增量。
std::size_t calculateIntervalCombos(double startTime, double endTime,
                                    const ::MMM::BeatMap* beatmap)
{
    if ( !beatmap || endTime <= startTime ) return 0U;

    double totalQuarterBeats = 0.0;
    double currentTime       = startTime;
    double currentBpm =
        ::MMM::normalizeBpmValue(beatmap->m_baseMapMetadata.preference_bpm);
    std::size_t nextTimingIndex = 0U;

    for ( std::size_t index = 0U; index < beatmap->m_timings.size(); ++index ) {
        const auto& timing = beatmap->m_timings[index];
        if ( timing.m_timingEffect != ::MMM::TimingEffect::BPM ) continue;
        if ( timing.m_timestamp <= startTime ) {
            currentBpm = ::MMM::normalizeBpmValue(timing.m_bpm, currentBpm);
        } else {
            nextTimingIndex = index;
            break;
        }
    }

    while ( currentTime < endTime ) {
        double      nextEventTime = endTime;
        double      nextBpm       = currentBpm;
        std::size_t foundIndex    = beatmap->m_timings.size();
        for ( std::size_t index = nextTimingIndex;
              index < beatmap->m_timings.size();
              ++index ) {
            const auto& timing = beatmap->m_timings[index];
            if ( timing.m_timingEffect == ::MMM::TimingEffect::BPM &&
                 timing.m_timestamp > currentTime ) {
                if ( timing.m_timestamp < endTime ) {
                    nextEventTime = timing.m_timestamp;
                    nextBpm =
                        ::MMM::normalizeBpmValue(timing.m_bpm, currentBpm);
                    foundIndex = index + 1U;
                }
                break;
            }
        }
        totalQuarterBeats +=
            (nextEventTime - currentTime) * (currentBpm / 15.0);
        currentTime = nextEventTime;
        currentBpm  = nextBpm;
        if ( foundIndex < beatmap->m_timings.size() ) {
            nextTimingIndex = foundIndex;
        }
    }

    const double tolerance = 0.003 * (currentBpm / 15.0);
    return static_cast<std::size_t>(std::floor(totalQuarterBeats + tolerance));
}

/// @brief 计算单个音符组件对状态栏统计的贡献。
NoteStatisticsContribution calculateNoteStatistics(
    const NoteComponent& note, const ::MMM::BeatMap* beatmap)
{
    NoteStatisticsContribution result;
    if ( note.m_isDraft || note.m_isSubNote ) return result;

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( const auto& sub : note.m_subNotes ) {
            if ( sub.type == ::MMM::NoteType::NOTE ||
                 sub.type == ::MMM::NoteType::HOLD ||
                 sub.type == ::MMM::NoteType::FLICK ) {
                ++result.noteCount;
            }
        }
    } else if ( note.m_type == ::MMM::NoteType::NOTE ||
                note.m_type == ::MMM::NoteType::HOLD ||
                note.m_type == ::MMM::NoteType::FLICK ) {
        ++result.noteCount;
    }

    if ( note.m_type == ::MMM::NoteType::NOTE ||
         note.m_type == ::MMM::NoteType::FLICK ) {
        ++result.maxCombo;
    } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
        ++result.maxCombo;
        result.maxCombo += calculateIntervalCombos(
            note.m_timestamp, note.m_timestamp + note.m_duration, beatmap);
    } else if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                !note.m_subNotes.empty() ) {
        ++result.maxCombo;
        if ( note.m_subNotes.front().type == ::MMM::NoteType::HOLD ) {
            const auto& first = note.m_subNotes.front();
            result.maxCombo += calculateIntervalCombos(
                first.timestamp, first.timestamp + first.duration, beatmap);
        }
        for ( std::size_t index = 1U; index < note.m_subNotes.size();
              ++index ) {
            const auto& sub = note.m_subNotes[index];
            if ( sub.type == ::MMM::NoteType::FLICK ) {
                ++result.maxCombo;
            } else if ( sub.type == ::MMM::NoteType::HOLD ) {
                result.maxCombo += calculateIntervalCombos(
                    sub.timestamp, sub.timestamp + sub.duration, beatmap);
            }
        }
    }
    return result;
}

/// @brief 将单个顶层音符的可计数时间追加到输出缓存。
void appendDensityTimes(const NoteComponent& note, std::vector<double>& output)
{
    if ( note.m_isDraft || note.m_isSubNote ) return;
    const auto append = [&output](::MMM::NoteType type, double timestamp) {
        if ( (type == ::MMM::NoteType::NOTE || type == ::MMM::NoteType::HOLD ||
              type == ::MMM::NoteType::FLICK) &&
             std::isfinite(timestamp) && timestamp >= 0.0 ) {
            output.push_back(timestamp);
        }
    };
    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( const auto& sub : note.m_subNotes ) {
            append(sub.type, sub.timestamp);
        }
    } else {
        append(note.m_type, note.m_timestamp);
    }
}

/// @brief 计算音符用于可见性前缀的结束时间。
double noteEndTime(const NoteComponent& note)
{
    double result = note.m_timestamp + std::max(0.0, note.m_duration);
    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( const auto& sub : note.m_subNotes ) {
            result =
                std::max(result, sub.timestamp + std::max(0.0, sub.duration));
        }
    }
    return result;
}

/// @brief 判断两个可选采样绑定是否相同。
bool sameSampleBinding(const std::optional<::MMM::AudioSampleBinding>& lhs,
                       const std::optional<::MMM::AudioSampleBinding>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    return !lhs || (lhs->m_audioResourceId == rhs->m_audioResourceId &&
                    lhs->m_volume == rhs->m_volume);
}

/// @brief 将单个正式或草稿顶层音符转换为打击事件。
void appendHitEvents(const NoteComponent&                        note,
                     std::vector<System::HitFXSystem::HitEvent>& output)
{
    using HitEvent = System::HitFXSystem::HitEvent;
    using HitRole  = HitEvent::Role;
    if ( note.m_isSubNote ) return;

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( std::size_t index = 0U; index < note.m_subNotes.size();
              ++index ) {
            const auto& sub = note.m_subNotes[index];
            HitRole   role = index == 0U ? HitRole::Head
                                         : (index + 1U == note.m_subNotes.size()
                                                ? HitRole::Tail
                                                : HitRole::Internal);
            const int span = sub.type == ::MMM::NoteType::FLICK
                                 ? std::abs(sub.dtrack) + 1
                                 : 1;
            auto      binding = sub.sampleBinding;
            if ( !binding && role == HitRole::Head ) {
                binding = note.m_sampleBinding;
            }
            output.push_back({ sub.timestamp,
                               sub.type,
                               role,
                               span,
                               sub.trackIndex,
                               sub.dtrack,
                               sub.duration,
                               true,
                               std::move(binding),
                               note.m_isDraft });
        }
        return;
    }

    const int span =
        note.m_type == ::MMM::NoteType::FLICK ? std::abs(note.m_dtrack) + 1 : 1;
    output.push_back({ note.m_timestamp,
                       note.m_type,
                       HitRole::None,
                       span,
                       note.m_trackIndex,
                       note.m_dtrack,
                       note.m_duration,
                       false,
                       note.m_sampleBinding,
                       note.m_isDraft });
}

/// @brief 判断两个打击事件是否来自同一个音符状态。
bool sameHitEvent(const System::HitFXSystem::HitEvent& lhs,
                  const System::HitFXSystem::HitEvent& rhs)
{
    return lhs.timestamp == rhs.timestamp && lhs.type == rhs.type &&
           lhs.role == rhs.role && lhs.trackSpan == rhs.trackSpan &&
           lhs.trackIndex == rhs.trackIndex &&
           lhs.trackOffset == rhs.trackOffset && lhs.duration == rhs.duration &&
           lhs.isSubNote == rhs.isSubNote && lhs.isDraft == rhs.isDraft &&
           sameSampleBinding(lhs.sampleBinding, rhs.sampleBinding);
}
}  // namespace

bool applyNoteCacheMutationsIncrementally(
    SessionContext& ctx, std::span<const NoteCacheMutationView> mutations)
{
    if ( mutations.empty() ) return true;
    if ( ctx.isNoteOrderDirty || ctx.isNotePruneDirty || ctx.isNoteStatsDirty ||
         ctx.sortedNoteEntities.size() != ctx.sortedNoteMaxEndPrefix.size() ) {
        return false;
    }

    std::unordered_set<entt::entity>           affectedEntities;
    std::vector<entt::entity>                  replacementEntities;
    std::vector<double>                        removedDensityTimes;
    std::vector<double>                        addedDensityTimes;
    std::vector<System::HitFXSystem::HitEvent> removedHitEvents;
    std::vector<System::HitFXSystem::HitEvent> addedHitEvents;
    NoteStatisticsContribution                 beforeStatistics;
    NoteStatisticsContribution                 afterStatistics;
    double earliestTimestamp   = std::numeric_limits<double>::infinity();
    bool   touchesHitEventNote = false;

    affectedEntities.reserve(mutations.size());
    replacementEntities.reserve(mutations.size());
    for ( const auto& mutation : mutations ) {
        if ( mutation.entity == entt::null ||
             !affectedEntities.insert(mutation.entity).second ) {
            return false;
        }
        if ( mutation.before ) {
            earliestTimestamp =
                std::min(earliestTimestamp, mutation.before->m_timestamp);
            const auto contribution = calculateNoteStatistics(
                *mutation.before, ctx.currentBeatmap.get());
            beforeStatistics.noteCount += contribution.noteCount;
            beforeStatistics.maxCombo += contribution.maxCombo;
            appendDensityTimes(*mutation.before, removedDensityTimes);
            if ( !mutation.before->m_isSubNote ) {
                touchesHitEventNote = true;
                if ( !ctx.isHitEventsDirty ) {
                    appendHitEvents(*mutation.before, removedHitEvents);
                }
            }
        }
        if ( mutation.after ) {
            if ( !ctx.noteRegistry.valid(mutation.entity) ||
                 !ctx.noteRegistry.all_of<NoteComponent>(mutation.entity) ) {
                return false;
            }
            replacementEntities.push_back(mutation.entity);
            earliestTimestamp =
                std::min(earliestTimestamp, mutation.after->m_timestamp);
            const auto contribution = calculateNoteStatistics(
                *mutation.after, ctx.currentBeatmap.get());
            afterStatistics.noteCount += contribution.noteCount;
            afterStatistics.maxCombo += contribution.maxCombo;
            appendDensityTimes(*mutation.after, addedDensityTimes);
            if ( !mutation.after->m_isSubNote ) {
                touchesHitEventNote = true;
                if ( !ctx.isHitEventsDirty ) {
                    appendHitEvents(*mutation.after, addedHitEvents);
                }
            }
        }
    }

    for ( const auto& mutation : mutations ) {
        if ( mutation.before &&
             std::find(ctx.sortedNoteEntities.begin(),
                       ctx.sortedNoteEntities.end(),
                       mutation.entity) == ctx.sortedNoteEntities.end() ) {
            return false;
        }
    }

    std::erase_if(ctx.sortedNoteEntities, [&](entt::entity entity) {
        return affectedEntities.contains(entity);
    });
    const auto entityLess = [&ctx](entt::entity lhs, entt::entity rhs) {
        return ctx.noteRegistry.get<const NoteComponent>(lhs).m_timestamp <
               ctx.noteRegistry.get<const NoteComponent>(rhs).m_timestamp;
    };
    std::sort(
        replacementEntities.begin(), replacementEntities.end(), entityLess);
    std::vector<entt::entity> mergedEntities;
    mergedEntities.reserve(ctx.sortedNoteEntities.size() +
                           replacementEntities.size());
    std::merge(ctx.sortedNoteEntities.begin(),
               ctx.sortedNoteEntities.end(),
               replacementEntities.begin(),
               replacementEntities.end(),
               std::back_inserter(mergedEntities),
               entityLess);
    ctx.sortedNoteEntities.swap(mergedEntities);

    const auto prefixBegin = std::lower_bound(
        ctx.sortedNoteEntities.begin(),
        ctx.sortedNoteEntities.end(),
        earliestTimestamp,
        [&ctx](entt::entity entity, double timestamp) {
            return ctx.noteRegistry.get<const NoteComponent>(entity)
                       .m_timestamp < timestamp;
        });
    const auto prefixIndex = static_cast<std::size_t>(
        std::distance(ctx.sortedNoteEntities.begin(), prefixBegin));
    ctx.sortedNoteMaxEndPrefix.resize(ctx.sortedNoteEntities.size());
    double maximumEnd =
        prefixIndex == 0U ? 0.0 : ctx.sortedNoteMaxEndPrefix[prefixIndex - 1U];
    for ( std::size_t index = prefixIndex;
          index < ctx.sortedNoteEntities.size();
          ++index ) {
        const auto entity = ctx.sortedNoteEntities[index];
        maximumEnd        = std::max(
            maximumEnd,
            noteEndTime(ctx.noteRegistry.get<const NoteComponent>(entity)));
        ctx.sortedNoteMaxEndPrefix[index] = maximumEnd;
    }

    ctx.noteCount = ctx.noteCount >= beforeStatistics.noteCount
                        ? ctx.noteCount - beforeStatistics.noteCount
                        : 0U;
    ctx.maxCombo  = ctx.maxCombo >= beforeStatistics.maxCombo
                        ? ctx.maxCombo - beforeStatistics.maxCombo
                        : 0U;
    ctx.noteCount += afterStatistics.noteCount;
    ctx.maxCombo += afterStatistics.maxCombo;

    for ( const double timestamp : removedDensityTimes ) {
        const auto found =
            std::lower_bound(ctx.previewDensityObjectTimes.begin(),
                             ctx.previewDensityObjectTimes.end(),
                             timestamp);
        if ( found == ctx.previewDensityObjectTimes.end() ||
             *found != timestamp ) {
            return false;
        }
        ctx.previewDensityObjectTimes.erase(found);
    }
    for ( const double timestamp : addedDensityTimes ) {
        const auto insertion =
            std::upper_bound(ctx.previewDensityObjectTimes.begin(),
                             ctx.previewDensityObjectTimes.end(),
                             timestamp);
        ctx.previewDensityObjectTimes.insert(insertion, timestamp);
    }
    ctx.isPreviewDensityDirty =
        !removedDensityTimes.empty() || !addedDensityTimes.empty();

    if ( touchesHitEventNote && !ctx.isHitEventsDirty ) {
        for ( const auto& event : removedHitEvents ) {
            const auto range = std::equal_range(
                ctx.hitEvents.begin(), ctx.hitEvents.end(), event);
            const auto found = std::find_if(
                range.first, range.second, [&](const auto& current) {
                    return sameHitEvent(current, event);
                });
            if ( found == range.second ) {
                ctx.isHitEventsDirty = true;
                break;
            }
            ctx.hitEvents.erase(found);
        }
        if ( !ctx.isHitEventsDirty ) {
            for ( auto& event : addedHitEvents ) {
                const auto insertion = std::upper_bound(
                    ctx.hitEvents.begin(), ctx.hitEvents.end(), event);
                ctx.hitEvents.insert(insertion, std::move(event));
            }
            syncHitIndex(ctx);
        }
    }

    ++ctx.noteVisibilityIndexRevision;
    ctx.isNoteOrderDirty = false;
    ctx.isNotePruneDirty = false;
    ctx.isNoteStatsDirty = false;
    return true;
}

/// @brief 根据共用专业模式与独立折线编辑开关判断音符是否可编辑。
/// @warning 逻辑与渲染热路径：只读取组件属性和配置布尔值。
bool isNoteEditable(const NoteComponent&          note,
                    const Config::EditorSettings& settings)
{
    if ( note.m_isDraft && !settings.professionalMode ) return false;
    if ( settings.enablePolylineEditing ) return true;
    return !note.m_isSubNote && (note.m_type == ::MMM::NoteType::NOTE ||
                                 note.m_type == ::MMM::NoteType::HOLD);
}

bool isMainCanvasCameraId(const std::string& cameraId)
{
    return cameraId != "Preview" && cameraId != "PreviewCanvas" &&
           cameraId != "Timeline" && cameraId != "AudioWaveform" &&
           cameraId != "AudioSpectrum";
}

const CameraInfo* findMainCanvasCamera(
    const std::unordered_map<std::string, CameraInfo>& cameras)
{
    auto itLegacy = cameras.find("Basic2DCanvas");
    if ( itLegacy != cameras.end() ) {
        return &itLegacy->second;
    }

    for ( const auto& [cameraId, camera] : cameras ) {
        if ( isMainCanvasCameraId(cameraId) ) {
            return &camera;
        }
    }

    return nullptr;
}

int calculateBeatIndex(double                                       time,
                       const std::vector<const TimelineComponent*>& bpmEvents,
                       double                                       fallbackBpm)
{
    if ( bpmEvents.empty() || !std::isfinite(time) ) {
        return 0;
    }

    int64_t totalBeats = 0;
    for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
        const auto* currentBpm = bpmEvents[i];
        if ( !currentBpm ) {
            continue;
        }

        const double bpm =
            ::MMM::normalizeBpmValue(currentBpm->m_value, fallbackBpm);

        const double nextBpmTime =
            i + 1 < bpmEvents.size() && bpmEvents[i + 1]
                ? bpmEvents[i + 1]->m_timestamp
                : std::numeric_limits<double>::infinity();
        if ( time >= currentBpm->m_timestamp && time < nextBpmTime ) {
            const double beatDuration = 60.0 / bpm;
            const auto   beatsInBpm   = static_cast<int64_t>(std::floor(
                (time - currentBpm->m_timestamp) / beatDuration + 1e-6));
            return static_cast<int>(totalBeats + beatsInBpm + 1);
        }
        if ( time >= nextBpmTime ) {
            const double beatDuration = 60.0 / bpm;
            const double segmentDuration =
                nextBpmTime - currentBpm->m_timestamp;
            auto beatsInBpm = static_cast<int64_t>(
                std::ceil(segmentDuration / beatDuration - 1e-6));
            if ( segmentDuration > 0.0 ) {
                beatsInBpm = std::max<int64_t>(beatsInBpm, 1);
            }
            totalBeats += beatsInBpm;
            continue;
        }
        break;
    }
    return 0;
}

double getEffectiveTotalTimeSeconds(const SessionContext& ctx)
{
    double totalTime = ctx.audioTimelineDescriptor.m_chartEndSeconds;
    if ( ctx.audioTimelineTotalTime > 0.0 &&
         std::isfinite(ctx.audioTimelineTotalTime) ) {
        totalTime = std::max(totalTime, ctx.audioTimelineTotalTime);
    }

    const auto& audio = Audio::AudioManager::instance();
    if ( !ctx.audioTimelineDescriptor.m_fingerprint.empty() &&
         audio.getLoadedAudioTimelineFingerprint() ==
             ctx.audioTimelineDescriptor.m_fingerprint ) {
        totalTime = std::max(totalTime, audio.getTotalTime());
    }
    return std::max(0.0, totalTime);
}

double calculateChartContentEndSeconds(const MMM::BeatMap& beatMap)
{
    double     chartEndMs       = 0.0;
    const auto includeTimestamp = [&chartEndMs](double timestampMs) {
        if ( std::isfinite(timestampMs) ) {
            chartEndMs = std::max(chartEndMs, timestampMs);
        }
    };
    const auto includeDuration = [&chartEndMs](double timestampMs,
                                               double durationMs) {
        if ( std::isfinite(timestampMs) && std::isfinite(durationMs) ) {
            chartEndMs =
                std::max(chartEndMs, timestampMs + std::max(durationMs, 0.0));
        }
    };

    for ( const auto& timing : beatMap.m_timings ) {
        includeTimestamp(timing.m_timestamp);
    }
    for ( const auto& note : beatMap.m_noteData.notes ) {
        includeTimestamp(note.m_timestamp);
    }
    for ( const auto& hold : beatMap.m_noteData.holds ) {
        includeDuration(hold.m_timestamp, hold.m_duration);
    }
    for ( const auto& flick : beatMap.m_noteData.flicks ) {
        includeTimestamp(flick.m_timestamp);
    }
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        includeTimestamp(polyline.m_timestamp);
        for ( const auto& subNoteReference : polyline.m_subNotes ) {
            const auto& subNote = subNoteReference.get();
            if ( subNote.m_type == NoteType::HOLD ) {
                const auto& subHold = static_cast<const Hold&>(subNote);
                includeDuration(subHold.m_timestamp, subHold.m_duration);
            } else {
                includeTimestamp(subNote.m_timestamp);
            }
        }
    }
    return std::max(chartEndMs, 0.0) / 1000.0;
}

bool rebuildAudioTimelineDescriptor(SessionContext&       ctx,
                                    const ::MMM::Project* project)
{
    const std::string previousFingerprint =
        ctx.audioTimelineDescriptor.m_fingerprint;
    const bool activationWasPending = ctx.isAudioTimelineActivationPending;
    if ( !ctx.currentBeatmap ) {
        ctx.audioTimelineDescriptor                  = {};
        ctx.audioTimelineTotalTime                   = 0.0;
        ctx.isAudioTimelineDescriptorDirty           = false;
        ctx.isAudioTimelineActivationPending         = true;
        ctx.isAudioTimelineFingerprintPublishPending = true;
        return !previousFingerprint.empty();
    }

    Project fallbackProject;
    if ( !project ) {
        fallbackProject.m_projectRoot =
            ctx.currentBeatmap->m_baseMapMetadata.map_path.parent_path();
    }
    const Project& descriptorProject = project ? *project : fallbackProject;
    ctx.audioTimelineDescriptor      = buildAudioTimelineDescriptor(
        *ctx.currentBeatmap,
        descriptorProject,
        ctx.currentBeatmap->m_baseMapMetadata.map_path,
        calculateChartContentEndSeconds(*ctx.currentBeatmap));
    ctx.audioTimelineTotalTime = ctx.audioTimelineDescriptor.m_chartEndSeconds;
    const bool fingerprintChanged =
        previousFingerprint != ctx.audioTimelineDescriptor.m_fingerprint;
    ctx.isAudioTimelineDescriptorDirty = false;
    ctx.isAudioTimelineActivationPending =
        activationWasPending || fingerprintChanged;
    ctx.isAudioTimelineFingerprintPublishPending = true;

    for ( const auto& diagnostic : ctx.audioTimelineDescriptor.m_diagnostics ) {
        XWARN("Audio timeline descriptor: {}", diagnostic.m_message);
    }
    return fingerprintChanged;
}

bool activateAudioTimeline(SessionContext& ctx, bool shouldPlay)
{
    if ( !ctx.isActiveSession ) {
        return false;
    }

    if ( ctx.isAudioTimelineDescriptorDirty ) {
        const auto* project =
            ctx.collaborationProject
                ? ctx.collaborationProject.get()
                : EditorEngine::instance().getCurrentProject();
        rebuildAudioTimelineDescriptor(ctx, project);
    }

    auto& audio = Audio::AudioManager::instance();
    if ( !ctx.currentBeatmap ) {
        audio.unloadAudioTimeline();
        ctx.audioTimelineTotalTime           = 0.0;
        ctx.missingAudioTimelineClipCount    = 0U;
        ctx.isAudioTimelineActivationPending = false;
        return false;
    }

    const auto& descriptor = ctx.audioTimelineDescriptor;
    const bool  needsReload =
        ctx.isAudioTimelineActivationPending ||
        !audio.hasLoadedAudioTimeline() ||
        audio.getLoadedAudioTimelineFingerprint() != descriptor.m_fingerprint;
    if ( needsReload ) {
        const auto result =
            audio.loadAudioTimeline(descriptor.m_events,
                                    descriptor.m_chartEndSeconds,
                                    descriptor.m_fingerprint);
        ctx.isAudioTimelineActivationPending = false;
        ctx.missingAudioTimelineClipCount    = result.missingClipCount;
        if ( !result.success ) {
            ctx.audioTimelineTotalTime = descriptor.m_chartEndSeconds;
            for ( const auto& diagnostic : result.diagnostics ) {
                XWARN("Audio timeline load: {}", diagnostic.message);
            }
            return false;
        }
        for ( const auto& diagnostic : result.diagnostics ) {
            XWARN("Audio timeline load: {}", diagnostic.message);
        }
    }

    ctx.audioTimelineTotalTime = audio.getTotalTime();
    audio.seek(ctx.currentTime);
    if ( shouldPlay ) {
        audio.play();
    } else {
        audio.pause();
    }
    const double activationTime =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    ctx.playbackVisualClock.reset();
    ctx.playbackVisualClock.rebase(
        ctx.currentTime, activationTime, audio.getPlaybackSpeed(), shouldPlay);
    return true;
}

AudioTimelineSwitchDecision resolveAudioTimelineSwitch(
    std::string_view previousSyncFingerprint,
    std::string_view targetSyncFingerprint, double previousTime,
    double targetTime, bool previousWasPlaying, bool stopPlaybackOnScroll,
    bool synchronizeMatchingTimelines)
{
    const bool sameTimeline = !previousSyncFingerprint.empty() &&
                              previousSyncFingerprint == targetSyncFingerprint;
    return {
        .m_targetTime = sameTimeline && synchronizeMatchingTimelines
                            ? previousTime
                            : targetTime,
        .m_resumePlayback =
            sameTimeline && previousWasPlaying && !stopPlaybackOnScroll,
    };
}

bool applyAudioTimelineTransportSnapshot(
    SessionContext& ctx, std::string_view loadedFingerprint,
    const Audio::AudioTimelineClockSnapshot& snapshot, double nowSteadySeconds,
    const Config::SyncConfig& syncConfig)
{
    const std::string_view expectedFingerprint =
        ctx.isAudioTimelineSyncFollower
            ? std::string_view(ctx.m_audioTimelineSyncSourceFingerprint)
            : std::string_view(ctx.audioTimelineDescriptor.m_fingerprint);
    const bool readsCurrentTransport = !expectedFingerprint.empty() &&
                                       loadedFingerprint == expectedFingerprint;
    if ( !readsCurrentTransport ) {
        ctx.isPlaying                   = false;
        ctx.isAudioTimelineSyncFollower = false;
        ctx.m_audioTimelineSyncSourceFingerprint.clear();
        ctx.playbackVisualClock.reset();
        return false;
    }

    const double currentTime =
        ctx.isAudioTimelineSyncFollower
            ? (ctx.playbackVisualClock.initialized()
                   ? ctx.playbackVisualClock.resolveAt(nowSteadySeconds)
                   : ctx.currentTime)
            : ctx.playbackVisualClock.update(
                  snapshot, nowSteadySeconds, syncConfig);
    if ( std::isfinite(currentTime) ) {
        const double totalTime = getEffectiveTotalTimeSeconds(ctx);
        ctx.currentTime =
            totalTime > 0.0 ? std::min(currentTime, totalTime) : currentTime;
    }
    if ( ctx.isPlaying && snapshot.valid &&
         snapshot.state == Audio::AudioTimelinePlaybackState::Stopped ) {
        ctx.restartPlaybackAfterFinishPending = snapshot.finished;
        ctx.isPlaying                         = false;
        return false;
    }
    if ( ctx.isAudioTimelineSyncFollower && snapshot.valid &&
         snapshot.state != Audio::AudioTimelinePlaybackState::Playing ) {
        ctx.isAudioTimelineSyncFollower = false;
        ctx.m_audioTimelineSyncSourceFingerprint.clear();
        return false;
    }
    return ctx.isPlaying || ctx.isAudioTimelineSyncFollower;
}

SnapResult calculateObjectPlacementSnap(double rawTime, double timingTime,
                                        double nextTimingTime, double bpm,
                                        const Config::EditorSettings& settings)
{
    SnapResult result;
    if ( !settings.objectPlacementSnap || !std::isfinite(rawTime) ||
         !std::isfinite(timingTime) || !std::isfinite(bpm) || bpm <= 0.0 ) {
        return result;
    }

    const double beatDuration = 60.0 / bpm;
    if ( !std::isfinite(beatDuration) || beatDuration <= 1e-9 ) {
        return result;
    }

    double bestDistance    = std::numeric_limits<double>::infinity();
    auto   considerDivisor = [&](int divisor) {
        if ( divisor <= 0 ) return;

        const double stepDuration = beatDuration / divisor;
        if ( !std::isfinite(stepDuration) || stepDuration <= 1e-9 ) return;

        const double relativeTime = rawTime - timingTime;
        const double stepCount =
            settings.snapFloor ? std::floor(relativeTime / stepDuration + 1e-6)
                               : std::round(relativeTime / stepDuration);
        double candidate = timingTime + stepCount * stepDuration;
        if ( candidate > nextTimingTime ) candidate = nextTimingTime;
        if ( !std::isfinite(candidate) ) return;

        const double distance = std::abs(candidate - rawTime);
        if ( distance >= bestDistance - 1e-12 ) return;

        result.isSnapped   = true;
        result.snappedTime = candidate;
        bestDistance       = distance;

        if ( std::isfinite(nextTimingTime) &&
             std::abs(candidate - nextTimingTime) <= 1e-9 ) {
            result.numerator   = 1;
            result.denominator = 1;
            return;
        }

        auto stepIndex = static_cast<std::int64_t>(
            std::llround((candidate - timingTime) / stepDuration));
        int beatIndex = static_cast<int>(stepIndex % divisor);
        if ( beatIndex < 0 ) beatIndex += divisor;
        if ( beatIndex == 0 ) {
            result.numerator   = 1;
            result.denominator = 1;
            return;
        }
        const int factor   = std::gcd(beatIndex, divisor);
        result.numerator   = beatIndex / factor;
        result.denominator = divisor / factor;
    };

    if ( settings.objectPlacementSnapMode ==
         Config::ObjectPlacementSnapMode::CommonBeatDivisors ) {
        for ( int divisor = Config::COMMON_BEAT_DIVISOR_MIN;
              divisor <= Config::COMMON_BEAT_DIVISOR_MAX;
              ++divisor ) {
            if ( Config::isCommonBeatDivisorEnabled(
                     settings.commonBeatDivisorMask, divisor) ) {
                considerDivisor(divisor);
            }
        }
    } else {
        considerDivisor(std::max(settings.beatDivisor, 1));
    }

    return result;
}

SnapResult getSnapResult(
    double rawTime, float mouseY, const CameraInfo& camera,
    const Config::EditorConfig&                  config,
    const std::vector<const TimelineComponent*>& bpmEvents,
    entt::registry& timelineRegistry, double animateTime,
    const std::unordered_map<std::string, CameraInfo>& cameras,
    double                                             fallbackBpm)
{
    SnapResult result;

    auto* cache = timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return result;

    if ( bpmEvents.empty() ) return result;

    /// @brief 首个 BPM 前是否沿用首个 BPM 向前反推分拍网格。
    const bool allowBeforeFirstTiming =
        config.visual.drawBeatLinesBeforeFirstTiming;
    if ( rawTime < bpmEvents[0]->m_timestamp && !allowBeforeFirstTiming )
        return result;

    float  judgmentLineY = camera.viewportHeight * config.visual.judgeline_pos;
    double currentAbsY   = cache->getAbsY(animateTime);

    float renderScaleY = 1.0f;
    if ( camera.id == "Preview" || camera.id == "PreviewCanvas" ) {
        const auto* mainCamera = findMainCanvasCamera(cameras);
        float       mainViewportHeight =
            mainCamera ? mainCamera->viewportHeight : camera.viewportHeight;

        float mainEffectiveH =
            (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
            mainViewportHeight;
        float ty = config.visual.previewConfig.margin.top;
        float by =
            camera.viewportHeight - config.visual.previewConfig.margin.bottom;
        float previewDrawH = by - ty;

        renderScaleY = previewDrawH /
                       (mainEffectiveH * config.visual.previewConfig.areaRatio);
    }

    for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
        const auto* currentBPM  = bpmEvents[i];
        double      bpmTime     = currentBPM->m_timestamp;
        double      bpmVal      = currentBPM->m_value;
        double      nextBpmTime = (i + 1 < bpmEvents.size())
                                      ? bpmEvents[i + 1]->m_timestamp
                                      : std::numeric_limits<double>::infinity();

        if ( rawTime < bpmTime && i > 0 ) continue;
        if ( rawTime > nextBpmTime ) continue;

        double bVal = bpmVal;
        if ( !std::isfinite(bVal) || bVal <= 0.0 ) {
            bVal = fallbackBpm;
        }
        if ( !std::isfinite(bVal) || bVal <= 0.0 ) bVal = 120.0;
        auto candidate = calculateObjectPlacementSnap(
            rawTime, bpmTime, nextBpmTime, bVal, config.settings);
        if ( !candidate.isSnapped ) continue;

        double snapAbsY = cache->getAbsY(candidate.snappedTime);
        float snapY = judgmentLineY -
                      static_cast<float>(snapAbsY - currentAbsY) * renderScaleY;
        if ( !std::isfinite(snapY) || !std::isfinite(mouseY) ) continue;

        result = candidate;
        break;
    }

    return result;
}


void syncHitIndex(SessionContext& ctx)
{
    ensureHitEvents(ctx);
    auto it = std::lower_bound(ctx.hitEvents.begin(),
                               ctx.hitEvents.end(),
                               System::HitFXSystem::HitEvent{
                                   ctx.animateTime, ::MMM::NoteType::NOTE });
    ctx.nextHitIndex                = std::distance(ctx.hitEvents.begin(), it);
    ctx.nextPredictHitIndex         = ctx.nextHitIndex;
    ctx.nextBoundSoundPrefetchIndex = ctx.nextHitIndex;
}

void ensureBpmEvents(SessionContext& ctx)
{
    if ( !ctx.isBpmEventsDirty ) return;

    ctx.bpmEvents.clear();
    auto tlView = ctx.timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        const auto& tl = tlView.get<const TimelineComponent>(entity);
        if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
            ctx.bpmEvents.push_back(&tl);
        }
    }
    std::stable_sort(
        ctx.bpmEvents.begin(),
        ctx.bpmEvents.end(),
        [](const TimelineComponent* a, const TimelineComponent* b) {
            return a->m_timestamp < b->m_timestamp;
        });
    ctx.isBpmEventsDirty = false;
}

void markHitEventsDirty(SessionContext& ctx)
{
    ctx.isHitEventsDirty = true;
}

void ensureHitEvents(SessionContext& ctx)
{
    if ( ctx.isHitEventsDirty ) {
        rebuildHitEvents(ctx);
    }
}

void rebuildHitEvents(SessionContext& ctx)
{
    ctx.hitEvents.clear();
    ctx.nextHitIndex                = 0;
    ctx.nextBoundSoundPrefetchIndex = 0;

    double maxEndTime = 0.0;

    auto view     = ctx.noteRegistry.view<NoteComponent>();
    using HitRole = System::HitFXSystem::HitEvent::Role;

    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        double noteEndTime = note.m_timestamp + note.m_duration;
        if ( !note.m_isDraft && noteEndTime > maxEndTime ) {
            maxEndTime = noteEndTime;
        }

        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            size_t subNoteCount = note.m_subNotes.size();
            for ( size_t i = 0; i < subNoteCount; ++i ) {
                const auto& sn   = note.m_subNotes[i];
                HitRole     role = HitRole::Internal;
                if ( i == 0 )
                    role = HitRole::Head;
                else if ( i == subNoteCount - 1 )
                    role = HitRole::Tail;

                int span = 1;
                if ( sn.type == ::MMM::NoteType::FLICK ) {
                    span = std::abs(sn.dtrack) + 1;
                }

                auto sampleBinding = sn.sampleBinding;
                if ( !sampleBinding && role == HitRole::Head ) {
                    sampleBinding = note.m_sampleBinding;
                }
                ctx.hitEvents.push_back({ sn.timestamp,
                                          sn.type,
                                          role,
                                          span,
                                          sn.trackIndex,
                                          sn.dtrack,
                                          sn.duration,
                                          true,
                                          std::move(sampleBinding),
                                          note.m_isDraft });

                double snEndTime = sn.timestamp + sn.duration;
                if ( !note.m_isDraft && snEndTime > maxEndTime ) {
                    maxEndTime = snEndTime;
                }
            }
        } else {
            int span = 1;
            if ( note.m_type == ::MMM::NoteType::FLICK ) {
                span = std::abs(note.m_dtrack) + 1;
            }

            ctx.hitEvents.push_back({ note.m_timestamp,
                                      note.m_type,
                                      HitRole::None,
                                      span,
                                      note.m_trackIndex,
                                      note.m_dtrack,
                                      note.m_duration,
                                      false,
                                      note.m_sampleBinding,
                                      note.m_isDraft });
        }
    }
    std::sort(ctx.hitEvents.begin(), ctx.hitEvents.end());
    ctx.isHitEventsDirty = false;
    syncHitIndex(ctx);

    if ( ctx.currentBeatmap ) {
        double maxEndTimeMs = maxEndTime * 1000.0;
        if ( maxEndTimeMs > ctx.currentBeatmap->m_baseMapMetadata.map_length ) {
            ctx.currentBeatmap->m_baseMapMetadata.map_length = maxEndTimeMs;
        }
    }
}

}  // namespace MMM::Logic::SessionUtils
