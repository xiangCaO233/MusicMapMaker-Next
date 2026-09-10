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

/// @file BoundNoteSoundTest.cpp
/// @brief 验证玩家物件绑定音效与自动采样在各格式中的职责边界。
///
/// 玩家物件绑定描述“击打该物件时播放哪个采样”，自动采样描述“时间线到达
/// 指定位置时播放音频”。二者可能引用不同资源，不能因为 Malody 都使用
/// `sound` 字段而混入同一容器。本测试覆盖 osu! 字符串字段、Malody JSON 与
/// 原生 MMM JSON 三个表示边界。
///
/// osu! 场景验证：
/// - 普通 Note 从 hitSample 的 sampleFile 读取资源名；
/// - 普通 Note 写回时保留 sample set、addition、index 与 volume；
/// - 主动清空绑定后不会从兼容元数据恢复旧资源名；
/// - Hold 在结束时间之后的 hitSample 中读取相同绑定语义；
/// - Hold 写回时仍保持结束时间与 hitSample 的字段边界。
///
/// Malody 场景验证：
/// - 带 column 的 note.sound 进入玩家命中绑定；
/// - 玩家 note.vol 转换为绑定音量；
/// - type=1 的无 column 项进入自动采样列表；
/// - 自动采样保留 sound、offset、x 和 vol；
/// - 两类采样使用不同资源时不会相互覆盖；
/// - 写出并重载后两类采样仍各自只有一项。
///
/// 原生 MMM v3 场景验证：
/// - 顶层 format_version 明确为 3；
/// - 自动采样写入 audio_samples 数组；
/// - 自动采样保留 audio_ref、offset_ms、track 与 volume；
/// - 玩家绑定写入物件的 sample 子对象；
/// - 玩家绑定不再写出旧 bound_sound 平铺键；
/// - 玩家绑定不再写出旧 bound_volume 平铺键；
/// - 自动采样扩展元数据可完成加载往返。
///
/// 这些断言有意同时检查磁盘表示和重载模型。只检查磁盘 JSON 会遗漏加载器
/// 回归，只检查重载模型则可能让读写器的同向错误相互抵消。
///
/// 数值约定：
/// - osu! 的 volume 70 只需随原格式字符串保留；
/// - Malody 玩家音量 -35 对应统一值 0.65；
/// - Malody 自动音量 -20 对应统一值 0.8；
/// - 浮点音量比较使用 1e-6 容差；
/// - 自动采样偏移 -125 必须按有符号毫秒保留；
/// - 四轨谱面的首条 BGM 轨索引为 4。
///
/// 测试生成的 JSON 和谱面仅位于调用者提供的输出目录，可安全重复覆盖。
/// 失败日志按加载、Malody 往返和 MMM 往返三个阶段区分定位。

/// @brief 验证 osu! Note 与 Hold 的 sampleFile 映射到通用绑定音效字段。
/// @return 读写位置和清空回退语义均正确时返回 true。
bool testOsuHitSampleField()
{
    // 输入字段索引依次为 x、y、time、type、hitSound、hitSample。
    // `1:2:3:70` 是非默认前缀，可确认保存器没有为简化资源名而重置其他值。
    // 普通 Note 的 hitSample 尾字段携带资源名，前四个数值字段必须原样保留。
    MMM::Note                      note;
    const std::vector<std::string> noteDescription{
        "64", "192", "1000", "1", "2", "1:2:3:70:custom-note.wav"
    };
    note.from_osu_description(noteDescription, 4);
    // 加载断言验证资源身份进入通用绑定，而不是只停留在 osu! 私有元数据中。
    const auto noteBinding = note.getSampleBinding();
    if ( !noteBinding || noteBinding->m_audioResourceId != "custom-note.wav" ) {
        XERROR("osu! Note hit sample binding was not loaded");
        return false;
    }
    // 保存断言同时覆盖 bank、addition、index 与 volume 前缀未被资源名破坏。
    if ( !note.to_osu_description(4).ends_with("1:2:3:70:custom-note.wav") ) {
        XERROR("osu! Note hit sample binding was not saved");
        return false;
    }

    // 清空通用绑定后不允许从旧私有元数据“复活”已经删除的资源名。
    note.clearSampleBinding();
    if ( !note.to_osu_description(4).ends_with("1:2:3:70:") ) {
        XERROR("Cleared osu! Note sample binding was restored from metadata");
        return false;
    }

    // Hold 把结束时间放在 hitSample 前面，需单独验证分隔位置。
    MMM::Hold                      hold;
    const std::vector<std::string> holdDescription{
        "192", "192", "1000", "128", "0", "2000:1:2:3:70:custom-hold.wav"
    };
    // Hold 的 hitSample 前有 `endTime:`，故不能复用普通 Note 的尾段下标假设。
    hold.from_osu_description(holdDescription, 4);
    // Hold 与 Note 应共享绑定语义，但仍保留各自格式字符串的结构。
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
    // 夹具只包含一个 BPM、一个玩家物件和一个自动采样，任何数量变化都能直接
    // 定位为分类错误，而不会被复杂谱面中的其他对象掩盖。
    json map;
    // meta.song.file 提供项目主音频身份，mode_ext 固定四个玩家轨道。
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
    // 零拍 BPM 为后续拍位置转换提供确定的 120 BPM 基准。
    map["time"] = json::array({ { { "beat", json::array({ 0, 0, 1 }) },
                                  { "bpm", 120.0 },
                                  { "delay", 0.0 } } });
    // 第一项有 column，表示玩家 Note；sound 与 vol 应成为命中采样绑定。
    // 第二项 type=1 且没有 column，表示自动 BGM 采样，offset、x 与 vol 应进入
    // AudioSampleEvent，而不能绑定到第一项玩家物件。
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
    // 两项的拍位置不同不是测试重点，但可防止排序后依赖原数组索引得出结论。
    return map;
}

/// @brief 验证玩家命中采样与自动采样在 Malody 和当前 MMM 格式中独立往返。
/// @param outputDirectory 测试输出目录。
/// @return 两种格式均保留通用字段时返回 true。
bool testMalodyAndNativeRoundTrip(const fs::path& outputDirectory)
{
    // 输入和两种输出都位于构建目录，由调用者保证目录存在。
    const fs::path sourcePath = outputDirectory / "bound_sound_source.mc";
    {
        std::ofstream output(sourcePath);
        if ( !output ) {
            XERROR("Failed to open Malody bound sound test input");
            return false;
        }
        // 直接序列化程序化夹具，测试不依赖额外受版本控制的数据文件。
        output << makeMalodyMap().dump();
    }

    // 第一层验证 Malody 加载：玩家物件数量及其绑定必须独立于自动采样。
    MMM::BeatMap map = MMM::BeatMap::loadFromFile(sourcePath);
    if ( map.m_noteData.notes.size() != 1 ) {
        XERROR("Malody playable note count differs");
        return false;
    }
    // Malody 负分贝值会转换为统一模型的线性百分比表示。
    const auto sourceBinding = map.m_noteData.notes.front().getSampleBinding();
    if ( !sourceBinding || sourceBinding->m_audioResourceId != "sample.wav" ||
         std::abs(sourceBinding->m_volume - 0.65F) > 1e-6F ) {
        XERROR("Malody playable hit sample was not loaded");
        return false;
    }
    // 0.65 与输入 -35 的约定对应，使用小容差吸收 float 表示误差。
    // 自动采样保留资源、负偏移、BGM 轨位置与独立音量。
    if ( map.m_audioSamples.size() != 1 ||
         map.m_audioSamples.front().m_audioResourceId != "audio.ogg" ||
         map.m_audioSamples.front().m_offsetMs != -125 ||
         map.m_audioSamples.front().m_track != 4 ||
         std::abs(map.m_audioSamples.front().m_volume - 0.8F) > 1e-6F ) {
        XERROR("Malody automatic sample was not loaded independently");
        return false;
    }
    // 自动采样轨道 4 位于四个玩家轨之后，证明 x 没有被误当作玩家 column。

    // 第二层执行 Malody 自身往返，防止保存器把通用字段写回错误 JSON 节点。
    const fs::path malodyOutput = outputDirectory / "bound_sound_export.mc";
    if ( !map.saveToFile(malodyOutput) ) {
        XERROR("Failed to save Malody bound sound test output");
        return false;
    }
    // 重新加载导出文件后只观察公开模型，不依赖 JSON 键排序或格式化文本。
    MMM::BeatMap malodyReloaded = MMM::BeatMap::loadFromFile(malodyOutput);
    if ( malodyReloaded.m_noteData.notes.size() != 1 ) {
        XERROR("Malody playable note count changed after round trip");
        return false;
    }
    // 单个复合断言要求两类采样同时存活，避免只修复其中一条路径也误报通过。
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

    // 给自动采样附加原生 MMM 专属元数据，验证通用字段之外的扩展映射。
    map.m_audioSamples.front()
        .m_metadata
        .sample_properties[MMM::SampleMetadataType::MMM]["editor_label"] =
        "stem";
    // 第三层把同一统一模型写入原生格式，目标是验证 v3 的规范字段结构。
    const fs::path nativeOutput = outputDirectory / "bound_sound_export.mmm";
    if ( !map.saveToFile(nativeOutput) ) {
        XERROR("Failed to save native bound sound test output");
        return false;
    }

    // 先检查磁盘 JSON 形状，再通过加载器检查语义，区分写错键名与读写相互
    // 抵消的对称错误。
    json nativeJson;
    {
        std::ifstream input(nativeOutput);
        if ( !input ) {
            XERROR("Failed to inspect native sample output");
            return false;
        }
        input >> nativeJson;
    }
    // 自动采样必须位于顶层 audio_samples，完整携带资源、偏移、轨道与音量。
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
    // 玩家绑定使用单一 sample 对象；旧版平铺键不能继续出现在规范 v3 输出中。
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
    // 同时拒绝旧键能保证新保存器始终产生唯一的规范表示。

    // 最终重载验证规范 JSON 能恢复相同模型，包括自动采样的扩展标签。
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
    // editor_label 是格式专属扩展字段，与公共自动采样字段一起完成往返。
    return true;
}
}  // namespace

/// @brief 运行跨格式物件绑定音效测试。
/// @param argc 参数数量。
/// @param argv 第一个参数为构建目录下的测试输出目录。
/// @return 全部测试通过时返回 0。
int main(int argc, char** argv)
{
    // 输出目录是唯一运行参数，避免测试把临时文件写入当前目录或源码树。
    if ( argc < 2 ) {
        XERROR("Usage: BoundNoteSoundTest <output_directory>");
        return 1;
    }

    const fs::path  outputDirectory = argv[1];
    std::error_code filesystemError;
    // 使用 error_code 将目录创建失败转为明确测试结果，不依赖异常机制。
    fs::create_directories(outputDirectory, filesystemError);
    if ( filesystemError ) {
        XERROR("Failed to create bound sound test output directory");
        return 1;
    }

    // 短路执行让 osu! 基础绑定失败时不再生成无关的跨格式产物。
    return testOsuHitSampleField() &&
                   testMalodyAndNativeRoundTrip(outputDirectory)
               ? 0
               : 1;
}
