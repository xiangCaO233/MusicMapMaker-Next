#include "logic/session/SessionUtils.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include <numeric>

namespace MMM::Logic::SessionUtils
{

SnapResult getSnapResult(
    double rawTime, float mouseY, const CameraInfo& camera,
    const Config::EditorConfig&                  config,
    const std::vector<const TimelineComponent*>& bpmEvents,
    entt::registry& timelineRegistry, double visualTime,
    const std::unordered_map<std::string, CameraInfo>& cameras)
{
    SnapResult result;
    int        beatDivisor = config.settings.beatDivisor;
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    auto* cache = timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return result;

    if ( bpmEvents.empty() ) return result;

    // 只有在首个 BPM 事件之后才进行磁吸计算
    if ( rawTime < bpmEvents[0]->m_timestamp ) return result;

    float  judgmentLineY = camera.viewportHeight * config.visual.judgeline_pos;
    double currentAbsY   = cache->getAbsY(visualTime);

    float renderScaleY = 1.0f;
    if ( camera.id == "Preview" || camera.id == "PreviewCanvas" ) {
        auto  itMain             = cameras.find("Basic2DCanvas");
        float mainViewportHeight = itMain != cameras.end()
                                       ? itMain->second.viewportHeight
                                       : camera.viewportHeight;

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

        double      bVal = bpmVal;
        if ( bVal <= 0.0 ) {
            bVal = 120.0;
            // 获取全局活动 Session 里的 Beatmap 预设 BPM
            if ( auto session = EditorEngine::instance().getActiveSession() ) {
                if ( auto beatmap = session->getContext().currentBeatmap ) {
                    if ( beatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
                        bVal = beatmap->m_baseMapMetadata.preference_bpm;
                    }
                }
            }
        }
        double beatDuration = 60.0 / bVal;
        double stepDuration = beatDuration / beatDivisor;

        double relativeTime    = rawTime - bpmTime;
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

        if ( config.settings.scrollSnap || std::abs(snapY - mouseY) <= config.visual.snapThreshold ) {
            result.isSnapped   = true;
            result.snappedTime = nearestStepTime;

            // Calculate fraction
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
    auto it = std::lower_bound(
        ctx.hitEvents.begin(),
        ctx.hitEvents.end(),
        System::HitFXSystem::HitEvent{ ctx.visualTime, ::MMM::NoteType::NOTE });
    ctx.nextHitIndex        = std::distance(ctx.hitEvents.begin(), it);
    ctx.nextPredictHitIndex = ctx.nextHitIndex;
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
    syncHitIndex(ctx);

    if ( ctx.currentBeatmap ) {
        if ( maxEndTime > ctx.currentBeatmap->m_baseMapMetadata.map_length ) {
            ctx.currentBeatmap->m_baseMapMetadata.map_length = maxEndTime;
        }
    }
}

}  // namespace MMM::Logic::SessionUtils
