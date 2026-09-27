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
/// @note 只约束 create_room、join_room 等 broker 消息，不等同于业务帧版本。
constexpr std::uint64_t BROKER_PROTOCOL_VERSION = 1;
/// @brief 中心完成配对后两端直接交换身份使用的协议版本。
/// @note 该版本覆盖 join、accepted、description、candidate 与 p2p_ready。
constexpr std::uint64_t P2P_SIGNALING_PROTOCOL_VERSION = 2;
/// @brief 协作 DataChannel 的稳定标签。
/// @note 房主拒绝任何其他标签，避免把同一 PeerConnection 的扩展通道误接入。
constexpr std::string_view COLLABORATION_CHANNEL_LABEL = "mmm-collaboration-v2";
/// @brief 单条信令消息大小上限。
/// @note 同时配置 WebSocket 接收边界并约束本地序列化发送大小。
constexpr int MAX_SIGNALING_MESSAGE_BYTES = 256 * 1024;
/// @brief 房间目录封面 Base64 文本上限，预留 JSON 与信令字段空间。
/// @note 封面总量在 startHost 校验，不允许通过分块绕过房间级限制。
constexpr std::size_t MAX_ROOM_COVER_BASE64_BYTES = 96U * 1024U;
/// @brief 单条封面上传消息携带的 Base64 文本上限。
/// @note 分块显著小于信令消息上限，避免大目录帧长期占用发送队列。
constexpr std::size_t MAX_ROOM_COVER_CHUNK_BYTES = 8U * 1024U;
/// @brief WebRTC 协商的数据通道消息上限。
/// @note receive 回调和 PeerConnection 配置共同执行该边界。
constexpr int MAX_DATA_CHANNEL_MESSAGE_BYTES = 2 * 1024 * 1024;
/// @brief 回调线程允许积压的完整 DataChannel 消息数。
/// @note 达到上限后拒绝新帧，优先保护进程内存和 RTC 工作线程。
constexpr std::size_t MAX_QUEUED_DATA_CHANNEL_PACKETS = 4096;
/// @brief 产品层未及时消费时保留的连接事件数。
/// @note 达到上限后淘汰最旧事件，保留更接近当前状态的生命周期信息。
constexpr std::size_t MAX_QUEUED_TRANSPORT_EVENTS = 1024;
/// @brief 中心服务允许下发的 ICE URI 数量。
/// @note 单个 URI 另限制为 512 字节，二者共同约束第三方配置输入。
constexpr std::size_t MAX_ICE_SERVERS = 8;
/// @brief 房主控制令牌使用的无歧义字符表。
/// @note 排除 0/O、1/l/I 等字符，便于必要时人工识别日志中的脱敏片段。
constexpr std::string_view TOKEN_ALPHABET =
    "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/// @brief 规范化可在目录公开展示的房间名称。
/// @param value 用户输入的 UTF-8 房间名称。
/// @return 去除首尾空白后的名称；格式或长度不合法时返回空字符串。
///
/// @details
/// 目录名称允许 UTF-8 多字节内容，但禁止 ASCII 控制字符，避免换行等字符
/// 污染目录协议和日志。长度按传输字节计并限制为 128，和服务端输入边界保持
/// 一致。这里只裁剪首尾空白，不改写名称内部的可见空格。
std::string normalizeRoomName(std::string_view value)
{
    // 对 char 先转 unsigned char，避免 ctype 在负值输入上产生未定义行为。
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.front())) != 0 ) {
        value.remove_prefix(1);
    }
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.back())) != 0 ) {
        value.remove_suffix(1);
    }
    // 裁剪后的空名称和超长 UTF-8 字节序列都不能公开发布。
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
/// @param value 待校验的目录 roomId。
/// @return 非空、长度受限且只含安全 URL 字符时返回 true。
///
/// @details roomId 会重新发送给中心服务，因此客户端在信任和存储前再次限制
///          字符集，防止控制字符或路径字符进入后续消息。
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
/// @param object 已解析的信令 JSON 对象。
/// @param key 字段名。
/// @param value 成功时接收字符串副本。
/// @return 字段存在且类型严格为 string 时返回 true。
/// @note 失败时调用方不得依赖 value 的内容。
bool readStringField(const nlohmann::json& object, std::string_view key,
                     std::string& value)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_string() ) return false;
    value = iterator->get_ref<const std::string&>();
    return true;
}

/// @brief 从 JSON 对象读取无符号整数字段。
/// @param object 已解析的信令 JSON 对象。
/// @param key 字段名。
/// @param value 成功时接收 uint64 值。
/// @return 字段严格属于 JSON unsigned number 时返回 true。
/// @note 不接受负数或字符串形式数字，避免协议解析产生隐式转换。
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
/// @param object 已解析的信令 JSON 对象。
/// @param key 字段名。
/// @param value 成功时接收布尔值。
/// @return 字段严格属于 JSON boolean 时返回 true。
bool readBooleanField(const nlohmann::json& object, std::string_view key,
                      bool& value)
{
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_boolean() ) return false;
    value = iterator->get<bool>();
    return true;
}

/// @brief 生成仅房主控制连接持有的随机令牌。
/// @return 由 48 个无歧义字符组成的随机控制令牌。
///
/// @details 令牌只用于当前房间的中心服务控制消息，不作为长期身份。字符表排除
///          容易视觉混淆的字符，长度提供充足随机空间，并保持 JSON 传输安全。
std::string generateOwnerToken()
{
    // 每次建房重新播种，房间停止后令牌会随状态清理。
    std::mt19937_64                            random{ std::random_device{}() };
    std::uniform_int_distribution<std::size_t> distribution(
        0, TOKEN_ALPHABET.size() - 1U);
    std::string token(48U, '0');
    // 独立均匀选择每一位，避免把随机整数转码时引入取模偏差。
    for ( char& character : token ) {
        character = TOKEN_ALPHABET[distribution(random)];
    }
    return token;
}
}  // namespace

/// @brief WebRtcTransport 的线程安全 libdatachannel 实现。
///
/// @details
/// 一个 Impl 表示一次可停止并重启的房主或访客传输会话。房主保留一个长期
/// HostControl WebSocket，并为每位获准访客创建一个瞬时 Peer WebSocket；访客
/// 只保留一个 Peer WebSocket。P2P DataChannel 打开且双方确认后，瞬时信令即可
/// 关闭，业务帧不再经过中心服务。
///
/// 状态由两类锁保护：m_rtcApiMutex 串行化非线程安全或涉及句柄生命周期的 C API
/// 调用，m_mutex 保护连接字段、队列和公开状态。需要同时持有时始终先取得 RTC
/// 锁再取得状态锁。回调不能在持有状态锁时调用可能同步重入的 C API。
///
/// @par 事件边界
/// RTC 回调线程只把完整二进制帧和结构化生命周期事件放入有界队列；产品层通过
/// receive/receiveEvent 非阻塞拉取。传输层不解释协作业务帧，也不在回调中等待
/// 编辑器线程，从而保持底层网络职责单一。
///
/// @par 安全边界
/// broker 与透明信令消息在使用前都执行类型、长度、版本和角色检查；稳定身份
/// 还要经过统一规范化。传输层不信任中心服务返回值，也不允许 HostControl 与
/// Peer 连接互相执行对方职责，从客户端侧缩小协议输入面。
/// 任何验证失败都转换为稳定事件或拒绝原因，不把异常跨越 C 回调边界传播。
/// 未识别消息保持忽略，便于新旧协议端点在不扩大现有权限的前提下协商演进。
///
///
/// @warning Connection 地址被注册到 C 回调，必须先解绑全部回调和 user pointer，
///          再删除句柄，最后才能销毁 Connection 对象。
class WebRtcTransport::Impl
{
public:
    /// @brief 中心 WebSocket 在当前传输中的职责。
    ///
    /// HostControl 在房间整个生命周期保持，用于目录管理和准入请求；Peer
    /// 只服务一对 P2P 协商，DataChannel 双向确认后即可关闭。
    enum class ConnectionRole {
        HostControl,
        Peer,
    };

    /// @brief 一个访客与房主之间的临时信令和长期 P2P 上下文。
    ///
    /// @details
    /// Connection 的地址会注册为 libdatachannel user pointer，因此对象存放在
    /// unique_ptr 中并在句柄回调完全解绑前保持稳定。字段同时被产品线程和 RTC
    /// 回调线程访问，所有非原子共享状态都由 Impl::m_mutex 保护。
    ///
    /// @warning openHandled 是回调注册竞争所需的唯一原子门闩：WebSocket 可能
    ///          在设置回调前已经打开，手工补检和异步回调必须只处理一次。
    struct Connection {
        /// @brief 所属传输实现的稳定观察指针。
        /// @warning 仅在全部句柄回调解绑后才能让 owner 或本对象失效。
        Impl* owner = nullptr;
        /// @brief 中心连接职责。
        /// @note 角色在连接创建时确定，生命周期内不再改变。
        ConnectionRole role = ConnectionRole::Peer;
        /// @brief 信令 WebSocket 标识。
        /// @note 负值表示尚未创建或已经转移到 RetiredHandles。
        int websocketId = -1;
        /// @brief 防止打开回调与注册后的状态补检重复声明连接角色。
        /// @warning 写入来自 RTC 回调或创建线程，必须使用原子交换去重。
        std::atomic_bool openHandled{ false };
        /// @brief WebRTC PeerConnection 标识。
        /// @note 负值表示身份握手尚未创建或 stop 已退役句柄。
        int peerConnectionId = -1;
        /// @brief 可靠有序 DataChannel 标识。
        /// @note 访客主动创建，房主通过 onDataChannel 接收。
        int dataChannelId = -1;
        /// @brief 远端 PeerId。
        /// @note 房主侧在 handleJoin 分配，访客侧初始即指向 hostId。
        PeerId remotePeerId = 0;
        /// @brief 远端 Creator。
        /// @note 房主侧必须与中心审批的 approvedCreator 相等。
        std::string remoteCreator;
        /// @brief 远端不随路由槽位复用变化的稳定协作者标识。
        ParticipantId remoteParticipantId;
        /// @brief 远端本次加入流程的操作会话标识。
        OperationSessionId remoteSessionId;
        /// @brief 服务端加入请求标识；仅房主的访客通道使用。
        /// @note 用于 accept_join 将新 WebSocket 绑定到已批准请求。
        std::string requestId;
        /// @brief 房主批准加入时由中心服务确认的访客 Creator。
        /// @note P2P join 再次携带 creator 时必须与此字段一致。
        std::string approvedCreator;
        /// @brief 是否已经完成 P2P 身份握手。
        /// @note 只有 joined 的连接才允许把 DataChannel 业务帧交给产品层。
        bool joined = false;
        /// @brief 是否已经上报 DataChannel 建立事件。
        /// @note 同时充当本地通道已打开门闩，确保 PeerConnected 只入队一次。
        bool connectedEventSent = false;
        /// @brief 对端是否已经确认 DataChannel 打开。
        /// @note 与 connectedEventSent 同时成立后才能关闭瞬时信令。
        bool remoteDataChannelReady = false;
        /// @brief 是否已经上报离开事件。
        /// @note 关闭、错误和房主移除路径共享该幂等门闩。
        bool disconnectedEventSent = false;
        /// @brief
        /// 是否已经把中心服务拒绝作为终止事件上报，避免关闭回调覆盖原因。
        /// @note 仅 Peer 信令连接使用；HostControl 关闭始终视为目录错误。
        bool signalingTerminalEventSent = false;
    };

    /// @brief 从连接状态中原子退役、随后在锁外删除的一组 RTC 句柄。
    ///
    /// @details stop() 先在 RTC 锁内解绑回调并把句柄移出活动 Connection，再在
    /// 锁外调用 delete API，避免删除过程触发回调时重入同一临界区。
    struct RetiredHandles {
        /// @brief DataChannel 句柄。
        int dataChannelId = -1;
        /// @brief PeerConnection 句柄。
        int peerConnectionId = -1;
        /// @brief WebSocket 句柄。
        int websocketId = -1;
    };

    /// @brief 停止并释放所有 libdatachannel 句柄。
    /// @note stop() 可重复调用，因此显式 stop 后析构不会二次删除句柄。
    ~Impl() { stop(); }

    /// @brief 连接中心服务并发布房间。
    /// @param config 房主身份、目录端点、容量、封面和兼容性策略。
    /// @return 参数有效且中心 WebSocket 成功创建时返回 true。
    ///
    /// @details
    /// 该函数只启动异步建房流程，roomId 由后续 RoomPublished 事件提供。所有
    /// 外部文本先规范化，构建指纹和 URL 在修改对象状态前验证。初始化状态在
    /// m_mutex 下完成，RTC 句柄创建由 m_rtcApiMutex 串行化。
    ///
    /// @warning 成功返回不代表房间已经公开；调用方仍需消费并推进传输事件。
    bool startHost(const WebRtcHostConfig& config)
    {
        // 先在栈上规范化配置，失败时不留下半初始化成员状态。
        const auto creator  = Config::normalizeCreatorIdentity(config.creator);
        const auto roomName = normalizeRoomName(config.roomName);
        const auto participantId =
            Config::normalizeCollaborationStableId(config.participantId);
        const auto sessionId =
            Config::normalizeCollaborationStableId(config.sessionId);
        const auto& buildFingerprint = config.buildFingerprint.empty()
                                           ? collaborationBuildFingerprint()
                                           : config.buildFingerprint;
        // 封面采用 Base64 文本上限，端点必须能生成非空 ws/wss URL。
        if ( creator.empty() || roomName.empty() || config.hostId == 0 ||
             participantId.empty() || sessionId.empty() ||
             config.roomCoverImage.size() > MAX_ROOM_COVER_BASE64_BYTES ||
             !isValidCollaborationBuildFingerprint(buildFingerprint) ||
             makeCollaborationSignalingUrl(config.endpoint).empty() ) {
            return false;
        }

        // 控制连接对象先取得稳定地址，再放入容器供 C 回调长期引用。
        auto control        = std::make_unique<Connection>();
        control->owner      = this;
        control->role       = ConnectionRole::HostControl;
        Connection* pointer = control.get();
        bool        opened  = false;
        {
            // 锁顺序固定为 RTC API 锁后状态锁，和回调及 stop() 保持一致。
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
                m_roomName       = roomName;
                m_roomCoverImage = config.roomCoverImage;
                m_signalingUrl = makeCollaborationSignalingUrl(config.endpoint);
                m_ownerToken   = generateOwnerToken();
                m_localPeerId  = config.hostId;
                m_hostId       = config.hostId;
                // 即使调用方越界，公开容量也始终落在协议允许范围内。
                m_maxParticipants = std::clamp(config.maxParticipants,
                                               MIN_COLLABORATION_PARTICIPANTS,
                                               MAX_COLLABORATION_PARTICIPANTS);
                m_stopping        = false;
                m_running         = true;
                m_connections.push_back(std::move(control));
            }
            opened = openWebSocket(*pointer);
        }
        // 创建失败统一走 stop，清除已经写入的身份、令牌和 Connection。
        if ( !opened ) {
            stop();
            return false;
        }
        return true;
    }

    /// @brief 连接中心服务并请求加入公开房间。
    /// @param config 访客身份、目标房间、端点和构建指纹。
    /// @return 参数有效且信令 WebSocket 成功创建时返回 true。
    ///
    /// @details
    /// 访客最初只知道固定 hostId 和公开 roomId，本地 peerId 要等房主接受后才
    /// 分配。访客始终要求房主的构建指纹兼容性策略生效，具体是否强制匹配由
    /// accepted 消息携带的房主配置决定。
    ///
    /// @warning 返回 true 仅表示加入请求已启动，不表示房主已经批准或 P2P 已连。
    bool connectToHost(const WebRtcGuestConfig& config)
    {
        // 与房主入口相同，先验证并规范化全部外部值再进入临界区。
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

        // 访客只有一个 Peer 连接，其远端预先标记为协议约定的 hostId。
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
                // 本地 peerId 必须保持未分配，直到 accepted 消息通过全部校验。
                m_localPeerId = 0;
                m_hostId      = config.hostId;
                m_stopping    = false;
                m_running     = true;
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
    /// @param requestId 中心服务分配并由 JoinRequested 事件上报的请求 ID。
    /// @return 请求存在且访客专用信令连接成功启动时返回 true。
    ///
    /// @details 先复制请求资料并释放状态锁，再创建可能触发同步回调的 RTC 连接。
    ///          只有连接启动成功后才从待审批表删除，允许调用方在失败后重试。
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
        // RTC 创建不在 m_mutex 内执行，避免同步回调重入状态访问造成死锁。
        if ( !openHostPeerConnection(request, creator) ) return false;
        std::scoped_lock lock(m_mutex);
        m_pendingJoinRequests.erase(request);
        return true;
    }

    /// @brief 拒绝一个由中心服务暂存的访客加入请求。
    /// @param requestId 待拒绝请求的中心标识。
    /// @param reason 发送给访客的稳定拒绝原因。
    /// @return 控制连接可用且拒绝消息成功发送时返回 true。
    ///
    /// @details 拒绝通过长期 HostControl WebSocket 发给中心服务，不创建 P2P
    ///          连接。请求只在发送成功后移除，避免网络瞬时失败静默丢失审批项。
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
        // HostControl 缺失表示房间无法再管理目录，此时保留请求供上层诊断。
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
    /// @param peerId 要移除的访客 peer ID。
    /// @param detail 上报给产品层的原因；为空时使用 removed_by_host。
    /// @return 找到活动访客并发起关闭时返回 true。
    ///
    /// @details
    /// 先通过 notifyDisconnected 原子标记并上报一次离开，再关闭 DataChannel 和
    /// PeerConnection。关闭回调可能随后再次到达，但 disconnectedEventSent 会
    /// 抑制重复事件和重复人数更新。
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
            // 只允许移除已经上报 Connected 且尚未离开的 Peer 连接。
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
    ///
    /// @details
    /// stop() 分三阶段执行：在双锁内停止接收新工作并移出连接；继续持有 RTC
    /// 锁逐句柄解绑回调；释放 RTC 锁后调用 delete API；最后清空可重启状态。
    /// 这种顺序保证 user pointer 指向的 Connection 在任何可能回调期间仍存活。
    ///
    /// @warning 删除 libdatachannel 句柄可能同步触发内部收尾，不能在 m_mutex
    ///          下执行，否则回调读取状态会死锁。
    void stop()
    {
        std::vector<std::unique_ptr<Connection>> connections;
        std::vector<RetiredHandles>              retiredHandles;
        {
            std::scoped_lock rtcLock(m_rtcApiMutex);
            {
                std::scoped_lock lock(m_mutex);
                if ( !m_running && m_connections.empty() ) return;
                m_stopping = true;
                m_running  = false;
                // 移出 unique_ptr 后地址不变，并把产品层队列立即置空。
                connections = std::move(m_connections);
                m_incomingPackets.clear();
                m_events.clear();
                m_pendingJoinRequests.clear();
            }

            // 先让每个 Connection 不再拥有句柄并切断全部 C 回调。
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

        // 此处已释放全部内部锁，删除期间即使库内部等待也不会阻塞状态访问。
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

        // 最终清除会话特定字段，使同一 Impl 可以安全开始下一次连接。
        std::scoped_lock lock(m_mutex);
        m_stopping    = false;
        m_localPeerId = 0;
        m_participantId.clear();
        m_sessionId.clear();
        m_buildFingerprint.clear();
        m_roomId.clear();
        m_roomName.clear();
        m_roomCoverImage.clear();
        m_ownerToken.clear();
        m_iceServers.clear();
    }

    /// @brief 查询运行状态。
    /// @return startHost/connectToHost 已启动且 stop 尚未开始时返回 true。
    /// @note 结果是调用瞬间快照，返回后连接状态仍可能由回调线程改变。
    bool isRunning() const
    {
        std::scoped_lock lock(m_mutex);
        return m_running;
    }

    /// @brief 查询房主角色。
    /// @return 当前会话由 startHost 启动时返回 true。
    /// @note 该值只在持锁下读取，避免与重新启动会话的数据竞争。
    bool isHost() const
    {
        std::scoped_lock lock(m_mutex);
        return m_isHost;
    }

    /// @brief 获取本地 PeerId。
    /// @return 房主固定 ID 或访客获准后分配的 ID；尚未分配时为零。
    PeerId localPeerId() const
    {
        std::scoped_lock lock(m_mutex);
        return m_localPeerId;
    }

    /// @brief 获取公开房间标识。
    /// @return 房主发布所得或访客配置携带的 roomId 副本。
    std::string roomId() const
    {
        std::scoped_lock lock(m_mutex);
        return m_roomId;
    }

    /// @brief 发送协作协议二进制帧。
    /// @param recipientId 目标远端 peer ID。
    /// @param payload 完整协议帧，不得为空且必须适配 C API 的 int 长度。
    /// @return 找到已连接通道且 libdatachannel 接受发送时返回 true。
    ///
    /// @details 句柄查找与 rtcIsOpen/rtcSendMessage 均在 RTC API 锁范围内，避免
    ///          stop() 在两步之间退役句柄。函数不保留 payload，库调用返回后
    ///          输入 span 可立即失效。
    bool send(PeerId recipientId, std::span<const std::uint8_t> payload)
    {
        if ( payload.empty() ||
             payload.size() > static_cast<std::size_t>(INT_MAX) ) {
            return false;
        }

        // RTC 锁覆盖句柄选择、打开检查和实际发送，保证句柄生命周期连续。
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
    /// @param packet 成功时接收队首帧及发送者 ID。
    /// @return 队列非空并弹出一帧时返回 true。
    /// @note 采用移动赋值转移 payload，临界区内不复制大二进制缓冲。
    bool receive(TransportPacket& packet)
    {
        std::scoped_lock lock(m_mutex);
        if ( m_incomingPackets.empty() ) return false;
        packet = std::move(m_incomingPackets.front());
        m_incomingPackets.pop_front();
        return true;
    }

    /// @brief 非阻塞读取连接生命周期事件。
    /// @param event 成功时接收最早尚未消费的传输事件。
    /// @return 事件队列非空时返回 true。
    /// @note 队列保持回调入队顺序，产品层应持续排空以避免有界队列淘汰旧项。
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
    /// @param channelId libdatachannel 的 DataChannel 或 WebSocket 句柄。
    ///
    /// @details 两类句柄共享 open/message/closed/error 回调 API。最后清除 user
    ///          pointer，保证随后删除句柄时不会再访问已退役 Connection。
    /// @warning 调用方必须持有 m_rtcApiMutex 并保证句柄尚未被删除。
    static void detachChannelCallbacks(int channelId)
    {
        rtcSetOpenCallback(channelId, nullptr);
        rtcSetMessageCallback(channelId, nullptr);
        rtcSetClosedCallback(channelId, nullptr);
        rtcSetErrorCallback(channelId, nullptr);
        rtcSetUserPointer(channelId, nullptr);
    }

    /// @brief 在句柄删除前切断 PeerConnection 的全部回调。
    /// @param peerConnectionId 要退役的 PeerConnection 句柄。
    /// @warning 调用方必须持有 m_rtcApiMutex；解绑后才允许释放 Connection。
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
    /// @param connection 将取得句柄并作为 C 回调 user pointer 的稳定连接对象。
    /// @return 句柄创建并完成全部回调注册时返回 true。
    ///
    /// @details
    /// wss 目前关闭证书验证只解决预编译 mbedTLS 无法加载系统信任链的限制，
    /// 仍保留链路加密。消息上限与客户端解析边界一致。若套接字在回调注册完成
    /// 前已打开，rtcIsOpen 补检会主动调用 open 处理；原子门闩防止重复声明角色。
    bool openWebSocket(Connection& connection)
    {
        rtcWsConfiguration config{};
        config.disableTlsVerification = m_signalingUrl.starts_with("wss://");
        config.connectionTimeoutMs    = 10000;
        config.pingIntervalMs         = 5000;
        config.maxOutstandingPings    = 3;
        config.maxMessageSize         = MAX_SIGNALING_MESSAGE_BYTES;
        // 只有句柄创建成功后才写入 Connection，失败不会留下可误删的 ID。
        const int websocketId =
            rtcCreateWebSocketEx(m_signalingUrl.c_str(), &config);
        if ( websocketId < 0 ) return false;
        connection.websocketId = websocketId;
        rtcSetUserPointer(websocketId, &connection);
        rtcSetOpenCallback(websocketId, &Impl::onWebSocketOpen);
        rtcSetMessageCallback(websocketId, &Impl::onWebSocketMessage);
        rtcSetClosedCallback(websocketId, &Impl::onWebSocketClosed);
        rtcSetErrorCallback(websocketId, &Impl::onWebSocketError);
        // 覆盖“连接极快、open 事件早于回调注册”竞态。
        if ( rtcIsOpen(websocketId) ) {
            onWebSocketOpen(websocketId, &connection);
        }
        return true;
    }

    /// @brief 创建房主接受单个访客所需的瞬时信令连接。
    /// @param requestId 已由产品层批准的中心请求标识。
    /// @param approvedCreator 中心请求携带且已规范化的访客名称。
    /// @return Peer Connection 专用 WebSocket 成功启动时返回 true。
    ///
    /// @details 每个获准访客拥有独立信令 WebSocket，中心只透明转发该对端的
    ///          身份、SDP 和 candidate。连接对象先加入活动表，保证同步回调能
    ///          找到完整上下文；启动失败会通过 Error 事件暴露。
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
    /// @param type 结构化生命周期事件类型。
    /// @param peerId 关联 peer，尚未分配时为零。
    /// @param creator 关联显示身份。
    /// @param detail 稳定原因或诊断标识。
    /// @param requestId 可选准入请求 ID。
    /// @param participantId 可选稳定参与者 ID。
    /// @param sessionId 可选本次操作会话 ID。
    /// @param buildFingerprint 可选远端构建指纹。
    ///
    /// @details 队列达到上限时淘汰最旧事件，保证 RTC 回调线程不会因产品层停滞
    ///          无界分配。停止期间丢弃新事件，因为产品层已经请求会话终止。
    void pushEvent(WebRtcTransportEventType type, PeerId peerId,
                   std::string creator, std::string detail,
                   std::string requestId = {}, ParticipantId participantId = {},
                   OperationSessionId sessionId        = {},
                   std::string        buildFingerprint = {})
    {
        std::scoped_lock lock(m_mutex);
        if ( m_stopping ) return;
        // 丢弃最旧项保留最新连接状态，使恢复消费后更接近当前事实。
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
    /// @param connection 目标中心 WebSocket 的连接上下文。
    /// @param message 已构造的协议 JSON 对象。
    /// @return 文本大小合法、句柄打开且发送成功时返回 true。
    ///
    /// @details dump 在获取 RTC 锁前完成，缩短串行化句柄操作的临界区。长度检查
    ///          使用序列化后的真实字节数；发送时 -1 表示以 NUL 结尾文本帧。
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

    /// @brief 在房间注册完成后分块上传可选封面，避免大首帧阻塞建房。
    /// @param connection 长期 HostControl 连接。
    /// @return 无封面或全部分块成功发送时返回 true。
    ///
    /// @details 封面先在状态锁内复制，之后分块发送不长期占用 m_mutex。每个消息
    ///          携带零基 chunkIndex 与总
    ///          chunkCount，服务端据此重组并验证完整性。 封面总量已在 startHost
    ///          校验，此处只负责安全切片。
    bool sendRoomCover(Connection& connection)
    {
        std::string coverImage;
        {
            std::scoped_lock lock(m_mutex);
            if ( m_stopping || m_roomCoverImage.empty() ) return true;
            coverImage = m_roomCoverImage;
        }

        // 向上取整保证非整块尾部仍生成一条消息。
        const std::size_t chunkCount =
            (coverImage.size() + MAX_ROOM_COVER_CHUNK_BYTES - 1U) /
            MAX_ROOM_COVER_CHUNK_BYTES;
        for ( std::size_t chunkIndex = 0; chunkIndex < chunkCount;
              ++chunkIndex ) {
            const std::size_t offset = chunkIndex * MAX_ROOM_COVER_CHUNK_BYTES;
            nlohmann::json    message;
            message["type"]       = "set_room_cover";
            message["version"]    = BROKER_PROTOCOL_VERSION;
            message["chunkIndex"] = chunkIndex;
            message["chunkCount"] = chunkCount;
            message["coverChunk"] =
                coverImage.substr(offset,
                                  std::min(MAX_ROOM_COVER_CHUNK_BYTES,
                                           coverImage.size() - offset));
            if ( !sendSignal(connection, message) ) return false;
        }
        return true;
    }

    /// @brief 校验并保存服务端下发的 ICE URI。
    /// @param message 含 iceServers 数组的中心协议消息。
    /// @return 数组、元素类型、数量和单 URI 长度都合法时返回 true。
    ///
    /// @details 先在局部 vector 中完成全量校验，避免部分非法列表覆盖当前可用
    ///          配置。空数组合法，表示只使用 host candidate；URI 语义交由
    ///          libdatachannel 解析。
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
    /// @param connection 将拥有新句柄的稳定连接上下文。
    /// @param createDataChannel true
    /// 时由访客主动创建可靠有序通道；房主等待回调。
    /// @return PeerConnection 以及所需 DataChannel 均创建成功时返回 true。
    ///
    /// @details ICE 字符串先复制到局部存储，再构造 C 指针数组，确保
    /// rtcCreatePeerConnection 调用期间指针有效。访客是协商发起方并主动创建
    /// DataChannel，房主则通过 onDataChannel 接受同一稳定标签的通道。
    ///
    /// @warning 句柄配置由 m_rtcApiMutex 串行化；创建失败时必须先解绑回调、
    ///          清空 Connection 句柄，再解锁删除，防止回调访问半初始化状态。
    bool createPeerConnection(Connection& connection, bool createDataChannel)
    {
        std::unique_lock         rtcLock(m_rtcApiMutex);
        std::vector<std::string> iceServers;
        {
            std::scoped_lock lock(m_mutex);
            if ( m_stopping || !m_running ) return false;
            iceServers = m_iceServers;
        }
        // 指针只引用本函数的 iceServers 副本，生命周期覆盖 C API 创建调用。
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

        // 房主端由远端访客创建通道，本地只等待 onDataChannel。
        if ( !createDataChannel ) return true;

        rtcDataChannelInit channelConfig{};
        channelConfig.reliability.unordered  = false;
        channelConfig.reliability.unreliable = false;
        channelConfig.protocol               = "mmm-collaboration";
        const std::string label(COLLABORATION_CHANNEL_LABEL);
        const int         dataChannelId = rtcCreateDataChannelEx(
            peerConnectionId, label.c_str(), &channelConfig);
        // 通道失败时 PeerConnection 对调用方不可用，完整回滚本次创建。
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
    /// @param connection 通道所属连接上下文。
    /// @param dataChannelId 已创建或由远端回调交付的句柄。
    ///
    /// @details 在写入句柄前再次确认传输未停止，随后一次性注册生命周期和消息
    ///          回调。可靠、有序属性在访客创建通道时确定，房主接受端无需重设。
    /// @warning Connection 必须比所有已注册回调存活更久。
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
    /// @return 最小可用访客 ID；容量已满时返回零。
    ///
    /// @details hostId 自身不参与候选，从其下一值起顺序查找。只把已经完成身份
    ///          join 的 Peer 视为占用，断开后 ID 可安全复用；稳定身份另由
    ///          ParticipantId 表达，不依赖路由槽位长期唯一。
    /// @warning 调用方必须持有 m_mutex。
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
    /// @param connection 房主为已批准请求创建的 Peer 连接。
    /// @param message 访客发送的 join 消息。
    ///
    /// @details
    /// 先严格验证协议版本、字段类型、稳定 ID 和构建指纹，再比较中心审批记录的
    /// creator，防止访客在透明转发阶段更换显示身份。容量检查与连接状态写入在
    /// m_mutex 下原子完成，成功后回复分配的 peerId 和房主完整身份。
    ///
    /// @note 身份或容量失败通过 P2P signaling 的 rejected 消息返回，不创建
    ///       PeerConnection，也不会占用 peer ID。
    void handleJoin(Connection& connection, const nlohmann::json& message)
    {
        std::string   creator;
        std::string   participantId;
        std::string   sessionId;
        std::string   buildFingerprint;
        std::uint64_t version = 0;
        // 任何缺失、类型不符或指纹格式错误都归为协议级 invalid_join。
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
        // creator 必须与中心审批项一致，稳定 ID 则必须通过统一规范化。
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

        // peer ID 分配与 joined 标记处于同一临界区，避免两个访客取得同一槽位。
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

        // accepted 同时携带房主兼容策略，访客据此决定是否强制指纹相等。
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
    /// @param connection 访客唯一 Peer 连接。
    /// @param message 房主发送的 accepted 消息。
    ///
    /// @details 访客严格验证分配 ID、固定
    /// hostId、房主稳定身份、会话身份和指纹。
    ///          只有全部通过后才写入本地 peerId 并主动创建
    ///          DataChannel，避免非法 消息让半初始化身份泄漏给产品层。
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
        // JSON 数字必须为 unsigned，字符串或负数不能隐式转换成路由 ID。
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
        // 是否强制匹配由房主策略决定，但远端指纹本身始终要求格式有效。
        if ( requiresMatchingBuildFingerprint &&
             hostBuildFingerprint != m_buildFingerprint ) {
            pushEvent(WebRtcTransportEventType::Rejected,
                      0,
                      hostCreator,
                      "build_fingerprint_mismatch");
            return;
        }

        {
            // 首个合法 accepted 才能设置本地身份，重复消息保持幂等。
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
    /// @param connection 消息所属 P2P 连接。
    /// @param message description 或 candidate 消息。
    /// @param type 已解析的消息类型。
    ///
    /// @details PeerConnection 必须先由身份握手创建。description 分支设置远端
    /// SDP 并立即返回；其余调用只会以 candidate 路径读取 candidate 与 mid。
    /// C API 调用处于 RTC 锁内，避免 stop 同时删除句柄。
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
        // 协商消息早于身份接受表示协议顺序错误，而不是可缓存的正常乱序。
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
    /// @param connection 要拒绝的瞬时 Peer 信令连接。
    /// @param reason 稳定拒绝原因，同时发给远端并上报本地产品层。
    ///
    /// @details 即使发送 rejected 失败，本地仍上报 Rejected，使房主侧状态不会
    ///          因网络故障悬挂。远端最终会通过拒绝消息或信令关闭结束加入流程。
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
    /// @param connection 收到消息的控制或 Peer WebSocket。
    /// @param message 已解析的 JSON 对象。
    /// @param type 已读取的消息类型。
    /// @return 消息属于中心协议并已消费时返回 true；P2P 透明信令返回 false。
    ///
    /// @details
    /// 中心协议负责房间创建、封面结果、准入请求、配对就绪和服务端错误；它不
    /// 解析 SDP 等 P2P 内容。每个分支同时限制 ConnectionRole，防止 Peer 通道
    /// 伪造房间管理消息。未知类型交回 handleSignalingMessage 决定是否属于
    /// 透明 P2P 协议。
    ///
    /// @note 识别但格式非法的中心消息仍返回 true，并生成 Error 事件，避免其
    ///       继续落入其他协议分支造成二次解释。
    bool handleBrokerMessage(Connection&           connection,
                             const nlohmann::json& message,
                             std::string_view      type)
    {
        // room_created 只允许长期 HostControl 消费，并同时更新服务端 ICE 配置。
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
                // roomId 在完成全部校验后一次性发布给其他线程。
                std::scoped_lock lock(m_mutex);
                m_roomId = roomId;
            }
            pushEvent(WebRtcTransportEventType::RoomPublished,
                      m_hostId,
                      m_creator,
                      roomId);
            // 房间先对产品层可见，封面作为后续可选分块上传，失败单独报告。
            if ( !sendRoomCover(connection) ) {
                pushEvent(WebRtcTransportEventType::Error,
                          m_hostId,
                          m_creator,
                          "room_cover_upload_failed");
            }
            return true;
        }
        // 服务端拒绝封面不撤销房间，只上报可诊断错误供 UI 处理。
        if ( type == "room_cover_rejected" &&
             connection.role == ConnectionRole::HostControl ) {
            std::string reason;
            if ( !readStringField(message, "reason", reason) ) {
                reason = "room_cover_rejected";
            }
            pushEvent(WebRtcTransportEventType::Error,
                      m_hostId,
                      m_creator,
                      std::move(reason));
            return true;
        }
        // join_requested 由中心发送给房主控制连接，进入本地人工审批队列。
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
                // 忽略不属于当前已发布 roomId 的过期或串房请求。
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
                // 相同 requestId 更新 creator，保持服务端重发幂等。
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
        // join_pending 告知访客请求已登记，但尚未建立专用透明信令连接。
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
        // 访客在审批前离开时，房主必须同步移除待审批项并通知产品层。
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
        // relay_ready 表示中心已把本 Peer WebSocket 配对为透明信令通道。
        if ( type == "relay_ready" &&
             connection.role == ConnectionRole::Peer ) {
            if ( !updateIceServers(message) ) {
                pushEvent(WebRtcTransportEventType::Error,
                          connection.remotePeerId,
                          {},
                          "invalid_ice_configuration");
                return true;
            }
            // 访客先发送 join 身份；房主等待该消息与已批准 creator 做交叉校验。
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
        // 中心 error 对 HostControl 是普通 Error；对加入 Peer
        // 则代表终止性拒绝。
        if ( type == "error" ) {
            std::string reason;
            static_cast<void>(readStringField(message, "reason", reason));
            const bool hostControl =
                connection.role == ConnectionRole::HostControl;
            // 记录终止事件后，随后 WebSocket close 不得覆盖更精确的服务端原因。
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
        // p2p_ready 是对端已打开同一 DataChannel 的确认，双方收到后才能安全
        // 释放只用于 SDP/ICE 的瞬时 WebSocket。
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
    /// @param connection 消息所属连接。
    /// @param message 已无异常解析的 JSON 值。
    ///
    /// @details 首先要求顶层对象和字符串 type，再让中心协议优先消费。剩余消息
    ///          根据本地角色分派身份握手、拒绝或 SDP/ICE；角色不匹配和未知类型
    ///          被忽略，不允许跨职责执行管理操作。
    void handleSignalingMessage(Connection&           connection,
                                const nlohmann::json& message)
    {
        if ( !message.is_object() ) return;
        std::string type;
        if ( !readStringField(message, "type", type) ) return;
        // 中心消息一旦识别即终止处理，防止同名字段进入 P2P 分支。
        if ( handleBrokerMessage(connection, message, type) ) return;

        if ( m_isHost && connection.role == ConnectionRole::Peer &&
             type == "join" ) {
            handleJoin(connection, message);
        } else if ( !m_isHost && type == "accepted" ) {
            handleAccepted(connection, message);
        } else if ( !m_isHost && type == "rejected" ) {
            // P2P 拒绝是访客加入的终止原因，先设置门闩再上报事件。
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
    ///
    /// @details 在线人数从一名房主起算，只统计已经发送 PeerConnected 且尚未发送
    /// PeerDisconnected 的访客。控制句柄与计数在状态锁内取快照，实际发送仍在
    /// RTC 锁保护下进行；上报是目录展示信息，失败不改变本地连接状态。
    ///
    /// @warning 调用发生在 DataChannel 回调链，必须保持有界且不得等待产品层。
    ///
    /// @par 一致性边界
    /// 目录人数是最终一致的展示字段，不参与准入容量判断；真正容量仍由房主在
    /// handleJoin 分配 peer ID 时根据活动 joined 连接决定。因此人数消息发送失败
    /// 不会撤销已经建立的 P2P 会话，也不会允许超额访客加入。
    void sendParticipantCount()
    {
        std::scoped_lock rtcLock(m_rtcApiMutex);
        int              controlWebSocketId = -1;
        std::size_t      participants       = 1;
        {
            std::scoped_lock lock(m_mutex);
            if ( !m_isHost || m_stopping ) return;
            // 同一次遍历定位控制连接并计算真实活动访客数。
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
    /// @param connection 待读取的连接。
    /// @return 已上报连接且尚未上报断开时返回 true。
    /// @note 该辅助函数供回调错误抑制使用，读取过程由 m_mutex 串行化。
    bool isPeerConnected(const Connection& connection) const
    {
        std::scoped_lock lock(m_mutex);
        return connection.connectedEventSent &&
               !connection.disconnectedEventSent;
    }

    /// @brief 判断中心服务拒绝是否已经提供了更准确的终止原因。
    /// @param connection 待读取的 Peer 信令连接。
    /// @return error/rejected 已作为终止事件入队时返回 true。
    bool hasSignalingTerminalEvent(const Connection& connection) const
    {
        std::scoped_lock lock(m_mutex);
        return connection.signalingTerminalEventSent;
    }

    /// @brief 双方均确认 DataChannel 打开后释放瞬时信令连接。
    /// @param connection P2P 已建立的 Peer 连接。
    ///
    /// @details 本地 open 与远端 p2p_ready 两个门闩都成立才关闭 WebSocket，保证
    ///          对端不再需要传输 SDP/ICE。关闭只影响信令句柄，DataChannel 与
    ///          Connection 继续存活并承载业务帧。
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
        // 正常关闭会触发 onWebSocketClosed，但 isPeerConnected 会抑制断线事件。
        if ( websocketId >= 0 && rtcIsOpen(websocketId) ) {
            static_cast<void>(rtcClose(websocketId));
        }
    }

    /// @brief 中心 WebSocket 打开后声明当前连接职责。
    /// @param pointer libdatachannel 注册的稳定 Connection 指针。
    ///
    /// @details
    /// HostControl 发送 create_room；房主为已批准访客创建的 Peer 连接发送
    /// accept_join；访客 Peer 连接发送 join_room。openHandled 使用 acq_rel
    /// 保证异步打开回调与 openWebSocket 的同步补检只执行一次。
    ///
    /// @warning 这是库回调线程入口，不得把异常传播给 C ABI。
    static void onWebSocketOpen(int, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        if ( connection->openHandled.exchange(true,
                                              std::memory_order_acq_rel) ) {
            return;
        }
        Impl& owner = *connection->owner;

        // 三种角色共享 broker version，但携带的鉴权和身份字段不同。
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
    /// @param message 文本帧数据；libdatachannel 以负 size 表示文本消息。
    /// @param size 消息类型标记，非负值代表二进制帧并在此拒绝。
    /// @param pointer 稳定 Connection 指针。
    ///
    /// @details 使用无异常 JSON 解析模式，格式错误转为产品层 Error 事件。中心
    ///          WebSocket 只承载文本协议，二进制帧不会尝试解释。
    static void onWebSocketMessage(int, const char* message, int size,
                                   void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner || !message || size >= 0 ) {
            return;
        }
        // allow_exceptions=false 符合项目禁用异常约束。
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
    /// @param pointer 稳定 Connection 指针。
    ///
    /// @details HostControl 关闭会使公开房间管理失效，始终报告 Error。Peer 信令
    ///          在 DataChannel 已连或中心已经给出终止原因时正常结束；其他情况
    ///          才转换为 signaling_closed 断线事件。
    ///
    /// @par 原因优先级
    /// 中心 error/rejected 已经携带业务原因时，signalingTerminalEventSent 阻止
    /// close 回调再生成宽泛 signaling_closed；DataChannel 已连接时关闭则是主动
    /// 释放协商通道。只有两者都不成立，关闭才代表加入过程意外中断。
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
    /// @param error libdatachannel 提供的诊断文本，可为空。
    /// @param pointer 稳定 Connection 指针。
    ///
    /// @details 已建立 P2P 或已收到终止拒绝的 Peer 不再报告瞬时信令错误，避免
    ///          正常信令释放覆盖业务连接状态。HostControl 错误始终可见。
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
    /// @param sdp libdatachannel 生成的 SDP 文本。
    /// @param type offer 或 answer 类型文本。
    /// @param pointer 对应 Peer 连接。
    ///
    /// @details SDP 通过透明信令 WebSocket 转发。若发送失败但 DataChannel 已经
    ///          建立，说明迟到协商消息已无必要，因此不制造错误事件。
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
    /// @param candidate ICE candidate 文本。
    /// @param mid 媒体标识。
    /// @param pointer 对应 Peer 连接。
    ///
    /// @details candidate 与 SDP 使用相同失败抑制规则；P2P 已连后信令关闭属于
    ///          正常生命周期，迟到 candidate 发送失败不应让会话降级。
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
    /// @param state libdatachannel 的连接状态。
    /// @param pointer 对应 Peer 连接。
    ///
    /// @details FAILED 先上报明确错误；DISCONNECTED/CLOSED 统一通过
    ///          notifyDisconnected 生成幂等离开事件。其他过渡态不暴露给产品层。
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
    /// @param dataChannelId 新到达的远端 DataChannel 句柄。
    /// @param pointer 所属房主 PeerConnection 的 Connection。
    ///
    /// @details 房主只接受稳定 collaboration label，防止同一 PeerConnection 上
    ///          其他用途通道被误当成协作帧入口。无上下文、停止中或标签错误的
    ///          通道立即删除；合法通道交给统一配置函数注册回调。
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
        // 固定缓冲足以容纳稳定标签，零初始化保证 string_view 正确终止。
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
    /// @param pointer 所属 Connection。
    ///
    /// @details 首次 open 原子地标记 connectedEventSent，并向产品层上报完整远端
    /// 身份。房主更新目录人数。随后通过仍打开的信令通道发送 p2p_ready；只有
    /// 双方都确认后 closeSignalingIfReady 才关闭该通道。
    ///
    /// @par 事件时序
    /// PeerConnected 在 ready 发送前入队，因为 DataChannel 已经可以承载业务帧；
    /// ready 仅协调信令释放。若 ready 发送失败，连接事实仍保留，同时追加 Error
    /// 供上层决定是否继续使用通道或重连。
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
        // 事件先于人数上报和 ready 发送，使产品层不会错过已建立连接事实。
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
    /// @param message 完整二进制帧缓冲。
    /// @param size 正数表示二进制负载字节数。
    /// @param pointer 所属 Connection。
    ///
    /// @details 回调立即复制库拥有的临时缓冲到 TransportPacket，再在状态锁内
    /// 校验连接仍活动并压入有界队列。队列已满时丢弃新帧，避免 RTC 线程无界
    /// 分配；上层协议会把缺失帧视为同步失败并走重连/重同步路径。
    ///
    /// @warning 该回调可能运行在 RTC 工作线程，禁止阻塞文件 I/O 或等待产品层。
    ///
    /// @par 接收门槛
    /// 帧只有在连接完成身份 join、已经上报 Connected 且传输未停止时才能入队。
    /// 这样可丢弃身份握手前的抢跑数据和关闭竞态中的迟到帧，不让未认证数据
    /// 进入上层协议解析器。
    static void onDataChannelMessage(int, const char* message, int size,
                                     void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner || !message || size <= 0 ||
             size > MAX_DATA_CHANNEL_MESSAGE_BYTES ) {
            return;
        }
        // 先在锁外完成 payload 分配与复制，缩短共享队列临界区。
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
    /// @param pointer 所属 Connection。
    /// @note 多个底层关闭状态可能重复到达，由 notifyDisconnected 统一去重。
    static void onDataChannelClosed(int, void* pointer)
    {
        auto* connection = static_cast<Connection*>(pointer);
        if ( !connection || !connection->owner ) return;
        connection->owner->notifyDisconnected(*connection,
                                              "data_channel_closed");
    }

    /// @brief DataChannel 错误回调。
    /// @param error 库诊断文本，可为空。
    /// @param pointer 所属 Connection。
    /// @note 错误事件与后续关闭事件职责不同，产品层可先显示原因再处理离开。
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
    /// @param connection 已关闭或由房主主动移除的 Peer 连接。
    /// @param detail 稳定断线原因。
    ///
    /// @details 在状态锁内设置 disconnectedEventSent 并撤销 joined/connected，
    /// 随后锁外入队事件。只有曾经真正连接的房主侧访客才触发目录人数更新，
    /// 未完成握手的失败连接不会错误减少公开人数。
    ///
    /// @par 状态转换
    /// disconnectedEventSent 先置位，随后 connectedEventSent 与 joined 清零。
    /// 事件入队发生在锁外，但其他关闭回调已经会被门闩拒绝。remotePeerId、
    /// ParticipantId 和 SessionId 保留到 stop，供离开事件携带完整清理身份。
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
        // 保留稳定参与者与会话 ID，使上层能清理正确的远端状态。
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
    ///
    /// @note 使用 recursive_mutex 是因为部分 libdatachannel API
    /// 可能同步触发回调，
    ///       回调会沿 sendSignal 等路径再次取得同一 RTC 锁。
    mutable std::recursive_mutex m_rtcApiMutex;
    /// @brief 保护跨 libdatachannel 回调线程共享的队列和连接表。
    /// @warning 不得在持有此锁时删除 RTC 句柄或执行可能同步回调的外部操作。
    mutable std::mutex m_mutex;
    /// @brief 传输是否已启动。
    /// @note 由 startHost/connectToHost 置位，stop 开始时立即清除。
    bool m_running = false;
    /// @brief 当前是否正在析构网络句柄。
    /// @note 回调和入队入口据此静默放弃新工作，避免 stop 期间状态复活。
    bool m_stopping = false;
    /// @brief 当前角色是否为房主。
    /// @note 决定消息分派、peer ID 分配和目录人数上报职责。
    bool m_isHost = false;
    /// @brief 当前 Creator。
    /// @note 已通过 CreatorIdentity 统一规范化，可安全用于协议显示字段。
    std::string m_creator;
    /// @brief 当前应用配置持久化的稳定协作者标识。
    ParticipantId m_participantId;
    /// @brief 当前加入流程生成的操作会话标识。
    OperationSessionId m_sessionId;
    /// @brief 当前主程序二进制的 SHA-256 构建指纹。
    /// @note 身份握手使用短协议格式的有效指纹，不接受任意用户文本。
    std::string m_buildFingerprint;
    /// @brief 房主是否在身份握手中强制构建指纹相等。
    bool m_requireMatchingBuildFingerprint = true;
    /// @brief 公网目录展示名称。
    std::string m_roomName;
    /// @brief 房间注册后分块提交给目录服务的 Base64 JPEG 缩略图。
    std::string m_roomCoverImage;
    /// @brief 中心服务分配的公开房间标识。
    std::string m_roomId;
    /// @brief 中心信令 URL。
    /// @note 在会话启动前由端点统一生成，空值会使入口失败。
    std::string m_signalingUrl;
    /// @brief 房主控制连接鉴权令牌。
    /// @note 仅随 create/accept/reject 等 broker 控制消息发送，不进入 P2P
    /// 事件。
    std::string m_ownerToken;
    /// @brief 中心服务下发的 ICE URI。
    std::vector<std::string> m_iceServers;
    /// @brief 当前本地 PeerId。
    /// @note 房主启动时确定；访客在 accepted 校验成功前保持零。
    PeerId m_localPeerId = 0;
    /// @brief 房主 PeerId。
    /// @note 由上层协议配置，目前用于访客预先寻址和分配访客槽位起点。
    PeerId m_hostId = 0;
    /// @brief 房间总人数上限。
    std::size_t m_maxParticipants = MAX_COLLABORATION_PARTICIPANTS;
    /// @brief 房主控制连接、房主访客连接或访客唯一连接。
    /// @note unique_ptr 保证 vector 重分配不会改变注册给 C API 的 Connection
    /// 地址。
    std::vector<std::unique_ptr<Connection>> m_connections;
    /// @brief 等待产品层房主批准或拒绝的中心服务加入请求。
    std::unordered_map<std::string, std::string> m_pendingJoinRequests;
    /// @brief DataChannel 收到的完整协作协议帧。
    /// @warning 队列有固定上限；产品层必须持续非阻塞排空。
    std::deque<TransportPacket> m_incomingPackets;
    /// @brief 等待产品层消费的连接生命周期事件。
    /// @warning 队列满时淘汰最旧项，因此事件消费者不得长期停滞。
    std::deque<WebRtcTransportEvent> m_events;
};

/// @brief 创建独占的 WebRTC 传输实现状态。
/// @note PImpl 隔离 libdatachannel C API 与同步原语，公开头无需暴露第三方定义。
WebRtcTransport::WebRtcTransport() : m_impl(std::make_unique<Impl>()) {}

/// @brief 销毁传输并由 Impl 析构函数幂等停止全部连接。
WebRtcTransport::~WebRtcTransport() = default;

/// @brief 启动房主目录发布流程。
/// @param config 房主连接配置。
/// @return 参数和 WebSocket 创建成功时返回 true。
bool WebRtcTransport::startHost(const WebRtcHostConfig& config)
{
    return m_impl->startHost(config);
}

/// @brief 启动访客加入公开房间流程。
/// @param config 访客连接配置。
/// @return 参数和 WebSocket 创建成功时返回 true。
bool WebRtcTransport::connectToHost(const WebRtcGuestConfig& config)
{
    return m_impl->connectToHost(config);
}

/// @brief 批准待处理加入请求。
/// @param requestId 中心服务请求 ID。
/// @return 专用 P2P 信令连接启动成功时返回 true。
bool WebRtcTransport::approveJoinRequest(std::string_view requestId)
{
    return m_impl->approveJoinRequest(requestId);
}

/// @brief 拒绝待处理加入请求。
/// @param requestId 中心服务请求 ID。
/// @param reason 返回给访客的原因。
/// @return 拒绝消息成功发送时返回 true。
bool WebRtcTransport::rejectJoinRequest(std::string_view requestId,
                                        std::string_view reason)
{
    return m_impl->rejectJoinRequest(requestId, reason);
}

/// @brief 由房主主动移除已连接访客。
/// @param peerId 目标访客 peer ID。
/// @param detail 可选断线原因。
/// @return 目标存在且关闭已发起时返回 true。
bool WebRtcTransport::disconnectPeer(PeerId peerId, std::string detail)
{
    return m_impl->disconnectPeer(peerId, std::move(detail));
}

/// @brief 幂等停止当前会话并清理全部 RTC 句柄与队列。
void WebRtcTransport::stop()
{
    m_impl->stop();
}

/// @brief 查询传输是否处于运行状态。
/// @return 当前运行状态快照。
bool WebRtcTransport::isRunning() const
{
    return m_impl->isRunning();
}

/// @brief 查询当前会话是否为房主角色。
/// @return 房主角色返回 true。
bool WebRtcTransport::isHost() const
{
    return m_impl->isHost();
}

/// @brief 获取当前本地 peer ID。
/// @return 房主 ID、访客获准后的 ID 或未分配时的零。
PeerId WebRtcTransport::localPeerId() const
{
    return m_impl->localPeerId();
}

/// @brief 获取当前公开房间 ID。
/// @return 线程安全复制的 roomId。
std::string WebRtcTransport::roomId() const
{
    return m_impl->roomId();
}

/// @brief 非阻塞取出最早的连接生命周期事件。
/// @param event 成功时接收事件。
/// @return 存在事件时返回 true。
bool WebRtcTransport::receiveEvent(WebRtcTransportEvent& event)
{
    return m_impl->receiveEvent(event);
}

/// @brief 向指定 peer 发送完整二进制协作帧。
/// @param recipientId 目标 peer ID。
/// @param payload 发送期间有效的帧视图。
/// @return 底层通道接受消息时返回 true。
bool WebRtcTransport::send(PeerId                        recipientId,
                           std::span<const std::uint8_t> payload)
{
    return m_impl->send(recipientId, payload);
}

/// @brief 非阻塞取出最早的入站协作帧。
/// @param packet 成功时接收帧。
/// @return 存在帧时返回 true。
bool WebRtcTransport::receive(TransportPacket& packet)
{
    return m_impl->receive(packet);
}
}  // namespace MMM::Network::Collaboration
