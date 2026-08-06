#pragma once

#include "collaboration/ICollaborationTransport.h"

#include <memory>

namespace MMM::Collaboration
{
class LoopbackTransportState;

/// @brief 为单进程 2～8 客户端测试提供可靠有序内存传输。
class LoopbackTransportHub
{
public:
    /// @brief 创建空的本地传输中心。
    LoopbackTransportHub();
    /// @brief 释放本地传输中心。
    ~LoopbackTransportHub();

    LoopbackTransportHub(const LoopbackTransportHub&)            = delete;
    LoopbackTransportHub& operator=(const LoopbackTransportHub&) = delete;

    /// @brief 为一个客户端创建唯一传输端点。
    /// @param peerId 端点所属客户端标识。
    /// @return 标识未被占用时返回端点，否则返回空指针。
    [[nodiscard]] std::unique_ptr<ICollaborationTransport> createEndpoint(
        PeerId peerId);

    /// @brief 配置是否复制后续传输包，用于验证协议去重。
    /// @param enabled 为 true 时每个发送包入队两次。
    void setDuplicatePackets(bool enabled);

    /// @brief 丢弃匹配发送方和接收方的下一条消息。
    /// @param senderId 目标发送方。
    /// @param recipientId 目标接收方。
    void dropNextPacket(PeerId senderId, PeerId recipientId);

private:
    /// @brief Hub 与端点共享的队列状态，仅在本地测试传输生命周期内持有。
    std::shared_ptr<LoopbackTransportState> m_state;
};
}  // namespace MMM::Collaboration
