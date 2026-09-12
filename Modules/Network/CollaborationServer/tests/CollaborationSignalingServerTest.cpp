/**
 * @file CollaborationSignalingServerTest.cpp
 * @brief 验证本机信令服务的目录、入房审批、双向中继和资源清理。
 * @details 测试使用真实 libdatachannel WebSocket，但流量仅经过动态回环端口。
 * 所有等待均有固定上限，失败时通过阶段名与服务端计数定位状态机位置。
 */

#include "network/collaboration_server/CollaborationSignalingServer.h"

#include "log/colorful-log.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
// 服务实现和配置类型只在本翻译单元内使用，避免测试辅助符号外泄。
using MMM::Network::CollaborationServer::CollaborationSignalingServer;
using MMM::Network::CollaborationServer::CollaborationSignalingServerConfig;

/// @brief 本机目录与信令集成测试允许的最长等待时间。
/// @note 每个协议阶段各自使用该窗口，慢速 CI 不会继承上阶段耗时。
constexpr auto TEST_TIMEOUT = std::chrono::seconds(10);
/// @brief 测试访客提交的固定 SHA-256 构建指纹。
/// @note 固定小写 64 位文本满足生产协议格式，不依赖当前二进制文件。
constexpr std::string_view TEST_BUILD_FINGERPRINT =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/// @brief 测试 WebSocket 的跨回调线程收件箱。
/// @note 对象地址注册为第三方 user pointer，因此整个连接期间不可移动。
struct TestSocket {
    /// @brief libdatachannel WebSocket 句柄。
    /// @note 非负值由本对象唯一持有，并在析构时释放。
    int id = -1;
    /// @brief WebSocket 是否已经打开。
    /// @warning 回调线程写入、测试主线程读取，使用 release/acquire 同步。
    std::atomic_bool opened{ false };
    /// @brief WebSocket 是否已经关闭。
    /// @warning 回调线程写入、测试主线程读取，使用 release/acquire 同步。
    std::atomic_bool closed{ false };
    /// @brief 保护文本消息队列。
    /// @note onMessage 写入与 takeMessage 遍历、删除必须持有同一把锁。
    std::mutex mutex;
    /// @brief 回调线程收到的文本消息。
    /// @note 保持到达顺序，未匹配类型会留给后续协议阶段消费。
    /// @warning 只能在持有 mutex 时由非回调路径遍历或删除。
    std::deque<std::string> messages;

    /// @brief 释放 WebSocket 句柄。
    /// @warning 析构前测试必须停止使用回调中的 user pointer。
    ~TestSocket()
    {
        // 创建失败时 id 保持负值，不向第三方库传入无效句柄。
        if ( id >= 0 ) rtcDeleteWebSocket(id);
    }

    /// @brief 创建尚未绑定 WebSocket 句柄的测试收件箱。
    TestSocket() = default;
    /// @brief 禁止复制，避免一个第三方句柄被多个对象释放。
    TestSocket(const TestSocket&) = delete;
    /// @brief 禁止复制赋值，保持句柄和回调 user pointer 地址唯一。
    TestSocket& operator=(const TestSocket&) = delete;
    /// @brief 禁止移动，确保注册给回调的对象地址在生命周期内稳定。
    TestSocket(TestSocket&&) = delete;
    /// @brief 禁止移动赋值，避免收件箱地址与已注册指针分离。
    TestSocket& operator=(TestSocket&&) = delete;
};

/// @brief 标记测试 WebSocket 已打开。
/// @param socketId libdatachannel 句柄；状态已由 user pointer 绑定，无需读取。
/// @param pointer connectSocket 注册的 TestSocket 地址。
/// @warning 由 libdatachannel 回调线程调用，只执行原子状态发布。
void onOpen(int, void* pointer)
{
    // 回调注册与关闭竞态下允许空 pointer，防御后直接忽略。
    auto* socket = static_cast<TestSocket*>(pointer);
    // release 使主线程 acquire 观察到连接建立前的初始化状态。
    if ( socket ) socket->opened.store(true, std::memory_order_release);
}

/// @brief 把测试 WebSocket 文本消息压入收件箱。
/// @param socketId 产生消息的句柄；每个回调已绑定独立收件箱。
/// @param message 回调期间有效的消息缓冲区。
/// @param size libdatachannel 长度标识；负值表示零结尾文本帧。
/// @param pointer 目标 TestSocket 地址。
/// @warning 回调线程只复制消息并短时持锁，禁止解析 JSON 或等待主线程。
void onMessage(int, const char* message, int size, void* pointer)
{
    auto* socket = static_cast<TestSocket*>(pointer);
    // 测试协议只处理文本帧；二进制帧或无效回调参数直接忽略。
    if ( !socket || !message || size >= 0 ) return;
    // 锁保护跨线程 deque，复制后不再依赖第三方消息缓冲生命周期。
    std::scoped_lock lock(socket->mutex);
    socket->messages.emplace_back(message);
}

/// @brief 标记测试 WebSocket 已关闭。
/// @param socketId 已关闭的句柄；测试只关心绑定对象的状态。
/// @param pointer connectSocket 注册的 TestSocket 地址。
/// @warning 由第三方回调线程调用，不释放句柄或访问消息队列。
void onClosed(int, void* pointer)
{
    auto* socket = static_cast<TestSocket*>(pointer);
    // release 允许测试线程以 acquire 方式确认关闭回调已经发生。
    if ( socket ) socket->closed.store(true, std::memory_order_release);
}

/// @brief 测试不单独消费错误文本，错误最终表现为关闭或超时。
/// @param socketId 报错的第三方句柄。
/// @param message 第三方错误文本；当前测试不把它写入共享状态。
/// @param pointer 与连接绑定的 TestSocket 地址。
void onError(int, const char*, void* pointer)
{
    // 错误统一转为 closed 状态，协议阶段由超时或连接计数给出诊断。
    onClosed(-1, pointer);
}

/// @brief 创建一个连接本机目录服务的 WebSocket。
/// @param port 信令服务动态分配的回环监听端口。
/// @return 创建成功时返回地址稳定的测试套接字，否则返回空。
std::unique_ptr<TestSocket> connectSocket(std::uint16_t port)
{
    // unique_ptr 保证回调 user pointer 在函数返回后仍保持稳定地址。
    auto               socket = std::make_unique<TestSocket>();
    rtcWsConfiguration config{};
    // 第三方握手自身限制为三秒，外层阶段仍有更宽的测试超时。
    config.connectionTimeoutMs = 3000;
    // 256 KiB 足以容纳协议 JSON 和本测试的小型封面分块。
    config.maxMessageSize = 256 * 1024;
    // 固定路径与产品客户端一致，仅端口由测试服务动态提供。
    const std::string url =
        "ws://127.0.0.1:" + std::to_string(port) + "/mmm-collaboration";
    socket->id = rtcCreateWebSocketEx(url.c_str(), &config);
    // 负句柄表示第三方连接对象未创建，局部对象析构不会调用删除。
    if ( socket->id < 0 ) return {};
    // 先绑定稳定对象地址，再注册所有状态和消息回调。
    rtcSetUserPointer(socket->id, socket.get());
    rtcSetOpenCallback(socket->id, &onOpen);
    rtcSetMessageCallback(socket->id, &onMessage);
    rtcSetClosedCallback(socket->id, &onClosed);
    rtcSetErrorCallback(socket->id, &onError);
    // 连接可能在回调注册前快速完成，主动查询可补齐打开状态。
    if ( rtcIsOpen(socket->id) ) {
        socket->opened.store(true, std::memory_order_release);
    }
    // 返回后调用方负责保持对象活到 WebSocket 删除完成。
    return socket;
}

/// @brief 驱动服务直到条件满足或超时。
/// @tparam Predicate 每轮服务更新后执行的非阻塞完成条件。
/// @param server 当前进程内运行的信令服务。
/// @param predicate 判断协议阶段是否完成的可调用对象。
/// @return 截止时间前条件成立时返回 true，否则返回 false。
/// @warning 固定 sleep 仅用于集成测试轮询，禁止复制到产品热路径。
template<typename Predicate>
bool pumpUntil(CollaborationSignalingServer& server, Predicate predicate)
{
    // steady_clock 避免系统时间调整改变超时窗口。
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        // 服务端在测试主线程推进，WebSocket 回调可能由第三方线程触发。
        server.update();
        // 更新完成后再观察收件箱和计数，避免检查半处理事件。
        if ( predicate() ) return true;
        // 短暂休眠限制空轮询 CPU，同时给回调线程运行机会。
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // 调用方使用阶段标签补充超时上下文。
    return false;
}

/// @brief 向测试 WebSocket 发送 JSON 文本消息。
/// @param socket 已打开的目标测试连接。
/// @param message 待序列化的协议信令对象。
/// @return 第三方库接受完整文本帧时返回 true。
bool sendJson(const TestSocket& socket, const nlohmann::json& message)
{
    // 测试只在 opened 后调用该帮助器，连接状态由各阶段前置条件保证。
    // dump 生成紧凑文本，服务端协议不依赖空白或键顺序。
    const std::string payload = message.dump();
    // 负长度要求 libdatachannel 按零结尾文本发送，而非二进制缓冲。
    return rtcSendMessage(socket.id, payload.c_str(), -1) == RTC_ERR_SUCCESS;
}

/// @brief 从收件箱取出指定类型的 JSON 消息。
/// @param socket 待读取的跨线程收件箱。
/// @param type 目标协议消息的 type 字段值。
/// @param output 成功时接收完整 JSON 对象。
/// @return 找到并移除首个匹配对象时返回 true。
/// @note 非匹配与非法 JSON 保留在队列中，供后续阶段诊断或消费。
bool takeMessage(TestSocket& socket, std::string_view type,
                 nlohmann::json& output)
{
    // 解析和删除期间保持锁，避免回调追加导致迭代器失效。
    std::scoped_lock lock(socket.mutex);
    // 按到达顺序寻找目标类型，不要求不同消息类型严格排序。
    for ( auto iterator = socket.messages.begin();
          iterator != socket.messages.end();
          ++iterator ) {
        // 禁用异常解析；部分或非法文本只作为非匹配项处理。
        auto message = nlohmann::json::parse(*iterator, nullptr, false);
        // 只接受对象且 type 精确匹配，防止数组或标量误判。
        if ( message.is_object() && message.value("type", "") == type ) {
            // 先移动完整对象到输出，再移除已消费的队列元素。
            output = std::move(message);
            socket.messages.erase(iterator);
            return true;
        }
    }
    // 没有匹配消息属于异步等待的正常状态，不记录错误。
    return false;
}

/// @brief 验证房间目录、加入配对和透明双向信令转发。
/// @return 所有协议阶段和资源计数断言通过时返回 true。
/// @note 测试覆盖一个目录连接、房主控制连接以及访客中继连接。
bool testDirectoryAndSignalingRelay()
{
    // 局部对象按服务、控制连接、中继连接的顺序建立并在返回时逆序销毁。
    // 动态端口避免并行测试争用，回环地址阻止服务暴露到局域网。
    CollaborationSignalingServerConfig config;
    config.port        = 0;
    config.bindAddress = "127.0.0.1";
    // 固定 ICE 列表用于验证 create_room 响应完整回传配置。
    config.iceServers = { "stun:stun.example.test:3478" };
    CollaborationSignalingServer server;
    // 启动成功后实际监听端口必须非零，客户端才能构造 URL。
    if ( !server.start(std::move(config)) || server.listeningPort() == 0 ) {
        XERROR("Signaling server test failed at server_start");
        return false;
    }
    // 失败帮助器统一记录异步阶段名和服务端当前对象计数。
    const auto fail = [&server](std::string_view stage) {
        // 不输出消息负载或 owner token，只保留安全的结构状态。
        XERROR("Signaling server test failed at {}: clients={}, rooms={}",
               stage,
               server.clientCount(),
               server.roomCount());
        return false;
    };
    // 给 libdatachannel 监听线程完成初始化，避免首个握手受启动抖动影响。
    // 该等待只存在于测试进程，不属于服务端产品更新循环。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 目录与房主控制面使用两个独立 WebSocket，验证角色不会共享状态。
    auto directory = connectSocket(server.listeningPort());
    auto host      = connectSocket(server.listeningPort());
    // 创建失败或任一连接未在阶段窗口打开都终止后续协议检查。
    if ( !directory || !host || !pumpUntil(server, [&]() {
             // acquire 与回调 release 配对，读取稳定的 opened 标志。
             return directory->opened.load(std::memory_order_acquire) &&
                    host->opened.load(std::memory_order_acquire);
         }) ) {
        return fail("directory_host_open");
    }

    // 目录先订阅房间列表，房主随后用版本一协议创建公开房间。
    if ( !sendJson(*directory,
                   { { "type", "list_rooms" }, { "version", 1 } }) ||
         !sendJson(*host,
                   { { "type", "create_room" },
                     { "version", 1 },
                     { "roomName", "Public Test Room" },
                     { "creator", "Host Creator" },
                     // ownerToken 只用于后续房主审批授权，不应出现在目录响应。
                     { "ownerToken", "0123456789abcdef0123456789abcdef" },
                     { "capacity", 8 } }) ) {
        return fail("room_requests_send");
    }

    // 两类响应可能以任意先后到达，分别记录后等待二者齐备。
    nlohmann::json created;
    nlohmann::json roomList;
    bool           receivedCreated  = false;
    bool           receivedRoomList = false;
    if ( !pumpUntil(server,
                    [&]() {
                        // room_created 只由房主连接接收，包含服务端分配的 ID。
                        receivedCreated =
                            receivedCreated ||
                            takeMessage(*host, "room_created", created);
                        if ( takeMessage(*directory, "room_list", roomList) &&
                             roomList.contains("rooms") &&
                             roomList["rooms"].is_array() &&
                             roomList["rooms"].size() == 1U ) {
                            // 目录可能先收到初始空列表，仅接受包含唯一房间的快照。
                            receivedRoomList = true;
                        }
                        return receivedCreated && receivedRoomList;
                    }) ||
         !created.contains("roomId") || !created["roomId"].is_string() ||
         created.value("iceServers", std::vector<std::string>{}) !=
             std::vector<std::string>{ "stun:stun.example.test:3478" } ) {
        // 同时验证房间 ID 类型和服务端 ICE 配置回传，避免半成功状态。
        return fail("room_created_and_listed");
    }
    // 经过类型检查后提取稳定 roomId，后续封面和加入请求均使用它。
    const std::string roomId = created["roomId"].get<std::string>();
    // 初始目录不得宣称存在封面；随后分两块上传 Base64 文本 Hello。
    if ( roomList["rooms"][0].value("hasCoverImage", false) ||
         !sendJson(*host,
                   { { "type", "set_room_cover" },
                     { "version", 1 },
                     { "chunkIndex", 0 },
                     { "chunkCount", 2 },
                     // 第一块 "SGVs" 与第二块拼接为完整 "SGVsbG8="。
                     { "coverChunk", "SGVs" } }) ||
         !sendJson(*host,
                   { { "type", "set_room_cover" },
                     { "version", 1 },
                     { "chunkIndex", 1 },
                     { "chunkCount", 2 },
                     // 最后一块到达后服务端才可发布 hasCoverImage。
                     { "coverChunk", "bG8=" } }) ) {
        return fail("room_cover_upload");
    }
    // 封面完成会触发新的目录快照，等待唯一房间的标志变为 true。
    bool receivedCoveredRoomList = false;
    if ( !pumpUntil(server,
                    [&]() {
                        if ( takeMessage(*directory, "room_list", roomList) &&
                             roomList.contains("rooms") &&
                             roomList["rooms"].is_array() &&
                             roomList["rooms"].size() == 1U &&
                             roomList["rooms"][0].value("hasCoverImage",
                                                        false) ) {
                            // 只接受结构完整且已标记封面的最新快照。
                            receivedCoveredRoomList = true;
                        }
                        return receivedCoveredRoomList;
                    }) ||
         !sendJson(*directory,
                   { { "type", "get_room_cover" },
                     { "version", 1 },
                     { "roomId", roomId } }) ) {
        // 目录更新或封面请求发送任一失败都归入请求阶段。
        return fail("room_cover_request");
    }
    // 封面响应应把两块内容按 chunkIndex 顺序拼回原始 Base64 文本。
    nlohmann::json roomCover;
    if ( !pumpUntil(server,
                    [&]() {
                        return takeMessage(*directory, "room_cover", roomCover);
                    }) ||
         roomCover.value("roomId", "") != roomId ||
         roomCover.value("coverImage", "") != "SGVsbG8=" ) {
        // 同时校验 roomId，防止从其他房间缓存误取封面内容。
        return fail("room_cover_response");
    }

    // 新访客使用独立控制连接向刚创建的房间提交加入申请。
    auto guest = connectSocket(server.listeningPort());
    if ( !guest ||
         !pumpUntil(
             server,
             [&]() { return guest->opened.load(std::memory_order_acquire); }) ||
         !sendJson(*guest,
                   { { "type", "join_room" },
                     { "version", 1 },
                     { "roomId", roomId },
                     // 首次申请故意不提供构建指纹，覆盖兼容旧客户端的路径。
                     { "creator", "Guest Creator" } }) ) {
        return fail("guest_open_and_join_send");
    }

    // 访客收到 pending、房主收到 requested；两者共享服务端 requestId。
    nlohmann::json pending;
    nlohmann::json requested;
    bool           receivedPending   = false;
    bool           receivedRequested = false;
    if ( !pumpUntil(server, [&]() {
             // 两个连接的回调顺序不确定，状态布尔值跨轮保留已收到结果。
             receivedPending = receivedPending ||
                               takeMessage(*guest, "join_pending", pending);
             receivedRequested =
                 receivedRequested ||
                 takeMessage(*host, "join_requested", requested);
             return receivedPending && receivedRequested;
         }) ) {
        return fail("join_notifications");
    }
    // requestId 是房主审批与访客等待状态之间的唯一关联键。
    const std::string requestId = requested.value("requestId", "");
    // 未提供构建指纹的旧式请求不应凭空产生 guestBuildFingerprint 字段。
    if ( requestId.empty() || requestId != pending.value("requestId", "") ||
         requested.contains("guestBuildFingerprint") ) {
        return fail("join_request_identity");
    }

    // 房主另建中继连接执行 accept_join，控制连接仍保留房间所有权。
    auto hostPeer = connectSocket(server.listeningPort());
    if ( !hostPeer ||
         !pumpUntil(server,
                    [&]() {
                        return hostPeer->opened.load(std::memory_order_acquire);
                    }) ||
         !sendJson(*hostPeer,
                   { { "type", "accept_join" },
                     { "version", 1 },
                     { "roomId", roomId },
                     { "requestId", requestId },
                     // 正确 ownerToken 授权该连接代表房主接受指定申请。
                     { "ownerToken", "0123456789abcdef0123456789abcdef" } }) ) {
        return fail("host_peer_open_and_accept_send");
    }

    // 审批成功后中继两端分别收到 relay_ready，才能交换 RTC 信令。
    nlohmann::json hostReady;
    nlohmann::json guestReady;
    bool           receivedHostReady  = false;
    bool           receivedGuestReady = false;
    if ( !pumpUntil(server,
                    [&]() {
                        // hostPeer 与 guest 的通知分别消费，不能假设到达顺序。
                        receivedHostReady =
                            receivedHostReady ||
                            takeMessage(*hostPeer, "relay_ready", hostReady);
                        receivedGuestReady =
                            receivedGuestReady ||
                            takeMessage(*guest, "relay_ready", guestReady);
                        return receivedHostReady && receivedGuestReady;
                    }) ||
         hostReady.value("roomId", "") != roomId ||
         guestReady.value("requestId", "") != requestId ) {
        // 房主响应关联房间、访客响应关联申请，分别核对关键身份。
        return fail("relay_ready");
    }

    // 访客向房主发送 SDP description，模拟 WebRTC offer 信令。
    const nlohmann::json guestSignal = {
        { "type", "description" },
        { "sdp", "guest-offer" },
        { "descriptionType", "offer" },
    };
    // 房主反向发送 ICE candidate，覆盖另一种透明中继消息类型。
    const nlohmann::json hostSignal = {
        { "type", "candidate" },
        { "candidate", "host-candidate" },
        { "mid", "0" },
    };
    // 两个方向都必须被第三方 WebSocket 接受后才进入接收阶段。
    if ( !sendJson(*guest, guestSignal) || !sendJson(*hostPeer, hostSignal) ) {
        return fail("relay_payload_send");
    }
    // 分别保存中继后的 JSON，验证服务端不改写关键负载字段。
    nlohmann::json relayedToHost;
    nlohmann::json relayedToGuest;
    bool           receivedByHost  = false;
    bool           receivedByGuest = false;
    if ( !pumpUntil(server,
                    [&]() {
                        // 任一方向可先到达，布尔标志防止已消费消息丢失进度。
                        receivedByHost =
                            receivedByHost ||
                            takeMessage(
                                *hostPeer, "description", relayedToHost);
                        receivedByGuest =
                            receivedByGuest ||
                            takeMessage(*guest, "candidate", relayedToGuest);
                        return receivedByHost && receivedByGuest;
                    }) ||
         relayedToHost.value("sdp", "") != "guest-offer" ||
         relayedToGuest.value("candidate", "") != "host-candidate" ) {
        // description 与 candidate 的方向和值都必须保持透明转发。
        return fail("relay_payload_receive");
    }

    // 主动关闭访客中继连接，验证服务端清理配对但保留房间控制连接。
    static_cast<void>(rtcClose(guest->id));
    if ( !pumpUntil(server, [&]() {
             // 目录、房主控制连接仍存活，因此期望两客户端和一房间。
             return server.roomCount() == 1U && server.clientCount() == 2U;
         }) ) {
        return fail("guest_cleanup");
    }
    // 清理断言同时证明关闭一个中继端不会误删仍由 host 持有的房间。

    // 第二个访客携带构建指纹，用于覆盖房主明确拒绝的审批路径。
    auto rejectedGuest = connectSocket(server.listeningPort());
    if ( !rejectedGuest ||
         !pumpUntil(server,
                    [&]() {
                        return rejectedGuest->opened.load(
                            std::memory_order_acquire);
                    }) ||
         !sendJson(*rejectedGuest,
                   { { "type", "join_room" },
                     { "version", 1 },
                     { "roomId", roomId },
                     { "creator", "Rejected Creator" },
                     // 合法固定指纹应原样出现在房主的 join_requested 中。
                     { "buildFingerprint", TEST_BUILD_FINGERPRINT } }) ) {
        return fail("rejected_guest_join_send");
    }
    // 拒绝路径同样先要求访客 pending 与房主 requested 两侧通知齐备。
    nlohmann::json rejectedPending;
    nlohmann::json rejectedRequested;
    bool           rejectedPendingReceived   = false;
    bool           rejectedRequestedReceived = false;
    if ( !pumpUntil(server, [&]() {
             // 保留跨轮进度，避免先到通知从收件箱删除后无法再次匹配。
             rejectedPendingReceived =
                 rejectedPendingReceived ||
                 takeMessage(*rejectedGuest, "join_pending", rejectedPending);
             rejectedRequestedReceived =
                 rejectedRequestedReceived ||
                 takeMessage(*host, "join_requested", rejectedRequested);
             return rejectedPendingReceived && rejectedRequestedReceived;
         }) ) {
        return fail("rejected_join_notifications");
    }
    // 从房主通知取得服务端 requestId，访客 pending 已在上一步证明存在。
    const std::string rejectedRequestId =
        rejectedRequested.value("requestId", "");
    // 指纹必须无损转发，随后房主用所有权令牌拒绝这一次申请。
    if ( rejectedRequestId.empty() ||
         rejectedRequested.value("guestBuildFingerprint", "") !=
             TEST_BUILD_FINGERPRINT ||
         !sendJson(*host,
                   { { "type", "reject_join" },
                     { "version", 1 },
                     { "roomId", roomId },
                     { "requestId", rejectedRequestId },
                     { "ownerToken", "0123456789abcdef0123456789abcdef" },
                     // 稳定 reason 用于客户端映射为版本不兼容提示。
                     { "reason", "build_fingerprint_mismatch" } }) ) {
        return fail("host_reject_send");
    }
    // 被拒访客必须收到 error，而不是 relay_ready 或静默断开。
    nlohmann::json rejectedMessage;
    if ( !pumpUntil(server,
                    [&]() {
                        return takeMessage(
                            *rejectedGuest, "error", rejectedMessage);
                    }) ||
         rejectedMessage.value("reason", "") != "build_fingerprint_mismatch" ) {
        // reason 精确保持可证明服务端透明传递房主拒绝原因。
        return fail("host_reject_delivery");
    }
    // rejectedGuest 未进入中继配对，局部析构只需关闭其控制连接。
    // 创建、封面、接受、中继、断开清理和拒绝路径全部通过。
    return true;
}
}  // namespace

/// @brief 运行完整的本机信令服务集成场景。
/// @return 全部协议阶段通过时返回 0，否则返回 1。
int main()
{
    // 自动测试不接受外部地址参数，保证 CTest 永远不会访问公网服务。
    // 测试函数拥有服务与套接字生命周期，返回前按逆序完成资源释放。
    return testDirectoryAndSignalingRelay() ? 0 : 1;
}
