#include "FormatTestHelpers.hpp"
#include "TestHelper.hpp"
#include "config/Utf8Path.h"
#include "mmm/beatmap/BeatMap.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

/**
 * @file OSUConsistencyTest.cpp
 * @brief 对单个 osu! 谱面执行文本章节和领域模型两层往返检查。
 *
 * 第一层比较七个主要文本章节，第二层使用共享帮助器比较重新加载后的通用谱面
 * 数据。两层必须同时通过；生成文件由调用方放入构建测试输出目录，来源只读。
 */

/// @brief 当前文本帮助器检查的 osu! 主要章节数量。
static constexpr int TOTAL_OSU_SECTIONS = 7;

/// @brief 用同一谱面覆盖普通单键和同时含有组合标志的长条。
/// @param output 当前用例的输出文件，用作夹具的命名空间。
/// @return 两类物件各自落入正确容器，长条持续时间保持不变。
/// @details 原始夹具使用整数位标志，长条和单键都有新增组合位，
/// 以证明只有长条位决定分类。
static bool checkHoldTypeFlags(const std::filesystem::path& output)
{
    auto fixture = output;
    // 保留 osu 扩展名，让生产加载器按扩展名进入相同的格式分派。
    fixture.replace_extension(".hold-flags.osu");
    {
        // 每个调用以自己的输出名生成夹具，三个 CTest 可以并行运行。
        // 用合法时间点及四键模式限定场景，避免其他格式回退影响分类。
        std::ofstream file(fixture);
        file << "osu file format v14\n[General]\nMode:3\n"
                "[Metadata]\nTitle:hold-flags\n[Difficulty]\nCircleSize:4\n"
                "[TimingPoints]\n0,500,4,2,0,100,1,0\n[HitObjects]\n"
                "64,192,1000,132,0,2000:0:0:0:0:\n"
                "192,192,1500,5,0,0:0:0:0:\n";
        if ( !file ) return false;
    }
    // 保存前检查模型，防止两次加载都错误地丢弃长条时往返测试仍通过。
    const auto      map = MMM::BeatMap::loadFromFile(fixture);
    std::error_code error;
    // 保留目录内已有测试输出，只移除本用例生成的输入。
    std::filesystem::remove(fixture, error);
    return map.m_noteData.holds.size() == 1 &&
           map.m_noteData.holds.front().m_duration == 1000.0 &&
           map.m_noteData.notes.size() == 1;
}

/// @brief 验证数字和文字休息段的时间与顺序经过实际保存后仍然完整。
/// @param output 当前测试输出名，两个夹具都在同一测试输出目录。
/// @return 首次加载和再次打开均获得原区间时返回 true。
/// @details 每种事件名只出现一次，用不同的区间区分顺序反转和默认归零。
static bool checkBreakRoundTrip(const std::filesystem::path& output)
{
    // 从同一测试输出派生两个文件名，避免导出时覆盖用来对照的源。
    auto fixture = output;
    fixture.replace_extension(".break-source.osu");
    // 导出仍使用 osu 扩展名，两次读取走同一正式分派。
    auto exported = output;
    exported.replace_extension(".break-export.osu");
    constexpr auto BREAKS = "2,1500,2500\nBreak,3500,4500\n";
    {
        std::ofstream file(fixture);
        // 混用两个合法事件名，保持各自原文并核对同一顺序。
        // 两个区间都位于首个物件之前，便于核对导出章节。
        // 输入含有效时间点与物件，防止空谱面的拒绝分支提前终止测试。
        file
            << "osu file format v14\n[General]\nMode:3\n"
               "[Metadata]\nTitle:break-roundtrip\n[Difficulty]\nCircleSize:4\n"
               "[Events]\n"
            << BREAKS
            << "[TimingPoints]\n0,500,4,2,0,100,1,0\n[HitObjects]\n"
               "64,192,5000,1,0,0:0:0:0:\n";
        if ( !file ) return false;
    }
    auto map = MMM::BeatMap::loadFromFile(fixture);
    // 使用只读查询，缺失的元数据不能在断言阶段被默认插入后掩盖。
    const auto breaksMatch = [=](const MMM::BeatMap& beatmap) {
        const auto& properties = beatmap.m_metadata.map_properties;
        const auto  osu        = properties.find(MMM::MapMetadataType::OSU);
        if ( osu == properties.end() ) return false;
        // 完整文本比较才能检出归零和事件调换，条数可能仍正确。
        const auto breaks = osu->second.find("Events::breaks");
        return breaks != osu->second.end() && breaks->second == BREAKS;
    };
    // 同时核查首次导入和再次打开的结果，避免将错误输出误当作新基准。
    // sync 与正常导出调用保持一致，更新通用物件元数据和时长。
    // 再次打开时使用真实落盘的数据，避免在同一内存对象上做无效往返。
    map.sync();
    const bool preserved = breaksMatch(map) && map.saveToFile(exported) &&
                           breaksMatch(MMM::BeatMap::loadFromFile(exported));
    std::error_code error;
    std::filesystem::remove(fixture, error);
    // 两个夹具都只归此用例所有，保留调用方提供的主输入和正式测试输出。
    std::filesystem::remove(exported, error);
    return preserved;
}

/// @brief 生成实际文件，显式检查关闭状态，防止不完整夹具误报解析成功。
/// @param path 由用例输出路径派生的独立物理文件。
/// @param content 精确字节，允许 CRLF、BOM 和空输入这些专门的边界样本。
/// @return 包含 close 在内的所有写入成功时返回真。
bool writeFixture(const std::filesystem::path& path, std::string_view content)
{
    // 二进制模式避免 Windows 额外转换换行，保留夹具的原始结构。
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
    // 显式关闭让底层 I/O 失败传播到用例，不会拿上一次留下的字节继续验证。
    file.close();
    return file.good();
}

/// @brief 用生产加载入口验证有效签名、无签名首节及损坏版本的行为。
/// @param output 同一 CTest 注册提供的输出基名，各谱面用例可并行。
/// @return 签名状态及首节数据均正确、遇到无效文件返回空图时为真。
/// @details 用真实文件覆盖有签名和无签名两条读入路径。
/// 读取只能出现在首节的字段，防止猜测签名时误丢首个节标题。
bool checkHeaderAndBounds(const std::filesystem::path& output)
{
    auto path = output;
    path.replace_extension(".header.osu");
    // 各输入共用首节内容，以便把差异明确归因于首行处理。
    const std::string sections =
        "[General]\r\nAudioFilename:test.wav\r\nMode:3\r\n"
        "[Metadata]\r\nTitle:header\r\n[Difficulty]\r\nCircleSize:4\r\n";
    // 同一判定覆盖标准签名、无签名的第一节和不被识别的普通文本行。
    const auto valid = [&](const std::string& input, bool hasHeader) {
        if ( !writeFixture(path, input) ) return false;
        // 读取的是实际分派后的谱面，只有解析入口能填充来源属性和歌曲提示。
        const auto map = MMM::BeatMap::loadFromFile(path);
        // 使用已有属性迭代器。不能使用会插入新键的下标访问填补缺失元数据。
        const auto osu =
            map.m_metadata.map_properties.find(MMM::MapMetadataType::OSU);
        if ( osu == map.m_metadata.map_properties.end() ) return false;
        // 没有签名时根本不应赋值；检查不存在与默认 v14 的结果不同。
        const auto version = osu->second.find("file_format_version");
        // 首节上的 AudioFilename 证明没有因探测首行而掉失该节。
        return map.m_baseMapMetadata.song_file_hint == "test.wav" &&
               (hasHeader
                    ? version != osu->second.end() && version->second == "v14"
                    : version == osu->second.end());
    };
    // 普通文本 format 曾误触发越界；整行不能作为版本，后续节必须保留。
    if ( !valid("\xEF\xBB\xBFosu file format v14\r\n" + sections, true) ||
         !valid(sections, false) || !valid("format\r\n" + sections, false) )
        return false;

    // 遍历显式截断版本、畸形版本和空文件。失败返回空路径而非默认有效谱面。
    // 损坏声明和空内容都应拒绝，但无签名的正常节区仍能加载。
    // 截断样本不带后续行，暴露此前从 format 位置向后跨越长度的错误。
    for ( const auto malformed : { "osu file format",
                                   "osu file format v",
                                   "osu file format v14broken",
                                   "" } ) {
        if ( !writeFixture(path, malformed) ||
             !MMM::BeatMap::loadFromFile(path)
                  .m_baseMapMetadata.map_path.empty() )
            return false;
    }
    std::error_code ignored;
    // 夹具文件与测试套件正式输出分开，此处只删除自己重复使用的输入。
    std::filesystem::remove(path, ignored);
    return true;
}

/// @brief 对 1K、4K、7K 和密集轨道检查边界、无效物件及长条往返。
/// @param output 用于派生独立输入和重开文件，避免覆盖资源原件。
/// @return 所有模式的单键和长条都在原轨往返、非法坐标未进入模型时为真。
/// @details 起止两轨各有独立时刻和时长，用对象属性而非总数确认映射。
/// 非法物件和异常键数分别覆盖单行跳过与整图拒绝。
bool checkManiaTracks(const std::filesystem::path& output)
{
    auto input = output;
    input.replace_extension(".tracks.osu");
    // 读入与再打开使用不同文件名，确保断言确实覆盖了写出的正文字节。
    auto saved = output;
    saved.replace_extension(".tracks-export.osu");
    // 把键数作为唯一可替换的字段，合法和非法模式共用同一组对象。
    const std::string prefix =
        "osu file format v14\n[General]\nMode:3\n"
        "[Metadata]\nTitle:track-boundary\n[Difficulty]\nCircleSize:";
    // 范围外、非有限、数值溢出和尾随垃圾交替放在不同对象类型，
    // 使两个读取器必须遵守同样的字段验证规则。
    const std::string rows =
        "\n[TimingPoints]\n0,500,4,2,0,100,1,0\n[HitObjects]\n"
        "0,192,1000,1,0,0:0:0:0:\n"
        "2,192,1200,1,0,0:0:0:0:\n"
        "512,192,1500,1,0,0:0:0:0:\n"
        "0,192,2000,128,0,2300:0:0:0:0:\n"
        "512,192,2500,128,0,2900:0:0:0:0:\n"
        "-1,192,3000,1,0,0:0:0:0:\n"
        "513,192,3500,128,0,3800:0:0:0:0:\n"
        "nan,192,4000,1,0,0:0:0:0:\n"
        "1e999,192,4500,128,0,4800:0:0:0:0:\n"
        "2invalid,192,5000,1,0,0:0:0:0:\n";
    // 依次覆盖单轨、常用键型和稠密键型。
    // 稠密模式的整数坐标不能只按理论中点向下取整。
    for ( const int keys : { 1, 4, 7, 500 } ) {
        if ( !writeFixture(input, prefix + std::to_string(keys) + rows) )
            return false;
        // 内部轨号从零开始，末端检查同时能检测误映射到 BGM 轨。
        const auto lastTrack = static_cast<unsigned>(keys - 1);
        // 同一判定作用于原始夹具和 writer 输出，不允许两次不同方式变更物件。
        const auto matches = [&](const MMM::BeatMap& map) {
            const auto& notes = map.m_noteData.notes;
            const auto& holds = map.m_noteData.holds;
            // 两端各有单键和长条，非法对象应全部跳过。
            // 中间的 2 像素在 500K 会落到内列，写出时不能取整到它的前一轨。
            // 长条时长保持原值，证明轨道变化未改动持续时间字段。
            return map.m_baseMapMetadata.track_count == keys &&
                   notes.size() == 3 && notes[0].m_track == 0 &&
                   notes[1].m_track == static_cast<unsigned>(2 * keys / 512) &&
                   notes[2].m_track == lastTrack && holds.size() == 2 &&
                   holds[0].m_track == 0 && holds[0].m_duration == 300 &&
                   holds[1].m_track == lastTrack && holds[1].m_duration == 400;
        };
        // 先断言读入结果，再执行真实写出和重开；前后共同丢掉一个对象也不能通过。
        auto map = MMM::BeatMap::loadFromFile(input);
        map.sync();
        if ( !matches(map) || !map.saveToFile(saved) ||
             !matches(MMM::BeatMap::loadFromFile(saved)) )
            return false;
    }
    // 小于一、超出坐标范围和解析失败都需要在转换前拒绝，
    // 否则可能构造错误轨号或让视图为无法表达的轨数分配资源。
    // 校验键数先于浮点转整和物件构造；越界文件不得被识别成有效谱面。
    for ( const auto bad : { "0", "-1", "513", "nan", "1e999", "0x4p0" } ) {
        if ( !writeFixture(input, prefix + bad + rows) ||
             !MMM::BeatMap::loadFromFile(input)
                  .m_baseMapMetadata.map_path.empty() )
            return false;
    }
    // 旧版允许仅有时间点的文件写出 -1，把这种存量文件读为正常默认键数。
    // 与上一组含物件的 -1 区分，确保回退不会吞掉真实的轨道编辑错误。
    if ( !writeFixture(input,
                       prefix + "-1\n[TimingPoints]\n0,500,4,2,0,100,1,0\n") )
        return false;
    // 时间点计数证明兼容路径完成了实际导入，而不是返回空图后碰巧使用默认键数。
    const auto timingOnly = MMM::BeatMap::loadFromFile(input);
    // 兼容回退只补齐轨道数，时间点不可从旧文件中丢失。
    if ( timingOnly.m_baseMapMetadata.track_count != 4 ||
         timingOnly.m_timings.size() != 1 )
        return false;
    // 夹具和重开文件都由本次测试独占，移除它们不会影响下方整谱的章节比较。
    std::error_code ignored;
    std::filesystem::remove(input, ignored);
    std::filesystem::remove(saved, ignored);
    return true;
}

/// @brief 用完整谱面验证四种后缀的实际保存、打开及原路径再次保存。
/// @param output 提供工作目录和各测试用例独立的文件名前缀。
/// @return 四种格式都按原路径保存、读取及再次写出时为真。
/// @details 使用四种格式共同可表达的简单模型，避免依赖各格式不共通的扩展数据。
/// 中文及重音字母覆盖原生路径编码，两次保存都使用同一个混合大小写的后缀。
bool checkMixedCaseExtensions(const std::filesystem::path& output)
{
    // 加入一个真实物件，确保输出经过格式 writer 且从磁盘重新解析到内容。
    MMM::BeatMap source;
    source.m_baseMapMetadata.track_count = 4;
    source.m_baseMapMetadata.title       = "mixed case";
    MMM::Note note;
    note.m_track     = 1;
    note.m_timestamp = 1000;
    source.m_noteData.notes.push_back(note);
    source.sync();

    // Unicode 部分先转为平台路径，再追加到基名，Windows 不经过 ANSI 代码页。
    const auto name = MMM::Config::utf8ToPath("歌曲-é");
    // 仅扩展名参与大小写分派，同一个 Unicode 基名由四个 writer 原样使用。
    for ( const auto suffix : { ".oSu", ".mC", ".iMd", ".mMm" } ) {
        // 输出文件的前缀含各 CTest 资源名；并发执行不会覆盖其他测试的结果。
        auto path = output;
        path.replace_extension();
        path += name.native();
        path += suffix;
        // 保存分派只读后缀，目标名和源元数据路径都使用传入的 Unicode 名称。
        if ( !source.saveToFile(path) ) return false;
        // 依次使用同一原路径完成写入、读取和覆盖，防止只修正加载端。
        auto loaded = MMM::BeatMap::loadFromFile(path);
        loaded.sync();
        // 同时核对路径身份与内部音符。创建了输出文件本身不足以证明成功识别格式。
        if ( loaded.m_baseMapMetadata.map_path.filename() != path.filename() ||
             loaded.m_noteData.notes.size() != 1 || !loaded.saveToFile(path) ||
             MMM::BeatMap::loadFromFile(path).m_noteData.notes.size() != 1 )
            return false;
        std::error_code ignored;
        // 只回收该格式的独立输出，不触碰其余夹具。
        std::filesystem::remove(path, ignored);
    }
    return true;
}

/// @brief 运行指定 osu! 输入与输出路径的一致性测试。
/// @param argc 参数数量，必须包含输入和输出路径。
/// @param argv 参数数组。
/// @return 文本七节与领域模型全部一致时返回 0，否则返回 1。
int main(int argc, char* argv[])
{
    // 路径不完整时不猜测默认资源，直接返回参数错误。
    if ( argc < 3 ) return 1;
    std::filesystem::path input  = argv[1];
    std::filesystem::path output = argv[2];

    if ( !checkHoldTypeFlags(output) ) return 1;

    if ( !checkBreakRoundTrip(output) ) return 1;
    // 将解析、轨道和路径断言分开记录，便于直接定位格式回归。
    if ( !checkHeaderAndBounds(output) ) {
        XERROR("osu! header handling failed");
        return 1;
    }
    // 主动标明阶段，避免再次引入的轨号错误被误诊为项目测试资源不足。
    if ( !checkManiaTracks(output) ) {
        XERROR("osu!mania coordinate handling failed");
        return 1;
    }
    if ( !checkMixedCaseExtensions(output) ) {
        XERROR("Mixed-case chart extension round trip failed");
        return 1;
    }

    XINFO("========================================");
    XINFO("  OSU Consistency Test: {}", input.filename().string());
    XINFO("========================================");

    // 第一轮先走真实加载和保存入口，生成待比较的 osu! 文本。
    MMM::BeatMap m1 = MMM::BeatMap::loadFromFile(input);
    m1.sync();

    // 写出拒绝表示来源包含 osu! 无法无损表达的数据，后续比较没有意义。
    if ( !m1.saveToFile(output) ) return 1;

    MMM::BeatMap m2 = MMM::BeatMap::loadFromFile(output);
    m2.sync();

    // 文本比较验证章节级字段，不用文件整体字节相等限制合法排版变化。
    int sectionPassed = MMM::Test::compareOSUSections(input, output);
    XINFO("OSU Text Section Comparison: {}/{} sections passed",
          sectionPassed,
          TOTAL_OSU_SECTIONS);

    // 第二轮比较通用模型，覆盖文本层无法直接判断的数值和对象语义。
    bool logicPassed = MMM::Test::compareBeatMaps(m1, m2);
    if ( logicPassed ) {
        XINFO("[OSU Logical Consistency]: PASS");
    } else {
        XERROR("[OSU Logical Consistency]: FAIL");
    }

    // 汇总把逻辑比较作为第八项，最终仍要求七个章节全部通过。
    int totalPassed = sectionPassed + (logicPassed ? 1 : 0);
    int totalTests  = TOTAL_OSU_SECTIONS + 1;
    XINFO("========================================");
    if ( sectionPassed == TOTAL_OSU_SECTIONS && logicPassed ) {
        XINFO("  OSU Consistency: ALL {}/{} PASSED", totalPassed, totalTests);
        return 0;
    } else {
        XERROR("  OSU Consistency: {}/{} passed, {} failed",
               totalPassed,
               totalTests,
               totalTests - totalPassed);
        return 1;
    }
}
