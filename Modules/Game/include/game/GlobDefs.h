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

/// @brief 正常完成主循环和资源收尾的进程退出码。
constexpr int EXIT_NORMAL = 0;

/// @brief 窗口或 Vulkan 上下文无法初始化时使用的进程退出码。
/// @note 保留既有拼写以维持调用点和外部脚本兼容。
constexpr int EXIT_WINDOW_EXEPTION = 1;

/**
 * @brief RAII 日志管理器
 *
 * 利用静态对象的生命周期自动初始化和关闭日志系统。
 * 构造时先完成平台控制台与崩溃处理配置，再解析当前可执行文件目录并切换
 * 工作目录，最后初始化日志。路径失败不会中止启动，而是在日志可用后告警。
 *
 * @warning 全局静态生命周期：构造发生在 main 之前，析构发生在其他业务静态
 * 对象收尾阶段；不得依赖尚未初始化或已经销毁的业务单例。
 */
struct RTTILogger {
    /// @brief 初始化平台控制台、崩溃处理、工作目录与日志系统。
    ///
    /// 可执行目录解析全部使用非抛出路径接口或平台返回值，失败原因暂存到
    /// workingDirectoryError，等待 XLogger 初始化完成后统一输出。
    RTTILogger()
    {
#ifdef _WIN32
        // Windows 控制台统一输出 UTF-8，避免中文日志使用系统代码页乱码。
        SetConsoleOutputCP(CP_UTF8);

        // 为标准输出和错误输出启用 ANSI 虚拟终端颜色支持。
        /// @brief 尝试给指定标准句柄启用虚拟终端处理位。
        /// @param stdHandle STD_OUTPUT_HANDLE 或 STD_ERROR_HANDLE。
        auto enableVT = [](DWORD stdHandle) {
            // 无效或不可查询的句柄保持原状态，应用仍可继续启动。
            HANDLE hOut = GetStdHandle(stdHandle);
            if ( hOut != INVALID_HANDLE_VALUE ) {
                DWORD dwMode = 0;
                if ( GetConsoleMode(hOut, &dwMode) ) {
                    // 0x0004 对应 ENABLE_VIRTUAL_TERMINAL_PROCESSING。
                    dwMode |= 0x0004;
                    SetConsoleMode(hOut, dwMode);
                }
            }
        };
        enableVT(STD_OUTPUT_HANDLE);
        enableVT(STD_ERROR_HANDLE);

        // 崩溃处理器必须在后续业务初始化前注册，以覆盖早期故障。
        register_crash_handler();
#endif
        // C 与 C++ 路径及格式化依赖统一 UTF-8 区域设置。
        std::setlocale(LC_ALL, ".UTF-8");
        // 设置工作目录为可执行程序所在目录。
        std::string           workingDirectoryError;
        std::filesystem::path exePath;
#ifdef _WIN32
        // Windows 使用宽字符 API，避免可执行路径中的非 ASCII 字符丢失。
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
        // 缓冲不足时 API 返回非零并通过 pathLength 报告需求；此处记录失败，
        // 不在全局构造阶段进行动态扩容或异常处理。
        if ( _NSGetExecutablePath(buffer, &pathLength) != 0 ) {
            workingDirectoryError = "failed to query executable path";
        } else {
            exePath = std::filesystem::path(buffer);
        }
#else
        // Linux 通过 procfs 读取当前进程实际可执行文件，不依赖 argv[0]。
        // readlink 不追加终止空字符，因此成功后使用显式长度构造 std::string。
        // PATH_MAX 同时作为缓冲容量和结果上界，等于容量视为不可安全使用。
        char    buffer[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", buffer, PATH_MAX);
        if ( count < 0 || count >= PATH_MAX ) {
            workingDirectoryError = "failed to query executable path";
        } else {
            exePath = std::filesystem::path(std::string(buffer, count));
        }
#endif
        if ( !exePath.empty() ) {
            // 只在成功取得可执行路径后尝试切换其父目录。
            std::error_code currentPathError;
            const auto      executableDirectory = exePath.parent_path();
            if ( executableDirectory.empty() ) {
                workingDirectoryError = "empty executable directory";
            } else {
                // error_code 重载避免在静态初始化阶段抛出异常。
                std::filesystem::current_path(executableDirectory,
                                              currentPathError);
                if ( currentPathError ) {
                    workingDirectoryError = currentPathError.message();
                }
            }
        }
        // 日志初始化必须先于路径错误输出，确保警告进入统一日志后端。
        XLogger::init("MMM");
        if ( !workingDirectoryError.empty() ) {
            XWARN("Failed to set working directory: {}", workingDirectoryError);
        }
    }

    /// @brief 在进程静态析构阶段关闭日志后端。
    /// @note shutdown 必须能够处理业务代码已经完成或部分初始化失败的状态。
    ~RTTILogger() { XLogger::shutdown(); }
};

/// @brief 程序启动时自动初始化、退出时自动关闭的全局日志管理器。
/// @warning 静态初始化顺序要求其他全局对象不要在本实例构造前写业务日志。
inline RTTILogger rttiLogger{};

}  // namespace MMM
