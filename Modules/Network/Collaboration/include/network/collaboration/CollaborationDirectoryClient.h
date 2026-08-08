#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::Network::Collaboration
{
/// @brief 公网协作目录、信令和 TLS 连接参数。
struct CollaborationServerEndpoint {
    /// @brief 中心服务器地址或域名，不包含协议和路径。
    std::string address = "xiang233.top";
    /// @brief WebSocket 信令端口。
    std::uint16_t signalingPort = 443;
    /// @brief 是否使用 TLS/WSS。
    bool useTls = true;

    /// @brief 比较地址、信令端口和 TLS 配置。
    bool operator==(const CollaborationServerEndpoint&) const = default;
};

/// @brief 校验并构造中心服务内部 WebSocket URL。
/// @param endpoint 用户可配置的地址、端口和 TLS 选项。
/// @return 配置无效时返回空字符串。
[[nodiscard]] std::string makeCollaborationSignalingUrl(
    const CollaborationServerEndpoint& endpoint);

/// @brief 公网目录中一间可直接加入的在线房间。
struct CollaborationDirectoryRoom {
    /// @brief 服务端分配的公开房间标识。
    std::string roomId;
    /// @brief 房主发布的房间名称。
    std::string roomName;
    /// @brief 房主 Creator 展示身份。
    std::string hostCreator;
    /// @brief 当前已建立 P2P 连接的成员数量。
    std::size_t participants = 0;
    /// @brief 房间允许的总成员数量。
    std::size_t capacity = 0;
};

/// @brief 公网房间目录连接状态。
enum class CollaborationDirectoryState {
    Idle,
    Connecting,
    Connected,
    Error,
};

/// @brief 订阅中心服务公开房间列表的轻量 WebSocket 客户端。
class CollaborationDirectoryClient final
{
public:
    /// @brief 创建尚未连接的目录客户端。
    CollaborationDirectoryClient();
    /// @brief 关闭目录 WebSocket。
    ~CollaborationDirectoryClient();

    CollaborationDirectoryClient(const CollaborationDirectoryClient&) = delete;
    CollaborationDirectoryClient& operator=(
        const CollaborationDirectoryClient&)                     = delete;
    CollaborationDirectoryClient(CollaborationDirectoryClient&&) = delete;
    CollaborationDirectoryClient& operator=(CollaborationDirectoryClient&&) =
        delete;

    /// @brief 开始连接指定目录入口。
    /// @param endpoint 地址、信令端口与 TLS 配置。
    /// @return WebSocket 成功创建时返回 true。
    [[nodiscard]] bool connect(CollaborationServerEndpoint endpoint);

    /// @brief 关闭连接并清空当前房间列表。
    void disconnect();

    /// @brief 消费回调事件并更新房间快照。
    /// @warning UI
    /// 热路径：每帧调用，只处理有界内存队列；禁止等待和文件系统访问。
    void update();

    /// @brief 主动请求服务端重发当前房间列表。
    /// @return WebSocket 已连接且消息发送成功时返回 true。
    [[nodiscard]] bool refresh();

    /// @brief 返回当前目录状态。
    [[nodiscard]] CollaborationDirectoryState state() const;
    /// @brief 返回最近一次有效的房间列表快照。
    [[nodiscard]] const std::vector<CollaborationDirectoryRoom>& rooms() const;
    /// @brief 返回最近连接或协议错误。
    [[nodiscard]] const std::string& lastError() const;
    /// @brief 返回当前中心服务器配置。
    [[nodiscard]] const CollaborationServerEndpoint& endpoint() const;

private:
    class Impl;
    /// @brief 隔离 libdatachannel 回调和第三方类型。
    std::unique_ptr<Impl> m_impl;
};
}  // namespace MMM::Network::Collaboration
