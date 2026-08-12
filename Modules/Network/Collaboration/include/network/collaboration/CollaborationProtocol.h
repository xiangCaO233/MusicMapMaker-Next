#pragma once

#include "network/collaboration/CollaborationTypes.h"

#include <cstdint>
#include <expected>
#include <span>
#include <variant>

namespace MMM::Network::Collaboration
{
/// @brief 当前协作线协议主版本。
inline constexpr std::uint16_t COLLABORATION_PROTOCOL_VERSION = 6;

/// @brief 线协议允许的消息类型。
enum class CollaborationMessageKind : std::uint8_t {
    EditRequest            = 1,
    CommittedOperation     = 2,
    RevisionAck            = 3,
    ResyncRequest          = 4,
    ParticipantIdentity    = 5,
    ParticipantLeft        = 6,
    StateSnapshot          = 7,
    ResourceManifest       = 8,
    ResourceRequest        = 9,
    ResourceChunk          = 10,
    ParticipantViewport    = 11,
    ChatMessage            = 12,
    ParticipantPermissions = 13,
};

/// @brief 访客确认已经连续应用到的版本。
struct RevisionAck {
    /// @brief 已连续应用的最高版本。
    std::uint64_t revision = 0;
};

/// @brief 访客请求房主从指定版本开始补发增量日志。
struct ResyncRequest {
    /// @brief 需要补发的首个版本。
    std::uint64_t fromRevision = 0;
};

/// @brief 房主向新加入客户端发送的当前完整谱面状态。
struct StateSnapshot {
    /// @brief 快照已经包含的最高房间版本。
    std::uint64_t revision = 0;
    /// @brief 可独立恢复的完整规范化谱面负载。
    ByteBuffer payload;
};

/// @brief 房主向访客发布的项目资源清单。
struct ResourceManifest {
    /// @brief 本次清单的非零稳定代次。
    std::uint64_t generation = 0;
    /// @brief 独立资源清单编解码器生成的规范负载。
    ByteBuffer payload;
};

/// @brief 访客按偏移请求一个资源分块。
struct ResourceRequest {
    /// @brief 对应资源清单代次。
    std::uint64_t generation = 0;
    /// @brief 清单内的资源索引。
    std::uint32_t resourceIndex = 0;
    /// @brief 请求的文件字节偏移。
    std::uint64_t offset = 0;
    /// @brief 本次允许返回的最大字节数。
    std::uint32_t requestedBytes = 0;
};

/// @brief 房主返回的一个连续资源分块。
struct ResourceChunk {
    /// @brief 对应资源清单代次。
    std::uint64_t generation = 0;
    /// @brief 清单内的资源索引。
    std::uint32_t resourceIndex = 0;
    /// @brief 分块在文件中的字节偏移。
    std::uint64_t offset = 0;
    /// @brief 分块原始字节。
    ByteBuffer payload;
};

/// @brief 可编码到协作线协议的消息联合体。
using CollaborationMessage =
    std::variant<EditRequest, CommittedOperation, RevisionAck, ResyncRequest,
                 ParticipantIdentity, ParticipantLeft, StateSnapshot,
                 ResourceManifest, ResourceRequest, ResourceChunk,
                 ParticipantViewport, CollaborationChatMessage,
                 ParticipantPermissions>;

/// @brief 协作消息编解码失败原因。
enum class ProtocolError : std::uint8_t {
    InvalidMagic,
    UnsupportedVersion,
    InvalidReservedField,
    UnknownMessageKind,
    TruncatedMessage,
    InvalidMessageLength,
    OperationTooLarge,
    InvalidStableIdentity,
    InvalidCreatorIdentity,
    InvalidViewportState,
    InvalidChatMessage,
    InvalidPermissions,
};

/// @brief 把一条协作消息编码为带版本和长度字段的二进制帧。
/// @param message 待编码消息。
/// @param maxOperationBytes 单条操作负载上限。
/// @return 成功时返回完整帧，失败时返回协议错误。
[[nodiscard]] std::expected<ByteBuffer, ProtocolError>
encodeCollaborationMessage(const CollaborationMessage& message,
                           std::size_t                 maxOperationBytes);

/// @brief 从完整二进制帧解析一条协作消息。
/// @param bytes 完整帧字节。
/// @param maxOperationBytes 单条操作负载上限。
/// @return 成功时返回消息，失败时返回协议错误。
[[nodiscard]] std::expected<CollaborationMessage, ProtocolError>
decodeCollaborationMessage(std::span<const std::uint8_t> bytes,
                           std::size_t                   maxOperationBytes);
}  // namespace MMM::Network::Collaboration
