#include "ui/utils/DesktopPathUtils.h"

#include "config/Utf8Path.h"

#include <string>
#include <system_error>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <shellapi.h>
#    include <windows.h>
#else
#    include <cerrno>
#    include <fcntl.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace MMM::UI::DesktopPathUtils
{
namespace
{
#if !defined(_WIN32)
/// @brief 通过双重 fork 启动单参数桌面命令。
/// @param command 桌面环境命令。
/// @param option 可选命令参数；为空时不传递。
/// @param argument 传递给命令的 UTF-8 参数。
/// @return 成功创建脱离 UI 进程的子进程时返回 true。
///
/// 第一层子进程立即创建第二层并退出，父进程只回收第一层；第二层通过 setsid
/// 脱离当前会话后执行目标命令，从而避免桌面程序成为 UI 的长期子进程。
/// 参数直接交给 execlp，不经过 shell 展开。
/// @warning 用户触发的低频路径：父进程会短暂等待第一层子进程退出。
bool launchDetachedArgument(const char* command, const char* option,
                            std::string_view argument)
{
    // string_view 转为拥有字符串，保证 fork 后 exec 参数以 NUL
    // 结尾且生命周期充足。
    const std::string argumentText(argument);
    const pid_t       child = fork();
    // 第一层 fork 失败时没有子进程可回收，直接报告启动失败。
    if ( child < 0 ) {
        return false;
    }
    if ( child == 0 ) {
        // 第二层 fork 让最终桌面进程脱离主程序的直接子进程关系。
        const pid_t detachedChild = fork();
        if ( detachedChild < 0 ) {
            _exit(127);
        }
        if ( detachedChild > 0 ) {
            // 中间子进程立即正常退出，向父进程证明脱离步骤已完成。
            _exit(0);
        }

        // 新会话避免桌面工具继承 UI 的控制终端和进程组。
        (void)setsid();
        if ( option ) {
            // option 作为独立 argv 元素传递，不拼接用户路径或 URL。
            execlp(command,
                   command,
                   option,
                   argumentText.c_str(),
                   static_cast<char*>(nullptr));
        } else {
            // 无选项分支保持 argument 为唯一业务参数。
            execlp(command,
                   command,
                   argumentText.c_str(),
                   static_cast<char*>(nullptr));
        }
        // exec 只在失败时返回，使用 _exit 避免执行父进程析构和缓冲刷新。
        _exit(127);
    }

    // 父进程只等待短生命周期中间子进程，不等待实际桌面应用退出。
    int   status = 0;
    pid_t waitResult;
    do {
        waitResult = waitpid(child, &status, 0);
        // 信号中断不代表启动失败，继续等待同一 PID 的最终状态。
    } while ( waitResult < 0 && errno == EINTR );
    return waitResult == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/// @brief 通过双重 fork 启动桌面命令，避免 UI 进程遗留僵尸子进程。
/// @param command 桌面环境命令。
/// @param option 可选命令参数；为空时不传递。
/// @param path 传递给命令的 UTF-8 路径。
/// @return 成功创建脱离 UI 进程的子进程时返回 true。
/// @warning 用户触发的低频路径：路径转换后转交双重 fork 启动流程。
bool launchDetached(const char* command, const char* option,
                    const std::filesystem::path& path)
{
    // 统一路径转换保证 Linux 和 macOS 命令接收项目约定的 UTF-8 文本。
    const std::string pathText = Config::pathToUtf8(path);
    return launchDetachedArgument(command, option, pathText);
}

#    if !defined(__APPLE__)
/// @brief 等待短生命周期辅助进程并判断其退出状态。
/// @param child 待等待的子进程标识。
/// @return 子进程以零状态正常退出时返回 true。
/// @warning 用户触发的低频路径：同步等待 portal 或 D-Bus 辅助进程完成。
bool waitForSuccessfulChild(pid_t child)
{
    // fork 失败以负 PID 表示，不能传给 waitpid。
    if ( child < 0 ) return false;

    int   status = 0;
    pid_t waitResult;
    do {
        waitResult = waitpid(child, &status, 0);
        // EINTR 时重试，其他 waitpid 错误由最终条件统一判失败。
    } while ( waitResult < 0 && errno == EINTR );
    // 只有目标 PID 正常退出且返回零，才认为接口调用成功。
    return waitResult == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/// @brief 将本地绝对路径编码为 freedesktop 文件管理器接口使用的 file URI。
/// @param path 待编码的本地路径。
/// @return 已完成 UTF-8 百分号编码的 file URI。
///
/// URI 保留 RFC 3986 非保留字符和路径分隔符，其余 UTF-8 字节逐个转换为大写
/// 十六进制百分号序列。按字节编码可正确保留任意多字节文件名。
std::string makeFileUri(const std::filesystem::path& path)
{
    // 固定大写数字表使输出稳定，便于 D-Bus 接口和诊断日志比较。
    constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

    const std::string pathText = Config::pathToUtf8(path);
    std::string       uri      = "file://";
    // 至少预留原始字节数，常见 ASCII 路径无需多次扩容。
    uri.reserve(uri.size() + pathText.size());
    for ( const unsigned char byte : pathText ) {
        const bool isUnreserved =
            (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
            byte == '_' || byte == '~' || byte == '/';
        if ( isUnreserved ) {
            // 斜杠保留为路径层级，不编码成普通数据字符。
            uri.push_back(static_cast<char>(byte));
            continue;
        }

        // 其他字节展开为 %HH；不按 Unicode 码点重新解释 UTF-8。
        uri.push_back('%');
        uri.push_back(HEX_DIGITS[(byte >> 4U) & 0x0FU]);
        uri.push_back(HEX_DIGITS[byte & 0x0FU]);
    }
    return uri;
}

/// @brief 通过 XDG Desktop Portal 定位文件，并提供跨桌面兼容回退。
/// @param path 待定位的文件路径。
/// @return 成功创建后台辅助进程时返回 true。
///
/// 优先调用 Portal OpenDirectory 并传递已打开文件描述符；失败后尝试
/// org.freedesktop.FileManager1.ShowItems；仍失败则用 xdg-open 打开父目录。
/// 整个回退链位于脱离进程中，不让实际文件管理器成为 UI 的直接子进程。
/// @warning 用户触发的低频路径：辅助进程内部最多执行两次限时同步 IPC。
bool showItemWithXdgDesktopInterfaces(const std::filesystem::path& path)
{
    // 使用 error_code 重载避免文件系统路径规范化抛出异常。
    std::error_code       absolutePathError;
    std::filesystem::path absolutePath =
        std::filesystem::absolute(path, absolutePathError);
    if ( absolutePathError ) return false;
    // 词法规范化清除 . 和 ..，但不解析符号链接或改变目标身份。
    absolutePath = absolutePath.lexically_normal();

    // FileManager1 接口需要 array:string: 形式的 URI 参数。
    const std::string fileUriArgument =
        "array:string:" + makeFileUri(absolutePath);
    const std::string parentPathText =
        Config::pathToUtf8(absolutePath.parent_path());

    // 外层双重 fork 与通用启动器相同，用于隔离后续回退链生命周期。
    const pid_t child = fork();
    if ( child < 0 ) return false;
    if ( child == 0 ) {
        const pid_t detachedChild = fork();
        if ( detachedChild < 0 ) _exit(127);
        if ( detachedChild > 0 ) _exit(0);

        (void)setsid();
        // Portal 以文件描述符授权目标，适用于沙箱化桌面环境。
        const int fileDescriptor = open(absolutePath.c_str(), O_RDONLY);
        if ( fileDescriptor >= 0 ) {
            // gdbus 的 handle 参数引用当前子进程继承给 portalChild 的描述符号。
            const std::string handleArgument =
                "handle " + std::to_string(fileDescriptor);
            const pid_t portalChild = fork();
            if ( portalChild == 0 ) {
                // 两秒超时防止不可用 Portal 永久阻塞回退链。
                execlp("gdbus",
                       "gdbus",
                       "call",
                       "--session",
                       "--timeout=2",
                       "--dest=org.freedesktop.portal.Desktop",
                       "--object-path=/org/freedesktop/portal/desktop",
                       "--method=org.freedesktop.portal.OpenURI.OpenDirectory",
                       "''",
                       handleArgument.c_str(),
                       "@a{sv} {}",
                       static_cast<char*>(nullptr));
                _exit(127);
            }
            const bool portalSucceeded = waitForSuccessfulChild(portalChild);
            // 无论调用结果如何，本进程都释放用于授权的文件描述符。
            close(fileDescriptor);
            // Portal 已接受请求时结束脱离进程，不再重复打开文件管理器。
            if ( portalSucceeded ) _exit(0);
        }

        // 非沙箱环境通常实现 FileManager1，可直接请求选中 URI。
        const pid_t dbusChild = fork();
        if ( dbusChild == 0 ) {
            // reply-timeout 限制服务缺失或桌面会话异常时的等待时间。
            execlp("dbus-send",
                   "dbus-send",
                   "--session",
                   "--print-reply",
                   "--reply-timeout=2000",
                   "--dest=org.freedesktop.FileManager1",
                   "/org/freedesktop/FileManager1",
                   "org.freedesktop.FileManager1.ShowItems",
                   fileUriArgument.c_str(),
                   "string:",
                   static_cast<char*>(nullptr));
            _exit(127);
        }
        // D-Bus 成功代表文件管理器已接收定位请求，无需继续回退。
        if ( waitForSuccessfulChild(dbusChild) ) _exit(0);

        // 最低兼容回退只能打开父目录，无法保证自动选中目标文件。
        execlp("xdg-open",
               "xdg-open",
               parentPathText.c_str(),
               static_cast<char*>(nullptr));
        _exit(127);
    }

    // 主进程只确认脱离辅助流程成功启动，不等待文件管理器窗口生命周期。
    return waitForSuccessfulChild(child);
}
#    endif
#endif

/// @brief 判断 URL 是否可安全交给系统浏览器。
/// @param url 待校验的外部链接。
/// @return 仅 HTTP(S) 且不含空白、控制字符或 NUL 时返回 true。
///
/// 该检查限制协议并拒绝可拆分命令行或头部的控制字符。调用方仍以单一 argv
/// 传递 URL，不经过 shell；长度上限防止异常输入造成过量参数分配。
bool isSafeExternalUrl(std::string_view url)
{
    // 只允许明确的小写 HTTP(S) 前缀，其他协议不交给外部处理程序。
    if ( !(url.starts_with("https://") || url.starts_with("http://")) ||
         url.size() > 8192U ) {
        return false;
    }
    for ( const unsigned char byte : url ) {
        // 空白、DEL 和全部 C0 控制字符均可能改变外部程序解释方式。
        if ( byte <= 0x20U || byte == 0x7FU ) return false;
    }
    return true;
}
}  // namespace

/// @brief 使用当前桌面文件管理器打开目录或定位文件。
/// @param path 需要打开或定位的现有本地路径。
/// @param selectItem 对文件为 true 时请求在父目录中选中目标。
/// @return 系统接口接受启动请求时返回 true。
///
/// 输入先以无异常文件系统查询验证存在性及目录类型，再按平台选择原生接口。
/// Windows 使用 Explorer，macOS 使用 open，Linux 优先定位接口并回退 xdg-open。
/// 返回成功只表示系统接受请求，不保证外部窗口最终可见。
/// @warning 用户触发的低频路径：可能启动进程或调用桌面 IPC，不得在每帧调用。
bool openInFileManager(const std::filesystem::path& path, bool selectItem)
{
    // 空路径没有明确目标，也不能安全推导父目录。
    if ( path.empty() ) {
        return false;
    }

    std::error_code filesystemError;
    // error_code 重载保证权限、编码或设备错误通过返回值表达而非异常。
    const bool exists = std::filesystem::exists(path, filesystemError);
    if ( filesystemError || !exists ) {
        return false;
    }
    filesystemError.clear();
    // 目录目标忽略 selectItem，因为目标本身就是需要打开的位置。
    const bool isDirectory =
        std::filesystem::is_directory(path, filesystemError);
    if ( filesystemError ) {
        return false;
    }

#if defined(_WIN32)
    // ShellExecute 返回值大于 32 才表示系统成功接受操作。
    HINSTANCE result = nullptr;
    if ( selectItem && !isDirectory ) {
        // Explorer /select 需要完整文件参数，引号保护包含空格的原生路径。
        const std::wstring parameters = L"/select,\"" + path.native() + L"\"";
        result                        = ShellExecuteW(nullptr,
                                                      L"open",
                                                      L"explorer.exe",
                                                      parameters.c_str(),
                                                      nullptr,
                                                      SW_SHOWNORMAL);
    } else {
        // 目录或无需选中时直接对目标执行系统默认 open 动作。
        result = ShellExecuteW(
            nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return reinterpret_cast<INT_PTR>(result) > 32;
#elif defined(__APPLE__)
    // macOS 的 -R 表示在 Finder 中 reveal；普通打开不传选项。
    return launchDetached(
        "open", selectItem && !isDirectory ? "-R" : nullptr, path);
#else
    if ( selectItem && !isDirectory ) {
        // Linux 定位文件需要 Portal/FileManager1 多级兼容流程。
        return showItemWithXdgDesktopInterfaces(path);
    }
    // 普通文件打开其父目录，目录目标则直接打开自身。
    const std::filesystem::path directoryToOpen =
        isDirectory ? path : path.parent_path();
    return !directoryToOpen.empty() &&
           launchDetached("xdg-open", nullptr, directoryToOpen);
#endif
}

/// @brief 使用系统默认浏览器打开经过安全校验的 HTTP(S) URL。
/// @param url 待打开的外部链接。
/// @return URL 合法且系统接受打开请求时返回 true。
///
/// 各平台均把完整 URL 作为单一参数交给系统，不使用 shell。函数拒绝非 HTTP
/// 协议、超长输入以及空白或控制字符，防止外部命令参数被拆分。
/// @warning 用户触发的低频路径：会启动系统浏览器处理程序。
bool openUrlInBrowser(std::string_view url)
{
    // 验证必须先于创建拥有字符串和任何平台调用。
    if ( !isSafeExternalUrl(url) ) return false;
    const std::string urlText(url);

#if defined(_WIN32)
    // Windows URL 为 UTF-8 ASCII 兼容协议文本，交由 ShellExecuteA 处理。
    const HINSTANCE result = ShellExecuteA(
        nullptr, "open", urlText.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
#elif defined(__APPLE__)
    // macOS 与 Linux 共用脱离启动策略，避免遗留浏览器子进程。
    return launchDetachedArgument("open", nullptr, urlText);
#else
    return launchDetachedArgument("xdg-open", nullptr, urlText);
#endif
}

}  // namespace MMM::UI::DesktopPathUtils
