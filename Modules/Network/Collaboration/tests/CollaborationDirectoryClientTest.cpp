/**
 * @file CollaborationDirectoryClientTest.cpp
 * @brief 验证房间目录订阅、刷新、封面分块和外部部署连通性。
 * @details 自动模式只使用本机回环服务；真实网络访问必须由命令行模式显式选择。
 */

#include "network/collaboration/CollaborationDirectoryClient.h"
#include "network/collaboration/WebRtcTransport.h"
#include "network/collaboration_server/CollaborationSignalingServer.h"

#include "log/colorful-log.h"

#include <algorithm>
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
// 客户端协议类型仅引入测试内部命名空间，避免产生对外符号。
using MMM::Network::Collaboration::CollaborationDirectoryClient;
using MMM::Network::Collaboration::CollaborationDirectoryState;
using MMM::Network::Collaboration::CollaborationServerEndpoint;
using MMM::Network::Collaboration::WebRtcHostConfig;
using MMM::Network::Collaboration::WebRtcTransport;
using MMM::Network::Collaboration::WebRtcTransportEvent;
using MMM::Network::Collaboration::WebRtcTransportEventType;
// 服务端类型用于建立进程内回环环境，不依赖外部部署进程。
using MMM::Network::CollaborationServer::CollaborationSignalingServer;
using MMM::Network::CollaborationServer::CollaborationSignalingServerConfig;

/// @brief 公网目录客户端本机集成测试允许的最长等待时间。
/// @note 每个独立连接阶段各自使用完整窗口，避免共享截止时间造成误报。
constexpr auto TEST_TIMEOUT = std::chrono::seconds(10);

/// @brief 驱动服务端和目录客户端直到条件满足。
/// @tparam Predicate 不阻塞且可重复调用的完成条件类型。
/// @param server 当前测试进程内运行的信令服务。
/// @param directory 接收房间目录事件的客户端。
/// @param predicate 每轮更新后检查的完成条件。
/// @return 截止时间前条件成立时返回 true，否则返回 false。
/// @warning 仅用于集成测试轮询；固定 sleep 不得复制到产品交互路径。
template<typename Predicate>
bool pumpUntil(CollaborationSignalingServer& server,
               CollaborationDirectoryClient& directory, Predicate predicate)
{
    // steady_clock 不受系统时间校准影响，适合作为测试超时基准。
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        // 服务端与客户端在同一线程交替推进，使事件顺序保持可复现。
        server.update();
        directory.update();
        // 条件在双方都处理一轮事件后判断，避免观察半推进状态。
        if ( predicate() ) return true;
        // 2ms 只限制测试忙轮询占用，不延迟任何应用运行时逻辑。
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // 超时由调用方结合阶段名和状态快照输出诊断。
    return false;
}

/// @brief 验证结构化端点、房间发布和目录订阅刷新。
/// @return 本机目录、封面与刷新流程全部成功时返回 true。
/// @note 测试使用操作系统分配的回环端口，不依赖外部部署服务。
bool testPublishedRoomAppearsInDirectory()
{
    // 零端口请求系统选择空闲端口，避免并行测试之间固定端口冲突。
    CollaborationSignalingServerConfig serverConfig;
    serverConfig.port = 0;
    // 回环地址把测试流量限制在本机，不暴露临时服务到局域网。
    serverConfig.bindAddress = "127.0.0.1";
    CollaborationSignalingServer server;
    // start 成功后还要求实际端口非零，保证客户端拥有可连接端点。
    if ( !server.start(std::move(serverConfig)) ||
         server.listeningPort() == 0 ) {
        XERROR("Directory client test failed at server_start");
        return false;
    }

    // 目录客户端与房主必须使用同一实际监听地址、端口和明文模式。
    CollaborationServerEndpoint endpoint;
    endpoint.address       = "127.0.0.1";
    endpoint.signalingPort = server.listeningPort();
    // 回环测试不配置证书，因此明确关闭 TLS。
    endpoint.useTls = false;
    CollaborationDirectoryClient directory;
    // 失败帮助器统一捕获当前状态、错误和服务端对象计数。
    const auto fail = [&server, &directory](std::string_view stage) {
        // 不输出任何信令负载，只保留定位异步阶段所需的状态快照。
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
    // connect 只启动异步握手；完成状态由后续 pumpUntil 观察。
    if ( !directory.connect(endpoint) ) return fail("directory_connect");

    // 先完成目录订阅，再发布房间；两个独立 WebSocket 握手不应共享一个
    // 测试超时窗口，避免高负载 CI 把后发起的房主连接挤到截止点。
    // 首阶段等待 Connecting 退出，再单独断言成功状态和空初始快照。
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

    // 房主使用真实 WebRtcTransport 发布房间元数据到同一信令服务。
    WebRtcTransport  transport;
    WebRtcHostConfig hostConfig;
    // 80 KiB 超过单帧封面载荷，用于覆盖目录协议的分块路径。
    const std::string roomCoverImage(80U * 1024U, 'A');
    hostConfig.endpoint = endpoint;
    // 名称与创建者使用稳定文本，后续目录快照逐字段核对。
    hostConfig.roomName       = "Directory Test Room";
    hostConfig.creator        = "Directory Host";
    hostConfig.roomCoverImage = roomCoverImage;
    // 固定 64 位小写指纹满足握手格式，不依赖当前测试二进制摘要。
    hostConfig.buildFingerprint = std::string(64U, 'a');
    // 参与者与会话 ID 分离，验证服务端按正式身份字段发布房间。
    hostConfig.participantId = "a0000000000000000000000000000001";
    hostConfig.sessionId     = "b0000000000000000000000000000001";
    // startHost 同样只启动异步流程，RoomPublished 事件才表示发布完成。
    if ( !transport.startHost(hostConfig) ) return fail("host_start");

    // 发布标志与错误文本由事件队列推进，条件函数每轮完整排空队列。
    bool        published = false;
    std::string transportError;
    if ( !pumpUntil(server,
                    directory,
                    [&]() {
                        // 单轮可能积累多个事件，必须读取到队列为空。
                        WebRtcTransportEvent event;
                        while ( transport.receiveEvent(event) ) {
                            if ( event.type ==
                                 WebRtcTransportEventType::RoomPublished ) {
                                // 记录发布确认，但仍等待目录快照和封面标志到达。
                                published = true;
                            } else if ( event.type ==
                                        WebRtcTransportEventType::Error ) {
                                // 保存最后错误详情，条件立即结束并由外层记录。
                                transportError = std::move(event.detail);
                            }
                        }
                        // 错误优先终止；成功需同时满足发布、连接与唯一房间。
                        return !transportError.empty() ||
                               (published &&
                                directory.state() ==
                                    CollaborationDirectoryState::Connected &&
                                directory.rooms().size() == 1U &&
                                directory.rooms().front().hasCoverImage);
                    }) ||
         !transportError.empty() ) {
        if ( !transportError.empty() ) {
            // 传输错误单独输出详情，通用 fail 再补充服务端和目录状态。
            XERROR("Directory client test host transport failed: {}",
                   transportError);
        }
        return fail("room_publish");
    }
    // 发布阶段保证列表恰有一个房间，因此 front 引用在以下检查中稳定。
    const auto& room = directory.rooms().front();
    // 快照必须保留服务端分配 ID、展示字段、人数、容量和封面可用标志。
    if ( room.roomId != transport.roomId() ||
         room.roomName != "Directory Test Room" ||
         room.hostCreator != "Directory Host" || room.participants != 1U ||
         room.capacity != 8U || !room.hasCoverImage ||
         !directory.requestRoomCover(room.roomId) ) {
        return fail("room_snapshot");
    }
    // 封面请求异步返回，等待重组后的完整内容与原 80 KiB 字符串相等。
    if ( !pumpUntil(server, directory, [&]() {
             return directory.roomCover(room.roomId) == roomCoverImage;
         }) ) {
        // 分块缺失、顺序错误或内容尚未发布均统一归入封面阶段失败。
        return fail("room_cover");
    }
    // 显式 refresh 应保持连接并重新获得同一房间快照。
    if ( !directory.refresh() ) return fail("directory_refresh_request");
    if ( !pumpUntil(server, directory, [&]() {
             return directory.state() ==
                        CollaborationDirectoryState::Connected &&
                    directory.rooms().size() == 1U;
         }) ) {
        // 刷新后仍要求唯一房间，防止重复快照被错误追加。
        return fail("directory_refresh");
    }
    // 目录订阅、发布、封面分块和刷新四阶段均完成。
    return true;
}

/// @brief 验证部署后的外部房间目录可由产品使用的 WebSocket 客户端访问。
/// @param address 外部信令服务地址。
/// @param signalingPort 外部 WebSocket 端口。
/// @param useTls 是否使用 TLS WebSocket。
/// @return 截止时间前目录进入 Connected 状态时返回 true。
/// @warning 仅由显式 external 模式调用，不属于自动 CTest。
bool testExternalDirectoryEndpoint(std::string   address,
                                   std::uint16_t signalingPort, bool useTls)
{
    // 按值接收地址后移动到端点，探针无需保留调用方字符串。
    CollaborationServerEndpoint endpoint;
    endpoint.address       = std::move(address);
    endpoint.signalingPort = signalingPort;
    endpoint.useTls        = useTls;
    CollaborationDirectoryClient directory;
    // 该探针只订阅目录，不发布房间，因此不会改变目标服务状态。
    // 同步失败表示连异步连接任务都无法创建，无需进入轮询。
    if ( !directory.connect(std::move(endpoint)) ) return false;

    // 外部探针只推进客户端，远端服务由独立部署进程推进。
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        directory.update();
        // 成功只要求目录连接建立，不要求部署环境当前存在房间。
        if ( directory.state() == CollaborationDirectoryState::Connected ) {
            return true;
        }
        // 明确错误无需等待满超时，立即输出客户端保留的原因。
        if ( directory.state() == CollaborationDirectoryState::Error ) {
            XERROR("External collaboration directory probe failed: {}",
                   directory.lastError());
            return false;
        }
        // 5ms 限制命令行探针忙轮询，不用于产品更新循环。
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 超时与明确错误分开报告，便于部署诊断网络不可达问题。
    XERROR("External collaboration directory probe timed out");
    return false;
}

/// @brief 验证公网端点能够发布并取回大尺寸分块房间封面。
/// @param address 外部信令服务地址。
/// @param signalingPort 外部 WebSocket 端口。
/// @param useTls 是否使用 TLS WebSocket。
/// @return 房间发布且封面完整取回时返回 true。
/// @warning 该部署探针创建真实外部连接，只能由显式 external-room 模式运行。
bool testExternalRoomCover(std::string address, std::uint16_t signalingPort,
                           bool useTls)
{
    // 目录与房主共享同一外部端点，确保查询的是刚发布的房间。
    CollaborationServerEndpoint endpoint;
    endpoint.address       = std::move(address);
    endpoint.signalingPort = signalingPort;
    endpoint.useTls        = useTls;

    // 目录连接先启动，但其握手与房主发布在同一轮询中并行推进。
    CollaborationDirectoryClient directory;
    // 房间探针会短暂发布测试元数据，局部 transport 负责结束其生命周期。
    if ( !directory.connect(endpoint) ) return false;

    // 房主使用稳定探针元数据，避免与普通用户房间标识混淆。
    WebRtcTransport  transport;
    WebRtcHostConfig hostConfig;
    // 大于单块阈值的确定性内容同时验证分块顺序和重组完整性。
    const std::string roomCoverImage(80U * 1024U, 'A');
    hostConfig.endpoint       = endpoint;
    hostConfig.roomName       = "External Cover Probe";
    hostConfig.creator        = "Cover Probe Host";
    hostConfig.roomCoverImage = roomCoverImage;
    // 指纹和身份字段满足正式协议格式，但不关联本机用户配置。
    hostConfig.buildFingerprint = std::string(64U, 'a');
    hostConfig.participantId    = "c0000000000000000000000000000001";
    hostConfig.sessionId        = "d0000000000000000000000000000001";
    // 无法创建房主连接时直接失败，避免目录探针产生无关超时。
    if ( !transport.startHost(hostConfig) ) return false;

    // published 防止目录残留同名房间被误判为本次发布结果。
    bool published = false;
    // coverRequested 保证对目标房间只发送一次封面请求。
    bool        coverRequested = false;
    std::string transportError;
    // 整个外部封面探针共享一个窗口，反映端到端部署响应时间。
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        // 目录客户端负责接收列表快照和封面分块。
        directory.update();
        // 房主事件队列可能在一轮积累发布确认与错误，需完整排空。
        WebRtcTransportEvent event;
        while ( transport.receiveEvent(event) ) {
            if ( event.type == WebRtcTransportEventType::RoomPublished ) {
                // 发布确认后才允许将目录中的目标 ID视为本次房间。
                published = true;
            } else if ( event.type == WebRtcTransportEventType::Error ) {
                // 移动错误详情，避免复制潜在较长的第三方诊断文本。
                transportError = std::move(event.detail);
            }
        }
        // 房主传输错误属于终止条件，不能靠继续轮询恢复。
        if ( !transportError.empty() ) {
            XERROR("External room cover probe failed: {}", transportError);
            return false;
        }
        // 目录连接错误与房主错误分别报告各自状态来源。
        if ( directory.state() == CollaborationDirectoryState::Error ) {
            XERROR("External room cover directory failed: {}",
                   directory.lastError());
            return false;
        }

        // 外部目录可能已有其他房间，必须按服务端分配的 roomId 查找。
        const auto roomIterator =
            std::find_if(directory.rooms().begin(),
                         directory.rooms().end(),
                         [&transport](const auto& room) {
                             // transport.roomId
                             // 在发布后固定，可安全用于谓词读取。
                             return room.roomId == transport.roomId();
                         });
        // 只在发布确认、目录可见且声明有封面后发起获取请求。
        if ( published && roomIterator != directory.rooms().end() &&
             roomIterator->hasCoverImage && !coverRequested ) {
            // 保存 API 返回值；提交失败时下一轮仍可重试。
            coverRequested = directory.requestRoomCover(roomIterator->roomId);
        }
        // 完整字节相等证明所有分块均到达且按正确顺序重组。
        if ( coverRequested &&
             directory.roomCover(transport.roomId()) == roomCoverImage ) {
            return true;
        }
        // 探针等待不会进入编辑器热路径，仅降低命令行运行时 CPU 占用。
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 超时日志保留发布状态和目标 ID，区分列表与封面阶段卡住。
    XERROR("External room cover probe timed out: published={}, room_id={}",
           published,
           transport.roomId());
    return false;
}
}  // namespace

/// @brief 运行本机自动测试或显式选择外部部署探针。
/// @param argc 命令行参数数量。
/// @param argv 模式、地址、端口与 TLS 开关参数。
/// @return 成功返回 0，探针失败返回 1，外部参数错误返回 2。
int main(int argc, char** argv)
{
    // 无参数时 mode 为空并落入可重复的本机集成测试。
    const std::string_view mode = argc > 1 ? argv[1] : "";
    // 外部模式要求严格五项参数，防止缺省地址或 TLS 语义不明确。
    if ( argc == 5 && (mode == "external" || mode == "external-room") ) {
        // argv[2] 在严格参数数量下必定存在，作为原始服务地址传递。
        // 使用较宽整数解析后再检查 uint16_t 范围，避免截断。
        std::uint32_t          port = 0;
        const std::string_view portText(argv[3]);
        // from_chars 不分配且不抛异常，end 用于拒绝尾随字符。
        const auto [end, error] = std::from_chars(
            portText.data(), portText.data() + portText.size(), port);
        if ( error != std::errc{} || end != portText.data() + portText.size() ||
             port == 0 || port > 65535 ) {
            // 外部服务端口必须是完整十进制文本且处于可连接范围。
            return 2;
        }
        // TLS 参数只接受完整小写布尔值，避免模糊命令行解释。
        const std::string_view tlsText(argv[4]);
        if ( tlsText != "true" && tlsText != "false" ) return 2;
        // 外部探针可能输出网络诊断，运行前初始化独立测试日志器。
        XLogger::init("CollaborationDirectoryClientTest");
        // external 只验证目录握手，external-room 额外验证发布与大封面。
        const bool success =
            mode == "external"
                ? testExternalDirectoryEndpoint(
                      argv[2],
                      static_cast<std::uint16_t>(port),
                      tlsText == "true")
                : testExternalRoomCover(argv[2],
                                        static_cast<std::uint16_t>(port),
                                        tlsText == "true");
        // 所有外部返回路径在此汇合，确保异步日志被刷新并关闭。
        XLogger::shutdown();
        return success ? 0 : 1;
    }
    // 自动 CTest 始终使用进程内回环服务，不访问 argv 中的外部地址。
    return testPublishedRoomAppearsInDirectory() ? 0 : 1;
}
