#include "network/collaboration/CollaborationRoom.h"

#include "config/CreatorIdentity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <string_view>
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

/// @brief 规范化房间码，供产品层保存和展示。
std::string normalizeRoomCode(std::string_view value)
{
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.front())) != 0 ) {
        value.remove_prefix(1);
    }
    while ( !value.empty() &&
            std::isspace(static_cast<unsigned char>(value.back())) != 0 ) {
        value.remove_suffix(1);
    }
    if ( value.size() < 4 || value.size() > 12 ) return {};

    std::string result;
    result.reserve(value.size());
    for ( const char character : value ) {
        const auto byte = static_cast<unsigned char>(character);
        if ( std::isalnum(byte) == 0 ) return {};
        result.push_back(static_cast<char>(std::toupper(byte)));
    }
    return result;
}
}  // namespace

CollaborationRoom::CollaborationRoom()
    : m_startedAt(std::chrono::steady_clock::now())
{
}

CollaborationRoom::~CollaborationRoom()
{
    m_acceptLocalMutations.store(false, std::memory_order_release);
    m_peer.reset();
    m_pendingTransport.reset();
    m_transport = nullptr;
}

bool CollaborationRoom::startHost(CollaborationHostRoomConfig config)
{
    if ( m_state != CollaborationRoomState::Idle ) return false;

    config.creator  = Config::normalizeCreatorIdentity(config.creator);
    config.roomCode = config.roomCode.empty()
                          ? generateRoomCode()
                          : normalizeRoomCode(config.roomCode);
    if ( config.creator.empty() || config.roomCode.empty() ) {
        fail("invalid_host_configuration");
        return false;
    }

    auto             transport = std::make_unique<WebRtcTransport>();
    WebRtcHostConfig transportConfig;
    transportConfig.port     = config.port;
    transportConfig.roomCode = config.roomCode;
    transportConfig.creator  = config.creator;
    transportConfig.hostId   = DEFAULT_HOST_ID;
    if ( !transport->startHost(transportConfig) ) {
        fail("host_signaling_start_failed");
        return false;
    }

    m_state    = CollaborationRoomState::Hosting;
    m_isHost   = true;
    m_roomCode = std::move(config.roomCode);
    m_hostAddress.clear();
    m_port    = transport->listeningPort();
    m_creator = std::move(config.creator);
    m_lastError.clear();
    m_startedAt       = std::chrono::steady_clock::now();
    m_nextLogSequence = 1;
    m_logs.clear();
    m_documentCodec.reset();
    m_hasDocument.store(false, std::memory_order_relaxed);
    m_initialSnapshotQueued.store(false, std::memory_order_relaxed);
    m_hostRoleForObserver.store(true, std::memory_order_relaxed);
    m_acceptLocalMutations.store(true, std::memory_order_release);
    {
        std::lock_guard lock(m_localOperationMutex);
        m_localOperationQueue.clear();
    }

    m_transport = transport.get();
    CollaborationPeerConfig peerConfig;
    peerConfig.clientId = DEFAULT_HOST_ID;
    peerConfig.hostId   = DEFAULT_HOST_ID;
    peerConfig.creator  = m_creator;
    peerConfig.isHost   = true;
    m_peer              = std::make_unique<CollaborationPeer>(
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
              std::to_string(m_port));
    return true;
}

bool CollaborationRoom::join(CollaborationJoinRoomConfig config)
{
    if ( m_state != CollaborationRoomState::Idle ) return false;

    config.creator  = Config::normalizeCreatorIdentity(config.creator);
    config.roomCode = normalizeRoomCode(config.roomCode);
    if ( config.creator.empty() || config.host.empty() || config.port == 0 ||
         config.roomCode.empty() ) {
        fail("invalid_join_configuration");
        return false;
    }

    auto              transport = std::make_unique<WebRtcTransport>();
    WebRtcGuestConfig transportConfig;
    transportConfig.host     = config.host;
    transportConfig.port     = config.port;
    transportConfig.roomCode = config.roomCode;
    transportConfig.creator  = config.creator;
    transportConfig.hostId   = DEFAULT_HOST_ID;
    if ( !transport->connectToHost(transportConfig) ) {
        fail("signaling_connect_start_failed");
        return false;
    }

    m_state       = CollaborationRoomState::Joining;
    m_isHost      = false;
    m_roomCode    = std::move(config.roomCode);
    m_hostAddress = std::move(config.host);
    m_port        = config.port;
    m_creator     = std::move(config.creator);
    m_lastError.clear();
    m_startedAt       = std::chrono::steady_clock::now();
    m_nextLogSequence = 1;
    m_logs.clear();
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
        m_localOperationQueue.clear();
    }
    m_transport        = transport.get();
    m_pendingTransport = std::move(transport);
    appendLog(CollaborationLogEventType::SignalingConnected,
              DEFAULT_HOST_ID,
              {},
              "connecting");
    return true;
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

    const bool host     = m_hostRoleForObserver.load(std::memory_order_relaxed);
    bool       snapshot = false;
    if ( host ) {
        snapshot =
            !m_initialSnapshotQueued.exchange(true, std::memory_order_relaxed);
    } else if ( !m_hasDocument.load(std::memory_order_acquire) ) {
        return;
    }

    auto payload = m_documentCodec.encode(beatmap, flags, snapshot);
    if ( !payload.has_value() ) return;

    std::lock_guard lock(m_localOperationMutex);
    if ( m_localOperationQueue.size() >= MAX_QUEUED_LOCAL_OPERATIONS ) {
        return;
    }
    m_localOperationQueue.push_back(std::move(payload.value()));
}

void CollaborationRoom::disconnect()
{
    m_acceptLocalMutations.store(false, std::memory_order_release);
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
    m_roomCode.clear();
    m_hostAddress.clear();
    m_port = 0;
    m_creator.clear();
    m_lastError.clear();
    m_documentCodec.reset();
    m_resourceSync.reset();
    m_resourceManifest.reset();
    m_resourceManifestRecipients.clear();
    m_hasDocument.store(false, std::memory_order_relaxed);
    m_initialSnapshotQueued.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_localOperationMutex);
        m_localOperationQueue.clear();
    }
}

void CollaborationRoom::update()
{
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
    }
    processResourceEvents();
}

SubmitOperationResult CollaborationRoom::submitOperation(
    std::span<const std::uint8_t> payload)
{
    if ( !m_peer ) return SubmitOperationResult::InvalidPeer;
    return m_peer->submitOperation(payload);
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

const std::string& CollaborationRoom::roomCode() const
{
    return m_roomCode;
}

const std::string& CollaborationRoom::hostAddress() const
{
    return m_hostAddress;
}

std::uint16_t CollaborationRoom::port() const
{
    return m_port;
}

PeerId CollaborationRoom::localPeerId() const
{
    if ( m_peer ) return m_peer->localPeerId();
    return m_transport ? m_transport->localPeerId() : 0;
}

const std::unordered_map<PeerId, std::string>&
CollaborationRoom::participants() const
{
    return m_peer ? m_peer->participantCreators() : m_emptyParticipants;
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

std::string CollaborationRoom::generateRoomCode()
{
    static std::atomic<std::uint64_t> sequence{ 1 };
    constexpr std::array<char, 32>    alphabet = {
        '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C',
        'D', 'E', 'F', 'G', 'H', 'J', 'K', 'M', 'N', 'P', 'Q',
        'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '0',
    };
    auto value =
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^
        sequence.fetch_add(1, std::memory_order_relaxed);
    std::string roomCode(6, '0');
    for ( char& character : roomCode ) {
        character = alphabet[value % alphabet.size()];
        value     = (value / alphabet.size()) ^ (value << 7U);
    }
    return roomCode;
}

void CollaborationRoom::appendLog(CollaborationLogEventType type, PeerId peerId,
                                  std::string creator, std::string detail)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_startedAt);
    m_logs.push_back({ m_nextLogSequence++,
                       static_cast<std::uint64_t>(
                           std::max<std::int64_t>(0, elapsed.count())),
                       type,
                       peerId,
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
    case WebRtcTransportEventType::PeerConnected:
        if ( m_isHost ) {
            if ( !m_peer ||
                 !m_peer->addParticipant(event.peerId, event.creator) ) {
                appendLog(CollaborationLogEventType::Error,
                          event.peerId,
                          event.creator,
                          "participant_registration_failed");
                break;
            }
            sendResourceManifest(event.peerId);
        } else {
            m_state = CollaborationRoomState::Connected;
        }
        appendLog(CollaborationLogEventType::ParticipantJoined,
                  event.peerId,
                  event.creator,
                  event.detail);
        break;
    case WebRtcTransportEventType::PeerDisconnected:
        if ( m_isHost && m_peer ) {
            m_peer->removeParticipant(event.peerId);
            m_resourceManifestRecipients.erase(event.peerId);
        } else if ( !m_isHost ) {
            m_state     = CollaborationRoomState::Error;
            m_lastError = event.detail;
        }
        appendLog(CollaborationLogEventType::ParticipantLeft,
                  event.peerId,
                  event.creator,
                  event.detail);
        break;
    case WebRtcTransportEventType::Rejected:
        appendLog(CollaborationLogEventType::Error,
                  event.peerId,
                  event.creator,
                  event.detail);
        if ( !m_isHost ) {
            m_state     = CollaborationRoomState::Error;
            m_lastError = event.detail;
        }
        break;
    case WebRtcTransportEventType::Error:
        appendLog(CollaborationLogEventType::Error,
                  event.peerId,
                  event.creator,
                  event.detail);
        if ( !m_isHost ) {
            m_state     = CollaborationRoomState::Error;
            m_lastError = event.detail;
        }
        break;
    }
}

void CollaborationRoom::ensureGuestPeer()
{
    if ( m_isHost || m_peer || !m_pendingTransport ) return;
    const PeerId clientId = m_pendingTransport->localPeerId();
    if ( clientId == 0 ) return;

    CollaborationPeerConfig peerConfig;
    peerConfig.clientId = clientId;
    peerConfig.hostId   = DEFAULT_HOST_ID;
    peerConfig.creator  = m_creator;
    peerConfig.isHost   = false;
    m_transport         = m_pendingTransport.get();
    m_peer              = std::make_unique<CollaborationPeer>(
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
    const auto        participant = participants().find(operation.clientId);
    const std::string creator     = participant == participants().end()
                                        ? std::string{}
                                        : participant->second;
    appendLog(CollaborationLogEventType::OperationCommitted,
              operation.clientId,
              creator,
              std::to_string(operation.revision));

    auto patch = m_documentCodec.apply(operation.payload);
    if ( !patch.has_value() ) {
        appendLog(CollaborationLogEventType::Error,
                  operation.clientId,
                  creator,
                  "invalid_beatmap_operation");
        return;
    }
    auto beatmap = m_documentCodec.materialize();
    if ( !beatmap ) {
        appendLog(CollaborationLogEventType::Error,
                  operation.clientId,
                  creator,
                  "invalid_beatmap_document");
        return;
    }

    m_hasDocument.store(true, std::memory_order_release);
    if ( m_isHost && m_peer ) {
        auto snapshot = m_documentCodec.encodeCurrentSnapshot();
        if ( snapshot.has_value() ) {
            static_cast<void>(
                m_peer->setStateSnapshot(std::move(snapshot.value())));
        }
    }
    const bool originatedLocally = operation.clientId == localPeerId();
    if ( m_applyBeatmapCallback && !originatedLocally ) {
        m_applyBeatmapCallback(std::move(beatmap), patch->flags);
    }
}

void CollaborationRoom::submitQueuedLocalOperations()
{
    for ( std::size_t index = 0; index < MAX_LOCAL_OPERATIONS_PER_UPDATE;
          ++index ) {
        ByteBuffer payload;
        {
            std::lock_guard lock(m_localOperationMutex);
            if ( m_localOperationQueue.empty() ) return;
            payload = std::move(m_localOperationQueue.front());
            m_localOperationQueue.pop_front();
        }
        const auto result = m_peer->submitOperation(payload);
        if ( result == SubmitOperationResult::Accepted ) continue;

        appendLog(CollaborationLogEventType::Error,
                  localPeerId(),
                  m_creator,
                  "local_operation_submit_failed");
        return;
    }
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
    m_state     = CollaborationRoomState::Error;
    m_lastError = message;
    appendLog(CollaborationLogEventType::Error, 0, {}, std::move(message));
}
}  // namespace MMM::Network::Collaboration
