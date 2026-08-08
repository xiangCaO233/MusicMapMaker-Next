#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MMM::Network::Collaboration
{
/// @brief 协作房间内的客户端稳定标识。
using PeerId = std::uint64_t;

/// @brief 协作协议传输的原始字节序列。
using ByteBuffer = std::vector<std::uint8_t>;

/// @brief 单个协作房间允许的最小客户端数。
inline constexpr std::size_t MIN_COLLABORATION_PARTICIPANTS = 2;
/// @brief 单个协作房间允许的最大客户端数。
inline constexpr std::size_t MAX_COLLABORATION_PARTICIPANTS = 8;

/// @brief 单条规范化增量编辑请求。
struct EditRequest {
    /// @brief 发起请求的客户端标识。
    PeerId clientId = 0;
    /// @brief 发起客户端内严格递增的请求序号。
    std::uint64_t clientSequence = 0;
    /// @brief 与渲染和进程内实体无关的规范化操作负载。
    ByteBuffer payload;
};

/// @brief 房主已经排序并分配版本的增量编辑操作。
struct CommittedOperation {
    /// @brief 房间内严格连续递增的谱面版本。
    std::uint64_t revision = 0;
    /// @brief 原始请求的客户端标识。
    PeerId clientId = 0;
    /// @brief 原始请求在该客户端内的序号。
    std::uint64_t clientSequence = 0;
    /// @brief 可直接应用到本地谱面模型的规范化操作负载。
    ByteBuffer payload;
};

/// @brief 房间内可展示的客户端 Creator 身份。
struct ParticipantIdentity {
    /// @brief 传输、路由和操作去重继续使用的内部客户端标识。
    PeerId clientId = 0;
    /// @brief 展示给全部参与者的 Creator。
    std::string creator;
};

/// @brief 通知访客移除一个已经离开的展示身份。
struct ParticipantLeft {
    /// @brief 已离开客户端的内部标识。
    PeerId clientId = 0;
};

/// @brief 协作参与者在主画布中的轻量视口状态。
struct ParticipantViewport {
    /// @brief 发布该状态的客户端标识。
    PeerId clientId = 0;
    /// @brief 发布客户端内严格递增的视口状态序号。
    std::uint64_t sequence = 0;
    /// @brief 可直接传给 CmdSeek 的原始谱面播放时间，单位为秒。
    double playbackTime = 0.0;
    /// @brief 主画布判定线对应的视觉时间，单位为秒。
    double visualTime = 0.0;
    /// @brief 发布端主画布轨道区下边界对应的视觉时间，单位为秒。
    double visibleTimeStart = 0.0;
    /// @brief 发布端主画布轨道区上边界对应的视觉时间，单位为秒。
    double visibleTimeEnd = 0.0;
    /// @brief 主画布横向像素偏移除以发布端视口宽度后的比例。
    double horizontalOffsetRatio = 0.0;
};

/// @brief 协作 Peer 的有界处理参数。
struct CollaborationPeerLimits {
    /// @brief 单条增量操作允许的最大字节数。
    std::size_t maxOperationBytes = 1024U * 1024U;
    /// @brief 每次 update 最多读取的传输消息数。
    std::size_t maxMessagesPerUpdate = 256;
    /// @brief 每次 update 最多提交的房主请求数。
    std::size_t maxRequestsPerUpdate = 256;
    /// @brief 房主待处理请求队列的最大长度。
    std::size_t maxPendingRequests = 4096;
    /// @brief 房主保留的增量操作日志长度。
    std::size_t maxJournalOperations = 4096;
};

/// @brief 创建统一房主或访客 Peer 所需的身份配置。
struct CollaborationPeerConfig {
    /// @brief 当前客户端标识。
    PeerId clientId = 0;
    /// @brief 当前房间的房主标识。
    PeerId hostId = 0;
    /// @brief 当前客户端的 Creator 展示身份；为空时禁止进入联机。
    std::string creator;
    /// @brief 当前客户端是否承担权威排序职责。
    bool isHost = false;
    /// @brief 房间允许的总客户端数，构造时限制到 2～8。
    std::size_t maxParticipants = MAX_COLLABORATION_PARTICIPANTS;
    /// @brief 有界队列和消息大小限制。
    CollaborationPeerLimits limits;
};
}  // namespace MMM::Network::Collaboration
