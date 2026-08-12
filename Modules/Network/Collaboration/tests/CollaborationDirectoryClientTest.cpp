#include "network/collaboration/CollaborationDirectoryClient.h"
#include "network/collaboration/WebRtcTransport.h"
#include "network/collaboration_server/CollaborationSignalingServer.h"

#include "log/colorful-log.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace
{
using MMM::Network::Collaboration::CollaborationDirectoryClient;
using MMM::Network::Collaboration::CollaborationDirectoryState;
using MMM::Network::Collaboration::CollaborationServerEndpoint;
using MMM::Network::Collaboration::WebRtcHostConfig;
using MMM::Network::Collaboration::WebRtcTransport;
using MMM::Network::Collaboration::WebRtcTransportEvent;
using MMM::Network::Collaboration::WebRtcTransportEventType;
using MMM::Network::CollaborationServer::CollaborationSignalingServer;
using MMM::Network::CollaborationServer::CollaborationSignalingServerConfig;

/// @brief 公网目录客户端本机集成测试允许的最长等待时间。
constexpr auto TEST_TIMEOUT = std::chrono::seconds(10);

/// @brief 驱动服务端和目录客户端直到条件满足。
template<typename Predicate>
bool pumpUntil(CollaborationSignalingServer& server,
               CollaborationDirectoryClient& directory, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        server.update();
        directory.update();
        if ( predicate() ) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

/// @brief 验证结构化端点、房间发布和目录订阅刷新。
bool testPublishedRoomAppearsInDirectory()
{
    CollaborationSignalingServerConfig serverConfig;
    serverConfig.port        = 0;
    serverConfig.bindAddress = "127.0.0.1";
    CollaborationSignalingServer server;
    if ( !server.start(std::move(serverConfig)) ||
         server.listeningPort() == 0 ) {
        XERROR("Directory client test failed at server_start");
        return false;
    }

    CollaborationServerEndpoint endpoint;
    endpoint.address       = "127.0.0.1";
    endpoint.signalingPort = server.listeningPort();
    endpoint.useTls        = false;
    CollaborationDirectoryClient directory;
    const auto fail = [&server, &directory](std::string_view stage) {
        XERROR(
            "Directory client test failed at {}: state={}, error={}, "
            "clients={}, rooms={}",
            stage,
            static_cast<int>(directory.state()),
            directory.lastError(),
            server.clientCount(),
            server.roomCount());
        return false;
    };
    if ( !directory.connect(endpoint) ) return fail("directory_connect");

    // 先完成目录订阅，再发布房间；两个独立 WebSocket 握手不应共享一个
    // 测试超时窗口，避免高负载 CI 把后发起的房主连接挤到截止点。
    if ( !pumpUntil(server,
                    directory,
                    [&]() {
                        return directory.state() !=
                               CollaborationDirectoryState::Connecting;
                    }) ||
         directory.state() != CollaborationDirectoryState::Connected ||
         !directory.rooms().empty() ) {
        return fail("directory_bootstrap");
    }

    WebRtcTransport  transport;
    WebRtcHostConfig hostConfig;
    hostConfig.endpoint         = endpoint;
    hostConfig.roomName         = "Directory Test Room";
    hostConfig.creator          = "Directory Host";
    hostConfig.buildFingerprint = std::string(64U, 'a');
    hostConfig.participantId    = "a0000000000000000000000000000001";
    hostConfig.sessionId        = "b0000000000000000000000000000001";
    if ( !transport.startHost(hostConfig) ) return fail("host_start");

    bool        published = false;
    std::string transportError;
    if ( !pumpUntil(server,
                    directory,
                    [&]() {
                        WebRtcTransportEvent event;
                        while ( transport.receiveEvent(event) ) {
                            if ( event.type ==
                                 WebRtcTransportEventType::RoomPublished ) {
                                published = true;
                            } else if ( event.type ==
                                        WebRtcTransportEventType::Error ) {
                                transportError = std::move(event.detail);
                            }
                        }
                        return !transportError.empty() ||
                               (published &&
                                directory.state() ==
                                    CollaborationDirectoryState::Connected &&
                                directory.rooms().size() == 1U);
                    }) ||
         !transportError.empty() ) {
        if ( !transportError.empty() ) {
            XERROR("Directory client test host transport failed: {}",
                   transportError);
        }
        return fail("room_publish");
    }
    const auto& room = directory.rooms().front();
    if ( room.roomId != transport.roomId() ||
         room.roomName != "Directory Test Room" ||
         room.hostCreator != "Directory Host" || room.participants != 1U ||
         room.capacity != 8U || !directory.refresh() ) {
        return fail("room_snapshot");
    }
    if ( !pumpUntil(server, directory, [&]() {
             return directory.state() ==
                        CollaborationDirectoryState::Connected &&
                    directory.rooms().size() == 1U;
         }) ) {
        return fail("directory_refresh");
    }
    return true;
}

/// @brief 验证部署后的外部房间目录可由产品使用的 WebSocket 客户端访问。
bool testExternalDirectoryEndpoint(std::string   address,
                                   std::uint16_t signalingPort, bool useTls)
{
    CollaborationServerEndpoint endpoint;
    endpoint.address       = std::move(address);
    endpoint.signalingPort = signalingPort;
    endpoint.useTls        = useTls;
    CollaborationDirectoryClient directory;
    if ( !directory.connect(std::move(endpoint)) ) return false;

    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        directory.update();
        if ( directory.state() == CollaborationDirectoryState::Connected ) {
            return true;
        }
        if ( directory.state() == CollaborationDirectoryState::Error ) {
            XERROR("External collaboration directory probe failed: {}",
                   directory.lastError());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    XERROR("External collaboration directory probe timed out");
    return false;
}
}  // namespace

int main(int argc, char** argv)
{
    if ( argc == 5 && std::string_view(argv[1]) == "external" ) {
        std::uint32_t          port = 0;
        const std::string_view portText(argv[3]);
        const auto [end, error] = std::from_chars(
            portText.data(), portText.data() + portText.size(), port);
        if ( error != std::errc{} || end != portText.data() + portText.size() ||
             port == 0 || port > 65535 ) {
            return 2;
        }
        const std::string_view tlsText(argv[4]);
        if ( tlsText != "true" && tlsText != "false" ) return 2;
        XLogger::init("CollaborationDirectoryClientTest");
        const bool success = testExternalDirectoryEndpoint(
            argv[2], static_cast<std::uint16_t>(port), tlsText == "true");
        XLogger::shutdown();
        return success ? 0 : 1;
    }
    return testPublishedRoomAppearsInDirectory() ? 0 : 1;
}
