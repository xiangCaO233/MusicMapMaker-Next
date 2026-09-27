#include "CollaborationResourceCipher.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <mbedtls/cipher.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <string_view>

namespace MMM::Network::Collaboration::Detail
{
namespace
{
/// @brief 加密容器魔数与格式版本。
/// @note 末尾 01 表示当前布局版本，不兼容变更必须使用新的魔数。
constexpr std::array<std::uint8_t, 8> RESOURCE_CONTAINER_MAGIC{ 'M', 'M', 'M',
                                                                'R', 'S', 'C',
                                                                '0', '1' };
/// @brief 每个认证分块的 GCM Tag 长度。
/// @note 16 字节提供完整 128 位认证强度，读取端按固定长度定位记录。
constexpr std::size_t RESOURCE_TAG_BYTES = 16U;
/// @brief 每个分块 Nonce 中随机前缀的长度。
/// @note 剩余八字节由单调分块索引占用，确保单个容器内不复用 Nonce。
constexpr std::size_t RESOURCE_NONCE_PREFIX_BYTES = 4U;
/// @brief GCM 推荐的 96 位 Nonce 长度。
/// @note 由四字节容器随机前缀和八字节大端分块索引组成。
constexpr std::size_t RESOURCE_NONCE_BYTES = 12U;
/// @brief 固定容器头长度：魔数、分块长度、明文总长度和 Nonce 前缀。
/// @note 8 + 4 + 8 + 4 的布局也作为每块认证数据的一部分。
constexpr std::size_t RESOURCE_HEADER_BYTES = 24U;
/// @brief 分块附加认证数据长度：完整容器头与分块索引。
/// @note 认证索引阻止合法密文记录在同一容器内被调换位置。
constexpr std::size_t RESOURCE_AAD_BYTES = RESOURCE_HEADER_BYTES + 8U;
/// @brief DRBG 个性化字符串，隔离项目内其它随机用途。
/// @note 字符串不作为秘密，只用于区分随机生成上下文。
constexpr std::string_view RESOURCE_DRBG_PERSONALIZATION =
    "MusicMapMaker-Collaboration-Resource-AES256GCM";

/// @brief 已验证的加密容器头。
struct ResourceContainerHeader {
    /// @brief 原始头字节，直接参与每个分块的附加认证数据。
    /// @note 必须保留读取时的原始表示，不能重新序列化后替代认证输入。
    std::array<std::uint8_t, RESOURCE_HEADER_BYTES> bytes{};
    /// @brief 容器声明的明文总长度。
    /// @note 解密入口要求它与调用方清单中的期望长度完全一致。
    std::uint64_t plainSize = 0;
    /// @brief 每个分块 Nonce 的随机前缀。
    /// @note 同一容器所有记录共享前缀，并用分块索引区分完整 Nonce。
    std::array<std::uint8_t, RESOURCE_NONCE_PREFIX_BYTES> noncePrefix{};
};

/// @brief 向固定缓冲区写入大端 32 位整数。
/// @param value 待编码的无符号值。
/// @param bytes 至少可写四字节的目标缓冲区。
/// @note 网络字节序使容器格式不依赖主机端序。
void writeBigEndian32(std::uint32_t value, std::uint8_t* bytes)
{
    // 从最高有效字节开始写入，掩码明确截取每个八位分量。
    bytes[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    bytes[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

/// @brief 从固定缓冲区读取大端 32 位整数。
/// @param bytes 至少可读四字节的输入缓冲区。
/// @return 按网络字节序恢复的无符号值。
/// @note 每个字节先提升到 uint32_t，避免移位发生符号扩展。
std::uint32_t readBigEndian32(const std::uint8_t* bytes)
{
    // 位或组合与写入布局严格互逆，用于验证容器声明的分块大小。
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

/// @brief 向固定缓冲区写入大端 64 位整数。
/// @param value 待编码的无符号值。
/// @param bytes 至少可写八字节的目标缓冲区。
void writeBigEndian64(std::uint64_t value, std::uint8_t* bytes)
{
    // index 从零递增，而移位量从最高字节递减到最低字节。
    for ( std::size_t index = 0; index < 8U; ++index ) {
        bytes[index] =
            static_cast<std::uint8_t>((value >> ((7U - index) * 8U)) & 0xFFU);
    }
}

/// @brief 从固定缓冲区读取大端 64 位整数。
/// @param bytes 至少可读八字节的输入缓冲区。
/// @return 恢复后的明文长度或分块索引。
std::uint64_t readBigEndian64(const std::uint8_t* bytes)
{
    // 每轮先为下一个字节腾出低八位，再合入当前输入。
    std::uint64_t value = 0;
    for ( std::size_t index = 0; index < 8U; ++index ) {
        value = (value << 8U) | bytes[index];
    }
    return value;
}

/// @brief 使用独立 CTR-DRBG 上下文生成安全随机字节。
/// @param output 接收随机数据的可写缓冲区。
/// @return 系统熵播种和随机生成均成功时返回 true。
/// @warning 涉及系统熵源和初始化开销，只能在资源密钥或容器创建路径调用。
bool fillSecureRandom(std::span<std::uint8_t> output)
{
    // 熵源与 DRBG 都限定为本次调用的局部上下文，避免共享锁和状态。
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    // 个性化字符串在播种阶段绑定本用途，不参与生成后的密钥材料。
    const int seedResult =
        mbedtls_ctr_drbg_seed(&drbg,
                              mbedtls_entropy_func,
                              &entropy,
                              reinterpret_cast<const unsigned char*>(
                                  RESOURCE_DRBG_PERSONALIZATION.data()),
                              RESOURCE_DRBG_PERSONALIZATION.size());
    // 只有播种成功才请求输出；否则保留原错误码统一返回失败。
    const int randomResult =
        seedResult == 0
            ? mbedtls_ctr_drbg_random(&drbg, output.data(), output.size())
            : seedResult;
    // 两个 mbedTLS 上下文在所有路径上按构造逆序释放。
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return randomResult == 0;
}

/// @brief 构造并写入一个新的容器头。
/// @param output 已以二进制模式打开的目标流。
/// @param plainSize 源文件的完整明文字节数。
/// @return 随机前缀生成且完整头写入成功时返回已解析头。
std::optional<ResourceContainerHeader> writeContainerHeader(
    std::ostream& output, std::uint64_t plainSize)
{
    // 值初始化保证所有保留字节为零，再覆盖当前格式定义的字段。
    ResourceContainerHeader header;
    // 魔数同时标识文件类型和格式版本，固定占据头部前八字节。
    std::copy(RESOURCE_CONTAINER_MAGIC.begin(),
              RESOURCE_CONTAINER_MAGIC.end(),
              header.bytes.begin());
    // 分块长度写入容器，读取方拒绝与编译期协议常量不一致的文件。
    writeBigEndian32(COLLABORATION_RESOURCE_BLOCK_BYTES,
                     header.bytes.data() + 8U);
    writeBigEndian64(plainSize, header.bytes.data() + 12U);
    // 每个新容器生成独立随机前缀，失败时不得写出可误识别的头。
    if ( !fillSecureRandom(header.noncePrefix) ) return std::nullopt;
    // 将随机前缀复制进原始头，使其随每块 AAD 一同被认证。
    std::copy(header.noncePrefix.begin(),
              header.noncePrefix.end(),
              header.bytes.begin() + 20U);
    header.plainSize = plainSize;
    // 一次写出固定头，避免调用方看到部分字段分阶段落盘。
    output.write(reinterpret_cast<const char*>(header.bytes.data()),
                 static_cast<std::streamsize>(header.bytes.size()));
    if ( !output ) return std::nullopt;
    // 返回值复用相同原始字节生成后续 Nonce 和 AAD。
    return header;
}

/// @brief 读取并验证容器头的魔数、版本和固定分块长度。
/// @param input 已定位到容器起点的二进制输入流。
/// @param expectedPlainSize 清单或调用方声明的明文总长度。
/// @return 所有固定字段与预期长度合法时返回已验证头。
std::optional<ResourceContainerHeader> readContainerHeader(
    std::istream& input, std::uint64_t expectedPlainSize)
{
    // 必须完整读满固定头，截断文件不能进入后续记录偏移计算。
    ResourceContainerHeader header;
    input.read(reinterpret_cast<char*>(header.bytes.data()),
               static_cast<std::streamsize>(header.bytes.size()));
    if ( input.gcount() != static_cast<std::streamsize>(header.bytes.size()) ||
         // 魔数检查同时拒绝未知格式版本和非资源容器文件。
         !std::equal(RESOURCE_CONTAINER_MAGIC.begin(),
                     RESOURCE_CONTAINER_MAGIC.end(),
                     header.bytes.begin()) ||
         readBigEndian32(header.bytes.data() + 8U) !=
             COLLABORATION_RESOURCE_BLOCK_BYTES ) {
        return std::nullopt;
    }
    // 明文长度由大端字段恢复，并与外部可信清单交叉验证。
    header.plainSize = readBigEndian64(header.bytes.data() + 12U);
    if ( header.plainSize != expectedPlainSize ) return std::nullopt;
    // 复制已认证前缀供每块 Nonce 重建，不重新生成任何随机数据。
    std::copy_n(header.bytes.begin() + 20U,
                header.noncePrefix.size(),
                header.noncePrefix.begin());
    return header;
}

/// @brief 计算指定明文长度对应的认证分块数量。
/// @param plainSize 明文总字节数。
/// @return 空文件为零，否则返回向上取整后的固定分块数。
std::uint64_t blockCount(std::uint64_t plainSize)
{
    // 单独处理零值，避免 plainSize - 1 发生无符号下溢。
    if ( plainSize == 0 ) return 0;
    // 减一后整除等价于向上取整，且不会产生加法溢出。
    return 1U + (plainSize - 1U) / COLLABORATION_RESOURCE_BLOCK_BYTES;
}

/// @brief 计算完整加密容器应有的字节数并拒绝整数溢出。
/// @param plainSize 容器声明的明文总长度。
/// @return 头、密文和所有 Tag 的精确总长度；溢出时返回空。
std::optional<std::uint64_t> encryptedContainerSize(std::uint64_t plainSize)
{
    // GCM 密文与明文等长，每个非空分块额外追加固定 Tag。
    const auto blocks = blockCount(plainSize);
    // 在执行乘法和总和前反向检查剩余 uint64_t 空间。
    if ( blocks > (std::numeric_limits<std::uint64_t>::max() -
                   RESOURCE_HEADER_BYTES - plainSize) /
                      RESOURCE_TAG_BYTES ) {
        return std::nullopt;
    }
    // 已证明不溢出，可作为随机访问和完整容器校验的权威长度。
    return RESOURCE_HEADER_BYTES + plainSize + blocks * RESOURCE_TAG_BYTES;
}

/// @brief 计算指定分块的实际明文长度。
/// @param plainSize 完整明文长度。
/// @param blockIndex 从零开始的分块索引。
/// @return 完整块大小、尾块剩余大小或越界时的零。
std::size_t plainBlockSize(std::uint64_t plainSize, std::uint64_t blockIndex)
{
    // 调用方先将索引限制在 blockCount 内，乘积因固定块大小保持有效。
    const std::uint64_t offset =
        blockIndex * COLLABORATION_RESOURCE_BLOCK_BYTES;
    // 越过或恰好位于 EOF 的索引没有可处理明文。
    if ( offset >= plainSize ) return 0;
    // 非尾块取固定大小，尾块只取剩余字节。
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        COLLABORATION_RESOURCE_BLOCK_BYTES, plainSize - offset));
}

/// @brief 构造不会在同一文件中复用的 96 位 GCM Nonce。
/// @param header 含容器随机前缀的已验证头。
/// @param blockIndex 当前记录的单调索引。
/// @return 四字节随机前缀加八字节大端索引的 Nonce。
std::array<std::uint8_t, RESOURCE_NONCE_BYTES> makeNonce(
    const ResourceContainerHeader& header, std::uint64_t blockIndex)
{
    // 值初始化后先复制容器级随机前缀到高位部分。
    std::array<std::uint8_t, RESOURCE_NONCE_BYTES> nonce{};
    std::copy(
        header.noncePrefix.begin(), header.noncePrefix.end(), nonce.begin());
    // 完整 64 位索引避免大型资源内重复 Nonce。
    writeBigEndian64(blockIndex, nonce.data() + RESOURCE_NONCE_PREFIX_BYTES);
    return nonce;
}

/// @brief 将容器头和分块索引绑定为 GCM 附加认证数据。
/// @param header 含格式、长度和 Nonce 前缀的原始容器头。
/// @param blockIndex 当前记录位置。
/// @return 不加密但参与 GCM Tag 认证的固定字节数组。
std::array<std::uint8_t, RESOURCE_AAD_BYTES> makeAdditionalData(
    const ResourceContainerHeader& header, std::uint64_t blockIndex)
{
    // 认证完整原始头可检测明文长度、格式和随机前缀的任何篡改。
    std::array<std::uint8_t, RESOURCE_AAD_BYTES> aad{};
    std::copy(header.bytes.begin(), header.bytes.end(), aad.begin());
    // 认证分块索引可检测记录交换、重复或错误偏移读取。
    writeBigEndian64(blockIndex, aad.data() + RESOURCE_HEADER_BYTES);
    return aad;
}

/// @brief 使用 AES-256-GCM 加密并认证单个固定分块。
/// @param key 当前同步会话的 256 位密钥。
/// @param header 当前目标容器头。
/// @param blockIndex 当前明文分块索引。
/// @param plainBytes 只在调用期间有效的明文视图。
/// @param cipherBytes 接收等长密文的输出缓冲区。
/// @param tag 接收固定 128 位认证标签。
/// @return 密钥设置和一次性 GCM 加密均成功时返回 true。
bool encryptBlock(const CollaborationResourceKey&               key,
                  const ResourceContainerHeader&                header,
                  std::uint64_t                                 blockIndex,
                  std::span<const std::uint8_t>                 plainBytes,
                  ByteBuffer&                                   cipherBytes,
                  std::array<std::uint8_t, RESOURCE_TAG_BYTES>& tag)
{
    // GCM 不改变载荷长度，先精确调整输出以供 mbedTLS 写入。
    cipherBytes.resize(plainBytes.size());
    // Nonce 与 AAD 都从相同头和索引确定生成，解密端可无状态重建。
    const auto          nonce = makeNonce(header, blockIndex);
    const auto          aad   = makeAdditionalData(header, blockIndex);
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    // key.size 以字节表示，mbedTLS 接口要求传入位数。
    int result = mbedtls_gcm_setkey(
        &context, MBEDTLS_CIPHER_ID_AES, key.data(), key.size() * 8U);
    if ( result == 0 ) {
        // 单次调用同时产生密文与 Tag，避免未认证中间状态被写入容器。
        result = mbedtls_gcm_crypt_and_tag(&context,
                                           MBEDTLS_GCM_ENCRYPT,
                                           plainBytes.size(),
                                           nonce.data(),
                                           nonce.size(),
                                           aad.data(),
                                           aad.size(),
                                           plainBytes.data(),
                                           cipherBytes.data(),
                                           tag.size(),
                                           tag.data());
    }
    // 无论密钥设置或加密是否成功都释放 GCM 上下文。
    mbedtls_gcm_free(&context);
    return result == 0;
}

/// @brief 使用 AES-256-GCM 验证并解密单个固定分块。
/// @param key 当前同步会话的 256 位密钥。
/// @param header 已验证的容器头。
/// @param blockIndex 当前密文记录索引。
/// @param cipherBytes 等同预期明文长度的密文视图。
/// @param tag 紧随密文的固定认证标签。
/// @param plainBytes 成功时接收明文，认证失败时被清零并清空。
/// @return Tag、Nonce、AAD 与密钥共同验证成功时返回 true。
bool decryptBlock(const CollaborationResourceKey& key,
                  const ResourceContainerHeader&  header,
                  std::uint64_t                   blockIndex,
                  std::span<const std::uint8_t>   cipherBytes,
                  std::span<const std::uint8_t> tag, ByteBuffer& plainBytes)
{
    // 预分配等长输出，认证 API 只在成功时产生可消费明文语义。
    plainBytes.resize(cipherBytes.size());
    const auto          nonce = makeNonce(header, blockIndex);
    const auto          aad   = makeAdditionalData(header, blockIndex);
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    // 与加密端使用完全相同的 AES-256 密钥位数和派生输入。
    int result = mbedtls_gcm_setkey(
        &context, MBEDTLS_CIPHER_ID_AES, key.data(), key.size() * 8U);
    if ( result == 0 ) {
        // auth_decrypt 在返回成功前同时验证 Tag，调用方不接触未认证数据。
        result = mbedtls_gcm_auth_decrypt(&context,
                                          cipherBytes.size(),
                                          nonce.data(),
                                          nonce.size(),
                                          aad.data(),
                                          aad.size(),
                                          tag.data(),
                                          tag.size(),
                                          cipherBytes.data(),
                                          plainBytes.data());
    }
    // 上下文释放与认证结果无关，避免错误路径遗留密码学状态。
    mbedtls_gcm_free(&context);
    if ( result != 0 ) {
        // 认证失败时显式擦除可能写入输出缓冲的任何字节。
        mbedtls_platform_zeroize(plainBytes.data(), plainBytes.size());
        // 清空长度进一步阻止调用方误用未经认证的数据。
        plainBytes.clear();
    }
    return result == 0;
}

/// @brief 将 SHA-256 原始摘要格式化为小写十六进制文本。
/// @param digest 固定 32 字节明文摘要。
/// @return 固定 64 位小写十六进制字符串。
std::string formatSha256(const std::array<std::uint8_t, 32>& digest)
{
    // 固定查表避免区域设置和流格式器产生大小写差异。
    constexpr std::string_view digits = "0123456789abcdef";
    // 每个摘要字节展开为两个字符，前导零始终保留。
    std::string result(digest.size() * 2U, '\0');
    for ( std::size_t index = 0; index < digest.size(); ++index ) {
        // 高四位与低四位分别映射到规范小写字符。
        result[index * 2U]     = digits[digest[index] >> 4U];
        result[index * 2U + 1] = digits[digest[index] & 0x0FU];
    }
    return result;
}

/// @brief 向输出流写入一个完整的密文与 Tag 记录。
/// @param output 已定位到下一条记录末尾的二进制流。
/// @param key 当前会话密钥。
/// @param header 当前容器头。
/// @param blockIndex 待写记录索引。
/// @param plainBytes 当前分块明文。
/// @return 加密、密文写入和 Tag 写入均成功时返回 true。
bool writeEncryptedRecord(std::ostream&                   output,
                          const CollaborationResourceKey& key,
                          const ResourceContainerHeader&  header,
                          std::uint64_t                   blockIndex,
                          std::span<const std::uint8_t>   plainBytes)
{
    // 密文和 Tag 只在本函数栈内暂存，写出后立即擦除密文缓冲。
    ByteBuffer                                   cipherBytes;
    std::array<std::uint8_t, RESOURCE_TAG_BYTES> tag{};
    if ( !encryptBlock(
             key, header, blockIndex, plainBytes, cipherBytes, tag) ) {
        // 加密失败时输出流尚未写入当前记录，调用方可安全终止容器。
        return false;
    }
    // 记录布局固定为等长密文后紧跟 16 字节认证 Tag。
    output.write(reinterpret_cast<const char*>(cipherBytes.data()),
                 static_cast<std::streamsize>(cipherBytes.size()));
    output.write(reinterpret_cast<const char*>(tag.data()),
                 static_cast<std::streamsize>(tag.size()));
    // 密文虽非秘密明文，仍按敏感中间缓冲统一主动擦除。
    mbedtls_platform_zeroize(cipherBytes.data(), cipherBytes.size());
    // 两次 write 的任一失败都会反映在流状态中。
    return static_cast<bool>(output);
}
}  // namespace

/// @brief 从系统熵生成新的资源会话密钥，并在失败时保持全零输出。
/// @param key 接收 32 字节密钥的固定数组。
/// @return 随机生成成功时返回 true。
bool generateCollaborationResourceKey(CollaborationResourceKey& key)
{
    // 先擦除调用方旧密钥，防止生成失败后继续使用过期会话材料。
    clearCollaborationResourceKey(key);
    if ( fillSecureRandom(key) ) return true;
    // 随机函数可能部分写入缓冲，失败路径必须再次完整擦除。
    clearCollaborationResourceKey(key);
    return false;
}

/// @brief 使用 mbedTLS 不可优化掉的入口擦除资源会话密钥。
/// @param key 待清零的固定密钥数组。
void clearCollaborationResourceKey(CollaborationResourceKey& key)
{
    // platform_zeroize 防止编译器把生命周期末尾的安全清零删除。
    mbedtls_platform_zeroize(key.data(), key.size());
}

/// @brief 流式计算明文摘要并写出逐块认证的加密资源容器。
/// @param source 明文源文件。
/// @param destination 失败时会删除的目标容器路径。
/// @param key 当前同步会话密钥。
/// @param stopToken 后台任务取消令牌。
/// @return 成功时返回明文 SHA-256 和字节数，否则返回空。
/// @warning 后台文件 I/O 路径，不得从渲染、UI 或逻辑热路径直接调用。
std::optional<CollaborationEncryptedResourceDigest>
encryptCollaborationResourceFile(const std::filesystem::path&    source,
                                 const std::filesystem::path&    destination,
                                 const CollaborationResourceKey& key,
                                 std::stop_token                 stopToken)
{
    // 先固定源文件长度，整个读取循环都以该快照约束边界。
    std::error_code sizeError;
    const auto      fileSize = std::filesystem::file_size(source, sizeError);
    if ( sizeError ) return std::nullopt;
    // 输入和目标都使用二进制模式，避免平台文本转换改变摘要或密文。
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if ( !input || !output ) return std::nullopt;
    // 容器头先写入并生成随机 Nonce 前缀，失败时不进入摘要循环。
    const auto header = writeContainerHeader(output, fileSize);
    if ( !header ) return std::nullopt;

    mbedtls_sha256_context sha256;
    mbedtls_sha256_init(&sha256);
    // 参数零选择 SHA-256；初始化失败时释放上下文并保留失败结果。
    if ( mbedtls_sha256_starts(&sha256, 0) != 0 ) {
        mbedtls_sha256_free(&sha256);
        return std::nullopt;
    }
    // 固定块缓冲限制内存峰值，并在退出前完整擦除明文内容。
    std::array<std::uint8_t, COLLABORATION_RESOURCE_BLOCK_BYTES> plain{};
    std::uint64_t                                                blockIndex = 0;
    std::uint64_t                                                totalBytes = 0;
    bool                                                         success = true;
    // 以初始 fileSize 为终点，源文件并发缩短会通过读取长度不符失败。
    while ( input && totalBytes < fileSize ) {
        // 每个分块边界响应取消，绝不发布部分完成的目标容器。
        if ( stopToken.stop_requested() ) {
            success = false;
            break;
        }
        // 尾块只请求剩余长度，其他轮次请求完整固定块。
        const auto requested = static_cast<std::streamsize>(
            std::min<std::uint64_t>(plain.size(), fileSize - totalBytes));
        input.read(reinterpret_cast<char*>(plain.data()), requested);
        const auto count = input.gcount();
        // 短读表示源文件变化或 I/O 错误，不能以部分数据完成容器。
        if ( count != requested || count <= 0 ) {
            success = false;
            break;
        }
        // span 只覆盖本轮有效字节，尾块不会认证缓冲区剩余旧数据。
        const auto bytes = std::span<const std::uint8_t>(
            plain.data(), static_cast<std::size_t>(count));
        if ( mbedtls_sha256_update(&sha256, bytes.data(), bytes.size()) != 0 ||
             !writeEncryptedRecord(output, key, *header, blockIndex, bytes) ) {
            // 摘要或加密任一失败都终止，统一走目标文件删除路径。
            success = false;
            break;
        }
        totalBytes += bytes.size();
        // 索引只在记录成功写出后推进，Nonce 与落盘顺序严格一致。
        ++blockIndex;
    }
    // 仅在全部字节处理后 finalize 摘要，结果对应明文而非密文。
    std::array<std::uint8_t, 32> digest{};
    if ( !success || input.bad() || totalBytes != fileSize ||
         mbedtls_sha256_finish(&sha256, digest.data()) != 0 ) {
        success = false;
    }
    // 摘要上下文和明文缓冲在检查所有结果后统一释放、擦除。
    mbedtls_sha256_free(&sha256);
    mbedtls_platform_zeroize(plain.data(), plain.size());
    output.close();
    if ( !success || !output ) {
        // 取消、读取、加密或 flush 失败都删除不可用的部分容器。
        std::error_code removeError;
        std::filesystem::remove(destination, removeError);
        return std::nullopt;
    }
    // 只在完整容器关闭成功后发布摘要和固定源长度。
    return CollaborationEncryptedResourceDigest{ formatSha256(digest),
                                                 fileSize };
}

std::optional<ByteBuffer> readCollaborationResourceBlock(
    const std::filesystem::path& source, const CollaborationResourceKey& key,
    std::uint64_t expectedPlainSize, std::uint64_t plainOffset)
{
    // 偏移必须指向现有分块起点，EOF 和块内偏移都不能随机解密。
    if ( plainOffset >= expectedPlainSize ||
         plainOffset % COLLABORATION_RESOURCE_BLOCK_BYTES != 0 ) {
        return std::nullopt;
    }
    // 先用完整容器长度排除截断、尾随数据和整数溢出。
    std::error_code sizeError;
    const auto      actualSize = std::filesystem::file_size(source, sizeError);
    const auto      expectedSize = encryptedContainerSize(expectedPlainSize);
    if ( sizeError || !expectedSize || actualSize != *expectedSize ) {
        return std::nullopt;
    }
    // 二进制输入打开后首先认证容器头的固定格式和明文长度。
    std::ifstream input(source, std::ios::binary);
    if ( !input ) return std::nullopt;
    const auto header = readContainerHeader(input, expectedPlainSize);
    if ( !header ) return std::nullopt;
    // 明文对齐偏移可无损换算为记录索引和对应 Tag 开销。
    const std::uint64_t blockIndex =
        plainOffset / COLLABORATION_RESOURCE_BLOCK_BYTES;
    const auto encryptedOffset =
        RESOURCE_HEADER_BYTES + plainOffset + blockIndex * RESOURCE_TAG_BYTES;
    // 每条记录密文等长，因此随机偏移只需累计之前记录的 Tag。
    input.seekg(static_cast<std::streamoff>(encryptedOffset), std::ios::beg);
    // 最后一块可能短于固定块长，按声明的明文边界精确读取。
    const auto plainSize = plainBlockSize(expectedPlainSize, blockIndex);
    ByteBuffer cipherBytes(plainSize);
    std::array<std::uint8_t, RESOURCE_TAG_BYTES> tag{};
    input.read(reinterpret_cast<char*>(cipherBytes.data()),
               static_cast<std::streamsize>(cipherBytes.size()));
    input.read(reinterpret_cast<char*>(tag.data()),
               static_cast<std::streamsize>(tag.size()));
    // 密文或 Tag 任一短读都拒绝，不向解密器传入部分记录。
    if ( !input ) return std::nullopt;
    ByteBuffer plainBytes;
    if ( !decryptBlock(
             key, *header, blockIndex, cipherBytes, tag, plainBytes) ) {
        return std::nullopt;
    }
    // 只有 GCM 认证通过的明文才离开函数。
    return plainBytes;
}

/// @brief 创建仅含认证容器头的接收端资源文件。
/// @param destination 新容器路径。
/// @param plainSize 预期最终明文总长度。
/// @return 随机头完整关闭并写入成功时返回 true。
bool initializeCollaborationResourceFile(
    const std::filesystem::path& destination, std::uint64_t plainSize)
{
    // trunc 确保重试不会在旧容器记录之后继续追加。
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if ( !output ) return false;
    const auto header = writeContainerHeader(output, plainSize);
    // close 后再次检查流状态，捕获延迟到 flush 的写入错误。
    output.close();
    return header.has_value() && static_cast<bool>(output);
}

/// @brief 验证顺序和长度后追加一个认证加密分块。
/// @param destination 已初始化且只含连续前序记录的容器。
/// @param key 当前会话密钥。
/// @param plainSize 最终明文总长度。
/// @param plainOffset 当前分块的对齐明文偏移。
/// @param plainBytes 与当前位置期望长度完全一致的明文。
/// @return 头、顺序、长度和记录写入均成功时返回 true。
bool appendCollaborationResourceBlock(const std::filesystem::path& destination,
                                      const CollaborationResourceKey& key,
                                      std::uint64_t                   plainSize,
                                      std::uint64_t                 plainOffset,
                                      std::span<const std::uint8_t> plainBytes)
{
    // 只接受固定分块边界，且不能从声明明文末尾之后追加。
    if ( plainOffset % COLLABORATION_RESOURCE_BLOCK_BYTES != 0 ||
         plainOffset > plainSize ) {
        return false;
    }
    const std::uint64_t blockIndex =
        plainOffset / COLLABORATION_RESOURCE_BLOCK_BYTES;
    // 完整块与尾块都必须一次提供精确长度，禁止部分记录落盘。
    if ( plainBytes.size() != plainBlockSize(plainSize, blockIndex) ) {
        return false;
    }
    // 当前文件长度必须恰好对应前序连续记录，阻止跳块或重复追加。
    std::error_code sizeError;
    const auto currentSize = std::filesystem::file_size(destination, sizeError);
    const auto expectedCurrentSize =
        RESOURCE_HEADER_BYTES + plainOffset + blockIndex * RESOURCE_TAG_BYTES;
    if ( sizeError || currentSize != expectedCurrentSize ) return false;
    // 追加前重新读取头，验证容器格式和本次调用的明文长度一致。
    std::ifstream headerInput(destination, std::ios::binary);
    if ( !headerInput ) return false;
    const auto header = readContainerHeader(headerInput, plainSize);
    headerInput.close();
    if ( !header ) return false;
    // 只有所有前置条件通过后才以 app 模式打开，避免失败时改变文件。
    std::ofstream output(destination, std::ios::binary | std::ios::app);
    if ( !output ) return false;
    const bool success =
        writeEncryptedRecord(output, key, *header, blockIndex, plainBytes);
    // close 后检查流状态，保证缓冲写入错误不会被 success 掩盖。
    output.close();
    return success && static_cast<bool>(output);
}

/// @brief 逐条认证解密完整容器，并用临时文件落地最终明文。
/// @param source 完整加密容器。
/// @param destination 成功后原子出现的会话素材路径。
/// @param key 当前会话密钥。
/// @param expectedPlainSize 清单声明的明文长度。
/// @return 全部记录认证且临时文件重命名成功时返回 true。
bool materializeCollaborationResourceFile(
    const std::filesystem::path&    source,
    const std::filesystem::path&    destination,
    const CollaborationResourceKey& key, std::uint64_t expectedPlainSize)
{
    // 完整长度校验在创建任何明文临时文件前执行。
    std::error_code sizeError;
    const auto      actualSize = std::filesystem::file_size(source, sizeError);
    const auto      expectedSize = encryptedContainerSize(expectedPlainSize);
    if ( sizeError || !expectedSize || actualSize != *expectedSize ) {
        return false;
    }
    // 固定后缀与目标相邻，最终 rename 不跨文件系统。
    auto temporary = destination;
    temporary += ".part-materialize";
    std::error_code cleanupError;
    std::filesystem::remove(temporary, cleanupError);
    // 输出只写临时路径，认证完成前不会覆盖正式目的文件。
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if ( !input || !output ) return false;
    // 所有失败路径关闭并删除可能含部分明文的临时文件。
    const auto fail = [&]() {
        output.close();
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return false;
    };
    const auto header = readContainerHeader(input, expectedPlainSize);
    if ( !header ) return fail();
    // 记录数由可信期望长度推导，逐块顺序读取密文与 Tag。
    for ( std::uint64_t blockIndex = 0;
          blockIndex < blockCount(expectedPlainSize);
          ++blockIndex ) {
        const auto plainSize = plainBlockSize(expectedPlainSize, blockIndex);
        ByteBuffer cipherBytes(plainSize);
        std::array<std::uint8_t, RESOURCE_TAG_BYTES> tag{};
        input.read(reinterpret_cast<char*>(cipherBytes.data()),
                   static_cast<std::streamsize>(cipherBytes.size()));
        input.read(reinterpret_cast<char*>(tag.data()),
                   static_cast<std::streamsize>(tag.size()));
        // 任何短读都触发临时明文清理。
        if ( !input ) return fail();
        ByteBuffer plainBytes;
        if ( !decryptBlock(
                 key, *header, blockIndex, cipherBytes, tag, plainBytes) ) {
            return fail();
        }
        // 认证通过后才写出当前明文块，随后立即擦除内存缓冲。
        output.write(reinterpret_cast<const char*>(plainBytes.data()),
                     static_cast<std::streamsize>(plainBytes.size()));
        mbedtls_platform_zeroize(plainBytes.data(), plainBytes.size());
        if ( !output ) return fail();
    }
    // 关闭成功证明所有明文已刷新到临时文件。
    output.close();
    if ( !output ) return fail();
    std::error_code renameError;
    // 最后一步重命名使调用方不会观察到半明文化的目标文件。
    std::filesystem::rename(temporary, destination, renameError);
    if ( renameError ) return fail();
    return true;
}
}  // namespace MMM::Network::Collaboration::Detail
