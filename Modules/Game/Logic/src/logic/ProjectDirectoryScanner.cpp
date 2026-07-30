#include "logic/ProjectDirectoryScanner.h"
#include "config/Utf8Path.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace MMM::Logic
{

/// @brief 从指定项目根目录递归扫描谱面和音频文件。
/// @param projectRoot 作为递归扫描起点的项目根目录。
/// @return 记录扫描成功状态、谱面文件列表和音频文件列表的扫描结果。
ProjectDirectoryScanner::ScanResult ProjectDirectoryScanner::scan(
    const std::filesystem::path& projectRoot) const
{
    /// @brief 本次目录扫描的输出结果。
    ScanResult result;
    /// @brief 文件系统 API 返回的错误码，避免目录遍历时抛出异常。
    std::error_code filesystemError;

    if ( projectRoot.empty() ||
         !std::filesystem::exists(projectRoot, filesystemError) ||
         filesystemError ) {
        return result;
    }
    filesystemError.clear();

    if ( !std::filesystem::is_directory(projectRoot, filesystemError) ||
         filesystemError ) {
        return result;
    }
    filesystemError.clear();

    /// @brief 目录遍历选项，跳过无权限目录以降低同步失败概率。
    constexpr auto directoryOptions =
        std::filesystem::directory_options::skip_permission_denied;
    /// @brief 当前递归目录迭代器，负责逐个访问项目目录下的条目。
    std::filesystem::recursive_directory_iterator iterator(
        projectRoot, directoryOptions, filesystemError);
    /// @brief 递归目录迭代终点，用于判断扫描是否完成。
    const std::filesystem::recursive_directory_iterator endIterator;
    if ( filesystemError ) {
        return result;
    }

    while ( iterator != endIterator ) {
        /// @brief 当前正在检查的目录条目。
        const auto& entry = *iterator;
        if ( entry.path().lexically_relative(projectRoot) ==
                 std::filesystem::path(".mmm") &&
             entry.is_directory(filesystemError) && !filesystemError ) {
            iterator.disable_recursion_pending();
        }
        filesystemError.clear();
        if ( entry.is_regular_file(filesystemError) && !filesystemError ) {
            /// @brief 当前条目对应的完整文件系统路径。
            const auto path = entry.path();
            if ( isBeatmapFile(path) ) {
                result.m_beatmapFiles.push_back(path);
            } else if ( isAudioFile(path) ) {
                result.m_audioFiles.push_back(path);
            }
        }
        filesystemError.clear();

        iterator.increment(filesystemError);
        if ( filesystemError ) {
            return result;
        }
    }

    result.m_success = true;
    return result;
}

/// @brief 判断路径扩展名是否属于支持的谱面格式。
/// @param path 需要检查扩展名的文件系统路径。
/// @return 扩展名属于支持的谱面格式时返回 true。
bool ProjectDirectoryScanner::isBeatmapFile(const std::filesystem::path& path)
{
    /// @brief 规范化后的文件扩展名。
    const auto extension = normalizedExtension(path);
    return extension == ".osu" || extension == ".imd" || extension == ".mc" ||
           extension == ".mmm";
}

/// @brief 判断路径扩展名是否属于支持的音频格式。
/// @param path 需要检查扩展名的文件系统路径。
/// @return 扩展名属于支持的音频格式时返回 true。
bool ProjectDirectoryScanner::isAudioFile(const std::filesystem::path& path)
{
    /// @brief 规范化后的文件扩展名。
    const auto extension = normalizedExtension(path);
    return extension == ".mp3" || extension == ".ogg" || extension == ".wav" ||
           extension == ".flac" || extension == ".opus" ||
           extension == ".aac" || extension == ".m4a";
}

/// @brief 将路径扩展名转换为小写 UTF-8 字符串。
/// @param path 需要读取扩展名的文件系统路径。
/// @return 规范化后的小写扩展名字符串。
std::string ProjectDirectoryScanner::normalizedExtension(
    const std::filesystem::path& path)
{
    /// @brief 从文件系统路径提取出的 UTF-8 扩展名字符串。
    auto extension = Config::pathToUtf8(path.extension());
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        /// @brief 将单个扩展名字符转换为小写字符。
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension;
}

}  // namespace MMM::Logic
