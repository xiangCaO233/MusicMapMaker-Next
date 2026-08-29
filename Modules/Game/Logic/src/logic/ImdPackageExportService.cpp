#include "logic/ImdPackageExportService.h"

#include "audio/AudioTimelineExportService.h"
#include "config/Utf8Path.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/PackageFileTypes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <miniz.h>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM::Logic
{
namespace
{

/// @brief 负责在导出完成或提前失败时清理工作目录。
struct TemporaryDirectoryGuard {
    /// @brief 需要递归清理的临时目录。
    std::filesystem::path path;

    /// @brief 清理临时目录；清理失败不覆盖原始导出结果。
    ~TemporaryDirectoryGuard()
    {
        std::error_code filesystemError;
        std::filesystem::remove_all(path, filesystemError);
    }
};

/// @brief 判断 ASCII 字符是否不适合出现在资源包文件名中。
/// @param character 待检查字符。
/// @return 需要替换时返回 true。
bool isInvalidFileNameCharacter(unsigned char character) noexcept
{
    if ( character < 0x20U ) return true;
    switch ( character ) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '/':
    case '\\':
    case '|':
    case '?':
    case '*':
    case '_': return true;
    default: return false;
    }
}

/// @brief 将图片扩展名规范为 IMD 解析器能够查找的小写形式。
/// @param extension 原始扩展名。
/// @return 仅转换 ASCII 大写字母后的扩展名。
std::string lowerAsciiExtension(std::string extension)
{
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char character) {
                       return character >= 'A' && character <= 'Z'
                                  ? static_cast<char>(character - 'A' + 'a')
                                  : static_cast<char>(character);
                   });
    return extension;
}

/// @brief 生成不会破坏 IMD 首段命名规则的文件名片段。
/// @param value 用户选择的包名或谱面元数据文本。
/// @param fallback 清理后为空时使用的回退值。
/// @return 不包含下划线和路径非法字符的 UTF-8 文件名片段。
std::string sanitizeImdFileNamePart(std::string      value,
                                    std::string_view fallback)
{
    for ( char& character : value ) {
        const auto byte = static_cast<unsigned char>(character);
        if ( isInvalidFileNameCharacter(byte) ) character = '-';
    }
    value.erase(std::unique(value.begin(),
                            value.end(),
                            [](char lhs, char rhs) {
                                return lhs == '-' && rhs == '-';
                            }),
                value.end());
    while ( !value.empty() && (value.front() == ' ' || value.front() == '.' ||
                               value.front() == '-') ) {
        value.erase(value.begin());
    }
    while ( !value.empty() && (value.back() == ' ' || value.back() == '.' ||
                               value.back() == '-') ) {
        value.pop_back();
    }
    return value.empty() ? std::string(fallback) : value;
}

/// @brief 为本次导出创建唯一临时目录。
/// @param outputPath 目标包路径，用于生成可诊断的目录名。
/// @param directory 接收成功创建的临时目录。
/// @return 创建成功时返回 true。
bool createTemporaryDirectory(const std::filesystem::path& outputPath,
                              std::filesystem::path&       directory)
{
    std::error_code filesystemError;
    const auto      temporaryRoot =
        std::filesystem::temp_directory_path(filesystemError);
    if ( filesystemError || temporaryRoot.empty() ) return false;

    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string stem =
        sanitizeImdFileNamePart(Config::pathToUtf8(outputPath.stem()), "map");
    for ( std::uint32_t attempt = 0U; attempt < 16U; ++attempt ) {
        directory =
            temporaryRoot / Config::utf8ToPath("mmm-imd-export-" + stem + "-" +
                                               std::to_string(stamp) + "-" +
                                               std::to_string(attempt));
        filesystemError.clear();
        if ( std::filesystem::create_directory(directory, filesystemError) ) {
            return true;
        }
        if ( filesystemError && filesystemError != std::errc::file_exists ) {
            return false;
        }
    }
    return false;
}

/// @brief 清除导出副本中 RM/IMD 无法表达的玩家物件采样绑定。
/// @param beatMap 仅用于导出的可修改谱面副本。
void clearNoteSampleBindings(BeatMap& beatMap)
{
    const auto clearBindings = [](auto& notes) {
        for ( auto& note : notes ) {
            note.clearSampleBinding();
        }
    };
    clearBindings(beatMap.m_noteData.notes);
    clearBindings(beatMap.m_noteData.holds);
    clearBindings(beatMap.m_noteData.flicks);
    clearBindings(beatMap.m_noteData.polylines);
}

/// @brief 读取完整二进制文件。
/// @param path 来源路径。
/// @param bytes 接收文件字节。
/// @return 成功读取时返回 true。
bool readFileBytes(const std::filesystem::path& path,
                   std::vector<std::uint8_t>&   bytes)
{
    bytes.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if ( !file ) return false;
    const auto size = file.tellg();
    if ( size < 0 ) return false;
    bytes.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if ( !bytes.empty() ) {
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    return file.good();
}

/// @brief 将内存归档写入目标文件。
/// @param path 目标路径。
/// @param data 归档字节指针。
/// @param size 归档字节数。
/// @return 成功完整写入时返回 true。
bool writePackageFile(const std::filesystem::path& path, const void* data,
                      std::size_t size)
{
    std::error_code filesystemError;
    if ( !path.parent_path().empty() ) {
        std::filesystem::create_directories(path.parent_path(),
                                            filesystemError);
        if ( filesystemError ) return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    if ( size > 0U ) {
        file.write(static_cast<const char*>(data),
                   static_cast<std::streamsize>(size));
    }
    return file.good();
}

/// @brief 向 zip 归档添加一个根目录文件。
/// @param archive 正在写入的归档。
/// @param archiveName 包内 UTF-8 文件名。
/// @param sourcePath 来源文件路径。
/// @param errorMessage 接收失败原因。
/// @return 成功添加时返回 true。
bool addArchiveFile(mz_zip_archive& archive, const std::string& archiveName,
                    const std::filesystem::path& sourcePath,
                    std::string&                 errorMessage)
{
    std::vector<std::uint8_t> bytes;
    if ( !readFileBytes(sourcePath, bytes) ) {
        errorMessage =
            "无法读取待打包文件：" + Config::pathToUtf8(sourcePath.filename());
        return false;
    }
    const void* data = bytes.empty() ? nullptr : bytes.data();
    if ( !mz_zip_writer_add_mem(&archive,
                                archiveName.c_str(),
                                data,
                                bytes.size(),
                                MZ_BEST_COMPRESSION) ) {
        errorMessage = "无法压缩文件：" + archiveName;
        return false;
    }
    return true;
}

}  // namespace

ImdPackageExportResult ImdPackageExportService::exportPackage(
    const BeatMap&                                    beatMap,
    const std::vector<Audio::AudioTimelineLoadEvent>& audioEvents,
    double chartEndSeconds, const std::filesystem::path& coverPath,
    const std::filesystem::path& outputPath)
{
    ImdPackageExportResult result;
    if ( outputPath.empty() ) {
        result.errorMessage = "IMD 资源包输出路径为空";
        return result;
    }
    if ( beatMap.m_baseMapMetadata.track_count <= 0 ) {
        result.errorMessage = "IMD 资源包要求谱面轨道数大于零";
        return result;
    }

    std::error_code filesystemError;
    if ( !std::filesystem::is_regular_file(coverPath, filesystemError) ||
         filesystemError ) {
        result.errorMessage = "找不到谱面背景图片";
        return result;
    }
    const std::string coverExtension =
        lowerAsciiExtension(Config::pathToUtf8(coverPath.extension()));
    static constexpr std::array<std::string_view, 3> IMD_COVER_EXTENSIONS{
        ".png", ".jpg", ".jpeg"
    };
    if ( !packageExtensionInList(IMD_COVER_EXTENSIONS, coverExtension) ) {
        result.errorMessage = "IMD 资源包背景只支持 PNG、JPG 或 JPEG";
        return result;
    }

    std::string rawPrefix = Config::pathToUtf8(outputPath.stem());
    if ( rawPrefix.empty() ) {
        const auto& meta = beatMap.m_baseMapMetadata;
        rawPrefix        = !meta.title_unicode.empty()
                               ? meta.title_unicode
                               : (!meta.title.empty() ? meta.title : meta.name);
    }
    const std::string prefix =
        sanitizeImdFileNamePart(std::move(rawPrefix), "map");
    const std::string version =
        sanitizeImdFileNamePart(beatMap.m_baseMapMetadata.version, "default");
    result.beatmapFileName =
        prefix + "_" + std::to_string(beatMap.m_baseMapMetadata.track_count) +
        "k_" + version + ".imd";
    result.audioFileName = prefix + ".mp3";
    result.coverFileName = prefix + coverExtension;

    std::filesystem::path temporaryDirectory;
    if ( !createTemporaryDirectory(outputPath, temporaryDirectory) ) {
        result.errorMessage = "无法创建 IMD 资源包临时目录";
        return result;
    }
    const TemporaryDirectoryGuard cleanup{ temporaryDirectory };

    const auto sourceBeatmapPath = temporaryDirectory / "source.mmm";
    if ( !beatMap.saveToFile(sourceBeatmapPath) ) {
        result.errorMessage = "无法创建 IMD 导出副本";
        return result;
    }
    BeatMap exportBeatMap = BeatMap::loadFromFile(sourceBeatmapPath);
    clearNoteSampleBindings(exportBeatMap);
    exportBeatMap.m_audioSamples.clear();
    exportBeatMap.m_baseMapMetadata.bgm_track_count =
        std::max(exportBeatMap.m_baseMapMetadata.bgm_track_count, 1);
    exportBeatMap.m_audioSamples.push_back(AudioSampleEvent{
        .m_timestamp = 0.0,
        .m_offsetMs  = 0,
        .m_track     = static_cast<std::uint32_t>(
            exportBeatMap.m_baseMapMetadata.track_count),
        .m_audioResourceId = result.audioFileName,
        .m_volume          = 1.0F,
    });
    exportBeatMap.sync();

    const auto imdPath =
        temporaryDirectory / Config::utf8ToPath(result.beatmapFileName);
    if ( !exportBeatMap.saveToFile(imdPath) ) {
        result.errorMessage = "无法生成兼容的 IMD 谱面";
        return result;
    }

    const auto audioPath =
        temporaryDirectory / Config::utf8ToPath(result.audioFileName);
    const auto audioResult =
        Audio::AudioTimelineExportService::exportMixedAudio(
            Audio::AudioTimelineExportOptions{
                .events          = audioEvents,
                .chartEndSeconds = chartEndSeconds,
                .outputPath      = audioPath,
            });
    if ( !audioResult.success ) {
        result.errorMessage = "无法拼装 MP3 音频：" + audioResult.errorMessage;
        return result;
    }

    mz_zip_archive archive{};
    if ( !mz_zip_writer_init_heap(&archive, 0, 0) ) {
        result.errorMessage = "无法初始化 IMD 资源包压缩器";
        return result;
    }

    bool success =
        addArchiveFile(
            archive, result.beatmapFileName, imdPath, result.errorMessage) &&
        addArchiveFile(
            archive, result.audioFileName, audioPath, result.errorMessage) &&
        addArchiveFile(
            archive, result.coverFileName, coverPath, result.errorMessage);
    void*       archiveBuffer = nullptr;
    std::size_t archiveSize   = 0U;
    if ( success && !mz_zip_writer_finalize_heap_archive(
                        &archive, &archiveBuffer, &archiveSize) ) {
        result.errorMessage = "无法完成 IMD 资源包压缩";
        success             = false;
    }
    mz_zip_writer_end(&archive);

    if ( success &&
         !writePackageFile(outputPath, archiveBuffer, archiveSize) ) {
        result.errorMessage = "无法写入 IMD 资源包文件";
        success             = false;
    }
    if ( archiveBuffer ) mz_free(archiveBuffer);

    result.success = success;
    return result;
}

}  // namespace MMM::Logic
