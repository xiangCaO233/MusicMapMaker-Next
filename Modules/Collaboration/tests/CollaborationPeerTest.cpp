#include "collaboration/CollaborationPeer.h"
#include "collaboration/LoopbackTransport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
using MMM::Collaboration::ByteBuffer;
using MMM::Collaboration::CollaborationPeer;
using MMM::Collaboration::CollaborationPeerConfig;
using MMM::Collaboration::CommittedOperation;
using MMM::Collaboration::LoopbackTransportHub;
using MMM::Collaboration::PeerId;
using MMM::Collaboration::ProtocolError;
using MMM::Collaboration::SubmitOperationResult;
using MMM::Collaboration::decodeCollaborationMessage;
using MMM::Collaboration::encodeCollaborationMessage;

/// @brief 测试中的客户端总数。
constexpr std::size_t PEER_COUNT = 8;
/// @brief 房主使用的稳定客户端标识。
constexpr PeerId HOST_ID = 1;

/// @brief 为指定客户端和局部序号生成唯一规范化操作负载。
/// @param peerId 发起客户端标识。
/// @param localIndex 客户端内的测试操作序号。
/// @return 可直接比较的短字节负载。
[[nodiscard]] ByteBuffer makeOperation(PeerId peerId, std::uint8_t localIndex)
{
    return {
        static_cast<std::uint8_t>(peerId),
        localIndex,
        static_cast<std::uint8_t>(peerId ^ localIndex),
    };
}

/// @brief 驱动所有 Peer 的有界非阻塞 update，直到消息队列收敛。
/// @param host 房主 Peer。
/// @param guests 七个访客 Peer。
/// @param rounds 最多驱动轮数。
void pumpPeers(CollaborationPeer&                               host,
               std::vector<std::unique_ptr<CollaborationPeer>>& guests,
               std::size_t                                      rounds)
{
    for ( std::size_t round = 0; round < rounds; ++round ) {
        for ( auto& guest : guests ) {
            guest->update();
        }
        host.update();
        for ( auto& guest : guests ) {
            guest->update();
        }
        host.update();
    }
}

/// @brief 判断全部访客是否与房主模型及连续版本完全一致。
/// @param host 房主 Peer。
/// @param guests 七个访客 Peer。
/// @param models 各 Peer 按提交顺序应用的操作负载。
/// @return 全部客户端收敛时返回 true。
[[nodiscard]] bool allPeersConverged(
    const CollaborationPeer&                               host,
    const std::vector<std::unique_ptr<CollaborationPeer>>& guests,
    const std::array<std::vector<ByteBuffer>, PEER_COUNT>& models)
{
    for ( std::size_t index = 0; index < guests.size(); ++index ) {
        if ( guests[index]->appliedRevision() != host.appliedRevision() ||
             models[index + 1] != models[0] ) {
            return false;
        }
    }
    return true;
}

/// @brief 覆盖 8 Peer 并发提交、重复包去重和缺失版本日志补发。
/// @return 全部断言通过时返回 true。
[[nodiscard]] bool testEightPeerIncrementalConvergence()
{
    LoopbackTransportHub                            hub;
    std::array<std::vector<ByteBuffer>, PEER_COUNT> models;

    CollaborationPeerConfig hostConfig;
    hostConfig.clientId = HOST_ID;
    hostConfig.hostId   = HOST_ID;
    hostConfig.isHost   = true;
    CollaborationPeer host(hostConfig,
                           hub.createEndpoint(HOST_ID),
                           [&models](const CommittedOperation& operation) {
                               models[0].push_back(operation.payload);
                           });
    if ( !host.isValid() ) {
        return false;
    }

    std::vector<std::unique_ptr<CollaborationPeer>> guests;
    guests.reserve(PEER_COUNT - 1);
    for ( std::size_t index = 1; index < PEER_COUNT; ++index ) {
        const PeerId            peerId = static_cast<PeerId>(index + 1);
        CollaborationPeerConfig guestConfig;
        guestConfig.clientId = peerId;
        guestConfig.hostId   = HOST_ID;
        guestConfig.isHost   = false;
        auto guest           = std::make_unique<CollaborationPeer>(
            guestConfig,
            hub.createEndpoint(peerId),
            [&models, index](const CommittedOperation& operation) {
                models[index].push_back(operation.payload);
            });
        if ( !guest->isValid() || !host.addParticipant(peerId) ) {
            return false;
        }
        guests.push_back(std::move(guest));
    }
    if ( host.addParticipant(static_cast<PeerId>(PEER_COUNT + 1)) ) {
        return false;
    }

    hub.setDuplicatePackets(true);
    for ( std::uint8_t localIndex = 1; localIndex <= 4; ++localIndex ) {
        const ByteBuffer hostOperation = makeOperation(HOST_ID, localIndex);
        if ( host.submitOperation(hostOperation) !=
             SubmitOperationResult::Accepted ) {
            return false;
        }
        for ( std::size_t index = 0; index < guests.size(); ++index ) {
            const ByteBuffer guestOperation =
                makeOperation(static_cast<PeerId>(index + 2), localIndex);
            if ( guests[index]->submitOperation(guestOperation) !=
                 SubmitOperationResult::Accepted ) {
                return false;
            }
        }
    }
    pumpPeers(host, guests, 32);

    if ( host.appliedRevision() != 32 || host.stats().duplicateRequests == 0 ||
         !allPeersConverged(host, guests, models) ) {
        return false;
    }
    for ( const auto& guest : guests ) {
        if ( guest->stats().duplicateCommits == 0 ) {
            return false;
        }
    }

    hub.setDuplicatePackets(false);
    hub.dropNextPacket(HOST_ID, 2);
    const ByteBuffer missingOperation = makeOperation(HOST_ID, 5);
    if ( host.submitOperation(missingOperation) !=
         SubmitOperationResult::Accepted ) {
        return false;
    }
    host.update();
    guests[0]->update();

    const ByteBuffer followingOperation = makeOperation(HOST_ID, 6);
    if ( host.submitOperation(followingOperation) !=
         SubmitOperationResult::Accepted ) {
        return false;
    }
    host.update();
    guests[0]->update();
    host.update();
    guests[0]->update();
    pumpPeers(host, guests, 16);

    return guests[0]->stats().resyncRequests == 1 &&
           host.stats().resyncUnavailable == 0 &&
           host.appliedRevision() == 34 &&
           allPeersConverged(host, guests, models);
}

/// @brief 覆盖二进制帧往返、长度上限和截断消息拒绝。
/// @return 全部协议断言通过时返回 true。
[[nodiscard]] bool testProtocolBounds()
{
    MMM::Collaboration::EditRequest request;
    request.clientId       = 7;
    request.clientSequence = 11;
    request.payload        = { 1, 2, 3, 4 };

    auto encoded = encodeCollaborationMessage(request, 4);
    if ( !encoded.has_value() ) {
        return false;
    }
    auto decoded = decodeCollaborationMessage(encoded.value(), 4);
    if ( !decoded.has_value() ) {
        return false;
    }
    const auto* decodedRequest =
        std::get_if<MMM::Collaboration::EditRequest>(&decoded.value());
    if ( decodedRequest == nullptr ||
         decodedRequest->clientId != request.clientId ||
         decodedRequest->clientSequence != request.clientSequence ||
         decodedRequest->payload != request.payload ) {
        return false;
    }

    auto oversized = encodeCollaborationMessage(request, 3);
    if ( oversized.has_value() ||
         oversized.error() != ProtocolError::OperationTooLarge ) {
        return false;
    }

    ByteBuffer invalidReserved = encoded.value();
    invalidReserved[7]         = 1;
    auto reservedResult        = decodeCollaborationMessage(invalidReserved, 4);
    if ( reservedResult.has_value() ||
         reservedResult.error() != ProtocolError::InvalidReservedField ) {
        return false;
    }

    encoded->pop_back();
    auto truncated = decodeCollaborationMessage(encoded.value(), 4);
    return !truncated.has_value() &&
           truncated.error() == ProtocolError::InvalidMessageLength;
}
}  // namespace

/// @brief 运行协作增量同步回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testProtocolBounds() && testEightPeerIncrementalConvergence() ? 0
                                                                         : 1;
}
