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
    /// @note 端点创建时登记，端点析构时连同未消费消息一起移除。
    std::unordered_map<PeerId, std::deque<TransportPacket>> queues;
    /// @brief 需要丢弃一次的发送方和接收方组合。
    /// @note 命中后立即清空，使故障注入只影响一条匹配消息。
    std::optional<std::pair<PeerId, PeerId>> dropNext;
    /// @brief 是否复制后续发送包。
    /// @note 复制发生在同一接收队列内，用于验证上层协议去重。
    bool duplicatePackets = false;
    /// @brief 保护端点队列和故障注入配置。
    /// @note 所有字段必须在持有此锁时访问，避免测试线程并发竞态。
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
        // 队列已由 Hub 在构造端点前登记，端点只保存身份和共享状态。
    }

    /// @brief 注销端点并丢弃其尚未读取的本地测试消息。
    ~LoopbackTransport() override
    {
        // 析构与并发发送串行化，确保发送方不会取得即将失效的队列。
        std::scoped_lock lock(m_state->mutex);
        // 移除队列也明确丢弃尚未接收的数据，模拟本地端点断开。
        m_state->queues.erase(m_peerId);
    }

    /// @brief 向另一个本地端点的接收队列追加完整消息。
    /// @param recipientId 接收端点标识。
    /// @param payload 完整线协议帧。
    /// @return 接收端点存在且消息成功入队时返回 true。
    /// @warning 协作状态更新可高频调用；只允许短时持锁和内存入队，禁止等待。
    [[nodiscard]] bool send(PeerId                        recipientId,
                            std::span<const std::uint8_t> payload) override
    {
        // 查找、故障注入和入队必须共用一次临界区，保持消息顺序确定。
        std::scoped_lock lock(m_state->mutex);
        const auto       queueIt = m_state->queues.find(recipientId);
        if ( queueIt == m_state->queues.end() ) {
            // 未登记的接收方视为断开；调用方可按发送失败处理。
            return false;
        }

        // 丢包条件同时匹配发送方与接收方，避免误伤其他测试链路。
        if ( m_state->dropNext.has_value() &&
             m_state->dropNext->first == m_peerId &&
             m_state->dropNext->second == recipientId ) {
            // 故障注入模拟网络静默丢包，因此发送端仍观察到成功。
            m_state->dropNext.reset();
            return true;
        }

        // payload 的 span 只在调用期间有效，必须复制到队列自有存储。
        TransportPacket packet;
        packet.senderId = m_peerId;
        packet.payload.assign(payload.begin(), payload.end());
        // deque 保持同一接收方的发送顺序，符合可靠有序传输测试模型。
        queueIt->second.push_back(packet);
        if ( m_state->duplicatePackets ) {
            // 首份已完成复制，此处移动局部副本可避免第三次负载复制。
            queueIt->second.push_back(std::move(packet));
        }
        return true;
    }

    /// @brief 从当前本地端点读取下一条完整消息。
    /// @param packet 接收消息的输出对象。
    /// @return 队列非空时返回 true。
    /// @warning 协作轮询路径可高频调用；空队列必须立即返回，禁止阻塞等待。
    [[nodiscard]] bool receive(TransportPacket& packet) override
    {
        // 读取与端点析构、其他线程发送串行化，确保队首引用始终有效。
        std::scoped_lock lock(m_state->mutex);
        const auto       queueIt = m_state->queues.find(m_peerId);
        if ( queueIt == m_state->queues.end() || queueIt->second.empty() ) {
            // 未登记和暂时无消息都使用非阻塞 false 结果表达。
            return false;
        }
        // 将队首所有权交给调用方，避免复制可能较大的协议负载。
        packet = std::move(queueIt->second.front());
        // 只有输出赋值完成后才移除队首，维持异常禁用下的清晰状态转换。
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
    // Hub 与全部端点共享同一状态，使端点可安全晚于 Hub 的局部调用栈存在。
}

LoopbackTransportHub::~LoopbackTransportHub() = default;

/// @brief 登记唯一客户端队列并创建绑定该队列的本地端点。
/// @param peerId 非零客户端标识。
/// @return 登记成功时返回端点；无效或重复标识返回空。
std::unique_ptr<ICollaborationTransport> LoopbackTransportHub::createEndpoint(
    PeerId peerId)
{
    // 零值保留给协议的无效身份，不能成为可寻址端点。
    if ( peerId == 0 ) {
        return {};
    }
    {
        // 先原子式登记空队列，再构造端点，避免端点可见时尚无接收槽位。
        std::scoped_lock lock(m_state->mutex);
        const auto [queueIt, inserted] =
            m_state->queues.try_emplace(peerId, std::deque<TransportPacket>{});
        // 仅需 inserted 判定唯一性，显式忽略迭代器避免未使用告警。
        static_cast<void>(queueIt);
        if ( !inserted ) {
            // 拒绝重复标识，防止两个端点竞争同一接收队列。
            return {};
        }
    }
    // 端点构造不再访问队列表，缩短全局锁持有时间。
    return std::make_unique<LoopbackTransport>(peerId, m_state);
}

/// @brief 切换后续消息的重复注入模式。
/// @param enabled 为 true 时每次正常发送产生两个相邻副本。
void LoopbackTransportHub::setDuplicatePackets(bool enabled)
{
    // 与 send 共用锁，使开关变更对下一次入队具有确定边界。
    std::scoped_lock lock(m_state->mutex);
    m_state->duplicatePackets = enabled;
}

/// @brief 配置一次性的定向静默丢包。
/// @param senderId 必须匹配的发送端点标识。
/// @param recipientId 必须匹配的接收端点标识。
void LoopbackTransportHub::dropNextPacket(PeerId senderId, PeerId recipientId)
{
    // 新配置覆盖尚未命中的旧配置，便于测试精确控制下一次故障。
    std::scoped_lock lock(m_state->mutex);
    m_state->dropNext = std::pair(senderId, recipientId);
}
}  // namespace MMM::Network::Collaboration
