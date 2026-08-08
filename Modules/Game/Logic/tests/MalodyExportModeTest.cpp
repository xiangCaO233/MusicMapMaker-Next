#include "common/LogicCommands.h"
#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/MalodyPackageCompatibility.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/MalodyMode.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <unordered_set>

namespace
{

/// @brief 创建带单个玩家物件的最小 Malody 测试谱面。
/// @param sourceMode 谱面原始 Malody 模式。
/// @param useFlick 是否创建 Flick；否则创建 Note。
/// @return 可载入逻辑会话的谱面。
std::shared_ptr<MMM::BeatMap> makeBeatmap(MMM::MalodyMode sourceMode,
                                          bool            useFlick)
{
    auto beatMap                           = std::make_shared<MMM::BeatMap>();
    beatMap->m_baseMapMetadata.name        = "MalodyExportModeTest";
    beatMap->m_baseMapMetadata.title       = "MalodyExportModeTest";
    beatMap->m_baseMapMetadata.artist      = "Test";
    beatMap->m_baseMapMetadata.author      = "Test";
    beatMap->m_baseMapMetadata.version     = "Test";
    beatMap->m_baseMapMetadata.track_count = 4;
    beatMap->m_baseMapMetadata.bgm_track_count = 1;
    beatMap->m_baseMapMetadata.preference_bpm  = 120.0;
    beatMap->m_metadata.map_properties[MMM::MapMetadataType::MALODY]["mode"] =
        std::to_string(MMM::malodyModeValue(sourceMode));

    MMM::Timing timing;
    timing.m_timestamp             = 0.0;
    timing.m_bpm                   = 120.0;
    timing.m_beat_length           = 500.0;
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = 120.0;
    beatMap->m_timings.push_back(timing);

    if ( useFlick ) {
        auto& flick       = beatMap->m_noteData.flicks.emplace_back();
        flick.m_type      = MMM::NoteType::FLICK;
        flick.m_timestamp = 1000.0;
        flick.m_track     = 1;
        flick.m_dtrack    = 1;
    } else {
        auto& note       = beatMap->m_noteData.notes.emplace_back();
        note.m_type      = MMM::NoteType::NOTE;
        note.m_timestamp = 1000.0;
        note.m_track     = 1;
    }
    beatMap->sync();
    return beatMap;
}

/// @brief 读取测试导出的 JSON，不使用异常解析路径。
/// @param path 待读取的 MC 文件。
/// @return 成功时返回 JSON，否则返回 discarded JSON。
nlohmann::json readJson(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if ( !file ) return nlohmann::json::parse("", nullptr, false);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    return nlohmann::json::parse(text, nullptr, false, true);
}

/// @brief 通过逻辑命令按指定模式导出当前谱面。
/// @param beatMap 待载入谱面。
/// @param exportMode 用户选择的导出模式。
/// @param outputPath 输出路径。
/// @return 导出文件可解析时返回 true。
bool exportWithMode(const std::shared_ptr<MMM::BeatMap>& beatMap,
                    MMM::MalodyMode                      exportMode,
                    const std::filesystem::path&         outputPath)
{
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);

    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = beatMap },
    });
    session.update(0.0, config, false);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdSaveBeatmapAs{
            .malodyExportMode = exportMode,
            .path             = MMM::Config::pathToUtf8(outputPath),
        },
    });
    session.update(0.0, config, false);
    return !readJson(outputPath).is_discarded();
}

/// @brief 验证 Key 覆盖会转换 Flick 且不污染会话元数据。
/// @return 模式、物件字段和元数据恢复均正确时返回 true。
bool checkKeyOverrideConvertsFlickAndRestoresMetadata()
{
    auto       beatMap    = makeBeatmap(MMM::MalodyMode::Slide, true);
    const auto outputPath = std::filesystem::temp_directory_path() /
                            "mmm_malody_export_mode_key.mc";
    if ( !exportWithMode(beatMap, MMM::MalodyMode::Key, outputPath) ) {
        XERROR("Key mode command export failed");
        return false;
    }

    const auto output = readJson(outputPath);
    const auto propsIt =
        beatMap->m_metadata.map_properties.find(MMM::MapMetadataType::MALODY);
    if ( !output.is_object() || !output.contains("meta") ||
         !output["meta"].is_object() || !output.contains("note") ||
         !output["note"].is_array() ||
         propsIt == beatMap->m_metadata.map_properties.end() ) {
        XERROR("Key mode output structure is invalid");
        return false;
    }
    const auto modeIt = propsIt->second.find("mode");
    if ( modeIt == propsIt->second.end() ) return false;
    bool valid = output["meta"].value("mode", -1) == 0 &&
                 output["meta"].value("free", -1) == 0 && modeIt->second == "7";
    std::size_t gameNoteCount = 0;
    for ( const auto& node : output["note"] ) {
        if ( node.contains("sound") ) continue;
        ++gameNoteCount;
        valid = valid && node.contains("column") && !node.contains("x") &&
                !node.contains("dir") && !node.contains("seg");
    }
    valid = valid && gameNoteCount == 1;

    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);
    if ( !valid ) XERROR("Key mode override did not convert or restore state");
    return valid;
}

/// @brief 验证 Slide 覆盖会写出自由模式字段且不污染会话元数据。
/// @return 模式、物件字段和元数据恢复均正确时返回 true。
bool checkSlideOverrideUsesSlideFieldsAndRestoresMetadata()
{
    auto       beatMap    = makeBeatmap(MMM::MalodyMode::Key, false);
    const auto outputPath = std::filesystem::temp_directory_path() /
                            "mmm_malody_export_mode_slide.mc";
    if ( !exportWithMode(beatMap, MMM::MalodyMode::Slide, outputPath) ) {
        XERROR("Slide mode command export failed");
        return false;
    }

    const auto output = readJson(outputPath);
    const auto propsIt =
        beatMap->m_metadata.map_properties.find(MMM::MapMetadataType::MALODY);
    if ( !output.is_object() || !output.contains("meta") ||
         !output["meta"].is_object() || !output.contains("note") ||
         !output["note"].is_array() ||
         propsIt == beatMap->m_metadata.map_properties.end() ) {
        XERROR("Slide mode output structure is invalid");
        return false;
    }
    const auto modeIt = propsIt->second.find("mode");
    if ( modeIt == propsIt->second.end() ) return false;
    bool valid = output["meta"].value("mode", -1) == 7 &&
                 output["meta"].value("free", -1) == 1 && modeIt->second == "0";
    std::size_t gameNoteCount = 0;
    for ( const auto& node : output["note"] ) {
        if ( node.contains("sound") ) continue;
        ++gameNoteCount;
        valid = valid && node.contains("x") && node.contains("w") &&
                !node.contains("column");
    }
    valid = valid && gameNoteCount == 1;

    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);
    if ( !valid ) XERROR("Slide mode override did not export or restore state");
    return valid;
}

/// @brief 验证 Main 自动采样清理不会影响 Effect 或玩家物件音量。
/// @return 两种自动采样格式都只清理 Main vol 时返回 true。
bool checkMainAudioVolumeCompatibilityPatch()
{
    auto document = nlohmann::json{
        { "note",
          nlohmann::json::array(
              { { { "type", "SOUND" },
                  { "sound", "main-track" },
                  { "vol", 75 } },
                { { "type", 1 }, { "sound", "main-track" }, { "vol", 80 } },
                { { "type", "SOUND" },
                  { "sound", "effect-track" },
                  { "vol", 45 } },
                { { "column", 1 },
                  { "sound", "main-track" },
                  { "vol", 60 } } }) }
    };
    const std::unordered_set<std::string> mainReferences{ "main-track" };

    const auto removed =
        MMM::Logic::stripMalodyMainAudioVolumeFields(document, mainReferences);
    const auto& notes = document["note"];
    const bool  valid = removed == 2 && !notes[0].contains("vol") &&
                        !notes[1].contains("vol") &&
                        notes[2].value("vol", -1) == 45 &&
                        notes[3].value("vol", -1) == 60;
    if ( !valid ) {
        XERROR("Main audio volume compatibility patch changed wrong nodes");
    }
    return valid;
}

}  // namespace

/// @brief 覆盖 MC 导出模式临时覆盖、自动转换与会话元数据恢复。
/// @return 全部场景通过时返回 0。
int main()
{
    return checkKeyOverrideConvertsFlickAndRestoresMetadata() &&
                   checkSlideOverrideUsesSlideFieldsAndRestoresMetadata() &&
                   checkMainAudioVolumeCompatibilityPatch()
               ? 0
               : 1;
}
