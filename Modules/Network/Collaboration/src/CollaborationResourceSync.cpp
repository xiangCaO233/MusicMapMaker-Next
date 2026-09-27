#include "network/collaboration/CollaborationResourceSync.h"

#include "CollaborationResourceCipher.h"

#include "config/Utf8Path.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <iomanip>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <sstream>
#include <stop_token>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MMM::Network::Collaboration
{
namespace
{
using Json = nlohmann::json;

/// @brief 资源清单格式版本。
/// @note 参与清单规范化哈希；字段语义变化时必须同步提升版本。
constexpr std::uint32_t RESOURCE_MANIFEST_VERSION = 1U;
/// @brief 单次文件请求大小，避免占满数据通道消息上限。
/// @note 与加密容器认证块大小一致，除最后一块外请求必须严格对齐。
constexpr std::uint32_t RESOURCE_CHUNK_BYTES =
    Detail::COLLABORATION_RESOURCE_BLOCK_BYTES;
/// @brief 单个清单允许的最大文件数。
/// @note 在分配访客状态数组前验证，限制不可信清单的内存规模。
constexpr std::size_t MAX_RESOURCE_FILES = 4096U;
/// @brief 单个清单允许声明的最大总字节数。
/// @note 累加时逐项检查，阻止合法单文件组合成超大房间资源集。
constexpr std::uint64_t MAX_RESOURCE_TOTAL_BYTES =
    32ULL * 1024ULL * 1024ULL * 1024ULL;
/// @brief 单个文件允许的最大字节数。
/// @note 同时约束房主加密快照和访客清单解码。
constexpr std::uint64_t MAX_RESOURCE_FILE_BYTES =
    8ULL * 1024ULL * 1024ULL * 1024ULL;

/// @brief 为同机多客户端生成互不覆盖的临时分块后缀。
/// @details 单调时钟计数与进程内原子序号组合，避免同一时刻多个同步器使用相同
/// 快照、session 或 `.part-` 路径。该值不是安全令牌，只承担文件名唯一性。
/// @return 只含数字和连字符的会话后缀。
std::string makeTransferSuffix()
{
    static std::atomic<std::uint64_t> sequence{ 1U };
    // relaxed 原子只需保证序号唯一，不发布其他共享状态。
    const auto timestamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::to_string(timestamp) + "-" +
           std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed));
}

/// @brief SHA-256 初始状态。
/// @note 八个标准链状态字的顺序不可修改。
constexpr std::array<std::uint32_t, 8> SHA256_INITIAL_STATE{
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};
/// @brief SHA-256 轮常量。
/// @note 每个 512 位块的 64 轮按相同索引读取。
constexpr std::array<std::uint32_t, 64> SHA256_ROUND_CONSTANTS{
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

/// @brief 循环右移 32 位整数。
/// @param value 要旋转的工作字。
/// @param bits 右旋位数，算法调用保证在 1～31。
/// @return 保留全部位的循环结果。
std::uint32_t rotateRight(std::uint32_t value, std::uint32_t bits)
{
    return (value >> bits) | (value << (32U - bits));
}

/// @brief 从大端字节读取 32 位整数。
/// @details SHA-256 消息字固定为大端，与主机字节序无关。
std::uint32_t readBigEndian32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

/// @brief 向字节数组写入大端 32 位整数。
/// @details 最终链状态按标准网络字节序写入摘要。
void writeBigEndian32(std::uint32_t value, std::uint8_t* bytes)
{
    bytes[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    bytes[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

/// @brief 对一个完整 64 字节块执行 SHA-256 压缩。
/// @details
/// 前 16 个消息字从块中读取，后 48 个按标准小 Sigma 递推；八个工作变量执行
/// 64 轮选择、主函数和大 Sigma 组合，最后以 32 位模加法累加回链状态。
/// @param state 输入并接收更新后的八字链状态。
/// @param block 恰好 64 字节的消息块。
void transformSha256(std::array<std::uint32_t, 8>& state,
                     const std::uint8_t*           block)
{
    std::array<std::uint32_t, 64> words{};
    // 原始块按大端拆成前 16 个调度字。
    for ( std::size_t index = 0; index < 16U; ++index ) {
        words[index] = readBigEndian32(block + index * 4U);
    }
    for ( std::size_t index = 16U; index < 64U; ++index ) {
        // 标准递推只依赖固定历史位置，不读取块范围外数据。
        const std::uint32_t s0 = rotateRight(words[index - 15U], 7U) ^
                                 rotateRight(words[index - 15U], 18U) ^
                                 (words[index - 15U] >> 3U);
        const std::uint32_t s1 = rotateRight(words[index - 2U], 17U) ^
                                 rotateRight(words[index - 2U], 19U) ^
                                 (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;
    // 结构化绑定复制链状态，当前块完成前不修改输入数组。
    for ( std::size_t index = 0; index < 64U; ++index ) {
        // choice 和 majority 分别组合 e/f/g 与 a/b/c 三组工作字。
        const std::uint32_t s1 =
            rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
        const std::uint32_t choice = (e & f) ^ ((~e) & g);
        const std::uint32_t temporary1 =
            h + s1 + choice + SHA256_ROUND_CONSTANTS[index] + words[index];
        const std::uint32_t s0 =
            rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
        const std::uint32_t majority   = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temporary2 = s0 + majority;
        h                              = g;
        // 工作字按轮管线移动，a 与 e 注入本轮临时和。
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state[0] += a;
    // 当前块结果按标准顺序累加到上一块链状态。
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/// @brief 支持分块输入、固定内存占用的 SHA-256 累加器。
/// @details
/// update 接受任意分片边界并只保留不足 64 字节的尾部；finish 添加标准填充和
/// 原始比特长度，输出固定小写摘要。对象按单次哈希使用，finish 后不再追加数据。
class Sha256Accumulator
{
public:
    /// @brief 追加任意长度字节。
    /// @param bytes 当前输入分片，可为空。
    void update(std::span<const std::uint8_t> bytes)
    {
        // totalBytes 记录未填充原文长度，供 finish 生成 64 位比特长度。
        m_totalBytes += bytes.size();
        while ( !bytes.empty() ) {
            // 每次只填满当前尾块剩余空间。
            const auto copied =
                std::min(bytes.size(), m_buffer.size() - m_used);
            std::copy_n(bytes.begin(), copied, m_buffer.begin() + m_used);
            m_used += copied;
            bytes = bytes.subspan(copied);
            if ( m_used == m_buffer.size() ) {
                // 完整块立即压缩并复用固定缓冲，内存不随文件大小增长。
                transformSha256(m_state, m_buffer.data());
                m_used = 0;
            }
        }
    }

    /// @brief 写入末尾长度并返回小写十六进制摘要。
    /// @return 当前累计输入的 64 位小写十六进制 SHA-256。
    [[nodiscard]] std::string finish()
    {
        // bitLength 必须在加入 0x80 和零填充前计算。
        const std::uint64_t bitLength = m_totalBytes * 8ULL;
        m_buffer[m_used++]            = 0x80U;
        if ( m_used > 56U ) {
            // 当前块容不下八字节长度时，先补零并压缩，再启用新块。
            std::fill(m_buffer.begin() + m_used, m_buffer.end(), 0U);
            transformSha256(m_state, m_buffer.data());
            m_used = 0;
        }
        std::fill(m_buffer.begin() + m_used, m_buffer.begin() + 56U, 0U);
        // 最后八字节按大端写入原始消息比特长度。
        for ( std::size_t index = 0; index < 8U; ++index ) {
            m_buffer[56U + index] = static_cast<std::uint8_t>(
                (bitLength >> ((7U - index) * 8U)) & 0xFFU);
        }
        transformSha256(m_state, m_buffer.data());

        std::array<std::uint8_t, 32> digest{};
        // 八个状态字序列化为固定 32 字节摘要。
        for ( std::size_t index = 0; index < m_state.size(); ++index ) {
            writeBigEndian32(m_state[index], digest.data() + index * 4U);
        }
        std::ostringstream output;
        // 每字节补齐两位，形成规范化小写十六进制文本。
        output << std::hex << std::setfill('0');
        for ( const auto byte : digest ) {
            output << std::setw(2) << static_cast<unsigned int>(byte);
        }
        return output.str();
    }

private:
    /// @brief 当前压缩状态。
    /// @note 只由 update/finish 所在线程访问。
    std::array<std::uint32_t, 8> m_state = SHA256_INITIAL_STATE;
    /// @brief 尚不足一个压缩块的尾部。
    /// @note m_used 指明有效前缀，剩余字节在 finish 时显式清零。
    std::array<std::uint8_t, 64> m_buffer{};
    /// @brief 尾部已占用字节数。
    /// @invariant 始终小于 64。
    std::size_t m_used = 0;
    /// @brief 已输入的完整字节数。
    /// @note 不包含 SHA-256 填充字节。
    std::uint64_t m_totalBytes = 0;
};

/// @brief 计算内存字节的 SHA-256 小写十六进制摘要。
/// @details 复用流式累加器保证内存与文件哈希获得完全相同格式。
std::string sha256Bytes(const ByteBuffer& input)
{
    Sha256Accumulator accumulator;
    accumulator.update(input);
    return accumulator.finish();
}

/// @brief 判断字符串是否为固定长度的小写 SHA-256。
/// @return 长度与字符集均满足规范时返回 true。
bool isSha256(std::string_view value);

/// @brief 从清单摘要稳定派生资源协议代次。
/// @details 读取摘要前 16 个十六进制字符作为 64 位 generation；全零值映射为一，
/// 保留零作为无效/未初始化代次。相同清单必然得到相同代次。
std::uint64_t manifestGeneration(std::string_view manifestId)
{
    if ( !isSha256(manifestId) ) return 0;
    // 逐半字节构造，避免依赖可能抛异常的字符串数值转换。
    std::uint64_t generation = 0;
    for ( std::size_t index = 0; index < 16U; ++index ) {
        const char character = manifestId[index];
        generation =
            (generation << 4U) |
            static_cast<std::uint64_t>(character <= '9' ? character - '0'
                                                        : character - 'a' + 10);
    }
    return generation == 0 ? 1 : generation;
}

/// @brief 以固定缓冲区读取文件并计算 SHA-256；资源线程外禁止调用。
/// @details
/// 打开后先检查文件长度不超过单文件限制，再用 64 KiB 固定缓冲逐块更新摘要。
/// 每轮读取前响应 stop_token；取消、读取错误或超限均返回空摘要。
/// @warning 执行阻塞文件 I/O，只允许后台资源工作线程调用。
std::string sha256File(const std::filesystem::path& path,
                       std::stop_token              stopToken = {})
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if ( !input ) return {};
    const auto end = input.tellg();
    if ( end < 0 || static_cast<std::uint64_t>(static_cast<std::streamoff>(
                        end)) > MAX_RESOURCE_FILE_BYTES ) {
        return {};
    }
    // 通过长度边界后回到文件开头执行流式读取。
    input.seekg(0, std::ios::beg);
    Sha256Accumulator                     accumulator;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while ( input ) {
        // 取消发生时不调用 finish，空值会阻止资源发布。
        if ( stopToken.stop_requested() ) return {};
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if ( count > 0 ) {
            // 只把本轮实际读取字节送入累加器。
            accumulator.update(std::span<const std::uint8_t>(
                buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    if ( input.bad() ) return {};
    return accumulator.finish();
}

/// @brief 判断字符串是否为固定长度的小写 SHA-256。
/// @details 大写摘要被拒绝，清单生成和传输统一使用规范化小写形式。
bool isSha256(std::string_view value)
{
    return value.size() == 64U &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

/// @brief 生成只包含安全字符的短扩展名。
/// @details 只保留不超过 12 字节、由点和 ASCII 字母数字组成的扩展名，并转为
/// 小写；复杂或可疑扩展名直接丢弃，缓存身份仍由 SHA-256 保证。
std::string safeExtension(const std::filesystem::path& path)
{
    std::string extension = Config::pathToUtf8(path.extension());
    if ( extension.size() > 12U ||
         !std::all_of(extension.begin(), extension.end(), [](char character) {
             return character == '.' ||
                    (character >= '0' && character <= '9') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= 'a' && character <= 'z');
         }) ) {
        return {};
    }
    // 规范化大小写使相同内容和等价扩展得到稳定 cachePath。
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return extension;
}

/// @brief 资源清单中的单个文件。
/// @details 文件以明文 SHA-256 为稳定身份，cachePath 是会话内 `files/`
/// 相对路径， size 同时约束请求终点和加密容器物化长度。
struct ManifestFile {
    /// @brief 明文内容的 64 位小写 SHA-256。
    std::string sha256;
    /// @brief 访客会话根下的安全相对缓存路径。
    std::string cachePath;
    /// @brief 明文资源大小。
    std::uint64_t size = 0;
};

/// @brief 解码后的完整资源清单。
/// @details 仅在
/// CBOR、规范化摘要、文件边界、音频引用和路径重映射全部验证后构造。
struct DecodedManifest {
    /// @brief 清单去除 id 字段后的规范化 CBOR 摘要。
    std::string id;
    /// @brief 按房主清单顺序排列的唯一文件集合。
    std::vector<ManifestFile> files;
    /// @brief 路径已改写为 cachePath 的被引用音频资源。
    std::vector<::MMM::AudioResource> audioResources;
    /// @brief 原项目逻辑路径到访客缓存路径的映射。
    std::unordered_map<std::string, std::string> pathRemap;
};

/// @brief 安全读取 JSON 字符串字段。
/// @details 字段缺失或类型不符时不隐式转换，输出只在成功时覆盖。
bool readString(const Json& object, std::string_view key, std::string& output)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_string() ) return false;
    output = iterator->get<std::string>();
    return true;
}

/// @brief 安全读取 JSON 无符号整数字段。
/// @details 先读取 uint64_t，再检查目标 Value
/// 的最大值，防止分块索引和大小窄化。
/// @tparam Value 目标无符号整数类型。
template<typename Value>
bool readUnsigned(const Json& object, std::string_view key, Value& output)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_number_unsigned() ) {
        return false;
    }
    const auto value = iterator->get<std::uint64_t>();
    // 边界检查先于 static_cast，避免截断不可信清单数值。
    if ( value > std::numeric_limits<Value>::max() ) return false;
    output = static_cast<Value>(value);
    return true;
}

/// @brief 从已验证对象读取音轨配置。
/// @details
/// 所有标量和两个 EQ 数组都是必需字段；数字可由 JSON 整数或浮点表示，布尔和
/// preset 类型严格检查。任一数组元素非数字都使完整配置失败。
bool decodeAudioConfig(const Json& source, ::MMM::AudioTrackConfig& config)
{
    if ( !source.is_object() ) return false;
    // 局部读取器捕获同一 source，保持字段类型规则集中。
    const auto readFloat = [&](std::string_view key, float& output) {
        const auto iterator = source.find(key);
        if ( iterator == source.end() || !iterator->is_number() ) return false;
        output = iterator->get<float>();
        return true;
    };
    const auto readBool = [&](std::string_view key, bool& output) {
        const auto iterator = source.find(key);
        if ( iterator == source.end() || !iterator->is_boolean() ) return false;
        output = iterator->get<bool>();
        return true;
    };
    const auto readFloatArray = [&](std::string_view    key,
                                    std::vector<float>& output) {
        const auto iterator = source.find(key);
        if ( iterator == source.end() || !iterator->is_array() ) return false;
        output.clear();
        // 先清空旧配置，失败结果不会混入调用方先前数组元素。
        output.reserve(iterator->size());
        for ( const auto& value : *iterator ) {
            if ( !value.is_number() ) return false;
            output.push_back(value.get<float>());
        }
        return true;
    };
    const auto preset = source.find("eqPreset");
    // 使用短路链保证所有必需字段成功后才返回完整配置。
    return readFloat("volume", config.volume) &&
           readFloat("playbackSpeed", config.playbackSpeed) &&
           readFloat("playbackPitch", config.playbackPitch) &&
           readBool("muted", config.muted) &&
           readBool("eqEnabled", config.eqEnabled) && preset != source.end() &&
           preset->is_number_integer() &&
           ((config.eqPreset = preset->get<int>()), true) &&
           readFloatArray("eqBandGains", config.eqBandGains) &&
           readFloatArray("eqBandQs", config.eqBandQs);
}

/// @brief 从清单对象读取音频资源。
/// @details 资源必须具有非空 ID、缓存路径、Main/Effect 类型和完整
/// AudioTrackConfig； 原项目路径已在房主编码阶段重写为清单文件 cachePath。
bool decodeAudioResource(const Json& source, ::MMM::AudioResource& resource)
{
    if ( !source.is_object() || !readString(source, "m_id", resource.m_id) ||
         !readString(source, "m_path", resource.m_path) ||
         resource.m_id.empty() || resource.m_path.empty() ) {
        return false;
    }
    std::string type;
    // 只接受协议明确支持的两种音轨类别。
    if ( !readString(source, "m_type", type) ||
         (type != "Main" && type != "Effect") ) {
        return false;
    }
    resource.m_type   = type == "Effect" ? ::MMM::AudioTrackType::Effect
                                         : ::MMM::AudioTrackType::Main;
    const auto config = source.find("m_config");
    return config != source.end() &&
           decodeAudioConfig(*config, resource.m_config);
}

/// @brief 解码并严格校验资源清单。
/// @par 清单认证不变量
/// - version 必须恰为当前 RESOURCE_MANIFEST_VERSION。
/// - id 必须是去除自身字段后规范化 CBOR 的 SHA-256。
/// - 外层 generation 由 id 确定性派生，不由发送者任意选择。
/// - 文件数量、单文件大小和总明文大小分别受限。
/// - resourceIndex 严格对应 files 数组位置。
/// @par 引用完整性
/// - 每个 cachePath 在文件数组中唯一。
/// - cachePath 必须位于 files 直接子级并以对应摘要开头。
/// - 每个 AudioResource ID 唯一且路径命中已认证文件。
/// - 每个原始 pathRemap source 唯一。
/// - 每个 pathRemap target 必须命中已认证文件。
/// @details
/// 先无异常解码 CBOR，验证版本和 64 位小写 id，再删除 id 对其余规范化对象重新
/// 编码并校验摘要。随后限制文件数量、单文件和总大小，确保每个 cachePath
/// 形如 `files/<sha256><safe-extension>` 且唯一；音频资源和 pathRemap 只能引用
/// 已声明文件，资源 ID 与源路径映射键也必须唯一。
/// @param payload ResourceManifest 携带的 CBOR 正文。
/// @return 全部结构和引用约束通过的值对象；任一失败返回空值。
std::optional<DecodedManifest> decodeManifest(const ByteBuffer& payload)
{
    // 严格模式解析失败返回 discarded，不通过异常跨越工作线程。
    Json root = Json::from_cbor(payload, true, false);
    if ( root.is_discarded() || !root.is_object() ) return std::nullopt;
    std::uint32_t   version = 0;
    DecodedManifest manifest;
    if ( !readUnsigned(root, "version", version) ||
         version != RESOURCE_MANIFEST_VERSION ||
         !readString(root, "id", manifest.id) || !isSha256(manifest.id) ) {
        return std::nullopt;
    }
    // id 不参与自身摘要；删除后重新生成规范化 CBOR 验证清单未被篡改。
    Json canonical = root;
    canonical.erase("id");
    if ( sha256Bytes(Json::to_cbor(canonical)) != manifest.id ) {
        return std::nullopt;
    }

    const auto files = root.find("files");
    // 三个集合字段必须同时存在且类型正确，文件数在分配前受限。
    const auto audio = root.find("audio_resources");
    const auto remap = root.find("path_remap");
    if ( files == root.end() || !files->is_array() ||
         files->size() > MAX_RESOURCE_FILES || audio == root.end() ||
         !audio->is_array() || remap == root.end() || !remap->is_array() ) {
        return std::nullopt;
    }

    std::uint64_t                   totalBytes = 0;
    std::unordered_set<std::string> cachePaths;
    // 文件顺序保留为后续 resourceIndex 协议索引。
    for ( const auto& item : *files ) {
        ManifestFile file;
        if ( !item.is_object() || !readString(item, "sha256", file.sha256) ||
             !readString(item, "path", file.cachePath) ||
             !readUnsigned(item, "size", file.size) || !isSha256(file.sha256) ||
             file.size > MAX_RESOURCE_FILE_BYTES ) {
            return std::nullopt;
        }
        const std::filesystem::path cachePath =
            Config::utf8ToPath(file.cachePath);
        // cachePath 必须是 files 目录的直接子项，文件名以前缀摘要绑定内容。
        if ( cachePath.is_absolute() || cachePath.has_root_path() ||
             cachePath.parent_path() != "files" ||
             !file.cachePath.starts_with("files/") ||
             !cachePath.filename().string().starts_with(file.sha256) ) {
            return std::nullopt;
        }
        if ( !cachePaths.insert(file.cachePath).second ) return std::nullopt;
        // 路径唯一后累加大小，阻止重复目标互相覆盖。
        totalBytes += file.size;
        if ( totalBytes > MAX_RESOURCE_TOTAL_BYTES ) return std::nullopt;
        manifest.files.push_back(std::move(file));
    }
    std::unordered_set<std::string> resourceIds;
    // 音频资源 ID 唯一，且 m_path 必须引用文件集合中的缓存路径。
    for ( const auto& item : *audio ) {
        ::MMM::AudioResource resource;
        if ( !decodeAudioResource(item, resource) ||
             !resourceIds.insert(resource.m_id).second ||
             !cachePaths.contains(resource.m_path) ) {
            return std::nullopt;
        }
        manifest.audioResources.push_back(std::move(resource));
    }
    for ( const auto& item : *remap ) {
        std::string source;
        std::string target;
        if ( !item.is_object() || !readString(item, "source", source) ||
             !readString(item, "target", target) || source.empty() ||
             target.empty() || !cachePaths.contains(target) ||
             manifest.pathRemap.contains(source) ) {
            return std::nullopt;
        }
        // 同一原始路径只能映射一次，目标必须来自已认证文件集合。
        manifest.pathRemap.insert_or_assign(std::move(source),
                                            std::move(target));
    }
    return manifest;
}

/// @brief 判断音频资源是否被当前谱面引用。
/// @details
/// 优先匹配采样绑定收集到的资源 ID 或路径；主音频兼容旧谱面提示，可按完整
/// ID、完整路径或文件名命中。只有被引用资源进入协作清单，未用项目素材不上传。
bool isReferencedAudioResource(
    const ::MMM::AudioResource&            resource,
    const std::unordered_set<std::string>& references,
    const ::MMM::BaseMapMeta&              metadata)
{
    if ( references.contains(resource.m_id) ||
         references.contains(resource.m_path) ) {
        return true;
    }
    // 元数据 hint 可能保存 ID、相对路径或仅文件名，三种形式均兼容。
    const auto matchesHint = [&](const std::filesystem::path& hint) {
        if ( hint.empty() ) return false;
        const auto hintText = Config::pathToUtf8(hint);
        return hintText == resource.m_id || hintText == resource.m_path ||
               hint.filename() ==
                   Config::utf8ToPath(resource.m_path).filename();
    };
    return matchesHint(metadata.song_file_hint) ||
           matchesHint(metadata.main_audio_path);
}

/// @brief 房主准备任务的纯值快照。
/// @details startHost 在调用线程复制项目根、筛选后的音频资源和封面路径，后台
/// worker 不持有 Project 或 BeatMap 引用，避免调用方继续编辑时产生数据竞争。
struct HostPreparation {
    /// @brief 解析资源相对路径的项目根。
    std::filesystem::path projectRoot;
    /// @brief 已确认被谱面引用的音频资源值副本。
    std::vector<::MMM::AudioResource> audioResources;
    /// @brief 封面等非音频资源逻辑路径。
    std::vector<std::filesystem::path> extraPaths;
};

/// @brief 后台线程输入任务。
/// @details 单一变体式 DTO 由 Type 决定有效载荷；任务队列串行处理，保证清单、
/// 请求与分块不会并发修改同一同步状态。
struct ResourceTask {
    /// @brief 后台状态机可处理的任务类别。
    enum class Type {
        /// @brief 房主扫描、冻结资源并生成清单。
        PrepareHost,
        /// @brief 访客验证清单并比较会话缓存。
        Manifest,
        /// @brief 房主读取一个请求分块。
        Request,
        /// @brief 访客顺序写入一个响应分块。
        Chunk,
    };
    Type             type{ Type::PrepareHost };
    HostPreparation  preparation;
    PeerId           peerId = 0;
    ResourceManifest manifest;
    ResourceRequest  request;
    ResourceChunk    chunk;
};

/// @brief 房主清单文件及其本机来源路径。
/// @details manifest 提供网络可见身份与大小，sourcePath 指向本次房间冻结的认证
/// 加密容器，不再直接读取可变项目源文件。
struct HostFile {
    ManifestFile          manifest;
    std::filesystem::path sourcePath;
};

/// @brief 判断待清理目录是否为指定缓存根目录下由本模块生成的直接子目录。
/// @details
/// 删除前把两侧转换为绝对词法规范路径，要求 sessionRoot 的父目录恰为
/// cacheRoot， 且文件名严格使用非空 `session-`
/// 前缀。任何路径解析错误都拒绝删除。
bool isOwnedGuestSessionRoot(const std::filesystem::path& cacheRoot,
                             const std::filesystem::path& sessionRoot)
{
    if ( cacheRoot.empty() || sessionRoot.empty() ) return false;
    // 空路径不能交给递归清理，避免解析成当前目录。
    std::error_code baseError;
    std::error_code sessionError;
    const auto      absoluteBase =
        std::filesystem::absolute(cacheRoot, baseError).lexically_normal();
    const auto absoluteSession =
        std::filesystem::absolute(sessionRoot, sessionError).lexically_normal();
    const auto name = Config::pathToUtf8(absoluteSession.filename());
    // 只接受本模块生成的直接子目录，不允许更深路径或 cacheRoot 本身。
    return !baseError && !sessionError && !absoluteBase.empty() &&
           absoluteSession.parent_path() == absoluteBase &&
           name.starts_with("session-") && name.size() > 8U;
}

/// @brief 删除一个已经验证归属关系的访客会话目录。
/// @details 先执行严格所有权检查，再以 error_code 尽力递归删除；清理失败不从
/// 析构路径传播异常。
void removeOwnedGuestSessionRoot(const std::filesystem::path& cacheRoot,
                                 const std::filesystem::path& sessionRoot)
{
    if ( !isOwnedGuestSessionRoot(cacheRoot, sessionRoot) ) return;
    std::error_code removeError;
    std::filesystem::remove_all(sessionRoot, removeError);
}

/// @brief 让临时素材目录仅对当前用户开放；不支持的文件系统保持原权限。
/// @details 用 owner_all 替换权限，减少同机其他用户读取会话明文或密钥相关缓存；
/// 平台不支持时忽略错误，不阻断资源同步。
void restrictGuestSessionPermissions(const std::filesystem::path& path)
{
    std::error_code permissionError;
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 permissionError);
}

/// @brief 将访客临时项目与其加密缓存、明文素材目录绑定到同一生命周期。
/// @details aliasing shared_ptr 对外暴露内嵌 Project，但控制块持有本 Owner；
/// 最后一个项目引用释放时才能安全删除其仍被音频解码器访问的 session 目录。
struct GuestResourceBundleOwner {
    /// @brief 访客临时项目值对象。
    ::MMM::Project project;
    /// @brief 用户配置中的协作缓存根目录，仅用于删除边界验证。
    std::filesystem::path cacheRoot;
    /// @brief 本次联机会话独占的待清理目录。
    std::filesystem::path sessionRoot;

    /// @brief 最后一个项目引用释放时自动清理会话目录。
    /// @warning 执行递归文件系统清理，只发生在临时项目最终释放的低频路径。
    ~GuestResourceBundleOwner()
    {
        removeOwnedGuestSessionRoot(cacheRoot, sessionRoot);
    }
};
}  // namespace

/// @brief 在单一后台序列中协调房主资源快照和访客分块缓存。
/// @details
/// 调用线程只提交纯值任务、轮询事件和读取进度；worker 独占文件系统、哈希、
/// 加密与清单状态迁移。房主和访客模式复用同一实现，但每次 start 都先 restart，
/// 因而一个实例不会同时服务两种角色或两个 generation。
/// @par 房主流水线
/// - 筛选谱面引用并复制 HostPreparation。
/// - 冻结认证加密快照并发布 ManifestReady。
/// - 校验访客 Request 并发布定向 SendChunk。
/// @par 访客流水线
/// - 验证 Manifest、创建隔离 session 并比较明文缓存。
/// - 串行发布 SendRequest 和认证写入 Chunk。
/// - 整文件物化、摘要验证并提交加密容器。
/// - 全部文件完成后发布带生命周期 Owner 的 BundleReady。
/// @par 线程约束
/// task、event 和 progress 使用独立互斥量；大文件载荷尽量移动，任何公开入口
/// 都不会直接执行磁盘 I/O。restart/析构等待 worker 后才清理文件与密钥。
/// @par 错误约束
/// - 错误 detail 同时进入进度和 Error 事件。
/// - 主动 stop 不发布伪造错误。
/// - 未认证数据永不进入 BundleReady。
/// - 失败会话目录只由 restart、析构或 Bundle Owner 清理。
/// - 密钥在任何会话重置和对象销毁时显式清零。
class CollaborationResourceSync::Impl
{
public:
    /// @brief 创建同步状态并启动串行后台工作循环。
    /// @note transferSuffix 在对象生命周期内区分同机其他同步器的临时文件。
    Impl() : m_transferSuffix(makeTransferSuffix()) { startWorker(); }

    /// @brief 停止工作循环并清理快照、事件、会话目录和两侧密钥。
    /// @warning 会等待后台任务并执行文件系统清理，只用于对象销毁。
    ~Impl()
    {
        // 后台线程停止后才能安全删除其可能正在访问的文件和密钥。
        stopWorker();
        clearHostSnapshot();
        {
            std::lock_guard eventLock(m_eventMutex);
            m_events.clear();
        }
        clearGuestSession();
        Detail::clearCollaborationResourceKey(m_hostResourceKey);
        // 无论对应 ready 标志如何都覆盖清零固定密钥缓冲。
        Detail::clearCollaborationResourceKey(m_guestResourceKey);
    }

    /// @brief 重启后台线程并清空全部状态。
    /// @par 重启不变量
    /// - 旧 worker 停止后才清理其输入和文件。
    /// - 旧完成事件不会泄漏到新会话。
    /// - 两侧密钥在生成新密钥前显式清零。
    /// - 旧 generation、清单索引和 offset 全部作废。
    /// - 已发布项目的 session 不由新会话提前删除。
    /// - 新 transferSuffix 与旧部分文件路径不同。
    /// @details
    /// 用于 startHost、startGuest 和 reset。先等待旧
    /// worker，清除房主快照、任务、 事件、未发布访客 session
    /// 与密钥，再重置进度和协议水位，生成新临时后缀， 最后启动新 worker。已发布
    /// Bundle 的目录由其 Owner 独立管理，不在此删除。
    /// @warning 会等待旧后台文件任务，只允许低频会话切换调用。
    void restart()
    {
        // 停止顺序保证后续容器和文件清理不与 worker 并发。
        stopWorker();
        clearHostSnapshot();
        {
            std::lock_guard taskLock(m_taskMutex);
            // 旧 generation 的未处理请求和分块全部作废。
            m_tasks.clear();
        }
        {
            std::lock_guard eventLock(m_eventMutex);
            m_events.clear();
        }
        clearGuestSession();
        // 未发布 session 由同步器清理，已发布 session 已交给资源包所有者。
        Detail::clearCollaborationResourceKey(m_hostResourceKey);
        Detail::clearCollaborationResourceKey(m_guestResourceKey);
        m_hostResourceKeyReady  = false;
        m_guestResourceKeyReady = false;
        {
            std::lock_guard progressLock(m_progressMutex);
            m_progress = {};
        }
        m_hostFiles.clear();
        // 清空两侧清单、索引和偏移，避免新会话复用旧 resourceIndex。
        m_hostGeneration = 0;
        m_guestManifest.reset();
        m_guestRoot.clear();
        m_guestMissingFiles.clear();
        m_guestCurrentFile = 0;
        m_guestOffset      = 0;
        m_cacheRoot.clear();
        m_transferSuffix = makeTransferSuffix();
        // 新 worker 只会看到本轮之后入队的任务。
        startWorker();
    }

    /// @brief 排队后台任务。
    /// @details 在互斥量下移动到 FIFO 队列，再通知一个等待 worker；调用方不再
    /// 保留任务中大型 manifest/chunk 载荷的所有权。
    void enqueue(ResourceTask task)
    {
        {
            std::lock_guard lock(m_taskMutex);
            m_tasks.push_back(std::move(task));
        }
        m_taskCondition.notify_one();
    }

    /// @brief 非阻塞取出完成事件。
    /// @details 只持锁移动队首并立即返回，不等待 worker 产出；事件顺序与后台
    /// 串行处理顺序一致。
    bool poll(CollaborationResourceSyncEvent& event)
    {
        std::lock_guard lock(m_eventMutex);
        if ( m_events.empty() ) return false;
        event = std::move(m_events.front());
        m_events.pop_front();
        return true;
    }

    /// @brief 获取进度副本。
    /// @details 在锁内复制完整 DTO，调用方释放锁后可安全读取。
    CollaborationResourceSyncProgress progress() const
    {
        std::lock_guard lock(m_progressMutex);
        return m_progress;
    }

    /// @brief 设置访客缓存根路径。
    /// @details startGuest 重启后调用，把进度置为 WaitingManifest；目录实际创建
    /// 延迟到合法清单到达，避免空会话残留。
    void setCacheRoot(std::filesystem::path cacheRoot)
    {
        m_cacheRoot = std::move(cacheRoot);
        setProgress([](auto& progress) {
            progress       = {};
            progress.phase = CollaborationResourceSyncPhase::WaitingManifest;
        });
    }

    /// @brief 设置房主准备阶段。
    /// @details startHost 在入队 HostPreparation 前同步发布 Preparing，供 UI
    /// 立即展示。
    void setPreparing()
    {
        setProgress([](auto& progress) {
            progress       = {};
            progress.phase = CollaborationResourceSyncPhase::Preparing;
        });
    }

private:
    /// @brief 启动后台循环。
    /// @details 必须使用全局 AppThreadPool；未初始化时发布稳定错误，不创建私有
    /// 线程绕过应用生命周期。stop_token 与 future 一起保存供 restart/析构回收。
    void startWorker()
    {
        auto* appThreadPool = Runtime::AppThreadPool::instance().get();
        if ( !appThreadPool ) {
            fail("runtime_thread_pool_not_initialized");
            return;
        }
        m_stopSource = std::stop_source{};
        // 每次启动创建新 stop state，旧 token 不会取消新 worker。
        const auto token = m_stopSource.get_token();
        m_workerFuture =
            appThreadPool->enqueue([this, token]() { workerLoop(token); });
    }

    /// @brief 请求后台线程停止。
    /// @details 请求 stop、唤醒条件变量并等待 future；无有效 future
    /// 时幂等返回。
    /// @warning 等待当前哈希、加密或物化任务响应
    /// stop_token，只用于低频重启/析构。
    void stopWorker()
    {
        if ( !m_workerFuture.valid() ) return;
        m_stopSource.request_stop();
        // notify_all 唤醒尚未有任务的 worker，使其立即观察停止请求。
        m_taskCondition.notify_all();
        m_workerFuture.wait();
        m_workerFuture = std::future<void>{};
    }

    /// @brief 后台串行处理文件任务。
    /// @par 串行保证
    /// - PrepareHost 完成前不会处理随后请求。
    /// - Manifest 完成比较前不会处理随后分块。
    /// - 同一访客的 Chunk 按入队顺序验证 offset。
    /// - stop 请求优先于继续弹出任务。
    /// - 处理器发布事件不需要持有任务队列锁。
    /// @details 条件变量可由任务或 stop_token 唤醒；每次只弹出一个 FIFO 任务，
    /// 根据类型调用对应状态机阶段，避免磁盘文件和索引状态并发修改。
    void workerLoop(std::stop_token stopToken)
    {
        while ( !stopToken.stop_requested() ) {
            ResourceTask task;
            {
                std::unique_lock lock(m_taskMutex);
                // 谓词只检查任务，stop_token 由 condition_variable_any
                // 单独处理。
                m_taskCondition.wait(
                    lock, stopToken, [this]() { return !m_tasks.empty(); });
                if ( stopToken.stop_requested() ) return;
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            switch ( task.type ) {
            // 大型值载荷移动到处理器，减少清单与分块复制。
            case ResourceTask::Type::PrepareHost:
                prepareHost(std::move(task.preparation), stopToken);
                break;
            case ResourceTask::Type::Manifest:
                processManifest(std::move(task.manifest), stopToken);
                break;
            case ResourceTask::Type::Request:
                processRequest(task.peerId, task.request);
                break;
            case ResourceTask::Type::Chunk:
                processChunk(std::move(task.chunk), stopToken);
                break;
            }
        }
    }

    /// @brief 原子更新进度。
    /// @details 回调只在持有 progress mutex 时执行，不得在其中发布事件或阻塞
    /// I/O。
    template<typename Callback> void setProgress(Callback&& callback)
    {
        std::lock_guard lock(m_progressMutex);
        callback(m_progress);
    }

    /// @brief 发布完成事件。
    /// @details 事件队列与任务队列分锁，worker 发布不会阻止调用方继续入队。
    void pushEvent(CollaborationResourceSyncEvent event)
    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(std::move(event));
    }

    /// @brief 将同步器切换为错误状态并发布日志事件。
    /// @details 同一稳定 detail 同时写入进度和 Error 事件，调用方无论轮询哪条
    /// 接口都得到一致原因。函数不自动清理会话，restart/析构统一回收。
    void fail(std::string detail)
    {
        setProgress([&](auto& progress) {
            progress.phase  = CollaborationResourceSyncPhase::Error;
            progress.detail = detail;
        });
        CollaborationResourceSyncEvent event;
        // Error 事件不携带协议 message 或 bundle，只使用 detail。
        event.type   = CollaborationResourceSyncEvent::Type::Error;
        event.detail = std::move(detail);
        pushEvent(std::move(event));
    }

    /// @brief 清理当前同步器独占的房主资源快照目录。
    /// @details 快照从未交给外部对象，worker 停止后可直接递归删除并清空路径。
    void clearHostSnapshot()
    {
        if ( m_hostSnapshotRoot.empty() ) return;
        std::error_code error;
        std::filesystem::remove_all(m_hostSnapshotRoot, error);
        m_hostSnapshotRoot.clear();
    }

    /// @brief 清理尚未发布的访客目录；已发布目录由项目共享生命周期接管。
    /// @details m_guestBundlePublished 防止 restart 删除仍被外部 Project
    /// 使用的明文； 无论是否删除，都清空当前同步器路径，后续清单建立新
    /// session。
    void clearGuestSession()
    {
        if ( !m_guestBundlePublished ) {
            removeOwnedGuestSessionRoot(m_cacheRoot, m_guestSessionRoot);
        }
        m_guestBundlePublished = false;
        m_guestSessionRoot.clear();
        m_guestEncryptedRoot.clear();
        m_guestRoot.clear();
    }

    /// @brief 准备房主清单并计算全部文件摘要。
    /// @par 快照不变量
    /// - 每个真实源文件在一个 generation 内只加密一次。
    /// - 多个逻辑路径可映射到同一内容快照。
    /// - 所有源文件必须规范化后仍位于项目根内。
    /// - 目录、缺失文件和超限文件都使完整准备失败。
    /// - AudioResource 路径只在值副本中改写。
    /// - pathRemap 排序后才参与 CBOR 摘要。
    /// - m_hostFiles 只在清单完全构造成功后替换。
    /// - ManifestReady 发布后项目源变化不影响当前房间响应。
    /// @details
    /// 为本房间生成临时密钥和独占快照目录，把筛选后的项目资源逐项规范化到项目根
    /// 内，按真实源路径去重，再加密复制为不可变 `.mmrsc`。清单保存明文摘要、
    /// 大小、内容寻址 cachePath、音频配置和原路径重映射；排序映射后生成规范化
    /// CBOR，以其 SHA-256 作为清单 ID 并派生 generation。所有文件冻结完成前
    /// 不发布 ManifestReady，后续分块只读取这些快照而不读取仍可编辑的项目源。
    /// @param preparation 调用线程捕获的项目资源值快照。
    /// @param stopToken 重启或析构发出的后台取消令牌。
    /// @warning
    /// 执行目录创建、路径规范化、文件加密和哈希，只能在资源工作线程调用。
    void prepareHost(HostPreparation preparation, std::stop_token stopToken)
    {
        // 每次房主准备都使用新的进程内密钥，旧房间密钥已在 restart 清零。
        if ( !Detail::generateCollaborationResourceKey(m_hostResourceKey) ) {
            fail("host_resource_key_generation_failed");
            return;
        }
        m_hostResourceKeyReady = true;
        // hostFiles 保留去重后的加密快照，sourceIndexes 以规范化真实路径去重。
        std::vector<HostFile>                        hostFiles;
        std::unordered_map<std::string, std::size_t> sourceIndexes;
        std::unordered_map<std::string, std::string> pathRemap;

        std::error_code temporaryError;
        // 房主快照不放项目目录，避免被保存、打包或用户编辑操作扫描到。
        const auto temporaryRoot =
            std::filesystem::temp_directory_path(temporaryError);
        if ( temporaryError ) {
            fail("host_resource_snapshot_root_failed");
            return;
        }
        m_hostSnapshotRoot =
            temporaryRoot / "mmm-collaboration-resources" / m_transferSuffix;
        // transferSuffix 保证同机多个房间不会截断彼此快照。
        std::filesystem::create_directories(m_hostSnapshotRoot, temporaryError);
        if ( temporaryError ) {
            fail("host_resource_snapshot_create_failed");
            return;
        }
        std::error_code projectRootError;
        // 后续所有资源必须以规范化项目根为包含边界。
        const auto canonicalProjectRoot = std::filesystem::weakly_canonical(
            preparation.projectRoot, projectRootError);
        if ( projectRootError || canonicalProjectRoot.empty() ) {
            fail("host_project_root_invalid");
            return;
        }

        const auto addFile = [&](const std::filesystem::path& logicalPath,
                                 std::filesystem::path        sourcePath)
            -> std::optional<std::string> {
            // logicalPath 用于 pathRemap，sourcePath
            // 用于实际读取，两者都不能为空。
            if ( logicalPath.empty() || sourcePath.empty() )
                return std::nullopt;
            if ( sourcePath.is_relative() ) {
                // 项目资源相对路径统一相对于 canonicalProjectRoot 解析。
                sourcePath = canonicalProjectRoot / sourcePath;
            }
            std::error_code canonicalError;
            sourcePath =
                std::filesystem::weakly_canonical(sourcePath, canonicalError);
            // weakly_canonical 错误时不能依赖词法路径判断真实文件归属。
            if ( canonicalError ) return std::nullopt;
            const auto relativeSource =
                sourcePath.lexically_relative(canonicalProjectRoot);
            // 只接受项目根内严格相对位置，阻止绝对路径和 `..` 逃逸。
            if ( relativeSource.empty() || relativeSource.is_absolute() ||
                 *relativeSource.begin() == ".." ) {
                return std::nullopt;
            }
            std::error_code regularError;
            if ( !std::filesystem::is_regular_file(sourcePath, regularError) ||
                 regularError ) {
                return std::nullopt;
            }
            const std::string sourceKey = Config::pathToUtf8(sourcePath);
            if ( const auto found = sourceIndexes.find(sourceKey);
                 found != sourceIndexes.end() ) {
                // 同一真实文件被多个谱面字段引用时复用单一快照和 cachePath。
                const auto& cachePath =
                    hostFiles[found->second].manifest.cachePath;
                pathRemap.insert_or_assign(Config::pathToUtf8(logicalPath),
                                           cachePath);
                return cachePath;
            }

            setProgress([&](auto& progress) {
                // UI 展示逻辑路径，不暴露主机绝对项目路径。
                progress.currentFile = Config::pathToUtf8(logicalPath);
            });
            const auto snapshotPath =
                m_hostSnapshotRoot /
                (std::to_string(hostFiles.size()) + ".mmrsc");
            // 加密函数流式冻结源文件，并同时返回明文大小和摘要。
            const auto digest = Detail::encryptCollaborationResourceFile(
                sourcePath, snapshotPath, m_hostResourceKey, stopToken);
            if ( !digest || digest->size > MAX_RESOURCE_FILE_BYTES ||
                 !isSha256(digest->sha256) ) {
                return std::nullopt;
            }
            // 内容摘要形成访客 cachePath，安全扩展名仅用于保留资源类型提示。
            const std::string cachePath =
                "files/" + digest->sha256 + safeExtension(sourcePath);
            sourceIndexes.emplace(sourceKey, hostFiles.size());
            // 索引在 push 前记录即将插入位置，随后不重排 hostFiles。
            hostFiles.push_back(
                { ManifestFile{ digest->sha256, cachePath, digest->size },
                  snapshotPath });
            pathRemap.insert_or_assign(Config::pathToUtf8(logicalPath),
                                       cachePath);
            return cachePath;
        };

        setProgress([&](auto& progress) {
            // 初始总数按逻辑引用计数，去重后在 Ready 阶段修正为实际文件数。
            progress.totalFiles =
                static_cast<std::uint32_t>(preparation.audioResources.size() +
                                           preparation.extraPaths.size());
        });
        for ( auto& resource : preparation.audioResources ) {
            // 音频资源的 m_path 在清单副本中改写为访客 cachePath。
            const auto cachePath = addFile(Config::utf8ToPath(resource.m_path),
                                           Config::utf8ToPath(resource.m_path));
            if ( !cachePath ) {
                // 主动停止不发布 Error，新 restart 会建立下一状态。
                if ( stopToken.stop_requested() ) return;
                fail("host_resource_missing:" + resource.m_id);
                return;
            }
            resource.m_path = *cachePath;
            setProgress([](auto& progress) { ++progress.completedFiles; });
        }
        for ( const auto& path : preparation.extraPaths ) {
            // 封面等额外资源只进入 pathRemap，不进入 AudioResource 数组。
            if ( !addFile(path, path) ) {
                if ( stopToken.stop_requested() ) return;
                fail("host_resource_missing:" + Config::pathToUtf8(path));
                return;
            }
            setProgress([](auto& progress) { ++progress.completedFiles; });
        }
        if ( hostFiles.size() > MAX_RESOURCE_FILES ) {
            // 去重后实际文件数仍超限时拒绝整个清单。
            fail("host_resource_file_limit");
            return;
        }

        std::uint64_t totalBytes = 0;
        Json          files      = Json::array();
        // hostFiles 顺序决定协议 resourceIndex，序列化时必须保持不变。
        for ( const auto& file : hostFiles ) {
            totalBytes += file.manifest.size;
            // 每次累加后检查总量，单文件上限不足以限制房间总资源规模。
            if ( totalBytes > MAX_RESOURCE_TOTAL_BYTES ) {
                fail("host_resource_size_limit");
                return;
            }
            files.push_back(Json{
                { "sha256", file.manifest.sha256 },
                { "path", file.manifest.cachePath },
                { "size", file.manifest.size },
            });
        }
        Json audio = Json::array();
        // nlohmann ADL 序列化完整 AudioResource 配置，路径已经重写。
        for ( const auto& resource : preparation.audioResources ) {
            audio.push_back(resource);
        }
        std::vector<std::pair<std::string, std::string>> sortedRemap(
            pathRemap.begin(), pathRemap.end());
        // unordered_map 顺序不稳定，排序后才能生成跨进程一致的清单摘要。
        std::ranges::sort(sortedRemap);
        Json remap = Json::array();
        for ( const auto& [source, target] : sortedRemap ) {
            remap.push_back(Json{ { "source", source }, { "target", target } });
        }
        Json manifest{
            // id 暂不加入对象，摘要必须覆盖除自身之外的全部规范化字段。
            { "version", RESOURCE_MANIFEST_VERSION },
            { "files", std::move(files) },
            { "audio_resources", std::move(audio) },
            { "path_remap", std::move(remap) },
        };
        const auto canonical  = Json::to_cbor(manifest);
        const auto manifestId = sha256Bytes(canonical);
        // 先计算 canonical 摘要，再把 id 写回最终传输 CBOR。
        manifest["id"]     = manifestId;
        ByteBuffer payload = Json::to_cbor(manifest);
        if ( payload.empty() || payload.size() > 1024U * 1024U ) {
            // 清单自身必须落在协作单条消息上限内。
            fail("host_resource_manifest_too_large");
            return;
        }

        const std::uint64_t generation = manifestGeneration(manifestId);
        // 只有全部验证完成后才替换房主可服务快照和 generation。
        m_hostFiles      = std::move(hostFiles);
        m_hostGeneration = generation;
        setProgress([&](auto& progress) {
            // Ready 进度反映去重后的实际文件数和明文总量。
            progress.phase = CollaborationResourceSyncPhase::Ready;
            progress.totalFiles =
                static_cast<std::uint32_t>(m_hostFiles.size());
            progress.completedFiles = progress.totalFiles;
            progress.totalBytes     = totalBytes;
            progress.currentFile.clear();
            progress.detail = "host_manifest_ready";
        });
        CollaborationResourceSyncEvent event;
        // ManifestReady 携带 generation、CBOR 正文和诊断用完整 manifestId。
        event.type    = CollaborationResourceSyncEvent::Type::ManifestReady;
        event.message = ResourceManifest{ generation, std::move(payload) };
        event.detail  = manifestId;
        pushEvent(std::move(event));
    }

    /// @brief 接收并比对访客缓存。
    /// @par generation 隔离
    /// - 外层 generation 必须匹配清单 id 派生值。
    /// - 新清单清理未发布旧 session 和旧访客密钥。
    /// - 新清单使用新的 transferSuffix 和会话目录。
    /// - 旧 generation 分块无法通过 processChunk 校验。
    /// @par 比较结果
    /// - 大小和 SHA-256 都匹配才算缓存命中。
    /// - 零字节文件本地认证物化后计为命中。
    /// - 非空缺失项保持清单索引顺序。
    /// - totalBytes 只累计真正需要传输的明文大小。
    /// - 全命中路径不得产生 ResourceRequest。
    /// @details
    /// 验证 CBOR 清单及 generation 后，为该清单创建新的 session、独立密钥、
    /// encrypted 与 materialized 目录，并限制目录权限。逐个文件比较会话内明文
    /// 大小和 SHA-256；命中项计入 cachedFiles，零字节缺失项本地建立认证容器，
    /// 其余项进入顺序下载列表。全命中直接发布 Bundle，否则请求第一块。
    /// @param manifestMessage 房主发布的版本化资源清单。
    /// @param stopToken 重启或析构发出的取消令牌。
    /// @warning 执行目录创建和文件哈希，只能在资源工作线程调用。
    void processManifest(ResourceManifest manifestMessage,
                         std::stop_token  stopToken)
    {
        // 清单内容摘要派生的 generation 必须与外层协议字段一致。
        auto decoded = decodeManifest(manifestMessage.payload);
        if ( !decoded ||
             manifestMessage.generation != manifestGeneration(decoded->id) ||
             m_cacheRoot.empty() ) {
            // startGuest 未设置根路径或任何结构/代次不一致都统一拒绝。
            fail("invalid_resource_manifest");
            return;
        }
        if ( !m_guestSessionRoot.empty() ) {
            // 同一同步器收到新清单时先清理未发布旧 session 和旧密钥。
            clearGuestSession();
            Detail::clearCollaborationResourceKey(m_guestResourceKey);
            m_guestResourceKeyReady = false;
            m_transferSuffix        = makeTransferSuffix();
            // 新清单使用新目录后缀，避免旧分块与新 generation 混合。
        }
        m_guestManifest = std::move(decoded.value());
        // 保存经验证值对象后再建立按索引下载状态。
        m_hostGeneration   = manifestMessage.generation;
        m_guestCurrentFile = 0;
        m_guestOffset      = 0;
        if ( !m_guestResourceKeyReady &&
             !Detail::generateCollaborationResourceKey(m_guestResourceKey) ) {
            fail("guest_resource_key_generation_failed");
            return;
        }
        m_guestResourceKeyReady = true;
        if ( m_guestSessionRoot.empty() ) {
            // 每次联机会话在用户缓存根下拥有独立直接子目录。
            m_guestSessionRoot = m_cacheRoot / ("session-" + m_transferSuffix);
            m_guestEncryptedRoot = m_guestSessionRoot / "encrypted";
            m_guestRoot          = m_guestSessionRoot / "materialized";
        }
        std::error_code createError;
        // 先建立加密 files 目录，再建立明文 files 目录，任一失败终止同步。
        std::filesystem::create_directories(m_guestEncryptedRoot / "files",
                                            createError);
        if ( !createError ) {
            std::filesystem::create_directories(m_guestRoot / "files",
                                                createError);
        }
        if ( createError ) {
            fail("resource_cache_create_failed");
            return;
        }
        restrictGuestSessionPermissions(m_guestSessionRoot);
        // 三层目录都尽力限制为仅当前用户访问。
        restrictGuestSessionPermissions(m_guestEncryptedRoot);
        restrictGuestSessionPermissions(m_guestRoot);

        setProgress([&](auto& progress) {
            // 清单验证完成后进入缓存比较，重置上一代次全部计数。
            progress       = {};
            progress.phase = CollaborationResourceSyncPhase::ComparingCache;
            progress.totalFiles =
                static_cast<std::uint32_t>(m_guestManifest->files.size());
        });
        std::vector<std::size_t> missing;
        // missing 保留 manifest.files 索引，后续协议 request 直接复用。
        std::uint64_t missingBytes = 0;
        std::uint32_t cachedFiles  = 0;
        for ( std::size_t index = 0; index < m_guestManifest->files.size();
              ++index ) {
            const auto& file = m_guestManifest->files[index];
            setProgress(
                [&](auto& progress) { progress.currentFile = file.cachePath; });
            const auto finalPath =
                m_guestRoot / Config::utf8ToPath(file.cachePath);
            // session 是新建目录，检查仍覆盖同一会话重复清单或预物化资源。
            std::error_code sizeError;
            const auto size = std::filesystem::file_size(finalPath, sizeError);
            const bool cached = !sizeError && size == file.size &&
                                sha256File(finalPath, stopToken) == file.sha256;
            // 大小先筛选，匹配后才承担后台文件哈希成本。
            if ( stopToken.stop_requested() ) return;
            if ( cached ) {
                // 命中项无需网络传输，比较和完成计数同步推进。
                ++cachedFiles;
                setProgress([](auto& progress) { ++progress.comparedFiles; });
                continue;
            }
            if ( file.size == 0 ) {
                // 零字节文件没有可请求分块，直接建立空认证容器并物化验证。
                const auto encryptedPath =
                    m_guestEncryptedRoot /
                    Config::utf8ToPath(file.cachePath + ".mmrsc");
                if ( !Detail::initializeCollaborationResourceFile(encryptedPath,
                                                                  0U) ||
                     !Detail::materializeCollaborationResourceFile(
                         encryptedPath, finalPath, m_guestResourceKey, 0U) ||
                     sha256File(finalPath, stopToken) != file.sha256 ) {
                    if ( stopToken.stop_requested() ) return;
                    fail("empty_resource_verify_failed");
                    return;
                }
                ++cachedFiles;
                // 成功本地建立后按缓存命中计数，不增加 missingBytes。
                setProgress([](auto& progress) { ++progress.comparedFiles; });
                continue;
            }
            missing.push_back(index);
            // 非空未命中项按清单顺序串行下载。
            missingBytes += file.size;
            setProgress([](auto& progress) { ++progress.comparedFiles; });
        }
        m_guestMissingFiles = std::move(missing);
        // 比较完成后一次性发布缓存数、完成数和需要传输的总字节。
        setProgress([&](auto& progress) {
            progress.cachedFiles    = cachedFiles;
            progress.completedFiles = cachedFiles;
            progress.totalBytes     = missingBytes;
        });
        if ( m_guestMissingFiles.empty() ) {
            // 所有文件已验证时不发送任何 ResourceRequest。
            finishGuestBundle("resource_cache_hit");
            return;
        }
        requestCurrentGuestChunk();
        // 下载状态机任何时刻只保持一个在途顺序分块。
    }

    /// @brief 从房主文件读取并发布一个分块。
    /// @par 请求边界
    /// - generation 必须等于当前房主清单代次。
    /// - resourceIndex 必须位于 m_hostFiles 范围内。
    /// - offset 必须小于非空文件大小并按块对齐。
    /// - requestedBytes 必须恰为固定块大小。
    /// - 实际读取长度必须等于 min(剩余量, 请求量)。
    /// - 认证读取失败不得发布 SendChunk。
    /// @details
    /// 验证房主密钥、generation、resourceIndex、固定请求大小和块对齐，再从对应
    /// 不可变加密快照按 offset 认证读取一块明文。末块允许响应小于请求大小，
    /// 其他字段保持请求值并通过 SendChunk 事件定向返回原 peerId。
    /// @warning 执行认证文件读取，只由串行资源 worker 调用。
    void processRequest(PeerId peerId, const ResourceRequest& request)
    {
        // 所有范围检查在索引 m_hostFiles 前完成。
        if ( !m_hostResourceKeyReady ||
             request.generation != m_hostGeneration ||
             request.resourceIndex >= m_hostFiles.size() ||
             request.requestedBytes == 0 ||
             request.requestedBytes > RESOURCE_CHUNK_BYTES ) {
            fail("invalid_resource_request");
            return;
        }
        const auto& file = m_hostFiles[request.resourceIndex];
        // offset 必须指向文件内至少一个剩余字节，空文件不会产生请求。
        if ( request.offset >= file.manifest.size ) {
            fail("invalid_resource_offset");
            return;
        }
        const auto remaining = file.manifest.size - request.offset;
        // 最后一块实际响应长度由文件剩余量限制。
        const auto bytesToRead = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, request.requestedBytes));
        if ( request.offset % RESOURCE_CHUNK_BYTES != 0 ||
             request.requestedBytes != RESOURCE_CHUNK_BYTES ) {
            // 除最后响应缩短外，客户端始终请求固定完整块并按块边界递增。
            fail("invalid_resource_request_alignment");
            return;
        }
        auto bytes = Detail::readCollaborationResourceBlock(file.sourcePath,
                                                            m_hostResourceKey,
                                                            file.manifest.size,
                                                            request.offset);
        if ( !bytes || bytes->size() != bytesToRead ) {
            // 认证失败、文件损坏或读取长度异常均不能发送部分数据。
            fail("host_resource_decrypt_failed");
            return;
        }
        CollaborationResourceSyncEvent event;
        // peerId 只决定路由，payload 内容由房主快照和请求位置决定。
        event.type    = CollaborationResourceSyncEvent::Type::SendChunk;
        event.peerId  = peerId;
        event.message = ResourceChunk{ request.generation,
                                       request.resourceIndex,
                                       request.offset,
                                       std::move(*bytes) };
        pushEvent(std::move(event));
    }

    /// @brief 顺序写入访客分块并在文件边界验证摘要。
    /// @par 分块序列不变量
    /// - 任一时刻只接受当前缺失文件的当前 offset。
    /// - generation 和 resourceIndex 必须与唯一请求一致。
    /// - 中间块长度恰为 RESOURCE_CHUNK_BYTES。
    /// - 末块长度恰为文件剩余量且不得为空。
    /// - offset 只在认证块成功追加后推进。
    /// @par 文件提交不变量
    /// - 第一块到达时初始化声明完整明文长度的 part 容器。
    /// - 所有块到齐后先物化并验证明文摘要。
    /// - 验证失败删除 part 和明文。
    /// - 验证成功后才把 part 重命名为正式加密容器。
    /// - completedFiles 只在正式容器提交后递增。
    /// - 最后文件完成后只发布一次 BundleReady。
    /// @details
    /// 当前只接受 generation、resourceIndex 和 offset 与唯一在途请求完全一致的
    /// 非空分块。每块按明文长度写入 `.part-<session>` 认证容器；整文件到齐后
    /// 先认证物化到明文，再校验清单 SHA-256，最后把 part 原子重命名为正式
    /// `.mmrsc` 缓存。任一失败删除不可信中间或明文，成功后推进下一文件。
    /// @param chunk 房主返回的单个资源分块。
    /// @param stopToken 重启或析构发出的取消令牌。
    /// @warning 执行加密写入、物化、哈希和文件重命名，只由资源 worker 调用。
    void processChunk(ResourceChunk chunk, std::stop_token stopToken)
    {
        // 未安装清单或所有缺失文件已完成时，任何分块都属于状态机错误。
        if ( !m_guestManifest ||
             m_guestCurrentFile >= m_guestMissingFiles.size() ) {
            fail("unexpected_resource_chunk");
            return;
        }
        const std::size_t expectedIndex =
            m_guestMissingFiles[m_guestCurrentFile];
        // generation、文件索引和 offset 三者共同绑定唯一预期响应。
        if ( chunk.generation != m_hostGeneration ||
             chunk.resourceIndex != expectedIndex ||
             chunk.offset != m_guestOffset || chunk.payload.empty() ) {
            fail("resource_chunk_sequence_mismatch");
            return;
        }
        const auto& file = m_guestManifest->files[expectedIndex];
        // 当前 offset 已由先前块推进，剩余量决定末块唯一合法长度。
        const auto expectedBytes =
            static_cast<std::size_t>(std::min<std::uint64_t>(
                RESOURCE_CHUNK_BYTES, file.size - m_guestOffset));
        if ( chunk.payload.size() != expectedBytes ||
             m_guestOffset % RESOURCE_CHUNK_BYTES != 0 ) {
            // 超长、短块或内部偏移失配都可能破坏认证容器布局，立即拒绝。
            fail("resource_chunk_overflow");
            return;
        }
        const auto finalPath = m_guestRoot / Config::utf8ToPath(file.cachePath);
        // 明文与加密容器使用平行目录，cachePath 保持一致。
        const auto encryptedPath =
            m_guestEncryptedRoot /
            Config::utf8ToPath(file.cachePath + ".mmrsc");
        auto partPath = encryptedPath;
        partPath += ".part-" + m_transferSuffix;
        // 会话后缀避免多个访客同步器共享 cacheRoot 时覆盖部分文件。
        if ( (m_guestOffset == 0 &&
              !Detail::initializeCollaborationResourceFile(partPath,
                                                           file.size)) ||
             !Detail::appendCollaborationResourceBlock(partPath,
                                                       m_guestResourceKey,
                                                       file.size,
                                                       m_guestOffset,
                                                       chunk.payload) ) {
            fail("resource_cache_write_failed");
            return;
        }
        m_guestOffset += chunk.payload.size();
        // 只有认证块完整追加成功后才推进 offset 和传输进度。
        setProgress([&](auto& progress) {
            progress.phase = CollaborationResourceSyncPhase::Downloading;
            progress.transferredBytes += chunk.payload.size();
            progress.currentFile = file.cachePath;
        });
        if ( m_guestOffset < file.size ) {
            // 文件尚未结束时立即请求唯一下一块，不建立并行窗口。
            requestCurrentGuestChunk();
            return;
        }

        setProgress([](auto& progress) {
            // 整文件到齐后进入验证阶段，尚未计入 completedFiles。
            progress.phase = CollaborationResourceSyncPhase::Verifying;
        });
        if ( !Detail::materializeCollaborationResourceFile(
                 partPath, finalPath, m_guestResourceKey, file.size) ||
             sha256File(finalPath, stopToken) != file.sha256 ) {
            // GCM 认证或端到端明文摘要任一失败都删除 part 和明文。
            if ( stopToken.stop_requested() ) return;
            std::error_code removeError;
            std::filesystem::remove(partPath, removeError);
            std::filesystem::remove(finalPath, removeError);
            fail("resource_sha256_mismatch");
            return;
        }
        std::error_code removeError;
        // 正式容器理论上不存在；删除允许同一 session 内重新同步覆盖。
        std::filesystem::remove(encryptedPath, removeError);
        std::error_code renameError;
        std::filesystem::rename(partPath, encryptedPath, renameError);
        // 只有摘要验证后的 part 才能原子提交为正式加密缓存。
        if ( renameError ) {
            std::filesystem::remove(finalPath, removeError);
            fail("resource_cache_commit_failed");
            return;
        }
        ++m_guestCurrentFile;
        // 文件提交成功后重置 offset，下一文件从零开始。
        m_guestOffset = 0;
        setProgress([](auto& progress) { ++progress.completedFiles; });
        if ( m_guestCurrentFile >= m_guestMissingFiles.size() ) {
            // 最后一项完成后发布项目 Bundle，把 session 生命周期交给外部。
            finishGuestBundle("resource_download_complete");
            return;
        }
        requestCurrentGuestChunk();
    }

    /// @brief 请求当前缺失文件的下一个连续分块。
    /// @details 依据 m_guestCurrentFile 和 m_guestOffset 构造唯一固定大小请求，
    /// resourceIndex 保持原清单索引；函数不修改 offset，只有成功响应才推进。
    void requestCurrentGuestChunk()
    {
        const std::size_t index = m_guestMissingFiles[m_guestCurrentFile];
        // 调用点保证索引和清单存在，状态机内部无需重复边界判断。
        const auto& file = m_guestManifest->files[index];
        setProgress([&](auto& progress) {
            progress.phase       = CollaborationResourceSyncPhase::Downloading;
            progress.currentFile = file.cachePath;
        });
        CollaborationResourceSyncEvent event;
        // requestedBytes 始终为固定块大小，房主自行缩短末块响应。
        event.type    = CollaborationResourceSyncEvent::Type::SendRequest;
        event.message = ResourceRequest{
            m_hostGeneration,
            static_cast<std::uint32_t>(index),
            m_guestOffset,
            RESOURCE_CHUNK_BYTES,
        };
        pushEvent(std::move(event));
    }

    /// @brief 发布已经完整校验的访客项目资源。
    /// @par 发布不变量
    /// - m_guestManifest 已通过摘要和引用完整性校验。
    /// - 所有非空缺失文件已提交正式认证容器。
    /// - materialized 根中的每个文件已通过明文 SHA-256。
    /// - Project 只保存改写后的音频资源路径。
    /// - pathRemap 独立复制进 Bundle。
    /// - m_guestBundlePublished 在事件入队前设置。
    /// - 同步器重启不会删除外部仍持有的 session。
    /// - 最后一个 Project 引用释放时 Owner 才递归清理目录。
    /// @details
    /// 创建 GuestResourceBundleOwner 并把项目根指向
    /// materialized，会话音频资源和 pathRemap 来自已认证清单。aliasing
    /// shared_ptr 对外表现为 Project 所有权， 实际控制块保持
    /// cacheRoot/sessionRoot，最后引用释放时自动删除整个会话。 进度先切换
    /// Ready，再发布 BundleReady；此后 restart 不再清理该目录。
    void finishGuestBundle(std::string detail)
    {
        // Owner 在堆上统一承载 Project 值与目录清理责任。
        auto owner         = std::make_shared<GuestResourceBundleOwner>();
        owner->cacheRoot   = m_cacheRoot;
        owner->sessionRoot = m_guestSessionRoot;
        owner->project.m_projectRoot        = m_guestRoot;
        owner->project.m_audioResources     = m_guestManifest->audioResources;
        owner->project.m_isTemporaryProject = true;
        // aliasing 指针指向内嵌 project，但共享 owner 控制块。
        std::shared_ptr<::MMM::Project> project(owner, &owner->project);
        m_guestBundlePublished = true;
        // 标志必须在事件发布前设置，防止紧接着 reset 删除外部即将接管的目录。
        setProgress([&](auto& progress) {
            progress.phase          = CollaborationResourceSyncPhase::Ready;
            progress.completedFiles = progress.totalFiles;
            progress.currentFile.clear();
            progress.detail = detail;
        });
        CollaborationResourceSyncEvent event;
        // pathRemap 按值复制，Bundle 生命周期不依赖 m_guestManifest 继续存在。
        event.type = CollaborationResourceSyncEvent::Type::BundleReady;
        event.bundle.project   = std::move(project);
        event.bundle.pathRemap = m_guestManifest->pathRemap;
        event.detail           = std::move(detail);
        pushEvent(std::move(event));
    }

    /// @brief 保护跨线程读取的进度快照。
    mutable std::mutex m_progressMutex;
    /// @brief 最近一次完整资源同步进度。
    CollaborationResourceSyncProgress m_progress;
    /// @brief 保护调用线程写入、worker 读取的 FIFO 任务队列。
    std::mutex m_taskMutex;
    /// @brief 在新任务或停止请求到达时唤醒 worker。
    std::condition_variable_any m_taskCondition;
    /// @brief 尚未由串行 worker 消费的资源任务。
    std::deque<ResourceTask> m_tasks;
    /// @brief 保护 worker 写入、调用线程轮询的完成事件队列。
    std::mutex m_eventMutex;
    /// @brief 按状态机处理顺序发布的完成事件。
    std::deque<CollaborationResourceSyncEvent> m_events;
    /// @brief 全局应用线程池中当前 worker 的完成句柄。
    std::future<void> m_workerFuture;
    /// @brief restart 和析构用于请求当前 worker 停止的来源。
    std::stop_source m_stopSource;

    /// @brief 当前房主 generation 可按索引读取的不可变快照文件。
    std::vector<HostFile> m_hostFiles;
    /// @brief 房主清单摘要派生的当前协议代次。
    std::uint64_t m_hostGeneration = 0;
    /// @brief 房主清单对应的认证加密不可变快照根目录。
    std::filesystem::path m_hostSnapshotRoot;
    /// @brief 房主加密快照的进程内会话密钥。
    Detail::CollaborationResourceKey m_hostResourceKey{};
    /// @brief 房主会话密钥是否已成功生成。
    bool m_hostResourceKeyReady{ false };
    /// @brief 用户配置中的协作缓存根目录。
    std::filesystem::path m_cacheRoot;
    /// @brief 访客已认证并安装的当前清单值对象。
    std::optional<DecodedManifest> m_guestManifest;
    /// @brief 本次访客联机独占、断开后自动删除的目录。
    std::filesystem::path m_guestSessionRoot;
    /// @brief 访客认证加密容器根目录。
    std::filesystem::path m_guestEncryptedRoot;
    /// @brief 仅在会话活跃期间供解码器读取的临时明文素材根目录。
    std::filesystem::path m_guestRoot;
    /// @brief 访客加密缓存的进程内会话密钥。
    Detail::CollaborationResourceKey m_guestResourceKey{};
    /// @brief 访客会话密钥是否已成功生成。
    bool m_guestResourceKeyReady{ false };
    /// @brief 访客项目是否已接管目录清理生命周期。
    bool m_guestBundlePublished{ false };
    /// @brief 访客仍需下载的 manifest.files 索引序列。
    std::vector<std::size_t> m_guestMissingFiles;
    /// @brief 当前处理的缺失文件序列位置。
    std::size_t m_guestCurrentFile = 0;
    /// @brief 当前文件下一块期望的明文偏移。
    std::uint64_t m_guestOffset = 0;
    /// @brief 本同步器独占的临时文件后缀，防止同机客户端互相截断。
    std::string m_transferSuffix;
};

/// @brief 构造资源同步外观并创建独立后台状态。
CollaborationResourceSync::CollaborationResourceSync()
    : m_impl(std::make_unique<Impl>())
{
}

/// @brief 销毁同步器并通过 Impl 回收 worker、临时目录和密钥。
CollaborationResourceSync::~CollaborationResourceSync() = default;

/// @brief 从当前项目和谱面引用异步准备房主资源清单。
/// @par 引用收集范围
/// - 普通 Note、Hold 与 Flick 的采样绑定。
/// - Polyline 根和全部通用子 Note 的采样绑定。
/// - BeatMap 全局自动采样的资源 ID。
/// - song_file_hint 与 main_audio_path 匹配的主音频资源。
/// - main_cover_path 与 cover_path 两个封面路径。
/// @par 调用线程边界
/// 只遍历当前领域对象并构造值快照，不执行文件系统读取、哈希或加密；所有阻塞
/// 资源工作在 PrepareHost 后台任务中完成。
/// @details
/// 重启状态后在调用线程收集所有音符采样绑定、自动采样资源 ID 和元数据主音频
/// hint，只复制被引用 AudioResource；封面路径作为额外文件。纯值 HostPreparation
/// 入队后，后台线程负责路径验证、快照和哈希。
/// @param project 提供根目录和音频资源表的当前项目。
/// @param beatmap 提供实际资源引用的当前谱面。
void CollaborationResourceSync::startHost(const ::MMM::Project& project,
                                          const ::MMM::BeatMap& beatmap)
{
    // 新房主准备作废旧 generation 和任务，并立即发布 Preparing。
    m_impl->restart();
    m_impl->setPreparing();
    std::unordered_set<std::string> references;
    const auto collectBinding = [&](const ::MMM::Note& note) {
        // 只收集非空绑定 ID，集合自动合并重复引用。
        if ( note.m_sampleBinding &&
             !note.m_sampleBinding->m_audioResourceId.empty() ) {
            references.insert(note.m_sampleBinding->m_audioResourceId);
        }
    };
    for ( const auto& note : beatmap.m_noteData.notes ) collectBinding(note);
    for ( const auto& hold : beatmap.m_noteData.holds ) collectBinding(hold);
    for ( const auto& flick : beatmap.m_noteData.flicks ) collectBinding(flick);
    for ( const auto& polyline : beatmap.m_noteData.polylines ) {
        // Polyline 根和通用子物件都可能携带独立采样绑定。
        collectBinding(polyline);
        for ( const auto& subNote : polyline.m_subNotes ) {
            collectBinding(subNote.get());
        }
    }
    for ( const auto& sample : beatmap.m_audioSamples ) {
        // 自动采样不是 Note，需单独纳入引用集合。
        if ( !sample.m_audioResourceId.empty() ) {
            references.insert(sample.m_audioResourceId);
        }
    }

    HostPreparation preparation;
    // 后台任务只得到值副本，不引用可被编辑线程继续修改的领域对象。
    preparation.projectRoot = project.m_projectRoot;
    for ( const auto& resource : project.m_audioResources ) {
        // 未被当前谱面使用的项目素材不加入房间清单和传输。
        if ( isReferencedAudioResource(
                 resource, references, beatmap.m_baseMapMetadata) ) {
            preparation.audioResources.push_back(resource);
        }
    }
    const auto appendExtra = [&](const std::filesystem::path& path) {
        // 空封面字段不是错误，也不应增加准备进度总数。
        if ( !path.empty() ) preparation.extraPaths.push_back(path);
    };
    appendExtra(beatmap.m_baseMapMetadata.main_cover_path);
    appendExtra(beatmap.m_baseMapMetadata.cover_path);
    ResourceTask task;
    // 单个 PrepareHost 任务串行冻结全部筛选资源。
    task.type        = ResourceTask::Type::PrepareHost;
    task.preparation = std::move(preparation);
    m_impl->enqueue(std::move(task));
}

/// @brief 重启为访客并等待指定缓存根上的房主清单。
/// @param cacheRoot 本次联机会话目录的父级缓存根。
void CollaborationResourceSync::startGuest(std::filesystem::path cacheRoot)
{
    m_impl->restart();
    m_impl->setCacheRoot(std::move(cacheRoot));
}

/// @brief 取消当前工作并恢复空同步状态与新后台 worker。
/// @warning 会等待旧文件任务结束，只用于会话切换或显式关闭。
void CollaborationResourceSync::reset()
{
    m_impl->restart();
}

/// @brief 把房主清单移动到访客后台验证队列。
/// @note 调用点通常来自网络事件线程，函数本身只移动值并短暂锁定任务队列。
void CollaborationResourceSync::receiveManifest(ResourceManifest manifest)
{
    ResourceTask task;
    task.type     = ResourceTask::Type::Manifest;
    task.manifest = std::move(manifest);
    m_impl->enqueue(std::move(task));
}

/// @brief 把访客资源请求排入房主后台读取队列。
/// @note PeerId 与请求一起排队，响应事件保持原访客路由身份。
/// @param peerId 响应事件需要定向发送的访客。
/// @param request 固定分块请求。
void CollaborationResourceSync::receiveRequest(PeerId          peerId,
                                               ResourceRequest request)
{
    ResourceTask task;
    task.type    = ResourceTask::Type::Request;
    task.peerId  = peerId;
    task.request = request;
    m_impl->enqueue(std::move(task));
}

/// @brief 把房主资源分块移动到访客后台写入队列。
/// @note 大型 payload 通过移动转交，不在调用线程执行缓存写入。
void CollaborationResourceSync::receiveChunk(ResourceChunk chunk)
{
    ResourceTask task;
    task.type  = ResourceTask::Type::Chunk;
    task.chunk = std::move(chunk);
    m_impl->enqueue(std::move(task));
}

/// @brief 非阻塞取得下一条清单、请求、分块、Bundle 或错误事件。
/// @note 空队列立即返回 false，适合由外部 update 循环轮询。
/// @return 有事件可用并已移动到输出参数时返回 true。
bool CollaborationResourceSync::pollEvent(CollaborationResourceSyncEvent& event)
{
    return m_impl->poll(event);
}

/// @brief 获取当前线程安全资源同步进度副本。
/// @return 最近一次完整进度状态。
CollaborationResourceSyncProgress CollaborationResourceSync::progress() const
{
    return m_impl->progress();
}
}  // namespace MMM::Network::Collaboration
