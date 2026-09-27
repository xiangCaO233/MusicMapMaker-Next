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

// 测试覆盖另存为命令的模式覆盖和生成 JSON，不重新导入文件，也不运行 Malody。
// 临时文件使用固定测试名称；运行前应确认这些路径不承载需要保留的文件。
namespace
{

/// @brief 创建带单个玩家物件的最小 Malody 测试谱面。
/// @param sourceMode 谱面原始 Malody 模式。
/// @param useFlick 是否创建 Flick；否则创建 Note。
/// @return 可载入逻辑会话的谱面。
/// @note 各场景独立创建模型，避免前一场景的导出模式影响后续断言。
/// @note 夹具只包含一条 BPM 与一个玩家物件，不覆盖多段变速和复合物件转换。
std::shared_ptr<MMM::BeatMap> makeBeatmap(MMM::MalodyMode sourceMode,
                                          bool            useFlick)
{
    // 元数据给出有效四轨布局，BGM 轨道与玩家物件轨道分别配置。
    auto beatMap                           = std::make_shared<MMM::BeatMap>();
    beatMap->m_baseMapMetadata.name        = "MalodyExportModeTest";
    beatMap->m_baseMapMetadata.title       = "MalodyExportModeTest";
    beatMap->m_baseMapMetadata.artist      = "Test";
    beatMap->m_baseMapMetadata.author      = "Test";
    beatMap->m_baseMapMetadata.version     = "Test";
    beatMap->m_baseMapMetadata.track_count = 4;
    beatMap->m_baseMapMetadata.bgm_track_count = 1;
    beatMap->m_baseMapMetadata.preference_bpm  = 120.0;
    // 偏好 BPM 与红线一致，避免默认节奏回退掩盖模式转换本身的问题。
    // 来源模式保存在格式扩展元数据中，导出完成后应恢复这份原始值。
    beatMap->m_metadata.map_properties[MMM::MapMetadataType::MALODY]["mode"] =
        std::to_string(MMM::malodyModeValue(sourceMode));

    // 零点 120 BPM 使物件位于确定的整拍上，模式转换不需要处理额外时间偏移。
    MMM::Timing timing;
    timing.m_timestamp   = 0.0;
    timing.m_bpm         = 120.0;
    timing.m_beat_length = 500.0;
    // 持久化时间以毫秒表示，1000 ms 的物件恰好落在零点后的两拍。
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = 120.0;
    beatMap->m_timings.push_back(timing);

    if ( useFlick ) {
        // 选择非零滑动跨度，使 Key 导出必须处理无法直接保留的 Slide 语义。
        auto& flick       = beatMap->m_noteData.flicks.emplace_back();
        flick.m_type      = MMM::NoteType::FLICK;
        flick.m_timestamp = 1000.0;
        flick.m_track     = 1;
        flick.m_dtrack    = 1;
        // 只检查降级后的字段集合，不在此断言横向滑动的视觉表现。
    } else {
        // 普通 Note 用于反向转换，测试自由位置字段而不混入折线或长条分段。
        auto& note       = beatMap->m_noteData.notes.emplace_back();
        note.m_type      = MMM::NoteType::NOTE;
        note.m_timestamp = 1000.0;
        note.m_track     = 1;
        // 不使用首末轨道，避免把边界裁剪与正常位置转换混入同一场景。
    }
    // 完成容器填充后同步派生索引，让会话看到完整物件集合。
    beatMap->sync();
    return beatMap;
}

/// @brief 读取测试导出的 JSON，不使用异常解析路径。
/// @param path 待读取的 MC 文件。
/// @return 成功时返回 JSON，否则返回 discarded JSON。
/// @note 解析失败通过 JSON 状态返回，不把文件内容错误转成测试进程异常。
/// @note 这是测试辅助读取，不承担导入器的格式诊断或字段校验职责。
nlohmann::json readJson(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    // 打不开文件与语法错误统一映射为 discarded，由上层报告导出失败。
    if ( !file ) return nlohmann::json::parse("", nullptr, false);
    // 按原始字节读取，不做平台换行转换；JSON 解析器负责解释文本结构。
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    // 允许注释与实际导出读取习惯一致，但不因此跳过模式和物件字段断言。
    return nlohmann::json::parse(text, nullptr, false, true);
}

/// @brief 通过逻辑命令按指定模式导出当前谱面。
/// @param beatMap 待载入谱面。
/// @param exportMode 用户选择的导出模式。
/// @param outputPath 输出路径。
/// @return 导出文件可解析时返回 true。
/// @pre outputPath 是本测试可删除和覆盖的临时文件，不得指向用户谱面。
/// @note 可解析不代表导出语义正确；模式及对象字段由各场景继续检查。
bool exportWithMode(const std::shared_ptr<MMM::BeatMap>& beatMap,
                    MMM::MalodyMode                      exportMode,
                    const std::filesystem::path&         outputPath)
{
    std::error_code removeError;
    // 去掉上轮产物，防止本次保存失败却读到旧 JSON 而误判成功。
    // 当前辅助函数不单独断言删除错误，因此调用前确认路径归属仍然必要。
    std::filesystem::remove(outputPath, removeError);

    // 使用局部会话隔离命令队列，经由真实保存命令而非直接调用序列化器。
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    // 默认配置配合零时间步运行，测试不依赖界面设置或播放时钟推进。
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = beatMap },
    });
    session.update(0.0, config, false);
    // 先消费加载命令，再发另存为命令，避免保存空会话。
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdSaveBeatmapAs{
            // 模式是本次导出的显式选择，不应永久替换源模型的格式元数据。
            .malodyExportMode = exportMode,
            .path             = MMM::Config::pathToUtf8(outputPath),
        },
    });
    session.update(0.0, config, false);
    // 以最终磁盘产物为证据，不把命令入队本身当作保存成功。
    // 返回之前局部会话仍存活；外部共享模型在辅助函数退出后继续用于恢复检查。
    return !readJson(outputPath).is_discarded();
}

/// @brief 验证 Key 覆盖会转换 Flick 且不污染会话元数据。
/// @return 模式、物件字段和元数据恢复均正确时返回 true。
/// @note 不逐字段对比源模型，只检查与模式覆盖直接相关的来源 mode 恢复。
bool checkKeyOverrideConvertsFlickAndRestoresMetadata()
{
    auto beatMap = makeBeatmap(MMM::MalodyMode::Slide, true);
    // 源 Slide 配 Flick，目标 Key，刻意避免同模式导出掩盖转换遗漏。
    const auto outputPath = std::filesystem::temp_directory_path() /
                            "mmm_malody_export_mode_key.mc";
    // 固定 .mc 扩展名选择 Malody 输出；这不是打包成 .mcz 的资源包测试。
    if ( !exportWithMode(beatMap, MMM::MalodyMode::Key, outputPath) ) {
        // 产物无法解析时立即失败，不继续把缺失字段解释为普通模式不匹配。
        XERROR("Key mode command export failed");
        return false;
    }

    const auto output = readJson(outputPath);
    // 输出文件与源模型分别取证：目标应为 Key，源模式仍应为 Slide。
    const auto propsIt =
        beatMap->m_metadata.map_properties.find(MMM::MapMetadataType::MALODY);
    if ( !output.is_object() || !output.contains("meta") ||
         // 先核对结构再读取字段，区分不可用文档与字段值不符合预期。
         !output["meta"].is_object() || !output.contains("note") ||
         !output["note"].is_array() ||
         propsIt == beatMap->m_metadata.map_properties.end() ) {
        XERROR("Key mode output structure is invalid");
        return false;
    }
    const auto modeIt = propsIt->second.find("mode");
    // 来源属性缺失也是恢复失败，不能用默认模式填补后继续通过断言。
    if ( modeIt == propsIt->second.end() ) return false;
    // Key 编码为 mode=0/free=0；源 Slide 的字符串模式值应仍为 7。
    bool valid = output["meta"].value("mode", -1) == 0 &&
                 output["meta"].value("free", -1) == 0 && modeIt->second == "7";
    std::size_t gameNoteCount = 0;
    // 布尔累积保留前面的模式错误，不能让后一个合法物件覆盖已有失败。
    for ( const auto& node : output["note"] ) {
        // 此夹具没有绑定音效的玩家物件，sound 节点在这里按采样排除。
        if ( node.contains("sound") ) continue;
        ++gameNoteCount;
        // Key 使用离散 column，不应残留自由坐标、滑向或分段字段。
        valid = valid && node.contains("column") && !node.contains("x") &&
                !node.contains("dir") && !node.contains("seg");
    }
    valid = valid && gameNoteCount == 1;
    // 数量也要检查，防止丢掉 Flick 后因空循环而误判字段均正确。

    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);
    // 正常验证路径清理产物；提前失败路径可能保留文件供诊断。
    if ( !valid ) XERROR("Key mode override did not convert or restore state");
    // 清理错误不参与模式断言，返回值只报告当前回归所关注的导出语义。
    return valid;
}

/// @brief 验证 Slide 覆盖会写出自由模式字段且不污染会话元数据。
/// @return 模式、物件字段和元数据恢复均正确时返回 true。
bool checkSlideOverrideUsesSlideFieldsAndRestoresMetadata()
{
    // 源 Key 的单 Note 转到 Slide，用最小物件验证自由模式的基础字段。
    auto       beatMap    = makeBeatmap(MMM::MalodyMode::Key, false);
    const auto outputPath = std::filesystem::temp_directory_path() /
                            "mmm_malody_export_mode_slide.mc";
    // 与 Key 场景使用不同产物路径，两种模式不会互相覆盖验证证据。
    if ( !exportWithMode(beatMap, MMM::MalodyMode::Slide, outputPath) ) {
        // 独立执行反向覆盖，不能沿用上一场景已转换过的谱面。
        XERROR("Slide mode command export failed");
        return false;
    }

    const auto output = readJson(outputPath);
    // 再次读取实际产物，不复用保存辅助函数里的解析对象来掩盖路径差异。
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
    // Slide 编码为 mode=7/free=1；模型中的来源 Key 仍保留为字符串 0。
    bool valid = output["meta"].value("mode", -1) == 7 &&
                 output["meta"].value("free", -1) == 1 && modeIt->second == "0";
    std::size_t gameNoteCount = 0;
    for ( const auto& node : output["note"] ) {
        if ( node.contains("sound") ) continue;
        ++gameNoteCount;
        // 自由模式以位置和宽度表达物件，不能同时输出 Key 的 column。
        // 这里只断言字段存在性，不固定位置量化和宽度数值的实现细节。
        valid = valid && node.contains("x") && node.contains("w") &&
                !node.contains("column");
    }
    valid = valid && gameNoteCount == 1;

    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);
    if ( !valid ) XERROR("Slide mode override did not export or restore state");
    // 输出模式成功与源元数据恢复需要同时成立，二者不互相替代。
    return valid;
}

/// @brief 验证 Main 自动采样清理不会影响 Effect 或玩家物件音量。
/// @return 两种自动采样格式都只清理 Main vol 时返回 true。
/// @note 此例直接检查兼容性补丁，不经文件保存，聚焦节点分类与字段删除范围。
bool checkMainAudioVolumeCompatibilityPatch()
{
    // 前两项分别使用字符串 SOUND 和数值 1 两种自动采样类型表示。
    // 第三项是其他音频，第四项是绑定主音频的玩家物件，都必须保留音量。
    // 四项音量彼此不同，便于精确定位被错误覆盖或删除的节点。
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
    // 明确主音频引用集合，不能仅凭节点存在 sound 字段就移除音量。
    // 相同 sound 引用也不代表相同节点种类，玩家物件仍应保留局部音量。

    const auto removed =
        MMM::Logic::stripMalodyMainAudioVolumeFields(document, mainReferences);
    const auto& notes = document["note"];
    // 原地补丁不应改变数组顺序，按夹具索引核对两类保留节点。
    // 同时检查移除计数和各节点结果，防止误删与漏删数量碰巧抵消。
    const bool valid = removed == 2 && !notes[0].contains("vol") &&
                       !notes[1].contains("vol") &&
                       notes[2].value("vol", -1) == 45 &&
                       notes[3].value("vol", -1) == 60;
    if ( !valid ) {
        // 不把 Main 自动采样去音量推广到整个谱面，错误信息强调误改节点的风险。
        XERROR("Main audio volume compatibility patch changed wrong nodes");
    }
    return valid;
}

}  // namespace

/// @brief 覆盖 MC 导出模式临时覆盖、自动转换与会话元数据恢复。
/// @return 全部场景通过时返回 0。
/// @note 前两个场景写临时 MC，第三个场景只修改内存 JSON。
/// @note 短路返回首个失败，未执行的后续场景不能算作已经通过。
int main()
{
    // 两个方向都覆盖，防止只实现 Slide 到 Key 而漏掉反向的自由字段生成。
    // 最后验证音量补丁，与模式字段转换保持独立的失败定位。
    return checkKeyOverrideConvertsFlickAndRestoresMetadata() &&
                   checkSlideOverrideUsesSlideFieldsAndRestoresMetadata() &&
                   checkMainAudioVolumeCompatibilityPatch()
               ? 0
               : 1;
}
