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

namespace
{
/// @brief 生成短 PCM 文件，测试使用生产编码器而无需外部音频资源。
bool makeWav(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary);
    const auto    little = [&](std::uint32_t value, int bytes) {
        for ( int index = 0; index < bytes; ++index )
            output.put(static_cast<char>(value >> (8 * index)));
    };
    // 一秒 48 kHz 单声道 PCM，包头与采样均显式使用小端表示。
    output.write("RIFF", 4);
    little(96036, 4);
    output.write("WAVEfmt ", 8);
    little(16, 4);
    little(1, 2);
    little(1, 2);
    little(48000, 4);
    little(96000, 4);
    little(2, 2);
    little(16, 2);
    output.write("data", 4);
    little(96000, 4);
    for ( unsigned frame = 0; frame < 48000; ++frame )
        little(frame % 64 < 32 ? 3000 : 62536, 2);
    return output.good();
}

/// @brief 用二进制读取比对源文件和精确换行的谱面文本。
std::string read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), {} };
}

/// @brief 验证混合格式、音频冲突、部分失败和项目资源 ID 与路径之间的映射。
bool run(const std::filesystem::path& base)
{
    using namespace MMM;
    using namespace MMM::Logic;
    const auto root =
        base / std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
    const auto      exported = root / "converted";
    std::error_code error;
    if ( !std::filesystem::create_directories(exported, error) ) return false;
    if ( !makeWav(root / "Song.wav") || !makeWav(root / "Keys.wav") )
        return false;
    const auto wavBefore = read(root / "Keys.wav");
    // 存在同名文件时仍应输出，同时引用编号后的真实文件。
    std::ofstream(exported / "Keys.ogg") << "existing";
    // IMD 需要避让能抢占新前缀的其他音频，而不仅检查目标扩展名。
    std::ofstream(exported / Config::utf8ToPath("Song (转码).mp3"))
        << "reserved";
    std::ofstream(root / "Song.png") << "cover";

    AudioBatchExportRequest request;
    request.m_inputs    = { root / "Song.wav",
                            root / "Missing.wav",
                            root / "Keys.wav" };
    request.m_directory = exported;
    request.m_extension = ".ogg";
    const auto addChart = [&](const std::string& name,
                              std::string_view   contents) {
        request.m_charts.push_back(root / name);
        std::ofstream file(root / name, std::ios::binary);
        file.write(contents.data(),
                   static_cast<std::streamsize>(contents.size()));
    };
    // 用不同来源的资源 ID 和实际文件名验证没有按扩展名全文替换。
    Project project;
    project.m_projectRoot = root;
    project.m_audioResources.push_back(
        AudioResource{ .m_id = "asset-id", .m_path = "Keys.wav" });
    std::string storageError;
    if ( !ProjectStorage{}.save(project, root, storageError) ) return false;
    addChart("Native.mmm",
             R"({"metadata":{"base":{"song_file_hint":"Song.wav"}},
        "note":[{"sample":{"audio_ref":"asset-id"},"extra":{"description":"Keys.wav"}},
        {"sub_notes":[{"bound_sound":"Keys.wav"}]}],
        "audio_samples":[{"audio_ref":"Song.wav"},{"audio_ref":"Missing.wav"}]})");
    addChart("Malody.mc",
             R"({"meta":{"song":{"file":"Song.wav","title":"Keys.wav"}},
        "note":[{"type":1,"sound":"Keys.wav"},{"sound":"Missing.wav"}]})");
    constexpr auto osu =
        "osu file format v14\r\n[General]\r\nAudioFilename: Song.wav\r\n"
        "[Events]\r\nSample,625,0,\"Song.wav\",50\r\n"
        "[HitObjects]\r\n64,192,500,1,0,0:0:0:80:Keys.wav\r\n"
        "192,192,1000,128,0,1500:0:0:0:60:Missing.wav";
    addChart("First.osu", osu);
    addChart("Second.osu", osu);
    for ( const std::string extension : { ".bms", ".bme", ".bml", ".pms" } )
        addChart("Traditional" + extension,
                 "#TITLE Keys.wav\n#BPM 120\n#WAV01 \"Keys.wav\"\n#WAV02 "
                 "Missing.wav\n#00111:01\n");
    // 完整的空 IMD 二进制仍使用生产加载器推导同名前缀；副本必须逐字节相同。
    std::array<char, 14> imd{};
    imd[8] = imd[9] = 3;
    addChart("Song_4k_EZ.imd", std::string_view(imd.data(), imd.size()));

    const auto result = exportAudioBatch(std::move(request));
    for ( const auto& message : result.m_errors ) XINFO("{}", message);
    if ( result.m_audioWritten != 2 || result.m_chartsWritten != 9 ||
         result.m_chartsUnchanged != 0 || result.m_errors.size() != 1 ||
         result.m_errors.front().find("Missing.wav") == std::string::npos )
        return false;

    if ( read(exported / "Keys.ogg") != "existing" ||
         !read(exported / "Keys (2).ogg").starts_with("OggS") ||
         read(root / "Keys.wav") != wavBefore ||
         read(root / "First.osu") != osu )
        return false;
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

    const auto native = read(root / named("Native (转码).mmm"));
    if ( !native.contains("\"audio_ref\": \"converted/Keys (2).ogg\"") ||
         !native.contains("\"bound_sound\": \"converted/Keys (2).ogg\"") ||
         !native.contains("\"description\": \"Keys.wav\"") ||
         !read(root / named("Malody (转码).mc"))
              .contains("\"sound\": \"converted/Keys (2).ogg\"") )
        return false;
    for ( const std::string extension : { ".bms", ".bme", ".bml", ".pms" } ) {
        const auto bms = read(root / named("Traditional (转码)" + extension));
        if ( bms !=
             "#TITLE Keys.wav\n#BPM 120\n#WAV01 \"converted/Keys "
             "(2).ogg\"\n#WAV02 Missing.wav\n#00111:01\n" )
            return false;
    }
    const auto copiedImd = exported / named("Song (转码) (2)_4k_EZ.imd");
    const auto loaded    = BeatMap::loadFromFile(copiedImd);
    return read(copiedImd) == read(root / "Song_4k_EZ.imd") &&
           loaded.m_baseMapMetadata.song_file_hint ==
               named("Song (转码) (2).ogg") &&
           read(exported / named("Song (转码) (2).png")) == "cover";
}
}  // namespace

/// @brief 从 CTest 输出根运行真实音频转码和谱面格式回归。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    XLogger::init("AudioBatchExportServiceTest");
    MMM::Runtime::AppThreadPool::instance().init();
    return run(MMM::Config::utf8ToPath(argv[1])) ? 0 : 2;
}
