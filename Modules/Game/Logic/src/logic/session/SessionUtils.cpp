#include "logic/session/SessionUtils.h"
#include "audio/AudioManager.h"
#include "config/Utf8Path.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/project/Project.h"
#include <algorithm>
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
    double totalTime = 0.0;
    if ( ctx.mainAudioTotalTime > 0.0 &&
         std::isfinite(ctx.mainAudioTotalTime) ) {
        totalTime = ctx.mainAudioTotalTime;
    }

    const auto& loadedBgmPath =
        Audio::AudioManager::instance().getLoadedBGMPath();
    if ( !ctx.loadedMainAudioPath.empty() &&
         loadedBgmPath == ctx.loadedMainAudioPath ) {
        totalTime =
            std::max(totalTime, Audio::AudioManager::instance().getTotalTime());
    }

    if ( ctx.currentBeatmap ) {
        const double mapLengthMs =
            ctx.currentBeatmap->m_baseMapMetadata.map_length;
        if ( mapLengthMs > 0.0 && std::isfinite(mapLengthMs) ) {
            totalTime = std::max(totalTime, mapLengthMs / 1000.0);
        }
    }
    return std::max(0.0, totalTime);
}

namespace
{
/// @brief 判断路径是否指向可访问的普通文件。
/// @param path 待检查路径。
/// @return 路径存在且为普通文件时返回 true。
bool isRegularFilePath(const std::filesystem::path& path)
{
    if ( path.empty() ) return false;

    std::error_code filesystemError;
    const bool      isRegular =
        std::filesystem::is_regular_file(path, filesystemError);
    return isRegular && !filesystemError;
}

/// @brief 将候选路径加入解析列表。
/// @param candidates 候选路径列表。
/// @param path 待加入路径。
void appendAudioPathCandidate(std::vector<std::filesystem::path>& candidates,
                              const std::filesystem::path&        path)
{
    if ( path.empty() ) return;
    candidates.push_back(path.lexically_normal());
}

/// @brief 若路径形如 项目目录名/资源文件，则加入去掉项目前缀后的候选。
/// @param candidates 候选路径列表。
/// @param projectRoot 项目根目录。
/// @param audioPath 元数据中的音频路径。
void appendProjectFolderStrippedCandidate(
    std::vector<std::filesystem::path>& candidates,
    const std::filesystem::path&        projectRoot,
    const std::filesystem::path&        audioPath)
{
    if ( projectRoot.empty() || audioPath.empty() || audioPath.is_absolute() ) {
        return;
    }

    auto iterator = audioPath.begin();
    if ( iterator == audioPath.end() || *iterator != projectRoot.filename() ) {
        return;
    }

    std::filesystem::path stripped;
    ++iterator;
    for ( ; iterator != audioPath.end(); ++iterator ) {
        stripped /= *iterator;
    }
    appendAudioPathCandidate(candidates, projectRoot / stripped);
}
}  // namespace

std::filesystem::path resolveMainAudioPath(const SessionContext& ctx,
                                           const ::MMM::Project* project)
{
    if ( !ctx.currentBeatmap ) return {};

    const auto& meta      = ctx.currentBeatmap->m_baseMapMetadata;
    const auto& audioPath = meta.main_audio_path;
    if ( audioPath.empty() ) return {};

    std::vector<std::filesystem::path> candidates;
    candidates.reserve(8);

    if ( audioPath.is_absolute() ) {
        appendAudioPathCandidate(candidates, audioPath);
    }

    std::filesystem::path projectRoot;
    if ( project && !project->m_projectRoot.empty() ) {
        projectRoot = project->m_projectRoot.lexically_normal();
        appendAudioPathCandidate(candidates, projectRoot / audioPath);
        appendProjectFolderStrippedCandidate(
            candidates, projectRoot, audioPath);

        const std::string audioPathUtf8 = Config::pathToUtf8(audioPath);
        const std::string genericAudioPathUtf8 =
            Config::pathToUtf8Generic(audioPath);
        const std::string audioFileNameUtf8 =
            Config::pathToUtf8(audioPath.filename());
        for ( const auto& resource : project->m_audioResources ) {
            if ( resource.m_path != audioPathUtf8 &&
                 resource.m_path != genericAudioPathUtf8 &&
                 resource.m_id != audioFileNameUtf8 ) {
                continue;
            }
            const auto resourcePath = Config::utf8ToPath(resource.m_path);
            appendAudioPathCandidate(candidates, projectRoot / resourcePath);
            appendProjectFolderStrippedCandidate(
                candidates, projectRoot, resourcePath);
        }
    }

    const std::filesystem::path mapDirectory =
        meta.map_path.parent_path().lexically_normal();
    if ( !mapDirectory.empty() ) {
        if ( projectRoot.empty() ) {
            appendAudioPathCandidate(candidates, mapDirectory / audioPath);
        } else {
            appendAudioPathCandidate(candidates,
                                     projectRoot / mapDirectory / audioPath);
        }
    }

    for ( const auto& candidate : candidates ) {
        if ( isRegularFilePath(candidate) ) {
            return candidate.lexically_normal();
        }
    }

    if ( !candidates.empty() ) {
        return candidates.front().lexically_normal();
    }
    return audioPath.lexically_normal();
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
        float  snapY    = judgmentLineY -
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
    auto it                 = std::lower_bound(ctx.hitEvents.begin(),
                               ctx.hitEvents.end(),
                               System::HitFXSystem::HitEvent{
                                   ctx.animateTime, ::MMM::NoteType::NOTE });
    ctx.nextHitIndex        = std::distance(ctx.hitEvents.begin(), it);
    ctx.nextPredictHitIndex = ctx.nextHitIndex;
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
    ctx.nextHitIndex = 0;

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

                ctx.hitEvents.push_back({ sn.timestamp,
                                          sn.type,
                                          role,
                                          span,
                                          sn.trackIndex,
                                          sn.dtrack,
                                          sn.duration,
                                          true });

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
                                      false });
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
