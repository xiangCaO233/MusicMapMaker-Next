#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

/// @brief 浮点近似比较。
/// @param actual 实际值。
/// @param expected 期望值。
/// @param tolerance 容忍误差。
/// @return 足够接近时返回 true。
bool isNearlyEqual(double actual, double expected, double tolerance = 1e-6)
{
    return std::abs(actual - expected) < tolerance;
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

/// @brief 判断路径是否为支持的谱面文件。
/// @param path 文件路径。
/// @return 支持时返回 true。
bool isSupportedBeatmapFile(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".mc" || extension == ".mmm" || extension == ".osu" ||
           extension == ".imd";
}

/// @brief 递归收集测试资源中的谱面文件。
/// @param root 资源根目录。
/// @return 谱面文件列表。
std::vector<std::filesystem::path> collectBeatmapFiles(
    const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> files;
    std::error_code                    error;
    if ( !std::filesystem::is_directory(root, error) || error ) {
        return files;
    }

    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    std::filesystem::recursive_directory_iterator end;
    while ( !error && it != end ) {
        const auto& path = it->path();
        if ( it->is_regular_file(error) && !error &&
             isSupportedBeatmapFile(path) ) {
            files.push_back(path);
        }
        it.increment(error);
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// @brief 获取谱面中第一个物件的时间戳。
/// @param beatmap 谱面。
/// @return 第一个物件时间戳。
double firstNoteTime(const MMM::BeatMap& beatmap)
{
    if ( beatmap.m_allNotes.empty() ) return 0.0;
    return beatmap.m_allNotes.front().get().m_timestamp;
}

/// @brief 获取谱面中最后一个物件的时间戳。
/// @param beatmap 谱面。
/// @return 最后一个物件时间戳。
double lastNoteTime(const MMM::BeatMap& beatmap)
{
    if ( beatmap.m_allNotes.empty() ) return 0.0;
    return beatmap.m_allNotes.back().get().m_timestamp;
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
    beatmap.m_baseMapMetadata.track_count     = 4;
    beatmap.m_baseMapMetadata.bgm_track_count = 2;
    beatmap.m_baseMapMetadata.main_audio_path = "legacy-source.ogg";
    beatmap.m_baseMapMetadata.song_file_hint  = "source-hint.ogg";

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

    MMM::AudioSampleEvent sample;
    sample.m_timestamp       = 7200.0;
    sample.m_offsetMs        = 101;
    sample.m_track           = 5;
    sample.m_audioResourceId = "stem.ogg";
    sample.m_volume          = 0.75F;
    sample.m_metadata.sample_properties[MMM::SampleMetadataType::MMM]["label"] =
        "stem";
    beatmap.m_audioSamples.push_back(sample);

    beatmap.m_noteData.polylines.push_back(polyline);
    beatmap.sync();
    return beatmap;
}

/// @brief 运行程序化 fixture 覆盖。
/// @return 通过时返回 true。
bool runFixtureCoverage()
{
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
    ok &= check(
        isNearlyEqual(result.beatmap.m_baseMapMetadata.preference_bpm, 180.0),
        "metadata bpm scaled");
    ok &= check(
        isNearlyEqual(result.beatmap.m_baseMapMetadata.map_length, 4867.0),
        "map length uses content end");
    ok &= check(
        isNearlyEqual(
            MMM::BeatmapSpeedTransform::calculateContentEndTime(result.beatmap),
            4867.0),
        "content end time calculated");
    ok &= check(result.beatmap.m_baseMapMetadata.video_starttime == 667,
                "video start time scaled");
    ok &= check(result.beatmap.m_baseMapMetadata.main_audio_path.empty(),
                "legacy main audio path cleared");
    ok &= check(result.beatmap.m_baseMapMetadata.song_file_hint ==
                    std::filesystem::path("audio_1_5x.wav"),
                "song file hint updated");

    ok &= check(result.beatmap.m_timings.size() == 2, "timing count kept");
    ok &= check(
        isNearlyEqual(result.beatmap.m_timings[0].m_timestamp, 666.6666666667),
        "bpm timing timestamp scaled");
    ok &= check(isNearlyEqual(result.beatmap.m_timings[0].m_bpm, 180.0),
                "bpm timing value scaled");
    ok &= check(isNearlyEqual(result.beatmap.m_timings[0].m_beat_length,
                              333.3333333333),
                "bpm beat length scaled");
    ok &= check(isNearlyEqual(result.beatmap.m_timings[1].m_timestamp, 2000.0),
                "scroll timing timestamp scaled");
    ok &= check(
        isNearlyEqual(result.beatmap.m_timings[1].m_timingEffectParameter, 1.5),
        "scroll multiplier kept");

    ok &= check(result.beatmap.m_noteData.notes.size() == 1,
                "standalone note count kept");
    ok &=
        check(isNearlyEqual(result.beatmap.m_noteData.notes.front().m_timestamp,
                            1333.3333333333),
              "note timestamp scaled");
    ok &= check(result.beatmap.m_noteData.holds.size() == 2,
                "hold and sub hold copied");
    ok &=
        check(isNearlyEqual(result.beatmap.m_noteData.holds.front().m_timestamp,
                            2666.6666666667),
              "hold timestamp scaled");
    ok &= check(isNearlyEqual(
                    result.beatmap.m_noteData.holds.front().m_duration, 400.0),
                "hold duration scaled");
    ok &= check(result.beatmap.m_noteData.polylines.size() == 1,
                "polyline copied");

    const auto& polyline = result.beatmap.m_noteData.polylines.front();
    ok &= check(polyline.m_subNotes.size() == 2, "polyline sub notes rebound");
    ok &= check(isNearlyEqual(polyline.m_timestamp, 3333.3333333333),
                "polyline timestamp follows first sub note");
    ok &= check(isNearlyEqual(polyline.m_subNotes.front().get().m_timestamp,
                              3333.3333333333),
                "polyline first sub timestamp scaled");
    ok &= check(
        isNearlyEqual(polyline.m_subNotes.back().get().m_timestamp, 4000.0),
        "polyline second sub timestamp scaled");

    ok &= check(result.beatmap.m_audioSamples.size() == 1,
                "automatic sample count kept");
    if ( result.beatmap.m_audioSamples.size() == 1 ) {
        const auto& sample = result.beatmap.m_audioSamples.front();
        ok &= check(isNearlyEqual(sample.m_timestamp, 4800.0),
                    "automatic sample anchor scaled");
        ok &= check(sample.m_offsetMs == 67,
                    "automatic sample offset rounded to nearest millisecond");
        ok &= check(isNearlyEqual(sample.effectiveTimestamp(), 4867.0),
                    "automatic sample trigger time scaled");
        ok &= check(sample.m_track == 5 &&
                        sample.m_audioResourceId == "stem.ogg" &&
                        isNearlyEqual(sample.m_volume, 0.75F),
                    "automatic sample identity kept");
        ok &= check(sample.m_metadata.getValue<std::string>(
                        MMM::SampleMetadataType::MMM, "label") == "stem",
                    "automatic sample metadata kept");
    }

    auto shortSource                         = makeFixture();
    shortSource.m_baseMapMetadata.map_length = 3000.0;
    auto shortResult =
        MMM::BeatmapSpeedTransform::createSpeedVersion(shortSource, options);
    ok &= check(shortResult.success, "short metadata transform succeeds");
    ok &= check(
        isNearlyEqual(shortResult.beatmap.m_baseMapMetadata.map_length, 4867.0),
        "map length ignores divided metadata tail");

    auto tieSource                              = makeFixture();
    tieSource.m_audioSamples.front().m_offsetMs = -1;
    auto tieOptions                             = options;
    tieOptions.speed                            = 2.0;
    auto tieResult =
        MMM::BeatmapSpeedTransform::createSpeedVersion(tieSource, tieOptions);
    ok &= check(tieResult.success &&
                    tieResult.beatmap.m_audioSamples.size() == 1 &&
                    tieResult.beatmap.m_audioSamples.front().m_offsetMs == -1,
                "negative half millisecond rounds away from zero");

    MMM::BeatmapSpeedTransformOptions invalidOptions;
    invalidOptions.speed = 0.0;
    auto invalidResult =
        MMM::BeatmapSpeedTransform::createSpeedVersion(source, invalidOptions);
    ok &= check(!invalidResult.success, "invalid speed rejected");
    return ok;
}

/// @brief 验证 Malody time.delay 在倍速后保持数值语义并可往返。
/// @param outputRoot 测试输出目录。
/// @return 通过时返回 true。
bool runMalodyDelayRoundTrip(const std::filesystem::path& outputRoot)
{
    std::error_code createError;
    std::filesystem::create_directories(outputRoot, createError);
    if ( createError ) {
        return check(false, "Malody delay output directory created");
    }

    const auto     sourcePath = outputRoot / "speed_delay_source.mc";
    const auto     outputPath = outputRoot / "speed_delay_transformed.mc";
    nlohmann::json sourceJson{
        { "meta",
          { { "creator", "MMM" },
            { "version", "Delay" },
            { "mode", 0 },
            { "mode_ext", { { "column", 4 } } },
            { "song",
              { { "title", "Delay" },
                { "artist", "MMM" },
                { "file", "source.ogg" },
                { "bpm", 120.0 } } } } },
        { "time",
          nlohmann::json::array(
              { { { "beat", nlohmann::json::array({ 0, 0, 1 }) },
                  { "bpm", 120.0 },
                  { "delay", 300.0 } },
                { { "beat", nlohmann::json::array({ 4, 0, 1 }) },
                  { "bpm", 150.0 },
                  { "delay", -80.0 } } }) },
        { "note",
          nlohmann::json::array(
              { { { "beat", nlohmann::json::array({ 2, 0, 1 }) },
                  { "column", 1 } } }) }
    };

    {
        std::ofstream sourceFile(sourcePath,
                                 std::ios::binary | std::ios::trunc);
        if ( !sourceFile ) {
            return check(false, "Malody delay source opened");
        }
        sourceFile << sourceJson.dump();
    }

    MMM::BeatMap source = MMM::BeatMap::loadFromFile(sourcePath);
    MMM::BeatmapSpeedTransformOptions options;
    options.speed     = 2.0;
    options.mapPath   = outputPath.filename();
    options.audioPath = "transformed.ogg";
    options.name      = "Delay 2x";
    options.version   = "Delay 2x";

    auto result =
        MMM::BeatmapSpeedTransform::createSpeedVersion(source, options);
    bool ok = check(result.success, "Malody delay transform succeeds");
    ok &= check(result.beatmap.m_timings.size() == 2,
                "Malody delay timing count kept");
    if ( result.beatmap.m_timings.size() == 2 ) {
        const auto& firstProperties =
            result.beatmap.m_timings[0]
                .m_metadata.timing_properties[MMM::TimingMetadataType::MALODY];
        const auto& secondProperties =
            result.beatmap.m_timings[1]
                .m_metadata.timing_properties[MMM::TimingMetadataType::MALODY];
        const auto firstDelayIt  = firstProperties.find("delay");
        const auto secondDelayIt = secondProperties.find("delay");
        const auto firstDelay =
            firstDelayIt == firstProperties.end()
                ? nlohmann::json{}
                : nlohmann::json::parse(firstDelayIt->second, nullptr, false);
        const auto secondDelay =
            secondDelayIt == secondProperties.end()
                ? nlohmann::json{}
                : nlohmann::json::parse(secondDelayIt->second, nullptr, false);
        ok &= check(firstDelayIt != firstProperties.end() &&
                        firstDelay.is_number() &&
                        isNearlyEqual(firstDelay.get<double>(), 150.0),
                    "first Malody delay scaled as a number");
        ok &= check(secondDelayIt != secondProperties.end() &&
                        secondDelay.is_number() &&
                        isNearlyEqual(secondDelay.get<double>(), -40.0),
                    "second Malody delay scaled as a number");
    }

    ok &= check(result.beatmap.saveToFile(outputPath),
                "transformed Malody delay map saved");
    nlohmann::json savedJson;
    {
        std::ifstream outputFile(outputPath);
        if ( outputFile ) {
            savedJson = nlohmann::json::parse(outputFile, nullptr, false);
        }
    }
    ok &= check(
        !savedJson.is_discarded() && savedJson.contains("time") &&
            savedJson["time"].size() == 2 &&
            savedJson["time"][0]["delay"].is_number() &&
            savedJson["time"][1]["delay"].is_number() &&
            isNearlyEqual(savedJson["time"][0]["delay"].get<double>(), 150.0) &&
            isNearlyEqual(savedJson["time"][1]["delay"].get<double>(), -40.0),
        "Malody delay remains numeric in exported mc");

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
    ok &= check(reloaded.m_timings.size() == 2 &&
                    isNearlyEqual(reloaded.m_timings[0].m_timestamp, 150.0) &&
                    isNearlyEqual(reloaded.m_timings[1].m_timestamp, 1110.0),
                "scaled Malody timing anchors survive mc round trip");
    return ok;
}

/// @brief 校验真实谱面资源的倍速变换结果。
/// @param inputPath 输入谱面路径。
/// @param outputPath 输出 MMM 谱面路径。
/// @param speed 倍速倍率。
/// @return 通过时返回 true。
bool validateResourceBeatmap(const std::filesystem::path& inputPath,
                             const std::filesystem::path& outputPath,
                             double                       speed)
{
    MMM::BeatMap source = MMM::BeatMap::loadFromFile(inputPath);
    source.sync();

    bool ok = true;
    if ( source.m_allNotes.empty() && source.m_timings.empty() ) {
        XWARN("[speed-transform] Resource map has no notes or timings: {}",
              inputPath.string());
    } else {
        ok &= check(true, "resource loaded: " + inputPath.string());
    }

    MMM::BeatmapSpeedTransformOptions options;
    options.speed     = speed;
    options.mapPath   = outputPath.filename();
    options.audioPath = "coverage_audio.ogg";
    options.name      = source.m_baseMapMetadata.name + " coverage";
    options.version   = source.m_baseMapMetadata.version + " coverage";

    const double sourceContentEnd =
        MMM::BeatmapSpeedTransform::calculateContentEndTime(source);
    const double sourceFirst = firstNoteTime(source);
    const double sourceLast  = lastNoteTime(source);

    auto result =
        MMM::BeatmapSpeedTransform::createSpeedVersion(source, options);
    result.beatmap.sync();

    ok &= check(result.success, "resource transform succeeds");
    ok &= check(result.beatmap.m_allNotes.size() == source.m_allNotes.size(),
                "resource note count kept");
    ok &= check(result.beatmap.m_timings.size() == source.m_timings.size(),
                "resource timing count kept");
    ok &= check(
        result.beatmap.m_audioSamples.size() == source.m_audioSamples.size(),
        "resource automatic sample count kept");

    if ( sourceContentEnd > 0.0 ) {
        ok &= check(isNearlyEqual(result.beatmap.m_baseMapMetadata.map_length,
                                  sourceContentEnd / speed,
                                  0.501),
                    "resource content end scaled");
    }
    if ( !source.m_allNotes.empty() && !result.beatmap.m_allNotes.empty() ) {
        ok &= check(
            isNearlyEqual(firstNoteTime(result.beatmap), sourceFirst / speed),
            "resource first note time scaled");
        ok &= check(
            isNearlyEqual(lastNoteTime(result.beatmap), sourceLast / speed),
            "resource last note time scaled");
    }

    const auto timingCount =
        std::min(source.m_timings.size(), result.beatmap.m_timings.size());
    bool timingTimestampsOk = true;
    bool bpmValuesOk        = true;
    for ( std::size_t i = 0; i < timingCount; ++i ) {
        const auto& before = source.m_timings[i];
        const auto& after  = result.beatmap.m_timings[i];
        if ( !isNearlyEqual(
                 after.m_timestamp, before.m_timestamp / speed, 1e-3) ) {
            XERROR(
                "[speed-transform] timing timestamp mismatch at {}: {} vs {}",
                i,
                after.m_timestamp,
                before.m_timestamp / speed);
            timingTimestampsOk = false;
        }
        if ( before.m_timingEffect == MMM::TimingEffect::BPM &&
             before.m_bpm > 0.0 &&
             !isNearlyEqual(after.m_bpm, before.m_bpm * speed, 1e-3) ) {
            XERROR("[speed-transform] bpm mismatch at {}: {} vs {}",
                   i,
                   after.m_bpm,
                   before.m_bpm * speed);
            bpmValuesOk = false;
        }
    }
    ok &= check(timingTimestampsOk, "resource timing timestamps scaled");
    ok &= check(bpmValuesOk, "resource bpm values scaled");

    const auto sampleCount = std::min(source.m_audioSamples.size(),
                                      result.beatmap.m_audioSamples.size());
    bool       sampleTimelineOk = true;
    for ( std::size_t i = 0; i < sampleCount; ++i ) {
        const auto& before         = source.m_audioSamples[i];
        const auto& after          = result.beatmap.m_audioSamples[i];
        const auto  expectedOffset = static_cast<std::int64_t>(
            std::round(static_cast<long double>(before.m_offsetMs) / speed));
        if ( !isNearlyEqual(
                 after.m_timestamp, before.m_timestamp / speed, 1e-3) ||
             after.m_offsetMs != expectedOffset ||
             after.m_track != before.m_track ||
             after.m_audioResourceId != before.m_audioResourceId ||
             !isNearlyEqual(after.m_volume, before.m_volume) ) {
            XERROR("[speed-transform] automatic sample mismatch at {}", i);
            sampleTimelineOk = false;
        }
    }
    ok &= check(sampleTimelineOk, "resource automatic sample timeline scaled");

    std::error_code createError;
    std::filesystem::create_directories(outputPath.parent_path(), createError);
    ok &= check(!createError, "resource output directory created");
    ok &= check(result.beatmap.saveToFile(outputPath),
                "resource transformed map saved");
    if ( ok ) {
        MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
        reloaded.sync();
        if ( reloaded.m_allNotes.size() != result.beatmap.m_allNotes.size() ) {
            XWARN(
                "[speed-transform] transformed MMM reload note count differs "
                "for {}: saved={} reloaded={}",
                inputPath.string(),
                result.beatmap.m_allNotes.size(),
                reloaded.m_allNotes.size());
        } else {
            check(true, "resource transformed map reload note count");
        }
        if ( reloaded.m_timings.size() != result.beatmap.m_timings.size() ) {
            XWARN(
                "[speed-transform] transformed MMM reload timing count "
                "differs for {}: saved={} reloaded={}",
                inputPath.string(),
                result.beatmap.m_timings.size(),
                reloaded.m_timings.size());
        } else {
            check(true, "resource transformed map reload timing count");
        }
        if ( reloaded.m_audioSamples.size() !=
             result.beatmap.m_audioSamples.size() ) {
            XWARN(
                "[speed-transform] transformed MMM reload sample count "
                "differs for {}: saved={} reloaded={}",
                inputPath.string(),
                result.beatmap.m_audioSamples.size(),
                reloaded.m_audioSamples.size());
        } else {
            check(true, "resource transformed map reload sample count");
        }
    }

    return ok;
}

/// @brief 运行真实资源覆盖测试。
/// @param resourceRoot 资源根目录。
/// @param outputRoot 输出根目录。
/// @return 通过时返回 true。
bool runResourceCoverage(const std::filesystem::path& resourceRoot,
                         const std::filesystem::path& outputRoot)
{
    const auto files = collectBeatmapFiles(resourceRoot);
    bool       ok    = true;
    ok &= check(!files.empty(), "resource beatmap files discovered");

    std::size_t passed = 0;
    for ( std::size_t i = 0; i < files.size(); ++i ) {
        XINFO("[speed-transform] Resource case {} / {}: {}",
              i + 1,
              files.size(),
              files[i].string());
        const auto outputPath =
            outputRoot / ("speed_transform_case_" + std::to_string(i) + ".mmm");
        if ( validateResourceBeatmap(files[i], outputPath, 1.5) ) {
            ++passed;
        } else {
            ok = false;
        }
    }

    XINFO("[speed-transform] Resource coverage passed {}/{}",
          passed,
          files.size());
    return ok;
}

}  // namespace

int main(int argc, char* argv[])
{
    XLogger::init("BeatmapSpeedTransformTest");

    bool ok = runFixtureCoverage();
    if ( argc >= 3 ) {
        ok &= runMalodyDelayRoundTrip(argv[2]);
        ok &= runResourceCoverage(argv[1], argv[2]);
    }

    if ( !ok ) {
        XLogger::shutdown();
        return EXIT_FAILURE;
    }

    XINFO("BeatmapSpeedTransformTest passed.");
    XLogger::shutdown();
    return EXIT_SUCCESS;
}
