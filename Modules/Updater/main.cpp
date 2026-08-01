/**
 * @file main.cpp
 * @brief 独立更新器辅助程序
 *
 * 用法: MusicMapMaker-Updater <downloaded_file> <target_path> <parent_pid>
 *
 * 流程:
 *   1. 等待父进程 (parent_pid) 退出
 *   2. Windows/Linux 替换单个可执行文件，macOS 解压并替换完整 .app
 *   3. 写入更新成功标记
 *   4. 启动新版本
 *
 * 完全独立，不依赖任何项目库，仅使用标准库和平台 API。
 */

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
// clang-format off
#    include <windows.h>
#    include <shellapi.h>
// clang-format on
#else
#    include <signal.h>
#    include <sys/types.h>
#    include <unistd.h>
#    if defined(__APPLE__)
#        include <fcntl.h>
#        include <spawn.h>
#        include <sys/stdio.h>
#        include <sys/wait.h>

extern char** environ;
#    endif
#endif

/// @brief 向标准错误流写入独立更新器诊断。
/// @param message 诊断内容。
void writeStderr(std::string_view message)
{
    if ( message.empty() ) return;
    std::fwrite(message.data(), 1, message.size(), stderr);
}

namespace
{

#if defined(_WIN32)

/// @brief 检查指定 PID 进程是否仍在运行。
/// @param pid 需要检查的 Windows 进程 ID。
/// @return 进程仍在运行时返回 true。
bool isProcessAlive(DWORD pid)
{
    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if ( !hProcess ) return false;
    const DWORD waitResult = WaitForSingleObject(hProcess, 0);
    CloseHandle(hProcess);
    return waitResult == WAIT_TIMEOUT;
}

/// @brief 等待父进程退出，最多等待 30 秒。
/// @param pid 父进程 ID。
void waitForParent(DWORD pid)
{
    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
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

/// @brief 启动更新后的 Windows 可执行文件。
/// @param targetPath 可执行文件路径。
/// @return 成功请求系统启动时返回 true。
bool launchTarget(const std::string& targetPath)
{
    std::string dir =
        std::filesystem::path(targetPath).parent_path().generic_string();
    return reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr,
                                                   "open",
                                                   targetPath.c_str(),
                                                   nullptr,
                                                   dir.c_str(),
                                                   SW_SHOWNORMAL)) > 32;
}

#else

/// @brief 检查指定 PID 进程是否仍在运行。
/// @param pid 需要检查的 POSIX 进程 ID。
/// @return 进程仍在运行或无权探测时返回 true。
bool isProcessAlive(pid_t pid)
{
    return kill(pid, 0) == 0 || errno == EPERM;
}

/// @brief 等待父进程退出，最多等待 30 秒。
/// @param pid 父进程 ID。
void waitForParent(pid_t pid)
{
    for ( int i = 0; i < 60; ++i ) {
        if ( !isProcessAlive(pid) ) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

#    if defined(__APPLE__)

/// @brief 启动 macOS App bundle。
/// @param targetPath App bundle 根路径。
/// @return 成功创建启动进程时返回 true。
bool launchTarget(const std::string& targetPath)
{
    const pid_t child = fork();
    if ( child == 0 ) {
        setsid();
        execl("/usr/bin/open",
              "open",
              "-n",
              targetPath.c_str(),
              static_cast<char*>(nullptr));
        _exit(1);
    }
    return child > 0;
}

/// @brief 运行系统工具并等待其结束。
/// @param executable 系统工具绝对路径。
/// @param arguments 不含 argv[0] 的参数列表。
/// @return 工具以 0 状态正常退出时返回 true。
bool runSystemTool(const char*                     executable,
                   const std::vector<std::string>& arguments)
{
    std::vector<std::string> mutableArguments = arguments;
    std::vector<char*>       argv;
    argv.reserve(mutableArguments.size() + 2);
    argv.push_back(const_cast<char*>(executable));
    for ( std::string& argument : mutableArguments ) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    pid_t child = 0;
    if ( posix_spawn(
             &child, executable, nullptr, nullptr, argv.data(), environ) !=
         0 ) {
        return false;
    }

    int waitStatus = 0;
    while ( waitpid(child, &waitStatus, 0) < 0 ) {
        if ( errno != EINTR ) return false;
    }
    return WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0;
}

/// @brief 尽力删除更新过程使用的专属临时目录。
/// @param path 仅由本次更新创建的临时路径。
void removeTreeBestEffort(const std::filesystem::path& path)
{
    std::error_code removeError;
    std::filesystem::remove_all(path, removeError);
}

/// @brief 在解压目录中定位唯一的目标 App bundle。
/// @param stagingRoot 解压根目录。
/// @param bundleName 目标 App bundle 文件名。
/// @return 找到唯一匹配目录时返回其路径，否则返回空路径。
std::filesystem::path findExtractedApplication(
    const std::filesystem::path& stagingRoot,
    const std::filesystem::path& bundleName)
{
    const std::filesystem::path directPath = stagingRoot / bundleName;
    std::error_code             directError;
    if ( std::filesystem::is_directory(directPath, directError) &&
         !directError ) {
        return directPath;
    }

    std::filesystem::path                         foundPath;
    std::error_code                               iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        stagingRoot,
        std::filesystem::directory_options::skip_permission_denied,
        iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    while ( !iteratorError && iterator != end ) {
        std::error_code entryError;
        if ( iterator->path().filename() == bundleName &&
             iterator->is_directory(entryError) && !entryError ) {
            if ( !foundPath.empty() ) return {};
            foundPath = iterator->path();
            iterator.disable_recursion_pending();
        }
        iterator.increment(iteratorError);
    }
    return iteratorError ? std::filesystem::path{} : foundPath;
}

/// @brief 验证解压后的 App 结构与代码签名。
/// @param applicationPath 待验证的 App bundle。
/// @return 结构完整且 codesign 严格验证通过时返回 true。
bool validateApplication(const std::filesystem::path& applicationPath)
{
    const std::filesystem::path infoPlist =
        applicationPath / "Contents" / "Info.plist";
    const std::filesystem::path executable =
        applicationPath / "Contents" / "MacOS" / "MusicMapMaker-Next";

    std::error_code pathError;
    if ( !std::filesystem::is_regular_file(infoPlist, pathError) ||
         pathError ) {
        writeStderr("Extracted app is missing Contents/Info.plist\n");
        return false;
    }
    pathError.clear();
    if ( !std::filesystem::is_regular_file(executable, pathError) ||
         pathError ) {
        writeStderr("Extracted app is missing its main executable\n");
        return false;
    }

    if ( !runSystemTool(
             "/usr/bin/codesign",
             { "--verify", "--deep", "--strict", applicationPath.string() }) ) {
        writeStderr("Extracted app failed code signature verification\n");
        return false;
    }
    return true;
}

/// @brief 解压并以可回滚方式替换完整 macOS App bundle。
/// @param archivePath 已完成 SHA256 校验的 App ZIP。
/// @param targetApplication 当前 App bundle 根路径。
/// @return 完整替换成功时返回 true。
bool installMacApplication(const std::filesystem::path& archivePath,
                           const std::filesystem::path& targetApplication)
{
    if ( targetApplication.extension() != ".app" ) {
        writeStderr("macOS update target must be an .app bundle\n");
        return false;
    }

    std::error_code targetError;
    if ( !std::filesystem::is_directory(targetApplication, targetError) ||
         targetError ) {
        writeStderr("Current app bundle does not exist\n");
        return false;
    }

    const std::filesystem::path targetParent = targetApplication.parent_path();
    const std::string           uniqueSuffix =
        std::to_string(static_cast<long>(getpid()));
    const std::filesystem::path stagingRoot =
        targetParent / ("." + targetApplication.filename().string() +
                        ".update-" + uniqueSuffix);
    const std::filesystem::path backupPath =
        targetParent / ("." + targetApplication.filename().string() +
                        ".backup-" + uniqueSuffix);

    removeTreeBestEffort(stagingRoot);
    removeTreeBestEffort(backupPath);

    std::error_code directoryError;
    if ( !std::filesystem::create_directory(stagingRoot, directoryError) ||
         directoryError ) {
        writeStderr("Failed to create app update staging directory: " +
                    directoryError.message() + "\n");
        return false;
    }

    if ( !runSystemTool("/usr/bin/ditto",
                        { "-x",
                          "-k",
                          "--noqtn",
                          archivePath.string(),
                          stagingRoot.string() }) ) {
        writeStderr("Failed to extract app update archive\n");
        removeTreeBestEffort(stagingRoot);
        return false;
    }

    const std::filesystem::path extractedApplication =
        findExtractedApplication(stagingRoot, targetApplication.filename());
    if ( extractedApplication.empty() ) {
        writeStderr("Update archive must contain exactly one matching .app\n");
        removeTreeBestEffort(stagingRoot);
        return false;
    }
    if ( !validateApplication(extractedApplication) ) {
        removeTreeBestEffort(stagingRoot);
        return false;
    }

    // APFS/HFS+ 支持原子交换目录；交换后旧 App
    // 位于解压目录中，可随临时目录清理。
    if ( renameatx_np(AT_FDCWD,
                      extractedApplication.c_str(),
                      AT_FDCWD,
                      targetApplication.c_str(),
                      RENAME_SWAP) == 0 ) {
        removeTreeBestEffort(stagingRoot);
        std::error_code removeArchiveError;
        std::filesystem::remove(archivePath, removeArchiveError);
        return true;
    }

    // 不支持 RENAME_SWAP 的卷使用带回滚的双重 rename。
    std::error_code replaceError;
    std::filesystem::rename(targetApplication, backupPath, replaceError);
    if ( replaceError ) {
        writeStderr("Failed to prepare app bundle backup: " +
                    replaceError.message() + "\n");
        removeTreeBestEffort(stagingRoot);
        return false;
    }

    replaceError.clear();
    std::filesystem::rename(
        extractedApplication, targetApplication, replaceError);
    if ( replaceError ) {
        writeStderr("Failed to install new app bundle: " +
                    replaceError.message() + "\n");
        std::error_code restoreError;
        std::filesystem::rename(backupPath, targetApplication, restoreError);
        if ( restoreError ) {
            writeStderr("Failed to restore previous app bundle: " +
                        restoreError.message() + "\n");
        }
        removeTreeBestEffort(stagingRoot);
        return false;
    }

    removeTreeBestEffort(backupPath);
    removeTreeBestEffort(stagingRoot);
    std::error_code removeArchiveError;
    std::filesystem::remove(archivePath, removeArchiveError);
    return true;
}

/// @brief 兼容旧版调用参数并定位 macOS App bundle 根路径。
/// @param targetPath 新版传入的 .app 路径或旧版传入的包内 Mach-O 路径。
/// @return 成功定位时返回 .app 根路径，否则返回空路径。
std::filesystem::path normalizeMacApplicationTarget(
    const std::filesystem::path& targetPath)
{
    std::filesystem::path current = targetPath;
    while ( !current.empty() ) {
        if ( current.extension() == ".app" ) return current;
        const std::filesystem::path parent = current.parent_path();
        if ( parent == current ) break;
        current = parent;
    }
    return {};
}

#    else

/// @brief 启动更新后的 Linux 可执行文件。
/// @param targetPath 可执行文件路径。
/// @return 成功创建启动进程时返回 true。
bool launchTarget(const std::string& targetPath)
{
    const pid_t child = fork();
    if ( child == 0 ) {
        setsid();
        execl(targetPath.c_str(), targetPath.c_str(), nullptr);
        _exit(1);
    }
    return child > 0;
}

#    endif
#endif

/// @brief 计算本次更新的成功标记路径。
/// @param targetPath Windows/Linux 可执行文件或 macOS App 路径。
/// @return 成功标记文件路径；无法取得临时目录时返回空路径。
std::filesystem::path updateSuccessMarkerPath(
    const std::filesystem::path& targetPath)
{
#if defined(__APPLE__)
    std::error_code tempError;
    const auto      tempPath = std::filesystem::temp_directory_path(tempError);
    if ( tempError ) return {};
    return tempPath / "MusicMapMaker-Next.mm_update_success";
#else
    return targetPath.parent_path() / ".mm_update_success";
#endif
}

/// @brief 写入更新成功标记。
/// @param targetPath Windows/Linux 可执行文件或 macOS App 路径。
void writeUpdateSuccessMarker(const std::filesystem::path& targetPath)
{
    const std::filesystem::path markerPath =
        updateSuccessMarkerPath(targetPath);
    if ( markerPath.empty() ) return;

    std::ofstream marker(markerPath);
    if ( marker.is_open() ) {
        marker << targetPath.string();
    }
}

}  // namespace

/// @brief 解析父进程 PID 参数。
/// @param text 命令行中的 PID 文本。
/// @return 解析成功时返回正数 PID，否则返回 0。
long parseParentPid(std::string_view text)
{
    long       value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
         value <= 0 ) {
        return 0;
    }
    return value;
}

/// @brief 执行独立更新器流程。
/// @param argc 命令行参数数量。
/// @param argv 下载文件、更新目标和父进程 PID 参数。
/// @return 更新并启动成功时返回 0，否则返回非零值。
int main(int argc, char* argv[])
{
    if ( argc != 4 ) {
        writeStderr(
            "Usage: MusicMapMaker-Updater <downloaded_file> <target_path> "
            "<parent_pid>\n");
        return 1;
    }

    const std::filesystem::path downloadedFile = argv[1];
    std::filesystem::path       targetPath     = argv[2];
    const long                  parentPid      = parseParentPid(argv[3]);
    if ( parentPid == 0 ) {
        writeStderr("Invalid parent_pid\n");
        return 1;
    }

#if defined(__APPLE__)
    targetPath = normalizeMacApplicationTarget(targetPath);
    if ( targetPath.empty() ) {
        writeStderr("Cannot locate the macOS app bundle update target\n");
        return 1;
    }
#endif

    // 必须先等待主程序退出，确保待替换目标不再使用旧文件。
#if defined(_WIN32)
    waitForParent(static_cast<DWORD>(parentPid));
#else
    waitForParent(static_cast<pid_t>(parentPid));
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::error_code replaceError;
#if defined(_WIN32)
    const std::filesystem::path backupPath = targetPath.string() + ".old";
    std::error_code             cleanupError;
    std::filesystem::remove(backupPath, cleanupError);

    std::error_code existsError;
    const bool targetExists = std::filesystem::exists(targetPath, existsError);
    if ( existsError ) {
        writeStderr("Failed to check target executable: " +
                    existsError.message() + "\n");
        return 1;
    }
    if ( targetExists ) {
        std::filesystem::rename(targetPath, backupPath, replaceError);
        if ( replaceError ) {
            writeStderr("Failed to prepare update backup: " +
                        replaceError.message() + "\n");
            return 1;
        }
    }
    std::filesystem::copy_file(
        downloadedFile,
        targetPath,
        std::filesystem::copy_options::overwrite_existing,
        replaceError);
    if ( replaceError ) {
        writeStderr("Failed to copy update: " + replaceError.message() + "\n");
        std::error_code backupExistsError;
        if ( std::filesystem::exists(backupPath, backupExistsError) &&
             !backupExistsError ) {
            std::error_code restoreError;
            std::filesystem::rename(backupPath, targetPath, restoreError);
        }
        return 1;
    }
    std::error_code backupExistsError;
    if ( std::filesystem::exists(backupPath, backupExistsError) &&
         !backupExistsError ) {
        std::error_code removeBackupError;
        std::filesystem::remove(backupPath, removeBackupError);
    }
    std::error_code removeDownloadError;
    std::filesystem::remove(downloadedFile, removeDownloadError);
#elif defined(__APPLE__)
    if ( !installMacApplication(downloadedFile, targetPath) ) {
        launchTarget(targetPath.string());
        return 1;
    }
#else
    std::filesystem::rename(downloadedFile, targetPath, replaceError);
    if ( replaceError ) {
        replaceError.clear();
        std::filesystem::copy_file(
            downloadedFile,
            targetPath,
            std::filesystem::copy_options::overwrite_existing,
            replaceError);
        if ( !replaceError ) {
            std::error_code removeDownloadError;
            std::filesystem::remove(downloadedFile, removeDownloadError);
        }
    }
    if ( replaceError ) {
        writeStderr("Failed to replace executable: " + replaceError.message() +
                    "\n");
        return 1;
    }
    std::filesystem::permissions(targetPath,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add,
                                 replaceError);
#endif

    writeUpdateSuccessMarker(targetPath);

    if ( !launchTarget(targetPath.string()) ) {
        writeStderr("Failed to launch updated application\n");
        return 1;
    }
    return 0;
}
