#include "network/collaboration/WebRtcTransport.h"

#include "config/CreatorIdentity.h"
#include "network/collaboration/CollaborationBuildFingerprint.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <climits>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::Network::Collaboration
{
namespace
{
/// @brief 中心目录和配对服务使用的协议版本。
constexpr std::uint64_t BROKER_PROTOCOL_VERSION = 1;
/// @brief 中心完成配对后两端直接交换身份使用的协议版本。
constexpr std::uint64_t P2P_SIGNALING_PROTOCOL_VERSION = 2;
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
/// @brief 中心服务允许下发的 ICE URI 数量。
constexpr std::size_t MAX_ICE_SERVERS = 8;
/// @brief 房主控制令牌使用的无歧义字符表。
constexpr std::string_view TOKEN_ALPHABET =
    "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/// @brief 规范化可在目录公开展示的房间名称。
std::string normalizeRoomName(std::string_view value)
{
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.front())) != 0 ) {
        value.remove_prefix(1);
    }
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.back())) != 0 ) {
        value.remove_suffix(1);
    }
    if ( value.empty() || value.size() > 128U ) return {};
    if ( std::any_of(value.begin(), value.end(), [](char character) {
             const auto byte = static_cast<unsigned char>(character);
             return byte < 0x20U || byte == 0x7FU;
         }) ) {
        return {};
    }
    return std::string(value);
}

/// @brief 校验中心服务分配的房间标识。
bool isValidRoomId(std::string_view value)
{
    return !value.empty() && value.size() <= 64U &&
           std::all_of(value.begin(), value.end(), [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return std::isalnum(byte) != 0 || character == '-' ||
                      character == '_';
           });
}

/// @brief 从 JSON 对象读取字符串字段。
bool readStringField(const nlohmann::json& object, std::string_view key,
                     std::string& value)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_string() ) return false;
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

/// @brief 从 JSON 对象读取布尔字段。
bool readBooleanField(const nlohmann::json& object, std::string_view key,
                      bool& value)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_boolean() ) return false;
    value = iterator->get<bool>();
    return true;
}

/// @brief 生成仅房主控制连接持有的随机令牌。
std::string generateOwnerToken()
{
    std::mt19937_64                            random{ std::random_device{}() };
    std::uniform_int_distribution<std::size_t> distribution(
        0, TOKEN_ALPHABET.size() - 1U);
    std::string token(48U, '0');
    for ( char& character : token ) {
        character = TOKEN_ALPHABET[distribution(random)];
    }
    return token;
}
}  // namespace

class WebRtcTransport::Impl
{
public:
    /// @brief 中心 WebSocket 在当前传输中的职责。
    enum class ConnectionRole {
        HostControl,
        Peer,
    };

    /// @brief 一个访客与房主之间的临时信令和长期 P2P 上下文。
    struct Connection {
        /// @brief 所属传输实现的稳定观察指针。
        Impl* owner = nullptr;
        /// @brief 中心连接职责。
        ConnectionRole role = ConnectionRole::Peer;
        /// @brief 信令 WebSocket 标识。
        int websocketId = -1;
        /// @brief 防止打开回调与注册后的状态补检重复声明连接角色。
        std::atomic_bool openHandled{ false };
        /// @brief WebRTC PeerConnection 标识。
        int peerConnectionId = -1;
        /// @brief 可靠有序 DataChannel 标识。
        int dataChannelId = -1;
        /// @brief 远端 PeerId。
        PeerId remotePeerId = 0;
        /// @brief 远端 Creator。
        std::string remoteCreator;
        /// @brief 远端不随路由槽位复用变化的稳定协作者标识。
        ParticipantId remoteParticipantId;
        /// @brief 远端本次加入流程的操作会话标识。
        OperationSessionId remoteSessionId;
        /// @brief 服务端加入请求标识；仅房主的访客通道使用。
        std::string requestId;
        /// @brief 房主批准加入时由中心服务确认的访客 Creator。
        std::string approvedCreator;
        /// @brief 是否已经完成 P2P 身份握手。
        bool joined = false;
        /// @brief 是否已经上报 DataChannel 建立事件。
        bool connectedEventSent = false;
        /// @brief 对端是否已经确认 DataChannel 打开。
        bool remoteDataChannelReady = false;
        /// @brief 是否已经上报离开事件。
        bool disconnectedEventSent = false;
        /// @brief
        /// 是否已经把中心服务拒绝作为终止事件上报，避免关闭回调覆盖原因。
        bool signalingTerminalEventSent = false;
    };

    /// @brief 从连接状态中原子退役、随后在锁外删除的一组 RTC 句柄。
    struct RetiredHandles {
        /// @brief DataChannel 句柄。
        int dataChannelId = -1;
        /// @brief PeerConnection 句柄。
        int peerConnectionId = -1;
        /// @brief WebSocket 句柄。
        int websocketId = -1;
    };

    /// @brief 停止并释放所有 libdatachannel 句柄。
    ~Impl() { stop(); }

    /// @brief 连接中心服务并发布房间。
    bool startHost(const WebRtcHostConfig& config)
    {
        const auto creator  = Config::normalizeCreatorIdentity(config.creator);
        const auto roomName = normalizeRoomName(config.roomName);
        const auto participantId =
            Config::normalizeCollaborationStableId(config.participantId);
        const auto sessionId =
            Config::normalizeCollaborationStableId(config.sessionId);
        const auto& buildFingerprint = config.buildFingerprint.empty()
                                           ? collaborationBuildFingerprint()
                                           : config.buildFingerprint;
        if ( creator.empty() || roomName.empty() || config.hostId == 0 ||
             participantId.empty() || sessionId.empty() ||
             !isValidCollaborationBuildFingerprint(buildFingerprint) ||
             makeCollaborationSignalingUrl(config.endpoint).empty() ) {
            return false;
        }

        auto control        = std::make_unique<Connection>();
        control->owner      = this;
        control->role       = ConnectionRole::HostControl;
        Connection* pointer = control.get();
        bool        opened  = false;
        {
            std::scoped_lock rtcLock(m_rtcApiMutex);
            {
                std::scoped_lock lock(m_mutex);
                if ( m_running || m_stopping ) return false;
                m_isHost           = true;
                m_creator          = creator;
                m_participantId    = participantId;
                m_sessionId        = sessionId;
                m_buildFingerprint = buildFingerprint;
                m_requireMatchingBuildFingerprint =
                    config.requireMatchingBuildFingerprint;
                m_roomName     = roomName;
                m_signalingUrl = makeCollaborationSignalingUrl(config.endpoint);
                m_ownerToken   = generateOwnerToken();
                m_localPeerId  = config.hostId;
                m_hostId       = config.hostId;
                m_maxParticipants = std::clamp(config.maxParticipants,
                                               MIN_COLLABORATION_PARTICIPANTS,
                                               MAX_COLLABORATION_PARTICIPANTS);
                m_stopping        = false;
                m_running         = true;
                m_connections.push_back(std::move(control));
            }
            opened = openWebSocket(*pointer);
        }
        if ( !opened ) {
            stop();
            return false;
        }
        return true;
    }

    /// @brief 连接中心服务并请求加入公开房间。
    bool connectToHost(const WebRtcGuestConfig& config)
    {
        const auto creator = Config::normalizeCreatorIdentity(config.creator);
        const auto participantId =
            Config::normalizeCollaborationStableId(config.participantId);
        const auto sessionId =
            Config::normalizeCollaborationStableId(config.sessionId);
        const auto& buildFingerprint = config.buildFingerprint.empty()
                                           ? collaborationBuildFingerprint()
                                           : config.buildFingerprint;
        if ( creator.empty() || config.hostId == 0 || participantId.empty() ||
             sessionId.empty() ||
             !isValidCollaborationBuildFingerprint(buildFingerprint) ||
             !isValidRoomId(config.roomId) ||
             makeCollaborationSignalingUrl(config.endpoint).empty() ) {
            return false;
        }

        auto connection           = std::make_unique<Connection>();
        connection->owner         = this;
        connection->role          = ConnectionRole::Peer;
        connection->remotePeerId  = config.hostId;
        Connection* connectionPtr = connection.get();
        bool        opened        = false;
        {
            std::scoped_lock rtcLock(m_rtcApiMutex);
            {
                std::scoped_lock lock(m_mutex);
                if ( m_running || m_stopping ) return false;
                m_isHost                          = false;
                m_creator                         = creator;
                m_participantId                   = participantId;
                m_sessionId                       = sessionId;
                m_buildFingerprint                = buildFingerprint;
                m_requireMatchingBuildFingerprint = true;
                m_signalingUrl = makeCollaborationSignalingUrl(config.endpoint);
                m_roomId       = config.roomId;
                m_localPeerId  = 0;
                m_hostId       = config.hostId;
                m_stopping     = false;
                m_running      = true;
                m_connections.push_back(std::move(connection));
            }
            opened = openWebSocket(*connectionPtr);
        }
        if ( !opened ) {
            stop();
            return false;
        }
        return true;
    }

    /// @brief 批准一个由中心服务暂存的访客加入请求。
    bool approveJoinRequest(std::string_view requestId)
    {
        std::string request;
        std::string creator;
        {
            std::scoped_lock lock(m_mutex);
            if ( !m_isHost || m_stopping || requestId.empty() ) return false;
            const auto iterator =
                m_pendingJoinRequests.find(std::string(requestId));
            if ( iterator == m_pendingJoinRequests.end() ) return false;
            request = iterator->first;
            creator = iterator->second;
        }
        if ( !openHostPeerConnection(request, creator) ) return false;
        std::scoped_lock lock(m_mutex);
        m_pendingJoinRequests.erase(request);
        return true;
    }

    /// @brief 拒绝一个由中心服务暂存的访客加入请求。
    bool rejectJoinRequest(std::string_view requestId, std::string_view reason)
    {
        std::scoped_lock rtcLock(m_rtcApiMutex);
        Connection*      control = nullptr;
        std::string      request;
        std::string      roomId;
        std::string      ownerToken;
        {
            std::scoped_lock lock(m_mutex);
            if ( !m_isHost || m_stopping || requestId.empty() ) return false;
            const auto iterator =
                m_pendingJoinRequests.find(std::string(requestId));
            if ( iterator == m_pendingJoinRequests.end() ) return false;
            request    = iterator->first;
            roomId     = m_roomId;
            ownerToken = m_ownerToken;
            for ( const auto& connection : m_connections ) {
                if ( connection->role == ConnectionRole::HostControl ) {
                    control = connection.get();
                    break;
                }
            }
        }
        if ( !control ) return false;
        const nlohmann::json rejected = {
            { "type", "reject_join" },
            { "version", BROKER_PROTOCOL_VERSION },
            { "roomId", roomId },
            { "requestId", request },
            { "ownerToken", ownerToken },
            { "reason", reason },
        };
        if ( !sendSignal(*control, rejected) ) return false;
        std::scoped_lock lock(m_mutex);
        m_pendingJoinRequests.erase(request);
        return true;
    }

    /// @brief 主动关闭一个已经建立的访客 P2P 连接。
    bool disconnectPeer(PeerId peerId, std::string detail)
    {
        std::scoped_lock rtcLock(m_rtcApiMutex);
        Connection*      connection       = nullptr;
        int              dataChannelId    = -1;
        int              peerConnectionId = -1;
        {
            std::scoped_lock lock(m_mutex);
            if ( !m_isHost || m_stopping || peerId == 0 ||
                 peerId == m_localPeerId ) {
                return false;
            }
            for ( const auto& candidate : m_connections ) {
                if ( candidate->role == ConnectionRole::Peer &&
                     candidate->remotePeerId == peerId &&
                     candidate->connectedEventSent &&
                     !candidate->disconnectedEventSent ) {
                    connection       = candidate.get();
                    dataChannelId    = candidate->dataChannelId;
                    peerConnectionId = candidate->peerConnectionId;
                    break;
                }
            }
        }
        if ( !connection ) return false;
        notifyDisconnected(
            *connection,
            detail.empty() ? "removed_by_host" : std::move(detail));
        if ( dataChannelId >= 0 ) {
            static_cast<void>(rtcClose(dataChannelId));
        }
        if ( peerConnectionId >= 0 ) {
            static_cast<void>(rtcClose(peerConnectionId));
        }
        return true;
    }

    /// @brief 释放通道、PeerConnection 和中心 WebSocket。
    void stop()
    {
        std::vector<std::unique_ptr<Connection>> connections;
        std::vector<RetiredHandles>              retiredHandles;
        {
            std::scoped_lock rtcLock(m_rtcApiMutex);
            {
                std::scoped_lock lock(m_mutex);
                if ( !m_running && m_connections.empty() ) return;
                m_stopping  = true;
                m_running   = false;
                connections = std::move(m_connections);
                m_incomingPackets.clear();
                m_events.clear();
                m_pendingJoinRequests.clear();
            }

            retiredHandles.reserve(connections.size());
            for ( auto& connection : connections ) {
                RetiredHandles handles{ connection->dataChannelId,
                                        connection->peerConnectionId,
                                        connection->websocketId };
                connection->dataChannelId    = -1;
                connection->peerConnectionId = -1;
                connection->websocketId      = -1;
                if ( handles.dataChannelId >= 0 ) {
                    detachChannelCallbacks(handles.dataChannelId);
                }
                if ( handles.peerConnectionId >= 0 ) {
                    detachPeerConnectionCallbacks(handles.peerConnectionId);
                }
                if ( handles.websocketId >= 0 ) {
                    detachChannelCallbacks(handles.websocketId);
                }
                retiredHandles.push_back(handles);
            }
        }

        for ( const auto& handles : retiredHandles ) {
            if ( handles.dataChannelId >= 0 ) {
                rtcDeleteDataChannel(handles.dataChannelId);
            }
            if ( handles.peerConnectionId >= 0 ) {
                rtcDeletePeerConnection(handles.peerConnectionId);
            }
            if ( handles.websocketId >= 0 ) {
                rtcDeleteWebSocket(handles.websocketId);
            }
        }

        std::scoped_lock lock(m_mutex);
        m_stopping    = false;
        m_localPeerId = 0;
        m_participantId.clear();
        m_sessionId.clear();
        m_buildFingerprint.clear();
        m_roomId.clear();
        m_roomName.clear();
        m_ownerToken.clear();
        m_iceServers.clear();
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

    /// @brief 获取公开房间标识。
    std::string roomId() const
    {
        std::scoped_lock lock(m_mutex);
        return m_roomId;
    }

    /// @brief 发送协作协议二进制帧。
    bool send(PeerId recipientId, std::span<const std::uint8_t> payload)
    {
        if ( payload.empty() ||
             payload.size() > static_cast<std::size_t>(INT_MAX) ) {
            return false;
        }

        std::scoped_lock rtcLock(m_rtcApiMutex);
        int              dataChannelId = -1;
        {
            std::scoped_lock lock(m_mutex);
            for ( const auto& connection : m_connections ) {
                if ( connection->role == ConnectionRole::Peer &&
                     connection->remotePeerId == recipientId &&
                     connection->dataChannelId >= 0 &&
                     connection->connectedEventSent ) {
                    dataChannelId = connection->dataChannelId;
                    break;
                }
            }
        }
        if ( dataChannelId < 0 || !rtcIsOpen(dataChannelId) ) return false;
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
    /// @brief 在句柄删除前切断 DataChannel 或 WebSocket 的全部回调。
    static void detachChannelCallbacks(int channelId)
    {
        rtcSetOpenCallback(channelId, nullptr);
        rtcSetMessageCallback(channelId, nullptr);
        rtcSetClosedCallback(channelId, nullptr);
        rtcSetErrorCallback(channelId, nullptr);
        rtcSetUserPointer(channelId, nullptr);
    }

    /// @brief 在句柄删除前切断 PeerConnection 的全部回调。
    static void detachPeerConnectionCallbacks(int peerConnectionId)
    {
        rtcSetLocalDescriptionCallback(peerConnectionId, nullptr);
        rtcSetLocalCandidateCallback(peerConnectionId, nullptr);
        rtcSetStateChangeCallback(peerConnectionId, nullptr);
        rtcSetDataChannelCallback(peerConnectionId, nullptr);
        rtcSetUserPointer(peerConnectionId, nullptr);
    }

    /// @brief 创建并配置一个中心 WebSocket。
    /// @warning libdatachannel C API 尚未暴露 CA 注入，当前 mbedTLS
    /// 预编译库无法建立系统信任链，因此 WSS 暂时只提供传输加密。
    /// @warning 调用方必须持有 m_rtcApiMutex，保证句柄注册期间连接不会退役。
    bool openWebSocket(Connection& connection)
    {
        rtcWsConfiguration config{};
        config.disableTlsVerification = m_signalingUrl.starts_with("wss://");
        config.connectionTimeoutMs    = 10000;
        config.pingIntervalMs         = 5000;
        config.maxOutstandingPings    = 3;
        config.maxMessageSize         = MAX_SIGNALING_MESSAGE_BYTES;
        const int websocketId =
            rtcCreateWebSocketEx(m_signalingUrl.c_str(), &config);
        if ( websocketId < 0 ) return false;
        connection.websocketId = websocketId;
        rtcSetUserPointer(websocketId, &connection);
        rtcSetOpenCallback(websocketId, &Impl::onWebSocketOpen);
        rtcSetMessageCallback(websocketId, &Impl::onWebSocketMessage);
        rtcSetClosedCallback(websocketId, &Impl::onWebSocketClosed);
        rtcSetErrorCallback(websocketId, &Impl::onWebSocketError);
        if ( rtcIsOpen(websocketId) ) {
            onWebSocketOpen(websocketId, &connection);
        }
        return true;
    }

    /// @brief 创建房主接受单个访客所需的瞬时信令连接。
    bool openHostPeerConnection(std::string requestId,
                                std::string approvedCreator)
    {
        auto connection             = std::make_unique<Connection>();
        connection->owner           = this;
        connection->role            = ConnectionRole::Peer;
        connection->requestId       = std::move(requestId);
        connection->approvedCreator = std::move(approvedCreator);
        Connection* pointer         = connection.get();
        bool        opened          = false;
        {
            std::scoped_lock rtcLock(m_rtcApiMutex);
            {
                std::scoped_lock lock(m_mutex);
                if ( m_stopping || !m_running ) return false;
                m_connections.push_back(std::move(connection));
            }
            opened = openWebSocket(*pointer);
        }
        if ( !opened ) {
            pushEvent(WebRtcTransportEventType::Error,
                      0,
                      {},
                      "peer_signaling_connect_start_failed");
            return false;
        }
        return true;
    }

    /// @brief 将连接事件压入线程安全队列。
    void pushEvent(WebRtcTransportEventType type, PeerId peerId,
                   std::string creator, std::string detail,
                   std::string requestId = {}, ParticipantId participantId = {},
                   OperationSessionId sessionId        = {},
                   std::string        buildFingerprint = {})
    {
        std::scoped_lock lock(m_mutex);
        if ( m_stopping ) return;
        if ( m_events.size() >= MAX_QUEUED_TRANSPORT_EVENTS ) {
            m_events.pop_front();
        }
        m_events.push_back({ type,
                             peerId,
                             std::move(participantId),
                             std::move(sessionId),
                             std::move(creator),
                             std::move(detail),
                             std::move(requestId),
                             std::move(buildFingerprint) });
    }

    /// @brief 发送一条 JSON 信令消息。
    bool sendSignal(Connection& connection, const nlohmann::json& message)
    {
        const std::string payload = message.dump();
        if ( payload.size() >
             static_cast<std::size_t>(MAX_SIGNALING_MESSAGE_BYTES) ) {
            return false;
        }
        std::scoped_lock rtcLock(m_rtcApiMutex);
        int              websocketId = -1;
        {
            std::scoped_lock lock(m_mutex);
            if ( m_stopping ) return false;
            websocketId = connection.websocketId;
        }
        if ( websocketId < 0 || !rtcIsOpen(websocketId) ) return false;
        return rtcSendMessage(websocketId, payload.c_str(), -1) ==
               RTC_ERR_SUCCESS;
    }

    /// @brief 校验并保存服务端下发的 ICE URI。
    bool updateIceServers(const nlohmann::json& message)
    {
        const auto iterator = message.find("iceServers");
        if ( iterator == message.end() || !iterator->is_array() ||
             iterator->size() > MAX_ICE_SERVERS ) {
            return false;
        }
        std::vector<std::string> iceServers;
        iceServers.reserve(iterator->size());
        for ( const auto& value : *iterator ) {
            if ( !value.is_string() ) return false;
            const auto& uri = value.get_ref<const std::string&>();
            if ( uri.empty() || uri.size() > 512U ) return false;
            iceServers.push_back(uri);
        }
        std::scoped_lock lock(m_mutex);
        m_iceServers = std::move(iceServers);
        return true;
    }

    /// @brief 创建并配置一个 WebRTC PeerConnection。
    bool createPeerConnection(Connection& connection, bool createDataChannel)
    {
        std::unique_lock         rtcLock(m_rtcApiMutex);
        std::vector<std::string> iceServers;
        {
            std::scoped_lock lock(m_mutex);
            if ( m_stopping || !m_running ) return false;
            iceServers = m_iceServers;
        }
        std::vector<const char*> iceServerPointers;
        iceServerPointers.reserve(iceServers.size());
        for ( const auto& uri : iceServers ) {
            iceServerPointers.push_back(uri.c_str());
        }

        rtcConfiguration configuration{};
        configuration.iceServers =
            iceServerPointers.empty() ? nullptr : iceServerPointers.data();
        configuration.iceServersCount =
            static_cast<int>(iceServerPointers.size());
        configuration.maxMessageSize = MAX_DATA_CHANNEL_MESSAGE_BYTES;
        const int peerConnectionId   = rtcCreatePeerConnection(&configuration);
        if ( peerConnectionId < 0 ) return false;

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
        const std::string label(COLLABORATION_CHANNEL_LABEL);
        const int         dataChannelId = rtcCreateDataChannelEx(
            peerConnectionId, label.c_str(), &channelConfig);
        if ( dataChannelId < 0 ) {
            detachPeerConnectionCallbacks(peerConnectionId);
            connection.peerConnectionId = -1;
            rtcLock.unlock();
            rtcDeletePeerConnection(peerConnectionId);
            return false;
        }
        configureDataChannel(connection, dataChannelId);
        return true;
    }

    /// @brief 配置 DataChannel 的可靠消息回调。
    void configureDataChannel(Connection& connection, int dataChannelId)
    {
        std::scoped_lock rtcLock(m_rtcApiMutex);
        {
            std::scoped_lock lock(m_mutex);
            if ( m_stopping || !m_running ) return;
        }
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
            const bool occupied = std::any_of(
                m_connections.begin(),
                m_connections.end(),
                [candidate](const auto& connection) {
                    return connection->role == ConnectionRole::Peer &&
                           connection->joined &&
                           connection->remotePeerId == candidate;
                });
            if ( !occupied ) return candidate;
        }
        return 0;
    }

    /// @brief 处理访客在透明信令通道上的身份握手。
    void handleJoin(Connection& connection, const nlohmann::json& message)
    {
        std::string   creator;
        std::string   participantId;
        std::string   sessionId;
        std::string   buildFingerprint;
        std::uint64_t version = 0;
        if ( !readUnsignedField(message, "version", version) ||
             version != P2P_SIGNALING_PROTOCOL_VERSION ||
             !readStringField(message, "creator", creator) ||
             !readStringField(message, "participantId", participantId) ||
             !readStringField(message, "sessionId", sessionId) ||
             !readStringField(message, "buildFingerprint", buildFingerprint) ||
             !isValidCollaborationBuildFingerprint(buildFingerprint) ) {
            reject(connection, "invalid_join");
            return;
        }
        creator       = Config::normalizeCreatorIdentity(creator);
        participantId = Config::normalizeCollaborationStableId(participantId);
        sessionId     = Config::normalizeCollaborationStableId(sessionId);
        if ( creator.empty() || participantId.empty() || sessionId.empty() ||
             creator != connection.approvedCreator ) {
            reject(connection, "invalid_identity");
            return;
        }
        if ( m_requireMatchingBuildFingerprint &&
             buildFingerprint != m_buildFingerprint ) {
            reject(connection, "build_fingerprint_mismatch");
            return;
        }

        PeerId assignedPeerId = 0;
        {
            std::scoped_lock lock(m_mutex);
            if ( connection.joined ) return;
            assignedPeerId = allocatePeerId();
            if ( assignedPeerId != 0 ) {
                connection.joined              = true;
                connection.remotePeerId        = assignedPeerId;
                connection.remoteCreator       = creator;
                connection.remoteParticipantId = participantId;
                connection.remoteSessionId     = sessionId;
            }
        }
        if ( assignedPeerId == 0 ) {
            reject(connection, "room_full");
            return;
        }

        nlohmann::json accepted;
        accepted["type"]                 = "accepted";
        accepted["version"]              = P2P_SIGNALING_PROTOCOL_VERSION;
        accepted["peerId"]               = assignedPeerId;
        accepted["hostId"]               = m_hostId;
        accepted["hostCreator"]          = m_creator;
        accepted["hostParticipantId"]    = m_participantId;
        accepted["hostSessionId"]        = m_sessionId;
        accepted["hostBuildFingerprint"] = m_buildFingerprint;
        accepted["requiresMatchingBuildFingerprint"] =
            m_requireMatchingBuildFingerprint;
        if ( !createPeerConnection(connection, false) ||
             !sendSignal(connection, accepted) ) {
            pushEvent(WebRtcTransportEventType::Error,
                      assignedPeerId,
                      creator,
                      "peer_connection_create_failed");
        }
    }

    /// @brief 处理房主接受访客后的身份配置消息。
    void handleAccepted(Connection& connection, const nlohmann::json& message)
    {
        std::uint64_t peerId  = 0;
        std::uint64_t hostId  = 0;
        std::uint64_t version = 0;
        std::string   hostCreator;
        std::string   hostParticipantId;
        std::string   hostSessionId;
        std::string   hostBuildFingerprint;
        bool          requiresMatchingBuildFingerprint = true;
        if ( !readUnsignedField(message, "version", version) ||
             version != P2P_SIGNALING_PROTOCOL_VERSION ||
             !readUnsignedField(message, "peerId", peerId) || peerId == 0 ||
             !readUnsignedField(message, "hostId", hostId) || hostId == 0 ||
             !readStringField(message, "hostCreator", hostCreator) ||
             !readStringField(
                 message, "hostParticipantId", hostParticipantId) ||
             !readStringField(message, "hostSessionId", hostSessionId) ||
             !readStringField(
                 message, "hostBuildFingerprint", hostBuildFingerprint) ||
             !readBooleanField(message,
                               "requiresMatchingBuildFingerprint",
                               requiresMatchingBuildFingerprint) ||
             !isValidCollaborationBuildFingerprint(hostBuildFingerprint) ) {
            pushEvent(WebRtcTransportEventType::Error,
                      0,
                      {},
                      "invalid_accept_message");
            return;
        }
        hostCreator = Config::normalizeCreatorIdentity(hostCreator);
        hostParticipantId =
            Config::normalizeCollaborationStableId(hostParticipantId);
        hostSessionId = Config::normalizeCollaborationStableId(hostSessionId);
        if ( hostCreator.empty() || hostParticipantId.empty() ||
             hostSessionId.empty() || hostId != m_hostId ) {
            pushEvent(WebRtcTransportEventType::Error,
                      0,
                      {},
                      "invalid_host_identity");
            return;
        }
        if ( requiresMatchingBuildFingerprint &&
             hostBuildFingerprint != m_buildFingerprint ) {
            pushEvent(WebRtcTransportEventType::Rejected,
                      0,
                      hostCreator,
                      "build_fingerprint_mismatch");
            return;
        }

        {
            std::scoped_lock lock(m_mutex);
            if ( m_localPeerId != 0 ) return;
            m_localPeerId                  = static_cast<PeerId>(peerId);
            connection.remotePeerId        = static_cast<PeerId>(hostId);
            connection.remoteCreator       = hostCreator;
            connection.remoteParticipantId = std::move(hostParticipantId);
            connection.remoteSessionId     = std::move(hostSessionId);
            connection.joined              = true;
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
        std::scoped_lock rtcLock(m_rtcApiMutex);
        int              peerConnectionId = -1;
        {
            std::scoped_lock lock(m_mutex);
            if ( !m_stopping ) {
                peerConnectionId = connection.peerConnectionId;
            }
        }
        if ( peerConnectionId < 0 ) {
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
                 rtcSetRemoteDescription(
                     peerConnectionId, sdp.c_str(), descriptionType.c_str()) !=
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
             rtcAddRemoteCandidate(peerConnectionId,
                                   candidate.c_str(),
                                   mid.c_str()) != RTC_ERR_SUCCESS ) {
            pushEvent(WebRtcTransportEventType::Error,
                      connection.remotePeerId,
                      connection.remoteCreator,
                      "remote_candidate_failed");
        }
    }

    /// @brief 拒绝一条已经由中心配对的访客连接。
    void reject(Connection& connection, std::string reason)
    {
        nlohmann::json rejected;
        rejected["type"]    = "rejected";
        rejected["version"] = P2P_SIGNALING_PROTOCOL_VERSION;
        rejected["reason"]  = reason;
        static_cast<void>(sendSignal(connection, rejected));
        pushEvent(WebRtcTransportEventType::Rejected,
                  connection.remotePeerId,
                  connection.remoteCreator,
                  std::move(reason));
    }

    /// @brief 处理中心服务自身的目录与配对消息。
    bool handleBrokerMessage(Connection&           connection,
                             const nlohmann::json& message,
                             std::string_view      type)
    {
        if ( type == "room_created" &&
             connection.role == ConnectionRole::HostControl ) {
            std::string roomId;
            if ( !readStringField(message, "roomId", roomId) ||
                 !isValidRoomId(roomId) || !updateIceServers(message) ) {
                pushEvent(WebRtcTransportEventType::Error,
                          m_hostId,
                          m_creator,
                          "invalid_room_created");
                return true;
            }
            {
                std::scoped_lock lock(m_mutex);
                m_roomId = roomId;
            }
            pushEvent(WebRtcTransportEventType::RoomPublished,
                      m_hostId,
                      m_creator,
                      std::move(roomId));
            return true;
        }
        if ( type == "join_requested" &&
             connection.role == ConnectionRole::HostControl ) {
            std::string roomId;
            std::string requestId;
            std::string guestCreator;
            std::string guestBuildFingerprint;
            if ( !readStringField(message, "roomId", roomId) ||
                 !readStringField(message, "requestId", requestId) ||
                 !readStringField(message, "guestCreator", guestCreator) ||
                 !readStringField(
                     message, "guestBuildFingerprint", guestBuildFingerprint) ||
                 !isValidCollaborationBuildFingerprint(
                     guestBuildFingerprint) ) {
                pushEvent(WebRtcTransportEventType::Error,
                          0,
                          {},
                          "invalid_join_request");
                return true;
            }
            {
                std::scoped_lock lock(m_mutex);
                if ( roomId != m_roomId ) return true;
            }
            guestCreator = Config::normalizeCreatorIdentity(guestCreator);
            if ( requestId.empty() || requestId.size() > 128U ||
                 guestCreator.empty() ) {
                pushEvent(WebRtcTransportEventType::Error,
                          0,
                          {},
                          "invalid_join_request");
                return true;
            }
            {
                std::scoped_lock lock(m_mutex);
                m_pendingJoinRequests.insert_or_assign(requestId, guestCreator);
            }
            pushEvent(WebRtcTransportEventType::JoinRequested,
                      0,
                      std::move(guestCreator),
                      "approval_required",
                      std::move(requestId),
                      {},
                      {},
                      std::move(guestBuildFingerprint));
            return true;
        }
        if ( type == "join_pending" ) {
            std::string requestId;
            if ( !readStringField(message, "requestId", requestId) ||
                 requestId.empty() || requestId.size() > 128U ) {
                pushEvent(WebRtcTransportEventType::Error,
                          0,
                          {},
                          "invalid_join_pending");
                return true;
            }
            pushEvent(WebRtcTransportEventType::JoinPending,
                      m_hostId,
                      {},
                      "awaiting_host_approval",
                      std::move(requestId));
            return true;
        }
        if ( type == "join_cancelled" &&
             connection.role == ConnectionRole::HostControl ) {
            std::string requestId;
            std::string guestCreator;
            if ( !readStringField(message, "requestId", requestId) ||
                 !readStringField(message, "guestCreator", guestCreator) ) {
                pushEvent(WebRtcTransportEventType::Error,
                          0,
                          {},
                          "invalid_join_cancellation");
                return true;
            }
            {
                std::scoped_lock lock(m_mutex);
                m_pendingJoinRequests.erase(requestId);
            }
            pushEvent(WebRtcTransportEventType::JoinCancelled,
                      0,
                      std::move(guestCreator),
                      "join_cancelled",
                      std::move(requestId));
            return true;
        }
        if ( type == "relay_ready" &&
             connection.role == ConnectionRole::Peer ) {
            if ( !updateIceServers(message) ) {
                pushEvent(WebRtcTransportEventType::Error,
                          connection.remotePeerId,
                          {},
                          "invalid_ice_configuration");
                return true;
            }
            if ( !m_isHost ) {
                nlohmann::json join;
                join["type"]             = "join";
                join["version"]          = P2P_SIGNALING_PROTOCOL_VERSION;
                join["creator"]          = m_creator;
                join["participantId"]    = m_participantId;
                join["sessionId"]        = m_sessionId;
                join["buildFingerprint"] = m_buildFingerprint;
                if ( !sendSignal(connection, join) ) {
                    pushEvent(WebRtcTransportEventType::Error,
                              m_hostId,
                              {},
                              "join_signal_send_failed");
                    return true;
                }
            }
            pushEvent(WebRtcTransportEventType::SignalingConnected,
                      connection.remotePeerId,
                      {},
                      "p2p_signaling_ready");
            return true;
        }
        if ( type == "error" ) {
            std::string reason;
            static_cast<void>(readStringField(message, "reason", reason));
            const bool hostControl =
                connection.role == ConnectionRole::HostControl;
            if ( !hostControl ) {
                std::scoped_lock lock(m_mutex);
                connection.signalingTerminalEventSent = true;
            }
            pushEvent(hostControl ? WebRtcTransportEventType::Error
                                  : WebRtcTransportEventType::Rejected,
                      connection.remotePeerId,
                      connection.remoteCreator,
                      reason.empty() ? "signaling_server_error" : reason);
            return true;
        }
        if ( type == "p2p_ready" && connection.role == ConnectionRole::Peer ) {
            std::uint64_t version = 0;
            if ( !readUnsignedField(message, "version", version) ||
                 version != P2P_SIGNALING_PROTOCOL_VERSION ) {
                pushEvent(WebRtcTransportEventType::Error,
                          connection.remotePeerId,
                          connection.remoteCreator,
                          "unsupported_p2p_protocol");
                return true;
            }
            {
                std::scoped_lock lock(m_mutex);
                connection.remoteDataChannelReady = true;
            }
            closeSignalingIfReady(connection);
            return true;
        }
        return false;
    }

    /// @brief 处理一条已经解析的 WebSocket 信令消息。
    void handleSignalingMessage(Connection&           connection,
                                const nlohmann::json& message)
    {
        if ( !message.is_object() ) return;
        std::string type;
        if ( !readStringField(message, "type", type) ) return;
        if ( handleBrokerMessage(connection, message, type) ) return;

        if ( m_isHost && connection.role == ConnectionRole::Peer &&
             type == "join" ) {
            handleJoin(connection, message);
        } else if ( !m_isHost && type == "accepted" ) {
            handleAccepted(connection, message);
        } else if ( !m_isHost && type == "rejected" ) {
            std::string reason;
            static_cast<void>(readStringField(message, "reason", reason));
            {
                std::scoped_lock lock(m_mutex);
                connection.signalingTerminalEventSent = true;
            }
            pushEvent(WebRtcTransportEventType::Rejected,
                      0,
                      {},
                      reason.empty() ? "rejected" : reason);
        } else if ( type == "description" || type == "candidate" ) {
            handleNegotiation(connection, message, type);
        }
    }

    /// @brief 向中心服务上报房主当前真实 P2P 在线人数。
    void sendParticipantCount()
    {
        std::scoped_lock rtcLock(m_rtcApiMutex);
        int              controlWebSocketId = -1;
        std::size_t      participants       = 1;
        {
            std::scoped_lock lock(m_mutex);
            if ( !m_isHost || m_stopping ) return;
            for ( const auto& connection : m_connections ) {
                if ( connection->role == ConnectionRole::HostControl ) {
                    controlWebSocketId = connection->websocketId;
                } else if ( connection->connectedEventSent &&
                            !connection->disconnectedEventSent ) {
                    ++participants;
                }
            }
        }
        if ( controlWebSocketId < 0 || !rtcIsOpen(controlWebSocketId) ) return;
        const nlohmann::json message = {
            { "type", "update_room" },
            { "version", BROKER_PROTOCOL_VERSION },
            { "participants", participants },
        };
        const std::string payload = message.dump();
        static_cast<void>(
            rtcSendMessage(controlWebSocketId, payload.c_str(), -1));
    }

    /// @brief 判断连接已经打开 DataChannel。
    bool isPeerConnected(const Connection& connection) const
    {
        std::scoped_lock lock(m_mutex);
        return connection.connectedEventSent &&
               !connection.disconnectedEventSent;
    }

    /// @brief 判断中心服务拒绝是否已经提供了更准确的终止原因。
    bool hasSignalingTerminalEvent(const Connection& connection) const
    {
        std::scoped_lock lock(m_mutex);
        return connection.signalingTerminalEventSent;
    }

    /// @brief 双方均确认 DataChannel 打开后释放瞬时信令连接。
    void closeSignalingIfReady(Connection& connection)
    {
        std::scoped_lock rtcLock(m_rtcApiMutex);
        int              websocketId = -1;
        {
            std::scoped_lock lock(m_mutex);
            if ( !m_stopping && connection.connectedEventSent &&
                 connection.remoteDataChannelReady ) {
                websocketId = connection.websocketId;
            }
        }
        if ( websocketId >= 0 && rtcIsOpen(websocketId) ) {
            static_cast<void>(rtcClose(websocketId));
        }
    }

    /// @brief 中心 WebSocket 打开后声明当前连接职责。
    static void onWebSocketOpen(int, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        if ( connection->openHandled.exchange(true,
                                              std::memory_order_acq_rel) ) {
            return;
        }
        Impl& owner = *connection->owner;

        nlohmann::json message;
        message["version"] = BROKER_PROTOCOL_VERSION;
        if ( connection->role == ConnectionRole::HostControl ) {
            message["type"]       = "create_room";
            message["roomName"]   = owner.m_roomName;
            message["creator"]    = owner.m_creator;
            message["ownerToken"] = owner.m_ownerToken;
            message["capacity"]   = owner.m_maxParticipants;
        } else if ( owner.m_isHost ) {
            std::scoped_lock lock(owner.m_mutex);
            message["type"]       = "accept_join";
            message["roomId"]     = owner.m_roomId;
            message["requestId"]  = connection->requestId;
            message["ownerToken"] = owner.m_ownerToken;
        } else {
            message["type"]             = "join_room";
            message["roomId"]           = owner.m_roomId;
            message["creator"]          = owner.m_creator;
            message["buildFingerprint"] = owner.m_buildFingerprint;
        }
        if ( !owner.sendSignal(*connection, message) ) {
            owner.pushEvent(WebRtcTransportEventType::Error,
                            connection->remotePeerId,
                            {},
                            "broker_request_send_failed");
        }
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

    /// @brief WebSocket 关闭回调；P2P 已建立时信令关闭属于正常释放。
    static void onWebSocketClosed(int, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        Impl& owner = *connection->owner;
        if ( connection->role == ConnectionRole::HostControl ) {
            owner.pushEvent(WebRtcTransportEventType::Error,
                            owner.m_hostId,
                            owner.m_creator,
                            "room_directory_connection_closed");
        } else if ( !owner.isPeerConnected(*connection) &&
                    !owner.hasSignalingTerminalEvent(*connection) ) {
            owner.notifyDisconnected(*connection, "signaling_closed");
        }
    }

    /// @brief WebSocket 错误回调。
    static void onWebSocketError(int, const char* error, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        if ( connection->role == ConnectionRole::Peer &&
             (connection->owner->isPeerConnected(*connection) ||
              connection->owner->hasSignalingTerminalEvent(*connection)) ) {
            return;
        }
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
        if ( !connection->owner->sendSignal(*connection, description) &&
             !connection->owner->isPeerConnected(*connection) ) {
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
        if ( !connection->owner->sendSignal(*connection, candidateMessage) &&
             !connection->owner->isPeerConnected(*connection) ) {
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
        Impl&            owner = *connection->owner;
        std::scoped_lock rtcLock(owner.m_rtcApiMutex);
        {
            std::scoped_lock lock(owner.m_mutex);
            if ( owner.m_stopping || !owner.m_running ) return;
        }
        std::array<char, 64> label{};
        const int            labelLength = rtcGetDataChannelLabel(
            dataChannelId, label.data(), static_cast<int>(label.size()));
        if ( labelLength <= 0 ||
             std::string_view(label.data()) != COLLABORATION_CHANNEL_LABEL ) {
            rtcDeleteDataChannel(dataChannelId);
            owner.pushEvent(WebRtcTransportEventType::Error,
                            connection->remotePeerId,
                            connection->remoteCreator,
                            "unexpected_data_channel");
            return;
        }
        owner.configureDataChannel(*connection, dataChannelId);
    }

    /// @brief DataChannel 打开后与对端确认，再释放瞬时信令连接。
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
                        "data_channel_open",
                        {},
                        connection->remoteParticipantId,
                        connection->remoteSessionId);
        if ( owner.m_isHost ) owner.sendParticipantCount();
        nlohmann::json ready;
        ready["type"]    = "p2p_ready";
        ready["version"] = P2P_SIGNALING_PROTOCOL_VERSION;
        if ( !owner.sendSignal(*connection, ready) ) {
            owner.pushEvent(WebRtcTransportEventType::Error,
                            connection->remotePeerId,
                            connection->remoteCreator,
                            "p2p_ready_send_failed");
            return;
        }
        owner.closeSignalingIfReady(*connection);
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

    /// @brief 保证每个 P2P 连接只上报一次离开事件。
    void notifyDisconnected(Connection& connection, std::string detail)
    {
        bool updateParticipants = false;
        {
            std::scoped_lock lock(m_mutex);
            if ( connection.disconnectedEventSent || m_stopping ) return;
            connection.disconnectedEventSent = true;
            updateParticipants = m_isHost && connection.connectedEventSent;
            connection.connectedEventSent = false;
            connection.joined             = false;
        }
        pushEvent(WebRtcTransportEventType::PeerDisconnected,
                  connection.remotePeerId,
                  connection.remoteCreator,
                  std::move(detail),
                  {},
                  connection.remoteParticipantId,
                  connection.remoteSessionId);
        if ( updateParticipants ) sendParticipantCount();
    }

    /// @brief 串行化 RTC 句柄创建、发送、关闭与退役。
    mutable std::recursive_mutex m_rtcApiMutex;
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
    /// @brief 当前应用配置持久化的稳定协作者标识。
    ParticipantId m_participantId;
    /// @brief 当前加入流程生成的操作会话标识。
    OperationSessionId m_sessionId;
    /// @brief 当前主程序二进制的 SHA-256 构建指纹。
    std::string m_buildFingerprint;
    /// @brief 房主是否在身份握手中强制构建指纹相等。
    bool m_requireMatchingBuildFingerprint = true;
    /// @brief 公网目录展示名称。
    std::string m_roomName;
    /// @brief 中心服务分配的公开房间标识。
    std::string m_roomId;
    /// @brief 中心信令 URL。
    std::string m_signalingUrl;
    /// @brief 房主控制连接鉴权令牌。
    std::string m_ownerToken;
    /// @brief 中心服务下发的 ICE URI。
    std::vector<std::string> m_iceServers;
    /// @brief 当前本地 PeerId。
    PeerId m_localPeerId = 0;
    /// @brief 房主 PeerId。
    PeerId m_hostId = 0;
    /// @brief 房间总人数上限。
    std::size_t m_maxParticipants = MAX_COLLABORATION_PARTICIPANTS;
    /// @brief 房主控制连接、房主访客连接或访客唯一连接。
    std::vector<std::unique_ptr<Connection>> m_connections;
    /// @brief 等待产品层房主批准或拒绝的中心服务加入请求。
    std::unordered_map<std::string, std::string> m_pendingJoinRequests;
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

bool WebRtcTransport::approveJoinRequest(std::string_view requestId)
{
    return m_impl->approveJoinRequest(requestId);
}

bool WebRtcTransport::rejectJoinRequest(std::string_view requestId,
                                        std::string_view reason)
{
    return m_impl->rejectJoinRequest(requestId, reason);
}

bool WebRtcTransport::disconnectPeer(PeerId peerId, std::string detail)
{
    return m_impl->disconnectPeer(peerId, std::move(detail));
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

std::string WebRtcTransport::roomId() const
{
    return m_impl->roomId();
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
