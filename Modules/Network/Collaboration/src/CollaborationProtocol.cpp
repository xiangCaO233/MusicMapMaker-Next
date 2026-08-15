#include "network/collaboration/CollaborationProtocol.h"
#include "config/CreatorIdentity.h"

#include <algorithm>
#include <bit>
#include <cmath>
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

/// @brief 以 IEEE 754 位模式追加 64 位浮点数。
/// @param output 目标字节序列。
/// @param value 待写入数值。
void appendDouble(ByteBuffer& output, double value)
{
    appendUint64(output, std::bit_cast<std::uint64_t>(value));
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

/// @brief 追加一个已经规范化的固定长度协作稳定标识。
/// @param output 目标字节序列。
/// @param identity 32 字符小写十六进制标识。
void appendStableIdentity(ByteBuffer& output, std::string_view identity)
{
    output.insert(output.end(), identity.begin(), identity.end());
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

    /// @brief 读取 IEEE 754 位模式编码的 64 位浮点数。
    /// @param value 输出数值。
    /// @return 剩余字节足够时返回 true。
    [[nodiscard]] bool readDouble(double& value)
    {
        std::uint64_t bits = 0;
        if ( !readUint64(bits) ) return false;
        value = std::bit_cast<double>(bits);
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

/// @brief 读取并规范化一个固定长度协作稳定标识。
/// @param reader 当前帧读取器。
/// @param identity 输出小写十六进制标识。
/// @return 字节完整且满足稳定标识格式时返回 true。
[[nodiscard]] bool readStableIdentity(ByteReader& reader, std::string& identity)
{
    if ( !reader.readString(Config::COLLABORATION_STABLE_ID_CHARACTERS,
                            identity) ) {
        return false;
    }
    identity = Config::normalizeCollaborationStableId(identity);
    return !identity.empty();
}

/// @brief 校验远端主画布视口状态是否适合进入房间状态表。
/// @param viewport 待校验状态。
/// @return 标识、序号与全部数值有限且范围有界时返回 true。
[[nodiscard]] bool isViewportStateValid(const ParticipantViewport& viewport)
{
    constexpr double MAX_TIME_MAGNITUDE = 7.0 * 24.0 * 60.0 * 60.0;
    constexpr double MAX_VISIBLE_SPAN   = 24.0 * 60.0 * 60.0;
    constexpr double MAX_OFFSET_RATIO   = 100.0;
    const double     visibleMinimum =
        std::min(viewport.visibleTimeStart, viewport.visibleTimeEnd);
    const double visibleMaximum =
        std::max(viewport.visibleTimeStart, viewport.visibleTimeEnd);
    return viewport.peerId != 0 && viewport.sequence != 0 &&
           std::isfinite(viewport.playbackTime) &&
           std::isfinite(viewport.visualTime) &&
           std::isfinite(viewport.visibleTimeStart) &&
           std::isfinite(viewport.visibleTimeEnd) &&
           std::isfinite(viewport.horizontalOffsetRatio) &&
           std::abs(viewport.playbackTime) <= MAX_TIME_MAGNITUDE &&
           std::abs(viewport.visualTime) <= MAX_TIME_MAGNITUDE &&
           std::abs(visibleMinimum) <= MAX_TIME_MAGNITUDE &&
           std::abs(visibleMaximum) <= MAX_TIME_MAGNITUDE &&
           visibleMaximum - visibleMinimum <= MAX_VISIBLE_SPAN &&
           std::abs(viewport.horizontalOffsetRatio) <= MAX_OFFSET_RATIO;
}

/// @brief 校验单行聊天文本的 UTF-8、控制字符和长度边界。
/// @param text 待进入协议的聊天正文。
/// @return 文本非空、含可见内容且每个 UTF-8 序列合法时返回 true。
[[nodiscard]] bool isChatTextValid(std::string_view text)
{
    if ( text.empty() || text.size() > MAX_COLLABORATION_CHAT_MESSAGE_BYTES ) {
        return false;
    }

    bool        hasVisibleContent = false;
    std::size_t offset            = 0;
    auto        isContinuation    = [](std::uint8_t value) {
        return value >= 0x80U && value <= 0xBFU;
    };
    while ( offset < text.size() ) {
        const auto first = static_cast<std::uint8_t>(text[offset]);
        if ( first <= 0x7FU ) {
            if ( first < 0x20U || first == 0x7FU ) return false;
            hasVisibleContent = hasVisibleContent || first != 0x20U;
            ++offset;
            continue;
        }

        std::size_t sequenceBytes = 0;
        if ( first >= 0xC2U && first <= 0xDFU ) {
            sequenceBytes = 2;
        } else if ( first >= 0xE0U && first <= 0xEFU ) {
            sequenceBytes = 3;
        } else if ( first >= 0xF0U && first <= 0xF4U ) {
            sequenceBytes = 4;
        } else {
            return false;
        }
        if ( text.size() - offset < sequenceBytes ) return false;

        const auto second = static_cast<std::uint8_t>(text[offset + 1U]);
        if ( !isContinuation(second) || (first == 0xE0U && second < 0xA0U) ||
             (first == 0xEDU && second > 0x9FU) ||
             (first == 0xF0U && second < 0x90U) ||
             (first == 0xF4U && second > 0x8FU) ) {
            return false;
        }
        for ( std::size_t index = 2U; index < sequenceBytes; ++index ) {
            if ( !isContinuation(
                     static_cast<std::uint8_t>(text[offset + index])) ) {
                return false;
            }
        }
        hasVisibleContent = true;
        offset += sequenceBytes;
    }
    return hasVisibleContent;
}
}  // namespace

std::expected<ByteBuffer, ProtocolError> encodeCollaborationMessage(
    const CollaborationMessage& message, std::size_t maxOperationBytes)
{
    ByteBuffer               body;
    CollaborationMessageKind kind{};

    if ( const auto* request = std::get_if<EditRequest>(&message) ) {
        const auto participantId =
            Config::normalizeCollaborationStableId(request->participantId);
        const auto sessionId =
            Config::normalizeCollaborationStableId(request->sessionId);
        if ( participantId.empty() || sessionId.empty() ||
             request->clientSequence == 0 ) {
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        if ( !isOperationSizeValid(request->payload.size(),
                                   maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::EditRequest;
        body.reserve(76U + request->payload.size());
        appendStableIdentity(body, participantId);
        appendStableIdentity(body, sessionId);
        appendUint64(body, request->clientSequence);
        appendPayload(body, request->payload);
    } else if ( const auto* committed =
                    std::get_if<CommittedOperation>(&message) ) {
        const auto participantId =
            Config::normalizeCollaborationStableId(committed->participantId);
        const auto sessionId =
            Config::normalizeCollaborationStableId(committed->sessionId);
        if ( participantId.empty() || sessionId.empty() ||
             committed->revision == 0 || committed->clientSequence == 0 ) {
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        if ( !isOperationSizeValid(committed->payload.size(),
                                   maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::CommittedOperation;
        body.reserve(84U + committed->payload.size());
        appendUint64(body, committed->revision);
        appendStableIdentity(body, participantId);
        appendStableIdentity(body, sessionId);
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
        const auto participantId =
            Config::normalizeCollaborationStableId(identity->participantId);
        const auto sessionId =
            Config::normalizeCollaborationStableId(identity->sessionId);
        if ( identity->peerId == 0 || participantId.empty() ||
             sessionId.empty() ) {
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        if ( creator.empty() ) {
            return std::unexpected(ProtocolError::InvalidCreatorIdentity);
        }
        kind = CollaborationMessageKind::ParticipantIdentity;
        body.reserve(74U + creator.size());
        appendUint64(body, identity->peerId);
        appendStableIdentity(body, participantId);
        appendStableIdentity(body, sessionId);
        appendShortString(body, creator);
    } else if ( const auto* participantLeft =
                    std::get_if<ParticipantLeft>(&message) ) {
        if ( participantLeft->peerId == 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        kind = CollaborationMessageKind::ParticipantLeft;
        appendUint64(body, participantLeft->peerId);
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
    } else if ( const auto* viewport =
                    std::get_if<ParticipantViewport>(&message) ) {
        if ( !isViewportStateValid(*viewport) ) {
            return std::unexpected(ProtocolError::InvalidViewportState);
        }
        kind = CollaborationMessageKind::ParticipantViewport;
        body.reserve(56U);
        appendUint64(body, viewport->peerId);
        appendUint64(body, viewport->sequence);
        appendDouble(body, viewport->playbackTime);
        appendDouble(body, viewport->visualTime);
        appendDouble(body, viewport->visibleTimeStart);
        appendDouble(body, viewport->visibleTimeEnd);
        appendDouble(body, viewport->horizontalOffsetRatio);
    } else if ( const auto* chat =
                    std::get_if<CollaborationChatMessage>(&message) ) {
        if ( chat->peerId == 0 || chat->sequence == 0 ||
             !isChatTextValid(chat->text) ) {
            return std::unexpected(ProtocolError::InvalidChatMessage);
        }
        kind = CollaborationMessageKind::ChatMessage;
        body.reserve(18U + chat->text.size());
        appendUint64(body, chat->peerId);
        appendUint64(body, chat->sequence);
        appendShortString(body, chat->text);
    } else if ( const auto* permissions =
                    std::get_if<ParticipantPermissions>(&message) ) {
        if ( permissions->peerId == 0 ||
             !isCollaborationPermissionMaskValid(permissions->permissions) ) {
            return std::unexpected(ProtocolError::InvalidPermissions);
        }
        kind = CollaborationMessageKind::ParticipantPermissions;
        body.reserve(12U);
        appendUint64(body, permissions->peerId);
        appendUint32(body, permissions->permissions);
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
        if ( !readStableIdentity(reader, request.participantId) ||
             !readStableIdentity(reader, request.sessionId) ||
             !reader.readUint64(request.clientSequence) ||
             !reader.readUint32(payloadBytes) ) {
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        if ( request.clientSequence == 0 ) {
            return std::unexpected(ProtocolError::InvalidStableIdentity);
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
             !readStableIdentity(reader, committed.participantId) ||
             !readStableIdentity(reader, committed.sessionId) ||
             !reader.readUint64(committed.clientSequence) ||
             !reader.readUint32(payloadBytes) ) {
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        if ( committed.revision == 0 || committed.clientSequence == 0 ) {
            return std::unexpected(ProtocolError::InvalidStableIdentity);
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
        if ( !reader.readUint64(identity.peerId) ||
             !readStableIdentity(reader, identity.participantId) ||
             !readStableIdentity(reader, identity.sessionId) ||
             !reader.readUint16(creatorBytes) ) {
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        if ( identity.peerId == 0 || creatorBytes == 0 ||
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
        if ( !reader.readUint64(participantLeft.peerId) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( participantLeft.peerId == 0 || reader.remaining() != 0 ) {
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
    case CollaborationMessageKind::ParticipantViewport: {
        ParticipantViewport viewport;
        if ( !reader.readUint64(viewport.peerId) ||
             !reader.readUint64(viewport.sequence) ||
             !reader.readDouble(viewport.playbackTime) ||
             !reader.readDouble(viewport.visualTime) ||
             !reader.readDouble(viewport.visibleTimeStart) ||
             !reader.readDouble(viewport.visibleTimeEnd) ||
             !reader.readDouble(viewport.horizontalOffsetRatio) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( reader.remaining() != 0 || !isViewportStateValid(viewport) ) {
            return std::unexpected(ProtocolError::InvalidViewportState);
        }
        return CollaborationMessage(viewport);
    }
    case CollaborationMessageKind::ChatMessage: {
        CollaborationChatMessage chat;
        std::uint16_t            textBytes = 0;
        if ( !reader.readUint64(chat.peerId) ||
             !reader.readUint64(chat.sequence) ||
             !reader.readUint16(textBytes) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( textBytes == 0 ||
             textBytes > MAX_COLLABORATION_CHAT_MESSAGE_BYTES ||
             !reader.readString(textBytes, chat.text) ) {
            return std::unexpected(ProtocolError::InvalidChatMessage);
        }
        if ( reader.remaining() != 0 || chat.peerId == 0 ||
             chat.sequence == 0 || !isChatTextValid(chat.text) ) {
            return std::unexpected(ProtocolError::InvalidChatMessage);
        }
        return CollaborationMessage(std::move(chat));
    }
    case CollaborationMessageKind::ParticipantPermissions: {
        ParticipantPermissions permissions;
        if ( !reader.readUint64(permissions.peerId) ||
             !reader.readUint32(permissions.permissions) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        if ( reader.remaining() != 0 || permissions.peerId == 0 ||
             !isCollaborationPermissionMaskValid(permissions.permissions) ) {
            return std::unexpected(ProtocolError::InvalidPermissions);
        }
        return CollaborationMessage(permissions);
    }
    }
    return std::unexpected(ProtocolError::UnknownMessageKind);
}
}  // namespace MMM::Network::Collaboration
