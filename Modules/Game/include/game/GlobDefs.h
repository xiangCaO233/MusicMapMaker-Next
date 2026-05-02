#pragma once

#include <clocale>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#    include <windows.h>
#endif

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
#endif
        std::setlocale(LC_ALL, ".UTF-8");
        // 2. 核心逻辑：设置工作目录为可执行程序所在目录
        try {
            std::filesystem::path exePath;
#ifdef _WIN32
            wchar_t buffer[MAX_PATH];
            GetModuleFileNameW(NULL, buffer, MAX_PATH);
            exePath = std::filesystem::path(buffer);
#else
            char    buffer[PATH_MAX];
            ssize_t count = readlink("/proc/self/exe", buffer, PATH_MAX);
            if ( count != -1 ) {
                exePath = std::filesystem::path(std::string(buffer, count));
            }
#endif
            if ( !exePath.empty() ) {
                // 取父目录并设置为当前工作目录
                std::filesystem::current_path(exePath.parent_path());
            }
        } catch ( const std::exception& e ) {
            // 注意：此时日志系统还没初始化，只能用 std::cerr
            std::cerr << "Failed to set working directory: " << e.what()
                      << std::endl;
        }
        XLogger::init("MMM");
    }

    ~RTTILogger() { XLogger::shutdown(); }
};

/// @brief 全局日志管理器实例 (程序启动时自动初始化)
inline RTTILogger rttiLogger{};

}  // namespace MMM
