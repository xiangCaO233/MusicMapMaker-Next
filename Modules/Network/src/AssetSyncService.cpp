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
/// @note 仅在环境变量未覆盖时使用，所有站内相对地址都以此补全。
constexpr const char* kDefaultAssetBaseUrl = "https://mmm.xiang233.top";

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

/// @brief SHA256 初始哈希值。
/// @note 八个 32 位常量来自 SHA-256 标准初始状态，顺序不可调整。
constexpr std::array<std::uint32_t, 8> kSha256InitialState{
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};

/// @brief SHA256 轮常量。
/// @note 每个 512 位消息块执行 64 轮时按索引使用，数值必须保持标准定义。
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

/// @brief 判断相对资源路径是否安全。
/// @details
/// 禁止绝对路径、Windows 反斜杠/盘符、空段、`.` 与 `..` 段，确保同一清单在
/// 各平台解析为相同的项目内路径。该词法检查之后仍需结合实际根目录验证。
/// @par 拒绝样式
/// - `/absolute/path` 等 POSIX 绝对路径。
/// - `\\server` 或包含反斜杠的 Windows 路径。
/// - `C:/path` 或含冒号的驱动器、URI 形式。
/// - `a//b`、`a/` 等含空路径段的形式。
/// - `a/./b` 与 `a/../b` 等点段导航。
/// @par 允许样式
/// 只接受由非空普通段和正斜杠组成的相对资源路径；字符编码本身保持 UTF-8，
/// 由 Config::utf8ToPath 在最终平台路径组合时转换。
bool isSafeRelativeAssetPath(const std::string& path)
{
    if ( path.empty() || path.front() == '/' || path.front() == '\\' ) {
        // 空路径和两种平台绝对路径起始符不能映射到资源根内文件。
        return false;
    }
    if ( path.find('\\') != std::string::npos ||
         path.find(':') != std::string::npos ) {
        // 统一只接受正斜杠，并拒绝 Windows 驱动器或 URI 冒号。
        return false;
    }

    std::size_t start = 0;
    while ( start <= path.size() ) {
        // 逐段检查同时拒绝连续斜杠和末尾斜杠产生的空段。
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
/// @details 两侧词法规范化后计算 target 相对 root 的路径；空结果、绝对结果或
/// 任一 `..` 段都表示目标不是严格根内子项。
bool isPathInsideRoot(const std::filesystem::path& root,
                      const std::filesystem::path& target)
{
    const auto normalizedRoot   = root.lexically_normal();
    const auto normalizedTarget = target.lexically_normal();
    const auto relativePath =
        normalizedTarget.lexically_relative(normalizedRoot);
    // 资源必须是根内具体文件，根目录本身不算合法目标。
    if ( relativePath.empty() || relativePath.is_absolute() ) return false;
    for ( const auto& part : relativePath ) {
        if ( part == ".." ) return false;
    }
    return true;
}

/// @brief 向文件写入字节。
/// @details 先创建父目录，再以二进制截断模式完整写入；用于 ZIP 条目和 file URL
/// 本地复制，不保留旧文件尾部。
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

/// @brief SHA256 右旋。
/// @details SHA-256 的大写 Sigma 与小写 sigma 函数都由该 32 位循环右移构成。
/// @param value 要旋转的工作字。
/// @param bits 右旋位数，算法调用保证位于 1～31。
/// @return 保留全部位的循环右移结果。
std::uint32_t sha256RotateRight(std::uint32_t value, std::uint32_t bits)
{
    return (value >> bits) | (value << (32U - bits));
}

/// @brief 从 4 个字节读取大端 32 位整数。
/// @details SHA-256 消息字固定使用网络字节序，与主机端序无关。
std::uint32_t readBigEndian32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

/// @brief 向输出缓冲区写入大端 32 位整数。
/// @details 最终八个状态字按标准顺序序列化为 32 字节摘要。
void writeBigEndian32(std::uint32_t value, std::uint8_t* bytes)
{
    bytes[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[3] = static_cast<std::uint8_t>(value & 0xffU);
}

/// @brief 计算内存数据的 SHA256。
/// @details
/// 按 SHA-256 标准追加 0x80、零填充和 64 位大端原始比特长度，再逐个 512 位块
/// 扩展 64 个消息字并执行压缩轮。最终摘要转换为固定 64 位小写十六进制文本。
/// 本实现服务资源清单和测试，不依赖平台加密库的可用性。
/// @par 填充不变量
/// - bitLength 始终记录原始输入而非填充后长度。
/// - 0x80 是原始消息后的首个填充字节。
/// - 长度字段开始位置在块内偏移 56。
/// - 填充后总长度必为 64 的整数倍。
/// @par 压缩不变量
/// - 每块前 16 个消息字按大端读取。
/// - 后 48 个字只依赖标准递推位置。
/// - 八个工作变量使用 32 位无符号模加法。
/// - 每块结果累加到上一块链状态。
/// - 最终八个状态字按大端顺序输出。
/// @param input 要哈希的完整字节序列。
/// @return 规范化 SHA-256 摘要。
std::string sha256Bytes(const std::vector<std::uint8_t>& input)
{
    // 在副本尾部填充，调用方原始资源内容保持不变。
    std::vector<std::uint8_t> data = input;
    // 长度字段记录填充前的比特数。
    const std::uint64_t bitLength =
        static_cast<std::uint64_t>(data.size()) * 8ULL;

    data.push_back(0x80U);
    // 每块最后八字节预留给原始长度，因此填充到模 64 等于 56。
    while ( (data.size() % 64U) != 56U ) {
        data.push_back(0U);
    }
    for ( int shift = 56; shift >= 0; shift -= 8 ) {
        // 从最高字节开始写入 64 位大端长度。
        data.push_back(static_cast<std::uint8_t>((bitLength >> shift) & 0xffU));
    }

    auto state = kSha256InitialState;
    // 每个块独立建立消息调度表，再累加到全局链状态。
    for ( std::size_t offset = 0; offset < data.size(); offset += 64U ) {
        std::array<std::uint32_t, 64> words{};
        for ( std::size_t i = 0; i < 16U; ++i ) {
            // 前 16 个字直接来自当前消息块的大端输入。
            words[i] = readBigEndian32(data.data() + offset + i * 4U);
        }
        for ( std::size_t i = 16U; i < 64U; ++i ) {
            // 后 48 个字按标准小 sigma 递推扩展。
            const std::uint32_t s0 = sha256RotateRight(words[i - 15U], 7U) ^
                                     sha256RotateRight(words[i - 15U], 18U) ^
                                     (words[i - 15U] >> 3U);
            const std::uint32_t s1 = sha256RotateRight(words[i - 2U], 17U) ^
                                     sha256RotateRight(words[i - 2U], 19U) ^
                                     (words[i - 2U] >> 10U);
            words[i]               = words[i - 16U] + s0 + words[i - 7U] + s1;
        }

        std::uint32_t a = state[0];
        // 工作变量复制当前链状态，64 轮结束后再整体累加。
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];

        for ( std::size_t i = 0; i < 64U; ++i ) {
            // 计算选择函数、主函数及两组大 Sigma 的轮结果。
            const std::uint32_t s1 = sha256RotateRight(e, 6U) ^
                                     sha256RotateRight(e, 11U) ^
                                     sha256RotateRight(e, 25U);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t tmp1 =
                h + s1 + ch + kSha256RoundConstants[i] + words[i];
            const std::uint32_t s0   = sha256RotateRight(a, 2U) ^
                                       sha256RotateRight(a, 13U) ^
                                       sha256RotateRight(a, 22U);
            const std::uint32_t maj  = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t tmp2 = s0 + maj;

            h = g;
            // 工作变量按标准管线移位，e 与 a 注入本轮临时和。
            g = f;
            f = e;
            e = d + tmp1;
            d = c;
            c = b;
            b = a;
            a = tmp1 + tmp2;
        }

        state[0] += a;
        // 当前块压缩结果与上一块链状态按 32 位模加法合并。
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<std::uint8_t, 32> digest{};
    // 八个状态字按大端写入最终 32 字节摘要。
    for ( std::size_t i = 0; i < state.size(); ++i ) {
        writeBigEndian32(state[i], digest.data() + i * 4U);
    }

    std::ostringstream stream;
    // 每个字节固定输出两位并以零补齐，得到可比较的小写文本。
    stream << std::hex;
    for ( const auto byte : digest ) {
        stream.width(2);
        stream.fill('0');
        stream << static_cast<int>(byte);
    }
    return stream.str();
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

    CURL* curl = curl_easy_init();
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
/// @return 文件完整写入时返回 true。
bool downloadFile(
    const std::string& url, const std::filesystem::path& path,
    std::string&                                           errorMessage,
    const std::function<void(std::int64_t, std::int64_t)>& progressCallback,
    const std::function<bool()>&                           cancellationCallback)
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

    CURL* curl = curl_easy_init();
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

    const CURLcode result = curl_easy_perform(curl);

    // 先关闭文件和 curl，随后处理失败并删除部分输出。
    std::fclose(file);
    curl_easy_cleanup(curl);

    if ( result == CURLE_ABORTED_BY_CALLBACK && cancellationCallback &&
         cancellationCallback() ) {
        errorMessage = "Asset sync cancelled";
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        // 删除失败不覆盖取消主语义，临时路径不会被发布给后续流程。
        return false;
    }
    if ( result != CURLE_OK ) {
        errorMessage =
            std::string("Download error: ") + curl_easy_strerror(result);
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        // 任何传输错误都不得保留可被摘要阶段误读的不完整文件。
        return false;
    }
    return true;
}

/// @brief 获取临时下载文件路径。
/// @details 优先使用系统临时目录下的 MusicMapMaker 子目录；查询失败时回落到
/// 当前目录。创建错误由真正打开文件的下载函数再次报告。
/// @param fileName 固定临时文件名。
/// @return 用于本次下载的候选路径。
std::filesystem::path temporaryDownloadPath(const char* fileName)
{
    std::error_code       tempPathError;
    std::filesystem::path path =
        std::filesystem::temp_directory_path(tempPathError);
    if ( tempPathError ) path = ".";
    // 使用专用子目录避免与系统临时根其他程序文件冲突。
    path /= "MusicMapMaker";

    std::error_code createError;
    std::filesystem::create_directories(path, createError);
    path /= fileName;
    return path;
}

/// @brief 移动文件，跨文件系统时回退到复制。
/// @details 先确保目标父目录存在，优先 rename 保持原子性和低开销；若源与目标
/// 跨文件系统，则覆盖复制到目标并尽力删除源临时文件。
/// @par 发布不变量
/// - 目标父目录必须先成功建立。
/// - 同文件系统优先以 rename 一步发布。
/// - rename 失败才进入覆盖复制回退。
/// - copy_file 失败时保留源临时文件用于诊断。
/// - copy_file 成功后目标已经完整，源删除失败不撤销成功。
/// - errorMessage 只记录阻止目标完整发布的错误。
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
    // 同文件系统重命名成功即完成，无需再次复制数据。
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
    // 复制已成功发布目标，源清理失败不使更新结果回退为失败。
    std::filesystem::remove(source, removeError);
    return true;
}

/// @brief 写入本地资源版本文件。
/// @details 仅在非空远端版本完成安装后截断写入；尾随换行方便人工查看。
/// @note 空版本不删除既有标记，避免无法识别的清单覆盖已知本地版本。
/// @note 调用点保证资源更新已完成，版本文件不是事务日志或下载锁。
void writeLocalVersionFile(const std::filesystem::path& assetsRootPath,
                           const std::string&           version)
{
    if ( version.empty() ) return;

    const auto    versionPath = assetsRootPath / kLocalVersionFileName;
    std::ofstream file(versionPath, std::ios::binary | std::ios::trunc);
    if ( file ) file << version << '\n';
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
bool isAssetFileOutdated(const AssetFileEntry&        file,
                         const std::filesystem::path& assetsRootPath)
{
    if ( !isSafeRelativeAssetPath(file.path) ) return true;

    const auto localPath =
        (assetsRootPath / Config::utf8ToPath(file.path)).lexically_normal();
    if ( !isPathInsideRoot(assetsRootPath, localPath) ) return true;
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

    const auto localSha256 = AssetSyncService::sha256File(localPath);
    // 哈希读取失败返回空串，也自然判定为过期。
    return localSha256 != file.sha256;
}

/// @brief 收集本地缺失或变更文件，并向启动 UI 汇报校验进度。
/// @details 按清单顺序逐项检查，每项开始前响应取消并发布当前文件索引；返回值
/// 保持清单顺序，供后续下载进度使用稳定序号。
std::vector<AssetFileEntry> collectOutdatedFilesWithProgress(
    const AssetManifest& manifest, const std::filesystem::path& assetsRootPath,
    const AssetSyncOptions& options)
{
    std::vector<AssetFileEntry> outdatedFiles;
    for ( std::size_t index = 0; index < manifest.files.size(); ++index ) {
        // 取消后保留已收集内容但调用方会立即返回统一取消结果。
        if ( isCancellationRequested(options) ) break;

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
/// - 最后发布 Finished 和 kDownloaded。
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

    const auto tempZipPath = temporaryDownloadPath("assets.zip");
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
        const auto actualSha256 = AssetSyncService::sha256File(tempZipPath);
        if ( actualSha256 != packageSha256 ) {
            result.status       = AssetSyncStatus::kError;
            result.errorMessage = "Asset package checksum mismatch; expected " +
                                  packageSha256 + ", got " + actualSha256;
            return result;
        }
    }

    // 归档约定自身包含 assets 顶层，解压目标因此是资源根的父目录。
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
        if ( isCancellationRequested(options) ) return cancelledResult();
        result.status = AssetSyncStatus::kError;
        return result;
    }

    // 只有全部条目成功后才删除临时 ZIP；清理失败不使已安装资源回退。
    std::error_code removeError;
    std::filesystem::remove(tempZipPath, removeError);

    writeLocalVersionFile(options.assetsRootPath, result.remoteVersion);
    // 版本标记写入后发布 Finished，使下次启动可使用快速版本路径。
    emitProgress(options,
                 AssetSyncProgress{
                     AssetSyncProgressStage::kFinished,
                     "Assets ready",
                     0,
                     0,
                     0,
                     0,
                 });
    result.status = AssetSyncStatus::kDownloaded;
    // 完整包无法知道实际写入数时，使用清单文件数作为可观测更新规模。
    result.updatedFileCount = manifest ? manifest->files.size() : 0;
    return result;
}

/// @brief 按清单下载缺失或变更的单文件。
/// @details
/// 先逐项检查安全路径、普通文件、大小和 SHA-256，按清单顺序收集过期项。
/// 若全部当前则只更新版本标记并返回 kReady；否则逐文件下载到统一临时路径，
/// 校验摘要后移动到最终根内位置。每项发布文件索引和字节进度，任一取消或错误
/// 立即停止，只有全部文件成功后才写版本并返回 kUpdated。
/// @par 单文件事务
/// - 每项开始前检查取消。
/// - URL 补全不改变清单中的目标相对路径。
/// - 下载只写统一临时文件，不直接截断现有资源。
/// - 临时文件 SHA-256 必须与清单完全匹配。
/// - 校验成功后才 rename 或跨文件系统复制到目标。
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
    auto outdatedFiles = collectOutdatedFilesWithProgress(
        manifest, options.assetsRootPath, options);

    // 收集函数遇取消只提前停止遍历，外层必须在使用部分结果前检查。
    if ( isCancellationRequested(options) ) return cancelledResult();

    if ( outdatedFiles.empty() ) {
        // 全部摘要匹配时刷新版本标记，后续启动可以跳过精确哈希。
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
        // 文件边界响应取消，避免开始下一项下载。
        if ( isCancellationRequested(options) ) return cancelledResult();

        const std::size_t currentFileIndex = result.updatedFileCount + 1;
        // 同一时刻只串行下载一个文件，因此固定临时名不会发生竞争。
        const auto tempPath = temporaryDownloadPath("asset-file.tmp");
        const auto fileUrl =
            AssetSyncService::resolveDownloadUrl(options.baseUrl, file.url);
        // parseManifest 已确保 URL 非空；此处补全相对地址供下载器使用。
        emitProgress(options,
                     AssetSyncProgress{
                         AssetSyncProgressStage::kDownloadingFile,
                         "Downloading asset file: " + file.path,
                         0,
                         static_cast<std::int64_t>(file.size),
                         currentFileIndex,
                         outdatedFiles.size(),
                     });

        if ( !downloadFile(
                 fileUrl,
                 tempPath,
                 result.errorMessage,
                 [&options,
                  currentFileIndex,
                  totalFiles = outdatedFiles.size(),
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
                 options.cancellationCallback) ) {
            if ( isCancellationRequested(options) ) return cancelledResult();
            result.status = AssetSyncStatus::kError;
            return result;
        }

        // 下载完成后、哈希前再次响应取消，避免无意义的大文件读取。
        if ( isCancellationRequested(options) ) return cancelledResult();

        const auto actualSha256 = AssetSyncService::sha256File(tempPath);
        // 单文件必须无条件具备清单摘要，任何不匹配都不能覆盖现有资源。
        if ( actualSha256 != file.sha256 ) {
            result.status       = AssetSyncStatus::kError;
            result.errorMessage = "Asset file checksum mismatch: " + file.path;
            return result;
        }

        const auto destinationPath =
            (options.assetsRootPath / Config::utf8ToPath(file.path))
                .lexically_normal();
        // 清单解析已验证相对路径；规范化目标后再交给移动函数建立父目录。
        if ( !moveDownloadedFile(
                 tempPath, destinationPath, result.errorMessage) ) {
            result.status = AssetSyncStatus::kError;
            return result;
        }

        ++result.updatedFileCount;
        // 计数仅在摘要验证和最终发布都成功后递增。
    }

    writeLocalVersionFile(options.assetsRootPath, manifest.version);
    // 所有项目成功后才推进版本标记，防止部分更新被误认成完整版本。
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
/// @par 路径前置条件
/// 已存在的 assetsRootPath 必须是目录；其他文件类型不能参与降级，也不能被
/// 下载流程自动覆盖为目录。
/// @param options 资源路径、远端地址、校验策略和回调。
/// @return 就绪、下载、更新、取消或首次安装错误结果。
AssetSyncResult AssetSyncService::sync(const AssetSyncOptions& options)
{
    // libcurl 全局初始化是幂等入口，确保调用线程尚未使用下载 API 时也可工作。
    curl_global_init(CURL_GLOBAL_DEFAULT);

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
    const bool manifestDownloaded =
        downloadString(options.manifestUrl,
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

        if ( !isSafeRelativeAssetPath(file.path) ) {
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
        if ( isAssetFileOutdated(file, assetsRootPath) ) {
            outdatedFiles.push_back(file);
        }
    }
    return outdatedFiles;
}

/// @brief 安全地把资源 ZIP 解压到指定根目录。
/// @details
/// 先完整读取归档并用 miniz 初始化内存读取器，再逐条验证名称为安全相对路径、
/// 组合目标仍在 destinationRoot 内。目录条目只创建目录；文件条目解压到临时堆
/// 缓冲后通过统一二进制写函数落盘。取消和任一错误停止后续条目，始终关闭归档。
/// @par Zip Slip 防御
/// - 条目名先去除目录尾斜杠。
/// - 空目录名不产生文件系统操作。
/// - 绝对路径、反斜杠、冒号、空段和点段全部拒绝。
/// - UTF-8 名称与目标根组合后执行词法规范化。
/// - 规范化目标必须是 destinationRoot 的严格子路径。
/// - 目录与文件条目遵守同一套路径约束。
/// @par 资源生命周期
/// - ZIP 原始字节覆盖 reader 完整生命周期。
/// - reader 初始化成功后所有出口统一调用 mz_zip_reader_end。
/// - 每个文件的 extractedData 在写入后立即 mz_free。
/// - 写入失败时释放当前缓冲并停止后续条目。
/// - 取消只发生在条目边界，不泄漏 miniz 状态。
/// @par 发布限制
/// 本函数不写版本标记、不删除 ZIP，也不宣告同步完成；这些事务性终态由
/// downloadFullPackage 在整个解压成功后负责。
/// @param zipPath 已下载且完成摘要校验的 ZIP 文件。
/// @param destinationRoot 归档相对路径的唯一写入根。
/// @param errorMessage 接收取消、格式、路径或写入错误。
/// @param cancellationCallback 可选的逐条取消查询。
/// @return 所有条目安全写入时返回 true。
bool AssetSyncService::extractZipArchive(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& destinationRoot, std::string& errorMessage,
    const std::function<bool()>& cancellationCallback)
{
    // 成功调用不保留上一次错误详情。
    errorMessage.clear();

    // 开始前取消不读取归档或创建目标目录。
    if ( cancellationCallback && cancellationCallback() ) {
        errorMessage = "Asset sync cancelled";
        return false;
    }

    std::vector<std::uint8_t> zipBytes;
    // 当前实现用内存 miniz 接口，读取失败与归档格式失败分开报告。
    if ( !readFileBytes(zipPath, zipBytes) ) {
        errorMessage = "Failed to read asset package";
        return false;
    }

    mz_zip_archive zipArchive{};
    // 零初始化满足 miniz reader 生命周期前置条件。
    if ( !mz_zip_reader_init_mem(
             &zipArchive, zipBytes.data(), zipBytes.size(), 0) ) {
        errorMessage = "Failed to open asset package";
        return false;
    }

    bool          success   = true;
    const mz_uint fileCount = mz_zip_reader_get_num_files(&zipArchive);
    // 按归档目录顺序处理，首次失败后不再触碰后续条目。
    for ( mz_uint index = 0; index < fileCount; ++index ) {
        // 每个条目前响应取消，使大型多文件包可及时停止。
        if ( cancellationCallback && cancellationCallback() ) {
            errorMessage = "Asset sync cancelled";
            success      = false;
            break;
        }

        mz_zip_archive_file_stat fileStat{};
        // 统计信息提供原始文件名和条目类型，读取失败不能猜测路径。
        if ( !mz_zip_reader_file_stat(&zipArchive, index, &fileStat) ) {
            errorMessage = "Failed to read asset package entry";
            success      = false;
            break;
        }

        std::string archiveName = fileStat.m_filename;
        // 去除目录条目末尾斜杠后执行与文件相同的安全分段检查。
        while ( !archiveName.empty() && archiveName.back() == '/' ) {
            archiveName.pop_back();
        }
        if ( archiveName.empty() ) continue;

        // 第一层拒绝绝对路径、反斜杠、盘符和点段。
        if ( !isSafeRelativeAssetPath(archiveName) ) {
            errorMessage = "Unsafe path in asset package: " + archiveName;
            success      = false;
            break;
        }

        const auto destinationPath =
            (destinationRoot / Config::utf8ToPath(archiveName))
                .lexically_normal();
        // 第二层以组合结果验证根目录包含关系，形成纵深 Zip Slip 防护。
        if ( !isPathInsideRoot(destinationRoot, destinationPath) ) {
            errorMessage =
                "Asset package path escapes destination: " + archiveName;
            success = false;
            break;
        }

        if ( mz_zip_reader_is_file_a_directory(&zipArchive, index) ) {
            // 显式目录仅建立层级；普通文件写入也会按需创建父目录。
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
        // miniz 为单条文件分配缓冲，写入后无论成功失败都必须 mz_free。
        void* extractedData = mz_zip_reader_extract_to_heap(
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
        // 释放缓冲先于错误返回，避免写入失败泄漏条目内存。
        if ( !success ) {
            errorMessage = "Failed to write asset file: " + archiveName;
            break;
        }
    }

    mz_zip_reader_end(&zipArchive);
    // 统一结束 reader，覆盖成功、取消和中途格式/路径错误。
    return success;
}

/// @brief 计算普通文件的规范化 SHA-256 文本。
/// @details 当前实现完整读取文件后复用内存哈希；读取失败返回空字符串，调用方
/// 将其视为摘要不匹配而不会接受文件。
/// @note 该实现会按文件大小分配内存，只应在后台资源检查和下载验证路径调用。
/// @note 返回格式与清单 normalizeSha256 的成功输出完全一致。
/// @param path 要校验的文件。
/// @return 64 位小写十六进制摘要；读取失败时为空。
std::string AssetSyncService::sha256File(const std::filesystem::path& path)
{
    // bytes 只在本函数存活，哈希完成后自动释放大文件缓冲。
    std::vector<std::uint8_t> bytes;
    if ( !readFileBytes(path, bytes) ) return {};
    return sha256Bytes(bytes);
}

}  // namespace MMM::Network
