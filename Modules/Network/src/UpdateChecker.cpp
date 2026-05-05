#include "network/UpdateChecker.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <cstdlib>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <regex>
#include <thread>

#if defined(_WIN32)
#    include <shellapi.h>
#    include <windows.h>
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

}  // namespace

bool UpdateChecker::parseVersion(const std::string& verStr, int& major,
                                 int& minor, int& patch)
{
    major = minor = patch = 0;

    // 尝试匹配 "v<major>.<minor>" 或 "v<major>.<minor>.<patch>" 格式
    // 允许前缀（如 "gamma"）
    std::regex  versionRegex(R"(v(\d+)\.(\d+)(?:\.(\d+))?)",
                             std::regex::ECMAScript);
    std::smatch match;
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

    if ( !parseVersion(remote, rMajor, rMinor, rPatch) ||
         !parseVersion(local, lMajor, lMinor, lPatch) ) {
        // 如果解析失败，回退到字符串比较
        return remote != local;
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

void UpdateChecker::checkAsync()
{
    m_info.status         = UpdateStatus::kChecking;
    m_info.currentVersion = APP_VERSION;

    std::thread([this]() {
        UpdateInfo result;
        result.status         = UpdateStatus::kChecking;
        result.currentVersion = APP_VERSION;

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

            const char* platform = APP_PLATFORM;
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

}  // namespace MMM::Network
