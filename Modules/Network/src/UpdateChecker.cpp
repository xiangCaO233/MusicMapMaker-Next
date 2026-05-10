#include "network/UpdateChecker.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmmversion.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <thread>

#if defined(_WIN32)
#    include <shellapi.h>
#    include <windows.h>
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

}  // namespace

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

void UpdateChecker::applyUpdateAndRestart(const std::string& downloadedFilePath)
{
    std::string exePath = currentExecutablePath();
    if ( exePath.empty() ) {
        XERROR("UpdateChecker: Cannot determine executable path");
        return;
    }

    std::filesystem::path exePathFs   = Config::utf8ToPath(exePath);
    std::filesystem::path updaterPath = exePathFs.parent_path();

#if defined(_WIN32)
    updaterPath /= "MusicMapMaker-Updater.exe";
#else
    updaterPath /= "MusicMapMaker-Updater";
#endif

    if ( !std::filesystem::exists(updaterPath) ) {
        XERROR("UpdateChecker: Updater not found at {}",
               Config::pathToUtf8(updaterPath));
        return;
    }

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
    std::filesystem::path dlPath = Config::utf8ToPath(downloadedFilePath);
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
        XERROR("UpdateChecker: Failed to launch updater (error={})",
               GetLastError());
        return;
    }
#else
    pid_t child = fork();
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
    m_info.status         = UpdateStatus::kChecking;
    m_info.currentVersion = MMM_VERSION_STRING;

    std::thread([this]() {
        UpdateInfo result;
        result.status         = UpdateStatus::kChecking;
        result.currentVersion = MMM_VERSION_STRING;

        CURL* curl = curl_easy_init();
        if ( !curl ) {
            result.status       = UpdateStatus::kError;
            result.errorMessage = "Failed to initialize libcurl";
            m_info              = result;
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

        if ( res != CURLE_OK ) {
            result.status = UpdateStatus::kError;
            result.errorMessage =
                fmt::format("Network error: {}", curl_easy_strerror(res));
            m_info = result;
            XERROR("UpdateChecker: {}", result.errorMessage);
            return;
        }

        if ( httpCode != 200 ) {
            result.status       = UpdateStatus::kError;
            result.errorMessage = fmt::format("HTTP error: {}", httpCode);
            m_info              = result;
            XERROR("UpdateChecker: {}", result.errorMessage);
            return;
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
            }

            if ( result.latestVersion.empty() ) {
                result.status       = UpdateStatus::kError;
                result.errorMessage = "No version info in response";
            } else if ( isNewer(result.latestVersion, result.currentVersion) ) {
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
            result.status       = UpdateStatus::kError;
            result.errorMessage = fmt::format("JSON parse error: {}", e.what());
            XERROR("UpdateChecker: {}", result.errorMessage);
        }

        m_info = result;
    }).detach();
}

void UpdateChecker::downloadAsync()
{
    if ( m_info.downloadUrl.empty() ) {
        m_info.status       = UpdateStatus::kError;
        m_info.errorMessage = "No download URL";
        return;
    }

    m_info.status = UpdateStatus::kDownloading;

    std::thread([this]() {
        UpdateInfo result       = m_info;
        result.status           = UpdateStatus::kDownloading;
        result.downloadedBytes  = 0;
        result.downloadProgress = 0.0;
        m_info                  = result;  // 立即通知 UI 进入下载状态

        // 构造临时文件路径
        std::filesystem::path tempPath =
            std::filesystem::temp_directory_path() / "MusicMapMaker_update";

#ifdef _WIN32
        FILE* file = _wfopen(tempPath.wstring().c_str(), L"wb");
#else
        FILE* file = fopen(tempPath.c_str(), "wb");
#endif
        if ( !file ) {
            m_info.status       = UpdateStatus::kError;
            m_info.errorMessage = "Failed to create temp file";
            XERROR("UpdateChecker: {}", m_info.errorMessage);
            return;
        }

        CURL* curl = curl_easy_init();
        if ( !curl ) {
            fclose(file);
            m_info.status       = UpdateStatus::kError;
            m_info.errorMessage = "Failed to initialize libcurl";
            XERROR("UpdateChecker: {}", m_info.errorMessage);
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, result.downloadUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fileWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(
            curl,
            CURLOPT_XFERINFOFUNCTION,
            +[](void*      clientp,
                curl_off_t dltotal,
                curl_off_t dlnow,
                curl_off_t /*ultotal*/,
                curl_off_t /*ulnow*/) -> int {
                auto* p = static_cast<UpdateChecker*>(clientp);
                if ( dltotal > 0 ) {
                    p->m_info.downloadedBytes  = dlnow;
                    p->m_info.downloadProgress = static_cast<double>(dlnow) /
                                                 static_cast<double>(dltotal);
                }
                return 0;
            });
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(
            curl, CURLOPT_USERAGENT, "MusicMapMaker-UpdateChecker/1.0");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L);

        CURLcode res = curl_easy_perform(curl);

        fclose(file);
        curl_easy_cleanup(curl);

        if ( res != CURLE_OK ) {
            m_info.status = UpdateStatus::kError;
            m_info.errorMessage =
                fmt::format("Download error: {}", curl_easy_strerror(res));
            std::filesystem::remove(tempPath);
            XERROR("UpdateChecker: {}", m_info.errorMessage);
            return;
        }

        m_info.status             = UpdateStatus::kDownloaded;
        m_info.downloadProgress   = 1.0;
        m_info.downloadedFilePath = Config::pathToUtf8(tempPath);
        XINFO("UpdateChecker: Download complete -> {}",
              Config::pathToUtf8(tempPath));
    }).detach();
}

}  // namespace MMM::Network
