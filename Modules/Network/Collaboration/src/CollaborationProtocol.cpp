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
/// @note 解码器在读取消息类型前校验，避免把任意二进制误判为协议帧。
constexpr std::uint32_t PROTOCOL_MAGIC = 0x434D4D4DU;
/// @brief 帧头固定字节数。
/// @note 布局为魔数 4、版本 2、类型 1、保留 1、正文长度 4 字节。
/// @note 正文长度不包含本身及其他头字段。
constexpr std::size_t PROTOCOL_HEADER_BYTES = 12;

/// @brief 以小端序追加 16 位无符号整数。
/// @param output 目标字节序列。
/// @param value 待写入数值。
/// @note 仅在已完成容量或长度上限校验的编码路径调用。
void appendUint16(ByteBuffer& output, std::uint16_t value)
{
    // 低字节先写入，显式掩码避免整数提升影响窄化结果。
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

/// @brief 以小端序追加 32 位无符号整数。
/// @param output 目标字节序列。
/// @param value 待写入数值。
/// @note 用于魔数、正文长度、资源索引和权限掩码。
void appendUint32(ByteBuffer& output, std::uint32_t value)
{
    // 每轮追加下一个低有效字节，产生与主机端序无关的线格式。
    for ( std::uint32_t shift = 0; shift < 32U; shift += 8U ) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/// @brief 以小端序追加 64 位无符号整数。
/// @param output 目标字节序列。
/// @param value 待写入数值。
/// @note 用于修订、序号、PeerId、代次和资源偏移。
void appendUint64(ByteBuffer& output, std::uint64_t value)
{
    // 完整八轮保留序号、修订号和偏移量的全部 64 位。
    for ( std::uint32_t shift = 0; shift < 64U; shift += 8U ) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/// @brief 以 IEEE 754 位模式追加 64 位浮点数。
/// @param output 目标字节序列。
/// @param value 待写入数值。
/// @note 数值有限性和范围必须由消息级校验器先行证明。
void appendDouble(ByteBuffer& output, double value)
{
    // bit_cast 保留 IEEE 754 位模式，不执行数值或文本转换。
    appendUint64(output, std::bit_cast<std::uint64_t>(value));
}

/// @brief 追加带 32 位长度的操作负载。
/// @param output 目标字节序列。
/// @param payload 操作负载。
/// @note 输出布局固定为长度字段后紧跟等长原始字节。
void appendPayload(ByteBuffer& output, const ByteBuffer& payload)
{
    // 调用方必须先证明大小可表示为 uint32_t，避免此处截断。
    appendUint32(output, static_cast<std::uint32_t>(payload.size()));
    // 长度之后紧跟原始字节，解码器按相同顺序读取。
    output.insert(output.end(), payload.begin(), payload.end());
}

/// @brief 追加带 16 位长度的短 UTF-8 字符串。
/// @param output 目标字节序列。
/// @param value 待写入字符串。
/// @note 不负责 UTF-8 校验，调用分支必须先完成内容规范化。
void appendShortString(ByteBuffer& output, std::string_view value)
{
    // Creator 和聊天上限均不超过 uint16_t，校验由具体消息分支完成。
    appendUint16(output, static_cast<std::uint16_t>(value.size()));
    // 字符串按 UTF-8 原始字节传输，不附加零终止符。
    output.insert(output.end(), value.begin(), value.end());
}

/// @brief 追加一个已经规范化的固定长度协作稳定标识。
/// @param output 目标字节序列。
/// @param identity 32 字符小写十六进制标识。
/// @note 不写长度字段，协议版本固定其字符数和编码。
void appendStableIdentity(ByteBuffer& output, std::string_view identity)
{
    // 稳定标识已经规范化为固定 32 字符，因此无需额外长度字段。
    output.insert(output.end(), identity.begin(), identity.end());
}

/// @brief 对二进制帧执行有界小端读取。
/// @note 所有失败读取保持 offset 不变，便于调用方准确分类错误。
class ByteReader
{
public:
    /// @brief 创建读取器。
    /// @param bytes 待读取字节视图。
    explicit ByteReader(std::span<const std::uint8_t> bytes) : m_bytes(bytes)
    {
        // reader 不拥有输入，调用方保证整个解码期间 span 保持有效。
    }

    /// @brief 读取 8 位无符号整数。
    /// @param value 输出数值。
    /// @return 剩余字节足够时返回 true。
    /// @warning 协议热路径使用，只执行边界检查和一次字节读取。
    [[nodiscard]] bool readUint8(std::uint8_t& value)
    {
        // 边界检查先于索引与偏移推进，失败不会改变读取位置。
        if ( remaining() < 1 ) {
            return false;
        }
        // 单字节直接复制并只在成功路径推进一位。
        value = m_bytes[m_offset++];
        return true;
    }

    /// @brief 读取小端 16 位无符号整数。
    /// @param value 输出数值。
    /// @return 剩余字节足够时返回 true。
    /// @warning 协议热路径使用，不分配、不阻塞。
    [[nodiscard]] bool readUint16(std::uint16_t& value)
    {
        // 固定宽度读取不允许部分消费截断输入。
        if ( remaining() < 2 ) {
            return false;
        }
        // 每个字节先提升后移位，避免窄类型溢出。
        value = static_cast<std::uint16_t>(m_bytes[m_offset]) |
                static_cast<std::uint16_t>(m_bytes[m_offset + 1]) << 8U;
        m_offset += 2;
        return true;
    }

    /// @brief 读取小端 32 位无符号整数。
    /// @param value 输出数值。
    /// @return 剩余字节足够时返回 true。
    /// @warning 协议热路径使用，循环次数固定为四。
    [[nodiscard]] bool readUint32(std::uint32_t& value)
    {
        // 四字节必须完整存在，失败时输出和 offset 都保持原值。
        if ( remaining() < 4 ) {
            return false;
        }
        // 从零累积各小端分量，与 appendUint32 的写入顺序互逆。
        value = 0;
        for ( std::uint32_t shift = 0; shift < 32U; shift += 8U ) {
            value |= static_cast<std::uint32_t>(m_bytes[m_offset++]) << shift;
        }
        return true;
    }

    /// @brief 读取小端 64 位无符号整数。
    /// @param value 输出数值。
    /// @return 剩余字节足够时返回 true。
    /// @warning 协议热路径使用，循环次数固定为八。
    [[nodiscard]] bool readUint64(std::uint64_t& value)
    {
        // 八字节预检保证循环内部每次索引均在 span 范围内。
        if ( remaining() < 8 ) {
            return false;
        }
        // uint64_t 提升发生在移位之前，保留最高有效字节。
        value = 0;
        for ( std::uint32_t shift = 0; shift < 64U; shift += 8U ) {
            value |= static_cast<std::uint64_t>(m_bytes[m_offset++]) << shift;
        }
        return true;
    }

    /// @brief 读取 IEEE 754 位模式编码的 64 位浮点数。
    /// @param value 输出数值。
    /// @return 剩余字节足够时返回 true。
    /// @note 只恢复位模式，不接受或拒绝 NaN/Inf。
    [[nodiscard]] bool readDouble(double& value)
    {
        // 先读取完整整数位模式，截断时不改写目标浮点值。
        std::uint64_t bits = 0;
        if ( !readUint64(bits) ) return false;
        // bit_cast 恢复原始浮点表示，有限性由视口校验器单独检查。
        value = std::bit_cast<double>(bits);
        return true;
    }

    /// @brief 读取指定长度的字节序列。
    /// @param length 读取字节数。
    /// @param output 输出缓冲区。
    /// @return 剩余字节足够时返回 true。
    /// @warning 会按已验证远端长度分配输出，调用前必须应用产品上限。
    [[nodiscard]] bool readBytes(std::size_t length, ByteBuffer& output)
    {
        // 从 remaining 做减法式边界判断，避免 offset + length 溢出。
        if ( remaining() < length ) {
            return false;
        }
        // 只在长度验证后构造迭代器，确保 ptrdiff_t 偏移有效。
        const auto begin =
            m_bytes.begin() + static_cast<std::ptrdiff_t>(m_offset);
        output.assign(begin, begin + static_cast<std::ptrdiff_t>(length));
        // 输出复制成功后才推进读取器位置。
        m_offset += length;
        return true;
    }

    /// @brief 读取指定长度的 UTF-8 字符串字节。
    /// @param length 读取字节数。
    /// @param output 输出字符串。
    /// @return 剩余字节足够时返回 true。
    /// @warning 会复制字符串，调用前必须限制 length。
    [[nodiscard]] bool readString(std::size_t length, std::string& output)
    {
        // 协议长度不足时保留原输出，交由消息分支选择错误类型。
        if ( remaining() < length ) {
            return false;
        }
        // 将当前字节地址解释为 char 仅用于原样复制 UTF-8 数据。
        const auto* begin = reinterpret_cast<const char*>(m_bytes.data()) +
                            static_cast<std::ptrdiff_t>(m_offset);
        output.assign(begin, length);
        // 字符串不依赖输入 span 生命周期，随后推进精确字节数。
        m_offset += length;
        return true;
    }

    /// @brief 返回尚未读取的字节数。
    /// @return 剩余字节数。
    [[nodiscard]] std::size_t remaining() const
    {
        // 所有成功读取都维持 m_offset <= m_bytes.size() 不变量。
        return m_bytes.size() - m_offset;
    }

private:
    /// @brief 完整只读字节视图。
    /// @note 生命周期由 decodeCollaborationMessage 的输入参数覆盖。
    std::span<const std::uint8_t> m_bytes;
    /// @brief 当前读取偏移。
    /// @note 只在一次完整读取成功后单调递增。
    std::size_t m_offset = 0;
};

/// @brief 校验操作负载能否进入线协议的 32 位长度字段。
/// @param payloadBytes 实际负载字节数。
/// @param maxOperationBytes 配置的负载上限。
/// @return 负载合法时返回 true。
/// @note maxOperationBytes 可比协议硬上限更严格，但不能放宽硬上限。
/// @warning 接收路径在任何按远端长度分配前调用。
[[nodiscard]] bool isOperationSizeValid(std::size_t payloadBytes,
                                        std::size_t maxOperationBytes)
{
    // 同时应用运行时策略上限和线协议 32 位长度字段硬上限。
    return payloadBytes <= maxOperationBytes &&
           payloadBytes <= std::numeric_limits<std::uint32_t>::max();
}

/// @brief 读取并规范化一个固定长度协作稳定标识。
/// @param reader 当前帧读取器。
/// @param identity 输出小写十六进制标识。
/// @return 字节完整且满足稳定标识格式时返回 true。
/// @note 格式错误和固定字节截断均以 false 返回给具体消息分支。
/// @note 成功输出始终为协议统一的小写十六进制表示。
[[nodiscard]] bool readStableIdentity(ByteReader& reader, std::string& identity)
{
    // 固定读取 32 字节，不信任帧提供可变身份长度。
    if ( !reader.readString(Config::COLLABORATION_STABLE_ID_CHARACTERS,
                            identity) ) {
        return false;
    }
    // 规范化同时验证十六进制格式，并统一为小写表示。
    identity = Config::normalizeCollaborationStableId(identity);
    return !identity.empty();
}

/// @brief 校验远端主画布视口状态是否适合进入房间状态表。
/// @param viewport 待校验状态。
/// @return 标识、序号与全部数值有限且范围有界时返回 true。
/// @note 校验不修改视口起止顺序，只用最小值和最大值计算跨度。
/// @warning 高频接收路径调用，只执行固定数量数值比较。
[[nodiscard]] bool isViewportStateValid(const ParticipantViewport& viewport)
{
    // 时间范围允许前后七天，足以覆盖编辑视野同时限制异常数值。
    constexpr double MAX_TIME_MAGNITUDE = 7.0 * 24.0 * 60.0 * 60.0;
    // 单个可见窗口最多一天，阻止极端跨度污染远端视图状态。
    constexpr double MAX_VISIBLE_SPAN = 24.0 * 60.0 * 60.0;
    // 横向偏移允许宽范围但仍拒绝无界输入。
    constexpr double MAX_OFFSET_RATIO = 100.0;
    // 起止可反向传入，校验统一使用排序后的范围。
    const double visibleMinimum =
        std::min(viewport.visibleTimeStart, viewport.visibleTimeEnd);
    const double visibleMaximum =
        std::max(viewport.visibleTimeStart, viewport.visibleTimeEnd);
    // 身份与序号零值保留为无效，所有 double 必须有限且有界。
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
/// @note 不执行 Unicode 归一化，线协议保留用户输入的合法字节序列。
/// @warning 扫描复杂度与受上限约束的文本字节数线性相关。
[[nodiscard]] bool isChatTextValid(std::string_view text)
{
    // 空消息和超过 uint16_t/产品限制的消息在扫描前直接拒绝。
    if ( text.empty() || text.size() > MAX_COLLABORATION_CHAT_MESSAGE_BYTES ) {
        return false;
    }

    // 纯 ASCII 空格不算可见内容，合法非 ASCII 序列则视为可见。
    bool        hasVisibleContent = false;
    std::size_t offset            = 0;
    auto        isContinuation    = [](std::uint8_t value) {
        // UTF-8 延续字节固定落在 10xxxxxx 范围。
        return value >= 0x80U && value <= 0xBFU;
    };
    // 按 Unicode 标量序列逐段前进，任何非法首字节立即拒绝整条消息。
    while ( offset < text.size() ) {
        const auto first = static_cast<std::uint8_t>(text[offset]);
        if ( first <= 0x7FU ) {
            // C0 控制字符和 DEL 禁止进入单行聊天协议。
            if ( first < 0x20U || first == 0x7FU ) return false;
            // ASCII 空格允许用于排版，但不能单独构成消息内容。
            hasVisibleContent = hasVisibleContent || first != 0x20U;
            ++offset;
            continue;
        }

        // 首字节范围同时排除过长编码的 C0/C1 和超出 Unicode 的 F5-FF。
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
        // 剩余字节不足完整序列时拒绝截断文本。
        if ( text.size() - offset < sequenceBytes ) return false;

        // 第二字节承担过长编码、代理项和 Unicode 上界的附加限制。
        const auto second = static_cast<std::uint8_t>(text[offset + 1U]);
        if ( !isContinuation(second) || (first == 0xE0U && second < 0xA0U) ||
             (first == 0xEDU && second > 0x9FU) ||
             (first == 0xF0U && second < 0x90U) ||
             (first == 0xF4U && second > 0x8FU) ) {
            return false;
        }
        // 第三、第四字节只需满足标准延续字节范围。
        for ( std::size_t index = 2U; index < sequenceBytes; ++index ) {
            if ( !isContinuation(
                     static_cast<std::uint8_t>(text[offset + index])) ) {
                return false;
            }
        }
        // 任意合法非 ASCII 标量均视为可见内容。
        hasVisibleContent = true;
        offset += sequenceBytes;
    }
    // 全空格 ASCII 文本即使编码合法也不允许发送。
    return hasVisibleContent;
}
}  // namespace

/// @brief 将类型安全协作消息编码为固定头和小端正文。
/// @param message 待编码的消息变体。
/// @param maxOperationBytes 操作及资源负载的运行时上限。
/// @return 成功时返回完整帧，字段非法时返回精确协议错误。
/// @warning 协作发送热路径；允许构造单帧缓冲，禁止文件 I/O、等待和异常。
std::expected<ByteBuffer, ProtocolError> encodeCollaborationMessage(
    const CollaborationMessage& message, std::size_t maxOperationBytes)
{
    // 先独立构造正文并确定消息类型，验证失败不会产生部分帧头。
    ByteBuffer               body;
    CollaborationMessageKind kind{};

    if ( const auto* request = std::get_if<EditRequest>(&message) ) {
        // 编辑请求在线协议入口再次规范化持久参与者与本次会话身份。
        const auto participantId =
            Config::normalizeCollaborationStableId(request->participantId);
        const auto sessionId =
            Config::normalizeCollaborationStableId(request->sessionId);
        if ( participantId.empty() || sessionId.empty() ||
             request->clientSequence == 0 ) {
            // 身份或序号无效时不允许产生可进入房主排序器的请求。
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        // 可变操作负载必须同时满足配置上限和 uint32_t 表示范围。
        if ( !isOperationSizeValid(request->payload.size(),
                                   maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::EditRequest;
        // 固定字段 32+32+8+4 字节，再加操作负载。
        body.reserve(76U + request->payload.size());
        appendStableIdentity(body, participantId);
        appendStableIdentity(body, sessionId);
        appendUint64(body, request->clientSequence);
        // payload 作为不透明文档操作字节，协议层不解析内部格式。
        appendPayload(body, request->payload);
    } else if ( const auto* committed =
                    std::get_if<CommittedOperation>(&message) ) {
        // 已提交操作额外携带房主分配的全局 revision。
        const auto participantId =
            Config::normalizeCollaborationStableId(committed->participantId);
        const auto sessionId =
            Config::normalizeCollaborationStableId(committed->sessionId);
        if ( participantId.empty() || sessionId.empty() ||
             committed->revision == 0 || committed->clientSequence == 0 ) {
            // 修订和客户端序号均从一开始，零值不能参与去重或排序。
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        if ( !isOperationSizeValid(committed->payload.size(),
                                   maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::CommittedOperation;
        // 固定字段比 EditRequest 多一个八字节 revision。
        body.reserve(84U + committed->payload.size());
        appendUint64(body, committed->revision);
        // 身份字段紧跟 revision，便于接收端建立操作来源和幂等键。
        appendStableIdentity(body, participantId);
        appendStableIdentity(body, sessionId);
        appendUint64(body, committed->clientSequence);
        appendPayload(body, committed->payload);
    } else if ( const auto* ack = std::get_if<RevisionAck>(&message) ) {
        // Ack 正文只含接收方已连续应用到的修订号。
        kind = CollaborationMessageKind::RevisionAck;
        appendUint64(body, ack->revision);
        // revision 的零值处理由房间状态机决定，协议保持固定宽度透传。
    } else if ( const auto* resync = std::get_if<ResyncRequest>(&message) ) {
        // 重同步请求指定希望开始补发的第一条修订号。
        kind = CollaborationMessageKind::ResyncRequest;
        appendUint64(body, resync->fromRevision);
        // fromRevision 可表达从初始状态开始重建，不在协议层强制非零。
    } else if ( const auto* identity =
                    std::get_if<ParticipantIdentity>(&message) ) {
        // Creator 使用展示身份规范，参与者与会话使用固定稳定标识规范。
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
            // 展示身份无效与稳定身份无效使用不同错误，便于调用方提示。
            return std::unexpected(ProtocolError::InvalidCreatorIdentity);
        }
        kind = CollaborationMessageKind::ParticipantIdentity;
        // Creator 有独立产品字节上限，可安全写入 16 位短字符串字段。
        body.reserve(74U + creator.size());
        appendUint64(body, identity->peerId);
        // 两个固定身份后再写变长 Creator，解码无需扫描分隔符。
        appendStableIdentity(body, participantId);
        appendStableIdentity(body, sessionId);
        appendShortString(body, creator);
    } else if ( const auto* participantLeft =
                    std::get_if<ParticipantLeft>(&message) ) {
        // peerId 零值没有可移除对象，因此按固定消息长度语义拒绝。
        if ( participantLeft->peerId == 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        kind = CollaborationMessageKind::ParticipantLeft;
        // 离开消息不携带原因，断开与主动退出由上层事件区分。
        appendUint64(body, participantLeft->peerId);
    } else if ( const auto* snapshot = std::get_if<StateSnapshot>(&message) ) {
        // 快照必须关联非零修订，并复用操作负载大小上限。
        if ( snapshot->revision == 0 ||
             !isOperationSizeValid(snapshot->payload.size(),
                                   maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::StateSnapshot;
        // 正文为八字节修订号、四字节长度和快照字节。
        body.reserve(12U + snapshot->payload.size());
        appendUint64(body, snapshot->revision);
        // 快照载荷内容由 BeatmapDocumentCodec 负责验证。
        appendPayload(body, snapshot->payload);
    } else if ( const auto* manifest =
                    std::get_if<ResourceManifest>(&message) ) {
        // 资源代次和清单正文都必须非空，防止覆盖为无效资源状态。
        if ( manifest->generation == 0 || manifest->payload.empty() ||
             !isOperationSizeValid(manifest->payload.size(),
                                   maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::ResourceManifest;
        body.reserve(12U + manifest->payload.size());
        appendUint64(body, manifest->generation);
        // 清单载荷保持不透明，资源同步层负责 JSON 及路径安全校验。
        appendPayload(body, manifest->payload);
    } else if ( const auto* request = std::get_if<ResourceRequest>(&message) ) {
        // 请求长度非零且不能超过单消息操作上限，偏移由资源层继续校验。
        if ( request->generation == 0 || request->requestedBytes == 0 ||
             request->requestedBytes > maxOperationBytes ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::ResourceRequest;
        // 固定 24 字节依次承载代次、资源索引、偏移与请求长度。
        body.reserve(24U);
        appendUint64(body, request->generation);
        appendUint32(body, request->resourceIndex);
        // offset 允许指向大文件任意协议块边界，资源层验证对齐。
        appendUint64(body, request->offset);
        appendUint32(body, request->requestedBytes);
    } else if ( const auto* chunk = std::get_if<ResourceChunk>(&message) ) {
        // 分块载荷必须非空，代次用于丢弃过期资源传输数据。
        if ( chunk->generation == 0 || chunk->payload.empty() ||
             !isOperationSizeValid(chunk->payload.size(), maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        kind = CollaborationMessageKind::ResourceChunk;
        // 固定定位字段后追加带 32 位长度的实际资源字节。
        body.reserve(24U + chunk->payload.size());
        appendUint64(body, chunk->generation);
        appendUint32(body, chunk->resourceIndex);
        // offset 与 payload 长度共同描述本次返回区间。
        appendUint64(body, chunk->offset);
        appendPayload(body, chunk->payload);
    } else if ( const auto* viewport =
                    std::get_if<ParticipantViewport>(&message) ) {
        // 高频视口消息在分配正文前验证身份、序号和所有浮点边界。
        if ( !isViewportStateValid(*viewport) ) {
            return std::unexpected(ProtocolError::InvalidViewportState);
        }
        kind = CollaborationMessageKind::ParticipantViewport;
        // 两个 uint64_t 加五个 double，正文固定 56 字节。
        body.reserve(56U);
        appendUint64(body, viewport->peerId);
        appendUint64(body, viewport->sequence);
        appendDouble(body, viewport->playbackTime);
        appendDouble(body, viewport->visualTime);
        appendDouble(body, viewport->visibleTimeStart);
        appendDouble(body, viewport->visibleTimeEnd);
        // 水平偏移作为最后一个 double，固定布局便于高频解码。
        appendDouble(body, viewport->horizontalOffsetRatio);
    } else if ( const auto* chat =
                    std::get_if<CollaborationChatMessage>(&message) ) {
        // 聊天要求非零发送者、单调序号和完整 UTF-8 可见文本。
        if ( chat->peerId == 0 || chat->sequence == 0 ||
             !isChatTextValid(chat->text) ) {
            return std::unexpected(ProtocolError::InvalidChatMessage);
        }
        kind = CollaborationMessageKind::ChatMessage;
        // 固定身份和序号 16 字节，另含两字节文本长度。
        body.reserve(18U + chat->text.size());
        appendUint64(body, chat->peerId);
        appendUint64(body, chat->sequence);
        // 已验证文本长度受 uint16_t 上限约束，可使用短字符串布局。
        appendShortString(body, chat->text);
    } else if ( const auto* permissions =
                    std::get_if<ParticipantPermissions>(&message) ) {
        // 权限位掩码必须只包含当前协议定义的位。
        if ( permissions->peerId == 0 ||
             !isCollaborationPermissionMaskValid(permissions->permissions) ) {
            return std::unexpected(ProtocolError::InvalidPermissions);
        }
        kind = CollaborationMessageKind::ParticipantPermissions;
        // 正文固定为八字节 peerId 与四字节权限掩码。
        body.reserve(12U);
        appendUint64(body, permissions->peerId);
        // 掩码原样传输，合法位集合已在前置校验确认。
        appendUint32(body, permissions->permissions);
    } else {
        // variant 若未来新增类型而编码器未更新，必须显式报未知类型。
        return std::unexpected(ProtocolError::UnknownMessageKind);
    }

    // 最终正文长度仍做统一硬校验，防止新增分支遗漏自身限制。
    if ( body.size() > std::numeric_limits<std::uint32_t>::max() ) {
        return std::unexpected(ProtocolError::InvalidMessageLength);
    }

    // 头与正文一次组装到最终缓冲，预留容量避免增长时多次重分配。
    ByteBuffer output;
    output.reserve(PROTOCOL_HEADER_BYTES + body.size());
    appendUint32(output, PROTOCOL_MAGIC);
    // 固定版本紧跟魔数，接收方在读取正文前完成兼容性判断。
    appendUint16(output, COLLABORATION_PROTOCOL_VERSION);
    output.push_back(static_cast<std::uint8_t>(kind));
    // 保留字节当前必须为零，为未来兼容扩展保留明确拒绝边界。
    output.push_back(0);
    appendUint32(output, static_cast<std::uint32_t>(body.size()));
    output.insert(output.end(), body.begin(), body.end());
    // 成功结果始终包含完整 12 字节帧头和声明长度一致的正文。
    return output;
}

/// @brief 校验固定帧头并把正文解码为类型安全消息变体。
/// @param bytes 单个完整可靠有序传输帧。
/// @param maxOperationBytes 操作及资源负载的运行时上限。
/// @return 成功时返回具体消息，失败时返回精确协议错误。
/// @warning 协作接收热路径；仅处理单帧有界内存，禁止等待和文件系统访问。
std::expected<CollaborationMessage, ProtocolError> decodeCollaborationMessage(
    std::span<const std::uint8_t> bytes, std::size_t maxOperationBytes)
{
    // ByteReader 对所有字段执行剩余长度检查，不进行未对齐指针解引用。
    ByteReader    reader(bytes);
    std::uint32_t magic     = 0;
    std::uint16_t version   = 0;
    std::uint8_t  rawKind   = 0;
    std::uint8_t  reserved  = 0;
    std::uint32_t bodyBytes = 0;
    // 局部字段先零初始化，失败解析不会泄漏未定义值到错误分支。
    // 固定头任何字段截断都统一归类为 TruncatedMessage。
    if ( !reader.readUint32(magic) || !reader.readUint16(version) ||
         !reader.readUint8(rawKind) || !reader.readUint8(reserved) ||
         !reader.readUint32(bodyBytes) ) {
        return std::unexpected(ProtocolError::TruncatedMessage);
    }
    // 魔数、版本和保留字段分别报告，便于诊断对端不兼容原因。
    if ( magic != PROTOCOL_MAGIC ) {
        // 非本协议数据无需继续解释版本或正文类型。
        return std::unexpected(ProtocolError::InvalidMagic);
    }
    if ( version != COLLABORATION_PROTOCOL_VERSION ) {
        // 不兼容版本必须在任何消息级分配或规范化之前拒绝。
        return std::unexpected(ProtocolError::UnsupportedVersion);
    }
    if ( reserved != 0 ) {
        // 当前版本不定义扩展标志，非零值不能静默忽略。
        return std::unexpected(ProtocolError::InvalidReservedField);
    }
    // 可靠传输的一帧必须恰好包含声明正文，拒绝截断和尾随字节。
    if ( reader.remaining() != bodyBytes ) {
        return std::unexpected(ProtocolError::InvalidMessageLength);
    }

    // rawKind 先转枚举，未知数值在 switch 末尾统一拒绝。
    const auto kind = static_cast<CollaborationMessageKind>(rawKind);
    switch ( kind ) {
    case CollaborationMessageKind::EditRequest: {
        // 编辑请求按固定身份、会话、客户端序号、负载长度和负载顺序读取。
        EditRequest   request;
        std::uint32_t payloadBytes = 0;
        if ( !readStableIdentity(reader, request.participantId) ||
             !readStableIdentity(reader, request.sessionId) ||
             !reader.readUint64(request.clientSequence) ||
             !reader.readUint32(payloadBytes) ) {
            // 固定身份字段失败优先按身份错误报告，避免使用部分 DTO。
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        // 零客户端序号不能参与幂等去重。
        if ( request.clientSequence == 0 ) {
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        // 在按远端长度分配 ByteBuffer 前应用本地运行时上限。
        if ( !isOperationSizeValid(payloadBytes, maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        // 声明长度合法但实际字节不足属于帧截断。
        if ( !reader.readBytes(payloadBytes, request.payload) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 具体消息解析后必须耗尽正文，拒绝隐藏的扩展或拼接字段。
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        // 完整 DTO 按值移动进 variant，避免复制操作负载。
        return CollaborationMessage(std::move(request));
    }
    case CollaborationMessageKind::CommittedOperation: {
        // 已提交操作在编辑请求字段前额外携带全局 revision。
        CommittedOperation committed;
        std::uint32_t      payloadBytes = 0;
        if ( !reader.readUint64(committed.revision) ||
             !readStableIdentity(reader, committed.participantId) ||
             !readStableIdentity(reader, committed.sessionId) ||
             !reader.readUint64(committed.clientSequence) ||
             !reader.readUint32(payloadBytes) ) {
            // 任一固定字段截断或身份格式错误都不能产生部分提交记录。
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        // revision 与 clientSequence 都以零作为未初始化保留值。
        if ( committed.revision == 0 || committed.clientSequence == 0 ) {
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        // 校验长度上限先于 payload 分配与复制。
        if ( !isOperationSizeValid(payloadBytes, maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        // 实际帧不足声明长度时返回截断，而不是操作过大。
        if ( !reader.readBytes(payloadBytes, committed.payload) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 提交消息没有可选尾随字段，剩余字节必须为零。
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        // payload 所有权随 DTO 一并移动到消息变体。
        return CollaborationMessage(std::move(committed));
    }
    case CollaborationMessageKind::RevisionAck: {
        // Ack 正文固定八字节，不含身份或可变负载。
        RevisionAck ack;
        if ( !reader.readUint64(ack.revision) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 固定消息必须精确八字节，额外内容按长度错误拒绝。
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        // Ack 是固定大小值类型，无需移动动态资源。
        return CollaborationMessage(ack);
    }
    case CollaborationMessageKind::ResyncRequest: {
        // 重同步起点允许由上层解释零值语义，协议层只验证固定宽度。
        ResyncRequest resync;
        if ( !reader.readUint64(resync.fromRevision) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 不接受未定义的扩展字段，保证版本一布局唯一。
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        // 重同步请求按值返回，由房间层决定补发或快照策略。
        return CollaborationMessage(resync);
    }
    case CollaborationMessageKind::ParticipantIdentity: {
        // 身份消息先读取 peerId、两个固定 ID，再读取 Creator 字节长度。
        ParticipantIdentity identity;
        std::uint16_t       creatorBytes = 0;
        if ( !reader.readUint64(identity.peerId) ||
             !readStableIdentity(reader, identity.participantId) ||
             !readStableIdentity(reader, identity.sessionId) ||
             !reader.readUint16(creatorBytes) ) {
            // 固定身份解析失败使用 InvalidStableIdentity，与 Creator 错误区分。
            return std::unexpected(ProtocolError::InvalidStableIdentity);
        }
        // Creator 长度在字符串分配前应用产品上限，peerId 零值也拒绝。
        if ( identity.peerId == 0 || creatorBytes == 0 ||
             creatorBytes > Config::MAX_CREATOR_IDENTITY_BYTES ) {
            return std::unexpected(ProtocolError::InvalidCreatorIdentity);
        }
        // 长度字段合法但正文不足属于截断帧。
        if ( !reader.readString(creatorBytes, identity.creator) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 规范化负责 UTF-8 与展示身份内容约束，并产生统一形式。
        identity.creator = Config::normalizeCreatorIdentity(identity.creator);
        if ( identity.creator.empty() ) {
            return std::unexpected(ProtocolError::InvalidCreatorIdentity);
        }
        // Creator 后不允许附加版本一未定义字段。
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        // 三个字符串均已独立拥有，可安全移动出 reader 输入生命周期。
        return CollaborationMessage(std::move(identity));
    }
    case CollaborationMessageKind::ParticipantLeft: {
        // 离开通知只携带待移除的非零 peerId。
        ParticipantLeft participantLeft;
        if ( !reader.readUint64(participantLeft.peerId) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 零身份和任意尾随字节都违反固定消息布局。
        if ( participantLeft.peerId == 0 || reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        // 合法离开通知只表达身份，不包含需要转移的所有权。
        return CollaborationMessage(participantLeft);
    }
    case CollaborationMessageKind::StateSnapshot: {
        // 快照正文为非零 revision、32 位 payload 长度和完整文档字节。
        StateSnapshot snapshot;
        std::uint32_t payloadBytes = 0;
        if ( !reader.readUint64(snapshot.revision) ||
             !reader.readUint32(payloadBytes) ) {
            // 固定前缀不足时没有足够信息判定负载上限。
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // revision 零或负载超过上限都不能进入文档替换路径。
        if ( snapshot.revision == 0 ||
             !isOperationSizeValid(payloadBytes, maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        // 按已验证长度复制快照，短读明确报告 TruncatedMessage。
        if ( !reader.readBytes(payloadBytes, snapshot.payload) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 快照 payload 必须占据正文全部剩余字节。
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        // 大型 payload 通过移动转交状态机。
        return CollaborationMessage(std::move(snapshot));
    }
    case CollaborationMessageKind::ResourceManifest: {
        // 清单正文包含非零资源代次和带长度 JSON/二进制载荷。
        ResourceManifest manifest;
        std::uint32_t    payloadBytes = 0;
        if ( !reader.readUint64(manifest.generation) ||
             !reader.readUint32(payloadBytes) ) {
            // 固定字段截断时尚不能信任 generation 或 payloadBytes。
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 空代次、空清单与超限清单统一阻止资源状态切换。
        if ( manifest.generation == 0 || payloadBytes == 0 ||
             !isOperationSizeValid(payloadBytes, maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        // 只有通过上限检查后才分配并读取清单内容。
        if ( !reader.readBytes(payloadBytes, manifest.payload) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 版本一清单帧不允许尾随第二份载荷。
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        // 载荷按值移动给资源同步器。
        return CollaborationMessage(std::move(manifest));
    }
    case CollaborationMessageKind::ResourceRequest: {
        // 资源请求使用全固定宽度字段，不携带可变 payload。
        ResourceRequest request;
        // 字段顺序与编码端保持代次、索引、偏移、请求长度一致。
        if ( !reader.readUint64(request.generation) ||
             !reader.readUint32(request.resourceIndex) ||
             !reader.readUint64(request.offset) ||
             !reader.readUint32(request.requestedBytes) ) {
            // 任一字段不足都表示可靠传输帧被截断。
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 代次和请求长度必须非零，长度还受当前房间策略上限约束。
        if ( request.generation == 0 || request.requestedBytes == 0 ||
             request.requestedBytes > maxOperationBytes ) {
            // 使用 OperationTooLarge 统一表达零长度和超限资源请求。
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        // 固定 24 字节正文之外不接受任何尾随内容。
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        // DTO 不持有动态载荷，可直接按值放入变体。
        return CollaborationMessage(request);
    }
    case CollaborationMessageKind::ResourceChunk: {
        // 资源分块在固定定位字段后携带带长度的密文或资源字节。
        ResourceChunk chunk;
        std::uint32_t payloadBytes = 0;
        if ( !reader.readUint64(chunk.generation) ||
             !reader.readUint32(chunk.resourceIndex) ||
             !reader.readUint64(chunk.offset) ||
             !reader.readUint32(payloadBytes) ) {
            // 固定前缀不足时不能安全计算或读取 payload。
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 资源代次和分块均不可为空，大小校验先于缓冲分配。
        if ( chunk.generation == 0 || payloadBytes == 0 ||
             !isOperationSizeValid(payloadBytes, maxOperationBytes) ) {
            return std::unexpected(ProtocolError::OperationTooLarge);
        }
        // 声明载荷未完整到达时返回截断错误。
        if ( !reader.readBytes(payloadBytes, chunk.payload) ) {
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 分块帧必须恰好消费全部正文，偏移连续性由资源状态机检查。
        if ( reader.remaining() != 0 ) {
            return std::unexpected(ProtocolError::InvalidMessageLength);
        }
        // payload 通过移动避免一次额外大块复制。
        return CollaborationMessage(std::move(chunk));
    }
    case CollaborationMessageKind::ParticipantViewport: {
        // 视口正文固定为 peerId、sequence 和五个 IEEE 754 double。
        ParticipantViewport viewport;
        if ( !reader.readUint64(viewport.peerId) ||
             !reader.readUint64(viewport.sequence) ||
             !reader.readDouble(viewport.playbackTime) ||
             !reader.readDouble(viewport.visualTime) ||
             !reader.readDouble(viewport.visibleTimeStart) ||
             !reader.readDouble(viewport.visibleTimeEnd) ||
             !reader.readDouble(viewport.horizontalOffsetRatio) ) {
            // 任一浮点位模式不足都属于截断，而非数值范围错误。
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 固定正文必须耗尽，随后统一检查有限性、跨度和幅度边界。
        if ( reader.remaining() != 0 || !isViewportStateValid(viewport) ) {
            // 尾随字段和非法数值都不能进入高频远端视口表。
            return std::unexpected(ProtocolError::InvalidViewportState);
        }
        // 验证后的轻量视口对象按值进入消息变体。
        return CollaborationMessage(viewport);
    }
    case CollaborationMessageKind::ChatMessage: {
        // 聊天正文为发送者、序号、16 位字节长度和 UTF-8 文本。
        CollaborationChatMessage chat;
        std::uint16_t            textBytes = 0;
        if ( !reader.readUint64(chat.peerId) ||
             !reader.readUint64(chat.sequence) ||
             !reader.readUint16(textBytes) ) {
            // 固定前缀不足时统一报告截断消息。
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 空文本和超产品上限文本在分配前拒绝。
        if ( textBytes == 0 ||
             textBytes > MAX_COLLABORATION_CHAT_MESSAGE_BYTES ||
             !reader.readString(textBytes, chat.text) ) {
            // 聊天长度与内容是单一语义，短读也归为无效聊天。
            return std::unexpected(ProtocolError::InvalidChatMessage);
        }
        // 读取后同时验证正文耗尽、身份序号和完整 UTF-8 可见内容。
        if ( reader.remaining() != 0 || chat.peerId == 0 ||
             chat.sequence == 0 || !isChatTextValid(chat.text) ) {
            // 二次内容校验覆盖 UTF-8、控制字符和纯空格输入。
            return std::unexpected(ProtocolError::InvalidChatMessage);
        }
        // 合法文本按值移动，避免复制聊天字符串。
        return CollaborationMessage(std::move(chat));
    }
    case CollaborationMessageKind::ParticipantPermissions: {
        // 权限消息固定携带目标 peerId 和 32 位权限掩码。
        ParticipantPermissions permissions;
        if ( !reader.readUint64(permissions.peerId) ||
             !reader.readUint32(permissions.permissions) ) {
            // 固定 12 字节不足时报告帧截断。
            return std::unexpected(ProtocolError::TruncatedMessage);
        }
        // 不允许尾随内容、零 peerId 或协议未定义的权限位。
        if ( reader.remaining() != 0 || permissions.peerId == 0 ||
             !isCollaborationPermissionMaskValid(permissions.permissions) ) {
            // 未知权限位不允许向后猜测，必须由协议版本升级承载。
            return std::unexpected(ProtocolError::InvalidPermissions);
        }
        // 验证后的固定大小 DTO 直接按值返回。
        return CollaborationMessage(permissions);
    }
    }
    // rawKind 未匹配任何当前枚举项时显式拒绝，不能猜测正文布局。
    return std::unexpected(ProtocolError::UnknownMessageKind);
}
}  // namespace MMM::Network::Collaboration
