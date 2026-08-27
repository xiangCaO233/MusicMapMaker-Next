#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{

/// @brief 单次谱面备份写入与轮转结果。
struct BeatmapBackupResult {
    /// @brief 是否成功写入新的备份文件。
    bool m_success{ false };

    /// @brief 成功写入的备份文件路径。
    std::filesystem::path m_backupPath;

    /// @brief 本次轮转删除的旧备份数量。
    std::size_t m_removedBackupCount{ 0U };

    /// @brief 写入失败或轮转不完整时的诊断信息。
    std::string m_errorMessage;
};

/// @brief 将谱面快照写入项目隐藏目录并按单谱面数量轮转。
class BeatmapBackupService
{
public:
    /// @brief 获取指定谱面的独立备份目录。
    /// @param projectRoot 当前项目根目录。
    /// @param sourceBeatmapPath 当前谱面源文件路径。
    /// @return 位于 .mmm/backups 下且不会被项目资源扫描器导入的目录。
    [[nodiscard]] static std::filesystem::path backupDirectory(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& sourceBeatmapPath);

    /// @brief 使用当前系统时间写入一个原生 MMM 备份并轮转旧文件。
    /// @param beatmap 已同步的谱面快照。
    /// @param projectRoot 当前项目根目录。
    /// @param sourceBeatmapPath 当前谱面源文件路径。
    /// @param maxBackupCount 每个谱面允许保留的备份数量。
    /// @return 备份路径、轮转数量与错误信息。
    /// @warning 低频阻塞文件系统路径：只允许自动备份到期或事件触发时调用。
    [[nodiscard]] static BeatmapBackupResult createBackup(
        const BeatMap& beatmap, const std::filesystem::path& projectRoot,
        const std::filesystem::path& sourceBeatmapPath, int maxBackupCount);

    /// @brief 使用指定时间写入一个原生 MMM 备份并轮转旧文件。
    /// @param beatmap 已同步的谱面快照。
    /// @param projectRoot 当前项目根目录。
    /// @param sourceBeatmapPath 当前谱面源文件路径。
    /// @param maxBackupCount 每个谱面允许保留的备份数量。
    /// @param timestamp 用于生成可排序文件名的系统时间。
    /// @return 备份路径、轮转数量与错误信息。
    /// @warning 测试与低频持久化路径：会创建目录、写文件、枚举并删除旧备份。
    [[nodiscard]] static BeatmapBackupResult createBackupAt(
        const BeatMap& beatmap, const std::filesystem::path& projectRoot,
        const std::filesystem::path& sourceBeatmapPath, int maxBackupCount,
        std::chrono::system_clock::time_point timestamp);
};

}  // namespace MMM::Logic
