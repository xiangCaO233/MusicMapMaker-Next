#pragma once

#include "collaboration/CollaborationTypes.h"

#include <cstdint>
#include <expected>
#include <span>
#include <variant>

namespace MMM::Collaboration
{
/// @brief 当前协作线协议主版本。
inline constexpr std::uint16_t COLLABORATION_PROTOCOL_VERSION = 1;

/// @brief 线协议允许的消息类型。
enum class CollaborationMessageKind : std::uint8_t {
    EditRequest        = 1,
    CommittedOperation = 2,
    RevisionAck        = 3,
    ResyncRequest      = 4,
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

/// @brief 可编码到协作线协议的消息联合体。
using CollaborationMessage =
    std::variant<EditRequest, CommittedOperation, RevisionAck, ResyncRequest>;

/// @brief 协作消息编解码失败原因。
enum class ProtocolError : std::uint8_t {
    InvalidMagic,
    UnsupportedVersion,
    InvalidReservedField,
    UnknownMessageKind,
    TruncatedMessage,
    InvalidMessageLength,
    OperationTooLarge,
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
}  // namespace MMM::Collaboration
