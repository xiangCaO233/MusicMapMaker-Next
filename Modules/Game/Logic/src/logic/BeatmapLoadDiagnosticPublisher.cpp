#include "logic/BeatmapLoadDiagnosticPublisher.h"

#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "mmm/beatmap/BeatMap.h"

#include <string>
#include <unordered_set>
#include <utility>

namespace MMM::Logic
{
namespace
{

/// @brief 将 MMM Loader 诊断级别转换为 Event 层稳定枚举。
/// @param severity Loader 诊断级别。
/// @return 对应的事件提示级别。
Event::BeatmapLoadDiagnosticSeverity convertSeverity(
    MMM::BeatmapLoadDiagnosticSeverity severity)
{
    switch ( severity ) {
    case MMM::BeatmapLoadDiagnosticSeverity::INFO:
        return Event::BeatmapLoadDiagnosticSeverity::Info;
    case MMM::BeatmapLoadDiagnosticSeverity::WARNING:
        return Event::BeatmapLoadDiagnosticSeverity::Warning;
    case MMM::BeatmapLoadDiagnosticSeverity::ERROR:
        return Event::BeatmapLoadDiagnosticSeverity::Error;
    }
    return Event::BeatmapLoadDiagnosticSeverity::Warning;
}

}  // namespace

std::vector<Event::BeatmapLoadDiagnosticEvent> buildBeatmapLoadDiagnosticEvents(
    const MMM::BeatMap& beatmap)
{
    std::vector<Event::BeatmapLoadDiagnosticEvent> events;
    events.reserve(beatmap.m_loadDiagnostics.size());

    std::unordered_set<std::string> emittedKeys;
    emittedKeys.reserve(beatmap.m_loadDiagnostics.size());

    const std::string beatmapPath = Config::pathToUtf8(
        beatmap.m_baseMapMetadata.map_path.lexically_normal());
    for ( const auto& diagnostic : beatmap.m_loadDiagnostics ) {
        Event::BeatmapLoadDiagnosticKind kind;
        switch ( diagnostic.m_code ) {
        case MMM::BeatmapLoadDiagnosticCode::
            LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE:
            kind = Event::BeatmapLoadDiagnosticKind::
                LegacyMmmOriginalMalodyAvailable;
            break;
        case MMM::BeatmapLoadDiagnosticCode::AUDIO_SAMPLE_TRACK_RELOCATED:
            kind = Event::BeatmapLoadDiagnosticKind::AudioSampleTrackRelocated;
            break;
        }

        const std::string relatedPath =
            Config::pathToUtf8(diagnostic.m_relatedPath.lexically_normal());
        const std::string deduplicationKey =
            std::to_string(static_cast<std::uint8_t>(kind)) + '\n' +
            relatedPath;
        if ( !emittedKeys.insert(deduplicationKey).second ) {
            continue;
        }

        events.push_back(Event::BeatmapLoadDiagnosticEvent{
            .m_kind        = kind,
            .m_severity    = convertSeverity(diagnostic.m_severity),
            .m_beatmapPath = beatmapPath,
            .m_relatedPath = relatedPath,
            .m_message     = diagnostic.m_message,
        });
    }
    return events;
}

void publishBeatmapLoadDiagnostics(const MMM::BeatMap& beatmap)
{
    for ( const auto& event : buildBeatmapLoadDiagnosticEvents(beatmap) ) {
        Event::EventBus::instance().publish(event);
    }
}

}  // namespace MMM::Logic
