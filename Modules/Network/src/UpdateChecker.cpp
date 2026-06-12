#include "network/UpdateChecker.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmmversion.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
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
#else
#    include <unistd.h>
#endif

using json = nlohmann::json;

namespace MMM::Network
{

namespace
{

/// @brief libcurl 写回调，将响应数据追加到字符串
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t totalSize = size * nmemb;
    auto*  str       = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

/// @brief libcurl 写回调，将响应数据写入文件
size_t fileWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    return fwrite(contents, size, nmemb, static_cast<FILE*>(userp));
}

/// @brief 写入更新启动失败原因。
void writeRestartError(std::string* errorMessage, const std::string& message)
{
    if ( errorMessage ) {
        *errorMessage = message;
    }
}

/// @brief 检查路径是否指向普通文件。
bool hasRegularFile(const std::filesystem::path& path,
                    std::string* errorMessage, const char* label)
{
    std::error_code filesystemError;
    if ( !std::filesystem::is_regular_file(path, filesystemError) ||
         filesystemError ) {
        const std::string message =
            fmt::format("{} not found: {}", label, Config::pathToUtf8(path));
        writeRestartError(errorMessage, message);
        XERROR("UpdateChecker: {}", message);
        return false;
    }
    return true;
}

#if !defined(_WIN32)
/// @brief 确保更新器文件在 POSIX 平台有执行权限。
bool ensureExecutablePermission(const std::filesystem::path& path,
                                std::string*                 errorMessage)
{
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
    if ( (status.permissions() & executableBits) !=
         std::filesystem::perms::none ) {
        return true;
    }

    std::error_code permissionError;
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

}  // namespace

UpdateInfo UpdateChecker::getInfo() const
{
    std::lock_guard<std::mutex> lock(m_infoMutex);
    return m_info;
}

bool UpdateChecker::parseVersion(const std::string& verStr, int& major,
                                 int& minor, int& patch)
{
    major = minor = patch = 0;

    // 尝试匹配 "v<major>.<minor>" 或 "v<major>.<minor>.<patch>" 格式
    // 允许前缀（如 "gamma"）
    static const std::regex versionRegex(R"(v(\d+)\.(\d+)(?:\.(\d+))?)",
                                         std::regex::ECMAScript);
    std::smatch             match;
    if ( std::regex_search(verStr, match, versionRegex) && match.size() >= 3 ) {
        major = std::stoi(match[1].str());
        minor = std::stoi(match[2].str());
        if ( match.size() >= 4 && match[3].matched ) {
            patch = std::stoi(match[3].str());
        }
        return true;
    }
    return false;
}

bool UpdateChecker::isNewer(const std::string& remote, const std::string& local)
{
    int rMajor, rMinor, rPatch;
    int lMajor, lMinor, lPatch;

    bool rOk = parseVersion(remote, rMajor, rMinor, rPatch);
    bool lOk = parseVersion(local, lMajor, lMinor, lPatch);

    if ( !rOk || !lOk ) {
        // 任一方解析失败则无法判定大小关系，保守返回 false
        return false;
    }

    if ( rMajor != lMajor ) return rMajor > lMajor;
    if ( rMinor != lMinor ) return rMinor > lMinor;
    if ( rPatch != lPatch ) return rPatch > lPatch;
    return false;
}

void UpdateChecker::openUrlInBrowser(const std::string& url)
{
#if defined(_WIN32)
    ShellExecuteA(
        nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::system(("open \"" + url + "\"").c_str());
#else
    std::system(("xdg-open \"" + url + "\" &").c_str());
#endif
}

std::string UpdateChecker::currentExecutablePath()
{
#if defined(_WIN32)
    wchar_t bufW[MAX_PATH];
    DWORD   len = GetModuleFileNameW(nullptr, bufW, MAX_PATH);
    if ( len > 0 && len < MAX_PATH ) {
        std::filesystem::path p(bufW);
        return Config::pathToUtf8(p);
    }
    return "";
#elif defined(__APPLE__)
    char     buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if ( _NSGetExecutablePath(buf, &size) == 0 ) return std::string(buf);
    return "";
#else
    std::error_code ec;
    auto            path = std::filesystem::canonical("/proc/self/exe", ec);
    if ( !ec ) return path.string();
    return "";
#endif
}

bool UpdateChecker::applyUpdateAndRestart(const std::string& downloadedFilePath,
                                          const std::string& updaterFilePath,
                                          std::string*       errorMessage)
{
    std::string exePath = currentExecutablePath();
    if ( exePath.empty() ) {
        constexpr const char* message = "Cannot determine executable path";
        writeRestartError(errorMessage, message);
        XERROR("UpdateChecker: {}", message);
        return false;
    }

    const std::filesystem::path downloadedPath =
        Config::utf8ToPath(downloadedFilePath);
    if ( !hasRegularFile(downloadedPath, errorMessage, "Downloaded update") ) {
        return false;
    }

    // 优先使用下载的更新器，不存在时回退到同目录查找
    std::filesystem::path updaterPath;
    if ( !updaterFilePath.empty() ) {
        updaterPath = Config::utf8ToPath(updaterFilePath);
    } else {
        std::filesystem::path exePathFs = Config::utf8ToPath(exePath);
        updaterPath                     = exePathFs.parent_path();
#if defined(_WIN32)
        updaterPath /= "MusicMapMaker-Updater.exe";
#else
        updaterPath /= "MusicMapMaker-Updater";
#endif
    }

    if ( !hasRegularFile(updaterPath, errorMessage, "Updater") ) {
        return false;
    }

#if !defined(_WIN32)
    if ( !ensureExecutablePermission(updaterPath, errorMessage) ) {
        return false;
    }
#endif

    long pid = 0;
#if defined(_WIN32)
    pid = static_cast<long>(GetCurrentProcessId());
#else
    pid = static_cast<long>(getpid());
#endif

    XINFO("UpdateChecker: Launching updater: {} {} {} {}",
          Config::pathToUtf8(updaterPath),
          downloadedFilePath,
          exePath,
          pid);

#if defined(_WIN32)
    std::filesystem::path dlPath    = Config::utf8ToPath(downloadedFilePath);
    std::filesystem::path exePathFs = Config::utf8ToPath(exePath);
    std::wstring cmdLine = L"\"" + updaterPath.wstring() + L"\" \"" +
                           dlPath.wstring() + L"\" \"" + exePathFs.wstring() +
                           L"\" " + std::to_wstring(pid);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
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
    pid_t child = fork();
    if ( child < 0 ) {
        constexpr const char* message = "Failed to fork updater process";
        writeRestartError(errorMessage, message);
        XERROR("UpdateChecker: {}", message);
        return false;
    }
    if ( child == 0 ) {
        // 子进程: 执行更新器
        std::string updater = Config::pathToUtf8(updaterPath);
        std::string pidStr  = std::to_string(pid);
        execl(updater.c_str(),
              updater.c_str(),
              downloadedFilePath.c_str(),
              exePath.c_str(),
              pidStr.c_str(),
              nullptr);
        _exit(1);
    }
#endif

    XINFO("UpdateChecker: Exiting for update...");
    std::exit(0);
    return true;
}

bool UpdateChecker::checkStartupUpdateMarker()
{
    std::string exePath = currentExecutablePath();
    if ( exePath.empty() ) return false;

    std::filesystem::path markerPath =
        Config::utf8ToPath(exePath).parent_path() / ".mm_update_success";

    if ( !std::filesystem::exists(markerPath) ) return false;

    std::filesystem::remove(markerPath);
    return true;
}

void UpdateChecker::checkAsync()
{
    {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        m_info.status         = UpdateStatus::kChecking;
        m_info.currentVersion = MMM_VERSION_STRING;
    }

    std::thread([this]() {
        int maxRetries = 3;
        int retries    = 0;

        while ( retries < maxRetries ) {
            UpdateInfo result;
            result.status         = UpdateStatus::kChecking;
            result.currentVersion = MMM_VERSION_STRING;

            CURL* curl = curl_easy_init();
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
            const char* checkUrl =
                "https://mmm.xiang233.top/download/check/check.json";

            curl_easy_setopt(curl, CURLOPT_URL, checkUrl);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(
                curl, CURLOPT_USERAGENT, "MusicMapMaker-UpdateChecker/1.0");

            CURLcode res = curl_easy_perform(curl);

            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_easy_cleanup(curl);

            if ( res != CURLE_OK || httpCode != 200 ) {
                retries++;
                if ( retries < maxRetries ) {
                    XWARN(
                        "UpdateChecker: Check failed (res={}, code={}), "
                        "retrying... ({}/{})",
                        static_cast<int>(res),
                        httpCode,
                        retries,
                        maxRetries);
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                } else {
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

            try {
                json data = json::parse(responseBody);

                result.latestVersion = data.value("version", "");
                result.changelog     = data.value("changelog", "");
                result.releaseDate   = data.value("release_date", "");

                const char* platform = MMM_PLATFORM;
                if ( data.contains("platforms") &&
                     data["platforms"].contains(platform) ) {
                    auto& plat          = data["platforms"][platform];
                    result.downloadUrl  = plat.value("url", "");
                    result.downloadSize = plat.value("size", 0);
                    result.checksum     = plat.value("checksum", "");

                    // 拼接完整 URL
                    if ( !result.downloadUrl.empty() &&
                         result.downloadUrl[0] == '/' ) {
                        result.downloadUrl =
                            "https://mmm.xiang233.top" + result.downloadUrl;
                    }

                    // 解析更新器地址
                    if ( plat.contains("updater") ) {
                        result.updaterUrl = plat["updater"].value("url", "");
                        if ( !result.updaterUrl.empty() &&
                             result.updaterUrl[0] == '/' ) {
                            result.updaterUrl =
                                "https://mmm.xiang233.top" + result.updaterUrl;
                        }
                    }
                }

                if ( result.latestVersion.empty() ) {
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
            } catch ( const std::exception& e ) {
                result.status = UpdateStatus::kError;
                result.errorMessage =
                    fmt::format("JSON parse error: {}", e.what());
                XERROR("UpdateChecker: {}", result.errorMessage);
            }

            {
                std::lock_guard<std::mutex> lock(m_infoMutex);
                m_info = result;
            }
            return;
        }
    }).detach();
}

void UpdateChecker::downloadAsync()
{
    UpdateInfo initialInfo;
    {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        initialInfo = m_info;
        if ( initialInfo.downloadUrl.empty() ) {
            m_info.status       = UpdateStatus::kError;
            m_info.errorMessage = "No download URL";
            return;
        }

        m_info.status = UpdateStatus::kDownloading;
    }

    std::thread([this, initialInfo]() {
        auto fail = [this](const std::string& message) {
            {
                std::lock_guard<std::mutex> lock(m_infoMutex);
                m_info.status       = UpdateStatus::kError;
                m_info.errorMessage = message;
            }
            XERROR("UpdateChecker: {}", message);
        };

        UpdateInfo result       = initialInfo;
        result.status           = UpdateStatus::kDownloading;
        result.downloadedBytes  = 0;
        result.downloadProgress = 0.0;
        {
            std::lock_guard<std::mutex> lock(m_infoMutex);
            m_info = result;  // 立即通知 UI 进入下载状态
        }

        // Phase 1: 下载更新器（有 updaterUrl 时）
        if ( !result.updaterUrl.empty() ) {
            std::filesystem::path updaterTempPath =
                std::filesystem::temp_directory_path() /
                "MusicMapMaker_updater";

#ifdef _WIN32
            FILE* uFile = _wfopen(updaterTempPath.wstring().c_str(), L"wb");
#else
            FILE* uFile = fopen(updaterTempPath.c_str(), "wb");
#endif
            if ( !uFile ) {
                fail("Failed to create updater temp file");
                return;
            }

            CURL* uCurl = curl_easy_init();
            if ( !uCurl ) {
                fclose(uFile);
                fail("Failed to initialize libcurl for updater");
                return;
            }

            curl_easy_setopt(uCurl, CURLOPT_URL, result.updaterUrl.c_str());
            curl_easy_setopt(uCurl, CURLOPT_WRITEFUNCTION, fileWriteCallback);
            curl_easy_setopt(uCurl, CURLOPT_WRITEDATA, uFile);
            curl_easy_setopt(uCurl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(uCurl, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(
                uCurl, CURLOPT_USERAGENT, "MusicMapMaker-UpdateChecker/1.0");
            curl_easy_setopt(uCurl, CURLOPT_CONNECTTIMEOUT, 30L);
            curl_easy_setopt(uCurl, CURLOPT_TIMEOUT, 60L);

            CURLcode uRes = curl_easy_perform(uCurl);

            fclose(uFile);
            curl_easy_cleanup(uCurl);

            if ( uRes != CURLE_OK ) {
                std::filesystem::remove(updaterTempPath);
                fail(fmt::format("Updater download error: {}",
                                 curl_easy_strerror(uRes)));
                return;
            }

#if !defined(_WIN32)
            if ( !ensureExecutablePermission(updaterTempPath, nullptr) ) {
                std::filesystem::remove(updaterTempPath);
                fail("Failed to mark updater executable");
                return;
            }
#endif

            result.updaterFilePath = Config::pathToUtf8(updaterTempPath);
            {
                std::lock_guard<std::mutex> lock(m_infoMutex);
                m_info.updaterFilePath = result.updaterFilePath;
            }
            XINFO("UpdateChecker: Updater downloaded -> {}",
                  result.updaterFilePath);
        }

        // Phase 2: 下载主程序更新
        std::filesystem::path mainTempPath =
            std::filesystem::temp_directory_path() / "MusicMapMaker_update";

#ifdef _WIN32
        FILE* mFile = _wfopen(mainTempPath.wstring().c_str(), L"wb");
#else
        FILE* mFile = fopen(mainTempPath.c_str(), "wb");
#endif
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
                if ( dltotal > 0 ) {
                    std::lock_guard<std::mutex> lock(p->m_infoMutex);
                    p->m_info.downloadedBytes  = dlnow;
                    p->m_info.downloadProgress = static_cast<double>(dlnow) /
                                                 static_cast<double>(dltotal);
                }
                return 0;
            });
        curl_easy_setopt(mCurl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(mCurl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(mCurl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(mCurl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(
            mCurl, CURLOPT_USERAGENT, "MusicMapMaker-UpdateChecker/1.0");
        curl_easy_setopt(mCurl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(mCurl, CURLOPT_TIMEOUT, 3600L);

        CURLcode mRes = curl_easy_perform(mCurl);

        fclose(mFile);
        curl_easy_cleanup(mCurl);

        if ( mRes != CURLE_OK ) {
            std::filesystem::remove(mainTempPath);
            fail(fmt::format("Download error: {}", curl_easy_strerror(mRes)));
            return;
        }

        result.downloadProgress   = 1.0;
        result.downloadedFilePath = Config::pathToUtf8(mainTempPath);
        result.status             = UpdateStatus::kDownloaded;
        {
            std::lock_guard<std::mutex> lock(m_infoMutex);
            m_info = result;
        }
        XINFO("UpdateChecker: Download complete -> {}",
              Config::pathToUtf8(mainTempPath));
    }).detach();
}

}  // namespace MMM::Network
