#include "UpdaterPlatform.h"

#include <limits>
#include <ostream>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on

namespace MMM::Update
{
/// @brief 等待 Windows 父进程明确退出，超时或无法验证时返回 false。
/// @param pid 调用者传入的正数 Windows 进程 ID。
/// @return 进程已退出或 PID 已不存在时返回 true。
/// @note 同一个句柄固定进程身份，等待期间不再按 PID 重新打开。
/// @details 只请求等待所需权限，访问拒绝无法用于推断进程状态。
/// @warning 最多阻塞三十秒，只能在独立更新器进程调用。
bool waitForParent(long pid)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    // 正数 PID 不存在时系统返回
    // ERROR_INVALID_PARAMETER。拒绝权限不足等其他错误。
    if ( !process ) return GetLastError() == ERROR_INVALID_PARAMETER;
    // 句柄变为有信号状态才代表退出。WAIT_FAILED 和 WAIT_TIMEOUT 均阻止写入。
    // 关闭句柄前保存等待结果，后续清理不会覆盖用于决策的状态。
    const DWORD result = WaitForSingleObject(process, 30000);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
}

/// @brief 在目标所在卷暂存完整文件，再原子发布 Windows 更新。
/// @param downloadedFile 主程序已验证的下载包路径。
/// @param targetPath 已退出的主程序所在路径。
/// @return 失败时保全原安装与下载源，成功后清理旧版备份。
/// @note 进程中断最多留下私有暂存目录，正式路径在发布前仍指向旧程序。
bool install(const std::filesystem::path& downloadedFile,
             const std::filesystem::path& targetPath)
{
    namespace fs = std::filesystem;
    std::error_code error;
    auto            backup = targetPath;
    backup += L".old";
    // 正式路径与旧协议备份共用同一父目录。本轮不会在新文件验证前删备份。
    // 目标查询失败后不能再根据旧备份是否存在推断该安装的状态。
    const bool targetExists = fs::exists(targetPath, error);
    if ( error ) {
        writeStderr("Failed to check update target: " + error.message() + "\n");
        return false;
    }
    // 旧更新器可能已把唯一可用的程序移到 .old，先恢复可启动路径。
    // 恢复操作不允许覆盖目标，避免抹掉另一进程刚发布的完整程序。
    if ( !targetExists && fs::exists(backup, error) &&
         !MoveFileExW(
             backup.c_str(), targetPath.c_str(), MOVEFILE_WRITE_THROUGH) )
        error = { static_cast<int>(GetLastError()), std::system_category() };
    if ( error ) {
        writeStderr(
            "Failed to recover previous executable: " + error.message() + "\n");
        return false;
    }

    // 独占创建同卷目录，保证复制失败时不会留下半写的正式程序。
    // 每个进程只清理自己的目录，旧的 .old 在提交前一直保留。
    const auto staging =
        targetPath.parent_path() /
        (L".mmm-update-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    if ( !fs::create_directory(staging, error) ) {
        writeStderr("Failed to create update staging directory: " +
                    error.message() + "\n");
        return false;
    }
    // 暂存目录独占，因此复制不会碰到另一更新进程留下的文件。
    // 复制失败时保留最大限度的原始系统错误，例如源文件失效或目标卷空间不足。
    const auto staged = staging / L"executable";
    fs::copy_file(downloadedFile, staged, error);
    // 不允许跨卷回退为复制。正式路径只通过一次同卷改名提交。
    if ( !error &&
         !MoveFileExW(staged.c_str(),
                      targetPath.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) )
        error = { static_cast<int>(GetLastError()), std::system_category() };
    // 清理错误不覆盖 MoveFileExW 保存的错误码，以便分辨目标被锁与复制失败。
    // 若提交成功，staged 名称已经消失，清空的目录仍由本次进程负责删除。
    std::error_code cleanupError;
    fs::remove(staged, cleanupError);
    fs::remove(staging, cleanupError);
    if ( error ) {
        writeStderr("Failed to replace executable: " + error.message() + "\n");
        return false;
    }

    // 显式调用可能给源传入正式路径或旧备份。清理只在成功后进行，
    // 正式目标的别名不能作为已消费的下载文件删掉。
    if ( !fs::equivalent(downloadedFile, targetPath, cleanupError) &&
         !cleanupError )
        fs::remove(downloadedFile, cleanupError);
    fs::remove(backup, cleanupError);
    return true;
}

/// @brief 启动更新后的 Windows 可执行文件。
/// @param targetPath 可执行文件路径。
/// @return 成功请求系统启动时返回 true。
///
/// 工作目录设置为目标父目录，使更新后的程序继续按安装目录解析相对资源。
bool launchTarget(const std::filesystem::path& targetPath)
{
    // 路径及工作目录保留 Windows 原生 UTF-16，系统 ANSI 代码页不参与重启。
    const auto directory = targetPath.parent_path();
    // ShellExecute 大于 32 表示系统已接受启动请求，不代表子进程完成初始化。
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,
                                                   L"open",
                                                   targetPath.c_str(),
                                                   nullptr,
                                                   directory.c_str(),
                                                   SW_SHOWNORMAL)) > 32;
}

/// @brief 从宽命令行解析 Windows 父进程 PID。
/// @param text 十进制 ASCII 数字在 UTF-16 命令行中的表示。
/// @return 正数 PID，空参数、非数字或超出 long 范围时为零。
long parseParentPid(std::wstring_view text)
{
    long value = 0;
    for ( const wchar_t character : text ) {
        // 只接受 ASCII 形式的十进制数字，先检查范围再累加。
        if ( character < L'0' || character > L'9' ) return 0;
        const long digit = character - L'0';
        if ( value > (std::numeric_limits<long>::max() - digit) / 10 ) return 0;
        value = value * 10 + digit;
    }
    return value;
}

/// @brief 单文件安装直接使用调用方传入的可执行路径。
bool normalizeTarget(std::filesystem::path&)
{
    return true;
}

/// @brief 将标记留在目标可执行文件所在目录。
std::filesystem::path updateSuccessMarkerPath(
    const std::filesystem::path& target)
{
    return target.parent_path() / ".mm_update_success";
}

/// @brief 以 UTF-8 写入目标身份，避免 Windows 当前代码页改变路径。
void writeMarkerContents(std::ostream&                marker,
                         const std::filesystem::path& target)
{
    // 下游按 UTF-8 读取该标记。显式长度能够保留包含多字节字符的完整路径。
    // 写入本身是尽力操作，不会影响此时已经完成的事务。
    const auto text = target.u8string();
    marker.write(reinterpret_cast<const char*>(text.data()),
                 static_cast<std::streamsize>(text.size()));
}

}  // namespace MMM::Update

/// @brief Windows CRT 以 UTF-16 传入路径。
int wmain(int argc, wchar_t* argv[])
{
    // 解析与安装仍共享同一进程，不在中间构造窄参数或重新拉起更新器。
    // CMake 负责 MinGW 的宽入口选项，MSVC 使用自己的 CRT 入口选择。
    return MMM::Update::runUpdater(argc, argv);
}
