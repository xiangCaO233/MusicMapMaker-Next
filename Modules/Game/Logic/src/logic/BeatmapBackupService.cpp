#include "logic/BeatmapBackupService.h"

#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM::Logic
{
namespace
{
/// @brief 自动备份在项目隐藏配置目录中的相对根路径。
constexpr std::string_view BACKUP_ROOT_DIRECTORY = ".mmm/backups";

/// @brief 判断项目相对谱面路径是否不会逃逸出项目目录。
/// @param relativePath 待检查的词法相对路径。
/// @return 路径非空且不包含父目录分量时返回 true。
bool isSafeProjectRelativePath(const std::filesystem::path& relativePath)
{
    if ( relativePath.empty() || relativePath.is_absolute() ) return false;
    for ( const auto& part : relativePath ) {
        if ( part == ".." ) return false;
    }
    return true;
}

/// @brief 为项目外谱面路径生成稳定的短哈希。
/// @param path 谱面路径。
/// @return FNV-1a 64 位哈希。
std::uint64_t stablePathHash(const std::filesystem::path& path)
{
    constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t FNV_PRIME  = 1099511628211ULL;
    std::uint64_t           hash       = FNV_OFFSET;
    const auto              text = Config::pathToUtf8(path.lexically_normal());
    for ( const unsigned char value : text ) {
        hash ^= static_cast<std::uint64_t>(value);
        hash *= FNV_PRIME;
    }
    return hash;
}

/// @brief 判断文件名是否属于服务生成的正式备份。
/// @param path 待检查文件路径。
/// @return 文件名严格匹配固定宽度的自动备份格式时返回 true。
bool isManagedBackupFile(const std::filesystem::path& path)
{
    const auto            fileName        = Config::pathToUtf8(path.filename());
    constexpr std::size_t EXPECTED_LENGTH = 37U;
    if ( fileName.size() != EXPECTED_LENGTH ||
         !fileName.starts_with("backup-") || fileName[26] != '-' ||
         !fileName.ends_with(".mmm") ) {
        return false;
    }
    const auto allDigits = [&fileName](std::size_t begin, std::size_t end) {
        return std::all_of(
            fileName.begin() + static_cast<std::ptrdiff_t>(begin),
            fileName.begin() + static_cast<std::ptrdiff_t>(end),
            [](char value) { return value >= '0' && value <= '9'; });
    };
    return allDigits(7U, 26U) && allDigits(27U, 33U);
}

/// @brief 不跟随符号链接读取路径状态，并把路径不存在视为正常状态。
/// @param path 待检查路径。
/// @param status 接收文件状态。
/// @param filesystemError 接收真实文件系统错误。
/// @return 成功读取或确认路径不存在时返回 true。
bool readSymlinkStatus(const std::filesystem::path&  path,
                       std::filesystem::file_status& status,
                       std::error_code&              filesystemError)
{
    filesystemError.clear();
    status = std::filesystem::symlink_status(path, filesystemError);
    if ( filesystemError && filesystemError.default_error_condition() ==
                                std::errc::no_such_file_or_directory ) {
        filesystemError.clear();
        status = std::filesystem::file_status{
            std::filesystem::file_type::not_found
        };
    }
    return !filesystemError;
}

/// @brief 创建备份目录并拒绝任何符号链接目录分量。
/// @param projectRoot 当前项目根目录。
/// @param directory 需要创建并验证的单谱面备份目录。
/// @param errorMessage 接收失败原因。
/// @return 目录位于项目真实路径内且所有分量均为普通目录时返回 true。
bool prepareBackupDirectory(const std::filesystem::path& projectRoot,
                            const std::filesystem::path& directory,
                            std::string&                 errorMessage)
{
    std::error_code filesystemError;
    const auto      canonicalRoot =
        std::filesystem::weakly_canonical(projectRoot, filesystemError);
    if ( filesystemError ) {
        errorMessage = "无法解析项目真实路径：" + filesystemError.message();
        return false;
    }
    const auto relativeDirectory =
        directory.lexically_normal().lexically_relative(
            projectRoot.lexically_normal());
    if ( !isSafeProjectRelativePath(relativeDirectory) ) {
        errorMessage = "谱面备份目录不在项目目录内";
        return false;
    }

    auto current = projectRoot.lexically_normal();
    for ( const auto& part : relativeDirectory ) {
        current /= part;
        std::filesystem::file_status status;
        if ( !readSymlinkStatus(current, status, filesystemError) ) {
            errorMessage = "无法检查谱面备份目录：" +
                           Config::pathToUtf8(current) + "（" +
                           filesystemError.message() + "）";
            return false;
        }
        if ( std::filesystem::is_symlink(status) ) {
            errorMessage =
                "谱面备份目录包含符号链接：" + Config::pathToUtf8(current);
            return false;
        }
        if ( std::filesystem::exists(status) ) {
            if ( !std::filesystem::is_directory(status) ) {
                errorMessage =
                    "谱面备份路径不是目录：" + Config::pathToUtf8(current);
                return false;
            }
            continue;
        }

        filesystemError.clear();
        if ( !std::filesystem::create_directory(current, filesystemError) ||
             filesystemError ) {
            errorMessage = "无法创建谱面备份目录：" +
                           Config::pathToUtf8(current) + "（" +
                           filesystemError.message() + "）";
            return false;
        }
        if ( !readSymlinkStatus(current, status, filesystemError) ||
             std::filesystem::is_symlink(status) ||
             !std::filesystem::is_directory(status) ) {
            errorMessage =
                "谱面备份目录创建后验证失败：" + Config::pathToUtf8(current);
            return false;
        }
    }

    filesystemError.clear();
    const auto canonicalDirectory =
        std::filesystem::weakly_canonical(directory, filesystemError);
    if ( filesystemError ||
         !isSafeProjectRelativePath(
             canonicalDirectory.lexically_relative(canonicalRoot)) ) {
        errorMessage = "谱面备份目录的真实路径逃逸出项目目录";
        return false;
    }
    return true;
}

/// @brief 读取目录中最新的单调备份时间戳。
/// @param directory 单谱面备份目录。
/// @param latestMilliseconds 接收文件名中的最大毫秒值；无备份时保持 -1。
/// @param errorMessage 接收目录枚举失败信息。
/// @return 完成枚举时返回 true。
bool findLatestBackupMilliseconds(const std::filesystem::path& directory,
                                  std::int64_t& latestMilliseconds,
                                  std::string&  errorMessage)
{
    std::error_code                     filesystemError;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        filesystemError);
    const std::filesystem::directory_iterator end;
    if ( filesystemError ) {
        errorMessage = "无法枚举谱面备份目录：" + filesystemError.message();
        return false;
    }

    constexpr std::size_t TIMESTAMP_OFFSET = 7U;
    constexpr std::size_t TIMESTAMP_LENGTH = 19U;
    while ( iterator != end ) {
        if ( isManagedBackupFile(iterator->path()) ) {
            const auto fileName =
                Config::pathToUtf8(iterator->path().filename());
            if ( fileName.size() >= TIMESTAMP_OFFSET + TIMESTAMP_LENGTH ) {
                std::int64_t value  = -1;
                const char*  begin  = fileName.data() + TIMESTAMP_OFFSET;
                const char*  endPtr = begin + TIMESTAMP_LENGTH;
                const auto   parsed = std::from_chars(begin, endPtr, value);
                if ( parsed.ec == std::errc{} && parsed.ptr == endPtr ) {
                    latestMilliseconds = std::max(latestMilliseconds, value);
                }
            }
        }
        iterator.increment(filesystemError);
        if ( filesystemError ) {
            errorMessage =
                "无法完成谱面备份目录枚举：" + filesystemError.message();
            return false;
        }
    }
    return true;
}

/// @brief 删除超出保留数量的最旧备份。
/// @param directory 单个谱面的独立备份目录。
/// @param maxBackupCount 允许保留的备份数量。
/// @param removedCount 接收成功删除数量。
/// @param errorMessage 接收轮转失败信息。
/// @return 完整轮转成功时返回 true。
bool rotateBackups(const std::filesystem::path& directory, int maxBackupCount,
                   std::size_t& removedCount, std::string& errorMessage)
{
    std::vector<std::filesystem::path>  backups;
    std::error_code                     filesystemError;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        filesystemError);
    const std::filesystem::directory_iterator end;
    if ( filesystemError ) {
        errorMessage = "无法枚举谱面备份目录：" + filesystemError.message();
        return false;
    }

    while ( iterator != end ) {
        std::error_code itemError;
        if ( iterator->is_regular_file(itemError) && !itemError &&
             isManagedBackupFile(iterator->path()) ) {
            backups.push_back(iterator->path());
        }
        iterator.increment(filesystemError);
        if ( filesystemError ) {
            errorMessage =
                "无法完成谱面备份目录枚举：" + filesystemError.message();
            return false;
        }
    }

    std::sort(
        backups.begin(), backups.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.filename() < rhs.filename();
        });
    const auto safeCount =
        static_cast<std::size_t>(std::clamp(maxBackupCount,
                                            Config::AUTO_BACKUP_COUNT_MIN,
                                            Config::AUTO_BACKUP_COUNT_MAX));
    const std::size_t removeCount =
        backups.size() > safeCount ? backups.size() - safeCount : 0U;
    for ( std::size_t index = 0; index < removeCount; ++index ) {
        filesystemError.clear();
        if ( !std::filesystem::remove(backups[index], filesystemError) ||
             filesystemError ) {
            errorMessage =
                "无法删除最旧谱面备份：" + Config::pathToUtf8(backups[index]);
            if ( filesystemError ) {
                errorMessage += "（" + filesystemError.message() + "）";
            }
            return false;
        }
        ++removedCount;
    }
    return true;
}
}  // namespace

std::filesystem::path BeatmapBackupService::backupDirectory(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourceBeatmapPath)
{
    const auto normalizedRoot = projectRoot.lexically_normal();
    const auto sourceAbsolute =
        sourceBeatmapPath.is_absolute()
            ? sourceBeatmapPath.lexically_normal()
            : (normalizedRoot / sourceBeatmapPath).lexically_normal();
    const auto relativePath = sourceAbsolute.lexically_relative(normalizedRoot);

    auto directory = normalizedRoot / BACKUP_ROOT_DIRECTORY;
    if ( isSafeProjectRelativePath(relativePath) ) {
        directory /= relativePath;
        return directory;
    }

    auto fallbackName = sourceBeatmapPath.filename();
    if ( fallbackName.empty() ) fallbackName = "unnamed-beatmap";
    directory /= fallbackName;
    directory +=
        fmt::format("-external-{:016x}", stablePathHash(sourceAbsolute));
    return directory;
}

BeatmapBackupResult BeatmapBackupService::createBackup(
    const BeatMap& beatmap, const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourceBeatmapPath, int maxBackupCount)
{
    return createBackupAt(beatmap,
                          projectRoot,
                          sourceBeatmapPath,
                          maxBackupCount,
                          std::chrono::system_clock::now());
}

BeatmapBackupResult BeatmapBackupService::createBackupAt(
    const BeatMap& beatmap, const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourceBeatmapPath, int maxBackupCount,
    std::chrono::system_clock::time_point timestamp)
{
    BeatmapBackupResult result;
    if ( projectRoot.empty() || sourceBeatmapPath.empty() ) {
        result.m_errorMessage = "项目路径或谱面路径为空，无法创建自动备份";
        return result;
    }

    const auto directory = backupDirectory(projectRoot, sourceBeatmapPath);
    if ( !prepareBackupDirectory(
             projectRoot, directory, result.m_errorMessage) ) {
        return result;
    }

    std::int64_t latestMilliseconds = -1;
    if ( !findLatestBackupMilliseconds(
             directory, latestMilliseconds, result.m_errorMessage) ) {
        return result;
    }
    auto milliseconds = std::max<std::int64_t>(
        0,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch())
            .count());
    if ( milliseconds <= latestMilliseconds ) {
        if ( latestMilliseconds == std::numeric_limits<std::int64_t>::max() ) {
            result.m_errorMessage = "谱面备份时间戳已达到上限";
            return result;
        }
        milliseconds = latestMilliseconds + 1;
    }

    std::error_code       filesystemError;
    std::filesystem::path backupPath;
    std::filesystem::path pendingPath;
    for ( std::uint32_t suffix = 0; suffix < 1'000'000U; ++suffix ) {
        const auto fileName =
            fmt::format("backup-{:019d}-{:06d}.mmm", milliseconds, suffix);
        auto                         candidate = directory / fileName;
        std::filesystem::file_status candidateStatus;
        if ( !readSymlinkStatus(candidate, candidateStatus, filesystemError) ) {
            result.m_errorMessage =
                "无法检查谱面备份文件：" + filesystemError.message();
            return result;
        }
        if ( std::filesystem::exists(candidateStatus) ) continue;

        backupPath  = std::move(candidate);
        pendingPath = directory / (".pending-" + fileName);
        break;
    }
    if ( backupPath.empty() ) {
        result.m_errorMessage = "同一时间戳下的谱面备份数量过多";
        return result;
    }

    filesystemError.clear();
    std::filesystem::remove(pendingPath, filesystemError);
    if ( filesystemError ) {
        result.m_errorMessage =
            "无法清理谱面备份临时文件：" + filesystemError.message();
        return result;
    }
    if ( !beatmap.saveToFile(pendingPath) ) {
        std::error_code removeError;
        std::filesystem::remove(pendingPath, removeError);
        result.m_errorMessage = "无法写入谱面自动备份";
        return result;
    }

    filesystemError.clear();
    std::filesystem::rename(pendingPath, backupPath, filesystemError);
    if ( filesystemError ) {
        std::error_code removeError;
        std::filesystem::remove(pendingPath, removeError);
        result.m_errorMessage =
            "无法提交谱面自动备份：" + filesystemError.message();
        return result;
    }

    result.m_success    = true;
    result.m_backupPath = backupPath;
    static_cast<void>(rotateBackups(directory,
                                    maxBackupCount,
                                    result.m_removedBackupCount,
                                    result.m_errorMessage));
    return result;
}

}  // namespace MMM::Logic
