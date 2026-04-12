#include "logic/BeatmapSession.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include <numeric>

namespace MMM::Logic
{

void BeatmapSession::syncHitIndex()
{
    auto it = std::lower_bound(
        m_hitEvents.begin(),
        m_hitEvents.end(),
        System::HitFXSystem::HitEvent{ m_visualTime, ::MMM::NoteType::NOTE });
    m_nextHitIndex = std::distance(m_hitEvents.begin(), it);

    // 预测索引也同步到同样的位置（预测逻辑会自动往后扫）
    m_nextPredictHitIndex = m_nextHitIndex;
}

BeatmapSession::SnapResult BeatmapSession::getSnapResult(
    double rawTime, float mouseY, const CameraInfo& camera,
    const Config::EditorConfig&                  config,
    const std::vector<const TimelineComponent*>& bpmEvents) const
{
    SnapResult result;
    int        beatDivisor = config.settings.beatDivisor;
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    auto* cache = m_timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return result;

    if ( bpmEvents.empty() ) return result;

    // 只有在首个 BPM 事件之后才进行磁吸计算
    if ( rawTime < bpmEvents[0]->m_timestamp ) return result;

    float  judgmentLineY = camera.viewportHeight * config.visual.judgeline_pos;
    double currentAbsY   = cache->getAbsY(m_visualTime);

    float renderScaleY = config.visual.noteScaleY;
    if ( camera.id == "Preview" || camera.id == "PreviewCanvas" ) {
        auto  itMain             = m_cameras.find("Basic2DCanvas");
        float mainViewportHeight = itMain != m_cameras.end()
                                       ? itMain->second.viewportHeight
                                       : camera.viewportHeight;

        float mainEffectiveH = (config.visual.trackLayout.bottom -
                                config.visual.trackLayout.top) *
                               mainViewportHeight;
        float ty = config.visual.previewConfig.margin.top;
        float by = camera.viewportHeight -
                   config.visual.previewConfig.margin.bottom;
        float previewDrawH = by - ty;

        renderScaleY = previewDrawH / (mainEffectiveH *
                                       config.visual.previewConfig.areaRatio);
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

        double beatDuration = 60.0 / (bpmVal > 0 ? bpmVal : 120.0);
        double stepDuration = beatDuration / beatDivisor;

        double relativeTime    = rawTime - bpmTime;
        double stepCount       = std::round(relativeTime / stepDuration);
        double nearestStepTime = bpmTime + stepCount * stepDuration;

        if ( nearestStepTime > nextBpmTime ) nearestStepTime = nextBpmTime;

        double snapAbsY = cache->getAbsY(nearestStepTime);
        float  snapY =
            judgmentLineY - static_cast<float>(snapAbsY - currentAbsY) * renderScaleY;

        if ( std::abs(snapY - mouseY) <= config.visual.snapThreshold ) {
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

void BeatmapSession::rebuildHitEvents()
{
    m_hitEvents.clear();
    m_nextHitIndex = 0;

    auto view     = m_noteRegistry.view<NoteComponent>();
    using HitRole = System::HitFXSystem::HitEvent::Role;

    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);

        // 如果是子物件，它的发声逻辑在 Polyline 处理中进行（为了获取 Role）
        if ( note.m_isSubNote ) continue;

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

                m_hitEvents.push_back({ sn.timestamp,
                                        sn.type,
                                        role,
                                        span,
                                        sn.trackIndex,
                                        sn.dtrack,
                                        sn.duration,
                                        true });
            }
        } else {
            int span = 1;
            if ( note.m_type == ::MMM::NoteType::FLICK ) {
                span = std::abs(note.m_dtrack) + 1;
            }

            m_hitEvents.push_back({ note.m_timestamp,
                                    note.m_type,
                                    HitRole::None,
                                    span,
                                    note.m_trackIndex,
                                    note.m_dtrack,
                                    note.m_duration,
                                    false });
        }
    }

    std::sort(m_hitEvents.begin(), m_hitEvents.end());
    syncHitIndex();
}

} // namespace MMM::Logic
