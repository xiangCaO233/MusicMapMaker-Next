#include "network/collaboration/CollaborationProtocol.h"
#include "config/CreatorIdentity.h"

#include <limits>
#include <type_traits>
#include <utility>

namespace MMM::Network::Collaboration
{
namespace
{
/// @brief 协作帧固定魔数，对应 ASCII `MMMC` 的小端表示。
constexpr std::uint32_t PROTOCOL_MAGIC = 0x434D4D4DU;
/// @brief 帧头固定字节数。
constexpr std::size_t PROTOCOL_HEADER_BYTES = 12;

/// @brief 以小端序追加 16 位无符号整数。
/// @param output 目标字节序列。
/// @param value 待写入数值。
void appendUint16(ByteBuffer& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

/// @brief 以小端序追加 32 位无符号整数。
/// @param output 目标字节序列。
/// @param value 待写入数值。
void appendUint32(ByteBuffer& output, std::uint32_t value)
{
    for ( std::uint32_t shift = 0; shift < 32U; shift += 8U ) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/// @brief 以小端序追加 64 位无符号整数。
/// @param output 目标字节序列。
/// @param value 待写入数值。
void appendUint64(ByteBuffer& output, std::uint64_t value)
{
    for ( std::uint32_t shift = 0; shift < 64U; shift += 8U ) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/// @brief 追加带 32 位长度的操作负载。
/// @param output 目标字节序列。
/// @param payload 操作负载。
void appendPayload(ByteBuffer& output, const ByteBuffer& payload)
{
    appendUint32(output, static_cast<std::uint32_t>(payload.size()));
    output.insert(output.end(), payload.begin(), payload.end());
}

/// @brief 追加带 16 位长度的短 UTF-8 字符串。
/// @param output 目标字节序列。
/// @param value 待写入字符串。
void appendShortString(ByteBuffer& output, std::string_view value)
{
    appendUint16(output, static_cast<std::uint16_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

/// @brief 对二进制帧执行有界小端读取。
class ByteReader
{
public:
    /// @brief 创建读取器。
    /// @param bytes 待读取字节视图。
    explicit ByteReader(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

    /// @brief 读取 8 位无符号整数。
    /// @param value 输出数值。
    /// @return 剩余字节足够时返回 true。
    [[nodiscard]] bool readUint8(std::uint8_t& value)
    {
        if ( remaining() < 1 ) {
            return false;
        }
        value = m_bytes[m_offset++];
        return true;
    }

    /// @brief 读取小端 16 位无符号整数。
    /// @param value 输出数值。
    /// @return 剩余字节足够时返回 true。
    [[nodiscard]] bool readUint16(std::uint16_t& value)
    {
        if ( remaining() < 2 ) {
            return false;
        }
        value = static_cast<std::uint16_t>(m_bytes[m_offset]) |
                static_cast<std::uint16_t>(m_bytes[m_offset + 1]) << 8U;
        m_offset += 2;
        return true;
    }

    /// @brief 读取小端 32 位无符号整数。
    /// @param value 输出数值。
    /// @return 剩余字节足够时返回 true。
    [[nodiscard]] bool readUint32(std::uint32_t& value)
    {
        if ( remaining() < 4 ) {
            return false;
        }
        value = 0;
        for ( std::uint32_t shift = 0; shift < 32U; shift += 8U ) {
            value |= static_cast<std::uint32_t>(m_bytes[m_offset++]) << shift;
        }
        return true;
    }

    /// @brief 读取小端 64 位无符号整数。
    /// @param value 输出数值。
    /// @return 剩余字节足够时返回 true。
    [[nodiscard]] bool readUint64(std::uint64_t& value)
    {
        if ( remaining() < 8 ) {
            return false;
        }
        value = 0;
        for ( std::uint32_t shift = 0; shift < 64U; shift += 8U ) {
            value |= static_cast<std::uint64_t>(m_bytes[m_offset++]) << shift;
        }
        return true;
    }

    /// @brief 读取指定长度的字节序列。
    /// @param length 读取字节数。
    /// @param output 输出缓冲区。
    /// @return 剩余字节足够时返回 true。
    [[nodiscard]] bool readBytes(std::size_t length, ByteBuffer& output)
    {
        if ( remaining() < length ) {
            return false;
        }
        const auto begin =
            m_bytes.begin() + static_cast<std::ptrdiff_t>(m_offset);
        output.assign(begin, begin + static_cast<std::ptrdiff_t>(length));
        m_offset += length;
        return true;
    }

    /// @brief 读取指定长度的 UTF-8 字符串字节。
    /// @param length 读取字节数。
    /// @param output 输出字符串。
    /// @return 剩余字节足够时返回 true。
    [[nodiscard]] bool readString(std::size_t length, std::string& output)
    {
        if ( remaining() < length ) {
            return false;
        }
        const auto* begin = reinterpret_cast<const char*>(m_bytes.data()) +
                            static_cast<std::ptrdiff_t>(m_offset);
        output.assign(begin, length);
        m_offset += length;
        return true;
    }

    /// @brief 返回尚未读取的字节数。
    /// @return 剩余字节数。
    [[nodiscard]] std::size_t remaining() const
    {
        return m_bytes.size() - m_offset;
    }

private:
    /// @brief 完整只读字节视图。
    std::span<const std::uint8_t> m_bytes;
    /// @brief 当前读取偏移。
    std::size_t m_offset = 0;
};

/// @brief 校验操作负载能否进入线协议的 32 位长度字段。
/// @param payloadBytes 实际负载字节数。
/// @param maxOperationBytes 配置的负载上限。
/// @return 负载合法时返回 true。
[[nodiscard]] bool isOperationSizeValid(std::size_t payloadBytes,
                                        std::size_t maxOperationBytes)
{
    return payloadBytes <= maxOperationBytes &&
           payloadBytes <= std::numeric_limits<std::uint32_t>::max();
}
}  // namespace

std::expected<ByteBuffer, ProtocolError> encodeCollaborationMessage(
    const CollaborationMessage& message, std::size_t maxOperationBytes)
{
    ByteBuffer               body;
    CollaborationMessageKind kind{};

    if ( const auto* request = std::get_if<EditRequest>(&message) ) {
        if ( !isOperationSizeValid(request->payload.size(),
                                   maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::EditRequest;
        body.reserve(20U + request->payload.size());
        appendUint64(body, request->clientId);
        appendUint64(body, request->clientSequence);
        appendPayload(body, request->payload);
    } else if ( const auto* committed =
                    std::get_if<CommittedOperation>(&message) ) {
        if ( !isOperationSizeValid(committed->payload.size(),
                                   maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::CommittedOperation;
        body.reserve(28U + committed->payload.size());
        appendUint64(body, committed->revision);
        appendUint64(body, committed->clientId);
        appendUint64(body, committed->clientSequence);
        appendPayload(body, committed->payload);
    } else if ( const auto* ack = std::get_if<RevisionAck>(&message) ) {
        kind = CollaborationMessageKind::RevisionAck;
        appendUint64(body, ack->revision);
    } else if ( const auto* resync = std::get_if<ResyncRequest>(&message) ) {
        kind = CollaborationMessageKind::ResyncRequest;
        appendUint64(body, resync->fromRevision);
    } else if ( const auto* identity =
                    std::get_if<ParticipantIdentity>(&message) ) {
        const auto creator =
            Config::normalizeCreatorIdentity(identity->creator);
        if ( identity->clientId == 0 || creator.empty() ) {
            return std::unexpected(ProtocolError::InvalidCreatorIdentity);
        }
        kind = CollaborationMessageKind::ParticipantIdentity;
        body.reserve(10U + creator.size());
        appendUint64(body, identity->clientId);
        appendShortString(body, creator);
    } else if ( const auto* participantLeft =
                    std::get_if<ParticipantLeft>(&message) ) {
        if ( participantLeft->clientId == 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        kind = CollaborationMessageKind::ParticipantLeft;
        appendUint64(body, participantLeft->clientId);
    } else if ( const auto* snapshot = std::get_if<StateSnapshot>(&message) ) {
        if ( snapshot->revision == 0 ||
             !isOperationSizeValid(snapshot->payload.size(),
                                   maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::StateSnapshot;
        body.reserve(12U + snapshot->payload.size());
        appendUint64(body, snapshot->revision);
        appendPayload(body, snapshot->payload);
    } else if ( const auto* manifest =
                    std::get_if<ResourceManifest>(&message) ) {
        if ( manifest->generation == 0 || manifest->payload.empty() ||
             !isOperationSizeValid(manifest->payload.size(),
                                   maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::ResourceManifest;
        body.reserve(12U + manifest->payload.size());
        appendUint64(body, manifest->generation);
        appendPayload(body, manifest->payload);
    } else if ( const auto* request = std::get_if<ResourceRequest>(&message) ) {
        if ( request->generation == 0 || request->requestedBytes == 0 ||
             request->requestedBytes > maxOperationBytes ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::ResourceRequest;
        body.reserve(24U);
        appendUint64(body, request->generation);
        appendUint32(body, request->resourceIndex);
        appendUint64(body, request->offset);
        appendUint32(body, request->requestedBytes);
    } else if ( const auto* chunk = std::get_if<ResourceChunk>(&message) ) {
        if ( chunk->generation == 0 || chunk->payload.empty() ||
             !isOperationSizeValid(chunk->payload.size(), maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::ResourceChunk;
        body.reserve(24U + chunk->payload.size());
        appendUint64(body, chunk->generation);
        appendUint32(body, chunk->resourceIndex);
        appendUint64(body, chunk->offset);
        appendPayload(body, chunk->payload);
    } else {
        return std::unexpected(ProtocolError::UnknownMessageKind);
    }

    if ( body.size() > std::numeric_limits<std::uint32_t>::max() ) {
        return std::unexpected(ProtocolError::InvalidMessageLength);
    }

    ByteBuffer output;
    output.reserve(PROTOCOL_HEADER_BYTES + body.size());
    appendUint32(output, PROTOCOL_MAGIC);
    appendUint16(output, COLLABORATION_PROTOCOL_VERSION);
    output.push_back(static_cast<std::uint8_t>(kind));
    output.push_back(0);
    appendUint32(output, static_cast<std::uint32_t>(body.size()));
    output.insert(output.end(), body.begin(), body.end());
    return output;
}

std::expected<CollaborationMessage, ProtocolError> decodeCollaborationMessage(
    std::span<const std::uint8_t> bytes, std::size_t maxOperationBytes)
{
    ByteReader    reader(bytes);
    std::uint32_t magic     = 0;
    std::uint16_t version   = 0;
    std::uint8_t  rawKind   = 0;
    std::uint8_t  reserved  = 0;
    std::uint32_t bodyBytes = 0;
    if ( !reader.readUint32(magic) || !reader.readUint16(version) ||
         !reader.readUint8(rawKind) || !reader.readUint8(reserved) ||
         !reader.readUint32(bodyBytes) ) {
        return std::unexpected(ProtocolError::TruncatedMessage);
    }
    if ( magic != PROTOCOL_MAGIC ) {
        return std::unexpected(ProtocolError::InvalidMagic);
    }
    if ( version != COLLABORATION_PROTOCOL_VERSION ) {
        return std::unexpected(ProtocolError::UnsupportedVersion);
    }
    if ( reserved != 0 ) {
        return std::unexpected(ProtocolError::InvalidReservedField);
    }
    if ( reader.remaining() != bodyBytes ) {
        return std::unexpected(ProtocolError::InvalidMessageLength);
    }

    const auto kind = static_cast<CollaborationMessageKind>(rawKind);
    switch ( kind ) {
    case CollaborationMessageKind::EditRequest: {
        EditRequest   request;
        std::uint32_t payloadBytes = 0;
        if ( !reader.readUint64(request.clientId) ||
             !reader.readUint64(request.clientSequence) ||
             !reader.readUint32(payloadBytes) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( !isOperationSizeValid(payloadBytes, maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        if ( !reader.readBytes(payloadBytes, request.payload) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        return CollaborationMessage(std::move(request));
    }
    case CollaborationMessageKind::CommittedOperation: {
        CommittedOperation committed;
        std::uint32_t      payloadBytes = 0;
        if ( !reader.readUint64(committed.revision) ||
             !reader.readUint64(committed.clientId) ||
             !reader.readUint64(committed.clientSequence) ||
             !reader.readUint32(payloadBytes) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( !isOperationSizeValid(payloadBytes, maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        if ( !reader.readBytes(payloadBytes, committed.payload) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        return CollaborationMessage(std::move(committed));
    }
    case CollaborationMessageKind::RevisionAck: {
        RevisionAck ack;
        if ( !reader.readUint64(ack.revision) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        return CollaborationMessage(ack);
    }
    case CollaborationMessageKind::ResyncRequest: {
        ResyncRequest resync;
        if ( !reader.readUint64(resync.fromRevision) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        return CollaborationMessage(resync);
    }
    case CollaborationMessageKind::ParticipantIdentity: {
        ParticipantIdentity identity;
        std::uint16_t       creatorBytes = 0;
        if ( !reader.readUint64(identity.clientId) ||
             !reader.readUint16(creatorBytes) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( identity.clientId == 0 || creatorBytes == 0 ||
             creatorBytes > Config::MAX_CREATOR_IDENTITY_BYTES ) {
            return std::unexpected(ProtocolError::InvalidCreatorIdentity);
        }
        if ( !reader.readString(creatorBytes, identity.creator) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        identity.creator = Config::normalizeCreatorIdentity(identity.creator);
        if ( identity.creator.empty() ) {
            return std::unexpected(ProtocolError::InvalidCreatorIdentity);
        }
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        return CollaborationMessage(std::move(identity));
    }
    case CollaborationMessageKind::ParticipantLeft: {
        ParticipantLeft participantLeft;
        if ( !reader.readUint64(participantLeft.clientId) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( participantLeft.clientId == 0 || reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        return CollaborationMessage(participantLeft);
    }
    case CollaborationMessageKind::StateSnapshot: {
        StateSnapshot snapshot;
        std::uint32_t payloadBytes = 0;
        if ( !reader.readUint64(snapshot.revision) ||
             !reader.readUint32(payloadBytes) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( snapshot.revision == 0 ||
             !isOperationSizeValid(payloadBytes, maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        if ( !reader.readBytes(payloadBytes, snapshot.payload) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        return CollaborationMessage(std::move(snapshot));
    }
    case CollaborationMessageKind::ResourceManifest: {
        ResourceManifest manifest;
        std::uint32_t    payloadBytes = 0;
        if ( !reader.readUint64(manifest.generation) ||
             !reader.readUint32(payloadBytes) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( manifest.generation == 0 || payloadBytes == 0 ||
             !isOperationSizeValid(payloadBytes, maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        if ( !reader.readBytes(payloadBytes, manifest.payload) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        return CollaborationMessage(std::move(manifest));
    }
    case CollaborationMessageKind::ResourceRequest: {
        ResourceRequest request;
        if ( !reader.readUint64(request.generation) ||
             !reader.readUint32(request.resourceIndex) ||
             !reader.readUint64(request.offset) ||
             !reader.readUint32(request.requestedBytes) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( request.generation == 0 || request.requestedBytes == 0 ||
             request.requestedBytes > maxOperationBytes ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        return CollaborationMessage(request);
    }
    case CollaborationMessageKind::ResourceChunk: {
        ResourceChunk chunk;
        std::uint32_t payloadBytes = 0;
        if ( !reader.readUint64(chunk.generation) ||
             !reader.readUint32(chunk.resourceIndex) ||
             !reader.readUint64(chunk.offset) ||
             !reader.readUint32(payloadBytes) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( chunk.generation == 0 || payloadBytes == 0 ||
             !isOperationSizeValid(payloadBytes, maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        if ( !reader.readBytes(payloadBytes, chunk.payload) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        return CollaborationMessage(std::move(chunk));
    }
    }
    return std::unexpected(ProtocolError::UnknownMessageKind);
}
}  // namespace MMM::Network::Collaboration
