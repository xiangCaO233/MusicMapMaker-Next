#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Note.h"

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
    if ( note.m_boundSound != "custom-note.wav" ) {
        XERROR("osu! Note sampleFile was not loaded into m_boundSound");
        return false;
    }
    if ( !note.to_osu_description(4).ends_with("1:2:3:70:custom-note.wav") ) {
        XERROR("osu! Note m_boundSound was not saved as sampleFile");
        return false;
    }

    note.m_boundSound.clear();
    if ( !note.to_osu_description(4).ends_with("1:2:3:70:") ) {
        XERROR("Cleared osu! Note sampleFile was restored from metadata");
        return false;
    }

    MMM::Hold                      hold;
    const std::vector<std::string> holdDescription{
        "192", "192", "1000", "128", "0", "2000:1:2:3:70:custom-hold.wav"
    };
    hold.from_osu_description(holdDescription, 4);
    if ( hold.m_boundSound != "custom-hold.wav" ) {
        XERROR("osu! Hold sampleFile was not loaded into m_boundSound");
        return false;
    }
    if ( !hold.to_osu_description(4).ends_with(
             "2000:1:2:3:70:custom-hold.wav") ) {
        XERROR("osu! Hold m_boundSound was not saved as sampleFile");
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
                                  { "sound", "sample.wav" } },
                                { { "beat", json::array({ 0, 0, 1 }) },
                                  { "type", "SOUND" },
                                  { "sound", "audio.ogg" },
                                  { "offset", 0 } } });
    return map;
}

/// @brief 验证 Malody sound 与原生 MMM bound_sound 的往返保存。
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
    if ( map.m_noteData.notes.size() != 1 ||
         map.m_noteData.notes.front().m_boundSound != "sample.wav" ) {
        XERROR("Malody playable sound was not loaded into m_boundSound");
        return false;
    }

    const fs::path malodyOutput = outputDirectory / "bound_sound_export.mc";
    if ( !map.saveToFile(malodyOutput) ) {
        XERROR("Failed to save Malody bound sound test output");
        return false;
    }
    MMM::BeatMap malodyReloaded = MMM::BeatMap::loadFromFile(malodyOutput);
    if ( malodyReloaded.m_noteData.notes.size() != 1 ||
         malodyReloaded.m_noteData.notes.front().m_boundSound !=
             "sample.wav" ) {
        XERROR("Malody bound sound did not survive round trip");
        return false;
    }

    const fs::path nativeOutput = outputDirectory / "bound_sound_export.mmm";
    if ( !map.saveToFile(nativeOutput) ) {
        XERROR("Failed to save native bound sound test output");
        return false;
    }
    MMM::BeatMap nativeReloaded = MMM::BeatMap::loadFromFile(nativeOutput);
    if ( nativeReloaded.m_noteData.notes.size() != 1 ||
         nativeReloaded.m_noteData.notes.front().m_boundSound !=
             "sample.wav" ) {
        XERROR("Native MMM bound sound did not survive round trip");
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
