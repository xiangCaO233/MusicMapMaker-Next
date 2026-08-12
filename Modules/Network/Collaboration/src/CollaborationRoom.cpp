#include "network/collaboration/CollaborationRoom.h"

#include "network/collaboration/CollaborationBuildFingerprint.h"

#include "config/CreatorIdentity.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <concurrentqueue.h>
#include <ice/thread/ThreadPool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <utility>
#include <vector>

namespace MMM::Network::Collaboration
{
namespace
{
/// @brief 判断权限集合是否覆盖一次谱面分类变更。
[[nodiscard]] bool permissionsAllowMutation(
    CollaborationPermissionMask permissions, ::MMM::BeatmapMutationFlags flags)
{
    if ( !hasCollaborationPermission(permissions,
                                     CollaborationPermission::Edit) ) {
        return false;
    }
    const auto allows = [permissions,
                         flags](::MMM::BeatmapMutationFlags flag,
                                CollaborationPermission     permission) {
        return !hasBeatmapMutationFlag(flags, flag) ||
               hasCollaborationPermission(permissions, permission);
    };
    return allows(::MMM::BeatmapMutationFlags::Objects,
                  CollaborationPermission::Objects) &&
           allows(::MMM::BeatmapMutationFlags::Timelines,
                  CollaborationPermission::Timelines) &&
           allows(::MMM::BeatmapMutationFlags::AudioSamples,
                  CollaborationPermission::AudioSamples) &&
           allows(::MMM::BeatmapMutationFlags::Metadata,
                  CollaborationPermission::Metadata) &&
           allows(::MMM::BeatmapMutationFlags::Annotations,
                  CollaborationPermission::Annotations);
}
}  // namespace

namespace
{
/// @brief 房主使用的固定内部 PeerId。
constexpr PeerId DEFAULT_HOST_ID = 1;
/// @brief 每个房间最多保留的日志条数。
constexpr std::size_t MAX_COLLABORATION_LOG_ENTRIES = 1000;
/// @brief 当前联机会话在内存中保留的最大聊天记录数。
constexpr std::size_t MAX_COLLABORATION_CHAT_ENTRIES = 200;
/// @brief 每帧最多消费的传输生命周期事件数。
constexpr std::size_t MAX_TRANSPORT_EVENTS_PER_UPDATE = 256;
/// @brief 每帧最多向权威 Peer 提交的本地谱面操作数。
constexpr std::size_t MAX_LOCAL_OPERATIONS_PER_UPDATE = 256;
/// @brief 每帧最多向逻辑线程发布的后台谱面合并结果数。
constexpr std::size_t MAX_REMOTE_RESULTS_PER_UPDATE = 4;
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

/// @brief 隔离协作文档后台流水线的队列和任务生命周期实现。
class CollaborationRoom::RemoteOperationPipeline
{
public:
    /// @brief 后台协作文档流水线接收的一条权威提交。
    struct Task {
        /// @brief 房主已经排序的提交内容。
        CommittedOperation operation;
        /// @brief 提交者当前对应的 PeerId。
        PeerId peerId{ 0 };
        /// @brief 提交者显示身份。
        std::string creator;
        /// @brief 当前实例是否为房主，用于生成最新重同步快照。
        bool host{ false };
        /// @brief 本地待确认操作是否需要在权威文档上重放。
        bool reapplyLocalState{ false };
        /// @brief 本次提交是否由当前进程发起。
        bool originatedLocally{ false };
    };

    /// @brief 后台协作文档流水线交还 UI 线程的有界结果。
    struct Result {
        /// @brief 提交者当前对应的 PeerId。
        PeerId peerId{ 0 };
        /// @brief 提交者稳定协作标识。
        ParticipantId participantId;
        /// @brief 提交者显示身份。
        std::string creator;
        /// @brief 提交修订号。
        std::uint64_t revision{ 0 };
        /// @brief 后台合并成功后需要替换的谱面类别。
        ::MMM::BeatmapMutationFlags flags{ ::MMM::BeatmapMutationFlags::None };
        /// @brief 已重放本地待确认操作的可见谱面。
        std::shared_ptr<::MMM::BeatMap> beatmap;
        /// @brief 房主用于后续访客重同步的最新快照。
        std::optional<ByteBuffer> hostSnapshot;
        /// @brief 后台处理失败时写入协作日志的错误标识。
        std::string error;
    };

    /// @brief UI 线程向后台文档消费者投递的无锁权威提交队列。
    moodycamel::ConcurrentQueue<Task> tasks;
    /// @brief 后台文档消费者向 UI 线程发布的无锁结果队列。
    moodycamel::ConcurrentQueue<Result> results;
    /// @brief 后台消费者状态：0 为空闲、1 为运行、2 为运行且收到新唤醒。
    /// @warning UI 线程在提交任务后使用 acq_rel 更新，后台消费者在排空边界
    /// 使用 acq_rel 交接；只协调唯一消费者，不承载文档数据同步。
    std::atomic_uint8_t workerState{ 0U };
    /// @brief 已提交后台消费者任务，用于房间销毁前完成全部生命周期握手。
    std::vector<std::future<void>> workerFutures;
};

CollaborationRoom::CollaborationRoom()
    : m_nextDirectoryReconnect(std::chrono::steady_clock::now())
    , m_startedAt(std::chrono::steady_clock::now())
    , m_nextViewportPublish(std::chrono::steady_clock::now())
{
    m_remoteOperationPipeline = std::make_unique<RemoteOperationPipeline>();
    m_remoteOperationPipeline->workerFutures.reserve(4U);
}

CollaborationRoom::~CollaborationRoom()
{
    stopAcceptingLocalMutations();
    m_peer.reset();
    m_pendingTransport.reset();
    m_transport = nullptr;
    resetRemoteOperationPipeline();
}

bool CollaborationRoom::startHost(CollaborationHostRoomConfig config)
{
    if ( m_state != CollaborationRoomState::Idle ) return false;

    config.creator = Config::normalizeCreatorIdentity(config.creator);
    config.participantId =
        Config::normalizeCollaborationStableId(config.participantId);
    if ( config.buildFingerprint.empty() ) {
        config.buildFingerprint = collaborationBuildFingerprint();
    }
    if ( config.creator.empty() || config.participantId.empty() ||
         config.roomName.empty() ||
         !isValidCollaborationBuildFingerprint(config.buildFingerprint) ||
         makeCollaborationSignalingUrl(config.endpoint).empty() ) {
        fail("invalid_host_configuration");
        return false;
    }
    m_directory.disconnect();

    auto             transport = std::make_unique<WebRtcTransport>();
    WebRtcHostConfig transportConfig;
    transportConfig.endpoint         = config.endpoint;
    transportConfig.roomName         = config.roomName;
    transportConfig.creator          = config.creator;
    transportConfig.participantId    = config.participantId;
    transportConfig.sessionId        = Config::makeCollaborationStableId();
    transportConfig.hostId           = DEFAULT_HOST_ID;
    transportConfig.buildFingerprint = config.buildFingerprint;
    transportConfig.requireMatchingBuildFingerprint =
        config.requireMatchingBuildFingerprint;
    if ( !transport->startHost(transportConfig) ) {
        fail("host_signaling_start_failed");
        return false;
    }

    m_state  = CollaborationRoomState::Hosting;
    m_isHost = true;
    m_roomId.clear();
    m_roomName                        = std::move(config.roomName);
    m_serverEndpoint                  = std::move(config.endpoint);
    m_creator                         = std::move(config.creator);
    m_participantId                   = std::move(config.participantId);
    m_operationSessionId              = std::move(transportConfig.sessionId);
    m_buildFingerprint                = std::move(config.buildFingerprint);
    m_requireMatchingBuildFingerprint = config.requireMatchingBuildFingerprint;
    m_lastError.clear();
    m_startedAt       = std::chrono::steady_clock::now();
    m_nextLogSequence = 1;
    m_logs.clear();
    m_nextChatSequence = 1;
    m_chatMessages.clear();
    m_pendingLocalViewport.reset();
    m_lastPublishedLocalViewport.reset();
    m_nextViewportPublish = std::chrono::steady_clock::now();
    m_followedParticipantId.clear();
    m_pendingJoinRequests.clear();
    resetRemoteOperationPipeline();
    m_hasDocument.store(false, std::memory_order_relaxed);
    m_localPermissions.store(COLLABORATION_PERMISSION_ALL,
                             std::memory_order_release);
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
        },
        [this](const CollaborationChatMessage& message) {
            handleChatMessage(message);
        },
        [this](PeerId peerId, std::span<const std::uint8_t> payload) {
            return authorizeParticipantEdit(peerId, payload);
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
    if ( config.buildFingerprint.empty() ) {
        config.buildFingerprint = collaborationBuildFingerprint();
    }
    if ( config.creator.empty() || config.participantId.empty() ||
         config.roomId.empty() ||
         !isValidCollaborationBuildFingerprint(config.buildFingerprint) ||
         makeCollaborationSignalingUrl(config.endpoint).empty() ) {
        fail("invalid_join_configuration");
        return false;
    }
    m_directory.disconnect();

    auto              transport = std::make_unique<WebRtcTransport>();
    WebRtcGuestConfig transportConfig;
    transportConfig.endpoint         = config.endpoint;
    transportConfig.roomId           = config.roomId;
    transportConfig.creator          = config.creator;
    transportConfig.participantId    = config.participantId;
    transportConfig.sessionId        = Config::makeCollaborationStableId();
    transportConfig.hostId           = DEFAULT_HOST_ID;
    transportConfig.buildFingerprint = config.buildFingerprint;
    if ( !transport->connectToHost(transportConfig) ) {
        fail("signaling_connect_start_failed");
        return false;
    }

    m_state                           = CollaborationRoomState::Joining;
    m_isHost                          = false;
    m_roomId                          = std::move(config.roomId);
    m_roomName                        = std::move(config.roomName);
    m_serverEndpoint                  = std::move(config.endpoint);
    m_creator                         = std::move(config.creator);
    m_participantId                   = std::move(config.participantId);
    m_operationSessionId              = std::move(transportConfig.sessionId);
    m_buildFingerprint                = std::move(config.buildFingerprint);
    m_requireMatchingBuildFingerprint = true;
    m_lastError.clear();
    m_startedAt       = std::chrono::steady_clock::now();
    m_nextLogSequence = 1;
    m_logs.clear();
    m_nextChatSequence = 1;
    m_chatMessages.clear();
    m_pendingLocalViewport.reset();
    m_lastPublishedLocalViewport.reset();
    m_nextViewportPublish = std::chrono::steady_clock::now();
    m_followedParticipantId.clear();
    m_pendingJoinRequests.clear();
    resetRemoteOperationPipeline();
    m_hasDocument.store(false, std::memory_order_relaxed);
    m_localPermissions.store(0U, std::memory_order_release);
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

bool CollaborationRoom::setParticipantPermissions(
    PeerId peerId, CollaborationPermissionMask permissions)
{
    return m_peer && m_peer->setParticipantPermissions(peerId, permissions);
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
    resetRemoteOperationPipeline();
    m_resourceSync.reset();
    m_resourceManifest.reset();
    m_resourceManifestRecipients.clear();
    m_nextChatSequence = 1;
    m_chatMessages.clear();
    m_pendingLocalViewport.reset();
    m_lastPublishedLocalViewport.reset();
    m_followedParticipantId.clear();
    m_pendingJoinRequests.clear();
    m_hasDocument.store(false, std::memory_order_relaxed);
    m_localPermissions.store(0U, std::memory_order_release);
    m_initialSnapshotQueued.store(false, std::memory_order_relaxed);
}

void CollaborationRoom::update()
{
    updateDirectory();
    processRemoteOperationResults();
    processPendingPeerConnections();
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
        const auto previousPermissions =
            m_localPermissions.load(std::memory_order_acquire);
        m_peer->update();
        const auto permission =
            m_peer->participantPermissions().find(m_peer->localPeerId());
        const auto currentPermissions =
            permission == m_peer->participantPermissions().end()
                ? (m_isHost ? COLLABORATION_PERMISSION_ALL : 0U)
                : permission->second;
        m_localPermissions.store(currentPermissions, std::memory_order_release);
        if ( !m_isHost && previousPermissions != currentPermissions ) {
            std::lock_guard lock(m_localOperationMutex);
            const auto      unauthorized =
                [currentPermissions](const ByteBuffer& payload) {
                    const auto patch = BeatmapDocumentCodec::inspect(payload);
                    return !patch || !permissionsAllowMutation(
                                         currentPermissions, patch->flags);
                };
            // 权限收紧时只丢弃已经失去授权的传输增量；不触碰逻辑线程当前
            // BeatMap、未结束手势或选择状态，仍获授权的其它类别继续排队。
            std::erase_if(m_inFlightLocalOperations, unauthorized);
            std::erase_if(m_localOperationQueue, unauthorized);
        }
        submitQueuedLocalOperations();
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

SubmitChatMessageResult CollaborationRoom::sendChatMessage(std::string text)
{
    if ( !m_peer ) return SubmitChatMessageResult::InvalidPeer;
    return m_peer->submitChatMessage(std::move(text));
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

const std::unordered_map<PeerId, CollaborationPermissionMask>&
CollaborationRoom::participantPermissions() const
{
    return m_peer ? m_peer->participantPermissions() : m_emptyPermissions;
}

CollaborationPermissionMask CollaborationRoom::localPermissions() const
{
    return m_localPermissions.load(std::memory_order_acquire);
}

::MMM::BeatmapMutationFlags CollaborationRoom::localAllowedMutationFlags() const
{
    const auto permissions = localPermissions();
    if ( !hasCollaborationPermission(permissions,
                                     CollaborationPermission::Edit) ) {
        return ::MMM::BeatmapMutationFlags::None;
    }
    auto flags = ::MMM::BeatmapMutationFlags::None;
    if ( hasCollaborationPermission(permissions,
                                    CollaborationPermission::Objects) ) {
        flags |= ::MMM::BeatmapMutationFlags::Objects;
    }
    if ( hasCollaborationPermission(permissions,
                                    CollaborationPermission::Timelines) ) {
        flags |= ::MMM::BeatmapMutationFlags::Timelines;
    }
    if ( hasCollaborationPermission(permissions,
                                    CollaborationPermission::AudioSamples) ) {
        flags |= ::MMM::BeatmapMutationFlags::AudioSamples;
    }
    if ( hasCollaborationPermission(permissions,
                                    CollaborationPermission::Metadata) ) {
        flags |= ::MMM::BeatmapMutationFlags::Metadata;
    }
    if ( hasCollaborationPermission(permissions,
                                    CollaborationPermission::Annotations) ) {
        flags |= ::MMM::BeatmapMutationFlags::Annotations;
    }
    return flags;
}

bool CollaborationRoom::canLocalAnnotate() const
{
    const auto permissions = localPermissions();
    return hasCollaborationPermission(permissions,
                                      CollaborationPermission::Edit) &&
           hasCollaborationPermission(permissions,
                                      CollaborationPermission::Annotations);
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

const std::vector<CollaborationChatEntry>&
CollaborationRoom::chatMessages() const
{
    return m_chatMessages;
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

void CollaborationRoom::handleChatMessage(
    const CollaborationChatMessage& message)
{
    const auto participant = participants().find(message.peerId);
    if ( participant == participants().end() ) return;

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_startedAt);
    m_chatMessages.push_back({ m_nextChatSequence++,
                               static_cast<std::uint64_t>(
                                   std::max<std::int64_t>(0, elapsed.count())),
                               message.peerId,
                               participant->second.participantId,
                               participant->second.creator,
                               message.text });
    if ( m_chatMessages.size() > MAX_COLLABORATION_CHAT_ENTRIES ) {
        m_chatMessages.erase(
            m_chatMessages.begin(),
            m_chatMessages.begin() +
                static_cast<std::ptrdiff_t>(m_chatMessages.size() -
                                            MAX_COLLABORATION_CHAT_ENTRIES));
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
            if ( m_requireMatchingBuildFingerprint &&
                 event.buildFingerprint != m_buildFingerprint ) {
                static_cast<void>(m_transport->rejectJoinRequest(
                    event.requestId, "build_fingerprint_mismatch"));
                appendLog(CollaborationLogEventType::Error,
                          0,
                          event.creator,
                          "build_fingerprint_mismatch");
                break;
            }
            const bool known = std::any_of(
                m_pendingJoinRequests.begin(),
                m_pendingJoinRequests.end(),
                [&event](const CollaborationPendingJoinRequest& request) {
                    return request.requestId == event.requestId;
                });
            if ( !known ) {
                m_pendingJoinRequests.push_back(
                    { event.requestId, event.creator, event.buildFingerprint });
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
            if ( m_peer &&
                 m_publishedDocumentRevision < m_peer->appliedRevision() ) {
                const bool alreadyPending =
                    std::any_of(m_pendingPeerConnections.begin(),
                                m_pendingPeerConnections.end(),
                                [&event](const WebRtcTransportEvent& pending) {
                                    return pending.peerId == event.peerId;
                                });
                if ( !alreadyPending ) {
                    m_pendingPeerConnections.push_back(event);
                }
                break;
            }
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
            std::erase_if(m_pendingPeerConnections,
                          [&event](const WebRtcTransportEvent& pending) {
                              return pending.peerId == event.peerId;
                          });
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
        },
        [this](const CollaborationChatMessage& message) {
            handleChatMessage(message);
        },
        [this](PeerId peerId, std::span<const std::uint8_t> payload) {
            return authorizeParticipantEdit(peerId, payload);
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
    const std::string creator    = participant == participants().end()
                                       ? std::string{}
                                       : participant->second.creator;
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
    RemoteOperationPipeline::Task task;
    task.operation         = operation;
    task.peerId            = peerId;
    task.creator           = creator;
    task.host              = m_isHost;
    task.reapplyLocalState = reapplyLocalState;
    task.originatedLocally = originatedLocally;
    m_remoteOperationPipeline->tasks.enqueue(std::move(task));
    scheduleRemoteOperationWorker();
}

bool CollaborationRoom::authorizeParticipantEdit(
    PeerId peerId, std::span<const std::uint8_t> payload) const
{
    if ( !m_isHost || !m_peer || peerId == 0 ||
         peerId == m_peer->localPeerId() ) {
        return false;
    }
    const auto permission = m_peer->participantPermissions().find(peerId);
    if ( permission == m_peer->participantPermissions().end() ) return false;

    const auto patch = BeatmapDocumentCodec::inspect(payload);
    return patch.has_value() && !patch->isSnapshot &&
           permissionsAllowMutation(permission->second, patch->flags);
}

void CollaborationRoom::scheduleRemoteOperationWorker()
{
    while ( true ) {
        std::uint8_t state = m_remoteOperationPipeline->workerState.load(
            std::memory_order_acquire);
        if ( state == 0U ) {
            if ( m_remoteOperationPipeline->workerState.compare_exchange_weak(
                     state,
                     1U,
                     std::memory_order_acq_rel,
                     std::memory_order_acquire) ) {
                break;
            }
            continue;
        }
        if ( state == 2U ||
             m_remoteOperationPipeline->workerState.compare_exchange_weak(
                 state,
                 2U,
                 std::memory_order_acq_rel,
                 std::memory_order_acquire) ) {
            return;
        }
    }

    auto* threadPool = Runtime::AppThreadPool::instance().get();
    if ( !threadPool ) {
        XERROR("Collaboration document worker requires AppThreadPool");
        processRemoteOperations();
        return;
    }
    std::erase_if(m_remoteOperationPipeline->workerFutures,
                  [](std::future<void>& task) {
                      return task.wait_for(std::chrono::seconds(0)) ==
                             std::future_status::ready;
                  });
    m_remoteOperationPipeline->workerFutures.push_back(
        threadPool->enqueue([this]() { processRemoteOperations(); }));
}

void CollaborationRoom::processRemoteOperations()
{
    while ( true ) {
        RemoteOperationPipeline::Task task;
        while ( m_remoteOperationPipeline->tasks.try_dequeue(task) ) {
            RemoteOperationPipeline::Result result;
            result.peerId        = task.peerId;
            result.participantId = task.operation.participantId;
            result.creator       = std::move(task.creator);
            result.revision      = task.operation.revision;

            auto patch = m_documentCodec.apply(task.operation.payload);
            if ( !patch.has_value() ) {
                result.error = "invalid_beatmap_operation";
                m_remoteOperationPipeline->results.enqueue(std::move(result));
                continue;
            }
            m_hasDocument.store(true, std::memory_order_release);
            result.flags = patch->flags;

            if ( task.host ) {
                auto snapshot = m_documentCodec.encodeCurrentSnapshot();
                if ( snapshot.has_value() ) {
                    result.hostSnapshot.emplace(std::move(snapshot.value()));
                }
            }

            const bool refreshLocalObjectCommit =
                task.originatedLocally &&
                patch->flags == ::MMM::BeatmapMutationFlags::Objects;
            if ( !task.originatedLocally || task.reapplyLocalState ||
                 refreshLocalObjectCommit ) {
                result.beatmap = materializeRebasedLocalBeatmap();
                if ( !result.beatmap ) {
                    result.error = "invalid_beatmap_document";
                }
            }
            m_remoteOperationPipeline->results.enqueue(std::move(result));
        }

        std::uint8_t expected = 2U;
        if ( m_remoteOperationPipeline->workerState.compare_exchange_strong(
                 expected,
                 1U,
                 std::memory_order_acq_rel,
                 std::memory_order_acquire) ) {
            continue;
        }
        expected = 1U;
        if ( m_remoteOperationPipeline->workerState.compare_exchange_strong(
                 expected,
                 0U,
                 std::memory_order_acq_rel,
                 std::memory_order_acquire) ) {
            return;
        }
    }
}

void CollaborationRoom::processRemoteOperationResults()
{
    std::shared_ptr<::MMM::BeatMap> mergedBeatmap;
    ::MMM::BeatmapMutationFlags mergedFlags = ::MMM::BeatmapMutationFlags::None;
    for ( std::size_t index = 0; index < MAX_REMOTE_RESULTS_PER_UPDATE;
          ++index ) {
        RemoteOperationPipeline::Result result;
        if ( !m_remoteOperationPipeline->results.try_dequeue(result) ) break;

        appendLog(CollaborationLogEventType::OperationCommitted,
                  result.peerId,
                  result.creator,
                  std::to_string(result.revision),
                  result.participantId);
        if ( !result.error.empty() ) {
            appendLog(CollaborationLogEventType::Error,
                      result.peerId,
                      std::move(result.creator),
                      std::move(result.error),
                      std::move(result.participantId));
            continue;
        }
        if ( result.hostSnapshot && m_isHost && m_peer ) {
            if ( m_peer->setStateSnapshot(result.revision,
                                          std::move(*result.hostSnapshot)) ) {
                m_publishedDocumentRevision = result.revision;
            }
        }
        if ( result.beatmap ) {
            mergedFlags |= result.flags;
            mergedBeatmap = std::move(result.beatmap);
        }
    }
    if ( mergedBeatmap && m_applyBeatmapCallback ) {
        // 同帧完成的连续修订只回灌最新领域快照，并合并其真实变更类别，避免
        // 逻辑线程为中间状态重复扫描 ECS。实际应用仍由逻辑命令队列串行完成，
        // 因而本地手势、交互状态和未确认增量不会交给后台线程。
        m_applyBeatmapCallback(std::move(mergedBeatmap), mergedFlags);
    }
}

void CollaborationRoom::processPendingPeerConnections()
{
    if ( m_pendingPeerConnections.empty() || !m_peer ||
         m_publishedDocumentRevision < m_peer->appliedRevision() ) {
        return;
    }

    auto pending = std::move(m_pendingPeerConnections);
    m_pendingPeerConnections.clear();
    for ( const auto& event : pending ) {
        handleTransportEvent(event);
    }
}

void CollaborationRoom::resetRemoteOperationPipeline()
{
    for ( auto& task : m_remoteOperationPipeline->workerFutures ) {
        if ( task.valid() ) task.wait();
    }
    m_remoteOperationPipeline->workerFutures.clear();
    RemoteOperationPipeline::Task pendingTask;
    while ( m_remoteOperationPipeline->tasks.try_dequeue(pendingTask) ) {}
    RemoteOperationPipeline::Result pendingResult;
    while ( m_remoteOperationPipeline->results.try_dequeue(pendingResult) ) {}
    m_remoteOperationPipeline->workerState.store(0U, std::memory_order_release);
    m_pendingPeerConnections.clear();
    m_publishedDocumentRevision = 0;
    m_documentCodec.reset();
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
        const auto patch = BeatmapDocumentCodec::inspect(payload);
        if ( !patch || !permissionsAllowMutation(
                           m_localPermissions.load(std::memory_order_acquire),
                           patch->flags) ) {
            // 权限收紧时保留已经完成的本地编辑及其规范增量，不清空、不覆盖；
            // 房主重新授权后仍按原顺序提交。
            return;
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
