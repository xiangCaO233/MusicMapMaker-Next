#pragma once

#include "collaboration/CollaborationTypes.h"

#include <span>

namespace MMM::Collaboration
{
/// @brief 传输层交给协作状态机的一条完整可靠消息。
struct TransportPacket {
    /// @brief 发送客户端标识。
    PeerId senderId = 0;
    /// @brief 完整线协议帧。
    ByteBuffer payload;
};

/// @brief 协作状态机使用的可靠有序字节传输抽象。
class ICollaborationTransport
{
public:
    /// @brief 释放传输实现。
    virtual ~ICollaborationTransport() = default;

    /// @brief 向指定客户端发送一条完整消息。
    /// @param recipientId 接收客户端标识。
    /// @param payload 完整线协议帧。
    /// @return 消息进入传输队列时返回 true。
    [[nodiscard]] virtual bool send(PeerId                        recipientId,
                                    std::span<const std::uint8_t> payload) = 0;

    /// @brief 非阻塞读取下一条完整消息。
    /// @param packet 接收消息的输出对象。
    /// @return 读取到消息时返回 true。
    [[nodiscard]] virtual bool receive(TransportPacket& packet) = 0;
};
}  // namespace MMM::Collaboration
