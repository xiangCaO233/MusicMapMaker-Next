#include "network/collaboration/WebRtcTransport.h"

#include "config/CreatorIdentity.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace MMM::Network::Collaboration
{
namespace
{
/// @brief 最小信令协议版本，与协作数据协议独立演进。
constexpr std::uint64_t SIGNALING_PROTOCOL_VERSION = 1;
/// @brief 协作 DataChannel 的稳定标签。
constexpr std::string_view COLLABORATION_CHANNEL_LABEL = "mmm-collaboration-v2";
/// @brief 单条信令消息大小上限。
constexpr int MAX_SIGNALING_MESSAGE_BYTES = 256 * 1024;
/// @brief WebRTC 协商的数据通道消息上限。
constexpr int MAX_DATA_CHANNEL_MESSAGE_BYTES = 2 * 1024 * 1024;
/// @brief 回调线程允许积压的完整 DataChannel 消息数。
constexpr std::size_t MAX_QUEUED_DATA_CHANNEL_PACKETS = 4096;
/// @brief 产品层未及时消费时保留的连接事件数。
constexpr std::size_t MAX_QUEUED_TRANSPORT_EVENTS = 1024;

/// @brief 将人工输入的房间码规范化为大写 ASCII。
/// @param value 输入房间码。
/// @return 只包含 4～12 位字母数字的规范化房间码，否则为空。
std::string normalizeRoomCode(std::string_view value)
{
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.front())) != 0 ) {
        value.remove_prefix(1);
    }
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.back())) != 0 ) {
        value.remove_suffix(1);
    }
    if ( value.size() < 4 || value.size() > 12 ) {
        return {};
    }

    std::string result;
    result.reserve(value.size());
    for ( const char character : value ) {
        const auto byte = static_cast<unsigned char>(character);
        if ( std::isalnum(byte) == 0 ) {
            return {};
        }
        result.push_back(static_cast<char>(std::toupper(byte)));
    }
    return result;
}

/// @brief 从 JSON 对象读取字符串字段。
bool readStringField(const nlohmann::json& object, std::string_view key,
                     std::string& value)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_string() ) {
        return false;
    }
    value = iterator->get_ref<const std::string&>();
    return true;
}

/// @brief 从 JSON 对象读取无符号整数字段。
bool readUnsignedField(const nlohmann::json& object, std::string_view key,
                       std::uint64_t& value)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_number_unsigned() ) {
        return false;
    }
    value = iterator->get<std::uint64_t>();
    return true;
}

/// @brief 为 WebSocket 客户端构造 ws URL，兼容裸 IPv6 地址。
std::string makeWebSocketUrl(std::string host, std::uint16_t port)
{
    if ( host.find(':') != std::string::npos &&
         !(host.starts_with('[') && host.ends_with(']')) ) {
        host = '[' + host + ']';
    }
    return "ws://" + host + ':' + std::to_string(port) + "/mmm-collaboration";
}
}  // namespace

class WebRtcTransport::Impl
{
public:
    /// @brief 一个访客到房主的信令、PeerConnection 与 DataChannel 上下文。
    struct Connection {
        /// @brief 所属传输实现的稳定观察指针。
        Impl* owner = nullptr;
        /// @brief 信令 WebSocket 标识。
        int websocketId = -1;
        /// @brief WebRTC PeerConnection 标识。
        int peerConnectionId = -1;
        /// @brief 可靠有序 DataChannel 标识。
        int dataChannelId = -1;
        /// @brief 远端 PeerId。
        PeerId remotePeerId = 0;
        /// @brief 远端 Creator。
        std::string remoteCreator;
        /// @brief 是否已经完成房间码和 Creator 校验。
        bool joined = false;
        /// @brief 是否已经上报 DataChannel 建立事件。
        bool connectedEventSent = false;
        /// @brief 是否已经上报离开事件。
        bool disconnectedEventSent = false;
    };

    /// @brief 停止并释放所有 libdatachannel 句柄。
    ~Impl() { stop(); }

    /// @brief 启动房主端信令服务。
    bool startHost(const WebRtcHostConfig& config)
    {
        const auto creator  = Config::normalizeCreatorIdentity(config.creator);
        const auto roomCode = normalizeRoomCode(config.roomCode);
        if ( creator.empty() || roomCode.empty() || config.hostId == 0 ) {
            return false;
        }

        {
            std::scoped_lock lock(m_mutex);
            if ( m_running ) return false;
            m_isHost          = true;
            m_creator         = creator;
            m_roomCode        = roomCode;
            m_localPeerId     = config.hostId;
            m_hostId          = config.hostId;
            m_maxParticipants = std::clamp(config.maxParticipants,
                                           MIN_COLLABORATION_PARTICIPANTS,
                                           MAX_COLLABORATION_PARTICIPANTS);
            m_stopping        = false;
        }

        rtcWsServerConfiguration serverConfig{};
        serverConfig.port                = config.port;
        serverConfig.enableTls           = false;
        serverConfig.bindAddress         = nullptr;
        serverConfig.maxMessageSize      = MAX_SIGNALING_MESSAGE_BYTES;
        serverConfig.connectionTimeoutMs = 10000;

        const int serverId =
            rtcCreateWebSocketServer(&serverConfig, &Impl::onWebSocketClient);
        if ( serverId < 0 ) {
            resetStartFailure();
            return false;
        }
        rtcSetUserPointer(serverId, this);

        const int actualPort = rtcGetWebSocketServerPort(serverId);
        if ( actualPort <= 0 || actualPort > 65535 ) {
            rtcDeleteWebSocketServer(serverId);
            resetStartFailure();
            return false;
        }

        {
            std::scoped_lock lock(m_mutex);
            m_webSocketServerId = serverId;
            m_listeningPort     = static_cast<std::uint16_t>(actualPort);
            m_running           = true;
        }
        return true;
    }

    /// @brief 启动访客端信令连接。
    bool connectToHost(const WebRtcGuestConfig& config)
    {
        const auto creator  = Config::normalizeCreatorIdentity(config.creator);
        const auto roomCode = normalizeRoomCode(config.roomCode);
        if ( creator.empty() || roomCode.empty() || config.host.empty() ||
             config.port == 0 || config.hostId == 0 ) {
            return false;
        }

        auto connection           = std::make_unique<Connection>();
        connection->owner         = this;
        connection->remotePeerId  = config.hostId;
        Connection* connectionPtr = connection.get();

        {
            std::scoped_lock lock(m_mutex);
            if ( m_running ) return false;
            m_isHost      = false;
            m_creator     = creator;
            m_roomCode    = roomCode;
            m_localPeerId = 0;
            m_hostId      = config.hostId;
            m_stopping    = false;
            m_connections.push_back(std::move(connection));
        }

        const std::string  url = makeWebSocketUrl(config.host, config.port);
        rtcWsConfiguration webSocketConfig{};
        webSocketConfig.connectionTimeoutMs = 10000;
        webSocketConfig.pingIntervalMs      = 5000;
        webSocketConfig.maxOutstandingPings = 3;
        webSocketConfig.maxMessageSize      = MAX_SIGNALING_MESSAGE_BYTES;
        const int webSocketId =
            rtcCreateWebSocketEx(url.c_str(), &webSocketConfig);
        if ( webSocketId < 0 ) {
            stop();
            return false;
        }

        connectionPtr->websocketId = webSocketId;
        rtcSetUserPointer(webSocketId, connectionPtr);
        rtcSetOpenCallback(webSocketId, &Impl::onGuestWebSocketOpen);
        rtcSetMessageCallback(webSocketId, &Impl::onWebSocketMessage);
        rtcSetClosedCallback(webSocketId, &Impl::onWebSocketClosed);
        rtcSetErrorCallback(webSocketId, &Impl::onWebSocketError);

        {
            std::scoped_lock lock(m_mutex);
            m_running = true;
        }
        return true;
    }

    /// @brief 释放服务器、通道、PeerConnection 和 WebSocket。
    void stop()
    {
        int                                      serverId = -1;
        std::vector<std::unique_ptr<Connection>> connections;
        {
            std::scoped_lock lock(m_mutex);
            if ( !m_running && m_webSocketServerId < 0 &&
                 m_connections.empty() ) {
                return;
            }
            m_stopping      = true;
            m_running       = false;
            serverId        = std::exchange(m_webSocketServerId, -1);
            m_listeningPort = 0;
            m_localPeerId   = 0;
            connections     = std::move(m_connections);
            m_incomingPackets.clear();
        }

        if ( serverId >= 0 ) {
            rtcDeleteWebSocketServer(serverId);
        }
        for ( auto& connection : connections ) {
            if ( connection->dataChannelId >= 0 ) {
                rtcDeleteDataChannel(connection->dataChannelId);
                connection->dataChannelId = -1;
            }
            if ( connection->peerConnectionId >= 0 ) {
                rtcDeletePeerConnection(connection->peerConnectionId);
                connection->peerConnectionId = -1;
            }
            if ( connection->websocketId >= 0 ) {
                rtcDeleteWebSocket(connection->websocketId);
                connection->websocketId = -1;
            }
        }

        std::scoped_lock lock(m_mutex);
        m_stopping = false;
    }

    /// @brief 查询运行状态。
    bool isRunning() const
    {
        std::scoped_lock lock(m_mutex);
        return m_running;
    }

    /// @brief 查询房主角色。
    bool isHost() const
    {
        std::scoped_lock lock(m_mutex);
        return m_isHost;
    }

    /// @brief 获取本地 PeerId。
    PeerId localPeerId() const
    {
        std::scoped_lock lock(m_mutex);
        return m_localPeerId;
    }

    /// @brief 获取监听端口。
    std::uint16_t listeningPort() const
    {
        std::scoped_lock lock(m_mutex);
        return m_listeningPort;
    }

    /// @brief 发送协作协议二进制帧。
    bool send(PeerId recipientId, std::span<const std::uint8_t> payload)
    {
        if ( payload.empty() ||
             payload.size() > static_cast<std::size_t>(INT_MAX) ) {
            return false;
        }

        int dataChannelId = -1;
        {
            std::scoped_lock lock(m_mutex);
            for ( const auto& connection : m_connections ) {
                if ( connection->remotePeerId == recipientId &&
                     connection->dataChannelId >= 0 &&
                     connection->connectedEventSent ) {
                    dataChannelId = connection->dataChannelId;
                    break;
                }
            }
        }
        if ( dataChannelId < 0 || !rtcIsOpen(dataChannelId) ) {
            return false;
        }
        return rtcSendMessage(dataChannelId,
                              reinterpret_cast<const char*>(payload.data()),
                              static_cast<int>(payload.size())) ==
               RTC_ERR_SUCCESS;
    }

    /// @brief 非阻塞读取协作协议帧。
    bool receive(TransportPacket& packet)
    {
        std::scoped_lock lock(m_mutex);
        if ( m_incomingPackets.empty() ) return false;
        packet = std::move(m_incomingPackets.front());
        m_incomingPackets.pop_front();
        return true;
    }

    /// @brief 非阻塞读取连接生命周期事件。
    bool receiveEvent(WebRtcTransportEvent& event)
    {
        std::scoped_lock lock(m_mutex);
        if ( m_events.empty() ) return false;
        event = std::move(m_events.front());
        m_events.pop_front();
        return true;
    }

private:
    /// @brief 启动失败时清理已写入的状态。
    void resetStartFailure()
    {
        std::scoped_lock lock(m_mutex);
        m_isHost      = false;
        m_localPeerId = 0;
        m_hostId      = 0;
        m_creator.clear();
        m_roomCode.clear();
    }

    /// @brief 将连接事件压入线程安全队列。
    void pushEvent(WebRtcTransportEventType type, PeerId peerId,
                   std::string creator, std::string detail)
    {
        std::scoped_lock lock(m_mutex);
        if ( m_stopping ) return;
        if ( m_events.size() >= MAX_QUEUED_TRANSPORT_EVENTS ) {
            m_events.pop_front();
        }
        m_events.push_back(
            { type, peerId, std::move(creator), std::move(detail) });
    }

    /// @brief 发送一条 JSON 信令消息。
    bool sendSignal(Connection& connection, const nlohmann::json& message)
    {
        const std::string payload = message.dump();
        if ( connection.websocketId < 0 ||
             payload.size() >
                 static_cast<std::size_t>(MAX_SIGNALING_MESSAGE_BYTES) ) {
            return false;
        }
        return rtcSendMessage(connection.websocketId, payload.c_str(), -1) ==
               RTC_ERR_SUCCESS;
    }

    /// @brief 创建并配置一个 WebRTC PeerConnection。
    bool createPeerConnection(Connection& connection, bool createDataChannel)
    {
        rtcConfiguration configuration{};
        configuration.maxMessageSize = MAX_DATA_CHANNEL_MESSAGE_BYTES;
        const int peerConnectionId   = rtcCreatePeerConnection(&configuration);
        if ( peerConnectionId < 0 ) {
            return false;
        }

        connection.peerConnectionId = peerConnectionId;
        rtcSetUserPointer(peerConnectionId, &connection);
        rtcSetLocalDescriptionCallback(peerConnectionId,
                                       &Impl::onLocalDescription);
        rtcSetLocalCandidateCallback(peerConnectionId, &Impl::onLocalCandidate);
        rtcSetStateChangeCallback(peerConnectionId,
                                  &Impl::onPeerConnectionState);
        rtcSetDataChannelCallback(peerConnectionId, &Impl::onDataChannel);

        if ( !createDataChannel ) return true;

        rtcDataChannelInit channelConfig{};
        channelConfig.reliability.unordered  = false;
        channelConfig.reliability.unreliable = false;
        channelConfig.protocol               = "mmm-collaboration";
        const int dataChannelId              = rtcCreateDataChannelEx(
            peerConnectionId,
            std::string(COLLABORATION_CHANNEL_LABEL).c_str(),
            &channelConfig);
        if ( dataChannelId < 0 ) {
            rtcDeletePeerConnection(peerConnectionId);
            connection.peerConnectionId = -1;
            return false;
        }
        configureDataChannel(connection, dataChannelId);
        return true;
    }

    /// @brief 配置 DataChannel 的可靠消息回调。
    void configureDataChannel(Connection& connection, int dataChannelId)
    {
        connection.dataChannelId = dataChannelId;
        rtcSetUserPointer(dataChannelId, &connection);
        rtcSetOpenCallback(dataChannelId, &Impl::onDataChannelOpen);
        rtcSetMessageCallback(dataChannelId, &Impl::onDataChannelMessage);
        rtcSetClosedCallback(dataChannelId, &Impl::onDataChannelClosed);
        rtcSetErrorCallback(dataChannelId, &Impl::onDataChannelError);
    }

    /// @brief 为新访客分配当前房间内未使用的 PeerId。
    PeerId allocatePeerId() const
    {
        for ( PeerId candidate = m_hostId + 1;
              candidate <= static_cast<PeerId>(m_maxParticipants);
              ++candidate ) {
            const bool occupied =
                std::any_of(m_connections.begin(),
                            m_connections.end(),
                            [candidate](const auto& connection) {
                                return connection->joined &&
                                       connection->remotePeerId == candidate;
                            });
            if ( !occupied ) return candidate;
        }
        return 0;
    }

    /// @brief 处理访客的初始加入信令。
    void handleJoin(Connection& connection, const nlohmann::json& message)
    {
        std::string   roomCode;
        std::string   creator;
        std::uint64_t version = 0;
        if ( !readUnsignedField(message, "version", version) ||
             version != SIGNALING_PROTOCOL_VERSION ||
             !readStringField(message, "roomCode", roomCode) ||
             !readStringField(message, "creator", creator) ) {
            reject(connection, "invalid_join");
            return;
        }
        roomCode = normalizeRoomCode(roomCode);
        creator  = Config::normalizeCreatorIdentity(creator);
        if ( roomCode != m_roomCode || creator.empty() ) {
            reject(connection,
                   roomCode != m_roomCode ? "room_code_mismatch"
                                          : "invalid_creator");
            return;
        }

        PeerId assignedPeerId = 0;
        {
            std::scoped_lock lock(m_mutex);
            if ( connection.joined ) return;
            assignedPeerId = allocatePeerId();
            if ( assignedPeerId != 0 ) {
                connection.joined        = true;
                connection.remotePeerId  = assignedPeerId;
                connection.remoteCreator = creator;
            }
        }
        if ( assignedPeerId == 0 ) {
            reject(connection, "room_full");
            return;
        }

        nlohmann::json accepted;
        accepted["type"]        = "accepted";
        accepted["version"]     = SIGNALING_PROTOCOL_VERSION;
        accepted["peerId"]      = assignedPeerId;
        accepted["hostId"]      = m_hostId;
        accepted["hostCreator"] = m_creator;
        if ( !createPeerConnection(connection, false) ||
             !sendSignal(connection, accepted) ) {
            pushEvent(WebRtcTransportEventType::Error,
                      assignedPeerId,
                      creator,
                      "peer_connection_create_failed");
        }
    }

    /// @brief 处理房主接受访客后的配置消息。
    void handleAccepted(Connection& connection, const nlohmann::json& message)
    {
        std::uint64_t peerId = 0;
        std::uint64_t hostId = 0;
        std::string   hostCreator;
        if ( !readUnsignedField(message, "peerId", peerId) || peerId == 0 ||
             !readUnsignedField(message, "hostId", hostId) || hostId == 0 ||
             !readStringField(message, "hostCreator", hostCreator) ) {
            pushEvent(WebRtcTransportEventType::Error,
                      0,
                      {},
                      "invalid_accept_message");
            return;
        }
        hostCreator = Config::normalizeCreatorIdentity(hostCreator);
        if ( hostCreator.empty() || hostId != m_hostId ) {
            pushEvent(WebRtcTransportEventType::Error,
                      0,
                      {},
                      "invalid_host_identity");
            return;
        }

        {
            std::scoped_lock lock(m_mutex);
            if ( m_localPeerId != 0 ) return;
            m_localPeerId            = static_cast<PeerId>(peerId);
            connection.remotePeerId  = static_cast<PeerId>(hostId);
            connection.remoteCreator = hostCreator;
            connection.joined        = true;
        }
        if ( !createPeerConnection(connection, true) ) {
            pushEvent(WebRtcTransportEventType::Error,
                      static_cast<PeerId>(hostId),
                      hostCreator,
                      "peer_connection_create_failed");
        }
    }

    /// @brief 处理 SDP 或 ICE candidate 信令。
    void handleNegotiation(Connection&           connection,
                           const nlohmann::json& message, std::string_view type)
    {
        if ( connection.peerConnectionId < 0 ) {
            pushEvent(WebRtcTransportEventType::Error,
                      connection.remotePeerId,
                      connection.remoteCreator,
                      "negotiation_before_peer_connection");
            return;
        }

        if ( type == "description" ) {
            std::string sdp;
            std::string descriptionType;
            if ( !readStringField(message, "sdp", sdp) ||
                 !readStringField(
                     message, "descriptionType", descriptionType) ||
                 rtcSetRemoteDescription(connection.peerConnectionId,
                                         sdp.c_str(),
                                         descriptionType.c_str()) !=
                     RTC_ERR_SUCCESS ) {
                pushEvent(WebRtcTransportEventType::Error,
                          connection.remotePeerId,
                          connection.remoteCreator,
                          "remote_description_failed");
            }
            return;
        }

        std::string candidate;
        std::string mid;
        if ( !readStringField(message, "candidate", candidate) ||
             !readStringField(message, "mid", mid) ||
             rtcAddRemoteCandidate(connection.peerConnectionId,
                                   candidate.c_str(),
                                   mid.c_str()) != RTC_ERR_SUCCESS ) {
            pushEvent(WebRtcTransportEventType::Error,
                      connection.remotePeerId,
                      connection.remoteCreator,
                      "remote_candidate_failed");
        }
    }

    /// @brief 拒绝一个信令连接。
    void reject(Connection& connection, std::string reason)
    {
        nlohmann::json rejected;
        rejected["type"]    = "rejected";
        rejected["version"] = SIGNALING_PROTOCOL_VERSION;
        rejected["reason"]  = reason;
        static_cast<void>(sendSignal(connection, rejected));
        pushEvent(WebRtcTransportEventType::Rejected,
                  connection.remotePeerId,
                  connection.remoteCreator,
                  std::move(reason));
    }

    /// @brief 处理一条已经解析的 WebSocket 信令消息。
    void handleSignalingMessage(Connection&           connection,
                                const nlohmann::json& message)
    {
        if ( !message.is_object() ) return;
        std::string type;
        if ( !readStringField(message, "type", type) ) return;

        if ( m_isHost && type == "join" ) {
            handleJoin(connection, message);
        } else if ( !m_isHost && type == "accepted" ) {
            handleAccepted(connection, message);
        } else if ( !m_isHost && type == "rejected" ) {
            std::string reason;
            static_cast<void>(readStringField(message, "reason", reason));
            pushEvent(WebRtcTransportEventType::Rejected,
                      0,
                      {},
                      reason.empty() ? "rejected" : reason);
        } else if ( type == "description" || type == "candidate" ) {
            handleNegotiation(connection, message, type);
        }
    }

    /// @brief WebSocketServer 接收客户端连接的回调。
    static void onWebSocketClient(int, int websocketId, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        if ( !owner ) {
            rtcDeleteWebSocket(websocketId);
            return;
        }

        auto connection           = std::make_unique<Connection>();
        connection->owner         = owner;
        connection->websocketId   = websocketId;
        Connection* connectionPtr = connection.get();
        {
            std::scoped_lock lock(owner->m_mutex);
            if ( owner->m_stopping || !owner->m_running ) {
                rtcDeleteWebSocket(websocketId);
                return;
            }
            owner->m_connections.push_back(std::move(connection));
        }

        rtcSetUserPointer(websocketId, connectionPtr);
        rtcSetMessageCallback(websocketId, &Impl::onWebSocketMessage);
        rtcSetClosedCallback(websocketId, &Impl::onWebSocketClosed);
        rtcSetErrorCallback(websocketId, &Impl::onWebSocketError);
        owner->pushEvent(WebRtcTransportEventType::SignalingConnected,
                         0,
                         {},
                         "incoming_signaling_connection");
    }

    /// @brief 访客 WebSocket 建立后的加入请求回调。
    static void onGuestWebSocketOpen(int, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        Impl& owner = *connection->owner;

        nlohmann::json join;
        join["type"]     = "join";
        join["version"]  = SIGNALING_PROTOCOL_VERSION;
        join["roomCode"] = owner.m_roomCode;
        join["creator"]  = owner.m_creator;
        if ( !owner.sendSignal(*connection, join) ) {
            owner.pushEvent(WebRtcTransportEventType::Error,
                            owner.m_hostId,
                            {},
                            "join_signal_send_failed");
            return;
        }
        owner.pushEvent(WebRtcTransportEventType::SignalingConnected,
                        owner.m_hostId,
                        {},
                        "signaling_connected");
    }

    /// @brief 解析 WebSocket 文本信令。
    static void onWebSocketMessage(int, const char* message, int size,
                                   void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner || !message || size >= 0 ) {
            return;
        }
        const auto parsed = nlohmann::json::parse(message, nullptr, false);
        if ( parsed.is_discarded() ) {
            connection->owner->pushEvent(WebRtcTransportEventType::Error,
                                         connection->remotePeerId,
                                         connection->remoteCreator,
                                         "invalid_signaling_json");
            return;
        }
        connection->owner->handleSignalingMessage(*connection, parsed);
    }

    /// @brief WebSocket 关闭回调。
    static void onWebSocketClosed(int, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        connection->owner->notifyDisconnected(*connection, "signaling_closed");
    }

    /// @brief WebSocket 错误回调。
    static void onWebSocketError(int, const char* error, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        connection->owner->pushEvent(WebRtcTransportEventType::Error,
                                     connection->remotePeerId,
                                     connection->remoteCreator,
                                     error ? error : "websocket_error");
    }

    /// @brief 本地 SDP 生成回调。
    static void onLocalDescription(int, const char* sdp, const char* type,
                                   void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner || !sdp || !type ) return;
        nlohmann::json description;
        description["type"]            = "description";
        description["sdp"]             = sdp;
        description["descriptionType"] = type;
        if ( !connection->owner->sendSignal(*connection, description) ) {
            connection->owner->pushEvent(WebRtcTransportEventType::Error,
                                         connection->remotePeerId,
                                         connection->remoteCreator,
                                         "local_description_send_failed");
        }
    }

    /// @brief 本地 ICE candidate 生成回调。
    static void onLocalCandidate(int, const char* candidate, const char* mid,
                                 void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner || !candidate || !mid ) return;
        nlohmann::json candidateMessage;
        candidateMessage["type"]      = "candidate";
        candidateMessage["candidate"] = candidate;
        candidateMessage["mid"]       = mid;
        if ( !connection->owner->sendSignal(*connection, candidateMessage) ) {
            connection->owner->pushEvent(WebRtcTransportEventType::Error,
                                         connection->remotePeerId,
                                         connection->remoteCreator,
                                         "local_candidate_send_failed");
        }
    }

    /// @brief PeerConnection 状态变化回调。
    static void onPeerConnectionState(int, rtcState state, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        if ( state == RTC_FAILED ) {
            connection->owner->pushEvent(WebRtcTransportEventType::Error,
                                         connection->remotePeerId,
                                         connection->remoteCreator,
                                         "peer_connection_failed");
        } else if ( state == RTC_DISCONNECTED || state == RTC_CLOSED ) {
            connection->owner->notifyDisconnected(*connection,
                                                  "peer_connection_closed");
        }
    }

    /// @brief 房主收到访客 DataChannel 的回调。
    static void onDataChannel(int, int dataChannelId, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) {
            rtcDeleteDataChannel(dataChannelId);
            return;
        }
        std::array<char, 64> label{};
        const int            labelLength = rtcGetDataChannelLabel(
            dataChannelId, label.data(), static_cast<int>(label.size()));
        if ( labelLength <= 0 ||
             std::string_view(label.data()) != COLLABORATION_CHANNEL_LABEL ) {
            rtcDeleteDataChannel(dataChannelId);
            connection->owner->pushEvent(WebRtcTransportEventType::Error,
                                         connection->remotePeerId,
                                         connection->remoteCreator,
                                         "unexpected_data_channel");
            return;
        }
        connection->owner->configureDataChannel(*connection, dataChannelId);
    }

    /// @brief DataChannel 打开回调。
    static void onDataChannelOpen(int, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        Impl& owner = *connection->owner;
        {
            std::scoped_lock lock(owner.m_mutex);
            if ( connection->connectedEventSent || owner.m_stopping ) return;
            connection->connectedEventSent = true;
        }
        owner.pushEvent(WebRtcTransportEventType::PeerConnected,
                        connection->remotePeerId,
                        connection->remoteCreator,
                        "data_channel_open");
    }

    /// @brief DataChannel 二进制消息回调。
    static void onDataChannelMessage(int, const char* message, int size,
                                     void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner || !message || size <= 0 ||
             size > MAX_DATA_CHANNEL_MESSAGE_BYTES ) {
            return;
        }
        TransportPacket packet;
        packet.senderId = connection->remotePeerId;
        packet.payload.assign(
            reinterpret_cast<const std::uint8_t*>(message),
            reinterpret_cast<const std::uint8_t*>(message) + size);
        std::scoped_lock lock(connection->owner->m_mutex);
        if ( connection->owner->m_stopping || !connection->joined ||
             !connection->connectedEventSent ||
             connection->owner->m_incomingPackets.size() >=
                 MAX_QUEUED_DATA_CHANNEL_PACKETS ) {
            return;
        }
        connection->owner->m_incomingPackets.push_back(std::move(packet));
    }

    /// @brief DataChannel 关闭回调。
    static void onDataChannelClosed(int, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        connection->owner->notifyDisconnected(*connection,
                                              "data_channel_closed");
    }

    /// @brief DataChannel 错误回调。
    static void onDataChannelError(int, const char* error, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        connection->owner->pushEvent(WebRtcTransportEventType::Error,
                                     connection->remotePeerId,
                                     connection->remoteCreator,
                                     error ? error : "data_channel_error");
    }

    /// @brief 保证每个连接只上报一次离开事件。
    void notifyDisconnected(Connection& connection, std::string detail)
    {
        {
            std::scoped_lock lock(m_mutex);
            if ( connection.disconnectedEventSent || m_stopping ) return;
            connection.disconnectedEventSent = true;
            connection.connectedEventSent    = false;
            connection.joined                = false;
        }
        pushEvent(WebRtcTransportEventType::PeerDisconnected,
                  connection.remotePeerId,
                  connection.remoteCreator,
                  std::move(detail));
    }

    /// @brief 保护跨 libdatachannel 回调线程共享的队列和连接表。
    mutable std::mutex m_mutex;
    /// @brief 传输是否已启动。
    bool m_running = false;
    /// @brief 当前是否正在析构网络句柄。
    bool m_stopping = false;
    /// @brief 当前角色是否为房主。
    bool m_isHost = false;
    /// @brief 当前 Creator。
    std::string m_creator;
    /// @brief 当前房间码。
    std::string m_roomCode;
    /// @brief 当前本地 PeerId。
    PeerId m_localPeerId = 0;
    /// @brief 房主 PeerId。
    PeerId m_hostId = 0;
    /// @brief 房间总人数上限。
    std::size_t m_maxParticipants = MAX_COLLABORATION_PARTICIPANTS;
    /// @brief WebSocketServer 句柄。
    int m_webSocketServerId = -1;
    /// @brief 房主实际监听端口。
    std::uint16_t m_listeningPort = 0;
    /// @brief 所有房主访客连接或访客到房主的唯一连接。
    std::vector<std::unique_ptr<Connection>> m_connections;
    /// @brief DataChannel 收到的完整协作协议帧。
    std::deque<TransportPacket> m_incomingPackets;
    /// @brief 等待产品层消费的连接生命周期事件。
    std::deque<WebRtcTransportEvent> m_events;
};

WebRtcTransport::WebRtcTransport() : m_impl(std::make_unique<Impl>()) {}

WebRtcTransport::~WebRtcTransport() = default;

bool WebRtcTransport::startHost(const WebRtcHostConfig& config)
{
    return m_impl->startHost(config);
}

bool WebRtcTransport::connectToHost(const WebRtcGuestConfig& config)
{
    return m_impl->connectToHost(config);
}

void WebRtcTransport::stop()
{
    m_impl->stop();
}

bool WebRtcTransport::isRunning() const
{
    return m_impl->isRunning();
}

bool WebRtcTransport::isHost() const
{
    return m_impl->isHost();
}

PeerId WebRtcTransport::localPeerId() const
{
    return m_impl->localPeerId();
}

std::uint16_t WebRtcTransport::listeningPort() const
{
    return m_impl->listeningPort();
}

bool WebRtcTransport::receiveEvent(WebRtcTransportEvent& event)
{
    return m_impl->receiveEvent(event);
}

bool WebRtcTransport::send(PeerId                        recipientId,
                           std::span<const std::uint8_t> payload)
{
    return m_impl->send(recipientId, payload);
}

bool WebRtcTransport::receive(TransportPacket& packet)
{
    return m_impl->receive(packet);
}
}  // namespace MMM::Network::Collaboration
