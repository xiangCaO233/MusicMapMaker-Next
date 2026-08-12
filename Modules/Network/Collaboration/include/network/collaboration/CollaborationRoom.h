#pragma once

#include "network/collaboration/BeatmapDocumentCodec.h"
#include "network/collaboration/CollaborationDirectoryClient.h"
#include "network/collaboration/CollaborationPeer.h"
#include "network/collaboration/CollaborationResourceSync.h"
#include "network/collaboration/WebRtcTransport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMM::Network::Collaboration
{
/// @brief 产品层协作房间生命周期状态。
enum class CollaborationRoomState {
    Idle,
    Hosting,
    Joining,
    AwaitingApproval,
    Connected,
    Error,
};

/// @brief 协作日志中的事件分类。
enum class CollaborationLogEventType {
    RoomStarted,
    SignalingConnected,
    ParticipantJoined,
    ParticipantLeft,
    OperationCommitted,
    ResourceManifest,
    ResourceCompleted,
    Disconnected,
    Error,
};

/// @brief 一条可由 UI 实时展示的结构化协作日志。
struct CollaborationLogEntry {
    /// @brief 房间内严格递增的日志序号。
    std::uint64_t sequence = 0;
    /// @brief 从本次房间启动起经过的毫秒数。
    std::uint64_t elapsedMilliseconds = 0;
    /// @brief 日志事件类型。
    CollaborationLogEventType type = CollaborationLogEventType::Error;
    /// @brief 事件关联 PeerId。
    PeerId peerId = 0;
    /// @brief 事件关联且不随 PeerId 复用变化的稳定协作者标识。
    ParticipantId participantId;
    /// @brief 事件关联 Creator。
    std::string creator;
    /// @brief 事件附加详情。
    std::string detail;
};

/// @brief 当前联机会话内一条可由 UI 展示的文字聊天记录。
struct CollaborationChatEntry {
    /// @brief 当前客户端接收记录时分配的严格递增序号。
    std::uint64_t sequence = 0;
    /// @brief 从本次房间启动起经过的毫秒数。
    std::uint64_t elapsedMilliseconds = 0;
    /// @brief 原始发送者的临时路由槽位。
    PeerId peerId = 0;
    /// @brief 原始发送者的稳定协作者标识。
    ParticipantId participantId;
    /// @brief 原始发送者的 Creator 展示身份。
    std::string creator;
    /// @brief 已通过协议校验的单行 UTF-8 正文。
    std::string text;
};

/// @brief 房主创建房间所需的产品层参数。
struct CollaborationHostRoomConfig {
    /// @brief 房主 Creator。
    std::string creator;
    /// @brief 房主应用配置持久化的稳定协作者标识。
    ParticipantId participantId;
    /// @brief 公网目录展示的房间名称。
    std::string roomName;
    /// @brief 中心服务器地址、信令端口与 TLS 配置。
    CollaborationServerEndpoint endpoint;
};

/// @brief 访客加入房间所需的产品层参数。
struct CollaborationJoinRoomConfig {
    /// @brief 访客 Creator。
    std::string creator;
    /// @brief 访客应用配置持久化的稳定协作者标识。
    ParticipantId participantId;
    /// @brief 从公网目录选择的房间标识。
    std::string roomId;
    /// @brief 目录中显示的房间名称。
    std::string roomName;
    /// @brief 中心服务器地址、信令端口与 TLS 配置。
    CollaborationServerEndpoint endpoint;
    /// @brief 访客内容寻址资源缓存根目录。
    std::filesystem::path resourceCacheRoot;
};

/// @brief 等待房主决定的访客加入请求。
struct CollaborationPendingJoinRequest {
    /// @brief 中心服务生成的短期请求标识。
    std::string requestId;
    /// @brief 访客提交并经过规范化的 Creator 展示身份。
    std::string creator;
};

/// @brief 协调 WebRTC 传输、房主权威 Peer 与实时协作日志。
class CollaborationRoom : public ::MMM::IBeatmapMutationObserver
{
public:
    /// @brief 把房主已排序的谱面状态回灌到当前本地会话。
    using ApplyBeatmapCallback = std::function<void(
        std::shared_ptr<::MMM::BeatMap>, ::MMM::BeatmapMutationFlags)>;
    /// @brief 访客资源完整校验后绑定到协作会话的入口。
    using ResourceBundleCallback =
        std::function<void(CollaborationResourceBundle)>;

    /// @brief 创建离线房间控制器。
    CollaborationRoom();
    /// @brief 断开房间并释放网络资源。
    ~CollaborationRoom();

    CollaborationRoom(const CollaborationRoom&)            = delete;
    CollaborationRoom& operator=(const CollaborationRoom&) = delete;
    CollaborationRoom(CollaborationRoom&&)                 = delete;
    CollaborationRoom& operator=(CollaborationRoom&&)      = delete;

    /// @brief 启动房主房间。
    /// @param config 房主参数。
    /// @return 信令服务成功启动时返回 true。
    [[nodiscard]] bool startHost(CollaborationHostRoomConfig config);

    /// @brief 连接房主房间。
    /// @param config 访客参数。
    /// @return 成功开始连接时返回 true。
    [[nodiscard]] bool join(CollaborationJoinRoomConfig config);

    /// @brief 房主批准一个等待中的访客加入请求。
    /// @param requestId 待批准请求标识。
    /// @return 当前为房主、请求仍有效且 P2P 信令开始建立时返回 true。
    [[nodiscard]] bool approveJoinRequest(std::string_view requestId);

    /// @brief 房主拒绝一个等待中的访客加入请求。
    /// @param requestId 待拒绝请求标识。
    /// @return 当前为房主且拒绝指令成功发出时返回 true。
    [[nodiscard]] bool rejectJoinRequest(std::string_view requestId);

    /// @brief 房主将一个已连接访客移出当前房间。
    /// @param peerId 待移出的非房主 PeerId。
    /// @return 目标是当前在线访客且已开始关闭 P2P 连接时返回 true。
    [[nodiscard]] bool removeParticipant(PeerId peerId);

    /// @brief 设置离线房间浏览使用的中心服务器。
    /// @param endpoint 地址、信令端口和 TLS 配置。
    /// @return 当前未加入房间且配置有效时返回 true。
    [[nodiscard]] bool setServerEndpoint(CollaborationServerEndpoint endpoint);

    /// @brief 主动请求刷新公网房间目录。
    /// @return 目录连接可用且请求发送成功时返回 true。
    [[nodiscard]] bool refreshDirectory();

    /// @brief 设置远端提交谱面数据的本地逻辑命令入口。
    /// @param callback UI 线程调用的非阻塞回灌函数。
    void setApplyBeatmapCallback(ApplyBeatmapCallback callback);

    /// @brief 设置访客资源完成回调。
    /// @param callback UI 线程消费的资源包回调。
    void setResourceBundleCallback(ResourceBundleCallback callback);

    /// @brief
    /// 异步准备当前谱面引用的项目资源；房主换谱时可再次调用并下发新清单。
    /// @param project 当前房主项目。
    /// @param beatmap 当前房间谱面。
    void prepareHostResources(const ::MMM::Project& project,
                              const ::MMM::BeatMap& beatmap);

    /// @brief 接收逻辑线程已经物化的本地谱面变化并排队发送。
    /// @warning 逻辑线程低频编辑分支调用；只执行内存编码和有界入队，
    /// 不直接访问 WebRTC 或等待 UI 线程。
    void onBeatmapMutated(const ::MMM::BeatMap&       beatmap,
                          ::MMM::BeatmapMutationFlags flags) override;

    /// @brief 接收逻辑线程已经合并远端提交后的本地编码基线。
    /// @warning 逻辑线程低频远端提交路径调用；仅执行内存编码并获取短期互斥锁。
    void onBeatmapSynchronized(const ::MMM::BeatMap& beatmap) override;

    /// @brief 主动离开并回到离线状态。
    void disconnect();

    /// @brief 驱动连接事件与协作协议消息。
    /// @warning UI 热路径：每帧调用一次，只执行有界非阻塞队列轮询；禁止加入
    /// 文件系统访问、等待或完整谱面遍历。
    void update();

    /// @brief 提交一条规范化增量操作。
    /// @param payload 与 UI/ECS 无关的操作负载。
    /// @return 当前未完成连接时返回 InvalidPeer。
    [[nodiscard]] SubmitOperationResult submitOperation(
        std::span<const std::uint8_t> payload);

    /// @brief 向当前房间发送一条文字聊天消息。
    /// @param text 单行 UTF-8 正文。
    /// @return 房间未连接或消息不合法时返回对应失败原因。
    [[nodiscard]] SubmitChatMessageResult sendChatMessage(std::string text);

    /// @brief 更新等待节流发布的本地主画布视口状态。
    /// @param viewport 当前主画布的时间范围和横向偏移比例。
    /// @warning UI 热路径：主画布每帧调用；只覆盖一个固定大小内存状态，
    /// 实际网络发送由 update 按固定频率执行。
    void publishLocalViewport(ParticipantViewport viewport);

    /// @brief 设置 P2P 主画布状态的本地发送频率。
    /// @param rateHz 目标频率；房间层限制到 5～60 Hz。
    void setViewportPublishRateHz(std::uint32_t rateHz);

    /// @brief 设置用户列表选择的持续跟随目标。
    /// @param peerId 远端参与者标识；传入 0 停止跟随。
    /// @return 目标是当前在线的非本机参与者或成功停止时返回 true。
    bool setFollowedPeer(PeerId peerId);

    /// @brief 获取当前房间状态。
    [[nodiscard]] CollaborationRoomState state() const;
    /// @brief 查询当前客户端是否为房主。
    [[nodiscard]] bool isHost() const;
    /// @brief 查询是否处于房间生命周期中。
    [[nodiscard]] bool isActive() const;
    /// @brief 获取当前公开房间标识。
    [[nodiscard]] const std::string& roomId() const;
    /// @brief 获取当前公开房间名称。
    [[nodiscard]] const std::string& roomName() const;
    /// @brief 获取当前中心服务器配置。
    [[nodiscard]] const CollaborationServerEndpoint& serverEndpoint() const;
    /// @brief 获取公网目录连接状态。
    [[nodiscard]] CollaborationDirectoryState directoryState() const;
    /// @brief 获取最近一次在线房间列表。
    [[nodiscard]] const std::vector<CollaborationDirectoryRoom>&
    directoryRooms() const;
    /// @brief 获取最近目录连接错误。
    [[nodiscard]] const std::string& directoryError() const;
    /// @brief 获取当前客户端 PeerId。
    [[nodiscard]] PeerId localPeerId() const;
    /// @brief 获取当前已知参与者的稳定身份表。
    [[nodiscard]] const std::unordered_map<PeerId, ParticipantIdentity>&
    participants() const;
    /// @brief 获取等待房主批准或拒绝的访客请求。
    [[nodiscard]] const std::vector<CollaborationPendingJoinRequest>&
    pendingJoinRequests() const;
    /// @brief 获取已同步的各参与者主画布视口状态。
    /// @return 以 PeerId 索引的最新只读状态表。
    [[nodiscard]] const std::unordered_map<PeerId, ParticipantViewport>&
    participantViewports() const;
    /// @brief 获取用户列表当前选择的持续跟随目标。
    /// @return 远端 PeerId；未跟随时为 0。
    [[nodiscard]] PeerId followedPeerId() const;
    /// @brief 获取当前 P2P 主画布状态发送频率。
    /// @return 5～60 Hz 范围内的本地发送频率。
    [[nodiscard]] std::uint32_t viewportPublishRateHz() const;
    /// @brief 获取实时协作日志。
    [[nodiscard]] const std::vector<CollaborationLogEntry>& logs() const;
    /// @brief 获取当前联机会话内的有界文字聊天记录。
    [[nodiscard]] const std::vector<CollaborationChatEntry>&
    chatMessages() const;
    /// @brief 获取最近错误；没有错误时为空。
    [[nodiscard]] const std::string& lastError() const;
    /// @brief 获取资源同步进度快照。
    [[nodiscard]] CollaborationResourceSyncProgress resourceProgress() const;

private:
    /// @brief 隔离无锁队列与后台任务生命周期的私有实现。
    class RemoteOperationPipeline;

    /// @brief 驱动目录订阅并在断线后低频重连。
    /// @warning UI 热路径：只处理有界事件队列，重连受时间节流。
    void updateDirectory();
    /// @brief 记录结构化协作日志。
    void appendLog(CollaborationLogEventType type, PeerId peerId,
                   std::string creator, std::string detail,
                   ParticipantId participantId = {});
    /// @brief 处理传输层生命周期事件。
    void handleTransportEvent(const WebRtcTransportEvent& event);
    /// @brief 在访客取得 PeerId 后创建协作状态机。
    void ensureGuestPeer();
    /// @brief 将一条房主已排序的规范化谱面操作排入后台文档流水线。
    /// @warning UI 热路径：只整理常量级元数据并写入无锁队列，不得解压、物化
    /// 完整谱面或等待后台任务。
    void handleCommittedOperation(const CommittedOperation& operation);
    /// @brief 确保无锁远端操作队列已有一个后台消费者。
    /// @warning UI 热路径：每次远端提交调用；只在消费者空闲时向共享线程池提交
    /// 一个串行任务，不得等待任务结束。
    void scheduleRemoteOperationWorker();
    /// @brief 串行消费权威提交并生成可交给逻辑线程的谱面结果。
    /// @warning 后台任务路径：允许执行内存解压、JSON 合并和谱面物化，禁止访问
    /// UI、ECS 或直接调用本地谱面回调。
    void processRemoteOperations();
    /// @brief 在 UI 帧内有界消费后台协作文档结果。
    /// @warning UI 热路径：每帧最多处理固定数量结果，只移动智能指针并触发
    /// 非阻塞逻辑命令回调。
    void processRemoteOperationResults();
    /// @brief 在房主快照追上协议修订后登记等待中的新访客。
    /// @warning UI 热路径：每帧仅比较修订号；满足条件时才消费低频连接事件。
    void processPendingPeerConnections();
    /// @brief 停止并清空远端操作流水线，然后重置规范协作文档。
    /// @warning 房间生命周期低频阻塞点：仅在创建、加入、断开或析构时调用，
    /// 可能等待正在执行的纯内存后台任务结束。
    void resetRemoteOperationPipeline();
    /// @brief 向协作状态机提交逻辑线程排队的本地谱面操作。
    void submitQueuedLocalOperations();
    /// @brief 在权威文档上重放尚未提交的本地增量，构造本机可见谱面。
    [[nodiscard]] std::shared_ptr<::MMM::BeatMap>
    materializeRebasedLocalBeatmap();
    /// @brief 关闭本地谱面变更入口并丢弃尚未提交的操作。
    /// @warning UI 网络事件路径调用；会短暂获取本地操作队列锁，以与逻辑线程
    /// 正在编码的 mutation 完成终止握手。
    void stopAcceptingLocalMutations();
    /// @brief 在节流周期到达时发布有变化的本地主画布状态。
    /// @warning UI 热路径：每帧仅做时间点和固定大小数值比较，达到配置的
    /// 5～60 Hz 周期后才编码并发送一个小消息。
    void flushLocalViewport();
    /// @brief 处理一个已校验的资源协议消息。
    void handleResourceMessage(PeerId                      senderId,
                               const CollaborationMessage& message);
    /// @brief 将已通过 Peer 身份与序号校验的聊天消息加入会话记录。
    /// @param message 已验证的聊天消息。
    void handleChatMessage(const CollaborationChatMessage& message);
    /// @brief 将后台资源事件转为 P2P 消息、回调和协作日志。
    void processResourceEvents();
    /// @brief 向指定访客发送当前资源清单。
    void sendResourceManifest(PeerId peerId);
    /// @brief 将当前连接切换为错误状态。
    void fail(std::string message);

    /// @brief 当前房间状态。
    CollaborationRoomState m_state = CollaborationRoomState::Idle;
    /// @brief 当前角色是否为房主。
    bool m_isHost = false;
    /// @brief 服务端分配的公开房间标识。
    std::string m_roomId;
    /// @brief 公网目录展示的房间名称。
    std::string m_roomName;
    /// @brief 当前中心服务器配置。
    CollaborationServerEndpoint m_serverEndpoint;
    /// @brief 公网在线房间目录订阅客户端。
    CollaborationDirectoryClient m_directory;
    /// @brief 目录断线后的下一次重连时间。
    std::chrono::steady_clock::time_point m_nextDirectoryReconnect;
    /// @brief 跨重连保留的最近目录错误。
    std::string m_directoryError;
    /// @brief 当前 Creator。
    std::string m_creator;
    /// @brief 当前应用配置持久化的稳定协作者标识。
    ParticipantId m_participantId;
    /// @brief 当前加入房间流程生成且不会复用的操作会话标识。
    OperationSessionId m_operationSessionId;
    /// @brief 最近错误。
    std::string m_lastError;
    /// @brief 访客取得 PeerId 前暂存的传输所有权。
    std::unique_ptr<WebRtcTransport> m_pendingTransport;
    /// @brief 协作状态机持有传输后的稳定观察指针。
    WebRtcTransport* m_transport = nullptr;
    /// @brief 统一房主/访客协作状态机。
    std::unique_ptr<CollaborationPeer> m_peer;
    /// @brief 当前客户端的规范化谱面文档。
    BeatmapDocumentCodec m_documentCodec;
    /// @brief 逻辑线程专用的本地编辑增量编码器。
    BeatmapDocumentCodec m_localMutationCodec;
    /// @brief 远端提交数据的本地逻辑命令入口。
    ApplyBeatmapCallback m_applyBeatmapCallback;
    /// @brief 私有无锁队列、消费者状态与后台任务生命周期。
    std::unique_ptr<RemoteOperationPipeline> m_remoteOperationPipeline;
    /// @brief 房主等待最新文档快照完成后再登记的访客连接事件。
    std::deque<WebRtcTransportEvent> m_pendingPeerConnections;
    /// @brief 已发布给 Peer 且可安全交给新访客的文档修订号。
    std::uint64_t m_publishedDocumentRevision{ 0 };
    /// @brief 访客完成资源校验后的会话绑定入口。
    ResourceBundleCallback m_resourceBundleCallback;
    /// @brief 后台资源清单和分块状态机。
    CollaborationResourceSync m_resourceSync;
    /// @brief 房主当前已经准备好的资源清单。
    std::optional<ResourceManifest> m_resourceManifest;
    /// @brief 已发送当前清单的访客集合。
    std::unordered_set<PeerId> m_resourceManifestRecipients;
    /// @brief 逻辑线程等待 UI 网络循环提交的本地操作。
    std::deque<ByteBuffer> m_localOperationQueue;
    /// @brief 已交给可靠传输且等待房主提交回执的本地操作。
    std::deque<ByteBuffer> m_inFlightLocalOperations;
    /// @brief 保护本地操作队列。
    std::mutex m_localOperationMutex;
    /// @brief 当前房间是否接受逻辑线程发布的谱面变化。
    std::atomic_bool m_acceptLocalMutations{ false };
    /// @brief 本地队首因传输背压暂时无法提交时抑制重复错误日志。
    bool m_localOperationSubmitBlocked = false;
    /// @brief 远端回灌曾覆盖含待确认编辑的本地状态，需要由本人提交回执重放。
    bool m_localStateNeedsRebase = false;
    /// @brief 当前房间角色是否为房主。
    std::atomic_bool m_hostRoleForObserver{ false };
    /// @brief 房主是否已经排队初始完整快照。
    std::atomic_bool m_initialSnapshotQueued{ false };
    /// @brief 当前客户端是否已应用完整房主文档。
    std::atomic_bool m_hasDocument{ false };
    /// @brief 房间启动时钟，用于日志相对时间。
    std::chrono::steady_clock::time_point m_startedAt;
    /// @brief 下一日志序号。
    std::uint64_t m_nextLogSequence = 1;
    /// @brief 有界实时日志。
    std::vector<CollaborationLogEntry> m_logs;
    /// @brief 下一条本地聊天记录序号。
    std::uint64_t m_nextChatSequence = 1;
    /// @brief 当前联机会话内的有界聊天记录。
    std::vector<CollaborationChatEntry> m_chatMessages;
    /// @brief 主画布最近写入且等待节流发布的本地状态。
    std::optional<ParticipantViewport> m_pendingLocalViewport;
    /// @brief 最近一次成功交给 Peer 发布的本地状态。
    std::optional<ParticipantViewport> m_lastPublishedLocalViewport;
    /// @brief 允许下一次视口状态发送的时间点。
    std::chrono::steady_clock::time_point m_nextViewportPublish;
    /// @brief P2P 主画布状态的本地发送频率，限制为 5～60 Hz。
    std::uint32_t m_viewportPublishRateHz = 10;
    /// @brief 用户列表选择的持续跟随稳定身份；为空表示未跟随。
    ParticipantId m_followedParticipantId;
    /// @brief 尚未获准建立 P2P 连接的访客请求。
    std::vector<CollaborationPendingJoinRequest> m_pendingJoinRequests;
    /// @brief 无 Peer 时返回的空参与者表。
    std::unordered_map<PeerId, ParticipantIdentity> m_emptyParticipants;
    /// @brief 无 Peer 时返回的空视口状态表。
    std::unordered_map<PeerId, ParticipantViewport> m_emptyViewports;
};
}  // namespace MMM::Network::Collaboration
