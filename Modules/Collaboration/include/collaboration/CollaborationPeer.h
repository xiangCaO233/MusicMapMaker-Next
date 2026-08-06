#pragma once

#include "collaboration/CollaborationProtocol.h"
#include "collaboration/ICollaborationTransport.h"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace MMM::Collaboration
{
/// @brief 本地操作提交结果。
enum class SubmitOperationResult : std::uint8_t {
    Accepted,
    InvalidPeer,
    EmptyOperation,
    OperationTooLarge,
    QueueFull,
    TransportUnavailable,
};

/// @brief 协作 Peer 的诊断计数。
struct CollaborationPeerStats {
    /// @brief 无法解析或身份不合法的消息数。
    std::uint64_t invalidMessages = 0;
    /// @brief 房主忽略的重复编辑请求数。
    std::uint64_t duplicateRequests = 0;
    /// @brief 访客忽略的重复提交操作数。
    std::uint64_t duplicateCommits = 0;
    /// @brief 因房主待处理队列达到上限而丢弃的请求数。
    std::uint64_t droppedRequests = 0;
    /// @brief 访客发出的版本缺口补发请求数。
    std::uint64_t resyncRequests = 0;
    /// @brief 房主日志已无法覆盖的补发请求数。
    std::uint64_t resyncUnavailable = 0;
    /// @brief 传输层拒绝发送的消息数。
    std::uint64_t sendFailures = 0;
};

/// @brief 房主和访客共用的权威增量同步状态机。
class CollaborationPeer
{
public:
    /// @brief 已提交操作应用到本地谱面模型时调用的回调。
    using ApplyOperationCallback =
        std::function<void(const CommittedOperation&)>;

    /// @brief 创建统一房主或访客 Peer。
    /// @param config 身份和有界处理配置。
    /// @param transport 可靠有序字节传输实现。
    /// @param applyCallback 已提交操作的本地应用回调。
    CollaborationPeer(CollaborationPeerConfig                  config,
                      std::unique_ptr<ICollaborationTransport> transport,
                      ApplyOperationCallback                   applyCallback);

    /// @brief 释放 Peer 和其独占传输端点。
    ~CollaborationPeer();

    CollaborationPeer(const CollaborationPeer&)            = delete;
    CollaborationPeer& operator=(const CollaborationPeer&) = delete;

    /// @brief 返回身份和传输配置是否有效。
    /// @return Peer 可以处理操作时返回 true。
    [[nodiscard]] bool isValid() const;

    /// @brief 返回当前 Peer 是否承担房主权威职责。
    /// @return `isHost` 配置值。
    [[nodiscard]] bool isHost() const;

    /// @brief 返回已经连续应用到本地模型的版本。
    /// @return 当前连续版本。
    [[nodiscard]] std::uint64_t appliedRevision() const;

    /// @brief 返回只读诊断计数。
    /// @return 当前累计统计。
    [[nodiscard]] const CollaborationPeerStats& stats() const;

    /// @brief 房主登记一个可提交和接收增量操作的访客。
    /// @param peerId 访客标识。
    /// @return 当前 Peer 是房主且标识合法时返回 true。
    [[nodiscard]] bool addParticipant(PeerId peerId);

    /// @brief 房主移除一个访客及其确认状态。
    /// @param peerId 访客标识。
    void removeParticipant(PeerId peerId);

    /// @brief 提交一条规范化本地增量操作。
    /// @param payload 与 UI 坐标、渲染状态和 ECS 实体无关的操作负载。
    /// @return 请求入房主队列或访客传输队列后的结果。
    [[nodiscard]] SubmitOperationResult submitOperation(
        std::span<const std::uint8_t> payload);

    /// @brief 处理有界数量的网络消息和房主编辑请求。
    /// @warning
    /// 计划在每次逻辑更新调用；不得加入文件系统访问、全量谱面遍历、完整排序或阻塞等待。
    void update();

private:
    /// @brief 把请求加入房主的有界权威队列。
    /// @param request 待处理请求。
    /// @return 入队成功时返回 true。
    [[nodiscard]] bool enqueueHostRequest(EditRequest request);

    /// @brief 根据角色和发送方分派一条已解析消息。
    /// @param senderId 传输层确认的发送方。
    /// @param message 已解析消息。
    void handleMessage(PeerId senderId, const CollaborationMessage& message);

    /// @brief 房主校验并接收访客编辑请求。
    /// @param senderId 传输层确认的发送方。
    /// @param request 编辑请求。
    void handleEditRequest(PeerId senderId, const EditRequest& request);

    /// @brief 访客按连续版本应用房主提交操作。
    /// @param senderId 传输层确认的发送方。
    /// @param committed 房主提交操作。
    void handleCommittedOperation(PeerId                    senderId,
                                  const CommittedOperation& committed);

    /// @brief 房主记录访客连续确认版本。
    /// @param senderId 访客标识。
    /// @param ack 确认消息。
    void handleRevisionAck(PeerId senderId, const RevisionAck& ack);

    /// @brief 房主从有界日志补发指定版本后的操作。
    /// @param senderId 请求补发的访客标识。
    /// @param request 补发范围。
    void handleResyncRequest(PeerId senderId, const ResyncRequest& request);

    /// @brief 房主提交本轮允许数量的待处理请求。
    void processHostRequests();

    /// @brief 把一条提交操作应用到当前客户端模型。
    /// @param committed 已保证连续的提交操作。
    void applyCommittedOperation(const CommittedOperation& committed);

    /// @brief 向一个客户端发送已编码消息。
    /// @param recipientId 接收客户端标识。
    /// @param message 待编码消息。
    /// @return 编码和传输均成功时返回 true。
    [[nodiscard]] bool sendMessage(PeerId                      recipientId,
                                   const CollaborationMessage& message);

    /// @brief 房主广播一条已提交操作。
    /// @param committed 已提交操作。
    void broadcastCommittedOperation(const CommittedOperation& committed);

    /// @brief 访客向房主发送当前连续版本确认。
    void sendRevisionAck();

    /// @brief 访客请求从当前缺失版本开始补发。
    /// @param observedRevision 触发缺口检测的较新版本。
    void requestResync(std::uint64_t observedRevision);

    /// @brief 身份、房主角色和有界处理参数。
    CollaborationPeerConfig m_config;
    /// @brief 当前 Peer 独占的可靠有序传输端点。
    std::unique_ptr<ICollaborationTransport> m_transport;
    /// @brief 已提交操作的本地谱面应用入口。
    ApplyOperationCallback m_applyCallback;
    /// @brief 当前身份和传输是否满足状态机前置条件。
    bool m_valid = false;
    /// @brief 当前客户端下一个本地请求序号。
    std::uint64_t m_nextClientSequence = 1;
    /// @brief 房主要分配的下一个全房间版本。
    std::uint64_t m_nextRevision = 1;
    /// @brief 当前客户端已经连续应用的最高版本。
    std::uint64_t m_appliedRevision = 0;
    /// @brief 当前补发流程期望追赶到的版本。
    std::optional<std::uint64_t> m_resyncTargetRevision;
    /// @brief 房主允许参与协作的访客集合。
    std::unordered_set<PeerId> m_participants;
    /// @brief 房主尚未排序提交的本地和远端请求。
    std::deque<EditRequest> m_pendingRequests;
    /// @brief 房主为每个客户端接受的最高请求序号。
    std::unordered_map<PeerId, std::uint64_t> m_lastAcceptedSequence;
    /// @brief 房主保留的有界已提交操作日志。
    std::deque<CommittedOperation> m_journal;
    /// @brief 房主记录的各访客最高连续确认版本。
    std::unordered_map<PeerId, std::uint64_t> m_lastAcknowledgedRevision;
    /// @brief 当前 Peer 累计诊断统计。
    CollaborationPeerStats m_stats;
};
}  // namespace MMM::Collaboration
