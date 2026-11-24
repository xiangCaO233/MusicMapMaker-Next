#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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

/// @brief 公网目录中一间可直接加入的在线房间。
struct CollaborationDirectoryRoom {
    /// @brief 服务端分配的公开房间标识。
    std::string roomId;
    /// @brief 房主发布的房间名称。
    std::string roomName;
    /// @brief 房主 Creator 展示身份。
    std::string hostCreator;
    /// @brief 房主是否为目录卡片发布了封面缩略图。
    bool hasCoverImage = false;
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

}  // namespace MMM::Network::Collaboration
