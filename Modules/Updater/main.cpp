/**
 * @file main.cpp
 * @brief 独立更新器辅助程序
 *
 * 用法: MusicMapMaker-Updater <downloaded_file> <target_exe> <parent_pid>
 *
 * 流程:
 *   1. 等待父进程 (parent_pid) 退出
 *   2. 将 downloaded_file 复制/替换到 target_exe
 *   3. 启动 target_exe
 *   4. 退出
 *
 * 完全独立，不依赖任何项目库，仅使用标准库和平台 API。
 */

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <shellapi.h>
#    include <windows.h>
#else
#    include <signal.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace
{

#if defined(_WIN32)

/// @brief 检查指定 PID 进程是否仍在运行
bool isProcessAlive(DWORD pid)
{
    HANDLE hProcess =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if ( !hProcess ) return false;
    DWORD exitCode;
    GetExitCodeProcess(hProcess, &exitCode);
    CloseHandle(hProcess);
    return exitCode == STILL_ACTIVE;
}

/// @brief 等待父进程退出（最多等待 30 秒）
void waitForParent(DWORD pid)
{
    HANDLE hProcess =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if ( hProcess ) {
        WaitForSingleObject(hProcess, 30000);
        CloseHandle(hProcess);
    } else {
        for ( int i = 0; i < 60; ++i ) {
            if ( !isProcessAlive(pid) ) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

/// @brief 启动目标可执行文件
bool launchTarget(const std::string& targetPath)
{
    std::string dir = std::filesystem::path(targetPath).parent_path().string();
    return reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr,
                                                   "open",
                                                   targetPath.c_str(),
                                                   nullptr,
                                                   dir.c_str(),
                                                   SW_SHOWNORMAL)) > 32;
}

#else

/// @brief 检查指定 PID 进程是否仍在运行
bool isProcessAlive(pid_t pid)
{
    return kill(pid, 0) == 0;
}

/// @brief 等待父进程退出（最多等待 30 秒）
void waitForParent(pid_t pid)
{
    for ( int i = 0; i < 60; ++i ) {
        if ( !isProcessAlive(pid) ) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

/// @brief 启动目标可执行文件
bool launchTarget(const std::string& targetPath)
{
    pid_t child = fork();
    if ( child == 0 ) {
        // 子进程：分离终端并启动新程序
        setsid();
        execl(targetPath.c_str(), targetPath.c_str(), nullptr);
        _exit(1);
    }
    return child > 0;
}

#endif

}  // namespace

int main(int argc, char* argv[])
{
    if ( argc != 4 ) {
        std::fprintf(stderr,
                     "Usage: %s <downloaded_file> <target_exe> <parent_pid>\n",
                     argv[0]);
        return 1;
    }

    std::string downloadedFile = argv[1];
    std::string targetExe      = argv[2];
    long        parentPid      = std::atol(argv[3]);

    // 1. 等待父进程退出
    waitForParent(static_cast<decltype(::getpid())>(parentPid));

    // 再额外等半秒确保文件句柄释放
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 2. 复制文件
    std::error_code ec;
#if defined(_WIN32)
    // Windows: 目标文件仍被占用时改用临时方案
    std::string backupPath = targetExe + ".old";
    if ( std::filesystem::exists(targetExe) ) {
        std::filesystem::rename(targetExe, backupPath, ec);
    }
    std::filesystem::copy_file(
        downloadedFile,
        targetExe,
        std::filesystem::copy_options::overwrite_existing,
        ec);
    if ( ec ) {
        std::fprintf(
            stderr, "Failed to copy update: %s\n", ec.message().c_str());
        // 尝试恢复
        if ( std::filesystem::exists(backupPath) ) {
            std::filesystem::rename(backupPath, targetExe);
        }
        return 1;
    }
    // 删除备份和下载文件
    if ( std::filesystem::exists(backupPath) ) {
        std::filesystem::remove(backupPath);
    }
    std::filesystem::remove(downloadedFile);
#else
    // Linux/macOS: 直接 rename 是原子操作
    std::filesystem::rename(downloadedFile, targetExe, ec);
    if ( ec ) {
        // rename 失败则尝试 copy + remove
        std::filesystem::copy_file(
            downloadedFile,
            targetExe,
            std::filesystem::copy_options::overwrite_existing,
            ec);
        std::filesystem::remove(downloadedFile);
    }
    if ( ec ) {
        std::fprintf(
            stderr, "Failed to replace executable: %s\n", ec.message().c_str());
        return 1;
    }
    // 确保可执行权限
    std::filesystem::permissions(targetExe,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add,
                                 ec);
#endif

    // 2.5. 写入更新成功标记文件
    {
        std::filesystem::path markerPath =
            std::filesystem::path(targetExe).parent_path() /
            ".mm_update_success";
        std::ofstream marker(markerPath);
        if ( marker.is_open() ) {
            marker << targetExe;
        }
    }

    // 3. 启动新版本
    if ( !launchTarget(targetExe) ) {
        std::fprintf(stderr, "Failed to launch updated executable\n");
        return 1;
    }

    return 0;
}
