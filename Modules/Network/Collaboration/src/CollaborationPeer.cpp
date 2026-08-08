#include "network/collaboration/CollaborationPeer.h"
#include "config/CreatorIdentity.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace MMM::Network::Collaboration
{
CollaborationPeer::CollaborationPeer(
    CollaborationPeerConfig                  config,
    std::unique_ptr<ICollaborationTransport> transport,
    ApplyOperationCallback                   applyCallback,
    ResourceMessageCallback                  resourceCallback)
    : m_config(std::move(config))
    , m_transport(std::move(transport))
    , m_applyCallback(std::move(applyCallback))
    , m_resourceCallback(std::move(resourceCallback))
{
    m_config.maxParticipants = std::clamp(m_config.maxParticipants,
                                          MIN_COLLABORATION_PARTICIPANTS,
                                          MAX_COLLABORATION_PARTICIPANTS);
    m_config.limits.maxOperationBytes =
        std::max<std::size_t>(1, m_config.limits.maxOperationBytes);
    m_config.limits.maxMessagesPerUpdate =
        std::max<std::size_t>(1, m_config.limits.maxMessagesPerUpdate);
    m_config.limits.maxRequestsPerUpdate =
        std::max<std::size_t>(1, m_config.limits.maxRequestsPerUpdate);
    m_config.limits.maxPendingRequests =
        std::max<std::size_t>(1, m_config.limits.maxPendingRequests);
    m_config.limits.maxJournalOperations =
        std::max<std::size_t>(1, m_config.limits.maxJournalOperations);
    m_config.creator = Config::normalizeCreatorIdentity(m_config.creator);

    const bool identityValid = m_config.clientId != 0 && m_config.hostId != 0 &&
                               !m_config.creator.empty();
    const bool roleValid     = m_config.isHost
                                   ? m_config.clientId == m_config.hostId
                                   : m_config.clientId != m_config.hostId;
    m_valid = identityValid && roleValid && m_transport != nullptr;
    if ( m_valid ) {
        m_participantCreators.emplace(m_config.clientId, m_config.creator);
    }
}

CollaborationPeer::~CollaborationPeer() = default;

bool CollaborationPeer::isValid() const
{
    return m_valid;
}

bool CollaborationPeer::isHost() const
{
    return m_config.isHost;
}

PeerId CollaborationPeer::localPeerId() const
{
    return m_config.clientId;
}

std::uint64_t CollaborationPeer::appliedRevision() const
{
    return m_appliedRevision;
}

const CollaborationPeerStats& CollaborationPeer::stats() const
{
    return m_stats;
}

const std::unordered_map<PeerId, std::string>&
CollaborationPeer::participantCreators() const
{
    return m_participantCreators;
}

const std::unordered_map<PeerId, ParticipantViewport>&
CollaborationPeer::participantViewports() const
{
    return m_participantViewports;
}

bool CollaborationPeer::addParticipant(PeerId peerId, std::string creator)
{
    creator = Config::normalizeCreatorIdentity(creator);
    if ( !m_valid || !m_config.isHost || peerId == 0 ||
         peerId == m_config.clientId || creator.empty() ) {
        return false;
    }
    if ( m_participants.contains(peerId) ) {
        const auto creatorIterator = m_participantCreators.find(peerId);
        return creatorIterator != m_participantCreators.end() &&
               creatorIterator->second == creator;
    }
    if ( (m_participants.size() + 1) >= m_config.maxParticipants ) {
        return false;
    }
    for ( const auto& [knownPeerId, knownCreator] : m_participantCreators ) {
        static_cast<void>(sendMessage(
            peerId, ParticipantIdentity{ knownPeerId, knownCreator }));
    }
    for ( const auto& [knownPeerId, viewport] : m_participantViewports ) {
        static_cast<void>(knownPeerId);
        static_cast<void>(sendMessage(peerId, viewport));
    }

    m_participants.insert(peerId);
    m_lastAcknowledgedRevision.try_emplace(peerId, 0);
    m_participantCreators.emplace(peerId, creator);
    const ParticipantIdentity identity{ peerId, std::move(creator) };
    for ( const PeerId participantId : m_participants ) {
        static_cast<void>(sendMessage(participantId, identity));
    }
    if ( m_stateSnapshot ) {
        static_cast<void>(sendMessage(peerId, *m_stateSnapshot));
    }
    return true;
}

bool CollaborationPeer::setStateSnapshot(ByteBuffer payload)
{
    if ( !m_valid || !m_config.isHost || m_appliedRevision == 0 ||
         payload.empty() ||
         payload.size() > m_config.limits.maxOperationBytes ) {
        return false;
    }
    m_stateSnapshot = StateSnapshot{ m_appliedRevision, std::move(payload) };
    return true;
}

void CollaborationPeer::removeParticipant(PeerId peerId)
{
    if ( !m_config.isHost || !m_participants.contains(peerId) ) {
        return;
    }
    m_participants.erase(peerId);
    m_lastAcknowledgedRevision.erase(peerId);
    m_participantViewports.erase(peerId);
    if ( m_participantCreators.erase(peerId) == 0 ) {
        return;
    }
    const ParticipantLeft participantLeft{ peerId };
    for ( const PeerId participantId : m_participants ) {
        static_cast<void>(sendMessage(participantId, participantLeft));
    }
}

SubmitOperationResult CollaborationPeer::submitOperation(
    std::span<const std::uint8_t> payload)
{
    if ( !m_valid ) {
        return SubmitOperationResult::InvalidPeer;
    }
    if ( payload.empty() ) {
        return SubmitOperationResult::EmptyOperation;
    }
    if ( payload.size() > m_config.limits.maxOperationBytes ) {
        return SubmitOperationResult::OperationTooLarge;
    }

    EditRequest request;
    request.clientId       = m_config.clientId;
    request.clientSequence = m_nextClientSequence;
    request.payload.assign(payload.begin(), payload.end());

    if ( m_config.isHost ) {
        if ( !enqueueHostRequest(std::move(request)) ) {
            return SubmitOperationResult::QueueFull;
        }
    } else if ( !sendMessage(m_config.hostId, request) ) {
        return SubmitOperationResult::TransportUnavailable;
    }

    ++m_nextClientSequence;
    return SubmitOperationResult::Accepted;
}

bool CollaborationPeer::publishViewport(ParticipantViewport viewport)
{
    if ( !m_valid || !std::isfinite(viewport.playbackTime) ||
         !std::isfinite(viewport.visualTime) ||
         !std::isfinite(viewport.visibleTimeStart) ||
         !std::isfinite(viewport.visibleTimeEnd) ||
         !std::isfinite(viewport.horizontalOffsetRatio) ) {
        return false;
    }

    viewport.clientId = m_config.clientId;
    viewport.sequence = m_nextViewportSequence++;
    m_participantViewports.insert_or_assign(m_config.clientId, viewport);
    if ( m_config.isHost ) {
        bool sent = true;
        for ( const PeerId participantId : m_participants ) {
            sent = sendMessage(participantId, viewport) && sent;
        }
        return sent;
    }
    return sendMessage(m_config.hostId, viewport);
}

bool CollaborationPeer::sendResourceMessage(PeerId recipientId,
                                            const CollaborationMessage& message)
{
    if ( !m_valid ) return false;
    if ( m_config.isHost ) {
        if ( !m_participants.contains(recipientId) ||
             (!std::holds_alternative<ResourceManifest>(message) &&
              !std::holds_alternative<ResourceChunk>(message)) ) {
            return false;
        }
    } else if ( recipientId != m_config.hostId ||
                !std::holds_alternative<ResourceRequest>(message) ) {
        return false;
    }
    return sendMessage(recipientId, message);
}

void CollaborationPeer::update()
{
    if ( !m_valid ) {
        return;
    }

    for ( std::size_t index = 0; index < m_config.limits.maxMessagesPerUpdate;
          ++index ) {
        TransportPacket packet;
        if ( !m_transport->receive(packet) ) {
            break;
        }
        auto message = decodeCollaborationMessage(
            packet.payload, m_config.limits.maxOperationBytes);
        if ( !message.has_value() ) {
            ++m_stats.invalidMessages;
            continue;
        }
        handleMessage(packet.senderId, message.value());
    }

    if ( m_config.isHost ) {
        processHostRequests();
    }
}

bool CollaborationPeer::enqueueHostRequest(EditRequest request)
{
    if ( m_pendingRequests.size() >= m_config.limits.maxPendingRequests ) {
        return false;
    }
    m_pendingRequests.push_back(std::move(request));
    return true;
}

void CollaborationPeer::handleMessage(PeerId                      senderId,
                                      const CollaborationMessage& message)
{
    if ( m_config.isHost ) {
        if ( const auto* request = std::get_if<EditRequest>(&message) ) {
            handleEditRequest(senderId, *request);
        } else if ( const auto* ack = std::get_if<RevisionAck>(&message) ) {
            handleRevisionAck(senderId, *ack);
        } else if ( const auto* resync =
                        std::get_if<ResyncRequest>(&message) ) {
            handleResyncRequest(senderId, *resync);
        } else if ( const auto* viewport =
                        std::get_if<ParticipantViewport>(&message) ) {
            handleParticipantViewport(senderId, *viewport);
        } else if ( std::holds_alternative<ResourceRequest>(message) &&
                    m_participants.contains(senderId) && m_resourceCallback ) {
            m_resourceCallback(senderId, message);
        } else {
            ++m_stats.invalidMessages;
        }
        return;
    }

    if ( const auto* committed = std::get_if<CommittedOperation>(&message) ) {
        handleCommittedOperation(senderId, *committed);
    } else if ( const auto* identity =
                    std::get_if<ParticipantIdentity>(&message) ) {
        handleParticipantIdentity(senderId, *identity);
    } else if ( const auto* participantLeft =
                    std::get_if<ParticipantLeft>(&message) ) {
        handleParticipantLeft(senderId, *participantLeft);
    } else if ( const auto* snapshot = std::get_if<StateSnapshot>(&message) ) {
        handleStateSnapshot(senderId, *snapshot);
    } else if ( const auto* viewport =
                    std::get_if<ParticipantViewport>(&message) ) {
        handleParticipantViewport(senderId, *viewport);
    } else if ( (std::holds_alternative<ResourceManifest>(message) ||
                 std::holds_alternative<ResourceChunk>(message)) &&
                senderId == m_config.hostId && m_resourceCallback ) {
        m_resourceCallback(senderId, message);
    } else {
        ++m_stats.invalidMessages;
    }
}

void CollaborationPeer::handleEditRequest(PeerId             senderId,
                                          const EditRequest& request)
{
    if ( m_participants.find(senderId) == m_participants.end() ||
         senderId != request.clientId || request.clientSequence == 0 ||
         request.payload.empty() ||
         request.payload.size() > m_config.limits.maxOperationBytes ) {
        ++m_stats.invalidMessages;
        return;
    }
    if ( !enqueueHostRequest(request) ) {
        ++m_stats.droppedRequests;
    }
}

void CollaborationPeer::handleCommittedOperation(
    PeerId senderId, const CommittedOperation& committed)
{
    if ( senderId != m_config.hostId || committed.revision == 0 ||
         committed.clientId == 0 || committed.clientSequence == 0 ||
         committed.payload.empty() ) {
        ++m_stats.invalidMessages;
        return;
    }
    if ( committed.revision <= m_appliedRevision ) {
        ++m_stats.duplicateCommits;
        return;
    }
    if ( committed.revision != m_appliedRevision + 1 ) {
        requestResync(committed.revision);
        return;
    }

    applyCommittedOperation(committed);
    if ( m_resyncTargetRevision.has_value() &&
         m_appliedRevision >= m_resyncTargetRevision.value() ) {
        m_resyncTargetRevision.reset();
    }
    sendRevisionAck();
}

void CollaborationPeer::handleRevisionAck(PeerId             senderId,
                                          const RevisionAck& ack)
{
    const auto participantIt = m_participants.find(senderId);
    if ( participantIt == m_participants.end() ||
         ack.revision > m_appliedRevision ) {
        ++m_stats.invalidMessages;
        return;
    }
    auto& acknowledged = m_lastAcknowledgedRevision[senderId];
    acknowledged       = std::max(acknowledged, ack.revision);
}

void CollaborationPeer::handleResyncRequest(PeerId               senderId,
                                            const ResyncRequest& request)
{
    if ( m_participants.find(senderId) == m_participants.end() ||
         request.fromRevision == 0 ||
         request.fromRevision > m_appliedRevision + 1 ) {
        ++m_stats.invalidMessages;
        return;
    }
    if ( request.fromRevision == m_appliedRevision + 1 ) {
        return;
    }
    if ( m_journal.empty() ||
         request.fromRevision < m_journal.front().revision ) {
        ++m_stats.resyncUnavailable;
        if ( m_stateSnapshot ) {
            static_cast<void>(sendMessage(senderId, *m_stateSnapshot));
        }
        return;
    }

    for ( const auto& committed : m_journal ) {
        if ( committed.revision >= request.fromRevision ) {
            static_cast<void>(sendMessage(senderId, committed));
        }
    }
}

void CollaborationPeer::handleParticipantIdentity(
    PeerId senderId, const ParticipantIdentity& identity)
{
    const auto creator = Config::normalizeCreatorIdentity(identity.creator);
    if ( senderId != m_config.hostId || identity.clientId == 0 ||
         creator.empty() ||
         (identity.clientId == m_config.clientId &&
          creator != m_config.creator) ) {
        ++m_stats.invalidMessages;
        return;
    }
    m_participantCreators.insert_or_assign(identity.clientId, creator);
}

void CollaborationPeer::handleParticipantLeft(
    PeerId senderId, const ParticipantLeft& participantLeft)
{
    if ( senderId != m_config.hostId || participantLeft.clientId == 0 ||
         participantLeft.clientId == m_config.hostId ||
         participantLeft.clientId == m_config.clientId ) {
        ++m_stats.invalidMessages;
        return;
    }
    m_participantCreators.erase(participantLeft.clientId);
    m_participantViewports.erase(participantLeft.clientId);
}

void CollaborationPeer::handleParticipantViewport(
    PeerId senderId, const ParticipantViewport& viewport)
{
    const bool valuesValid = viewport.clientId != 0 && viewport.sequence != 0 &&
                             std::isfinite(viewport.playbackTime) &&
                             std::isfinite(viewport.visualTime) &&
                             std::isfinite(viewport.visibleTimeStart) &&
                             std::isfinite(viewport.visibleTimeEnd) &&
                             std::isfinite(viewport.horizontalOffsetRatio);
    if ( !valuesValid ) {
        ++m_stats.invalidMessages;
        return;
    }

    if ( m_config.isHost ) {
        if ( !m_participants.contains(senderId) ||
             viewport.clientId != senderId ) {
            ++m_stats.invalidMessages;
            return;
        }
    } else if ( senderId != m_config.hostId ||
                !m_participantCreators.contains(viewport.clientId) ||
                viewport.clientId == m_config.clientId ) {
        ++m_stats.invalidMessages;
        return;
    }

    const auto existing = m_participantViewports.find(viewport.clientId);
    if ( existing != m_participantViewports.end() &&
         viewport.sequence <= existing->second.sequence ) {
        return;
    }
    m_participantViewports.insert_or_assign(viewport.clientId, viewport);

    if ( m_config.isHost ) {
        for ( const PeerId participantId : m_participants ) {
            if ( participantId != senderId ) {
                static_cast<void>(sendMessage(participantId, viewport));
            }
        }
    }
}

void CollaborationPeer::handleStateSnapshot(PeerId               senderId,
                                            const StateSnapshot& snapshot)
{
    if ( senderId != m_config.hostId || snapshot.revision == 0 ||
         snapshot.payload.empty() ||
         snapshot.payload.size() > m_config.limits.maxOperationBytes ||
         snapshot.revision < m_appliedRevision ) {
        ++m_stats.invalidMessages;
        return;
    }
    if ( snapshot.revision == m_appliedRevision ) return;

    CommittedOperation committed;
    committed.revision       = snapshot.revision;
    committed.clientId       = m_config.hostId;
    committed.clientSequence = snapshot.revision;
    committed.payload        = snapshot.payload;
    applyCommittedOperation(committed);
    m_resyncTargetRevision.reset();
    sendRevisionAck();
}

void CollaborationPeer::processHostRequests()
{
    for ( std::size_t index = 0; index < m_config.limits.maxRequestsPerUpdate &&
                                 !m_pendingRequests.empty();
          ++index ) {
        EditRequest request = std::move(m_pendingRequests.front());
        m_pendingRequests.pop_front();

        auto& lastSequence = m_lastAcceptedSequence[request.clientId];
        if ( request.clientSequence <= lastSequence ) {
            ++m_stats.duplicateRequests;
            continue;
        }
        lastSequence = request.clientSequence;

        CommittedOperation committed;
        committed.revision       = m_nextRevision++;
        committed.clientId       = request.clientId;
        committed.clientSequence = request.clientSequence;
        committed.payload        = std::move(request.payload);

        applyCommittedOperation(committed);
        m_journal.push_back(committed);
        while ( m_journal.size() > m_config.limits.maxJournalOperations ) {
            m_journal.pop_front();
        }
        broadcastCommittedOperation(committed);
    }
}

void CollaborationPeer::applyCommittedOperation(
    const CommittedOperation& committed)
{
    m_appliedRevision = committed.revision;
    if ( m_applyCallback ) {
        m_applyCallback(committed);
    }
}

bool CollaborationPeer::sendMessage(PeerId                      recipientId,
                                    const CollaborationMessage& message)
{
    auto encoded =
        encodeCollaborationMessage(message, m_config.limits.maxOperationBytes);
    if ( !encoded.has_value() ||
         !m_transport->send(recipientId, encoded.value()) ) {
        ++m_stats.sendFailures;
        return false;
    }
    return true;
}

void CollaborationPeer::broadcastCommittedOperation(
    const CommittedOperation& committed)
{
    for ( const PeerId participantId : m_participants ) {
        static_cast<void>(sendMessage(participantId, committed));
    }
}

void CollaborationPeer::sendRevisionAck()
{
    static_cast<void>(
        sendMessage(m_config.hostId, RevisionAck{ m_appliedRevision }));
}

void CollaborationPeer::requestResync(std::uint64_t observedRevision)
{
    if ( m_resyncTargetRevision.has_value() &&
         observedRevision <= m_resyncTargetRevision.value() ) {
        return;
    }
    m_resyncTargetRevision = observedRevision;
    ++m_stats.resyncRequests;
    static_cast<void>(
        sendMessage(m_config.hostId, ResyncRequest{ m_appliedRevision + 1 }));
}
}  // namespace MMM::Network::Collaboration
