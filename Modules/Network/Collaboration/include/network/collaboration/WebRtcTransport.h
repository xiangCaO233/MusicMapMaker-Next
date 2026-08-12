#pragma once

#include "network/collaboration/CollaborationDirectoryClient.h"
#include "network/collaboration/ICollaborationTransport.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace MMM::Network::Collaboration
{
/// @brief WebRTC 传输层异步产生的连接生命周期事件类型。
enum class WebRtcTransportEventType {
    SignalingConnected,
    RoomPublished,
    JoinPending,
    JoinRequested,
    JoinCancelled,
    PeerConnected,
    PeerDisconnected,
    Rejected,
    Error,
};

/// @brief WebRTC 传输层提供给房间控制器的连接生命周期事件。
struct WebRtcTransportEvent {
    /// @brief 事件类型。
    WebRtcTransportEventType type = WebRtcTransportEventType::Error;
    /// @brief 事件关联的远端 PeerId；尚未分配时为 0。
    PeerId peerId = 0;
    /// @brief 事件关联的远端稳定协作者标识。
    ParticipantId participantId;
    /// @brief 事件关联的远端操作会话标识。
    OperationSessionId sessionId;
    /// @brief 事件关联的远端 Creator。
    std::string creator;
    /// @brief 面向诊断日志的简短详情。
    std::string detail;
    /// @brief 等待房主处理的服务端加入请求标识；其它事件为空。
    std::string requestId;
    /// @brief 加入请求携带的主程序二进制 SHA-256 构建指纹。
    std::string buildFingerprint;
};

/// @brief 房主通过公网目录发布房间所需配置。
struct WebRtcHostConfig {
    /// @brief 公网目录与信令服务器配置。
    CollaborationServerEndpoint endpoint;
    /// @brief 在公网目录展示的房间名称。
    std::string roomName;
    /// @brief 房主 Creator 展示身份。
    std::string creator;
    /// @brief 房主持久化的稳定协作者标识。
    ParticipantId participantId;
    /// @brief 房主本次房间生命周期的操作会话标识。
    OperationSessionId sessionId;
    /// @brief 房主固定路由槽位。
    PeerId hostId = 1;
    /// @brief 房间允许的总客户端数。
    std::size_t maxParticipants = MAX_COLLABORATION_PARTICIPANTS;
    /// @brief 房主当前主程序二进制的 SHA-256 构建指纹；为空时只读取就绪缓存。
    std::string buildFingerprint;
    /// @brief 是否在 P2P 身份握手中拒绝构建指纹不同的访客。
    bool requireMatchingBuildFingerprint{ true };
};

/// @brief 访客通过公网目录加入房间所需配置。
struct WebRtcGuestConfig {
    /// @brief 公网目录与信令服务器配置。
    CollaborationServerEndpoint endpoint;
    /// @brief 从目录列表选择的房间标识。
    std::string roomId;
    /// @brief 当前访客 Creator 展示身份。
    std::string creator;
    /// @brief 当前访客持久化的稳定协作者标识。
    ParticipantId participantId;
    /// @brief 当前访客本次加入流程的操作会话标识。
    OperationSessionId sessionId;
    /// @brief 房主固定路由槽位。
    PeerId hostId = 1;
    /// @brief 访客当前主程序二进制的 SHA-256 构建指纹；为空时只读取就绪缓存。
    std::string buildFingerprint;
};

/// @brief 基于 libdatachannel 的可靠有序 WebRTC DataChannel 传输。
///
/// 双方通过中心服务发现房间并瞬时交换身份、SDP 与 ICE candidate；
/// DataChannel 打开后关闭访客信令连接，协作协议数据不经过中心服务。
class WebRtcTransport final : public ICollaborationTransport
{
public:
    /// @brief 创建尚未启动的传输实例。
    WebRtcTransport();
    /// @brief 停止信令与全部 PeerConnection。
    ~WebRtcTransport() override;

    WebRtcTransport(const WebRtcTransport&)            = delete;
    WebRtcTransport& operator=(const WebRtcTransport&) = delete;
    WebRtcTransport(WebRtcTransport&&)                 = delete;
    WebRtcTransport& operator=(WebRtcTransport&&)      = delete;

    /// @brief 作为房主连接中心服务并发布房间。
    /// @param config 房主配置。
    /// @return WebSocket 已开始连接时返回 true。
    [[nodiscard]] bool startHost(const WebRtcHostConfig& config);

    /// @brief 作为访客通过中心服务加入公开房间。
    /// @param config 访客配置。
    /// @return WebSocket 已开始连接时返回 true。
    [[nodiscard]] bool connectToHost(const WebRtcGuestConfig& config);

    /// @brief 房主批准一个仍在中心服务等待的加入请求。
    /// @param requestId 服务端生成并随 JoinRequested 事件提供的请求标识。
    /// @return 请求存在且已开始创建专用 P2P 信令连接时返回 true。
    [[nodiscard]] bool approveJoinRequest(std::string_view requestId);

    /// @brief 房主拒绝一个仍在中心服务等待的加入请求。
    /// @param requestId 服务端生成并随 JoinRequested 事件提供的请求标识。
    /// @return 拒绝指令成功发送到房主控制连接时返回 true。
    [[nodiscard]] bool rejectJoinRequest(
        std::string_view requestId, std::string_view reason = "host_rejected");

    /// @brief 房主主动关闭一个已建立的访客 P2P 连接。
    /// @param peerId 待移出的远端 PeerId。
    /// @param detail 房主侧离开事件使用的稳定原因码。
    /// @return 找到在线访客并开始关闭时返回 true。
    [[nodiscard]] bool disconnectPeer(PeerId peerId, std::string detail);

    /// @brief 停止当前传输；可重复调用。
    void stop();

    /// @brief 查询传输是否已启动。
    [[nodiscard]] bool isRunning() const;
    /// @brief 查询当前传输是否为房主端。
    [[nodiscard]] bool isHost() const;
    /// @brief 获取当前客户端 PeerId；访客等待房主分配期间为 0。
    [[nodiscard]] PeerId localPeerId() const;
    /// @brief 获取中心服务分配的公开房间标识。
    [[nodiscard]] std::string roomId() const;

    /// @brief 非阻塞读取下一条连接生命周期事件。
    /// @param event 输出事件。
    /// @return 存在事件时返回 true。
    [[nodiscard]] bool receiveEvent(WebRtcTransportEvent& event);

    /// @copydoc ICollaborationTransport::send
    [[nodiscard]] bool send(PeerId                        recipientId,
                            std::span<const std::uint8_t> payload) override;

    /// @copydoc ICollaborationTransport::receive
    [[nodiscard]] bool receive(TransportPacket& packet) override;

private:
    class Impl;
    /// @brief 隔离 libdatachannel 回调状态与第三方头文件。
    std::unique_ptr<Impl> m_impl;
};
}  // namespace MMM::Network::Collaboration
