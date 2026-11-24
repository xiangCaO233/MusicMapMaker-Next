#include "UpdaterPlatform.h"

#include "UpdateSuccessMarker.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <ostream>
#include <spawn.h>
#include <string>
#include <sys/stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace MMM::Update
{
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
    // 路径经原生 argv 直送子进程。文件名中的空格仍是同一个参数。
    // 构造 argv 前完成字符串复制，后续 push_back 只会扩充指针数组。
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

    // 这里只回收本次 spawn 得到的 PID。其他系统工具的返回不参与事务。
    // 正常退出和信号终止须分别判断，终止状态中的数字不是退出码。
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
bool launchTarget(const std::filesystem::path& targetPath)
{
    // open -n 要求启动新实例。检查工具退出码能将无效应用包或启动错误
    // 与可执行工具本身已启动的情况区分开。
    // 交给 LaunchServices 管理应用身份，不直接执行包内 Mach-O。
    // 使用刚安装的绝对包路径，避免按 bundle ID 打开另一份安装。
    return runSystemTool("/usr/bin/open", { "-n", targetPath.native() });
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
    // 即使先前遇到匹配目录，遍历中断也无法证明候选唯一。
    // 让调用方保留正式包，并只回收属于当前事务的解压目录。
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

    // 结构检查只确认启动入口存在。签名工具负责复核整个 bundle 的封装。
    // 检查在事务暂存路径上完成，失败时不会触碰原应用目录。
    if ( !runSystemTool(
             "/usr/bin/codesign",
             { "--verify", "--deep", "--strict", applicationPath.native() }) ) {
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
        targetParent / ("." + targetApplication.filename().native() +
                        ".update-" + uniqueSuffix);
    // 备份名和 stagingRoot 分开。交换不可用时，回滚目标不会落入解压树。
    // 两者都依赖已规范化的目标，以免相同包的 Mach-O 路径产生不同事务根。
    const std::filesystem::path backupPath =
        targetParent / ("." + targetApplication.filename().native() +
                        ".backup-" + uniqueSuffix);

    // 清除同 PID 遗留目录，避免上次异常退出内容混入本次验证。
    // 清理沿用原协议限定的保留名称。不会遍历相邻的其他安装包。
    // 删除结果不能吞掉后续新建目录的真实错误。
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
                          archivePath.native(),
                          stagingRoot.native() }) ) {
        writeStderr("Failed to extract app update archive\n");
        removeTreeBestEffort(stagingRoot);
        return false;
    }

    // ZIP 可能由打包工具加一层父目录。按匹配的包名定位并保留大小写。
    // 返回空路径既包括缺失也包括重复候选，此时不得随便选择第一个。
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
    // 交换前 extractedApplication 是新应用，交换后同一路径是旧应用。
    // 正式路径在一次操作里切换，冷启动不会观察到旧包已移开但新包未就位。
    // 在替换结果确认前保留源归档，进程退出后仍能按原下载路径重试。
    if ( renameatx_np(AT_FDCWD,
                      extractedApplication.c_str(),
                      AT_FDCWD,
                      targetApplication.c_str(),
                      RENAME_SWAP) == 0 ) {
        removeTreeBestEffort(stagingRoot);
        // 删除归档是提交后的垃圾回收。异步启动失败仍由共同流程处理标记。
        // 当前包已经就位，归档清理错误不会把事务重新变成可回滚状态。
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
    // 删除归档是提交后的垃圾回收。异步启动失败仍由共同流程处理标记。
    // 当前包已经就位，归档清理错误不会把事务重新变成可回滚状态。
    std::error_code removeArchiveError;
    std::filesystem::remove(archivePath, removeArchiveError);
    return true;
}
// 安装事务失败后尝试启动仍在原路径的应用。
/// @brief 提交完整应用包并保留失败时的旧版重启行为。
bool install(const std::filesystem::path& source,
             const std::filesystem::path& target)
{
    // 失败分支沿用旧应用重启约定。共同流程只给成功安装的应用生成标记。
    // 旧版重启即使被系统拒绝，返回结果仍代表实际安装失败。
    if ( installMacApplication(source, target) ) return true;
    launchTarget(target);
    return false;
}

/// @brief 允许将 .app 本身或 bundle 内的可执行路径交给更新器。
bool normalizeTarget(std::filesystem::path& path)
{
    path = macApplicationRoot(path);
    if ( !path.empty() ) return true;
    writeStderr("Cannot locate the macOS app bundle update target\n");
    return false;
}

/// @brief 以每份应用的规范化身份计算系统临时目录标记。
std::filesystem::path updateSuccessMarkerPath(
    const std::filesystem::path& target)
{
    std::error_code error;
    const auto      temp = std::filesystem::temp_directory_path(error);
    if ( error ) return {};
    return macUpdateMarkerPath(target, temp);
}

/// @brief 标记正文使用实际包身份，启动端还会独立复核。
void writeMarkerContents(std::ostream&                marker,
                         const std::filesystem::path& target)
{
    std::error_code error;
    const auto      identity = std::filesystem::canonical(target, error);
    // 标记路径用身份散列区分安装副本。正文仍保留完整身份供读取方确认。
    // 若规范化失败，读取模块不能把空标记解释为已成功升级。
    if ( !error ) marker << identity.native();
}

}  // namespace MMM::Update
