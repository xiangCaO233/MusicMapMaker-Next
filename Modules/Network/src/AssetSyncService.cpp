#include "network/AssetSyncService.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <fstream>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <system_error>

namespace MMM::Network
{

namespace
{

/// @brief 官方网站根 URL。
constexpr const char* kDefaultAssetBaseUrl = "https://mmm.xiang233.top";

/// @brief 默认资源清单路径。
constexpr const char* kDefaultManifestPath = "/download/assets/manifest.json";

/// @brief 默认完整资源包路径。
constexpr const char* kDefaultPackagePath = "/download/assets.zip";

/// @brief 本地记录资源版本的文件名。
constexpr const char* kLocalVersionFileName = ".mmm-assets-version";

/// @brief 下载资源时使用的 User-Agent。
constexpr const char* kAssetUserAgent = "MusicMapMaker-AssetSync/1.0";

/// @brief SHA256 初始哈希值。
constexpr std::array<std::uint32_t, 8> kSha256InitialState{
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};

/// @brief SHA256 轮常量。
constexpr std::array<std::uint32_t, 64> kSha256RoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

/// @brief libcurl 字符串写回调。
std::size_t writeStringCallback(void* contents, std::size_t size,
                                std::size_t nmemb, void* userp)
{
    const std::size_t totalSize = size * nmemb;
    auto*             response  = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

/// @brief libcurl 文件写回调。
std::size_t writeFileCallback(void* contents, std::size_t size,
                              std::size_t nmemb, void* userp)
{
    return std::fwrite(contents, size, nmemb, static_cast<FILE*>(userp));
}

/// @brief libcurl 下载进度状态。
struct DownloadProgressState {
    std::function<void(std::int64_t, std::int64_t)>
        callback;  ///< 下载进度回调。
};

/// @brief libcurl 下载进度回调。
int downloadProgressCallback(void* clientp, curl_off_t dltotal,
                             curl_off_t dlnow, curl_off_t /*ultotal*/,
                             curl_off_t /*ulnow*/)
{
    auto* state = static_cast<DownloadProgressState*>(clientp);
    if ( state && state->callback ) {
        state->callback(static_cast<std::int64_t>(dlnow),
                        static_cast<std::int64_t>(dltotal));
    }
    return 0;
}

/// @brief 判断字符串是否以指定前缀开头。
bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

/// @brief 判断字符串是否是绝对 URL。
bool isAbsoluteUrl(std::string_view url)
{
    return startsWith(url, "http://") || startsWith(url, "https://") ||
           startsWith(url, "file://");
}

/// @brief 移除 URL 末尾的斜杠。
std::string trimTrailingSlash(std::string value)
{
    while ( !value.empty() && value.back() == '/' ) {
        value.pop_back();
    }
    return value;
}

/// @brief 将远程 URL 规格化为绝对 URL。
std::string resolveUrlInternal(const std::string& baseUrl,
                               const std::string& url)
{
    if ( url.empty() || isAbsoluteUrl(url) ) return url;

    const std::string base = trimTrailingSlash(baseUrl);
    if ( url.front() == '/' ) return base + url;
    return base + "/" + url;
}

/// @brief 从 file URL 中提取本地路径。
std::filesystem::path pathFromFileUrl(const std::string& url)
{
    constexpr std::string_view kFilePrefix = "file://";
    if ( !startsWith(url, kFilePrefix) ) return {};
    return Config::utf8ToPath(url.substr(kFilePrefix.size()));
}

/// @brief 读取环境变量字符串。
std::string readEnvironmentString(const char* name)
{
    const char* value = std::getenv(name);
    if ( value && value[0] != '\0' ) return value;
    return {};
}

/// @brief 发送资源同步进度事件。
void emitProgress(const AssetSyncOptions&  options,
                  const AssetSyncProgress& progress)
{
    if ( options.progressCallback ) options.progressCallback(progress);
}

/// @brief 判断字符是否为十六进制数字。
bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/// @brief 将 SHA256 字符串归一化为 64 位小写十六进制。
std::string normalizeSha256(std::string value)
{
    if ( startsWith(value, "sha256:") ) value.erase(0, 7);
    if ( value.size() != 64 ) return {};

    for ( char& c : value ) {
        if ( !isHexDigit(c) ) return {};
        if ( c >= 'A' && c <= 'F' ) {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

/// @brief 读取 JSON 对象中的字符串字段。
std::string jsonStringField(const nlohmann::json& value, const char* key)
{
    if ( !value.is_object() ) return {};
    const auto iter = value.find(key);
    if ( iter == value.end() || !iter->is_string() ) return {};
    return iter->get<std::string>();
}

/// @brief 读取 JSON 对象中的无符号整数字段。
std::uint64_t jsonUnsignedField(const nlohmann::json& value, const char* key)
{
    if ( !value.is_object() ) return 0;
    const auto iter = value.find(key);
    if ( iter == value.end() ) return 0;
    if ( iter->is_number_unsigned() ) return iter->get<std::uint64_t>();
    if ( iter->is_number_integer() ) {
        const auto signedValue = iter->get<std::int64_t>();
        if ( signedValue > 0 ) return static_cast<std::uint64_t>(signedValue);
    }
    return 0;
}

/// @brief 判断相对资源路径是否安全。
bool isSafeRelativeAssetPath(const std::string& path)
{
    if ( path.empty() || path.front() == '/' || path.front() == '\\' ) {
        return false;
    }
    if ( path.find('\\') != std::string::npos ||
         path.find(':') != std::string::npos ) {
        return false;
    }

    std::size_t start = 0;
    while ( start <= path.size() ) {
        const std::size_t end     = path.find('/', start);
        const auto        segment = path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if ( segment.empty() || segment == "." || segment == ".." ) {
            return false;
        }
        if ( end == std::string::npos ) break;
        start = end + 1;
    }
    return true;
}

/// @brief 判断目标路径是否仍处在根目录内。
bool isPathInsideRoot(const std::filesystem::path& root,
                      const std::filesystem::path& target)
{
    const auto normalizedRoot   = root.lexically_normal();
    const auto normalizedTarget = target.lexically_normal();
    const auto relativePath =
        normalizedTarget.lexically_relative(normalizedRoot);
    if ( relativePath.empty() || relativePath.is_absolute() ) return false;
    for ( const auto& part : relativePath ) {
        if ( part == ".." ) return false;
    }
    return true;
}

/// @brief 向文件写入字节。
bool writeBytesToFile(const std::filesystem::path& path, const void* data,
                      std::size_t size)
{
    const auto parentPath = path.parent_path();
    if ( !parentPath.empty() ) {
        std::error_code createError;
        std::filesystem::create_directories(parentPath, createError);
        if ( createError ) return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    if ( size > 0 ) {
        file.write(static_cast<const char*>(data),
                   static_cast<std::streamsize>(size));
    }
    return file.good();
}

/// @brief 读取完整文件。
bool readFileBytes(const std::filesystem::path& path,
                   std::vector<std::uint8_t>&   bytes)
{
    bytes.clear();

    std::error_code fileSizeError;
    const auto      fileSize = std::filesystem::file_size(path, fileSizeError);
    if ( fileSizeError ) return false;

    std::ifstream file(path, std::ios::binary);
    if ( !file ) return false;

    bytes.resize(static_cast<std::size_t>(fileSize));
    if ( !bytes.empty() ) {
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    return file.good() || (bytes.empty() && file.eof());
}

/// @brief SHA256 右旋。
std::uint32_t sha256RotateRight(std::uint32_t value, std::uint32_t bits)
{
    return (value >> bits) | (value << (32U - bits));
}

/// @brief 从 4 个字节读取大端 32 位整数。
std::uint32_t readBigEndian32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

/// @brief 向输出缓冲区写入大端 32 位整数。
void writeBigEndian32(std::uint32_t value, std::uint8_t* bytes)
{
    bytes[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[3] = static_cast<std::uint8_t>(value & 0xffU);
}

/// @brief 计算内存数据的 SHA256。
std::string sha256Bytes(const std::vector<std::uint8_t>& input)
{
    std::vector<std::uint8_t> data = input;
    const std::uint64_t       bitLength =
        static_cast<std::uint64_t>(data.size()) * 8ULL;

    data.push_back(0x80U);
    while ( (data.size() % 64U) != 56U ) {
        data.push_back(0U);
    }
    for ( int shift = 56; shift >= 0; shift -= 8 ) {
        data.push_back(static_cast<std::uint8_t>((bitLength >> shift) & 0xffU));
    }

    auto state = kSha256InitialState;
    for ( std::size_t offset = 0; offset < data.size(); offset += 64U ) {
        std::array<std::uint32_t, 64> words{};
        for ( std::size_t i = 0; i < 16U; ++i ) {
            words[i] = readBigEndian32(data.data() + offset + i * 4U);
        }
        for ( std::size_t i = 16U; i < 64U; ++i ) {
            const std::uint32_t s0 = sha256RotateRight(words[i - 15U], 7U) ^
                                     sha256RotateRight(words[i - 15U], 18U) ^
                                     (words[i - 15U] >> 3U);
            const std::uint32_t s1 = sha256RotateRight(words[i - 2U], 17U) ^
                                     sha256RotateRight(words[i - 2U], 19U) ^
                                     (words[i - 2U] >> 10U);
            words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];

        for ( std::size_t i = 0; i < 64U; ++i ) {
            const std::uint32_t s1 = sha256RotateRight(e, 6U) ^
                                     sha256RotateRight(e, 11U) ^
                                     sha256RotateRight(e, 25U);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t tmp1 =
                h + s1 + ch + kSha256RoundConstants[i] + words[i];
            const std::uint32_t s0 = sha256RotateRight(a, 2U) ^
                                     sha256RotateRight(a, 13U) ^
                                     sha256RotateRight(a, 22U);
            const std::uint32_t maj  = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t tmp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + tmp1;
            d = c;
            c = b;
            b = a;
            a = tmp1 + tmp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<std::uint8_t, 32> digest{};
    for ( std::size_t i = 0; i < state.size(); ++i ) {
        writeBigEndian32(state[i], digest.data() + i * 4U);
    }

    std::ostringstream stream;
    stream << std::hex;
    for ( const auto byte : digest ) {
        stream.width(2);
        stream.fill('0');
        stream << static_cast<int>(byte);
    }
    return stream.str();
}

/// @brief 下载文本响应。
bool downloadString(const std::string& url, std::string& response,
                    std::string& errorMessage)
{
    response.clear();

    if ( startsWith(url, "file://") ) {
        std::vector<std::uint8_t> bytes;
        if ( !readFileBytes(pathFromFileUrl(url), bytes) ) {
            errorMessage = "Failed to read local file URL";
            return false;
        }
        if ( bytes.empty() ) {
            response.clear();
        } else {
            response.assign(reinterpret_cast<const char*>(bytes.data()),
                            bytes.size());
        }
        return true;
    }

    CURL* curl = curl_easy_init();
    if ( !curl ) {
        errorMessage = "Failed to initialize libcurl";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kAssetUserAgent);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    const CURLcode result   = curl_easy_perform(curl);
    long           httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if ( result != CURLE_OK ) {
        errorMessage =
            std::string("Network error: ") + curl_easy_strerror(result);
        return false;
    }
    if ( httpCode != 0 && httpCode != 200 ) {
        errorMessage = "HTTP error: " + std::to_string(httpCode);
        return false;
    }
    return true;
}

/// @brief 以二进制形式下载文件。
bool downloadFile(const std::string& url, const std::filesystem::path& path,
                  std::string& errorMessage,
                  const std::function<void(std::int64_t, std::int64_t)>&
                      progressCallback = {})
{
    if ( startsWith(url, "file://") ) {
        std::vector<std::uint8_t> bytes;
        if ( !readFileBytes(pathFromFileUrl(url), bytes) ) {
            errorMessage = "Failed to read local file URL";
            return false;
        }
        if ( progressCallback ) {
            progressCallback(0, static_cast<std::int64_t>(bytes.size()));
        }
        if ( !writeBytesToFile(path, bytes.data(), bytes.size()) ) {
            errorMessage = "Failed to create output file";
            return false;
        }
        if ( progressCallback ) {
            progressCallback(static_cast<std::int64_t>(bytes.size()),
                             static_cast<std::int64_t>(bytes.size()));
        }
        return true;
    }

    const auto parentPath = path.parent_path();
    if ( !parentPath.empty() ) {
        std::error_code createError;
        std::filesystem::create_directories(parentPath, createError);
        if ( createError ) {
            errorMessage = createError.message();
            return false;
        }
    }

#ifdef _WIN32
    FILE* file = _wfopen(path.wstring().c_str(), L"wb");
#else
    FILE* file = std::fopen(path.c_str(), "wb");
#endif
    if ( !file ) {
        errorMessage = "Failed to create output file";
        return false;
    }

    CURL* curl = curl_easy_init();
    if ( !curl ) {
        std::fclose(file);
        errorMessage = "Failed to initialize libcurl";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kAssetUserAgent);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);

    DownloadProgressState progressState{ progressCallback };
    if ( progressCallback ) {
        curl_easy_setopt(
            curl, CURLOPT_XFERINFOFUNCTION, downloadProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressState);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    const CURLcode result = curl_easy_perform(curl);

    std::fclose(file);
    curl_easy_cleanup(curl);

    if ( result != CURLE_OK ) {
        errorMessage =
            std::string("Download error: ") + curl_easy_strerror(result);
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        return false;
    }
    return true;
}

/// @brief 获取临时下载文件路径。
std::filesystem::path temporaryDownloadPath(const char* fileName)
{
    std::error_code       tempPathError;
    std::filesystem::path path =
        std::filesystem::temp_directory_path(tempPathError);
    if ( tempPathError ) path = ".";
    path /= "MusicMapMaker";

    std::error_code createError;
    std::filesystem::create_directories(path, createError);
    path /= fileName;
    return path;
}

/// @brief 移动文件，跨文件系统时回退到复制。
bool moveDownloadedFile(const std::filesystem::path& source,
                        const std::filesystem::path& destination,
                        std::string&                 errorMessage)
{
    const auto parentPath = destination.parent_path();
    if ( !parentPath.empty() ) {
        std::error_code createError;
        std::filesystem::create_directories(parentPath, createError);
        if ( createError ) {
            errorMessage = createError.message();
            return false;
        }
    }

    std::error_code renameError;
    std::filesystem::rename(source, destination, renameError);
    if ( !renameError ) return true;

    std::error_code copyError;
    std::filesystem::copy_file(
        source,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        copyError);
    if ( copyError ) {
        errorMessage = copyError.message();
        return false;
    }

    std::error_code removeError;
    std::filesystem::remove(source, removeError);
    return true;
}

/// @brief 写入本地资源版本文件。
void writeLocalVersionFile(const std::filesystem::path& assetsRootPath,
                           const std::string&           version)
{
    if ( version.empty() ) return;

    const auto    versionPath = assetsRootPath / kLocalVersionFileName;
    std::ofstream file(versionPath, std::ios::binary | std::ios::trunc);
    if ( file ) file << version << '\n';
}

/// @brief 读取本地资源版本文件。
std::string readLocalVersionFile(const std::filesystem::path& assetsRootPath)
{
    const auto    versionPath = assetsRootPath / kLocalVersionFileName;
    std::ifstream file(versionPath, std::ios::binary);
    if ( !file ) return {};

    std::string version;
    std::getline(file, version);
    while ( !version.empty() &&
            (version.back() == '\r' || version.back() == '\n') ) {
        version.pop_back();
    }
    return version;
}

/// @brief 判断本地资源文件是否缺失或内容不匹配。
bool isAssetFileOutdated(const AssetFileEntry&        file,
                         const std::filesystem::path& assetsRootPath)
{
    if ( !isSafeRelativeAssetPath(file.path) ) return true;

    const auto localPath =
        (assetsRootPath / Config::utf8ToPath(file.path)).lexically_normal();
    if ( !isPathInsideRoot(assetsRootPath, localPath) ) return true;

    std::error_code regularFileError;
    const bool      regularFile =
        std::filesystem::is_regular_file(localPath, regularFileError);
    if ( regularFileError || !regularFile ) return true;

    if ( file.size > 0 ) {
        std::error_code sizeError;
        const auto localSize = std::filesystem::file_size(localPath, sizeError);
        if ( sizeError || localSize != file.size ) return true;
    }

    const auto localSha256 = AssetSyncService::sha256File(localPath);
    return localSha256 != file.sha256;
}

/// @brief 收集本地缺失或变更文件，并向启动 UI 汇报校验进度。
std::vector<AssetFileEntry> collectOutdatedFilesWithProgress(
    const AssetManifest& manifest, const std::filesystem::path& assetsRootPath,
    const AssetSyncOptions& options)
{
    std::vector<AssetFileEntry> outdatedFiles;
    for ( std::size_t index = 0; index < manifest.files.size(); ++index ) {
        const auto& file = manifest.files[index];
        emitProgress(options,
                     AssetSyncProgress{
                         AssetSyncProgressStage::kCheckingFiles,
                         "Checking local asset: " + file.path,
                         0,
                         0,
                         index + 1,
                         manifest.files.size(),
                     });
        if ( isAssetFileOutdated(file, assetsRootPath) ) {
            outdatedFiles.push_back(file);
        }
    }
    return outdatedFiles;
}

/// @brief 下载并解压完整资源包。
AssetSyncResult downloadFullPackage(const AssetSyncOptions& options,
                                    const AssetManifest*    manifest)
{
    AssetSyncResult result;

    std::string packageUrl    = options.packageUrl;
    std::string packageSha256 = {};
    if ( manifest ) {
        result.remoteVersion = manifest->version;
        if ( !manifest->packageUrl.empty() ) packageUrl = manifest->packageUrl;
        packageSha256 = manifest->packageSha256;
    }

    packageUrl =
        AssetSyncService::resolveDownloadUrl(options.baseUrl, packageUrl);

    if ( packageUrl.empty() ) {
        result.status       = AssetSyncStatus::kError;
        result.errorMessage = "No asset package URL configured";
        return result;
    }

    const auto tempZipPath = temporaryDownloadPath("assets.zip");
    XINFO("AssetSync: downloading full asset package from {}", packageUrl);
    emitProgress(options,
                 AssetSyncProgress{
                     AssetSyncProgressStage::kDownloadingPackage,
                     "Downloading asset package",
                     0,
                     0,
                     0,
                     0,
                 });

    if ( !downloadFile(packageUrl,
                       tempZipPath,
                       result.errorMessage,
                       [&options](std::int64_t downloaded, std::int64_t total) {
                           emitProgress(
                               options,
                               AssetSyncProgress{
                                   AssetSyncProgressStage::kDownloadingPackage,
                                   "Downloading asset package",
                                   downloaded,
                                   total,
                                   0,
                                   0,
                               });
                       }) ) {
        result.status = AssetSyncStatus::kError;
        return result;
    }

    if ( !packageSha256.empty() ) {
        const auto actualSha256 = AssetSyncService::sha256File(tempZipPath);
        if ( actualSha256 != packageSha256 ) {
            result.status       = AssetSyncStatus::kError;
            result.errorMessage = "Asset package checksum mismatch; expected " +
                                  packageSha256 + ", got " + actualSha256;
            return result;
        }
    }

    const auto destinationRoot = options.assetsRootPath.parent_path();
    emitProgress(options,
                 AssetSyncProgress{
                     AssetSyncProgressStage::kExtractingPackage,
                     "Extracting asset package",
                     0,
                     0,
                     0,
                     0,
                 });
    if ( !AssetSyncService::extractZipArchive(
             tempZipPath, destinationRoot, result.errorMessage) ) {
        result.status = AssetSyncStatus::kError;
        return result;
    }

    std::error_code removeError;
    std::filesystem::remove(tempZipPath, removeError);

    writeLocalVersionFile(options.assetsRootPath, result.remoteVersion);
    emitProgress(options,
                 AssetSyncProgress{
                     AssetSyncProgressStage::kFinished,
                     "Assets ready",
                     0,
                     0,
                     0,
                     0,
                 });
    result.status           = AssetSyncStatus::kDownloaded;
    result.updatedFileCount = manifest ? manifest->files.size() : 0;
    return result;
}

/// @brief 按清单下载缺失或变更的单文件。
AssetSyncResult updateFromManifest(const AssetSyncOptions& options,
                                   const AssetManifest&    manifest)
{
    AssetSyncResult result;
    result.remoteVersion    = manifest.version;
    result.checkedFileCount = manifest.files.size();
    emitProgress(options,
                 AssetSyncProgress{
                     AssetSyncProgressStage::kCheckingFiles,
                     "Checking local assets",
                     0,
                     0,
                     0,
                     manifest.files.size(),
                 });
    auto outdatedFiles = collectOutdatedFilesWithProgress(
        manifest, options.assetsRootPath, options);

    if ( outdatedFiles.empty() ) {
        writeLocalVersionFile(options.assetsRootPath, manifest.version);
        emitProgress(options,
                     AssetSyncProgress{
                         AssetSyncProgressStage::kFinished,
                         "Assets ready",
                         0,
                         0,
                         0,
                         0,
                     });
        result.status = AssetSyncStatus::kReady;
        return result;
    }

    XINFO("AssetSync: updating {} changed asset file(s)", outdatedFiles.size());

    for ( const auto& file : outdatedFiles ) {
        const std::size_t currentFileIndex = result.updatedFileCount + 1;
        const auto        tempPath = temporaryDownloadPath("asset-file.tmp");
        const auto        fileUrl =
            AssetSyncService::resolveDownloadUrl(options.baseUrl, file.url);
        emitProgress(options,
                     AssetSyncProgress{
                         AssetSyncProgressStage::kDownloadingFile,
                         "Downloading asset file: " + file.path,
                         0,
                         static_cast<std::int64_t>(file.size),
                         currentFileIndex,
                         outdatedFiles.size(),
                     });

        if ( !downloadFile(fileUrl,
                           tempPath,
                           result.errorMessage,
                           [&options,
                            currentFileIndex,
                            totalFiles = outdatedFiles.size(),
                            path       = file.path](std::int64_t downloaded,
                                              std::int64_t total) {
                               emitProgress(
                                   options,
                                   AssetSyncProgress{
                                       AssetSyncProgressStage::kDownloadingFile,
                                       "Downloading asset file: " + path,
                                       downloaded,
                                       total,
                                       currentFileIndex,
                                       totalFiles,
                                   });
                           }) ) {
            result.status = AssetSyncStatus::kError;
            return result;
        }

        const auto actualSha256 = AssetSyncService::sha256File(tempPath);
        if ( actualSha256 != file.sha256 ) {
            result.status       = AssetSyncStatus::kError;
            result.errorMessage = "Asset file checksum mismatch: " + file.path;
            return result;
        }

        const auto destinationPath =
            (options.assetsRootPath / Config::utf8ToPath(file.path))
                .lexically_normal();
        if ( !moveDownloadedFile(
                 tempPath, destinationPath, result.errorMessage) ) {
            result.status = AssetSyncStatus::kError;
            return result;
        }

        ++result.updatedFileCount;
    }

    writeLocalVersionFile(options.assetsRootPath, manifest.version);
    emitProgress(options,
                 AssetSyncProgress{
                     AssetSyncProgressStage::kFinished,
                     "Assets ready",
                     0,
                     0,
                     0,
                     0,
                 });
    result.status = AssetSyncStatus::kUpdated;
    return result;
}

/// @brief 在已有本地资源时远端更新失败，记录警告并继续启动。
AssetSyncResult keepExistingAssetsAfterUpdateFailure(const std::string& reason)
{
    XWARN("AssetSync: using existing local assets; remote update skipped: {}",
          reason);

    AssetSyncResult result;
    result.status       = AssetSyncStatus::kReady;
    result.errorMessage = reason;
    return result;
}

}  // namespace

AssetSyncOptions AssetSyncService::defaultOptions()
{
    AssetSyncOptions options;
    options.assetsRootPath = Config::AppPaths::assetsRootPath();
    options.baseUrl        = readEnvironmentString("MMM_ASSET_BASE_URL");
    if ( options.baseUrl.empty() ) options.baseUrl = kDefaultAssetBaseUrl;

    options.manifestUrl = readEnvironmentString("MMM_ASSET_MANIFEST_URL");
    if ( options.manifestUrl.empty() ) {
        options.manifestUrl =
            resolveDownloadUrl(options.baseUrl, kDefaultManifestPath);
    }

    options.packageUrl = readEnvironmentString("MMM_ASSET_PACKAGE_URL");
    if ( options.packageUrl.empty() ) {
        options.packageUrl =
            resolveDownloadUrl(options.baseUrl, kDefaultPackagePath);
    }
    return options;
}

std::string AssetSyncService::resolveDownloadUrl(const std::string& baseUrl,
                                                 const std::string& url)
{
    return resolveUrlInternal(baseUrl, url);
}

AssetSyncResult AssetSyncService::sync(const AssetSyncOptions& options)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::error_code existsError;
    const bool      assetsExist =
        std::filesystem::exists(options.assetsRootPath, existsError);
    if ( assetsExist ) {
        std::error_code directoryError;
        const bool      assetsIsDirectory = std::filesystem::is_directory(
            options.assetsRootPath, directoryError);
        if ( directoryError || !assetsIsDirectory ) {
            AssetSyncResult result;
            result.status       = AssetSyncStatus::kError;
            result.errorMessage = "Assets path exists but is not a directory";
            return result;
        }
    }

    std::string manifestText;
    std::string manifestDownloadError;
    emitProgress(options,
                 AssetSyncProgress{
                     AssetSyncProgressStage::kCheckingManifest,
                     "Checking remote asset manifest",
                     0,
                     0,
                     0,
                     0,
                 });
    const bool manifestDownloaded = downloadString(
        options.manifestUrl, manifestText, manifestDownloadError);

    std::optional<AssetManifest> manifest;
    std::string                  manifestParseError;
    if ( manifestDownloaded ) {
        manifest = parseManifest(manifestText, manifestParseError);
    }

    if ( !assetsExist ) {
        if ( manifestDownloaded && !manifest ) {
            AssetSyncResult result;
            result.status       = AssetSyncStatus::kError;
            result.errorMessage = manifestParseError;
            return result;
        }
        return downloadFullPackage(options, manifest ? &(*manifest) : nullptr);
    }

    if ( !manifestDownloaded ) {
        return keepExistingAssetsAfterUpdateFailure(
            "Could not check remote asset manifest: " + manifestDownloadError);
    }
    if ( !manifest ) {
        return keepExistingAssetsAfterUpdateFailure(manifestParseError);
    }

    const auto localVersion = readLocalVersionFile(options.assetsRootPath);
    if ( !manifest->version.empty() && localVersion == manifest->version ) {
        XINFO(
            "AssetSync: local assets version {} is current; precise file "
            "verification skipped",
            localVersion);
        AssetSyncResult result;
        result.status        = AssetSyncStatus::kReady;
        result.remoteVersion = manifest->version;
        emitProgress(options,
                     AssetSyncProgress{
                         AssetSyncProgressStage::kFinished,
                         "Assets ready",
                         0,
                         0,
                         0,
                         0,
                     });
        return result;
    }

    auto updateResult = updateFromManifest(options, *manifest);
    if ( updateResult.status == AssetSyncStatus::kError ) {
        return keepExistingAssetsAfterUpdateFailure(updateResult.errorMessage);
    }
    return updateResult;
}

std::optional<AssetManifest> AssetSyncService::parseManifest(
    std::string_view manifestText, std::string& errorMessage)
{
    errorMessage.clear();

    const nlohmann::json data = nlohmann::json::parse(
        manifestText.begin(), manifestText.end(), nullptr, false);
    if ( data.is_discarded() || !data.is_object() ) {
        errorMessage = "Invalid asset manifest JSON";
        return std::nullopt;
    }

    AssetManifest manifest;
    manifest.version = jsonStringField(data, "version");

    const auto packageIter = data.find("package");
    if ( packageIter != data.end() && packageIter->is_object() ) {
        manifest.packageUrl = jsonStringField(*packageIter, "url");
        manifest.packageSha256 =
            normalizeSha256(jsonStringField(*packageIter, "sha256"));
    }

    const auto filesIter = data.find("files");
    if ( filesIter == data.end() || !filesIter->is_array() ) {
        errorMessage = "Asset manifest missing files array";
        return std::nullopt;
    }

    for ( const auto& fileJson : *filesIter ) {
        AssetFileEntry file;
        file.path   = jsonStringField(fileJson, "path");
        file.url    = jsonStringField(fileJson, "url");
        file.sha256 = normalizeSha256(jsonStringField(fileJson, "sha256"));
        file.size   = jsonUnsignedField(fileJson, "size");

        if ( !isSafeRelativeAssetPath(file.path) ) {
            errorMessage = "Unsafe asset path in manifest: " + file.path;
            return std::nullopt;
        }
        if ( file.url.empty() ) {
            errorMessage = "Asset file URL missing: " + file.path;
            return std::nullopt;
        }
        if ( file.sha256.empty() ) {
            errorMessage = "Invalid asset SHA256: " + file.path;
            return std::nullopt;
        }
        manifest.files.push_back(std::move(file));
    }

    if ( manifest.files.empty() ) {
        errorMessage = "Asset manifest has no files";
        return std::nullopt;
    }
    return manifest;
}

std::vector<AssetFileEntry> AssetSyncService::collectOutdatedFiles(
    const AssetManifest& manifest, const std::filesystem::path& assetsRootPath)
{
    std::vector<AssetFileEntry> outdatedFiles;
    for ( const auto& file : manifest.files ) {
        if ( isAssetFileOutdated(file, assetsRootPath) ) {
            outdatedFiles.push_back(file);
        }
    }
    return outdatedFiles;
}

bool AssetSyncService::extractZipArchive(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& destinationRoot, std::string& errorMessage)
{
    errorMessage.clear();

    std::vector<std::uint8_t> zipBytes;
    if ( !readFileBytes(zipPath, zipBytes) ) {
        errorMessage = "Failed to read asset package";
        return false;
    }

    mz_zip_archive zipArchive{};
    if ( !mz_zip_reader_init_mem(
             &zipArchive, zipBytes.data(), zipBytes.size(), 0) ) {
        errorMessage = "Failed to open asset package";
        return false;
    }

    bool          success   = true;
    const mz_uint fileCount = mz_zip_reader_get_num_files(&zipArchive);
    for ( mz_uint index = 0; index < fileCount; ++index ) {
        mz_zip_archive_file_stat fileStat{};
        if ( !mz_zip_reader_file_stat(&zipArchive, index, &fileStat) ) {
            errorMessage = "Failed to read asset package entry";
            success      = false;
            break;
        }

        std::string archiveName = fileStat.m_filename;
        while ( !archiveName.empty() && archiveName.back() == '/' ) {
            archiveName.pop_back();
        }
        if ( archiveName.empty() ) continue;

        if ( !isSafeRelativeAssetPath(archiveName) ) {
            errorMessage = "Unsafe path in asset package: " + archiveName;
            success      = false;
            break;
        }

        const auto destinationPath =
            (destinationRoot / Config::utf8ToPath(archiveName))
                .lexically_normal();
        if ( !isPathInsideRoot(destinationRoot, destinationPath) ) {
            errorMessage =
                "Asset package path escapes destination: " + archiveName;
            success = false;
            break;
        }

        if ( mz_zip_reader_is_file_a_directory(&zipArchive, index) ) {
            std::error_code createError;
            std::filesystem::create_directories(destinationPath, createError);
            if ( createError ) {
                errorMessage = createError.message();
                success      = false;
                break;
            }
            continue;
        }

        std::size_t extractedSize = 0;
        void*       extractedData = mz_zip_reader_extract_to_heap(
            &zipArchive, index, &extractedSize, 0);
        if ( !extractedData ) {
            errorMessage =
                "Failed to extract asset package entry: " + archiveName;
            success = false;
            break;
        }

        success =
            writeBytesToFile(destinationPath, extractedData, extractedSize);
        mz_free(extractedData);
        if ( !success ) {
            errorMessage = "Failed to write asset file: " + archiveName;
            break;
        }
    }

    mz_zip_reader_end(&zipArchive);
    return success;
}

std::string AssetSyncService::sha256File(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    if ( !readFileBytes(path, bytes) ) return {};
    return sha256Bytes(bytes);
}

}  // namespace MMM::Network
