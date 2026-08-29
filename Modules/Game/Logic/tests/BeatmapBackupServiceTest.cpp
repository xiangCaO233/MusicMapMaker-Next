#include "logic/BeatmapBackupService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace
{
/// @brief 收集服务管理的正式 MMM 备份文件。
/// @param directory 单谱面备份目录。
/// @return 文件名使用 backup- 前缀的普通文件。
std::vector<std::filesystem::path> collectBackups(
    const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path>  backups;
    std::error_code                     error;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    if ( error ) return backups;

    while ( iterator != end ) {
        std::error_code itemError;
        const auto      fileName =
            MMM::Config::pathToUtf8(iterator->path().filename());
        if ( iterator->is_regular_file(itemError) && !itemError &&
             fileName.starts_with("backup-") &&
             iterator->path().extension() == ".mmm" ) {
            backups.push_back(iterator->path());
        }
        iterator.increment(error);
        if ( error ) {
            backups.clear();
            return backups;
        }
    }
    return backups;
}

/// @brief 检查目录中是否遗留自动备份临时文件。
/// @param directory 单谱面备份目录。
/// @return 发现 .pending- 前缀文件或目录枚举失败时返回 true。
bool hasPendingBackup(const std::filesystem::path& directory)
{
    std::error_code                     error;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    if ( error ) return true;
    while ( iterator != end ) {
        const auto fileName =
            MMM::Config::pathToUtf8(iterator->path().filename());
        if ( fileName.starts_with(".pending-") ) return true;
        iterator.increment(error);
        if ( error ) return true;
    }
    return false;
}

/// @brief 使用确定性毫秒时间点创建自动备份。
/// @param beatmap 待备份谱面。
/// @param root 测试项目根目录。
/// @param source 谱面源路径。
/// @param maxCount 保留数量。
/// @param milliseconds 系统时钟毫秒值。
/// @return 服务执行结果。
MMM::Logic::BeatmapBackupResult createAt(const MMM::BeatMap&          beatmap,
                                         const std::filesystem::path& root,
                                         const std::filesystem::path& source,
                                         int                          maxCount,
                                         std::int64_t milliseconds)
{
    return MMM::Logic::BeatmapBackupService::createBackupAt(
        beatmap,
        root,
        source,
        maxCount,
        std::chrono::system_clock::time_point(
            std::chrono::milliseconds(milliseconds)));
}

/// @brief 验证单谱面轮转、同时间戳去重及不同谱面隔离。
/// @param outputRoot 测试输出目录。
/// @return 全部备份行为符合预期时返回 true。
bool testBackupRotation(const std::filesystem::path& outputRoot)
{
    std::error_code error;
    std::filesystem::remove_all(outputRoot, error);
    error.clear();
    std::filesystem::create_directories(outputRoot / "charts", error);
    if ( error ) {
        XERROR("Failed to create backup test output: {}", error.message());
        return false;
    }
    if ( std::filesystem::exists(outputRoot / ".mmm", error) || error ) {
        XERROR(
            "Backup test unexpectedly started with an existing .mmm directory");
        return false;
    }

    MMM::BeatMap beatmap;
    beatmap.m_baseMapMetadata.name     = "Backup Test";
    beatmap.m_baseMapMetadata.version  = "Hard";
    beatmap.m_baseMapMetadata.map_path = "charts/song.osu";

    const auto source = outputRoot / "charts/song.osu";
    const auto first  = createAt(beatmap, outputRoot, source, 2, 1000);
    const auto second = createAt(beatmap, outputRoot, source, 2, 2000);
    const auto third  = createAt(beatmap, outputRoot, source, 2, 3000);
    const auto directory =
        MMM::Logic::BeatmapBackupService::backupDirectory(outputRoot, source);
    auto restored = MMM::BeatMap::loadFromFile(third.m_backupPath);
    if ( !first.m_success || !second.m_success || !third.m_success ||
         third.m_removedBackupCount != 1U ||
         collectBackups(directory).size() != 2U ||
         std::filesystem::exists(first.m_backupPath, error) ||
         restored.m_baseMapMetadata.name != "Backup Test" ) {
        XERROR("Per-beatmap backup rotation did not retain the newest files");
        return false;
    }

    const auto sameTimestamp = createAt(beatmap, outputRoot, source, 2, 3000);
    if ( !sameTimestamp.m_success ||
         sameTimestamp.m_backupPath == third.m_backupPath ||
         collectBackups(directory).size() != 2U ) {
        XERROR("Backup name collision was not resolved safely");
        return false;
    }

    const auto clockRollback = createAt(beatmap, outputRoot, source, 2, 500);
    if ( !clockRollback.m_success ||
         clockRollback.m_backupPath.filename() <=
             sameTimestamp.m_backupPath.filename() ||
         !std::filesystem::exists(clockRollback.m_backupPath, error) ||
         collectBackups(directory).size() != 2U ) {
        XERROR("Clock rollback did not preserve monotonic backup rotation");
        return false;
    }

    const auto otherSource = outputRoot / "charts/other.mmm";
    const auto other = createAt(beatmap, outputRoot, otherSource, 1, 4000);
    const auto otherDirectory =
        MMM::Logic::BeatmapBackupService::backupDirectory(outputRoot,
                                                          otherSource);
    if ( !other.m_success || collectBackups(otherDirectory).size() != 1U ||
         collectBackups(directory).size() != 2U ) {
        XERROR("Backup rotation crossed beatmap directory boundaries");
        return false;
    }

    const auto escapeDirectory = outputRoot.parent_path() / "backup_escape";
    std::filesystem::remove_all(escapeDirectory, error);
    error.clear();
    std::filesystem::create_directories(escapeDirectory, error);
    if ( error ) return false;
    const auto linkedDirectory = outputRoot / ".mmm/backups/linked";
    std::filesystem::create_directory_symlink(
        escapeDirectory, linkedDirectory, error);
    if ( !error ) {
        const auto linkedSource = outputRoot / "linked/chart.mmm";
        const auto linkedResult =
            createAt(beatmap, outputRoot, linkedSource, 2, 5000);
        if ( linkedResult.m_success ||
             !std::filesystem::is_empty(escapeDirectory, error) || error ) {
            XERROR("Backup service followed a symlink outside the project");
            return false;
        }
    }

    if ( hasPendingBackup(directory) ) {
        XERROR("Committed backup left a pending file behind");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行谱面自动备份写入与数量轮转测试。
/// @param argc 参数数量。
/// @param argv 第一个参数为构建目录下的测试输出路径。
/// @return 全部测试通过时返回 0。
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        XERROR("BeatmapBackupServiceTest requires an output directory");
        return 1;
    }
    return testBackupRotation(MMM::Config::utf8ToPath(argv[1])) ? 0 : 1;
}
