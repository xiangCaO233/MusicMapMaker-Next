#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace MMM::Internal
{

/// @brief 将谱面流暂存于目标所在文件系统，关闭成功才替换旧文件。
/// @details 同卷重命名只在暂存文件关闭成功后执行，避免用户打开部分写入的谱面。
/// 如果序列化失败、存储空间不足或重命名失败，旧谱面仍由原路径访问。
/// 路径是本地文件；向 Android 授权文档的发布由保存入口另行确认。
/// 保留目标权限位及最终路径上的文件链接。替换会产生新的文件身份，
/// 调用方不得依赖 inode 或硬链接仍随本次保存更新。
/// 并发保存各用独立暂存目录，最终内容对应最后一次成功的重命名。
/// 正式路径所在父目录需允许创建和重命名文件。
/// @note 调用者使用原目标路径构建媒体引用，暂存目录仅用于写出。
/// @warning 低频 I/O。没有持久化屏障，不能把提交成功当作断电恢复保证。
class BeatmapOutputFile
{
public:
    /// @brief 独占创建暂存目录；失败时 stream().is_open() 为假。
    /// @param path 最终文件，父目录应由调用方提前建立。
    /// @param mode 具体格式的流模式，文本格式保留平台换行习惯。
    /// @details 使用非谱面后缀，工程扫描不会把暂存内容当成已完成的谱面。
    explicit BeatmapOutputFile(const std::filesystem::path& path,
                               std::ios::openmode mode = std::ios::out)
        : m_target(path)
    {
        std::error_code error;
        const auto      status = std::filesystem::symlink_status(path, error);
        // 保留用户已有的文件链接。损坏的链接不改写为普通文件。
        if ( std::filesystem::is_symlink(status) ) {
            m_target = std::filesystem::canonical(path, error);
            if ( error ) return;
        }
        // 只允许谱面常规文件或尚不存在的路径。目录和设备没有同样的替换语义。
        const auto targetStatus = std::filesystem::status(m_target, error);
        if ( error && error != std::errc::no_such_file_or_directory ) return;
        if ( std::filesystem::exists(targetStatus) &&
             !std::filesystem::is_regular_file(targetStatus) )
            return;
        m_permissions = targetStatus.permissions();

        // 目录创建是原子的，其他进程使用相同编号时只尝试下一个。
        // 多线程只共享名称计数器。创建目录的结果才确认跨进程的所有权。
        // relaxed 顺序即可，成功创建前不读写其他调用者的文件。
        static std::atomic<unsigned long> next{ 0 };
        for ( unsigned attempt = 0; attempt < 1024; ++attempt ) {
            const auto candidate =
                m_target.parent_path() /
                (".mmm-save-" +
                 std::to_string(next.fetch_add(1, std::memory_order_relaxed)));
            if ( std::filesystem::create_directory(candidate, error) ) {
                m_directory = candidate;
                m_staged    = m_directory / "content.tmp";
                m_stream.open(m_staged, mode);
                return;
            }
            if ( error && error != std::errc::file_exists ) return;
        }
    }

    /// @brief 回收未提交的文件；先关闭流以兼容 Windows 删除语义。
    /// @details 重命名成功时只剩空目录。仅清理自己创建的条目。
    ~BeatmapOutputFile()
    {
        if ( m_stream.is_open() ) m_stream.close();
        std::error_code error;
        if ( !m_staged.empty() ) std::filesystem::remove(m_staged, error);
        if ( !m_directory.empty() ) std::filesystem::remove(m_directory, error);
    }

    /// @brief 借用写入流，调用者须先检查流是否成功打开。
    std::ofstream& stream() { return m_stream; }

    /// @brief 检查缓冲写出和关闭错误后进行同文件系统替换。
    /// @return 只有完整暂存内容已替换到目标位置时为真。
    bool commit()
    {
        if ( !m_stream.is_open() ) return false;
        // 短写可能直到关闭才出现；close 保留已发生的流失败状态。
        m_stream.close();
        if ( m_stream.fail() ) return false;
        std::error_code error;
        // 覆盖已有谱面时保留其访问权限。权限设置失败同样不触碰旧文件。
        if ( m_permissions != std::filesystem::perms::unknown ) {
            std::filesystem::permissions(m_staged, m_permissions, error);
            if ( error ) return false;
        }
        std::filesystem::rename(m_staged, m_target, error);
        return !error;
    }

private:
    /// @brief 实际替换的位置，已有文件链接时为解析后的目标。
    std::filesystem::path m_target;
    /// @brief 本次独占的暂存目录，用于销毁时回收遗留文件。
    std::filesystem::path m_directory;
    /// @brief 同卷暂存文件，仅在 commit 成功后成为最终谱面。
    std::filesystem::path m_staged;
    /// @brief 原谱面的权限，新文件使用正常创建权限。
    std::filesystem::perms m_permissions{ std::filesystem::perms::unknown };
    /// @brief 由写出器借用的流，持有至关闭或清理。
    std::ofstream m_stream;
};

}  // namespace MMM::Internal
