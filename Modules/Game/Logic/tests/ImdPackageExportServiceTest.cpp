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
#include <vector>

namespace
{

/// @brief 写入 16 位小端整数。
/// @param file 目标文件流。
/// @param value 待写入数值。
void writeU16(std::ofstream& file, std::uint16_t value)
{
    const char bytes[2]{ static_cast<char>(value & 0xffU),
                         static_cast<char>((value >> 8U) & 0xffU) };
    file.write(bytes, 2);
}

/// @brief 写入 32 位小端整数。
/// @param file 目标文件流。
/// @param value 待写入数值。
void writeU32(std::ofstream& file, std::uint32_t value)
{
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
bool writeFixtureWav(const std::filesystem::path& path,
                     std::uint32_t                frameCount)
{
    constexpr std::uint32_t SAMPLE_RATE     = 48000U;
    constexpr std::uint16_t CHANNEL_COUNT   = 2U;
    constexpr std::uint16_t BITS_PER_SAMPLE = 16U;
    constexpr std::uint16_t BLOCK_ALIGN = CHANNEL_COUNT * BITS_PER_SAMPLE / 8U;
    const std::uint32_t     dataBytes   = frameCount * BLOCK_ALIGN;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    file.write("RIFF", 4);
    writeU32(file, 36U + dataBytes);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    writeU32(file, 16U);
    writeU16(file, 1U);
    writeU16(file, CHANNEL_COUNT);
    writeU32(file, SAMPLE_RATE);
    writeU32(file, SAMPLE_RATE * BLOCK_ALIGN);
    writeU16(file, BLOCK_ALIGN);
    writeU16(file, BITS_PER_SAMPLE);
    file.write("data", 4);
    writeU32(file, dataBytes);
    for ( std::uint32_t frame = 0U; frame < frameCount; ++frame ) {
        const double phase = 2.0 * 3.14159265358979323846 * 440.0 *
                             static_cast<double>(frame) /
                             static_cast<double>(SAMPLE_RATE);
        const auto sample =
            static_cast<std::int16_t>(std::sin(phase) * 10000.0);
        writeU16(file, static_cast<std::uint16_t>(sample));
        writeU16(file, static_cast<std::uint16_t>(sample));
    }
    return file.good();
}

/// @brief 输出测试断言。
/// @param condition 断言条件。
/// @param label 断言名称。
/// @return 条件值。
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
bool extractPackage(const std::filesystem::path& packagePath,
                    const std::filesystem::path& outputDirectory,
                    std::set<std::string>&       fileNames)
{
    mz_zip_archive    archive{};
    const std::string packagePathUtf8 = MMM::Config::pathToUtf8(packagePath);
    if ( !mz_zip_reader_init_file(&archive, packagePathUtf8.c_str(), 0) ) {
        return false;
    }

    bool          success    = true;
    const mz_uint entryCount = mz_zip_reader_get_num_files(&archive);
    for ( mz_uint index = 0U; index < entryCount; ++index ) {
        mz_zip_archive_file_stat fileStat{};
        if ( !mz_zip_reader_file_stat(&archive, index, &fileStat) ||
             mz_zip_reader_is_file_a_directory(&archive, index) ) {
            success = false;
            break;
        }
        const std::string archiveName = fileStat.m_filename;
        fileNames.insert(archiveName);
        const auto outputPath =
            outputDirectory / MMM::Config::utf8ToPath(archiveName);
        const std::string outputPathUtf8 = MMM::Config::pathToUtf8(outputPath);
        if ( !mz_zip_reader_extract_to_file(
                 &archive, index, outputPathUtf8.c_str(), 0) ) {
            success = false;
            break;
        }
    }
    mz_zip_reader_end(&archive);
    return success;
}

}  // namespace

int main(int argc, char* argv[])
{
    XLogger::init("ImdPackageExportServiceTest");
    auto& appThreadPool = MMM::Runtime::AppThreadPool::instance();
    appThreadPool.init();

    const std::filesystem::path outputRoot =
        argc > 1 ? MMM::Config::utf8ToPath(argv[1])
                 : std::filesystem::temp_directory_path() /
                       "mmm_imd_package_export_test";
    std::error_code filesystemError;
    std::filesystem::remove_all(outputRoot, filesystemError);
    filesystemError.clear();
    std::filesystem::create_directories(outputRoot, filesystemError);

    bool       ok         = check(!filesystemError, "output directory created");
    const auto inputAudio = outputRoot / "source.wav";
    const auto coverPath  = outputRoot / "source.PNG";
    const auto packagePath = outputRoot / "Song_Name.zip";
    ok &= check(writeFixtureWav(inputAudio, 4800U), "fixture audio created");
    {
        std::ofstream coverFile(coverPath, std::ios::binary | std::ios::trunc);
        coverFile.write("test-cover", 10);
        ok &= check(coverFile.good(), "fixture cover created");
    }

    MMM::BeatMap beatMap;
    beatMap.m_baseMapMetadata.name            = "Song_Name";
    beatMap.m_baseMapMetadata.title_unicode   = "Song_Name";
    beatMap.m_baseMapMetadata.version         = "Hard_Mode";
    beatMap.m_baseMapMetadata.track_count     = 4;
    beatMap.m_baseMapMetadata.bgm_track_count = 2;
    beatMap.m_noteData.notes.emplace_back();
    beatMap.m_noteData.notes.back().m_timestamp = 100.0;
    beatMap.m_noteData.notes.back().setSampleBinding(MMM::AudioSampleBinding{
        .m_audioResourceId = "bound-sample", .m_volume = 0.75F });
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

    MMM::AudioTrackConfig                                 audioConfig;
    const std::vector<MMM::Audio::AudioTimelineLoadEvent> audioEvents{
        MMM::Audio::AudioTimelineLoadEvent{
            .eventId               = 1U,
            .resourceKey           = "first",
            .filePath              = MMM::Config::pathToUtf8(inputAudio),
            .effectiveStartSeconds = -0.02,
            .eventVolume           = 1.0F,
            .resourceConfig        = audioConfig,
        },
        MMM::Audio::AudioTimelineLoadEvent{
            .eventId               = 2U,
            .resourceKey           = "second",
            .filePath              = MMM::Config::pathToUtf8(inputAudio),
            .effectiveStartSeconds = 0.08,
            .eventVolume           = 0.5F,
            .resourceConfig        = audioConfig,
        },
    };
    const auto result = MMM::Logic::ImdPackageExportService::exportPackage(
        beatMap, audioEvents, 0.15, coverPath, packagePath);
    if ( !result.success ) {
        XERROR("[imd-package-export] export error: {}", result.errorMessage);
    }
    ok &= check(result.success, "package export succeeds");
    ok &= check(result.beatmapFileName == "Song-Name_4k_Hard-Mode.imd",
                "IMD filename follows prefix key version rule");
    ok &= check(result.audioFileName == "Song-Name.mp3",
                "audio stem matches IMD prefix");
    ok &= check(result.coverFileName == "Song-Name.png",
                "cover stem matches IMD prefix");

    const auto extractedRoot = outputRoot / "extracted";
    filesystemError.clear();
    std::filesystem::create_directories(extractedRoot, filesystemError);
    std::set<std::string> archiveNames;
    ok &= check(!filesystemError &&
                    extractPackage(packagePath, extractedRoot, archiveNames),
                "package extracts successfully");
    ok &= check(
        archiveNames == std::set<std::string>{ "Song-Name_4k_Hard-Mode.imd",
                                               "Song-Name.mp3",
                                               "Song-Name.png" },
        "package contains exactly three same-prefix files");

    const auto extractedAudio = extractedRoot / "Song-Name.mp3";
    filesystemError.clear();
    ok &= check(
        std::filesystem::file_size(extractedAudio, filesystemError) > 0U &&
            !filesystemError,
        "mixed MP3 is non-empty");
    const auto loadedBeatMap = MMM::BeatMap::loadFromFile(
        extractedRoot / "Song-Name_4k_Hard-Mode.imd");
    ok &= check(loadedBeatMap.m_baseMapMetadata.track_count == 4,
                "exported IMD key count parses");
    ok &= check(loadedBeatMap.m_baseMapMetadata.version == "Hard-Mode",
                "exported IMD version parses");
    ok &= check(loadedBeatMap.m_audioSamples.size() == 1U &&
                    loadedBeatMap.m_audioSamples.front().m_audioResourceId ==
                        "Song-Name.mp3",
                "exported IMD resolves same-prefix MP3");
    ok &= check(
        MMM::Config::pathToUtf8(
            loadedBeatMap.m_baseMapMetadata.main_cover_path) == "Song-Name.png",
        "exported IMD resolves same-prefix cover");

    appThreadPool.shutdown();
    XLogger::shutdown();
    return ok ? 0 : 1;
}
