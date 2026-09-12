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
///
/// 更新器不链接主项目日志库，stderr 是替换失败时唯一可靠的诊断通道。
/// 使用 string_view 长度写入，消息不必以空字符结束。
void writeStderr(std::string_view message)
{
    // 空消息不调用 C I/O，避免无意义的零长度写入。
    if ( message.empty() ) return;
    // fwrite 不抛出异常；异常路径只做尽力诊断，不递归报告写入失败。
    std::fwrite(message.data(), 1, message.size(), stderr);
}

namespace
{

#if defined(_WIN32)

/// @brief 检查指定 PID 进程是否仍在运行。
/// @param pid 需要检查的 Windows 进程 ID。
/// @return 进程仍在运行时返回 true。
///
/// 只申请 SYNCHRONIZE 权限，避免普通用户因过高访问权限导致无谓失败。
bool isProcessAlive(DWORD pid)
{
    // 无法打开句柄按“不再可等待”处理，由后续替换操作给出真实错误。
    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if ( !hProcess ) return false;
    // 零超时只探测当前状态，不阻塞轮询路径。
    const DWORD waitResult = WaitForSingleObject(hProcess, 0);
    CloseHandle(hProcess);
    return waitResult == WAIT_TIMEOUT;
}

/// @brief 等待父进程退出，最多等待 30 秒。
/// @param pid 父进程 ID。
///
/// 正常路径直接等待进程句柄；句柄不可用时退回有限次数存活探测。超时后仍
/// 继续替换，让文件系统操作返回可诊断错误而不是永久挂起更新器。
void waitForParent(DWORD pid)
{
    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if ( hProcess ) {
        // 最长三十秒覆盖主程序保存配置和释放图形资源的正常收尾时间。
        WaitForSingleObject(hProcess, 30000);
        CloseHandle(hProcess);
    } else {
        // 每半秒探测一次，总等待上限与句柄路径一致。
        for ( int i = 0; i < 60; ++i ) {
            if ( !isProcessAlive(pid) ) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

/// @brief 启动更新后的 Windows 可执行文件。
/// @param targetPath 可执行文件路径。
/// @return 成功请求系统启动时返回 true。
///
/// 工作目录设置为目标父目录，使更新后的程序继续按安装目录解析相对资源。
bool launchTarget(const std::string& targetPath)
{
    std::string dir =
        std::filesystem::path(targetPath).parent_path().generic_string();
    // ShellExecute 大于 32 表示系统已接受启动请求，不代表子进程完成初始化。
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
///
/// kill(pid, 0) 不发送信号；EPERM 表示进程存在但当前用户无权操作。
bool isProcessAlive(pid_t pid)
{
    return kill(pid, 0) == 0 || errno == EPERM;
}

/// @brief 等待父进程退出，最多等待 30 秒。
/// @param pid 父进程 ID。
///
/// 有限轮询避免父进程卡死时更新器永久驻留；超时后由 rename/copy 判断目标
/// 是否已经可替换。固定等待只存在于独立更新进程，不阻塞应用 UI 或渲染。
void waitForParent(pid_t pid)
{
    // 六十次半秒间隔构成三十秒上限。
    for ( int i = 0; i < 60; ++i ) {
        if ( !isProcessAlive(pid) ) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

#    if defined(__APPLE__)

/// @brief 启动 macOS App bundle。
/// @param targetPath App bundle 根路径。
/// @return 成功创建启动进程时返回 true。
///
/// 子进程脱离当前会话后执行系统 open -n，由 LaunchServices 建立新应用实例。
bool launchTarget(const std::string& targetPath)
{
    const pid_t child = fork();
    if ( child == 0 ) {
        // 子进程只执行异步启动，不返回更新器主流程。
        setsid();
        execl("/usr/bin/open",
              "open",
              "-n",
              targetPath.c_str(),
              static_cast<char*>(nullptr));
        _exit(1);
    }
    // 父进程只确认 fork 成功，不等待新应用退出。
    return child > 0;
}

/// @brief 运行系统工具并等待其结束。
/// @param executable 系统工具绝对路径。
/// @param arguments 不含 argv[0] 的参数列表。
/// @return 工具以 0 状态正常退出时返回 true。
///
/// 参数字符串先复制到可写存储，再建立 posix_spawn 所需的 char* 数组。函数
/// 同步等待系统工具完成，并在 EINTR 时继续 waitpid，其他等待错误立即失败。
bool runSystemTool(const char*                     executable,
                   const std::vector<std::string>& arguments)
{
    // vector<string> 在 spawn 返回前保持所有参数字符缓冲地址有效。
    std::vector<std::string> mutableArguments = arguments;
    std::vector<char*>       argv;
    // 额外两个槽位分别用于 argv[0] 和末尾 nullptr。
    argv.reserve(mutableArguments.size() + 2);
    argv.push_back(const_cast<char*>(executable));
    for ( std::string& argument : mutableArguments ) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    pid_t child = 0;
    // 使用当前环境，不通过 shell 解析参数，避免路径中的空格或元字符变义。
    if ( posix_spawn(
             &child, executable, nullptr, nullptr, argv.data(), environ) !=
         0 ) {
        return false;
    }

    int waitStatus = 0;
    // 信号中断仅重试等待，不重复启动系统工具。
    while ( waitpid(child, &waitStatus, 0) < 0 ) {
        if ( errno != EINTR ) return false;
    }
    return WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0;
}

/// @brief 尽力删除更新过程使用的专属临时目录。
/// @param path 仅由本次更新创建的临时路径。
///
/// error_code 故意忽略：清理失败不能覆盖更早、更具体的更新诊断。
void removeTreeBestEffort(const std::filesystem::path& path)
{
    std::error_code removeError;
    std::filesystem::remove_all(path, removeError);
}

/// @brief 在解压目录中定位唯一的目标 App bundle。
/// @param stagingRoot 解压根目录。
/// @param bundleName 目标 App bundle 文件名。
/// @return 找到唯一匹配目录时返回其路径，否则返回空路径。
///
/// 先检查归档根目录的常见直接布局，再递归兼容额外顶层目录。发现第二个同名
/// bundle 时拒绝选择，避免安装来源不明确的归档内容。
std::filesystem::path findExtractedApplication(
    const std::filesystem::path& stagingRoot,
    const std::filesystem::path& bundleName)
{
    const std::filesystem::path directPath = stagingRoot / bundleName;
    std::error_code             directError;
    // 直接匹配优先且无需递归遍历大型归档树。
    if ( std::filesystem::is_directory(directPath, directError) &&
         !directError ) {
        return directPath;
    }

    std::filesystem::path foundPath;
    std::error_code       iteratorError;
    // 跳过无权限子目录，但保留 iteratorError 作为整体失败信号。
    std::filesystem::recursive_directory_iterator iterator(
        stagingRoot,
        std::filesystem::directory_options::skip_permission_denied,
        iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    while ( !iteratorError && iterator != end ) {
        std::error_code entryError;
        if ( iterator->path().filename() == bundleName &&
             iterator->is_directory(entryError) && !entryError ) {
            // 多个匹配 bundle 无法确定目标，按无效归档处理。
            if ( !foundPath.empty() ) return {};
            foundPath = iterator->path();
            // 找到 bundle 后不进入其 Contents，避免内部目录产生无关遍历成本。
            iterator.disable_recursion_pending();
        }
        iterator.increment(iteratorError);
    }
    return iteratorError ? std::filesystem::path{} : foundPath;
}

/// @brief 验证解压后的 App 结构与代码签名。
/// @param applicationPath 待验证的 App bundle。
/// @return 结构完整且 codesign 严格验证通过时返回 true。
///
/// 结构检查先确保 Info.plist 和主 Mach-O 均为普通文件，再调用系统 codesign
/// 执行 deep、strict 验证。任何失败都在替换现有应用之前终止。
bool validateApplication(const std::filesystem::path& applicationPath)
{
    const std::filesystem::path infoPlist =
        applicationPath / "Contents" / "Info.plist";
    const std::filesystem::path executable =
        applicationPath / "Contents" / "MacOS" / "MusicMapMaker-Next";

    // 所有 filesystem 查询使用 error_code，独立更新器禁止异常中断回滚流程。
    std::error_code pathError;
    if ( !std::filesystem::is_regular_file(infoPlist, pathError) ||
         pathError ) {
        writeStderr("Extracted app is missing Contents/Info.plist\n");
        return false;
    }
    // 清除上一项状态，第二次查询独立报告主可执行文件问题。
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
///
/// 安装优先使用 APFS/HFS+ 的 RENAME_SWAP 原子交换；不支持时先把旧 App 改名
/// 为专属备份，再移动新 App，第二步失败则尝试恢复。暂存和备份路径包含更新器
/// PID，且只清理由本次流程推导出的同名目录。
///
/// 归档在解压前已经由主程序完成校验，但解压结果仍需验证结构和代码签名。
/// 只有替换成功后才删除下载归档，失败路径尽量保留可恢复的旧应用。
bool installMacApplication(const std::filesystem::path& archivePath,
                           const std::filesystem::path& targetApplication)
{
    // 目标必须是 bundle 根目录，禁止把包内 Mach-O 当成目录替换。
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

    // 所有临时目录与目标同卷创建，保证 rename 和 RENAME_SWAP 不跨文件系统。
    const std::filesystem::path targetParent = targetApplication.parent_path();
    const std::string           uniqueSuffix =
        std::to_string(static_cast<long>(getpid()));
    const std::filesystem::path stagingRoot =
        targetParent / ("." + targetApplication.filename().string() +
                        ".update-" + uniqueSuffix);
    const std::filesystem::path backupPath =
        targetParent / ("." + targetApplication.filename().string() +
                        ".backup-" + uniqueSuffix);

    // 清除同 PID 遗留目录，避免上次异常退出内容混入本次验证。
    removeTreeBestEffort(stagingRoot);
    removeTreeBestEffort(backupPath);

    std::error_code directoryError;
    // create_directory 必须确实创建新目录；已存在也视为不安全失败。
    if ( !std::filesystem::create_directory(stagingRoot, directoryError) ||
         directoryError ) {
        writeStderr("Failed to create app update staging directory: " +
                    directoryError.message() + "\n");
        return false;
    }

    // ditto 保留 macOS bundle 元数据，--noqtn 避免更新归档隔离属性传播。
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
    // 归档必须恰好包含一个与当前应用同名的 bundle。
    if ( extractedApplication.empty() ) {
        writeStderr("Update archive must contain exactly one matching .app\n");
        removeTreeBestEffort(stagingRoot);
        return false;
    }
    // 在触碰当前应用前完成所有新 bundle 验证。
    if ( !validateApplication(extractedApplication) ) {
        removeTreeBestEffort(stagingRoot);
        return false;
    }

    // APFS/HFS+ 支持原子交换目录；交换后旧 App 位于解压目录中，可随暂存清理。
    if ( renameatx_np(AT_FDCWD,
                      extractedApplication.c_str(),
                      AT_FDCWD,
                      targetApplication.c_str(),
                      RENAME_SWAP) == 0 ) {
        removeTreeBestEffort(stagingRoot);
        std::error_code removeArchiveError;
        // 交换成功后归档不再承担回滚职责，可尽力删除。
        std::filesystem::remove(archivePath, removeArchiveError);
        return true;
    }

    // 不支持 RENAME_SWAP 的卷使用带回滚的双重 rename。
    // 第一步只移动旧应用，第二步才把已验证新 bundle 放入正式路径。
    std::error_code replaceError;
    std::filesystem::rename(targetApplication, backupPath, replaceError);
    if ( replaceError ) {
        writeStderr("Failed to prepare app bundle backup: " +
                    replaceError.message() + "\n");
        removeTreeBestEffort(stagingRoot);
        return false;
    }

    // 复用 error_code 前先清除旧状态，确保只判断安装 rename 结果。
    replaceError.clear();
    std::filesystem::rename(
        extractedApplication, targetApplication, replaceError);
    if ( replaceError ) {
        writeStderr("Failed to install new app bundle: " +
                    replaceError.message() + "\n");
        // 新 bundle 安装失败后尽力把旧应用恢复到原路径。
        std::error_code restoreError;
        std::filesystem::rename(backupPath, targetApplication, restoreError);
        if ( restoreError ) {
            writeStderr("Failed to restore previous app bundle: " +
                        restoreError.message() + "\n");
        }
        removeTreeBestEffort(stagingRoot);
        return false;
    }

    // 新应用就位后清理旧备份、暂存根和下载归档。
    removeTreeBestEffort(backupPath);
    removeTreeBestEffort(stagingRoot);
    std::error_code removeArchiveError;
    std::filesystem::remove(archivePath, removeArchiveError);
    return true;
}

/// @brief 兼容旧版调用参数并定位 macOS App bundle 根路径。
/// @param targetPath 新版传入的 .app 路径或旧版传入的包内 Mach-O 路径。
/// @return 成功定位时返回 .app 根路径，否则返回空路径。
///
/// 只做词法父路径上溯，不访问文件系统；实际存在性随后由安装函数验证。
std::filesystem::path normalizeMacApplicationTarget(
    const std::filesystem::path& targetPath)
{
    std::filesystem::path current = targetPath;
    // 从传入路径自身开始，兼容参数已经指向 bundle 根目录。
    while ( !current.empty() ) {
        if ( current.extension() == ".app" ) return current;
        const std::filesystem::path parent = current.parent_path();
        // 根目录 parent 等于自身时终止，避免无限循环。
        if ( parent == current ) break;
        current = parent;
    }
    return {};
}

#    else

/// @brief 启动更新后的 Linux 可执行文件。
/// @param targetPath 可执行文件路径。
/// @return 成功创建启动进程时返回 true。
///
/// 子进程创建新会话并直接 exec 更新后的文件；父更新器不等待应用结束。
bool launchTarget(const std::string& targetPath)
{
    const pid_t child = fork();
    if ( child == 0 ) {
        // exec 失败时使用 _exit，避免运行父进程的静态析构和缓冲刷新。
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
///
/// macOS 应用包可能位于只读或签名保护位置，标记写到系统临时目录；桌面平台
/// 单文件更新则把隐藏标记放在目标可执行文件旁，便于新进程发现。
std::filesystem::path updateSuccessMarkerPath(
    const std::filesystem::path& targetPath)
{
#if defined(__APPLE__)
    std::error_code tempError;
    const auto      tempPath = std::filesystem::temp_directory_path(tempError);
    // 临时目录查询失败时跳过标记，不影响已经完成的安装结果。
    if ( tempError ) return {};
    return tempPath / "MusicMapMaker-Next.mm_update_success";
#else
    return targetPath.parent_path() / ".mm_update_success";
#endif
}

/// @brief 写入更新成功标记。
/// @param targetPath Windows/Linux 可执行文件或 macOS App 路径。
///
/// 标记内容保存最终目标路径，供新版本确认此次启动来自更新。写入采用尽力
/// 语义，失败不回滚已经成功替换的程序。
void writeUpdateSuccessMarker(const std::filesystem::path& targetPath)
{
    const std::filesystem::path markerPath =
        updateSuccessMarkerPath(targetPath);
    if ( markerPath.empty() ) return;

    // ofstream 默认截断旧标记，确保内容对应最近一次更新。
    std::ofstream marker(markerPath);
    if ( marker.is_open() ) {
        marker << targetPath.string();
    }
}

}  // namespace

/// @brief 解析父进程 PID 参数。
/// @param text 命令行中的 PID 文本。
/// @return 解析成功时返回正数 PID，否则返回 0。
///
/// from_chars 不受区域设置影响且不分配；要求完整消费输入，拒绝后缀字符、
/// 溢出、零值和负值。零作为统一无效哨兵不会与合法进程 ID 冲突。
long parseParentPid(std::string_view text)
{
    long       value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    // 三项检查分别覆盖数值错误、未完整消费和非正进程 ID。
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
///
/// 主流程假设下载文件已经由主程序完成完整性校验。Windows 通过备份后复制
/// 支持回滚，Linux 优先原子 rename、跨文件系统时退回复制，macOS 交给完整
/// bundle 安装事务。替换完成后写成功标记并启动新版本。
///
/// 退出码约定：
/// - 参数数量或 PID 非法返回 1，且不触碰下载文件与目标；
/// - 平台目标规范化失败返回 1；
/// - 备份、复制、重命名或 bundle 验证失败返回 1；
/// - 新版本启动请求失败返回 1，但保留已安装文件；
/// - 全部步骤成功返回 0。
///
/// 更新器不删除失败时仍可能用于恢复或重试的下载文件。各平台只在新目标
/// 已安全就位后清理下载源；临时和备份路径均从明确目标及当前 PID 推导，
/// 禁止把未验证的命令行目录作为递归删除范围。
int main(int argc, char* argv[])
{
    // 参数数量必须精确，避免把缺失 PID 或多余字符串误当作路径片段。
    if ( argc != 4 ) {
        writeStderr(
            "Usage: MusicMapMaker-Updater <downloaded_file> <target_path> "
            "<parent_pid>\n");
        return 1;
    }

    // 路径对象只做词法保存，存在性和可操作性在平台替换步骤验证。
    const std::filesystem::path downloadedFile = argv[1];
    std::filesystem::path       targetPath     = argv[2];
    const long                  parentPid      = parseParentPid(argv[3]);
    if ( parentPid == 0 ) {
        // 无效 PID 时不能安全判断主程序是否已释放目标文件。
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
    // 父进程退出后留出短暂时间给系统释放映像和防病毒扫描句柄。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::error_code replaceError;
#if defined(_WIN32)
    // Windows 不能覆盖仍被占用的映像，先把旧文件移动到同目录备份。
    const std::filesystem::path backupPath = targetPath.string() + ".old";
    std::error_code             cleanupError;
    // 旧备份只做尽力清理，后续 rename 会给出实际冲突错误。
    std::filesystem::remove(backupPath, cleanupError);

    std::error_code existsError;
    const bool targetExists = std::filesystem::exists(targetPath, existsError);
    if ( existsError ) {
        writeStderr("Failed to check target executable: " +
                    existsError.message() + "\n");
        return 1;
    }
    if ( targetExists ) {
        // 只有原目标存在时才建立回滚备份，新安装允许目标缺失。
        std::filesystem::rename(targetPath, backupPath, replaceError);
        if ( replaceError ) {
            writeStderr("Failed to prepare update backup: " +
                        replaceError.message() + "\n");
            return 1;
        }
    }
    // 下载文件复制到正式路径，保留下载源直到复制成功。
    std::filesystem::copy_file(
        downloadedFile,
        targetPath,
        std::filesystem::copy_options::overwrite_existing,
        replaceError);
    if ( replaceError ) {
        writeStderr("Failed to copy update: " + replaceError.message() + "\n");
        // 复制失败时尽力恢复旧可执行文件，原始下载仍保留供重试。
        std::error_code backupExistsError;
        if ( std::filesystem::exists(backupPath, backupExistsError) &&
             !backupExistsError ) {
            std::error_code restoreError;
            std::filesystem::rename(backupPath, targetPath, restoreError);
        }
        return 1;
    }
    // 新文件就位后删除旧备份，再清理下载文件。
    std::error_code backupExistsError;
    if ( std::filesystem::exists(backupPath, backupExistsError) &&
         !backupExistsError ) {
        std::error_code removeBackupError;
        std::filesystem::remove(backupPath, removeBackupError);
    }
    std::error_code removeDownloadError;
    std::filesystem::remove(downloadedFile, removeDownloadError);
#elif defined(__APPLE__)
    // macOS 失败时尝试重新启动仍在目标路径的旧应用。
    if ( !installMacApplication(downloadedFile, targetPath) ) {
        launchTarget(targetPath.string());
        return 1;
    }
#else
    // 同一文件系统优先 rename，通常是原子且无需复制文件内容。
    std::filesystem::rename(downloadedFile, targetPath, replaceError);
    if ( replaceError ) {
        // rename 失败可能来自跨文件系统，清除错误后尝试覆盖复制。
        replaceError.clear();
        std::filesystem::copy_file(
            downloadedFile,
            targetPath,
            std::filesystem::copy_options::overwrite_existing,
            replaceError);
        if ( !replaceError ) {
            // 复制成功后下载源不再需要，删除失败不影响已安装文件。
            std::error_code removeDownloadError;
            std::filesystem::remove(downloadedFile, removeDownloadError);
        }
    }
    if ( replaceError ) {
        writeStderr("Failed to replace executable: " + replaceError.message() +
                    "\n");
        return 1;
    }
    // 确保复制路径也拥有各类用户执行位；权限错误沿用 replaceError 诊断通道。
    std::filesystem::permissions(targetPath,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add,
                                 replaceError);
#endif

    // 替换成功后先写标记，新进程启动即可读取完整更新结果。
    writeUpdateSuccessMarker(targetPath);

    // 启动失败不回滚新版本，保留更新结果供用户手工启动和诊断。
    if ( !launchTarget(targetPath.string()) ) {
        writeStderr("Failed to launch updated application\n");
        return 1;
    }
    return 0;
}
