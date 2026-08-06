#include "collaboration/CollaborationPeer.h"

#include <algorithm>
#include <utility>

namespace MMM::Collaboration
{
CollaborationPeer::CollaborationPeer(
    CollaborationPeerConfig                  config,
    std::unique_ptr<ICollaborationTransport> transport,
    ApplyOperationCallback                   applyCallback)
    : m_config(std::move(config))
    , m_transport(std::move(transport))
    , m_applyCallback(std::move(applyCallback))
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

    const bool identityValid = m_config.clientId != 0 && m_config.hostId != 0;
    const bool roleValid     = m_config.isHost
                                   ? m_config.clientId == m_config.hostId
                                   : m_config.clientId != m_config.hostId;
    m_valid = identityValid && roleValid && m_transport != nullptr;
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

std::uint64_t CollaborationPeer::appliedRevision() const
{
    return m_appliedRevision;
}

const CollaborationPeerStats& CollaborationPeer::stats() const
{
    return m_stats;
}

bool CollaborationPeer::addParticipant(PeerId peerId)
{
    if ( !m_valid || !m_config.isHost || peerId == 0 ||
         peerId == m_config.clientId ) {
        return false;
    }
    if ( m_participants.contains(peerId) ) {
        return true;
    }
    if ( (m_participants.size() + 1) >= m_config.maxParticipants ) {
        return false;
    }
    m_participants.insert(peerId);
    m_lastAcknowledgedRevision.try_emplace(peerId, 0);
    return true;
}

void CollaborationPeer::removeParticipant(PeerId peerId)
{
    if ( !m_config.isHost ) {
        return;
    }
    m_participants.erase(peerId);
    m_lastAcknowledgedRevision.erase(peerId);
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
        } else {
            ++m_stats.invalidMessages;
        }
        return;
    }

    if ( const auto* committed = std::get_if<CommittedOperation>(&message) ) {
        handleCommittedOperation(senderId, *committed);
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
        return;
    }

    for ( const auto& committed : m_journal ) {
        if ( committed.revision >= request.fromRevision ) {
            static_cast<void>(sendMessage(senderId, committed));
        }
    }
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
}  // namespace MMM::Collaboration
