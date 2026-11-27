#include "config/StagedFile.h"

#if defined(_WIN32)
#    include <sdkddkver.h>
// 仅在此文件开放较新的枚举声明；实际调用的函数自 Vista 即存在。
// 旧系统会拒绝扩展信息类，运行时仍走兼容重命名。
// 宏作用域恢复后，随后包含的头文件仍按构建配置的目标版本选择声明。
#    pragma push_macro("NTDDI_VERSION")
#    if NTDDI_VERSION < NTDDI_WIN10_RS1
#        undef NTDDI_VERSION
#        define NTDDI_VERSION NTDDI_WIN10_RS1
#    endif
#    include <windows.h>
#    pragma pop_macro("NTDDI_VERSION")

#    include <cstddef>
#    include <cstring>
#    include <limits>
#    include <mutex>
#    include <vector>
#endif

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
#if defined(_WIN32)
namespace
{
/// @brief 使用文件句柄将完整暂存文件替换到正式路径。
/// @param staged 已关闭并位于目标同卷的暂存文件。
/// @param target 最终文件路径，可能相对于当前目录。
/// @return 发布失败的系统错误；成功时为空。
/// @details POSIX 替换允许具有兼容共享权限的旧句柄继续访问旧文件；
/// 之后按目标名称打开的读者直接得到新文件，避免并发发布互相拒绝。
/// 源和目标的共享权限由内核判定，不能通过覆盖复制或先删除目标规避。
/// 失败不改变正式名称，暂存目录仍由事务对象清理。
/// @warning 低频文件提交。仅旧式接口回退需要同步重命名，流操作不在锁内。
std::error_code replaceStagedFile(const std::filesystem::path& staged,
                                  const std::filesystem::path& target)
{
    // Windows 以绝对路径解析目标，避免用额外目录句柄绑定相对名称。
    // 构造阶段可能已把文件链接解析到实际目标；这里不再做链接或权限查询。
    std::error_code error;
    auto            absoluteTarget = std::filesystem::absolute(target, error);
    if ( error ) return error;
    // 使用路径提供的原生 UTF-16，不能经过系统 ANSI 代码页丢失名称字符。
    const auto&    name        = absoluteTarget.make_preferred().native();
    constexpr auto NAME_OFFSET = offsetof(FILE_RENAME_INFO, FileName);
    constexpr auto MAX_NAME_SIZE =
        ((std::numeric_limits<DWORD>::max)() - NAME_OFFSET) / sizeof(wchar_t) -
        1;
    // 请求长度是 DWORD，先校验再计算，末尾另留一个宽字符作为终止符。
    if ( name.size() > MAX_NAME_SIZE )
        return std::make_error_code(std::errc::filename_too_long);
    const auto nameBytes = name.size() * sizeof(wchar_t);
    const auto size      = NAME_OFFSET + nameBytes + sizeof(wchar_t);
    // 请求缓冲在取得句柄前完成构造，打开期间不分配内存或访问其他路径。
    std::vector<std::byte> request(size);
    FILE_RENAME_INFO       info{};
    // 允许替换已有目标，但保留系统对实际只读属性与访问权限的检查。
    info.Flags =
        FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info.FileNameLength = static_cast<DWORD>(nameBytes);
    // 分开复制固定头与变长部分，避免访问 FILE_RENAME_INFO 单元素数组的边界外。
    // 缓冲区初始化为零，终止字符未计入 FileNameLength。
    std::memcpy(request.data(), &info, NAME_OFFSET);
    std::memcpy(request.data() + NAME_OFFSET, name.c_str(), nameBytes);

    // DELETE 用于改名，允许其他读写及删除共享。本句柄不能阻拦下一次发布。
    // 流已经关闭，这个句柄只负责把同卷文件发布到目标，新内容不再写入。
    // 打开暂存项本身即可提交，若路径意外变成重解析点也不跟随到外部文件。
    const auto handle =
        CreateFileW(staged.c_str(),
                    DELETE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
    if ( handle == INVALID_HANDLE_VALUE )
        return { static_cast<int>(GetLastError()), std::system_category() };
    const bool renamed = SetFileInformationByHandle(
        handle, FileRenameInfoEx, request.data(), static_cast<DWORD>(size));
    // 在关闭句柄前捕获失败码，保持调用方看到的是发布错误。
    // 兼容回退重新按路径打开源，因此无论如何都先还给系统这个句柄。
    const auto reason = renamed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if ( renamed ) return {};

    // 系统或卷不支持扩展信息类时沿用原路径。访问拒绝保留原始失败诊断。
    // 扩展参数被拒绝时旧操作仍负责验证实际路径，且不会清除原有目标。
    if ( reason != ERROR_INVALID_PARAMETER && reason != ERROR_NOT_SUPPORTED &&
         reason != ERROR_INVALID_FUNCTION )
        return { static_cast<int>(reason), std::system_category() };

    // 旧接口并发调用时可能暂时持有目标。只串行化本进程内的回退重命名。
    // 回退临界区不能覆盖调用方序列化或下一阶段的暂存目录清理。
    static std::mutex     renameMutex;
    const std::lock_guard lock(renameMutex);
    std::filesystem::rename(staged, target, error);
    return error;
}
}  // namespace
#endif

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
/// @warning 低频 I/O；Windows 使用句柄替换，旧式回退只同步重命名调用。
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
    // 重命名成功的次序就是文件版本生效的次序，两次保存的字节不会混合。
    // 两条平台路径共用已关闭、已准备好权限的源文件，不能在失败时重新写入。
#if defined(_WIN32)
    m_error = replaceStagedFile(m_staged, m_target);
#else
    std::filesystem::rename(m_staged, m_target, m_error);
#endif
    return !m_error;
}

}  // namespace MMM::Config
