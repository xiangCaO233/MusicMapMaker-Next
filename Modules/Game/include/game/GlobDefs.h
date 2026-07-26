#pragma once

#include <clocale>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#elif defined(__APPLE__)
#    include <limits.h>
#    include <mach-o/dyld.h>
#else
#    include <limits.h>
#    include <unistd.h>
#endif

#include "game/CrashHandler.h"
#include "log/colorful-log.h"

namespace MMM
{

/// @brief 正常退出码
constexpr int EXIT_NORMAL = 0;

/// @brief 窗口异常退出码
constexpr int EXIT_WINDOW_EXEPTION = 1;

/**
 * @brief RAII 日志管理器
 *
 * 利用静态对象的生命周期自动初始化和关闭日志系统。
 */
struct RTTILogger {
    /// @brief 初始化日志，并把工作目录切换到当前可执行文件所在目录。
    RTTILogger()
    {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);  // 强制当前控制台输出为 UTF-8

        // 开启虚拟终端处理以支持 ANSI 转义序列 (颜色)
        auto enableVT = [](DWORD stdHandle) {
            HANDLE hOut = GetStdHandle(stdHandle);
            if ( hOut != INVALID_HANDLE_VALUE ) {
                DWORD dwMode = 0;
                if ( GetConsoleMode(hOut, &dwMode) ) {
                    dwMode |= 0x0004;  // ENABLE_VIRTUAL_TERMINAL_PROCESSING
                    SetConsoleMode(hOut, dwMode);
                }
            }
        };
        enableVT(STD_OUTPUT_HANDLE);
        enableVT(STD_ERROR_HANDLE);

        // 注册崩溃处理器
        register_crash_handler();
#endif
        std::setlocale(LC_ALL, ".UTF-8");
        // 设置工作目录为可执行程序所在目录。
        std::string           workingDirectoryError;
        std::filesystem::path exePath;
#ifdef _WIN32
        wchar_t buffer[MAX_PATH];
        DWORD   pathLength = GetModuleFileNameW(NULL, buffer, MAX_PATH);
        if ( pathLength == 0 || pathLength >= MAX_PATH ) {
            workingDirectoryError = "failed to query executable path";
        } else {
            exePath = std::filesystem::path(buffer);
        }
#elif defined(__APPLE__)
        /// @brief 保存 macOS 当前可执行文件路径。
        char buffer[PATH_MAX];
        /// @brief 记录路径缓冲区容量，并接收实际所需容量。
        uint32_t pathLength = sizeof(buffer);
        if ( _NSGetExecutablePath(buffer, &pathLength) != 0 ) {
            workingDirectoryError = "failed to query executable path";
        } else {
            exePath = std::filesystem::path(buffer);
        }
#else
        char    buffer[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", buffer, PATH_MAX);
        if ( count < 0 || count >= PATH_MAX ) {
            workingDirectoryError = "failed to query executable path";
        } else {
            exePath = std::filesystem::path(std::string(buffer, count));
        }
#endif
        if ( !exePath.empty() ) {
            std::error_code currentPathError;
            const auto      executableDirectory = exePath.parent_path();
            if ( executableDirectory.empty() ) {
                workingDirectoryError = "empty executable directory";
            } else {
                std::filesystem::current_path(executableDirectory,
                                              currentPathError);
                if ( currentPathError ) {
                    workingDirectoryError = currentPathError.message();
                }
            }
        }
        XLogger::init("MMM");
        if ( !workingDirectoryError.empty() ) {
            XWARN("Failed to set working directory: {}", workingDirectoryError);
        }
    }

    ~RTTILogger() { XLogger::shutdown(); }
};

/// @brief 全局日志管理器实例 (程序启动时自动初始化)
inline RTTILogger rttiLogger{};

}  // namespace MMM
