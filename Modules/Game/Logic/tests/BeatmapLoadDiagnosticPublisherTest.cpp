#include "logic/BeatmapLoadDiagnosticPublisher.h"

#include "event/core/EventBus.h"
#include "mmm/beatmap/BeatMap.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{

/// @brief 验证一次载入内的重复 Loader 诊断只产生一条 UI 事件。
/// @return 事件字段和去重行为均正确时返回 true。
bool testDiagnosticConversionAndDeduplication()
{
    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.map_path = "/project/chart/legacy_chart.mmm";

    const MMM::BeatmapLoadDiagnostic diagnostic{
        .m_code = MMM::BeatmapLoadDiagnosticCode::
            LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE,
        .m_severity    = MMM::BeatmapLoadDiagnosticSeverity::WARNING,
        .m_message     = "Loader detail",
        .m_relatedPath = "/project/chart/legacy_chart.mc",
    };
    beatmap.m_loadDiagnostics.push_back(diagnostic);
    beatmap.m_loadDiagnostics.push_back(diagnostic);

    const auto events = MMM::Logic::buildBeatmapLoadDiagnosticEvents(beatmap);
    return events.size() == 1 &&
           events.front().m_kind == MMM::Event::BeatmapLoadDiagnosticKind::
                                        LegacyMmmOriginalMalodyAvailable &&
           events.front().m_severity ==
               MMM::Event::BeatmapLoadDiagnosticSeverity::Warning &&
           events.front().m_beatmapPath == "/project/chart/legacy_chart.mmm" &&
           events.front().m_relatedPath == "/project/chart/legacy_chart.mc" &&
           events.front().m_message == "Loader detail";
}

/// @brief 验证非法自动采样轨道迁移诊断具有独立且确定的事件类型。
/// @return 诊断类型、级别和详情均完整转换时返回 true。
bool testAudioSampleTrackRelocationDiagnostic()
{
    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.map_path = "/project/chart/relocated.mmm";
    beatmap.m_loadDiagnostics.push_back(MMM::BeatmapLoadDiagnostic{
        .m_code = MMM::BeatmapLoadDiagnosticCode::AUDIO_SAMPLE_TRACK_RELOCATED,
        .m_severity = MMM::BeatmapLoadDiagnosticSeverity::WARNING,
        .m_message  = "Moved x=2 to x=4",
    });

    const auto events = MMM::Logic::buildBeatmapLoadDiagnosticEvents(beatmap);
    return events.size() == 1 &&
           events.front().m_kind == MMM::Event::BeatmapLoadDiagnosticKind::
                                        AudioSampleTrackRelocated &&
           events.front().m_severity ==
               MMM::Event::BeatmapLoadDiagnosticSeverity::Warning &&
           events.front().m_beatmapPath == "/project/chart/relocated.mmm" &&
           events.front().m_relatedPath.empty() &&
           events.front().m_message == "Moved x=2 to x=4";
}

/// @brief 验证无 Loader 诊断时不会生成或发布事件。
/// @return 无事件生成且 EventBus 订阅者未被调用时返回 true。
bool testNoDiagnosticHasNoBehavior()
{
    MMM::BeatMap beatmap;
    std::size_t  receivedCount = 0;
    const auto   subscriptionId =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::BeatmapLoadDiagnosticEvent>(
                [&](const MMM::Event::BeatmapLoadDiagnosticEvent&) {
                    ++receivedCount;
                });

    MMM::Logic::publishBeatmapLoadDiagnostics(beatmap);
    MMM::Event::EventBus::instance()
        .unsubscribe<MMM::Event::BeatmapLoadDiagnosticEvent>(subscriptionId);
    return receivedCount == 0;
}

/// @brief 验证发布入口按一次载入的去重结果投递事件。
/// @return 相同诊断只发布一次时返回 true。
bool testPublisherUsesDeduplicatedEvents()
{
    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.map_path = "/project/chart/legacy.mmm";
    const MMM::BeatmapLoadDiagnostic diagnostic{
        .m_code = MMM::BeatmapLoadDiagnosticCode::
            LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE,
        .m_severity    = MMM::BeatmapLoadDiagnosticSeverity::WARNING,
        .m_message     = "Loader detail",
        .m_relatedPath = "/project/chart/legacy.mc",
    };
    beatmap.m_loadDiagnostics = { diagnostic, diagnostic };

    std::size_t receivedCount = 0;
    std::string receivedPath;
    const auto  subscriptionId =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::BeatmapLoadDiagnosticEvent>(
                [&](const MMM::Event::BeatmapLoadDiagnosticEvent& event) {
                    ++receivedCount;
                    receivedPath = event.m_relatedPath;
                });

    MMM::Logic::publishBeatmapLoadDiagnostics(beatmap);
    MMM::Event::EventBus::instance()
        .unsubscribe<MMM::Event::BeatmapLoadDiagnosticEvent>(subscriptionId);
    return receivedCount == 1 && receivedPath == "/project/chart/legacy.mc";
}

}  // namespace

/// @brief 覆盖旧 MMM 加载诊断的事件转换、去重和空输入行为。
/// @return 所有断言通过时返回 0。
int main()
{
    return testDiagnosticConversionAndDeduplication() &&
                   testAudioSampleTrackRelocationDiagnostic() &&
                   testNoDiagnosticHasNoBehavior() &&
                   testPublisherUsesDeduplicatedEvents()
               ? 0
               : 1;
}
