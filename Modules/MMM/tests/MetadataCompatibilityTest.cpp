#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/ProjectSettings.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
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

/// @brief 验证项目音频工具的打开状态、选择和方块布局可完整往返。
bool testProjectAudioToolWorkspaceRoundTrip()
{
    MMM::ProjectWorkspaceState source;
    source.m_projectAudioToolOpen               = true;
    source.m_projectAudioToolSelectedResourceId = "main";
    source.m_projectAudioToolPlacements         = {
        MMM::ProjectAudioToolItemPlacement{
            .m_audioResourceId = "main",
            .m_x               = 12.5F,
            .m_y               = 30.0F,
            .m_width           = 260.0F,
            .m_height          = 120.0F,
            .m_zOrder          = 4,
        },
        MMM::ProjectAudioToolItemPlacement{
            .m_audioResourceId = "effect",
            .m_x               = 80.0F,
            .m_y               = 50.0F,
            .m_zOrder          = 5,
        },
    };

    const json serialized = source;
    const auto restored   = serialized.get<MMM::ProjectWorkspaceState>();
    const auto legacyPlacement =
        json{
            { "m_audioResourceId", "legacy" },
            { "m_x", 4.0F },
            { "m_y", 8.0F },
            { "m_zOrder", 1 },
        }
            .get<MMM::ProjectAudioToolItemPlacement>();
    return check(restored.m_projectAudioToolOpen,
                 "project audio tool open state should round trip") &&
           check(restored.m_projectAudioToolSelectedResourceId == "main",
                 "project audio selection should round trip") &&
           check(restored.m_projectAudioToolPlacements.size() == 2,
                 "project audio placements should round trip") &&
           check(
               restored.m_projectAudioToolPlacements[1].m_audioResourceId ==
                       "effect" &&
                   std::abs(restored.m_projectAudioToolPlacements[1].m_x -
                            80.0F) < 1e-6F &&
                   std::abs(restored.m_projectAudioToolPlacements[0].m_width -
                            260.0F) < 1e-6F &&
                   std::abs(restored.m_projectAudioToolPlacements[0].m_height -
                            120.0F) < 1e-6F &&
                   restored.m_projectAudioToolPlacements[1].m_zOrder == 5,
               "project audio placement fields should remain intact") &&
           check(legacyPlacement.m_width == 0.0F &&
                     legacyPlacement.m_height == 0.0F,
                 "legacy project audio placements should keep automatic size");
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
    ok &= check(!saved["metadata"]["base"].contains("audio"),
                "MMM v2 should not emit the legacy audio field");
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
    ok &= check(loaded.m_baseMapMetadata.main_audio_path.empty() &&
                    loaded.m_baseMapMetadata.song_file_hint ==
                        std::filesystem::path("display-hint.ogg"),
                "MMM v2 should keep song hint separate from legacy audio path");
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
    const auto originalMalodyPath =
        outputDirectory / "legacy_metadata_defaults.mc";
    constexpr std::string_view content =
        R"({"metadata":{"base":{"name":"Legacy","audio":"legacy.ogg","cover":"legacy.png","track_count":4}},"timing":[{"timestamp":250,"bpm":120,"beat_length":500,"effect":"bpm","param":120}],"note":[{"type":"note","timestamp":1000,"track":1,"bound_sound":"legacy-hit.wav"}]})";
    if ( !writeTextFile(path, content) ||
         !writeTextFile(originalMalodyPath, "{}") ) {
        return false;
    }

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
    ok &= check(
        loaded.m_loadDiagnostics.size() == 1 &&
            loaded.m_loadDiagnostics.front().m_code ==
                MMM::BeatmapLoadDiagnosticCode::
                    LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE &&
            loaded.m_loadDiagnostics.front().m_severity ==
                MMM::BeatmapLoadDiagnosticSeverity::
                    BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING &&
            loaded.m_loadDiagnostics.front().m_relatedPath ==
                originalMalodyPath,
        "legacy MMM should expose a structured reimport diagnostic when the "
        "original Malody file exists");
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
        ok &= check(loaded.m_allNotes.front().get().m_timestamp == 1000.0,
                    "legacy audio migration should not move playable notes");
        const auto binding = loaded.m_allNotes.front().get().getSampleBinding();
        ok &= check(binding.has_value() &&
                        binding->m_audioResourceId == "legacy-hit.wav" &&
                        std::abs(binding->m_volume - 1.0F) < 1e-6F,
                    "legacy bound_sound should default binding volume to one");
    }
    ok &= check(loaded.m_timings.size() == 1 &&
                    loaded.m_timings.front().m_timestamp == 250.0,
                "legacy audio migration should not move timing events");

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
        ok &= check(!migrated["metadata"]["base"].contains("audio"),
                    "migrated MMM v2 should omit the legacy audio field");
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

    const auto withoutOriginalPath =
        outputDirectory / "legacy_metadata_without_original.mmm";
    auto absentOriginalPath = withoutOriginalPath;
    absentOriginalPath.replace_extension(".mc");
    std::error_code removeError;
    std::filesystem::remove(absentOriginalPath, removeError);
    ok &= check(writeTextFile(withoutOriginalPath, content),
                "legacy MMM fixture without original Malody should be written");
    if ( std::filesystem::exists(withoutOriginalPath) ) {
        const MMM::BeatMap withoutOriginal =
            MMM::BeatMap::loadFromFile(withoutOriginalPath);
        ok &= check(withoutOriginal.m_loadDiagnostics.empty(),
                    "legacy MMM should not emit a reimport diagnostic without "
                    "a sibling Malody file");
    }
    return ok;
}

/// @brief 验证带版本号的过渡 MMM 仍能读取旧 metadata.base.audio 提示。
/// @param outputDirectory 测试输出目录。
/// @return 旧字段仅迁移为提示且不会隐式生成播放对象时返回 true。
bool testVersion2LegacyAudioHintCompatibility(
    const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "v2_legacy_audio_hint.mmm";
    constexpr std::string_view content =
        R"({"format_version":2,"metadata":{"base":{"audio":"legacy-hint.ogg","track_count":4}},"audio_samples":[],"timing":[],"note":[]})";
    if ( !writeTextFile(path, content) ) return false;

    const MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    bool               ok     = true;
    ok &= check(loaded.m_baseMapMetadata.main_audio_path.empty(),
                "MMM v2 legacy audio field must not regain runtime authority");
    ok &= check(loaded.m_baseMapMetadata.song_file_hint ==
                    std::filesystem::path("legacy-hint.ogg"),
                "MMM v2 should accept metadata.base.audio as a hint fallback");
    ok &= check(loaded.m_audioSamples.empty(),
                "MMM v2 hint fallback must not synthesize a playback object");
    return ok;
}

/// @brief 验证 MMM v2 自动采样不会停留在玩家轨道区。
/// @param outputDirectory 测试输出目录。
/// @return 非法绝对轨道迁移到第一条 BGM 轨并保留来源值时返回 true。
bool testVersion2InvalidSampleTrackRelocation(
    const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "v2_invalid_sample_track.mmm";
    constexpr std::string_view content =
        R"({"format_version":2,"metadata":{"base":{"track_count":4,"bgm_track_count":0}},"audio_samples":[{"timestamp":125,"offset_ms":-25,"track":2,"audio_ref":"effect.wav","volume":0.75}],"timing":[],"note":[]})";
    if ( !writeTextFile(path, content) ) return false;

    const MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    bool               ok     = true;
    ok &= check(loaded.m_audioSamples.size() == 1,
                "MMM v2 should retain an invalid-track sample");
    if ( loaded.m_audioSamples.size() == 1 ) {
        const auto& sample = loaded.m_audioSamples.front();
        ok &=
            check(sample.m_track == 4 &&
                      loaded.m_baseMapMetadata.bgm_track_count == 1,
                  "MMM v2 invalid sample track should move to first BGM lane");
        ok &= check(sample.m_metadata.getValue<std::string>(
                        MMM::SampleMetadataType::MMM, "original_track") == "2",
                    "MMM v2 should retain the original invalid track");
    }
    ok &= check(std::any_of(loaded.m_loadDiagnostics.begin(),
                            loaded.m_loadDiagnostics.end(),
                            [](const MMM::BeatmapLoadDiagnostic& diagnostic) {
                                return diagnostic.m_code ==
                                       MMM::BeatmapLoadDiagnosticCode::
                                           AUDIO_SAMPLE_TRACK_RELOCATED;
                            }),
                "MMM v2 invalid sample track should emit a diagnostic");
    return ok;
}

/// @brief 验证 osu! 单音频字段迁移到第一条 BGM 轨且往返不移动谱面事件。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testOSUSingleAudioMigration(const std::filesystem::path& outputDirectory)
{
    const auto sourcePath = outputDirectory / "osu_single_audio_source.osu";
    constexpr std::string_view content = R"(osu file format v14

[General]
AudioFilename: legacy audio.ogg
Mode: 3

[Difficulty]
CircleSize: 4

[TimingPoints]
0,500,4,2,0,100,1,0

[HitObjects]
64,192,1000,1,0,0:0:0:0:
)";
    if ( !writeTextFile(sourcePath, content) ) return false;

    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    bool         ok     = true;
    ok &= check(loaded.m_baseMapMetadata.main_audio_path ==
                        std::filesystem::path("legacy audio.ogg") &&
                    loaded.m_baseMapMetadata.song_file_hint ==
                        std::filesystem::path("legacy audio.ogg"),
                "osu! audio should populate both compatibility fields");
    ok &= check(loaded.m_baseMapMetadata.track_count == 4 &&
                    loaded.m_baseMapMetadata.bgm_track_count >= 1,
                "osu! audio should reserve the first BGM track");
    ok &= check(loaded.m_audioSamples.size() == 1,
                "osu! audio should migrate to one automatic sample");
    if ( loaded.m_audioSamples.size() == 1 ) {
        const auto& sample = loaded.m_audioSamples.front();
        ok &= check(sample.m_timestamp == 0.0 && sample.m_offsetMs == 0 &&
                        sample.m_track == 4 &&
                        sample.m_audioResourceId == "legacy audio.ogg" &&
                        std::abs(sample.m_volume - 1.0F) < 1e-6F,
                    "osu! automatic sample migration should use 0/0/K");
    }
    ok &= check(loaded.m_timings.size() == 1 &&
                    loaded.m_timings.front().m_timestamp == 0.0,
                "osu! audio migration should not move timing events");
    ok &= check(loaded.m_allNotes.size() == 1 &&
                    loaded.m_allNotes.front().get().m_timestamp == 1000.0,
                "osu! audio migration should not move playable notes");

    const auto exportedPath = outputDirectory / "osu_single_audio_exported.osu";
    ok &= check(loaded.saveToFile(exportedPath),
                "canonical osu! single audio should export");
    if ( ok ) {
        MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportedPath);
        ok &= check(reloaded.m_audioSamples.size() == 1 &&
                        reloaded.m_audioSamples.front().m_timestamp == 0.0 &&
                        reloaded.m_audioSamples.front().m_offsetMs == 0 &&
                        reloaded.m_audioSamples.front().m_track == 4 &&
                        reloaded.m_audioSamples.front().m_audioResourceId ==
                            "legacy audio.ogg",
                    "canonical osu! single audio should round trip");
        ok &= check(reloaded.m_timings.size() == 1 &&
                        reloaded.m_timings.front().m_timestamp == 0.0 &&
                        reloaded.m_allNotes.size() == 1 &&
                        reloaded.m_allNotes.front().get().m_timestamp == 1000.0,
                    "osu! round trip should keep timing and note positions");
    }
    return ok;
}

/// @brief 验证 RM/IMD 同名前缀音频迁移到第一条 BGM 轨。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testRMSingleAudioMigration(const std::filesystem::path& outputDirectory)
{
    const auto audioPath = outputDirectory / "LegacyAudio.flac";
    if ( !writeTextFile(audioPath, "fLaC") ) return false;

    MMM::BeatMap source;
    source.m_baseMapMetadata.track_count     = 4;
    source.m_baseMapMetadata.bgm_track_count = 1;
    MMM::AudioSampleEvent sample;
    sample.m_timestamp       = 0.0;
    sample.m_offsetMs        = 0;
    sample.m_track           = 4;
    sample.m_audioResourceId = "LegacyAudio.flac";
    sample.m_volume          = 1.0F;
    source.m_audioSamples.push_back(sample);

    const auto mapPath = outputDirectory / "LegacyAudio_4k_Test.imd";
    bool       ok      = true;
    ok &= check(source.saveToFile(mapPath),
                "canonical RM/IMD single audio should export");
    if ( !ok ) return false;

    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(mapPath);
    ok &= check(loaded.m_baseMapMetadata.main_audio_path ==
                        std::filesystem::path("LegacyAudio.flac") &&
                    loaded.m_baseMapMetadata.song_file_hint ==
                        std::filesystem::path("LegacyAudio.flac"),
                "RM/IMD audio should populate both compatibility fields");
    ok &= check(loaded.m_baseMapMetadata.track_count == 4 &&
                    loaded.m_baseMapMetadata.bgm_track_count >= 1,
                "RM/IMD audio should reserve the first BGM track");
    ok &= check(loaded.m_audioSamples.size() == 1,
                "RM/IMD audio should migrate to one automatic sample");
    if ( loaded.m_audioSamples.size() == 1 ) {
        const auto& loadedSample = loaded.m_audioSamples.front();
        ok &= check(loadedSample.m_timestamp == 0.0 &&
                        loadedSample.m_offsetMs == 0 &&
                        loadedSample.m_track == 4 &&
                        loadedSample.m_audioResourceId == "LegacyAudio.flac" &&
                        std::abs(loadedSample.m_volume - 1.0F) < 1e-6F,
                    "RM/IMD automatic sample migration should use 0/0/K");
    }
    ok &= check(loaded.m_timings.empty() && loaded.m_allNotes.empty(),
                "RM/IMD audio migration should not add timing or notes");
    return ok;
}

/// @brief 验证单音频格式拒绝无法无损表达的自动采样时间线。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testSingleAudioExporterRejection(
    const std::filesystem::path& outputDirectory)
{
    MMM::BeatMap timedSource;
    timedSource.m_baseMapMetadata.track_count     = 4;
    timedSource.m_baseMapMetadata.bgm_track_count = 1;
    MMM::AudioSampleEvent timedSample;
    timedSample.m_timestamp       = 500.0;
    timedSample.m_offsetMs        = -25;
    timedSample.m_track           = 4;
    timedSample.m_audioResourceId = "Reject.ogg";
    timedSource.m_audioSamples.push_back(timedSample);

    const auto      timedOSUPath = outputDirectory / "reject_timed_sample.osu";
    const auto      timedIMDPath = outputDirectory / "Reject_4k_Timed.imd";
    std::error_code removeError;
    std::filesystem::remove(timedOSUPath, removeError);
    removeError.clear();
    std::filesystem::remove(timedIMDPath, removeError);

    bool ok = true;
    ok &= check(!timedSource.saveToFile(timedOSUPath),
                "osu! should reject a timed automatic sample");
    ok &= check(!timedSource.saveToFile(timedIMDPath),
                "RM/IMD should reject a timed automatic sample");
    ok &= check(!std::filesystem::exists(timedOSUPath) &&
                    !std::filesystem::exists(timedIMDPath),
                "rejected timed exports should not leave partial files");

    MMM::BeatMap layeredSource;
    layeredSource.m_baseMapMetadata.track_count     = 4;
    layeredSource.m_baseMapMetadata.bgm_track_count = 1;
    MMM::AudioSampleEvent first;
    first.m_track           = 4;
    first.m_audioResourceId = "Layered.ogg";
    layeredSource.m_audioSamples.push_back(first);
    MMM::AudioSampleEvent second;
    second.m_track           = 5;
    second.m_audioResourceId = "effect.wav";
    layeredSource.m_audioSamples.push_back(second);

    const auto layeredOSUPath = outputDirectory / "reject_layered_audio.osu";
    const auto layeredIMDPath = outputDirectory / "Layered_4k_Test.imd";
    removeError.clear();
    std::filesystem::remove(layeredOSUPath, removeError);
    removeError.clear();
    std::filesystem::remove(layeredIMDPath, removeError);
    ok &= check(!layeredSource.saveToFile(layeredOSUPath),
                "osu! should reject multiple automatic samples");
    ok &= check(!layeredSource.saveToFile(layeredIMDPath),
                "RM/IMD should reject multiple automatic samples");
    ok &= check(!std::filesystem::exists(layeredOSUPath) &&
                    !std::filesystem::exists(layeredIMDPath),
                "rejected layered exports should not leave partial files");

    MMM::BeatMap boundSource;
    boundSource.m_baseMapMetadata.track_count = 4;
    MMM::Note boundNote;
    boundNote.m_track = 1;
    boundNote.setSampleBinding({ "hit.wav", 0.6F });
    boundSource.m_noteData.notes.push_back(boundNote);
    boundSource.sync();

    const auto boundOSUPath = outputDirectory / "reject_bound_note.osu";
    const auto boundIMDPath = outputDirectory / "Bound_4k_Note.imd";
    removeError.clear();
    std::filesystem::remove(boundOSUPath, removeError);
    removeError.clear();
    std::filesystem::remove(boundIMDPath, removeError);
    ok &= check(boundSource.saveToFile(boundOSUPath),
                "osu! should preserve representable playable sample bindings");
    ok &= check(!boundSource.saveToFile(boundIMDPath),
                "RM/IMD should reject playable sample bindings");
    ok &= check(std::filesystem::exists(boundOSUPath) &&
                    !std::filesystem::exists(boundIMDPath),
                "bound-note exports should follow each format capability");
    if ( std::filesystem::exists(boundOSUPath) ) {
        const MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(boundOSUPath);
        const auto         binding =
            reloaded.m_allNotes.empty()
                ? std::optional<MMM::AudioSampleBinding>{}
                : reloaded.m_allNotes.front().get().getSampleBinding();
        ok &= check(
            binding.has_value() && binding->m_audioResourceId == "hit.wav",
            "osu! should round-trip a playable sample file");
    }

    MMM::BeatMap emptyBgmSource;
    emptyBgmSource.m_baseMapMetadata.track_count     = 4;
    emptyBgmSource.m_baseMapMetadata.bgm_track_count = 2;

    const auto emptyBgmOSUPath = outputDirectory / "reject_empty_bgm_lanes.osu";
    const auto emptyBgmIMDPath = outputDirectory / "Empty_4k_Bgm.imd";
    removeError.clear();
    std::filesystem::remove(emptyBgmOSUPath, removeError);
    removeError.clear();
    std::filesystem::remove(emptyBgmIMDPath, removeError);
    ok &= check(!emptyBgmSource.saveToFile(emptyBgmOSUPath),
                "osu! should reject unrepresentable empty BGM tracks");
    ok &= check(!emptyBgmSource.saveToFile(emptyBgmIMDPath),
                "RM/IMD should reject unrepresentable empty BGM tracks");
    ok &= check(!std::filesystem::exists(emptyBgmOSUPath) &&
                    !std::filesystem::exists(emptyBgmIMDPath),
                "rejected empty-BGM exports should not leave partial files");
    return ok;
}

/// @brief 验证 Malody 保存器不再根据歌曲提示伪造 SOUND 对象。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testMalodySaverDoesNotSynthesizeAudioSample(
    const std::filesystem::path& outputDirectory)
{
    MMM::BeatMap source;
    source.m_baseMapMetadata.track_count    = 4;
    source.m_baseMapMetadata.song_file_hint = "hint.ogg";

    const auto path = outputDirectory / "malody_hint_without_sample.mc";
    if ( !source.saveToFile(path) ) {
        return false;
    }

    json          saved;
    std::ifstream input(path);
    if ( !input ) return false;
    input >> saved;

    bool hasAutomaticSample = false;
    if ( saved.contains("note") && saved["note"].is_array() ) {
        for ( const auto& note : saved["note"] ) {
            if ( note.is_object() && note.value("type", 0) == 1 ) {
                hasAutomaticSample = true;
                break;
            }
        }
    }
    return check(!hasAutomaticSample,
                 "Malody saver should serialize only explicit samples");
}

/// @brief 验证 osu! 保存器不会把仅作提示的旧音频字段重新物化为播放内容。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testOSUSaverDoesNotSynthesizeAudioSample(
    const std::filesystem::path& outputDirectory)
{
    MMM::BeatMap source;
    source.m_baseMapMetadata.track_count     = 4;
    source.m_baseMapMetadata.main_audio_path = "legacy.ogg";
    source.m_baseMapMetadata.song_file_hint  = "hint.ogg";
    source.m_metadata
        .map_properties[MMM::MapMetadataType::OSU]["General::AudioFilename"] =
        "source.ogg";

    const auto path = outputDirectory / "osu_hint_without_sample.osu";
    if ( !source.saveToFile(path) ) return false;

    std::ifstream input(path);
    if ( !input ) return false;
    const std::string saved((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    return check(saved.find("AudioFilename: \n") != std::string::npos,
                 "osu! saver should serialize audio only from an explicit "
                 "sample");
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
    ok &= testProjectAudioToolWorkspaceRoundTrip();
    ok &= testPureStringVideoEvent(outputDirectory);
    ok &= testNumericVideoEventPriority(outputDirectory);
    ok &= testImageEventFallback(outputDirectory);
    ok &= testMMMVideoMetadataRoundTrip(outputDirectory);
    ok &= testMMMVersion2AudioSampleRoundTrip(outputDirectory);
    ok &= testLegacyMMMMetadataDefaults(outputDirectory);
    ok &= testVersion2LegacyAudioHintCompatibility(outputDirectory);
    ok &= testVersion2InvalidSampleTrackRelocation(outputDirectory);
    ok &= testOSUSingleAudioMigration(outputDirectory);
    ok &= testRMSingleAudioMigration(outputDirectory);
    ok &= testSingleAudioExporterRejection(outputDirectory);
    ok &= testMalodySaverDoesNotSynthesizeAudioSample(outputDirectory);
    ok &= testOSUSaverDoesNotSynthesizeAudioSample(outputDirectory);

    if ( ok ) {
        XINFO("Metadata compatibility tests passed.");
        return 0;
    }
    return 1;
}
