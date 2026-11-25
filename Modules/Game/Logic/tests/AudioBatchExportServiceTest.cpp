#include "logic/AudioBatchExportService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/ProjectStorage.h"
#include "mmm/beatmap/BeatMap.h"
#include "runtime/AppThreadPool.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

/// @file AudioBatchExportServiceTest.cpp
/// @brief 用真实编码与回读验证批量音频发布以及多格式谱面改链。
/// @details 测试文件在构建树中生成，输入、旧输出和本轮制品可逐字节区分。
/// 涵盖两个成功音频、一个失败音频以及依赖它们的不同类型谱面。
/// @note 测试以实际读出的文件和生产解析结果为准；只检查批量统计不足以
/// 判断覆盖保护、改链语义与 IMD 伴随资源是否正确。
namespace
{
/// @brief 生成短 PCM 文件，测试使用生产编码器而无需外部音频资源。
/// @param path 由测试拥有的项目目录内目标。
/// @return 数据和头已写入输出流时返回真。
/// @note 用最小 RIFF/WAVE 表示，无需依赖平台录音设备或外部样本文件。
bool makeWav(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary);
    // 直接按字段写入，避免将 C++ 结构布局与平台大小端带入测试资源格式。
    // 音频样本和容器长度共用同一辅助写入器。
    const auto little = [&](std::uint32_t value, int bytes) {
        for ( int index = 0; index < bytes; ++index )
            output.put(static_cast<char>(value >> (8 * index)));
    };
    // 一秒 48 kHz 单声道 PCM，包头与采样均显式使用小端表示。
    output.write("RIFF", 4);
    // RIFF 内容大小包含格式块和数据块头，不包含前面 8 字节的 RIFF 标识及长度。
    little(96036, 4);
    output.write("WAVEfmt ", 8);
    // 包含 PCM 基础格式字段，不带编解码器扩展区。
    little(16, 4);
    little(1, 2);
    little(1, 2);
    little(48000, 4);
    // 每秒字节数和块对齐都按 48 kHz 单声道 16 位 PCM 推导。
    little(96000, 4);
    little(2, 2);
    little(16, 2);
    output.write("data", 4);
    little(96000, 4);
    // 有符号采样使用两种不同振幅，避免输出只是可被静音优化跳过的空内容。
    // 62536 按 16 位二补码解释为负采样值，和正半周组成固定周期波形。
    for ( unsigned frame = 0; frame < 48000; ++frame )
        little(frame % 64 < 32 ? 3000 : 62536, 2);
    return output.good();
}

/// @brief 用二进制读取比对源文件和精确换行的谱面文本。
std::string read(const std::filesystem::path& path)
{
    // 读取时保留 CRLF 与最后一行边界；文本模式会隐藏正要检测的格式变化。
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), {} };
}

/// @brief 验证混合格式、音频冲突、部分失败和项目资源 ID 与路径之间的映射。
/// @param base CTest 授权的输出目录，场景在独立子目录内生成数据。
/// @return 同时满足发布统计、保留规则和引用语义时返回真。
/// @note 请求顺序把失败输入放在两个有效音频之间，能观察失败后是否继续。
bool run(const std::filesystem::path& base)
{
    using namespace MMM;
    using namespace MMM::Logic;
    // 每次启动使用新子目录，并预置可区分的碰撞内容，避免上次测试误导命名结果。
    const auto root =
        base / std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
    const auto      exported = root / "converted";
    std::error_code error;
    if ( !std::filesystem::create_directories(exported, error) ) return false;
    if ( !makeWav(root / "Song.wav") || !makeWav(root / "Keys.wav") )
        return false;
    // 保留实际源数据用于运行后比对，确认改链不写回原有音频。
    const auto wavBefore = read(root / "Keys.wav");
    // 存在同名文件时仍应输出，同时引用编号后的真实文件。
    std::ofstream(exported / "Keys.ogg") << "existing";
    // IMD 需要避让能抢占新前缀的其他音频，而不仅检查目标扩展名。
    std::ofstream(exported / Config::utf8ToPath("Song (转码).mp3"))
        << "reserved";
    // IMD 封面按相同前缀隐式关联；内容标记用来验证新封面确实来自正确来源。
    std::ofstream(root / "Song.png") << "cover";

    // 两个 WAV 允许音频编码与普通文件避让在同一次请求内出现。
    // 失败引用保留原文件名，验证未发布制品时不会生成悬空覆盖引用。
    AudioBatchExportRequest request;
    request.m_inputs    = { root / "Song.wav",
                            root / "Missing.wav",
                            root / "Keys.wav" };
    request.m_directory = exported;
    request.m_extension = ".ogg";
    // 让源文件和请求列表一起登记，后续数量断言覆盖每一种输入谱面。
    // 二进制写入避免 Windows 文本模式改写已声明的行结束符。
    const auto addChart = [&](const std::string& name,
                              std::string_view   contents) {
        request.m_charts.push_back(root / name);
        std::ofstream file(root / name, std::ios::binary);
        file.write(contents.data(),
                   static_cast<std::streamsize>(contents.size()));
    };
    // 用不同来源的资源 ID 和实际文件名验证没有按扩展名全文替换。
    // 项目资源表使用非文件名 ID，同一实际文件同时由 ID 与显式路径找到。
    Project project;
    project.m_projectRoot = root;
    project.m_audioResources.push_back(
        AudioResource{ .m_id = "asset-id", .m_path = "Keys.wav" });
    std::string storageError;
    if ( !ProjectStorage{}.save(project, root, storageError) ) return false;
    // 原生谱面同时引用元数据、音轨采样与子音符绑定。
    // 描述中的相同文字和未导出采样提供反例，只有真实资源字段可被改写。
    addChart("Native.mmm",
             R"({"metadata":{"base":{"song_file_hint":"Song.wav"}},
        "note":[{"sample":{"audio_ref":"asset-id"},"extra":{"description":"Keys.wav"}},
        {"sub_notes":[{"bound_sound":"Keys.wav"}]}],
        "audio_samples":[{"audio_ref":"Song.wav"},{"audio_ref":"Missing.wav"}]})");
    // Malody 直接保存文件名且不使用原生项目 ID。
    // 标题也包含文件名，用于说明这份输入不能只按后缀整体改写。
    addChart("Malody.mc",
             R"({"meta":{"song":{"file":"Song.wav","title":"Keys.wav"}},
        "note":[{"type":1,"sound":"Keys.wav"},{"sound":"Missing.wav"}]})");
    // 同一 osu 文本同时有主音轨、自动采样和带附加字段的物件采样。
    // 末行无结束符，且未转码文件放在末行，可一起验证两种保留契约。
    constexpr auto osu =
        "osu file format v14\r\n[General]\r\nAudioFilename: Song.wav\r\n"
        "[Events]\r\nSample,625,0,\"Song.wav\",50\r\n"
        "[HitObjects]\r\n64,192,500,1,0,0:0:0:80:Keys.wav\r\n"
        "192,192,1000,128,0,1500:0:0:0:60:Missing.wav";
    // 两个独立谱面内容相同，能捕捉暂存文件重复使用时跨项污染或漏处理。
    addChart("First.osu", osu);
    addChart("Second.osu", osu);
    // 四种传统扩展名共享声明语法，标题与时间轴事件保留，只有 WAV 路径变化。
    for ( const std::string extension : { ".bms", ".bme", ".bml", ".pms" } )
        addChart("Traditional" + extension,
                 "#TITLE Keys.wav\n#BPM 120\n#WAV01 \"Keys.wav\"\n#WAV02 "
                 "Missing.wav\n#00111:01\n");
    // 完整的空 IMD 二进制仍使用生产加载器推导同名前缀；副本必须逐字节相同。
    std::array<char, 14> imd{};
    // 使用能被正式解析器接受的最小 IMD 头，音频和封面仍由同名前缀推导。
    imd[8] = imd[9] = 3;
    addChart("Song_4k_EZ.imd", std::string_view(imd.data(), imd.size()));

    // 执行实际插件使用的后台服务。结果同时包含制品与未处理项，
    // 不通过伪造回调或预建预期制品替代编码、改名与发布。
    const auto result = exportAudioBatch(std::move(request));
    for ( const auto& message : result.m_errors ) XINFO("{}", message);
    // 失败音频必须有具体诊断，仍应报告音频和谱面各自的发布统计。
    // 九份谱面都有至少一个已成功输出的关联，因此未更改计数应为零。
    if ( result.m_audioWritten != 2 || result.m_chartsWritten != 9 ||
         result.m_chartsUnchanged != 0 || result.m_errors.size() != 1 ||
         result.m_errors.front().find("Missing.wav") == std::string::npos )
        return false;

    // 预置同名文件的字节不可改变，编号制品必须包含 Ogg 容器标识。
    // 原音频与原谱面保持逐字节一致，证明副本并非原地改写。
    if ( read(exported / "Keys.ogg") != "existing" ||
         !read(exported / "Keys (2).ogg").starts_with("OggS") ||
         read(root / "Keys.wav") != wavBefore ||
         read(root / "First.osu") != osu )
        return false;
    // 平台路径使用同一 UTF-8 转换入口，避免 Windows
    // 测试在查找中文后缀时依赖代码页。
    const auto named = [](std::string name) {
        return Config::utf8ToPath(std::move(name));
    };
    const auto source = read(root / named("First (转码).osu"));
    // 检查句法、音量、触发时间、换行和未转码资源都能跨局部改写保留。
    if ( !source.contains(
             "Sample,625,0,\"converted/Song (转码) (2).ogg\",50\r\n") ||
         !source.contains("AudioFilename: converted/Song (转码) (2).ogg\r\n") ||
         !source.contains("0:0:0:80:converted/Keys (2).ogg\r\n") ||
         !source.ends_with("0:0:0:60:Missing.wav") ||
         read(root / named("Second (转码).osu")) != source )
        return false;

    // 原生格式需要同时验证 ID 解析、嵌套物件和不可触碰的描述字段。
    // Malody 音符声音独立输出；原生路径语义不能被直接套到外部格式。
    const auto native = read(root / named("Native (转码).mmm"));
    if ( !native.contains("\"audio_ref\": \"converted/Keys (2).ogg\"") ||
         !native.contains("\"bound_sound\": \"converted/Keys (2).ogg\"") ||
         !native.contains("\"description\": \"Keys.wav\"") ||
         !read(root / named("Malody (转码).mc"))
              .contains("\"sound\": \"converted/Keys (2).ogg\"") )
        return false;
    for ( const std::string extension : { ".bms", ".bme", ".bml", ".pms" } ) {
        // 逐字节比较正文，保留未匹配的 WAV、引号、标题和节拍事件。
        // 这也验证编号输出的相对路径仍位于源谱面预期的目录基准。
        const auto bms = read(root / named("Traditional (转码)" + extension));
        if ( bms !=
             "#TITLE Keys.wav\n#BPM 120\n#WAV01 \"converted/Keys "
             "(2).ogg\"\n#WAV02 Missing.wav\n#00111:01\n" )
            return false;
    }
    // 新前缀和被占用的 MP3 同 stem，避让时必须选择第二编号而非原编号。
    // 通过正式 IMD 加载器再找一次隐式引用，不能只检查复制路径的拼写。
    // 副本正文仍与源文件相同；独立封面标记同时验证完整的伴随资源布局。
    const auto copiedImd = exported / named("Song (转码) (2)_4k_EZ.imd");
    const auto loaded    = BeatMap::loadFromFile(copiedImd);
    return read(copiedImd) == read(root / "Song_4k_EZ.imd") &&
           loaded.m_baseMapMetadata.song_file_hint ==
               named("Song (转码) (2).ogg") &&
           read(exported / named("Song (转码) (2).png")) == "cover";
}
}  // namespace

/// @brief 从 CTest 输出根运行真实音频转码和谱面格式回归。
/// @param argc 约定只接收 CTest 输出根一个业务参数。
/// @param argv 输出目录按 UTF-8 转换为文件系统路径。
/// @return 参数错误为 1，业务回归失败为 2，通过为 0。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    XLogger::init("AudioBatchExportServiceTest");
    // 解码器与编码服务沿用应用线程池初始化，测试不需要启动编辑器或图形设备。
    MMM::Runtime::AppThreadPool::instance().init();
    return run(MMM::Config::utf8ToPath(argv[1])) ? 0 : 2;
}
