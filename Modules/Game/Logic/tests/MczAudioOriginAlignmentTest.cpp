#include "logic/MczAudioOriginAlignment.h"

#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <cmath>
#include <string>
#include <unordered_set>

namespace
{

/// @brief 测试时间比较容差。
constexpr double TIME_EPSILON_MS = 1.0e-6;

/// @brief 比较两个毫秒时间是否一致。
bool nearTime(double lhs, double rhs)
{
    return std::abs(lhs - rhs) <= TIME_EPSILON_MS;
}

/// @brief 创建含主采样、普通采样、后续红线和批注的测试谱面。
MMM::BeatMap makeBeatMap(double firstBpmTime)
{
    MMM::BeatMap beatMap;
    MMM::Timing  firstBpm;
    firstBpm.m_timestamp             = firstBpmTime;
    firstBpm.m_bpm                   = 120.0;
    firstBpm.m_beat_length           = 500.0;
    firstBpm.m_timingEffect          = MMM::TimingEffect::BPM;
    firstBpm.m_timingEffectParameter = 120.0;
    firstBpm.m_metadata
        .timing_properties[MMM::TimingMetadataType::MALODY]["delay"] = "123.0";
    beatMap.m_timings.push_back(firstBpm);

    MMM::Timing secondBpm             = firstBpm;
    secondBpm.m_timestamp             = 2100.0;
    secondBpm.m_bpm                   = 150.0;
    secondBpm.m_beat_length           = 400.0;
    secondBpm.m_timingEffectParameter = 150.0;
    beatMap.m_timings.push_back(secondBpm);

    auto& note           = beatMap.m_noteData.notes.emplace_back();
    note.m_timestamp     = 2100.0;
    note.m_track         = 1U;
    auto& hold           = beatMap.m_noteData.holds.emplace_back();
    hold.m_timestamp     = 1800.0;
    hold.m_duration      = 300.0;
    hold.m_track         = 2U;
    auto& flick          = beatMap.m_noteData.flicks.emplace_back();
    flick.m_timestamp    = 2300.0;
    flick.m_track        = 3U;
    auto& polyline       = beatMap.m_noteData.polylines.emplace_back();
    polyline.m_timestamp = 2500.0;
    polyline.m_track     = 4U;

    beatMap.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_timestamp       = 0.0,
        .m_offsetMs        = 0,
        .m_track           = 4U,
        .m_audioResourceId = "main-audio",
    });
    beatMap.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_timestamp       = 300.0,
        .m_offsetMs        = -20,
        .m_track           = 5U,
        .m_audioResourceId = "effect-audio",
    });
    beatMap.m_annotations.push_back(MMM::BeatmapAnnotation{
        .m_timestamp = 700.0,
        .m_content   = "alignment",
    });
    beatMap.sync();
    return beatMap;
}

/// @brief 验证正时间多拍前导只裁切一拍内相位。
bool checkPositiveMultiBeatAlignment()
{
    auto       beatMap = makeBeatMap(1600.0);
    const auto timing =
        MMM::Logic::calculateMczAudioOriginAlignmentTiming(beatMap);
    std::string error;
    const bool  applied =
        timing.success && nearTime(timing.phaseMilliseconds, 100.0) &&
        MMM::Logic::applyMczAudioOriginAlignment(
            beatMap, { "main-audio" }, timing.phaseMilliseconds, error);
    const bool valid =
        applied && error.empty() &&
        nearTime(beatMap.m_timings[0].m_timestamp, 0.0) &&
        !beatMap.m_timings[0]
             .m_metadata.timing_properties[MMM::TimingMetadataType::MALODY]
             .contains("delay") &&
        nearTime(beatMap.m_timings[1].m_timestamp, 2000.0) &&
        nearTime(beatMap.m_noteData.notes[0].m_timestamp, 2000.0) &&
        nearTime(beatMap.m_noteData.holds[0].m_timestamp, 1700.0) &&
        nearTime(beatMap.m_noteData.holds[0].m_duration, 300.0) &&
        nearTime(beatMap.m_noteData.flicks[0].m_timestamp, 2200.0) &&
        nearTime(beatMap.m_noteData.polylines[0].m_timestamp, 2400.0) &&
        nearTime(beatMap.m_audioSamples[0].effectiveTimestamp(), 0.0) &&
        nearTime(beatMap.m_audioSamples[1].m_timestamp, 200.0) &&
        beatMap.m_audioSamples[1].m_offsetMs == -20 &&
        nearTime(beatMap.m_annotations[0].m_timestamp, 600.0);
    if ( !valid )
        XERROR("Positive multi-beat MCZ origin alignment failed: {}", error);
    return valid;
}

/// @brief 验证负时间多拍前导折回后通过补静音相位统一平移。
bool checkNegativeMultiBeatAlignment()
{
    auto       beatMap = makeBeatMap(-600.0);
    const auto timing =
        MMM::Logic::calculateMczAudioOriginAlignmentTiming(beatMap);
    std::string error;
    const bool  applied =
        timing.success && nearTime(timing.phaseMilliseconds, -100.0) &&
        MMM::Logic::applyMczAudioOriginAlignment(
            beatMap, { "main-audio" }, timing.phaseMilliseconds, error);
    const bool valid =
        applied && nearTime(beatMap.m_timings[0].m_timestamp, 0.0) &&
        nearTime(beatMap.m_timings[1].m_timestamp, 2200.0) &&
        nearTime(beatMap.m_noteData.notes[0].m_timestamp, 2200.0) &&
        nearTime(beatMap.m_audioSamples[0].effectiveTimestamp(), 0.0) &&
        nearTime(beatMap.m_audioSamples[1].m_timestamp, 400.0) &&
        nearTime(beatMap.m_annotations[0].m_timestamp, 800.0);
    if ( !valid )
        XERROR("Negative multi-beat MCZ origin alignment failed: {}", error);
    return valid;
}

/// @brief 验证整数拍首红线归零时不重编码相位或移动普通内容。
bool checkWholeBeatAlignment()
{
    auto       beatMap = makeBeatMap(1500.0);
    const auto timing =
        MMM::Logic::calculateMczAudioOriginAlignmentTiming(beatMap);
    std::string error;
    const bool  applied =
        timing.success && nearTime(timing.phaseMilliseconds, 0.0) &&
        MMM::Logic::applyMczAudioOriginAlignment(
            beatMap, { "main-audio" }, timing.phaseMilliseconds, error);
    return applied && nearTime(beatMap.m_timings[0].m_timestamp, 0.0) &&
           nearTime(beatMap.m_timings[1].m_timestamp, 2100.0) &&
           nearTime(beatMap.m_noteData.notes[0].m_timestamp, 2100.0) &&
           nearTime(beatMap.m_audioSamples[1].m_timestamp, 300.0);
}

/// @brief 验证缺少 BPM 或 Main 自动采样时安全失败。
bool checkInvalidInputsFail()
{
    MMM::BeatMap emptyBeatMap;
    const auto   missingTiming =
        MMM::Logic::calculateMczAudioOriginAlignmentTiming(emptyBeatMap);
    auto        beatMap = makeBeatMap(100.0);
    std::string error;
    if ( missingTiming.success ||
         MMM::Logic::applyMczAudioOriginAlignment(
             beatMap,
             std::unordered_set<std::string>{ "missing-main" },
             100.0,
             error) ||
         error.empty() ) {
        return false;
    }

    auto nonOriginMain                          = makeBeatMap(100.0);
    nonOriginMain.m_audioSamples[0].m_timestamp = 10.0;
    error.clear();
    if ( MMM::Logic::applyMczAudioOriginAlignment(
             nonOriginMain,
             std::unordered_set<std::string>{ "main-audio" },
             100.0,
             error) ||
         error.empty() ) {
        return false;
    }

    auto duplicateMain = makeBeatMap(100.0);
    duplicateMain.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_timestamp       = 500.0,
        .m_offsetMs        = 0,
        .m_track           = 4U,
        .m_audioResourceId = "main-audio",
    });
    error.clear();
    return !MMM::Logic::applyMczAudioOriginAlignment(
               duplicateMain,
               std::unordered_set<std::string>{ "main-audio" },
               100.0,
               error) &&
           !error.empty();
}

}  // namespace

int main()
{
    XLogger::init("MczAudioOriginAlignmentTest");
    return checkPositiveMultiBeatAlignment() &&
                   checkNegativeMultiBeatAlignment() &&
                   checkWholeBeatAlignment() && checkInvalidInputsFail()
               ? 0
               : 1;
}
