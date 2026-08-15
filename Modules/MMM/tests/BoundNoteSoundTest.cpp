#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Note.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using json   = nlohmann::json;

/// @brief 验证 osu! Note 与 Hold 的 sampleFile 映射到通用绑定音效字段。
/// @return 读写位置和清空回退语义均正确时返回 true。
bool testOsuHitSampleField()
{
    MMM::Note                      note;
    const std::vector<std::string> noteDescription{
        "64", "192", "1000", "1", "2", "1:2:3:70:custom-note.wav"
    };
    note.from_osu_description(noteDescription, 4);
    const auto noteBinding = note.getSampleBinding();
    if ( !noteBinding || noteBinding->m_audioResourceId != "custom-note.wav" ) {
        XERROR("osu! Note hit sample binding was not loaded");
        return false;
    }
    if ( !note.to_osu_description(4).ends_with("1:2:3:70:custom-note.wav") ) {
        XERROR("osu! Note hit sample binding was not saved");
        return false;
    }

    note.clearSampleBinding();
    if ( !note.to_osu_description(4).ends_with("1:2:3:70:") ) {
        XERROR("Cleared osu! Note sample binding was restored from metadata");
        return false;
    }

    MMM::Hold                      hold;
    const std::vector<std::string> holdDescription{
        "192", "192", "1000", "128", "0", "2000:1:2:3:70:custom-hold.wav"
    };
    hold.from_osu_description(holdDescription, 4);
    const auto holdBinding = hold.getSampleBinding();
    if ( !holdBinding || holdBinding->m_audioResourceId != "custom-hold.wav" ) {
        XERROR("osu! Hold hit sample binding was not loaded");
        return false;
    }
    if ( !hold.to_osu_description(4).ends_with(
             "2000:1:2:3:70:custom-hold.wav") ) {
        XERROR("osu! Hold sample binding was not saved as sampleFile");
        return false;
    }
    return true;
}

/// @brief 创建带自定义采样物件的最小 Malody JSON。
/// @return 可由 Malody 加载器读取的 JSON 对象。
json makeMalodyMap()
{
    json map;
    map["meta"] = {
        { "id", 0 },
        { "creator", "Test" },
        { "version", "4K" },
        { "mode", 0 },
        { "mode_ext", { { "column", 4 }, { "bar_begin", 0 } } },
        { "song",
          { { "title", "BoundSound" },
            { "artist", "Test" },
            { "file", "audio.ogg" },
            { "bpm", 120.0 } } },
    };
    map["time"] = json::array({ { { "beat", json::array({ 0, 0, 1 }) },
                                  { "bpm", 120.0 },
                                  { "delay", 0.0 } } });
    map["note"] = json::array({ { { "beat", json::array({ 1, 0, 1 }) },
                                  { "column", 2 },
                                  { "sound", "sample.wav" },
                                  { "vol", -35 } },
                                { { "beat", json::array({ 0, 0, 1 }) },
                                  { "type", 1 },
                                  { "sound", "audio.ogg" },
                                  { "offset", -125 },
                                  { "x", 4 },
                                  { "vol", -20 } } });
    return map;
}

/// @brief 验证玩家命中采样与自动采样在 Malody 和当前 MMM 格式中独立往返。
/// @param outputDirectory 测试输出目录。
/// @return 两种格式均保留通用字段时返回 true。
bool testMalodyAndNativeRoundTrip(const fs::path& outputDirectory)
{
    const fs::path sourcePath = outputDirectory / "bound_sound_source.mc";
    {
        std::ofstream output(sourcePath);
        if ( !output ) {
            XERROR("Failed to open Malody bound sound test input");
            return false;
        }
        output << makeMalodyMap().dump();
    }

    MMM::BeatMap map = MMM::BeatMap::loadFromFile(sourcePath);
    if ( map.m_noteData.notes.size() != 1 ) {
        XERROR("Malody playable note count differs");
        return false;
    }
    const auto sourceBinding = map.m_noteData.notes.front().getSampleBinding();
    if ( !sourceBinding || sourceBinding->m_audioResourceId != "sample.wav" ||
         std::abs(sourceBinding->m_volume - 0.65F) > 1e-6F ) {
        XERROR("Malody playable hit sample was not loaded");
        return false;
    }
    if ( map.m_audioSamples.size() != 1 ||
         map.m_audioSamples.front().m_audioResourceId != "audio.ogg" ||
         map.m_audioSamples.front().m_offsetMs != -125 ||
         map.m_audioSamples.front().m_track != 4 ||
         std::abs(map.m_audioSamples.front().m_volume - 0.8F) > 1e-6F ) {
        XERROR("Malody automatic sample was not loaded independently");
        return false;
    }

    const fs::path malodyOutput = outputDirectory / "bound_sound_export.mc";
    if ( !map.saveToFile(malodyOutput) ) {
        XERROR("Failed to save Malody bound sound test output");
        return false;
    }
    MMM::BeatMap malodyReloaded = MMM::BeatMap::loadFromFile(malodyOutput);
    if ( malodyReloaded.m_noteData.notes.size() != 1 ) {
        XERROR("Malody playable note count changed after round trip");
        return false;
    }
    const auto malodyBinding =
        malodyReloaded.m_noteData.notes.front().getSampleBinding();
    if ( !malodyBinding || malodyBinding->m_audioResourceId != "sample.wav" ||
         std::abs(malodyBinding->m_volume - 0.65F) > 1e-6F ||
         malodyReloaded.m_audioSamples.size() != 1 ||
         malodyReloaded.m_audioSamples.front().m_audioResourceId !=
             "audio.ogg" ||
         malodyReloaded.m_audioSamples.front().m_offsetMs != -125 ||
         malodyReloaded.m_audioSamples.front().m_track != 4 ||
         std::abs(malodyReloaded.m_audioSamples.front().m_volume - 0.8F) >
             1e-6F ) {
        XERROR("Malody sample bindings did not survive round trip");
        return false;
    }

    map.m_audioSamples.front()
        .m_metadata
        .sample_properties[MMM::SampleMetadataType::MMM]["editor_label"] =
        "stem";
    const fs::path nativeOutput = outputDirectory / "bound_sound_export.mmm";
    if ( !map.saveToFile(nativeOutput) ) {
        XERROR("Failed to save native bound sound test output");
        return false;
    }

    json nativeJson;
    {
        std::ifstream input(nativeOutput);
        if ( !input ) {
            XERROR("Failed to inspect native sample output");
            return false;
        }
        input >> nativeJson;
    }
    if ( nativeJson.value("format_version", 0) != 3 ||
         !nativeJson.contains("audio_samples") ||
         nativeJson["audio_samples"].size() != 1 ||
         nativeJson["audio_samples"][0].value("offset_ms", 0) != -125 ||
         nativeJson["audio_samples"][0].value("track", 0) != 4 ||
         nativeJson["audio_samples"][0].value("audio_ref", "") != "audio.ogg" ||
         std::abs(nativeJson["audio_samples"][0].value("volume", 0.0) - 0.8) >
             1e-6 ) {
        XERROR("MMM v3 automatic sample JSON is incomplete");
        return false;
    }
    if ( nativeJson["note"].size() != 1 ||
         !nativeJson["note"][0].contains("sample") ||
         nativeJson["note"][0].contains("bound_sound") ||
         nativeJson["note"][0].contains("bound_volume") ||
         nativeJson["note"][0]["sample"].value("audio_ref", "") !=
             "sample.wav" ||
         std::abs(nativeJson["note"][0]["sample"].value("volume", 0.0) - 0.65) >
             1e-6 ) {
        XERROR("MMM v3 playable sample binding is not canonical");
        return false;
    }

    MMM::BeatMap nativeReloaded = MMM::BeatMap::loadFromFile(nativeOutput);
    if ( nativeReloaded.m_noteData.notes.size() != 1 ) {
        XERROR("Native MMM playable note count changed after round trip");
        return false;
    }
    const auto nativeBinding =
        nativeReloaded.m_noteData.notes.front().getSampleBinding();
    if ( !nativeBinding || nativeBinding->m_audioResourceId != "sample.wav" ||
         std::abs(nativeBinding->m_volume - 0.65F) > 1e-6F ||
         nativeReloaded.m_audioSamples.size() != 1 ||
         nativeReloaded.m_audioSamples.front().m_audioResourceId !=
             "audio.ogg" ||
         nativeReloaded.m_audioSamples.front().m_offsetMs != -125 ||
         nativeReloaded.m_audioSamples.front().m_track != 4 ||
         std::abs(nativeReloaded.m_audioSamples.front().m_volume - 0.8F) >
             1e-6F ||
         nativeReloaded.m_audioSamples.front().m_metadata.getValue<std::string>(
             MMM::SampleMetadataType::MMM, "editor_label") != "stem" ) {
        XERROR("Native MMM sample bindings did not survive round trip");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行跨格式物件绑定音效测试。
/// @param argc 参数数量。
/// @param argv 第一个参数为构建目录下的测试输出目录。
/// @return 全部测试通过时返回 0。
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        XERROR("Usage: BoundNoteSoundTest <output_directory>");
        return 1;
    }

    const fs::path  outputDirectory = argv[1];
    std::error_code filesystemError;
    fs::create_directories(outputDirectory, filesystemError);
    if ( filesystemError ) {
        XERROR("Failed to create bound sound test output directory");
        return 1;
    }

    return testOsuHitSampleField() &&
                   testMalodyAndNativeRoundTrip(outputDirectory)
               ? 0
               : 1;
}
