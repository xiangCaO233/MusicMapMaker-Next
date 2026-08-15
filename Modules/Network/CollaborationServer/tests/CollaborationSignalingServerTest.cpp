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
using MMM::Network::CollaborationServer::CollaborationSignalingServer;
using MMM::Network::CollaborationServer::CollaborationSignalingServerConfig;

/// @brief 本机目录与信令集成测试允许的最长等待时间。
constexpr auto TEST_TIMEOUT = std::chrono::seconds(10);
/// @brief 测试访客提交的固定 SHA-256 构建指纹。
constexpr std::string_view TEST_BUILD_FINGERPRINT =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/// @brief 测试 WebSocket 的跨回调线程收件箱。
struct TestSocket {
    /// @brief libdatachannel WebSocket 句柄。
    int id = -1;
    /// @brief WebSocket 是否已经打开。
    std::atomic_bool opened{ false };
    /// @brief WebSocket 是否已经关闭。
    std::atomic_bool closed{ false };
    /// @brief 保护文本消息队列。
    std::mutex mutex;
    /// @brief 回调线程收到的文本消息。
    std::deque<std::string> messages;

    /// @brief 释放 WebSocket 句柄。
    ~TestSocket()
    {
        if ( id >= 0 ) rtcDeleteWebSocket(id);
    }

    TestSocket()                             = default;
    TestSocket(const TestSocket&)            = delete;
    TestSocket& operator=(const TestSocket&) = delete;
    TestSocket(TestSocket&&)                 = delete;
    TestSocket& operator=(TestSocket&&)      = delete;
};

/// @brief 标记测试 WebSocket 已打开。
void onOpen(int, void* pointer)
{
    auto* socket = static_cast<TestSocket*>(pointer);
    if ( socket ) socket->opened.store(true, std::memory_order_release);
}

/// @brief 把测试 WebSocket 文本消息压入收件箱。
void onMessage(int, const char* message, int size, void* pointer)
{
    auto* socket = static_cast<TestSocket*>(pointer);
    if ( !socket || !message || size >= 0 ) return;
    std::scoped_lock lock(socket->mutex);
    socket->messages.emplace_back(message);
}

/// @brief 标记测试 WebSocket 已关闭。
void onClosed(int, void* pointer)
{
    auto* socket = static_cast<TestSocket*>(pointer);
    if ( socket ) socket->closed.store(true, std::memory_order_release);
}

/// @brief 测试不单独消费错误文本，错误最终表现为关闭或超时。
void onError(int, const char*, void* pointer)
{
    onClosed(-1, pointer);
}

/// @brief 创建一个连接本机目录服务的 WebSocket。
std::unique_ptr<TestSocket> connectSocket(std::uint16_t port)
{
    auto               socket = std::make_unique<TestSocket>();
    rtcWsConfiguration config{};
    config.connectionTimeoutMs = 3000;
    config.maxMessageSize      = 256 * 1024;
    const std::string url =
        "ws://127.0.0.1:" + std::to_string(port) + "/mmm-collaboration";
    socket->id = rtcCreateWebSocketEx(url.c_str(), &config);
    if ( socket->id < 0 ) return {};
    rtcSetUserPointer(socket->id, socket.get());
    rtcSetOpenCallback(socket->id, &onOpen);
    rtcSetMessageCallback(socket->id, &onMessage);
    rtcSetClosedCallback(socket->id, &onClosed);
    rtcSetErrorCallback(socket->id, &onError);
    if ( rtcIsOpen(socket->id) ) {
        socket->opened.store(true, std::memory_order_release);
    }
    return socket;
}

/// @brief 驱动服务直到条件满足或超时。
template<typename Predicate>
bool pumpUntil(CollaborationSignalingServer& server, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        server.update();
        if ( predicate() ) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

/// @brief 向测试 WebSocket 发送 JSON 文本消息。
bool sendJson(const TestSocket& socket, const nlohmann::json& message)
{
    const std::string payload = message.dump();
    return rtcSendMessage(socket.id, payload.c_str(), -1) == RTC_ERR_SUCCESS;
}

/// @brief 从收件箱取出指定类型的 JSON 消息。
bool takeMessage(TestSocket& socket, std::string_view type,
                 nlohmann::json& output)
{
    std::scoped_lock lock(socket.mutex);
    for ( auto iterator = socket.messages.begin();
          iterator != socket.messages.end();
          ++iterator ) {
        auto message = nlohmann::json::parse(*iterator, nullptr, false);
        if ( message.is_object() && message.value("type", "") == type ) {
            output = std::move(message);
            socket.messages.erase(iterator);
            return true;
        }
    }
    return false;
}

/// @brief 验证房间目录、加入配对和透明双向信令转发。
bool testDirectoryAndSignalingRelay()
{
    CollaborationSignalingServerConfig config;
    config.port        = 0;
    config.bindAddress = "127.0.0.1";
    config.iceServers  = { "stun:stun.example.test:3478" };
    CollaborationSignalingServer server;
    if ( !server.start(std::move(config)) || server.listeningPort() == 0 ) {
        XERROR("Signaling server test failed at server_start");
        return false;
    }
    const auto fail = [&server](std::string_view stage) {
        XERROR("Signaling server test failed at {}: clients={}, rooms={}",
               stage,
               server.clientCount(),
               server.roomCount());
        return false;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto directory = connectSocket(server.listeningPort());
    auto host      = connectSocket(server.listeningPort());
    if ( !directory || !host || !pumpUntil(server, [&]() {
             return directory->opened.load(std::memory_order_acquire) &&
                    host->opened.load(std::memory_order_acquire);
         }) ) {
        return fail("directory_host_open");
    }

    if ( !sendJson(*directory,
                   { { "type", "list_rooms" }, { "version", 1 } }) ||
         !sendJson(*host,
                   { { "type", "create_room" },
                     { "version", 1 },
                     { "roomName", "Public Test Room" },
                     { "creator", "Host Creator" },
                     { "coverImage", "SGVsbG8=" },
                     { "ownerToken", "0123456789abcdef0123456789abcdef" },
                     { "capacity", 8 } }) ) {
        return fail("room_requests_send");
    }

    nlohmann::json created;
    nlohmann::json roomList;
    bool           receivedCreated  = false;
    bool           receivedRoomList = false;
    if ( !pumpUntil(server,
                    [&]() {
                        receivedCreated =
                            receivedCreated ||
                            takeMessage(*host, "room_created", created);
                        if ( takeMessage(*directory, "room_list", roomList) &&
                             roomList.contains("rooms") &&
                             roomList["rooms"].is_array() &&
                             roomList["rooms"].size() == 1U ) {
                            receivedRoomList = true;
                        }
                        return receivedCreated && receivedRoomList;
                    }) ||
         !created.contains("roomId") || !created["roomId"].is_string() ||
         created.value("iceServers", std::vector<std::string>{}) !=
             std::vector<std::string>{ "stun:stun.example.test:3478" } ) {
        return fail("room_created_and_listed");
    }
    const std::string roomId = created["roomId"].get<std::string>();
    if ( !roomList["rooms"][0].value("hasCoverImage", false) ||
         !sendJson(*directory,
                   { { "type", "get_room_cover" },
                     { "version", 1 },
                     { "roomId", roomId } }) ) {
        return fail("room_cover_request");
    }
    nlohmann::json roomCover;
    if ( !pumpUntil(server,
                    [&]() {
                        return takeMessage(*directory, "room_cover", roomCover);
                    }) ||
         roomCover.value("roomId", "") != roomId ||
         roomCover.value("coverImage", "") != "SGVsbG8=" ) {
        return fail("room_cover_response");
    }

    auto guest = connectSocket(server.listeningPort());
    if ( !guest ||
         !pumpUntil(
             server,
             [&]() { return guest->opened.load(std::memory_order_acquire); }) ||
         !sendJson(*guest,
                   { { "type", "join_room" },
                     { "version", 1 },
                     { "roomId", roomId },
                     { "creator", "Guest Creator" } }) ) {
        return fail("guest_open_and_join_send");
    }

    nlohmann::json pending;
    nlohmann::json requested;
    bool           receivedPending   = false;
    bool           receivedRequested = false;
    if ( !pumpUntil(server, [&]() {
             receivedPending = receivedPending ||
                               takeMessage(*guest, "join_pending", pending);
             receivedRequested =
                 receivedRequested ||
                 takeMessage(*host, "join_requested", requested);
             return receivedPending && receivedRequested;
         }) ) {
        return fail("join_notifications");
    }
    const std::string requestId = requested.value("requestId", "");
    if ( requestId.empty() || requestId != pending.value("requestId", "") ||
         requested.contains("guestBuildFingerprint") ) {
        return fail("join_request_identity");
    }

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
                     { "ownerToken", "0123456789abcdef0123456789abcdef" } }) ) {
        return fail("host_peer_open_and_accept_send");
    }

    nlohmann::json hostReady;
    nlohmann::json guestReady;
    bool           receivedHostReady  = false;
    bool           receivedGuestReady = false;
    if ( !pumpUntil(server,
                    [&]() {
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
        return fail("relay_ready");
    }

    const nlohmann::json guestSignal = {
        { "type", "description" },
        { "sdp", "guest-offer" },
        { "descriptionType", "offer" },
    };
    const nlohmann::json hostSignal = {
        { "type", "candidate" },
        { "candidate", "host-candidate" },
        { "mid", "0" },
    };
    if ( !sendJson(*guest, guestSignal) || !sendJson(*hostPeer, hostSignal) ) {
        return fail("relay_payload_send");
    }
    nlohmann::json relayedToHost;
    nlohmann::json relayedToGuest;
    bool           receivedByHost  = false;
    bool           receivedByGuest = false;
    if ( !pumpUntil(server,
                    [&]() {
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
        return fail("relay_payload_receive");
    }

    static_cast<void>(rtcClose(guest->id));
    if ( !pumpUntil(server, [&]() {
             return server.roomCount() == 1U && server.clientCount() == 2U;
         }) ) {
        return fail("guest_cleanup");
    }

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
                     { "buildFingerprint", TEST_BUILD_FINGERPRINT } }) ) {
        return fail("rejected_guest_join_send");
    }
    nlohmann::json rejectedPending;
    nlohmann::json rejectedRequested;
    bool           rejectedPendingReceived   = false;
    bool           rejectedRequestedReceived = false;
    if ( !pumpUntil(server, [&]() {
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
    const std::string rejectedRequestId =
        rejectedRequested.value("requestId", "");
    if ( rejectedRequestId.empty() ||
         rejectedRequested.value("guestBuildFingerprint", "") !=
             TEST_BUILD_FINGERPRINT ||
         !sendJson(*host,
                   { { "type", "reject_join" },
                     { "version", 1 },
                     { "roomId", roomId },
                     { "requestId", rejectedRequestId },
                     { "ownerToken", "0123456789abcdef0123456789abcdef" },
                     { "reason", "build_fingerprint_mismatch" } }) ) {
        return fail("host_reject_send");
    }
    nlohmann::json rejectedMessage;
    if ( !pumpUntil(server,
                    [&]() {
                        return takeMessage(
                            *rejectedGuest, "error", rejectedMessage);
                    }) ||
         rejectedMessage.value("reason", "") != "build_fingerprint_mismatch" ) {
        return fail("host_reject_delivery");
    }
    return true;
}
}  // namespace

int main()
{
    return testDirectoryAndSignalingRelay() ? 0 : 1;
}
