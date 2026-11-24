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
 *   4. 启动新版本，启动失败时撤下本次标记
 *
 * 完全独立，不依赖任何项目库，仅使用标准库和平台 API。
 */

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
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
#    include <spawn.h>
#    include <sys/types.h>
#    include <unistd.h>
#    if defined(__APPLE__)
#        include <fcntl.h>
#        include <sys/stdio.h>
#        include <sys/wait.h>
#    endif

extern char** environ;
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

/// @brief 等待 Windows 父进程明确退出，超时或无法验证时返回 false。
/// @param pid 调用者传入的正数 Windows 进程 ID。
/// @return 进程已退出或 PID 已不存在时返回 true。
/// @note 同一个句柄固定进程身份，等待期间不再按 PID 重新打开。
/// @details 只请求等待所需权限，访问拒绝无法用于推断进程状态。
/// @warning 最多阻塞三十秒，只能在独立更新器进程调用。
bool waitForParent(DWORD pid)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    // 正数 PID 不存在时系统返回
    // ERROR_INVALID_PARAMETER。拒绝权限不足等其他错误。
    if ( !process ) return GetLastError() == ERROR_INVALID_PARAMETER;
    // 句柄变为有信号状态才代表退出。WAIT_FAILED 和 WAIT_TIMEOUT 均阻止写入。
    // 关闭句柄前保存等待结果，后续清理不会覆盖用于决策的状态。
    const DWORD result = WaitForSingleObject(process, 30000);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
}

/// @brief 启动更新后的 Windows 可执行文件。
/// @param targetPath 可执行文件路径。
/// @return 成功请求系统启动时返回 true。
///
/// 工作目录设置为目标父目录，使更新后的程序继续按安装目录解析相对资源。
bool launchTarget(const std::wstring& targetPath)
{
    // 路径及工作目录保留 Windows 原生 UTF-16，系统 ANSI 代码页不参与重启。
    const auto directory = std::filesystem::path(targetPath).parent_path();
    // ShellExecute 大于 32 表示系统已接受启动请求，不代表子进程完成初始化。
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,
                                                   L"open",
                                                   targetPath.c_str(),
                                                   nullptr,
                                                   directory.c_str(),
                                                   SW_SHOWNORMAL)) > 32;
}

#else

/// @brief 有限等待 POSIX 父进程退出，只有 ESRCH 证明该 PID 已不存在。
/// @param pid 已检查范围的正数进程标识，避开进程组语义。
/// @return 仅进程探测报告 ESRCH 时返回 true。
/// @note 探测不会发信号。权限不足仍可能有旧进程，安全起见不得继续安装。
/// @details POSIX 不依赖父进程作为更新器的子进程，因此这里不调用 waitpid。
/// @warning 低频更新阶段有三十秒等待上限，主应用线程不参与这段等待。
bool waitForParent(pid_t pid)
{
    // 每半秒检查一次，最后一次检查包含完整三十秒等待窗口。
    // 探测返回成功或 EPERM 表示 PID 仍存在。其他错误直接结束探测，
    // 其中只有 ESRCH 可作为继续替换安装文件的依据。
    for ( int i = 0; i <= 60; ++i ) {
        if ( kill(pid, 0) == -1 && errno != EPERM ) return errno == ESRCH;
        if ( i < 60 )
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

#    if defined(__APPLE__)

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

/// @brief 请求 LaunchServices 启动更新后的 macOS 应用。
/// @param targetPath App bundle 根路径。
/// @return 系统 open 工具确认启动请求成功时返回 true。
/// @note 等待的是短时 open 工具，没有传入 -W，新应用无需先退出。
bool launchTarget(const std::string& targetPath)
{
    // open -n 要求启动新实例。检查工具退出码能将无效应用包或启动错误
    // 与可执行工具本身已启动的情况区分开。
    return runSystemTool("/usr/bin/open", { "-n", targetPath });
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

/// @brief 在 Linux 安装已验证的更新文件，仅用 rename 发布正式目标。
/// @param downloadedFile 已完成校验的普通文件。
/// @param targetPath 下一次启动使用的正式可执行路径。
/// @return 失败时保留旧目标和下载文件；成功后才清理下载源。
std::error_code installLinuxExecutable(
    const std::filesystem::path& downloadedFile,
    const std::filesystem::path& targetPath)
{
    namespace fs = std::filesystem;
    constexpr auto executable =
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    std::error_code error;
    const auto      source = fs::symlink_status(downloadedFile, error);
    if ( error ) return error;
    if ( !fs::is_regular_file(source) )
        return std::make_error_code(std::errc::invalid_argument);

    // 已备齐执行位的同卷文件可以直接提交。仅跨文件系统才需要复制退路；
    // 权限不足或目标类型冲突等错误不能降级为覆盖写入。
    if ( (source.permissions() & executable) == executable ) {
        fs::rename(downloadedFile, targetPath, error);
        if ( error != std::errc::cross_device_link ) return error;
    }

    // 临时目录与正式目标位于同一文件系统。目录独占给本次更新使用，
    // 避免并发更新覆盖对方的暂存文件，也不需要预先删除既有路径。
    auto stagingName =
        (targetPath.parent_path() / ".mmm-update-XXXXXX").native();
    if ( !mkdtemp(stagingName.data()) )
        return { errno, std::generic_category() };
    const fs::path stagingDirectory(stagingName);
    const auto     staged = stagingDirectory / "executable";

    // 复制和权限设置全部完成后才提交；即使磁盘写满或进程在复制中退出，
    // 写入过的字节也只留在暂存目录。下载源仍可重试，并保留原权限。
    fs::copy_file(downloadedFile, staged, error);
    if ( !error )
        fs::permissions(staged, executable, fs::perm_options::add, error);
    if ( !error ) fs::rename(staged, targetPath, error);

    // 清理限定在本次创建的文件和空目录内，保留导致失败的原始错误码。
    std::error_code cleanupError;
    fs::remove(staged, cleanupError);
    fs::remove(stagingDirectory, cleanupError);
    // 手工调用可能为两个参数传入同一路径。提交后的身份相同则不删除，
    // 否则源路径会连同刚安装的目标一起被移除。
    if ( !error && !fs::equivalent(downloadedFile, targetPath, cleanupError) &&
         !cleanupError )
        fs::remove(downloadedFile, cleanupError);
    return error;
}

/// @brief 启动更新后的 Linux 可执行文件并检查装载失败。
/// @param targetPath 已替换的目标可执行文件路径。
/// @return 系统成功装载程序映像时返回 true。
/// @details 进程映像一旦被内核装载，应用由其自己的生命周期管理。
/// 更新器不等待初始化完成，避免安装和应用关闭之间形成相互等待。
/// @note 支持会话属性的 libc 保留原先的脱离会话行为。
bool launchTarget(const std::string& targetPath)
{
    // posix_spawn 的 argv 形式需要可写字符指针，字符串由调用端借给系统。
    std::string path = targetPath;
    char*       arguments[]{ path.data(), nullptr };
    pid_t       child{};
#        if defined(POSIX_SPAWN_SETSID)
    // 明确要求在新会话中启动，以免终端关闭时顺带终止已更新的程序。
    posix_spawnattr_t attributes;
    int               launchError = posix_spawnattr_init(&attributes);
    if ( launchError == 0 ) {
        launchError = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
        if ( launchError == 0 )
            launchError = posix_spawn(
                &child, path.c_str(), nullptr, &attributes, arguments, environ);
        posix_spawnattr_destroy(&attributes);
    }
#        else
    // libc 未提供脱离会话属性时仍检查实际装载结果。
    const int launchError =
        posix_spawn(&child, path.c_str(), nullptr, nullptr, arguments, environ);
#        endif
    if ( launchError == 0 ) return true;

    // posix_spawn 的返回值直接携带 ENOEXEC、EACCES 等实际错误。
    writeStderr(
        "Cannot execute installed program: " +
        std::error_code(launchError, std::generic_category()).message() + "\n");
    return false;
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
#if defined(_WIN32)
        // Windows 路径输出为 UTF-8 文本，避免窄代码页转换失败中止更新。
        const auto pathText = targetPath.u8string();
        marker.write(reinterpret_cast<const char*>(pathText.data()),
                     static_cast<std::streamsize>(pathText.size()));
#else
        // POSIX 保留原生路径字节，与现有标记的读取语义一致。
        marker << targetPath.native();
#endif
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

#if defined(_WIN32)
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
#endif

/// @brief 执行独立更新器流程。
/// @param argc 命令行参数数量。
/// @param argv 下载文件、更新目标和父进程 PID 参数。
/// @return 更新并启动成功时返回 0，否则返回非零值。
///
/// 主流程假设下载文件已经由主程序完成完整性校验。Windows 通过备份后复制
/// 支持回滚，Linux 仅通过原子 rename 发布已备齐权限的文件，macOS 交给完整
/// bundle 安装事务。替换完成后准备成功标记并启动新版本；启动失败撤下标记。
///
/// 退出码约定：
/// - 参数数量或 PID 非法返回 1，且不触碰下载文件与目标；
/// - 平台目标规范化失败或无法确认父进程退出时返回 1；
/// - 备份、复制、重命名或 bundle 验证失败返回 1；
/// - 新版本启动请求失败返回 1，但保留已安装文件；
/// - 全部步骤成功返回 0。
///
/// 更新器不删除失败时仍可能用于恢复或重试的下载文件。各平台只在新目标
/// 已安全就位后清理下载源；临时和备份路径均从明确目标及当前 PID 推导，
/// 禁止把未验证的命令行目录作为递归删除范围。
#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[])
#else
int main(int argc, char* argv[])
#endif
{
    // 参数数量必须精确，避免把缺失 PID 或多余字符串误当作路径片段。
    if ( argc != 4 ) {
        writeStderr(
            "Usage: MusicMapMaker-Updater <downloaded_file> <target_path> "
            "<parent_pid>\n");
        return 1;
    }

    // Windows 从宽入口直接构造原生路径；POSIX 保留原始字节。
    // 存在性和可操作性在平台替换步骤验证。
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
    const bool parentExited = waitForParent(static_cast<DWORD>(parentPid));
#else
    // long 可能比 pid_t 宽，拒绝会被截断为其他进程或进程组的值。
    const bool parentExited = parentPid <= std::numeric_limits<pid_t>::max() &&
                              waitForParent(static_cast<pid_t>(parentPid));
#endif
    if ( !parentExited ) {
        // 正式目标、下载源和成功标记都必须留待下一次安全重试。
        writeStderr("Cannot confirm that the parent process exited\n");
        return 1;
    }
    // 父进程退出后留出短暂时间给系统释放映像和防病毒扫描句柄。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::error_code replaceError;
#if defined(_WIN32)
    // Windows 不能覆盖仍被占用的映像，先把旧文件移动到同目录备份。
    auto backupPath = targetPath;
    // 追加原生路径后缀，避免给已有的 UTF-16 字符做代码页往返。
    backupPath += L".old";
    std::error_code cleanupError;
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
    replaceError = installLinuxExecutable(downloadedFile, targetPath);
    if ( replaceError ) {
        writeStderr("Failed to replace executable: " + replaceError.message() +
                    "\n");
        return 1;
    }
#endif

    // 启动中的新应用可以立即消费标记，不能等启动返回后补写。
    // 若装载或系统请求失败，新应用尚未启动，撤下本次预先写入的标记。
    writeUpdateSuccessMarker(targetPath);

    // 启动失败不回滚新版本，保留更新结果供用户手工启动和诊断。
    if ( !launchTarget(targetPath.native()) ) {
        std::error_code markerError;
        const auto      markerPath = updateSuccessMarkerPath(targetPath);
        if ( !markerPath.empty() )
            std::filesystem::remove(markerPath, markerError);
        if ( markerError )
            writeStderr("Failed to clear update marker: " +
                        markerError.message() + "\n");
        writeStderr("Failed to launch updated application\n");
        return 1;
    }
    return 0;
}
