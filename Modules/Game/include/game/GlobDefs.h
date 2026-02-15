#pragma once

#include <clocale>

#ifdef _WIN32
#include <windows.h>
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
struct RTTILogger
{
    RTTILogger()
    {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8); // 强制当前控制台输出为 UTF-8
#endif
        std::setlocale(LC_ALL, ".UTF-8");
        XLogger::init("MMM");
    }

    ~RTTILogger() { XLogger::shutdown(); }
};

/// @brief 全局日志管理器实例 (程序启动时自动初始化)
inline RTTILogger rttiLogger;

} // namespace MMM
