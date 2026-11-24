#include "network/AssetSyncService.h"
#include "AssetPathSafety.h"
#include "config/AppPaths.h"
#include "config/StagedFile.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "network/CurlHandle.h"
#include "network/DownloadRouting.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <expected>
#include <fstream>
#include <functional>
#include <limits>
#include <mbedtls/sha256.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>

namespace MMM::Network
{

namespace
{

/// @brief 默认资源 CDN 根 URL。
/// @note 仅在环境变量未覆盖时使用，所有站内相对地址都以此补全。
constexpr const char* kDefaultAssetBaseUrl = "https://downloads.xiang233.top";

/// @brief 默认资源清单路径。
/// @note 清单优先于完整包，已有资源目录可据此执行逐文件校验与更新。
constexpr const char* kDefaultManifestPath = "/download/assets/manifest.json";

/// @brief 默认完整资源包路径。
/// @note 只在本地资源目录不存在时作为引导安装回退。
constexpr const char* kDefaultPackagePath = "/download/assets.zip";

/// @brief 本地记录资源版本的文件名。
/// @note 版本相等时可跳过昂贵逐文件哈希，强制精确校验选项会忽略该捷径。
constexpr const char* kLocalVersionFileName = ".mmm-assets-version";

/// @brief 下载资源时使用的 User-Agent。
/// @note 固定标识便于服务端区分资源同步流量和普通浏览器请求。
constexpr const char* kAssetUserAgent = "MusicMapMaker-AssetSync/1.0";

/// @brief 资源校验的固定读取粒度，也是取消查询之间最多处理的字节数。
constexpr std::size_t ASSET_HASH_READ_BYTES = 32U * 1024U;

/// @brief libcurl 字符串写回调。
/// @details 响应可能分多次到达，按显式字节数追加到调用方字符串；返回完整消费
/// 长度，短写语义交由 libcurl 处理。
/// @param contents 当前响应分片。
/// @param size 单元素字节数。
/// @param nmemb 元素数量。
/// @param userp 响应字符串指针。
/// @return 已追加字节总数。
std::size_t writeStringCallback(void* contents, std::size_t size,
                                std::size_t nmemb, void* userp)
{
    const std::size_t totalSize = size * nmemb;
    // 正文不保证零结尾，必须使用 libcurl 提供的长度。
    auto* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

/// @brief libcurl 文件写回调。
/// @details FILE 生命周期由 downloadFile 管理；直接返回 fwrite 的元素数，使磁盘
/// 短写被 libcurl 转换为传输失败。
std::size_t writeFileCallback(void* contents, std::size_t size,
                              std::size_t nmemb, void* userp)
{
    return std::fwrite(contents, size, nmemb, static_cast<FILE*>(userp));
}

/// @brief libcurl 下载进度状态。
/// @details 两个回调均为可选值，结构体地址在 curl_easy_perform 返回前保持稳定。
struct DownloadProgressState {
    /// @brief 向上层发布当前下载字节和服务端总字节。
    std::function<void(std::int64_t, std::int64_t)> callback;

    /// @brief 返回 true 时要求 libcurl 尽快中断传输。
    std::function<bool()> cancellationCallback;
};

/// @brief libcurl 下载进度回调。
/// @details 取消检查优先于进度发布，避免任务失效后继续触碰上层状态；返回非零
/// 触发 CURLE_ABORTED_BY_CALLBACK。
int downloadProgressCallback(void* clientp, curl_off_t dltotal,
                             curl_off_t dlnow, curl_off_t /*ultotal*/,
                             curl_off_t /*ulnow*/)
{
    auto* state = static_cast<DownloadProgressState*>(clientp);
    if ( state && state->cancellationCallback &&
         state->cancellationCallback() ) {
        // 取消只通知 libcurl 停止，临时文件由 downloadFile 统一删除。
        return 1;
    }
    if ( state && state->callback ) {
        // curl_off_t 在受支持平台可安全收窄到 API 约定的 int64_t。
        state->callback(static_cast<std::int64_t>(dlnow),
                        static_cast<std::int64_t>(dltotal));
    }
    return 0;
}

/// @brief 判断字符串是否以指定前缀开头。
/// @return value 长度足够且起始字节与 prefix 完全一致时返回 true。
bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

/// @brief 判断字符串是否是绝对 URL。
/// @details 同时允许 HTTP、HTTPS 和测试使用的 file URL；其他协议不进入下载器。
bool isAbsoluteUrl(std::string_view url)
{
    return startsWith(url, "http://") || startsWith(url, "https://") ||
           startsWith(url, "file://");
}

/// @brief 移除 URL 末尾的斜杠。
/// @details 连续尾斜杠全部移除，后续拼接统一只插入一个分隔符。
std::string trimTrailingSlash(std::string value)
{
    while ( !value.empty() && value.back() == '/' ) {
        value.pop_back();
    }
    return value;
}

/// @brief 将远程 URL 规格化为绝对 URL。
/// @details 空值和已有绝对协议原样返回；站点根路径直接附加到去尾斜杠 base，
/// 普通相对路径则补一个 `/`。函数不解析或重写查询参数。
std::string resolveUrlInternal(const std::string& baseUrl,
                               const std::string& url)
{
    if ( url.empty() || isAbsoluteUrl(url) ) return url;

    // 统一 base 末尾后再区分根相对与普通相对路径。
    const std::string base = trimTrailingSlash(baseUrl);
    if ( url.front() == '/' ) return base + url;
    return base + "/" + url;
}

/// @brief 从 file URL 中提取本地路径。
/// @details 仅去除固定 scheme 并执行 UTF-8 路径转换，调用方仍负责文件存在检查。
std::filesystem::path pathFromFileUrl(const std::string& url)
{
    constexpr std::string_view kFilePrefix = "file://";
    if ( !startsWith(url, kFilePrefix) ) return {};
    return Config::utf8ToPath(url.substr(kFilePrefix.size()));
}

/// @brief 读取环境变量字符串。
/// @details 空值视为未配置，使 defaultOptions 回落到官方地址。
std::string readEnvironmentString(const char* name)
{
    const char* value = std::getenv(name);
    if ( value && value[0] != '\0' ) return value;
    return {};
}

/// @brief 发送资源同步进度事件。
/// @details 没有注册回调时为零操作；事件在执行同步的调用线程直接送达。
/// @note 回调不得假设运行在 UI 线程，跨线程展示由调用方自行调度。
/// @note progress 是只读快照，回调返回后同步流程不会保留其引用。
/// @note 阶段枚举与文本共同描述进度，业务判断应优先使用枚举。
/// @note 取消查询与进度发布相互独立，回调不能替代 cancellationCallback。
void emitProgress(const AssetSyncOptions&  options,
                  const AssetSyncProgress& progress)
{
    if ( options.progressCallback ) options.progressCallback(progress);
}

/// @brief 判断调用方是否已经请求取消资源同步。
/// @details 回调缺失代表不可取消，不引入额外共享原子或全局状态。
/// @param options 当前同步选项。
/// @return 已请求取消时返回 true。
bool isCancellationRequested(const AssetSyncOptions& options)
{
    return options.cancellationCallback && options.cancellationCallback();
}

/// @brief 构造统一的资源同步取消结果。
/// @details 所有阶段使用同一状态和错误文本，调用方不需要区分取消发生位置。
/// @return 状态为 kCancelled 的结果。
AssetSyncResult cancelledResult()
{
    AssetSyncResult result;
    result.status       = AssetSyncStatus::kCancelled;
    result.errorMessage = "Asset sync cancelled";
    return result;
}

/// @brief 判断字符是否为十六进制数字。
/// @return ASCII 数字或大小写 a～f 时返回 true。
bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/// @brief 将 SHA256 字符串归一化为 64 位小写十六进制。
/// @details 接受可选小写 `sha256:` 前缀，正文必须恰好 64 位且全为十六进制；
/// 成功结果统一小写以便直接比较。
std::string normalizeSha256(std::string value)
{
    if ( startsWith(value, "sha256:") ) value.erase(0, 7);
    // SHA-256 固定为 32 字节，即 64 个十六进制字符。
    if ( value.size() != 64 ) return {};

    for ( char& c : value ) {
        // 非法字符立即使整份摘要失效，不能跳过校验。
        if ( !isHexDigit(c) ) return {};
        if ( c >= 'A' && c <= 'F' ) {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

/// @brief 读取 JSON 对象中的字符串字段。
/// @details 远端字段缺失或类型不符统一返回空值，由 parseManifest 决定必需性。
std::string jsonStringField(const nlohmann::json& value, const char* key)
{
    if ( !value.is_object() ) return {};
    const auto iter = value.find(key);
    if ( iter == value.end() || !iter->is_string() ) return {};
    return iter->get<std::string>();
}

/// @brief 读取 JSON 对象中的无符号整数字段。
/// @details 接受原生 unsigned 和正 signed 值以兼容 JSON 解析表示；零、负值、
/// 缺失或其他类型统一返回零。
std::uint64_t jsonUnsignedField(const nlohmann::json& value, const char* key)
{
    if ( !value.is_object() ) return 0;
    const auto iter = value.find(key);
    if ( iter == value.end() ) return 0;
    if ( iter->is_number_unsigned() ) return iter->get<std::uint64_t>();
    if ( iter->is_number_integer() ) {
        // 先检查正值再转换，避免负数绕回超大 uint64_t。
        const auto signedValue = iter->get<std::int64_t>();
        if ( signedValue > 0 ) return static_cast<std::uint64_t>(signedValue);
    }
    return 0;
}

/// @brief 向文件写入字节。
/// @details 先创建父目录，再以二进制截断模式完整写入；用于 file URL 本地复制，
/// 不保留旧文件尾部。
/// @note 下载调用者传入暂存路径，摘要通过后才替换已安装文件。
bool writeBytesToFile(const std::filesystem::path& path, const void* data,
                      std::size_t size)
{
    const auto parentPath = path.parent_path();
    // 目标位于当前目录时 parent 为空，无需创建目录。
    if ( !parentPath.empty() ) {
        std::error_code createError;
        std::filesystem::create_directories(parentPath, createError);
        if ( createError ) return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    if ( size > 0 ) {
        // 空文件避免向流传入可能为空的数据指针。
        file.write(static_cast<const char*>(data),
                   static_cast<std::streamsize>(size));
    }
    // 明确完成关闭，下载进度只在本地文件真正写完后报告完成。
    file.close();
    return file.good();
}

/// @brief 读取完整文件。
/// @details 先用 file_size 决定缓冲区，再执行一次精确二进制读取；失败时输出缓冲
/// 已清空或仅含不可信部分，调用方必须依据 false 丢弃。
bool readFileBytes(const std::filesystem::path& path,
                   std::vector<std::uint8_t>&   bytes)
{
    bytes.clear();
    // 清除调用方旧内容，任何早退都不会误用上次成功结果。

    std::error_code fileSizeError;
    const auto      fileSize = std::filesystem::file_size(path, fileSizeError);
    if ( fileSizeError ) return false;

    std::ifstream file(path, std::ios::binary);
    if ( !file ) return false;

    bytes.resize(static_cast<std::size_t>(fileSize));
    // 空文件是合法输入，不需要调用 read。
    if ( !bytes.empty() ) {
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    return file.good() || (bytes.empty() && file.eof());
}

/// @brief 校验未完成时，区分读取或计算失败与主动取消。
enum class AssetDigestError {
    Failed,     ///< 文件或摘要无效，调用方需按校验失败处理。
    Cancelled,  ///< 调用方请求中止，禁止生成更新计划或发布文件。
};

/// @brief 顺序读取普通文件并生成规范化 SHA-256；取消会丢弃部分摘要。
/// @param path 待读取的文件。
/// @param cancellationCallback 可选取消查询，在首块和各块之间执行。
/// @return 完整小写摘要，或用于调用方区分取消与失败的错误。
/// @warning 后台资源同步路径，执行文件读取及摘要计算；不在 UI 热路径使用。
/// @note 不把未完成的摘要交给调用方；读取错误、意外短读和取消都不进入
/// finalize。
/// @note 一个固定 32 KiB 块在所有迭代间复用，不随文件大小增长。
std::expected<std::string, AssetDigestError> hashAssetFile(
    const std::filesystem::path& path,
    const std::function<bool()>& cancellationCallback = {})
{
    if ( cancellationCallback && cancellationCallback() )
        return std::unexpected(AssetDigestError::Cancelled);

    // 记录读取前大小，只用来验证读取完整性，不据此申请内存。
    // SHA-256 的原始长度字段最多 64 位，避免极端文件尺寸导致位数回卷。
    std::error_code sizeError;
    auto            remaining = std::filesystem::file_size(path, sizeError);
    if ( sizeError ||
         remaining > std::numeric_limits<std::uint64_t>::max() / 8U )
        return std::unexpected(AssetDigestError::Failed);

    // 资源清单签的是原始字节，Windows 也必须绕开文本模式的换行转换。
    std::ifstream input(path, std::ios::binary);
    if ( !input ) return std::unexpected(AssetDigestError::Failed);

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    // 上下文由本函数在栈上持有，释放器覆盖读取失败、取消和正常完成。
    const std::unique_ptr<mbedtls_sha256_context,
                          decltype(&mbedtls_sha256_free)>
        cleanup(&context, &mbedtls_sha256_free);
    // 参数零选择清单所需的 256 位模式；SHA-224 也使用同一套更新接口。
    if ( mbedtls_sha256_starts(&context, 0) != 0 )
        return std::unexpected(AssetDigestError::Failed);

    // 每轮只读一块，短尾块也用 gcount 输入；EOF 本身不代表文件完整。
    // 比较预期剩余字节数可以识别读取期间截短或增长的文件。
    std::array<unsigned char, ASSET_HASH_READ_BYTES> block;
    // 整块文件也要再读取一次以确认 EOF；不能仅凭已读大小忽略后来追加的数据。
    do {
        input.read(reinterpret_cast<char*>(block.data()),
                   static_cast<std::streamsize>(block.size()));
        // 在处理本块之前查询，从而覆盖最后一次读取到摘要完成之间的取消。
        if ( cancellationCallback && cancellationCallback() )
            return std::unexpected(AssetDigestError::Cancelled);

        // 错误与正常短尾块分开处理，防止对损坏输入只哈希前面连续可读的字节。
        const auto count = static_cast<std::size_t>(input.gcount());
        if ( input.bad() || (input.fail() && !input.eof()) ||
             count > remaining )
            return std::unexpected(AssetDigestError::Failed);
        if ( count != 0 &&
             mbedtls_sha256_update(&context, block.data(), count) != 0 )
            return std::unexpected(AssetDigestError::Failed);
        remaining -= count;
    } while ( !input.eof() );

    // 流可对被截短的文件报告正常 EOF；只有字节计数才能区分这种短读。
    if ( remaining != 0 ) return std::unexpected(AssetDigestError::Failed);

    // 只在完整消费输入后结束摘要。库在此追加末尾填充，不需要复制文件。
    std::array<unsigned char, 32> digest;
    if ( mbedtls_sha256_finish(&context, digest.data()) != 0 )
        return std::unexpected(AssetDigestError::Failed);

    // 两个十六进制字符固定对应一个摘要字节，不依赖进程区域设置。
    constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::string    result(digest.size() * 2U, '0');
    for ( std::size_t index = 0; index < digest.size(); ++index ) {
        result[index * 2U]      = HEX_DIGITS[digest[index] >> 4U];
        result[index * 2U + 1U] = HEX_DIGITS[digest[index] & 0x0fU];
    }
    return result;
}

/// @brief 下载文本响应。
/// @details
/// file URL 用于离线测试，直接读取本地文件；HTTP(S) 使用 libcurl 跟随重定向，
/// 固定连接和总超时并可通过进度回调中断。成功时 response 完整替换，失败时在
/// errorMessage 中返回稳定原因。
/// @par 结果分类
/// - 开始前或传输中取消返回 `Asset sync cancelled`。
/// - file URL 读取失败返回本地文件错误。
/// - libcurl 初始化或传输失败返回网络错误。
/// - HTTP 非 200 状态返回明确状态码。
/// - 非 HTTP 协议的零 response code 不视为错误。
/// - 成功时 response 可为空，代表合法空文件。
/// @param url 绝对 HTTP(S) 或 file URL。
/// @param response 接收完整响应字节。
/// @param errorMessage 接收失败详情。
/// @param cancellationCallback 可选取消查询。
/// @return 成功读取且 HTTP 状态可接受时返回 true。
bool downloadString(const std::string& url, std::string& response,
                    std::string&                 errorMessage,
                    const std::function<bool()>& cancellationCallback)
{
    response.clear();
    // 任务开始前取消时不打开文件或网络句柄。

    if ( cancellationCallback && cancellationCallback() ) {
        errorMessage = "Asset sync cancelled";
        return false;
    }

    if ( startsWith(url, "file://") ) {
        // 本地路径仍经统一 UTF-8 转换和完整文件读取。
        std::vector<std::uint8_t> bytes;
        if ( !readFileBytes(pathFromFileUrl(url), bytes) ) {
            errorMessage = "Failed to read local file URL";
            return false;
        }
        if ( bytes.empty() ) {
            // 避免以空 vector data 构造字符串。
            response.clear();
        } else {
            response.assign(reinterpret_cast<const char*>(bytes.data()),
                            bytes.size());
        }
        return true;
    }

    CURL* curl = createCurlHandle();
    if ( !curl ) {
        errorMessage = "Failed to initialize libcurl";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // 资源站点可经 CDN 重定向，User-Agent 保持稳定便于服务端诊断。
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kAssetUserAgent);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    DownloadProgressState progressState{ {}, cancellationCallback };
    // 文本下载只有需要取消时才启用进度回调，减少无用回调开销。
    if ( cancellationCallback ) {
        curl_easy_setopt(
            curl, CURLOPT_XFERINFOFUNCTION, downloadProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressState);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    const CURLcode result = curl_easy_perform(curl);
    // 在释放句柄前取得 HTTP 状态，file 等非 HTTP 协议通常返回零。
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if ( result == CURLE_ABORTED_BY_CALLBACK && cancellationCallback &&
         cancellationCallback() ) {
        errorMessage = "Asset sync cancelled";
        // 主动取消与网络故障分开报告，供同步主流程返回 kCancelled。
        return false;
    }
    if ( result != CURLE_OK ) {
        errorMessage =
            std::string("Network error: ") + curl_easy_strerror(result);
        return false;
    }
    if ( httpCode != 0 && httpCode != 200 ) {
        // 非 HTTP 的零状态合法，HTTP 仅接受完整成功码 200。
        errorMessage = "HTTP error: " + std::to_string(httpCode);
        return false;
    }
    return true;
}

/// @brief 以二进制形式下载文件。
/// @details
/// file URL 路径完整读入后写到目标，便于测试走与网络相同的进度/取消接口；
/// HTTP(S) 则流式写入目标文件，失败或取消时删除不完整输出。父目录在打开前创建，
/// libcurl 回调同时承担进度发布和取消检查。
/// @par 临时文件所有权
/// - 开始前取消不创建或截断目标。
/// - file URL 在源完整读取后才创建目标。
/// - 网络路径在传输前截断目标，失败时负责删除。
/// - FILE 与 CURL 句柄在检查传输结果前关闭。
/// - 成功文件仍是未认证临时制品，调用方必须执行摘要校验。
/// @par 进度语义
/// - 本地复制先报告零完成和已知总量。
/// - 网络下载沿用 libcurl 提供的当前量与总量。
/// - 完成回调只在目标完整写入后发布。
/// - 取消检查可以在没有展示进度回调时独立启用。
/// @param url 绝对下载地址。
/// @param path 目标临时文件路径。
/// @param errorMessage 接收失败详情。
/// @param progressCallback 可选下载字节进度回调。
/// @param cancellationCallback 可选取消查询。
/// @param expectedSize 清单总长，已知小文件跳过独立测速，未知为零。
/// @return 文件完整写入时返回 true。
bool downloadFile(
    const std::string& url, const std::filesystem::path& path,
    std::string&                                           errorMessage,
    const std::function<void(std::int64_t, std::int64_t)>& progressCallback,
    const std::function<bool()>&                           cancellationCallback,
    std::uint64_t                                          expectedSize = 0)
{
    // 开始前取消不触碰目标路径。
    if ( cancellationCallback && cancellationCallback() ) {
        errorMessage = "Asset sync cancelled";
        return false;
    }

    if ( startsWith(url, "file://") ) {
        // 本地测试路径先完整读取，确保源读取失败不会截断目标。
        std::vector<std::uint8_t> bytes;
        if ( !readFileBytes(pathFromFileUrl(url), bytes) ) {
            errorMessage = "Failed to read local file URL";
            return false;
        }
        if ( progressCallback ) {
            // 首次回调发布已知总量和零完成量。
            progressCallback(0, static_cast<std::int64_t>(bytes.size()));
        }
        if ( cancellationCallback && cancellationCallback() ) {
            // 写目标前再检查一次，避免取消后仍产生输出。
            errorMessage = "Asset sync cancelled";
            return false;
        }
        if ( !writeBytesToFile(path, bytes.data(), bytes.size()) ) {
            errorMessage = "Failed to create output file";
            return false;
        }
        if ( progressCallback ) {
            // 写入成功后发布完成进度，与网络路径语义一致。
            progressCallback(static_cast<std::int64_t>(bytes.size()),
                             static_cast<std::int64_t>(bytes.size()));
        }
        return true;
    }

    const auto parentPath = path.parent_path();
    // 网络路径需要在 fopen 前显式建立目标父目录。
    if ( !parentPath.empty() ) {
        std::error_code createError;
        std::filesystem::create_directories(parentPath, createError);
        if ( createError ) {
            errorMessage = createError.message();
            return false;
        }
    }

#ifdef _WIN32
    // Windows 使用宽路径 API 保留非 ASCII 临时目录。
    FILE* file = _wfopen(path.wstring().c_str(), L"wb");
#else
    FILE* file = std::fopen(path.c_str(), "wb");
#endif
    if ( !file ) {
        errorMessage = "Failed to create output file";
        return false;
    }

    CURL* curl = createCurlHandle();
    if ( !curl ) {
        // curl 初始化失败时先关闭已创建文件；空文件由调用方错误处理覆盖。
        std::fclose(file);
        errorMessage = "Failed to initialize libcurl";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileCallback);
    // 响应分片直接落盘，避免完整资源包占用内存。
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    // HTTP 错误页不得作为资源文件成功保存。
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kAssetUserAgent);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);

    DownloadProgressState progressState{ progressCallback,
                                         cancellationCallback };
    // 任一上层回调存在时启用 libcurl 进度机制。
    if ( progressCallback || cancellationCallback ) {
        curl_easy_setopt(
            curl, CURLOPT_XFERINFOFUNCTION, downloadProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressState);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    // 官方大文件共享按吞吐量选择线路与续传策略；清单和本地 file URL
    // 保留原路径。
    DownloadRoutingOptions routing;
    routing.m_cancelled = cancellationCallback;
    routing.m_progress  = progressCallback;
    // 增量文件已知总长，避免为小图标或脚本重复读取探测前缀。
    routing.m_expectedSize = expectedSize;
    const CURLcode result  = performRoutedDownload(curl, url, file, routing);

    // 先关闭文件和 curl，随后处理失败并删除部分输出。
    // libcurl 只知道回调接收的数据量，标准库缓冲区可能在关闭时才报磁盘错误。
    const bool closed = std::fclose(file) == 0;
    curl_easy_cleanup(curl);

    if ( result == CURLE_ABORTED_BY_CALLBACK && cancellationCallback &&
         cancellationCallback() ) {
        errorMessage = "Asset sync cancelled";
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        // 删除失败不覆盖取消主语义，临时路径不会被发布给后续流程。
        return false;
    }
    if ( result != CURLE_OK || !closed ) {
        errorMessage =
            result != CURLE_OK
                ? std::string("Download error: ") + curl_easy_strerror(result)
                : "Could not close downloaded asset file";
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        // 任何传输错误都不得保留可被摘要阶段误读的不完整文件。
        return false;
    }
    return true;
}

/// @brief 找到临时下载根，错误直接由同步结果报告。
/// @details 下载目录由调用方排他创建，绝不共享系统临时根下的固定文件名。
/// @param errorMessage 临时根不可查询时接收诊断；成功时保持原内容。
/// @return 解析出的临时根；错误时为空。
/// @details 一个同步可能和多个其他启动操作重叠。下载、摘要验证、解压或取消
/// 的任何阶段都只能访问已经取得排他所有权的路径。
std::filesystem::path temporaryDownloadRoot(std::string& errorMessage)
{
    std::error_code error;
    // 系统临时根不可确定时不尝试从进程工作目录推断位置。
    // 下载可能在用户资料之外运行，工作目录不属于本轮同步的清理范围。
    auto root = std::filesystem::temp_directory_path(error);
    if ( error )
        errorMessage =
            "Could not locate asset download directory: " + error.message();
    return root;
}

/// @brief 将已下载且校验的文件复制到目标卷、复验并原子发布。
/// @details 直接覆盖复制会在写入中途损毁旧文件。暂存文件与正式路径同卷，只有
/// 完整复验、最终目录检查和取消检查都通过，才进入单次重命名。
/// @param source 独占下载文件，已通过清单摘要校验。
/// @param root 用户选择并解析过的安装根。
/// @param relative 已验证的清单路径，下载来源不决定最终文件名。
/// @param file 提供复验摘要以及诊断所用的原路径文本。
/// @param options 提供本轮取消查询。
/// @param errorMessage 接收目录拒绝、暂存失败、摘要不符或发布失败原因。
/// @return 完整的暂存内容已完成重命名时为真。
bool installDownloadedAsset(const std::filesystem::path& source,
                            const std::filesystem::path& root,
                            const std::filesystem::path& relative,
                            const AssetFileEntry&        file,
                            const AssetSyncOptions&      options,
                            std::string&                 errorMessage)
{
    std::error_code error;
    // 前后两次复用同一检查，确保传输回调结束后仍以原目标边界为准。
    const auto checkDestination = [&] {
        if ( AssetPaths::isUnredirectedDestination(root, relative, error) )
            return true;
        errorMessage =
            "Unsafe asset destination: " + file.path + ": " + error.message();
        return false;
    };
    if ( !checkDestination() ) return false;
    // 清单路径原样转换为原生路径，安装不改变文件名大小写。
    const auto destination = root / relative;
    const auto parent      = destination.parent_path();
    std::filesystem::create_directories(parent, error);
    if ( error ) {
        errorMessage = error.message();
        return false;
    }

    // 在确认真实父目录后才建立独占暂存。与系统下载目录相比，
    // 这个目录保证与最终文件位于同卷，安装边界不依赖跨卷重命名。
    const Config::ScopedTemporaryDirectory staging(parent,
                                                   ".mmm-asset-install-");
    if ( staging.error() ) {
        errorMessage = staging.error().message();
        return false;
    }
    // 使用实现内部文件名。copy_file 采用默认拒绝覆盖语义，
    // 如果意外出现同名文件，不会写进已有条目。
    const auto staged = staging.path() / "content.tmp";
    // 允许下载卷与安装卷不同。任何写入失败只影响当前任务的独占文件。
    if ( !std::filesystem::copy_file(source, staged, error) ) {
        errorMessage =
            "Could not stage asset file: " + file.path + ": " + error.message();
        return false;
    }
    // 复制成功还需要复验目标卷的字节，源 ZIP 或下载目录的摘要
    // 不能替代对即将提交的文件的确认。
    const auto digest = hashAssetFile(staged, options.cancellationCallback);
    if ( !digest || *digest != file.sha256 ) {
        errorMessage = "Staged asset file checksum failed: " + file.path;
        return false;
    }
    // 校验回调可能运行用户代码，提交前再检查取消和最终路径。
    if ( isCancellationRequested(options) || !checkDestination() ) return false;
    // 最后一个操作才写正式文件名。遇到外部占用和其他发布错误时，
    // 原路径仍可读取完整内容，后续启动可以重新下载。
    std::filesystem::rename(staged, destination, error);
    if ( error ) errorMessage = error.message();
    return !error;
}

/// @brief 原子保存本地资源版本文件；空版本沿用原标记。
/// @details 版本在正文全部校验成功后提交。写入、关闭或替换失败都保留已有标记。
/// @param assetsRootPath 已安装的资源根。
/// @param version 来自完整包或增量清单的版本。
/// @param errorMessage 失败时接收标记路径和 I/O 诊断。
/// @return 不需要刷新标记或已成功发布时为真。
bool writeLocalVersionFile(const std::filesystem::path& assetsRootPath,
                           const std::string&           version,
                           std::string&                 errorMessage)
{
    if ( version.empty() ) return true;

    std::error_code error;
    const auto      root = AssetPaths::resolveRoot(assetsRootPath, error);
    const auto      path = root / kLocalVersionFileName;
    // 版本标记同样不能跟随安装树内的重定向；失败时不改变任何已有版本。
    if ( !error && AssetPaths::isUnredirectedDestination(
                       root, kLocalVersionFileName, error) ) {
        Config::StagedOutputFile output(path, std::ios::binary);
        if ( output.stream().is_open() ) {
            output.stream() << version << '\n';
            if ( output.commit() ) return true;
        }
        // 最先失败的 I/O 步骤由暂存对象保留，已校验的正文不回滚。
        error = output.error();
    }
    errorMessage = "Could not save asset version marker " +
                   Config::pathToUtf8(assetsRootPath / kLocalVersionFileName) +
                   ": " + error.message();
    return false;
}

/// @brief 资源验证完成后提交版本，再发布成功终态。
/// @details 版本写回失败保留更新计数并单独返回，启动端可以验证并使用现有资源，
/// 下次同步则会重新对照清单。失败不发出成功的 Finished 事件。
AssetSyncResult finishAssetSync(const AssetSyncOptions& options,
                                AssetSyncResult result, AssetSyncStatus status)
{
    if ( !writeLocalVersionFile(options.assetsRootPath,
                                result.remoteVersion,
                                result.errorMessage) ) {
        result.status = AssetSyncStatus::kVersionWriteFailed;
        return result;
    }
    result.status = status;
    emitProgress(options,
                 { AssetSyncProgressStage::kFinished, "Assets ready" });
    return result;
}

/// @brief 读取本地资源版本文件。
/// @details 只读取首行并移除 CR/LF，文件缺失或不可读视为未知版本。
std::string readLocalVersionFile(const std::filesystem::path& assetsRootPath)
{
    const auto    versionPath = assetsRootPath / kLocalVersionFileName;
    std::ifstream file(versionPath, std::ios::binary);
    if ( !file ) return {};

    std::string version;
    std::getline(file, version);
    // 同时兼容 Windows CRLF 和 POSIX LF 版本文件。
    while ( !version.empty() &&
            (version.back() == '\r' || version.back() == '\n') ) {
        version.pop_back();
    }
    return version;
}

/// @brief 判断本地资源文件是否缺失或内容不匹配。
/// @details
/// 先验证清单相对路径和根目录边界，再检查普通文件、可选大小及最终 SHA-256。
/// 任一文件系统错误都保守视为过期，避免跳过需要修复的资源。
/// @return 文件是否需要更新；空值表示校验期间取消。
std::optional<bool> isAssetFileOutdated(
    const AssetFileEntry& file, const std::filesystem::path& assetsRootPath,
    const std::function<bool()>& cancellationCallback = {})
{
    if ( !AssetPaths::isSafeRelativeAssetPath(file.path) ) return true;

    const auto localPath =
        (assetsRootPath / Config::utf8ToPath(file.path)).lexically_normal();
    if ( !AssetPaths::isPathInsideRoot(assetsRootPath, localPath) ) return true;
    // 清单已检查过路径，仍以组合后的本地路径执行第二层逃逸防御。

    std::error_code regularFileError;
    const bool      regularFile =
        std::filesystem::is_regular_file(localPath, regularFileError);
    if ( regularFileError || !regularFile ) return true;

    if ( file.size > 0 ) {
        // 清单大小非零时可在哈希前快速排除截断或旧版本文件。
        std::error_code sizeError;
        const auto localSize = std::filesystem::file_size(localPath, sizeError);
        if ( sizeError || localSize != file.size ) return true;
    }

    const auto localSha256 = hashAssetFile(localPath, cancellationCallback);
    // 中途取消必须逐层传播，不能把它当成过期文件并开始下载。
    if ( !localSha256 && localSha256.error() == AssetDigestError::Cancelled )
        return std::nullopt;
    return !localSha256 || *localSha256 != file.sha256;
}

/// @brief 收集本地缺失或变更文件，并向启动 UI 汇报校验进度。
/// @details 按清单顺序逐项检查，每项开始前响应取消并发布当前文件索引；返回值
/// 保持清单顺序，供后续下载进度使用稳定序号。
/// @return 完整更新计划；取消后废弃任何部分结果。
std::optional<std::vector<AssetFileEntry>> collectOutdatedFilesWithProgress(
    const AssetManifest& manifest, const std::filesystem::path& assetsRootPath,
    const AssetSyncOptions& options)
{
    std::vector<AssetFileEntry> outdatedFiles;
    for ( std::size_t index = 0; index < manifest.files.size(); ++index ) {
        // 取消是本次检查的终态，不需要调用方再次查询同一回调。
        if ( isCancellationRequested(options) ) return std::nullopt;

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
        const auto outdated = isAssetFileOutdated(
            file, assetsRootPath, options.cancellationCallback);
        // 取消后即使已找到其他过期文件，也不能发布部分更新计划。
        if ( !outdated ) return std::nullopt;
        if ( *outdated ) outdatedFiles.push_back(file);
    }
    return outdatedFiles;
}

/// @brief 下载并解压完整资源包。
/// @details
/// 本地资源目录尚不存在时，优先采用清单声明的包地址与摘要；清单不可用时回落
/// 到 options.packageUrl。下载始终落到临时 ZIP，存在摘要时先校验，再解压到
/// assetsRootPath 的父目录，使归档内顶层 `assets/` 建立目标目录。
/// 成功后删除临时包、写入远端版本并发布 kDownloaded；取消与错误保持不同终态。
/// @par 阶段顺序
/// - 解析清单覆盖的包地址、摘要和远端版本。
/// - 把相对包地址补全为绝对下载 URL。
/// - 下载到专用临时 ZIP 并持续发布进度。
/// - 若有摘要则在解压前验证完整 ZIP。
/// - 在根目录父级安全解压全部归档条目。
/// - 删除临时 ZIP 并写入本地版本标记。
/// - 版本保存成功后发布 Finished 和 kDownloaded，失败则单独报告标记错误。
/// @par 失败约束
/// 任何取消、下载、摘要或解压失败都不能写入版本标记；未经摘要验证的 ZIP
/// 绝不能进入解压器，错误结果保留具体阶段详情。
/// @param options 资源根、地址、进度和取消配置。
/// @param manifest 可选的已验证远端清单。
/// @return 完整包安装结果及远端版本、更新文件数量。
AssetSyncResult downloadFullPackage(const AssetSyncOptions& options,
                                    const AssetManifest*    manifest)
{
    // 默认结果保持零计数，只有完成安装后才发布下载状态。
    AssetSyncResult result;

    std::string packageUrl    = options.packageUrl;
    std::string packageSha256 = {};
    if ( manifest ) {
        // 清单版本写入最终本地标记，清单包地址优先于静态 options 回退。
        result.remoteVersion = manifest->version;
        if ( !manifest->packageUrl.empty() ) packageUrl = manifest->packageUrl;
        packageSha256 = manifest->packageSha256;
    }

    packageUrl =
        AssetSyncService::resolveDownloadUrl(options.baseUrl, packageUrl);

    // 空包地址意味着既无清单制品也无调用方回退，不能创建资源目录。
    if ( packageUrl.empty() ) {
        result.status       = AssetSyncStatus::kError;
        result.errorMessage = "No asset package URL configured";
        return result;
    }

    const auto tempRoot = temporaryDownloadRoot(result.errorMessage);
    if ( tempRoot.empty() ) {
        result.status = AssetSyncStatus::kError;
        return result;
    }
    const Config::ScopedTemporaryDirectory download(tempRoot,
                                                    ".mmm-asset-download-");
    if ( download.error() ) {
        result.status       = AssetSyncStatus::kError;
        result.errorMessage = download.error().message();
        return result;
    }
    // 固定 ZIP 名只在本轮目录内使用。退出时统一回收，
    // 包括下载失败、摘要出错和解压部分条目后取消。
    const auto tempZipPath = download.path() / "assets.zip";
    // 下载阶段先发布无总量进度，libcurl 回调获得总长后再更新字节字段。
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

    if ( !downloadFile(
             packageUrl,
             tempZipPath,
             result.errorMessage,
             [&options](std::int64_t downloaded, std::int64_t total) {
                 // 进度回调沿用完整包阶段和稳定用户提示。
                 emitProgress(options,
                              AssetSyncProgress{
                                  AssetSyncProgressStage::kDownloadingPackage,
                                  "Downloading asset package",
                                  downloaded,
                                  total,
                                  0,
                                  0,
                              });
             },
             options.cancellationCallback) ) {
        // downloadFile 已区分取消并清理部分文件，外层转换为统一结果枚举。
        if ( isCancellationRequested(options) ) return cancelledResult();
        result.status = AssetSyncStatus::kError;
        return result;
    }

    if ( isCancellationRequested(options) ) return cancelledResult();

    if ( !packageSha256.empty() ) {
        // 摘要在 parseManifest 中已规范化，直接与实际小写 SHA-256 比较。
        const auto actualSha256 =
            hashAssetFile(tempZipPath, options.cancellationCallback);
        if ( !actualSha256 &&
             actualSha256.error() == AssetDigestError::Cancelled )
            return cancelledResult();
        if ( !actualSha256 || *actualSha256 != packageSha256 ) {
            result.status       = AssetSyncStatus::kError;
            result.errorMessage = "Asset package checksum mismatch; expected " +
                                  packageSha256 + ", got " +
                                  actualSha256.value_or("");
            return result;
        }
    }

    // 归档约定自身包含 assets 顶层，解压目标因此是资源根的父目录。
    // 下载包的摘要检查和解压预检均先于版本更新。预检通过后，各文件
    // 独立发布，因此中途失败后可以保留已验证文件并由下一轮重新补全。
    // 已解压的单条资源不足以判定整包安装成功，不能用来推进版本快路径。
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
    if ( isCancellationRequested(options) ) return cancelledResult();
    if ( !AssetSyncService::extractZipArchive(tempZipPath,
                                              destinationRoot,
                                              result.errorMessage,
                                              options.cancellationCallback) ) {
        // 解压过程中可能已经完成较早的条目，版本文件仍表示上次成功安装。
        // 上层使用同一取消查询区分主动退出与预算、格式和文件写入错误。
        if ( isCancellationRequested(options) ) return cancelledResult();
        result.status = AssetSyncStatus::kError;
        return result;
    }

    // 完整包无法知道实际写入数时，使用清单文件数作为可观测更新规模。
    result.updatedFileCount = manifest ? manifest->files.size() : 0;
    return finishAssetSync(
        options, std::move(result), AssetSyncStatus::kDownloaded);
}

/// @brief 先完整解压本地包，再把 assets 树发布到实际资源根。
/// @details 临时目录与资源根共用文件系统。首次导入整树重命名，已有目录
/// 只覆盖包内同名文件，保留自定义资源。已发布文件用同目录树内的原子移动。
/// @param options 本地 ZIP、资源位置以及可选取消回调。
/// @param assetsExist 调用前已确认当前资源根是否存在。
/// @param errorMessage 解压、路径检查或发布失败时的诊断。
/// @return 包内资源已经发布且本地版本快路径已失效。
bool importLocalPackage(const AssetSyncOptions& options, bool assetsExist,
                        std::string& errorMessage)
{
    std::error_code ec;
    // 暂存与实际资源根共用文件系统，避免跨盘时降级成截断覆盖。
    // 目标父目录可能尚不存在，冷启动先建立该层，再创建任务目录。
    const auto parent = options.assetsRootPath.parent_path();
    std::filesystem::create_directories(parent, ec);
    if ( ec ) {
        errorMessage = ec.message();
        return false;
    }
    // 使用单调时钟后缀只为隔离同名导入任务，目录创建仍检查是否已存在。
    // 目录名不取自 ZIP，归档不能指定待清理的上级目录。
    // 仅回收本次独占创建的目录，不能在重试或其他进程运行时误删其工作区。
    const Config::ScopedTemporaryDirectory import(parent,
                                                  ".mmm-assets-import-");
    if ( import.error() ) {
        errorMessage = import.error().message();
        return false;
    }
    // 本地导入也需要隔离多个同路径发起的重试。解压树的目录归属
    // 从创建开始明确，清理不会触及已装资源。
    const auto& staging = import.path();

    emitProgress(options,
                 { AssetSyncProgressStage::kImportingPackage,
                   "Importing local asset package" });
    // 这里复用下载路径的 ZIP 读取和校验，解压错误只影响任务独占目录。
    // 解压过程也可以被退出或新一轮资源选择打断，已有安装尚未被写入。
    // 即使临时树中的部分条目已经写出，只要整个归档失败就不会进入发布阶段。
    // 暂存的单条文件先经长度及 CRC 检查，切换到安装目录之前还需核对根结构。
    if ( !AssetSyncService::extractZipArchive(options.localPackagePath,
                                              staging,
                                              errorMessage,
                                              options.cancellationCallback) )
        return false;

    // 仅发布约定的 assets 顶层；压缩包里的其他顶层文件不进入配置目录。
    // 发布时只读取资源子树，压缩包不能覆盖编辑器的配置、布局和日志文件。
    // 资源根可由调用方改变名称，归档顶层仍与网站的固定 assets 布局一致。
    const auto extracted = staging / "assets";
    if ( !std::filesystem::is_directory(extracted, ec) || ec ) {
        errorMessage = "Asset package must contain an assets directory";
        return false;
    }
    // 本地 ZIP 中的版本号不代表本机实际文件已通过远端清单校验。
    std::filesystem::remove(extracted / kLocalVersionFileName, ec);
    if ( ec ) {
        errorMessage = ec.message();
        return false;
    }

    // 先检查整个文件树。对于已有安装，当前文件与所属目录须仍处于实际资源根内。
    // 配置根本身可能位于管理员或用户配置的软链接路径下，因此先解析根。
    // 在这个根以下不沿用目录重定向，防止覆盖参照资产目录之外的自定义文件。
    const auto canonicalRoot =
        AssetPaths::resolveRoot(options.assetsRootPath, ec);
    if ( ec || !canonicalRoot.is_absolute() ) {
        // 路径转换失败不能继续遍历；迭代器构造会重设错误码。
        // 资源根不明确时也绝不能把相对条目写入进程工作目录。
        errorMessage =
            ec ? ec.message() : "Asset import destination is not absolute";
        return false;
    }
    // 收集相对路径使预检与发布使用同一个基准，不依赖遍历器在 rename 后的状态。
    // ZIP 显式空目录无需逐个合并；增量文件和非空目录的上级会在写入前创建。
    std::vector<std::filesystem::path> files;
    for ( std::filesystem::recursive_directory_iterator it(extracted, ec), end;
          !ec && it != end;
          it.increment(ec) ) {
        if ( isCancellationRequested(options) ) return false;
        const auto relative = it->path().lexically_relative(extracted);
        if ( !AssetPaths::isUnredirectedDestination(
                 canonicalRoot, relative, ec) ) {
            errorMessage = "Asset import destination contains a redirect: " +
                           Config::pathToUtf8(relative);
            return false;
        }
        // 解压器只生成普通文件；目录检查仍参与预检，以拦住上层路径的重定向。
        if ( it->is_regular_file(ec) ) files.push_back(relative);
    }
    // 空归档或只有版本标记的归档不能建立可供离线启动的资源安装。
    // 校验失败不删除已经存在的版本标记，原安装仍能正常重试。
    if ( ec || files.empty() ) {
        errorMessage =
            ec ? ec.message() : "Asset package contains no resource files";
        return false;
    }
    if ( isCancellationRequested(options) ) return false;
    if ( !assetsExist ) {
        // 冷启动一次发布整棵树；无效压缩包或中途取消不会留下半份资源根。
        std::filesystem::rename(extracted, options.assetsRootPath, ec);
    } else {
        // 在首次覆盖前移除旧标记。即使后续失败或离线，下次也会精确核对。
        std::filesystem::remove(canonicalRoot / kLocalVersionFileName, ec);
        // 按单文件原子替换保留旧安装中未包含于归档的用户资源。
        // 取消或 I/O 失败时不恢复被淘汰的版本号，避免下次跳过剩余差异。
        for ( const auto& relative : files ) {
            if ( ec || isCancellationRequested(options) ) break;
            if ( !AssetPaths::isUnredirectedDestination(
                     canonicalRoot, relative, ec) )
                break;
            const auto destination = canonicalRoot / relative;
            std::filesystem::create_directories(destination.parent_path(), ec);
            if ( !ec )
                std::filesystem::rename(extracted / relative, destination, ec);
        }
    }
    if ( ec ) errorMessage = ec.message();
    // 在最终操作后确认取消位，防止切换来源时把旧导入报告为就绪。
    return !ec && !isCancellationRequested(options);
}

/// @brief 按清单下载缺失或变更的单文件。
/// @details
/// 先逐项检查安全路径、普通文件、大小和 SHA-256，按清单顺序收集过期项。
/// 若全部当前则只更新版本标记并返回 kReady；否则逐文件下载到本轮独占目录，
/// 校验摘要后移动到最终根内位置。每项发布文件索引和字节进度，任一取消或错误
/// 立即停止，只有全部文件成功后才写版本并返回 kUpdated。
/// @par 单文件事务
/// - 每项开始前检查取消。
/// - URL 补全不改变清单中的目标相对路径。
/// - 下载只写本轮独占文件，安装前核对真实目标。
/// - 临时文件 SHA-256 必须与清单完全匹配。
/// - 在目标卷独占暂存并复验摘要，最后才重命名到目标。
/// - 发布目标成功后才增加 updatedFileCount。
/// @par 批次终态
/// - 无过期项返回 kReady。
/// - 全部过期项成功返回 kUpdated。
/// - 中途取消返回 kCancelled 且不推进版本标记。
/// - 中途错误返回 kError，外层可选择保留已有资源启动。
/// @param options 资源根、下载地址和回调配置。
/// @param manifest 已通过结构及路径验证的清单。
/// @return 精确文件更新结果和检查、更新计数。
AssetSyncResult updateFromManifest(const AssetSyncOptions& options,
                                   const AssetManifest&    manifest)
{
    AssetSyncResult result;
    // checkedFileCount 代表计划校验总数，不因中途取消而缩小。
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
    std::error_code rootError;
    const auto      root =
        AssetPaths::resolveRoot(options.assetsRootPath, rootError);
    if ( rootError ) {
        result.status       = AssetSyncStatus::kError;
        result.errorMessage = rootError.message();
        return result;
    }
    // 已存在的资源也要检查真实位置，不能将根之外的同内容文件记为已安装。
    for ( const auto& file : manifest.files ) {
        if ( isCancellationRequested(options) ) return cancelledResult();
        if ( !AssetPaths::isUnredirectedDestination(
                 root, Config::utf8ToPath(file.path), rootError) ) {
            result.status       = AssetSyncStatus::kError;
            result.errorMessage = "Unsafe asset destination: " + file.path +
                                  ": " + rootError.message();
            return result;
        }
    }
    // 使用可取消的校验路径；已有匹配文件也可能在扫描大型内容时收到取消。
    auto outdatedFiles =
        collectOutdatedFilesWithProgress(manifest, root, options);

    // 空 optional 表示已在检查中取消；空列表才代表校验成功且无需更新。
    if ( !outdatedFiles || isCancellationRequested(options) )
        return cancelledResult();

    // 没有差异时不分配下载目录，版本维护和只读校验沿用既有状态。
    // 独占暂存仅服务实际的数据传输。
    if ( outdatedFiles->empty() ) {
        // 全部摘要匹配时刷新版本标记，后续启动可以跳过精确哈希。
        return finishAssetSync(
            options, std::move(result), AssetSyncStatus::kReady);
    }

    XINFO("AssetSync: updating {} changed asset file(s)",
          outdatedFiles->size());

    const auto tempRoot = temporaryDownloadRoot(result.errorMessage);
    if ( tempRoot.empty() ) {
        result.status = AssetSyncStatus::kError;
        return result;
    }
    const Config::ScopedTemporaryDirectory download(tempRoot,
                                                    ".mmm-asset-download-");
    if ( download.error() ) {
        result.status       = AssetSyncStatus::kError;
        result.errorMessage = download.error().message();
        return result;
    }
    // 一个任务内部串行处理文件；不同任务从不会打开彼此的暂存路径。
    // 本轮文件串行发布后才复用文件名。不同目录里的同名 staging
    // 没有任何数据共享，因此一个任务取消不会使另一个任务读到半成品。
    const auto tempPath = download.path() / "asset-file.tmp";
    for ( const auto& file : *outdatedFiles ) {
        // 文件边界响应取消，避免开始下一项下载。
        if ( isCancellationRequested(options) ) return cancelledResult();

        const std::size_t currentFileIndex = result.updatedFileCount + 1;
        const auto        fileUrl =
            AssetSyncService::resolveDownloadUrl(options.baseUrl, file.url);
        // parseManifest 已确保 URL 非空；此处补全相对地址供下载器使用。
        emitProgress(options,
                     AssetSyncProgress{
                         AssetSyncProgressStage::kDownloadingFile,
                         "Downloading asset file: " + file.path,
                         0,
                         static_cast<std::int64_t>(file.size),
                         currentFileIndex,
                         outdatedFiles->size(),
                     });

        if ( !downloadFile(
                 fileUrl,
                 tempPath,
                 result.errorMessage,
                 [&options,
                  currentFileIndex,
                  totalFiles = outdatedFiles->size(),
                  path       = file.path](std::int64_t downloaded,
                                    std::int64_t total) {
                     emitProgress(options,
                                  AssetSyncProgress{
                                      AssetSyncProgressStage::kDownloadingFile,
                                      "Downloading asset file: " + path,
                                      downloaded,
                                      total,
                                      currentFileIndex,
                                      totalFiles,
                                  });
                 },
                 options.cancellationCallback,
                 file.size) ) {
            if ( isCancellationRequested(options) ) return cancelledResult();
            result.status = AssetSyncStatus::kError;
            return result;
        }

        // 下载完成后、哈希前再次响应取消，避免无意义的大文件读取。
        if ( isCancellationRequested(options) ) return cancelledResult();

        const auto actualSha256 =
            hashAssetFile(tempPath, options.cancellationCallback);
        if ( !actualSha256 &&
             actualSha256.error() == AssetDigestError::Cancelled )
            return cancelledResult();
        // 单文件必须无条件具备清单摘要，任何不匹配都不能覆盖现有资源。
        if ( !actualSha256 || *actualSha256 != file.sha256 ) {
            result.status       = AssetSyncStatus::kError;
            result.errorMessage = "Asset file checksum mismatch: " + file.path;
            return result;
        }

        // 以本轮确认的根与已经通过词法校验的清单路径提交。
        // 复制不同卷时仍把任何错误和取消留在各自的暂存范围内。
        if ( !installDownloadedAsset(tempPath,
                                     root,
                                     Config::utf8ToPath(file.path),
                                     file,
                                     options,
                                     result.errorMessage) ) {
            if ( isCancellationRequested(options) ) return cancelledResult();
            result.status = AssetSyncStatus::kError;
            return result;
        }

        ++result.updatedFileCount;
        // 计数仅在摘要验证和最终发布都成功后递增。
    }

    // 所有项目成功后才推进版本标记，防止部分更新被误认成完整版本。
    return finishAssetSync(
        options, std::move(result), AssetSyncStatus::kUpdated);
}

/// @brief 在已有本地资源时远端更新失败，记录警告并继续启动。
/// @details
/// 远端不可用、清单非法或逐文件更新失败不应使已有可用资源阻塞应用启动。
/// 返回 kReady 并保留 errorMessage 供 UI/日志提示；该策略绝不用于首次安装，
/// 因为本地目录不存在时没有安全降级基础。
/// @param reason 远端检查或更新失败详情。
/// @return 使用现有资源继续启动的就绪结果。
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

/// @brief 构造应用启动时的默认资源同步选项。
/// @details
/// 资源根来自 AppPaths；base、manifest 和 package URL 可分别通过环境变量覆盖，
/// 便于测试、镜像站或离线部署。未覆盖的相对官方路径经统一 URL 解析补全。
/// @par 覆盖优先级
/// - assetsRootPath 始终来自应用路径配置，不由远端决定。
/// - MMM_ASSET_BASE_URL 覆盖官方站点根。
/// - MMM_ASSET_MANIFEST_URL 可独立覆盖清单完整地址。
/// - MMM_ASSET_PACKAGE_URL 可独立覆盖首次安装包完整地址。
/// - 单项 URL 未覆盖时才基于最终 baseUrl 拼接默认路径。
/// - 回调和精确校验开关保持调用方稍后显式设置。
/// @return 不含回调、可直接用于同步的默认配置。
AssetSyncOptions AssetSyncService::defaultOptions()
{
    // 资源根遵循应用配置目录策略，不依赖当前工作目录。
    AssetSyncOptions options;
    options.assetsRootPath = Config::AppPaths::assetsRootPath();
    options.baseUrl        = readEnvironmentString("MMM_ASSET_BASE_URL");
    // 空环境值与未设置一致，回落到官方 HTTPS 站点。
    if ( options.baseUrl.empty() ) options.baseUrl = kDefaultAssetBaseUrl;

    options.manifestUrl = readEnvironmentString("MMM_ASSET_MANIFEST_URL");
    if ( options.manifestUrl.empty() ) {
        // 清单可随 baseUrl 镜像覆盖而自动指向同一站点。
        options.manifestUrl =
            resolveDownloadUrl(options.baseUrl, kDefaultManifestPath);
    }

    options.packageUrl = readEnvironmentString("MMM_ASSET_PACKAGE_URL");
    if ( options.packageUrl.empty() ) {
        // 完整包是首次安装在清单不可得时的独立回退地址。
        options.packageUrl =
            resolveDownloadUrl(options.baseUrl, kDefaultPackagePath);
    }
    return options;
}

/// @brief 将清单中的资源地址补全为可下载绝对 URL。
/// @param baseUrl 站点或 file URL 根地址。
/// @param url 绝对、根相对或普通相对地址。
/// @return 按统一斜杠规则补全的地址。
std::string AssetSyncService::resolveDownloadUrl(const std::string& baseUrl,
                                                 const std::string& url)
{
    return resolveUrlInternal(baseUrl, url);
}

/// @brief 同步启动所需内置资源，并在已有资源时允许远端故障降级。
/// @details
/// 首先验证取消和本地资源根类型，再下载、解析远端清单。本地目录不存在时必须
/// 使用完整包完成首次安装，任何清单结构错误都直接失败；本地目录存在时，
/// 网络或清单失败保留现有资源并返回 kReady。版本标记匹配可走快速路径，
/// forcePreciseVerification 则强制逐文件检查。增量更新错误同样降级到已有资源。
/// @par 离线导入决策
/// - 本轮指定本地 ZIP 时，在安全解压和发布后直接返回 kReady。
/// - 本地包无效或取消时直接报告结果，由启动界面决定重试来源。
/// - 仅普通联网流程可确认版本；本地导入保留未验证状态。
/// @par 首次安装决策
/// - 资源根不存在且清单合法时使用清单完整包。
/// - 资源根不存在且清单下载失败时使用配置完整包回退。
/// - 资源根不存在且已下载清单非法时直接失败。
/// - 完整包失败时没有可降级资源，保留错误或取消终态。
/// @par 已有资源决策
/// - 清单下载失败时记录警告并返回 kReady。
/// - 清单解析失败时拒绝远端输入并返回 kReady。
/// - 版本一致且未强制精确校验时直接返回 kReady。
/// - 版本不同或强制模式进入逐文件校验。
/// - 增量更新错误保留现有资源并返回 kReady。
/// - 增量成功原样返回 kReady 或 kUpdated。
/// - 资源检查完成但版本标记不可写时返回 kVersionWriteFailed，保留诊断和计数。
/// @par 路径前置条件
/// 已存在的 assetsRootPath 必须是目录；其他文件类型不能参与降级，也不能被
/// 下载流程自动覆盖为目录。
/// @param options 资源路径、远端地址、校验策略和回调。
/// @return 就绪、下载、更新、取消或首次安装错误结果。
AssetSyncResult AssetSyncService::sync(const AssetSyncOptions& options)
{
    // 同步开始前取消不检查文件系统或触发进度回调。
    if ( isCancellationRequested(options) ) return cancelledResult();

    std::error_code existsError;
    // exists 查询失败等同于路径不可可靠使用；后续目录检查会形成明确错误。
    const bool assetsExist =
        std::filesystem::exists(options.assetsRootPath, existsError);
    if ( assetsExist ) {
        // 已存在路径必须是目录，普通文件不能被完整包解压覆盖。
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

    // 非空路径仅表示本轮选定来源，不保留到后续启动的远程配置中。
    if ( !options.localPackagePath.empty() ) {
        // 本地包优先于清单请求和完整下载，即使目标已有同版本资源也先导入。
        AssetSyncResult result;
        if ( !importLocalPackage(options, assetsExist, result.errorMessage) ) {
            if ( isCancellationRequested(options) ) return cancelledResult();
            result.status = AssetSyncStatus::kError;
            return result;
        }
        // 只跳过远端校验；归档的路径和解压完整性已检查，启动层仍要加载默认皮肤。
        // 导入已移除版本标记，未选择本地包的下一轮可以完整核对这些文件。
        XINFO(
            "AssetSync: local package imported; remote verification deferred");
        emitProgress(options,
                     { AssetSyncProgressStage::kFinished, "Assets ready" });
        return result;
    }

    // libcurl 初始化只用于联网启动，本地包完成路径无需进入下载准备。
    // 线程安全的静态初始化避免并行同步重复运行全局网络初始化；
    // 失败时在创建下载句柄前返回，可由上游统一展示错误。
    static const CURLcode curlInitialization =
        curl_global_init(CURL_GLOBAL_DEFAULT);
    if ( curlInitialization != CURLE_OK ) {
        AssetSyncResult result;
        result.status       = AssetSyncStatus::kError;
        result.errorMessage = curl_easy_strerror(curlInitialization);
        return result;
    }

    std::string manifestText;
    std::string manifestDownloadError;
    // 清单检查是所有远端决策入口，先发布稳定阶段事件。
    emitProgress(options,
                 AssetSyncProgress{
                     AssetSyncProgressStage::kCheckingManifest,
                     "Checking remote asset manifest",
                     0,
                     0,
                     0,
                     0,
                 });
    // 清单描述当前版本，不能复用 CDN 对固定地址保留的历史响应。
    // 每次检查使用新的缓存键；大文件仍使用清单中的内容摘要缓存键。
    std::string manifestRequestUrl = options.manifestUrl;
    if ( startsWith(manifestRequestUrl, "https://") ||
         startsWith(manifestRequestUrl, "http://") ) {
        // 本地 file:// 测试与离线镜像不添加查询串，避免改变文件路径语义。
        const auto fragment  = manifestRequestUrl.find('#');
        const auto insertion = fragment == std::string::npos
                                   ? manifestRequestUrl.size()
                                   : fragment;
        // 追加参数放在 fragment 之前；保留调用方已有查询串与覆盖地址。
        const auto  query     = manifestRequestUrl.find('?');
        const char* separator = query < insertion ? "&" : "?";
        // 只在低频远端校验阶段读取时间，渲染与逻辑更新路径不参与网络等待。
        const auto checkId =
            std::chrono::system_clock::now().time_since_epoch().count();
        manifestRequestUrl.insert(
            insertion,
            std::string(separator) + "mmm_check=" + std::to_string(checkId));
    }
    // 查询参数只影响清单的新鲜度，不改变文件哈希验证与资源版本判断。
    const bool manifestDownloaded =
        downloadString(manifestRequestUrl,
                       manifestText,
                       manifestDownloadError,
                       options.cancellationCallback);

    if ( isCancellationRequested(options) ) return cancelledResult();

    std::optional<AssetManifest> manifest;
    std::string                  manifestParseError;
    if ( manifestDownloaded ) {
        // 只有传输成功才解析，下载错误文本保留给已有资源降级提示。
        manifest = parseManifest(manifestText, manifestParseError);
    }

    if ( !assetsExist ) {
        // 首次安装没有可用资源，收到非法清单必须失败而不能忽略安全错误。
        if ( manifestDownloaded && !manifest ) {
            AssetSyncResult result;
            result.status       = AssetSyncStatus::kError;
            result.errorMessage = manifestParseError;
            return result;
        }
        // 清单下载失败时允许使用静态 packageUrl；成功清单则提供版本和摘要。
        return downloadFullPackage(options, manifest ? &(*manifest) : nullptr);
    }

    if ( !manifestDownloaded ) {
        // 已有资源优先保证可启动，远端网络故障只产生警告结果。
        return keepExistingAssetsAfterUpdateFailure(
            "Could not check remote asset manifest: " + manifestDownloadError);
    }
    if ( !manifest ) {
        // 非法远端清单不能用于修改磁盘，但不会破坏已有资源。
        return keepExistingAssetsAfterUpdateFailure(manifestParseError);
    }

    // 本地导入不留下已验证版本，首次联网同步会进入逐文件检查。
    const auto localVersion = readLocalVersionFile(options.assetsRootPath);
    // 默认快速路径信任完整安装后写入的版本标记，显式精确模式绕过该优化。
    if ( !options.forcePreciseVerification && !manifest->version.empty() &&
         localVersion == manifest->version ) {
        XINFO(
            "AssetSync: local assets version {} is current; precise file "
            "verification skipped",
            localVersion);
        AssetSyncResult result;
        result.status        = AssetSyncStatus::kReady;
        result.remoteVersion = manifest->version;
        emitProgress(options,
                     // 即使跳过逐文件检查也发布统一 Finished，让启动 UI 收尾。
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
        // 增量失败时旧文件可能仍完整可用，保留错误详情并继续启动。
        return keepExistingAssetsAfterUpdateFailure(updateResult.errorMessage);
    }
    return updateResult;
}

/// @brief 解析并验证远端资源清单。
/// @details
/// 以无异常 JSON 模式读取可选版本、可选完整包和必需 files 数组。每个文件必须
/// 具有安全相对路径、非空 URL 和规范化 SHA-256；大小缺失按零表示未知。
/// 首个错误立即返回空值及带资源路径的详情，空文件列表也视为无效清单。
/// @par 顶层字段
/// - version 可选，仅控制快速校验与本地标记。
/// - package 可选，用于首次安装覆盖默认完整包。
/// - files 必须存在、必须为数组且至少含一项。
/// @par 文件字段
/// - path 必须通过跨平台安全相对路径校验。
/// - url 必须是非空字符串，可在下载时补全。
/// - sha256 必须规范化为 64 位小写十六进制。
/// - size 可缺失或为零，非零时用于哈希前快速检查。
/// - 输出顺序必须与输入数组一致。
/// @param manifestText UTF-8 JSON 清单正文。
/// @param errorMessage 接收结构或字段错误详情。
/// @return 完整验证后的清单；失败时返回 std::nullopt。
std::optional<AssetManifest> AssetSyncService::parseManifest(
    std::string_view manifestText, std::string& errorMessage)
{
    // 清除调用方旧错误，成功返回时保持空文本。
    errorMessage.clear();

    // 禁用异常解析，不可信远端输入通过 discarded 显式失败。
    const nlohmann::json data = nlohmann::json::parse(
        manifestText.begin(), manifestText.end(), nullptr, false);
    if ( data.is_discarded() || !data.is_object() ) {
        errorMessage = "Invalid asset manifest JSON";
        return std::nullopt;
    }

    AssetManifest manifest;
    // version 可为空；它只影响快速路径和本地版本标记。
    manifest.version = jsonStringField(data, "version");

    const auto packageIter = data.find("package");
    if ( packageIter != data.end() && packageIter->is_object() ) {
        // 完整包字段可选，首次安装仍可回落到 options.packageUrl。
        manifest.packageUrl = jsonStringField(*packageIter, "url");
        manifest.packageSha256 =
            normalizeSha256(jsonStringField(*packageIter, "sha256"));
    }

    const auto filesIter = data.find("files");
    // 增量清单必须显式给出数组，其他类型不得静默当作空清单。
    if ( filesIter == data.end() || !filesIter->is_array() ) {
        errorMessage = "Asset manifest missing files array";
        return std::nullopt;
    }

    for ( const auto& fileJson : *filesIter ) {
        // 字段读取函数把类型错误转换为空/零，随后按各自必需性验证。
        AssetFileEntry file;
        file.path   = jsonStringField(fileJson, "path");
        file.url    = jsonStringField(fileJson, "url");
        file.sha256 = normalizeSha256(jsonStringField(fileJson, "sha256"));
        file.size   = jsonUnsignedField(fileJson, "size");

        if ( !AssetPaths::isSafeRelativeAssetPath(file.path) ) {
            // 路径错误优先报告，禁止进入 URL 下载或目标组合阶段。
            errorMessage = "Unsafe asset path in manifest: " + file.path;
            return std::nullopt;
        }
        if ( file.url.empty() ) {
            errorMessage = "Asset file URL missing: " + file.path;
            return std::nullopt;
        }
        if ( file.sha256.empty() ) {
            // 单文件摘要是增量覆盖现有资源前的强制认证条件。
            errorMessage = "Invalid asset SHA256: " + file.path;
            return std::nullopt;
        }
        manifest.files.push_back(std::move(file));
        // 保留远端顺序，用于确定检查和下载进度索引。
    }

    if ( manifest.files.empty() ) {
        // 空清单不能证明已有资源完整，也不应推进本地版本标记。
        errorMessage = "Asset manifest has no files";
        return std::nullopt;
    }
    return manifest;
}

/// @brief 不带 UI 回调地收集本地缺失或摘要不匹配的资源。
/// @details 公开辅助入口复用与启动同步相同的路径、类型、大小和 SHA-256 判断，
/// 返回顺序与清单一致，便于测试和调用方生成确定更新计划。
/// @param manifest 已验证资源清单。
/// @param assetsRootPath 本地资源根目录。
/// @return 需要重新下载的文件条目副本。
std::vector<AssetFileEntry> AssetSyncService::collectOutdatedFiles(
    const AssetManifest& manifest, const std::filesystem::path& assetsRootPath)
{
    std::vector<AssetFileEntry> outdatedFiles;
    // 不因单个错误中止，完整收集所有需要更新的条目。
    for ( const auto& file : manifest.files ) {
        if ( isAssetFileOutdated(file, assetsRootPath).value_or(true) ) {
            outdatedFiles.push_back(file);
        }
    }
    return outdatedFiles;
}

/// @brief 计算普通文件的规范化 SHA-256 文本。
/// @details
/// 复用资源同步的固定块摘要。公开入口无取消回调，维持失败返回空字符串。
/// @param path 要校验的文件。
/// @return 64 位小写十六进制摘要；读取或计算失败时为空。
std::string AssetSyncService::sha256File(const std::filesystem::path& path)
{
    // 更新器等原有消费者只需要成功摘要或空值，错误分类留给可取消路径。
    return hashAssetFile(path).value_or("");
}

}  // namespace MMM::Network
