#include "log/colorful-log.h"

#include "config/Utf8Path.h"

#include <spdlog/sinks/sink.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace
{

/// @brief Windows 下需要隐藏的用户日志根目录名。
constexpr const char* kLocalDataDirectoryName = ".local";

/// @brief 应用日志目录名。
constexpr const char* kLogAppDirectoryName = "mmm";

/// @brief 日志叶子目录名。
constexpr const char* kLogDirectoryName = "logs";

#ifdef _WIN32
/// @brief 读取 Windows 宽字符环境变量并转换为文件系统路径。
/// @param name 要读取的宽字符环境变量名称。
/// @return 环境变量对应路径，读取失败时返回空路径。
std::filesystem::path readWideEnvironmentPath(const wchar_t* name)
{
    DWORD requiredLength = GetEnvironmentVariableW(name, nullptr, 0);
    if ( requiredLength == 0 ) return {};

    std::wstring value(requiredLength, L'\0');
    DWORD        writtenLength =
        GetEnvironmentVariableW(name, value.data(), requiredLength);
    if ( writtenLength == 0 || writtenLength >= requiredLength ) return {};

    value.resize(writtenLength);
    return std::filesystem::path(value);
}
#endif

/// @brief 获取当前用户主目录路径。
/// @return 用户主目录；无法读取环境变量时退回到当前工作目录。
std::filesystem::path userHomePath()
{
#ifdef _WIN32
    std::filesystem::path userProfile = readWideEnvironmentPath(L"USERPROFILE");
    if ( !userProfile.empty() ) return userProfile;

    std::filesystem::path homeDrive = readWideEnvironmentPath(L"HOMEDRIVE");
    std::filesystem::path homePath  = readWideEnvironmentPath(L"HOMEPATH");
    if ( !homeDrive.empty() && !homePath.empty() ) {
        homeDrive /= homePath;
        return homeDrive;
    }
#else
    const char* home = std::getenv("HOME");
    if ( home && home[0] != '\0' ) {
        return MMM::Config::utf8ToPath(home);
    }
#endif

    std::error_code       currentPathError;
    std::filesystem::path currentPath =
        std::filesystem::current_path(currentPathError);
    if ( !currentPathError ) return currentPath;
    return ".";
}

/// @brief 获取日志输出目录。
/// @return 用户主目录下的 .local/mmm/logs 路径。
std::filesystem::path logDirectoryPath()
{
    std::filesystem::path path = userHomePath();
    path /= MMM::Config::utf8ToPath(kLocalDataDirectoryName);
    path /= MMM::Config::utf8ToPath(kLogAppDirectoryName);
    path /= MMM::Config::utf8ToPath(kLogDirectoryName);
    return path;
}

#ifdef _WIN32
/// @brief 将目录设置为 Windows 隐藏目录。
/// @param path 需要隐藏的目录。
void markDirectoryHidden(const std::filesystem::path& path)
{
    if ( path.empty() ) return;

    DWORD attrs = GetFileAttributesW(path.wstring().c_str());
    if ( attrs == INVALID_FILE_ATTRIBUTES ) return;
    if ( (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ) return;
    if ( (attrs & FILE_ATTRIBUTE_HIDDEN) != 0 ) return;

    SetFileAttributesW(path.wstring().c_str(), attrs | FILE_ATTRIBUTE_HIDDEN);
}
#endif

/// @brief 确保目录存在。
/// @param path 需要创建的目录。
/// @return 目录存在或创建成功时返回 true。
bool ensureDirectory(const std::filesystem::path& path)
{
    if ( path.empty() ) return false;

    std::error_code createError;
    std::filesystem::create_directories(path, createError);

    std::error_code existsError;
    bool            exists = std::filesystem::exists(path, existsError);
    if ( createError || existsError || !exists ) {
        return false;
    }

    return true;
}

/// @brief 解析可写日志目录，首选用户目录并在失败时回退。
/// @return 已存在的日志目录。
std::filesystem::path resolveWritableLogDirectory()
{
    std::filesystem::path preferred = logDirectoryPath();
    if ( ensureDirectory(preferred) ) {
#ifdef _WIN32
        std::filesystem::path localDataDirectory = userHomePath();
        localDataDirectory /= MMM::Config::utf8ToPath(kLocalDataDirectoryName);
        markDirectoryHidden(localDataDirectory);
#endif
        return preferred;
    }

    std::error_code       tempError;
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path(tempError);
    if ( !tempError ) {
        std::filesystem::path tempLogDir = tempDir;
        tempLogDir /= MMM::Config::utf8ToPath(kLogAppDirectoryName);
        tempLogDir /= MMM::Config::utf8ToPath(kLogDirectoryName);
        if ( ensureDirectory(tempLogDir) ) {
            return tempLogDir;
        }
    }

    std::filesystem::path localFallback = MMM::Config::utf8ToPath("logs");
    ensureDirectory(localFallback);
    return localFallback;
}

/// @brief 生成用于日志文件名的本地时间戳。
/// @return 格式为 YYYYMMDD-HHMMSS 的时间戳字符串。
std::string makeLogTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm    tmBuf{};
    char       buffer[32]{};
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tmBuf);
    return buffer;
}

}  // namespace

// 判断路径分隔符
inline bool is_path_sep(char c)
{
    return c == '/' || c == '\\';
}

namespace
{

/// @brief 从源码文件路径中提取模块名，用于日志前缀。
/// @param filename spdlog 记录的源码文件路径，可以为空。
/// @return 模块名；无法识别时返回 Main 或 Unknown。
std::string_view extractModuleNameFromPath(const char* filename)
{
    if ( !filename ) return "Unknown";

    std::string_view path(filename);
    const char*      keyword     = "Modules";
    size_t           keyword_len = 7;

    // 查找 "Modules" 出现的位置
    size_t pos = path.find(keyword);
    if ( pos != std::string_view::npos ) {
        // 移动到 "Modules" 之后
        size_t start = pos + keyword_len;
        // 跳过分隔符 (例如 / 或 \)
        while ( start < path.length() && is_path_sep(path[start]) ) {
            start++;
        }

        // 查找模块名的结束位置 (下一个分隔符)
        size_t end = start;
        while ( end < path.length() && !is_path_sep(path[end]) ) {
            end++;
        }

        if ( end > start ) {
            return path.substr(start, end - start);
        }
    }

    // 如果不在 Modules 目录下，回退到提取文件名（不含扩展名）或直接返回 "App"
    return "Main";
}

/// @brief 将 spdlog 字符串视图转为标准库字符串视图。
/// @param view spdlog 传入的字符串视图。
/// @return 指向同一段日志文本的标准库字符串视图。
std::string_view spdlogStringView(spdlog::string_view_t view)
{
    return { view.data(), view.size() };
}

/// @brief 向文件流写入字符串视图，避免临时字符串分配。
/// @param stream 日志文件流。
/// @param text 需要写入的文本。
void writeView(std::ofstream& stream, std::string_view text)
{
    if ( text.empty() ) return;
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

/// @brief 直接文件日志 sink，不持有 spdlog::pattern_formatter。
class PlainFileSink final : public spdlog::sinks::sink
{
public:
    /// @brief 打开指定日志文件。
    /// @param path 日志文件路径。
    explicit PlainFileSink(const std::filesystem::path& path)
    {
        m_stream.open(path, std::ios::out | std::ios::app | std::ios::binary);
    }

    /// @brief 写入一条文件日志。
    /// @param msg spdlog 传入的日志消息。
    void log(const spdlog::details::log_msg& msg) override
    {
        if ( !m_stream.is_open() ) return;

        const auto timeSinceEpoch = msg.time.time_since_epoch();
        const auto sec =
            std::chrono::duration_cast<std::chrono::seconds>(timeSinceEpoch);
        const auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                timeSinceEpoch - sec);

        const std::time_t localTime =
            std::chrono::system_clock::to_time_t(msg.time);
        std::tm tmBuf{};
#ifdef _WIN32
        localtime_s(&tmBuf, &localTime);
#else
        localtime_r(&localTime, &tmBuf);
#endif

        char timeBuffer[32]{};
        std::strftime(
            timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", &tmBuf);

        char millisBuffer[8]{};
        std::snprintf(millisBuffer,
                      sizeof(millisBuffer),
                      "%03d",
                      static_cast<int>(millis.count()));

        const std::string_view levelName =
            spdlogStringView(spdlog::level::to_string_view(msg.level));
        const std::string_view moduleName =
            extractModuleNameFromPath(msg.source.filename);
        const char* funcName =
            msg.source.funcname ? msg.source.funcname : "unknown";

        std::lock_guard lock(m_mutex);
        m_stream << '[' << timeBuffer << '.' << millisBuffer << "] [";
        writeView(m_stream, levelName);
        m_stream << '/';
        writeView(m_stream, moduleName);
        m_stream << '/' << funcName << "]: ";
        writeView(m_stream, spdlogStringView(msg.payload));

        if ( msg.level == spdlog::level::debug ||
             msg.level >= spdlog::level::err ) {
            m_stream << " ("
                     << (msg.source.filename ? msg.source.filename : "unknown")
                     << ':' << msg.source.line << ')';
        }
        m_stream << '\n';
    }

    /// @brief 刷新文件缓冲区。
    void flush() override
    {
        std::lock_guard lock(m_mutex);
        if ( m_stream.is_open() ) {
            m_stream.flush();
        }
    }

    /// @brief 忽略 spdlog 模式字符串，避免创建 pattern_formatter。
    /// @param pattern spdlog 传入的模式字符串。
    void set_pattern(const std::string& pattern) override { (void)pattern; }

    /// @brief 忽略外部 formatter，文件 sink 使用固定格式。
    /// @param sinkFormatter spdlog 传入的 formatter。
    void set_formatter(
        std::unique_ptr<spdlog::formatter> sinkFormatter) override
    {
        (void)sinkFormatter;
    }

private:
    /// @brief 文件写入互斥锁，匹配 spdlog 多线程 sink 语义。
    std::mutex m_mutex;

    /// @brief 日志文件输出流，打开失败时该 sink 静默丢弃文件日志。
    std::ofstream m_stream;
};

/// @brief 直接终端日志 sink，不持有 spdlog::pattern_formatter。
class PlainConsoleSink final : public spdlog::sinks::sink
{
public:
    /// @brief 使用项目自定义 formatter 写入控制台。
    /// @param msg spdlog 传入的日志消息。
    void log(const spdlog::details::log_msg& msg) override
    {
        spdlog::memory_buf_t buffer;
        m_formatter.format(msg, buffer);

        std::FILE* stream = msg.level >= spdlog::level::err ? stderr : stdout;
        std::lock_guard lock(m_mutex);
        std::fwrite(buffer.data(), sizeof(char), buffer.size(), stream);
    }

    /// @brief 刷新标准输出和标准错误，保持 flush_on 语义。
    void flush() override
    {
        std::lock_guard lock(m_mutex);
        std::fflush(stdout);
        std::fflush(stderr);
    }

    /// @brief 忽略 spdlog 模式字符串，控制台 sink 使用 ColorfulFormatter。
    /// @param pattern spdlog 传入的模式字符串。
    void set_pattern(const std::string& pattern) override { (void)pattern; }

    /// @brief 忽略外部 formatter，避免引入 spdlog::pattern_formatter 生命周期。
    /// @param sinkFormatter spdlog 传入的 formatter。
    void set_formatter(
        std::unique_ptr<spdlog::formatter> sinkFormatter) override
    {
        (void)sinkFormatter;
    }

private:
    /// @brief 控制台写入互斥锁，匹配 spdlog 多线程 sink 语义。
    std::mutex m_mutex;

    /// @brief 项目自定义日志格式器，避免依赖 spdlog 内置 pattern_formatter。
    ColorfulFormatter m_formatter;
};

}  // namespace

// 结构为 .../Modules/ModuleName/src/...
std::string_view ColorfulFormatter::extract_module_name(
    const char* filename) const
{
    return extractModuleNameFromPath(filename);
}

void ColorfulFormatter::format(const spdlog::details::log_msg& msg,
                               spdlog::memory_buf_t&           dest)
{
    // 时间处理
    // 使用chrono直接格式化时间点
    // 分离时间的秒和毫秒部分
    const auto time_since_epoch = msg.time.time_since_epoch();
    const auto sec =
        std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        time_since_epoch - sec);

    // 转换为本地时间
    const std::time_t t_c = std::chrono::system_clock::to_time_t(msg.time);
    std::tm           tm_buf;

#ifdef _WIN32
    localtime_s(&tm_buf, &t_c);
#else
    localtime_r(&t_c, &tm_buf);
#endif
    // 信息提取
    std::string_view module_name  = extract_module_name(msg.source.filename);
    const char*      module_color = "\033[35;1m";  // 洋红色 (用于模块名)

    // 格式化主要部分
    // 结构修改为: [时间] [级别/模块/函数]
    spdlog::fmt_lib::format_to(
        std::back_inserter(dest),
        "\033[40m[\033[36;1m{:%Y-%m-%d %H:%M:%S}.{:03d}\033[22m\033[37m] "
        "[\033[{};1m{}\033[37;22m/{}{}\033[37;22m/\033[32;1m{}\033[37;22m]",
        tm_buf,
        millis.count(),
        get_color(msg.level),
        // 级别颜色
        spdlog::level::to_string_view(msg.level),
        // 级别文本
        module_color,
        // 模块颜色
        module_name,
        // 模块文本
        msg.source.funcname ? msg.source.funcname : "unknown");  // 函数文本

    // 添加日志内容
    spdlog::fmt_lib::format_to(std::back_inserter(dest),
                               ": \033[{}m{}",
                               get_color(msg.level),
                               msg.payload);
    // 添加文件信息,仅debug/error及以上
    if ( msg.level == spdlog::level::debug ||
         msg.level >= spdlog::level::err ) {
        spdlog::fmt_lib::format_to(
            std::back_inserter(dest),
            " \033[37m(\033[35m{}\033[37m:\033[35m{}\033[37m)\033[0m\n",
            msg.source.filename,
            msg.source.line);
    } else {
        spdlog::fmt_lib::format_to(std::back_inserter(dest), "\033[0m\n");
    }
}

std::unique_ptr<spdlog::formatter> ColorfulFormatter::clone() const
{
    return std::make_unique<ColorfulFormatter>();
}

const char* ColorfulFormatter::get_color(spdlog::level::level_enum level) const
{
    switch ( level ) {
    case spdlog::level::trace: return "37";       // 白色
    case spdlog::level::debug: return "36";       // 青色
    case spdlog::level::info: return "32";        // 绿色
    case spdlog::level::warn: return "33";        // 黄色
    case spdlog::level::err: return "31";         // 红色
    case spdlog::level::critical: return "31;1";  // 亮红色
    default: return "0";
    }
}

uint32_t XLogger::glcalls   = 0;
uint32_t XLogger::drawcalls = 0;

std::shared_ptr<spdlog::logger> XLogger::logger;

void XLogger::init(const char* name)
{
    const std::filesystem::path logDirectory = resolveWritableLogDirectory();

    const std::string     timestamp  = makeLogTimestamp();
    std::filesystem::path allLogPath = logDirectory;
    allLogPath /= "mmm-" + timestamp + ".log";
    std::filesystem::path errorLogPath = logDirectory;
    errorLogPath /= "mmm-error-" + timestamp + ".log";

    // 创建三个sink（终端、全量文件、错误文件）
    auto console_sink    = std::make_shared<PlainConsoleSink>();
    auto file_all_sink   = std::make_shared<PlainFileSink>(allLogPath);
    auto file_error_sink = std::make_shared<PlainFileSink>(errorLogPath);

    // 设置sink级别过滤
    console_sink->set_level(spdlog::level::trace);
    file_all_sink->set_level(spdlog::level::debug);
    file_error_sink->set_level(spdlog::level::err);

    // 创建组合logger
    logger = std::make_shared<spdlog::logger>(
        name,
        spdlog::sinks_init_list{
            console_sink, file_all_sink, file_error_sink });

    // 设置全局日志级别
    logger->set_level(spdlog::level::trace);

    // 设置实时刷新
    logger->flush_on(spdlog::level::trace);

    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    XINFO("日志初始化完成，日志目录: {}",
          MMM::Config::pathToUtf8(logDirectory));
}

void XLogger::shutdown()
{
    auto activeLogger = std::move(logger);
    if ( !activeLogger ) return;

    activeLogger->flush();
    spdlog::shutdown();
    activeLogger.reset();
}

void XLogger::enable()
{
    logger->set_level(spdlog::level::trace);
}

void XLogger::disable()
{
    logger->set_level(spdlog::level::off);
}

void XLogger::setlevel(spdlog::level::level_enum level)
{
    // 设置全局日志级别
    logger->set_level(level);
}
