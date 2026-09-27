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
/// @note 每条服务控制消息都必须显式携带该版本，配对后的透明信令不再解析。
constexpr std::uint64_t DIRECTORY_PROTOCOL_VERSION = 1;
/// @brief 中心服务接受的 WebSocket 路径。
/// @note 路径检查在客户端进入状态表前完成，隔离同端口上的其他服务入口。
constexpr std::string_view SIGNALING_PATH = "/mmm-collaboration";
/// @brief 单条信令消息上限。
/// @note 同时传给 libdatachannel 和 sendJson，约束入站与出站内存占用。
constexpr int MAX_SIGNALING_MESSAGE_BYTES = 256 * 1024;
/// @brief 单间房卡封面 Base64 文本上限。
/// @note 上限针对去除 data URI 前缀后的编码文本，不是 JPEG 解码字节数。
constexpr std::size_t MAX_ROOM_COVER_BASE64_BYTES = 96U * 1024U;
/// @brief 单条封面上传消息携带的 Base64 文本上限。
/// @note 分块保证带 JSON 包装后的控制消息显著低于信令总上限。
constexpr std::size_t MAX_ROOM_COVER_CHUNK_BYTES = 8U * 1024U;
/// @brief 单次封面上传允许的最大分块数量。
/// @note 向上取整覆盖恰好达到封面总上限的最后一个不足分块。
constexpr std::size_t MAX_ROOM_COVER_CHUNKS =
    (MAX_ROOM_COVER_BASE64_BYTES + MAX_ROOM_COVER_CHUNK_BYTES - 1U) /
    MAX_ROOM_COVER_CHUNK_BYTES;
/// @brief 回调线程允许积压的事件上限。
/// @note 队列满时丢弃新事件，优先保护服务进程不被连接洪泛耗尽内存。
constexpr std::size_t MAX_CALLBACK_EVENTS = 8192;
/// @brief 未声明身份的连接保留时间。
/// @note 客户端完成首条角色消息后不再受该短超时约束。
constexpr auto UNKNOWN_CLIENT_TIMEOUT = std::chrono::seconds(15);
/// @brief 等待房主接受的加入请求保留时间。
/// @note 过期访客进入关闭流程，并通知仍在线的房主取消对应请求。
constexpr auto JOIN_REQUEST_TIMEOUT = std::chrono::minutes(2);
/// @brief 已关闭句柄延迟删除时间，避免 libdatachannel 立即复用整数句柄。
/// @note 连接 generation 仍会过滤迟到事件，宽限期额外保护第三方回调上下文。
constexpr auto RETIRED_WEBSOCKET_GRACE = std::chrono::seconds(5);
/// @brief 随机标识使用的无歧义字符表。
/// @note 排除 0/O、1/I 等易混字符，便于用户或日志人工核对房间 ID。
constexpr std::string_view IDENTIFIER_ALPHABET =
    "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

/// @brief 从 JSON 对象读取字符串字段。
/// @details 远端 JSON 不可信，字段缺失或类型不符时不修改上层状态；成功时复制
/// 字符串以脱离 message 解析树的生命周期。
/// @param object 输入 JSON 对象。
/// @param key 字段名。
/// @param value 输出字符串。
/// @return 字段存在且为字符串时返回 true。
bool readStringField(const nlohmann::json& object, std::string_view key,
                     std::string& value)
{
    // 调用方已经验证 message 为对象，但此处仍只通过 find 访问明确键。
    const auto iterator = object.find(key);
    if ( iterator == object.end() || !iterator->is_string() ) return false;
    value = iterator->get_ref<const std::string&>();
    return true;
}

/// @brief 从 JSON 对象读取无符号整数字段。
/// @details 只接受 JSON unsigned 类型，负数和浮点数不能经隐式转换进入容量、
/// 分块序号或协议版本等边界字段。
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
/// @details
/// 房间名与 Creator 允许任意 UTF-8 高位字节，但禁止空文本、超长正文、C0 控制
/// 字符和 DEL，防止目录消息注入换行或不可见状态。UTF-8 结构由客户端输入层
/// 保证，本函数只施加服务端展示安全边界。
/// @param value 输入文本。
/// @param maxBytes UTF-8 字节上限。
/// @return 文本可公开展示时返回 true。
bool isValidDisplayText(std::string_view value, std::size_t maxBytes)
{
    // 长度按协议传输字节计算，不按 Unicode 码点计算。
    if ( value.empty() || value.size() > maxBytes ) return false;
    return std::none_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        // 先转 unsigned char，避免有符号 char 高位字节错误落入控制范围。
        return byte < 0x20U || byte == 0x7FU;
    });
}

/// @brief 校验可安全保存在目录内的 Base64 图片文本。
/// @details
/// 空封面合法；非空值必须满足总长度、四字节对齐、最多两个尾部填充，并且正文
/// 只含标准 Base64 字母表。校验不解码图片格式，服务端只负责有界存储和转发。
/// @param value 不含 data URI 前缀的 Base64 数据。
/// @return 空值或长度、字符集与填充均合法时返回 true。
bool isValidRoomCoverImage(std::string_view value)
{
    // 四字节对齐和总上限先快速排除无法成立的载荷。
    if ( value.empty() ) return true;
    if ( value.size() > MAX_ROOM_COVER_BASE64_BYTES ||
         value.size() % 4U != 0U ) {
        return false;
    }

    std::size_t padding = 0U;
    // 只从末尾统计 `=`，正文中的等号会在字符集检查中失败。
    for ( std::size_t index = value.size();
          index > 0U && value[index - 1U] == '=';
          --index ) {
        ++padding;
    }
    if ( padding > 2U ) return false;

    // 排除尾部填充后，正文每个字符必须属于标准 Base64 字母表。
    const std::size_t payloadLength = value.size() - padding;
    return std::all_of(
        value.begin(),
        value.begin() + static_cast<std::ptrdiff_t>(payloadLength),
        [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return std::isalnum(byte) != 0 || character == '+' ||
                   character == '/';
        });
}

/// @brief 校验只用于房主控制连接的高熵令牌格式。
/// @details
/// 令牌从客户端生成且不进入公开目录，服务端只接受 32～128 字节 URL-safe
/// 字符串。该边界避免空令牌授权，也便于无转义地在 JSON 中往返。
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

/// @brief 校验客户端主程序 SHA-256 构建指纹。
/// @details 指纹必须是规范化的 64 位小写十六进制；空值仅为兼容旧客户端，
/// 一旦字段出现就必须满足完整格式。
bool isValidBuildFingerprint(std::string_view value)
{
    return value.size() == 64U &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

/// @brief 校验可安全回传给访客的稳定拒绝原因码。
/// @details 原因码由房主提供但会进入服务端生成的错误消息，只允许小写字母、
/// 数字与下划线，避免把任意展示文本当作协议控制值传播。
bool isValidReasonCode(std::string_view value)
{
    return !value.empty() && value.size() <= 64U &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') ||
                      character == '_';
           });
}
}  // namespace

class CollaborationSignalingServer::Impl
{
public:
    /// @brief WebSocket 客户端在目录服务中的职责。
    /// @details
    /// 连接从 Unknown 开始，只能通过首条合法控制消息进入目录订阅、房主控制或
    /// 等待访客状态。AcceptJoin 创建的新房主信令连接与访客进入 Paired，之后
    /// 服务端不再解析其正文。Closing 防止重复关闭和状态重入。
    enum class ClientRole {
        /// @brief 尚未声明用途的新连接。
        Unknown,
        /// @brief 订阅公开房间列表和按需封面的目录客户端。
        Directory,
        /// @brief 创建房间并管理人数、封面和加入请求的房主控制连接。
        HostControl,
        /// @brief 已提交加入请求、等待房主决策的访客。
        PendingGuest,
        /// @brief 已与专用房主连接配对并透明转发信令的端点。
        Paired,
        /// @brief 已请求关闭、等待第三方关闭回调的连接。
        Closing,
    };

    /// @brief libdatachannel 回调提交给主循环的事件类型。
    /// @details 回调线程只分类并复制数据，所有状态机修改统一由 update 消费。
    enum class CallbackEventType {
        Connected,
        Message,
        Closed,
        Error,
    };

    /// @brief 一条跨回调线程传递的 WebSocket 事件。
    /// @details generation 与 websocketId 共同标识连接生命周期，防止整数句柄
    /// 被第三方复用后，旧关闭或消息事件作用到新连接。
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
    /// @details 上下文由 generation 映射持有，直到 WebSocket 越过退役宽限期；
    /// 第三方回调的裸指针因此不会指向已移动的 Client 容器元素。
    struct CallbackContext {
        /// @brief 所属服务端实现。
        Impl* owner = nullptr;
        /// @brief WebSocket 句柄。
        int websocketId = -1;
        /// @brief 当前句柄的连接代次。
        std::uint64_t generation = 0;
    };

    /// @brief 一个已接入 WebSocket 客户端的主循环状态。
    /// @details 该结构只在服务主线程访问。role 决定允许的控制消息集合，roomId
    /// 和 requestId 关联房间状态，partnerWebSocketId 仅在 Paired 状态有效。
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
    /// @details
    /// Room 保存目录展示字段、房主控制凭据、容量和封面分块组装状态。
    /// ownerToken 永不写入房间列表；coverImage 只在目录客户端显式请求时下发。
    /// participants 由房主控制连接上报，不以临时信令配对数量推断。
    struct Room {
        /// @brief 公共房间 ID。
        std::string roomId;
        /// @brief 房间展示名称。
        std::string roomName;
        /// @brief 房主展示身份。
        std::string hostCreator;
        /// @brief 按需下发给目录客户端的 Base64 JPEG 封面缩略图。
        std::string coverImage;
        /// @brief 尚未接收完整的 Base64 JPEG 封面缩略图。
        std::string pendingCoverImage;
        /// @brief 当前封面上传等待的下一分块序号。
        std::size_t nextCoverChunkIndex = 0;
        /// @brief 当前封面上传声明的总分块数。
        std::size_t expectedCoverChunks = 0;
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
    /// @details 请求同时关联访客连接与目标房间；房主通过另一个 Unknown 连接携带
    /// requestId 和 ownerToken 接受，避免控制连接本身被占用为透明中继。
    struct PendingJoin {
        /// @brief 请求 ID。
        std::string requestId;
        /// @brief 目标房间 ID。
        std::string roomId;
        /// @brief 访客 WebSocket。
        int guestWebSocketId = -1;
        /// @brief 访客展示身份。
        std::string creator;
        /// @brief 访客主程序二进制的 SHA-256 构建指纹；旧客户端未提交时为空。
        std::string buildFingerprint;
        /// @brief 请求创建时间。
        std::chrono::steady_clock::time_point createdAt =
            std::chrono::steady_clock::now();
    };

    /// @brief 已清除回调、等待安全删除的 WebSocket 句柄。
    /// @details close 后先解除第三方回调并保留上下文，宽限期结束才删除句柄和
    /// generation 上下文，降低 libdatachannel 迟到回调与句柄复用风险。
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
    /// @warning stop 会清理服务和连接句柄，只在对象低频销毁路径执行。
    ~Impl() { stop(); }

    /// @brief 启动 WebSocket 信令服务。
    /// @details
    /// 拒绝重复启动、零容量和缺失 TLS 证书配置；随后把持久配置转移到成员，
    /// 以保证传给 libdatachannel 的字符串指针覆盖服务生命周期。创建成功后读取
    /// 实际监听端口，只有端口有效才发布 running=true。
    /// @par 启动状态不变量
    /// - running 为 true 时拒绝第二次 start。
    /// - maxRooms 与 maxClients 必须同时非零。
    /// - TLS 开启时证书和私钥路径必须同时配置。
    /// - 回调许可早于服务创建发布，失败时必须撤回。
    /// - serverId 只有在创建成功后进入成员状态。
    /// - listeningPort 只有在第三方返回有效端口后更新。
    /// - running 最后发布，使查询者不会看到半初始化服务。
    /// @param config 监听地址、端口、TLS、容量和 ICE 列表配置。
    /// @return 服务句柄与实际端口均有效时返回 true。
    bool start(CollaborationSignalingServerConfig config)
    {
        // 服务必须处于停止态，且房间与连接容量都至少允许一个对象。
        if ( m_running.load(std::memory_order_acquire) ||
             config.maxRooms == 0 || config.maxClients == 0 ) {
            return false;
        }
        if ( config.enableTls && (config.certificatePemFile.empty() ||
                                  config.keyPemFile.empty()) ) {
            // TLS 只允许证书和私钥成对出现，禁止静默退回明文。
            return false;
        }

        m_config = std::move(config);
        // C API 指针引用 m_config 内字符串，因此配置先移动到长生命周期成员。
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
        // 第三方层先限制单消息大小，主循环仍验证 JSON 字段级上限。
        serverConfig.maxMessageSize = MAX_SIGNALING_MESSAGE_BYTES;

        m_acceptCallbacks.store(true, std::memory_order_release);
        // 先允许回调，再创建服务，避免创建期间到达连接被误删。
        const int serverId =
            rtcCreateWebSocketServer(&serverConfig, &Impl::onWebSocketClient);
        if ( serverId < 0 ) {
            // 创建失败撤回回调许可，保持其他状态为停止态。
            m_acceptCallbacks.store(false, std::memory_order_release);
            return false;
        }
        rtcSetUserPointer(serverId, this);

        // 端口零允许系统选择空闲端口，因此必须从服务句柄读取实际值。
        const int actualPort = rtcGetWebSocketServerPort(serverId);
        if ( actualPort <= 0 || actualPort > 65535 ) {
            // 无法获得可发布端口时删除刚创建的服务，防止半启动状态。
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
    /// @details
    /// 先禁止新回调并清除 running，再删除监听器；随后清理活动和退役连接、
    /// 房间、加入请求以及回调队列。函数允许重复调用，析构和显式 stop
    /// 可安全重叠。
    /// @par 停止顺序
    /// - 禁止回调访问当前实例。
    /// - 对外发布停止状态。
    /// - 删除监听服务，阻止新连接进入。
    /// - 清除活动和退役 WebSocket 句柄。
    /// - 清空房间、请求与目录脏状态。
    /// - 在互斥量下最后释放回调事件和稳定上下文。
    /// @warning 会同步删除所有第三方网络句柄，只用于服务关闭路径。
    void stop()
    {
        // release/acquire 配对阻止新的回调继续把事件写入即将清空的队列。
        m_acceptCallbacks.store(false, std::memory_order_release);
        m_running.store(false, std::memory_order_release);

        const int serverId = std::exchange(m_serverId, -1);
        // 先移走句柄使重复 stop 看见无服务可删。
        if ( serverId >= 0 ) rtcDeleteWebSocketServer(serverId);

        for ( const auto& [websocketId, client] : m_clients ) {
            static_cast<void>(client);
            deleteWebSocketSafely(websocketId);
        }
        for ( const auto& retired : m_retiredWebSockets ) {
            // 关闭服务时无需继续等待宽限期，回调入口已整体禁止。
            rtcDeleteWebSocket(retired.websocketId);
        }
        m_retiredWebSockets.clear();
        m_clients.clear();
        m_rooms.clear();
        m_pendingJoins.clear();
        m_listeningPort  = 0;
        m_directoryDirty = false;

        // 最后在互斥量下释放事件和回调上下文，确保回调线程不并发访问。
        std::scoped_lock lock(m_callbackMutex);
        m_callbackEvents.clear();
        m_callbackContexts.clear();
    }

    /// @brief 在服务主线程消费回调事件与超时。
    /// @details
    /// 先一次性交换回调队列以缩短锁持有时间，再优先登记同批 Connected 事件，
    /// 使紧随连接到达的 Message 不会因客户端尚未入表而丢失。第二遍按原顺序处理
    /// 消息、关闭和错误，最后执行超时、退役删除及合并后的目录广播。
    /// @par 单轮阶段
    /// - 从回调线程队列取得事件所有权。
    /// - 预处理所有 Connected 并验证容量与路径。
    /// - 按队列顺序处理 Message、Closed 与 Error。
    /// - 关闭未知超时和加入请求超时连接。
    /// - 删除越过宽限期的退役句柄与上下文。
    /// - 若公开房间字段变化则只广播一次目录快照。
    /// @note 同轮新入队事件留到下一次 update，保持单轮工作量有界。
    /// @warning 通常由服务循环高频调用；不得加入网络等待、文件 I/O 或全局锁。
    void update()
    {
        if ( !m_running.load(std::memory_order_acquire) ) return;

        std::deque<CallbackEvent> events;
        {
            std::scoped_lock lock(m_callbackMutex);
            // swap 把待处理所有权移到主线程，回调可立即继续入队新事件。
            events.swap(m_callbackEvents);
        }
        for ( const auto& event : events ) {
            // 同一批次先建立客户端状态，保留连接后立即发消息的合法顺序。
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
                // 错误与关闭共享幂等清理流程，额外记录第三方诊断文本。
                XWARN("Collaboration signaling WebSocket {} error: {}",
                      event.websocketId,
                      event.payload);
                processClosed(event.websocketId, event.generation);
                break;
            }
        }

        expireIdleClientsAndJoins();
        // 低频维护集中在事件批次后，避免修改正在遍历的容器。
        deleteRetiredWebSockets();
        if ( m_directoryDirty ) broadcastRoomList();
    }

    /// @brief 返回运行状态。
    /// @return 服务已完成句柄和端口发布时返回 true。
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    /// @brief 返回实际监听端口。
    /// @return 系统分配或配置的有效端口；停止状态为零。
    std::uint16_t listeningPort() const { return m_listeningPort; }

    /// @brief 返回在线房间数。
    /// @return 主循环当前持有的公开房间数量。
    std::size_t roomCount() const { return m_rooms.size(); }

    /// @brief 返回已接入客户端数。
    /// @return 包含所有非退役角色的活动客户端数量。
    std::size_t clientCount() const { return m_clients.size(); }

private:
    /// @brief 清除回调后删除服务端 WebSocket，避免句柄复用命中迟到事件。
    /// @details 仅在整体 stop
    /// 且回调入口已禁用时立即删除；常规关闭使用退役队列。
    static void deleteWebSocketSafely(int websocketId)
    {
        // 先逐项解除回调和 user pointer，第三方删除期间不能再访问 Impl。
        rtcSetMessageCallback(websocketId, nullptr);
        rtcSetClosedCallback(websocketId, nullptr);
        rtcSetErrorCallback(websocketId, nullptr);
        rtcSetUserPointer(websocketId, nullptr);
        rtcDeleteWebSocket(websocketId);
    }

    /// @brief 释放已经清除全部第三方回调的连接代次上下文。
    /// @param generation 要从稳定上下文表移除的连接代次。
    void releaseCallbackContext(std::uint64_t generation)
    {
        std::scoped_lock lock(m_callbackMutex);
        m_callbackContexts.erase(generation);
    }

    /// @brief 清除回调并把关闭句柄放入延迟删除队列。
    /// @details 常规关闭先切断所有回调入口并请求 WebSocket
    /// close，但延迟真正删除 句柄和
    /// CallbackContext，使已经在第三方线程途中产生的回调安全失效。
    void retireWebSocket(int websocketId, std::uint64_t generation)
    {
        // user pointer 在 close 前清空，新的第三方回调无法再取得上下文。
        rtcSetMessageCallback(websocketId, nullptr);
        rtcSetClosedCallback(websocketId, nullptr);
        rtcSetErrorCallback(websocketId, nullptr);
        rtcSetUserPointer(websocketId, nullptr);
        static_cast<void>(rtcClose(websocketId));
        // steady_clock 时间与 generation 共同决定后续安全回收。
        m_retiredWebSockets.push_back(
            { websocketId, generation, std::chrono::steady_clock::now() });
    }

    /// @brief 删除已越过回调迟到窗口的退役 WebSocket。
    /// @details 队列按退役时间追加，因此只需从头删除连续到期项；未到期首项之后
    /// 的句柄必然也未到期。
    void deleteRetiredWebSockets()
    {
        const auto now = std::chrono::steady_clock::now();
        while ( !m_retiredWebSockets.empty() &&
                now - m_retiredWebSockets.front().retiredAt >=
                    RETIRED_WEBSOCKET_GRACE ) {
            const auto retired = m_retiredWebSockets.front();
            // 先删除第三方句柄，再释放它可能曾引用的稳定上下文。
            rtcDeleteWebSocket(retired.websocketId);
            releaseCallbackContext(retired.generation);
            m_retiredWebSockets.pop_front();
        }
    }

    /// @brief 生成不与现有房间或请求冲突的随机标识。
    /// @details 仅负责从无歧义字符表随机取样；具体唯一性由 room/request
    /// 包装函数 对各自状态表检查。
    /// @param length 期望标识字符数。
    /// @return 固定长度的随机大写字母数字标识。
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
    /// @details 最多尝试 32 次十字符候选；高熵空间下碰撞极低，耗尽时返回空值
    /// 让创建流程显式拒绝，而不是无限循环阻塞主线程。
    std::string generateRoomId()
    {
        for ( std::size_t attempt = 0; attempt < 32; ++attempt ) {
            auto candidate = generateIdentifier(10);
            if ( !m_rooms.contains(candidate) ) return candidate;
        }
        return {};
    }

    /// @brief 生成唯一等待请求 ID。
    /// @details 十六字符请求空间独立于房间空间，只需相对当前 PendingJoin
    /// 表唯一。
    std::string generateRequestId()
    {
        for ( std::size_t attempt = 0; attempt < 32; ++attempt ) {
            auto candidate = generateIdentifier(16);
            if ( !m_pendingJoins.contains(candidate) ) return candidate;
        }
        return {};
    }

    /// @brief 把 ICE URI 列表写入服务消息。
    /// @details 创建房间和配对完成消息使用同一配置快照，客户端据此建立 P2P
    /// 候选。
    void appendIceServers(nlohmann::json& message) const
    {
        message["iceServers"] = m_config.iceServers;
    }

    /// @brief 发送 JSON 文本消息。
    /// @details 在主线程序列化为紧凑文本，并再次检查与入站相同的总消息上限；
    /// C API 的负长度表示零结尾文本帧。
    /// @return 序列化结果未超限且 libdatachannel 接受消息时返回 true。
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
    /// @details 尽力发送版本化 error 后进入统一 close 流程；即使写回失败也必须
    /// 关闭违反状态机或输入约束的连接。
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
    /// @details 把角色先改为 Closing 防止重复
    /// rtcClose；实际容器清理等待关闭回调。
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
    /// @details
    /// 新连接只有在容量未满且 WebSocket 路径完全匹配时才建立 Unknown 客户端。
    /// 拒绝发生在状态表外，直接进入退役队列，不向未知协议端发送目录错误。
    void processConnected(int websocketId, std::uint64_t generation)
    {
        // 容量按已纳入 m_clients 的活动连接计算，不包含退役句柄。
        if ( m_clients.size() >= m_config.maxClients ) {
            retireWebSocket(websocketId, generation);
            return;
        }

        std::array<char, 128> path{};
        // 固定缓冲足以容纳唯一合法短路径，过长路径读取失败并拒绝。
        const int pathLength = rtcGetWebSocketPath(
            websocketId, path.data(), static_cast<int>(path.size()));
        if ( pathLength <= 0 ||
             std::string_view(path.data()) != SIGNALING_PATH ) {
            retireWebSocket(websocketId, generation);
            return;
        }
        Client client;
        // 保存回调 generation，后续每条事件都必须匹配同一连接生命期。
        client.generation = generation;
        m_clients.emplace(websocketId, std::move(client));
    }

    /// @brief 处理一条客户端消息或在配对后透明转发。
    /// @details
    /// 首先用 websocketId 与 generation 验证事件仍属于当前连接并刷新活动时间。
    /// Paired 状态不解析正文，按文本/二进制原样转发给对端；未配对状态只接受
    /// 版本化 JSON 文本，并依据当前 ClientRole 分派允许的消息类型。
    /// 任何格式、版本或状态错误都发送稳定原因码并关闭连接。
    void processMessage(int websocketId, std::uint64_t generation,
                        std::string payload, bool textMessage)
    {
        // 迟到事件或已复用句柄不应触碰当前客户端状态。
        const auto clientIterator = m_clients.find(websocketId);
        if ( clientIterator == m_clients.end() ||
             clientIterator->second.generation != generation ) {
            return;
        }
        Client& client      = clientIterator->second;
        client.lastActivity = std::chrono::steady_clock::now();

        if ( client.role == ClientRole::Paired ) {
            // 透明中继要求对端仍存在且也处于 Paired，避免向已改角色连接发送。
            const auto partnerIterator =
                m_clients.find(client.partnerWebSocketId);
            if ( partnerIterator == m_clients.end() ||
                 partnerIterator->second.role != ClientRole::Paired ) {
                closeClient(websocketId);
                return;
            }
            const int result =
                // 文本使用负长度约定，二进制保留嵌入零字节和显式长度。
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
            // 目录控制协议只接受文本 JSON，二进制仅在配对完成后合法。
            rejectClient(websocketId, "binary_before_pairing");
            return;
        }

        const auto message = nlohmann::json::parse(payload, nullptr, false);
        // 禁用异常解析，畸形 JSON 以 discarded 值进入协议拒绝。
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
            // list_rooms 可把任何未配对控制连接转为长期目录订阅者。
            client.role = ClientRole::Directory;
            sendRoomList(websocketId);
        } else if ( type == "get_room_cover" &&
                    client.role == ClientRole::Directory ) {
            handleGetRoomCover(websocketId, message);
        } else if ( type == "update_room" &&
                    client.role == ClientRole::HostControl ) {
            handleUpdateRoom(websocketId, message);
        } else if ( type == "set_room_cover" &&
                    client.role == ClientRole::HostControl ) {
            handleSetRoomCover(websocketId, message);
        } else if ( type == "create_room" &&
                    client.role == ClientRole::Unknown ) {
            handleCreateRoom(websocketId, message);
        } else if ( type == "join_room" &&
                    client.role == ClientRole::Unknown ) {
            handleJoinRoom(websocketId, message);
        } else if ( type == "accept_join" &&
                    client.role == ClientRole::Unknown ) {
            handleAcceptJoin(websocketId, message);
        } else if ( type == "reject_join" &&
                    client.role == ClientRole::HostControl ) {
            handleRejectJoin(websocketId, message);
        } else if ( type == "ping" ) {
            // ping 在所有未配对角色可用，只刷新活动并回送协议版本。
            nlohmann::json pong;
            pong["type"]    = "pong";
            pong["version"] = DIRECTORY_PROTOCOL_VERSION;
            static_cast<void>(sendJson(websocketId, pong));
        } else {
            // 已知类型出现在错误角色，与未知类型统一视为状态机违规。
            rejectClient(websocketId, "invalid_state");
        }
    }

    /// @brief 接受房主控制连接上报的真实 P2P 在线人数。
    /// @details 只允许
    /// 1～capacity，变化时更新目录脏标志；相同值不触发无意义广播。
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
            // 仅在公开字段变化时合并到本轮末尾广播。
            roomIterator->second.participants =
                static_cast<std::size_t>(participants);
            m_directoryDirty = true;
        }
    }

    /// @brief 拒绝可选封面上传但保留已经发布的房间控制连接。
    /// @details 封面不是加入房间的必要条件，非法上传只重置临时组装状态并回送
    /// room_cover_rejected，不关闭合法房主控制通道，也不清除旧封面。
    void rejectRoomCoverUpload(int websocketId, Room& room,
                               std::string_view reason)
    {
        // 三个字段整体归零，使下一次 chunkIndex=0 可重新开始。
        room.pendingCoverImage.clear();
        room.nextCoverChunkIndex = 0;
        room.expectedCoverChunks = 0;
        XWARN("Collaboration room cover from client {} rejected: {}",
              websocketId,
              reason);
        const nlohmann::json response = {
            { "type", "room_cover_rejected" },
            { "version", DIRECTORY_PROTOCOL_VERSION },
            { "reason", reason },
        };
        static_cast<void>(sendJson(websocketId, response));
    }

    /// @brief 接收房主在房间注册完成后分块提交的可选封面。
    /// @details
    /// 第一块声明总数并重置组装缓冲；后续块必须保持同一总数、严格连续索引，
    /// 且累计长度不能超过总上限。完整后再执行 Base64 校验并原子替换公开封面。
    /// 中途失败只丢弃 pendingCoverImage，已发布 coverImage 保持不变。
    void handleSetRoomCover(int websocketId, const nlohmann::json& message)
    {
        const auto clientIterator = m_clients.find(websocketId);
        if ( clientIterator == m_clients.end() ) return;
        const auto roomIterator = m_rooms.find(clientIterator->second.roomId);
        if ( roomIterator == m_rooms.end() ) return;

        Room&         room = roomIterator->second;
        std::string   coverChunk;
        std::uint64_t chunkIndex = 0;
        std::uint64_t chunkCount = 0;
        if ( !readStringField(message, "coverChunk", coverChunk) ||
             !readUnsignedField(message, "chunkIndex", chunkIndex) ||
             !readUnsignedField(message, "chunkCount", chunkCount) ||
             coverChunk.empty() ||
             coverChunk.size() > MAX_ROOM_COVER_CHUNK_BYTES ||
             chunkCount == 0 || chunkCount > MAX_ROOM_COVER_CHUNKS ||
             chunkIndex >= chunkCount ) {
            rejectRoomCoverUpload(
                websocketId, room, "invalid_room_cover_chunk");
            return;
        }

        if ( chunkIndex == 0 ) {
            // 新的零号块显式开始一次上传，可覆盖此前未完成序列。
            room.pendingCoverImage.clear();
            room.pendingCoverImage.reserve(
                // reserve 上限受总封面限制，恶意 chunkCount 不能造成超大分配。
                std::min(MAX_ROOM_COVER_BASE64_BYTES,
                         static_cast<std::size_t>(chunkCount) *
                             MAX_ROOM_COVER_CHUNK_BYTES));
            room.nextCoverChunkIndex = 0;
            room.expectedCoverChunks = static_cast<std::size_t>(chunkCount);
        }
        if ( room.expectedCoverChunks != chunkCount ||
             room.nextCoverChunkIndex != chunkIndex ||
             room.pendingCoverImage.size() + coverChunk.size() >
                 MAX_ROOM_COVER_BASE64_BYTES ) {
            rejectRoomCoverUpload(
                websocketId, room, "invalid_room_cover_sequence");
            return;
        }

        room.pendingCoverImage.append(coverChunk);
        // 只有全部分块到齐才验证整体 Base64 填充规则。
        ++room.nextCoverChunkIndex;
        if ( room.nextCoverChunkIndex != room.expectedCoverChunks ) return;
        if ( !isValidRoomCoverImage(room.pendingCoverImage) ) {
            rejectRoomCoverUpload(
                websocketId, room, "invalid_room_cover_image");
            return;
        }

        room.coverImage = std::move(room.pendingCoverImage);
        // 发布使用 move，避免复制接近 96 KiB 的封面正文。
        room.nextCoverChunkIndex = 0;
        room.expectedCoverChunks = 0;
        m_directoryDirty         = true;
    }

    /// @brief 注册一个公开房间和房主控制连接。
    /// @details
    /// 仅 Unknown 角色可进入此处理器。服务先检查全局房间容量，再读取并验证展示
    /// 字段、可选封面、私有 ownerToken 和 2～8 人容量。成功后生成唯一 roomId，
    /// 原子建立 Room 与 HostControl 关联，回送 ICE 配置并标记目录待广播。
    void handleCreateRoom(int websocketId, const nlohmann::json& message)
    {
        // 全局房间上限先于字段解析，过载时快速拒绝新创建请求。
        if ( m_rooms.size() >= m_config.maxRooms ) {
            rejectClient(websocketId, "room_limit_reached");
            return;
        }

        std::string   roomName;
        std::string   creator;
        std::string   ownerToken;
        std::string   coverImage;
        std::uint64_t capacity      = 0;
        const auto    coverIterator = message.find("coverImage");
        // 旧客户端可以省略封面；字段出现时必须明确为字符串。
        if ( coverIterator != message.end() ) {
            if ( !coverIterator->is_string() ) {
                rejectClient(websocketId, "invalid_room");
                return;
            }
            coverImage = coverIterator->get_ref<const std::string&>();
        }
        if ( !readStringField(message, "roomName", roomName) ||
             !readStringField(message, "creator", creator) ||
             !readStringField(message, "ownerToken", ownerToken) ||
             !readUnsignedField(message, "capacity", capacity) ||
             !isValidDisplayText(roomName, 128) ||
             !isValidDisplayText(creator, 64) ||
             !isValidRoomCoverImage(coverImage) ||
             !isValidOwnerToken(ownerToken) || capacity < 2 || capacity > 8 ) {
            rejectClient(websocketId, "invalid_room");
            return;
        }

        std::string roomId = generateRoomId();
        // 随机空间连续碰撞超过尝试上限时保持服务可响应并明确失败。
        if ( roomId.empty() ) {
            rejectClient(websocketId, "room_id_exhausted");
            return;
        }

        Room room;
        // ownerToken 只保存在服务端 Room，从不进入 created 或 room_list。
        room.roomId             = roomId;
        room.roomName           = std::move(roomName);
        room.hostCreator        = std::move(creator);
        room.coverImage         = std::move(coverImage);
        room.ownerToken         = std::move(ownerToken);
        room.controlWebSocketId = websocketId;
        room.capacity           = static_cast<std::size_t>(capacity);
        room.creationSequence   = ++m_roomSequence;
        // 单调序号用于目录稳定排序，不暴露为公共房间身份。
        m_rooms.emplace(roomId, std::move(room));

        Client& client = m_clients.at(websocketId);
        // Room 建立后同一连接转换为唯一房主控制通道。
        client.role   = ClientRole::HostControl;
        client.roomId = roomId;

        nlohmann::json created;
        created["type"]    = "room_created";
        created["version"] = DIRECTORY_PROTOCOL_VERSION;
        created["roomId"]  = roomId;
        appendIceServers(created);
        // 若 created 无法送达，关闭控制连接，关闭清理会移除刚建立房间。
        if ( !sendJson(websocketId, created) ) {
            closeClient(websocketId);
            return;
        }
        m_directoryDirty = true;
        XINFO("Collaboration room {} created by {}",
              roomId,
              m_rooms.at(roomId).hostCreator);
    }

    /// @brief 向目录订阅者按需返回一间房的封面缩略图。
    /// @details 房间列表只公开 hasCoverImage，实际 Base64 由目录客户端按 roomId
    /// 单独请求，避免每次列表广播携带所有大封面。房间不存在时返回空字符串，
    /// 保持响应结构稳定且不把查询失效视为连接错误。
    void handleGetRoomCover(int websocketId, const nlohmann::json& message)
    {
        std::string roomId;
        if ( !readStringField(message, "roomId", roomId) || roomId.empty() ||
             roomId.size() > 32U ) {
            rejectClient(websocketId, "invalid_room_cover_request");
            return;
        }

        nlohmann::json response;
        // 无论房间是否仍存在都回显 roomId，客户端可关联并丢弃过期响应。
        response["type"]    = "room_cover";
        response["version"] = DIRECTORY_PROTOCOL_VERSION;
        response["roomId"]  = roomId;
        if ( const auto iterator = m_rooms.find(roomId);
             iterator != m_rooms.end() ) {
            response["coverImage"] = iterator->second.coverImage;
        } else {
            response["coverImage"] = "";
        }
        static_cast<void>(sendJson(websocketId, response));
    }

    /// @brief 记录访客加入请求并通知房主建立专用信令通道。
    /// @details
    /// 验证房间、Creator 和可选构建指纹后，将已在线人数与同房间 PendingJoin
    /// 一起计入容量，避免大量等待请求超卖房间。成功时生成 requestId，先把访客
    /// 转为 PendingGuest 并回送 join_pending，再通知房主控制连接。
    /// 若房主通知失败，撤销 PendingJoin 并以 host_unavailable 关闭访客。
    void handleJoinRoom(int websocketId, const nlohmann::json& message)
    {
        std::string roomId;
        std::string creator;
        std::string buildFingerprint;
        if ( !readStringField(message, "roomId", roomId) ||
             !readStringField(message, "creator", creator) ||
             !isValidDisplayText(creator, 64) ) {
            rejectClient(websocketId, "invalid_join");
            return;
        }
        const auto fingerprintIterator = message.find("buildFingerprint");
        // 字段可省略以兼容旧客户端；出现时必须是规范化 SHA-256。
        if ( fingerprintIterator != message.end() ) {
            if ( !fingerprintIterator->is_string() ) {
                rejectClient(websocketId, "invalid_join");
                return;
            }
            buildFingerprint =
                fingerprintIterator->get_ref<const std::string&>();
            if ( !isValidBuildFingerprint(buildFingerprint) ) {
                rejectClient(websocketId, "invalid_join");
                return;
            }
        }
        const auto roomIterator = m_rooms.find(roomId);
        if ( roomIterator == m_rooms.end() ) {
            rejectClient(websocketId, "room_not_found");
            return;
        }
        const std::size_t pendingCount = static_cast<std::size_t>(
            // 等待房主决策的请求也预占容量，避免并发访客全部获得 pending。
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
        // 请求创建时间用于两分钟超时，访客连接持有相同 requestId 便于关闭清理。
        pending.requestId        = requestId;
        pending.roomId           = roomId;
        pending.guestWebSocketId = websocketId;
        pending.creator          = creator;
        pending.buildFingerprint = buildFingerprint;
        m_pendingJoins.emplace(requestId, std::move(pending));

        Client& client   = m_clients.at(websocketId);
        client.role      = ClientRole::PendingGuest;
        client.roomId    = roomId;
        client.requestId = requestId;
        client.creator   = creator;

        nlohmann::json waiting;
        // 先确认访客进入等待态，再向房主发送可接受请求。
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
        if ( !buildFingerprint.empty() ) {
            // 只在访客确实提交时转发，保留旧房主对缺失字段的兼容。
            requested["guestBuildFingerprint"] = buildFingerprint;
        }
        if ( !sendJson(roomIterator->second.controlWebSocketId, requested) ) {
            // 房主控制通道不可写时，请求不能继续悬挂到超时。
            m_pendingJoins.erase(requestId);
            rejectClient(websocketId, "host_unavailable");
        }
    }

    /// @brief 校验房主令牌并把访客与独立房主信令通道配对。
    /// @details
    /// 房主为每个接受动作创建新的 Unknown WebSocket，并携带 roomId、requestId
    /// 和 ownerToken。服务同时验证请求归属、私有令牌及访客仍处于 PendingGuest，
    /// 再把两端互设 partner 并发送相同 relay_ready；之后消息进入透明转发。
    /// @par 配对不变量
    /// - 接受连接不能复用长期 HostControl 通道。
    /// - requestId 必须仍指向目标房间的等待访客。
    /// - ownerToken 必须与目标房间控制凭据一致。
    /// - 两端 partnerWebSocketId 必须互相指向。
    /// - PendingJoin 必须在 relay_ready 前从表中移除。
    /// - 任一 ready 发送失败都必须关闭配对双方。
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
        const auto roomIterator = m_rooms.find(roomId);
        // 请求必须属于指定房间，令牌必须与房间创建时保存值完全一致。
        const auto pendingIterator = m_pendingJoins.find(requestId);
        if ( roomIterator == m_rooms.end() ||
             pendingIterator == m_pendingJoins.end() ||
             pendingIterator->second.roomId != roomId ||
             roomIterator->second.ownerToken != ownerToken ) {
            rejectClient(websocketId, "accept_not_authorized");
            return;
        }

        const int guestWebSocketId = pendingIterator->second.guestWebSocketId;
        // 访客可能已在房主响应前断开，失效请求需先从表中删除。
        const auto guestIterator = m_clients.find(guestWebSocketId);
        if ( guestIterator == m_clients.end() ||
             guestIterator->second.role != ClientRole::PendingGuest ) {
            m_pendingJoins.erase(pendingIterator);
            rejectClient(websocketId, "guest_unavailable");
            return;
        }

        Client& hostPeer = m_clients.at(websocketId);
        // 两端角色和 partner 在发送 ready 前一起建立，避免单向配对中间态。
        hostPeer.role                            = ClientRole::Paired;
        hostPeer.roomId                          = roomId;
        hostPeer.partnerWebSocketId              = guestWebSocketId;
        guestIterator->second.role               = ClientRole::Paired;
        guestIterator->second.partnerWebSocketId = websocketId;
        m_pendingJoins.erase(pendingIterator);
        // 一旦配对，请求 ID 不再承担生命周期，后续关闭按 partner 清理。

        nlohmann::json ready;
        ready["type"]      = "relay_ready";
        ready["version"]   = DIRECTORY_PROTOCOL_VERSION;
        ready["roomId"]    = roomId;
        ready["requestId"] = requestId;
        appendIceServers(ready);
        if ( !sendJson(websocketId, ready) ||
             !sendJson(guestWebSocketId, ready) ) {
            closeClient(websocketId);
            // 任一端未收到 ready 都关闭双方，禁止只留半条透明中继。
            closeClient(guestWebSocketId);
            return;
        }
        m_directoryDirty = true;
    }

    /// @brief 校验房主控制令牌并拒绝仍在等待的访客。
    /// @details
    /// 拒绝只能从原 HostControl 连接发起，且 roomId、requestId、ownerToken 必须
    /// 同时匹配。可选 reason 使用受限稳定码；成功后删除请求，向访客发送 error
    /// 并关闭其连接，房主控制通道继续管理房间。
    /// @par 拒绝不变量
    /// - 默认 reason 为 host_rejected。
    /// - 自定义 reason 必须通过稳定码字符集校验。
    /// - 非控制连接即使持有令牌也不能拒绝请求。
    /// - 请求删除后访客关闭不得再次产生 join_cancelled。
    /// - 拒绝访客不会改变房间已上报 participants。
    void handleRejectJoin(int websocketId, const nlohmann::json& message)
    {
        std::string roomId;
        std::string requestId;
        std::string ownerToken;
        std::string reason = "host_rejected";
        // 省略原因时使用稳定默认值，避免向访客暴露空原因。
        if ( !readStringField(message, "roomId", roomId) ||
             !readStringField(message, "requestId", requestId) ||
             !readStringField(message, "ownerToken", ownerToken) ) {
            rejectClient(websocketId, "invalid_reject");
            return;
        }
        if ( const auto reasonIterator = message.find("reason");
             reasonIterator != message.end() ) {
            if ( !reasonIterator->is_string() ) {
                rejectClient(websocketId, "invalid_reject");
                return;
            }
            reason = reasonIterator->get_ref<const std::string&>();
        }
        if ( !isValidReasonCode(reason) ) {
            rejectClient(websocketId, "invalid_reject");
            return;
        }

        const auto roomIterator    = m_rooms.find(roomId);
        const auto pendingIterator = m_pendingJoins.find(requestId);
        if ( roomIterator == m_rooms.end() ||
             pendingIterator == m_pendingJoins.end() ||
             pendingIterator->second.roomId != roomId ||
             roomIterator->second.controlWebSocketId != websocketId ||
             // 令牌和控制连接双重绑定，阻止其他连接复用泄露的 requestId。
             roomIterator->second.ownerToken != ownerToken ) {
            rejectClient(websocketId, "reject_not_authorized");
            return;
        }

        const int guestWebSocketId = pendingIterator->second.guestWebSocketId;
        m_pendingJoins.erase(pendingIterator);
        // 先移除权威请求，再向访客发送结果，关闭回调不会重复通知房主取消。
        const auto guestIterator = m_clients.find(guestWebSocketId);
        if ( guestIterator == m_clients.end() ) return;
        guestIterator->second.requestId.clear();

        nlohmann::json rejected;
        rejected["type"]    = "error";
        rejected["version"] = DIRECTORY_PROTOCOL_VERSION;
        rejected["reason"]  = reason;
        static_cast<void>(sendJson(guestWebSocketId, rejected));
        closeClient(guestWebSocketId);
    }

    /// @brief 通知房主一个等待请求已经由访客断开或超时取消。
    /// @details 通知为尽力发送；房间已删除或控制连接不可写时无需保留请求。
    void notifyJoinCancelled(const PendingJoin& pending)
    {
        const auto roomIterator = m_rooms.find(pending.roomId);
        if ( roomIterator == m_rooms.end() ) return;
        nlohmann::json cancelled;
        cancelled["type"]         = "join_cancelled";
        cancelled["version"]      = DIRECTORY_PROTOCOL_VERSION;
        cancelled["requestId"]    = pending.requestId;
        cancelled["guestCreator"] = pending.creator;
        static_cast<void>(
            sendJson(roomIterator->second.controlWebSocketId, cancelled));
    }

    /// @brief 处理 WebSocket 关闭并同步清理房间或配对端。
    /// @details
    /// generation 先过滤迟到关闭。PendingGuest 取消请求并通知房主；HostControl
    /// 关闭会删除整个房间及相关连接；Paired 任一端关闭会请求关闭另一端。
    /// 当前客户端最后从活动表移除并进入延迟退役队列。
    void processClosed(int websocketId, std::uint64_t generation)
    {
        const auto iterator = m_clients.find(websocketId);
        if ( iterator == m_clients.end() ||
             iterator->second.generation != generation ) {
            return;
        }
        const Client client = iterator->second;
        // 复制状态后再修改容器，避免 erase 导致引用失效。

        if ( !client.requestId.empty() ) {
            // 只有仍存在的请求需要通知，已接受或拒绝请求已经从表中删除。
            const auto pendingIterator = m_pendingJoins.find(client.requestId);
            if ( pendingIterator != m_pendingJoins.end() ) {
                notifyJoinCancelled(pendingIterator->second);
                m_pendingJoins.erase(pendingIterator);
            }
        }

        if ( client.role == ClientRole::HostControl ) {
            removeRoom(client.roomId, websocketId);
        } else if ( client.role == ClientRole::Paired ) {
            // 先把对端改为 Closing 并清除 partner，防止两次关闭互相递归。
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
        // 状态删除后保留第三方句柄宽限期，迟到事件无法再命中客户端。
        retireWebSocket(websocketId, generation);
    }

    /// @brief 删除房主控制连接对应的全部房间状态。
    /// @details 收集同房间活动连接后统一请求关闭，并删除全部 PendingJoin 与
    /// Room。 收集阶段只修改角色、不在遍历中擦除 m_clients，避免迭代器失效。
    void removeRoom(const std::string& roomId, int controlWebSocketId)
    {
        const auto roomIterator = m_rooms.find(roomId);
        if ( roomIterator == m_rooms.end() ) return;

        std::vector<int> clientsToClose;
        // 覆盖待加入访客和已配对专用通道，但跳过正在关闭的连接。
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
        // Room 先从目录移除，再异步关闭成员连接并合并一次目录广播。
        for ( int clientId : clientsToClose ) {
            static_cast<void>(rtcClose(clientId));
        }
        m_directoryDirty = true;
        XINFO("Collaboration room {} removed", roomId);
    }

    /// @brief 构造当前公开房间目录消息。
    /// @details
    /// unordered_map 不提供稳定顺序，因此先收集 Room 指针并按 creationSequence
    /// 排序。每项只公开房间 ID、名称、房主展示名、封面存在标记、人数和容量；
    /// ownerToken、封面正文、控制连接句柄及上传中间态都不得进入目录。
    /// @return 可直接发送给目录订阅者的版本化 room_list JSON。
    nlohmann::json makeRoomList() const
    {
        // 指针只在本函数内使用，排序期间主线程不会修改 m_rooms。
        std::vector<const Room*> orderedRooms;
        orderedRooms.reserve(m_rooms.size());
        for ( const auto& [roomId, room] : m_rooms ) {
            static_cast<void>(roomId);
            orderedRooms.push_back(&room);
        }
        std::sort(orderedRooms.begin(),
                  orderedRooms.end(),
                  [](const Room* left, const Room* right) {
                      // 创建序号单调唯一，为所有订阅者提供一致列表顺序。
                      return left->creationSequence < right->creationSequence;
                  });

        nlohmann::json rooms = nlohmann::json::array();
        // 封面正文按需获取，列表只携带轻量布尔提示。
        for ( const Room* room : orderedRooms ) {
            nlohmann::json item;
            item["roomId"]        = room->roomId;
            item["roomName"]      = room->roomName;
            item["hostCreator"]   = room->hostCreator;
            item["hasCoverImage"] = !room->coverImage.empty();
            item["participants"]  = room->participants;
            item["capacity"]      = room->capacity;
            rooms.push_back(std::move(item));
        }

        nlohmann::json message;
        // 顶层版本允许客户端在解析列表前验证目录协议兼容性。
        message["type"]    = "room_list";
        message["version"] = DIRECTORY_PROTOCOL_VERSION;
        message["rooms"]   = std::move(rooms);
        return message;
    }

    /// @brief 向单个目录客户端发送房间列表。
    /// @details 用于 list_rooms 首次响应；发送失败由 WebSocket
    /// 后续错误/关闭回调清理。
    void sendRoomList(int websocketId) const
    {
        static_cast<void>(sendJson(websocketId, makeRoomList()));
    }

    /// @brief 向所有目录订阅者广播最新房间列表。
    /// @details 同一更新周期只构造一次 JSON，并仅发送给 Directory 角色；完成后
    /// 清除 dirty 标志，使多次房间变化合并为一次广播。
    void broadcastRoomList()
    {
        // 构造发生在遍历前，所有订阅者收到同一时刻的目录快照。
        const auto message = makeRoomList();
        for ( const auto& [websocketId, client] : m_clients ) {
            if ( client.role == ClientRole::Directory ) {
                static_cast<void>(sendJson(websocketId, message));
            }
        }
        m_directoryDirty = false;
        // sendJson 个别失败不阻止其他订阅者，连接错误由第三方回调处理。
    }

    /// @brief 清理未声明身份连接和过期加入请求。
    /// @details
    /// Unknown 连接在十五秒内必须声明用途，PendingJoin 在两分钟内必须得到房主
    /// 决策。扫描阶段只收集 websocketId，遍历结束后调用 closeClient，避免在
    /// 容器遍历过程中触发异步状态变化。
    /// @warning 每次 update
    /// 调用都会线性扫描客户端和等待请求；容量由配置严格限制。
    void expireIdleClientsAndJoins()
    {
        const auto now = std::chrono::steady_clock::now();
        // 同一连接理论上只属于一个集合，closeClient 幂等处理仍防御重复 ID。
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
    /// @details
    /// 回调入口先以 acquire 检查服务生命周期，再在递归互斥量下检查有界队列并
    /// 移动事件。队列满时直接丢弃，避免不可信客户端用消息洪泛扩大内存。
    /// @warning 可从 libdatachannel 任意回调线程调用；不得访问主线程状态容器。
    void enqueueCallbackEvent(CallbackEvent event)
    {
        if ( !m_acceptCallbacks.load(std::memory_order_acquire) ) return;
        // 加锁后 stop 无法同时清空上下文；只在有容量时接受事件所有权。
        std::scoped_lock lock(m_callbackMutex);
        if ( m_callbackEvents.size() >= MAX_CALLBACK_EVENTS ) return;
        m_callbackEvents.push_back(std::move(event));
    }

    /// @brief 接收新的 WebSocket 客户端。
    /// @details
    /// 创建稳定 CallbackContext 并分配单调 generation，把 user pointer 和三个
    /// 第三方回调全部安装后再排入 Connected。所有步骤在 callback mutex 内完成，
    /// 防止主循环在上下文尚未登记完整时消费连接事件。
    /// @par 上下文不变量
    /// - generation 在服务实例内单调增加且不复用。
    /// - CallbackContext 地址在第三方持有期间保持稳定。
    /// - WebSocket user pointer 只指向对应 generation 的上下文。
    /// - Connected 事件与上下文使用相同 websocketId 和 generation。
    /// - 队列无容量时不安装一套无法被主循环接管的连接状态。
    /// - stop 禁止回调后才清空全部上下文。
    /// - 常规关闭必须越过退役宽限期才释放上下文。
    /// @warning 由 libdatachannel 回调线程调用，只能修改互斥量保护的回调状态。
    static void onWebSocketClient(int, int websocketId, void* pointer)
    {
        auto* owner = static_cast<Impl*>(pointer);
        if ( !owner ||
             !owner->m_acceptCallbacks.load(std::memory_order_acquire) ) {
            rtcDeleteWebSocket(websocketId);
            // 服务已停止时连接从未安装回调，可直接删除而无需退役队列。
            return;
        }
        CallbackContext* contextPointer = nullptr;
        {
            std::scoped_lock lock(owner->m_callbackMutex);
            if ( owner->m_callbackEvents.size() >= MAX_CALLBACK_EVENTS ) {
                // 无法可靠登记 Connected 时拒绝连接，避免存在未跟踪客户端。
                rtcDeleteWebSocket(websocketId);
                return;
            }
            auto context = std::make_unique<CallbackContext>();
            // unique_ptr 存入 map 后地址稳定，直到 generation 对应退役项删除。
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
            // Connected 入队晚于全部回调安装，随后消息事件保持队列顺序。
            owner->m_callbackEvents.push_back({ CallbackEventType::Connected,
                                                websocketId,
                                                contextPointer->generation,
                                                {},
                                                true });
        }
    }

    /// @brief 接收 WebSocket 消息并复制到主循环队列。
    /// @details 负 size 表示零结尾文本，非负 size 表示可含零字节的二进制数据；
    /// 两者都复制进事件，第三方缓冲区返回后不再被引用。
    /// @warning 由 libdatachannel 回调线程调用，不解析 JSON 或访问 Client
    /// 状态。
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
        // 文本依赖第三方零结尾契约，二进制严格按显式范围复制。
        event.payload = size < 0 ? std::string(message)
                                 : std::string(message, message + size);
        context->owner->enqueueCallbackEvent(std::move(event));
    }

    /// @brief 把 WebSocket 关闭事件提交到主循环。
    /// @details generation 随事件复制，主循环可识别句柄复用后的迟到关闭。
    /// @warning 由 libdatachannel 回调线程调用。
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
    /// @details 错误文本立即复制；第三方未提供详情时使用稳定占位码。
    /// @warning 由 libdatachannel 回调线程调用。
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
    /// @note 启动后保持字符串存储稳定，供 libdatachannel 配置指针间接使用。
    CollaborationSignalingServerConfig m_config;
    /// @brief 服务是否正在运行。
    /// @note 由 start/stop 写入，公开查询可跨线程读取。
    std::atomic_bool m_running{ false };
    /// @brief 回调是否仍可访问当前实例。
    /// @note stop 在清理上下文前先清除此标志，阻止新回调事件入队。
    std::atomic_bool m_acceptCallbacks{ false };
    /// @brief WebSocketServer 句柄。
    int m_serverId = -1;
    /// @brief 实际监听端口。
    std::uint16_t m_listeningPort = 0;
    /// @brief 主循环维护的客户端表。
    /// @warning 只允许 start/stop/update 所在线程访问，不由回调线程直接修改。
    std::unordered_map<int, Client> m_clients;
    /// @brief 公共房间表。
    /// @warning 只在服务主线程访问，目录序列化期间不允许并发修改。
    std::unordered_map<std::string, Room> m_rooms;
    /// @brief 等待房主接受的请求表。
    /// @warning 只在主线程修改，连接回调必须经 CallbackEvent 间接触发清理。
    std::unordered_map<std::string, PendingJoin> m_pendingJoins;
    /// @brief 是否需要向目录订阅者广播。
    /// @note 同一 update 内的多次变更合并到末尾一次广播。
    bool m_directoryDirty = false;
    /// @brief 房间创建顺序计数器。
    std::uint64_t m_roomSequence = 0;
    /// @brief 生成房间与请求标识的随机引擎。
    std::mt19937_64 m_random{ std::random_device{}() };
    /// @brief 保护跨线程回调队列。
    /// @note 使用 recursive_mutex
    /// 兼容第三方设置/删除回调期间可能发生的同步重入。
    std::recursive_mutex m_callbackMutex;
    /// @brief 等待主循环处理的回调事件。
    /// @warning 数量严格限制为 MAX_CALLBACK_EVENTS，过载时拒绝新增事件。
    std::deque<CallbackEvent> m_callbackEvents;
    /// @brief 等待跨过回调迟到窗口后删除的 WebSocket。
    std::deque<RetiredWebSocket> m_retiredWebSockets;
    /// @brief 为每次连接保留到句柄删除的稳定回调上下文。
    /// @note key 是单调 generation，不受 WebSocket 整数句柄复用影响。
    std::unordered_map<std::uint64_t, std::unique_ptr<CallbackContext>>
        m_callbackContexts;
    /// @brief 分配连接代次的单调计数器。
    std::uint64_t m_connectionGeneration = 0;
};

/// @brief 构造拥有独立实现状态的信令服务外观。
/// @details PImpl 隔离 libdatachannel
/// 类型和大型状态容器，公开头只暴露稳定接口。
CollaborationSignalingServer::CollaborationSignalingServer()
    : m_impl(std::make_unique<Impl>())
{
}

/// @brief 销毁服务并通过 Impl 析构统一停止网络句柄。
CollaborationSignalingServer::~CollaborationSignalingServer() = default;

/// @brief 启动中心目录与 WebSocket 信令服务。
/// @param config 服务监听、TLS、容量和 ICE 配置。
/// @return 实现完成监听并发布有效端口时返回 true。
bool CollaborationSignalingServer::start(
    CollaborationSignalingServerConfig config)
{
    return m_impl->start(std::move(config));
}

/// @brief 停止信令服务并释放全部房间和连接状态。
/// @warning 该调用同步删除第三方句柄，只应在低频服务关闭路径执行。
void CollaborationSignalingServer::stop()
{
    m_impl->stop();
}

/// @brief 推进回调事件、房间状态、超时清理和目录广播。
/// @warning 服务循环高频调用；调用方不得并发调用另一 update。
void CollaborationSignalingServer::update()
{
    m_impl->update();
}

/// @brief 查询服务是否已成功启动且尚未停止。
/// @return 运行中返回 true。
bool CollaborationSignalingServer::isRunning() const
{
    return m_impl->isRunning();
}

/// @brief 获取服务实际监听端口。
/// @return 启动成功后的有效端口；停止状态返回零。
std::uint16_t CollaborationSignalingServer::listeningPort() const
{
    return m_impl->listeningPort();
}

/// @brief 获取当前公开房间数量。
/// @return 主线程状态表中的房间数。
std::size_t CollaborationSignalingServer::roomCount() const
{
    return m_impl->roomCount();
}

/// @brief 获取当前活动 WebSocket 客户端数量。
/// @details 包含目录、房主控制、等待访客、配对和 Closing 状态，不含退役句柄。
/// @return 活动客户端表大小。
std::size_t CollaborationSignalingServer::clientCount() const
{
    return m_impl->clientCount();
}
}  // namespace MMM::Network::CollaborationServer
