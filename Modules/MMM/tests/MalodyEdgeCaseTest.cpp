#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
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

/// @brief 确认 7K、8K Slide 写出遵循 Rhythm Master 皮肤的分轨与宽度规则。
void test_slide_mode_7k_8k_uses_skin_compatible_layout()
{
    XINFO("=== Test: Slide 7K/8K uses skin-compatible layout ===");

    struct SlideLayoutCase {
        int trackCount;
        int noteWidth;
        int flickWidth;
        int noteX;
        int flickX;
        int segmentOffset;
    };

    constexpr std::array<SlideLayoutCase, 2> cases{
        SlideLayoutCase{ 7, 30, 36, 55, 18, 182 },
        SlideLayoutCase{ 8, 20, 27, 48, 16, 192 },
    };

    for ( const auto& testCase : cases ) {
        auto bm = makeMinimalBeatMap(7 /*Slide*/, testCase.trackCount);

        MMM::Note& note  = bm.m_noteData.notes.emplace_back();
        note.m_type      = MMM::NoteType::NOTE;
        note.m_timestamp = 1000.0;
        note.m_track     = 1;

        MMM::Flick& flick = bm.m_noteData.flicks.emplace_back();
        flick.m_type      = MMM::NoteType::FLICK;
        flick.m_timestamp = 1250.0;
        flick.m_track     = 0;
        flick.m_dtrack    = testCase.trackCount - 1;

        MMM::Polyline& polyline = bm.m_noteData.polylines.emplace_back();
        polyline.m_type         = MMM::NoteType::POLYLINE;
        polyline.m_timestamp    = 1500.0;
        polyline.m_track        = 1;

        MMM::Flick& subFlick = bm.m_noteData.flicks.emplace_back();
        subFlick.m_type      = MMM::NoteType::FLICK;
        subFlick.m_timestamp = 1750.0;
        subFlick.m_track     = 1;
        subFlick.m_dtrack    = testCase.trackCount - 2;
        subFlick.m_isSubNote = true;
        polyline.m_subNotes.push_back(subFlick);
        polyline.m_subFlicks.push_back(subFlick);

        bm.sync();
        const fs::path outPath =
            std::filesystem::temp_directory_path() /
            ("edge_slide_" + std::to_string(testCase.trackCount) + "k.mc");
        TEST_ASSERT(bm.saveToFile(outPath), "7K/8K Slide map should save");

        std::ifstream ifs(outPath);
        json          document;
        ifs >> document;

        const json* noteNode     = nullptr;
        const json* flickNode    = nullptr;
        const json* polylineNode = nullptr;
        for ( const auto& node : document["note"] ) {
            if ( isSoundNode(node) ) continue;
            if ( node.contains("dir") ) {
                flickNode = &node;
            } else if ( node.contains("seg") ) {
                polylineNode = &node;
            } else {
                noteNode = &node;
            }
        }

        TEST_ASSERT(noteNode != nullptr && flickNode != nullptr &&
                        polylineNode != nullptr,
                    "7K/8K Slide output should keep all note forms");
        TEST_ASSERT(
            noteNode->value("x", -1) == testCase.noteX &&
                noteNode->value("w", -1) == testCase.noteWidth,
            "plain note should use the skin-compatible center and width");
        TEST_ASSERT(
            flickNode->value("x", -1) == testCase.flickX &&
                flickNode->value("w", -1) == testCase.flickWidth,
            "Flick width should preserve its encoded cross-lane distance");
        TEST_ASSERT(
            polylineNode->value("x", -1) == testCase.noteX &&
                polylineNode->value("w", -1) == testCase.noteWidth,
            "Polyline root should stay inside the skin key-width range");
        TEST_ASSERT((*polylineNode)["seg"].size() == 1 &&
                        (*polylineNode)["seg"][0].value("x", 0) ==
                            testCase.segmentOffset,
                    "Polyline segment should land on the target lane center");

        const auto reloaded     = MMM::BeatMap::loadFromFile(outPath);
        const bool decodedFlick = std::any_of(
            reloaded.m_noteData.flicks.begin(),
            reloaded.m_noteData.flicks.end(),
            [&](const MMM::Flick& loadedFlick) {
                return !loadedFlick.m_isSubNote &&
                       std::abs(loadedFlick.m_timestamp - 1250.0) < 1e-5 &&
                       loadedFlick.m_dtrack == testCase.trackCount - 1;
            });
        TEST_ASSERT(decodedFlick,
                    "7K/8K Flick distance should survive MC round trip");

        std::error_code removeError;
        std::filesystem::remove(outPath, removeError);
    }

    XINFO("PASS: Slide 7K/8K layout matches the skin rules");
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

/// @brief 确认 Malody Key 自动采样对象使用数值 type=1 和绝对 BGM 轨道。
void testKeyAudioNodeUsesNumericType()
{
    XINFO("=== Test: Key audio node uses numeric type ===");

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
    TEST_ASSERT(audioNode->value("vol", -1.0) == 0.0,
                "unit volume should serialize as neutral gain 0");
    TEST_ASSERT(!audioNode->contains("column"),
                "audio sample must not use playable column");

    XINFO("PASS: Key audio node uses numeric type");
}

/// @brief 确认 Malody Slide 主音轨使用游戏可识别的字符串 SOUND 类型。
void testSlideAudioNodeUsesSoundType()
{
    XINFO("=== Test: Slide audio node uses SOUND type ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    const fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_slide_audio_type.mc";
    TEST_ASSERT(bm.saveToFile(outPath), "slide audio sample should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("note") && !j["note"].empty(),
                "slide note array should not be empty");
    const auto audioNode =
        std::find_if(j["note"].begin(), j["note"].end(), isSoundNode);
    TEST_ASSERT(audioNode != j["note"].end(),
                "slide audio sample should be present");
    TEST_ASSERT((*audioNode)["type"].is_string(),
                "slide audio sample type should be a string");
    TEST_ASSERT((*audioNode)["type"].get<std::string>() == "SOUND",
                "slide audio sample type should be SOUND");
    TEST_ASSERT(audioNode->value("sound", "") == "audio.ogg",
                "slide audio sample should keep its resource id");
    TEST_ASSERT(!audioNode->contains("x"),
                "slide audio sample should not export x");

    XINFO("PASS: Slide audio node uses SOUND type");
}

/// @brief 确认内部兼容 offset 元数据不会导出到 Malody meta。
void test_internal_offset_metadata_not_exported()
{
    XINFO("=== Test: Internal offset metadata not exported ===");

    auto  bm    = makeMinimalBeatMap(7 /*Slide*/, 4);
    auto& props = bm.m_metadata.map_properties[MMM::MapMetadataType::MALODY];
    props["initialDelay"]                       = "123";
    props["audioOffset"]                        = "456";
    bm.m_audioSamples.front().m_audioResourceId = "effect.ogg";
    bm.m_audioSamples.front().m_offsetMs        = -75;

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
    soundNote["vol"]    = 0;

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
                       { "vol", -35 } };
    json earlyStem{ { "beat", json::array({ 1, 0, 1 }) },
                    { "type", 1.0 },
                    { "sound", "stem.ogg" },
                    { "offset", -125 },
                    { "x", 4 },
                    { "vol", -20 } };
    json delayedEffect{ { "beat", json::array({ 2, 0, 1 }) },
                        { "type", "SOUND" },
                        { "sound", "effect.wav" },
                        { "offset", 250 },
                        { "x", 5 },
                        { "vol", -65 } };
    json sameBeatLayer{ { "beat", json::array({ 2, 0, 1 }) },
                        { "type", 1 },
                        { "sound", "layer.wav" },
                        { "offset", 0 },
                        { "x", 5 },
                        { "vol", 16 } };
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
    const MMM::AudioSampleEvent* layer = findSample("layer.wav");
    TEST_ASSERT(layer != nullptr, "same-beat sample must not be deduplicated");
    TEST_ASSERT(std::abs(layer->m_volume - 1.16F) < 1e-6F,
                "positive Malody gain should increase internal volume");

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
        const std::string sound = node.value("sound", "");
        if ( sound == "stem.ogg" ) {
            TEST_ASSERT(node.value("vol", 0) == -20,
                        "80% internal volume should export as -20 gain");
        } else if ( sound == "effect.wav" ) {
            TEST_ASSERT(node.value("vol", 0) == -65,
                        "35% internal volume should export as -65 gain");
        } else if ( sound == "layer.wav" ) {
            TEST_ASSERT(node.value("vol", 0) == 16,
                        "116% internal volume should export as +16 gain");
        }
    }
    TEST_ASSERT(canonicalSampleCount == 3,
                "export should preserve every automatic sample");
    const auto playableOutput = std::find_if(
        exported["note"].begin(), exported["note"].end(), [](const json& node) {
            return node.value("sound", "") == "hit.wav";
        });
    TEST_ASSERT(playableOutput != exported["note"].end() &&
                    playableOutput->value("vol", 0) == -35,
                "65% bound-note volume should export as -35 gain");

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    TEST_ASSERT(reloaded.m_audioSamples.size() == 3,
                "canonical Malody output should reload all samples");

    XINFO("PASS: Multiple SOUND objects round trip independently");
}

/// @brief 验证未与主采样配对的 delay 只延迟对应 Timing 锚点。
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
                "unpaired first timing should keep its positive delay");
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
                "round trip should keep the unpaired first timing");
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

/// @brief 验证非 Malody 谱面的前导时间导出为拍轴 delay 和采样补偿。
void test_non_malody_lead_in_exports_timing_origin_and_audio_compensation()
{
    XINFO("=== Test: Non-Malody lead-in exports paired note phase ===");

    constexpr double LEAD_IN_MS = 237.0;
    for ( const int mode : { 0, 7 } ) {
        auto beatMap                          = makeMinimalBeatMap(mode, 4);
        beatMap.m_timings.front().m_timestamp = LEAD_IN_MS;

        MMM::Note& note  = beatMap.m_noteData.notes.emplace_back();
        note.m_type      = MMM::NoteType::NOTE;
        note.m_timestamp = LEAD_IN_MS;
        note.m_track     = 0;
        beatMap.sync();

        const std::string modeName = mode == 0 ? "key" : "slide";
        const fs::path    outputPath =
            std::filesystem::temp_directory_path() /
            ("edge_non_malody_lead_in_" + modeName + ".mc");
        TEST_ASSERT(beatMap.saveToFile(outputPath),
                    "non-Malody lead-in map should save");

        std::ifstream outputFile(outputPath);
        json          exported;
        outputFile >> exported;

        TEST_ASSERT(exported["time"].size() == 1,
                    "lead-in export should keep one timing");
        TEST_ASSERT(exported["time"][0]["beat"] == json::array({ 0, 0, 1 }),
                    "paired first timing should remain at beat zero");
        TEST_ASSERT(
            std::abs(exported["time"][0].value("delay", 0.0) - 263.0) < 1e-6,
            "first timing should wrap the main-audio phase");

        const auto sampleIt = std::find_if(
            exported["note"].begin(), exported["note"].end(), isSoundNode);
        TEST_ASSERT(sampleIt != exported["note"].end(),
                    "lead-in export should keep the main sample");
        TEST_ASSERT((*sampleIt)["beat"] == json::array({ 0, 0, 1 }),
                    "main sample should move back one beat");
        TEST_ASSERT(sampleIt->value("offset", -1) == 263,
                    "first-half-beat red line should keep wrapped main "
                    "offset");

        const auto noteIt =
            std::find_if(exported["note"].begin(),
                         exported["note"].end(),
                         [](const json& node) { return !isSoundNode(node); });
        TEST_ASSERT(noteIt != exported["note"].end(),
                    "lead-in export should keep the playable note");
        TEST_ASSERT((*noteIt)["beat"] == json::array({ 1, 0, 1 }),
                    "positive paired phase should move playable note one "
                    "Malody beat forward");

        MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
        reloaded.sync();
        TEST_ASSERT(reloaded.m_timings.size() == 1 &&
                        std::abs(reloaded.m_timings.front().m_timestamp -
                                 LEAD_IN_MS) < 1e-6,
                    "round trip should keep the first timing timestamp");
        TEST_ASSERT(reloaded.m_noteData.notes.size() == 1 &&
                        std::abs(reloaded.m_noteData.notes.front().m_timestamp -
                                 LEAD_IN_MS) < 1e-6,
                    "round trip should keep the playable note timestamp");
        TEST_ASSERT(
            reloaded.m_audioSamples.size() == 1 &&
                std::abs(reloaded.m_audioSamples.front().effectiveTimestamp()) <
                    1e-6,
            "round trip should keep audio playback at time zero");
    }

    XINFO("PASS: Non-Malody lead-in uses timing delay and sample compensation");
}

/// @brief 验证半拍后相位统一补偿，且晚于第一拍的首红线会保留。
void test_late_first_timing_prepends_anchor_and_shifts_all_content()
{
    XINFO("=== Test: Late first timing prepends paired anchor ===");

    constexpr double BPM                = 195.0;
    constexpr double BEAT_LENGTH_MS     = 60000.0 / BPM;
    constexpr double PHASE_MS           = 256.3076923076919;
    constexpr double FIRST_TIME_MS      = 3.0 * BEAT_LENGTH_MS + PHASE_MS;
    constexpr double CONTENT_TIME_MS    = FIRST_TIME_MS + BEAT_LENGTH_MS;
    constexpr double SECOND_BPM_TIME_MS = FIRST_TIME_MS + 2.0 * BEAT_LENGTH_MS;
    constexpr double EXPECTED_DELAY_MS  = BEAT_LENGTH_MS - PHASE_MS;

    auto withinFirstBeat                             = makeMinimalBeatMap(0, 4);
    withinFirstBeat.m_baseMapMetadata.preference_bpm = BPM;
    auto& withinFirstTiming         = withinFirstBeat.m_timings.front();
    withinFirstTiming.m_timestamp   = PHASE_MS;
    withinFirstTiming.m_bpm         = BPM;
    withinFirstTiming.m_beat_length = BEAT_LENGTH_MS;
    withinFirstTiming.m_timingEffectParameter = BPM;

    MMM::Timing& withinScroll   = withinFirstBeat.m_timings.emplace_back();
    withinScroll.m_timestamp    = PHASE_MS + BEAT_LENGTH_MS;
    withinScroll.m_bpm          = BPM;
    withinScroll.m_beat_length  = 1.25;
    withinScroll.m_timingEffect = MMM::TimingEffect::SCROLL;
    withinScroll.m_timingEffectParameter = 1.25;

    MMM::Note& withinNote  = withinFirstBeat.m_noteData.notes.emplace_back();
    withinNote.m_type      = MMM::NoteType::NOTE;
    withinNote.m_timestamp = PHASE_MS + BEAT_LENGTH_MS;
    withinNote.m_track     = 0;
    withinFirstBeat.sync();

    const fs::path withinOutputPath = std::filesystem::temp_directory_path() /
                                      "edge_after_half_beat_first_timing.mc";
    TEST_ASSERT(withinFirstBeat.saveToFile(withinOutputPath),
                "after-half-beat first timing map should export");
    std::ifstream withinOutputFile(withinOutputPath);
    json          withinExported;
    withinOutputFile >> withinExported;
    const auto withinMainSample = std::find_if(withinExported["note"].begin(),
                                               withinExported["note"].end(),
                                               isSoundNode);
    const auto withinPlayable =
        std::find_if(withinExported["note"].begin(),
                     withinExported["note"].end(),
                     [](const json& node) { return !isSoundNode(node); });
    TEST_ASSERT(
        withinExported["time"].size() == 1 &&
            withinExported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
            std::abs(withinExported["time"][0].value("delay", 0.0) -
                     EXPECTED_DELAY_MS) < 1e-6 &&
            withinMainSample != withinExported["note"].end() &&
            (*withinMainSample)["beat"] == json::array({ 0, 0, 1 }) &&
            withinMainSample->value("offset", -1) == 0 &&
            withinPlayable != withinExported["note"].end() &&
            (*withinPlayable)["beat"] == json::array({ 2, 0, 1 }) &&
            withinExported["effect"].size() == 1 &&
            withinExported["effect"][0]["beat"] == json::array({ 2, 0, 1 }),
        "phase after half a beat should keep zero main offset and shift "
        "ordinary content without adding a synthetic timing");

    MMM::BeatMap withinReloaded = MMM::BeatMap::loadFromFile(withinOutputPath);
    withinReloaded.sync();
    TEST_ASSERT(
        withinReloaded.m_noteData.notes.size() == 1 &&
            std::abs(withinReloaded.m_noteData.notes.front().m_timestamp -
                     (PHASE_MS + BEAT_LENGTH_MS)) < 1e-6 &&
            withinReloaded.m_timings.size() == 2 &&
            std::abs(withinReloaded.m_timings[1].m_timestamp -
                     (PHASE_MS + BEAT_LENGTH_MS)) < 1e-6,
        "after-half-beat note and effect should round trip together");

    auto beatMap                             = makeMinimalBeatMap(0, 4);
    beatMap.m_baseMapMetadata.preference_bpm = BPM;
    auto& firstTiming                        = beatMap.m_timings.front();
    firstTiming.m_timestamp                  = FIRST_TIME_MS;
    firstTiming.m_bpm                        = BPM;
    firstTiming.m_beat_length                = BEAT_LENGTH_MS;
    firstTiming.m_timingEffectParameter      = BPM;

    MMM::Timing& scroll            = beatMap.m_timings.emplace_back();
    scroll.m_timestamp             = CONTENT_TIME_MS;
    scroll.m_bpm                   = BPM;
    scroll.m_beat_length           = 1.25;
    scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
    scroll.m_timingEffectParameter = 1.25;

    MMM::Timing& secondBpm            = beatMap.m_timings.emplace_back();
    secondBpm.m_timestamp             = SECOND_BPM_TIME_MS;
    secondBpm.m_bpm                   = 180.0;
    secondBpm.m_beat_length           = 60000.0 / secondBpm.m_bpm;
    secondBpm.m_timingEffect          = MMM::TimingEffect::BPM;
    secondBpm.m_timingEffectParameter = secondBpm.m_bpm;

    MMM::Note& note  = beatMap.m_noteData.notes.emplace_back();
    note.m_type      = MMM::NoteType::NOTE;
    note.m_timestamp = CONTENT_TIME_MS;
    note.m_track     = 0;
    beatMap.sync();

    const fs::path outputPath = std::filesystem::temp_directory_path() /
                                "edge_late_first_timing_anchor.mc";
    TEST_ASSERT(beatMap.saveToFile(outputPath),
                "late first timing map should export");

    std::ifstream outputFile(outputPath);
    json          exported;
    outputFile >> exported;
    TEST_ASSERT(
        exported["time"].size() == 3 &&
            exported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
            exported["time"][1]["beat"] == json::array({ 4, 0, 1 }) &&
            exported["time"][2]["beat"] == json::array({ 6, 0, 1 }),
        "late first timing should keep its original red line after the anchor");
    TEST_ASSERT(
        std::abs(exported["time"][0].value("delay", 0.0) - EXPECTED_DELAY_MS) <
                1e-6 &&
            !exported["time"][1].contains("delay"),
        "only the synthetic first-beat anchor should carry the paired delay");

    const auto mainSample = std::find_if(
        exported["note"].begin(), exported["note"].end(), isSoundNode);
    const auto playable = std::find_if(
        exported["note"].begin(), exported["note"].end(), [](const json& node) {
            return !isSoundNode(node);
        });
    TEST_ASSERT(
        mainSample != exported["note"].end() &&
            (*mainSample)["beat"] == json::array({ 0, 0, 1 }) &&
            mainSample->value("offset", -1) == 0,
        "main audio should use the synthetic anchor without duplicating delay");
    TEST_ASSERT(
        playable != exported["note"].end() &&
            (*playable)["beat"] == json::array({ 5, 0, 1 }) &&
            exported["effect"].size() == 1 &&
            exported["effect"][0]["beat"] == json::array({ 5, 0, 1 }),
        "phase after half a beat must shift notes and effects together");

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
    reloaded.sync();
    const auto hasTimingAt = [&](MMM::TimingEffect effect, double timestamp) {
        return std::any_of(reloaded.m_timings.begin(),
                           reloaded.m_timings.end(),
                           [&](const MMM::Timing& timing) {
                               return timing.m_timingEffect == effect &&
                                      std::abs(timing.m_timestamp - timestamp) <
                                          1e-6;
                           });
    };
    TEST_ASSERT(
        hasTimingAt(MMM::TimingEffect::BPM, PHASE_MS) &&
            hasTimingAt(MMM::TimingEffect::BPM, FIRST_TIME_MS) &&
            hasTimingAt(MMM::TimingEffect::SCROLL, CONTENT_TIME_MS) &&
            hasTimingAt(MMM::TimingEffect::BPM, SECOND_BPM_TIME_MS),
        "synthetic and original timings should round trip independently");
    TEST_ASSERT(
        reloaded.m_noteData.notes.size() == 1 &&
            std::abs(reloaded.m_noteData.notes.front().m_timestamp -
                     CONTENT_TIME_MS) < 1e-6,
        "late-first-timing playable note should keep its absolute time");

    XINFO("PASS: Late first timing keeps original red line and adds anchor");
}

/// @brief 验证 Malody 主音频与首 timing 使用同一非负回卷值。
void test_first_timing_delay_unwraps_with_its_bpm()
{
    XINFO("=== Test: First timing delay unwraps with first BPM ===");

    constexpr double BPM              = 210.0;
    constexpr double WRAPPED_DELAY_MS = 237.032272;
    constexpr double BEAT_LENGTH_MS   = 60000.0 / BPM;
    constexpr double EXPECTED_TIME_MS = BEAT_LENGTH_MS - WRAPPED_DELAY_MS;

    const fs::path sourcePath = std::filesystem::temp_directory_path() /
                                "edge_wrapped_first_timing_source.mc";
    const fs::path exportPath = std::filesystem::temp_directory_path() /
                                "edge_wrapped_first_timing_export.mc";

    json fileData;
    fileData["meta"] = { { "id", 0 },
                         { "creator", "Test" },
                         { "version", "Wrapped" },
                         { "mode", 7 },
                         { "mode_ext", { { "bar_begin", 0 } } },
                         { "song",
                           { { "title", "Wrapped" },
                             { "artist", "Test" },
                             { "file", "music.ogg" },
                             { "bpm", BPM } } } };
    fileData["time"] = json::array({ { { "beat", json::array({ 0, 0, 1 }) },
                                       { "bpm", BPM },
                                       { "delay", WRAPPED_DELAY_MS } } });
    fileData["note"] = json::array(
        { { { "beat", json::array({ 1, 0, 1 }) }, { "x", 31 }, { "w", 60 } },
          { { "beat", json::array({ 0, 0, 1 }) },
            { "type", 1 },
            { "sound", "music.ogg" },
            { "offset", 237 } } });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open wrapped timing input");
    source << fileData.dump();
    source.close();

    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(loaded.m_timings.size() == 1,
                "wrapped timing map should keep its first timing");
    TEST_ASSERT(std::abs(loaded.m_timings.front().m_timestamp -
                         EXPECTED_TIME_MS) < 1e-6,
                "237.032ms at 210 BPM should unwrap to about 48.682ms");
    TEST_ASSERT(loaded.m_noteData.notes.size() == 1 &&
                    std::abs(loaded.m_noteData.notes.front().m_timestamp -
                             EXPECTED_TIME_MS) < 1e-6,
                "shifted playable beat should use the unwrapped timing anchor");
    TEST_ASSERT(
        loaded.m_audioSamples.size() == 1 &&
            std::abs(loaded.m_audioSamples.front().m_timestamp - 0.0) < 1e-6,
        "paired main sample should normalize its anchor to time zero");
    TEST_ASSERT(loaded.m_audioSamples.front().m_offsetMs == 0,
                "paired main sample should normalize its offset to zero");
    TEST_ASSERT(
        std::abs(loaded.m_audioSamples.front().effectiveTimestamp()) < 0.51,
        "paired main sample should still play at time zero");

    TEST_ASSERT(loaded.saveToFile(exportPath),
                "wrapped timing map should export");
    std::ifstream exportedFile(exportPath);
    json          exported;
    exportedFile >> exported;
    TEST_ASSERT(exported["time"][0]["beat"] == json::array({ 0, 0, 1 }),
                "wrapped first timing should keep beat zero");
    TEST_ASSERT(std::abs(exported["time"][0].value("delay", 0.0) -
                         WRAPPED_DELAY_MS) < 1e-6,
                "wrapped first timing should keep its non-negative delay");
    const auto exportedSample = std::find_if(
        exported["note"].begin(), exported["note"].end(), isSoundNode);
    TEST_ASSERT(exportedSample != exported["note"].end(),
                "wrapped map should keep its main sample");
    TEST_ASSERT(
        (*exportedSample)["beat"] == json::array({ 0, 0, 1 }) &&
            exportedSample->value("offset", -1) == 237,
        "first-half-beat wrapped main sample should keep its delay offset");
    const auto exportedPlayable = std::find_if(
        exported["note"].begin(), exported["note"].end(), [](const json& node) {
            return !isSoundNode(node);
        });
    TEST_ASSERT(exportedPlayable != exported["note"].end() &&
                    (*exportedPlayable)["beat"] == json::array({ 1, 0, 1 }),
                "wrapped playable note should retain its one-beat phase shift");

    json positiveOffsetData                = fileData;
    positiveOffsetData["time"][0]["delay"] = 100.0;
    auto& positiveSample                   = positiveOffsetData["note"][1];
    positiveSample["offset"]               = 100;
    const fs::path positivePath =
        std::filesystem::temp_directory_path() / "edge_positive_main_offset.mc";
    const fs::path positiveExportPath = std::filesystem::temp_directory_path() /
                                        "edge_positive_main_offset_export.mc";
    std::ofstream  positiveFile(positivePath);
    TEST_ASSERT(positiveFile.good(), "should open positive offset input");
    positiveFile << positiveOffsetData.dump();
    positiveFile.close();

    MMM::BeatMap positiveLoaded = MMM::BeatMap::loadFromFile(positivePath);
    positiveLoaded.sync();
    constexpr double EXPECTED_SMALL_DELAY_TIME_MS = BEAT_LENGTH_MS - 100.0;
    TEST_ASSERT(positiveLoaded.m_timings.size() == 1 &&
                    std::abs(positiveLoaded.m_timings.front().m_timestamp -
                             EXPECTED_SMALL_DELAY_TIME_MS) < 1e-6,
                "small paired delay should use the same modulo phase rule");
    TEST_ASSERT(
        positiveLoaded.m_audioSamples.size() == 1 &&
            std::abs(positiveLoaded.m_audioSamples.front().m_timestamp) <
                1e-6 &&
            positiveLoaded.m_audioSamples.front().m_offsetMs == 0,
        "paired small delay should normalize the main sample to time zero");
    TEST_ASSERT(positiveLoaded.saveToFile(positiveExportPath),
                "paired small delay map should export");
    std::ifstream positiveExportFile(positiveExportPath);
    json          positiveExported;
    positiveExportFile >> positiveExported;
    const auto positiveExportedSample =
        std::find_if(positiveExported["note"].begin(),
                     positiveExported["note"].end(),
                     isSoundNode);
    TEST_ASSERT(
        std::abs(positiveExported["time"][0].value("delay", 0.0) - 100.0) <
                1e-6 &&
            positiveExportedSample != positiveExported["note"].end() &&
            positiveExportedSample->value("offset", -1) == 0,
        "small delay should round trip without duplicating main offset");

    constexpr double LARGE_DELAY_BPM     = 212.0;
    constexpr double LARGE_DELAY_BEAT_MS = 60000.0 / LARGE_DELAY_BPM;
    constexpr double LARGE_DELAY_MS      = 1083.54;
    constexpr double LARGE_DELAY_PHASE_MS =
        4.0 * LARGE_DELAY_BEAT_MS - LARGE_DELAY_MS;
    json largeDelayData                   = fileData;
    largeDelayData["meta"]["song"]["bpm"] = LARGE_DELAY_BPM;
    largeDelayData["time"][0]["bpm"]      = LARGE_DELAY_BPM;
    largeDelayData["time"][0]["delay"]    = LARGE_DELAY_MS;
    largeDelayData["note"][1]["offset"]   = 1084;
    const fs::path largeDelayPath = std::filesystem::temp_directory_path() /
                                    "edge_large_paired_main_offset.mc";
    const fs::path largeDelayExportPath =
        std::filesystem::temp_directory_path() /
        "edge_large_paired_main_offset_export.mc";
    std::ofstream largeDelayFile(largeDelayPath);
    TEST_ASSERT(largeDelayFile.good(), "should open large paired delay input");
    largeDelayFile << largeDelayData.dump();
    largeDelayFile.close();

    MMM::BeatMap largeDelayLoaded = MMM::BeatMap::loadFromFile(largeDelayPath);
    largeDelayLoaded.sync();
    TEST_ASSERT(
        largeDelayLoaded.m_timings.size() == 1 &&
            std::abs(largeDelayLoaded.m_timings.front().m_timestamp -
                     LARGE_DELAY_PHASE_MS) < 1e-6,
        "paired delay larger than one beat should use Euclidean modulo");
    TEST_ASSERT(
        largeDelayLoaded.m_audioSamples.size() == 1 &&
            std::abs(largeDelayLoaded.m_audioSamples.front().m_timestamp) <
                1e-6 &&
            largeDelayLoaded.m_audioSamples.front().m_offsetMs == 0,
        "large paired delay should normalize the main sample to time zero");
    TEST_ASSERT(largeDelayLoaded.saveToFile(largeDelayExportPath),
                "large paired delay map should export");
    std::ifstream largeDelayExportFile(largeDelayExportPath);
    json          largeDelayExported;
    largeDelayExportFile >> largeDelayExported;
    const auto largeDelayExportedSample =
        std::find_if(largeDelayExported["note"].begin(),
                     largeDelayExported["note"].end(),
                     isSoundNode);
    TEST_ASSERT(
        std::abs(largeDelayExported["time"][0].value("delay", 0.0) -
                 std::fmod(LARGE_DELAY_MS, LARGE_DELAY_BEAT_MS)) < 1e-6 &&
            largeDelayExportedSample != largeDelayExported["note"].end() &&
            largeDelayExportedSample->value("offset", -1) ==
                static_cast<std::int64_t>(std::llround(
                    std::fmod(LARGE_DELAY_MS, LARGE_DELAY_BEAT_MS))),
        "large first-half-beat delay should keep its wrapped main offset");

    json  unmatchedOffsetData    = fileData;
    auto& unmatchedSample        = unmatchedOffsetData["note"][1];
    unmatchedSample["offset"]    = 200;
    const fs::path unmatchedPath = std::filesystem::temp_directory_path() /
                                   "edge_unmatched_main_offset.mc";
    std::ofstream  unmatchedFile(unmatchedPath);
    TEST_ASSERT(unmatchedFile.good(), "should open unmatched offset input");
    unmatchedFile << unmatchedOffsetData.dump();
    unmatchedFile.close();

    MMM::BeatMap unmatchedLoaded = MMM::BeatMap::loadFromFile(unmatchedPath);
    unmatchedLoaded.sync();
    TEST_ASSERT(unmatchedLoaded.m_audioSamples.size() == 1 &&
                    unmatchedLoaded.m_audioSamples.front().m_offsetMs == 200,
                "offset not paired with first delay must remain positive");

    json effectOffsetData                = fileData;
    effectOffsetData["note"][1]["sound"] = "effect.ogg";
    const fs::path effectOffsetPath = std::filesystem::temp_directory_path() /
                                      "edge_effect_wrapped_offset.mc";
    std::ofstream  effectOffsetFile(effectOffsetPath);
    TEST_ASSERT(effectOffsetFile.good(), "should open effect offset input");
    effectOffsetFile << effectOffsetData.dump();
    effectOffsetFile.close();

    MMM::BeatMap effectOffsetLoaded =
        MMM::BeatMap::loadFromFile(effectOffsetPath);
    effectOffsetLoaded.sync();
    TEST_ASSERT(effectOffsetLoaded.m_audioSamples.size() == 1 &&
                    effectOffsetLoaded.m_audioSamples.front().m_offsetMs == 237,
                "non-main sample offset must never trigger paired wrapping");

    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    reloaded.sync();
    TEST_ASSERT(reloaded.m_timings.size() == 1 &&
                    std::abs(reloaded.m_timings.front().m_timestamp -
                             EXPECTED_TIME_MS) < 1e-6,
                "wrapped delay should remain stable after round trip");
    TEST_ASSERT(
        reloaded.m_audioSamples.size() == 1 &&
            std::abs(reloaded.m_audioSamples.front().m_timestamp) < 1e-6 &&
            reloaded.m_audioSamples.front().m_offsetMs == 0 &&
            std::abs(reloaded.m_audioSamples.front().effectiveTimestamp()) <
                0.51,
        "wrapped main sample should remain normalized at time zero");

    constexpr double SUB_MILLISECOND_EDIT_MS = 0.4;
    MMM::BeatMap     timingMoved = MMM::BeatMap::loadFromFile(sourcePath);
    timingMoved.m_timings.front().m_timestamp += SUB_MILLISECOND_EDIT_MS;
    timingMoved.sync();
    const fs::path timingMovedPath =
        std::filesystem::temp_directory_path() / "edge_wrapped_timing_moved.mc";
    TEST_ASSERT(timingMoved.saveToFile(timingMovedPath),
                "sub-millisecond timing edit should export");
    std::ifstream timingMovedFile(timingMovedPath);
    json          timingMovedJson;
    timingMovedFile >> timingMovedJson;
    TEST_ASSERT(std::abs(timingMovedJson["time"][0].value("delay", 0.0) -
                         (WRAPPED_DELAY_MS - SUB_MILLISECOND_EDIT_MS)) < 1e-6,
                "sub-millisecond timing edit must replace the imported phase");
    MMM::BeatMap timingMovedReloaded =
        MMM::BeatMap::loadFromFile(timingMovedPath);
    timingMovedReloaded.sync();
    TEST_ASSERT(std::abs(timingMovedReloaded.m_timings.front().m_timestamp -
                         (EXPECTED_TIME_MS + SUB_MILLISECOND_EDIT_MS)) < 1e-6,
                "sub-millisecond timing edit should survive round trip");

    for ( const int wholeBeatDelta : { -1, 1 } ) {
        MMM::BeatMap wholeBeatMoved = MMM::BeatMap::loadFromFile(sourcePath);
        wholeBeatMoved.m_timings.front().m_timestamp +=
            static_cast<double>(wholeBeatDelta) * BEAT_LENGTH_MS;
        wholeBeatMoved.sync();
        const fs::path wholeBeatMovedPath =
            std::filesystem::temp_directory_path() /
            (std::string("edge_wrapped_timing_whole_beat_") +
             (wholeBeatDelta < 0 ? "back.mc" : "forward.mc"));
        TEST_ASSERT(wholeBeatMoved.saveToFile(wholeBeatMovedPath),
                    "whole-beat timing edit should export");
        std::ifstream wholeBeatMovedFile(wholeBeatMovedPath);
        json          wholeBeatMovedJson;
        wholeBeatMovedFile >> wholeBeatMovedJson;
        const auto wholeBeatMovedSample =
            std::find_if(wholeBeatMovedJson["note"].begin(),
                         wholeBeatMovedJson["note"].end(),
                         isSoundNode);
        const json expectedOriginalBeat =
            json::array({ wholeBeatDelta < 0 ? 0 : 2, 0, 1 });
        TEST_ASSERT(
            wholeBeatMovedJson["time"].size() == 2 &&
                wholeBeatMovedJson["time"][0]["beat"] ==
                    json::array({ 0, 0, 1 }) &&
                std::abs(wholeBeatMovedJson["time"][0].value("delay", 0.0) -
                         WRAPPED_DELAY_MS) < 1e-6 &&
                wholeBeatMovedJson["time"][1]["beat"] == expectedOriginalBeat &&
                !wholeBeatMovedJson["time"][1].contains("delay") &&
                wholeBeatMovedSample != wholeBeatMovedJson["note"].end() &&
                (*wholeBeatMovedSample)["beat"] == json::array({ 0, 0, 1 }),
            "out-of-range first timing should keep a first-beat anchor and its "
            "original red line");
        MMM::BeatMap wholeBeatMovedReloaded =
            MMM::BeatMap::loadFromFile(wholeBeatMovedPath);
        wholeBeatMovedReloaded.sync();
        const double expectedOriginalTime =
            EXPECTED_TIME_MS +
            static_cast<double>(wholeBeatDelta) * BEAT_LENGTH_MS;
        TEST_ASSERT(
            wholeBeatMovedReloaded.m_timings.size() == 2 &&
                std::any_of(wholeBeatMovedReloaded.m_timings.begin(),
                            wholeBeatMovedReloaded.m_timings.end(),
                            [&](const MMM::Timing& timing) {
                                return std::abs(timing.m_timestamp -
                                                expectedOriginalTime) < 1e-6;
                            }),
            "whole-beat timing edit and synthetic anchor should round trip");
    }

    MMM::BeatMap mainMoved = MMM::BeatMap::loadFromFile(sourcePath);
    mainMoved.m_audioSamples.front().m_timestamp = SUB_MILLISECOND_EDIT_MS;
    mainMoved.sync();
    const fs::path mainMovedPath =
        std::filesystem::temp_directory_path() / "edge_wrapped_main_moved.mc";
    TEST_ASSERT(mainMoved.saveToFile(mainMovedPath),
                "sub-millisecond main-audio edit should export");
    std::ifstream mainMovedFile(mainMovedPath);
    json          mainMovedJson;
    mainMovedFile >> mainMovedJson;
    const auto mainMovedSample = std::find_if(mainMovedJson["note"].begin(),
                                              mainMovedJson["note"].end(),
                                              isSoundNode);
    TEST_ASSERT(
        std::abs(mainMovedJson["time"][0].value("delay", 0.0) -
                 EXPECTED_TIME_MS) < 1e-6 &&
            mainMovedSample != mainMovedJson["note"].end() &&
            mainMovedSample->value("offset", 0) == 0,
        "moved main audio must use ordinary sample timing instead of pairing");
    MMM::BeatMap mainMovedReloaded = MMM::BeatMap::loadFromFile(mainMovedPath);
    mainMovedReloaded.sync();
    TEST_ASSERT(
        std::abs(mainMovedReloaded.m_audioSamples.front().effectiveTimestamp() -
                 SUB_MILLISECOND_EDIT_MS) < 0.2,
        "sub-millisecond main-audio edit should survive beat quantization");

    MMM::BeatMap legacyShape = MMM::BeatMap::loadFromFile(sourcePath);
    legacyShape.m_audioSamples.front().m_timestamp = EXPECTED_TIME_MS;
    legacyShape.m_audioSamples.front().m_offsetMs =
        static_cast<std::int64_t>(std::llround(237.0 - BEAT_LENGTH_MS));
    legacyShape.sync();
    const fs::path legacyShapePath = std::filesystem::temp_directory_path() /
                                     "edge_legacy_wrapped_main_shape.mc";
    TEST_ASSERT(legacyShape.saveToFile(legacyShapePath),
                "legacy wrapped main-audio shape should export");
    std::ifstream legacyShapeFile(legacyShapePath);
    json          legacyShapeJson;
    legacyShapeFile >> legacyShapeJson;
    const auto legacyShapeSample = std::find_if(legacyShapeJson["note"].begin(),
                                                legacyShapeJson["note"].end(),
                                                isSoundNode);
    TEST_ASSERT(std::abs(legacyShapeJson["time"][0].value("delay", 0.0) -
                         WRAPPED_DELAY_MS) < 1e-6 &&
                    legacyShapeSample != legacyShapeJson["note"].end() &&
                    legacyShapeSample->value("offset", -1) == 237,
                "legacy anchor-plus-offset shape should export canonically");

    constexpr double LEGACY_ZERO_BOUNDARY_MS = -0.1;
    const double     legacyNearBeatDelayMs =
        BEAT_LENGTH_MS + LEGACY_ZERO_BOUNDARY_MS;
    MMM::BeatMap legacyZeroBoundary = MMM::BeatMap::loadFromFile(sourcePath);
    legacyZeroBoundary.m_timings.front().m_timestamp = LEGACY_ZERO_BOUNDARY_MS;
    legacyZeroBoundary.m_timings.front()
        .m_metadata
        .timing_properties[MMM::TimingMetadataType::MALODY]["delay"] =
        json(legacyNearBeatDelayMs).dump();
    legacyZeroBoundary.m_audioSamples.front().m_timestamp =
        LEGACY_ZERO_BOUNDARY_MS;
    legacyZeroBoundary.m_audioSamples.front().m_offsetMs = 0;
    legacyZeroBoundary.m_noteData.notes.front().m_timestamp =
        LEGACY_ZERO_BOUNDARY_MS;
    legacyZeroBoundary.sync();
    const fs::path legacyZeroBoundaryPath =
        std::filesystem::temp_directory_path() /
        "edge_legacy_wrapped_zero_boundary.mc";
    TEST_ASSERT(legacyZeroBoundary.saveToFile(legacyZeroBoundaryPath),
                "legacy near-zero wrapped shape should export");
    std::ifstream legacyZeroBoundaryFile(legacyZeroBoundaryPath);
    json          legacyZeroBoundaryJson;
    legacyZeroBoundaryFile >> legacyZeroBoundaryJson;
    const auto legacyZeroBoundaryPlayable =
        std::find_if(legacyZeroBoundaryJson["note"].begin(),
                     legacyZeroBoundaryJson["note"].end(),
                     [](const json& node) { return !isSoundNode(node); });
    TEST_ASSERT(
        legacyZeroBoundaryPlayable != legacyZeroBoundaryJson["note"].end() &&
            (*legacyZeroBoundaryPlayable)["beat"] == json::array({ 1, 0, 1 }),
        "legacy near-zero phase should use the exported timing position when "
        "shifting note-array beats");
    MMM::BeatMap legacyZeroBoundaryReloaded =
        MMM::BeatMap::loadFromFile(legacyZeroBoundaryPath);
    legacyZeroBoundaryReloaded.sync();
    TEST_ASSERT(
        legacyZeroBoundaryReloaded.m_timings.size() == 2 &&
            legacyZeroBoundaryReloaded.m_noteData.notes.size() == 1 &&
            legacyZeroBoundaryReloaded.m_timings.front().m_timestamp > 0.0 &&
            std::abs(legacyZeroBoundaryReloaded.m_noteData.notes.front()
                         .m_timestamp -
                     legacyZeroBoundaryReloaded.m_timings.front().m_timestamp) <
                1e-6,
        "legacy near-zero phase must keep note, original timing, and synthetic "
        "anchor aligned after canonicalization");

    for ( const int wholeBeatDelta : { -1, 1 } ) {
        MMM::BeatMap legacyWholeBeat = MMM::BeatMap::loadFromFile(sourcePath);
        legacyWholeBeat.m_timings.front().m_timestamp =
            EXPECTED_TIME_MS +
            static_cast<double>(wholeBeatDelta) * BEAT_LENGTH_MS;
        legacyWholeBeat.m_audioSamples.front().m_timestamp =
            legacyWholeBeat.m_timings.front().m_timestamp;
        legacyWholeBeat.m_audioSamples.front().m_offsetMs =
            static_cast<std::int64_t>(std::llround(237.0 - BEAT_LENGTH_MS));
        legacyWholeBeat.sync();
        const fs::path legacyWholeBeatPath =
            std::filesystem::temp_directory_path() /
            (std::string("edge_legacy_wrapped_main_whole_beat_") +
             (wholeBeatDelta < 0 ? "back.mc" : "forward.mc"));
        TEST_ASSERT(legacyWholeBeat.saveToFile(legacyWholeBeatPath),
                    "legacy whole-beat wrapped shape should export");
        std::ifstream legacyWholeBeatFile(legacyWholeBeatPath);
        json          legacyWholeBeatJson;
        legacyWholeBeatFile >> legacyWholeBeatJson;
        const auto legacyWholeBeatSample =
            std::find_if(legacyWholeBeatJson["note"].begin(),
                         legacyWholeBeatJson["note"].end(),
                         isSoundNode);
        const json expectedOriginalBeat =
            json::array({ wholeBeatDelta < 0 ? 0 : 2, 0, 1 });
        TEST_ASSERT(
            legacyWholeBeatJson["time"].size() == 2 &&
                legacyWholeBeatJson["time"][0]["beat"] ==
                    json::array({ 0, 0, 1 }) &&
                std::abs(legacyWholeBeatJson["time"][0].value("delay", 0.0) -
                         WRAPPED_DELAY_MS) < 1e-6 &&
                legacyWholeBeatJson["time"][1]["beat"] ==
                    expectedOriginalBeat &&
                legacyWholeBeatSample != legacyWholeBeatJson["note"].end() &&
                (*legacyWholeBeatSample)["beat"] == json::array({ 0, 0, 1 }) &&
                legacyWholeBeatSample->value("offset", -1) == 237,
            "legacy wrapped shape should retain the original red line after "
            "the synthetic anchor");
        MMM::BeatMap legacyWholeBeatReloaded =
            MMM::BeatMap::loadFromFile(legacyWholeBeatPath);
        legacyWholeBeatReloaded.sync();
        const double expectedLegacyOriginalTime =
            legacyWholeBeat.m_timings.front().m_timestamp;
        TEST_ASSERT(
            legacyWholeBeatReloaded.m_timings.size() == 2 &&
                std::any_of(legacyWholeBeatReloaded.m_timings.begin(),
                            legacyWholeBeatReloaded.m_timings.end(),
                            [&](const MMM::Timing& timing) {
                                return std::abs(timing.m_timestamp -
                                                expectedLegacyOriginalTime) <
                                       1e-6;
                            }) &&
                legacyWholeBeatReloaded.m_audioSamples.size() == 1 &&
                std::abs(legacyWholeBeatReloaded.m_audioSamples.front()
                             .m_timestamp) < 1e-6 &&
                legacyWholeBeatReloaded.m_audioSamples.front().m_offsetMs == 0,
            "legacy whole-beat shape and synthetic anchor should reload");
    }

    for ( const int mode : { 0, 7 } ) {
        auto generated = makeMinimalBeatMap(mode, 4);
        generated.m_baseMapMetadata.preference_bpm = 123.0;
        auto& firstTiming                   = generated.m_timings.front();
        firstTiming.m_timestamp             = EXPECTED_TIME_MS;
        firstTiming.m_bpm                   = BPM;
        firstTiming.m_beat_length           = BEAT_LENGTH_MS;
        firstTiming.m_timingEffectParameter = BPM;
        generated.m_baseMapMetadata.song_file_hint         = "music.ogg";
        generated.m_audioSamples.front().m_audioResourceId = "music.ogg";
        generated.m_audioSamples.front().m_timestamp       = 0.0;
        generated.m_audioSamples.front().m_offsetMs        = 0;

        MMM::Timing& scroll            = generated.m_timings.emplace_back();
        scroll.m_timestamp             = EXPECTED_TIME_MS;
        scroll.m_bpm                   = BPM;
        scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
        scroll.m_timingEffectParameter = 1.25;
        scroll.m_beat_length           = 1.25;

        MMM::Note& note  = generated.m_noteData.notes.emplace_back();
        note.m_type      = MMM::NoteType::NOTE;
        note.m_timestamp = EXPECTED_TIME_MS;
        note.m_track     = 0;

        MMM::Hold& hold  = generated.m_noteData.holds.emplace_back();
        hold.m_type      = MMM::NoteType::HOLD;
        hold.m_timestamp = EXPECTED_TIME_MS + BEAT_LENGTH_MS;
        hold.m_duration  = BEAT_LENGTH_MS;
        hold.m_track     = 1;

        MMM::AudioSampleEvent& ordinarySample =
            generated.m_audioSamples.emplace_back();
        ordinarySample.m_timestamp       = EXPECTED_TIME_MS;
        ordinarySample.m_track           = 5;
        ordinarySample.m_audioResourceId = "effect.ogg";
        generated.sync();

        const fs::path generatedPath =
            std::filesystem::temp_directory_path() /
            (std::string("edge_generated_wrapped_first_timing_") +
             (mode == 0 ? "key.mc" : "slide.mc"));
        TEST_ASSERT(generated.saveToFile(generatedPath),
                    "generated wrapped timing map should export");

        std::ifstream generatedFile(generatedPath);
        json          generatedJson;
        generatedFile >> generatedJson;
        TEST_ASSERT(
            generatedJson["time"][0]["beat"] == json::array({ 0, 0, 1 }),
            "paired first timing should export at beat zero");
        TEST_ASSERT(
            std::abs(generatedJson["time"][0].value("delay", 0.0) -
                     WRAPPED_DELAY_MS) < 1e-6,
            "paired first timing should export its wrapped phase delay");
        const auto generatedSample = std::find_if(generatedJson["note"].begin(),
                                                  generatedJson["note"].end(),
                                                  isSoundNode);
        TEST_ASSERT(
            generatedSample != generatedJson["note"].end() &&
                (*generatedSample)["beat"] == json::array({ 0, 0, 1 }) &&
                generatedSample->value("offset", -1) == 237,
            "first-half-beat main SOUND should keep the wrapped delay");
        const auto generatedPlayable =
            std::find_if(generatedJson["note"].begin(),
                         generatedJson["note"].end(),
                         [](const json& node) { return !isSoundNode(node); });
        TEST_ASSERT(
            generatedPlayable != generatedJson["note"].end() &&
                (*generatedPlayable)["beat"] == json::array({ 1, 0, 1 }),
            "positive paired phase should shift generated playable note");
        const auto generatedHold =
            std::find_if(generatedJson["note"].begin(),
                         generatedJson["note"].end(),
                         [&](const json& node) {
                             return !isSoundNode(node) &&
                                    (mode == 0 ? node.contains("endbeat")
                                               : node.contains("seg"));
                         });
        TEST_ASSERT(generatedHold != generatedJson["note"].end() &&
                        (*generatedHold)["beat"] == json::array({ 2, 0, 1 }),
                    "positive paired phase should shift Hold root beat");
        if ( mode == 0 ) {
            TEST_ASSERT(
                (*generatedHold)["endbeat"] == json::array({ 3, 0, 1 }),
                "Key Hold endbeat should receive the same absolute shift");
        } else {
            TEST_ASSERT(
                (*generatedHold)["seg"].size() == 1 &&
                    (*generatedHold)["seg"][0]["beat"] ==
                        json::array({ 1, 0, 1 }),
                "Slide Hold relative segment beat should remain unchanged");
        }
        const auto generatedOrdinarySample = std::find_if(
            generatedJson["note"].begin(),
            generatedJson["note"].end(),
            [](const json& node) {
                return isSoundNode(node) &&
                       node.value("sound", std::string{}) == "effect.ogg";
            });
        TEST_ASSERT(
            generatedOrdinarySample != generatedJson["note"].end() &&
                (*generatedOrdinarySample)["beat"] == json::array({ 1, 0, 1 }),
            "ordinary automatic sample should receive the note-array phase "
            "shift");
        TEST_ASSERT(
            !generatedJson["effect"].empty() &&
                generatedJson["effect"][0]["beat"] == json::array({ 1, 0, 1 }),
            "effect at the paired first timing should share the phase shift");

        MMM::BeatMap generatedReloaded =
            MMM::BeatMap::loadFromFile(generatedPath);
        generatedReloaded.sync();
        TEST_ASSERT(generatedReloaded.m_timings.size() == 2,
                    "generated wrapped map should keep BPM and effect");
        TEST_ASSERT(
            std::abs(generatedReloaded.m_timings[0].m_timestamp -
                     EXPECTED_TIME_MS) < 1e-6 &&
                std::abs(generatedReloaded.m_timings[1].m_timestamp -
                         EXPECTED_TIME_MS) < 1e-6,
            "generated BPM and effect should round trip at the paired phase");
        TEST_ASSERT(std::abs(generatedReloaded.m_timings[1].m_bpm - BPM) < 1e-6,
                    "effect before a positive first beat should use first BPM");
        TEST_ASSERT(
            generatedReloaded.m_noteData.notes.size() == 1 &&
                std::abs(
                    generatedReloaded.m_noteData.notes.front().m_timestamp -
                    EXPECTED_TIME_MS) < 1e-6,
            "generated playable note should remove the phase shift on import");
        TEST_ASSERT(
            generatedReloaded.m_noteData.holds.size() == 1 &&
                std::abs(
                    generatedReloaded.m_noteData.holds.front().m_timestamp -
                    (EXPECTED_TIME_MS + BEAT_LENGTH_MS)) < 1e-6 &&
                std::abs(generatedReloaded.m_noteData.holds.front().m_duration -
                         BEAT_LENGTH_MS) < 1e-6,
            "generated Hold absolute beats should round trip");
        TEST_ASSERT(
            generatedReloaded.m_audioSamples.size() == 2,
            "generated samples should keep main and ordinary automatic audio");
        const auto generatedReloadedMain =
            std::find_if(generatedReloaded.m_audioSamples.begin(),
                         generatedReloaded.m_audioSamples.end(),
                         [](const MMM::AudioSampleEvent& sample) {
                             return sample.m_audioResourceId == "music.ogg";
                         });
        const auto generatedReloadedOrdinary =
            std::find_if(generatedReloaded.m_audioSamples.begin(),
                         generatedReloaded.m_audioSamples.end(),
                         [](const MMM::AudioSampleEvent& sample) {
                             return sample.m_audioResourceId == "effect.ogg";
                         });
        TEST_ASSERT(
            generatedReloadedMain != generatedReloaded.m_audioSamples.end() &&
                std::abs(generatedReloadedMain->m_timestamp) < 1e-6 &&
                generatedReloadedMain->m_offsetMs == 0 &&
                generatedReloadedOrdinary !=
                    generatedReloaded.m_audioSamples.end() &&
                std::abs(generatedReloadedOrdinary->m_timestamp -
                         EXPECTED_TIME_MS) < 1e-6,
            "automatic samples should remove the note-array phase shift on "
            "import");
    }

    auto  displacedMain                     = makeMinimalBeatMap(7, 4);
    auto& displacedTiming                   = displacedMain.m_timings.front();
    displacedTiming.m_timestamp             = EXPECTED_TIME_MS;
    displacedTiming.m_bpm                   = BPM;
    displacedTiming.m_beat_length           = BEAT_LENGTH_MS;
    displacedTiming.m_timingEffectParameter = BPM;
    displacedMain.m_baseMapMetadata.song_file_hint         = "music.ogg";
    displacedMain.m_audioSamples.front().m_audioResourceId = "music.ogg";
    displacedMain.m_audioSamples.front().m_timestamp       = 20.0;
    displacedMain.sync();

    const fs::path displacedPath = std::filesystem::temp_directory_path() /
                                   "edge_displaced_main_sample.mc";
    TEST_ASSERT(displacedMain.saveToFile(displacedPath),
                "non-zero main sample should export");
    std::ifstream displacedFile(displacedPath);
    json          displacedJson;
    displacedFile >> displacedJson;
    const auto displacedSample = std::find_if(displacedJson["note"].begin(),
                                              displacedJson["note"].end(),
                                              isSoundNode);
    TEST_ASSERT(std::abs(displacedJson["time"][0].value("delay", 0.0) -
                         EXPECTED_TIME_MS) < 1e-6 &&
                    displacedSample != displacedJson["note"].end() &&
                    displacedSample->value("offset", 0) != 237,
                "main sample away from time zero must not use paired wrapping");

    XINFO("PASS: First timing delay unwraps with first BPM");
}

/// @brief 验证 note[] 拍号补偿在首拍相位边界仍保持绝对时间。
void test_malody_note_phase_shift_boundaries()
{
    XINFO("=== Test: Playable note phase shift boundaries ===");

    constexpr double BPM            = 120.0;
    constexpr double BEAT_LENGTH_MS = 60000.0 / BPM;
    struct TestCase {
        double       firstTimingMs;
        int          expectedBeat;
        double       expectedDelayMs;
        std::int64_t expectedMainOffsetMs;
        const char*  tag;
    };
    const std::array<TestCase, 2> cases{ {
        { -100.0, 0, 100.0, 100, "negative_timing" },
        { BEAT_LENGTH_MS, 1, 0.0, 0, "zero_phase" },
    } };

    for ( const auto& testCase : cases ) {
        for ( const int mode : { 0, 7 } ) {
            auto  beatMap                       = makeMinimalBeatMap(mode, 4);
            auto& firstTiming                   = beatMap.m_timings.front();
            firstTiming.m_timestamp             = testCase.firstTimingMs;
            firstTiming.m_bpm                   = BPM;
            firstTiming.m_beat_length           = BEAT_LENGTH_MS;
            firstTiming.m_timingEffectParameter = BPM;
            beatMap.m_audioSamples.front().m_timestamp = 0.0;
            beatMap.m_audioSamples.front().m_offsetMs  = 0;

            MMM::Note& note  = beatMap.m_noteData.notes.emplace_back();
            note.m_type      = MMM::NoteType::NOTE;
            note.m_timestamp = testCase.firstTimingMs;
            note.m_track     = 0;
            beatMap.sync();

            const fs::path outputPath =
                std::filesystem::temp_directory_path() /
                (std::string("edge_playable_phase_boundary_") + testCase.tag +
                 (mode == 0 ? "_key.mc" : "_slide.mc"));
            TEST_ASSERT(beatMap.saveToFile(outputPath),
                        "phase boundary map should export");
            std::ifstream outputFile(outputPath);
            json          exported;
            outputFile >> exported;
            const auto exportedPlayable = std::find_if(
                exported["note"].begin(),
                exported["note"].end(),
                [](const json& node) { return !isSoundNode(node); });
            const auto exportedMainSample = std::find_if(
                exported["note"].begin(), exported["note"].end(), isSoundNode);
            TEST_ASSERT(
                exportedPlayable != exported["note"].end() &&
                    (*exportedPlayable)["beat"] ==
                        json::array({ testCase.expectedBeat, 0, 1 }),
                "phase boundary should preserve the playable absolute time");
            TEST_ASSERT(
                !exported["time"].empty() &&
                    std::abs(exported["time"][0].value("delay", -1.0) -
                             testCase.expectedDelayMs) < 1e-6 &&
                    exportedMainSample != exported["note"].end() &&
                    exportedMainSample->value("offset", -1) ==
                        testCase.expectedMainOffsetMs,
                "first timing should export the expected delay and paired "
                "main SOUND offset");

            MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
            reloaded.sync();
            const bool keptFirstTiming = std::any_of(
                reloaded.m_timings.begin(),
                reloaded.m_timings.end(),
                [&](const MMM::Timing& timing) {
                    return timing.m_timingEffect == MMM::TimingEffect::BPM &&
                           std::abs(timing.m_timestamp -
                                    testCase.firstTimingMs) < 1e-6;
                });
            TEST_ASSERT(
                keptFirstTiming && reloaded.m_noteData.notes.size() == 1 &&
                    std::abs(reloaded.m_noteData.notes.front().m_timestamp -
                             testCase.firstTimingMs) < 1e-6,
                "phase boundary timing and playable note should round trip");
        }
    }

    XINFO("PASS: Playable note phase shift boundaries");
}

/// @brief 验证成对首拍相位统一补偿多 BPM 时间线和普通内容。
void test_paired_first_delay_round_trips_variable_bpm()
{
    XINFO("=== Test: Paired first delay round trips variable BPM ===");

    constexpr double FIRST_BPM          = 210.0;
    constexpr double SECOND_BPM         = 200.0;
    constexpr double WRAPPED_DELAY_MS   = 237.032272;
    constexpr double FIRST_BEAT_MS      = 60000.0 / FIRST_BPM;
    constexpr double SECOND_BEAT_MS     = 60000.0 / SECOND_BPM;
    constexpr double FIRST_TIMESTAMP_MS = FIRST_BEAT_MS - WRAPPED_DELAY_MS;
    constexpr double SECOND_TIMESTAMP_MS =
        FIRST_TIMESTAMP_MS + 100.0 * FIRST_BEAT_MS;
    constexpr double THIRD_TIMESTAMP_MS =
        SECOND_TIMESTAMP_MS + 10.0 * SECOND_BEAT_MS;

    for ( const int mode : { 0, 7 } ) {
        auto beatMap                             = makeMinimalBeatMap(mode, 4);
        beatMap.m_baseMapMetadata.preference_bpm = FIRST_BPM;
        beatMap.m_baseMapMetadata.song_file_hint = "music.ogg";
        beatMap.m_audioSamples.front().m_audioResourceId = "music.ogg";
        beatMap.m_audioSamples.front().m_timestamp       = 0.0;
        beatMap.m_audioSamples.front().m_offsetMs        = 0;
        auto& firstTiming                                = beatMap.m_timings[0];
        firstTiming.m_timestamp                          = FIRST_TIMESTAMP_MS;
        firstTiming.m_bpm                                = FIRST_BPM;
        firstTiming.m_beat_length                        = FIRST_BEAT_MS;
        firstTiming.m_timingEffectParameter              = FIRST_BPM;

        MMM::Timing& secondTiming            = beatMap.m_timings.emplace_back();
        secondTiming.m_timestamp             = SECOND_TIMESTAMP_MS;
        secondTiming.m_bpm                   = SECOND_BPM;
        secondTiming.m_beat_length           = SECOND_BEAT_MS;
        secondTiming.m_timingEffect          = MMM::TimingEffect::BPM;
        secondTiming.m_timingEffectParameter = SECOND_BPM;

        MMM::Timing& thirdTiming            = beatMap.m_timings.emplace_back();
        thirdTiming.m_timestamp             = THIRD_TIMESTAMP_MS;
        thirdTiming.m_bpm                   = FIRST_BPM;
        thirdTiming.m_beat_length           = FIRST_BEAT_MS;
        thirdTiming.m_timingEffect          = MMM::TimingEffect::BPM;
        thirdTiming.m_timingEffectParameter = FIRST_BPM;

        MMM::Timing& scroll            = beatMap.m_timings.emplace_back();
        scroll.m_timestamp             = THIRD_TIMESTAMP_MS;
        scroll.m_bpm                   = FIRST_BPM;
        scroll.m_beat_length           = 1.25;
        scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
        scroll.m_timingEffectParameter = 1.25;

        MMM::Note& note  = beatMap.m_noteData.notes.emplace_back();
        note.m_type      = MMM::NoteType::NOTE;
        note.m_timestamp = SECOND_TIMESTAMP_MS;
        note.m_track     = 0;
        beatMap.sync();

        const std::string modeName = mode == 0 ? "key" : "slide";
        const fs::path    outputPath =
            std::filesystem::temp_directory_path() /
            ("edge_paired_variable_bpm_" + modeName + ".mc");
        TEST_ASSERT(beatMap.saveToFile(outputPath),
                    "paired variable BPM map should export");

        std::ifstream outputFile(outputPath);
        json          exported;
        outputFile >> exported;
        TEST_ASSERT(exported["time"].size() == 3,
                    "variable BPM export should keep three timings");
        TEST_ASSERT(
            exported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
                exported["time"][1]["beat"] == json::array({ 101, 0, 1 }) &&
                exported["time"][2]["beat"] == json::array({ 111, 0, 1 }),
            "variable BPM timings should share the positive phase shift");
        TEST_ASSERT(std::abs(exported["time"][0].value("delay", 0.0) -
                             WRAPPED_DELAY_MS) < 1e-6,
                    "only the first timing should carry the paired delay");
        TEST_ASSERT(
            (!exported["time"][1].contains("delay") ||
             std::abs(exported["time"][1].value("delay", 0.0)) < 1e-6) &&
                (!exported["time"][2].contains("delay") ||
                 std::abs(exported["time"][2].value("delay", 0.0)) < 1e-6),
            "later BPM timings should not repeat the paired delay");

        const auto exportedSample = std::find_if(
            exported["note"].begin(), exported["note"].end(), isSoundNode);
        TEST_ASSERT(
            exportedSample != exported["note"].end() &&
                (*exportedSample)["beat"] == json::array({ 0, 0, 1 }) &&
                exportedSample->value("offset", -1) == 237,
            "first-half-beat main SOUND should keep its wrapped offset");
        const auto exportedPlayable =
            std::find_if(exported["note"].begin(),
                         exported["note"].end(),
                         [](const json& node) { return !isSoundNode(node); });
        TEST_ASSERT(
            exportedPlayable != exported["note"].end() &&
                (*exportedPlayable)["beat"] == json::array({ 101, 0, 1 }) &&
                exported["effect"].size() == 1 &&
                exported["effect"][0]["beat"] == json::array({ 111, 0, 1 }),
            "all ordinary content should receive the positive phase shift");

        MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
        reloaded.sync();
        TEST_ASSERT(
            reloaded.m_timings.size() == 4,
            "variable BPM round trip should keep BPM and effect timings");
        TEST_ASSERT(
            std::abs(reloaded.m_timings[0].m_timestamp - FIRST_TIMESTAMP_MS) <
                    1e-6 &&
                std::abs(reloaded.m_timings[1].m_timestamp -
                         SECOND_TIMESTAMP_MS) < 1e-6 &&
                std::abs(reloaded.m_timings[2].m_timestamp -
                         THIRD_TIMESTAMP_MS) < 1e-6,
            "variable BPM round trip should preserve every timing timestamp");
        TEST_ASSERT(
            reloaded.m_audioSamples.size() == 1 &&
                std::abs(reloaded.m_audioSamples.front().m_timestamp) < 1e-6 &&
                reloaded.m_audioSamples.front().m_offsetMs == 0,
            "variable BPM round trip should keep main audio normalized");

        reloaded.m_timings[1].m_timestamp += FIRST_BEAT_MS;
        reloaded.m_timings[2].m_timestamp += FIRST_BEAT_MS;
        reloaded.m_timings[3].m_timestamp += FIRST_BEAT_MS;
        reloaded.m_noteData.notes.front().m_timestamp += FIRST_BEAT_MS;
        reloaded.sync();
        const fs::path editedOutputPath =
            std::filesystem::temp_directory_path() /
            ("edge_paired_variable_bpm_edited_" + modeName + ".mc");
        TEST_ASSERT(reloaded.saveToFile(editedOutputPath),
                    "edited imported variable BPM map should export");
        std::ifstream editedOutputFile(editedOutputPath);
        json          editedExported;
        editedOutputFile >> editedExported;
        TEST_ASSERT(
            editedExported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
                editedExported["time"][1]["beat"] ==
                    json::array({ 102, 0, 1 }) &&
                editedExported["time"][2]["beat"] == json::array({ 112, 0, 1 }),
            "edited timings must replace imported beat metadata");
        const auto editedPlayable =
            std::find_if(editedExported["note"].begin(),
                         editedExported["note"].end(),
                         [](const json& node) { return !isSoundNode(node); });
        TEST_ASSERT(
            editedPlayable != editedExported["note"].end() &&
                (*editedPlayable)["beat"] == json::array({ 102, 0, 1 }) &&
                editedExported["effect"].size() == 1 &&
                editedExported["effect"][0]["beat"] ==
                    json::array({ 112, 0, 1 }),
            "edited objects must replace imported beat metadata");

        MMM::BeatMap editedReloaded =
            MMM::BeatMap::loadFromFile(editedOutputPath);
        editedReloaded.sync();
        TEST_ASSERT(
            std::abs(editedReloaded.m_timings[1].m_timestamp -
                     (SECOND_TIMESTAMP_MS + FIRST_BEAT_MS)) < 1e-6 &&
                std::abs(editedReloaded.m_timings[2].m_timestamp -
                         (THIRD_TIMESTAMP_MS + FIRST_BEAT_MS)) < 1e-6,
            "edited variable BPM timings should round trip at new positions");
        TEST_ASSERT(
            std::abs(editedReloaded.m_timings[3].m_timestamp -
                     (THIRD_TIMESTAMP_MS + FIRST_BEAT_MS)) < 1e-6 &&
                std::abs(editedReloaded.m_noteData.notes.front().m_timestamp -
                         (SECOND_TIMESTAMP_MS + FIRST_BEAT_MS)) < 1e-6,
            "edited effect and playable note should round trip at new "
            "positions");
    }

    XINFO("PASS: Paired first delay round trips variable BPM");
}

/// @brief 验证缺少 x 的旧版自动采样按 Malody Pro Editor 规则展开。
void test_legacy_samples_without_x_use_pro_editor_tracks()
{
    XINFO("=== Test: Legacy samples without x use Pro Editor tracks ===");

    const fs::path sourcePath =
        std::filesystem::temp_directory_path() / "edge_legacy_sample_tracks.mc";
    const fs::path exportPath = std::filesystem::temp_directory_path() /
                                "edge_legacy_sample_tracks_export.mc";

    json fileData;
    fileData["meta"] = { { "id", 0 },
                         { "creator", "Test" },
                         { "version", "4K" },
                         { "mode", 0 },
                         { "mode_ext",
                           { { "column", 4 }, { "bar_begin", 0 } } },
                         { "song",
                           { { "title", "LegacySamples" },
                             { "artist", "Test" },
                             { "bpm", 120.0 } } } };
    fileData["time"] = json::array(
        { { { "beat", json::array({ 0, 0, 1 }) }, { "bpm", 120.0 } } });
    fileData["note"] = json::array({ { { "beat", json::array({ 1, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "first.wav" } },
                                     { { "beat", json::array({ 1, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "second.wav" } },
                                     { { "beat", json::array({ 2, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "third.wav" } },
                                     { { "beat", json::array({ 2, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "explicit.wav" },
                                       { "x", 7 } } });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open legacy sample input");
    source << fileData.dump();
    source.close();

    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(loaded.m_baseMapMetadata.track_count == 4,
                "automatic samples must not change playable key count");
    TEST_ASSERT(loaded.m_audioSamples.size() == 4,
                "all legacy samples should load");
    TEST_ASSERT(loaded.m_audioSamples[0].m_track == 10 &&
                    loaded.m_audioSamples[1].m_track == 11,
                "simultaneous legacy samples should expand from track 10");
    TEST_ASSERT(loaded.m_audioSamples[2].m_track == 10,
                "a new trigger time should reuse legacy track 10");
    TEST_ASSERT(loaded.m_audioSamples[3].m_track == 7,
                "an explicit valid x should remain unchanged");
    TEST_ASSERT(loaded.m_baseMapMetadata.bgm_track_count == 8,
                "absolute track 11 after four keys should retain eight BGM "
                "tracks including the gap");

    const auto relocationCount = std::count_if(
        loaded.m_loadDiagnostics.begin(),
        loaded.m_loadDiagnostics.end(),
        [](const MMM::BeatmapLoadDiagnostic& diagnostic) {
            return diagnostic.m_code ==
                   MMM::BeatmapLoadDiagnosticCode::AUDIO_SAMPLE_TRACK_RELOCATED;
        });
    TEST_ASSERT(relocationCount == 1,
                "legacy auto-layout should emit one aggregate diagnostic");

    TEST_ASSERT(loaded.saveToFile(exportPath),
                "legacy auto-layout map should export");
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    reloaded.sync();
    const auto trackForSound = [&](const std::string& sound) {
        const auto sample =
            std::find_if(reloaded.m_audioSamples.begin(),
                         reloaded.m_audioSamples.end(),
                         [&](const MMM::AudioSampleEvent& candidate) {
                             return candidate.m_audioResourceId == sound;
                         });
        return sample == reloaded.m_audioSamples.end()
                   ? std::numeric_limits<std::uint32_t>::max()
                   : sample->m_track;
    };
    TEST_ASSERT(reloaded.m_audioSamples.size() == 4 &&
                    trackForSound("first.wav") == 10 &&
                    trackForSound("second.wav") == 11 &&
                    trackForSound("third.wav") == 10 &&
                    trackForSound("explicit.wav") == 7,
                "canonical x values should preserve the inferred layout");

    XINFO("PASS: Legacy samples without x use Pro Editor tracks");
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
                                       { "vol", -45 } },
                                     { { "beat", json::array({ 2, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "effect.wav" },
                                       { "offset", -20 },
                                       { "x", 2 },
                                       { "vol", -10 } } });

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
    const auto relocationDiagnostic = std::find_if(
        loaded.m_loadDiagnostics.begin(),
        loaded.m_loadDiagnostics.end(),
        [](const MMM::BeatmapLoadDiagnostic& diagnostic) {
            return diagnostic.m_code == MMM::BeatmapLoadDiagnosticCode::
                                            AUDIO_SAMPLE_TRACK_RELOCATED &&
                   diagnostic.m_severity ==
                       MMM::BeatmapLoadDiagnosticSeverity::
                           BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING;
        });
    TEST_ASSERT(relocationDiagnostic != loaded.m_loadDiagnostics.end(),
                "invalid sample x should emit a non-fatal diagnostic");
    TEST_ASSERT(relocationDiagnostic->m_relatedPath ==
                    loaded.m_baseMapMetadata.map_path,
                "sample track diagnostic should identify its source map");

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

/// @brief 验证移动自动采样锚点后不会被导入时缓存的 beat 覆盖。
void testEditedSampleTimestampOverridesImportedBeat()
{
    XINFO("=== Test: Edited sample timestamp overrides imported beat ===");

    const fs::path sourcePath =
        std::filesystem::temp_directory_path() / "edge_sample_move_source.mc";
    const fs::path exportPath =
        std::filesystem::temp_directory_path() / "edge_sample_move_export.mc";

    json fileData;
    fileData["meta"] = { { "creator", "Test" },
                         { "version", "4K" },
                         { "mode", 0 },
                         { "mode_ext", { { "column", 4 } } },
                         { "song",
                           { { "title", "MoveSample" },
                             { "artist", "Test" },
                             { "file", "stem.ogg" },
                             { "bpm", 120.0 } } } };
    fileData["time"] = json::array(
        { { { "beat", json::array({ 0, 0, 1 }) }, { "bpm", 120.0 } } });
    fileData["note"] = json::array({ { { "beat", json::array({ 1, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "stem.ogg" },
                                       { "offset", 0 },
                                       { "x", 4 },
                                       { "vol", 0 } } });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open moved sample input");
    source << fileData.dump();
    source.close();

    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    TEST_ASSERT(loaded.m_audioSamples.size() == 1,
                "moved sample fixture should load one sample");
    loaded.m_audioSamples.front().m_timestamp = 1500.0;

    TEST_ASSERT(loaded.saveToFile(exportPath),
                "map with moved sample should export");
    std::ifstream exportedFile(exportPath);
    json          exported;
    exportedFile >> exported;
    const auto sampleIt = std::find_if(
        exported["note"].begin(), exported["note"].end(), isSoundNode);
    TEST_ASSERT(sampleIt != exported["note"].end(),
                "moved sample should remain in output");
    TEST_ASSERT((*sampleIt)["beat"] == json::array({ 3, 0, 1 }),
                "exported beat should follow the edited sample timestamp");

    const MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    TEST_ASSERT(reloaded.m_audioSamples.size() == 1 &&
                    std::abs(reloaded.m_audioSamples.front().m_timestamp -
                             1500.0) < 1e-6,
                "moved sample timestamp should survive round trip");

    XINFO("PASS: Edited sample timestamp overrides imported beat");
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
    TEST_ASSERT(
        reloaded.m_audioSamples.size() == 1 &&
            std::abs(reloaded.m_audioSamples.front().m_volume - 1.0F) < 1e-6F,
        "missing Malody gain should default to unit volume");

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

/// @brief 验证 MC 时间线与相对 seg 均使用固定分拍候选并保留 1920 精度。
void testMalodyTimelineUsesFixedHighPrecisionFractions()
{
    XINFO("=== Test: Malody timeline fixed high precision fractions ===");
    auto             beatMap        = makeMinimalBeatMap(7 /*Slide*/, 4);
    constexpr double BEAT_LENGTH_MS = 500.0;

    MMM::Timing& scroll            = beatMap.m_timings.emplace_back();
    scroll.m_timestamp             = BEAT_LENGTH_MS * (1.0 + 1919.0 / 1920.0);
    scroll.m_bpm                   = 120.0;
    scroll.m_beat_length           = 1.25;
    scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
    scroll.m_timingEffectParameter = 1.25;

    MMM::Polyline& polyline = beatMap.m_noteData.polylines.emplace_back();
    polyline.m_type         = MMM::NoteType::POLYLINE;
    polyline.m_timestamp    = 0.0;
    polyline.m_track        = 0;
    MMM::Hold& hold         = beatMap.m_noteData.holds.emplace_back();
    hold.m_type             = MMM::NoteType::HOLD;
    hold.m_timestamp        = 0.0;
    hold.m_duration         = BEAT_LENGTH_MS * (287.0 / 288.0);
    hold.m_track            = 0;
    hold.m_isSubNote        = true;
    polyline.m_subNotes.push_back(hold);
    polyline.m_subHolds.push_back(hold);
    beatMap.sync();

    const fs::path outputPath =
        std::filesystem::temp_directory_path() / "edge_malody_fraction_1920.mc";
    TEST_ASSERT(beatMap.saveToFile(outputPath),
                "high precision Malody map should export");
    std::ifstream outputFile(outputPath);
    json          exported;
    outputFile >> exported;

    TEST_ASSERT(
        exported.contains("effect") && exported["effect"].size() == 1 &&
            exported["effect"][0]["beat"] == json::array({ 1, 1919, 1920 }),
        "timeline effect should preserve 1919/1920");
    const auto gameNote = std::find_if(
        exported["note"].begin(), exported["note"].end(), [](const json& node) {
            return !isSoundNode(node);
        });
    TEST_ASSERT(
        gameNote != exported["note"].end() && gameNote->contains("seg") &&
            (*gameNote)["seg"].size() == 1 &&
            (*gameNote)["seg"][0]["beat"] == json::array({ 0, 287, 288 }),
        "relative seg should use the same fixed denominator candidates");
    XINFO("PASS: Malody timeline preserves fixed 1920 precision fractions");
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
    test_slide_mode_7k_8k_uses_skin_compatible_layout();
    test_unsupported_malody_mode_rejected();
    test_key_mode_polyline_exports_key_fields();
    test_key_mode_flick_exports_single_note();
    testKeyAudioNodeUsesNumericType();
    testSlideAudioNodeUsesSoundType();
    test_internal_offset_metadata_not_exported();
    test_empty_version_exports_default_metadata();
    test_sound_track_does_not_expand_key_count();
    test_multiple_sound_objects_round_trip_without_global_shift();
    test_timing_delay_and_sample_offset_round_trip_independently();
    test_non_malody_lead_in_exports_timing_origin_and_audio_compensation();
    test_late_first_timing_prepends_anchor_and_shifts_all_content();
    test_first_timing_delay_unwraps_with_its_bpm();
    test_malody_note_phase_shift_boundaries();
    test_paired_first_delay_round_trips_variable_bpm();
    test_legacy_samples_without_x_use_pro_editor_tracks();
    test_invalid_sample_track_and_song_hint_conflict();
    testEditedSampleTimestampOverridesImportedBeat();
    testStringBpmInNearlyEmptyMapLoads();
    testMetadataOnlyMapLoadsWithDefaults();
    test_original_structure_not_leaked();
    test_hold_stay_at_head_creates_valid_seg();
    testPolylineSubnoteSampleBindingRejected();
    testMalodyTimelineUsesFixedHighPrecisionFractions();

    XINFO("========================================");
    if ( g_failed == 0 ) {
        XINFO("  ALL Malody Edge Case Tests PASSED");
        return 0;
    } else {
        XERROR("  {} Malody Edge Case Tests FAILED", g_failed);
        return 1;
    }
}
