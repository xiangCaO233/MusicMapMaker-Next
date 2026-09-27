/// @file PGOProfiler.cpp
/// @brief PGO profile 写入 + curl 上传实现

#include "main/PGOProfiler.h"
#include "log/colorful-log.h"
#include "mmmversion.h"
#include "runtime/AppThreadPool.h"

#ifdef MMM_PGO_INSTRUMENT
#    include "pgo_upload_url.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <mutex>
#include <string>
#include <utility>

#include <cstdlib>

#ifdef MMM_PGO_INSTRUMENT
#    include <curl/curl.h>
#endif

// LLVM compiler-rt 提供的 PGO 运行时函数，由 -fprofile-instr-generate 链接。
#ifdef MMM_PGO_INSTRUMENT
extern "C" {
/// @brief 在首次写出前覆盖 compiler-rt 的 profile 目标模板。
void __llvm_profile_set_filename(const char*);
/// @brief 立即把当前进程累计的计数器写入 profraw 文件。
/// @return 写出成功时返回 0。
int __llvm_profile_write_file(void);
}
#endif

#ifdef MMM_PGO_INSTRUMENT
/// @brief 在 main 之前为 compiler-rt 安装安全的临时 profile 路径模板。
///
/// 全局对象构造早于普通初始化代码，可避免 instrumented 静态初始化和 DLL
/// 加载阶段先在当前工作目录创建 default.profraw。
struct EarlyPGOInitializer {
    /// @brief 尽早设置 LLVM_PROFILE_FILE；失败时保留 compiler-rt 默认行为。
    EarlyPGOInitializer()
    {
        // 使用 error_code 版本避免静态初始化期间传播文件系统异常。
        std::error_code ec;
        auto            dir = std::filesystem::temp_directory_path(ec);
        if ( ec ) return;
        // 独立子目录隔离多个应用的临时 profile，目录已存在也视为成功。
        dir /= "MusicMapMaker";
        std::filesystem::create_directories(dir, ec);

        // 时间戳区分连续启动，compiler-rt 占位符再区分模块和进程。
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        std::string path =
            (dir / ("mmm_pgo_" + std::string(MMM_VERSION_STRING) + "_" +
                    std::to_string(now) + "_%m_%p.profraw"))
                .string();

#    ifdef _WIN32
        // Windows 使用 CRT 环境变量接口，避免直接修改环境内存。
        _putenv_s("LLVM_PROFILE_FILE", path.c_str());
#    else
        // POSIX 覆盖可能继承的值，保证本次启动采用版本化临时路径。
        setenv("LLVM_PROFILE_FILE", path.c_str(), 1);
#    endif
    }
};

/// @brief 触发 main 前的 profile 路径初始化。
static EarlyPGOInitializer g_earlyPGOInit;
#endif

namespace MMM::Main
{

#ifdef MMM_PGO_INSTRUMENT
namespace
{

/// @brief initPGOProfiler 最终交给 compiler-rt 的本次输出路径。
std::string s_profilePath;
/// @brief 计算上传门槛所需的单调启动时间点。
std::chrono::steady_clock::time_point s_profileStartTime;
/// @brief profile 路径和起始时间是否已经完成初始化。
bool s_profileInitialized{ false };
/// @brief 防止同步与异步退出入口重复写出同一份计数器。
bool s_shutdownStarted{ false };
/// @brief 共享线程池中正在执行的异步上传结果句柄。
std::future<void> s_shutdownFuture;
/// @brief 保护退出进度、执行权标志和上传字节计数。
std::mutex s_progressMutex;
/// @brief 供退出界面读取的完整 PGO 进度快照。
PGOProfilerShutdownProgress s_shutdownProgress;

/// @brief 默认最短上传运行时长，单位秒。
constexpr long long kDefaultMinUploadRuntimeSeconds = 600;

/// @brief 构建 profile 文件路径:
/// %TEMP%/MusicMapMaker/mmm_pgo_<version>_<ts>.profraw
/// @return 含应用版本和启动时间戳的绝对临时路径。
std::string buildProfilePath()
{
    // 正式路径去掉 compiler-rt 的模块和 PID 占位符，每次 init 只写一个文件。
    auto dir = std::filesystem::temp_directory_path() / "MusicMapMaker";
    // 目录创建放在启动低频路径，确保后续运行时写出无需再准备父目录。
    std::filesystem::create_directories(dir);

    // system_clock 数值用于名称唯一性，不参与运行时长判断。
    auto now = std::chrono::system_clock::now().time_since_epoch().count();

    return (dir / ("mmm_pgo_" + std::string(MMM_VERSION_STRING) + "_" +
                   std::to_string(now) + ".profraw"))
        .string();
}

/// @brief 确定上传 URL，运行时环境变量可覆盖 CMake 编译期配置。
/// @return 当前进程应使用的上传地址；未配置时为空字符串。
std::string resolveUploadUrl()
{
    // 环境变量用于测试和临时部署，不需要重新生成构建配置头。
    const char* envUrl = std::getenv("MMM_PGO_UPLOAD_URL");
    if ( envUrl && envUrl[0] != '\0' ) return envUrl;
    // 没有非空覆盖时回落到构建产物内固定的发布服务地址。
    return MMM_PGO_UPLOAD_URL;
}

/// @brief 读取最短上传运行时长。
/// @return 非负秒数；缺失或无效覆盖时返回发布默认值。
long long resolveMinUploadRuntimeSeconds()
{
    // 环境入口便于开发验证短会话，正式用户默认过滤低价值样本。
    const char* envValue = std::getenv("MMM_PGO_MIN_RUNTIME_SECONDS");
    if ( !envValue || envValue[0] == '\0' ) {
        // 未覆盖时保持发布配置的十分钟门槛。
        return kDefaultMinUploadRuntimeSeconds;
    }

    char*     endPtr = nullptr;
    long long value  = std::strtoll(envValue, &endPtr, 10);
    // 至少要求读到一个数字并拒绝负阈值；尾部兼容 strtoll 的既有行为。
    if ( endPtr == envValue || value < 0 ) {
        return kDefaultMinUploadRuntimeSeconds;
    }
    return value;
}

/// @brief 计算本次 profile 收集运行时长。
/// @return 从初始化起经过的完整秒数。
long long elapsedRuntimeSeconds()
{
    // steady_clock 不受系统校时影响，门槛不会因墙上时间跳变而失真。
    const auto elapsed = std::chrono::steady_clock::now() - s_profileStartTime;
    return std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
}

/// @brief 更新 PGO 退出进度阶段。
/// @param stage 当前阶段。
/// @param message 状态说明。
/// @param finished 是否已结束。
void setShutdownStage(PGOProfilerShutdownStage stage, std::string message,
                      bool finished)
{
    // 阶段、文案和完成标志必须作为一个快照更新，UI 不观察混合状态。
    std::lock_guard<std::mutex> lock(s_progressMutex);
    s_shutdownProgress.stage          = stage;
    s_shutdownProgress.message        = std::move(message);
    s_shutdownProgress.finished       = finished;
    s_shutdownProgress.runtimeSeconds = elapsedRuntimeSeconds();
    // 每次刷新同时暴露当前阈值，使环境覆盖也能在退出界面准确显示。
    s_shutdownProgress.minRuntimeSeconds = resolveMinUploadRuntimeSeconds();
}

/// @brief 更新 PGO 上传字节进度。
/// @param uploadedBytes 已上传字节。
/// @param totalBytes 预计总字节。
void setUploadByteProgress(std::uint64_t uploadedBytes,
                           std::uint64_t totalBytes)
{
    // libcurl 回调运行在上传工作线程，所有读取方都通过同一互斥量取快照。
    std::lock_guard<std::mutex> lock(s_progressMutex);
    s_shutdownProgress.uploadedBytes = uploadedBytes;
    s_shutdownProgress.totalBytes    = totalBytes;
}

/// @brief 添加 curl multipart 文本字段。
/// @param mime 正在构建的 multipart 表单。
/// @param name 服务端识别的字段名。
/// @param value 在请求完成前保持有效的字段内容。
void addMimeTextPart(curl_mime* mime, const char* name,
                     const std::string& value)
{
    // MIME 对象拥有 part 生命周期，统一在请求结束后随 mime 释放。
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, name);
    curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
}

/// @brief 添加 curl multipart profile 文件字段。
/// @param mime 正在构建的 multipart 表单。
/// @param filePath 本次已写出的 profraw 文件路径。
void addMimeProfilePart(curl_mime* mime, const std::string& filePath)
{
    // 服务端只接收文件名元数据，实际内容由 filedata 从完整路径读取。
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "profile");
    curl_mime_filename(
        part, std::filesystem::path(filePath).filename().string().c_str());
    curl_mime_filedata(part, filePath.c_str());
}

/// @brief 判断当前运行时长是否足够上传 profile。
/// @param runtimeSeconds 本次收集到的完整运行秒数。
/// @return 达到当前最短门槛时返回 true。
bool shouldUploadProfile(long long runtimeSeconds)
{
    const long long minRuntimeSeconds = resolveMinUploadRuntimeSeconds();
    // 较长会话覆盖更多业务路径，才值得占用退出时间和上传带宽。
    if ( runtimeSeconds >= minRuntimeSeconds ) return true;

    // 跳过属于正常策略结果，使用信息级日志而非错误级告警。
    XINFO("PGO: runtime {}s is below upload threshold {}s; skipping upload",
          runtimeSeconds,
          minRuntimeSeconds);
    return false;
}

/// @brief libcurl 上传进度回调。
/// @param ultotal curl 报告的预计上传总字节数。
/// @param ulnow curl 报告的当前已上传字节数。
/// @return 返回 0 表示继续上传。
int uploadProgressCallback(void*, curl_off_t, curl_off_t, curl_off_t ultotal,
                           curl_off_t ulnow)
{
    // curl_off_t 可能为有符号类型，未知或异常负值统一映射到零。
    auto safeTotal =
        static_cast<std::uint64_t>(std::max<curl_off_t>(0, ultotal));
    auto safeNow = static_cast<std::uint64_t>(std::max<curl_off_t>(0, ulnow));
    // 某些传输阶段的瞬时计数可能超过估计总量，界面只展示有效范围。
    if ( safeTotal > 0 && safeNow > safeTotal ) {
        safeNow = safeTotal;
    }
    // 回调只更新内存快照，不在 curl 热回调里写日志或访问文件系统。
    setUploadByteProgress(safeNow, safeTotal);
    return 0;
}

/// @brief 在退出低频路径中通过 curl multipart POST 上传 profile 文件。
/// @param filePath profile 文件路径。
/// @param runtimeSeconds 本次运行时长。
/// @return 上传成功时返回 true。
bool uploadProfile(const std::string& filePath, long long runtimeSeconds)
{
    // URL 在真正创建 curl 句柄前解析，空配置可无副作用地跳过网络路径。
    std::string url = resolveUploadUrl();
    if ( url.empty() ) {
        // profile 已成功写到本地，缺少远端配置只影响自动上传。
        XINFO("PGO: profile saved to {} (no upload URL configured)", filePath);
        setShutdownStage(PGOProfilerShutdownStage::Skipped,
                         "PGO: no upload URL configured",
                         true);
        return false;
    }

    // 写出成功仍不代表文件可读，上传前验证存在性和非空大小。
    std::error_code ec;
    auto            fileSize = std::filesystem::file_size(filePath, ec);
    if ( ec || fileSize == 0 ) {
        // 区分查询失败和零长度文件，日志保留更具体的本地诊断。
        if ( ec )
            XERROR("PGO: cannot stat profile: {}", ec.message());
        else
            // 零长度通常表示计数器没有成功写出，同样禁止提交无效样本。
            XERROR("PGO: profile file is empty, skipping upload");
        setShutdownStage(PGOProfilerShutdownStage::Failed,
                         "PGO: profile file is not readable",
                         true);
        return false;
    }

    // 上传日志仅报告大小和运行时长，不暴露可能含本机目录的目标 URL。
    XINFO("PGO: uploading profile ({} KB, runtime {}s) ...",
          fileSize / 1024,
          runtimeSeconds);

    // 全局 curl 已在初始化阶段准备，这里只创建当前请求的 easy 句柄。
    auto curl = curl_easy_init();
    if ( !curl ) {
        // 句柄创建失败时没有需要释放的 MIME 对象，可直接发布失败终态。
        XERROR("PGO: curl_easy_init failed");
        setShutdownStage(PGOProfilerShutdownStage::Failed,
                         "PGO: curl_easy_init failed",
                         true);
        return false;
    }

    // 在组装表单前发布 Uploading，让退出界面能立即切换到上传状态。
    setShutdownStage(
        PGOProfilerShutdownStage::Uploading, "PGO: uploading profile", false);
    // 初始总量来自磁盘文件大小，后续由 curl 回调校正已传输字节。
    setUploadByteProgress(0, static_cast<std::uint64_t>(fileSize));

    // MIME 表单同时携带二进制 profile 和服务端筛选所需的上下文字段。
    auto mime = curl_mime_init(curl);
    addMimeProfilePart(mime, filePath);
    addMimeTextPart(mime, "runtime_seconds", std::to_string(runtimeSeconds));
    addMimeTextPart(mime, "app_version", MMM_VERSION_STRING);
    addMimeTextPart(mime, "platform", MMM_PLATFORM);

    // 请求配置保持在退出低频路径，30 秒超时限制异常网络的阻塞上界。
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MusicMapMaker-PGO/1.0");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, uploadProgressCallback);
    // 显式启用进度回调，否则 XFERINFOFUNCTION 不会收到周期更新。
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // easy_perform 在当前上传工作线程同步运行，不阻塞渲染主线程。
    CURLcode res = curl_easy_perform(curl);
    if ( res == CURLE_OK ) {
        // 完成态把已上传值钉到文件大小，修正最后一次回调未到满值的情况。
        XINFO("PGO: profile uploaded successfully");
        setUploadByteProgress(static_cast<std::uint64_t>(fileSize),
                              static_cast<std::uint64_t>(fileSize));
        setShutdownStage(PGOProfilerShutdownStage::Succeeded,
                         "PGO: profile uploaded successfully",
                         true);
    } else {
        // HTTP 错误因 FAILONERROR 也映射为 curl 失败并进入统一终止状态。
        XERROR("PGO: upload failed: {}", curl_easy_strerror(res));
        setShutdownStage(
            PGOProfilerShutdownStage::Failed, curl_easy_strerror(res), true);
    }

    // part 由 mime 统一拥有；先释放表单，再销毁引用表单的 easy 句柄。
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}

/// @brief 声明 PGO 退出流程开始，避免重复写出或上传。
/// @param uploadAllowed 用户配置是否允许提交本次 profile。
/// @return 本次调用成功取得执行权时返回 true。
bool claimShutdownStart(bool uploadAllowed)
{
    // 执行权和初始进度共用一把锁，同步、异步入口只能有一个胜出。
    std::lock_guard<std::mutex> lock(s_progressMutex);
    if ( s_shutdownStarted ) return false;

    // 取得执行权后重建快照，清除上一状态中可能残留的字节与错误信息。
    s_shutdownStarted                = true;
    s_shutdownProgress               = {};
    s_shutdownProgress.uploadAllowed = uploadAllowed;
    // 起始快照记录决策当下的会话长度，后续阶段更新会继续刷新。
    s_shutdownProgress.runtimeSeconds    = elapsedRuntimeSeconds();
    s_shutdownProgress.minRuntimeSeconds = resolveMinUploadRuntimeSeconds();
    // Idle 表示已接管退出流程但尚未进入 compiler-rt 写出阶段。
    s_shutdownProgress.stage    = PGOProfilerShutdownStage::Idle;
    s_shutdownProgress.finished = false;
    return true;
}

/// @brief 写出 profraw，并判断后续是否需要上传。
/// @param uploadAllowed 用户是否允许自动上传。
/// @return 需要执行上传时返回 true。
bool writeProfileAndShouldUpload(bool uploadAllowed)
{
    // 未初始化意味着没有可靠路径或起始时间，只能结束本轮退出流程。
    if ( !s_profileInitialized ) {
        setShutdownStage(PGOProfilerShutdownStage::Skipped,
                         "PGO: profiler is not initialized",
                         true);
        return false;
    }

    // 先发布 Writing，退出窗口不会把可能较慢的计数器落盘误认为无响应。
    setShutdownStage(
        PGOProfilerShutdownStage::Writing, "PGO: writing profile", false);

    // compiler-rt 在此把内存中的全部插桩计数器强制刷新到已设置路径。
    int ret = __llvm_profile_write_file();
    if ( ret != 0 ) {
        // compiler-rt 返回码不携带异常，直接转换为可展示的失败阶段。
        XERROR("PGO: __llvm_profile_write_file() failed (code {})", ret);
        setShutdownStage(PGOProfilerShutdownStage::Failed,
                         "PGO: failed to write profile",
                         true);
        return false;
    }

    // 写出完成后固定运行时长，后续判断和界面展示使用同一数值。
    const long long runtimeSeconds = elapsedRuntimeSeconds();
    {
        // 只在持锁区更新共享快照，不把磁盘或网络工作带入临界区。
        std::lock_guard<std::mutex> lock(s_progressMutex);
        s_shutdownProgress.runtimeSeconds = runtimeSeconds;
    }

    // 用户隐私设置优先于时长和服务配置，profraw 只保留在本地。
    if ( !uploadAllowed ) {
        XINFO("PGO: automatic profile upload disabled by user");
        setShutdownStage(PGOProfilerShutdownStage::Skipped,
                         "PGO: upload disabled by user",
                         true);
        return false;
    }

    // 过短运行样本覆盖不足，不进入上传阶段但仍保留本地文件。
    if ( !shouldUploadProfile(runtimeSeconds) ) {
        setShutdownStage(PGOProfilerShutdownStage::Skipped,
                         "PGO: runtime is below upload threshold",
                         true);
        return false;
    }

    // 空服务地址是受支持配置，退出流程应以 Skipped 正常收尾。
    if ( resolveUploadUrl().empty() ) {
        setShutdownStage(PGOProfilerShutdownStage::Skipped,
                         "PGO: no upload URL configured",
                         true);
        return false;
    }

    {
        // 仅真正进入网络阶段时标记 attempted，便于 UI 区分各种跳过原因。
        std::lock_guard<std::mutex> lock(s_progressMutex);
        s_shutdownProgress.uploadAttempted = true;
    }
    return true;
}

/// @brief 执行已经准备好的 PGO 上传。
/// @warning 退出低频路径：包含文件读取和同步网络请求，只能在后台任务中调用。
void uploadPreparedProfile()
{
    // 临近请求时重新取得运行时长，上传元数据覆盖完整应用会话。
    uploadProfile(s_profilePath, elapsedRuntimeSeconds());
}

}  // namespace
#endif

// 以下公共入口在非插桩构建中保持无副作用，调用方无需条件编译。

/// @brief 初始化 compiler-rt profile 路径、计时起点和 curl 全局状态。
/// @warning 应用启动低频路径：必须早于需要计入样本的主要业务流程调用。
void initPGOProfiler()
{
#ifdef MMM_PGO_INSTRUMENT
    // steady_clock 起点只服务时长门槛，输出文件名另用墙上时间生成。
    s_profileStartTime   = std::chrono::steady_clock::now();
    s_profilePath        = buildProfilePath();
    s_profileInitialized = true;
    // 路径字符串由静态状态持有，满足 compiler-rt 后续写出期间的生命周期。
    __llvm_profile_set_filename(s_profilePath.c_str());
    XINFO("PGO: instrumentation active → {}", s_profilePath);

    // 删除早期运行时可能在当前目录留下的空默认文件；失败不影响正式收集。
    std::error_code ec;
    std::filesystem::remove("default.profraw", ec);

    // curl 全局初始化固定在单线程启动阶段，异步上传只创建 easy 句柄。
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
}

/// @brief 同步写出 profile，并按配置和运行时长决定是否上传。
/// @param uploadAllowed 用户是否允许向配置服务提交 profile。
/// @warning 应用退出低频路径：上传会在调用线程同步等待网络完成。
void shutdownPGOProfiler(bool uploadAllowed)
{
#ifdef MMM_PGO_INSTRUMENT
    // 若其他入口已取得执行权，只等待其异步工作，禁止再次写出计数器。
    if ( !claimShutdownStart(uploadAllowed) ) {
        // 重复同步调用可能发生在异步退出钩子之后，等待同一 future 即可。
        waitForShutdownPGOProfiler();
        return;
    }

    // 跳过和失败状态已由准备函数发布，只有 true 才能进入网络请求。
    if ( writeProfileAndShouldUpload(uploadAllowed) ) {
        // 同步接口保留给没有可用图形退出窗口的调用路径。
        uploadPreparedProfile();
    }
#else
    // 非插桩构建保留相同 API，显式消费参数避免平台编译警告。
    (void)uploadAllowed;
#endif
}

/// @brief 写出 profile，并在确需上传时投递共享线程池任务。
/// @param uploadAllowed 用户是否允许向配置服务提交 profile。
/// @return 成功投递后台上传任务时返回 true。
/// @warning 应用退出低频路径：写出仍同步完成，只有网络上传转入后台。
bool beginShutdownPGOProfilerAsync(bool uploadAllowed)
{
#ifdef MMM_PGO_INSTRUMENT
    // false 可能表示已有流程或当前样本无需上传，进度快照提供具体原因。
    if ( !claimShutdownStart(uploadAllowed) ) return false;
    if ( !writeProfileAndShouldUpload(uploadAllowed) ) return false;

    {
        // 在任务投递前发布 Uploading，避免退出 UI 短暂回退到 Writing。
        std::lock_guard<std::mutex> lock(s_progressMutex);
        s_shutdownProgress.stage    = PGOProfilerShutdownStage::Uploading;
        s_shutdownProgress.message  = "PGO: uploading profile";
        s_shutdownProgress.finished = false;
    }

    // 上传复用应用级共享池，但在池已关闭时必须明确失败而非解引用空指针。
    auto* appThreadPool = Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) {
        // 退出上传不能临时创建第二个线程池，否则会破坏全局关闭顺序。
        setShutdownStage(PGOProfilerShutdownStage::Failed,
                         "PGO: runtime thread pool is not initialized",
                         true);
        return false;
    }
    // future 保留任务完成句柄，最终退出可在必要时等待网络清理结束。
    s_shutdownFuture =
        // 任务不捕获局部变量，只读取退出阶段保持有效的静态 profile 状态。
        appThreadPool->enqueue([]() { uploadPreparedProfile(); });
    return true;
#else
    // 普通构建没有 compiler-rt 数据，始终表示无需展示异步上传界面。
    (void)uploadAllowed;
    return false;
#endif
}

/// @brief 获取线程安全的 PGO 退出进度副本。
/// @return 当前阶段、运行时长、上传计数和完成标志组成的值快照。
PGOProfilerShutdownProgress getPGOProfilerShutdownProgress()
{
#ifdef MMM_PGO_INSTRUMENT
    // 返回值复制使调用方离开临界区后可安全绘制，不持有共享状态引用。
    std::lock_guard<std::mutex> lock(s_progressMutex);
    // 整体复制包括 std::string，使 UI 不依赖锁外静态存储。
    return s_shutdownProgress;
#else
    // 非插桩构建直接表现为已跳过且完成，退出状态机无需特殊分支。
    return PGOProfilerShutdownProgress{
        .stage    = PGOProfilerShutdownStage::Skipped,
        .finished = true,
    };
#endif
}

/// @brief 查询写出或上传流程是否已经进入终态。
/// @return Succeeded、Failed 或 Skipped 发布完成后返回 true。
bool isShutdownPGOProfilerFinished()
{
#ifdef MMM_PGO_INSTRUMENT
    // 查询与进度更新使用同一互斥量，避免读取数据竞争。
    std::lock_guard<std::mutex> lock(s_progressMutex);
    // finished 是阶段发布的终态位，不以 future.valid 推断业务完成。
    return s_shutdownProgress.finished;
#else
    // 没有插桩工作时退出流程天然完成。
    return true;
#endif
}

/// @brief 等待已投递的 PGO 后台上传任务结束。
/// @warning 应用退出低频路径：可能等待剩余网络超时，不得在渲染循环调用。
void waitForShutdownPGOProfiler()
{
#ifdef MMM_PGO_INSTRUMENT
    // valid 区分从未投递或已经移动的 future，避免无状态 wait。
    if ( s_shutdownFuture.valid() ) {
        // wait 不重新抛出任务异常；上传函数通过进度快照报告失败。
        s_shutdownFuture.wait();
    }
#endif
}

}  // namespace MMM::Main
