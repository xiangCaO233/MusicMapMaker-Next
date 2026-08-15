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
constexpr std::array<std::uint8_t, 8> RESOURCE_CONTAINER_MAGIC{ 'M', 'M', 'M',
                                                                'R', 'S', 'C',
                                                                '0', '1' };
/// @brief 每个认证分块的 GCM Tag 长度。
constexpr std::size_t RESOURCE_TAG_BYTES = 16U;
/// @brief 每个分块 Nonce 中随机前缀的长度。
constexpr std::size_t RESOURCE_NONCE_PREFIX_BYTES = 4U;
/// @brief GCM 推荐的 96 位 Nonce 长度。
constexpr std::size_t RESOURCE_NONCE_BYTES = 12U;
/// @brief 固定容器头长度：魔数、分块长度、明文总长度和 Nonce 前缀。
constexpr std::size_t RESOURCE_HEADER_BYTES = 24U;
/// @brief 分块附加认证数据长度：完整容器头与分块索引。
constexpr std::size_t RESOURCE_AAD_BYTES = RESOURCE_HEADER_BYTES + 8U;
/// @brief DRBG 个性化字符串，隔离项目内其它随机用途。
constexpr std::string_view RESOURCE_DRBG_PERSONALIZATION =
    "MusicMapMaker-Collaboration-Resource-AES256GCM";

/// @brief 已验证的加密容器头。
struct ResourceContainerHeader {
    /// @brief 原始头字节，直接参与每个分块的附加认证数据。
    std::array<std::uint8_t, RESOURCE_HEADER_BYTES> bytes{};
    /// @brief 容器声明的明文总长度。
    std::uint64_t plainSize = 0;
    /// @brief 每个分块 Nonce 的随机前缀。
    std::array<std::uint8_t, RESOURCE_NONCE_PREFIX_BYTES> noncePrefix{};
};

/// @brief 向固定缓冲区写入大端 32 位整数。
void writeBigEndian32(std::uint32_t value, std::uint8_t* bytes)
{
    bytes[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    bytes[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

/// @brief 从固定缓冲区读取大端 32 位整数。
std::uint32_t readBigEndian32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

/// @brief 向固定缓冲区写入大端 64 位整数。
void writeBigEndian64(std::uint64_t value, std::uint8_t* bytes)
{
    for ( std::size_t index = 0; index < 8U; ++index ) {
        bytes[index] =
            static_cast<std::uint8_t>((value >> ((7U - index) * 8U)) & 0xFFU);
    }
}

/// @brief 从固定缓冲区读取大端 64 位整数。
std::uint64_t readBigEndian64(const std::uint8_t* bytes)
{
    std::uint64_t value = 0;
    for ( std::size_t index = 0; index < 8U; ++index ) {
        value = (value << 8U) | bytes[index];
    }
    return value;
}

/// @brief 使用独立 CTR-DRBG 上下文生成安全随机字节。
bool fillSecureRandom(std::span<std::uint8_t> output)
{
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    const int seedResult =
        mbedtls_ctr_drbg_seed(&drbg,
                              mbedtls_entropy_func,
                              &entropy,
                              reinterpret_cast<const unsigned char*>(
                                  RESOURCE_DRBG_PERSONALIZATION.data()),
                              RESOURCE_DRBG_PERSONALIZATION.size());
    const int randomResult =
        seedResult == 0
            ? mbedtls_ctr_drbg_random(&drbg, output.data(), output.size())
            : seedResult;
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return randomResult == 0;
}

/// @brief 构造并写入一个新的容器头。
std::optional<ResourceContainerHeader> writeContainerHeader(
    std::ostream& output, std::uint64_t plainSize)
{
    ResourceContainerHeader header;
    std::copy(RESOURCE_CONTAINER_MAGIC.begin(),
              RESOURCE_CONTAINER_MAGIC.end(),
              header.bytes.begin());
    writeBigEndian32(COLLABORATION_RESOURCE_BLOCK_BYTES,
                     header.bytes.data() + 8U);
    writeBigEndian64(plainSize, header.bytes.data() + 12U);
    if ( !fillSecureRandom(header.noncePrefix) ) return std::nullopt;
    std::copy(header.noncePrefix.begin(),
              header.noncePrefix.end(),
              header.bytes.begin() + 20U);
    header.plainSize = plainSize;
    output.write(reinterpret_cast<const char*>(header.bytes.data()),
                 static_cast<std::streamsize>(header.bytes.size()));
    if ( !output ) return std::nullopt;
    return header;
}

/// @brief 读取并验证容器头的魔数、版本和固定分块长度。
std::optional<ResourceContainerHeader> readContainerHeader(
    std::istream& input, std::uint64_t expectedPlainSize)
{
    ResourceContainerHeader header;
    input.read(reinterpret_cast<char*>(header.bytes.data()),
               static_cast<std::streamsize>(header.bytes.size()));
    if ( input.gcount() != static_cast<std::streamsize>(header.bytes.size()) ||
         !std::equal(RESOURCE_CONTAINER_MAGIC.begin(),
                     RESOURCE_CONTAINER_MAGIC.end(),
                     header.bytes.begin()) ||
         readBigEndian32(header.bytes.data() + 8U) !=
             COLLABORATION_RESOURCE_BLOCK_BYTES ) {
        return std::nullopt;
    }
    header.plainSize = readBigEndian64(header.bytes.data() + 12U);
    if ( header.plainSize != expectedPlainSize ) return std::nullopt;
    std::copy_n(header.bytes.begin() + 20U,
                header.noncePrefix.size(),
                header.noncePrefix.begin());
    return header;
}

/// @brief 计算指定明文长度对应的认证分块数量。
std::uint64_t blockCount(std::uint64_t plainSize)
{
    if ( plainSize == 0 ) return 0;
    return 1U + (plainSize - 1U) / COLLABORATION_RESOURCE_BLOCK_BYTES;
}

/// @brief 计算完整加密容器应有的字节数并拒绝整数溢出。
std::optional<std::uint64_t> encryptedContainerSize(std::uint64_t plainSize)
{
    const auto blocks = blockCount(plainSize);
    if ( blocks > (std::numeric_limits<std::uint64_t>::max() -
                   RESOURCE_HEADER_BYTES - plainSize) /
                      RESOURCE_TAG_BYTES ) {
        return std::nullopt;
    }
    return RESOURCE_HEADER_BYTES + plainSize + blocks * RESOURCE_TAG_BYTES;
}

/// @brief 计算指定分块的实际明文长度。
std::size_t plainBlockSize(std::uint64_t plainSize, std::uint64_t blockIndex)
{
    const std::uint64_t offset =
        blockIndex * COLLABORATION_RESOURCE_BLOCK_BYTES;
    if ( offset >= plainSize ) return 0;
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        COLLABORATION_RESOURCE_BLOCK_BYTES, plainSize - offset));
}

/// @brief 构造不会在同一文件中复用的 96 位 GCM Nonce。
std::array<std::uint8_t, RESOURCE_NONCE_BYTES> makeNonce(
    const ResourceContainerHeader& header, std::uint64_t blockIndex)
{
    std::array<std::uint8_t, RESOURCE_NONCE_BYTES> nonce{};
    std::copy(
        header.noncePrefix.begin(), header.noncePrefix.end(), nonce.begin());
    writeBigEndian64(blockIndex, nonce.data() + RESOURCE_NONCE_PREFIX_BYTES);
    return nonce;
}

/// @brief 将容器头和分块索引绑定为 GCM 附加认证数据。
std::array<std::uint8_t, RESOURCE_AAD_BYTES> makeAdditionalData(
    const ResourceContainerHeader& header, std::uint64_t blockIndex)
{
    std::array<std::uint8_t, RESOURCE_AAD_BYTES> aad{};
    std::copy(header.bytes.begin(), header.bytes.end(), aad.begin());
    writeBigEndian64(blockIndex, aad.data() + RESOURCE_HEADER_BYTES);
    return aad;
}

/// @brief 使用 AES-256-GCM 加密并认证单个固定分块。
bool encryptBlock(const CollaborationResourceKey&               key,
                  const ResourceContainerHeader&                header,
                  std::uint64_t                                 blockIndex,
                  std::span<const std::uint8_t>                 plainBytes,
                  ByteBuffer&                                   cipherBytes,
                  std::array<std::uint8_t, RESOURCE_TAG_BYTES>& tag)
{
    cipherBytes.resize(plainBytes.size());
    const auto          nonce = makeNonce(header, blockIndex);
    const auto          aad   = makeAdditionalData(header, blockIndex);
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int result = mbedtls_gcm_setkey(
        &context, MBEDTLS_CIPHER_ID_AES, key.data(), key.size() * 8U);
    if ( result == 0 ) {
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
    mbedtls_gcm_free(&context);
    return result == 0;
}

/// @brief 使用 AES-256-GCM 验证并解密单个固定分块。
bool decryptBlock(const CollaborationResourceKey& key,
                  const ResourceContainerHeader&  header,
                  std::uint64_t                   blockIndex,
                  std::span<const std::uint8_t>   cipherBytes,
                  std::span<const std::uint8_t> tag, ByteBuffer& plainBytes)
{
    plainBytes.resize(cipherBytes.size());
    const auto          nonce = makeNonce(header, blockIndex);
    const auto          aad   = makeAdditionalData(header, blockIndex);
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int result = mbedtls_gcm_setkey(
        &context, MBEDTLS_CIPHER_ID_AES, key.data(), key.size() * 8U);
    if ( result == 0 ) {
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
    mbedtls_gcm_free(&context);
    if ( result != 0 ) {
        mbedtls_platform_zeroize(plainBytes.data(), plainBytes.size());
        plainBytes.clear();
    }
    return result == 0;
}

/// @brief 将 SHA-256 原始摘要格式化为小写十六进制文本。
std::string formatSha256(const std::array<std::uint8_t, 32>& digest)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string                result(digest.size() * 2U, '\0');
    for ( std::size_t index = 0; index < digest.size(); ++index ) {
        result[index * 2U]     = digits[digest[index] >> 4U];
        result[index * 2U + 1] = digits[digest[index] & 0x0FU];
    }
    return result;
}

/// @brief 向输出流写入一个完整的密文与 Tag 记录。
bool writeEncryptedRecord(std::ostream&                   output,
                          const CollaborationResourceKey& key,
                          const ResourceContainerHeader&  header,
                          std::uint64_t                   blockIndex,
                          std::span<const std::uint8_t>   plainBytes)
{
    ByteBuffer                                   cipherBytes;
    std::array<std::uint8_t, RESOURCE_TAG_BYTES> tag{};
    if ( !encryptBlock(
             key, header, blockIndex, plainBytes, cipherBytes, tag) ) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(cipherBytes.data()),
                 static_cast<std::streamsize>(cipherBytes.size()));
    output.write(reinterpret_cast<const char*>(tag.data()),
                 static_cast<std::streamsize>(tag.size()));
    mbedtls_platform_zeroize(cipherBytes.data(), cipherBytes.size());
    return static_cast<bool>(output);
}
}  // namespace

bool generateCollaborationResourceKey(CollaborationResourceKey& key)
{
    clearCollaborationResourceKey(key);
    if ( fillSecureRandom(key) ) return true;
    clearCollaborationResourceKey(key);
    return false;
}

void clearCollaborationResourceKey(CollaborationResourceKey& key)
{
    mbedtls_platform_zeroize(key.data(), key.size());
}

std::optional<CollaborationEncryptedResourceDigest>
encryptCollaborationResourceFile(const std::filesystem::path&    source,
                                 const std::filesystem::path&    destination,
                                 const CollaborationResourceKey& key,
                                 std::stop_token                 stopToken)
{
    std::error_code sizeError;
    const auto      fileSize = std::filesystem::file_size(source, sizeError);
    if ( sizeError ) return std::nullopt;
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if ( !input || !output ) return std::nullopt;
    const auto header = writeContainerHeader(output, fileSize);
    if ( !header ) return std::nullopt;

    mbedtls_sha256_context sha256;
    mbedtls_sha256_init(&sha256);
    if ( mbedtls_sha256_starts(&sha256, 0) != 0 ) {
        mbedtls_sha256_free(&sha256);
        return std::nullopt;
    }
    std::array<std::uint8_t, COLLABORATION_RESOURCE_BLOCK_BYTES> plain{};
    std::uint64_t                                                blockIndex = 0;
    std::uint64_t                                                totalBytes = 0;
    bool                                                         success = true;
    while ( input && totalBytes < fileSize ) {
        if ( stopToken.stop_requested() ) {
            success = false;
            break;
        }
        const auto requested = static_cast<std::streamsize>(
            std::min<std::uint64_t>(plain.size(), fileSize - totalBytes));
        input.read(reinterpret_cast<char*>(plain.data()), requested);
        const auto count = input.gcount();
        if ( count != requested || count <= 0 ) {
            success = false;
            break;
        }
        const auto bytes = std::span<const std::uint8_t>(
            plain.data(), static_cast<std::size_t>(count));
        if ( mbedtls_sha256_update(&sha256, bytes.data(), bytes.size()) != 0 ||
             !writeEncryptedRecord(output, key, *header, blockIndex, bytes) ) {
            success = false;
            break;
        }
        totalBytes += bytes.size();
        ++blockIndex;
    }
    std::array<std::uint8_t, 32> digest{};
    if ( !success || input.bad() || totalBytes != fileSize ||
         mbedtls_sha256_finish(&sha256, digest.data()) != 0 ) {
        success = false;
    }
    mbedtls_sha256_free(&sha256);
    mbedtls_platform_zeroize(plain.data(), plain.size());
    output.close();
    if ( !success || !output ) {
        std::error_code removeError;
        std::filesystem::remove(destination, removeError);
        return std::nullopt;
    }
    return CollaborationEncryptedResourceDigest{ formatSha256(digest),
                                                 fileSize };
}

std::optional<ByteBuffer> readCollaborationResourceBlock(
    const std::filesystem::path& source, const CollaborationResourceKey& key,
    std::uint64_t expectedPlainSize, std::uint64_t plainOffset)
{
    if ( plainOffset >= expectedPlainSize ||
         plainOffset % COLLABORATION_RESOURCE_BLOCK_BYTES != 0 ) {
        return std::nullopt;
    }
    std::error_code sizeError;
    const auto      actualSize = std::filesystem::file_size(source, sizeError);
    const auto      expectedSize = encryptedContainerSize(expectedPlainSize);
    if ( sizeError || !expectedSize || actualSize != *expectedSize ) {
        return std::nullopt;
    }
    std::ifstream input(source, std::ios::binary);
    if ( !input ) return std::nullopt;
    const auto header = readContainerHeader(input, expectedPlainSize);
    if ( !header ) return std::nullopt;
    const std::uint64_t blockIndex =
        plainOffset / COLLABORATION_RESOURCE_BLOCK_BYTES;
    const auto encryptedOffset =
        RESOURCE_HEADER_BYTES + plainOffset + blockIndex * RESOURCE_TAG_BYTES;
    input.seekg(static_cast<std::streamoff>(encryptedOffset), std::ios::beg);
    const auto plainSize = plainBlockSize(expectedPlainSize, blockIndex);
    ByteBuffer cipherBytes(plainSize);
    std::array<std::uint8_t, RESOURCE_TAG_BYTES> tag{};
    input.read(reinterpret_cast<char*>(cipherBytes.data()),
               static_cast<std::streamsize>(cipherBytes.size()));
    input.read(reinterpret_cast<char*>(tag.data()),
               static_cast<std::streamsize>(tag.size()));
    if ( !input ) return std::nullopt;
    ByteBuffer plainBytes;
    if ( !decryptBlock(
             key, *header, blockIndex, cipherBytes, tag, plainBytes) ) {
        return std::nullopt;
    }
    return plainBytes;
}

bool initializeCollaborationResourceFile(
    const std::filesystem::path& destination, std::uint64_t plainSize)
{
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if ( !output ) return false;
    const auto header = writeContainerHeader(output, plainSize);
    output.close();
    return header.has_value() && static_cast<bool>(output);
}

bool appendCollaborationResourceBlock(const std::filesystem::path& destination,
                                      const CollaborationResourceKey& key,
                                      std::uint64_t                   plainSize,
                                      std::uint64_t                 plainOffset,
                                      std::span<const std::uint8_t> plainBytes)
{
    if ( plainOffset % COLLABORATION_RESOURCE_BLOCK_BYTES != 0 ||
         plainOffset > plainSize ) {
        return false;
    }
    const std::uint64_t blockIndex =
        plainOffset / COLLABORATION_RESOURCE_BLOCK_BYTES;
    if ( plainBytes.size() != plainBlockSize(plainSize, blockIndex) ) {
        return false;
    }
    std::error_code sizeError;
    const auto currentSize = std::filesystem::file_size(destination, sizeError);
    const auto expectedCurrentSize =
        RESOURCE_HEADER_BYTES + plainOffset + blockIndex * RESOURCE_TAG_BYTES;
    if ( sizeError || currentSize != expectedCurrentSize ) return false;
    std::ifstream headerInput(destination, std::ios::binary);
    if ( !headerInput ) return false;
    const auto header = readContainerHeader(headerInput, plainSize);
    headerInput.close();
    if ( !header ) return false;
    std::ofstream output(destination, std::ios::binary | std::ios::app);
    if ( !output ) return false;
    const bool success =
        writeEncryptedRecord(output, key, *header, blockIndex, plainBytes);
    output.close();
    return success && static_cast<bool>(output);
}

bool materializeCollaborationResourceFile(
    const std::filesystem::path&    source,
    const std::filesystem::path&    destination,
    const CollaborationResourceKey& key, std::uint64_t expectedPlainSize)
{
    std::error_code sizeError;
    const auto      actualSize = std::filesystem::file_size(source, sizeError);
    const auto      expectedSize = encryptedContainerSize(expectedPlainSize);
    if ( sizeError || !expectedSize || actualSize != *expectedSize ) {
        return false;
    }
    auto temporary = destination;
    temporary += ".part-materialize";
    std::error_code cleanupError;
    std::filesystem::remove(temporary, cleanupError);
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if ( !input || !output ) return false;
    const auto fail = [&]() {
        output.close();
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return false;
    };
    const auto header = readContainerHeader(input, expectedPlainSize);
    if ( !header ) return fail();
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
        if ( !input ) return fail();
        ByteBuffer plainBytes;
        if ( !decryptBlock(
                 key, *header, blockIndex, cipherBytes, tag, plainBytes) ) {
            return fail();
        }
        output.write(reinterpret_cast<const char*>(plainBytes.data()),
                     static_cast<std::streamsize>(plainBytes.size()));
        mbedtls_platform_zeroize(plainBytes.data(), plainBytes.size());
        if ( !output ) return fail();
    }
    output.close();
    if ( !output ) return fail();
    std::error_code renameError;
    std::filesystem::rename(temporary, destination, renameError);
    if ( renameError ) return fail();
    return true;
}
}  // namespace MMM::Network::Collaboration::Detail
