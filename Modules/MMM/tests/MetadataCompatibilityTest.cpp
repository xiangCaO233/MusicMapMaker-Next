#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string_view>
#include <system_error>

namespace
{

using json = nlohmann::json;

/// @brief 校验测试条件并记录失败原因。
/// @param condition 待校验条件。
/// @param message 条件失败时输出的说明。
/// @return 条件是否成立。
bool check(bool condition, std::string_view message)
{
    if ( !condition ) {
        XERROR("Metadata compatibility check failed: {}", message);
    }
    return condition;
}

/// @brief 将程序化测试内容写入构建输出目录。
/// @param path 输出文件路径。
/// @param content 待写入文本。
/// @return 文件是否写入成功。
bool writeTextFile(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file.is_open() ) {
        XERROR("Failed to open metadata compatibility fixture: {}",
               path.string());
        return false;
    }

    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file.good();
}

/// @brief 验证 osu! 字符串 Video 事件能够作为唯一背景载入。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testPureStringVideoEvent(const std::filesystem::path& outputDirectory)
{
    const auto                 path = outputDirectory / "pure_string_video.osu";
    constexpr std::string_view content = R"(osu file format v14

[Events]
Video,1234,"video.mp4"
)";
    if ( !writeTextFile(path, content) ) return false;

    const MMM::BeatMap map  = MMM::BeatMap::loadFromFile(path);
    const auto&        meta = map.m_baseMapMetadata;
    bool               ok   = true;
    ok &= check(meta.cover_type == MMM::CoverType::VIDEO,
                "string Video event should select video background");
    ok &= check(meta.video_starttime == 1234,
                "string Video event should keep start time");
    ok &= check(meta.main_cover_path == std::filesystem::path("video.mp4"),
                "string Video event should keep video path");
    return ok;
}

/// @brief 验证数字 1 视频事件优先于同文件中的图片背景事件。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testNumericVideoEventPriority(const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "numeric_video_priority.osu";
    constexpr std::string_view content = R"(osu file format v14

[Events]
0,0,"image.jpg",3,4
1,5678,"numeric.mp4"
)";
    if ( !writeTextFile(path, content) ) return false;

    const MMM::BeatMap map  = MMM::BeatMap::loadFromFile(path);
    const auto&        meta = map.m_baseMapMetadata;
    bool               ok   = true;
    ok &= check(meta.cover_type == MMM::CoverType::VIDEO,
                "numeric video event should override image background");
    ok &= check(meta.video_starttime == 5678,
                "numeric video event should keep start time");
    ok &= check(meta.main_cover_path == std::filesystem::path("numeric.mp4"),
                "numeric video event should keep video path");
    return ok;
}

/// @brief 验证没有视频事件时继续读取普通图片背景。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testImageEventFallback(const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "image_background_fallback.osu";
    constexpr std::string_view content = R"(osu file format v14

[Events]
0,0,"image.jpg",12,-8
)";
    if ( !writeTextFile(path, content) ) return false;

    const MMM::BeatMap map  = MMM::BeatMap::loadFromFile(path);
    const auto&        meta = map.m_baseMapMetadata;
    bool               ok   = true;
    ok &= check(meta.cover_type == MMM::CoverType::IMAGE,
                "image event should remain the fallback background");
    ok &= check(meta.main_cover_path == std::filesystem::path("image.jpg"),
                "image event should keep image path");
    ok &= check(meta.bgxoffset == 12 && meta.bgyoffset == -8,
                "image event should keep background offsets");
    return ok;
}

/// @brief 验证 MMM 原生格式完整往返视频背景元数据。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testMMMVideoMetadataRoundTrip(const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "video_metadata_round_trip.mmm";

    MMM::BeatMap source;
    auto&        sourceMeta    = source.m_baseMapMetadata;
    sourceMeta.name            = "Video metadata round trip";
    sourceMeta.main_cover_path = "videos/background.mp4";
    sourceMeta.cover_type      = MMM::CoverType::VIDEO;
    sourceMeta.video_starttime = 2468;
    sourceMeta.bgxoffset       = -17;
    sourceMeta.bgyoffset       = 29;
    sourceMeta.track_count     = 4;
    sourceMeta.preference_bpm  = 120.0;
    sourceMeta.map_length      = 30000.0;

    if ( !source.saveToFile(path) ) {
        XERROR("Failed to save MMM metadata round-trip fixture: {}",
               path.string());
        return false;
    }

    const MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    const auto&        meta   = loaded.m_baseMapMetadata;
    bool               ok     = true;
    ok &= check(meta.cover_type == MMM::CoverType::VIDEO,
                "MMM round trip should keep cover type");
    ok &= check(meta.video_starttime == 2468,
                "MMM round trip should keep video start time");
    ok &= check(meta.bgxoffset == -17 && meta.bgyoffset == 29,
                "MMM round trip should keep background offsets");
    ok &= check(
        meta.main_cover_path == std::filesystem::path("videos/background.mp4"),
        "MMM round trip should keep background path");
    return ok;
}

/// @brief 验证 MMM v2 完整保存玩家命中采样与多个自动采样对象。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testMMMVersion2AudioSampleRoundTrip(
    const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "audio_sample_v2_round_trip.mmm";

    MMM::BeatMap source;
    source.m_baseMapMetadata.name            = "Audio sample v2";
    source.m_baseMapMetadata.track_count     = 4;
    source.m_baseMapMetadata.bgm_track_count = 3;
    source.m_baseMapMetadata.preference_bpm  = 120.0;
    source.m_baseMapMetadata.main_audio_path = "legacy-main.ogg";
    source.m_baseMapMetadata.song_file_hint  = "display-hint.ogg";

    MMM::Note note;
    note.m_timestamp = 1500.0;
    note.m_track     = 2;
    note.setSampleBinding({ "hit.wav", 0.65F });
    source.m_noteData.notes.push_back(note);

    MMM::AudioSampleEvent stem;
    stem.m_timestamp       = 500.0;
    stem.m_offsetMs        = -125;
    stem.m_track           = 4;
    stem.m_audioResourceId = "stem.ogg";
    stem.m_volume          = 0.8F;
    stem.m_metadata
        .sample_properties[MMM::SampleMetadataType::MALODY]["source_x"] = "4";
    source.m_audioSamples.push_back(stem);

    MMM::AudioSampleEvent effect;
    effect.m_timestamp       = 1000.0;
    effect.m_offsetMs        = 250;
    effect.m_track           = 6;
    effect.m_audioResourceId = "effect.wav";
    effect.m_volume          = 0.35F;
    effect.m_metadata
        .sample_properties[MMM::SampleMetadataType::MMM]["editor_label"] =
        "effect layer";
    source.m_audioSamples.push_back(effect);
    source.sync();

    if ( !source.saveToFile(path) ) {
        XERROR("Failed to save MMM v2 audio sample fixture: {}", path.string());
        return false;
    }

    json saved;
    {
        std::ifstream input(path);
        if ( !input ) return false;
        input >> saved;
    }

    bool ok = true;
    ok &= check(saved.value("format_version", 0) == 2,
                "MMM v2 should declare format_version 2");
    ok &= check(saved["metadata"]["base"].value("bgm_track_count", 0) == 3,
                "MMM v2 should save BGM track count");
    ok &= check(saved["metadata"]["base"].value("song_file_hint", "") ==
                    "display-hint.ogg",
                "MMM v2 should save song file hint separately");
    ok &= check(
        saved.contains("audio_samples") && saved["audio_samples"].size() == 2,
        "MMM v2 should save every automatic sample");
    if ( saved.contains("audio_samples") &&
         saved["audio_samples"].size() == 2 ) {
        const auto& first = saved["audio_samples"][0];
        ok &= check(first.value("audio_ref", "") == "stem.ogg",
                    "MMM v2 should save sample resource id");
        ok &= check(first.value("offset_ms", 0) == -125,
                    "MMM v2 should use integer offset_ms");
        ok &= check(!first.contains("offset"),
                    "MMM v2 should not emit transitional offset");
        ok &= check(first.value("track", 0) == 4,
                    "MMM v2 should save absolute sample track");
        ok &= check(std::abs(first.value("volume", 0.0) - 0.8) < 1e-6,
                    "MMM v2 should save normalized sample volume");
    }
    ok &= check(saved.contains("note") && saved["note"].size() == 1,
                "MMM v2 should save the playable note");
    if ( saved.contains("note") && saved["note"].size() == 1 ) {
        const auto& savedNote = saved["note"][0];
        ok &= check(savedNote.contains("sample"),
                    "MMM v2 should use a nested playable sample binding");
        ok &= check(!savedNote.contains("bound_sound") &&
                        !savedNote.contains("bound_volume"),
                    "MMM v2 should not emit legacy bound fields");
        if ( savedNote.contains("sample") ) {
            ok &= check(savedNote["sample"].value("audio_ref", "") == "hit.wav",
                        "MMM v2 should save playable sample resource id");
            ok &= check(std::abs(savedNote["sample"].value("volume", 0.0) -
                                 0.65) < 1e-6,
                        "MMM v2 should save playable sample volume");
        }
    }

    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    loaded.sync();
    ok &= check(loaded.m_baseMapMetadata.bgm_track_count == 3,
                "MMM v2 should reload BGM track count");
    ok &= check(loaded.m_baseMapMetadata.main_audio_path ==
                        std::filesystem::path("legacy-main.ogg") &&
                    loaded.m_baseMapMetadata.song_file_hint ==
                        std::filesystem::path("display-hint.ogg"),
                "MMM v2 should keep legacy audio path and song hint distinct");
    ok &= check(loaded.m_audioSamples.size() == 2,
                "MMM v2 should reload every automatic sample");
    if ( loaded.m_audioSamples.size() == 2 ) {
        const auto& loadedStem   = loaded.m_audioSamples[0];
        const auto& loadedEffect = loaded.m_audioSamples[1];
        ok &= check(loadedStem.m_audioResourceId == "stem.ogg" &&
                        loadedStem.m_timestamp == 500.0 &&
                        loadedStem.m_offsetMs == -125 &&
                        loadedStem.m_track == 4 &&
                        std::abs(loadedStem.m_volume - 0.8F) < 1e-6F,
                    "MMM v2 should reload the first automatic sample");
        ok &= check(loadedStem.m_metadata.getValue<std::string>(
                        MMM::SampleMetadataType::MALODY, "source_x") == "4",
                    "MMM v2 should reload Malody sample metadata");
        ok &= check(loadedEffect.m_audioResourceId == "effect.wav" &&
                        loadedEffect.m_timestamp == 1000.0 &&
                        loadedEffect.m_offsetMs == 250 &&
                        loadedEffect.m_track == 6 &&
                        std::abs(loadedEffect.m_volume - 0.35F) < 1e-6F,
                    "MMM v2 should reload the second automatic sample");
        ok &= check(
            loadedEffect.m_metadata.getValue<std::string>(
                MMM::SampleMetadataType::MMM, "editor_label") == "effect layer",
            "MMM v2 should reload native sample metadata");
    }
    ok &= check(loaded.m_allNotes.size() == 1,
                "automatic samples should not enter playable note list");
    if ( loaded.m_allNotes.size() == 1 ) {
        const auto binding = loaded.m_allNotes.front().get().getSampleBinding();
        ok &= check(binding.has_value() &&
                        binding->m_audioResourceId == "hit.wav" &&
                        std::abs(binding->m_volume - 0.65F) < 1e-6F,
                    "MMM v2 should reload playable sample binding");
    }
    return ok;
}

/// @brief 验证旧版 MMM 文件迁移单音频字段和旧玩家采样字段。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testLegacyMMMMetadataDefaults(const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "legacy_metadata_defaults.mmm";
    constexpr std::string_view content =
        R"({"metadata":{"base":{"name":"Legacy","audio":"legacy.ogg","cover":"legacy.png","track_count":4}},"timing":[],"note":[{"type":"note","timestamp":1000,"track":1,"bound_sound":"legacy-hit.wav"}]})";
    if ( !writeTextFile(path, content) ) return false;

    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    const auto&  meta   = loaded.m_baseMapMetadata;
    bool         ok     = true;
    ok &= check(meta.cover_type == MMM::CoverType::IMAGE,
                "legacy MMM should default to image background");
    ok &= check(meta.video_starttime == 0,
                "legacy MMM should default video start time to zero");
    ok &= check(meta.bgxoffset == 0 && meta.bgyoffset == 0,
                "legacy MMM should default background offsets to zero");
    ok &= check(meta.main_audio_path == std::filesystem::path("legacy.ogg") &&
                    meta.song_file_hint == std::filesystem::path("legacy.ogg"),
                "legacy MMM audio should populate both compatibility fields");
    ok &= check(meta.track_count == 4 && meta.bgm_track_count == 1,
                "legacy MMM audio should create one BGM track");
    ok &= check(loaded.m_audioSamples.size() == 1,
                "legacy MMM audio should migrate to one automatic sample");
    if ( loaded.m_audioSamples.size() == 1 ) {
        const auto& sample = loaded.m_audioSamples.front();
        ok &= check(sample.m_timestamp == 0.0 && sample.m_offsetMs == 0 &&
                        sample.m_track == 4 &&
                        sample.m_audioResourceId == "legacy.ogg" &&
                        std::abs(sample.m_volume - 1.0F) < 1e-6F,
                    "legacy MMM automatic sample migration should be stable");
    }
    ok &= check(loaded.m_allNotes.size() == 1,
                "legacy playable note should still load");
    if ( loaded.m_allNotes.size() == 1 ) {
        const auto binding = loaded.m_allNotes.front().get().getSampleBinding();
        ok &= check(binding.has_value() &&
                        binding->m_audioResourceId == "legacy-hit.wav" &&
                        std::abs(binding->m_volume - 1.0F) < 1e-6F,
                    "legacy bound_sound should default binding volume to one");
    }

    const auto migratedPath =
        outputDirectory / "legacy_metadata_migrated_v2.mmm";
    ok &= check(loaded.saveToFile(migratedPath),
                "migrated legacy MMM should save");
    if ( ok ) {
        json          migrated;
        std::ifstream input(migratedPath);
        input >> migrated;
        ok &= check(migrated.value("format_version", 0) == 2,
                    "migrated legacy MMM should save as v2");
        ok &= check(migrated.contains("audio_samples") &&
                        migrated["audio_samples"].size() == 1 &&
                        migrated["audio_samples"][0].value("offset_ms", 1) == 0,
                    "migrated legacy MMM should emit canonical audio_samples");
        ok &=
            check(migrated["note"].size() == 1 &&
                      migrated["note"][0].contains("sample") &&
                      !migrated["note"][0].contains("bound_sound"),
                  "migrated legacy note should emit canonical sample binding");
    }
    return ok;
}

}  // namespace

/// @brief 运行背景元数据格式兼容测试。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数，首个参数为测试输出目录。
/// @return 全部验证通过时返回 0，否则返回 1。
int main(int argc, char* argv[])
{
    if ( argc < 2 ) {
        XERROR("Usage: MetadataCompatibilityTest <output_directory>");
        return 1;
    }

    const std::filesystem::path outputDirectory = argv[1];
    std::error_code             directoryError;
    std::filesystem::create_directories(outputDirectory, directoryError);
    if ( directoryError ) {
        XERROR("Failed to create metadata compatibility output directory: {}",
               directoryError.message());
        return 1;
    }

    bool ok = true;
    ok &= testPureStringVideoEvent(outputDirectory);
    ok &= testNumericVideoEventPriority(outputDirectory);
    ok &= testImageEventFallback(outputDirectory);
    ok &= testMMMVideoMetadataRoundTrip(outputDirectory);
    ok &= testMMMVersion2AudioSampleRoundTrip(outputDirectory);
    ok &= testLegacyMMMMetadataDefaults(outputDirectory);

    if ( ok ) {
        XINFO("Metadata compatibility tests passed.");
        return 0;
    }
    return 1;
}
