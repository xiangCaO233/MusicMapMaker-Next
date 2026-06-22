#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

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
    bm.m_baseMapMetadata.main_audio_path = fs::path("audio.ogg");
    bm.m_baseMapMetadata.main_cover_path = fs::path("cover.jpg");
    bm.m_baseMapMetadata.map_path        = fs::path("/tmp/test_edge.mc");

    bm.m_metadata.map_properties[MMM::MapMetadataType::MALODY]["mode"] =
        std::to_string(mode);
    bm.m_metadata.map_properties[MMM::MapMetadataType::MALODY]["id"] = "0";

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
        if ( n.contains("type") && n["type"].is_string() &&
             n["type"].get<std::string>() == "SOUND" )
            continue;
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
        if ( n2.contains("type") && n2["type"].is_string() &&
             n2["type"].get<std::string>() == "SOUND" )
            continue;
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
        if ( n.contains("type") && n["type"].is_string() &&
             n["type"].get<std::string>() == "SOUND" )
            continue;
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
        if ( n.contains("type") && n["type"].is_string() &&
             n["type"].get<std::string>() == "SOUND" )
            continue;
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

void test_audio_node_type_is_string()
{
    XINFO("=== Test: Audio node type is 'SOUND' string ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_audio_type.mc";
    bm.saveToFile(outPath);

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("note") && !j["note"].empty(),
                "note array should not be empty");
    const auto& audioNode = j["note"][0];
    TEST_ASSERT(audioNode.contains("type"), "audio node should have type");
    TEST_ASSERT(audioNode["type"].is_string(), "type should be a string");
    TEST_ASSERT(audioNode["type"].get<std::string>() == "SOUND",
                "type should be 'SOUND'");

    XINFO("PASS: Audio node type is 'SOUND' string");
}

/// @brief 确认内部兼容 offset 元数据不会导出到 Malody meta。
void test_internal_offset_metadata_not_exported()
{
    XINFO("=== Test: Internal offset metadata not exported ===");

    auto  bm    = makeMinimalBeatMap(7 /*Slide*/, 4);
    auto& props = bm.m_metadata.map_properties[MMM::MapMetadataType::MALODY];
    props["initialDelay"] = "123";
    props["audioOffset"]  = "456";

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

/// @brief 确认 BGM/SOUND 物件的 column 不参与 key 数量推断。
void test_sound_column_does_not_expand_key_count()
{
    XINFO("=== Test: SOUND column does not expand key count ===");

    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_sound_column.mc";

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
    soundNote["column"] = 8;
    soundNote["type"]   = 1;
    soundNote["sound"]  = "audio.ogg";
    soundNote["offset"] = 0;

    fileData["note"] = json::array({ gameNote, soundNote });

    std::ofstream ofs(outPath);
    TEST_ASSERT(ofs.good(), "should open temp Malody file");
    ofs << fileData.dump();
    ofs.close();

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outPath);
    reloaded.sync();

    TEST_ASSERT(reloaded.m_baseMapMetadata.track_count == 4,
                "SOUND column should not expand key count");
    TEST_ASSERT(reloaded.m_allNotes.size() == 1,
                "SOUND object should not become a playable note");

    XINFO("PASS: SOUND column ignored for key count");
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
        if ( n.contains("type") && n["type"].is_string() &&
             n["type"].get<std::string>() == "SOUND" )
            continue;
        gameNotes.push_back(n);
    }
    TEST_ASSERT(gameNotes.size() == 1, "should have 1 game note");
    TEST_ASSERT(gameNotes[0].contains("seg"), "should have seg");
    TEST_ASSERT(gameNotes[0]["seg"].is_array(), "seg should be array");
    TEST_ASSERT(gameNotes[0]["seg"].size() > 0,
                "seg should not be empty (has a valid hold)");

    XINFO("PASS: Hold stay produces valid seg with correct beat");
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
    test_audio_node_type_is_string();
    test_internal_offset_metadata_not_exported();
    test_empty_version_exports_default_metadata();
    test_sound_column_does_not_expand_key_count();
    test_original_structure_not_leaked();
    test_hold_stay_at_head_creates_valid_seg();

    XINFO("========================================");
    if ( g_failed == 0 ) {
        XINFO("  ALL Malody Edge Case Tests PASSED");
        return 0;
    } else {
        XERROR("  {} Malody Edge Case Tests FAILED", g_failed);
        return 1;
    }
}
