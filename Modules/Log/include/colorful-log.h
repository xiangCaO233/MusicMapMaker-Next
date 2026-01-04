#ifndef XIANG_COLORFULLOG_H
#define XIANG_COLORFULLOG_H

#include <spdlog/common.h>
#ifdef _WIN32
#endif

#include <memory>
#include <spdlog/details/log_msg.h>
#include <spdlog/fmt/chrono.h>
#include <spdlog/formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string_view>

#define XTRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define XDEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define XINFO(...) SPDLOG_INFO(__VA_ARGS__)
#define XWARN(...) SPDLOG_WARN(__VA_ARGS__)
#define XERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define XCRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

class ColorfulFormatter : public spdlog::formatter
{
public:
    void format(const spdlog::details::log_msg& msg,
                spdlog::memory_buf_t&           dest) override;
    std::unique_ptr<spdlog::formatter> clone() const override;

private:
    const char*      get_color(spdlog::level::level_enum level) const;
    std::string_view extract_module_name(const char* filename) const;
};

class XLogger
{
    // 终端日志实体
    static std::shared_ptr<spdlog::logger> logger;

public:
    static uint32_t glcalls;
    static uint32_t drawcalls;
    static void     init(const char* name);
    static void     shutdown();
    static void     enable();
    static void     disable();
    static void     setlevel(spdlog::level::level_enum level);
};
#endif  // XIANG_COLORFULLOG_H
