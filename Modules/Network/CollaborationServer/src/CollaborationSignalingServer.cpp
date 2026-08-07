#include "network/collaboration_server/CollaborationSignalingServer.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <random>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::Network::CollaborationServer
{
namespace
{
/// @brief 中心房间目录协议版本。
constexpr std::uint64_t DIRECTORY_PROTOCOL_VERSION = 1;
/// @brief 中心服务接受的 WebSocket 路径。
constexpr std::string_view SIGNALING_PATH = "/mmm-collaboration";
/// @brief 单条信令消息上限。
constexpr int MAX_SIGNALING_MESSAGE_BYTES = 256 * 1024;
/// @brief 回调线程允许积压的事件上限。
constexpr std::size_t MAX_CALLBACK_EVENTS = 8192;
/// @brief 未声明身份的连接保留时间。
constexpr auto UNKNOWN_CLIENT_TIMEOUT = std::chrono::seconds(15);
/// @brief 等待房主接受的加入请求保留时间。
constexpr auto JOIN_REQUEST_TIMEOUT = std::chrono::seconds(20);
/// @brief 已关闭句柄延迟删除时间，避免 libdatachannel 立即复用整数句柄。
constexpr auto RETIRED_WEBSOCKET_GRACE = std::chrono::seconds(5);
/// @brief 随机标识使用的无歧义字符表。
constexpr std::string_view IDENTIFIER_ALPHABET =
    "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

/// @brief 从 JSON 对象读取字符串字段。
/// @param object 输入 JSON 对象。
/// @param key 字段名。
/// @param value 输出字符串。
/// @return 字段存在且为字符串时返回 true。
bool readStringField(const nlohmann::json& object, std::string_view key,
                     std::string& value)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_string() ) return false;
    value = iterator->get_ref<const std::string&>();
    return true;
}

/// @brief 从 JSON 对象读取无符号整数字段。
/// @param object 输入 JSON 对象。
/// @param key 字段名。
/// @param value 输出整数。
/// @return 字段存在且为无符号整数时返回 true。
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

/// @brief 校验公开展示文本并拒绝控制字符。
/// @param value 输入文本。
/// @param maxBytes UTF-8 字节上限。
/// @return 文本可公开展示时返回 true。
bool isValidDisplayText(std::string_view value, std::size_t maxBytes)
{
    if ( value.empty() || value.size() > maxBytes ) return false;
    return std::none_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20U || byte == 0x7FU;
    });
}

/// @brief 校验只用于房主控制连接的高熵令牌格式。
/// @param value 输入令牌。
/// @return 长度和字符集满足约束时返回 true。
bool isValidOwnerToken(std::string_view value)
{
    if ( value.size() < 32 || value.size() > 128 ) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '-' || character == '_';
    });
}
}  // namespace

class CollaborationSignalingServer::Impl
{
public:
    /// @brief WebSocket 客户端在目录服务中的职责。
    enum class ClientRole {
        Unknown,
        Directory,
        HostControl,
        PendingGuest,
        Paired,
        Closing,
    };

    /// @brief libdatachannel 回调提交给主循环的事件类型。
    enum class CallbackEventType {
        Connected,
        Message,
        Closed,
        Error,
    };

    /// @brief 一条跨回调线程传递的 WebSocket 事件。
    struct CallbackEvent {
        /// @brief 事件类型。
        CallbackEventType type = CallbackEventType::Error;
        /// @brief WebSocket 句柄。
        int websocketId = -1;
        /// @brief 区分复用 WebSocket 句柄的服务端连接代次。
        std::uint64_t generation = 0;
        /// @brief 消息或错误文本。
        std::string payload;
        /// @brief 是否为 WebSocket 文本消息。
        bool textMessage = true;
    };

    /// @brief 绑定到单次 WebSocket 生命周期的稳定回调上下文。
    struct CallbackContext {
        /// @brief 所属服务端实现。
        Impl* owner = nullptr;
        /// @brief WebSocket 句柄。
        int websocketId = -1;
        /// @brief 当前句柄的连接代次。
        std::uint64_t generation = 0;
    };

    /// @brief 一个已接入 WebSocket 客户端的主循环状态。
    struct Client {
        /// @brief 当前 WebSocket 生命周期代次。
        std::uint64_t generation = 0;
        /// @brief 客户端职责。
        ClientRole role = ClientRole::Unknown;
        /// @brief 所属公开房间 ID。
        std::string roomId;
        /// @brief 等待配对的请求 ID。
        std::string requestId;
        /// @brief 客户端展示身份。
        std::string creator;
        /// @brief 配对后的对端 WebSocket。
        int partnerWebSocketId = -1;
        /// @brief 最近一次有效活动时间。
        std::chrono::steady_clock::time_point lastActivity =
            std::chrono::steady_clock::now();
    };

    /// @brief 一个公开可发现房间的服务器状态。
    struct Room {
        /// @brief 公共房间 ID。
        std::string roomId;
        /// @brief 房间展示名称。
        std::string roomName;
        /// @brief 房主展示身份。
        std::string hostCreator;
        /// @brief 仅房主知道的控制令牌。
        std::string ownerToken;
        /// @brief 房主控制 WebSocket。
        int controlWebSocketId = -1;
        /// @brief 当前已建立房间成员数，包含房主。
        std::size_t participants = 1;
        /// @brief 房间容量。
        std::size_t capacity = 8;
        /// @brief 房间创建顺序。
        std::uint64_t creationSequence = 0;
    };

    /// @brief 一个等待房主创建独立信令通道的加入请求。
    struct PendingJoin {
        /// @brief 请求 ID。
        std::string requestId;
        /// @brief 目标房间 ID。
        std::string roomId;
        /// @brief 访客 WebSocket。
        int guestWebSocketId = -1;
        /// @brief 访客展示身份。
        std::string creator;
        /// @brief 请求创建时间。
        std::chrono::steady_clock::time_point createdAt =
            std::chrono::steady_clock::now();
    };

    /// @brief 已清除回调、等待安全删除的 WebSocket 句柄。
    struct RetiredWebSocket {
        /// @brief WebSocket 句柄。
        int websocketId = -1;
        /// @brief 与句柄共同延迟释放的回调上下文代次。
        std::uint64_t generation = 0;
        /// @brief 进入退役队列的时间。
        std::chrono::steady_clock::time_point retiredAt =
            std::chrono::steady_clock::now();
    };

    /// @brief 析构时释放全部 libdatachannel 句柄。
    ~Impl() { stop(); }

    /// @brief 启动 WebSocket 信令服务。
    bool start(CollaborationSignalingServerConfig config)
    {
        if ( m_running.load(std::memory_order_acquire) ||
             config.maxRooms == 0 || config.maxClients == 0 ) {
            return false;
        }
        if ( config.enableTls && (config.certificatePemFile.empty() ||
                                  config.keyPemFile.empty()) ) {
            return false;
        }

        m_config = std::move(config);
        rtcWsServerConfiguration serverConfig{};
        serverConfig.port      = m_config.port;
        serverConfig.enableTls = m_config.enableTls;
        serverConfig.certificatePemFile =
            m_config.certificatePemFile.empty()
                ? nullptr
                : m_config.certificatePemFile.c_str();
        serverConfig.keyPemFile =
            m_config.keyPemFile.empty() ? nullptr : m_config.keyPemFile.c_str();
        serverConfig.bindAddress         = m_config.bindAddress.empty()
                                               ? nullptr
                                               : m_config.bindAddress.c_str();
        serverConfig.connectionTimeoutMs = 10000;
        serverConfig.maxMessageSize      = MAX_SIGNALING_MESSAGE_BYTES;

        m_acceptCallbacks.store(true, std::memory_order_release);
        const int serverId =
            rtcCreateWebSocketServer(&serverConfig, &Impl::onWebSocketClient);
        if ( serverId < 0 ) {
            m_acceptCallbacks.store(false, std::memory_order_release);
            return false;
        }
        rtcSetUserPointer(serverId, this);

        const int actualPort = rtcGetWebSocketServerPort(serverId);
        if ( actualPort <= 0 || actualPort > 65535 ) {
            m_acceptCallbacks.store(false, std::memory_order_release);
            rtcDeleteWebSocketServer(serverId);
            return false;
        }
        m_serverId      = serverId;
        m_listeningPort = static_cast<std::uint16_t>(actualPort);
        m_running.store(true, std::memory_order_release);
        return true;
    }

    /// @brief 停止服务并删除全部 WebSocket。
    void stop()
    {
        m_acceptCallbacks.store(false, std::memory_order_release);
        m_running.store(false, std::memory_order_release);

        const int serverId = std::exchange(m_serverId, -1);
        if ( serverId >= 0 ) rtcDeleteWebSocketServer(serverId);

        for ( const auto& [websocketId, client] : m_clients ) {
            static_cast<void>(client);
            deleteWebSocketSafely(websocketId);
        }
        for ( const auto& retired : m_retiredWebSockets ) {
            rtcDeleteWebSocket(retired.websocketId);
        }
        m_retiredWebSockets.clear();
        m_clients.clear();
        m_rooms.clear();
        m_pendingJoins.clear();
        m_listeningPort  = 0;
        m_directoryDirty = false;

        std::scoped_lock lock(m_callbackMutex);
        m_callbackEvents.clear();
        m_callbackContexts.clear();
    }

    /// @brief 在服务主线程消费回调事件与超时。
    void update()
    {
        if ( !m_running.load(std::memory_order_acquire) ) return;

        std::deque<CallbackEvent> events;
        {
            std::scoped_lock lock(m_callbackMutex);
            events.swap(m_callbackEvents);
        }
        for ( const auto& event : events ) {
            if ( event.type == CallbackEventType::Connected ) {
                processConnected(event.websocketId, event.generation);
            }
        }
        for ( auto& event : events ) {
            switch ( event.type ) {
            case CallbackEventType::Connected: break;
            case CallbackEventType::Message:
                processMessage(event.websocketId,
                               event.generation,
                               std::move(event.payload),
                               event.textMessage);
                break;
            case CallbackEventType::Closed:
                processClosed(event.websocketId, event.generation);
                break;
            case CallbackEventType::Error:
                XWARN("Collaboration signaling WebSocket {} error: {}",
                      event.websocketId,
                      event.payload);
                processClosed(event.websocketId, event.generation);
                break;
            }
        }

        expireIdleClientsAndJoins();
        deleteRetiredWebSockets();
        if ( m_directoryDirty ) broadcastRoomList();
    }

    /// @brief 返回运行状态。
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    /// @brief 返回实际监听端口。
    std::uint16_t listeningPort() const { return m_listeningPort; }

    /// @brief 返回在线房间数。
    std::size_t roomCount() const { return m_rooms.size(); }

    /// @brief 返回已接入客户端数。
    std::size_t clientCount() const { return m_clients.size(); }

private:
    /// @brief 清除回调后删除服务端 WebSocket，避免句柄复用命中迟到事件。
    static void deleteWebSocketSafely(int websocketId)
    {
        rtcSetMessageCallback(websocketId, nullptr);
        rtcSetClosedCallback(websocketId, nullptr);
        rtcSetErrorCallback(websocketId, nullptr);
        rtcSetUserPointer(websocketId, nullptr);
        rtcDeleteWebSocket(websocketId);
    }

    /// @brief 释放已经清除全部第三方回调的连接代次上下文。
    void releaseCallbackContext(std::uint64_t generation)
    {
        std::scoped_lock lock(m_callbackMutex);
        m_callbackContexts.erase(generation);
    }

    /// @brief 清除回调并把关闭句柄放入延迟删除队列。
    void retireWebSocket(int websocketId, std::uint64_t generation)
    {
        rtcSetMessageCallback(websocketId, nullptr);
        rtcSetClosedCallback(websocketId, nullptr);
        rtcSetErrorCallback(websocketId, nullptr);
        rtcSetUserPointer(websocketId, nullptr);
        static_cast<void>(rtcClose(websocketId));
        m_retiredWebSockets.push_back(
            { websocketId, generation, std::chrono::steady_clock::now() });
    }

    /// @brief 删除已越过回调迟到窗口的退役 WebSocket。
    void deleteRetiredWebSockets()
    {
        const auto now = std::chrono::steady_clock::now();
        while ( !m_retiredWebSockets.empty() &&
                now - m_retiredWebSockets.front().retiredAt >=
                    RETIRED_WEBSOCKET_GRACE ) {
            const auto retired = m_retiredWebSockets.front();
            rtcDeleteWebSocket(retired.websocketId);
            releaseCallbackContext(retired.generation);
            m_retiredWebSockets.pop_front();
        }
    }

    /// @brief 生成不与现有房间或请求冲突的随机标识。
    std::string generateIdentifier(std::size_t length)
    {
        std::uniform_int_distribution<std::size_t> distribution(
            0, IDENTIFIER_ALPHABET.size() - 1);
        std::string result(length, '0');
        for ( char& character : result ) {
            character = IDENTIFIER_ALPHABET[distribution(m_random)];
        }
        return result;
    }

    /// @brief 生成唯一公共房间 ID。
    std::string generateRoomId()
    {
        for ( std::size_t attempt = 0; attempt < 32; ++attempt ) {
            auto candidate = generateIdentifier(10);
            if ( !m_rooms.contains(candidate) ) return candidate;
        }
        return {};
    }

    /// @brief 生成唯一等待请求 ID。
    std::string generateRequestId()
    {
        for ( std::size_t attempt = 0; attempt < 32; ++attempt ) {
            auto candidate = generateIdentifier(16);
            if ( !m_pendingJoins.contains(candidate) ) return candidate;
        }
        return {};
    }

    /// @brief 把 ICE URI 列表写入服务消息。
    void appendIceServers(nlohmann::json& message) const
    {
        message["iceServers"] = m_config.iceServers;
    }

    /// @brief 发送 JSON 文本消息。
    bool sendJson(int websocketId, const nlohmann::json& message) const
    {
        const std::string payload = message.dump();
        if ( payload.size() >
             static_cast<std::size_t>(MAX_SIGNALING_MESSAGE_BYTES) ) {
            return false;
        }
        return rtcSendMessage(websocketId, payload.c_str(), -1) ==
               RTC_ERR_SUCCESS;
    }

    /// @brief 发送带稳定原因码的错误并关闭客户端。
    void rejectClient(int websocketId, std::string_view reason)
    {
        XWARN("Collaboration signaling client {} rejected: {}",
              websocketId,
              reason);
        nlohmann::json message;
        message["type"]    = "error";
        message["version"] = DIRECTORY_PROTOCOL_VERSION;
        message["reason"]  = reason;
        static_cast<void>(sendJson(websocketId, message));
        closeClient(websocketId);
    }

    /// @brief 请求关闭一个仍在跟踪的客户端。
    void closeClient(int websocketId)
    {
        const auto iterator = m_clients.find(websocketId);
        if ( iterator == m_clients.end() ||
             iterator->second.role == ClientRole::Closing ) {
            return;
        }
        iterator->second.role = ClientRole::Closing;
        static_cast<void>(rtcClose(websocketId));
    }

    /// @brief 验证新连接路径并纳入客户端表。
    void processConnected(int websocketId, std::uint64_t generation)
    {
        if ( m_clients.size() >= m_config.maxClients ) {
            retireWebSocket(websocketId, generation);
            return;
        }

        std::array<char, 128> path{};
        const int             pathLength = rtcGetWebSocketPath(
            websocketId, path.data(), static_cast<int>(path.size()));
        if ( pathLength <= 0 ||
             std::string_view(path.data()) != SIGNALING_PATH ) {
            retireWebSocket(websocketId, generation);
            return;
        }
        Client client;
        client.generation = generation;
        m_clients.emplace(websocketId, std::move(client));
    }

    /// @brief 处理一条客户端消息或在配对后透明转发。
    void processMessage(int websocketId, std::uint64_t generation,
                        std::string payload, bool textMessage)
    {
        const auto clientIterator = m_clients.find(websocketId);
        if ( clientIterator == m_clients.end() ||
             clientIterator->second.generation != generation ) {
            return;
        }
        Client& client      = clientIterator->second;
        client.lastActivity = std::chrono::steady_clock::now();

        if ( client.role == ClientRole::Paired ) {
            const auto partnerIterator =
                m_clients.find(client.partnerWebSocketId);
            if ( partnerIterator == m_clients.end() ||
                 partnerIterator->second.role != ClientRole::Paired ) {
                closeClient(websocketId);
                return;
            }
            const int result =
                textMessage
                    ? rtcSendMessage(
                          client.partnerWebSocketId, payload.c_str(), -1)
                    : rtcSendMessage(client.partnerWebSocketId,
                                     payload.data(),
                                     static_cast<int>(payload.size()));
            if ( result != RTC_ERR_SUCCESS ) closeClient(websocketId);
            return;
        }
        if ( !textMessage ) {
            rejectClient(websocketId, "binary_before_pairing");
            return;
        }

        const auto message = nlohmann::json::parse(payload, nullptr, false);
        if ( message.is_discarded() || !message.is_object() ) {
            rejectClient(websocketId, "invalid_json");
            return;
        }
        std::string   type;
        std::uint64_t version = 0;
        if ( !readStringField(message, "type", type) ||
             !readUnsignedField(message, "version", version) ||
             version != DIRECTORY_PROTOCOL_VERSION ) {
            rejectClient(websocketId, "unsupported_protocol");
            return;
        }

        if ( type == "list_rooms" ) {
            client.role = ClientRole::Directory;
            sendRoomList(websocketId);
        } else if ( type == "update_room" &&
                    client.role == ClientRole::HostControl ) {
            handleUpdateRoom(websocketId, message);
        } else if ( type == "create_room" &&
                    client.role == ClientRole::Unknown ) {
            handleCreateRoom(websocketId, message);
        } else if ( type == "join_room" &&
                    client.role == ClientRole::Unknown ) {
            handleJoinRoom(websocketId, message);
        } else if ( type == "accept_join" &&
                    client.role == ClientRole::Unknown ) {
            handleAcceptJoin(websocketId, message);
        } else if ( type == "ping" ) {
            nlohmann::json pong;
            pong["type"]    = "pong";
            pong["version"] = DIRECTORY_PROTOCOL_VERSION;
            static_cast<void>(sendJson(websocketId, pong));
        } else {
            rejectClient(websocketId, "invalid_state");
        }
    }

    /// @brief 接受房主控制连接上报的真实 P2P 在线人数。
    void handleUpdateRoom(int websocketId, const nlohmann::json& message)
    {
        std::uint64_t participants   = 0;
        const auto    clientIterator = m_clients.find(websocketId);
        if ( clientIterator == m_clients.end() ||
             !readUnsignedField(message, "participants", participants) ) {
            rejectClient(websocketId, "invalid_room_update");
            return;
        }
        const auto roomIterator = m_rooms.find(clientIterator->second.roomId);
        if ( roomIterator == m_rooms.end() || participants == 0 ||
             participants > roomIterator->second.capacity ) {
            rejectClient(websocketId, "invalid_room_update");
            return;
        }
        if ( roomIterator->second.participants != participants ) {
            roomIterator->second.participants =
                static_cast<std::size_t>(participants);
            m_directoryDirty = true;
        }
    }

    /// @brief 注册一个公开房间和房主控制连接。
    void handleCreateRoom(int websocketId, const nlohmann::json& message)
    {
        if ( m_rooms.size() >= m_config.maxRooms ) {
            rejectClient(websocketId, "room_limit_reached");
            return;
        }

        std::string   roomName;
        std::string   creator;
        std::string   ownerToken;
        std::uint64_t capacity = 0;
        if ( !readStringField(message, "roomName", roomName) ||
             !readStringField(message, "creator", creator) ||
             !readStringField(message, "ownerToken", ownerToken) ||
             !readUnsignedField(message, "capacity", capacity) ||
             !isValidDisplayText(roomName, 128) ||
             !isValidDisplayText(creator, 64) ||
             !isValidOwnerToken(ownerToken) || capacity < 2 || capacity > 8 ) {
            rejectClient(websocketId, "invalid_room");
            return;
        }

        std::string roomId = generateRoomId();
        if ( roomId.empty() ) {
            rejectClient(websocketId, "room_id_exhausted");
            return;
        }

        Room room;
        room.roomId             = roomId;
        room.roomName           = std::move(roomName);
        room.hostCreator        = std::move(creator);
        room.ownerToken         = std::move(ownerToken);
        room.controlWebSocketId = websocketId;
        room.capacity           = static_cast<std::size_t>(capacity);
        room.creationSequence   = ++m_roomSequence;
        m_rooms.emplace(roomId, std::move(room));

        Client& client = m_clients.at(websocketId);
        client.role    = ClientRole::HostControl;
        client.roomId  = roomId;

        nlohmann::json created;
        created["type"]    = "room_created";
        created["version"] = DIRECTORY_PROTOCOL_VERSION;
        created["roomId"]  = roomId;
        appendIceServers(created);
        if ( !sendJson(websocketId, created) ) {
            closeClient(websocketId);
            return;
        }
        m_directoryDirty = true;
        XINFO("Collaboration room {} created by {}",
              roomId,
              m_rooms.at(roomId).hostCreator);
    }

    /// @brief 记录访客加入请求并通知房主建立专用信令通道。
    void handleJoinRoom(int websocketId, const nlohmann::json& message)
    {
        std::string roomId;
        std::string creator;
        if ( !readStringField(message, "roomId", roomId) ||
             !readStringField(message, "creator", creator) ||
             !isValidDisplayText(creator, 64) ) {
            rejectClient(websocketId, "invalid_join");
            return;
        }
        const auto roomIterator = m_rooms.find(roomId);
        if ( roomIterator == m_rooms.end() ) {
            rejectClient(websocketId, "room_not_found");
            return;
        }
        const std::size_t pendingCount = static_cast<std::size_t>(
            std::count_if(m_pendingJoins.begin(),
                          m_pendingJoins.end(),
                          [&roomId](const auto& entry) {
                              return entry.second.roomId == roomId;
                          }));
        if ( roomIterator->second.participants + pendingCount >=
             roomIterator->second.capacity ) {
            rejectClient(websocketId, "room_full");
            return;
        }

        std::string requestId = generateRequestId();
        if ( requestId.empty() ) {
            rejectClient(websocketId, "request_id_exhausted");
            return;
        }

        PendingJoin pending;
        pending.requestId        = requestId;
        pending.roomId           = roomId;
        pending.guestWebSocketId = websocketId;
        pending.creator          = creator;
        m_pendingJoins.emplace(requestId, std::move(pending));

        Client& client   = m_clients.at(websocketId);
        client.role      = ClientRole::PendingGuest;
        client.roomId    = roomId;
        client.requestId = requestId;
        client.creator   = creator;

        nlohmann::json waiting;
        waiting["type"]      = "join_pending";
        waiting["version"]   = DIRECTORY_PROTOCOL_VERSION;
        waiting["requestId"] = requestId;
        static_cast<void>(sendJson(websocketId, waiting));

        nlohmann::json requested;
        requested["type"]         = "join_requested";
        requested["version"]      = DIRECTORY_PROTOCOL_VERSION;
        requested["roomId"]       = roomId;
        requested["requestId"]    = requestId;
        requested["guestCreator"] = creator;
        if ( !sendJson(roomIterator->second.controlWebSocketId, requested) ) {
            m_pendingJoins.erase(requestId);
            rejectClient(websocketId, "host_unavailable");
        }
    }

    /// @brief 校验房主令牌并把访客与独立房主信令通道配对。
    void handleAcceptJoin(int websocketId, const nlohmann::json& message)
    {
        std::string roomId;
        std::string requestId;
        std::string ownerToken;
        if ( !readStringField(message, "roomId", roomId) ||
             !readStringField(message, "requestId", requestId) ||
             !readStringField(message, "ownerToken", ownerToken) ) {
            rejectClient(websocketId, "invalid_accept");
            return;
        }

        const auto roomIterator    = m_rooms.find(roomId);
        const auto pendingIterator = m_pendingJoins.find(requestId);
        if ( roomIterator == m_rooms.end() ||
             pendingIterator == m_pendingJoins.end() ||
             pendingIterator->second.roomId != roomId ||
             roomIterator->second.ownerToken != ownerToken ) {
            rejectClient(websocketId, "accept_not_authorized");
            return;
        }

        const int  guestWebSocketId = pendingIterator->second.guestWebSocketId;
        const auto guestIterator    = m_clients.find(guestWebSocketId);
        if ( guestIterator == m_clients.end() ||
             guestIterator->second.role != ClientRole::PendingGuest ) {
            m_pendingJoins.erase(pendingIterator);
            rejectClient(websocketId, "guest_unavailable");
            return;
        }

        Client& hostPeer                         = m_clients.at(websocketId);
        hostPeer.role                            = ClientRole::Paired;
        hostPeer.roomId                          = roomId;
        hostPeer.partnerWebSocketId              = guestWebSocketId;
        guestIterator->second.role               = ClientRole::Paired;
        guestIterator->second.partnerWebSocketId = websocketId;
        m_pendingJoins.erase(pendingIterator);

        nlohmann::json ready;
        ready["type"]      = "relay_ready";
        ready["version"]   = DIRECTORY_PROTOCOL_VERSION;
        ready["roomId"]    = roomId;
        ready["requestId"] = requestId;
        appendIceServers(ready);
        if ( !sendJson(websocketId, ready) ||
             !sendJson(guestWebSocketId, ready) ) {
            closeClient(websocketId);
            closeClient(guestWebSocketId);
            return;
        }
        m_directoryDirty = true;
    }

    /// @brief 处理 WebSocket 关闭并同步清理房间或配对端。
    void processClosed(int websocketId, std::uint64_t generation)
    {
        const auto iterator = m_clients.find(websocketId);
        if ( iterator == m_clients.end() ||
             iterator->second.generation != generation ) {
            return;
        }
        const Client client = iterator->second;

        if ( client.role == ClientRole::HostControl ) {
            removeRoom(client.roomId, websocketId);
        } else if ( client.role == ClientRole::PendingGuest ) {
            m_pendingJoins.erase(client.requestId);
        } else if ( client.role == ClientRole::Paired ) {
            const auto partnerIterator =
                m_clients.find(client.partnerWebSocketId);
            if ( partnerIterator != m_clients.end() &&
                 partnerIterator->second.role == ClientRole::Paired ) {
                partnerIterator->second.role = ClientRole::Closing;
                partnerIterator->second.partnerWebSocketId = -1;
                static_cast<void>(rtcClose(client.partnerWebSocketId));
            }
        }

        m_clients.erase(websocketId);
        retireWebSocket(websocketId, generation);
    }

    /// @brief 删除房主控制连接对应的全部房间状态。
    void removeRoom(const std::string& roomId, int controlWebSocketId)
    {
        const auto roomIterator = m_rooms.find(roomId);
        if ( roomIterator == m_rooms.end() ) return;

        std::vector<int> clientsToClose;
        for ( auto& [clientId, client] : m_clients ) {
            if ( clientId != controlWebSocketId && client.roomId == roomId &&
                 client.role != ClientRole::Closing ) {
                client.role = ClientRole::Closing;
                clientsToClose.push_back(clientId);
            }
        }
        for ( const auto& [requestId, pending] : m_pendingJoins ) {
            static_cast<void>(requestId);
            if ( pending.roomId == roomId ) {
                const auto clientIterator =
                    m_clients.find(pending.guestWebSocketId);
                if ( clientIterator != m_clients.end() &&
                     clientIterator->second.role != ClientRole::Closing ) {
                    clientIterator->second.role = ClientRole::Closing;
                    clientsToClose.push_back(pending.guestWebSocketId);
                }
            }
        }
        std::erase_if(m_pendingJoins, [&roomId](const auto& entry) {
            return entry.second.roomId == roomId;
        });
        m_rooms.erase(roomIterator);
        for ( int clientId : clientsToClose ) {
            static_cast<void>(rtcClose(clientId));
        }
        m_directoryDirty = true;
        XINFO("Collaboration room {} removed", roomId);
    }

    /// @brief 构造当前公开房间目录消息。
    nlohmann::json makeRoomList() const
    {
        std::vector<const Room*> orderedRooms;
        orderedRooms.reserve(m_rooms.size());
        for ( const auto& [roomId, room] : m_rooms ) {
            static_cast<void>(roomId);
            orderedRooms.push_back(&room);
        }
        std::sort(orderedRooms.begin(),
                  orderedRooms.end(),
                  [](const Room* left, const Room* right) {
                      return left->creationSequence < right->creationSequence;
                  });

        nlohmann::json rooms = nlohmann::json::array();
        for ( const Room* room : orderedRooms ) {
            nlohmann::json item;
            item["roomId"]       = room->roomId;
            item["roomName"]     = room->roomName;
            item["hostCreator"]  = room->hostCreator;
            item["participants"] = room->participants;
            item["capacity"]     = room->capacity;
            rooms.push_back(std::move(item));
        }

        nlohmann::json message;
        message["type"]    = "room_list";
        message["version"] = DIRECTORY_PROTOCOL_VERSION;
        message["rooms"]   = std::move(rooms);
        return message;
    }

    /// @brief 向单个目录客户端发送房间列表。
    void sendRoomList(int websocketId) const
    {
        static_cast<void>(sendJson(websocketId, makeRoomList()));
    }

    /// @brief 向所有目录订阅者广播最新房间列表。
    void broadcastRoomList()
    {
        const auto message = makeRoomList();
        for ( const auto& [websocketId, client] : m_clients ) {
            if ( client.role == ClientRole::Directory ) {
                static_cast<void>(sendJson(websocketId, message));
            }
        }
        m_directoryDirty = false;
    }

    /// @brief 清理未声明身份连接和过期加入请求。
    void expireIdleClientsAndJoins()
    {
        const auto       now = std::chrono::steady_clock::now();
        std::vector<int> clientsToClose;
        for ( const auto& [websocketId, client] : m_clients ) {
            if ( client.role == ClientRole::Unknown &&
                 now - client.lastActivity > UNKNOWN_CLIENT_TIMEOUT ) {
                clientsToClose.push_back(websocketId);
            }
        }
        for ( const auto& [requestId, pending] : m_pendingJoins ) {
            static_cast<void>(requestId);
            if ( now - pending.createdAt > JOIN_REQUEST_TIMEOUT ) {
                clientsToClose.push_back(pending.guestWebSocketId);
            }
        }
        for ( int websocketId : clientsToClose ) closeClient(websocketId);
    }

    /// @brief 从 libdatachannel 线程安全提交回调事件。
    void enqueueCallbackEvent(CallbackEvent event)
    {
        if ( !m_acceptCallbacks.load(std::memory_order_acquire) ) return;
        std::scoped_lock lock(m_callbackMutex);
        if ( m_callbackEvents.size() >= MAX_CALLBACK_EVENTS ) return;
        m_callbackEvents.push_back(std::move(event));
    }

    /// @brief 接收新的 WebSocket 客户端。
    static void onWebSocketClient(int, int websocketId, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        if ( !owner ||
             !owner->m_acceptCallbacks.load(std::memory_order_acquire) ) {
            rtcDeleteWebSocket(websocketId);
            return;
        }
        CallbackContext* contextPointer = nullptr;
        {
            std::scoped_lock lock(owner->m_callbackMutex);
            if ( owner->m_callbackEvents.size() >= MAX_CALLBACK_EVENTS ) {
                rtcDeleteWebSocket(websocketId);
                return;
            }
            auto context         = std::make_unique<CallbackContext>();
            context->owner       = owner;
            context->websocketId = websocketId;
            context->generation  = ++owner->m_connectionGeneration;
            contextPointer       = context.get();
            owner->m_callbackContexts.emplace(context->generation,
                                              std::move(context));
            rtcSetUserPointer(websocketId, contextPointer);
            rtcSetClosedCallback(websocketId, &Impl::onWebSocketClosed);
            rtcSetErrorCallback(websocketId, &Impl::onWebSocketError);
            rtcSetMessageCallback(websocketId, &Impl::onWebSocketMessage);
            owner->m_callbackEvents.push_back({ CallbackEventType::Connected,
                                                websocketId,
                                                contextPointer->generation,
                                                {},
                                                true });
        }
    }

    /// @brief 接收 WebSocket 消息并复制到主循环队列。
    static void onWebSocketMessage(int websocketId, const char* message,
                                   int size, void* pointer)
    {
        auto* context = static_cast<CallbackContext*>(pointer);
        if ( !context || !context->owner || !message ) return;
        CallbackEvent event;
        event.type        = CallbackEventType::Message;
        event.websocketId = websocketId;
        event.generation  = context->generation;
        event.textMessage = size < 0;
        event.payload     = size < 0 ? std::string(message)
                                     : std::string(message, message + size);
        context->owner->enqueueCallbackEvent(std::move(event));
    }

    /// @brief 把 WebSocket 关闭事件提交到主循环。
    static void onWebSocketClosed(int websocketId, void* pointer)
    {
        auto* context = static_cast<CallbackContext*>(pointer);
        if ( context && context->owner ) {
            context->owner->enqueueCallbackEvent({ CallbackEventType::Closed,
                                                   websocketId,
                                                   context->generation,
                                                   {},
                                                   true });
        }
    }

    /// @brief 把 WebSocket 错误事件提交到主循环。
    static void onWebSocketError(int websocketId, const char* error,
                                 void* pointer)
    {
        auto* context = static_cast<CallbackContext*>(pointer);
        if ( context && context->owner ) {
            context->owner->enqueueCallbackEvent(
                { CallbackEventType::Error,
                  websocketId,
                  context->generation,
                  error ? error : "websocket_error",
                  true });
        }
    }

    /// @brief 服务启动配置。
    CollaborationSignalingServerConfig m_config;
    /// @brief 服务是否正在运行。
    std::atomic_bool m_running{ false };
    /// @brief 回调是否仍可访问当前实例。
    std::atomic_bool m_acceptCallbacks{ false };
    /// @brief WebSocketServer 句柄。
    int m_serverId = -1;
    /// @brief 实际监听端口。
    std::uint16_t m_listeningPort = 0;
    /// @brief 主循环维护的客户端表。
    std::unordered_map<int, Client> m_clients;
    /// @brief 公共房间表。
    std::unordered_map<std::string, Room> m_rooms;
    /// @brief 等待房主接受的请求表。
    std::unordered_map<std::string, PendingJoin> m_pendingJoins;
    /// @brief 是否需要向目录订阅者广播。
    bool m_directoryDirty = false;
    /// @brief 房间创建顺序计数器。
    std::uint64_t m_roomSequence = 0;
    /// @brief 生成房间与请求标识的随机引擎。
    std::mt19937_64 m_random{ std::random_device{}() };
    /// @brief 保护跨线程回调队列。
    std::recursive_mutex m_callbackMutex;
    /// @brief 等待主循环处理的回调事件。
    std::deque<CallbackEvent> m_callbackEvents;
    /// @brief 等待跨过回调迟到窗口后删除的 WebSocket。
    std::deque<RetiredWebSocket> m_retiredWebSockets;
    /// @brief 为每次连接保留到句柄删除的稳定回调上下文。
    std::unordered_map<std::uint64_t, std::unique_ptr<CallbackContext>>
        m_callbackContexts;
    /// @brief 分配连接代次的单调计数器。
    std::uint64_t m_connectionGeneration = 0;
};

CollaborationSignalingServer::CollaborationSignalingServer()
    : m_impl(std::make_unique<Impl>())
{
}

CollaborationSignalingServer::~CollaborationSignalingServer() = default;

bool CollaborationSignalingServer::start(
    CollaborationSignalingServerConfig config)
{
    return m_impl->start(std::move(config));
}

void CollaborationSignalingServer::stop()
{
    m_impl->stop();
}

void CollaborationSignalingServer::update()
{
    m_impl->update();
}

bool CollaborationSignalingServer::isRunning() const
{
    return m_impl->isRunning();
}

std::uint16_t CollaborationSignalingServer::listeningPort() const
{
    return m_impl->listeningPort();
}

std::size_t CollaborationSignalingServer::roomCount() const
{
    return m_impl->roomCount();
}

std::size_t CollaborationSignalingServer::clientCount() const
{
    return m_impl->clientCount();
}
}  // namespace MMM::Network::CollaborationServer
