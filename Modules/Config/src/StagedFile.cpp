#include "config/StagedFile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace MMM::Config
{

/// @file StagedFile.cpp
/// @brief 实现低频写入的暂存目录归属与单文件发布。
/// @details 正式文件在最后一次操作才成为写目标；目录创建、内容写入、关闭和
/// 权限准备分别具有独立失败点。重命名使用操作系统的同卷替换语义。
///
/// @brief 通过文件系统排他确认目录归属，碰撞只重试下一候选。
ScopedTemporaryDirectory::ScopedTemporaryDirectory(
    const std::filesystem::path& parent, std::string_view prefix)
{
    // 时间戳用于分散不同进程或短期重启的候选，计数只用于分散当前进程内的并发。
    // 即使两者都碰撞，也要通过 create_directory 的结果决定所有权。
    static std::atomic<std::uint64_t> next{ 0 };
    const auto                        stamp = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto namePrefix = std::string(prefix) + stamp + '-';
    // 重试有上界，避免临时根存在大量同名旧目录时让保存任务无限等待。
    // 创建失败后没有独占路径，析构不能尝试删除曾经检查过的候选。
    for ( unsigned attempt = 0; attempt < 1024; ++attempt ) {
        // 计数减少同进程碰撞，跨进程的安全性来自排他创建。
        auto candidate =
            parent / (namePrefix + std::to_string(next.fetch_add(
                                       1, std::memory_order_relaxed)));
        if ( std::filesystem::create_directory(candidate, m_error) ) {
            m_path = std::move(candidate);
            return;
        }
        // 同名目录已经存在时标准库也可以返回 false 且无错误。
        if ( m_error && m_error != std::errc::file_exists ) return;
    }
    m_error = std::make_error_code(std::errc::file_exists);
}

/// @brief 仅回收由此对象成功创建的目录。
ScopedTemporaryDirectory::~ScopedTemporaryDirectory()
{
    if ( m_path.empty() ) return;
    std::error_code ignored;
    // 成功提交后目录通常为空。失败路径要同时回收内容和目录，即使其中还有
    // 下载分片。清理是尽力操作，不能覆盖调用方已经取得的失败诊断。
    std::filesystem::remove_all(m_path, ignored);
}

/// @brief 在目标同卷打开输出流，保留已有普通文件的权限与链接。
StagedOutputFile::StagedOutputFile(const std::filesystem::path& target,
                                   std::ios::openmode           mode)
    : m_target(target)
{
    if ( target.filename().empty() ) {
        m_error = std::make_error_code(std::errc::invalid_argument);
        return;
    }
    // 第一次查询不沿着链接走，以便先判别链接对象与尚未创建的正式文件。
    // 正常缺失属于允许的首次保存；访问拒绝等错误不能降级为首次保存。
    const auto status = std::filesystem::symlink_status(target, m_error);
    if ( m_error == std::errc::no_such_file_or_directory ) m_error.clear();
    if ( m_error ) return;
    if ( std::filesystem::is_symlink(status) ) {
        // 保留原链接；悬空链接或不可访问目标不能被替换成普通文件。
        m_target = std::filesystem::canonical(target, m_error);
        if ( m_error ) return;
    }
    // 解析最终文件链接之后，后续的卷位置与权限均以真实文件为准。
    // 对普通路径重新查询也让受限目录的异常传播到保存结果。
    const auto outputStatus = std::filesystem::status(m_target, m_error);
    if ( m_error == std::errc::no_such_file_or_directory ) m_error.clear();
    if ( m_error ) return;
    if ( std::filesystem::exists(outputStatus) &&
         !std::filesystem::is_regular_file(outputStatus) ) {
        // 目录、设备等不接受文本保存语义。
        m_error = std::make_error_code(std::errc::invalid_argument);
        return;
    }
    m_permissions = outputStatus.permissions();

    // 选择最终父目录内的子目录，使暂存内容即使从用户目录的另一个卷发布，
    // 文件系统也不需要使用覆盖复制。目录名称不取自正式文件名。
    m_directory.emplace(m_target.parent_path(), ".mmm-save-");
    if ( m_directory->error() ) {
        m_error = m_directory->error();
        return;
    }
    // 通用扩展名使扫描器不会把尚未关闭的文件当作可用的谱面或布局。
    // 流对象声明在目录之后，未提交销毁时也先释放文件句柄。
    m_staged = m_directory->path() / "content.tmp";
    // 文本和二进制模式由既有 writer 决定，保持历史换行及制品字节格式。
    // 打开失败仍保留独占目录所有权，析构会回收可能形成的空文件。
    m_stream.open(m_staged, mode);
    if ( !m_stream.is_open() )
        m_error = std::make_error_code(std::errc::io_error);
}

/// @brief 校验写出和关闭，保留权限后通过同卷重命名提交。
bool StagedOutputFile::commit()
{
    // 已完成的事务不能重复发布，关闭前仍需确保流确实打开。
    if ( !m_stream.is_open() ) return false;
    // close 刷新缓冲且保留写入错误；失败时绝不能发布短文件。
    m_stream.close();
    if ( m_stream.fail() ) {
        m_error = std::make_error_code(std::errc::io_error);
        return false;
    }
    // 替换产生新文件身份，因此在它可见前将旧文件的权限应用到新文件。
    // 新建文件没有原权限要保留，使用父目录和进程提供的默认权限。
    if ( m_permissions != std::filesystem::perms::unknown ) {
        std::filesystem::permissions(m_staged, m_permissions, m_error);
        if ( m_error ) return false;
    }
    // 任何重命名错误保留旧目标，作用域清理只删除自己的暂存路径。
    // 同一目标的两个调用不共享流。重命名成功的次序就是文件版本生效的次序，
    // 早先写得较慢的调用可以最后提交，不会与另一份文件内容混合。
    std::filesystem::rename(m_staged, m_target, m_error);
    return !m_error;
}

}  // namespace MMM::Config
