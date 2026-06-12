/// @file PGOProfiler.cpp
/// @brief PGO profile 写入 + curl 上传实现

#include "main/PGOProfiler.h"
#include "log/colorful-log.h"
#include "mmmversion.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <utility>

#include <cstdlib>

#ifdef MMM_PGO_INSTRUMENT
#    include <curl/curl.h>
#endif

// LLVM compiler-rt 提供的 PGO 运行时函数 (通过 -fprofile-instr-generate 链接)
#ifdef MMM_PGO_INSTRUMENT
extern "C" {
void __llvm_profile_set_filename(const char*);
int  __llvm_profile_write_file(void);
}
#endif

// 上传 URL — 由 CMake configure_file 生成，或运行时可被环境变量覆盖
#ifdef MMM_PGO_INSTRUMENT
#    if __has_include("pgo_upload_url.h")
#        include "pgo_upload_url.h"
#    endif

// 早期静态初始化器：利用全局静态变量的构造函数，抢在任何 instrumented 代码或
// DLL 加载运行前， 将环境变量 LLVM_PROFILE_FILE 设置为用户 TEMP
// 目录下的自定义路径。 这彻底避免了编译器/链接器默认在当前工作目录 (CWD) 生成
// default.profraw 的问题。
struct EarlyPGOInitializer {
    EarlyPGOInitializer()
    {
        std::error_code ec;
        auto            dir = std::filesystem::temp_directory_path(ec);
        if ( ec ) return;
        dir /= "MusicMapMaker";
        std::filesystem::create_directories(dir, ec);

        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        std::string path =
            (dir / ("mmm_pgo_" + std::string(MMM_VERSION_STRING) + "_" +
                    std::to_string(now) + "_%m_%p.profraw"))
                .string();

#    ifdef _WIN32
        _putenv_s("LLVM_PROFILE_FILE", path.c_str());
#    else
        setenv("LLVM_PROFILE_FILE", path.c_str(), 1);
#    endif
    }
};

static EarlyPGOInitializer g_earlyPGOInit;
#endif

namespace MMM::Main
{

#ifdef MMM_PGO_INSTRUMENT
namespace
{

std::string                           s_profilePath;
std::chrono::steady_clock::time_point s_profileStartTime;
bool                                  s_profileInitialized{ false };
bool                                  s_shutdownStarted{ false };
std::future<void>                     s_shutdownFuture;
std::mutex                            s_progressMutex;
PGOProfilerShutdownProgress           s_shutdownProgress;

/// @brief 默认 PGO profile 上传 URL。
constexpr const char* kDefaultPGOUploadUrl =
    "https://mmm.xiang233.top/api/performance/upload";

/// @brief 默认最短上传运行时长，单位秒。
constexpr long long kDefaultMinUploadRuntimeSeconds = 600;

/// @brief 构建 profile 文件路径:
/// %TEMP%/MusicMapMaker/mmm_pgo_<version>_<ts>.profraw
std::string buildProfilePath()
{
    auto dir = std::filesystem::temp_directory_path() / "MusicMapMaker";
    std::filesystem::create_directories(dir);

    auto now = std::chrono::system_clock::now().time_since_epoch().count();

    return (dir / ("mmm_pgo_" + std::string(MMM_VERSION_STRING) + "_" +
                   std::to_string(now) + ".profraw"))
        .string();
}

/// @brief 确定上传 URL: 环境变量 > 编译期 > 默认 API。
std::string resolveUploadUrl()
{
    const char* envUrl = std::getenv("MMM_PGO_UPLOAD_URL");
    if ( envUrl && envUrl[0] != '\0' ) return envUrl;
#    ifdef MMM_PGO_UPLOAD_URL
    return MMM_PGO_UPLOAD_URL;
#    else
    return kDefaultPGOUploadUrl;
#    endif
}

/// @brief 读取最短上传运行时长。
long long resolveMinUploadRuntimeSeconds()
{
    const char* envValue = std::getenv("MMM_PGO_MIN_RUNTIME_SECONDS");
    if ( !envValue || envValue[0] == '\0' ) {
        return kDefaultMinUploadRuntimeSeconds;
    }

    char*     endPtr = nullptr;
    long long value  = std::strtoll(envValue, &endPtr, 10);
    if ( endPtr == envValue || value < 0 ) {
        return kDefaultMinUploadRuntimeSeconds;
    }
    return value;
}

/// @brief 计算本次 profile 收集运行时长。
long long elapsedRuntimeSeconds()
{
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
    std::lock_guard<std::mutex> lock(s_progressMutex);
    s_shutdownProgress.stage             = stage;
    s_shutdownProgress.message           = std::move(message);
    s_shutdownProgress.finished          = finished;
    s_shutdownProgress.runtimeSeconds    = elapsedRuntimeSeconds();
    s_shutdownProgress.minRuntimeSeconds = resolveMinUploadRuntimeSeconds();
}

/// @brief 更新 PGO 上传字节进度。
/// @param uploadedBytes 已上传字节。
/// @param totalBytes 预计总字节。
void setUploadByteProgress(std::uint64_t uploadedBytes,
                           std::uint64_t totalBytes)
{
    std::lock_guard<std::mutex> lock(s_progressMutex);
    s_shutdownProgress.uploadedBytes = uploadedBytes;
    s_shutdownProgress.totalBytes    = totalBytes;
}

/// @brief 添加 curl multipart 文本字段。
void addMimeTextPart(curl_mime* mime, const char* name,
                     const std::string& value)
{
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, name);
    curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
}

/// @brief 添加 curl multipart profile 文件字段。
void addMimeProfilePart(curl_mime* mime, const std::string& filePath)
{
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "profile");
    curl_mime_filename(
        part, std::filesystem::path(filePath).filename().string().c_str());
    curl_mime_filedata(part, filePath.c_str());
}

/// @brief 判断当前运行时长是否足够上传 profile。
bool shouldUploadProfile(long long runtimeSeconds)
{
    const long long minRuntimeSeconds = resolveMinUploadRuntimeSeconds();
    if ( runtimeSeconds >= minRuntimeSeconds ) return true;

    XINFO("PGO: runtime {}s is below upload threshold {}s; skipping upload",
          runtimeSeconds,
          minRuntimeSeconds);
    return false;
}

/// @brief libcurl 上传进度回调。
/// @return 返回 0 表示继续上传。
int uploadProgressCallback(void*, curl_off_t, curl_off_t, curl_off_t ultotal,
                           curl_off_t ulnow)
{
    auto safeTotal =
        static_cast<std::uint64_t>(std::max<curl_off_t>(0, ultotal));
    auto safeNow = static_cast<std::uint64_t>(std::max<curl_off_t>(0, ulnow));
    if ( safeTotal > 0 && safeNow > safeTotal ) {
        safeNow = safeTotal;
    }
    setUploadByteProgress(safeNow, safeTotal);
    return 0;
}

/// @brief 在退出低频路径中通过 curl multipart POST 上传 profile 文件。
/// @param filePath profile 文件路径。
/// @param runtimeSeconds 本次运行时长。
/// @return 上传成功时返回 true。
bool uploadProfile(const std::string& filePath, long long runtimeSeconds)
{
    std::string url = resolveUploadUrl();
    if ( url.empty() ) {
        XINFO("PGO: profile saved to {} (no upload URL configured)", filePath);
        setShutdownStage(PGOProfilerShutdownStage::Skipped,
                         "PGO: no upload URL configured",
                         true);
        return false;
    }

    // 验证文件存在且非空
    std::error_code ec;
    auto            fileSize = std::filesystem::file_size(filePath, ec);
    if ( ec || fileSize == 0 ) {
        if ( ec )
            XERROR("PGO: cannot stat profile: {}", ec.message());
        else
            XERROR("PGO: profile file is empty, skipping upload");
        setShutdownStage(PGOProfilerShutdownStage::Failed,
                         "PGO: profile file is not readable",
                         true);
        return false;
    }

    XINFO("PGO: uploading profile ({} KB, runtime {}s) ...",
          fileSize / 1024,
          runtimeSeconds);

    // 延迟初始化 curl (首次调用时)
    // curl_global_init 不是线程安全的，但我们在 init 阶段调用一次
    auto curl = curl_easy_init();
    if ( !curl ) {
        XERROR("PGO: curl_easy_init failed");
        setShutdownStage(PGOProfilerShutdownStage::Failed,
                         "PGO: curl_easy_init failed",
                         true);
        return false;
    }

    setShutdownStage(
        PGOProfilerShutdownStage::Uploading, "PGO: uploading profile", false);
    setUploadByteProgress(0, static_cast<std::uint64_t>(fileSize));

    auto mime = curl_mime_init(curl);
    addMimeProfilePart(mime, filePath);
    addMimeTextPart(mime, "runtime_seconds", std::to_string(runtimeSeconds));
    addMimeTextPart(mime, "app_version", MMM_VERSION_STRING);
    addMimeTextPart(mime, "platform", MMM_PLATFORM);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MusicMapMaker-PGO/1.0");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, uploadProgressCallback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(curl);
    if ( res == CURLE_OK ) {
        XINFO("PGO: profile uploaded successfully");
        setUploadByteProgress(static_cast<std::uint64_t>(fileSize),
                              static_cast<std::uint64_t>(fileSize));
        setShutdownStage(PGOProfilerShutdownStage::Succeeded,
                         "PGO: profile uploaded successfully",
                         true);
    } else {
        XERROR("PGO: upload failed: {}", curl_easy_strerror(res));
        setShutdownStage(
            PGOProfilerShutdownStage::Failed, curl_easy_strerror(res), true);
    }

    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}

/// @brief 声明 PGO 退出流程开始，避免重复写出或上传。
/// @return 本次调用成功取得执行权时返回 true。
bool claimShutdownStart(bool uploadAllowed)
{
    std::lock_guard<std::mutex> lock(s_progressMutex);
    if ( s_shutdownStarted ) return false;

    s_shutdownStarted                    = true;
    s_shutdownProgress                   = {};
    s_shutdownProgress.uploadAllowed     = uploadAllowed;
    s_shutdownProgress.runtimeSeconds    = elapsedRuntimeSeconds();
    s_shutdownProgress.minRuntimeSeconds = resolveMinUploadRuntimeSeconds();
    s_shutdownProgress.stage             = PGOProfilerShutdownStage::Idle;
    s_shutdownProgress.finished          = false;
    return true;
}

/// @brief 写出 profraw，并判断后续是否需要上传。
/// @param uploadAllowed 用户是否允许自动上传。
/// @return 需要执行上传时返回 true。
bool writeProfileAndShouldUpload(bool uploadAllowed)
{
    if ( !s_profileInitialized ) {
        setShutdownStage(PGOProfilerShutdownStage::Skipped,
                         "PGO: profiler is not initialized",
                         true);
        return false;
    }

    setShutdownStage(
        PGOProfilerShutdownStage::Writing, "PGO: writing profile", false);

    int ret = __llvm_profile_write_file();
    if ( ret != 0 ) {
        XERROR("PGO: __llvm_profile_write_file() failed (code {})", ret);
        setShutdownStage(PGOProfilerShutdownStage::Failed,
                         "PGO: failed to write profile",
                         true);
        return false;
    }

    const long long runtimeSeconds = elapsedRuntimeSeconds();
    {
        std::lock_guard<std::mutex> lock(s_progressMutex);
        s_shutdownProgress.runtimeSeconds = runtimeSeconds;
    }

    if ( !uploadAllowed ) {
        XINFO("PGO: automatic profile upload disabled by user");
        setShutdownStage(PGOProfilerShutdownStage::Skipped,
                         "PGO: upload disabled by user",
                         true);
        return false;
    }

    if ( !shouldUploadProfile(runtimeSeconds) ) {
        setShutdownStage(PGOProfilerShutdownStage::Skipped,
                         "PGO: runtime is below upload threshold",
                         true);
        return false;
    }

    if ( resolveUploadUrl().empty() ) {
        setShutdownStage(PGOProfilerShutdownStage::Skipped,
                         "PGO: no upload URL configured",
                         true);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_progressMutex);
        s_shutdownProgress.uploadAttempted = true;
    }
    return true;
}

/// @brief 执行已经准备好的 PGO 上传。
void uploadPreparedProfile()
{
    uploadProfile(s_profilePath, elapsedRuntimeSeconds());
}

}  // namespace
#endif

// =============================================================================
//  Public API
// =============================================================================

void initPGOProfiler()
{
#ifdef MMM_PGO_INSTRUMENT
    s_profileStartTime   = std::chrono::steady_clock::now();
    s_profilePath        = buildProfilePath();
    s_profileInitialized = true;
    __llvm_profile_set_filename(s_profilePath.c_str());
    XINFO("PGO: instrumentation active → {}", s_profilePath);

    // 删除运行时初始化残留的空 default.profraw (CWD)
    std::error_code ec;
    std::filesystem::remove("default.profraw", ec);

    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
}

void shutdownPGOProfiler(bool uploadAllowed)
{
#ifdef MMM_PGO_INSTRUMENT
    if ( !claimShutdownStart(uploadAllowed) ) {
        waitForShutdownPGOProfiler();
        return;
    }

    if ( writeProfileAndShouldUpload(uploadAllowed) ) {
        uploadPreparedProfile();
    }
#else
    (void)uploadAllowed;
#endif
}

bool beginShutdownPGOProfilerAsync(bool uploadAllowed)
{
#ifdef MMM_PGO_INSTRUMENT
    if ( !claimShutdownStart(uploadAllowed) ) return false;
    if ( !writeProfileAndShouldUpload(uploadAllowed) ) return false;

    {
        std::lock_guard<std::mutex> lock(s_progressMutex);
        s_shutdownProgress.stage    = PGOProfilerShutdownStage::Uploading;
        s_shutdownProgress.message  = "PGO: uploading profile";
        s_shutdownProgress.finished = false;
    }

    s_shutdownFuture =
        std::async(std::launch::async, []() { uploadPreparedProfile(); });
    return true;
#else
    (void)uploadAllowed;
    return false;
#endif
}

PGOProfilerShutdownProgress getPGOProfilerShutdownProgress()
{
#ifdef MMM_PGO_INSTRUMENT
    std::lock_guard<std::mutex> lock(s_progressMutex);
    return s_shutdownProgress;
#else
    return PGOProfilerShutdownProgress{
        .stage    = PGOProfilerShutdownStage::Skipped,
        .finished = true,
    };
#endif
}

bool isShutdownPGOProfilerFinished()
{
#ifdef MMM_PGO_INSTRUMENT
    std::lock_guard<std::mutex> lock(s_progressMutex);
    return s_shutdownProgress.finished;
#else
    return true;
#endif
}

void waitForShutdownPGOProfiler()
{
#ifdef MMM_PGO_INSTRUMENT
    if ( s_shutdownFuture.valid() ) {
        s_shutdownFuture.wait();
    }
#endif
}

}  // namespace MMM::Main
