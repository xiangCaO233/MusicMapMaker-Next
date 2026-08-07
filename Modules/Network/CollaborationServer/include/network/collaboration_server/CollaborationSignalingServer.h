#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MMM::Network::CollaborationServer
{
/// @brief 中心房间目录与 WebRTC 信令服务配置。
struct CollaborationSignalingServerConfig {
    /// @brief WebSocket 监听端口；0 表示自动选择。
    std::uint16_t port = 24864;
    /// @brief 监听地址；为空时监听全部地址。
    std::string bindAddress = "0.0.0.0";
    /// @brief 是否直接启用 TLS/WSS。
    bool enableTls = false;
    /// @brief PEM 证书文件路径；关闭 TLS 时忽略。
    std::string certificatePemFile;
    /// @brief PEM 私钥文件路径；关闭 TLS 时忽略。
    std::string keyPemFile;
    /// @brief 下发给 WebRTC 客户端的 STUN/TURN URI。
    std::vector<std::string> iceServers;
    /// @brief 同时保留的房间上限。
    std::size_t maxRooms = 256;
    /// @brief 同时接受的 WebSocket 客户端上限。
    std::size_t maxClients = 2048;
};

/// @brief 维护公开房间目录并在配对客户端间透明转发 WebRTC 信令。
class CollaborationSignalingServer final
{
public:
    /// @brief 创建尚未监听的服务。
    CollaborationSignalingServer();
    /// @brief 停止监听并释放全部客户端。
    ~CollaborationSignalingServer();

    CollaborationSignalingServer(const CollaborationSignalingServer&) = delete;
    CollaborationSignalingServer& operator=(
        const CollaborationSignalingServer&)                     = delete;
    CollaborationSignalingServer(CollaborationSignalingServer&&) = delete;
    CollaborationSignalingServer& operator=(CollaborationSignalingServer&&) =
        delete;

    /// @brief 启动服务。
    /// @param config 监听、容量和 ICE 配置。
    /// @return 成功创建 WebSocketServer 时返回 true。
    [[nodiscard]] bool start(CollaborationSignalingServerConfig config);

    /// @brief 停止服务；可重复调用。
    void stop();

    /// @brief 消费回调事件并推进配对、超时和目录广播。
    /// @warning
    /// 服务主循环热路径：高频调用，只处理有界内存队列；禁止文件系统访问和阻塞等待。
    void update();

    /// @brief 返回服务是否正在监听。
    [[nodiscard]] bool isRunning() const;
    /// @brief 返回实际监听端口。
    [[nodiscard]] std::uint16_t listeningPort() const;
    /// @brief 返回当前在线房间数。
    [[nodiscard]] std::size_t roomCount() const;
    /// @brief 返回当前已接入客户端数。
    [[nodiscard]] std::size_t clientCount() const;

private:
    class Impl;
    /// @brief 隔离 libdatachannel 回调与服务状态。
    std::unique_ptr<Impl> m_impl;
};
}  // namespace MMM::Network::CollaborationServer
