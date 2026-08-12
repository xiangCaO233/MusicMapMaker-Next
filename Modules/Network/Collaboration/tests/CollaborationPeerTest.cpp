#include "network/collaboration/CollaborationPeer.h"
#include "network/collaboration/LoopbackTransport.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
using MMM::Network::Collaboration::ByteBuffer;
using MMM::Network::Collaboration::CollaborationChatMessage;
using MMM::Network::Collaboration::CollaborationPeer;
using MMM::Network::Collaboration::CollaborationPeerConfig;
using MMM::Network::Collaboration::CommittedOperation;
using MMM::Network::Collaboration::LoopbackTransportHub;
using MMM::Network::Collaboration::ParticipantIdentity;
using MMM::Network::Collaboration::ParticipantViewport;
using MMM::Network::Collaboration::PeerId;
using MMM::Network::Collaboration::ProtocolError;
using MMM::Network::Collaboration::SubmitOperationResult;
using MMM::Network::Collaboration::SubmitChatMessageResult;
using MMM::Network::Collaboration::StateSnapshot;
using MMM::Network::Collaboration::decodeCollaborationMessage;
using MMM::Network::Collaboration::encodeCollaborationMessage;

/// @brief 测试中的客户端总数。
constexpr std::size_t PEER_COUNT = 8;
/// @brief 房主使用的固定路由槽位。
constexpr PeerId HOST_ID = 1;

/// @brief 为测试 Peer 构造固定长度且可读的稳定标识。
/// @param peerId 测试连接槽位。
/// @param discriminator 区分参与者标识和操作会话标识的十六进制字符。
/// @return 满足线上协议格式的 32 字符小写十六进制标识。
[[nodiscard]] std::string makeTestStableId(PeerId peerId, char discriminator)
{
    constexpr std::string_view DIGITS = "0123456789abcdef";
    std::string                identity(32U, '0');
    identity.front() = discriminator;
    for ( std::size_t index = identity.size(); index > 1U; --index ) {
        identity[index - 1U] = DIGITS[peerId & 0xFU];
        peerId >>= 4U;
    }
    return identity;
}

/// @brief 填充 Peer 测试所需的三层身份。
/// @param config 待填充配置。
/// @param peerId 本次连接路由槽位。
/// @param hostPeerId 房主路由槽位。
void setTestPeerIdentity(CollaborationPeerConfig& config, PeerId peerId,
                         PeerId hostPeerId = HOST_ID)
{
    config.peerId        = peerId;
    config.hostPeerId    = hostPeerId;
    config.participantId = makeTestStableId(peerId, 'a');
    config.sessionId     = makeTestStableId(peerId, 'b');
}

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
    if ( host.participantIdentities().size() != PEER_COUNT ) {
        return false;
    }
    return std::all_of(guests.begin(), guests.end(), [&](const auto& guest) {
        const auto& guestIdentities = guest->participantIdentities();
        if ( guestIdentities.size() != host.participantIdentities().size() ) {
            return false;
        }
        return std::all_of(
            host.participantIdentities().begin(),
            host.participantIdentities().end(),
            [&guestIdentities](const auto& entry) {
                const auto guestIdentity = guestIdentities.find(entry.first);
                return guestIdentity != guestIdentities.end() &&
                       guestIdentity->second.peerId == entry.second.peerId &&
                       guestIdentity->second.participantId ==
                           entry.second.participantId &&
                       guestIdentity->second.sessionId ==
                           entry.second.sessionId &&
                       guestIdentity->second.creator == entry.second.creator;
            });
    });
}

/// @brief 判断房主与全部访客是否持有一致的主画布状态表。
/// @param host 房主 Peer。
/// @param guests 七个访客 Peer。
/// @return 全部客户端都收敛到八个参与者的最新状态时返回 true。
[[nodiscard]] bool allParticipantViewportsConverged(
    const CollaborationPeer&                               host,
    const std::vector<std::unique_ptr<CollaborationPeer>>& guests)
{
    if ( host.participantViewports().size() != PEER_COUNT ) return false;
    return std::all_of(guests.begin(), guests.end(), [&](const auto& guest) {
        const auto& guestViewports = guest->participantViewports();
        if ( guestViewports.size() != PEER_COUNT ) return false;
        for ( const auto& [peerId, hostViewport] :
              host.participantViewports() ) {
            const auto viewport = guestViewports.find(peerId);
            if ( viewport == guestViewports.end() ||
                 viewport->second.sequence != hostViewport.sequence ||
                 viewport->second.playbackTime != hostViewport.playbackTime ||
                 viewport->second.horizontalOffsetRatio !=
                     hostViewport.horizontalOffsetRatio ) {
                return false;
            }
        }
        return true;
    });
}

/// @brief 覆盖 8 Peer 并发提交、重复包去重和缺失版本日志补发。
/// @return 全部断言通过时返回 true。
[[nodiscard]] bool testEightPeerIncrementalConvergence()
{
    LoopbackTransportHub                            hub;
    std::array<std::vector<ByteBuffer>, PEER_COUNT> models;

    CollaborationPeerConfig hostConfig;
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator = "Host Creator";
    hostConfig.isHost  = true;
    CollaborationPeer host(hostConfig,
                           hub.createEndpoint(HOST_ID),
                           [&models](const CommittedOperation& operation) {
                               models[0].push_back(operation.payload);
                           });
    if ( !host.isValid() ) {
        return false;
    }
    if ( host.addParticipant(99,
                             makeTestStableId(99, 'a'),
                             makeTestStableId(99, 'b'),
                             " \t") ) {
        return false;
    }

    {
        CollaborationPeerConfig invalidConfig;
        setTestPeerIdentity(invalidConfig, 99, 99);
        invalidConfig.creator = "";
        invalidConfig.isHost  = true;
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
        setTestPeerIdentity(guestConfig, peerId);
        guestConfig.creator = "Guest " + std::to_string(peerId);
        guestConfig.isHost  = false;
        auto guest          = std::make_unique<CollaborationPeer>(
            guestConfig,
            hub.createEndpoint(peerId),
            [&models, index](const CommittedOperation& operation) {
                models[index].push_back(operation.payload);
            });
        if ( !guest->isValid() ||
             !host.addParticipant(peerId,
                                  guestConfig.participantId,
                                  guestConfig.sessionId,
                                  guestConfig.creator) ) {
            return false;
        }
        guests.push_back(std::move(guest));
    }
    if ( host.addParticipant(static_cast<PeerId>(PEER_COUNT + 1),
                             makeTestStableId(PEER_COUNT + 1, 'a'),
                             makeTestStableId(PEER_COUNT + 1, 'b'),
                             "Overflow Guest") ) {
        return false;
    }
    pumpPeers(host, guests, 4);
    if ( !allCreatorIdentitiesConverged(host, guests) ) {
        return false;
    }

    ParticipantViewport hostViewport;
    hostViewport.playbackTime          = 12.5;
    hostViewport.visualTime            = 12.6;
    hostViewport.visibleTimeStart      = 9.0;
    hostViewport.visibleTimeEnd        = 15.0;
    hostViewport.horizontalOffsetRatio = 0.125;
    if ( !host.publishViewport(hostViewport) ) return false;
    for ( std::size_t index = 0; index < guests.size(); ++index ) {
        ParticipantViewport guestViewport = hostViewport;
        guestViewport.playbackTime        = 20.0 + static_cast<double>(index);
        guestViewport.visualTime          = guestViewport.playbackTime + 0.1;
        guestViewport.horizontalOffsetRatio =
            -0.05 * static_cast<double>(index + 1);
        if ( !guests[index]->publishViewport(guestViewport) ) return false;
    }
    pumpPeers(host, guests, 8);
    if ( !allParticipantViewportsConverged(host, guests) ) return false;

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
    if ( !host.participantIdentities().contains(HOST_ID) ) {
        return false;
    }

    constexpr PeerId DEPARTING_PEER_ID = PEER_COUNT;
    host.removeParticipant(DEPARTING_PEER_ID);
    pumpPeers(host, guests, 4);
    if ( host.participantIdentities().contains(DEPARTING_PEER_ID) ) {
        return false;
    }
    if ( host.participantViewports().contains(DEPARTING_PEER_ID) ) {
        return false;
    }
    for ( std::size_t index = 0; index + 1 < guests.size(); ++index ) {
        if ( guests[index]->participantIdentities().contains(
                 DEPARTING_PEER_ID) ) {
            return false;
        }
        if ( guests[index]->participantViewports().contains(
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
    request.participantId  = makeTestStableId(7, 'a');
    request.sessionId      = makeTestStableId(7, 'b');
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
         decodedRequest->participantId != request.participantId ||
         decodedRequest->sessionId != request.sessionId ||
         decodedRequest->clientSequence != request.clientSequence ||
         decodedRequest->payload != request.payload ) {
        return false;
    }

    auto oversized = encodeCollaborationMessage(request, 3);
    if ( oversized.has_value() ||
         oversized.error() != ProtocolError::OperationTooLarge ) {
        return false;
    }

    ParticipantIdentity identity{ 7,
                                  makeTestStableId(7, 'a'),
                                  makeTestStableId(7, 'b'),
                                  "  Creator Test  " };
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
    if ( decodedIdentity == nullptr || decodedIdentity->peerId != 7 ||
         decodedIdentity->participantId != identity.participantId ||
         decodedIdentity->sessionId != identity.sessionId ||
         decodedIdentity->creator != "Creator Test" ) {
        return false;
    }

    ParticipantIdentity invalidIdentity{
        7, makeTestStableId(7, 'a'), makeTestStableId(7, 'b'), " \n"
    };
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

    ParticipantViewport viewport{
        .peerId                = 7,
        .sequence              = 19,
        .playbackTime          = 18.25,
        .visualTime            = 18.35,
        .visibleTimeStart      = 15.0,
        .visibleTimeEnd        = 22.0,
        .horizontalOffsetRatio = -0.125,
    };
    const auto viewportEncoded = encodeCollaborationMessage(viewport, 4U);
    const auto viewportDecoded =
        viewportEncoded
            ? decodeCollaborationMessage(*viewportEncoded, 4U)
            : std::expected<MMM::Network::Collaboration::CollaborationMessage,
                            ProtocolError>(
                  std::unexpected(ProtocolError::InvalidViewportState));
    const auto* decodedViewport =
        viewportDecoded ? std::get_if<ParticipantViewport>(&*viewportDecoded)
                        : nullptr;
    if ( !decodedViewport || decodedViewport->peerId != viewport.peerId ||
         decodedViewport->sequence != viewport.sequence ||
         decodedViewport->playbackTime != viewport.playbackTime ||
         decodedViewport->visibleTimeStart != viewport.visibleTimeStart ||
         decodedViewport->horizontalOffsetRatio !=
             viewport.horizontalOffsetRatio ) {
        return false;
    }
    viewport.visualTime        = std::numeric_limits<double>::quiet_NaN();
    const auto invalidViewport = encodeCollaborationMessage(viewport, 4U);
    if ( invalidViewport ||
         invalidViewport.error() != ProtocolError::InvalidViewportState ) {
        return false;
    }

    CollaborationChatMessage chat{ 7, 23, "协作消息" };
    const auto               chatEncoded = encodeCollaborationMessage(chat, 4U);
    const auto               chatDecoded =
        chatEncoded
            ? decodeCollaborationMessage(*chatEncoded, 4U)
            : std::expected<MMM::Network::Collaboration::CollaborationMessage,
                            ProtocolError>(
                  std::unexpected(ProtocolError::InvalidChatMessage));
    const auto* decodedChat =
        chatDecoded ? std::get_if<CollaborationChatMessage>(&*chatDecoded)
                    : nullptr;
    if ( !decodedChat || decodedChat->peerId != chat.peerId ||
         decodedChat->sequence != chat.sequence ||
         decodedChat->text != chat.text ) {
        return false;
    }
    chat.text            = " \t";
    const auto emptyChat = encodeCollaborationMessage(chat, 4U);
    if ( emptyChat || emptyChat.error() != ProtocolError::InvalidChatMessage ) {
        return false;
    }
    chat.text.assign("\xC0\xAF", 2U);
    const auto invalidUtf8Chat = encodeCollaborationMessage(chat, 4U);
    if ( invalidUtf8Chat ||
         invalidUtf8Chat.error() != ProtocolError::InvalidChatMessage ) {
        return false;
    }

    MMM::Network::Collaboration::ResourceManifest manifest{
        0x1234U, { 0xA1U, 0xB2U, 0xC3U }
    };
    auto manifestEncoded = encodeCollaborationMessage(manifest, 4U);
    auto manifestDecoded =
        manifestEncoded
            ? decodeCollaborationMessage(*manifestEncoded, 4U)
            : std::expected<MMM::Network::Collaboration::CollaborationMessage,
                            ProtocolError>(
                  std::unexpected(ProtocolError::InvalidMessageLength));
    const auto* decodedManifest =
        manifestDecoded
            ? std::get_if<MMM::Network::Collaboration::ResourceManifest>(
                  &*manifestDecoded)
            : nullptr;
    if ( !decodedManifest ||
         decodedManifest->generation != manifest.generation ||
         decodedManifest->payload != manifest.payload ) {
        return false;
    }

    MMM::Network::Collaboration::ResourceRequest resourceRequest{
        0x1234U, 17U, 65536U, 4U
    };
    auto resourceRequestEncoded =
        encodeCollaborationMessage(resourceRequest, 4U);
    auto resourceRequestDecoded =
        resourceRequestEncoded
            ? decodeCollaborationMessage(*resourceRequestEncoded, 4U)
            : std::expected<MMM::Network::Collaboration::CollaborationMessage,
                            ProtocolError>(
                  std::unexpected(ProtocolError::InvalidMessageLength));
    const auto* decodedResourceRequest =
        resourceRequestDecoded
            ? std::get_if<MMM::Network::Collaboration::ResourceRequest>(
                  &*resourceRequestDecoded)
            : nullptr;
    if ( !decodedResourceRequest ||
         decodedResourceRequest->generation != resourceRequest.generation ||
         decodedResourceRequest->resourceIndex !=
             resourceRequest.resourceIndex ||
         decodedResourceRequest->offset != resourceRequest.offset ||
         decodedResourceRequest->requestedBytes !=
             resourceRequest.requestedBytes ) {
        return false;
    }

    MMM::Network::Collaboration::ResourceChunk resourceChunk{
        0x1234U, 17U, 65536U, { 4U, 3U, 2U, 1U }
    };
    auto resourceChunkEncoded = encodeCollaborationMessage(resourceChunk, 4U);
    auto resourceChunkDecoded =
        resourceChunkEncoded
            ? decodeCollaborationMessage(*resourceChunkEncoded, 4U)
            : std::expected<MMM::Network::Collaboration::CollaborationMessage,
                            ProtocolError>(
                  std::unexpected(ProtocolError::InvalidMessageLength));
    const auto* decodedResourceChunk =
        resourceChunkDecoded
            ? std::get_if<MMM::Network::Collaboration::ResourceChunk>(
                  &*resourceChunkDecoded)
            : nullptr;
    if ( !decodedResourceChunk ||
         decodedResourceChunk->generation != resourceChunk.generation ||
         decodedResourceChunk->resourceIndex != resourceChunk.resourceIndex ||
         decodedResourceChunk->offset != resourceChunk.offset ||
         decodedResourceChunk->payload != resourceChunk.payload ) {
        return false;
    }
    resourceChunk.payload.push_back(0U);
    const auto oversizedResource =
        encodeCollaborationMessage(resourceChunk, 4U);
    resourceRequest.requestedBytes = 5U;
    const auto oversizedRequest =
        encodeCollaborationMessage(resourceRequest, 4U);
    if ( oversizedResource || oversizedRequest ||
         oversizedResource.error() != ProtocolError::OperationTooLarge ||
         oversizedRequest.error() != ProtocolError::OperationTooLarge ) {
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

/// @brief 验证聊天消息经房主验证转发、重复包去重并拒绝身份伪造。
/// @return 三端消息记录一致且伪造消息未进入回调时返回 true。
[[nodiscard]] bool testChatRoutingAndValidation()
{
    constexpr PeerId                                     GUEST_A_ID = 2;
    constexpr PeerId                                     GUEST_B_ID = 3;
    LoopbackTransportHub                                 hub;
    std::array<std::vector<CollaborationChatMessage>, 3> messages;

    CollaborationPeerConfig hostConfig;
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator = "Host";
    hostConfig.isHost  = true;
    CollaborationPeer host(
        hostConfig,
        hub.createEndpoint(HOST_ID),
        nullptr,
        {},
        [&messages](const CollaborationChatMessage& message) {
            messages[0].push_back(message);
        });

    CollaborationPeerConfig guestAConfig;
    setTestPeerIdentity(guestAConfig, GUEST_A_ID);
    guestAConfig.creator     = "Guest A";
    guestAConfig.isHost      = false;
    auto  guestATransport    = hub.createEndpoint(GUEST_A_ID);
    auto* rawGuestATransport = guestATransport.get();
    auto  guestA             = std::make_unique<CollaborationPeer>(
        guestAConfig,
        std::move(guestATransport),
        nullptr,
        CollaborationPeer::ResourceMessageCallback{},
        [&messages](const CollaborationChatMessage& message) {
            messages[1].push_back(message);
        });

    CollaborationPeerConfig guestBConfig;
    setTestPeerIdentity(guestBConfig, GUEST_B_ID);
    guestBConfig.creator = "Guest B";
    guestBConfig.isHost  = false;
    auto guestB          = std::make_unique<CollaborationPeer>(
        guestBConfig,
        hub.createEndpoint(GUEST_B_ID),
        nullptr,
        CollaborationPeer::ResourceMessageCallback{},
        [&messages](const CollaborationChatMessage& message) {
            messages[2].push_back(message);
        });

    if ( !host.addParticipant(GUEST_A_ID,
                              guestAConfig.participantId,
                              guestAConfig.sessionId,
                              guestAConfig.creator) ||
         !host.addParticipant(GUEST_B_ID,
                              guestBConfig.participantId,
                              guestBConfig.sessionId,
                              guestBConfig.creator) ) {
        return false;
    }
    std::vector<std::unique_ptr<CollaborationPeer>> guests;
    guests.push_back(std::move(guestA));
    guests.push_back(std::move(guestB));
    pumpPeers(host, guests, 4U);

    hub.setDuplicatePackets(true);
    if ( host.submitChatMessage("房主消息") !=
             SubmitChatMessageResult::Accepted ||
         guests[0]->submitChatMessage("Guest message") !=
             SubmitChatMessageResult::Accepted ) {
        return false;
    }
    pumpPeers(host, guests, 8U);
    if ( std::any_of(messages.begin(),
                     messages.end(),
                     [](const auto& inbox) {
                         return inbox.size() != 2U ||
                                inbox[0].text != "房主消息" ||
                                inbox[1].text != "Guest message";
                     }) ||
         host.stats().duplicateChatMessages == 0U ||
         guests[0]->stats().duplicateChatMessages == 0U ) {
        return false;
    }

    hub.setDuplicatePackets(false);
    CollaborationChatMessage spoofed{ GUEST_B_ID, 99U, "spoofed" };
    const auto encodedSpoof = encodeCollaborationMessage(spoofed, 1024U);
    if ( !encodedSpoof || !rawGuestATransport->send(HOST_ID, *encodedSpoof) ) {
        return false;
    }
    const auto invalidMessagesBefore = host.stats().invalidMessages;
    pumpPeers(host, guests, 2U);
    if ( host.stats().invalidMessages != invalidMessagesBefore + 1U ||
         std::any_of(messages.begin(), messages.end(), [](const auto& inbox) {
             return inbox.size() != 2U;
         }) ) {
        return false;
    }

    return guests[0]->submitChatMessage("\n") ==
               SubmitChatMessageResult::InvalidMessage &&
           guests[1]->submitChatMessage(
               std::string(MMM::Network::Collaboration::
                                   MAX_COLLABORATION_CHAT_MESSAGE_BYTES +
                               1U,
                           'x')) == SubmitChatMessageResult::InvalidMessage;
}

/// @brief 验证资源消息只能按访客请求、房主响应的角色方向路由。
[[nodiscard]] bool testResourceMessageRouting()
{
    constexpr PeerId     GUEST_ID = 2;
    LoopbackTransportHub hub;
    std::vector<MMM::Network::Collaboration::CollaborationMessage>
        hostResources;
    std::vector<MMM::Network::Collaboration::CollaborationMessage>
        guestResources;

    CollaborationPeerConfig hostConfig;
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator = "Host";
    hostConfig.isHost  = true;
    CollaborationPeer host(
        hostConfig,
        hub.createEndpoint(HOST_ID),
        nullptr,
        [&hostResources](
            PeerId,
            const MMM::Network::Collaboration::CollaborationMessage& message) {
            hostResources.push_back(message);
        });

    CollaborationPeerConfig guestConfig;
    setTestPeerIdentity(guestConfig, GUEST_ID);
    guestConfig.creator = "Guest";
    guestConfig.isHost  = false;
    CollaborationPeer guest(
        guestConfig,
        hub.createEndpoint(GUEST_ID),
        nullptr,
        [&guestResources](
            PeerId,
            const MMM::Network::Collaboration::CollaborationMessage& message) {
            guestResources.push_back(message);
        });
    if ( !host.addParticipant(GUEST_ID,
                              guestConfig.participantId,
                              guestConfig.sessionId,
                              guestConfig.creator) ) {
        return false;
    }
    guest.update();

    const MMM::Network::Collaboration::ResourceRequest request{
        77U, 3U, 1024U, 4096U
    };
    const MMM::Network::Collaboration::ResourceManifest manifest{
        77U, { 9U, 8U, 7U }
    };
    const MMM::Network::Collaboration::ResourceChunk chunk{
        77U, 3U, 1024U, { 1U, 2U, 3U }
    };
    if ( !guest.sendResourceMessage(HOST_ID, request) ||
         guest.sendResourceMessage(HOST_ID, chunk) ||
         !host.sendResourceMessage(GUEST_ID, manifest) ||
         !host.sendResourceMessage(GUEST_ID, chunk) ||
         host.sendResourceMessage(GUEST_ID, request) ) {
        return false;
    }
    host.update();
    guest.update();
    if ( hostResources.size() != 1U || guestResources.size() != 2U ||
         !std::holds_alternative<MMM::Network::Collaboration::ResourceRequest>(
             hostResources.front()) ||
         !std::holds_alternative<MMM::Network::Collaboration::ResourceManifest>(
             guestResources[0]) ||
         !std::holds_alternative<MMM::Network::Collaboration::ResourceChunk>(
             guestResources[1]) ) {
        return false;
    }
    return true;
}

/// @brief 验证稳定协作者重连时复用 PeerId 仍按新操作会话独立去重。
/// @return 旧请求被清理且提交来源保留稳定身份和新会话时返回 true。
[[nodiscard]] bool testReusedPeerIdStartsFreshRequestSequence()
{
    constexpr PeerId                GUEST_ID = 2;
    LoopbackTransportHub            hub;
    std::vector<CommittedOperation> hostOperations;

    CollaborationPeerConfig hostConfig;
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator                     = "Host";
    hostConfig.isHost                      = true;
    hostConfig.limits.maxRequestsPerUpdate = 1;
    CollaborationPeer host(
        hostConfig,
        hub.createEndpoint(HOST_ID),
        [&hostOperations](const CommittedOperation& operation) {
            hostOperations.push_back(operation);
        });

    CollaborationPeerConfig guestConfig;
    setTestPeerIdentity(guestConfig, GUEST_ID);
    guestConfig.creator = "Guest";
    guestConfig.isHost  = false;
    auto guest          = std::make_unique<CollaborationPeer>(
        guestConfig, hub.createEndpoint(GUEST_ID), nullptr);
    if ( !host.addParticipant(GUEST_ID,
                              guestConfig.participantId,
                              guestConfig.sessionId,
                              guestConfig.creator) ) {
        return false;
    }

    const auto firstOperation  = makeOperation(GUEST_ID, 1);
    const auto queuedOperation = makeOperation(GUEST_ID, 2);
    if ( guest->submitOperation(firstOperation) !=
             SubmitOperationResult::Accepted ||
         guest->submitOperation(queuedOperation) !=
             SubmitOperationResult::Accepted ) {
        return false;
    }
    host.update();
    if ( host.appliedRevision() != 1 || hostOperations.size() != 1U ||
         hostOperations.front().payload != firstOperation ) {
        return false;
    }

    host.removeParticipant(GUEST_ID);
    guest.reset();
    guestConfig.sessionId = makeTestStableId(GUEST_ID, 'c');
    guest                 = std::make_unique<CollaborationPeer>(
        guestConfig, hub.createEndpoint(GUEST_ID), nullptr);
    if ( !host.addParticipant(GUEST_ID,
                              guestConfig.participantId,
                              guestConfig.sessionId,
                              guestConfig.creator) ) {
        return false;
    }

    const auto reconnectedOperation = makeOperation(GUEST_ID, 3);
    if ( guest->submitOperation(reconnectedOperation) !=
         SubmitOperationResult::Accepted ) {
        return false;
    }
    host.update();
    const auto identity = host.participantIdentities().find(GUEST_ID);
    return host.appliedRevision() == 2 && hostOperations.size() == 2U &&
           hostOperations.back().payload == reconnectedOperation &&
           hostOperations.back().participantId == guestConfig.participantId &&
           hostOperations.back().sessionId == guestConfig.sessionId &&
           identity != host.participantIdentities().end() &&
           identity->second.participantId == guestConfig.participantId &&
           identity->second.sessionId == guestConfig.sessionId;
}

/// @brief 验证房主拒绝同一稳定协作者或同一操作会话占用多个 PeerId。
/// @return 身份冲突均被拒绝且原身份保持不变时返回 true。
[[nodiscard]] bool testStableIdentityConflictsAreRejected()
{
    constexpr PeerId     FIRST_GUEST_ID  = 2;
    constexpr PeerId     SECOND_GUEST_ID = 3;
    LoopbackTransportHub hub;

    CollaborationPeerConfig hostConfig;
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator = "Host";
    hostConfig.isHost  = true;
    CollaborationPeer host(hostConfig, hub.createEndpoint(HOST_ID), nullptr);

    const auto participantId = makeTestStableId(FIRST_GUEST_ID, 'a');
    const auto sessionId     = makeTestStableId(FIRST_GUEST_ID, 'b');
    if ( !host.addParticipant(
             FIRST_GUEST_ID, participantId, sessionId, "First Guest") ||
         !host.addParticipant(
             FIRST_GUEST_ID, participantId, sessionId, "First Guest") ||
         host.addParticipant(SECOND_GUEST_ID,
                             participantId,
                             makeTestStableId(SECOND_GUEST_ID, 'b'),
                             "Duplicate Participant") ||
         host.addParticipant(SECOND_GUEST_ID,
                             makeTestStableId(SECOND_GUEST_ID, 'a'),
                             sessionId,
                             "Duplicate Session") ||
         host.addParticipant(FIRST_GUEST_ID,
                             makeTestStableId(SECOND_GUEST_ID, 'a'),
                             makeTestStableId(SECOND_GUEST_ID, 'b'),
                             "Changed Identity") ) {
        return false;
    }

    const auto identity = host.participantIdentities().find(FIRST_GUEST_ID);
    return host.participantIdentities().size() == 2U &&
           identity != host.participantIdentities().end() &&
           identity->second.participantId == participantId &&
           identity->second.sessionId == sessionId;
}

/// @brief 验证迟加入客户端可用房主完整快照直接越过已裁剪的增量日志。
[[nodiscard]] bool testLateJoinStateSnapshot()
{
    constexpr PeerId        GUEST_ID = 2;
    LoopbackTransportHub    hub;
    std::vector<ByteBuffer> hostModel;
    std::vector<ByteBuffer> guestModel;

    CollaborationPeerConfig hostConfig;
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator                     = "Host";
    hostConfig.isHost                      = true;
    hostConfig.limits.maxJournalOperations = 2;
    CollaborationPeer   host(hostConfig,
                             hub.createEndpoint(HOST_ID),
                             [&hostModel](const CommittedOperation& operation) {
                               hostModel.push_back(operation.payload);
                             });
    ParticipantViewport hostViewport;
    hostViewport.playbackTime          = 4.0;
    hostViewport.visualTime            = 4.1;
    hostViewport.visibleTimeStart      = 1.0;
    hostViewport.visibleTimeEnd        = 8.0;
    hostViewport.horizontalOffsetRatio = 0.2;
    if ( !host.publishViewport(hostViewport) ) return false;
    for ( std::uint8_t index = 1; index <= 5; ++index ) {
        const auto operation = makeOperation(HOST_ID, index);
        if ( host.submitOperation(operation) !=
             SubmitOperationResult::Accepted ) {
            return false;
        }
        host.update();
    }
    const ByteBuffer partialSnapshot{ 0xFA, 0xCE, 0x04 };
    const ByteBuffer fullSnapshot{ 0xFA, 0xCE, 0x05 };
    if ( host.appliedRevision() != 5 ||
         host.setStateSnapshot(6, fullSnapshot) ||
         !host.setStateSnapshot(4, partialSnapshot) ||
         host.setStateSnapshot(3, partialSnapshot) ||
         !host.setStateSnapshot(5, fullSnapshot) ) {
        return false;
    }

    CollaborationPeerConfig guestConfig;
    setTestPeerIdentity(guestConfig, GUEST_ID);
    guestConfig.creator = "Late Guest";
    guestConfig.isHost  = false;
    CollaborationPeer guest(guestConfig,
                            hub.createEndpoint(GUEST_ID),
                            [&guestModel](const CommittedOperation& operation) {
                                guestModel.push_back(operation.payload);
                            });
    if ( !host.addParticipant(GUEST_ID,
                              guestConfig.participantId,
                              guestConfig.sessionId,
                              guestConfig.creator) ) {
        return false;
    }
    for ( std::size_t round = 0; round < 4; ++round ) {
        guest.update();
        host.update();
    }
    return guest.appliedRevision() == host.appliedRevision() &&
           guest.participantViewports().contains(HOST_ID) &&
           guestModel.size() == 1U && guestModel.front() == fullSnapshot;
}

/// @brief 验证在线访客缺失的版本已经裁剪时自动回退房主最新快照。
[[nodiscard]] bool testTrimmedJournalSnapshotFallback()
{
    constexpr PeerId        GUEST_ID = 2;
    LoopbackTransportHub    hub;
    std::vector<ByteBuffer> guestModel;

    CollaborationPeerConfig hostConfig;
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator                     = "Host";
    hostConfig.isHost                      = true;
    hostConfig.limits.maxJournalOperations = 2;
    CollaborationPeer host(hostConfig, hub.createEndpoint(HOST_ID), nullptr);

    CollaborationPeerConfig guestConfig;
    setTestPeerIdentity(guestConfig, GUEST_ID);
    guestConfig.creator = "Slow Guest";
    guestConfig.isHost  = false;
    CollaborationPeer guest(guestConfig,
                            hub.createEndpoint(GUEST_ID),
                            [&guestModel](const CommittedOperation& operation) {
                                guestModel.push_back(operation.payload);
                            });
    if ( !host.addParticipant(GUEST_ID,
                              guestConfig.participantId,
                              guestConfig.sessionId,
                              guestConfig.creator) ) {
        return false;
    }
    guest.update();

    hub.dropNextPacket(HOST_ID, GUEST_ID);
    for ( std::uint8_t index = 1; index <= 5; ++index ) {
        const auto operation = makeOperation(HOST_ID, index);
        if ( host.submitOperation(operation) !=
             SubmitOperationResult::Accepted ) {
            return false;
        }
        host.update();
    }
    const ByteBuffer fullSnapshot{ 0xFA, 0xCE, 0x55 };
    if ( !host.setStateSnapshot(fullSnapshot) ) return false;

    guest.update();
    host.update();
    guest.update();
    host.update();
    return host.stats().resyncUnavailable > 0U &&
           guest.appliedRevision() == host.appliedRevision() &&
           guestModel.size() == 1U && guestModel.front() == fullSnapshot;
}
}  // namespace

/// @brief 运行协作增量同步回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    if ( !testProtocolBounds() ) return 1;
    if ( !testChatRoutingAndValidation() ) return 2;
    if ( !testResourceMessageRouting() ) return 3;
    if ( !testEightPeerIncrementalConvergence() ) return 4;
    if ( !testLateJoinStateSnapshot() ) return 5;
    if ( !testTrimmedJournalSnapshotFallback() ) return 6;
    if ( !testReusedPeerIdStartsFreshRequestSequence() ) return 7;
    if ( !testStableIdentityConflictsAreRejected() ) return 8;
    return 0;
}
