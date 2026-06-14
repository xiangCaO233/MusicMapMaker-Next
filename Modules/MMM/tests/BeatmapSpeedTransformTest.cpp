#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

/// @brief 浮点近似比较。
/// @param actual 实际值。
/// @param expected 期望值。
/// @param tolerance 容忍误差。
/// @return 足够接近时返回 true。
bool near(double actual, double expected, double tolerance = 1e-6)
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

    MMM::BeatmapSpeedTransformOptions invalidOptions;
    invalidOptions.speed = 0.0;
    auto invalidResult =
        MMM::BeatmapSpeedTransform::createSpeedVersion(source, invalidOptions);
    ok &= check(!invalidResult.success, "invalid speed rejected");
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

    if ( sourceContentEnd > 0.0 ) {
        ok &= check(near(result.beatmap.m_baseMapMetadata.map_length,
                         sourceContentEnd / speed,
                         1e-3),
                    "resource content end scaled");
    }
    if ( !source.m_allNotes.empty() && !result.beatmap.m_allNotes.empty() ) {
        ok &= check(near(firstNoteTime(result.beatmap), sourceFirst / speed),
                    "resource first note time scaled");
        ok &= check(near(lastNoteTime(result.beatmap), sourceLast / speed),
                    "resource last note time scaled");
    }

    const auto timingCount =
        std::min(source.m_timings.size(), result.beatmap.m_timings.size());
    bool timingTimestampsOk = true;
    bool bpmValuesOk        = true;
    for ( std::size_t i = 0; i < timingCount; ++i ) {
        const auto& before = source.m_timings[i];
        const auto& after  = result.beatmap.m_timings[i];
        if ( !near(after.m_timestamp, before.m_timestamp / speed, 1e-3) ) {
            XERROR(
                "[speed-transform] timing timestamp mismatch at {}: {} vs {}",
                i,
                after.m_timestamp,
                before.m_timestamp / speed);
            timingTimestampsOk = false;
        }
        if ( before.m_timingEffect == MMM::TimingEffect::BPM &&
             before.m_bpm > 0.0 &&
             !near(after.m_bpm, before.m_bpm * speed, 1e-3) ) {
            XERROR("[speed-transform] bpm mismatch at {}: {} vs {}",
                   i,
                   after.m_bpm,
                   before.m_bpm * speed);
            bpmValuesOk = false;
        }
    }
    ok &= check(timingTimestampsOk, "resource timing timestamps scaled");
    ok &= check(bpmValuesOk, "resource bpm values scaled");

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
