#include "network/collaboration/CollaborationDirectoryClient.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.h>

#include <atomic>
#include <cctype>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

namespace MMM::Network::Collaboration
{
namespace
{
/// @brief 公网目录协议版本。
constexpr std::uint64_t DIRECTORY_PROTOCOL_VERSION = 1;
/// @brief 单条目录消息大小上限。
constexpr int MAX_DIRECTORY_MESSAGE_BYTES = 256 * 1024;
/// @brief 回调线程允许积压的目录事件数量。
constexpr std::size_t MAX_DIRECTORY_EVENTS = 1024;
/// @brief 单次列表允许接收的房间数量。
constexpr std::size_t MAX_DIRECTORY_ROOMS = 256;
/// @brief 单间房卡封面 Base64 文本上限。
constexpr std::size_t MAX_ROOM_COVER_BASE64_BYTES = 96U * 1024U;

/// @brief 校验服务器地址未混入协议、端口或路径。
bool isValidServerAddress(std::string_view value)
{
    if ( value.empty() || value.size() > 253U || value.contains('/') ||
         value.contains("//") || value.starts_with("ws:") ||
         value.starts_with("wss:") ) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    });
}

/// @brief 从 JSON 对象读取字符串字段。
bool readString(const nlohmann::json& object, std::string_view key,
                std::string& output)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_string() ) return false;
    output = iterator->get_ref<const std::string&>();
    return true;
}
}  // namespace

class CollaborationDirectoryClient::Impl
{
public:
    /// @brief 回调线程提交给 UI 线程的事件类型。
    enum class EventType {
        Opened,
        Message,
        Closed,
        Error,
    };

    /// @brief 一条跨线程目录事件。
    struct Event {
        /// @brief 事件类型。
        EventType type = EventType::Error;
        /// @brief 文本消息或错误详情。
        std::string payload;
    };

    /// @brief 析构时关闭 WebSocket。
    ~Impl() { disconnect(); }

    /// @brief 开始连接目录入口。
    /// @warning libdatachannel C API 尚未暴露 CA 注入，当前 mbedTLS
    /// 预编译库无法建立系统信任链，因此 WSS 暂时只提供传输加密。
    bool connect(CollaborationServerEndpoint endpoint)
    {
        std::scoped_lock  rtcLock(m_rtcApiMutex);
        const std::string signalingUrl =
            makeCollaborationSignalingUrl(endpoint);
        if ( signalingUrl.empty() || m_websocketId >= 0 ) {
            return false;
        }
        m_endpoint     = std::move(endpoint);
        m_signalingUrl = signalingUrl;
        m_lastError.clear();
        m_rooms.clear();
        m_roomCovers.clear();
        m_requestedRoomCovers.clear();
        m_state = CollaborationDirectoryState::Connecting;
        m_openHandled.store(false, std::memory_order_release);
        m_acceptCallbacks.store(true, std::memory_order_release);

        rtcWsConfiguration config{};
        config.disableTlsVerification = m_endpoint.useTls;
        config.connectionTimeoutMs    = 10000;
        config.pingIntervalMs         = 10000;
        config.maxOutstandingPings    = 3;
        config.maxMessageSize         = MAX_DIRECTORY_MESSAGE_BYTES;
        const int websocketId =
            rtcCreateWebSocketEx(m_signalingUrl.c_str(), &config);
        if ( websocketId < 0 ) {
            m_acceptCallbacks.store(false, std::memory_order_release);
            m_state     = CollaborationDirectoryState::Error;
            m_lastError = "directory_connect_start_failed";
            return false;
        }
        m_websocketId = websocketId;
        rtcSetUserPointer(websocketId, this);
        rtcSetOpenCallback(websocketId, &Impl::onOpen);
        rtcSetMessageCallback(websocketId, &Impl::onMessage);
        rtcSetClosedCallback(websocketId, &Impl::onClosed);
        rtcSetErrorCallback(websocketId, &Impl::onError);
        if ( rtcIsOpen(websocketId) ) onOpen(websocketId, this);
        return true;
    }

    /// @brief 关闭目录连接并清空状态。
    void disconnect()
    {
        int websocketId = -1;
        {
            std::scoped_lock rtcLock(m_rtcApiMutex);
            m_acceptCallbacks.store(false, std::memory_order_release);
            websocketId = std::exchange(m_websocketId, -1);
            if ( websocketId >= 0 ) {
                detachWebSocketCallbacks(websocketId);
                rtcDeleteWebSocket(websocketId);
            }
        }
        {
            std::scoped_lock lock(m_eventMutex);
            m_events.clear();
        }
        m_rooms.clear();
        m_state = CollaborationDirectoryState::Idle;
        m_lastError.clear();
        m_openHandled.store(false, std::memory_order_release);
    }

    /// @brief 消费回调事件并更新目录快照。
    void update()
    {
        std::deque<Event> events;
        {
            std::scoped_lock lock(m_eventMutex);
            events.swap(m_events);
        }
        for ( auto& event : events ) {
            switch ( event.type ) {
            case EventType::Opened:
                if ( !sendListRequest() ) fail("directory_list_send_failed");
                break;
            case EventType::Message: processMessage(event.payload); break;
            case EventType::Closed:
                if ( m_state != CollaborationDirectoryState::Idle ) {
                    fail("directory_connection_closed");
                }
                break;
            case EventType::Error:
                fail(event.payload.empty() ? "directory_websocket_error"
                                           : std::move(event.payload));
                break;
            }
        }
    }

    /// @brief 请求刷新房间列表。
    bool refresh() const
    {
        return m_state == CollaborationDirectoryState::Connected &&
               sendListRequest();
    }

    /// @brief 为仍在目录中的房间按需请求一次封面。
    bool requestRoomCover(std::string_view roomId)
    {
        const auto room = std::find_if(
            m_rooms.begin(), m_rooms.end(), [roomId](const auto& candidate) {
                return candidate.roomId == roomId;
            });
        if ( room == m_rooms.end() || !room->hasCoverImage ) return false;
        if ( m_roomCovers.contains(roomId) ||
             m_requestedRoomCovers.contains(roomId) ) {
            return true;
        }

        const nlohmann::json request = {
            { "type", "get_room_cover" },
            { "version", DIRECTORY_PROTOCOL_VERSION },
            { "roomId", roomId },
        };
        const std::string payload = request.dump();
        std::scoped_lock  rtcLock(m_rtcApiMutex);
        if ( !m_acceptCallbacks.load(std::memory_order_acquire) ||
             m_websocketId < 0 || !rtcIsOpen(m_websocketId) ||
             rtcSendMessage(m_websocketId, payload.c_str(), -1) !=
                 RTC_ERR_SUCCESS ) {
            return false;
        }
        m_requestedRoomCovers.emplace(roomId);
        return true;
    }

    /// @brief 返回目录状态。
    CollaborationDirectoryState state() const { return m_state; }

    /// @brief 返回房间快照。
    const std::vector<CollaborationDirectoryRoom>& rooms() const
    {
        return m_rooms;
    }

    /// @brief 返回一份已经取得的房间封面。
    std::string_view roomCover(std::string_view roomId) const
    {
        const auto iterator = m_roomCovers.find(roomId);
        return iterator == m_roomCovers.end()
                   ? std::string_view{}
                   : std::string_view(iterator->second);
    }

    /// @brief 返回最近错误。
    const std::string& lastError() const { return m_lastError; }

    /// @brief 返回当前入口 URL。
    const CollaborationServerEndpoint& endpoint() const { return m_endpoint; }

private:
    /// @brief 在删除 WebSocket 前切断全部回调和用户指针。
    static void detachWebSocketCallbacks(int websocketId)
    {
        rtcSetOpenCallback(websocketId, nullptr);
        rtcSetMessageCallback(websocketId, nullptr);
        rtcSetClosedCallback(websocketId, nullptr);
        rtcSetErrorCallback(websocketId, nullptr);
        rtcSetUserPointer(websocketId, nullptr);
    }

    /// @brief 发送房间列表订阅请求。
    bool sendListRequest() const
    {
        std::scoped_lock rtcLock(m_rtcApiMutex);
        if ( !m_acceptCallbacks.load(std::memory_order_acquire) ||
             m_websocketId < 0 || !rtcIsOpen(m_websocketId) ) {
            return false;
        }
        const nlohmann::json request = {
            { "type", "list_rooms" },
            { "version", DIRECTORY_PROTOCOL_VERSION },
        };
        const std::string payload = request.dump();
        return rtcSendMessage(m_websocketId, payload.c_str(), -1) ==
               RTC_ERR_SUCCESS;
    }

    /// @brief 校验并替换一份服务端房间列表。
    void processMessage(std::string_view payload)
    {
        const auto message = nlohmann::json::parse(payload, nullptr, false);
        if ( !message.is_object() ||
             message.value("version", std::uint64_t{ 0 }) !=
                 DIRECTORY_PROTOCOL_VERSION ) {
            fail("invalid_directory_message");
            return;
        }
        const std::string type = message.value("type", "");
        if ( type == "room_cover" ) {
            processRoomCover(message);
            return;
        }
        if ( type != "room_list" ) {
            fail("invalid_directory_message");
            return;
        }

        processRoomList(message);
    }

    /// @brief 校验并替换一份服务端房间列表。
    void processRoomList(const nlohmann::json& message)
    {
        const auto roomsIterator = message.find("rooms");
        if ( roomsIterator == message.end() || !roomsIterator->is_array() ||
             roomsIterator->size() > MAX_DIRECTORY_ROOMS ) {
            fail("invalid_room_list");
            return;
        }

        std::vector<CollaborationDirectoryRoom> rooms;
        rooms.reserve(roomsIterator->size());
        for ( const auto& item : *roomsIterator ) {
            CollaborationDirectoryRoom room;
            const auto                 participants = item.find("participants");
            const auto                 capacity     = item.find("capacity");
            if ( !item.is_object() ||
                 !readString(item, "roomId", room.roomId) ||
                 !readString(item, "roomName", room.roomName) ||
                 !readString(item, "hostCreator", room.hostCreator) ||
                 participants == item.end() ||
                 !participants->is_number_unsigned() ||
                 capacity == item.end() || !capacity->is_number_unsigned() ) {
                fail("invalid_room_entry");
                return;
            }
            room.participants = participants->get<std::size_t>();
            room.capacity     = capacity->get<std::size_t>();
            if ( const auto cover = item.find("hasCoverImage");
                 cover != item.end() ) {
                if ( !cover->is_boolean() ) {
                    fail("invalid_room_entry");
                    return;
                }
                room.hasCoverImage = cover->get<bool>();
            }
            if ( room.roomId.empty() || room.roomName.empty() ||
                 room.hostCreator.empty() || room.capacity < 2U ||
                 room.capacity > 8U || room.participants == 0U ||
                 room.participants > room.capacity ) {
                fail("invalid_room_entry");
                return;
            }
            rooms.push_back(std::move(room));
        }
        m_rooms = std::move(rooms);
        for ( auto iterator = m_roomCovers.begin();
              iterator != m_roomCovers.end(); ) {
            const auto room = std::find_if(
                m_rooms.begin(), m_rooms.end(), [&](const auto& candidate) {
                    return candidate.roomId == iterator->first &&
                           candidate.hasCoverImage;
                });
            if ( room == m_rooms.end() ) {
                m_requestedRoomCovers.erase(iterator->first);
                iterator = m_roomCovers.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for ( auto iterator = m_requestedRoomCovers.begin();
              iterator != m_requestedRoomCovers.end(); ) {
            const auto room = std::find_if(
                m_rooms.begin(), m_rooms.end(), [&](const auto& candidate) {
                    return candidate.roomId == *iterator &&
                           candidate.hasCoverImage;
                });
            iterator = room == m_rooms.end()
                           ? m_requestedRoomCovers.erase(iterator)
                           : std::next(iterator);
        }
        m_state = CollaborationDirectoryState::Connected;
        m_lastError.clear();
    }

    /// @brief 校验并缓存一份按需返回的房间封面。
    void processRoomCover(const nlohmann::json& message)
    {
        std::string roomId;
        std::string coverImage;
        if ( !readString(message, "roomId", roomId) ||
             !readString(message, "coverImage", coverImage) || roomId.empty() ||
             roomId.size() > 32U ||
             coverImage.size() > MAX_ROOM_COVER_BASE64_BYTES ) {
            fail("invalid_room_cover");
            return;
        }

        const auto room = std::find_if(
            m_rooms.begin(), m_rooms.end(), [&](const auto& candidate) {
                return candidate.roomId == roomId;
            });
        if ( room == m_rooms.end() ) {
            m_requestedRoomCovers.erase(roomId);
            return;
        }
        if ( coverImage.empty() ) {
            m_requestedRoomCovers.erase(roomId);
            return;
        }
        m_requestedRoomCovers.erase(roomId);
        m_roomCovers.insert_or_assign(std::move(roomId), std::move(coverImage));
    }

    /// @brief 切换到错误状态并保留诊断文本。
    void fail(std::string error)
    {
        m_state     = CollaborationDirectoryState::Error;
        m_lastError = std::move(error);
    }

    /// @brief 有界压入回调事件。
    void enqueue(Event event)
    {
        if ( !m_acceptCallbacks.load(std::memory_order_acquire) ) return;
        std::scoped_lock lock(m_eventMutex);
        if ( m_events.size() >= MAX_DIRECTORY_EVENTS ) return;
        m_events.push_back(std::move(event));
    }

    /// @brief WebSocket 打开回调。
    static void onOpen(int, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        if ( owner &&
             !owner->m_openHandled.exchange(true, std::memory_order_acq_rel) ) {
            owner->enqueue({ EventType::Opened, {} });
        }
    }

    /// @brief WebSocket 文本消息回调。
    static void onMessage(int, const char* message, int size, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        if ( owner && message && size < 0 ) {
            owner->enqueue({ EventType::Message, std::string(message) });
        }
    }

    /// @brief WebSocket 关闭回调。
    static void onClosed(int, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        if ( owner ) owner->enqueue({ EventType::Closed, {} });
    }

    /// @brief WebSocket 错误回调。
    static void onError(int, const char* error, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        if ( owner ) {
            owner->enqueue({ EventType::Error,
                             error ? error : "directory_websocket_error" });
        }
    }

    /// @brief 当前 WebSocket 句柄。
    int m_websocketId = -1;
    /// @brief 串行化目录句柄的发送、退役与删除前解绑。
    mutable std::recursive_mutex m_rtcApiMutex;
    /// @brief 是否仍接受第三方线程回调。
    std::atomic_bool m_acceptCallbacks{ false };
    /// @brief 防止打开回调与注册后的状态补检重复发送目录请求。
    std::atomic_bool m_openHandled{ false };
    /// @brief 当前目录状态。
    CollaborationDirectoryState m_state = CollaborationDirectoryState::Idle;
    /// @brief 当前目录 URL。
    std::string m_signalingUrl;
    /// @brief 当前中心服务器结构化配置。
    CollaborationServerEndpoint m_endpoint;
    /// @brief 最近协议或连接错误。
    std::string m_lastError;
    /// @brief 最近一次有效房间快照。
    std::vector<CollaborationDirectoryRoom> m_rooms;
    /// @brief 已经按需取得的房间封面，随目录房间消失而清理。
    std::map<std::string, std::string, std::less<>> m_roomCovers;
    /// @brief 已发送但尚未收到响应的房间封面请求。
    std::set<std::string, std::less<>> m_requestedRoomCovers;
    /// @brief 保护回调事件队列。
    std::mutex m_eventMutex;
    /// @brief 等待 UI 线程消费的事件。
    std::deque<Event> m_events;
};

CollaborationDirectoryClient::CollaborationDirectoryClient()
    : m_impl(std::make_unique<Impl>())
{
}

CollaborationDirectoryClient::~CollaborationDirectoryClient() = default;

std::string makeCollaborationSignalingUrl(
    const CollaborationServerEndpoint& endpoint)
{
    if ( !isValidServerAddress(endpoint.address) ||
         endpoint.signalingPort == 0 ) {
        return {};
    }
    std::string address = endpoint.address;
    if ( address.contains(':') &&
         !(address.starts_with('[') && address.ends_with(']')) ) {
        address = '[' + address + ']';
    }
    return std::string(endpoint.useTls ? "wss://" : "ws://") + address + ':' +
           std::to_string(endpoint.signalingPort) + "/mmm-collaboration";
}

bool CollaborationDirectoryClient::connect(CollaborationServerEndpoint endpoint)
{
    return m_impl->connect(std::move(endpoint));
}

void CollaborationDirectoryClient::disconnect()
{
    m_impl->disconnect();
}

void CollaborationDirectoryClient::update()
{
    m_impl->update();
}

bool CollaborationDirectoryClient::refresh()
{
    return m_impl->refresh();
}

bool CollaborationDirectoryClient::requestRoomCover(std::string_view roomId)
{
    return m_impl->requestRoomCover(roomId);
}

CollaborationDirectoryState CollaborationDirectoryClient::state() const
{
    return m_impl->state();
}

const std::vector<CollaborationDirectoryRoom>&
CollaborationDirectoryClient::rooms() const
{
    return m_impl->rooms();
}

std::string_view CollaborationDirectoryClient::roomCover(
    std::string_view roomId) const
{
    return m_impl->roomCover(roomId);
}

const std::string& CollaborationDirectoryClient::lastError() const
{
    return m_impl->lastError();
}

const CollaborationServerEndpoint&
CollaborationDirectoryClient::endpoint() const
{
    return m_impl->endpoint();
}
}  // namespace MMM::Network::Collaboration
