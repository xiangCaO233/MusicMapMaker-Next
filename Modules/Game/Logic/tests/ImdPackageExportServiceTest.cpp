#include "logic/ImdPackageExportService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "runtime/AppThreadPool.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <miniz.h>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// 本测试生成自己的短音频与封面占位字节，不依赖 tests/data 中的媒体资源。
// 验证服务导出、压缩包结构和 IMD 回读；非空 MP3 不等于已验证实际听感。
namespace
{

/// @brief 写入 16 位小端整数。
/// @param file 目标文件流。
/// @param value 待写入数值。
/// @note 不负责打开或关闭流，也不逐字段清除错误状态。
void writeU16(std::ofstream& file, std::uint16_t value)
{
    // 显式拆字节，不依赖主机端序或整数对象的内存布局。
    const char bytes[2]{ static_cast<char>(value & 0xffU),
                         static_cast<char>((value >> 8U) & 0xffU) };
    file.write(bytes, 2);
}

/// @brief 写入 32 位小端整数。
/// @param file 目标文件流。
/// @param value 待写入数值。
/// @note 只写字段本身，不插入对齐填充；调用方决定 RIFF 块的顺序。
void writeU32(std::ofstream& file, std::uint32_t value)
{
    // 与 RIFF 固定小端字段约定一致；流错误由完整夹具写入结束时检查。
    const char bytes[4]{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
    };
    file.write(bytes, 4);
}

/// @brief 创建测试用短立体声 WAV。
/// @param path 输出路径。
/// @param frameCount 采样帧数。
/// @return 成功完整写出时返回 true。
/// @pre frameCount 足够小，使帧数乘帧宽与 RIFF 长度字段不会溢出 uint32。
/// @pre 父目录已创建，path 是允许覆盖的测试文件。
/// @note 此处只生成 PCM 输入；MP3 编码必须由被测导出服务完成。
/// @note 返回值仅反映流当前状态，音频是否能解码仍由后续导出步骤检验。
bool writeFixtureWav(const std::filesystem::path& path,
                     std::uint32_t                frameCount)
{
    constexpr std::uint32_t SAMPLE_RATE     = 48000U;
    constexpr std::uint16_t CHANNEL_COUNT   = 2U;
    constexpr std::uint16_t BITS_PER_SAMPLE = 16U;
    // 每帧交错存放左右两个 16 位采样，因此数据长度必须是四字节的倍数。
    // 该布局也保证 data 块天然偶数对齐，无需额外补齐字节。
    constexpr std::uint16_t BLOCK_ALIGN = CHANNEL_COUNT * BITS_PER_SAMPLE / 8U;
    const std::uint32_t     dataBytes   = frameCount * BLOCK_ALIGN;
    // frameCount 是双声道帧数，不是单个声道采样值的总数。

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    file.write("RIFF", 4);
    writeU32(file, 36U + dataBytes);
    // RIFF 长度不包含开头八字节；PCM fmt 块采用无扩展的标准布局。
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    writeU32(file, 16U);
    // 格式码 1 指定整数 PCM，不能改成浮点格式码而仍保留下面的 int16 数据。
    writeU16(file, 1U);
    writeU16(file, CHANNEL_COUNT);
    writeU32(file, SAMPLE_RATE);
    // 字节率以整帧宽度计算，不能只乘单声道采样宽度。
    writeU32(file, SAMPLE_RATE * BLOCK_ALIGN);
    writeU16(file, BLOCK_ALIGN);
    writeU16(file, BITS_PER_SAMPLE);
    file.write("data", 4);
    writeU32(file, dataBytes);
    for ( std::uint32_t frame = 0U; frame < frameCount; ++frame ) {
        // 440 Hz 正弦提供非静音内容，幅度低于 int16 上限以避免夹具削波。
        const double phase = 2.0 * 3.14159265358979323846 * 440.0 *
                             static_cast<double>(frame) /
                             static_cast<double>(SAMPLE_RATE);
        const auto sample =
            static_cast<std::int16_t>(std::sin(phase) * 10000.0);
        // 负采样转换为无符号值后再拆字节，保留 PCM 的二进制补码表示。
        writeU16(file, static_cast<std::uint16_t>(sample));
        writeU16(file, static_cast<std::uint16_t>(sample));
        // 左右声道写入相同值，本例不测试声道分离或声像处理。
    }
    return file.good();
}

/// @brief 输出测试断言。
/// @param condition 断言条件。
/// @param label 断言名称。
/// @return 条件值。
/// @note 调用方以 &= 累积结果，单个失败不会阻止后续检查输出更多诊断。
bool check(bool condition, const std::string& label)
{
    if ( condition ) {
        XINFO("[imd-package-export] PASS: {}", label);
    } else {
        XERROR("[imd-package-export] FAIL: {}", label);
    }
    return condition;
}

/// @brief 提取 zip 根目录中的全部文件。
/// @param packagePath zip 路径。
/// @param outputDirectory 提取目录。
/// @param fileNames 接收包内文件名集合。
/// @return 全部条目均成功提取时返回 true。
/// @pre 仅处理本测试刚导出的可信平铺包，不是通用不可信归档解压入口。
/// @note fileNames 追加文件名集合；调用方提供空集合以检查本包的完整内容。
/// @note 失败时可能已留下部分文件和名称；调用方必须同时检查返回值。
/// @note 集合用于比较名称，不用于证明归档内没有重复命名的条目。
/// @pre outputDirectory 已创建且可写，测试包的文件不要求创建嵌套父目录。
bool extractPackage(const std::filesystem::path& packagePath,
                    const std::filesystem::path& outputDirectory,
                    std::set<std::string>&       fileNames)
{
    mz_zip_archive    archive{};
    const std::string packagePathUtf8 = MMM::Config::pathToUtf8(packagePath);
    // 字符串在整个初始化调用期间存活，避免传入临时路径缓冲区。
    if ( !mz_zip_reader_init_file(&archive, packagePathUtf8.c_str(), 0) ) {
        return false;
    }

    bool          success    = true;
    const mz_uint entryCount = mz_zip_reader_get_num_files(&archive);
    for ( mz_uint index = 0U; index < entryCount; ++index ) {
        mz_zip_archive_file_stat fileStat{};
        if ( !mz_zip_reader_file_stat(&archive, index, &fileStat) ||
             mz_zip_reader_is_file_a_directory(&archive, index) ) {
            // RM 产物预期为根目录文件，出现目录项也视为结构不符合要求。
            success = false;
            break;
        }
        const std::string archiveName = fileStat.m_filename;
        // 保存归档原始名称用于断言，不能用提取目录的绝对路径代替。
        fileNames.insert(archiveName);
        const auto outputPath =
            outputDirectory / MMM::Config::utf8ToPath(archiveName);
        // 路径拼接交给 filesystem，传入 miniz 前再按项目约定转换为 UTF-8。
        const std::string outputPathUtf8 = MMM::Config::pathToUtf8(outputPath);
        if ( !mz_zip_reader_extract_to_file(
                 &archive, index, outputPathUtf8.c_str(), 0) ) {
            success = false;
            break;
        }
    }
    // 部分提取失败不在这里回滚文件，便于保留现场分析失败的具体条目。
    mz_zip_reader_end(&archive);
    // 初始化成功后的所有循环退出都到达这里，释放归档读取状态。
    return success;
}

}  // namespace

/// @brief 生成夹具、导出资源包并核对命名与回读资源关联。
/// @param argc 参数数量，可额外指定一个可清理的测试输出根目录。
/// @param argv UTF-8 参数；指定目录的全部旧内容会在测试开始时删除。
/// @return 全部断言通过时为零。
/// @note 运行前必须确认输出根是专用测试目录，不得传入源码、资源或用户项目目录。
/// @note 默认输出目录名称固定，不支持多个实例共享该目录并行执行。
/// @note 结束时保留输入与导出产物，下一次运行才清理指定工作区。
int main(int argc, char* argv[])
{
    XLogger::init("ImdPackageExportServiceTest");
    auto& appThreadPool = MMM::Runtime::AppThreadPool::instance();
    appThreadPool.init();
    // 导出涉及音频工作任务，先准备应用线程池，结束时统一关闭。

    const std::filesystem::path outputRoot =
        argc > 1 ? MMM::Config::utf8ToPath(argv[1])
                 : std::filesystem::temp_directory_path() /
                       "mmm_imd_package_export_test";
    std::error_code filesystemError;
    std::filesystem::remove_all(outputRoot, filesystemError);
    // 输出根是测试独占的工作区，清除旧产物以免失败导出读到历史文件。
    filesystemError.clear();
    std::filesystem::create_directories(outputRoot, filesystemError);

    bool       ok         = check(!filesystemError, "output directory created");
    const auto inputAudio = outputRoot / "source.wav";
    const auto coverPath  = outputRoot / "source.PNG";
    // 输入封面的扩展名故意大写，后面同时检查输出扩展名被规范为小写。
    const auto packagePath = outputRoot / "Song_Name.zip";
    // 输入文件和最终包共享工作区，但名称分离，导出不能覆盖自己的输入音频。
    ok &= check(writeFixtureWav(inputAudio, 4800U), "fixture audio created");
    // 4800 帧对应 0.1 秒，供两次不同起点的时间线事件复用。
    {
        // 将写入流限制在此作用域，确保调用导出服务前已关闭封面文件。
        std::ofstream coverFile(coverPath, std::ios::binary | std::ios::trunc);
        coverFile.write("test-cover", 10);
        // 仅验证封面复制及命名，不把这些占位字节当作可解码 PNG。
        ok &= check(coverFile.good(), "fixture cover created");
    }

    MMM::BeatMap beatMap;
    beatMap.m_baseMapMetadata.name          = "Song_Name";
    beatMap.m_baseMapMetadata.title_unicode = "Song_Name";
    beatMap.m_baseMapMetadata.version       = "Hard_Mode";
    // 名称和难度都包含下划线，输出预期将其规范为连字符以保留文件名分隔规则。
    beatMap.m_baseMapMetadata.track_count     = 4;
    beatMap.m_baseMapMetadata.bgm_track_count = 2;
    // 四条按键轨与两条音频轨分开建模，导出 IMD 的键数仍应为四。
    // 音符自带的音效绑定与下方独立音频事件并存，用于覆盖有绑定的输入模型。
    beatMap.m_noteData.notes.emplace_back();
    beatMap.m_noteData.notes.back().m_timestamp = 100.0;
    // 绑定资源不另建加载事件；实际待混音内容由传给服务的 audioEvents 明确指定。
    beatMap.m_noteData.notes.back().setSampleBinding(MMM::AudioSampleBinding{
        .m_audioResourceId = "bound-sample", .m_volume = 0.75F });
    // 两条音频轨从按键轨之后开始编号；事件起点一负一正，覆盖零点两侧输入。
    beatMap.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_timestamp       = -20.0,
        .m_track           = 4U,
        .m_audioResourceId = "first",
    });
    beatMap.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_timestamp       = 80.0,
        .m_track           = 5U,
        .m_audioResourceId = "second",
    });
    beatMap.sync();
    // 修改完原始模型后统一同步派生状态，再将模型交给导出流程。

    MMM::AudioTrackConfig audioConfig;
    // 使用默认资源配置，让本例只改变事件音量，不叠加额外资源级音量设置。
    // 模型采样时间是毫秒，加载事件的有效起点是秒，两份夹具保持对应。
    const std::vector<MMM::Audio::AudioTimelineLoadEvent> audioEvents{
        // 相同物理文件被不同资源键引用；事件身份不能按路径简单合并。
        MMM::Audio::AudioTimelineLoadEvent{
            .eventId               = 1U,
            .resourceKey           = "first",
            .filePath              = MMM::Config::pathToUtf8(inputAudio),
            .effectiveStartSeconds = -0.02,
            .eventVolume           = 1.0F,
            .resourceConfig        = audioConfig,
        },
        MMM::Audio::AudioTimelineLoadEvent{
            // 第二次播放降低音量，保留事件级增益的输入差异。
            .eventId               = 2U,
            .resourceKey           = "second",
            .filePath              = MMM::Config::pathToUtf8(inputAudio),
            .effectiveStartSeconds = 0.08,
            .eventVolume           = 0.5F,
            .resourceConfig        = audioConfig,
        },
    };
    std::vector<std::string> stages;
    // 0.15 秒是本次请求的导出时长，不由 WAV 夹具自身的 0.1 秒长度替代。
    // 本例检查产物结构，并未逐采样断言负起点裁剪或尾部截断的结果。
    // 复制阶段文本而非保留 string_view，避免回调参数生命周期结束后引用失效。
    const auto result = MMM::Logic::ImdPackageExportService::exportPackage(
        beatMap,
        audioEvents,
        0.15,
        coverPath,
        packagePath,
        [&](std::string_view stage) { stages.emplace_back(stage); });
    // 先判定数量再读取首尾，空阶段列表在导出提前失败时也能安全检查。
    // 仅断言阶段数量与首尾，不把中间所有阶段的顺序视为已逐项验证。
    ok &= check(stages.size() == 5 && stages.front() == "正在生成 IMD 谱面…" &&
                    stages.back() == "正在压缩 RM 资源包…",
                "export reports processing stages in order");
    if ( !result.success ) {
        // 输出服务提供的具体错误，避免只留下笼统的 success 断言失败。
        XERROR("[imd-package-export] export error: {}", result.errorMessage);
    }
    ok &= check(result.success, "package export succeeds");
    // IMD 文件名包含曲名前缀、键数和难度；资源文件只共享曲名前缀。
    // ZIP 外层文件名仍使用调用方的 Song_Name.zip，不参与内部命名断言。
    ok &= check(result.beatmapFileName == "Song-Name_4k_Hard-Mode.imd",
                "IMD filename follows prefix key version rule");
    ok &= check(result.audioFileName == "Song-Name.mp3",
                "audio stem matches IMD prefix");
    ok &= check(result.coverFileName == "Song-Name.png",
                "cover stem matches IMD prefix");
    // 返回名称检查和归档内容检查分开，能区分返回值错误与实际打包缺失。

    const auto extractedRoot = outputRoot / "extracted";
    // 创建解包目录的错误单独清零，不能继承之前文件操作的残留错误状态。
    filesystemError.clear();
    std::filesystem::create_directories(extractedRoot, filesystemError);
    std::set<std::string> archiveNames;
    // 解包到独立子目录，随后回读只能使用包内文件，不能借用导出暂存文件。
    ok &= check(!filesystemError &&
                    extractPackage(packagePath, extractedRoot, archiveNames),
                "package extracts successfully");
    ok &= check(
        archiveNames == std::set<std::string>{ "Song-Name_4k_Hard-Mode.imd",
                                               "Song-Name.mp3",
                                               "Song-Name.png" },
        "package contains exactly three same-prefix files");

    // 名称集合相等同时排除多余的不同名文件，例如未清理的原始 WAV。
    // 不依赖 ZIP 条目排列顺序，压缩实现调整排序不应影响此项检查。
    const auto extractedAudio = extractedRoot / "Song-Name.mp3";
    filesystemError.clear();
    // file_size 出错可能返回很大的哨兵值，必须同时检查 error_code。
    // 非空只证明编码产物存在，不在此声称已覆盖波形、时长或音质正确性。
    ok &= check(
        std::filesystem::file_size(extractedAudio, filesystemError) > 0U &&
            !filesystemError,
        "mixed MP3 is non-empty");
    // 通过正常 IMD 入口回读，检查同前缀 MP3 和封面能否由格式约定解析。
    // 不用导出返回的内存模型代替回读，否则无法覆盖真实文件间的关联。
    const auto loadedBeatMap = MMM::BeatMap::loadFromFile(
        extractedRoot / "Song-Name_4k_Hard-Mode.imd");
    ok &= check(loadedBeatMap.m_baseMapMetadata.track_count == 4,
                "exported IMD key count parses");
    // 回读难度取规范化后的文件名结果，不应恢复成输入中的下划线版本。
    ok &= check(loadedBeatMap.m_baseMapMetadata.version == "Hard-Mode",
                "exported IMD version parses");
    // 两条输入音频事件导出为单个混音文件；回读后应只有一个主音频引用。
    // 先检查样本数再访问 front，缺失资源时不让断言本身越界。
    ok &= check(loadedBeatMap.m_audioSamples.size() == 1U &&
                    loadedBeatMap.m_audioSamples.front().m_audioResourceId ==
                        "Song-Name.mp3",
                "exported IMD resolves same-prefix MP3");
    // 封面关联应保存包内相对名称，而不是输入夹具或解包工作区的绝对路径。
    // 此断言只覆盖资源寻址，不要求占位封面字节能够被图片解码器接受。
    ok &= check(
        MMM::Config::pathToUtf8(
            loadedBeatMap.m_baseMapMetadata.main_cover_path) == "Song-Name.png",
        "exported IMD resolves same-prefix cover");

    // 即使前面的断言失败，也经过正常关闭流程后才返回非零退出码。
    appThreadPool.shutdown();
    // 先结束后台任务再关闭日志，避免导出收尾任务失去日志通道。
    XLogger::shutdown();
    return ok ? 0 : 1;
}
