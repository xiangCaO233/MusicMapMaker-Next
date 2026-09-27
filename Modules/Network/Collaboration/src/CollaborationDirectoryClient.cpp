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
/// @note 客户端拒绝其他版本的服务端消息，避免按错误字段布局更新 UI。
constexpr std::uint64_t DIRECTORY_PROTOCOL_VERSION = 1;
/// @brief 单条目录消息大小上限。
/// @note 同时传给 libdatachannel，防止回调层接收无界 JSON 文本。
constexpr int MAX_DIRECTORY_MESSAGE_BYTES = 256 * 1024;
/// @brief 回调线程允许积压的目录事件数量。
/// @note 队列满时丢弃新事件，避免 UI 停顿期间无界占用内存。
constexpr std::size_t MAX_DIRECTORY_EVENTS = 1024;
/// @brief 单次列表允许接收的房间数量。
/// @note 在 reserve 前检查，限制服务端输入驱动的内存分配。
constexpr std::size_t MAX_DIRECTORY_ROOMS = 256;
/// @brief 单间房卡封面 Base64 文本上限。
/// @note 与服务端发布限制保持一致，缓存前再次执行客户端防御校验。
constexpr std::size_t MAX_ROOM_COVER_BASE64_BYTES = 96U * 1024U;

/// @brief 校验服务器地址未混入协议、端口或路径。
/// @param value 用户配置的纯域名、IPv4 或 IPv6 地址文本。
/// @return 非空、长度受限且不含协议、路径或空白时返回 true。
bool isValidServerAddress(std::string_view value)
{
    // 地址字段只承载 host，协议和固定路径由 URL 构造器统一添加。
    if ( value.empty() || value.size() > 253U || value.contains('/') ||
         value.contains("//") || value.starts_with("ws:") ||
         value.starts_with("wss:") ) {
        return false;
    }
    // 任何空白都可能改变 URL 解析或隐藏额外内容，按字节拒绝。
    return std::none_of(value.begin(), value.end(), [](char character) {
        // cctype 要求 unsigned char 范围，显式转换避免负 char 未定义行为。
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    });
}

/// @brief 从 JSON 对象读取字符串字段。
/// @param object 已确认或待确认的协议 JSON 对象。
/// @param key 必需字段名。
/// @param output 成功时接收独立字符串副本。
/// @return 字段存在且类型为字符串时返回 true。
bool readString(const nlohmann::json& object, std::string_view key,
                std::string& output)
{
    // find 避免缺失字段经 at 抛异常，符合协议解析的无异常失败路径。
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_string() ) return false;
    // 输出复制后不依赖临时 JSON 消息的生命周期。
    output = iterator->get_ref<const std::string&>();
    return true;
}
}  // namespace

class CollaborationDirectoryClient::Impl
{
public:
    /// @brief 回调线程提交给 UI 线程的事件类型。
    /// @note 第三方回调只产生事件，不直接修改目录状态或房间容器。
    enum class EventType {
        Opened,   ///< WebSocket 握手完成，需要发送首次列表订阅。
        Message,  ///< 收到待在 UI 线程解析的文本协议帧。
        Closed,   ///< 远端或本地关闭连接。
        Error,    ///< 第三方连接错误及可选诊断文本。
    };

    /// @brief 一条跨线程目录事件。
    struct Event {
        /// @brief 事件类型。
        /// @note 默认 Error 防止值初始化事件被误当作成功状态。
        EventType type = EventType::Error;
        /// @brief 文本消息或错误详情。
        /// @note Opened 与 Closed 不携带文本，保持为空。
        std::string payload;
    };

    /// @brief 析构时关闭 WebSocket。
    /// @note disconnect 幂等处理未连接和已经断开的状态。
    ~Impl()
    {
        // 在成员销毁前退役第三方回调，避免 user pointer 指向失效对象。
        disconnect();
    }

    /// @brief 开始连接目录入口。
    /// @param endpoint 结构化地址、端口与 TLS 选项。
    /// @return URL 合法且第三方 WebSocket 创建成功时返回 true。
    /// @warning libdatachannel C API 尚未暴露 CA 注入，当前 mbedTLS
    /// 预编译库无法建立系统信任链，因此 WSS 暂时只提供传输加密。
    bool connect(CollaborationServerEndpoint endpoint)
    {
        // 串行化句柄创建与 disconnect，防止并发删除刚建立的连接。
        std::scoped_lock rtcLock(m_rtcApiMutex);
        // URL 在修改现有状态前完成校验和构造。
        const std::string signalingUrl =
            makeCollaborationSignalingUrl(endpoint);
        // 同一客户端只允许一个活动句柄，重连前必须显式 disconnect。
        if ( signalingUrl.empty() || m_websocketId >= 0 ) {
            return false;
        }
        // 保存值语义端点和规范 URL，供 UI 查询及第三方连接使用。
        m_endpoint     = std::move(endpoint);
        m_signalingUrl = signalingUrl;
        m_lastError.clear();
        // 新连接不继承旧服务的目录、封面或已发送请求集合。
        m_rooms.clear();
        m_roomCovers.clear();
        m_requestedRoomCovers.clear();
        m_state = CollaborationDirectoryState::Connecting;
        // 打开状态与回调接受开关先发布，再创建可能立即回调的句柄。
        m_openHandled.store(false, std::memory_order_release);
        m_acceptCallbacks.store(true, std::memory_order_release);

        // WebSocket 配置值初始化，未指定字段保持 libdatachannel 默认值。
        rtcWsConfiguration config{};
        // 当前预编译 TLS 无系统信任链，因此 WSS 暂时禁用证书校验。
        config.disableTlsVerification = m_endpoint.useTls;
        // 连接与心跳均使用十秒窗口，连续三次 ping 无响应由库判定断开。
        config.connectionTimeoutMs = 10000;
        config.pingIntervalMs      = 10000;
        config.maxOutstandingPings = 3;
        // 在第三方分配消息缓冲前应用协议单帧上限。
        config.maxMessageSize = MAX_DIRECTORY_MESSAGE_BYTES;
        const int websocketId =
            rtcCreateWebSocketEx(m_signalingUrl.c_str(), &config);
        if ( websocketId < 0 ) {
            // 句柄未建立时先停止接收回调，再发布同步错误状态。
            m_acceptCallbacks.store(false, std::memory_order_release);
            m_state     = CollaborationDirectoryState::Error;
            m_lastError = "directory_connect_start_failed";
            return false;
        }
        // 保存句柄后绑定稳定 Impl 地址，再注册全部回调。
        m_websocketId = websocketId;
        rtcSetUserPointer(websocketId, this);
        rtcSetOpenCallback(websocketId, &Impl::onOpen);
        rtcSetMessageCallback(websocketId, &Impl::onMessage);
        rtcSetClosedCallback(websocketId, &Impl::onClosed);
        rtcSetErrorCallback(websocketId, &Impl::onError);
        // 握手可能早于回调注册完成，补检确保 Opened 事件不会遗漏。
        if ( rtcIsOpen(websocketId) ) onOpen(websocketId, this);
        return true;
    }

    /// @brief 关闭目录连接并清空状态。
    /// @warning 低频生命周期路径；可能调用第三方句柄删除，不得进入 UI 热路径。
    void disconnect()
    {
        // 句柄值在锁内交换为无效，使其他发送路径立即观察到退役。
        int websocketId = -1;
        {
            std::scoped_lock rtcLock(m_rtcApiMutex);
            // 先拒绝新回调入队，再解绑第三方函数指针和 user pointer。
            m_acceptCallbacks.store(false, std::memory_order_release);
            websocketId = std::exchange(m_websocketId, -1);
            if ( websocketId >= 0 ) {
                detachWebSocketCallbacks(websocketId);
                // 回调全部断开后才能删除底层句柄。
                rtcDeleteWebSocket(websocketId);
            }
        }
        {
            // 清除已经排队但尚未由 UI 线程消费的旧连接事件。
            std::scoped_lock lock(m_eventMutex);
            m_events.clear();
        }
        // 对外快照和错误同时恢复为未连接状态，封面缓存下次 connect 再清理。
        m_rooms.clear();
        m_state = CollaborationDirectoryState::Idle;
        m_lastError.clear();
        m_openHandled.store(false, std::memory_order_release);
    }

    /// @brief 消费回调事件并更新目录快照。
    /// @warning UI 每帧调用；只交换有界内存队列，禁止等待、文件 I/O
    /// 和联网阻塞。
    void update()
    {
        // 用局部 deque 一次交换积压事件，最小化回调线程持锁等待。
        std::deque<Event> events;
        {
            std::scoped_lock lock(m_eventMutex);
            events.swap(m_events);
        }
        // 所有状态和目录容器变更都在调用 update 的线程串行发生。
        for ( auto& event : events ) {
            switch ( event.type ) {
            case EventType::Opened:
                // 首次打开立即订阅列表；发送失败转为稳定错误状态。
                if ( !sendListRequest() ) fail("directory_list_send_failed");
                break;
            case EventType::Message:
                // 协议 JSON 解析延迟到 UI 线程，回调保持轻量。
                processMessage(event.payload);
                break;
            case EventType::Closed:
                // 主动 disconnect 已将状态设为 Idle，不应再覆盖为错误。
                if ( m_state != CollaborationDirectoryState::Idle ) {
                    fail("directory_connection_closed");
                }
                break;
            case EventType::Error:
                // 空第三方错误统一映射为稳定内部标识，非空详情按值移动。
                fail(event.payload.empty() ? "directory_websocket_error"
                                           : std::move(event.payload));
                break;
            }
        }
    }

    /// @brief 请求刷新房间列表。
    /// @return 已连接且列表请求成功写入 WebSocket 时返回 true。
    /// @warning UI 交互路径低频调用；不等待服务端响应。
    bool refresh() const
    {
        // 只有已完成首次列表握手的连接允许显式刷新。
        return m_state == CollaborationDirectoryState::Connected &&
               sendListRequest();
    }

    /// @brief 为仍在目录中的房间按需请求一次封面。
    /// @param roomId 当前目录快照中的公开房间标识。
    /// @return 已缓存、已请求或本次成功发送请求时返回 true。
    bool requestRoomCover(std::string_view roomId)
    {
        // 仅允许请求当前快照明确声明有封面的房间。
        const auto room = std::find_if(
            m_rooms.begin(), m_rooms.end(), [roomId](const auto& candidate) {
                return candidate.roomId == roomId;
            });
        if ( room == m_rooms.end() || !room->hasCoverImage ) return false;
        // 已缓存或请求在途时保持幂等，不重复占用服务端带宽。
        if ( m_roomCovers.contains(roomId) ||
             m_requestedRoomCovers.contains(roomId) ) {
            return true;
        }

        // 请求只携带协议版本和公开 roomId，不包含房主令牌。
        const nlohmann::json request = {
            { "type", "get_room_cover" },
            { "version", DIRECTORY_PROTOCOL_VERSION },
            { "roomId", roomId },
        };
        // 在取得 RTC 锁前序列化，缩短第三方句柄临界区。
        const std::string payload = request.dump();
        std::scoped_lock  rtcLock(m_rtcApiMutex);
        // 发送前重新验证回调生命周期、句柄与实时打开状态。
        if ( !m_acceptCallbacks.load(std::memory_order_acquire) ||
             m_websocketId < 0 || !rtcIsOpen(m_websocketId) ||
             rtcSendMessage(m_websocketId, payload.c_str(), -1) !=
                 RTC_ERR_SUCCESS ) {
            return false;
        }
        // 只有第三方确认接受文本帧后才登记请求在途。
        m_requestedRoomCovers.emplace(roomId);
        return true;
    }

    /// @brief 返回目录状态。
    /// @return update 线程维护的当前连接状态。
    CollaborationDirectoryState state() const { return m_state; }

    /// @brief 返回房间快照。
    /// @return 最近一次完整验证并整体替换的房间列表。
    const std::vector<CollaborationDirectoryRoom>& rooms() const
    {
        return m_rooms;
    }

    /// @brief 返回一份已经取得的房间封面。
    /// @param roomId 待查询的公开房间标识。
    /// @return 已缓存 Base64 文本视图，未命中时返回空视图。
    std::string_view roomCover(std::string_view roomId) const
    {
        // map 的节点地址稳定到对应房间缓存被目录刷新淘汰为止。
        const auto iterator = m_roomCovers.find(roomId);
        return iterator == m_roomCovers.end()
                   ? std::string_view{}
                   : std::string_view(iterator->second);
    }

    /// @brief 返回最近错误。
    /// @return 最近连接或协议错误；成功列表替换后清空。
    const std::string& lastError() const { return m_lastError; }

    /// @brief 返回当前入口 URL。
    /// @return 最近一次 connect 保存的结构化服务端配置。
    const CollaborationServerEndpoint& endpoint() const { return m_endpoint; }

private:
    /// @brief 在删除 WebSocket 前切断全部回调和用户指针。
    /// @param websocketId 即将删除的有效第三方句柄。
    /// @warning 必须在持有 m_rtcApiMutex 且 Impl 仍存活时调用。
    static void detachWebSocketCallbacks(int websocketId)
    {
        // 先移除事件回调，阻止后续网络活动调用静态入口。
        rtcSetOpenCallback(websocketId, nullptr);
        rtcSetMessageCallback(websocketId, nullptr);
        rtcSetClosedCallback(websocketId, nullptr);
        rtcSetErrorCallback(websocketId, nullptr);
        // 最后清除 user pointer，确保残余第三方状态不引用 Impl。
        rtcSetUserPointer(websocketId, nullptr);
    }

    /// @brief 发送房间列表订阅请求。
    /// @return 活动句柄接受协议文本帧时返回 true。
    /// @warning 可由打开事件和刷新动作调用；不等待服务端列表响应。
    bool sendListRequest() const
    {
        // 与 disconnect 共用递归锁，保证检查到发送期间句柄不被删除。
        std::scoped_lock rtcLock(m_rtcApiMutex);
        if ( !m_acceptCallbacks.load(std::memory_order_acquire) ||
             m_websocketId < 0 || !rtcIsOpen(m_websocketId) ) {
            return false;
        }
        // 列表请求固定使用当前协议版本且没有分页或用户输入字段。
        const nlohmann::json request = {
            { "type", "list_rooms" },
            { "version", DIRECTORY_PROTOCOL_VERSION },
        };
        // 文本在锁内保持到 rtcSendMessage 返回，满足 C API 指针生命周期。
        const std::string payload = request.dump();
        return rtcSendMessage(m_websocketId, payload.c_str(), -1) ==
               RTC_ERR_SUCCESS;
    }

    /// @brief 校验并替换一份服务端房间列表。
    /// @param payload WebSocket 回调复制出的完整文本帧。
    void processMessage(std::string_view payload)
    {
        // 禁用异常解析；非法 JSON 与协议版本错误共享稳定失败状态。
        const auto message = nlohmann::json::parse(payload, nullptr, false);
        if ( !message.is_object() ||
             message.value("version", std::uint64_t{ 0 }) !=
                 DIRECTORY_PROTOCOL_VERSION ) {
            fail("invalid_directory_message");
            return;
        }
        // type 决定使用封面响应或完整列表解析器，其他类型一律拒绝。
        const std::string type = message.value("type", "");
        if ( type == "room_cover" ) {
            processRoomCover(message);
            return;
        }
        if ( type != "room_list" ) {
            fail("invalid_directory_message");
            return;
        }

        // 只有类型和版本通过后才允许替换 UI 可见目录快照。
        processRoomList(message);
    }

    /// @brief 校验并替换一份服务端房间列表。
    /// @param message 已通过协议版本和 room_list 类型检查的对象。
    void processRoomList(const nlohmann::json& message)
    {
        // rooms 必须为有界数组，检查发生在 reserve 和逐项分配前。
        const auto roomsIterator = message.find("rooms");
        if ( roomsIterator == message.end() || !roomsIterator->is_array() ||
             roomsIterator->size() > MAX_DIRECTORY_ROOMS ) {
            fail("invalid_room_list");
            return;
        }

        // 先构造临时列表，任一非法条目都不会破坏旧的有效快照。
        std::vector<CollaborationDirectoryRoom> rooms;
        rooms.reserve(roomsIterator->size());
        for ( const auto& item : *roomsIterator ) {
            // 字符串字段写入局部 DTO，人数和容量先保留 JSON 迭代器。
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
                // 必需字段缺失或类型不符会拒绝整份快照。
                fail("invalid_room_entry");
                return;
            }
            room.participants = participants->get<std::size_t>();
            room.capacity     = capacity->get<std::size_t>();
            // hasCoverImage 为向后兼容可选字段，缺失时保持 false。
            if ( const auto cover = item.find("hasCoverImage");
                 cover != item.end() ) {
                if ( !cover->is_boolean() ) {
                    fail("invalid_room_entry");
                    return;
                }
                room.hasCoverImage = cover->get<bool>();
            }
            // 空标识、非法容量和超员数据都不能进入可交互目录。
            if ( room.roomId.empty() || room.roomName.empty() ||
                 room.hostCreator.empty() || room.capacity < 2U ||
                 room.capacity > 8U || room.participants == 0U ||
                 room.participants > room.capacity ) {
                fail("invalid_room_entry");
                return;
            }
            // 条目完整通过验证后才移动进临时快照。
            rooms.push_back(std::move(room));
        }
        // 全数组验证成功后一次替换，避免 UI 观察部分新旧混合数据。
        m_rooms = std::move(rooms);
        // 清除已离开目录或已不再声明封面的缓存内容。
        for ( auto iterator = m_roomCovers.begin();
              iterator != m_roomCovers.end(); ) {
            const auto room = std::find_if(
                m_rooms.begin(), m_rooms.end(), [&](const auto& candidate) {
                    return candidate.roomId == iterator->first &&
                           candidate.hasCoverImage;
                });
            if ( room == m_rooms.end() ) {
                // 缓存失效时同步清除可能残留的在途请求标记。
                m_requestedRoomCovers.erase(iterator->first);
                iterator = m_roomCovers.erase(iterator);
            } else {
                ++iterator;
            }
        }
        // 未取得缓存的在途请求也必须随房间消失或封面撤销而淘汰。
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
        // 有效快照同时完成首次握手，并清除此前可恢复的协议错误。
        m_state = CollaborationDirectoryState::Connected;
        m_lastError.clear();
    }

    /// @brief 校验并缓存一份按需返回的房间封面。
    /// @param message 已通过协议版本和 room_cover 类型检查的对象。
    void processRoomCover(const nlohmann::json& message)
    {
        // 先复制必需字符串，随后统一执行标识和内容大小限制。
        std::string roomId;
        std::string coverImage;
        if ( !readString(message, "roomId", roomId) ||
             !readString(message, "coverImage", coverImage) || roomId.empty() ||
             roomId.size() > 32U ||
             coverImage.size() > MAX_ROOM_COVER_BASE64_BYTES ) {
            // 畸形封面响应代表协议错误，停止使用当前连接的数据。
            fail("invalid_room_cover");
            return;
        }

        // 响应到达时目录可能已经刷新，必须重新确认房间仍然存在。
        const auto room = std::find_if(
            m_rooms.begin(), m_rooms.end(), [&](const auto& candidate) {
                return candidate.roomId == roomId;
            });
        if ( room == m_rooms.end() ) {
            // 过期响应不缓存，仅允许未来重新出现的房间发起新请求。
            m_requestedRoomCovers.erase(roomId);
            return;
        }
        if ( coverImage.empty() ) {
            // 空响应表示服务端当前没有可用封面，解除在途状态以便重试。
            m_requestedRoomCovers.erase(roomId);
            return;
        }
        // 成功响应先清除请求标记，再按 roomId 插入或替换缓存。
        m_requestedRoomCovers.erase(roomId);
        m_roomCovers.insert_or_assign(std::move(roomId), std::move(coverImage));
    }

    /// @brief 切换到错误状态并保留诊断文本。
    /// @param error 面向调用方的稳定协议标识或第三方错误详情。
    void fail(std::string error)
    {
        // 保留已有目录快照供 UI 展示，但状态阻止继续刷新或请求封面。
        m_state     = CollaborationDirectoryState::Error;
        m_lastError = std::move(error);
    }

    /// @brief 有界压入回调事件。
    /// @param event 已复制并与第三方回调参数解耦的事件。
    /// @warning 第三方回调线程调用；只允许原子检查、短时锁和有界入队。
    void enqueue(Event event)
    {
        // disconnect 发布 false 后，迟到回调不得重新填充已清空队列。
        if ( !m_acceptCallbacks.load(std::memory_order_acquire) ) return;
        std::scoped_lock lock(m_eventMutex);
        // 达到硬上限时丢弃新事件，防止失控服务端耗尽客户端内存。
        if ( m_events.size() >= MAX_DIRECTORY_EVENTS ) return;
        // payload 按值移动进队列，回调返回后仍可由 UI 线程解析。
        m_events.push_back(std::move(event));
    }

    /// @brief WebSocket 打开回调。
    /// @param websocketId 触发回调的句柄；对象已由 user pointer 识别。
    /// @param pointer 注册时绑定的 Impl 地址。
    /// @warning 可能与 connect 的 rtcIsOpen 补检并发，只能有一个 Opened 事件。
    static void onOpen(int, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        // exchange 同时去重真实回调与注册后补检路径。
        if ( owner &&
             !owner->m_openHandled.exchange(true, std::memory_order_acq_rel) ) {
            owner->enqueue({ EventType::Opened, {} });
        }
    }

    /// @brief WebSocket 文本消息回调。
    /// @param websocketId 消息所属句柄。
    /// @param message 仅在回调期间有效的消息缓冲。
    /// @param size 负值表示零结尾文本，非负值视为二进制帧。
    /// @param pointer 注册时绑定的 Impl 地址。
    static void onMessage(int, const char* message, int size, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        // 目录协议只接受文本帧，并立即复制以解除第三方缓冲生命周期。
        if ( owner && message && size < 0 ) {
            owner->enqueue({ EventType::Message, std::string(message) });
        }
    }

    /// @brief WebSocket 关闭回调。
    /// @param websocketId 已关闭句柄。
    /// @param pointer 注册时绑定的 Impl 地址。
    static void onClosed(int, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        // 关闭原因由独立 Error 事件提供，此处只传递状态转换。
        if ( owner ) owner->enqueue({ EventType::Closed, {} });
    }

    /// @brief WebSocket 错误回调。
    /// @param websocketId 报错句柄。
    /// @param error 仅在回调期间有效的可选错误文本。
    /// @param pointer 注册时绑定的 Impl 地址。
    static void onError(int, const char* error, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        if ( owner ) {
            // 立即复制错误或使用稳定后备标识，避免保存悬空 C 字符串。
            owner->enqueue({ EventType::Error,
                             error ? error : "directory_websocket_error" });
        }
    }

    /// @brief 当前 WebSocket 句柄。
    /// @note 仅在 m_rtcApiMutex 保护下创建、读取用于发送或置为无效。
    int m_websocketId = -1;
    /// @brief 串行化目录句柄的发送、退役与删除前解绑。
    /// @note 使用 recursive_mutex 兼容第三方 API 在同步回调时重入发送路径。
    mutable std::recursive_mutex m_rtcApiMutex;
    /// @brief 是否仍接受第三方线程回调。
    /// @warning disconnect 写入、回调线程读取，release/acquire 防止退役后入队。
    std::atomic_bool m_acceptCallbacks{ false };
    /// @brief 防止打开回调与注册后的状态补检重复发送目录请求。
    /// @warning connect 重置并由可能并发的打开路径 exchange 去重。
    std::atomic_bool m_openHandled{ false };
    /// @brief 当前目录状态。
    CollaborationDirectoryState m_state = CollaborationDirectoryState::Idle;
    /// @brief 当前目录 URL。
    /// @note 仅用于连接生命周期诊断，不直接暴露给公共接口。
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
    /// @note UI update 通过 swap 缩短持锁时间，回调只执行有界 push_back。
    std::mutex m_eventMutex;
    /// @brief 等待 UI 线程消费的事件。
    /// @note 数量始终不超过 MAX_DIRECTORY_EVENTS。
    std::deque<Event> m_events;
};

/// @brief 创建拥有独立 libdatachannel 状态的未连接目录客户端。
CollaborationDirectoryClient::CollaborationDirectoryClient()
    : m_impl(std::make_unique<Impl>())
{
    // Impl 地址在客户端生命周期内稳定，可安全注册为第三方 user pointer。
}

/// @brief 通过 Impl 析构关闭连接并退役全部第三方回调。
CollaborationDirectoryClient::~CollaborationDirectoryClient() = default;

/// @brief 校验结构化端点并生成固定目录 WebSocket URL。
/// @param endpoint 纯地址、非零端口与 TLS 选项。
/// @return 无效配置返回空，否则返回 ws/wss 固定路径 URL。
std::string makeCollaborationSignalingUrl(
    const CollaborationServerEndpoint& endpoint)
{
    // 地址负责拒绝协议和路径，端口零值不允许用于客户端连接。
    if ( !isValidServerAddress(endpoint.address) ||
         endpoint.signalingPort == 0 ) {
        return {};
    }
    // 裸 IPv6 地址需要方括号，域名和 IPv4 保持原样。
    std::string address = endpoint.address;
    if ( address.contains(':') &&
         !(address.starts_with('[') && address.ends_with(']')) ) {
        address = '[' + address + ']';
    }
    // 协议仅由 useTls 决定，路径固定为协作服务入口。
    return std::string(endpoint.useTls ? "wss://" : "ws://") + address + ':' +
           std::to_string(endpoint.signalingPort) + "/mmm-collaboration";
}

bool CollaborationDirectoryClient::connect(CollaborationServerEndpoint endpoint)
{
    // 公共对象保持轻量，只把值语义端点转交给稳定 Impl。
    return m_impl->connect(std::move(endpoint));
}

/// @brief 关闭当前目录连接并恢复空闲状态。
void CollaborationDirectoryClient::disconnect()
{
    // Impl 负责句柄退役、事件清空与快照重置的正确顺序。
    m_impl->disconnect();
}

/// @brief 在调用线程消费有界回调事件并推进目录状态。
/// @warning UI 每帧调用；不得在此转发层增加所有权复制、等待或文件 I/O。
void CollaborationDirectoryClient::update()
{
    m_impl->update();
}

/// @brief 非阻塞请求服务端重发当前房间列表。
/// @return 当前连接可用且消息提交成功时返回 true。
bool CollaborationDirectoryClient::refresh()
{
    return m_impl->refresh();
}

/// @brief 幂等请求当前目录房间的封面缓存。
/// @param roomId 当前快照中的公开房间标识。
/// @return 已缓存、请求在途或新请求成功发送时返回 true。
bool CollaborationDirectoryClient::requestRoomCover(std::string_view roomId)
{
    return m_impl->requestRoomCover(roomId);
}

/// @brief 返回调用线程维护的目录状态快照。
CollaborationDirectoryState CollaborationDirectoryClient::state() const
{
    return m_impl->state();
}

/// @brief 返回最近一次完整验证的房间列表。
const std::vector<CollaborationDirectoryRoom>&
CollaborationDirectoryClient::rooms() const
{
    return m_impl->rooms();
}

/// @brief 查询指定房间已经缓存的 Base64 封面。
/// @param roomId 当前目录房间标识。
/// @return 缓存存在时返回视图，否则返回空视图。
std::string_view CollaborationDirectoryClient::roomCover(
    std::string_view roomId) const
{
    return m_impl->roomCover(roomId);
}

/// @brief 返回最近连接或协议错误文本。
const std::string& CollaborationDirectoryClient::lastError() const
{
    return m_impl->lastError();
}

/// @brief 返回最近一次连接保存的结构化服务端端点。
const CollaborationServerEndpoint&
CollaborationDirectoryClient::endpoint() const
{
    return m_impl->endpoint();
}
}  // namespace MMM::Network::Collaboration
