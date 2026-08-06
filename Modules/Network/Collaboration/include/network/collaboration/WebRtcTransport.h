#pragma once

#include "network/collaboration/ICollaborationTransport.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace MMM::Network::Collaboration
{
/// @brief WebRTC 传输层异步产生的连接生命周期事件类型。
enum class WebRtcTransportEventType {
    SignalingConnected,
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
    /// @brief 事件关联的远端 Creator。
    std::string creator;
    /// @brief 面向诊断日志的简短详情。
    std::string detail;
};

/// @brief 房主启动本机 WebSocket 信令入口所需配置。
struct WebRtcHostConfig {
    /// @brief 监听端口；0 表示由系统自动选择可用端口。
    std::uint16_t port = 24864;
    /// @brief 加入房间时必须匹配的短房间码。
    std::string roomCode;
    /// @brief 房主 Creator 展示身份。
    std::string creator;
    /// @brief 房主稳定 PeerId。
    PeerId hostId = 1;
    /// @brief 房间允许的总客户端数。
    std::size_t maxParticipants = MAX_COLLABORATION_PARTICIPANTS;
};

/// @brief 访客连接房主信令入口所需配置。
struct WebRtcGuestConfig {
    /// @brief 房主 IP 地址或主机名。
    std::string host;
    /// @brief 房主信令端口。
    std::uint16_t port = 24864;
    /// @brief 房主显示的短房间码。
    std::string roomCode;
    /// @brief 当前访客 Creator 展示身份。
    std::string creator;
    /// @brief 房主稳定 PeerId。
    PeerId hostId = 1;
};

/// @brief 基于 libdatachannel 的可靠有序 WebRTC DataChannel 传输。
///
/// 房主在本地同时提供最小 WebSocket 信令入口；信令只交换身份、SDP 与
/// ICE candidate，协作协议数据不经过 WebSocket。
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

    /// @brief 作为房主启动信令服务。
    /// @param config 房主配置。
    /// @return 成功监听时返回 true。
    [[nodiscard]] bool startHost(const WebRtcHostConfig& config);

    /// @brief 作为访客连接房主信令服务。
    /// @param config 访客配置。
    /// @return WebSocket 已开始连接时返回 true。
    [[nodiscard]] bool connectToHost(const WebRtcGuestConfig& config);

    /// @brief 停止当前传输；可重复调用。
    void stop();

    /// @brief 查询传输是否已启动。
    [[nodiscard]] bool isRunning() const;
    /// @brief 查询当前传输是否为房主端。
    [[nodiscard]] bool isHost() const;
    /// @brief 获取当前客户端 PeerId；访客等待房主分配期间为 0。
    [[nodiscard]] PeerId localPeerId() const;
    /// @brief 获取房主实际监听端口。
    [[nodiscard]] std::uint16_t listeningPort() const;

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
