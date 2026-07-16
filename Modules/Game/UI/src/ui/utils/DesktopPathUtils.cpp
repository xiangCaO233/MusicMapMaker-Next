#include "ui/utils/DesktopPathUtils.h"

#include "config/Utf8Path.h"

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
    (void)selectItem;
    const std::filesystem::path directoryToOpen =
        isDirectory ? path : path.parent_path();
    return !directoryToOpen.empty() &&
           launchDetached("xdg-open", nullptr, directoryToOpen);
#endif
}

}  // namespace MMM::UI::DesktopPathUtils
