#include "log/colorful-log.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

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
        return std::filesystem::path(home);
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
    path /= kLocalDataDirectoryName;
    path /= kLogAppDirectoryName;
    path /= kLogDirectoryName;
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
        localDataDirectory /= kLocalDataDirectoryName;
        markDirectoryHidden(localDataDirectory);
#endif
        return preferred;
    }

    std::error_code       tempError;
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path(tempError);
    if ( !tempError ) {
        std::filesystem::path tempLogDir = tempDir;
        tempLogDir /= kLogAppDirectoryName;
        tempLogDir /= kLogDirectoryName;
        if ( ensureDirectory(tempLogDir) ) {
            return tempLogDir;
        }
    }

    std::filesystem::path localFallback = "logs";
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

// 结构为 .../Modules/ModuleName/src/...
std::string_view ColorfulFormatter::extract_module_name(
    const char* filename) const
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
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_all_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        allLogPath.string());
    auto file_error_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        errorLogPath.string());

    // 设置统一的自定义格式
    auto formatter = std::make_unique<ColorfulFormatter>();
    console_sink->set_formatter(formatter->clone());
    file_all_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
    file_error_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");

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

    XINFO("日志初始化完成，日志目录: {}", logDirectory.string());
}

void XLogger::shutdown()
{
    // 销毁 logger
    spdlog::drop("MMM");
    // 直接销毁 logger 对象
    logger.reset();
    // 销毁所有 logger
    spdlog::shutdown();
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
