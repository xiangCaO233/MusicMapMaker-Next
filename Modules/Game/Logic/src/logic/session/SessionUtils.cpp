#include "logic/session/SessionUtils.h"
#include "audio/AudioManager.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/project/Project.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace MMM::Logic::SessionUtils
{

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

        double bpm = currentBpm->m_value;
        if ( !std::isfinite(bpm) || bpm <= 0.0 ) {
            bpm = fallbackBpm;
        }
        if ( !std::isfinite(bpm) || bpm <= 0.0 ) {
            bpm = 120.0;
        }
        bpm = std::min(bpm, 10000.0);

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
            totalBeats += static_cast<int64_t>(std::round(
                (nextBpmTime - currentBpm->m_timestamp) / beatDuration));
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
        rebuildAudioTimelineDescriptor(
            ctx, EditorEngine::instance().getCurrentProject());
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
    std::string_view previousFingerprint, std::string_view targetFingerprint,
    double previousTime, double targetTime, bool previousWasPlaying,
    bool stopPlaybackOnScroll, bool synchronizeMatchingTimelines)
{
    const bool sameTimeline = !previousFingerprint.empty() &&
                              previousFingerprint == targetFingerprint;
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
    const bool readsCurrentTransport =
        !ctx.audioTimelineDescriptor.m_fingerprint.empty() &&
        loadedFingerprint == ctx.audioTimelineDescriptor.m_fingerprint;
    if ( !readsCurrentTransport ) {
        ctx.isPlaying                   = false;
        ctx.isAudioTimelineSyncFollower = false;
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
        return false;
    }
    return ctx.isPlaying || ctx.isAudioTimelineSyncFollower;
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
    int        beatDivisor = config.settings.beatDivisor;
    if ( beatDivisor <= 0 ) beatDivisor = 4;

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
        double beatDuration = 60.0 / bVal;
        double stepDuration = beatDuration / beatDivisor;

        double relativeTime = rawTime - bpmTime;
        double stepCount;
        if ( config.settings.snapFloor ) {
            stepCount = std::floor(relativeTime / stepDuration + 1e-6);
        } else {
            stepCount = std::round(relativeTime / stepDuration);
        }
        double nearestStepTime = bpmTime + stepCount * stepDuration;

        if ( nearestStepTime > nextBpmTime ) nearestStepTime = nextBpmTime;

        double snapAbsY = cache->getAbsY(nearestStepTime);
        float snapY = judgmentLineY -
                      static_cast<float>(snapAbsY - currentAbsY) * renderScaleY;

        if ( config.settings.scrollSnap ||
             std::abs(snapY - mouseY) <= config.visual.snapThreshold ) {
            result.isSnapped   = true;
            result.snappedTime = nearestStepTime;

            // 计算当前分拍位置。
            int64_t stepInt = static_cast<int64_t>(
                std::round((nearestStepTime - bpmTime) / stepDuration));
            int beatIndex = stepInt % beatDivisor;
            if ( beatIndex < 0 ) beatIndex += beatDivisor;

            if ( beatIndex == 0 ) {
                result.numerator   = 1;
                result.denominator = 1;
            } else {
                int gcd            = std::gcd(beatIndex, beatDivisor);
                result.numerator   = beatIndex / gcd;
                result.denominator = beatDivisor / gcd;
            }

            break;
        }
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
        if ( noteEndTime > maxEndTime ) {
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
                                          std::move(sampleBinding) });

                double snEndTime = sn.timestamp + sn.duration;
                if ( snEndTime > maxEndTime ) {
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
                                      note.m_sampleBinding });
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
