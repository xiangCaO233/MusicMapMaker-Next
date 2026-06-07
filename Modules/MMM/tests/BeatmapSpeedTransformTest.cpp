#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "log/colorful-log.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace
{

/// @brief 浮点近似比较。
/// @param actual 实际值。
/// @param expected 期望值。
/// @return 足够接近时返回 true。
bool near(double actual, double expected)
{
    return std::abs(actual - expected) < 1e-6;
}

/// @brief 输出测试断言。
/// @param condition 断言条件。
/// @param label 断言名称。
/// @return 条件是否成立。
bool check(bool condition, const std::string& label)
{
    if ( condition ) {
        XINFO("[speed-transform] PASS: {}", label);
    } else {
        XERROR("[speed-transform] FAIL: {}", label);
    }
    return condition;
}

/// @brief 构造包含普通物件、长条、折线和多类 Timing 的测试谱面。
/// @return 测试谱面。
MMM::BeatMap makeFixture()
{
    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.name            = "Source";
    beatmap.m_baseMapMetadata.version         = "Hard";
    beatmap.m_baseMapMetadata.preference_bpm  = 120.0;
    beatmap.m_baseMapMetadata.map_length      = 90000.0;
    beatmap.m_baseMapMetadata.video_starttime = 1000;

    MMM::Timing bpm;
    bpm.m_timestamp             = 1000.0;
    bpm.m_timingEffect          = MMM::TimingEffect::BPM;
    bpm.m_bpm                   = 120.0;
    bpm.m_beat_length           = 500.0;
    bpm.m_timingEffectParameter = 120.0;
    beatmap.m_timings.push_back(bpm);

    MMM::Timing scroll;
    scroll.m_timestamp             = 3000.0;
    scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
    scroll.m_beat_length           = 1.5;
    scroll.m_timingEffectParameter = 1.5;
    beatmap.m_timings.push_back(scroll);

    MMM::Note note;
    note.m_timestamp = 2000.0;
    note.m_track     = 1;
    beatmap.m_noteData.notes.push_back(note);

    MMM::Hold hold;
    hold.m_timestamp = 4000.0;
    hold.m_duration  = 600.0;
    hold.m_track     = 2;
    beatmap.m_noteData.holds.push_back(hold);

    MMM::Polyline polyline;
    polyline.m_timestamp = 5000.0;
    polyline.m_track     = 3;

    MMM::Hold subHold;
    subHold.m_timestamp = 5000.0;
    subHold.m_duration  = 1000.0;
    subHold.m_track     = 3;
    subHold.m_isSubNote = true;
    beatmap.m_noteData.holds.push_back(subHold);
    auto& subHoldRef = beatmap.m_noteData.holds.back();
    polyline.m_subNotes.push_back(subHoldRef);
    polyline.m_subHolds.push_back(subHoldRef);

    MMM::Flick subFlick;
    subFlick.m_timestamp = 6000.0;
    subFlick.m_track     = 4;
    subFlick.m_isSubNote = true;
    beatmap.m_noteData.flicks.push_back(subFlick);
    auto& subFlickRef = beatmap.m_noteData.flicks.back();
    polyline.m_subNotes.push_back(subFlickRef);
    polyline.m_subFlicks.push_back(subFlickRef);

    beatmap.m_noteData.polylines.push_back(polyline);
    beatmap.sync();
    return beatmap;
}

}  // namespace

int main()
{
    XLogger::init("BeatmapSpeedTransformTest");

    auto                              source = makeFixture();
    MMM::BeatmapSpeedTransformOptions options;
    options.speed     = 1.5;
    options.mapPath   = "Source_1_5x.mmm";
    options.audioPath = "audio_1_5x.wav";
    options.name      = "Source 1.5x";
    options.version   = "Hard 1.5x";

    auto result =
        MMM::BeatmapSpeedTransform::createSpeedVersion(source, options);

    bool ok = true;
    ok &= check(result.success, "transform succeeds");
    ok &= check(result.beatmap.m_baseMapMetadata.name == "Source 1.5x",
                "name updated");
    ok &= check(result.beatmap.m_baseMapMetadata.version == "Hard 1.5x",
                "version updated");
    ok &= check(near(result.beatmap.m_baseMapMetadata.preference_bpm, 180.0),
                "metadata bpm scaled");
    ok &= check(near(result.beatmap.m_baseMapMetadata.map_length, 4000.0),
                "map length uses content end");
    ok &= check(near(MMM::BeatmapSpeedTransform::calculateContentEndTime(
                         result.beatmap),
                     4000.0),
                "content end time calculated");
    ok &= check(result.beatmap.m_baseMapMetadata.video_starttime == 667,
                "video start time scaled");
    ok &= check(result.beatmap.m_baseMapMetadata.main_audio_path ==
                    std::filesystem::path("audio_1_5x.wav"),
                "audio path updated");

    ok &= check(result.beatmap.m_timings.size() == 2, "timing count kept");
    ok &= check(near(result.beatmap.m_timings[0].m_timestamp, 666.6666666667),
                "bpm timing timestamp scaled");
    ok &= check(near(result.beatmap.m_timings[0].m_bpm, 180.0),
                "bpm timing value scaled");
    ok &= check(near(result.beatmap.m_timings[0].m_beat_length, 333.3333333333),
                "bpm beat length scaled");
    ok &= check(near(result.beatmap.m_timings[1].m_timestamp, 2000.0),
                "scroll timing timestamp scaled");
    ok &= check(near(result.beatmap.m_timings[1].m_timingEffectParameter, 1.5),
                "scroll multiplier kept");

    ok &= check(result.beatmap.m_noteData.notes.size() == 1,
                "standalone note count kept");
    ok &= check(near(result.beatmap.m_noteData.notes.front().m_timestamp,
                     1333.3333333333),
                "note timestamp scaled");
    ok &= check(result.beatmap.m_noteData.holds.size() == 2,
                "hold and sub hold copied");
    ok &= check(near(result.beatmap.m_noteData.holds.front().m_timestamp,
                     2666.6666666667),
                "hold timestamp scaled");
    ok &= check(near(result.beatmap.m_noteData.holds.front().m_duration, 400.0),
                "hold duration scaled");
    ok &= check(result.beatmap.m_noteData.polylines.size() == 1,
                "polyline copied");

    const auto& polyline = result.beatmap.m_noteData.polylines.front();
    ok &= check(polyline.m_subNotes.size() == 2, "polyline sub notes rebound");
    ok &= check(near(polyline.m_timestamp, 3333.3333333333),
                "polyline timestamp follows first sub note");
    ok &= check(
        near(polyline.m_subNotes.front().get().m_timestamp, 3333.3333333333),
        "polyline first sub timestamp scaled");
    ok &= check(near(polyline.m_subNotes.back().get().m_timestamp, 4000.0),
                "polyline second sub timestamp scaled");

    auto shortSource                         = makeFixture();
    shortSource.m_baseMapMetadata.map_length = 3000.0;
    auto shortResult =
        MMM::BeatmapSpeedTransform::createSpeedVersion(shortSource, options);
    ok &= check(shortResult.success, "short metadata transform succeeds");
    ok &= check(near(shortResult.beatmap.m_baseMapMetadata.map_length, 4000.0),
                "map length ignores divided metadata tail");

    if ( !ok ) {
        XLogger::shutdown();
        return EXIT_FAILURE;
    }

    XINFO("BeatmapSpeedTransformTest passed.");
    XLogger::shutdown();
    return EXIT_SUCCESS;
}
