#include "network/collaboration/CollaborationRoom.h"

#include "config/CreatorIdentity.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <utility>

namespace MMM::Network::Collaboration
{
namespace
{
/// @brief 房主使用的固定内部 PeerId。
constexpr PeerId DEFAULT_HOST_ID = 1;
/// @brief 每个房间最多保留的日志条数。
constexpr std::size_t MAX_COLLABORATION_LOG_ENTRIES = 1000;
/// @brief 每帧最多消费的传输生命周期事件数。
constexpr std::size_t MAX_TRANSPORT_EVENTS_PER_UPDATE = 256;
/// @brief 每帧最多向权威 Peer 提交的本地谱面操作数。
constexpr std::size_t MAX_LOCAL_OPERATIONS_PER_UPDATE = 256;
/// @brief 逻辑线程等待 UI 网络循环提交的最大操作数。
constexpr std::size_t MAX_QUEUED_LOCAL_OPERATIONS = 4096;

/// @brief 目录连接失败后的重试间隔。
constexpr auto DIRECTORY_RECONNECT_INTERVAL = std::chrono::seconds(5);
/// @brief 主画布状态允许的最低发送频率。
constexpr std::uint32_t MIN_VIEWPORT_PUBLISH_RATE_HZ = 5;
/// @brief 主画布状态允许的最高发送频率。
constexpr std::uint32_t MAX_VIEWPORT_PUBLISH_RATE_HZ = 60;

/// @brief 判断两个主画布状态是否存在值得发送的可见变化。
/// @param previous 最近一次已发布状态。
/// @param current 当前等待发布状态。
/// @return 时间变化超过 1ms 或横向偏移变化超过万分之一时返回 true。
[[nodiscard]] bool viewportChanged(const ParticipantViewport& previous,
                                   const ParticipantViewport& current)
{
    constexpr double TIME_EPSILON   = 0.001;
    constexpr double OFFSET_EPSILON = 0.0001;
    return std::abs(previous.playbackTime - current.playbackTime) >
               TIME_EPSILON ||
           std::abs(previous.visualTime - current.visualTime) > TIME_EPSILON ||
           std::abs(previous.visibleTimeStart - current.visibleTimeStart) >
               TIME_EPSILON ||
           std::abs(previous.visibleTimeEnd - current.visibleTimeEnd) >
               TIME_EPSILON ||
           std::abs(previous.horizontalOffsetRatio -
                    current.horizontalOffsetRatio) > OFFSET_EPSILON;
}
}  // namespace

CollaborationRoom::CollaborationRoom()
    : m_nextDirectoryReconnect(std::chrono::steady_clock::now())
    , m_startedAt(std::chrono::steady_clock::now())
    , m_nextViewportPublish(std::chrono::steady_clock::now())
{
}

CollaborationRoom::~CollaborationRoom()
{
    stopAcceptingLocalMutations();
    m_peer.reset();
    m_pendingTransport.reset();
    m_transport = nullptr;
}

bool CollaborationRoom::startHost(CollaborationHostRoomConfig config)
{
    if ( m_state != CollaborationRoomState::Idle ) return false;

    config.creator = Config::normalizeCreatorIdentity(config.creator);
    config.participantId =
        Config::normalizeCollaborationStableId(config.participantId);
    if ( config.creator.empty() || config.participantId.empty() ||
         config.roomName.empty() ||
         makeCollaborationSignalingUrl(config.endpoint).empty() ) {
        fail("invalid_host_configuration");
        return false;
    }
    m_directory.disconnect();

    auto             transport = std::make_unique<WebRtcTransport>();
    WebRtcHostConfig transportConfig;
    transportConfig.endpoint      = config.endpoint;
    transportConfig.roomName      = config.roomName;
    transportConfig.creator       = config.creator;
    transportConfig.participantId = config.participantId;
    transportConfig.sessionId     = Config::makeCollaborationStableId();
    transportConfig.hostId        = DEFAULT_HOST_ID;
    if ( !transport->startHost(transportConfig) ) {
        fail("host_signaling_start_failed");
        return false;
    }

    m_state  = CollaborationRoomState::Hosting;
    m_isHost = true;
    m_roomId.clear();
    m_roomName           = std::move(config.roomName);
    m_serverEndpoint     = std::move(config.endpoint);
    m_creator            = std::move(config.creator);
    m_participantId      = std::move(config.participantId);
    m_operationSessionId = std::move(transportConfig.sessionId);
    m_lastError.clear();
    m_startedAt       = std::chrono::steady_clock::now();
    m_nextLogSequence = 1;
    m_logs.clear();
    m_pendingLocalViewport.reset();
    m_lastPublishedLocalViewport.reset();
    m_nextViewportPublish = std::chrono::steady_clock::now();
    m_followedParticipantId.clear();
    m_pendingJoinRequests.clear();
    m_documentCodec.reset();
    m_hasDocument.store(false, std::memory_order_relaxed);
    m_initialSnapshotQueued.store(false, std::memory_order_relaxed);
    m_hostRoleForObserver.store(true, std::memory_order_relaxed);
    m_acceptLocalMutations.store(true, std::memory_order_release);
    {
        std::lock_guard lock(m_localOperationMutex);
        m_localMutationCodec.reset();
        m_localOperationQueue.clear();
        m_inFlightLocalOperations.clear();
        m_localOperationSubmitBlocked = false;
        m_localStateNeedsRebase       = false;
    }

    m_transport = transport.get();
    CollaborationPeerConfig peerConfig;
    peerConfig.peerId        = DEFAULT_HOST_ID;
    peerConfig.hostPeerId    = DEFAULT_HOST_ID;
    peerConfig.participantId = m_participantId;
    peerConfig.sessionId     = m_operationSessionId;
    peerConfig.creator       = m_creator;
    peerConfig.isHost        = true;
    m_peer                   = std::make_unique<CollaborationPeer>(
        std::move(peerConfig),
        std::move(transport),
        [this](const CommittedOperation& operation) {
            handleCommittedOperation(operation);
        },
        [this](PeerId senderId, const CollaborationMessage& message) {
            handleResourceMessage(senderId, message);
        });
    if ( !m_peer->isValid() ) {
        fail("host_peer_create_failed");
        m_peer.reset();
        m_transport = nullptr;
        return false;
    }

    appendLog(CollaborationLogEventType::RoomStarted,
              DEFAULT_HOST_ID,
              m_creator,
              "publishing");
    return true;
}

bool CollaborationRoom::join(CollaborationJoinRoomConfig config)
{
    if ( m_state != CollaborationRoomState::Idle ) return false;

    config.creator = Config::normalizeCreatorIdentity(config.creator);
    config.participantId =
        Config::normalizeCollaborationStableId(config.participantId);
    if ( config.creator.empty() || config.participantId.empty() ||
         config.roomId.empty() ||
         makeCollaborationSignalingUrl(config.endpoint).empty() ) {
        fail("invalid_join_configuration");
        return false;
    }
    m_directory.disconnect();

    auto              transport = std::make_unique<WebRtcTransport>();
    WebRtcGuestConfig transportConfig;
    transportConfig.endpoint      = config.endpoint;
    transportConfig.roomId        = config.roomId;
    transportConfig.creator       = config.creator;
    transportConfig.participantId = config.participantId;
    transportConfig.sessionId     = Config::makeCollaborationStableId();
    transportConfig.hostId        = DEFAULT_HOST_ID;
    if ( !transport->connectToHost(transportConfig) ) {
        fail("signaling_connect_start_failed");
        return false;
    }

    m_state              = CollaborationRoomState::Joining;
    m_isHost             = false;
    m_roomId             = std::move(config.roomId);
    m_roomName           = std::move(config.roomName);
    m_serverEndpoint     = std::move(config.endpoint);
    m_creator            = std::move(config.creator);
    m_participantId      = std::move(config.participantId);
    m_operationSessionId = std::move(transportConfig.sessionId);
    m_lastError.clear();
    m_startedAt       = std::chrono::steady_clock::now();
    m_nextLogSequence = 1;
    m_logs.clear();
    m_pendingLocalViewport.reset();
    m_lastPublishedLocalViewport.reset();
    m_nextViewportPublish = std::chrono::steady_clock::now();
    m_followedParticipantId.clear();
    m_pendingJoinRequests.clear();
    m_documentCodec.reset();
    m_hasDocument.store(false, std::memory_order_relaxed);
    m_initialSnapshotQueued.store(false, std::memory_order_relaxed);
    m_hostRoleForObserver.store(false, std::memory_order_relaxed);
    m_acceptLocalMutations.store(true, std::memory_order_release);
    m_resourceSync.startGuest(std::move(config.resourceCacheRoot));
    m_resourceManifest.reset();
    m_resourceManifestRecipients.clear();
    {
        std::lock_guard lock(m_localOperationMutex);
        m_localMutationCodec.reset();
        m_localOperationQueue.clear();
        m_inFlightLocalOperations.clear();
        m_localOperationSubmitBlocked = false;
        m_localStateNeedsRebase       = false;
    }
    m_transport        = transport.get();
    m_pendingTransport = std::move(transport);
    appendLog(CollaborationLogEventType::SignalingConnected,
              DEFAULT_HOST_ID,
              {},
              "connecting");
    return true;
}

bool CollaborationRoom::approveJoinRequest(std::string_view requestId)
{
    if ( !m_isHost || !m_transport || requestId.empty() ) return false;
    const auto request = std::find_if(
        m_pendingJoinRequests.begin(),
        m_pendingJoinRequests.end(),
        [requestId](const CollaborationPendingJoinRequest& candidate) {
            return candidate.requestId == requestId;
        });
    if ( request == m_pendingJoinRequests.end() ||
         !m_transport->approveJoinRequest(requestId) ) {
        return false;
    }
    appendLog(CollaborationLogEventType::SignalingConnected,
              0,
              request->creator,
              "join_approved");
    m_pendingJoinRequests.erase(request);
    return true;
}

bool CollaborationRoom::rejectJoinRequest(std::string_view requestId)
{
    if ( !m_isHost || !m_transport || requestId.empty() ) return false;
    const auto request = std::find_if(
        m_pendingJoinRequests.begin(),
        m_pendingJoinRequests.end(),
        [requestId](const CollaborationPendingJoinRequest& candidate) {
            return candidate.requestId == requestId;
        });
    if ( request == m_pendingJoinRequests.end() ||
         !m_transport->rejectJoinRequest(requestId) ) {
        return false;
    }
    appendLog(CollaborationLogEventType::Disconnected,
              0,
              request->creator,
              "join_rejected");
    m_pendingJoinRequests.erase(request);
    return true;
}

bool CollaborationRoom::removeParticipant(PeerId peerId)
{
    if ( !m_isHost || !m_transport || !m_peer || peerId == 0 ||
         peerId == localPeerId() || !participants().contains(peerId) ) {
        return false;
    }
    return m_transport->disconnectPeer(peerId, "removed_by_host");
}

bool CollaborationRoom::setServerEndpoint(CollaborationServerEndpoint endpoint)
{
    if ( isActive() || makeCollaborationSignalingUrl(endpoint).empty() ) {
        return false;
    }
    if ( endpoint == m_serverEndpoint ) return true;
    m_serverEndpoint = std::move(endpoint);
    m_directory.disconnect();
    m_directoryError.clear();
    m_nextDirectoryReconnect = std::chrono::steady_clock::now();
    return true;
}

bool CollaborationRoom::refreshDirectory()
{
    return m_directory.refresh();
}

void CollaborationRoom::setApplyBeatmapCallback(ApplyBeatmapCallback callback)
{
    m_applyBeatmapCallback = std::move(callback);
}

void CollaborationRoom::setResourceBundleCallback(
    ResourceBundleCallback callback)
{
    m_resourceBundleCallback = std::move(callback);
}

void CollaborationRoom::prepareHostResources(const ::MMM::Project& project,
                                             const ::MMM::BeatMap& beatmap)
{
    m_resourceManifest.reset();
    m_resourceManifestRecipients.clear();
    m_resourceSync.startHost(project, beatmap);
}

void CollaborationRoom::onBeatmapMutated(const ::MMM::BeatMap&       beatmap,
                                         ::MMM::BeatmapMutationFlags flags)
{
    if ( !m_acceptLocalMutations.load(std::memory_order_acquire) ) return;

    const bool host = m_hostRoleForObserver.load(std::memory_order_relaxed);
    if ( !host && !m_hasDocument.load(std::memory_order_acquire) ) {
        return;
    }

    std::lock_guard lock(m_localOperationMutex);
    if ( !m_acceptLocalMutations.load(std::memory_order_acquire) ) return;
    if ( m_localOperationQueue.size() >= MAX_QUEUED_LOCAL_OPERATIONS ) {
        return;
    }
    const bool snapshot =
        host && !m_initialSnapshotQueued.load(std::memory_order_relaxed);
    auto payload = m_localMutationCodec.encode(beatmap, flags, snapshot);
    if ( !payload.has_value() ) return;

    m_localOperationQueue.push_back(std::move(payload.value()));
    if ( snapshot ) {
        m_initialSnapshotQueued.store(true, std::memory_order_relaxed);
    }
}

void CollaborationRoom::onBeatmapSynchronized(const ::MMM::BeatMap& beatmap)
{
    if ( !m_acceptLocalMutations.load(std::memory_order_acquire) ) return;
    std::lock_guard lock(m_localOperationMutex);
    if ( !m_acceptLocalMutations.load(std::memory_order_acquire) ) return;
    m_localMutationCodec.synchronizeEncodingBaseline(beatmap);
    if ( !m_localOperationQueue.empty() ||
         !m_inFlightLocalOperations.empty() ) {
        m_localStateNeedsRebase = true;
    }
}

void CollaborationRoom::disconnect()
{
    stopAcceptingLocalMutations();
    if ( m_state != CollaborationRoomState::Idle ) {
        appendLog(CollaborationLogEventType::Disconnected,
                  localPeerId(),
                  m_creator,
                  "local_disconnect");
    }
    m_peer.reset();
    m_pendingTransport.reset();
    m_transport = nullptr;
    m_state     = CollaborationRoomState::Idle;
    m_isHost    = false;
    m_roomId.clear();
    m_roomName.clear();
    m_creator.clear();
    m_participantId.clear();
    m_operationSessionId.clear();
    m_lastError.clear();
    m_documentCodec.reset();
    m_resourceSync.reset();
    m_resourceManifest.reset();
    m_resourceManifestRecipients.clear();
    m_pendingLocalViewport.reset();
    m_lastPublishedLocalViewport.reset();
    m_followedParticipantId.clear();
    m_pendingJoinRequests.clear();
    m_hasDocument.store(false, std::memory_order_relaxed);
    m_initialSnapshotQueued.store(false, std::memory_order_relaxed);
}

void CollaborationRoom::update()
{
    updateDirectory();
    if ( !m_transport ) return;

    ensureGuestPeer();
    for ( std::size_t index = 0; index < MAX_TRANSPORT_EVENTS_PER_UPDATE;
          ++index ) {
        WebRtcTransportEvent event;
        if ( !m_transport->receiveEvent(event) ) break;
        handleTransportEvent(event);
    }
    ensureGuestPeer();
    if ( m_peer ) {
        submitQueuedLocalOperations();
        m_peer->update();
        flushLocalViewport();
    }
    processResourceEvents();
}

SubmitOperationResult CollaborationRoom::submitOperation(
    std::span<const std::uint8_t> payload)
{
    if ( !m_peer ) return SubmitOperationResult::InvalidPeer;
    return m_peer->submitOperation(payload);
}

void CollaborationRoom::publishLocalViewport(ParticipantViewport viewport)
{
    if ( !m_peer || !std::isfinite(viewport.playbackTime) ||
         !std::isfinite(viewport.visualTime) ||
         !std::isfinite(viewport.visibleTimeStart) ||
         !std::isfinite(viewport.visibleTimeEnd) ||
         !std::isfinite(viewport.horizontalOffsetRatio) ) {
        return;
    }
    viewport.peerId        = 0;
    viewport.sequence      = 0;
    m_pendingLocalViewport = viewport;
}

void CollaborationRoom::setViewportPublishRateHz(std::uint32_t rateHz)
{
    m_viewportPublishRateHz = std::clamp(
        rateHz, MIN_VIEWPORT_PUBLISH_RATE_HZ, MAX_VIEWPORT_PUBLISH_RATE_HZ);
    m_nextViewportPublish = std::chrono::steady_clock::now();
}

bool CollaborationRoom::setFollowedPeer(PeerId peerId)
{
    if ( peerId == 0 ) {
        m_followedParticipantId.clear();
        return true;
    }
    if ( !m_peer || peerId == m_peer->localPeerId() ||
         !m_peer->participantIdentities().contains(peerId) ) {
        return false;
    }
    m_followedParticipantId =
        m_peer->participantIdentities().at(peerId).participantId;
    return true;
}

CollaborationRoomState CollaborationRoom::state() const
{
    return m_state;
}

bool CollaborationRoom::isHost() const
{
    return m_isHost;
}

bool CollaborationRoom::isActive() const
{
    return m_state != CollaborationRoomState::Idle;
}

const std::string& CollaborationRoom::roomId() const
{
    return m_roomId;
}

const std::string& CollaborationRoom::roomName() const
{
    return m_roomName;
}

const CollaborationServerEndpoint& CollaborationRoom::serverEndpoint() const
{
    return m_serverEndpoint;
}

CollaborationDirectoryState CollaborationRoom::directoryState() const
{
    return m_directory.state();
}

const std::vector<CollaborationDirectoryRoom>&
CollaborationRoom::directoryRooms() const
{
    return m_directory.rooms();
}

const std::string& CollaborationRoom::directoryError() const
{
    return m_directoryError;
}

PeerId CollaborationRoom::localPeerId() const
{
    if ( m_peer ) return m_peer->localPeerId();
    return m_transport ? m_transport->localPeerId() : 0;
}

const std::unordered_map<PeerId, ParticipantIdentity>&
CollaborationRoom::participants() const
{
    return m_peer ? m_peer->participantIdentities() : m_emptyParticipants;
}

const std::vector<CollaborationPendingJoinRequest>&
CollaborationRoom::pendingJoinRequests() const
{
    return m_pendingJoinRequests;
}

const std::unordered_map<PeerId, ParticipantViewport>&
CollaborationRoom::participantViewports() const
{
    return m_peer ? m_peer->participantViewports() : m_emptyViewports;
}

PeerId CollaborationRoom::followedPeerId() const
{
    if ( !m_peer || m_followedParticipantId.empty() ) return 0;
    const auto& identities  = m_peer->participantIdentities();
    const auto  participant = std::find_if(
        identities.begin(), identities.end(), [this](const auto& entry) {
            return entry.second.participantId == m_followedParticipantId;
        });
    return participant == identities.end() ? 0 : participant->first;
}

std::uint32_t CollaborationRoom::viewportPublishRateHz() const
{
    return m_viewportPublishRateHz;
}

const std::vector<CollaborationLogEntry>& CollaborationRoom::logs() const
{
    return m_logs;
}

const std::string& CollaborationRoom::lastError() const
{
    return m_lastError;
}

CollaborationResourceSyncProgress CollaborationRoom::resourceProgress() const
{
    return m_resourceSync.progress();
}

void CollaborationRoom::updateDirectory()
{
    if ( isActive() ) {
        if ( m_directory.state() != CollaborationDirectoryState::Idle ) {
            m_directory.disconnect();
        }
        return;
    }
    m_directory.update();
    const auto state = m_directory.state();
    if ( state == CollaborationDirectoryState::Connected ||
         state == CollaborationDirectoryState::Connecting ) {
        if ( state == CollaborationDirectoryState::Connected ) {
            m_directoryError.clear();
        }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if ( now < m_nextDirectoryReconnect ) return;
    if ( state == CollaborationDirectoryState::Error ) {
        m_directoryError = m_directory.lastError();
        m_directory.disconnect();
    }
    static_cast<void>(m_directory.connect(m_serverEndpoint));
    m_nextDirectoryReconnect = now + DIRECTORY_RECONNECT_INTERVAL;
}

void CollaborationRoom::appendLog(CollaborationLogEventType type, PeerId peerId,
                                  std::string creator, std::string detail,
                                  ParticipantId participantId)
{
    if ( participantId.empty() ) {
        const auto participant = participants().find(peerId);
        if ( participant != participants().end() ) {
            participantId = participant->second.participantId;
        } else if ( peerId != 0 && peerId == localPeerId() ) {
            participantId = m_participantId;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_startedAt);
    m_logs.push_back({ m_nextLogSequence++,
                       static_cast<std::uint64_t>(
                           std::max<std::int64_t>(0, elapsed.count())),
                       type,
                       peerId,
                       std::move(participantId),
                       std::move(creator),
                       std::move(detail) });
    if ( m_logs.size() > MAX_COLLABORATION_LOG_ENTRIES ) {
        m_logs.erase(m_logs.begin(),
                     m_logs.begin() +
                         static_cast<std::ptrdiff_t>(
                             m_logs.size() - MAX_COLLABORATION_LOG_ENTRIES));
    }
}

void CollaborationRoom::handleTransportEvent(const WebRtcTransportEvent& event)
{
    switch ( event.type ) {
    case WebRtcTransportEventType::SignalingConnected:
        appendLog(CollaborationLogEventType::SignalingConnected,
                  event.peerId,
                  event.creator,
                  event.detail);
        break;
    case WebRtcTransportEventType::RoomPublished:
        m_roomId = event.detail;
        appendLog(CollaborationLogEventType::SignalingConnected,
                  event.peerId,
                  event.creator,
                  "room_published");
        break;
    case WebRtcTransportEventType::JoinPending:
        if ( !m_isHost ) {
            m_state = CollaborationRoomState::AwaitingApproval;
            appendLog(CollaborationLogEventType::SignalingConnected,
                      event.peerId,
                      event.creator,
                      event.detail);
        }
        break;
    case WebRtcTransportEventType::JoinRequested:
        if ( m_isHost && !event.requestId.empty() ) {
            const bool known = std::any_of(
                m_pendingJoinRequests.begin(),
                m_pendingJoinRequests.end(),
                [&event](const CollaborationPendingJoinRequest& request) {
                    return request.requestId == event.requestId;
                });
            if ( !known ) {
                m_pendingJoinRequests.push_back(
                    { event.requestId, event.creator });
                appendLog(CollaborationLogEventType::SignalingConnected,
                          0,
                          event.creator,
                          event.detail);
            }
        }
        break;
    case WebRtcTransportEventType::JoinCancelled:
        if ( m_isHost ) {
            std::erase_if(
                m_pendingJoinRequests,
                [&event](const CollaborationPendingJoinRequest& request) {
                    return request.requestId == event.requestId;
                });
            appendLog(CollaborationLogEventType::Disconnected,
                      0,
                      event.creator,
                      event.detail);
        }
        break;
    case WebRtcTransportEventType::PeerConnected:
        if ( m_isHost ) {
            if ( !m_peer || !m_peer->addParticipant(event.peerId,
                                                    event.participantId,
                                                    event.sessionId,
                                                    event.creator) ) {
                appendLog(CollaborationLogEventType::Error,
                          event.peerId,
                          event.creator,
                          "participant_registration_failed");
                if ( m_transport ) {
                    static_cast<void>(m_transport->disconnectPeer(
                        event.peerId, "participant_registration_failed"));
                }
                break;
            }
            sendResourceManifest(event.peerId);
        } else {
            m_state = CollaborationRoomState::Connected;
        }
        appendLog(CollaborationLogEventType::ParticipantJoined,
                  event.peerId,
                  event.creator,
                  event.detail,
                  event.participantId);
        break;
    case WebRtcTransportEventType::PeerDisconnected:
        if ( m_isHost && m_peer ) {
            m_peer->removeParticipant(event.peerId);
            m_resourceManifestRecipients.erase(event.peerId);
        } else if ( !m_isHost ) {
            m_state     = CollaborationRoomState::Error;
            m_lastError = event.detail;
            stopAcceptingLocalMutations();
        }
        appendLog(CollaborationLogEventType::ParticipantLeft,
                  event.peerId,
                  event.creator,
                  event.detail,
                  event.participantId);
        break;
    case WebRtcTransportEventType::Rejected:
        appendLog(CollaborationLogEventType::Error,
                  event.peerId,
                  event.creator,
                  event.detail);
        if ( !m_isHost ) {
            m_state     = CollaborationRoomState::Error;
            m_lastError = event.detail;
            stopAcceptingLocalMutations();
        }
        break;
    case WebRtcTransportEventType::Error:
        appendLog(CollaborationLogEventType::Error,
                  event.peerId,
                  event.creator,
                  event.detail);
        if ( !m_isHost || event.detail == "room_directory_connection_closed" ) {
            m_state     = CollaborationRoomState::Error;
            m_lastError = event.detail;
            if ( !m_isHost ) stopAcceptingLocalMutations();
        }
        break;
    }
}

void CollaborationRoom::ensureGuestPeer()
{
    if ( m_isHost || m_peer || !m_pendingTransport ) return;
    const PeerId peerId = m_pendingTransport->localPeerId();
    if ( peerId == 0 ) return;

    CollaborationPeerConfig peerConfig;
    peerConfig.peerId        = peerId;
    peerConfig.hostPeerId    = DEFAULT_HOST_ID;
    peerConfig.participantId = m_participantId;
    peerConfig.sessionId     = m_operationSessionId;
    peerConfig.creator       = m_creator;
    peerConfig.isHost        = false;
    m_transport              = m_pendingTransport.get();
    m_peer                   = std::make_unique<CollaborationPeer>(
        std::move(peerConfig),
        std::move(m_pendingTransport),
        [this](const CommittedOperation& operation) {
            handleCommittedOperation(operation);
        },
        [this](PeerId senderId, const CollaborationMessage& message) {
            handleResourceMessage(senderId, message);
        });
    if ( !m_peer->isValid() ) {
        fail("guest_peer_create_failed");
        m_peer.reset();
        m_transport = nullptr;
    }
}

void CollaborationRoom::handleCommittedOperation(
    const CommittedOperation& operation)
{
    const auto participant = std::find_if(
        participants().begin(),
        participants().end(),
        [&operation](const auto& entry) {
            return entry.second.participantId == operation.participantId;
        });
    const PeerId peerId =
        participant == participants().end() ? 0 : participant->first;
    const std::string creator = participant == participants().end()
                                    ? std::string{}
                                    : participant->second.creator;
    appendLog(CollaborationLogEventType::OperationCommitted,
              peerId,
              creator,
              std::to_string(operation.revision),
              operation.participantId);

    auto patch = m_documentCodec.apply(operation.payload);
    if ( !patch.has_value() ) {
        appendLog(CollaborationLogEventType::Error,
                  peerId,
                  creator,
                  "invalid_beatmap_operation",
                  operation.participantId);
        return;
    }
    m_hasDocument.store(true, std::memory_order_release);
    const bool originatedLocally = operation.participantId == m_participantId &&
                                   operation.sessionId == m_operationSessionId;
    bool       reapplyLocalState = false;
    if ( originatedLocally ) {
        std::lock_guard lock(m_localOperationMutex);
        const auto      matching = std::find(m_inFlightLocalOperations.begin(),
                                             m_inFlightLocalOperations.end(),
                                             operation.payload);
        if ( matching != m_inFlightLocalOperations.end() ) {
            m_inFlightLocalOperations.erase(matching);
        }
        reapplyLocalState       = m_localStateNeedsRebase;
        m_localStateNeedsRebase = false;
    }
    if ( m_isHost && m_peer ) {
        auto snapshot = m_documentCodec.encodeCurrentSnapshot();
        if ( snapshot.has_value() ) {
            static_cast<void>(
                m_peer->setStateSnapshot(std::move(snapshot.value())));
        }
    }
    const bool refreshLocalObjectCommit =
        originatedLocally &&
        patch->flags == ::MMM::BeatmapMutationFlags::Objects;
    if ( m_applyBeatmapCallback && (!originatedLocally || reapplyLocalState ||
                                    refreshLocalObjectCommit) ) {
        auto beatmap = materializeRebasedLocalBeatmap();
        if ( !beatmap ) {
            appendLog(CollaborationLogEventType::Error,
                      peerId,
                      creator,
                      "invalid_beatmap_document",
                      operation.participantId);
            return;
        }
        m_applyBeatmapCallback(std::move(beatmap),
                               reapplyLocalState
                                   ? ::MMM::BeatmapMutationFlags::All
                                   : patch->flags);
    }
}

void CollaborationRoom::submitQueuedLocalOperations()
{
    if ( !m_peer || !m_acceptLocalMutations.load(std::memory_order_acquire) ) {
        return;
    }
    for ( std::size_t index = 0; index < MAX_LOCAL_OPERATIONS_PER_UPDATE;
          ++index ) {
        ByteBuffer payload;
        {
            std::lock_guard lock(m_localOperationMutex);
            if ( m_localOperationQueue.empty() ) return;
            payload = m_localOperationQueue.front();
        }
        const auto result = m_peer->submitOperation(payload);
        if ( result == SubmitOperationResult::Accepted ) {
            std::lock_guard lock(m_localOperationMutex);
            if ( !m_localOperationQueue.empty() &&
                 m_localOperationQueue.front() == payload ) {
                m_inFlightLocalOperations.push_back(
                    std::move(m_localOperationQueue.front()));
                m_localOperationQueue.pop_front();
            }
            m_localOperationSubmitBlocked = false;
            continue;
        }

        if ( !m_localOperationSubmitBlocked ) {
            appendLog(CollaborationLogEventType::Error,
                      localPeerId(),
                      m_creator,
                      "local_operation_submit_failed");
            m_localOperationSubmitBlocked = true;
        }
        return;
    }
}

std::shared_ptr<::MMM::BeatMap>
CollaborationRoom::materializeRebasedLocalBeatmap()
{
    auto snapshot = m_documentCodec.encodeCurrentSnapshot();
    if ( !snapshot.has_value() ) return nullptr;

    std::vector<ByteBuffer> pending;
    {
        std::lock_guard lock(m_localOperationMutex);
        pending.reserve(m_inFlightLocalOperations.size() +
                        m_localOperationQueue.size());
        pending.insert(pending.end(),
                       m_inFlightLocalOperations.begin(),
                       m_inFlightLocalOperations.end());
        pending.insert(pending.end(),
                       m_localOperationQueue.begin(),
                       m_localOperationQueue.end());
    }

    BeatmapDocumentCodec localView;
    if ( !localView.apply(*snapshot).has_value() ) return nullptr;
    for ( const auto& payload : pending ) {
        if ( !localView.apply(payload).has_value() ) return nullptr;
    }
    return localView.materialize();
}

void CollaborationRoom::stopAcceptingLocalMutations()
{
    m_acceptLocalMutations.store(false, std::memory_order_release);
    std::lock_guard lock(m_localOperationMutex);
    m_localOperationQueue.clear();
    m_inFlightLocalOperations.clear();
    m_localOperationSubmitBlocked = false;
    m_localStateNeedsRebase       = false;
}

void CollaborationRoom::flushLocalViewport()
{
    if ( !m_peer || !m_pendingLocalViewport ) return;
    if ( m_lastPublishedLocalViewport &&
         !viewportChanged(*m_lastPublishedLocalViewport,
                          *m_pendingLocalViewport) ) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if ( now < m_nextViewportPublish ) return;

    ParticipantViewport viewport = *m_pendingLocalViewport;
    if ( !m_peer->publishViewport(viewport) ) return;
    m_lastPublishedLocalViewport = viewport;
    const auto interval          = std::chrono::microseconds(
        1'000'000 / std::max<std::uint32_t>(1, m_viewportPublishRateHz));
    m_nextViewportPublish = now + interval;
}

void CollaborationRoom::handleResourceMessage(
    PeerId senderId, const CollaborationMessage& message)
{
    if ( const auto* manifest = std::get_if<ResourceManifest>(&message) ) {
        m_resourceSync.receiveManifest(*manifest);
    } else if ( const auto* request = std::get_if<ResourceRequest>(&message) ) {
        m_resourceSync.receiveRequest(senderId, *request);
    } else if ( const auto* chunk = std::get_if<ResourceChunk>(&message) ) {
        m_resourceSync.receiveChunk(*chunk);
    }
}

void CollaborationRoom::processResourceEvents()
{
    constexpr std::size_t MAX_RESOURCE_EVENTS_PER_UPDATE = 256U;
    for ( std::size_t index = 0; index < MAX_RESOURCE_EVENTS_PER_UPDATE;
          ++index ) {
        CollaborationResourceSyncEvent event;
        if ( !m_resourceSync.pollEvent(event) ) return;
        switch ( event.type ) {
        case CollaborationResourceSyncEvent::Type::ManifestReady: {
            const auto* manifest =
                std::get_if<ResourceManifest>(&event.message);
            if ( !manifest ) break;
            m_resourceManifest = *manifest;
            for ( const auto& [peerId, creator] : participants() ) {
                static_cast<void>(creator);
                if ( peerId != localPeerId() ) sendResourceManifest(peerId);
            }
            appendLog(CollaborationLogEventType::ResourceManifest,
                      localPeerId(),
                      m_creator,
                      event.detail);
            break;
        }
        case CollaborationResourceSyncEvent::Type::SendRequest:
            if ( !m_peer || !m_peer->sendResourceMessage(DEFAULT_HOST_ID,
                                                         event.message) ) {
                m_lastError = "resource_request_send_failed";
                appendLog(CollaborationLogEventType::Error,
                          localPeerId(),
                          m_creator,
                          m_lastError);
            }
            break;
        case CollaborationResourceSyncEvent::Type::SendChunk:
            if ( !m_peer ||
                 !m_peer->sendResourceMessage(event.peerId, event.message) ) {
                m_lastError = "resource_chunk_send_failed";
                appendLog(CollaborationLogEventType::Error,
                          event.peerId,
                          {},
                          m_lastError);
            }
            break;
        case CollaborationResourceSyncEvent::Type::BundleReady:
            appendLog(CollaborationLogEventType::ResourceCompleted,
                      localPeerId(),
                      m_creator,
                      event.detail);
            if ( m_resourceBundleCallback ) {
                m_resourceBundleCallback(std::move(event.bundle));
            }
            break;
        case CollaborationResourceSyncEvent::Type::Error:
            m_lastError = event.detail;
            appendLog(CollaborationLogEventType::Error,
                      localPeerId(),
                      m_creator,
                      event.detail);
            break;
        }
    }
}

void CollaborationRoom::sendResourceManifest(PeerId peerId)
{
    if ( !m_peer || !m_isHost || !m_resourceManifest ||
         m_resourceManifestRecipients.contains(peerId) ) {
        return;
    }
    if ( m_peer->sendResourceMessage(peerId, *m_resourceManifest) ) {
        m_resourceManifestRecipients.insert(peerId);
    }
}

void CollaborationRoom::fail(std::string message)
{
    stopAcceptingLocalMutations();
    m_pendingJoinRequests.clear();
    m_state     = CollaborationRoomState::Error;
    m_lastError = message;
    appendLog(CollaborationLogEventType::Error, 0, {}, std::move(message));
}
}  // namespace MMM::Network::Collaboration
