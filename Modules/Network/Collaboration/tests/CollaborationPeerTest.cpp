#include "network/collaboration/CollaborationPeer.h"
#include "network/collaboration/LoopbackTransport.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
using MMM::Network::Collaboration::ByteBuffer;
using MMM::Network::Collaboration::CollaborationPeer;
using MMM::Network::Collaboration::CollaborationPeerConfig;
using MMM::Network::Collaboration::CommittedOperation;
using MMM::Network::Collaboration::LoopbackTransportHub;
using MMM::Network::Collaboration::ParticipantIdentity;
using MMM::Network::Collaboration::PeerId;
using MMM::Network::Collaboration::ProtocolError;
using MMM::Network::Collaboration::SubmitOperationResult;
using MMM::Network::Collaboration::StateSnapshot;
using MMM::Network::Collaboration::decodeCollaborationMessage;
using MMM::Network::Collaboration::encodeCollaborationMessage;

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

/// @brief 判断房主与全部访客是否持有一致的 Creator 身份表。
/// @param host 房主 Peer。
/// @param guests 七个访客 Peer。
/// @return 全部身份映射与房主一致时返回 true。
[[nodiscard]] bool allCreatorIdentitiesConverged(
    const CollaborationPeer&                               host,
    const std::vector<std::unique_ptr<CollaborationPeer>>& guests)
{
    if ( host.participantCreators().size() != PEER_COUNT ) {
        return false;
    }
    return std::all_of(guests.begin(), guests.end(), [&](const auto& guest) {
        return guest->participantCreators() == host.participantCreators();
    });
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
    hostConfig.creator  = "Host Creator";
    hostConfig.isHost   = true;
    CollaborationPeer host(hostConfig,
                           hub.createEndpoint(HOST_ID),
                           [&models](const CommittedOperation& operation) {
                               models[0].push_back(operation.payload);
                           });
    if ( !host.isValid() ) {
        return false;
    }
    if ( host.addParticipant(99, " \t") ) {
        return false;
    }

    {
        CollaborationPeerConfig invalidConfig;
        invalidConfig.clientId = 99;
        invalidConfig.hostId   = 99;
        invalidConfig.creator  = "";
        invalidConfig.isHost   = true;
        CollaborationPeer invalidPeer(
            invalidConfig, hub.createEndpoint(99), nullptr);
        if ( invalidPeer.isValid() ) {
            return false;
        }
    }

    std::vector<std::unique_ptr<CollaborationPeer>> guests;
    guests.reserve(PEER_COUNT - 1);
    for ( std::size_t index = 1; index < PEER_COUNT; ++index ) {
        const PeerId            peerId = static_cast<PeerId>(index + 1);
        CollaborationPeerConfig guestConfig;
        guestConfig.clientId = peerId;
        guestConfig.hostId   = HOST_ID;
        guestConfig.creator  = "Guest " + std::to_string(peerId);
        guestConfig.isHost   = false;
        auto guest           = std::make_unique<CollaborationPeer>(
            guestConfig,
            hub.createEndpoint(peerId),
            [&models, index](const CommittedOperation& operation) {
                models[index].push_back(operation.payload);
            });
        if ( !guest->isValid() ||
             !host.addParticipant(peerId, guestConfig.creator) ) {
            return false;
        }
        guests.push_back(std::move(guest));
    }
    if ( host.addParticipant(static_cast<PeerId>(PEER_COUNT + 1),
                             "Overflow Guest") ) {
        return false;
    }
    pumpPeers(host, guests, 4);
    if ( !allCreatorIdentitiesConverged(host, guests) ) {
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

    if ( guests[0]->stats().resyncRequests != 1 ||
         host.stats().resyncUnavailable != 0 || host.appliedRevision() != 34 ||
         !allPeersConverged(host, guests, models) ) {
        return false;
    }

    host.removeParticipant(HOST_ID);
    if ( !host.participantCreators().contains(HOST_ID) ) {
        return false;
    }

    constexpr PeerId DEPARTING_PEER_ID = PEER_COUNT;
    host.removeParticipant(DEPARTING_PEER_ID);
    pumpPeers(host, guests, 4);
    if ( host.participantCreators().contains(DEPARTING_PEER_ID) ) {
        return false;
    }
    for ( std::size_t index = 0; index + 1 < guests.size(); ++index ) {
        if ( guests[index]->participantCreators().contains(
                 DEPARTING_PEER_ID) ) {
            return false;
        }
    }
    return true;
}

/// @brief 覆盖二进制帧往返、长度上限和截断消息拒绝。
/// @return 全部协议断言通过时返回 true。
[[nodiscard]] bool testProtocolBounds()
{
    MMM::Network::Collaboration::EditRequest request;
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
        std::get_if<MMM::Network::Collaboration::EditRequest>(&decoded.value());
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

    ParticipantIdentity identity{ 7, "  Creator Test  " };
    auto identityEncoded = encodeCollaborationMessage(identity, 4);
    if ( !identityEncoded.has_value() ) {
        return false;
    }
    auto identityDecoded =
        decodeCollaborationMessage(identityEncoded.value(), 4);
    const auto* decodedIdentity =
        identityDecoded.has_value()
            ? std::get_if<ParticipantIdentity>(&identityDecoded.value())
            : nullptr;
    if ( decodedIdentity == nullptr || decodedIdentity->clientId != 7 ||
         decodedIdentity->creator != "Creator Test" ) {
        return false;
    }

    ParticipantIdentity invalidIdentity{ 7, " \n" };
    auto invalidIdentityResult = encodeCollaborationMessage(invalidIdentity, 4);
    if ( invalidIdentityResult.has_value() ||
         invalidIdentityResult.error() !=
             ProtocolError::InvalidCreatorIdentity ) {
        return false;
    }

    StateSnapshot snapshot{ 42, { 9, 8, 7, 6 } };
    auto          snapshotEncoded = encodeCollaborationMessage(snapshot, 4);
    if ( !snapshotEncoded.has_value() ) return false;
    auto snapshotDecoded =
        decodeCollaborationMessage(snapshotEncoded.value(), 4);
    const auto* decodedSnapshot =
        snapshotDecoded.has_value()
            ? std::get_if<StateSnapshot>(&snapshotDecoded.value())
            : nullptr;
    if ( decodedSnapshot == nullptr ||
         decodedSnapshot->revision != snapshot.revision ||
         decodedSnapshot->payload != snapshot.payload ) {
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

/// @brief 验证迟加入客户端可用房主完整快照直接越过已裁剪的增量日志。
[[nodiscard]] bool testLateJoinStateSnapshot()
{
    constexpr PeerId        GUEST_ID = 2;
    LoopbackTransportHub    hub;
    std::vector<ByteBuffer> hostModel;
    std::vector<ByteBuffer> guestModel;

    CollaborationPeerConfig hostConfig;
    hostConfig.clientId                    = HOST_ID;
    hostConfig.hostId                      = HOST_ID;
    hostConfig.creator                     = "Host";
    hostConfig.isHost                      = true;
    hostConfig.limits.maxJournalOperations = 2;
    CollaborationPeer host(hostConfig,
                           hub.createEndpoint(HOST_ID),
                           [&hostModel](const CommittedOperation& operation) {
                               hostModel.push_back(operation.payload);
                           });
    for ( std::uint8_t index = 1; index <= 5; ++index ) {
        const auto operation = makeOperation(HOST_ID, index);
        if ( host.submitOperation(operation) !=
             SubmitOperationResult::Accepted ) {
            return false;
        }
        host.update();
    }
    const ByteBuffer fullSnapshot{ 0xFA, 0xCE, 0x05 };
    if ( host.appliedRevision() != 5 || !host.setStateSnapshot(fullSnapshot) ) {
        return false;
    }

    CollaborationPeerConfig guestConfig;
    guestConfig.clientId = GUEST_ID;
    guestConfig.hostId   = HOST_ID;
    guestConfig.creator  = "Late Guest";
    guestConfig.isHost   = false;
    CollaborationPeer guest(guestConfig,
                            hub.createEndpoint(GUEST_ID),
                            [&guestModel](const CommittedOperation& operation) {
                                guestModel.push_back(operation.payload);
                            });
    if ( !host.addParticipant(GUEST_ID, guestConfig.creator) ) return false;
    for ( std::size_t round = 0; round < 4; ++round ) {
        guest.update();
        host.update();
    }
    return guest.appliedRevision() == host.appliedRevision() &&
           guestModel.size() == 1U && guestModel.front() == fullSnapshot;
}
}  // namespace

/// @brief 运行协作增量同步回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testProtocolBounds() && testEightPeerIncrementalConvergence() &&
                   testLateJoinStateSnapshot()
               ? 0
               : 1;
}
