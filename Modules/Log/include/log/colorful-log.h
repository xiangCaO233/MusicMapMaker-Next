#pragma once

#include <spdlog/common.h>

#include <memory>
#include <spdlog/details/log_msg.h>
#include <spdlog/fmt/chrono.h>
#include <spdlog/formatter.h>
#include <spdlog/spdlog.h>
#include <string_view>

/// @brief 记录 trace 级别日志，并由 spdlog 自动附带源码位置。
#define XTRACE(...) SPDLOG_TRACE(__VA_ARGS__)
/// @brief 记录 debug 级别日志，并由 spdlog 自动附带源码位置。
#define XDEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
/// @brief 记录 info 级别日志，并由 spdlog 自动附带源码位置。
#define XINFO(...) SPDLOG_INFO(__VA_ARGS__)
/// @brief 记录 warn 级别日志，并由 spdlog 自动附带源码位置。
#define XWARN(...) SPDLOG_WARN(__VA_ARGS__)
/// @brief 记录 error 级别日志，并由 spdlog 自动附带源码位置。
#define XERROR(...) SPDLOG_ERROR(__VA_ARGS__)
/// @brief 记录 critical 级别日志，并由 spdlog 自动附带源码位置。
#define XCRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

/// @brief 将日志消息格式化为带颜色、模块名和源码位置的终端文本。
/// @note 该 formatter 只持有无状态配置，因此克隆不会共享可变状态。
class ColorfulFormatter : public spdlog::formatter
{
public:
    /// @brief 将单条日志消息追加到 spdlog 提供的目标缓冲区。
    /// @param msg 包含时间、级别、负载和源码位置的日志消息。
    /// @param dest 接收 ANSI 彩色文本的输出缓冲区。
    void format(const spdlog::details::log_msg& msg,
                spdlog::memory_buf_t&           dest) override;

    /// @brief 创建等价 formatter，满足不同 sink 独立持有格式器的约定。
    /// @return 新建的无状态 ColorfulFormatter。
    std::unique_ptr<spdlog::formatter> clone() const override;

private:
    /// @brief 查询日志级别对应的 ANSI 前景色代码。
    /// @param level 待着色的 spdlog 日志级别。
    /// @return 静态 ANSI 颜色代码字符串。
    const char* get_color(spdlog::level::level_enum level) const;

    /// @brief 从源码路径提取顶层模块名，作为日志上下文。
    /// @param filename 编译器记录的源码文件路径，可以为空。
    /// @return 指向原路径内容或静态回退文本的字符串视图。
    std::string_view extract_module_name(const char* filename) const;
};

/// @brief 管理进程级 spdlog 实例及其终端、文件 sink。
/// @note 调用方应在首次记录日志前初始化，并在进程退出阶段关闭。
class XLogger
{
    /// @brief 进程共享日志器，初始化后同时分发到终端和文件 sink。
    static std::shared_ptr<spdlog::logger> logger;

public:
    /// @brief OpenGL 调用统计计数，由图形诊断代码按帧维护。
    static uint32_t glcalls;

    /// @brief 绘制调用统计计数，由图形诊断代码按帧维护。
    static uint32_t drawcalls;

    /// @brief 创建并注册进程级日志器。
    /// @param name 注册到 spdlog 的日志器名称。
    static void init(const char* name);

    /// @brief 刷新所有 sink 并释放进程级日志器。
    static void shutdown();

    /// @brief 将最低日志级别恢复为 trace。
    static void enable();

    /// @brief 将日志级别设为 off，保留日志器供后续恢复。
    static void disable();

    /// @brief 设置进程级日志过滤阈值。
    /// @param level 新的最低日志级别。
    static void setlevel(spdlog::level::level_enum level);
};
