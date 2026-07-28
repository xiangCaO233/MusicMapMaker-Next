#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace fs = std::filesystem;

static int g_failed = 0;

#define TEST_ASSERT(cond, msg)       \
    do {                             \
        if ( !(cond) ) {             \
            XERROR("FAIL: {}", msg); \
            g_failed++;              \
            return;                  \
        }                            \
    } while ( 0 )

/// @brief 判断 Malody JSON 节点是否为自动采样对象。
/// @param node 待检查的 note 节点。
/// @return 数值 1 或兼容字符串 SOUND 时返回 true。
static bool isSoundNode(const json& node)
{
    if ( !node.contains("type") ) return false;
    if ( node["type"].is_string() ) {
        return node["type"].get<std::string>() == "SOUND";
    }
    return node["type"].is_number_integer() && node["type"].get<int>() == 1;
}

/// @brief 创建一个最小的可用 BeatMap，含一个 timing 点和基本元数据
static MMM::BeatMap makeMinimalBeatMap(int mode, int trackCount)
{
    MMM::BeatMap bm;
    MMM::Timing  t;
    t.m_timestamp             = 0.0;
    t.m_bpm                   = 120.0;
    t.m_beat_length           = 500.0;
    t.m_timingEffect          = MMM::TimingEffect::BPM;
    t.m_timingEffectParameter = 120.0;
    bm.m_timings.push_back(t);

    bm.m_baseMapMetadata.track_count     = trackCount;
    bm.m_baseMapMetadata.preference_bpm  = 120.0;
    bm.m_baseMapMetadata.title           = "EdgeCaseTest";
    bm.m_baseMapMetadata.artist          = "Test";
    bm.m_baseMapMetadata.author          = "Test";
    bm.m_baseMapMetadata.version         = "Test";
    bm.m_baseMapMetadata.song_file_hint  = fs::path("audio.ogg");
    bm.m_baseMapMetadata.main_cover_path = fs::path("cover.jpg");
    bm.m_baseMapMetadata.map_path        = fs::path("/tmp/test_edge.mc");
    bm.m_baseMapMetadata.bgm_track_count = 1;

    bm.m_metadata.map_properties[MMM::MapMetadataType::MALODY]["mode"] =
        std::to_string(mode);
    bm.m_metadata.map_properties[MMM::MapMetadataType::MALODY]["id"] = "0";

    MMM::AudioSampleEvent sample;
    sample.m_timestamp       = 0.0;
    sample.m_track           = static_cast<uint32_t>(trackCount);
    sample.m_audioResourceId = "audio.ogg";
    bm.m_audioSamples.push_back(sample);

    bm.sync();
    return bm;
}

/// @brief 保存、重新加载并返回 BeatMap
static MMM::BeatMap saveAndReload(const MMM::BeatMap& bm,
                                  const std::string&  tag)
{
    fs::path outPath =
        std::filesystem::temp_directory_path() / ("edge_" + tag + ".mc");

    if ( !bm.saveToFile(outPath) ) {
        XERROR("Failed to save {}", outPath.string());
        return {};
    }

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outPath);
    reloaded.sync();
    return reloaded;
}

void test_zero_length_hold_degrade_to_flick()
{
    XINFO("=== Test: Zero-length Hold + Flick → Flick (dir mode) ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    // 构建 Polyline: [Hold(dur=0, track=0), Flick(dtrack=+1, track=0)]
    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 0;

    MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
    h.m_type      = MMM::NoteType::HOLD;
    h.m_timestamp = 1000.0;
    h.m_track     = 0;
    h.m_duration  = 0.0;  // 零长度
    h.m_isSubNote = true;
    poly.m_subNotes.push_back(h);
    poly.m_subHolds.push_back(h);

    MMM::Flick& f = bm.m_noteData.flicks.emplace_back();
    f.m_type      = MMM::NoteType::FLICK;
    f.m_timestamp = 1000.0;
    f.m_track     = 0;
    f.m_dtrack    = 1;
    f.m_isSubNote = true;
    poly.m_subNotes.push_back(f);
    poly.m_subFlicks.push_back(f);

    bm.sync();
    auto reloaded = saveAndReload(bm, "zero_hold_flick");

    TEST_ASSERT(!reloaded.m_allNotes.empty(), "reloaded map should have notes");
    TEST_ASSERT(reloaded.m_noteData.flicks.size() == 1,
                "should have exactly 1 Flick");
    TEST_ASSERT(reloaded.m_noteData.polylines.empty(),
                "should have no Polylines");
    TEST_ASSERT(reloaded.m_noteData.holds.empty(), "should have no Holds");

    const auto& f2 = reloaded.m_noteData.flicks.front();
    TEST_ASSERT(f2.m_dtrack == 1, "Flick dtrack should be 1");
    TEST_ASSERT(!f2.m_isSubNote, "Flick should not be a sub-note");

    XINFO("PASS: Zero-length Hold+Flick degraded to single Flick with dir");
}

void test_multiple_zero_holds_same_flicks_merge()
{
    XINFO(
        "=== Test: Hold(0)+Flick(1)+Hold(0)+Flick(1)+Hold(0)+Flick(1) → "
        "single Flick(3) ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 0;

    // 三个零长度 Hold + 三个同向 Flick
    for ( int i = 0; i < 3; i++ ) {
        MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
        h.m_type      = MMM::NoteType::HOLD;
        h.m_timestamp = 1000.0;
        h.m_track     = i;  // track changes with each flick (after previous)
        h.m_duration  = 0.0;
        h.m_isSubNote = true;
        poly.m_subNotes.push_back(h);
        poly.m_subHolds.push_back(h);

        MMM::Flick& f = bm.m_noteData.flicks.emplace_back();
        f.m_type      = MMM::NoteType::FLICK;
        f.m_timestamp = 1000.0;
        f.m_track     = i;
        f.m_dtrack    = 1;
        f.m_isSubNote = true;
        poly.m_subNotes.push_back(f);
        poly.m_subFlicks.push_back(f);
    }

    bm.sync();
    auto reloaded = saveAndReload(bm, "multi_zero_hold_flick");

    TEST_ASSERT(!reloaded.m_allNotes.empty(), "reloaded map should have notes");
    TEST_ASSERT(reloaded.m_noteData.flicks.size() == 1,
                "should have exactly 1 Flick");
    TEST_ASSERT(reloaded.m_noteData.polylines.empty(),
                "should have no Polylines");
    TEST_ASSERT(reloaded.m_noteData.holds.empty(), "should have no Holds");

    const auto& f2 = reloaded.m_noteData.flicks.front();
    TEST_ASSERT(f2.m_dtrack == 3,
                "Flick dtrack should be 3 (merged from 3x+1)");
    TEST_ASSERT(!f2.m_isSubNote, "Flick should not be a sub-note");

    XINFO("PASS: Multiple zero-hold+flick merged into single Flick(3)");
}

void test_polyline_all_cleaned_degrade_to_note()
{
    XINFO("=== Test: All sub-notes cleaned → degrade to Note ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 0;

    // 只有零长度 Hold（无 Flick），全部被过滤掉
    for ( int i = 0; i < 3; i++ ) {
        MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
        h.m_type      = MMM::NoteType::HOLD;
        h.m_timestamp = 1000.0;
        h.m_track     = 0;
        h.m_duration  = 0.0;
        h.m_isSubNote = true;
        poly.m_subNotes.push_back(h);
        poly.m_subHolds.push_back(h);
    }

    bm.sync();
    auto reloaded = saveAndReload(bm, "all_cleaned_note");

    TEST_ASSERT(!reloaded.m_allNotes.empty(), "reloaded map should have notes");
    TEST_ASSERT(reloaded.m_noteData.notes.size() == 1,
                "should have exactly 1 Note (degraded from empty polyline)");
    TEST_ASSERT(reloaded.m_noteData.polylines.empty(),
                "should have no Polylines");
    TEST_ASSERT(reloaded.m_noteData.holds.empty(), "should have no Holds");
    TEST_ASSERT(reloaded.m_noteData.flicks.empty(), "should have no Flicks");

    XINFO("PASS: Empty polyline degraded to Note");
}

void test_key_mode_hold_uses_endbeat()
{
    XINFO("=== Test: Key mode Hold uses endbeat ===");

    auto bm = makeMinimalBeatMap(0 /*Key*/, 4);

    MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
    h.m_type      = MMM::NoteType::HOLD;
    h.m_timestamp = 1000.0;
    h.m_track     = 0;
    h.m_duration  = 500.0;
    h.m_isSubNote = false;

    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_key_hold_endbeat.mc";
    bm.saveToFile(outPath);

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta") && j["meta"].value("mode", -1) == 0,
                "mode should be 0 (Key)");
    TEST_ASSERT(j["meta"].value("free", -1) == 0,
                "free should be 0 in Key mode");

    auto gameNotes = json::array();
    for ( const auto& n : j["note"] ) {
        if ( isSoundNode(n) ) continue;
        gameNotes.push_back(n);
    }
    TEST_ASSERT(gameNotes.size() == 1, "should have 1 game note");
    TEST_ASSERT(gameNotes[0].contains("column"), "should have column");
    TEST_ASSERT(gameNotes[0].contains("endbeat"), "should have endbeat");
    TEST_ASSERT(!gameNotes[0].contains("seg"), "should NOT have seg");
    TEST_ASSERT(!gameNotes[0].contains("x"), "should NOT have x");

    XINFO("PASS: Key mode Hold correctly uses column + endbeat");
}

void test_slide_mode_saves_xw()
{
    XINFO("=== Test: Slide mode note uses x + w ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    MMM::Note& n  = bm.m_noteData.notes.emplace_back();
    n.m_type      = MMM::NoteType::NOTE;
    n.m_timestamp = 1000.0;
    n.m_track     = 1;

    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_slide_xw.mc";
    bm.saveToFile(outPath);

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta") && j["meta"].value("mode", -1) == 7,
                "mode should be 7 (Slide)");
    TEST_ASSERT(j["meta"].value("free", -1) == 1,
                "free should be 1 in Slide mode");

    auto gameNotes = json::array();
    for ( const auto& n2 : j["note"] ) {
        if ( isSoundNode(n2) ) continue;
        gameNotes.push_back(n2);
    }
    TEST_ASSERT(gameNotes.size() == 1, "should have 1 game note");
    TEST_ASSERT(gameNotes[0].contains("x"), "should have x");
    TEST_ASSERT(gameNotes[0].contains("w"), "should have w");
    TEST_ASSERT(!gameNotes[0].contains("column"), "should NOT have column");
    TEST_ASSERT(!gameNotes[0].contains("endbeat"), "should NOT have endbeat");

    XINFO("PASS: Slide mode note correctly uses x + w");
}

/// @brief 确认不支持的 Malody mode 会阻止导出。
void test_unsupported_malody_mode_rejected()
{
    XINFO("=== Test: Unsupported Malody mode rejected ===");

    auto bm = makeMinimalBeatMap(4 /*Catch*/, 4);

    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_unsupported_mode.mc";

    TEST_ASSERT(!bm.saveToFile(outPath),
                "mode 4 should be rejected by Malody exporter");

    XINFO("PASS: Unsupported Malody mode rejected");
}

/// @brief 确认 key 模式导出折线时只保留 Hold 子物件。
void test_key_mode_polyline_exports_key_fields()
{
    XINFO("=== Test: Key mode polyline exports key fields ===");

    auto bm = makeMinimalBeatMap(0 /*Key*/, 4);

    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 0;

    MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
    h.m_type      = MMM::NoteType::HOLD;
    h.m_timestamp = 1000.0;
    h.m_track     = 0;
    h.m_duration  = 500.0;
    h.m_isSubNote = true;
    poly.m_subNotes.push_back(h);
    poly.m_subHolds.push_back(h);

    MMM::Flick& f = bm.m_noteData.flicks.emplace_back();
    f.m_type      = MMM::NoteType::FLICK;
    f.m_timestamp = 1750.0;
    f.m_track     = 2;
    f.m_dtrack    = 1;
    f.m_isSubNote = true;
    poly.m_subNotes.push_back(f);
    poly.m_subFlicks.push_back(f);

    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_key_polyline.mc";
    TEST_ASSERT(bm.saveToFile(outPath), "key polyline map should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta") && j["meta"].value("mode", -1) == 0,
                "mode should be 0 (Key)");

    auto gameNotes = json::array();
    for ( const auto& n : j["note"] ) {
        if ( isSoundNode(n) ) continue;
        gameNotes.push_back(n);
    }

    TEST_ASSERT(gameNotes.size() == 1,
                "key polyline should only export subHold notes");

    bool hasHold = false;
    for ( const auto& note : gameNotes ) {
        TEST_ASSERT(note.contains("column"), "key note should have column");
        TEST_ASSERT(!note.contains("x"), "key note should not have x");
        TEST_ASSERT(!note.contains("w"), "key note should not have w");
        TEST_ASSERT(!note.contains("seg"), "key note should not have seg");
        TEST_ASSERT(!note.contains("dir"), "key note should not have dir");
        if ( note.contains("endbeat") ) {
            hasHold = true;
        }
    }

    TEST_ASSERT(hasHold, "flattened key polyline should keep hold endbeat");

    XINFO("PASS: Key mode polyline exports only key hold fields");
}

/// @brief 确认 key 模式导出普通 Flick 时作为单 Note 写出。
void test_key_mode_flick_exports_single_note()
{
    XINFO("=== Test: Key mode Flick exports single Note ===");

    auto bm = makeMinimalBeatMap(0 /*Key*/, 4);

    MMM::Flick& flick = bm.m_noteData.flicks.emplace_back();
    flick.m_type      = MMM::NoteType::FLICK;
    flick.m_timestamp = 1000.0;
    flick.m_track     = 1;
    flick.m_dtrack    = 2;

    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_key_flick.mc";
    TEST_ASSERT(bm.saveToFile(outPath), "key flick map should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    auto gameNotes = json::array();
    for ( const auto& n : j["note"] ) {
        if ( isSoundNode(n) ) continue;
        gameNotes.push_back(n);
    }

    TEST_ASSERT(gameNotes.size() == 1, "key flick should export one note");
    TEST_ASSERT(gameNotes[0].contains("column"),
                "key flick should have column");
    TEST_ASSERT(!gameNotes[0].contains("dir"), "key flick should not have dir");
    TEST_ASSERT(!gameNotes[0].contains("x"), "key flick should not have x");
    TEST_ASSERT(!gameNotes[0].contains("w"), "key flick should not have w");
    TEST_ASSERT(!gameNotes[0].contains("seg"), "key flick should not have seg");
    TEST_ASSERT(!gameNotes[0].contains("endbeat"),
                "key flick should not have endbeat");

    XINFO("PASS: Key mode Flick exports as single Note");
}

/// @brief 确认 Malody 自动采样对象使用数值 type=1 和绝对 BGM 轨道。
void test_audio_node_uses_canonical_fields()
{
    XINFO("=== Test: Audio node uses canonical fields ===");

    auto bm = makeMinimalBeatMap(0 /*Key*/, 4);

    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_audio_type.mc";
    TEST_ASSERT(bm.saveToFile(outPath), "canonical audio sample should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("note") && !j["note"].empty(),
                "note array should not be empty");
    const json* audioNode = nullptr;
    for ( const auto& node : j["note"] ) {
        if ( isSoundNode(node) ) {
            TEST_ASSERT(audioNode == nullptr,
                        "minimal map should export exactly one audio sample");
            audioNode = &node;
        }
    }
    TEST_ASSERT(audioNode != nullptr, "audio sample should be present");
    TEST_ASSERT((*audioNode)["type"].is_number_integer(),
                "audio sample type should be numeric");
    TEST_ASSERT((*audioNode)["type"].get<int>() == 1,
                "audio sample type should be 1");
    TEST_ASSERT(audioNode->value("sound", "") == "audio.ogg",
                "audio sample should keep its resource id");
    TEST_ASSERT(audioNode->value("x", -1) == 4,
                "first BGM track should immediately follow four key tracks");
    TEST_ASSERT(audioNode->value("offset", -1.0) == 0.0,
                "audio sample should keep zero offset");
    TEST_ASSERT(audioNode->value("vol", -1.0) == 100.0,
                "unit volume should serialize as 100");
    TEST_ASSERT(!audioNode->contains("column"),
                "audio sample must not use playable column");

    XINFO("PASS: Audio node uses canonical fields");
}

/// @brief 确认内部兼容 offset 元数据不会导出到 Malody meta。
void test_internal_offset_metadata_not_exported()
{
    XINFO("=== Test: Internal offset metadata not exported ===");

    auto  bm    = makeMinimalBeatMap(7 /*Slide*/, 4);
    auto& props = bm.m_metadata.map_properties[MMM::MapMetadataType::MALODY];
    props["initialDelay"]                = "123";
    props["audioOffset"]                 = "456";
    bm.m_audioSamples.front().m_offsetMs = -75;

    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_no_internal_meta.mc";
    TEST_ASSERT(bm.saveToFile(outPath),
                "map with internal metadata should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta"), "output should contain meta");
    TEST_ASSERT(!j["meta"].contains("initialDelay"),
                "initialDelay should not be exported to Malody meta");
    TEST_ASSERT(!j["meta"].contains("audioOffset"),
                "audioOffset should not be exported to Malody meta");
    const auto sampleIt =
        std::find_if(j["note"].begin(), j["note"].end(), isSoundNode);
    TEST_ASSERT(sampleIt != j["note"].end(),
                "explicit audio sample should remain present");
    TEST_ASSERT(sampleIt->value("offset", 0.0) == -75.0,
                "per-sample offset should not use legacy global metadata");

    XINFO("PASS: Internal offset metadata hidden from Malody meta");
}

/// @brief 确认空难度名导出 MC 时会写出可显示的默认 version。
void test_empty_version_exports_default_metadata()
{
    XINFO("=== Test: Empty Malody version exports default metadata ===");

    auto bm                      = makeMinimalBeatMap(7 /*Slide*/, 4);
    bm.m_baseMapMetadata.version = "";
    bm.m_baseMapMetadata.title   = "EmptyVersionTitle";
    bm.m_baseMapMetadata.artist  = "EmptyVersionArtist";
    fs::path outPath             = std::filesystem::temp_directory_path() /
                                   "edge_empty_version_metadata.mc";
    TEST_ASSERT(bm.saveToFile(outPath), "empty version map should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta"), "empty version output should contain meta");
    TEST_ASSERT(j["meta"].contains("version"),
                "empty version output should contain meta.version");
    TEST_ASSERT(j["meta"].value("version", "") == "default",
                "empty version should export as default");
    TEST_ASSERT(j["meta"].contains("song"),
                "empty version output should keep song metadata");
    TEST_ASSERT(j["meta"]["song"].value("title", "") == "EmptyVersionTitle",
                "empty version output should keep title metadata");

    XINFO("PASS: Empty Malody version exports default metadata");
}

/// @brief 确认 BGM 自动采样对象的 x 不参与玩家 key 数量推断。
void test_sound_track_does_not_expand_key_count()
{
    XINFO("=== Test: SOUND track does not expand key count ===");

    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_sound_track.mc";

    json  fileData;
    auto& meta         = fileData["meta"];
    meta["id"]         = 0;
    meta["creator"]    = "Test";
    meta["background"] = "";
    meta["cover"]      = "";
    meta["version"]    = "4K";
    meta["preview"]    = 0;
    meta["mode"]       = 0;
    meta["aimode"]     = "";

    auto& song        = meta["song"];
    song["title"]     = "SoundColumn";
    song["artist"]    = "Test";
    song["titleorg"]  = "";
    song["artistorg"] = "";
    song["file"]      = "audio.ogg";
    song["bpm"]       = 120.0;

    meta["mode_ext"]["column"]    = 4;
    meta["mode_ext"]["bar_begin"] = 0;

    json timing;
    timing["beat"]   = json::array({ 0, 0, 1 });
    timing["bpm"]    = 120.0;
    timing["delay"]  = 0.0;
    fileData["time"] = json::array({ timing });

    json gameNote;
    gameNote["beat"]   = json::array({ 1, 0, 1 });
    gameNote["column"] = 3;

    json soundNote;
    soundNote["beat"]   = json::array({ 0, 0, 1 });
    soundNote["x"]      = 8;
    soundNote["type"]   = 1;
    soundNote["sound"]  = "audio.ogg";
    soundNote["offset"] = 0;
    soundNote["vol"]    = 100;

    fileData["note"] = json::array({ gameNote, soundNote });

    std::ofstream ofs(outPath);
    TEST_ASSERT(ofs.good(), "should open temp Malody file");
    ofs << fileData.dump();
    ofs.close();

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outPath);
    reloaded.sync();

    TEST_ASSERT(reloaded.m_baseMapMetadata.track_count == 4,
                "SOUND x should not expand key count");
    TEST_ASSERT(reloaded.m_baseMapMetadata.bgm_track_count == 5,
                "x=8 after four key tracks should require five BGM tracks");
    TEST_ASSERT(reloaded.m_allNotes.size() == 1,
                "SOUND object should not become a playable note");
    TEST_ASSERT(reloaded.m_audioSamples.size() == 1,
                "SOUND object should become one automatic sample");
    TEST_ASSERT(reloaded.m_audioSamples.front().m_track == 8,
                "automatic sample should keep its absolute track");

    XINFO("PASS: SOUND track is separate from key count");
}

/// @brief 验证多个自动采样对象独立保留轨道、音量和有符号偏移。
void test_multiple_sound_objects_round_trip_without_global_shift()
{
    XINFO("=== Test: Multiple SOUND objects round trip independently ===");

    const fs::path sourcePath =
        std::filesystem::temp_directory_path() / "edge_multiple_sound.mc";
    const fs::path exportPath = std::filesystem::temp_directory_path() /
                                "edge_multiple_sound_export.mc";

    json  fileData;
    auto& meta         = fileData["meta"];
    meta["id"]         = 0;
    meta["creator"]    = "Test";
    meta["background"] = "";
    meta["cover"]      = "";
    meta["version"]    = "4K";
    meta["preview"]    = 0;
    meta["mode"]       = 0;
    meta["aimode"]     = "";
    meta["mode_ext"]   = { { "column", 4 }, { "bar_begin", 0 } };
    meta["song"]       = { { "title", "MultipleSound" }, { "artist", "Test" },
                           { "titleorg", "" },           { "artistorg", "" },
                           { "file", "stem.ogg" },       { "bpm", 120.0 } };
    fileData["time"]   = json::array({ { { "beat", json::array({ 0, 0, 1 }) },
                                         { "bpm", 120.0 },
                                         { "delay", 0.0 } } });

    json playableNote{ { "beat", json::array({ 4, 0, 1 }) },
                       { "column", 2 },
                       { "sound", "hit.wav" },
                       { "vol", 65 } };
    json earlyStem{ { "beat", json::array({ 1, 0, 1 }) },
                    { "type", 1 },
                    { "sound", "stem.ogg" },
                    { "offset", -125 },
                    { "x", 4 },
                    { "vol", 80 } };
    json delayedEffect{ { "beat", json::array({ 2, 0, 1 }) },
                        { "type", "SOUND" },
                        { "sound", "effect.wav" },
                        { "offset", 250 },
                        { "x", 5 },
                        { "vol", 35 } };
    json sameBeatLayer{ { "beat", json::array({ 2, 0, 1 }) },
                        { "type", 1 },
                        { "sound", "layer.wav" },
                        { "offset", 0 },
                        { "x", 5 },
                        { "vol", 100 } };
    fileData["note"] =
        json::array({ playableNote, earlyStem, delayedEffect, sameBeatLayer });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open multiple SOUND input");
    source << fileData.dump();
    source.close();

    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(loaded.m_timings.size() == 1,
                "multiple SOUND map should keep its timing");
    TEST_ASSERT(std::abs(loaded.m_timings.front().m_timestamp) < 1e-6,
                "sample offset must not shift the timing timeline");
    TEST_ASSERT(loaded.m_allNotes.size() == 1,
                "automatic samples must not enter playable note list");
    TEST_ASSERT(
        std::abs(loaded.m_allNotes.front().get().m_timestamp - 2000.0) < 1e-6,
        "sample offset must not shift playable note timestamps");
    const auto playableBinding =
        loaded.m_allNotes.front().get().getSampleBinding();
    TEST_ASSERT(playableBinding.has_value(),
                "playable note should keep its hit sample binding");
    TEST_ASSERT(playableBinding->m_audioResourceId == "hit.wav",
                "playable note should keep its hit sample resource");
    TEST_ASSERT(std::abs(playableBinding->m_volume - 0.65F) < 1e-6F,
                "playable note should keep its hit sample volume");
    TEST_ASSERT(loaded.m_audioSamples.size() == 3,
                "all automatic samples should load independently");
    TEST_ASSERT(loaded.m_baseMapMetadata.track_count == 4,
                "automatic samples must not expand playable track count");
    TEST_ASSERT(loaded.m_baseMapMetadata.bgm_track_count == 2,
                "x=4 and x=5 should create two BGM tracks");

    /// @brief 按音频资源标识查找已加载的自动采样对象。
    auto findSample =
        [&](const std::string& resourceId) -> const MMM::AudioSampleEvent* {
        const auto it =
            std::find_if(loaded.m_audioSamples.begin(),
                         loaded.m_audioSamples.end(),
                         [&](const MMM::AudioSampleEvent& sample) {
                             return sample.m_audioResourceId == resourceId;
                         });
        return it == loaded.m_audioSamples.end() ? nullptr : &*it;
    };

    const MMM::AudioSampleEvent* stem = findSample("stem.ogg");
    TEST_ASSERT(stem != nullptr, "main stem sample should load");
    TEST_ASSERT(std::abs(stem->m_timestamp - 500.0) < 1e-6,
                "main stem beat should remain its anchor timestamp");
    TEST_ASSERT(stem->m_offsetMs == -125,
                "negative sample offset should be retained");
    TEST_ASSERT(std::abs(stem->effectiveTimestamp() - 375.0) < 1e-6,
                "negative offset should advance only that sample");
    TEST_ASSERT(stem->m_track == 4,
                "main stem should remain on first BGM track");
    TEST_ASSERT(std::abs(stem->m_volume - 0.8F) < 1e-6F,
                "main stem volume should be normalized");

    const MMM::AudioSampleEvent* effect = findSample("effect.wav");
    TEST_ASSERT(effect != nullptr, "effect sample should load");
    TEST_ASSERT(std::abs(effect->m_timestamp - 1000.0) < 1e-6,
                "effect beat should remain its anchor timestamp");
    TEST_ASSERT(effect->m_offsetMs == 250,
                "positive sample offset should be retained");
    TEST_ASSERT(std::abs(effect->effectiveTimestamp() - 1250.0) < 1e-6,
                "positive offset should delay only that sample");
    TEST_ASSERT(effect->m_track == 5,
                "effect should remain on second BGM track");
    TEST_ASSERT(std::abs(effect->m_volume - 0.35F) < 1e-6F,
                "effect volume should be normalized");
    TEST_ASSERT(findSample("layer.wav") != nullptr,
                "same-beat sample must not be deduplicated");

    TEST_ASSERT(loaded.saveToFile(exportPath),
                "multiple automatic samples should export");
    std::ifstream exportedFile(exportPath);
    json          exported;
    exportedFile >> exported;
    size_t canonicalSampleCount = 0;
    for ( const auto& node : exported["note"] ) {
        if ( !isSoundNode(node) ) continue;
        ++canonicalSampleCount;
        TEST_ASSERT(
            node["type"].is_number_integer() && node["type"].get<int>() == 1,
            "all exported automatic samples should use numeric type 1");
        TEST_ASSERT(node.contains("x"),
                    "all exported automatic samples should keep BGM track x");
        TEST_ASSERT(!node.contains("column"),
                    "automatic samples must not use playable column");
    }
    TEST_ASSERT(canonicalSampleCount == 3,
                "export should preserve every automatic sample");

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    TEST_ASSERT(reloaded.m_audioSamples.size() == 3,
                "canonical Malody output should reload all samples");

    XINFO("PASS: Multiple SOUND objects round trip independently");
}

/// @brief 验证 time.delay 只延迟对应 Timing 锚点及其后续拍号映射。
void test_timing_delay_and_sample_offset_round_trip_independently()
{
    XINFO("=== Test: Timing delay and sample offset round trip ===");

    const fs::path sourcePath =
        std::filesystem::temp_directory_path() / "edge_timing_delay_source.mc";
    const fs::path exportPath =
        std::filesystem::temp_directory_path() / "edge_timing_delay_export.mc";

    json fileData;
    fileData["meta"] = { { "id", 0 },
                         { "creator", "Test" },
                         { "version", "4K" },
                         { "mode", 0 },
                         { "mode_ext",
                           { { "column", 4 }, { "bar_begin", 0 } } },
                         { "song",
                           { { "title", "TimingDelay" },
                             { "artist", "Test" },
                             { "file", "stem.ogg" },
                             { "bpm", 120.0 } } } };
    fileData["time"] = json::array({ { { "beat", json::array({ 0, 0, 1 }) },
                                       { "bpm", 120.0 },
                                       { "delay", 100.0 } },
                                     { { "beat", json::array({ 4, 0, 1 }) },
                                       { "bpm", 60.0 },
                                       { "delay", 200.0 } },
                                     { { "beat", json::array({ 4, 0, 1 }) },
                                       { "bpm", 60.0 },
                                       { "delay", 50.0 } } });
    fileData["note"] =
        json::array({ { { "beat", json::array({ 3, 0, 1 }) }, { "column", 0 } },
                      { { "beat", json::array({ 5, 0, 1 }) }, { "column", 1 } },
                      { { "beat", json::array({ 5, 0, 1 }) },
                        { "type", 1 },
                        { "sound", "effect.wav" },
                        { "offset", -75 },
                        { "x", 4 },
                        { "vol", 80 } } });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open timing delay input");
    source << fileData.dump();
    source.close();

    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(loaded.m_timings.size() == 3,
                "timing delay map should keep three BPM timings");
    TEST_ASSERT(std::abs(loaded.m_timings[0].m_timestamp - 100.0) < 1e-6,
                "first timing should apply its delay exactly once");
    TEST_ASSERT(std::abs(loaded.m_timings[1].m_timestamp - 2300.0) < 1e-6,
                "second timing should apply its 200ms delay once");
    TEST_ASSERT(std::abs(loaded.m_timings[2].m_timestamp - 2350.0) < 1e-6,
                "same-beat timings should accumulate delay in source order");
    TEST_ASSERT(loaded.m_noteData.notes.size() == 2,
                "timing delay map should keep two playable notes");

    const auto beforeDelay =
        std::find_if(loaded.m_noteData.notes.begin(),
                     loaded.m_noteData.notes.end(),
                     [](const MMM::Note& note) { return note.m_track == 0; });
    const auto afterDelay =
        std::find_if(loaded.m_noteData.notes.begin(),
                     loaded.m_noteData.notes.end(),
                     [](const MMM::Note& note) { return note.m_track == 1; });
    TEST_ASSERT(beforeDelay != loaded.m_noteData.notes.end(),
                "note before delayed timing should load");
    TEST_ASSERT(afterDelay != loaded.m_noteData.notes.end(),
                "note after delayed timing should load");
    TEST_ASSERT(std::abs(beforeDelay->m_timestamp - 1600.0) < 1e-6,
                "later timing delay must not shift earlier notes");
    TEST_ASSERT(std::abs(afterDelay->m_timestamp - 3350.0) < 1e-6,
                "later notes should use the delayed BPM anchor");

    TEST_ASSERT(loaded.m_audioSamples.size() == 1,
                "timing delay map should keep one automatic sample");
    const auto& sample = loaded.m_audioSamples.front();
    TEST_ASSERT(std::abs(sample.m_timestamp - 3350.0) < 1e-6,
                "sample anchor should follow only the timing delay");
    TEST_ASSERT(sample.m_offsetMs == -75,
                "sample should keep its independent signed offset");
    TEST_ASSERT(std::abs(sample.effectiveTimestamp() - 3275.0) < 1e-6,
                "sample offset should affect only effective playback time");

    // 强制导出器从内部时间戳重算 beat，覆盖 delay 的逆向换算路径。
    for ( auto& timing : loaded.m_timings ) {
        timing.m_metadata.timing_properties[MMM::TimingMetadataType::MALODY]
            .erase("beat");
    }
    for ( auto& note : loaded.m_noteData.notes ) {
        note.m_metadata.note_properties[MMM::NoteMetadataType::MALODY].erase(
            "beat");
    }
    loaded.m_audioSamples.front()
        .m_metadata.sample_properties[MMM::SampleMetadataType::MALODY]
        .erase("beat");

    TEST_ASSERT(loaded.saveToFile(exportPath),
                "timing delay map should export");
    std::ifstream exportedFile(exportPath);
    json          exported;
    exportedFile >> exported;

    TEST_ASSERT(exported["time"].size() == 3,
                "export should keep three BPM timings");
    TEST_ASSERT(exported["time"][0]["beat"] == json::array({ 0, 0, 1 }),
                "first delayed timing should convert back to beat 0");
    TEST_ASSERT(exported["time"][0].value("delay", 0.0) == 100.0,
                "first timing delay should survive export");
    TEST_ASSERT(exported["time"][1]["beat"] == json::array({ 4, 0, 1 }),
                "delayed timing timestamp should convert back to beat 4");
    TEST_ASSERT(exported["time"][1].value("delay", 0.0) == 200.0,
                "timing delay should survive export");
    TEST_ASSERT(exported["time"][2]["beat"] == json::array({ 4, 0, 1 }),
                "same-beat timing should convert back to beat 4");
    TEST_ASSERT(exported["time"][2].value("delay", 0.0) == 50.0,
                "same-beat timing delay should survive export");

    const auto exportedSample = std::find_if(
        exported["note"].begin(), exported["note"].end(), isSoundNode);
    TEST_ASSERT(exportedSample != exported["note"].end(),
                "export should keep the automatic sample");
    TEST_ASSERT((*exportedSample)["beat"] == json::array({ 5, 0, 1 }),
                "sample anchor should convert back to beat 5");
    TEST_ASSERT(exportedSample->value("offset", 0) == -75,
                "sample offset should survive export independently");

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    reloaded.sync();
    TEST_ASSERT(reloaded.m_timings.size() == 3,
                "round trip should keep three timings");
    TEST_ASSERT(std::abs(reloaded.m_timings[0].m_timestamp - 100.0) < 1e-6,
                "round trip should apply first timing delay once");
    TEST_ASSERT(std::abs(reloaded.m_timings[1].m_timestamp - 2300.0) < 1e-6,
                "round trip should keep delayed timing timestamp");
    TEST_ASSERT(std::abs(reloaded.m_timings[2].m_timestamp - 2350.0) < 1e-6,
                "round trip should keep same-beat timing order");
    TEST_ASSERT(reloaded.m_noteData.notes.size() == 2,
                "round trip should keep both playable notes");
    const auto reloadedBeforeDelay =
        std::find_if(reloaded.m_noteData.notes.begin(),
                     reloaded.m_noteData.notes.end(),
                     [](const MMM::Note& note) { return note.m_track == 0; });
    const auto reloadedAfterDelay =
        std::find_if(reloaded.m_noteData.notes.begin(),
                     reloaded.m_noteData.notes.end(),
                     [](const MMM::Note& note) { return note.m_track == 1; });
    TEST_ASSERT(reloadedBeforeDelay != reloaded.m_noteData.notes.end() &&
                    std::abs(reloadedBeforeDelay->m_timestamp - 1600.0) < 1e-6,
                "round trip must not shift the note before delayed timing");
    TEST_ASSERT(reloadedAfterDelay != reloaded.m_noteData.notes.end() &&
                    std::abs(reloadedAfterDelay->m_timestamp - 3350.0) < 1e-6,
                "round trip should keep the note after delayed timing");
    TEST_ASSERT(reloaded.m_audioSamples.size() == 1,
                "round trip should keep one automatic sample");
    TEST_ASSERT(
        std::abs(reloaded.m_audioSamples.front().m_timestamp - 3350.0) < 1e-6,
        "round trip should keep sample anchor separate from its offset");
    TEST_ASSERT(reloaded.m_audioSamples.front().m_offsetMs == -75,
                "round trip should keep signed sample offset");

    XINFO("PASS: Timing delay and sample offset round trip independently");
}

/// @brief 验证非法采样轨道归一化，且 song.file 不夺走同名玩家音效语义。
void test_invalid_sample_track_and_song_hint_conflict()
{
    XINFO("=== Test: Invalid sample x and song hint conflict ===");

    const fs::path sourcePath =
        std::filesystem::temp_directory_path() / "edge_invalid_sample_track.mc";
    const fs::path exportPath = std::filesystem::temp_directory_path() /
                                "edge_invalid_sample_track_export.mc";

    json fileData;
    fileData["meta"] = { { "id", 0 },
                         { "creator", "Test" },
                         { "version", "4K" },
                         { "mode", 0 },
                         { "mode_ext",
                           { { "column", 4 }, { "bar_begin", 0 } } },
                         { "song",
                           { { "title", "HintConflict" },
                             { "artist", "Test" },
                             { "file", "audio/shared.wav" },
                             { "bpm", 120.0 } } } };
    fileData["time"] = json::array(
        { { { "beat", json::array({ 0, 0, 1 }) }, { "bpm", 120.0 } } });
    fileData["note"] = json::array({ { { "beat", json::array({ 1, 0, 1 }) },
                                       { "column", 0 },
                                       { "type", 0 },
                                       { "sound", "audio/shared.wav" },
                                       { "vol", 55 } },
                                     { { "beat", json::array({ 2, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "effect.wav" },
                                       { "offset", -20 },
                                       { "x", 2 },
                                       { "vol", 90 } } });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open invalid sample input");
    source << fileData.dump();
    source.close();

    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(
        loaded.m_baseMapMetadata.song_file_hint == fs::path("audio/shared.wav"),
        "song.file should remain a non-scheduling hint");
    TEST_ASSERT(loaded.m_allNotes.size() == 1,
                "same-name song hint should not consume playable note");
    const auto binding = loaded.m_allNotes.front().get().getSampleBinding();
    TEST_ASSERT(binding.has_value() &&
                    binding->m_audioResourceId == "audio/shared.wav" &&
                    std::abs(binding->m_volume - 0.55F) < 1e-6F,
                "same-name playable sound should remain an effect binding");
    TEST_ASSERT(loaded.m_audioSamples.size() == 1,
                "song hint must not synthesize an extra automatic sample");
    const auto& sample = loaded.m_audioSamples.front();
    TEST_ASSERT(sample.m_track == 4,
                "invalid x inside key area should use first BGM track");
    TEST_ASSERT(sample.m_metadata.getValue<std::string>(
                    MMM::SampleMetadataType::MALODY, "original_x") == "2",
                "invalid source x should remain available for diagnostics");

    TEST_ASSERT(loaded.saveToFile(exportPath),
                "normalized sample map should export");
    std::ifstream exportedFile(exportPath);
    json          exported;
    exportedFile >> exported;
    TEST_ASSERT(
        exported["meta"]["song"].value("file", "") == "audio/shared.wav",
        "explicit song.file hint should retain its relative path");

    size_t soundCount    = 0;
    size_t playableCount = 0;
    for ( const auto& node : exported["note"] ) {
        if ( isSoundNode(node) ) {
            ++soundCount;
            TEST_ASSERT(node.value("x", -1) == 4,
                        "normalized sample should export first BGM track");
            continue;
        }
        ++playableCount;
        TEST_ASSERT(!node.contains("type"),
                    "canonical playable Note must not contain type");
        TEST_ASSERT(node.value("sound", "") == "audio/shared.wav",
                    "playable effect should survive song hint conflict");
    }
    TEST_ASSERT(soundCount == 1 && playableCount == 1,
                "hint conflict output should keep one sample and one Note");

    XINFO("PASS: Invalid sample x and song hint conflict");
}

/// @brief 确认近空 Malody 谱面中的字符串 BPM 可以无异常加载。
void testStringBpmInNearlyEmptyMapLoads()
{
    XINFO("=== Test: Nearly empty Malody map with string BPM loads ===");

    const fs::path outPath = std::filesystem::temp_directory_path() /
                             "edge_nearly_empty_string_bpm.mc";

    json  fileData;
    auto& meta             = fileData["meta"];
    meta["$ver"]           = 0;
    meta["creator"]        = "Test";
    meta["background"]     = "background.png";
    meta["version"]        = "4K HD";
    meta["id"]             = 0;
    meta["mode"]           = 7;
    meta["mode_ext"]       = json::object();
    meta["song"]["title"]  = "NearlyEmpty";
    meta["song"]["artist"] = "Test";

    json timing;
    timing["beat"]     = json::array({ 0, 0, 1 });
    timing["bpm"]      = "234";
    fileData["time"]   = json::array({ timing });
    fileData["effect"] = json::array();

    json playableNote;
    playableNote["beat"] = json::array({ 0, 0, 4 });
    playableNote["x"]    = 32;

    json soundNote;
    soundNote["beat"]  = json::array({ 0, 0, 1 });
    soundNote["sound"] = "audio.mp3";
    soundNote["type"]  = 1;
    fileData["note"]   = json::array({ playableNote, soundNote });

    std::ofstream ofs(outPath);
    TEST_ASSERT(ofs.good(), "should open string BPM temp Malody file");
    ofs << fileData.dump();
    ofs.close();

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outPath);

    TEST_ASSERT(reloaded.m_timings.size() == 1,
                "string BPM map should have one timing");
    TEST_ASSERT(reloaded.m_timings.front().m_bpm == 234.0,
                "string BPM should parse as 234");
    TEST_ASSERT(reloaded.m_baseMapMetadata.preference_bpm == 234.0,
                "string BPM should become preferred BPM");
    TEST_ASSERT(reloaded.m_allNotes.size() == 1,
                "SOUND node should not become a playable note");

    XINFO("PASS: Nearly empty Malody map with string BPM loaded");
}

/// @brief 确认只有元数据且缺少 time、effect、note 段的 Malody 谱面可加载。
void testMetadataOnlyMapLoadsWithDefaults()
{
    XINFO("=== Test: Metadata-only Malody map loads with defaults ===");

    const fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_metadata_only.mc";

    json  fileData;
    auto& meta             = fileData["meta"];
    meta["$ver"]           = 0;
    meta["creator"]        = "Test";
    meta["version"]        = "Empty";
    meta["mode"]           = 7;
    meta["mode_ext"]       = json::object();
    meta["song"]["title"]  = "MetadataOnly";
    meta["song"]["artist"] = "Test";

    std::ofstream ofs(outPath);
    TEST_ASSERT(ofs.good(), "should open metadata-only temp Malody file");
    ofs << fileData.dump();
    ofs.close();

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outPath);

    TEST_ASSERT(reloaded.m_allNotes.empty(),
                "metadata-only map should have no notes");
    TEST_ASSERT(reloaded.m_timings.size() == 1,
                "metadata-only map should receive a default timing");
    TEST_ASSERT(reloaded.m_timings.front().m_bpm == 120.0,
                "metadata-only map should use the default BPM");
    TEST_ASSERT(reloaded.m_baseMapMetadata.track_count == 4,
                "metadata-only map should use four default tracks");

    XINFO("PASS: Metadata-only Malody map loaded with defaults");
}

void test_original_structure_not_leaked()
{
    XINFO("=== Test: original_structure key not leaked into output ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 0;

    MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
    h.m_type      = MMM::NoteType::HOLD;
    h.m_timestamp = 1000.0;
    h.m_track     = 0;
    h.m_duration  = 500.0;
    h.m_isSubNote = true;
    poly.m_subNotes.push_back(h);
    poly.m_subHolds.push_back(h);

    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_no_leak.mc";
    bm.saveToFile(outPath);

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    bool leaked = false;
    for ( const auto& n : j["note"] ) {
        if ( n.contains("original_structure") ||
             n.contains("original_structure_flick") ) {
            leaked = true;
            XERROR("LEAK: original_structure found in item: {}",
                   n.dump().substr(0, 120));
        }
        if ( n.contains("seg") ) {
            for ( const auto& s : n["seg"] ) {
                if ( s.contains("original_structure") ||
                     s.contains("original_structure_flick") ) {
                    leaked = true;
                    XERROR("LEAK: original_structure in seg: {}",
                           s.dump().substr(0, 120));
                }
            }
        }
    }
    TEST_ASSERT(!leaked, "original_structure should not appear in output JSON");

    XINFO("PASS: No original_structure leaked");
}

void test_hold_stay_at_head_creates_valid_seg()
{
    XINFO("=== Test: Hold staying at head position → valid seg ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 1;

    // 单段 Hold 在 head 位置停留 0.5 秒
    MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
    h.m_type      = MMM::NoteType::HOLD;
    h.m_timestamp = 1000.0;
    h.m_track     = 1;
    h.m_duration  = 125.0;  // 0.5 beats at 120bpm = 250ms → ~0.5 beats
    h.m_isSubNote = true;
    poly.m_subNotes.push_back(h);
    poly.m_subHolds.push_back(h);

    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_hold_stay.mc";
    bm.saveToFile(outPath);

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta") && j["meta"].value("mode", -1) == 7,
                "mode should be 7");

    auto gameNotes = json::array();
    for ( const auto& n : j["note"] ) {
        if ( isSoundNode(n) ) continue;
        gameNotes.push_back(n);
    }
    TEST_ASSERT(gameNotes.size() == 1, "should have 1 game note");
    TEST_ASSERT(gameNotes[0].contains("seg"), "should have seg");
    TEST_ASSERT(gameNotes[0]["seg"].is_array(), "seg should be array");
    TEST_ASSERT(gameNotes[0]["seg"].size() > 0,
                "seg should not be empty (has a valid hold)");

    XINFO("PASS: Hold stay produces valid seg with correct beat");
}

/// @brief 验证 Malody 导出拒绝无法放入 seg 的子节点采样绑定。
void testPolylineSubnoteSampleBindingRejected()
{
    XINFO("=== Test: Polyline sub-note sample binding is rejected ===");

    auto           bm   = makeMinimalBeatMap(7 /*Slide*/, 4);
    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 1;

    MMM::Hold& hold  = bm.m_noteData.holds.emplace_back();
    hold.m_type      = MMM::NoteType::HOLD;
    hold.m_timestamp = 1000.0;
    hold.m_track     = 1;
    hold.m_duration  = 500.0;
    hold.m_isSubNote = true;
    hold.setSampleBinding({ "segment.wav", 0.45F });
    poly.m_subNotes.push_back(hold);
    poly.m_subHolds.push_back(hold);
    bm.sync();

    const fs::path  outputPath = std::filesystem::temp_directory_path() /
                                 "edge_polyline_bound_subnote.mc";
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);

    TEST_ASSERT(!bm.saveToFile(outputPath),
                "Malody saver should reject a bound Polyline sub-note");
    TEST_ASSERT(!std::filesystem::exists(outputPath),
                "rejected Polyline export should not leave a partial file");
    XINFO("PASS: Bound Polyline sub-note rejected without partial file");
}

int main()
{
    XINFO("========================================");
    XINFO("  Malody Edge Case Tests");
    XINFO("========================================");

    test_zero_length_hold_degrade_to_flick();
    test_multiple_zero_holds_same_flicks_merge();
    test_polyline_all_cleaned_degrade_to_note();
    test_key_mode_hold_uses_endbeat();
    test_slide_mode_saves_xw();
    test_unsupported_malody_mode_rejected();
    test_key_mode_polyline_exports_key_fields();
    test_key_mode_flick_exports_single_note();
    test_audio_node_uses_canonical_fields();
    test_internal_offset_metadata_not_exported();
    test_empty_version_exports_default_metadata();
    test_sound_track_does_not_expand_key_count();
    test_multiple_sound_objects_round_trip_without_global_shift();
    test_timing_delay_and_sample_offset_round_trip_independently();
    test_invalid_sample_track_and_song_hint_conflict();
    testStringBpmInNearlyEmptyMapLoads();
    testMetadataOnlyMapLoadsWithDefaults();
    test_original_structure_not_leaked();
    test_hold_stay_at_head_creates_valid_seg();
    testPolylineSubnoteSampleBindingRejected();

    XINFO("========================================");
    if ( g_failed == 0 ) {
        XINFO("  ALL Malody Edge Case Tests PASSED");
        return 0;
    } else {
        XERROR("  {} Malody Edge Case Tests FAILED", g_failed);
        return 1;
    }
}
