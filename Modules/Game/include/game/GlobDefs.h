#pragma once

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
struct RTIILogger {
    RTIILogger() { XLogger::init("MMM"); }
    ~RTIILogger() { XLogger::shutdown(); }
};

/// @brief 全局日志管理器实例 (程序启动时自动初始化)
inline RTIILogger rtiiLogger;

}  // namespace MMM
