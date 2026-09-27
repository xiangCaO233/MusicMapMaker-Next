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
    // 先查询包含结尾空字符所需的缓冲区长度，避免固定容量截断用户路径。
    DWORD requiredLength = GetEnvironmentVariableW(name, nullptr, 0);
    if ( requiredLength == 0 ) return {};

    // Windows 路径保持宽字符形式，直到交由 filesystem::path 接管。
    std::wstring value(requiredLength, L'\0');
    DWORD        writtenLength =
        GetEnvironmentVariableW(name, value.data(), requiredLength);
    // 环境变量在两次调用之间增长时，返回长度会越过原缓冲区。
    if ( writtenLength == 0 || writtenLength >= requiredLength ) return {};

    // 去掉为终止符预留的尾部空间，确保路径不包含额外空字符。
    value.resize(writtenLength);
    return std::filesystem::path(value);
}
#endif

/// @brief 获取当前用户主目录路径。
/// @return 用户主目录；无法读取环境变量时退回到当前工作目录。
std::filesystem::path userHomePath()
{
#ifdef _WIN32
    // USERPROFILE 是 Windows 用户目录的首选来源。
    std::filesystem::path userProfile = readWideEnvironmentPath(L"USERPROFILE");
    if ( !userProfile.empty() ) return userProfile;

    // 兼容仅提供传统驱动器与相对主目录变量的环境。
    std::filesystem::path homeDrive = readWideEnvironmentPath(L"HOMEDRIVE");
    std::filesystem::path homePath  = readWideEnvironmentPath(L"HOMEPATH");
    if ( !homeDrive.empty() && !homePath.empty() ) {
        homeDrive /= homePath;
        return homeDrive;
    }
#else
    // 类 Unix 平台使用进程环境中的 HOME，并通过统一 UTF-8 边界转换。
    const char* home = std::getenv("HOME");
    if ( home && home[0] != '\0' ) {
        return MMM::Config::utf8ToPath(home);
    }
#endif

    // 环境变量不可用时，以当前目录维持日志初始化的可用性。
    std::error_code       currentPathError;
    std::filesystem::path currentPath =
        std::filesystem::current_path(currentPathError);
    if ( !currentPathError ) return currentPath;
    // 连当前目录也不可查询时保留相对路径回退，避免日志初始化抛出异常。
    return ".";
}

/// @brief 获取日志输出目录。
/// @return 用户主目录下的 .local/mmm/logs 路径。
std::filesystem::path logDirectoryPath()
{
    // 分段拼接便于 filesystem 按平台选择正确的目录分隔符。
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
    // 空路径和不存在的路径都不应触发属性写入。
    if ( path.empty() ) return;

    DWORD attrs = GetFileAttributesW(path.wstring().c_str());
    if ( attrs == INVALID_FILE_ATTRIBUTES ) return;
    // 仅目录需要隐藏，避免意外修改同名普通文件的属性。
    if ( (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ) return;
    // 已隐藏时跳过系统调用，同时保留目录的其他属性位。
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

    // 使用 error_code 重载，保证日志基础设施失败时不会传播异常。
    std::error_code createError;
    std::filesystem::create_directories(path, createError);

    // create_directories 对已存在目录返回 false，因此再独立确认存在性。
    std::error_code existsError;
    bool            exists = std::filesystem::exists(path, existsError);
    if ( createError || existsError || !exists ) {
        return false;
    }

    return true;
}

/// @brief 解析可写日志目录，首选用户目录并在失败时回退。
/// @return 已存在的日志目录。
/// @note 回退顺序固定为用户目录、系统临时目录和当前工作目录。
/// @note 本函数只报告路径，不把目录创建失败升级为应用启动失败。
std::filesystem::path resolveWritableLogDirectory()
{
    // 用户目录是稳定日志位置，也是正常运行时的唯一首选路径。
    std::filesystem::path preferred = logDirectoryPath();
    if ( ensureDirectory(preferred) ) {
#ifdef _WIN32
        // 只隐藏 .local 根目录，日志文件仍可通过明确路径访问。
        std::filesystem::path localDataDirectory = userHomePath();
        localDataDirectory /= MMM::Config::utf8ToPath(kLocalDataDirectoryName);
        markDirectoryHidden(localDataDirectory);
#endif
        return preferred;
    }

    // 用户目录不可写时尝试系统临时目录，避免阻断应用启动。
    std::error_code       tempError;
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path(tempError);
    if ( !tempError ) {
        std::filesystem::path tempLogDir = tempDir;
        // 临时目录下仍保留应用与日志两级命名，防止污染目录根部。
        tempLogDir /= MMM::Config::utf8ToPath(kLogAppDirectoryName);
        tempLogDir /= MMM::Config::utf8ToPath(kLogDirectoryName);
        if ( ensureDirectory(tempLogDir) ) {
            return tempLogDir;
        }
    }

    // 最终回退为工作目录下的 logs；即使创建失败也返回确定路径。
    std::filesystem::path localFallback = MMM::Config::utf8ToPath("logs");
    ensureDirectory(localFallback);
    return localFallback;
}

/// @brief 生成用于日志文件名的本地时间戳。
/// @return 格式为 YYYYMMDD-HHMMSS 的时间戳字符串。
std::string makeLogTimestamp()
{
    // 文件名精确到秒，兼顾可读性与一次运行一组文件的配对需求。
    const auto now = std::chrono::system_clock::now();
    const auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm    tmBuf{};
    char       buffer[32]{};
#ifdef _WIN32
    // 使用线程安全的平台函数，避免共享 std::localtime 的静态缓冲区。
    localtime_s(&tmBuf, &t);
#else
    // POSIX 版本由调用方提供缓冲区，允许并发初始化日志文件名。
    localtime_r(&t, &tmBuf);
#endif
    // 固定宽度时间戳使同一目录中的日志按字典序排列。
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tmBuf);
    return buffer;
}

}  // namespace

/// @brief 判断字符是否为任一平台常见的路径分隔符。
/// @param c 待检查字符。
/// @return 是正斜杠或反斜杠时返回 true。
inline bool is_path_sep(char c)
{
    return c == '/' || c == '\\';
}

namespace
{

/// @brief 从源码文件路径中提取模块名，用于日志前缀。
/// @param filename spdlog 记录的源码文件路径，可以为空。
/// @return 模块名；无法识别时返回 Main 或 Unknown。
/// @note 返回值借用 filename 或指向静态文本，不拥有字符数据。
/// @note 调用方必须在原始日志消息有效期内消费返回的字符串视图。
std::string_view extractModuleNameFromPath(const char* filename)
{
    // 无源码位置的第三方日志使用显式占位，避免返回悬空视图。
    if ( !filename ) return "Unknown";

    // 字符串视图只借用 spdlog 消息在本次格式化期间有效的路径。
    std::string_view path(filename);
    const char*      keyword     = "Modules";
    size_t           keyword_len = 7;

    // 从路径中定位项目模块根，兼容绝对路径和相对路径。
    size_t pos = path.find(keyword);
    if ( pos != std::string_view::npos ) {
        // 模块名起点位于 Modules 片段及其路径分隔符之后。
        size_t start = pos + keyword_len;
        // 同时接受正反斜杠，以解析跨平台构建产物中的源码路径。
        while ( start < path.length() && is_path_sep(path[start]) ) {
            start++;
        }

        // 第一个后续分隔符界定顶层模块名，不暴露更深的目录层级。
        size_t end = start;
        while ( end < path.length() && !is_path_sep(path[end]) ) {
            end++;
        }

        if ( end > start ) {
            // 返回借用原路径的视图，不为每条日志分配模块名字符串。
            return path.substr(start, end - start);
        }
    }

    // 非 Modules 源码统一归入 Main，保持日志前缀稳定。
    return "Main";
}

/// @brief 将 spdlog 字符串视图转为标准库字符串视图。
/// @param view spdlog 传入的字符串视图。
/// @return 指向同一段日志文本的标准库字符串视图。
std::string_view spdlogStringView(spdlog::string_view_t view)
{
    // 显式传递长度，允许日志负载包含非终止的连续字符区间。
    return { view.data(), view.size() };
}

/// @brief 向文件流写入字符串视图，避免临时字符串分配。
/// @param stream 日志文件流。
/// @param text 需要写入的文本。
/// @note 调用方负责串行化 stream，并在需要时检查其错误状态。
void writeView(std::ofstream& stream, std::string_view text)
{
    // 空视图无需触碰流状态，非空内容则按精确长度写出。
    if ( text.empty() ) return;
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

/// @brief 直接文件日志 sink，不持有 spdlog::pattern_formatter。
/// @details 该 sink 生成无颜色的稳定文本格式，适合归档和文本检索。
/// 每条消息在同一互斥区内完成写入，保证多线程日志不会字段交错。
/// 文件打开失败不会影响其他 sink，后续调用会静默跳过文件输出。
class PlainFileSink final : public spdlog::sinks::sink
{
public:
    /// @brief 打开指定日志文件。
    /// @param path 日志文件路径。
    /// @note 文件采用二进制追加模式，避免平台文本转换改变格式。
    explicit PlainFileSink(const std::filesystem::path& path)
    {
        // 追加模式保留同一时间戳文件中此前已经落盘的诊断记录。
        m_stream.open(path, std::ios::out | std::ios::app | std::ios::binary);
    }

    /// @brief 写入一条文件日志。
    /// @param msg spdlog 传入的日志消息。
    /// @note 时间转换在加锁前完成，临界区只保护共享文件流。
    /// @note debug 与 err 以上级别会附加源码位置以支持问题定位。
    void log(const spdlog::details::log_msg& msg) override
    {
        // 文件打开失败时仅禁用该 sink，终端及其他 sink 仍可继续工作。
        if ( !m_stream.is_open() ) return;

        // 先拆分秒和毫秒，避免浮点时间换算造成边界舍入。
        const auto timeSinceEpoch = msg.time.time_since_epoch();
        const auto sec =
            std::chrono::duration_cast<std::chrono::seconds>(timeSinceEpoch);
        const auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                timeSinceEpoch - sec);

        const std::time_t localTime =
            std::chrono::system_clock::to_time_t(msg.time);
        // 每次调用使用栈上 tm，防止多个日志线程共享转换缓冲区。
        std::tm tmBuf{};
#ifdef _WIN32
        localtime_s(&tmBuf, &localTime);
#else
        localtime_r(&localTime, &tmBuf);
#endif

        char timeBuffer[32]{};
        // 文件日志不带 ANSI 控制符，便于文本工具直接处理。
        std::strftime(
            timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", &tmBuf);

        char millisBuffer[8]{};
        // 毫秒固定补齐三位，与终端格式保持字段宽度一致。
        std::snprintf(millisBuffer,
                      sizeof(millisBuffer),
                      "%03d",
                      static_cast<int>(millis.count()));

        const std::string_view levelName =
            spdlogStringView(spdlog::level::to_string_view(msg.level));
        // 模块与函数共同提供比单纯文件名更紧凑的调用上下文。
        const std::string_view moduleName =
            extractModuleNameFromPath(msg.source.filename);
        const char* funcName =
            msg.source.funcname ? msg.source.funcname : "unknown";

        // 一条消息的所有字段必须在同一临界区内写入，避免线程间交错。
        std::lock_guard lock(m_mutex);
        m_stream << '[' << timeBuffer << '.' << millisBuffer << "] [";
        writeView(m_stream, levelName);
        m_stream << '/';
        writeView(m_stream, moduleName);
        m_stream << '/' << funcName << "]: ";
        writeView(m_stream, spdlogStringView(msg.payload));

        // 调试和错误日志附带源码位置，普通信息保持简洁。
        if ( msg.level == spdlog::level::debug ||
             msg.level >= spdlog::level::err ) {
            m_stream << " ("
                     << (msg.source.filename ? msg.source.filename : "unknown")
                     << ':' << msg.source.line << ')';
        }
        m_stream << '\n';
    }

    /// @brief 刷新文件缓冲区。
    /// @note 允许在任意日志线程调用，并与正在进行的写入互斥。
    void flush() override
    {
        // flush 与写入使用同一把锁，确保流缓冲状态不会并发变化。
        std::lock_guard lock(m_mutex);
        if ( m_stream.is_open() ) {
            m_stream.flush();
        }
    }

    /// @brief 忽略 spdlog 模式字符串，避免创建 pattern_formatter。
    /// @param pattern spdlog 传入的模式字符串。
    /// @note 输出格式由 log 固定实现，调用不会改变 sink 状态。
    void set_pattern(const std::string& pattern) override { (void)pattern; }

    /// @brief 忽略外部 formatter，文件 sink 使用固定格式。
    /// @param sinkFormatter spdlog 传入的 formatter。
    /// @note 参数所有权随调用结束释放，不保存在 sink 中。
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
/// @details 消息先由 ColorfulFormatter 写入线程私有缓冲区，再原子化写入
/// stdout 或 stderr，避免来自不同日志线程的 ANSI 控制序列相互穿插。
/// sink 自身只共享输出锁和无状态 formatter，不共享消息缓冲区。
class PlainConsoleSink final : public spdlog::sinks::sink
{
public:
    /// @brief 使用项目自定义 formatter 写入控制台。
    /// @param msg spdlog 传入的日志消息。
    /// @note error 及以上写入 stderr，其余级别写入 stdout。
    /// @note 格式化位于锁外，锁内仅执行一次完整缓冲区写入。
    void log(const spdlog::details::log_msg& msg) override
    {
        // 先在线程私有缓冲区完成格式化，缩短控制台写锁持有时间。
        spdlog::memory_buf_t buffer;
        m_formatter.format(msg, buffer);

        // error 及以上走 stderr，其余级别走 stdout，符合命令行重定向惯例。
        std::FILE* stream = msg.level >= spdlog::level::err ? stderr : stdout;
        // fwrite 必须覆盖完整消息，避免彩色控制序列与正文被并发拆开。
        std::lock_guard lock(m_mutex);
        std::fwrite(buffer.data(), sizeof(char), buffer.size(), stream);
    }

    /// @brief 刷新标准输出和标准错误，保持 flush_on 语义。
    /// @note 两个流在同一临界区刷新，避免与消息写入交错。
    void flush() override
    {
        // 同时刷新两个标准流，确保 shutdown 前所有级别均已输出。
        std::lock_guard lock(m_mutex);
        std::fflush(stdout);
        std::fflush(stderr);
    }

    /// @brief 忽略 spdlog 模式字符串，控制台 sink 使用 ColorfulFormatter。
    /// @param pattern spdlog 传入的模式字符串。
    /// @note 终端布局是项目日志协议的一部分，不接受运行时模式替换。
    void set_pattern(const std::string& pattern) override { (void)pattern; }

    /// @brief 忽略外部 formatter，避免引入 spdlog::pattern_formatter 生命周期。
    /// @param sinkFormatter spdlog 传入的 formatter。
    /// @note 参数只用于满足 sink 接口，传入实例不会被安装。
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

/// @brief 从形如 Modules/ModuleName 的源码路径提取 ModuleName。
/// @param filename 编译器附加的源码路径。
/// @return 可用于日志前缀的模块名视图。
/// @note 路径解析委托给共享实现，保证终端与文件日志模块名一致。
std::string_view ColorfulFormatter::extract_module_name(
    const char* filename) const
{
    return extractModuleNameFromPath(filename);
}

void ColorfulFormatter::format(const spdlog::details::log_msg& msg,
                               spdlog::memory_buf_t&           dest)
{
    // dest 由 spdlog 管理，本函数只追加当前消息，不清除既有内容。
    // 将时间点拆成整秒与毫秒余量，确保两个显示字段来源一致。
    const auto time_since_epoch = msg.time.time_since_epoch();
    const auto sec =
        std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        time_since_epoch - sec);

    // 日志面向本机诊断，因此采用本地时区而非 UTC。
    const std::time_t t_c = std::chrono::system_clock::to_time_t(msg.time);
    std::tm           tm_buf;

#ifdef _WIN32
    // 两个平台均使用可重入接口，避免 std::localtime 的共享状态。
    localtime_s(&tm_buf, &t_c);
#else
    localtime_r(&t_c, &tm_buf);
#endif
    // 模块名由源码路径派生，颜色固定以区别于动态级别颜色。
    std::string_view module_name  = extract_module_name(msg.source.filename);
    const char*      module_color = "\033[35;1m";  // 模块名使用亮洋红色。

    // 主要前缀结构固定为 [时间] [级别/模块/函数]，便于快速定位来源。
    spdlog::fmt_lib::format_to(
        std::back_inserter(dest),
        "\033[40m[\033[36;1m{:%Y-%m-%d %H:%M:%S}.{:03d}\033[22m\033[37m] "
        "[\033[{};1m{}\033[37;22m/{}{}\033[37;22m/\033[32;1m{}\033[37;22m]",
        tm_buf,
        millis.count(),
        // 日志级别的颜色和文本必须相邻传入，保持格式串占位顺序。
        get_color(msg.level),
        spdlog::level::to_string_view(msg.level),
        // 模块使用独立颜色，避免与严重性视觉编码混淆。
        module_color,
        module_name,
        // 缺少函数名时输出稳定占位，保证前缀结构不变。
        msg.source.funcname ? msg.source.funcname : "unknown");

    // 正文沿用级别颜色，结尾分支负责恢复终端颜色状态。
    spdlog::fmt_lib::format_to(std::back_inserter(dest),
                               ": \033[{}m{}",
                               get_color(msg.level),
                               msg.payload);
    // debug 与 error 以上级别携带文件行号，兼顾定位能力和常规可读性。
    if ( msg.level == spdlog::level::debug ||
         msg.level >= spdlog::level::err ) {
        spdlog::fmt_lib::format_to(
            std::back_inserter(dest),
            " \033[37m(\033[35m{}\033[37m:\033[35m{}\033[37m)\033[0m\n",
            msg.source.filename,
            msg.source.line);
    } else {
        // 无源码后缀的消息仍必须重置 ANSI 状态并独占一行。
        spdlog::fmt_lib::format_to(std::back_inserter(dest), "\033[0m\n");
    }
}

/// @brief 克隆无状态的彩色日志格式器。
/// @return 独立 formatter 实例。
std::unique_ptr<spdlog::formatter> ColorfulFormatter::clone() const
{
    return std::make_unique<ColorfulFormatter>();
}

/// @brief 将 spdlog 日志级别映射为 ANSI SGR 前景色代码。
/// @param level 待映射的日志级别。
/// @return 静态颜色代码；未知级别返回重置代码。
const char* ColorfulFormatter::get_color(spdlog::level::level_enum level) const
{
    // 严重性使用从中性到醒目的固定配色，critical 额外启用高亮。
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

/// @brief 当前进程注册的共享日志器实例。
std::shared_ptr<spdlog::logger> XLogger::logger;

/// @brief 初始化终端、全量文件和错误文件三个日志输出端。
/// @param name 注册到 spdlog 全局表的日志器名称。
/// @note 调用方应保证进程生命周期内只初始化一次或先完成关闭。
/// @note 文件路径降级不会阻止终端日志器建立。
void XLogger::init(const char* name)
{
    // 路径解析在创建 sink 前完成，三个输出端共享同一次运行的目录。
    const std::filesystem::path logDirectory = resolveWritableLogDirectory();

    // 两个文件复用时间戳，使全量日志与错误日志可以直接配对。
    const std::string     timestamp  = makeLogTimestamp();
    std::filesystem::path allLogPath = logDirectory;
    allLogPath /= "mmm-" + timestamp + ".log";
    std::filesystem::path errorLogPath = logDirectory;
    errorLogPath /= "mmm-error-" + timestamp + ".log";

    // 三个 sink 分别承担交互输出、完整留档和错误聚合。
    auto console_sink    = std::make_shared<PlainConsoleSink>();
    auto file_all_sink   = std::make_shared<PlainFileSink>(allLogPath);
    auto file_error_sink = std::make_shared<PlainFileSink>(errorLogPath);

    // 文件全量从 debug 开始，终端保留 trace，错误文件仅接收 err 以上。
    console_sink->set_level(spdlog::level::trace);
    file_all_sink->set_level(spdlog::level::debug);
    file_error_sink->set_level(spdlog::level::err);

    // 组合 logger 保证同一条消息按各 sink 阈值分发。
    logger = std::make_shared<spdlog::logger>(
        name,
        spdlog::sinks_init_list{
            console_sink, file_all_sink, file_error_sink });

    // logger 自身先放行所有项目级别，再由各 sink 做二次过滤。
    logger->set_level(spdlog::level::trace);

    // trace 即时刷新确保崩溃前的最后一条诊断尽量落盘。
    logger->flush_on(spdlog::level::trace);

    // 同时注册命名实例和默认实例，兼容宏与显式 spdlog 调用。
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    XINFO("日志初始化完成，日志目录: {}",
          MMM::Config::pathToUtf8(logDirectory));
}

/// @brief 刷新并注销进程级日志器。
/// @note 重复关闭会直接返回，保持退出清理流程幂等。
void XLogger::shutdown()
{
    // 先转移本地所有权，防止后续调用继续通过静态成员访问旧实例。
    auto activeLogger = std::move(logger);
    if ( !activeLogger ) return;

    // 显式刷新后再关闭 spdlog 注册表，确保文件尾部完整写出。
    activeLogger->flush();
    spdlog::shutdown();
    activeLogger.reset();
}

/// @brief 恢复全部日志级别输出。
/// @pre init 已成功建立进程级日志器。
void XLogger::enable()
{
    logger->set_level(spdlog::level::trace);
}

/// @brief 临时关闭日志输出而不销毁 sink。
/// @pre init 已成功建立进程级日志器。
void XLogger::disable()
{
    logger->set_level(spdlog::level::off);
}

/// @brief 更新进程级 logger 的最低日志级别。
/// @param level 新的过滤阈值。
/// @pre init 已成功建立进程级日志器。
void XLogger::setlevel(spdlog::level::level_enum level)
{
    // 只调整 logger 总阈值，各 sink 的细分阈值保持初始化配置。
    logger->set_level(level);
}
