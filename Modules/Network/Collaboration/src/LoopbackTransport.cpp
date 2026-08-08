#include "network/collaboration/LoopbackTransport.h"

#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace MMM::Network::Collaboration
{
/// @brief 本地传输中心与所有端点共享的受锁队列状态。
class LoopbackTransportState
{
public:
    /// @brief 每个客户端各自的可靠有序接收队列。
    std::unordered_map<PeerId, std::deque<TransportPacket>> queues;
    /// @brief 需要丢弃一次的发送方和接收方组合。
    std::optional<std::pair<PeerId, PeerId>> dropNext;
    /// @brief 是否复制后续发送包。
    bool duplicatePackets = false;
    /// @brief 保护端点队列和故障注入配置。
    std::mutex mutex;
};

namespace
{
/// @brief 绑定单个客户端标识的本地传输端点。
class LoopbackTransport final : public ICollaborationTransport
{
public:
    /// @brief 创建绑定到共享传输状态的端点。
    /// @param peerId 当前端点标识。
    /// @param state 共享队列状态。
    LoopbackTransport(PeerId                                  peerId,
                      std::shared_ptr<LoopbackTransportState> state)
        : m_peerId(peerId), m_state(std::move(state))
    {
    }

    /// @brief 注销端点并丢弃其尚未读取的本地测试消息。
    ~LoopbackTransport() override
    {
        std::scoped_lock lock(m_state->mutex);
        m_state->queues.erase(m_peerId);
    }

    /// @brief 向另一个本地端点的接收队列追加完整消息。
    /// @param recipientId 接收端点标识。
    /// @param payload 完整线协议帧。
    /// @return 接收端点存在且消息成功入队时返回 true。
    [[nodiscard]] bool send(PeerId                        recipientId,
                            std::span<const std::uint8_t> payload) override
    {
        std::scoped_lock lock(m_state->mutex);
        const auto       queueIt = m_state->queues.find(recipientId);
        if ( queueIt == m_state->queues.end() ) {
            return false;
        }

        if ( m_state->dropNext.has_value() &&
             m_state->dropNext->first == m_peerId &&
             m_state->dropNext->second == recipientId ) {
            m_state->dropNext.reset();
            return true;
        }

        TransportPacket packet;
        packet.senderId = m_peerId;
        packet.payload.assign(payload.begin(), payload.end());
        queueIt->second.push_back(packet);
        if ( m_state->duplicatePackets ) {
            queueIt->second.push_back(std::move(packet));
        }
        return true;
    }

    /// @brief 从当前本地端点读取下一条完整消息。
    /// @param packet 接收消息的输出对象。
    /// @return 队列非空时返回 true。
    [[nodiscard]] bool receive(TransportPacket& packet) override
    {
        std::scoped_lock lock(m_state->mutex);
        const auto       queueIt = m_state->queues.find(m_peerId);
        if ( queueIt == m_state->queues.end() || queueIt->second.empty() ) {
            return false;
        }
        packet = std::move(queueIt->second.front());
        queueIt->second.pop_front();
        return true;
    }

private:
    /// @brief 当前端点的发送方和接收队列标识。
    PeerId m_peerId = 0;
    /// @brief 与 Hub 共享的测试队列状态，避免端点悬空访问。
    std::shared_ptr<LoopbackTransportState> m_state;
};
}  // namespace

LoopbackTransportHub::LoopbackTransportHub()
    : m_state(std::make_shared<LoopbackTransportState>())
{
}

LoopbackTransportHub::~LoopbackTransportHub() = default;

std::unique_ptr<ICollaborationTransport> LoopbackTransportHub::createEndpoint(
    PeerId peerId)
{
    if ( peerId == 0 ) {
        return {};
    }
    {
        std::scoped_lock lock(m_state->mutex);
        const auto [queueIt, inserted] =
            m_state->queues.try_emplace(peerId, std::deque<TransportPacket>{});
        static_cast<void>(queueIt);
        if ( !inserted ) {
            return {};
        }
    }
    return std::make_unique<LoopbackTransport>(peerId, m_state);
}

void LoopbackTransportHub::setDuplicatePackets(bool enabled)
{
    std::scoped_lock lock(m_state->mutex);
    m_state->duplicatePackets = enabled;
}

void LoopbackTransportHub::dropNextPacket(PeerId senderId, PeerId recipientId)
{
    std::scoped_lock lock(m_state->mutex);
    m_state->dropNext = std::pair(senderId, recipientId);
}
}  // namespace MMM::Network::Collaboration
