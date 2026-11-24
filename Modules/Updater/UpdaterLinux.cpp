#include "UpdaterPlatform.h"

#include <cerrno>
#include <cstdlib>
#include <ostream>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

extern char** environ;

namespace MMM::Update
{
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
    // 源必须是可消费的独立文件。沿符号链接替换会让正式程序也成为链接。
    // 文件状态和实际复制由错误码通知调用方，禁止异常跨出独立更新器。
    const auto source = fs::symlink_status(downloadedFile, error);
    if ( error ) return error;
    if ( !fs::is_regular_file(source) )
        return std::make_error_code(std::errc::invalid_argument);

    // 已备齐执行位的同卷文件可以直接提交。仅跨文件系统才需要复制退路；
    // 权限不足或目标类型冲突等错误不能降级为覆盖写入。
    if ( (source.permissions() & executable) == executable ) {
        // 已可执行时先尝试零拷贝提交。重命名成功后下载路径自然失效。
        // 目标现存文件不先移开，安装失败时启动端仍能找到旧程序。
        fs::rename(downloadedFile, targetPath, error);
        if ( error != std::errc::cross_device_link ) return error;
    }

    // 临时目录与正式目标位于同一文件系统。目录独占给本次更新使用，
    // 避免并发更新覆盖对方的暂存文件，也不需要预先删除既有路径。
    auto stagingName =
        (targetPath.parent_path() / ".mmm-update-XXXXXX").native();
    if ( !mkdtemp(stagingName.data()) )
        return { errno, std::generic_category() };
    // mkdtemp 回写本次事务的真实目录。旧的失败目录不会成为本次暂存。
    // 内容只落盘到这个私有目录，Linux 路径使用调用方的原生编码。
    const fs::path stagingDirectory(stagingName);
    const auto     staged = stagingDirectory / "executable";

    // 复制和权限设置全部完成后才提交；即使磁盘写满或进程在复制中退出，
    // 写入过的字节也只留在暂存目录。下载源仍可重试，并保留原权限。
    fs::copy_file(downloadedFile, staged, error);
    if ( !error )
        fs::permissions(staged, executable, fs::perm_options::add, error);
    // 只有写完并赋予可执行位才提交，正式程序不会在中间步骤短暂缺少权限。
    // 源和目标不同时，下载路径保留到 rename 成功后，便于重复尝试。
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
bool launchTarget(const std::filesystem::path& targetPath)
{
    // posix_spawn 的 argv 形式需要可写字符指针，字符串由调用端借给系统。
    std::string path = targetPath.native();
    // argv[0] 用真实安装路径，从更新器退出后新应用仍可定位同目录资源。
    // 末尾空指针供系统读取，不经过 shell 参数解释。
    char* arguments[]{ path.data(), nullptr };
    pid_t child{};
#if defined(POSIX_SPAWN_SETSID)
    // 明确要求在新会话中启动，以免终端关闭时顺带终止已更新的程序。
    posix_spawnattr_t attributes;
    int               launchError = posix_spawnattr_init(&attributes);
    if ( launchError == 0 ) {
        launchError = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
        if ( launchError == 0 )
            launchError = posix_spawn(
                &child, path.c_str(), nullptr, &attributes, arguments, environ);
        // init 成功后属性对象属于本函数，即使设置 flag 或 spawn 失败也须销毁。
        // 属性释放不影响已经由内核加载的新进程，也不覆盖启动状态。
        posix_spawnattr_destroy(&attributes);
    }
#else
    // libc 未提供脱离会话属性时仍检查实际装载结果。
    const int launchError =
        posix_spawn(&child, path.c_str(), nullptr, nullptr, arguments, environ);
#endif
    if ( launchError == 0 ) return true;

    // posix_spawn 的返回值直接携带 ENOEXEC、EACCES 等实际错误。
    writeStderr(
        "Cannot execute installed program: " +
        std::error_code(launchError, std::generic_category()).message() + "\n");
    return false;
}

/// @brief 执行 Linux 同卷安装事务，将失败原因写入诊断。
bool install(const std::filesystem::path& source,
             const std::filesystem::path& target)
{
    // Linux 事务提供 error_code 给平台入口，便于保留原生失败诊断。
    // 主流程只根据成功与否决定是否写标记和重启。
    const auto error = installLinuxExecutable(source, target);
    if ( !error ) return true;
    writeStderr("Failed to replace executable: " + error.message() + "\n");
    return false;
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

/// @brief POSIX 标记保存原生字节，供启动过程核对。
void writeMarkerContents(std::ostream&                marker,
                         const std::filesystem::path& target)
{
    marker << target.native();
}

}  // namespace MMM::Update
