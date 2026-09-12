#include "network/UpdateChecker.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmmversion.h"
#include "network/AssetSyncService.h"
#include "runtime/AppThreadPool.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <fmt/format.h>
#include <ice/thread/ThreadPool.hpp>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
// clang-format off
#    include <windows.h>
#    include <shellapi.h>
// clang-format on
#elif defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <unistd.h>
#else
#    include <unistd.h>
#endif

using json = nlohmann::json;

namespace MMM::Network
{

namespace
{

/// @brief 将 libcurl 响应正文追加到内存字符串。
/// @details
/// libcurl 可能把一次响应拆成多次回调，因此必须追加而非覆盖。userp 由调用点
/// 保证指向任务生命周期内有效的 std::string；返回消费字节数通知 libcurl
/// 当前分片已完整处理。
/// @param contents 当前收到的原始字节起点。
/// @param size 单个元素的字节数。
/// @param nmemb 当前分片的元素数量。
/// @param userp 调用方提供的响应字符串。
/// @return 已追加的总字节数。
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    // libcurl 以 size * nmemb 定义本次有效范围，正文不保证以零结尾。
    size_t totalSize = size * nmemb;
    auto*  str       = static_cast<std::string*>(userp);
    // 按显式长度追加，允许 JSON 正文分片边界落在任意字节位置。
    str->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

/// @brief 将 libcurl 下载分片写入已打开的二进制文件。
/// @details
/// FILE 句柄由下载任务负责打开和关闭，本回调只转交给 fwrite。直接返回写入的
/// 元素数，使短写被 libcurl 识别为
/// CURLE_WRITE_ERROR，而不是继续发布不完整文件。
/// @param contents 当前收到的原始字节起点。
/// @param size 单个元素的字节数。
/// @param nmemb 当前分片的元素数量。
/// @param userp 调用方提供的 FILE 句柄。
/// @return fwrite 实际写入的元素数量。
size_t fileWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    return fwrite(contents, size, nmemb, static_cast<FILE*>(userp));
}

/// @brief 以二进制写入模式打开更新临时文件。
/// @details
/// Windows 原生和 MinGW 路径使用宽字符 API，避免 UTF-8 更新目录经本地代码页
/// 损坏；POSIX 平台可直接传递原生 path 字节。所有分支均使用 `wb`，新下载会
/// 截断同名旧临时文件且不执行文本换行转换。
/// @param path 需要创建或覆盖的文件路径。
/// @return 打开成功时返回文件句柄，失败时返回空指针。
FILE* openBinaryWriteFile(const std::filesystem::path& path)
{
#if defined(_WIN32) && defined(_MSC_VER) && !defined(__MINGW32__) && \
    !defined(__MINGW64__)
    // MSVC 安全版本通过输出参数返回句柄，并用错误码表示打开失败。
    FILE* file = nullptr;
    if ( _wfopen_s(&file, path.wstring().c_str(), L"wb") != 0 ) {
        return nullptr;
    }
    return file;
#elif defined(_WIN32)
    // MinGW 提供标准宽路径 _wfopen，保持与 Windows 文件名编码一致。
    return _wfopen(path.wstring().c_str(), L"wb");
#else
    // POSIX std::filesystem::path::c_str 可直接交给 C 文件 API。
    return fopen(path.c_str(), "wb");
#endif
}

/// @brief 写入更新启动失败原因。
/// @details errorMessage 是可选的 UI 诊断出口；日志由具体失败位置负责记录，
/// 此辅助函数不重复输出，也不会在调用方不关心详情时分配额外目标对象。
/// @param errorMessage 可选的错误文本接收位置。
/// @param message 要保存的稳定诊断文本。
void writeRestartError(std::string* errorMessage, const std::string& message)
{
    if ( errorMessage ) {
        *errorMessage = message;
    }
}

/// @brief 检查路径是否指向普通文件。
/// @details
/// 更新包和更新器都必须是普通文件，目录、符号错误或查询失败均不能继续启动。
/// 使用 error_code 避免文件系统异常，并同时把带 UTF-8 路径的详情写入可选返回值
/// 与项目日志，便于 UI 提示和日志诊断保持一致。
/// @param path 待验证的更新包或更新器路径。
/// @param errorMessage 可选的错误文本接收位置。
/// @param label 用于区分文件角色的英文诊断标签。
/// @return 路径存在且类型为普通文件时返回 true。
bool hasRegularFile(const std::filesystem::path& path,
                    std::string* errorMessage, const char* label)
{
    // is_regular_file 同时覆盖不存在和类型不符，error_code 保留查询故障。
    std::error_code filesystemError;
    if ( !std::filesystem::is_regular_file(path, filesystemError) ||
         filesystemError ) {
        const std::string message =
            fmt::format("{} not found: {}", label, Config::pathToUtf8(path));
        // 返回错误和日志使用同一文本，避免两条诊断对实际路径描述不一致。
        writeRestartError(errorMessage, message);
        XERROR("UpdateChecker: {}", message);
        return false;
    }
    return true;
}

#if !defined(_WIN32)
/// @brief 确保更新器文件在 POSIX 平台有执行权限。
/// @details
/// 下载保存的新文件通常没有执行位。函数先保留现有权限，仅在 owner/group/others
/// 三个执行位均缺失时追加执行权限，不覆盖读写位或其他模式。所有 status 与
/// permissions 错误通过 error_code 显式处理。
/// @param path 已下载或随程序安装的更新器文件。
/// @param errorMessage 可选的错误文本接收位置。
/// @return 已具备任一执行位或成功追加执行位时返回 true。
bool ensureExecutablePermission(const std::filesystem::path& path,
                                std::string*                 errorMessage)
{
    // 先读取当前模式，避免无条件 chmod 改变可执行文件已有权限约束。
    std::error_code statusError;
    const auto      status = std::filesystem::status(path, statusError);
    if ( statusError ) {
        const std::string message = fmt::format(
            "Cannot read updater permissions: {}", statusError.message());
        writeRestartError(errorMessage, message);
        XERROR("UpdateChecker: {}", message);
        return false;
    }

    constexpr auto executableBits = std::filesystem::perms::owner_exec |
                                    std::filesystem::perms::group_exec |
                                    std::filesystem::perms::others_exec;
    // 任一执行位已存在即认为可启动，不扩大现有授权范围。
    if ( (status.permissions() & executableBits) !=
         std::filesystem::perms::none ) {
        return true;
    }

    std::error_code permissionError;
    // add 只补充执行位，保持源文件的读写权限和平台特定位不变。
    std::filesystem::permissions(path,
                                 executableBits,
                                 std::filesystem::perm_options::add,
                                 permissionError);
    if ( permissionError ) {
        const std::string message = fmt::format(
            "Failed to mark updater executable: {}", permissionError.message());
        writeRestartError(errorMessage, message);
        XERROR("UpdateChecker: {}", message);
        return false;
    }
    return true;
}
#endif

#if defined(__APPLE__)
/// @brief 从主程序 Mach-O 路径定位所属 macOS App bundle。
/// @details
/// 主可执行文件通常位于 `.app/Contents/MacOS`，更新器需要替换完整 bundle 而非
/// 单个 Mach-O。函数从可执行文件父目录逐级向上，返回首个扩展名为 `.app` 的
/// 祖先；抵达文件系统根仍未找到时报告明确错误。
/// @param executablePath 当前主程序路径。
/// @param errorMessage 定位失败时写入错误信息，可为空。
/// @return 成功时返回 .app 根路径，否则返回空路径。
std::filesystem::path applicationBundlePath(
    const std::filesystem::path& executablePath, std::string* errorMessage)
{
    // 从父目录开始，避免把名称异常的可执行文件本身误判为 bundle。
    std::filesystem::path current = executablePath.parent_path();
    while ( !current.empty() ) {
        if ( current.extension() == ".app" ) {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        // 根目录的 parent 等于自身，显式终止避免无限循环。
        if ( parent == current ) break;
        current = parent;
    }

    const std::string message =
        fmt::format("Executable is not inside an app bundle: {}",
                    Config::pathToUtf8(executablePath));
    writeRestartError(errorMessage, message);
    XERROR("UpdateChecker: {}", message);
    return {};
}
#endif

/// @brief 获取更新成功标记路径。
/// @details
/// 更新器在替换完成后写入该标记，下一次主程序启动读取并删除。macOS bundle
/// 本身会被整体替换且安装目录可能不可写，因此标记放系统临时目录；其他平台
/// 放在可执行文件旁，天然区分不同安装副本。
/// @param executablePath 当前主程序路径。
/// @return 标记文件路径；临时目录不可用时返回空路径。
std::filesystem::path updateSuccessMarkerPath(
    const std::filesystem::path& executablePath)
{
#if defined(__APPLE__)
    // 查询失败时返回空路径，调用方不会对未解析目标执行 exists/remove。
    std::error_code tempError;
    const auto      tempPath = std::filesystem::temp_directory_path(tempError);
    if ( tempError ) return {};
    return tempPath / "MusicMapMaker-Next.mm_update_success";
#else
    // Windows 与 Linux 更新器和主程序共享安装目录内的固定隐藏标记。
    return executablePath.parent_path() / ".mm_update_success";
#endif
}

/// @brief 从 JSON 对象读取字符串字段。
/// @details
/// 更新清单属于远端不可信输入；缺少字段、父值非对象或字段类型不符都统一返回
/// 空字符串，由上层根据字段是否必需决定错误状态。get_ref 避免不必要的临时
/// JSON 转换，返回值仍复制到 UpdateInfo 以脱离解析树生命周期。
/// @param object JSON 对象。
/// @param key 字段名。
/// @return 字段缺失或类型错误时返回空字符串。
std::string jsonString(const json& object, const char* key)
{
    // 先验证父类型，防止对数组或标量执行对象查找产生错误语义。
    if ( !object.is_object() ) return {};
    auto it = object.find(key);
    if ( it == object.end() || !it->is_string() ) return {};
    return it->get_ref<const std::string&>();
}

/// @brief 从 JSON 对象读取非负字节数字段。
/// @details
/// JSON 数字可能以整数或浮点形式出现，统一读取 double 后拒绝 NaN、无穷、负值
/// 和超过 int64_t 的范围。无效值返回零；下载逻辑把它视为未知大小，而不会执行
/// 未定义的窄化转换。
/// @param object JSON 对象。
/// @param key 字段名。
/// @return 字段缺失或类型错误时返回 0。
int64_t jsonByteSize(const json& object, const char* key)
{
    if ( !object.is_object() ) return 0;
    auto it = object.find(key);
    if ( it == object.end() || !it->is_number() ) return 0;
    const double value = it->get<double>();
    // 在强制转换前完成有限性和范围检查，避免浮点到整数越界。
    if ( !std::isfinite(value) || value < 0.0 ||
         value > static_cast<double>(std::numeric_limits<int64_t>::max()) ) {
        return 0;
    }
    return static_cast<int64_t>(value);
}

}  // namespace

/// @brief 规范化远端清单中的 SHA-256 摘要文本。
/// @details
/// 接受可选且大小写不敏感的 `sha256:` 前缀，正文必须恰好为 64 个十六进制
/// 字符。成功结果统一转换为小写，便于与 AssetSyncService 生成的摘要直接比较；
/// 任意长度或字符错误均返回空字符串，禁止跳过校验继续发布下载。
/// @par 接受形式
/// - 纯 64 位十六进制摘要。
/// - 带 `sha256:` 或大小写变体前缀的摘要。
/// - 十六进制正文允许输入大写，输出始终为小写。
/// @par 拒绝形式
/// - 摘要长度不足或超过 64 位。
/// - 正文包含十六进制集合之外的字符。
/// @param value 远端清单提供的摘要文本。
/// @return 规范化小写摘要；格式非法时返回空字符串。
std::string UpdateChecker::normalizeSha256(std::string value)
{
    // 前缀比较逐字符转小写，既接受 sha256: 也接受 SHA256:。
    constexpr std::string_view prefix = "sha256:";
    if ( value.size() >= prefix.size() &&
         std::equal(prefix.begin(),
                    prefix.end(),
                    value.begin(),
                    [](char lhs, char rhs) {
                        return std::tolower(static_cast<unsigned char>(lhs)) ==
                               std::tolower(static_cast<unsigned char>(rhs));
                    }) ) {
        value.erase(0, prefix.size());
    }

    // SHA-256 固定为 32 字节，即 64 个十六进制字符。
    if ( value.size() != 64 ) return {};
    // 先统一大小写，再以最小 ASCII 集合验证每个摘要字符。
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    const bool valid = std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
    return valid ? value : std::string{};
}

/// @brief 销毁检查器前取消并等待全部后台任务退出。
/// @details 后台 lambda 捕获 this，析构必须等待它们停止访问成员后才能释放对象。
UpdateChecker::~UpdateChecker()
{
    cancelAndJoinWorkers();
}

/// @brief 请求取消并回收版本检查和下载任务。
/// @details
/// 取消标志使用 relaxed 即可，因为它只传递“尽快停止”提示，不承担其他数据的
/// 发布同步；UpdateInfo 仍由互斥量保护。future::wait 保证捕获 this 的任务已
/// 完全结束，随后重置 future 允许下一次 checkAsync 或 downloadAsync 启动。
/// @par 回收顺序
/// - 先设置取消标志，使两个任务都能观察停止请求。
/// - 等待版本检查 future，结束网络请求与重试等待。
/// - 等待下载 future，结束文件回调与摘要计算。
/// - 最后清空 future，避免下次任务误判旧句柄仍有效。
/// @warning 本函数会等待正在执行的 libcurl/文件任务，仅用于析构或低频重启任务。
void UpdateChecker::cancelAndJoinWorkers()
{
    // 先发布取消请求，让 libcurl 进度回调和重试等待尽快结束。
    m_cancelRequested.store(true, std::memory_order_relaxed);

    // 两类任务理论上不会并行启动，但分别检查 future 保持生命周期自洽。
    if ( m_checkFuture.valid() ) {
        m_checkFuture.wait();
        m_checkFuture = std::future<void>{};
    }
    if ( m_downloadFuture.valid() ) {
        m_downloadFuture.wait();
        m_downloadFuture = std::future<void>{};
    }
}

/// @brief 查询后台任务是否收到取消请求。
/// @return 请求取消时返回 true。
/// @note relaxed 读取只观察单一原子标志，不用于同步 UpdateInfo 内容。
bool UpdateChecker::isCancelRequested() const
{
    return m_cancelRequested.load(std::memory_order_relaxed);
}

/// @brief 以可取消的小步等待实现网络重试退避。
/// @details
/// 一秒重试间隔被拆成最多 50 ms 的片段，使析构或新任务启动不会被完整退避
/// 窗口阻塞。该函数只在后台版本检查线程执行，不位于 UI 或渲染调用链。
/// @param duration 调用方希望等待的总退避时间。
/// @return 等待完整结束且期间未取消时返回 true。
bool UpdateChecker::waitRetryDelay(std::chrono::milliseconds duration) const
{
    // 50 ms 是取消响应粒度，不改变总退避时间。
    constexpr auto step   = std::chrono::milliseconds(50);
    auto           waited = std::chrono::milliseconds(0);
    while ( waited < duration ) {
        if ( isCancelRequested() ) {
            return false;
        }

        const auto remaining = duration - waited;
        // 最后一步只等待剩余时长，避免超过调用方指定窗口。
        const auto sleepTime = remaining < step ? remaining : step;
        std::this_thread::sleep_for(sleepTime);
        waited += sleepTime;
    }
    return !isCancelRequested();
}

/// @brief 获取当前更新状态的线程安全快照。
/// @details 返回值复制使 UI 在释放锁后读取字段，不暴露后台线程正在修改的对象。
/// @return 当前 UpdateInfo 的一致副本。
UpdateInfo UpdateChecker::getInfo() const
{
    std::lock_guard<std::mutex> lock(m_infoMutex);
    return m_info;
}

/// @brief 从包含前缀的版本文本中解析 major、minor、patch。
/// @details
/// 接受 `v1.2` 和 `v1.2.3`，也允许前方带渠道文本，例如 `gamma-v1.2.3`。
/// 缺失 patch 按零处理；任一数字超出 int 或转换不完整时清零输出并失败。
/// @par 解析示例
/// - `v1.2` 解析为 1、2、0。
/// - `v1.2.3` 解析为 1、2、3。
/// - `gamma-v1.2.3` 通过搜索得到同一数值版本。
/// - 缺少 `v` 或数字段时失败。
/// - 超出 int 表示范围的段由 from_chars 拒绝。
/// @param verStr 待搜索的版本文本。
/// @param major 接收主版本号。
/// @param minor 接收次版本号。
/// @param patch 接收补丁版本号。
/// @return 找到且三个数值均可安全解析时返回 true。
bool UpdateChecker::parseVersion(const std::string& verStr, int& major,
                                 int& minor, int& patch)
{
    // 先清零所有输出，保证任何失败路径都不会泄漏调用方旧值。
    major = minor = patch = 0;

    // regex_search 允许渠道前缀，表达式只捕获 v 后的二段或三段数字。
    static const std::regex versionRegex(R"(v(\d+)\.(\d+)(?:\.(\d+))?)",
                                         std::regex::ECMAScript);
    std::smatch             match;
    if ( std::regex_search(verStr, match, versionRegex) && match.size() >= 3 ) {
        // from_chars 不分配、不依赖 locale，并能显式检查是否完整消费。
        auto parseInt = [](const std::ssub_match& token, int& value) -> bool {
            const std::string text = token.str();
            const auto [ptr, ec] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            return ec == std::errc{} && ptr == text.data() + text.size();
        };

        if ( !parseInt(match[1], major) || !parseInt(match[2], minor) ) {
            // 任一必需段失败时恢复一致的全零输出。
            major = minor = patch = 0;
            return false;
        }
        if ( match.size() >= 4 && match[3].matched ) {
            // patch 是可选段；存在时必须与其他段遵守相同整数约束。
            if ( !parseInt(match[3], patch) ) {
                major = minor = patch = 0;
                return false;
            }
        }
        return true;
    }
    return false;
}

/// @brief 按语义版本三段数值判断远端版本是否更新。
/// @details
/// 两侧均通过 parseVersion 后依次比较 major、minor、patch；渠道前缀和其他文本
/// 不参与排序。解析失败时保守返回 false，避免用未知格式触发错误更新。
/// @param remote 远端清单版本文本。
/// @param local 当前程序版本文本。
/// @return 远端三个数值段按字典序严格更大时返回 true。
bool UpdateChecker::isNewer(const std::string& remote, const std::string& local)
{
    int rMajor, rMinor, rPatch;
    int lMajor, lMinor, lPatch;

    bool rOk = parseVersion(remote, rMajor, rMinor, rPatch);
    bool lOk = parseVersion(local, lMajor, lMinor, lPatch);

    if ( !rOk || !lOk ) {
        // 无法建立可靠数值顺序时，不把格式差异当作新版本。
        return false;
    }

    if ( rMajor != lMajor ) return rMajor > lMajor;
    if ( rMinor != lMinor ) return rMinor > lMinor;
    if ( rPatch != lPatch ) return rPatch > lPatch;
    return false;
}

/// @brief 使用平台默认浏览器打开更新页面。
/// @details Windows 交给 ShellExecute；macOS 与 Linux 调用系统提供的
/// open/xdg-open。 URL 来自应用定义的更新入口，调用方负责只传递受信地址。
/// @param url 要打开的完整网页地址。
void UpdateChecker::openUrlInBrowser(const std::string& url)
{
#if defined(_WIN32)
    // ShellExecute 使用系统关联，不需要应用知道具体浏览器路径。
    ShellExecuteA(
        nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    // macOS open 命令同步启动系统 URL 处理器。
    std::system(("open \"" + url + "\"").c_str());
#else
    // Linux 后台启动 xdg-open，避免命令进程占用当前调用线程。
    std::system(("xdg-open \"" + url + "\" &").c_str());
#endif
}

/// @brief 查询当前运行主程序的绝对路径。
/// @details
/// 各平台使用原生进程接口：Windows 模块路径、macOS dyld 路径和 Linux
/// `/proc/self/exe`。成功结果转换为 UTF-8；无法解析或规范化时返回空字符串。
/// @return 当前可执行文件 UTF-8 路径，失败时为空。
std::string UpdateChecker::currentExecutablePath()
{
#if defined(_WIN32)
    // 宽字符 API 保留安装路径中的非 ASCII 文件名。
    wchar_t bufW[MAX_PATH];
    DWORD   len = GetModuleFileNameW(nullptr, bufW, MAX_PATH);
    if ( len > 0 && len < MAX_PATH ) {
        std::filesystem::path p(bufW);
        return Config::pathToUtf8(p);
    }
    return "";
#elif defined(__APPLE__)
    // _NSGetExecutablePath 返回的路径可能含符号链接或相对片段，优先弱规范化。
    char     buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if ( _NSGetExecutablePath(buf, &size) == 0 ) {
        const std::filesystem::path rawPath = Config::utf8ToPath(buf);
        std::error_code             canonicalError;
        const auto                  canonicalPath =
            std::filesystem::weakly_canonical(rawPath, canonicalError);
        // 规范化失败仍返回原始 dyld 路径，保留后续诊断机会。
        return Config::pathToUtf8(canonicalError ? rawPath : canonicalPath);
    }
    return "";
#else
    // canonical 同时解析 procfs 符号链接并返回实际程序路径。
    std::error_code ec;
    auto            path = std::filesystem::canonical("/proc/self/exe", ec);
    if ( !ec ) return path.string();
    return "";
#endif
}

/// @brief 启动独立更新器替换当前程序并退出进程。
/// @details
/// 函数先解析当前可执行文件、下载包和更新器路径，再按平台确定替换目标。
/// macOS 更新完整 `.app` bundle 且强制使用下载到临时目录的新更新器；Windows
/// 和 Linux 在未显式给出更新器时使用主程序同目录的固定文件名。
/// 更新器接收“下载包、替换目标、当前进程 PID”三个参数，负责等待本进程退出、
/// 原子替换并重启。任一前置检查或子进程创建失败都返回详情且保持主程序运行。
/// 成功启动后本函数终止当前进程，通常不会返回。
/// @par 启动前置条件
/// - 必须能解析当前可执行文件路径。
/// - 下载包必须仍是普通文件。
/// - 替换目标必须来自当前安装位置而非远端输入。
/// - 更新器必须存在，POSIX 平台还必须具备执行权限。
/// - macOS 可执行文件必须位于可识别的 `.app` 祖先中。
/// @par 子进程参数契约
/// - 第一个参数是已验证下载包路径。
/// - 第二个参数是主程序或完整 App bundle 目标。
/// - 第三个参数是当前进程 PID。
/// - 更新器进程创建成功前不得结束主程序。
/// - 父进程不等待更新器完成替换，以便先释放目标文件。
/// @param downloadedFilePath 已完成校验的主程序更新包 UTF-8 路径。
/// @param updaterFilePath 可选的已完成校验更新器 UTF-8 路径。
/// @param errorMessage 可选的启动失败详情接收位置。
/// @return 启动失败时返回 false；成功路径在进程退出前语义上为 true。
bool UpdateChecker::applyUpdateAndRestart(const std::string& downloadedFilePath,
                                          const std::string& updaterFilePath,
                                          std::string*       errorMessage)
{
    // 替换目标必须从当前进程解析，不能信任下载清单提供本机安装路径。
    std::string exePath = currentExecutablePath();
    if ( exePath.empty() ) {
        constexpr const char* message = "Cannot determine executable path";
        writeRestartError(errorMessage, message);
        XERROR("UpdateChecker: {}", message);
        return false;
    }

    const std::filesystem::path downloadedPath =
        Config::utf8ToPath(downloadedFilePath);
    // 下载状态字段可能过期或被外部删除，启动前重新验证普通文件。
    if ( !hasRegularFile(downloadedPath, errorMessage, "Downloaded update") ) {
        return false;
    }

    const std::filesystem::path executablePath = Config::utf8ToPath(exePath);
    std::filesystem::path       updateTarget   = executablePath;
#if defined(__APPLE__)
    // macOS 签名和资源都属于 bundle，不能只覆盖 Contents/MacOS 下的二进制。
    updateTarget = applicationBundlePath(executablePath, errorMessage);
    if ( updateTarget.empty() ) return false;
#endif

    // 优先使用下载阶段明确记录且已经过摘要验证的更新器。
    std::filesystem::path updaterPath;
    if ( !updaterFilePath.empty() ) {
        updaterPath = Config::utf8ToPath(updaterFilePath);
    } else {
#if defined(__APPLE__)
        // bundle 内旧更新器会随目标一起替换，必须使用 bundle 外独立文件。
        constexpr const char* message =
            "A downloaded updater is required for macOS app updates";
        writeRestartError(errorMessage, message);
        XERROR("UpdateChecker: {}", message);
        return false;
#else
        // 其他平台允许使用安装目录随程序分发的更新器作为兼容回退。
        updaterPath = executablePath.parent_path();
#    if defined(_WIN32)
        updaterPath /= "MusicMapMaker-Updater.exe";
#    else
        updaterPath /= "MusicMapMaker-Updater";
#    endif
#endif
    }

    if ( !hasRegularFile(updaterPath, errorMessage, "Updater") ) {
        return false;
    }

#if !defined(_WIN32)
    // 新下载文件常缺少执行位，fork/exec 前确保内核允许启动。
    if ( !ensureExecutablePermission(updaterPath, errorMessage) ) {
        return false;
    }
#endif

    long pid = 0;
#if defined(_WIN32)
    // 更新器通过 PID 等待主程序释放正在占用的目标文件。
    pid = static_cast<long>(GetCurrentProcessId());
#else
    pid = static_cast<long>(getpid());
#endif

    XINFO("UpdateChecker: Launching updater: {} {} {} {}",
          Config::pathToUtf8(updaterPath),
          downloadedFilePath,
          Config::pathToUtf8(updateTarget),
          pid);

#if defined(_WIN32)
    // Windows 使用宽字符串拼接，保留下载和安装路径中的非 ASCII 字符。
    std::filesystem::path dlPath = Config::utf8ToPath(downloadedFilePath);
    std::wstring          cmdLine =
        L"\"" + updaterPath.wstring() + L"\" \"" + dlPath.wstring() + L"\" \"" +
        updateTarget.wstring() + L"\" " + std::to_wstring(pid);
    STARTUPINFOW        si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW 避免 GUI 更新流程弹出额外控制台窗口。
    if ( CreateProcessW(nullptr,
                        cmdLine.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &si,
                        &pi) ) {
        // 主程序不需要等待更新器，成功创建后立即关闭本地句柄。
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        const std::string message =
            fmt::format("Failed to launch updater (error={})", GetLastError());
        writeRestartError(errorMessage, message);
        XERROR("UpdateChecker: {}", message);
        return false;
    }
#else
    // POSIX fork 后父进程退出，子进程用 execl 替换为独立更新器。
    const std::string updateTargetPath = Config::pathToUtf8(updateTarget);
    pid_t             child            = fork();
    if ( child < 0 ) {
        constexpr const char* message = "Failed to fork updater process";
        writeRestartError(errorMessage, message);
        XERROR("UpdateChecker: {}", message);
        return false;
    }
    if ( child == 0 ) {
        // 子进程只构造 argv 并 exec，不返回主程序更新流程。
        std::string updater = Config::pathToUtf8(updaterPath);
        std::string pidStr  = std::to_string(pid);
        execl(updater.c_str(),
              updater.c_str(),
              downloadedFilePath.c_str(),
              updateTargetPath.c_str(),
              pidStr.c_str(),
              nullptr);
        _exit(1);
    }
#endif

    // 只有成功创建独立更新器后才结束当前进程，避免下载完成但无法替换时退出。
    XINFO("UpdateChecker: Exiting for update...");
#if defined(_WIN32)
    ExitProcess(0);
#else
    std::exit(0);
#endif
    return true;
}

/// @brief 检查并消费上一次更新成功标记。
/// @details
/// 启动时根据当前程序路径计算平台对应标记位置；文件存在即尝试删除，并向调用方
/// 报告本次启动由成功更新产生。不存在、路径解析失败或文件系统查询失败均返回
/// false。删除错误不会改变“标记曾存在”的结果，避免只读安装环境重复隐藏成功提示。
/// @return 启动时发现更新成功标记时返回 true。
bool UpdateChecker::checkStartupUpdateMarker()
{
    // 不知道当前安装位置时无法安全推导标记路径。
    std::string exePath = currentExecutablePath();
    if ( exePath.empty() ) return false;

    const std::filesystem::path markerPath =
        updateSuccessMarkerPath(Config::utf8ToPath(exePath));
    if ( markerPath.empty() ) return false;

    std::error_code markerError;
    // exists 的查询错误与不存在都不应报告成功更新。
    if ( !std::filesystem::exists(markerPath, markerError) || markerError ) {
        return false;
    }

    std::filesystem::remove(markerPath, markerError);
    // 标记存在事实已成立；清理失败留给后续启动再次观察。
    return true;
}

/// @brief 在应用线程池异步查询远端版本清单。
/// @details
/// 启动前取消旧检查或下载任务，重置取消标志，并把公开状态切换为 kChecking。
/// 后台任务最多请求三次固定官方 JSON 地址；网络错误之间以可取消的一秒退避，
/// 解析成功后按当前编译平台提取下载 URL、大小、摘要和可选更新器信息。
/// 相对 URL 只允许拼接到官方站点根，最终以语义版本比较写入 kUpdateFound 或
/// kUpToDate。所有 UpdateInfo 发布都由 m_infoMutex 保护。
/// @par 状态迁移
/// - 调用后同步进入 kChecking。
/// - 主动取消直接退出，不写入伪造的网络错误。
/// - 三次网络尝试全部失败后进入 kError。
/// - JSON 结构非法或缺少 version 时进入 kError。
/// - 远端版本严格更新时进入 kUpdateFound。
/// - 远端相等、更旧或不可判定时进入 kUpToDate。
/// @par 清单提取
/// - 只读取 `platforms[MMM_PLATFORM]` 节点。
/// - 主包与更新器 URL、摘要分别保存，不互相替代。
/// - 站内 `/...` 路径固定补全为官方 HTTPS 主机。
/// - 缺少可选展示字段不会使版本检查失败。
/// - 下载阶段负责判断当前平台所需制品字段是否齐全。
/// @par 并发约束
/// - 后台任务只通过捕获的 this 访问检查器，析构必须等待 future。
/// - 取消标志仅控制停止，不替代 m_infoMutex 的状态同步。
/// - responseBody 只属于当前工作线程，无需共享锁。
/// - CURL 句柄在同一工作线程创建、使用和销毁。
/// - 每次向 UI 发布的 UpdateInfo 都是一次完整值替换。
/// - 重试之间不会保留上次响应正文或部分解析字段。
/// @warning 会等待旧后台任务结束，仅应由低频用户操作或启动检查调用。
void UpdateChecker::checkAsync()
{
    // 同一检查器只允许一类后台任务存活，避免两个任务竞争状态和临时文件。
    cancelAndJoinWorkers();
    m_cancelRequested.store(false, std::memory_order_relaxed);

    auto* appThreadPool = Runtime::AppThreadPool::instance().get();
    // 全局线程池未初始化时同步发布错误，不能退回临时线程绕过生命周期管理。
    if ( !appThreadPool ) {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        m_info.status       = UpdateStatus::kError;
        m_info.errorMessage = "Runtime thread pool is not initialized";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        // UI 立即看到检查状态和当前编译版本，网络结果稍后覆盖其余字段。
        m_info.status         = UpdateStatus::kChecking;
        m_info.currentVersion = MMM_VERSION_STRING;
    }

    m_checkFuture = appThreadPool->enqueue([this]() {
        // 三次总尝试包含第一次请求，只有失败后两次会进入退避。
        int maxRetries = 3;
        int retries    = 0;

        while ( retries < maxRetries && !isCancelRequested() ) {
            // 每次重试构造独立结果，避免残留上一轮部分响应字段。
            UpdateInfo result;
            result.status         = UpdateStatus::kChecking;
            result.currentVersion = MMM_VERSION_STRING;

            CURL* curl = curl_easy_init();
            // libcurl 初始化失败无法通过重试改善，直接发布终态错误。
            if ( !curl ) {
                result.status       = UpdateStatus::kError;
                result.errorMessage = "Failed to initialize libcurl";
                {
                    std::lock_guard<std::mutex> lock(m_infoMutex);
                    m_info = result;
                }
                XERROR("UpdateChecker: {}", result.errorMessage);
                return;
            }

            std::string responseBody;
            // 更新检查固定访问项目官方 HTTPS 清单，不接受运行时任意地址。
            const char* checkUrl =
                "https://mmm.xiang233.top/download/check/check.json";

            curl_easy_setopt(curl, CURLOPT_URL, checkUrl);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            // 允许 CDN 或站点迁移重定向，并设置稳定 User-Agent 便于服务端诊断。
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(
                curl, CURLOPT_USERAGENT, "MusicMapMaker-UpdateChecker/1.0");
            curl_easy_setopt(
                curl,
                CURLOPT_XFERINFOFUNCTION,
                +[](void* clientp,
                    curl_off_t,
                    curl_off_t,
                    curl_off_t,
                    curl_off_t) -> int {
                    auto* checker = static_cast<UpdateChecker*>(clientp);
                    // 返回非零让 libcurl 尽快以取消错误结束阻塞传输。
                    return checker && checker->isCancelRequested() ? 1 : 0;
                });
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

            CURLcode res = curl_easy_perform(curl);

            // 即使传输失败也读取 HTTP 状态，日志可区分网络错误和服务端错误。
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_easy_cleanup(curl);

            if ( isCancelRequested() ) {
                // 主动取消不发布 kError，调用新任务的一方会立即写入下一状态。
                return;
            }

            if ( res != CURLE_OK || httpCode != 200 ) {
                retries++;
                if ( retries < maxRetries ) {
                    // 中间失败只记录警告，不覆盖 UI 当前 kChecking 状态。
                    XWARN(
                        "UpdateChecker: Check failed (res={}, code={}), "
                        "retrying... ({}/{})",
                        static_cast<int>(res),
                        httpCode,
                        retries,
                        maxRetries);
                    if ( !waitRetryDelay(std::chrono::seconds(1)) ) {
                        return;
                    }
                    continue;
                } else {
                    // 最终失败按 libcurl 或 HTTP 来源生成不同用户可见详情。
                    result.status = UpdateStatus::kError;
                    if ( res != CURLE_OK ) {
                        result.errorMessage = fmt::format(
                            "Network error: {}", curl_easy_strerror(res));
                    } else {
                        result.errorMessage =
                            fmt::format("HTTP error: {}", httpCode);
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_infoMutex);
                        m_info = result;
                    }
                    XERROR("UpdateChecker: {}", result.errorMessage);
                    return;
                }
            }

            json data = json::parse(responseBody, nullptr, false);
            // 禁用异常解析；语法错误由 discarded 值显式进入错误状态。
            if ( data.is_discarded() || !data.is_object() ) {
                result.status       = UpdateStatus::kError;
                result.errorMessage = "JSON parse error";
                XERROR("UpdateChecker: {}", result.errorMessage);
            } else {
                result.latestVersion = jsonString(data, "version");
                // 可选展示字段缺失时保留空值，不影响版本判断。
                result.changelog   = jsonString(data, "changelog");
                result.releaseDate = jsonString(data, "release_date");

                const char* platform = MMM_PLATFORM;
                // 只读取当前构建平台节点，避免错误下载其他平台制品。
                auto platformsIt = data.find("platforms");
                if ( platformsIt != data.end() && platformsIt->is_object() ) {
                    auto platformIt = platformsIt->find(platform);
                    if ( platformIt != platformsIt->end() &&
                         platformIt->is_object() ) {
                        const auto& plat    = *platformIt;
                        result.downloadUrl  = jsonString(plat, "url");
                        result.downloadSize = jsonByteSize(plat, "size");
                        result.checksum     = jsonString(plat, "checksum");

                        // 服务端可返回站内绝对路径，客户端固定补全官方 HTTPS
                        // 主机。
                        if ( !result.downloadUrl.empty() &&
                             result.downloadUrl[0] == '/' ) {
                            result.downloadUrl =
                                "https://mmm.xiang233.top" + result.downloadUrl;
                        }

                        // 更新器是平台制品的可选嵌套对象，与主包分别校验摘要。
                        auto updaterIt = plat.find("updater");
                        if ( updaterIt != plat.end() &&
                             updaterIt->is_object() ) {
                            result.updaterUrl = jsonString(*updaterIt, "url");
                            result.updaterChecksum =
                                jsonString(*updaterIt, "checksum");
                            if ( !result.updaterUrl.empty() &&
                                 result.updaterUrl[0] == '/' ) {
                                result.updaterUrl = "https://mmm.xiang233.top" +
                                                    result.updaterUrl;
                            }
                        }
                    }
                }

                if ( result.latestVersion.empty() ) {
                    // version 是唯一必需顶层字段，缺失时不能声明已是最新。
                    result.status       = UpdateStatus::kError;
                    result.errorMessage = "No version info in response";
                } else if ( isNewer(result.latestVersion,
                                    result.currentVersion) ) {
                    result.status = UpdateStatus::kUpdateFound;
                    XINFO("UpdateChecker: New version found: {} -> {}",
                          result.currentVersion,
                          result.latestVersion);
                } else {
                    result.status = UpdateStatus::kUpToDate;
                    XINFO("UpdateChecker: Already up to date ({})",
                          result.currentVersion);
                }
            }

            {
                // 一次性替换完整结果，UI 不会看到字段逐项更新的中间组合。
                std::lock_guard<std::mutex> lock(m_infoMutex);
                m_info = result;
            }
            return;
        }
    });
}

/// @brief 在应用线程池依次下载更新器和主程序更新包。
/// @details
/// 入口先取得版本检查阶段发布的 UpdateInfo 快照，并验证主包 URL；macOS 还强制
/// 要求主包和独立更新器都提供 URL 与 SHA-256。后台任务可选先下载更新器，
/// 完成摘要与 POSIX 执行权限校验后，再下载主包并持续发布字节数和进度。
/// 任一传输或校验失败都会删除对应不可信临时文件、保留稳定错误信息，且不会
/// 发布 kDownloaded。只有主包完整落盘并通过可用摘要校验后才写入最终路径。
/// @par 更新器阶段
/// - updaterUrl 为空时跳过下载并保留安装目录回退策略。
/// - HTTP 错误、短写和取消都不能保留部分更新器。
/// - 提供摘要时必须规范化成功且匹配实际文件。
/// - POSIX 文件必须在发布路径前具备执行权限。
/// - updaterFilePath 只在全部检查成功后写入共享状态。
/// @par 主包阶段
/// - 主包始终以二进制流式方式写入固定临时文件。
/// - 已知总长时进度回调同时更新字节数和比例。
/// - 未知总长时继续传输，不制造除零进度。
/// - HTTP 错误、取消或摘要失败都删除部分主包。
/// - kDownloaded 只与最终路径和 100% 进度一起发布。
/// @par 平台差异
/// - macOS 主包使用 zip 并强制要求两项摘要。
/// - Windows 更新器临时文件带 exe 扩展名。
/// - Linux 与 macOS 在下载后补充 POSIX 执行位。
/// - 非 macOS 兼容旧清单，可在摘要缺失时完成主包下载。
/// @par 生命周期
/// 所有 CURL 和 FILE 资源在进入错误出口前释放；任务捕获 initialInfo 副本，
/// 只通过互斥量发布进度和终态，不持有 UI 侧 UpdateInfo 引用。
/// @par 终态约束
/// - 主动取消不把状态改写为 kError。
/// - fail 只在任务仍有效时发布错误。
/// - 下载器制品成功不代表主包任务已经完成。
/// - 主包路径在摘要检查前始终保持线程局部。
/// - 进度一只与 kDownloaded 和有效下载路径一起发布。
/// - 失败路径不得暴露未校验临时文件供更新启动使用。
/// - 后台任务返回后 future 仍由检查器统一回收。
/// @warning 会等待旧后台任务结束，仅应从低频更新界面调用；实际下载在工作线程。
void UpdateChecker::downloadAsync()
{
    // 下载和检查共享状态与取消标志，启动前必须回收旧任务。
    cancelAndJoinWorkers();
    m_cancelRequested.store(false, std::memory_order_relaxed);

    auto* appThreadPool = Runtime::AppThreadPool::instance().get();
    // 线程池是应用统一后台生命周期边界，缺失时同步失败。
    if ( !appThreadPool ) {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        m_info.status       = UpdateStatus::kError;
        m_info.errorMessage = "Runtime thread pool is not initialized";
        return;
    }

    UpdateInfo initialInfo;
    {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        // 复制完整清单结果，后台任务不再依赖 UI 线程后续修改的 m_info。
        initialInfo = m_info;
        // 没有主包 URL 时不创建临时文件或后台任务。
        if ( initialInfo.downloadUrl.empty() ) {
            m_info.status       = UpdateStatus::kError;
            m_info.errorMessage = "No download URL";
            return;
        }
#if defined(__APPLE__)
        // macOS 替换整个 bundle，两个制品都必须具备可认证来源。
        if ( initialInfo.checksum.empty() ) {
            m_info.status       = UpdateStatus::kError;
            m_info.errorMessage = "No macOS app archive checksum";
            return;
        }
        if ( initialInfo.updaterUrl.empty() ) {
            m_info.status       = UpdateStatus::kError;
            m_info.errorMessage = "No macOS updater URL";
            return;
        }
        if ( initialInfo.updaterChecksum.empty() ) {
            m_info.status       = UpdateStatus::kError;
            m_info.errorMessage = "No macOS updater checksum";
            return;
        }
#endif

        // 后台任务入队前即更新状态，调用方下一帧可立即展示下载界面。
        m_info.status = UpdateStatus::kDownloading;
    }

    m_downloadFuture = appThreadPool->enqueue([this, initialInfo]() {
        // 统一失败出口在未取消时发布错误；取消属于生命周期事件而非下载故障。
        auto fail = [this](const std::string& message) {
            if ( isCancelRequested() ) {
                return;
            }
            {
                // 状态和详情在同一临界区提交，UI 不会看到 kError 配空旧文本。
                std::lock_guard<std::mutex> lock(m_infoMutex);
                m_info.status       = UpdateStatus::kError;
                m_info.errorMessage = message;
            }
            XERROR("UpdateChecker: {}", message);
        };

        UpdateInfo result = initialInfo;
        // 重置可能来自旧下载的进度字段，确保新任务从零开始。
        result.status           = UpdateStatus::kDownloading;
        result.downloadedBytes  = 0;
        result.downloadProgress = 0.0;
        {
            std::lock_guard<std::mutex> lock(m_infoMutex);
            // 完整发布初始进度，清除 m_info 中可能残留的旧下载路径。
            m_info = result;
        }

        // 第一阶段按需下载独立更新器；没有 URL 时使用安装目录内置更新器。
        if ( !result.updaterUrl.empty() ) {
            // 固定临时文件名由单任务约束保护，不会有同一检查器的并发写入。
            std::filesystem::path updaterTempPath =
                std::filesystem::temp_directory_path() /
#ifdef _WIN32
                "MusicMapMakerHelper.exe";
#else
                "MusicMapMaker_updater";
#endif

            FILE* uFile = openBinaryWriteFile(updaterTempPath);
            // 无法建立输出文件时尚未创建 curl 句柄，可直接失败。
            if ( !uFile ) {
                fail("Failed to create updater temp file");
                return;
            }

            CURL* uCurl = curl_easy_init();
            if ( !uCurl ) {
                // 文件句柄已取得，任何后续初始化失败都必须先关闭。
                fclose(uFile);
                fail("Failed to initialize libcurl for updater");
                return;
            }

            curl_easy_setopt(uCurl, CURLOPT_URL, result.updaterUrl.c_str());
            // 分片直接流入磁盘，避免把更新器完整保存在内存。
            curl_easy_setopt(uCurl, CURLOPT_WRITEFUNCTION, fileWriteCallback);
            curl_easy_setopt(uCurl, CURLOPT_WRITEDATA, uFile);
            curl_easy_setopt(uCurl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(uCurl, CURLOPT_FAILONERROR, 1L);
            // HTTP 4xx/5xx 转换为 curl 失败，不能把错误页当作可执行文件。
            curl_easy_setopt(
                uCurl, CURLOPT_USERAGENT, "MusicMapMaker-UpdateChecker/1.0");
            curl_easy_setopt(uCurl, CURLOPT_CONNECTTIMEOUT, 30L);
            // 更新器体积小，整体传输最多允许 60 秒。
            curl_easy_setopt(uCurl, CURLOPT_TIMEOUT, 60L);
            curl_easy_setopt(
                uCurl,
                CURLOPT_XFERINFOFUNCTION,
                +[](void* clientp,
                    curl_off_t,
                    curl_off_t,
                    curl_off_t,
                    curl_off_t) -> int {
                    auto* checker = static_cast<UpdateChecker*>(clientp);
                    // 更新器阶段不展示进度，回调仅承担取消检查。
                    return checker && checker->isCancelRequested() ? 1 : 0;
                });
            curl_easy_setopt(uCurl, CURLOPT_XFERINFODATA, this);
            curl_easy_setopt(uCurl, CURLOPT_NOPROGRESS, 0L);

            CURLcode uRes = curl_easy_perform(uCurl);

            // 传输返回后先关闭输出与 curl 资源，再进入任意失败分支。
            fclose(uFile);
            curl_easy_cleanup(uCurl);

            if ( uRes != CURLE_OK ) {
                // 网络错误或主动取消留下的部分更新器都不可信，统一删除。
                std::error_code removeError;
                std::filesystem::remove(updaterTempPath, removeError);
                if ( isCancelRequested() ) {
                    // 主动取消不覆盖新任务可能已写入的状态。
                    return;
                }
                fail(fmt::format("Updater download error: {}",
                                 curl_easy_strerror(uRes)));
                return;
            }

            if ( !result.updaterChecksum.empty() ) {
                // 远端摘要先规范化，再与文件实际 SHA-256 常量时间无关地比较。
                const std::string expectedUpdaterChecksum =
                    normalizeSha256(result.updaterChecksum);
                const std::string actualUpdaterChecksum =
                    AssetSyncService::sha256File(updaterTempPath);
                if ( expectedUpdaterChecksum.empty() ||
                     actualUpdaterChecksum.empty() ||
                     actualUpdaterChecksum != expectedUpdaterChecksum ) {
                    // 格式非法、哈希失败和摘要不匹配都禁止保留可执行制品。
                    std::error_code removeError;
                    std::filesystem::remove(updaterTempPath, removeError);
                    fail("Downloaded updater checksum verification failed");
                    return;
                }
            }

#if !defined(_WIN32)
            // POSIX 下载文件通常由 umask 创建为不可执行，启动前补充执行位。
            if ( !ensureExecutablePermission(updaterTempPath, nullptr) ) {
                std::error_code removeError;
                std::filesystem::remove(updaterTempPath, removeError);
                fail("Failed to mark updater executable");
                return;
            }
#endif

            // 只有传输、摘要和权限全部通过后才把路径发布给 UI/启动流程。
            result.updaterFilePath = Config::pathToUtf8(updaterTempPath);
            {
                std::lock_guard<std::mutex> lock(m_infoMutex);
                // 仅更新路径，保留主包阶段正在使用的 kDownloading 状态。
                m_info.updaterFilePath = result.updaterFilePath;
            }
            XINFO("UpdateChecker: Updater downloaded -> {}",
                  result.updaterFilePath);
        }

        // 第二阶段下载主程序；macOS 为 zip，其余平台为可供更新器替换的制品。
        std::filesystem::path mainTempPath =
            std::filesystem::temp_directory_path() /
#if defined(__APPLE__)
            "MusicMapMaker_update.zip";
#else
            "MusicMapMaker_update";
#endif

        FILE* mFile = openBinaryWriteFile(mainTempPath);
        // 主包文件创建失败不影响已验证更新器路径，但整体状态必须失败。
        if ( !mFile ) {
            fail("Failed to create main temp file");
            return;
        }

        CURL* mCurl = curl_easy_init();
        if ( !mCurl ) {
            fclose(mFile);
            fail("Failed to initialize libcurl for main program");
            return;
        }

        curl_easy_setopt(mCurl, CURLOPT_URL, result.downloadUrl.c_str());
        // 与更新器相同，主包以流式文件回调避免大体积内存缓冲。
        curl_easy_setopt(mCurl, CURLOPT_WRITEFUNCTION, fileWriteCallback);
        curl_easy_setopt(mCurl, CURLOPT_WRITEDATA, mFile);
        curl_easy_setopt(
            mCurl,
            CURLOPT_XFERINFOFUNCTION,
            +[](void*      clientp,
                curl_off_t dltotal,
                curl_off_t dlnow,
                curl_off_t /*ultotal*/,
                curl_off_t /*ulnow*/) -> int {
                auto* p = static_cast<UpdateChecker*>(clientp);
                // 空上下文或取消请求通过返回一中止 libcurl。
                if ( !p || p->isCancelRequested() ) {
                    return 1;
                }
                if ( dltotal > 0 ) {
                    // 服务端提供总长时，在同一锁内发布字节数与归一化进度。
                    std::lock_guard<std::mutex> lock(p->m_infoMutex);
                    p->m_info.downloadedBytes  = dlnow;
                    p->m_info.downloadProgress = static_cast<double>(dlnow) /
                                                 static_cast<double>(dltotal);
                }
                // 未知总长仍继续下载，只暂不更新比例。
                return 0;
            });
        curl_easy_setopt(mCurl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(mCurl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(mCurl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(mCurl, CURLOPT_FAILONERROR, 1L);
        // 主包允许较长整体超时，但连接建立仍限制为 30 秒。
        curl_easy_setopt(
            mCurl, CURLOPT_USERAGENT, "MusicMapMaker-UpdateChecker/1.0");
        curl_easy_setopt(mCurl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(mCurl, CURLOPT_TIMEOUT, 3600L);

        CURLcode mRes = curl_easy_perform(mCurl);

        // 文件和 curl 句柄在检查状态前关闭，确保摘要读取看到完整落盘内容。
        fclose(mFile);
        curl_easy_cleanup(mCurl);

        if ( mRes != CURLE_OK ) {
            // 包含取消在内的任何不完整主包都立即删除，不能留给启动流程。
            std::error_code removeError;
            std::filesystem::remove(mainTempPath, removeError);
            if ( isCancelRequested() ) {
                // 取消不发布错误，下一操作负责设置新的公开状态。
                return;
            }
            fail(fmt::format("Download error: {}", curl_easy_strerror(mRes)));
            return;
        }

        if ( !result.checksum.empty() ) {
            // 非 macOS 允许旧清单不提供摘要；提供时必须格式合法且完全匹配。
            const std::string expectedChecksum =
                normalizeSha256(result.checksum);
            const std::string actualChecksum =
                AssetSyncService::sha256File(mainTempPath);
            if ( expectedChecksum.empty() || actualChecksum.empty() ||
                 actualChecksum != expectedChecksum ) {
                // 摘要不可信时删除主包，避免用户稍后误触发应用。
                std::error_code removeError;
                std::filesystem::remove(mainTempPath, removeError);
                fail("Downloaded update checksum verification failed");
                return;
            }
        }

        result.downloadProgress = 1.0;
        // 只有完整成功后才暴露临时路径并切换为 kDownloaded。
        result.downloadedFilePath = Config::pathToUtf8(mainTempPath);
        result.status             = UpdateStatus::kDownloaded;
        {
            // 原子发布路径、最终进度、字节数和状态的一致快照。
            std::lock_guard<std::mutex> lock(m_infoMutex);
            m_info = result;
        }
        XINFO("UpdateChecker: Download complete -> {}",
              Config::pathToUtf8(mainTempPath));
    });
}

}  // namespace MMM::Network
