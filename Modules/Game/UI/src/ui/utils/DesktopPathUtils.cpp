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
/// @brief 通过双重 fork 启动桌面命令，避免 UI 进程遗留僵尸子进程。
/// @param command 桌面环境命令。
/// @param option 可选命令参数；为空时不传递。
/// @param path 传递给命令的 UTF-8 路径。
/// @return 成功创建脱离 UI 进程的子进程时返回 true。
bool launchDetached(const char* command, const char* option,
                    const std::filesystem::path& path)
{
    const std::string pathText = Config::pathToUtf8(path);
    const pid_t       child    = fork();
    if ( child < 0 ) {
        return false;
    }
    if ( child == 0 ) {
        const pid_t detachedChild = fork();
        if ( detachedChild < 0 ) {
            _exit(127);
        }
        if ( detachedChild > 0 ) {
            _exit(0);
        }

        (void)setsid();
        if ( option ) {
            execlp(command,
                   command,
                   option,
                   pathText.c_str(),
                   static_cast<char*>(nullptr));
        } else {
            execlp(command,
                   command,
                   pathText.c_str(),
                   static_cast<char*>(nullptr));
        }
        _exit(127);
    }

    int   status = 0;
    pid_t waitResult;
    do {
        waitResult = waitpid(child, &status, 0);
    } while ( waitResult < 0 && errno == EINTR );
    return waitResult == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#    if !defined(__APPLE__)
/// @brief 等待短生命周期辅助进程并判断其退出状态。
/// @param child 待等待的子进程标识。
/// @return 子进程以零状态正常退出时返回 true。
bool waitForSuccessfulChild(pid_t child)
{
    if ( child < 0 ) return false;

    int   status = 0;
    pid_t waitResult;
    do {
        waitResult = waitpid(child, &status, 0);
    } while ( waitResult < 0 && errno == EINTR );
    return waitResult == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/// @brief 将本地绝对路径编码为 freedesktop 文件管理器接口使用的 file URI。
/// @param path 待编码的本地路径。
/// @return 已完成 UTF-8 百分号编码的 file URI。
std::string makeFileUri(const std::filesystem::path& path)
{
    constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

    const std::string pathText = Config::pathToUtf8(path);
    std::string       uri      = "file://";
    uri.reserve(uri.size() + pathText.size());
    for ( const unsigned char byte : pathText ) {
        const bool isUnreserved =
            (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
            byte == '_' || byte == '~' || byte == '/';
        if ( isUnreserved ) {
            uri.push_back(static_cast<char>(byte));
            continue;
        }

        uri.push_back('%');
        uri.push_back(HEX_DIGITS[(byte >> 4U) & 0x0FU]);
        uri.push_back(HEX_DIGITS[byte & 0x0FU]);
    }
    return uri;
}

/// @brief 通过 XDG Desktop Portal 定位文件，并提供跨桌面兼容回退。
/// @param path 待定位的文件路径。
/// @return 成功创建后台辅助进程时返回 true。
bool showItemWithXdgDesktopInterfaces(const std::filesystem::path& path)
{
    std::error_code       absolutePathError;
    std::filesystem::path absolutePath =
        std::filesystem::absolute(path, absolutePathError);
    if ( absolutePathError ) return false;
    absolutePath = absolutePath.lexically_normal();

    const std::string fileUriArgument =
        "array:string:" + makeFileUri(absolutePath);
    const std::string parentPathText =
        Config::pathToUtf8(absolutePath.parent_path());

    const pid_t child = fork();
    if ( child < 0 ) return false;
    if ( child == 0 ) {
        const pid_t detachedChild = fork();
        if ( detachedChild < 0 ) _exit(127);
        if ( detachedChild > 0 ) _exit(0);

        (void)setsid();
        const int fileDescriptor = open(absolutePath.c_str(), O_RDONLY);
        if ( fileDescriptor >= 0 ) {
            const std::string handleArgument =
                "handle " + std::to_string(fileDescriptor);
            const pid_t portalChild = fork();
            if ( portalChild == 0 ) {
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
            close(fileDescriptor);
            if ( portalSucceeded ) _exit(0);
        }

        const pid_t dbusChild = fork();
        if ( dbusChild == 0 ) {
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
        if ( waitForSuccessfulChild(dbusChild) ) _exit(0);

        execlp("xdg-open",
               "xdg-open",
               parentPathText.c_str(),
               static_cast<char*>(nullptr));
        _exit(127);
    }

    return waitForSuccessfulChild(child);
}
#    endif
#endif
}  // namespace

bool openInFileManager(const std::filesystem::path& path, bool selectItem)
{
    if ( path.empty() ) {
        return false;
    }

    std::error_code filesystemError;
    const bool      exists = std::filesystem::exists(path, filesystemError);
    if ( filesystemError || !exists ) {
        return false;
    }
    filesystemError.clear();
    const bool isDirectory =
        std::filesystem::is_directory(path, filesystemError);
    if ( filesystemError ) {
        return false;
    }

#if defined(_WIN32)
    HINSTANCE result = nullptr;
    if ( selectItem && !isDirectory ) {
        const std::wstring parameters = L"/select,\"" + path.native() + L"\"";
        result                        = ShellExecuteW(nullptr,
                                                      L"open",
                                                      L"explorer.exe",
                                                      parameters.c_str(),
                                                      nullptr,
                                                      SW_SHOWNORMAL);
    } else {
        result = ShellExecuteW(
            nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return reinterpret_cast<INT_PTR>(result) > 32;
#elif defined(__APPLE__)
    return launchDetached(
        "open", selectItem && !isDirectory ? "-R" : nullptr, path);
#else
    if ( selectItem && !isDirectory ) {
        return showItemWithXdgDesktopInterfaces(path);
    }
    const std::filesystem::path directoryToOpen =
        isDirectory ? path : path.parent_path();
    return !directoryToOpen.empty() &&
           launchDetached("xdg-open", nullptr, directoryToOpen);
#endif
}

}  // namespace MMM::UI::DesktopPathUtils
