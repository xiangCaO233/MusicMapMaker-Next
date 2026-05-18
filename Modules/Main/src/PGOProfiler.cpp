/// @file PGOProfiler.cpp
/// @brief PGO profile 写入 + curl 异步上传实现

#include "main/PGOProfiler.h"
#include "log/colorful-log.h"
#include "mmmversion.h"

#include <chrono>
#include <curl/curl.h>
#include <filesystem>
#include <string>
#include <thread>

#include <cstdlib>

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

namespace
{

std::string s_profilePath;

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

/// @brief 确定上传 URL: 环境变量 > 编译期 > 空(不上传)
std::string resolveUploadUrl()
{
#ifdef MMM_PGO_UPLOAD_URL
    const char* envUrl = std::getenv("MMM_PGO_UPLOAD_URL");
    if ( envUrl && envUrl[0] != '\0' ) return envUrl;
    return MMM_PGO_UPLOAD_URL;
#else
    return {};
#endif
}

/// @brief 在独立线程中通过 curl multipart POST 上传 profile 文件
void uploadProfileAsync(const std::string& filePath)
{
    std::string url = resolveUploadUrl();
    if ( url.empty() ) {
        XINFO("PGO: profile saved to {} (no upload URL configured)", filePath);
        return;
    }

    // 验证文件存在且非空
    std::error_code ec;
    auto            fileSize = std::filesystem::file_size(filePath, ec);
    if ( ec || fileSize == 0 ) {
        if ( ec )
            XERROR("PGO: cannot stat profile: {}", ec.message());
        else
            XERROR("PGO: profile file is empty, skipping upload");
        return;
    }

    XINFO("PGO: uploading profile ({} KB) ...", fileSize / 1024);

    // 延迟初始化 curl (首次调用时)
    // curl_global_init 不是线程安全的，但我们在 init 阶段调用一次
    auto curl = curl_easy_init();
    if ( !curl ) {
        XERROR("PGO: curl_easy_init failed");
        return;
    }

    // 构建 multipart form
    auto           mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "profile");
    curl_mime_filename(
        part, std::filesystem::path(filePath).filename().string().c_str());
    curl_mime_filedata(part, filePath.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MusicMapMaker-PGO/1.0");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    if ( res == CURLE_OK )
        XINFO("PGO: profile uploaded successfully");
    else
        XERROR("PGO: upload failed: {}", curl_easy_strerror(res));

    curl_mime_free(mime);
    curl_easy_cleanup(curl);
}

}  // namespace

// =============================================================================
//  Public API
// =============================================================================

void initPGOProfiler()
{
#ifdef MMM_PGO_INSTRUMENT
    s_profilePath = buildProfilePath();
    __llvm_profile_set_filename(s_profilePath.c_str());
    XINFO("PGO: instrumentation active → {}", s_profilePath);

    // 删除运行时初始化残留的空 default.profraw (CWD)
    std::error_code ec;
    std::filesystem::remove("default.profraw", ec);

    curl_global_init(CURL_GLOBAL_DEFAULT);
#else
    (void)s_profilePath;
#endif
}

void shutdownPGOProfiler()
{
#ifdef MMM_PGO_INSTRUMENT
    int ret = __llvm_profile_write_file();
    if ( ret != 0 ) {
        XERROR("PGO: __llvm_profile_write_file() failed (code {})", ret);
        return;
    }

    // 异步上传: 在独立线程中执行 curl POST，不阻塞程序退出
    std::thread([path = s_profilePath]() {
        uploadProfileAsync(path);
    }).detach();
#endif
}

}  // namespace MMM::Main
