#include "canvas/AnnotationTableData.h"

#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/BpmNormalization.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace MMM::Canvas
{
AnnotationTableDataRefreshResult AnnotationTableData::refresh()
{
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    const auto*                           activeEntry =
        engine.getSessionEntry(engine.getActiveSessionIndex());
    if ( !activeEntry || activeEntry->isLogoPlaceholder ||
         !activeEntry->session ) {
        return { AnnotationTableDataStatus::Close, false };
    }

    const auto& context = activeEntry->session->getContext();
    if ( !context.currentBeatmap ) {
        return { AnnotationTableDataStatus::Close, false };
    }

    const auto beatmapInstanceId =
        reinterpret_cast<std::uintptr_t>(context.currentBeatmap.get());
    if ( beatmapInstanceId != m_beatmapInstanceId ) {
        reset();
        m_beatmapInstanceId = beatmapInstanceId;
    }

    if ( context.isAnnotationRenderCacheDirty ) {
        return { AnnotationTableDataStatus::Pending, false };
    }

    bool rowsChanged = false;
    if ( context.annotationRenderCacheRevision != m_annotationRevision ) {
        std::vector<AnnotationTableRow> refreshedRows;
        refreshedRows.reserve(context.currentBeatmap->m_annotations.size());
        for ( const auto& marker : context.annotationRenderCache ) {
            for ( const auto& item : marker.items ) {
                refreshedRows.push_back({
                    .timestamp     = marker.timestamp,
                    .id            = item.id,
                    .targetKind    = item.targetKind,
                    .track         = item.track,
                    .targetMissing = item.targetMissing,
                    .author        = item.author,
                    .content       = item.content,
                });
            }
        }
        m_rows.swap(refreshedRows);
        m_annotationRevision = context.annotationRenderCacheRevision;
        rowsChanged          = true;
    }

    const int beatDivisor =
        std::max(1, context.lastConfig.settings.beatDivisor);
    const auto* scrollCache =
        context.timelineRegistry.ctx().find<Logic::System::ScrollCache>();
    if ( scrollCache && !scrollCache->isDirty &&
         (scrollCache->getRevision() != m_timingRevision ||
          beatDivisor != m_beatDivisor) ) {
        UI::Utils::CanvasTimeFormatContext refreshedContext;
        refreshedContext.beatDivisor = beatDivisor;
        const auto& segments         = scrollCache->getSegments();
        refreshedContext.bpmPoints.reserve(segments.size());
        for ( const auto& segment : segments ) {
            if ( segment.bpmEntity == entt::null ) continue;
            refreshedContext.bpmPoints.push_back(
                { segment.time, ::MMM::normalizeBpmValue(segment.bpmValue) });
        }
        std::sort(refreshedContext.bpmPoints.begin(),
                  refreshedContext.bpmPoints.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.time < rhs.time;
                  });
        refreshedContext.bpmPoints.erase(
            std::unique(refreshedContext.bpmPoints.begin(),
                        refreshedContext.bpmPoints.end(),
                        [](const auto& lhs, const auto& rhs) {
                            return std::abs(lhs.time - rhs.time) < 1e-6;
                        }),
            refreshedContext.bpmPoints.end());
        m_timeFormatContext = std::move(refreshedContext);
        m_timingRevision    = scrollCache->getRevision();
        m_beatDivisor       = beatDivisor;
    }

    return { AnnotationTableDataStatus::Ready, rowsChanged };
}

void AnnotationTableData::reset()
{
    m_beatmapInstanceId  = 0U;
    m_annotationRevision = std::numeric_limits<std::uint64_t>::max();
    m_timingRevision     = std::numeric_limits<std::uint64_t>::max();
    m_beatDivisor        = 4;
    m_rows.clear();
    m_timeFormatContext.bpmPoints.clear();
    m_timeFormatContext.beatDivisor = 4;
}

}  // namespace MMM::Canvas
