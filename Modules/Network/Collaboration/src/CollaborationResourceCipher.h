#pragma once

#include "network/collaboration/CollaborationTypes.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>

namespace MMM::Network::Collaboration::Detail
{
/// @brief AES-256-GCM 会话资源密钥字节数。
inline constexpr std::size_t COLLABORATION_RESOURCE_KEY_BYTES = 32U;
/// @brief 加密容器的固定明文分块大小，与资源协议请求上限一致。
inline constexpr std::uint32_t COLLABORATION_RESOURCE_BLOCK_BYTES = 64U * 1024U;

/// @brief 只驻留于当前进程内存的资源会话密钥。
using CollaborationResourceKey =
    std::array<std::uint8_t, COLLABORATION_RESOURCE_KEY_BYTES>;

/// @brief 加密快照对应的明文摘要与长度。
struct CollaborationEncryptedResourceDigest {
    /// @brief 明文 SHA-256 小写十六进制摘要。
    std::string sha256;
    /// @brief 明文字节数。
    std::uint64_t size = 0;
};

/// @brief 从系统熵源生成一把新的 AES-256 会话密钥。
/// @param key 接收密钥的固定缓冲区。
/// @return 随机源与 DRBG 均成功时返回 true。
[[nodiscard]] bool generateCollaborationResourceKey(
    CollaborationResourceKey& key);

/// @brief 使用不可优化掉的清零入口擦除会话密钥。
/// @param key 待擦除的密钥。
void clearCollaborationResourceKey(CollaborationResourceKey& key);

/// @brief 将源文件流式写成固定分块的 AES-256-GCM 认证加密容器。
/// @param source 明文源文件。
/// @param destination 加密容器路径。
/// @param key 当前同步器独占的会话密钥。
/// @param stopToken 后台任务停止令牌。
/// @return 成功时返回明文摘要与长度。
[[nodiscard]] std::optional<CollaborationEncryptedResourceDigest>
encryptCollaborationResourceFile(const std::filesystem::path&    source,
                                 const std::filesystem::path&    destination,
                                 const CollaborationResourceKey& key,
                                 std::stop_token                 stopToken);

/// @brief 从加密容器认证解密一个协议分块，不生成明文临时文件。
/// @param source 加密容器路径。
/// @param key 当前同步器独占的会话密钥。
/// @param expectedPlainSize 清单声明的明文总长度。
/// @param plainOffset 请求的明文偏移，必须按固定分块对齐。
/// @return 容器头、长度和 GCM Tag 均验证成功时返回明文分块。
[[nodiscard]] std::optional<ByteBuffer> readCollaborationResourceBlock(
    const std::filesystem::path& source, const CollaborationResourceKey& key,
    std::uint64_t expectedPlainSize, std::uint64_t plainOffset);

/// @brief 创建一个空的访客加密资源容器并写入随机 Nonce 前缀。
/// @param destination 加密容器路径。
/// @param plainSize 清单声明的明文总长度。
/// @return 容器头成功落盘时返回 true。
[[nodiscard]] bool initializeCollaborationResourceFile(
    const std::filesystem::path& destination, std::uint64_t plainSize);

/// @brief 将一个连续明文协议分块认证加密后追加到访客容器。
/// @param destination 加密容器路径。
/// @param key 当前同步器独占的会话密钥。
/// @param plainSize 清单声明的明文总长度。
/// @param plainOffset 当前分块的明文偏移。
/// @param plainBytes 当前分块明文。
/// @return 容器顺序、长度和加密操作均有效时返回 true。
[[nodiscard]] bool appendCollaborationResourceBlock(
    const std::filesystem::path&    destination,
    const CollaborationResourceKey& key, std::uint64_t plainSize,
    std::uint64_t plainOffset, std::span<const std::uint8_t> plainBytes);

/// @brief 逐块认证解密会话容器到仅在联机期间存在的素材目录。
/// @param source 加密容器路径。
/// @param destination 会话素材明文路径。
/// @param key 当前同步器独占的会话密钥。
/// @param expectedPlainSize 清单声明的明文总长度。
/// @return 所有分块 GCM Tag 和容器边界均验证成功时返回 true。
[[nodiscard]] bool materializeCollaborationResourceFile(
    const std::filesystem::path&    source,
    const std::filesystem::path&    destination,
    const CollaborationResourceKey& key, std::uint64_t expectedPlainSize);
}  // namespace MMM::Network::Collaboration::Detail
