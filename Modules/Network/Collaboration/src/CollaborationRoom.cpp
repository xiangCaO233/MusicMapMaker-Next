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
/// @brief CollaborationRoom 的线程与状态编排约束。
///
/// @details
/// 房间把四个相互独立的子系统组合为单一产品接口：WebRtcTransport 提供连接与
/// 二进制帧，CollaborationPeer 提供权限、排序和协议消息，BeatmapDocumentCodec
/// 维护权威谱面文档，CollaborationResourceSync 负责资源扫描与分块落盘。
///
/// @par 线程边界
/// update、传输事件、Peer 和回调交付运行在 UI 网络循环。谱面观察入口可能来自
/// 逻辑线程，通过 m_localOperationMutex 与原子门闩投递本地操作。权威文档解码、
/// 合并、BeatMap 物化和快照压缩只由唯一 AppThreadPool 消费者执行。
///
/// @par 本地编辑路径
/// onBeatmapMutated 使用独立 m_localMutationCodec
/// 相对上层当前状态编码增量，分配 单调 sequence 并进入 queued。update
/// 按权限和固定预算提交给 Peer，成功后移动 到 in-flight。权威回执按 payload
/// 找回 sequence，并通过 apply 或 acknowledge 回调推进上层水位线。
///
/// @par 远端编辑路径
/// Peer 先按修订排序，再调用 handleCommittedOperation。后台 Codec
/// 应用权威补丁； 若本地仍有未确认编辑，则在最新权威文档上依次重放
/// queued/in-flight，向 UI 交付不会倒退本地即时状态的 BeatMap。UI
/// 实际应用后再确认编码基线。
///
/// @par 房主快照路径
/// 房主权威文档每次成功修订都标记 dirty，但完整 CBOR 压缩最多每秒一次。新访客
/// 的 PeerConnected 若早于最新 snapshot 发布，会暂存到 pending 列表，防止访客
/// 获得旧快照后错过已经提交的增量。
///
/// @par 状态生命周期
/// startHost/join 只允许从 Idle 进入。主动 disconnect 或访客失去房主会停止本地
/// 入队、销毁 Peer/Transport、等待后台任务并清理所有在线派生状态。目录客户端
/// 只在 Idle 时维持连接，避免活动房间重复订阅公开目录。
///
/// @par 性能边界
/// 每帧传输事件、本地提交、后台交付、日志和资源事件均有独立固定预算。连续视口
/// 只覆盖最新状态并按 5..60 Hz 发布。任何全谱面扫描、物化和压缩都不在 update
/// 热路径执行。
///
/// @par 权限边界
/// 传输层只认证连接身份，真正编辑授权由房主 Peer 调用
/// authorizeParticipantEdit。 授权依据 Codec inspect
/// 得到的实际分类；访客队列也在每次发送前使用最新权限
/// 复检，避免撤权与在途操作竞态。
///
/// @par 资源边界
/// 资源协议不进入谱面 Codec。Room 只负责选择 Peer 接收者；访客完成校验与落盘
/// 后才交付重定位项目。每个新 manifest 清空接收者集合，支持活动会话在线替换。
///
/// @par 身份边界
/// PeerId 是可复用路由槽位，ParticipantId 是跨重连稳定身份，SessionId 区分加入
/// 实例。日志、跟随和本地回执分别选择适合身份，不能把 PeerId 当作长期用户 ID。
///
/// @par 错误边界
/// 可恢复发送失败写入有界日志并保留队列重试；配置、Peer 构造或目录控制连接失败
/// 进入 Error。访客被拒绝保留原因等待用户处理，房主移除或断开则走明确的会话
/// 清理路径，不把正常离线编辑继续记录为网络提交故障。
///
/// @par 回调边界
/// apply、acknowledge 和 resource bundle 回调只在 update 线程调用。后台消费者只
/// 产生值语义 Result，不直接触碰 UI/ECS；用户回调也不在持有本地操作互斥锁时
/// 执行，避免上层同步回调重新进入房间造成死锁。
///
/// @par 重启边界
/// reset 保留 serverEndpoint 供目录恢复，但清除房间 ID、会话身份、权限、文档和
/// 资源状态。下一次 startHost/join 必须重新建立所有在线事实，不继承旧修订水位。
/// 目录错误与会话错误分开保存，避免一次公开列表故障污染随后正常的房间状态。
/// 日志序列在每次新会话重置，使同一实例的不同房间记录不会被误认为连续审计流。
///
namespace
{
/// @brief 判断权限集合是否覆盖一次谱面分类变更。
/// @param permissions 参与者当前权限位图。
/// @param flags 待提交补丁实际涉及的谱面分类。
/// @return 具有总 Edit 权限且每个分类权限均存在时返回 true。
///
/// @details Edit 是总开关，Objects、Timelines、AudioSamples、Metadata 与
/// Annotations 是细分门槛。未出现在 flags 中的分类不要求对应权限，使窄增量
/// 不会因无关权限缺失被拒绝。
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
/// @note 超限时删除最旧前缀，保留最新诊断且限制长期会话内存。
constexpr std::size_t MAX_COLLABORATION_LOG_ENTRIES = 1000;
/// @brief 当前联机会话在内存中保留的最大聊天记录数。
/// @note 聊天只存内存，不参与谱面文档或重同步快照。
constexpr std::size_t MAX_COLLABORATION_CHAT_ENTRIES = 200;
/// @brief 每帧最多消费的传输生命周期事件数。
/// @note 防止异常事件洪峰独占 UI 更新，其余事件留待下一帧。
constexpr std::size_t MAX_TRANSPORT_EVENTS_PER_UPDATE = 256;

/// @brief 返回只用于本地诊断的构建指纹短前缀。
/// @param fingerprint 完整的 SHA-256 构建指纹。
/// @return 最多前 12 个十六进制字符。
/// @note 前缀不得用于兼容性判断；实际拒绝始终比较完整指纹。
[[nodiscard]] std::string_view fingerprintPrefix(std::string_view fingerprint)
{
    return fingerprint.substr(0,
                              std::min<std::size_t>(12U, fingerprint.size()));
}
/// @brief 每帧最多向权威 Peer 提交的本地谱面操作数。
/// @note 保持原队列顺序，达到预算后下一帧继续。
constexpr std::size_t MAX_LOCAL_OPERATIONS_PER_UPDATE = 256;
/// @brief 每帧最多向逻辑线程发布的后台谱面合并结果数。
/// @note 同帧结果还会折叠为最新 BeatMap，减少上层 ECS 扫描。
constexpr std::size_t MAX_REMOTE_RESULTS_PER_UPDATE = 4;
/// @brief 每帧最多追加的后台操作日志数。
/// @note 日志预算独立于谱面结果，避免大量修订阻塞状态交付。
constexpr std::size_t MAX_REMOTE_LOG_RESULTS_PER_UPDATE = 256;
/// @brief 逻辑线程等待 UI 网络循环提交的最大操作数。
/// @note 队列满时观察入口返回零，不进行无界内存增长。
constexpr std::size_t MAX_QUEUED_LOCAL_OPERATIONS = 4096;
/// @brief 房主完整重同步快照的最短刷新间隔；普通操作增量不受此限制。
/// @note 新修订只覆盖 pending revision，截止时压缩最新文档一次。
constexpr auto HOST_SNAPSHOT_REFRESH_INTERVAL = std::chrono::seconds(1);

/// @brief 获取 steady_clock 当前时间的纳秒整数，供跨线程非阻塞截止时间比较。
/// @return 与系统时钟调整无关的单调纳秒计数。
/// @note 数值只在同一进程内比较，不序列化或解释为墙上时间。
std::int64_t steadyNowNanoseconds()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// @brief 目录连接失败后的重试间隔。
/// @note 使用非阻塞截止时间，不在 update 中 sleep 或 wait。
constexpr auto DIRECTORY_RECONNECT_INTERVAL = std::chrono::seconds(5);
/// @brief 主画布状态允许的最低发送频率。
/// @note 避免过低配置让跟随视图长期滞后。
constexpr std::uint32_t MIN_VIEWPORT_PUBLISH_RATE_HZ = 5;
/// @brief 主画布状态允许的最高发送频率。
/// @note 限制高频鼠标交互产生的网络与协议负载。
constexpr std::uint32_t MAX_VIEWPORT_PUBLISH_RATE_HZ = 60;

/// @brief 判断两个主画布状态是否存在值得发送的可见变化。
/// @param previous 最近一次已发布状态。
/// @param current 当前等待发布状态。
/// @return 时间变化超过 1ms 或横向偏移变化超过万分之一时返回 true。
///
/// @details 对播放、视觉和可见区间时间分别应用 1ms 阈值，对横向偏移应用更细
/// 阈值。微小浮点噪声被抑制，真实交互变化仍由发送频率限制器合并为最新状态。
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
///
/// @details
/// UI/网络线程只把已排序 CommittedOperation 投入 tasks；唯一后台消费者持有
/// m_documentCodec，批量应用操作、重放本地未确认增量并按需物化 BeatMap。结果
/// 经 results 返回 UI 线程，逐操作日志经独立 operationLogs 返回，避免日志数量
/// 迫使逻辑线程重复应用中间谱面。
///
/// workerState 用三态门闩保证至多一个消费者：0 空闲、1 正在排空、2 表示运行中
/// 又收到唤醒。消费者从 2 退回 1 后继续同一批，只有确认没有新任务才切回 0。
///
/// @warning RemoteOperationPipeline 的销毁必须先等待
/// workerFutures，随后才能清空
///          队列和 Codec；后台闭包捕获 CollaborationRoom::this。
class CollaborationRoom::RemoteOperationPipeline
{
public:
    /// @brief 后台协作文档流水线接收的一条权威提交。
    /// @details Task 同时携带参与者显示信息和本地提交上下文，后台无需读取可能
    ///          已变化的 CollaborationPeer 状态。
    struct Task {
        /// @brief 房主已经排序的提交内容。
        /// @note payload 生命周期随 Task 移入后台队列。
        CommittedOperation operation;
        /// @brief 提交者当前对应的 PeerId。
        PeerId peerId{ 0 };
        /// @brief 提交者显示身份。
        std::string creator;
        /// @brief 当前实例是否为房主，用于生成最新重同步快照。
        /// @note 以入队时角色快照为准，不在后台读取可变化房间状态。
        bool host{ false };
        /// @brief 本地待确认操作是否需要在权威文档上重放。
        /// @note 只对本地回执有意义，远端提交天然要求物化可见状态。
        bool reapplyLocalState{ false };
        /// @brief 本次提交是否由当前进程发起。
        /// @note participantId 与 operationSessionId 同时匹配才成立。
        bool originatedLocally{ false };
        /// @brief 本次权威提交已经确认的本地变化序号。
        /// @note 非本地提交或无法关联在途 payload 时为零。
        std::uint64_t committedLocalMutationSequence{ 0 };
    };

    /// @brief 后台协作文档流水线交还 UI 线程的有界结果。
    /// @details Result 可仅表示日志、仅表示房主快照，或携带最新可见 BeatMap；
    ///          logOperation 区分逐修订记录与批量交付记录。
    struct Result {
        /// @brief 是否把该结果记录为一条独立提交日志。
        /// @note 批量 BeatMap delivery 设为 false，逐 Task 结果保持 true。
        bool logOperation{ true };
        /// @brief 提交者当前对应的 PeerId。
        PeerId peerId{ 0 };
        /// @brief 提交者稳定协作标识。
        ParticipantId participantId;
        /// @brief 提交者显示身份。
        std::string creator;
        /// @brief 提交修订号。
        std::uint64_t revision{ 0 };
        /// @brief 后台合并成功后需要替换的谱面类别。
        /// @note 批量 delivery 为所有需回灌成功修订 flags 的并集。
        ::MMM::BeatmapMutationFlags flags{ ::MMM::BeatmapMutationFlags::None };
        /// @brief 已重放本地待确认操作的可见谱面。
        /// @note 为空时结果可能只承担日志、确认或房主快照交付。
        std::shared_ptr<::MMM::BeatMap> beatmap;
        /// @brief 当前可见谱面相对上次交付发生变化的根物件稳定标识。
        /// @note nullopt 表示需要全对象刷新，空 vector 表示确认无对象差异。
        std::optional<std::vector<std::string>> objectDeltaIdentities;
        /// @brief 后台已经完成扫描和编码的玩家物件基线。
        std::shared_ptr<BeatmapDocumentCodec::ObjectEncodingBaseline>
            objectEncodingBaseline;
        /// @brief 可见谱面已经包含的最新本地变化序号。
        std::uint64_t includedLocalMutationSequence{ 0 };
        /// @brief 房主用于后续访客重同步的最新快照。
        /// @note 按节流截止时间生成，可与 BeatMap delivery 同时或独立出现。
        std::optional<ByteBuffer> hostSnapshot;
        /// @brief 后台处理失败时写入协作日志的错误标识。
        /// @note 非空时其他交付字段不得作为成功状态应用。
        std::string error;
    };

    /// @brief UI 线程向后台文档消费者投递的无锁权威提交队列。
    moodycamel::ConcurrentQueue<Task> tasks;
    /// @brief 后台文档消费者向 UI 线程发布的无锁结果队列。
    moodycamel::ConcurrentQueue<Result> results;
    /// @brief 不得阻塞最新谱面交付的独立操作日志队列。
    moodycamel::ConcurrentQueue<Result> operationLogs;
    /// @brief 后台消费者状态：0 为空闲、1 为运行、2 为运行且收到新唤醒。
    /// @warning UI 线程在提交任务后使用 acq_rel 更新，后台消费者在排空边界
    /// 使用 acq_rel 交接；只协调唯一消费者，不承载文档数据同步。
    std::atomic_uint8_t workerState{ 0U };
    /// @brief 权威文档是否存在尚未压缩进房主重同步快照的修订。
    /// @warning 后台文档消费者写入，UI 网络循环读取；仅用于低频任务调度，
    /// 使用 acquire/release，不能承载文档数据或替代唯一消费者所有权。
    std::atomic_bool hostSnapshotDirty{ false };
    /// @brief 等待写入下一份房主重同步快照的最新权威修订号。
    /// @warning 后台文档消费者写入，UI 网络循环读取；只用于给快照标记修订。
    std::atomic_uint64_t pendingHostSnapshotRevision{ 0U };
    /// @brief 下次允许压缩完整房主快照的 steady_clock 纳秒时间。
    /// @warning 后台文档消费者写入，UI 网络循环读取；每帧只做一次 relaxed
    /// 整数比较，避免完整 JSON 压缩随每条物件操作重复执行。
    std::atomic_int64_t nextHostSnapshotBuildNanoseconds{ 0 };
    /// @brief 已提交后台消费者任务，用于房间销毁前完成全部生命周期握手。
    std::vector<std::future<void>> workerFutures;
    /// @brief 上一次交付给逻辑线程的可见规范文档，仅由唯一后台消费者访问。
    std::unique_ptr<BeatmapDocumentCodec> visibleDocument;
};

CollaborationRoom::CollaborationRoom()
    : m_nextDirectoryReconnect(std::chrono::steady_clock::now())
    , m_startedAt(std::chrono::steady_clock::now())
    , m_nextViewportPublish(std::chrono::steady_clock::now())
{
    // 流水线与房间同寿命，future 预留降低正常会话中的容器扩容次数。
    m_remoteOperationPipeline = std::make_unique<RemoteOperationPipeline>();
    m_remoteOperationPipeline->workerFutures.reserve(4U);
}

/// @brief 停止观察、销毁网络所有权并等待后台流水线安全退出。
///
/// @details 析构不再向 UI 发布结果，因此先关闭本地 mutation 入口和 Peer 回调，
/// 再由 resetRemoteOperationPipeline 等待所有捕获 this 的后台任务完成。
CollaborationRoom::~CollaborationRoom()
{
    // 先阻止观察线程继续入队，再销毁传输；最后等待后台文档任务完成。
    stopAcceptingLocalMutations();
    m_peer.reset();
    m_pendingTransport.reset();
    m_transport = nullptr;
    resetRemoteOperationPipeline();
}

/// @brief 从 Idle 状态启动房主、公开目录传输和权威 CollaborationPeer。
/// @param config 房间、身份、端点、资源封面与构建兼容策略。
/// @return 初始化全部组件成功时返回 true。
///
/// @details 配置在创建传输前规范化并验证。WebRtcTransport 启动成功后重置所有
/// 会话级状态、权限、日志、视口和本地操作队列，再把传输所有权移交给房主 Peer。
/// 初始谱面不在此编码，由首次 onBeatmapMutated 作为 snapshot 入队。
///
/// @warning 成功只表示开始发布；RoomPublished 事件到达前 roomId 仍为空。
///
/// @par 初始化不变量
/// 新会话必须具有规范 creator、稳定 participantId、唯一 operationSessionId
/// 和有效 构建指纹。房主固定 peerId=1
/// 并拥有全部权限。日志、聊天、审批、视口、资源
/// 接收者、文档流水线和本地操作水位线都从空状态开始。
///
/// @par 所有权转换
/// WebRtcTransport 先在局部 unique_ptr 中启动，避免失败污染成员。成功后裸观察
/// 指针 m_transport 指向同一对象，unique_ptr 移入 CollaborationPeer；Room
/// 只通过 Peer 销毁实际传输，不会重复释放。
bool CollaborationRoom::startHost(CollaborationHostRoomConfig config)
{
    if ( m_state != CollaborationRoomState::Idle ) return false;

    // 所有身份和指纹在写入成员前验证，失败保持旧 Idle 会话未启动。
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

    // transportConfig 生成本次会话唯一 sessionId，稳定 participantId
    // 则跨重连保留。
    auto             transport = std::make_unique<WebRtcTransport>();
    WebRtcHostConfig transportConfig;
    transportConfig.endpoint         = config.endpoint;
    transportConfig.roomName         = config.roomName;
    transportConfig.creator          = config.creator;
    transportConfig.roomCoverImage   = std::move(config.roomCoverImage);
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

    // 从此处开始提交初始化后的房间状态；下方字段全部属于新会话。
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
    m_localViewportPublishDeferred = false;
    m_nextViewportPublish          = std::chrono::steady_clock::now();
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
        // 本地编码器与队列受观察线程共享锁保护，必须作为一个状态组重置。
        std::lock_guard lock(m_localOperationMutex);
        m_localMutationCodec.reset();
        m_localOperationQueue.clear();
        m_inFlightLocalOperations.clear();
        m_localOperationSubmitBlocked          = false;
        m_localStateNeedsRebase                = false;
        m_remoteStateApplyPending              = false;
        m_nextLocalMutationSequence            = 1;
        m_latestCommittedLocalMutationSequence = 0;
    }

    // m_transport 是非拥有快捷指针，实际所有权随后移入 CollaborationPeer。
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
    // Peer 构造失败时撤销观察指针和所有权，房间进入可诊断 Error。
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

/// @brief 从 Idle 状态启动访客加入与审批流程。
/// @param config 目标房间、身份、端点和资源缓存根。
/// @return 信令传输成功启动时返回 true。
///
/// @details 访客在获得 accepted peerId 前不能构造 CollaborationPeer，因此传输
/// 暂存于 m_pendingTransport。会话状态、日志、权限和本地操作队列全部重置，资源
/// 同步器同时进入 Guest 模式。初始权限为零，等待房主权限消息。
///
/// @warning 成功返回后状态仍为 Joining，审批和 DataChannel 通过 update 推进。
///
/// @par 与房主路径差异
/// 访客不设置初始快照门闩，也不能在收到权威 snapshot 前编码本地增量。传输所有权
/// 暂存在 m_pendingTransport，accepted 分配本地 peerId 后才构造
/// CollaborationPeer。 资源缓存根在此移入同步器，旧房间 manifest
/// 与接收者集合不会跨加入复用。
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

    // 访客 sessionId 每次加入重新生成，用于区分同一稳定身份的旧在途操作。
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
    m_localViewportPublishDeferred = false;
    m_nextViewportPublish          = std::chrono::steady_clock::now();
    m_followedParticipantId.clear();
    m_pendingJoinRequests.clear();
    resetRemoteOperationPipeline();
    m_hasDocument.store(false, std::memory_order_relaxed);
    m_localPermissions.store(0U, std::memory_order_release);
    m_initialSnapshotQueued.store(false, std::memory_order_relaxed);
    m_hostRoleForObserver.store(false, std::memory_order_relaxed);
    m_acceptLocalMutations.store(true, std::memory_order_release);
    // 资源缓存根随本次会话移交，旧清单及已发送记录全部清除。
    m_resourceSync.startGuest(std::move(config.resourceCacheRoot));
    m_resourceManifest.reset();
    m_resourceManifestRecipients.clear();
    {
        std::lock_guard lock(m_localOperationMutex);
        m_localMutationCodec.reset();
        m_localOperationQueue.clear();
        m_inFlightLocalOperations.clear();
        m_localOperationSubmitBlocked          = false;
        m_localStateNeedsRebase                = false;
        m_remoteStateApplyPending              = false;
        m_nextLocalMutationSequence            = 1;
        m_latestCommittedLocalMutationSequence = 0;
    }
    // 在 accepted 前由 pendingTransport 拥有；ensureGuestPeer 后所有权转入
    // Peer。
    m_transport        = transport.get();
    m_pendingTransport = std::move(transport);
    appendLog(CollaborationLogEventType::SignalingConnected,
              DEFAULT_HOST_ID,
              {},
              "connecting");
    return true;
}

/// @brief 批准房主待审批列表中的加入请求。
/// @param requestId WebRtcTransport 上报的请求 ID。
/// @return 请求存在且传输层接受批准时返回 true。
/// @note 只有传输启动成功后才删除本地项，失败允许 UI 重试。
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

/// @brief 拒绝房主待审批列表中的加入请求。
/// @param requestId WebRtcTransport 上报的请求 ID。
/// @return 拒绝消息成功发送时返回 true。
/// @note 日志保留 creator，待审批项只在传输确认发送后移除。
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

/// @brief 由房主移除一个当前已注册的远端参与者。
/// @param peerId 目标访客路由 ID。
/// @return 目标合法且传输层开始断开时返回 true。
/// @note 禁止移除零 ID、房主自身或不在当前参与者表中的 ID。
bool CollaborationRoom::removeParticipant(PeerId peerId)
{
    if ( !m_isHost || !m_transport || !m_peer || peerId == 0 ||
         peerId == localPeerId() || !participants().contains(peerId) ) {
        return false;
    }
    return m_transport->disconnectPeer(peerId, "removed_by_host");
}

/// @brief 由 Peer 层更新指定参与者权限并广播。
/// @param peerId 目标参与者。
/// @param permissions 新权限位图。
/// @return 当前 Peer 允许并成功提交权限变化时返回 true。
bool CollaborationRoom::setParticipantPermissions(
    PeerId peerId, CollaborationPermissionMask permissions)
{
    return m_peer && m_peer->setParticipantPermissions(peerId, permissions);
}

/// @brief 在离线状态切换公开目录与信令端点。
/// @param endpoint 新端点配置。
/// @return 房间未活动且端点可生成有效 URL 时返回 true。
///
/// @details 端点变化会断开旧目录、清除目录错误并把重连截止时间提前到当前，
/// updateDirectory 下一轮即可连接新服务。活动房间禁止切换以免拆分会话路由。
///
/// @par 幂等语义
/// 与当前端点相同直接返回成功，不重建目录连接，也不打断正在进行的刷新。只有
/// 实际变化才清空错误并安排立即重连。
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

/// @brief 请求目录客户端立即刷新公开房间列表。
/// @return 请求被目录客户端接受时返回 true。
bool CollaborationRoom::refreshDirectory()
{
    return m_directory.refresh();
}

/// @brief 设置后台合并结果交付逻辑线程的回调。
/// @param callback 接收谱面、分类、序列、修订和可选对象 ID 差量。
void CollaborationRoom::setApplyBeatmapCallback(ApplyBeatmapCallback callback)
{
    m_applyBeatmapCallback = std::move(callback);
}

/// @brief 设置无需重新应用谱面时的本地操作确认回调。
/// @param callback 接收已提交的最高本地 mutation sequence。
void CollaborationRoom::setLocalMutationAcknowledgedCallback(
    LocalMutationAcknowledgedCallback callback)
{
    m_localMutationAcknowledgedCallback = std::move(callback);
}

/// @brief 设置访客完整资源包落盘后的交付回调。
/// @param callback 接收资源重定位后的项目 bundle。
void CollaborationRoom::setResourceBundleCallback(
    ResourceBundleCallback callback)
{
    m_resourceBundleCallback = std::move(callback);
}

/// @brief 扫描房主项目并准备可向访客发布的资源清单。
/// @param project 房主当前项目。
/// @param beatmap 用于筛选实际引用资源的谱面。
///
/// @details 新准备会撤销已缓存 manifest 和所有接收者记录，确保活动房间内替换
/// 项目后，每位参与者都能收到新清单。实际扫描由 ResourceSync 异步处理。
void CollaborationRoom::prepareHostResources(const ::MMM::Project& project,
                                             const ::MMM::BeatMap& beatmap)
{
    m_resourceManifest.reset();
    m_resourceManifestRecipients.clear();
    m_resourceSync.startHost(project, beatmap);
}

/// @brief 从观察线程编码并排队一次本地谱面变化。
/// @param beatmap 包含本次本地编辑后的完整业务状态。
/// @param flags 本次实际变化分类。
/// @return 成功排队的单调序列，拒绝或无净变化时返回零。
///
/// @details 房主首次提交自动编码完整 snapshot；访客必须先拥有远端文档。队列
/// 有固定上限，编码和状态变化在 m_localOperationMutex 下串行，保证发送基线与
/// 序列顺序一致。若远端应用正在等待逻辑线程采用，本地新编辑标记需要重基。
///
/// @warning 该入口可能由逻辑观察链调用，不执行网络发送；只做编码与有界入队。
std::uint64_t CollaborationRoom::onBeatmapMutated(
    const ::MMM::BeatMap& beatmap, ::MMM::BeatmapMutationFlags flags)
{
    if ( !m_acceptLocalMutations.load(std::memory_order_acquire) ) return 0;

    const bool host = m_hostRoleForObserver.load(std::memory_order_relaxed);
    if ( !host && !m_hasDocument.load(std::memory_order_acquire) ) {
        return 0;
    }

    std::lock_guard lock(m_localOperationMutex);
    if ( !m_acceptLocalMutations.load(std::memory_order_acquire) ) return 0;
    if ( m_localOperationQueue.size() >= MAX_QUEUED_LOCAL_OPERATIONS ) {
        return 0;
    }
    // 房主首个成功编码的本地变化承担加入者所需完整初始快照。
    const bool snapshot =
        host && !m_initialSnapshotQueued.load(std::memory_order_relaxed);
    auto payload = m_localMutationCodec.encode(beatmap, flags, snapshot);
    if ( !payload.has_value() ) return 0;

    // 只有 payload 非空且有效才消费序列，失败不会制造确认空洞。
    const std::uint64_t sequence = m_nextLocalMutationSequence++;
    m_localOperationQueue.push_back(
        LocalOperation{ std::move(payload.value()), sequence });
    if ( m_remoteStateApplyPending ) m_localStateNeedsRebase = true;
    if ( snapshot ) {
        m_initialSnapshotQueued.store(true, std::memory_order_relaxed);
    }
    return sequence;
}

/// @brief 通知房间上层已经采用完整可见 BeatMap。
/// @param beatmap 逻辑线程当前真实状态。
///
/// @details 发送编码基线与上层模型重新对齐，并清除尚未安装的后台对象基线。
/// 若仍有排队或在途本地操作，则标记后续权威结果必须重放它们。
///
/// @par 调用时机
/// 上层应在真正把后台 delivery 写入逻辑模型后调用，而不是仅在收到回调时调用。
/// 这样 encodingBaseline 始终对应用户当前可见并继续编辑的版本。
void CollaborationRoom::onBeatmapSynchronized(const ::MMM::BeatMap& beatmap)
{
    if ( !m_acceptLocalMutations.load(std::memory_order_acquire) ) return;
    std::lock_guard lock(m_localOperationMutex);
    if ( !m_acceptLocalMutations.load(std::memory_order_acquire) ) return;
    m_localMutationCodec.synchronizeEncodingBaseline(beatmap);
    m_pendingObjectEncodingBaselines.clear();
    m_remoteStateApplyPending = false;
    if ( !m_localOperationQueue.empty() ||
         !m_inFlightLocalOperations.empty() ) {
        m_localStateNeedsRebase = true;
    }
}

/// @brief 确认某次后台权威谱面已经由逻辑线程实际应用。
/// @param revision 已应用权威修订。
/// @param includedLocalMutationSequence 该谱面包含的最高本地序列。
///
/// @details 只有 revision、included sequence 和当前本地序列水位线全部吻合，才
/// 安装后台预计算对象编码基线，避免较旧 UI 命令覆盖更新基线。随后清除不晚于
/// 当前修订的待安装项，并根据仍在途操作设置重基标记。
///
/// @par 快速基线条件
/// pending baseline 只覆盖纯 Objects 交付。若确认时已有更新本地 sequence，条件
/// `next-1 <= included` 不成立，后台基线会被丢弃，避免漏掉确认后新发生的编辑。
void CollaborationRoom::onAuthoritativeBeatmapApplied(
    std::uint64_t revision, std::uint64_t includedLocalMutationSequence)
{
    std::lock_guard lock(m_localOperationMutex);
    const auto      baseline =
        std::find_if(m_pendingObjectEncodingBaselines.begin(),
                     m_pendingObjectEncodingBaselines.end(),
                     [revision](const PendingObjectEncodingBaseline& pending) {
                         return pending.revision == revision;
                     });
    if ( baseline != m_pendingObjectEncodingBaselines.end() &&
         baseline->includedLocalMutationSequence ==
             includedLocalMutationSequence &&
         m_nextLocalMutationSequence - 1U <= includedLocalMutationSequence ) {
        m_localMutationCodec.synchronizeObjectEncodingBaseline(
            std::move(baseline->baseline));
    }
    std::erase_if(m_pendingObjectEncodingBaselines,
                  [revision](const PendingObjectEncodingBaseline& pending) {
                      return pending.revision <= revision;
                  });
    m_remoteStateApplyPending = false;
    if ( !m_localOperationQueue.empty() ||
         !m_inFlightLocalOperations.empty() ) {
        m_localStateNeedsRebase = true;
    }
}

/// @brief 主动离开当前房间并恢复离线 Idle 状态。
/// @note 主动断开写入 local_disconnect 日志，但清除 lastError，不作为故障展示。
void CollaborationRoom::disconnect()
{
    if ( m_state != CollaborationRoomState::Idle ) {
        appendLog(CollaborationLogEventType::Disconnected,
                  localPeerId(),
                  m_creator,
                  "local_disconnect");
    }
    resetOnlineSessionState();
    m_lastError.clear();
}

/// @brief 清除所有在线会话所有权与派生状态。
///
/// @details 先停止本地入队，再销毁 Peer/Transport
/// 并等待后台流水线。身份、资源、
/// 聊天、视口、审批、权限和快照门闩全部复位；目录端点配置保留供离线重连。
/// 调用方可在 reset 后保留 lastError，用于展示远端断线原因。
///
/// @par 清理顺序
/// stopAcceptingLocalMutations 与 Peer
/// 析构先切断新输入和网络回调；流水线等待后台 任务后清 Codec；最后复位资源和 UI
/// 派生状态。该顺序防止后台结果在清理末尾 重新填充队列或调用已经失效的传输。
void CollaborationRoom::resetOnlineSessionState()
{
    stopAcceptingLocalMutations();
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
    resetRemoteOperationPipeline();
    m_resourceSync.reset();
    m_resourceManifest.reset();
    m_resourceManifestRecipients.clear();
    m_nextChatSequence = 1;
    m_chatMessages.clear();
    m_pendingLocalViewport.reset();
    m_lastPublishedLocalViewport.reset();
    m_localViewportPublishDeferred = false;
    m_followedParticipantId.clear();
    m_pendingJoinRequests.clear();
    m_hasDocument.store(false, std::memory_order_relaxed);
    m_localPermissions.store(0U, std::memory_order_release);
    m_initialSnapshotQueued.store(false, std::memory_order_relaxed);
    m_hostRoleForObserver.store(false, std::memory_order_relaxed);
}

/// @brief 推进目录、传输、Peer、后台结果、资源与视口状态机。
///
/// @details 每次 update 先处理离线目录与到期房主快照，再接收后台合并结果和延迟
/// 参与者。传输事件按固定上限消费，Peer 随后处理业务帧并刷新本地权限。权限收紧
/// 会删除已经不再授权的在途/排队类别，但不修改上层当前 BeatMap。
///
/// @warning 这是 UI 每帧热路径；所有队列消费有界，禁止文件 I/O、阻塞等待或
///          全谱面扫描。完整文档合并与压缩必须留在后台流水线。
///
/// @par 阶段顺序
/// 目录与后台结果优先，使最新 snapshot revision 在处理新 PeerConnected 前可见；
/// 传输事件可能创建访客 Peer，因此 ensureGuestPeer 在事件循环前后各执行一次。
/// Peer update 之后再提交本地操作，保证授权判断使用本帧最新权限。
///
/// @par 权限收紧
/// 访客权限变化时 inspect queued/in-flight 每个 payload 的实际分类，只删除已经
/// 失权类别。逻辑 BeatMap 不回滚，仍授权操作继续排队；这与 submit 阶段遇到暂时
/// 未授权时保留队首等待恢复的策略共同保证本地交互连续。
///
/// @par 早退语义
/// m_transport 为空时仍先完成目录、后台结果和 pending peer 阶段，再结束本帧。
/// 这样断线清理前已完成的后台结果仍可安全记录，不会访问已销毁 Peer。
void CollaborationRoom::update()
{
    // 离线目录始终先推进，活动房间则由 updateDirectory 保证目录连接关闭。
    updateDirectory();
    if ( m_isHost &&
         m_remoteOperationPipeline->hostSnapshotDirty.load(
             std::memory_order_acquire) &&
         steadyNowNanoseconds() >=
             m_remoteOperationPipeline->nextHostSnapshotBuildNanoseconds.load(
                 std::memory_order_relaxed) ) {
        scheduleRemoteOperationWorker();
    }
    // 每帧只消费有界后台结果，再尝试释放等待快照发布的访客连接。
    processRemoteOperationResults();
    processPendingPeerConnections();
    if ( !m_transport ) return;

    ensureGuestPeer();
    for ( std::size_t index = 0; index < MAX_TRANSPORT_EVENTS_PER_UPDATE;
          ++index ) {
        WebRtcTransportEvent event;
        if ( !m_transport->receiveEvent(event) ) break;
        handleTransportEvent(event);
        if ( !m_transport ) break;
    }
    ensureGuestPeer();
    if ( m_peer ) {
        const auto previousPermissions =
            m_localPermissions.load(std::memory_order_acquire);
        // Peer 更新可能改变本地权限；前后快照用于精确处理收紧情况。
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
                [currentPermissions](const LocalOperation& operation) {
                    const auto patch =
                        BeatmapDocumentCodec::inspect(operation.payload);
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

/// @brief 直接向 Peer 提交已编码操作负载。
/// @param payload 完整 BeatmapDocumentCodec 负载。
/// @return Peer 不存在时 InvalidPeer，否则转发 Peer 结果。
SubmitOperationResult CollaborationRoom::submitOperation(
    std::span<const std::uint8_t> payload)
{
    if ( !m_peer ) return SubmitOperationResult::InvalidPeer;
    return m_peer->submitOperation(payload);
}

/// @brief 向当前房间提交聊天文本。
/// @param text 待规范化和广播的消息。
/// @return Peer 不存在时 InvalidPeer，否则返回协议提交结果。
SubmitChatMessageResult CollaborationRoom::sendChatMessage(std::string text)
{
    if ( !m_peer ) return SubmitChatMessageResult::InvalidPeer;
    return m_peer->submitChatMessage(std::move(text));
}

/// @brief 覆盖等待发布的本地主画布状态。
/// @param viewport 当前播放与可见范围。
/// @param deferUntilCommitted true 时只缓存，不在拖拽中发送。
///
/// @details 拒绝非有限浮点值，清零由 Peer 生成的
/// peerId/sequence。连续交互只保留 最新状态；从 deferred
/// 切回立即模式时提前截止时间，确保手势结束马上提交。
///
/// @par 延迟语义
/// deferUntilCommitted 只抑制网络 flush，不延迟本地视觉。等待期间持续覆盖
/// pending 最新值；手势结束传 false，下一次 update 会立即尝试发送最终状态。
void CollaborationRoom::publishLocalViewport(ParticipantViewport viewport,
                                             bool deferUntilCommitted)
{
    if ( !std::isfinite(viewport.playbackTime) ||
         !std::isfinite(viewport.visualTime) ||
         !std::isfinite(viewport.visibleTimeStart) ||
         !std::isfinite(viewport.visibleTimeEnd) ||
         !std::isfinite(viewport.horizontalOffsetRatio) ) {
        return;
    }
    const bool wasDeferred         = m_localViewportPublishDeferred;
    m_localViewportPublishDeferred = deferUntilCommitted;
    if ( wasDeferred && !deferUntilCommitted ) {
        m_nextViewportPublish = std::chrono::steady_clock::now();
    }
    if ( !m_peer ) return;
    viewport.peerId        = 0;
    viewport.sequence      = 0;
    m_pendingLocalViewport = viewport;
}

/// @brief 设置视口广播频率并限制到安全范围。
/// @param rateHz 请求频率，最终限制为 5..60 Hz。
/// @note 修改后允许下一帧立即发布，不等待旧频率周期。
void CollaborationRoom::setViewportPublishRateHz(std::uint32_t rateHz)
{
    m_viewportPublishRateHz = std::clamp(
        rateHz, MIN_VIEWPORT_PUBLISH_RATE_HZ, MAX_VIEWPORT_PUBLISH_RATE_HZ);
    m_nextViewportPublish = std::chrono::steady_clock::now();
}

/// @brief 设置 UI 当前跟随的远端参与者。
/// @param peerId 目标路由 ID；零值取消跟随。
/// @return 目标是当前已知远端参与者时返回 true。
///
/// @details 内部保存稳定 participantId 而非可复用 peerId，重连后 followedPeerId
/// 可重新解析同一身份，不会误跟随占用旧槽位的新参与者。
///
/// @note 禁止跟随本地 peer；目标离线后查询返回零，稳定身份可等待同一参与者在
///       当前会话重新出现。
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

/// @brief 获取当前房间生命周期状态。
/// @return Idle、Hosting、Joining、AwaitingApproval、Connected 或 Error。
CollaborationRoomState CollaborationRoom::state() const
{
    return m_state;
}

/// @brief 查询当前会话是否承担房主职责。
/// @return 房主会话返回 true。
bool CollaborationRoom::isHost() const
{
    return m_isHost;
}

/// @brief 查询房间是否占用在线会话状态。
/// @return 非 Idle 状态返回 true，包括 Error 等待用户断开时。
bool CollaborationRoom::isActive() const
{
    return m_state != CollaborationRoomState::Idle;
}

/// @brief 获取当前公开房间 ID。
/// @return 房间内部字符串引用。
const std::string& CollaborationRoom::roomId() const
{
    return m_roomId;
}

/// @brief 获取当前房间显示名称。
/// @return 房间内部字符串引用。
const std::string& CollaborationRoom::roomName() const
{
    return m_roomName;
}

/// @brief 获取当前目录与信令端点配置。
/// @return 房间内部端点引用。
const CollaborationServerEndpoint& CollaborationRoom::serverEndpoint() const
{
    return m_serverEndpoint;
}

/// @brief 获取离线公开目录客户端状态。
/// @return 目录状态快照。
CollaborationDirectoryState CollaborationRoom::directoryState() const
{
    return m_directory.state();
}

const std::vector<CollaborationDirectoryRoom>&
/// @brief 获取最近一次目录返回的公开房间列表。
/// @return 目录客户端持有的只读列表。
CollaborationRoom::directoryRooms() const
{
    return m_directory.rooms();
}

/// @brief 请求目录客户端按房间 ID 拉取封面。
/// @param roomId 目标公开房间。
/// @return 请求成功排队时返回 true。
bool CollaborationRoom::requestDirectoryRoomCover(std::string_view roomId)
{
    return m_directory.requestRoomCover(roomId);
}

/// @brief 读取目录客户端缓存的房间封面 Base64 文本。
/// @param roomId 目标公开房间。
/// @return 缓存视图；未完成时为空。
std::string_view CollaborationRoom::directoryRoomCover(
    std::string_view roomId) const
{
    return m_directory.roomCover(roomId);
}

/// @brief 获取最近一次目录连接错误。
/// @return 本地缓存的稳定错误文本。
const std::string& CollaborationRoom::directoryError() const
{
    return m_directoryError;
}

/// @brief 获取当前本地路由 peer ID。
/// @return Peer 已建时取 Peer 值，否则取传输暂存值；未分配时为零。
PeerId CollaborationRoom::localPeerId() const
{
    if ( m_peer ) return m_peer->localPeerId();
    return m_transport ? m_transport->localPeerId() : 0;
}

const std::unordered_map<PeerId, ParticipantIdentity>&
/// @brief 获取当前参与者身份表。
/// @return Peer 表或未连接时的稳定空表引用。
CollaborationRoom::participants() const
{
    return m_peer ? m_peer->participantIdentities() : m_emptyParticipants;
}

const std::unordered_map<PeerId, CollaborationPermissionMask>&
/// @brief 获取当前参与者权限表。
/// @return Peer 表或未连接时的稳定空表引用。
CollaborationRoom::participantPermissions() const
{
    return m_peer ? m_peer->participantPermissions() : m_emptyPermissions;
}

/// @brief 原子读取本地参与者当前权限位图。
/// @return 最近一次 Peer update 发布的权限。
CollaborationPermissionMask CollaborationRoom::localPermissions() const
{
    return m_localPermissions.load(std::memory_order_acquire);
}

/// @brief 把本地权限映射为允许提交的谱面分类位图。
/// @return 缺少总 Edit 时为 None，否则包含每个已授权分类。
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

/// @brief 判断本地是否同时拥有编辑与批注权限。
/// @return 两个权限均存在时返回 true。
bool CollaborationRoom::canLocalAnnotate() const
{
    const auto permissions = localPermissions();
    return hasCollaborationPermission(permissions,
                                      CollaborationPermission::Edit) &&
           hasCollaborationPermission(permissions,
                                      CollaborationPermission::Annotations);
}

const std::vector<CollaborationPendingJoinRequest>&
/// @brief 获取房主当前待审批请求列表。
/// @return 内部只读列表；访客通常为空。
CollaborationRoom::pendingJoinRequests() const
{
    return m_pendingJoinRequests;
}

const std::unordered_map<PeerId, ParticipantViewport>&
/// @brief 获取最近接收的远端视口状态表。
/// @return Peer 表或未连接时的稳定空表引用。
CollaborationRoom::participantViewports() const
{
    return m_peer ? m_peer->participantViewports() : m_emptyViewports;
}

/// @brief 把保存的稳定跟随身份解析为当前 peer ID。
/// @return 当前匹配 peer；身份离线或未跟随时为零。
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

/// @brief 获取当前视口广播频率。
/// @return 已限制到安全范围的 Hz 值。
std::uint32_t CollaborationRoom::viewportPublishRateHz() const
{
    return m_viewportPublishRateHz;
}

/// @brief 获取当前会话有界诊断日志。
/// @return 按本地序列排序的只读日志引用。
const std::vector<CollaborationLogEntry>& CollaborationRoom::logs() const
{
    return m_logs;
}

const std::vector<CollaborationChatEntry>&
/// @brief 获取当前会话有界聊天记录。
/// @return 按接收顺序排列的只读记录引用。
CollaborationRoom::chatMessages() const
{
    return m_chatMessages;
}

/// @brief 获取导致当前 Error 或断线的最近原因。
/// @return 内部错误字符串引用。
const std::string& CollaborationRoom::lastError() const
{
    return m_lastError;
}

/// @brief 获取资源同步当前阶段与计数快照。
/// @return ResourceSync 的值语义进度对象。
CollaborationResourceSyncProgress CollaborationRoom::resourceProgress() const
{
    return m_resourceSync.progress();
}

/// @brief 在离线时维持目录连接，在活动房间中关闭独立目录客户端。
///
/// @details Connected/Connecting 状态只清理成功错误；Error 状态保存原因并断开，
/// 到达五秒截止时间后重新连接。活动会话不需要独立目录订阅，避免额外连接竞争。
/// @warning 每帧调用但不阻塞，重连通过截止时间非阻塞节流。
///
/// @par 错误保留
/// DirectoryClient Error 在 disconnect 前复制到
/// m_directoryError，确保重置客户端 状态后 UI 仍能展示原因。下一次 Connected
/// 才清空，Connecting 期间继续保留。
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

/// @brief 追加带稳定身份和会话相对时间的有界日志。
/// @param type 事件类型。
/// @param peerId 关联路由 ID。
/// @param creator 显示身份。
/// @param detail 稳定详情。
/// @param participantId 可选稳定身份，空时从参与者表推断。
///
/// @details 时间使用 steady_clock 相对
/// m_startedAt，避免系统时钟跳变。超过上限时
/// 一次删除最旧前缀，序列号保持单调，不因截断复用。
///
/// @par 身份补全
/// 未提供 participantId 时先按 peerId 查参与者表；本地 peer 使用房间稳定 ID。
/// 未知或尚未注册 peer 可保留空身份，但 creator/detail 仍提供阶段诊断。
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

/// @brief 把已验证 Peer 聊天消息转换为 UI 记录。
/// @param message 已由 CollaborationPeer 解码的消息。
///
/// @details 只接受当前参与者表中的 peer，身份和 creator 从权威表补齐，不信任
/// 消息自带显示字段。记录按会话相对时间编号，超过上限删除最旧前缀。
///
/// @note Peer 已完成聊天文本格式验证；Room 只绑定权威身份并维护 UI 有界历史，
///       不把聊天写入 BeatMap 或操作日志。
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

/// @brief 把底层传输生命周期事件映射为房间、Peer 与 UI 状态。
/// @param event WebRtcTransport 非阻塞队列交付的事件。
///
/// @details
/// 目录发布更新 roomId；访客 join_pending 进入 AwaitingApproval；房主对
/// JoinRequested 执行构建指纹策略并维护去重审批列表。PeerConnected 只有在房主
/// 已发布最新权威快照后才注册，否则暂存；访客则进入 Connected。
///
/// 房主侧 PeerDisconnected 移除参与者及资源清单发送记录。访客失去房主时保留
/// lastError 和 HostDisconnected 日志，再完整复位在线状态。Rejected 与 Error
/// 根据角色决定是否终止本地入队，避免房主单个访客错误摧毁整个房间。
///
/// @par 指纹拒绝
/// 严格房主比较完整 SHA-256 指纹，但日志只输出双方最多 12 字符前缀。拒绝在
/// 加入审批列表前完成，避免用户批准一个协议上注定不兼容的连接。
///
/// @par 连接注册
/// 房主把远端稳定身份、session 与 creator 一次性交给 Peer。注册失败会关闭底层
/// 连接；成功后才发送资源清单。访客分支只需把房间推进到 Connected。
///
/// @par 终止策略
/// 访客 Rejected 保留 Error 供 UI 展示；失去房主则 reset 为 Idle 并保留
/// lastError。 房主只有目录控制连接关闭才整体
/// Error，单个访客错误被隔离为参与者离开。
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
            // 严格模式在进入人工审批列表前拒绝构建不兼容访客。
            if ( m_requireMatchingBuildFingerprint &&
                 event.buildFingerprint != m_buildFingerprint ) {
                XWARN(
                    "Rejecting collaboration guest because build "
                    "fingerprints differ: host={} guest={}",
                    fingerprintPrefix(m_buildFingerprint),
                    fingerprintPrefix(event.buildFingerprint));
                static_cast<void>(m_transport->rejectJoinRequest(
                    event.requestId, "build_fingerprint_mismatch"));
                const std::string detail =
                    "build_fingerprint_mismatch host=" +
                    std::string(fingerprintPrefix(m_buildFingerprint)) +
                    " guest=" +
                    std::string(fingerprintPrefix(event.buildFingerprint));
                appendLog(
                    CollaborationLogEventType::Error, 0, event.creator, detail);
                break;
            }
            // 中心可能重发同一请求，本地按 requestId 去重，避免 UI 出现重复项。
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
            // 新访客必须等待房主最新 snapshot 发布，否则加入后会先看到旧文档。
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
            // 身份注册失败立即关闭该底层连接，不能留下未授权业务通道。
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
            // peerId 释放时同时允许未来重连者重新接收当前资源清单。
            std::erase_if(m_pendingPeerConnections,
                          [&event](const WebRtcTransportEvent& pending) {
                              return pending.peerId == event.peerId;
                          });
            m_peer->removeParticipant(event.peerId);
            m_resourceManifestRecipients.erase(event.peerId);
        } else if ( !m_isHost ) {
            // 访客只有房主连接，任意 PeerDisconnected 都结束整个在线会话。
            m_lastError = event.detail;
            appendLog(CollaborationLogEventType::HostDisconnected,
                      event.peerId,
                      event.creator,
                      event.detail,
                      event.participantId);
            resetOnlineSessionState();
            break;
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

/// @brief 在访客获得 peerId 后把暂存传输提升为 CollaborationPeer。
///
/// @details connectToHost 启动时 peerId 尚为零，因此传输先由 pendingTransport
/// 拥有。accepted 消息设置 ID 后，本函数构造
/// Peer、安装文档/资源/聊天/授权回调，
/// 并把传输所有权移动进去。构造失败清除观察指针并进入 Error。
void CollaborationRoom::ensureGuestPeer()
{
    if ( m_isHost || m_peer || !m_pendingTransport ) return;
    const PeerId peerId = m_pendingTransport->localPeerId();
    if ( peerId == 0 ) return;

    // 使用本次加入 sessionId，防止同一 participant 的旧回执被视为本地操作。
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

/// @brief 接收 Peer 已排序权威操作并投递后台文档流水线。
/// @param operation 含修订、稳定身份、会话 ID 和文档负载的提交。
///
/// @details 先由稳定 participantId 解析当前 peerId/creator。只有参与者和
/// sessionId 都匹配本地时才视为本次进程提交，并从 in-flight 队列按 payload
/// 找到序列。 远端提交会标记 UI
/// 应用等待；若仍有本地操作，后续交付必须重放本地状态。
///
/// @warning 此函数不解码文档，只做有界状态更新和无锁入队；完整应用在后台。
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
    // sessionId 同时匹配才能排除同一稳定身份旧连接产生的迟到操作。
    const bool originatedLocally = operation.participantId == m_participantId &&
                                   operation.sessionId == m_operationSessionId;
    bool       reapplyLocalState = false;
    std::uint64_t committedLocalMutationSequence = 0;
    {
        std::lock_guard lock(m_localOperationMutex);
        if ( !originatedLocally ) {
            // 远端权威状态交付前出现的本地编辑必须在后台结果上重放。
            m_remoteStateApplyPending = true;
            if ( !m_localOperationQueue.empty() ||
                 !m_inFlightLocalOperations.empty() ) {
                m_localStateNeedsRebase = true;
            }
        }
        if ( originatedLocally ) {
            // payload 匹配把权威回执关联到本地序列并从在途集合移除。
            const auto matching =
                std::find_if(m_inFlightLocalOperations.begin(),
                             m_inFlightLocalOperations.end(),
                             [&operation](const LocalOperation& pending) {
                                 return pending.payload == operation.payload;
                             });
            if ( matching != m_inFlightLocalOperations.end() ) {
                committedLocalMutationSequence = matching->sequence;
                m_latestCommittedLocalMutationSequence =
                    std::max(m_latestCommittedLocalMutationSequence,
                             committedLocalMutationSequence);
                m_inFlightLocalOperations.erase(matching);
            }
            reapplyLocalState       = m_localStateNeedsRebase;
            m_localStateNeedsRebase = false;
        }
    }
    RemoteOperationPipeline::Task task;
    task.operation                      = operation;
    task.peerId                         = peerId;
    task.creator                        = creator;
    task.host                           = m_isHost;
    task.reapplyLocalState              = reapplyLocalState;
    task.originatedLocally              = originatedLocally;
    task.committedLocalMutationSequence = committedLocalMutationSequence;
    m_remoteOperationPipeline->tasks.enqueue(std::move(task));
    scheduleRemoteOperationWorker();
}

/// @brief 房主按参与者权限和补丁分类授权入站编辑。
/// @param peerId 提交者路由 ID。
/// @param payload 待授权文档负载。
/// @return 已知远端、非快照且全部分类获授权时返回 true。
///
/// @details 授权前使用 inspect 解析实际 flags，不信任消息外围声明。访客、房主
/// 自身、未知 ID 和 snapshot 一律拒绝，只有房主可接受远端增量。
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

/// @brief 唤醒或创建唯一后台文档消费者。
///
/// @details CAS 把 0 切到 1 的线程负责投递 worker；发现 1 时只切到 2 表示有新
/// 工作，避免每条操作创建 future。若 AppThreadPool 不可用则记录错误并同步处理，
/// 保持功能正确但只作为异常降级路径。
///
/// @warning update 热路径只进行原子门闩与 future 就绪清理，不等待后台任务。
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

    // 正常应用生命周期必须已初始化共享线程池；缺失时同步处理避免丢任务。
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

/// @brief 在唯一后台消费者中批量应用权威操作并生成最新可见交付。
///
/// @details
/// tasks 持续排空到稳定边界，同一批所有成功操作都写入 operationLogs，但只对
/// 最新权威状态物化一次 BeatMap。远端操作或需要重基的本地回执才要求物化；纯
/// 本地回执可直接更新 visibleDocument，避免把 UI 已有状态重复回灌。
///
/// 仅 Objects 的交付会比较前后文档稳定 ID，并后台预计算对象编码基线。房主把
/// 最新权威文档标为 snapshot dirty，完整压缩最多每秒一次；普通增量不受此限。
///
/// workerState 在排空边界检查新唤醒：2->1 后继续合并，1->0 成功才真正退出，
/// 防止任务落在消费者退出与新 worker 创建之间。
///
/// @warning 该函数独占 m_documentCodec 与 visibleDocument，不得并发运行两份。
///
/// @par 批量聚合
/// batchVisibleFlags 只累计真正需要 UI 回灌的修订；纯本地回执不扩大刷新范围。
/// batchCommittedLocalMutationSequence 取最大确认值，lastSuccessfulResult
/// 指向最后 一个可用修订，失败 Task 仍保留日志但不取代交付身份。
///
/// @par 可见文档
/// visibleDocument 表示上次交给 UI 的规范 JSON。纯本地回执且本批尚无远端回灌
/// 时直接克隆最新权威文档，因为 UI 已经拥有相同编辑；一旦批内存在远端变化，
/// 直到完成重基后才更新，保证对象差量相对真正上次可见状态计算。
///
/// @par 空任务快照
/// snapshot dirty 到期但没有新 Task 时，消费者仍可生成独立 snapshotDelivery。
/// update 中的截止检查会唤醒 worker，从而无需伪造谱面操作来刷新加入者快照。
void CollaborationRoom::processRemoteOperations()
{
    std::vector<RemoteOperationPipeline::Result> batchResults;
    ::MMM::BeatmapMutationFlags                  batchVisibleFlags =
        ::MMM::BeatmapMutationFlags::None;
    std::uint64_t              batchCommittedLocalMutationSequence = 0;
    bool                       batchNeedsMaterialization           = false;
    bool                       batchHost                           = false;
    std::optional<std::size_t> lastSuccessfulResult;
    // 快照构建闭包只在 dirty 且截止时间到达时压缩最新 document。
    const auto buildHostSnapshotIfDue =
        [this](RemoteOperationPipeline::Result& delivery) {
            if ( !m_remoteOperationPipeline->hostSnapshotDirty.load(
                     std::memory_order_acquire) ) {
                return;
            }
            const auto now = steadyNowNanoseconds();
            const auto deadline =
                m_remoteOperationPipeline->nextHostSnapshotBuildNanoseconds
                    .load(std::memory_order_relaxed);
            if ( now < deadline ) return;

            m_remoteOperationPipeline->nextHostSnapshotBuildNanoseconds.store(
                now + std::chrono::duration_cast<std::chrono::nanoseconds>(
                          HOST_SNAPSHOT_REFRESH_INTERVAL)
                          .count(),
                std::memory_order_relaxed);
            auto snapshot = m_documentCodec.encodeCurrentSnapshot();
            if ( !snapshot.has_value() ) return;

            delivery.revision =
                m_remoteOperationPipeline->pendingHostSnapshotRevision.load(
                    std::memory_order_acquire);
            delivery.hostSnapshot.emplace(std::move(snapshot.value()));
            m_remoteOperationPipeline->hostSnapshotDirty.store(
                false, std::memory_order_release);
        };
    while ( true ) {
        RemoteOperationPipeline::Task task;
        // 一次排空中的中间修订仅记日志，最终可见状态在排空边界统一物化。
        while ( m_remoteOperationPipeline->tasks.try_dequeue(task) ) {
            RemoteOperationPipeline::Result result;
            result.peerId        = task.peerId;
            result.participantId = task.operation.participantId;
            result.creator       = std::move(task.creator);
            result.revision      = task.operation.revision;

            // Codec 失败不会推进文档；该修订只产生独立错误结果。
            auto patch = m_documentCodec.apply(task.operation.payload);
            if ( !patch.has_value() ) {
                result.error = "invalid_beatmap_operation";
                batchResults.push_back(std::move(result));
                continue;
            }
            m_hasDocument.store(true, std::memory_order_release);
            result.flags = patch->flags;
            batchHost    = task.host;
            batchCommittedLocalMutationSequence =
                std::max(batchCommittedLocalMutationSequence,
                         task.committedLocalMutationSequence);
            if ( !task.originatedLocally || task.reapplyLocalState ) {
                batchNeedsMaterialization = true;
                batchVisibleFlags |= patch->flags;
            } else if ( !batchNeedsMaterialization ) {
                // 本机提交已经存在于 ECS；在尚未混入待回灌远端提交时，直接把
                // 权威文档记为可见基线，避免下一名协作者编辑时重复回灌本机
                // 此前写入的全部物件。若本批已含远端提交则保留旧基线，确保
                // 远端差量不会被后续本机回执提前吞掉。
                m_remoteOperationPipeline->visibleDocument =
                    m_documentCodec.cloneDocument();
            }
            lastSuccessfulResult = batchResults.size();
            batchResults.push_back(std::move(result));
        }

        std::uint8_t expected = 2U;
        if ( m_remoteOperationPipeline->workerState.compare_exchange_strong(
                 expected,
                 1U,
                 std::memory_order_acq_rel,
                 std::memory_order_acquire) ) {
            // 有新任务在本轮排空期间到达时继续合并，不为即将过时的中间
            // revision 重复物化整张 BeatMap 或压缩房主快照。
            continue;
        }

        if ( lastSuccessfulResult ) {
            // 批量交付采用最后成功修订的身份，并合并全部需要回灌的 flags。
            const auto& latest = batchResults[*lastSuccessfulResult];
            RemoteOperationPipeline::Result delivery;
            delivery.logOperation  = false;
            delivery.peerId        = latest.peerId;
            delivery.participantId = latest.participantId;
            delivery.creator       = latest.creator;
            delivery.revision      = latest.revision;
            delivery.flags         = batchVisibleFlags;
            delivery.includedLocalMutationSequence =
                batchCommittedLocalMutationSequence;
            if ( batchNeedsMaterialization ) {
                // 在权威文档上重放仍未确认本地操作，保证 UI 即时编辑不倒退。
                auto rebased = materializeRebasedLocalBeatmap(
                    batchCommittedLocalMutationSequence);
                if ( !rebased ) {
                    delivery.error = "invalid_beatmap_document";
                } else {
                    const bool onlyObjects =
                        batchVisibleFlags ==
                        ::MMM::BeatmapMutationFlags::Objects;
                    // 只有纯 Objects 批次才能安全提供窄对象 ID 列表和编码基线。
                    if ( onlyObjects &&
                         m_remoteOperationPipeline->visibleDocument ) {
                        delivery.objectDeltaIdentities =
                            rebased->document
                                ->changedObjectIdentitiesComparedTo(
                                    *m_remoteOperationPipeline
                                         ->visibleDocument);
                        if ( delivery.objectDeltaIdentities ) {
                            delivery.objectEncodingBaseline =
                                BeatmapDocumentCodec::
                                    prepareObjectEncodingBaseline(
                                        *rebased->beatmap);
                            if ( !delivery.objectEncodingBaseline ) {
                                delivery.objectDeltaIdentities.reset();
                            }
                        }
                    }
                    m_remoteOperationPipeline->visibleDocument =
                        std::move(rebased->document);
                    delivery.beatmap = std::move(rebased->beatmap);
                    delivery.includedLocalMutationSequence =
                        rebased->includedLocalMutationSequence;
                }
            }
            // 房主成功应用新修订后安排低频完整快照供新访客或重同步使用。
            if ( batchHost && delivery.error.empty() ) {
                m_remoteOperationPipeline->pendingHostSnapshotRevision.store(
                    delivery.revision, std::memory_order_release);
                m_remoteOperationPipeline->hostSnapshotDirty.store(
                    true, std::memory_order_release);
            }
            buildHostSnapshotIfDue(delivery);
            m_remoteOperationPipeline->results.enqueue(std::move(delivery));
        } else if ( m_remoteOperationPipeline->hostSnapshotDirty.load(
                        std::memory_order_acquire) ) {
            RemoteOperationPipeline::Result snapshotDelivery;
            snapshotDelivery.logOperation = false;
            buildHostSnapshotIfDue(snapshotDelivery);
            if ( snapshotDelivery.hostSnapshot ) {
                m_remoteOperationPipeline->results.enqueue(
                    std::move(snapshotDelivery));
            }
        }
        // 每个权威修订仍独立进入日志队列，不因可见状态批量合并而丢审计记录。
        for ( auto& result : batchResults ) {
            m_remoteOperationPipeline->operationLogs.enqueue(std::move(result));
        }
        batchResults.clear();
        batchVisibleFlags                   = ::MMM::BeatmapMutationFlags::None;
        batchCommittedLocalMutationSequence = 0;
        batchNeedsMaterialization           = false;
        batchHost                           = false;
        lastSuccessfulResult.reset();

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

/// @brief 在逻辑更新线程有界消费后台谱面结果和逐操作日志。
///
/// @details 同一帧最多消费四个谱面交付和 256 个日志结果。多个 BeatMap 只保留
/// 最新值，flags 取并集，本地 sequence 取最大值；纯 Objects 的稳定 ID 差量可
/// 合并去重，任一结果缺失精确差量则整体退化为全对象刷新。
///
/// 房主快照成功交给 Peer 后更新 published revision。最终 BeatMap 通过回调进入
/// 逻辑命令队列；若只有本地确认而无需回灌，则调用 acknowledge 回调推进水位线。
///
/// @warning 这是每帧热路径，队列消费有界且不等待 future。
///
/// @par 对象差量交付
/// 精确 ID 列表只在最终 mergedFlags 恰好为 Objects 且对应预计算编码基线存在时
/// 保留。若同帧还包含元数据等分类，或任一中间结果无法计算身份差量，就清空
/// optional，要求上层执行安全的全对象刷新。
///
/// @par 基线握手
/// 后台编码基线先进入最多 16 项的 pending deque，并随 revision 和 included
/// local sequence 标记。上层应用完成后通过 onAuthoritativeBeatmapApplied
/// 精确匹配安装； 过旧未确认项会被有界淘汰，不阻塞正常谱面交付。
void CollaborationRoom::processRemoteOperationResults()
{
    std::shared_ptr<::MMM::BeatMap>         mergedBeatmap;
    std::optional<std::vector<std::string>> mergedObjectDeltaIdentities;
    std::shared_ptr<BeatmapDocumentCodec::ObjectEncodingBaseline>
                                mergedObjectEncodingBaseline;
    ::MMM::BeatmapMutationFlags mergedFlags = ::MMM::BeatmapMutationFlags::None;
    std::uint64_t               mergedLocalMutationSequence       = 0;
    std::uint64_t               acknowledgedLocalMutationSequence = 0;
    std::uint64_t               mergedRevision                    = 0;
    // 同一消费器统一处理 results 与 operationLogs，后者通常只写日志和确认。
    const auto consumeResult = [&](RemoteOperationPipeline::Result result) {
        if ( result.logOperation ) {
            appendLog(CollaborationLogEventType::OperationCommitted,
                      result.peerId,
                      result.creator,
                      std::to_string(result.revision),
                      result.participantId);
        }
        if ( !result.error.empty() ) {
            appendLog(CollaborationLogEventType::Error,
                      result.peerId,
                      std::move(result.creator),
                      std::move(result.error),
                      std::move(result.participantId));
            return;
        }
        // 只有仍为房主且 Peer 存活时发布后台压缩好的完整快照。
        if ( result.hostSnapshot && m_isHost && m_peer ) {
            if ( m_peer->setStateSnapshot(result.revision,
                                          std::move(*result.hostSnapshot)) ) {
                m_publishedDocumentRevision = result.revision;
            }
        }
        if ( result.beatmap ) {
            // 对象差量跨同帧交付求并集；无法证明精确时用 nullopt 表示全量刷新。
            if ( hasBeatmapMutationFlag(
                     result.flags, ::MMM::BeatmapMutationFlags::Objects) ) {
                if ( !mergedBeatmap ) {
                    mergedObjectDeltaIdentities =
                        std::move(result.objectDeltaIdentities);
                } else if ( mergedObjectDeltaIdentities &&
                            result.objectDeltaIdentities ) {
                    auto& merged = *mergedObjectDeltaIdentities;
                    merged.insert(merged.end(),
                                  std::make_move_iterator(
                                      result.objectDeltaIdentities->begin()),
                                  std::make_move_iterator(
                                      result.objectDeltaIdentities->end()));
                    std::sort(merged.begin(), merged.end());
                    merged.erase(std::unique(merged.begin(), merged.end()),
                                 merged.end());
                } else {
                    mergedObjectDeltaIdentities.reset();
                }
            }
            mergedFlags |= result.flags;
            mergedBeatmap = std::move(result.beatmap);
            mergedObjectEncodingBaseline =
                std::move(result.objectEncodingBaseline);
            mergedRevision = result.revision;
            mergedLocalMutationSequence =
                std::max(mergedLocalMutationSequence,
                         result.includedLocalMutationSequence);
        } else {
            acknowledgedLocalMutationSequence =
                std::max(acknowledgedLocalMutationSequence,
                         result.includedLocalMutationSequence);
        }
    };
    for ( std::size_t index = 0; index < MAX_REMOTE_RESULTS_PER_UPDATE;
          ++index ) {
        RemoteOperationPipeline::Result result;
        if ( !m_remoteOperationPipeline->results.try_dequeue(result) ) break;
        consumeResult(std::move(result));
    }
    for ( std::size_t index = 0; index < MAX_REMOTE_LOG_RESULTS_PER_UPDATE;
          ++index ) {
        RemoteOperationPipeline::Result result;
        if ( !m_remoteOperationPipeline->operationLogs.try_dequeue(result) ) {
            break;
        }
        consumeResult(std::move(result));
    }
    // 只有最新 BeatMap 回灌上层，中间修订的分类影响已合并进 mergedFlags。
    if ( mergedBeatmap && m_applyBeatmapCallback ) {
        if ( mergedFlags != ::MMM::BeatmapMutationFlags::Objects ||
             !mergedObjectEncodingBaseline ) {
            mergedObjectDeltaIdentities.reset();
        }
        // 同帧完成的连续修订只回灌最新领域快照，并合并其真实变更类别，避免
        // 逻辑线程为中间状态重复扫描 ECS。实际应用仍由逻辑命令队列串行完成，
        // 因而本地手势、交互状态和未确认增量不会交给后台线程。
        // 预计算基线要等上层确认实际应用同一 revision 后才能安装到本地 Codec。
        if ( mergedObjectEncodingBaseline && mergedObjectDeltaIdentities ) {
            std::lock_guard lock(m_localOperationMutex);
            m_pendingObjectEncodingBaselines.push_back(
                PendingObjectEncodingBaseline{
                    .revision = mergedRevision,
                    .includedLocalMutationSequence =
                        mergedLocalMutationSequence,
                    .baseline = std::move(mergedObjectEncodingBaseline),
                });
            constexpr std::size_t MAX_PENDING_OBJECT_BASELINES = 16U;
            while ( m_pendingObjectEncodingBaselines.size() >
                    MAX_PENDING_OBJECT_BASELINES ) {
                m_pendingObjectEncodingBaselines.pop_front();
            }
        }
        m_applyBeatmapCallback(std::move(mergedBeatmap),
                               mergedFlags,
                               mergedLocalMutationSequence,
                               mergedRevision,
                               std::move(mergedObjectDeltaIdentities));
    }
    if ( acknowledgedLocalMutationSequence != 0 &&
         m_localMutationAcknowledgedCallback ) {
        m_localMutationAcknowledgedCallback(acknowledgedLocalMutationSequence);
    }
}

/// @brief 在房主最新权威快照发布后重放延迟的 PeerConnected 事件。
///
/// @details 新访客若早于 snapshot 发布完成会暂存在 m_pendingPeerConnections。
/// publishedDocumentRevision 追上 Peer appliedRevision
/// 后，一次移出列表并重新经过 handleTransportEvent
/// 的标准注册和资源清单发送路径。
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

/// @brief 等待后台消费者退出并把文档流水线恢复为空状态。
///
/// @details 先等待全部有效 future，确保不再访问队列和 Codec；随后排空三条无锁
/// 队列、复位原子门闩、可见文档、快照状态与本地待安装对象基线。最后清零延迟
/// 连接、已发布修订并 reset 权威 Codec。
///
/// @warning 这是建房、加入、断开和析构的低频阻塞路径，禁止从每帧交互调用。
///
/// @par 原子复位
/// workerState 在 worker 全部完成后写回 0；snapshot dirty、pending revision 和
/// deadline 随后清零。由于此时不存在消费者，这些 release/relaxed 写入只为下一
/// 会话建立明确初态，不承担旧文档数据发布。
void CollaborationRoom::resetRemoteOperationPipeline()
{
    // future 完成前后台闭包仍捕获 this，必须先 join 再修改流水线成员。
    for ( auto& task : m_remoteOperationPipeline->workerFutures ) {
        if ( task.valid() ) task.wait();
    }
    m_remoteOperationPipeline->workerFutures.clear();
    RemoteOperationPipeline::Task pendingTask;
    while ( m_remoteOperationPipeline->tasks.try_dequeue(pendingTask) ) {}
    RemoteOperationPipeline::Result pendingResult;
    while ( m_remoteOperationPipeline->results.try_dequeue(pendingResult) ) {}
    while (
        m_remoteOperationPipeline->operationLogs.try_dequeue(pendingResult) ) {}
    m_remoteOperationPipeline->workerState.store(0U, std::memory_order_release);
    m_remoteOperationPipeline->visibleDocument.reset();
    m_remoteOperationPipeline->hostSnapshotDirty.store(
        false, std::memory_order_release);
    m_remoteOperationPipeline->pendingHostSnapshotRevision.store(
        0U, std::memory_order_release);
    m_remoteOperationPipeline->nextHostSnapshotBuildNanoseconds.store(
        0, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_localOperationMutex);
        m_pendingObjectEncodingBaselines.clear();
    }
    m_pendingPeerConnections.clear();
    m_publishedDocumentRevision = 0;
    m_documentCodec.reset();
}

/// @brief 按序向 Peer 提交本地编码队列并维护在途集合。
///
/// @details 每帧最多提交 256 条。提交前重新 inspect payload 并按最新本地权限
/// 授权；权限暂时收紧时保留队首及后续操作，恢复授权后仍按原顺序发送。Peer
/// Accepted 后才从 queued 移入 in-flight，等待权威回执关联序列。
///
/// 首次提交失败记录一次 local_operation_submit_failed
/// 并设置门闩，避免每帧刷屏；
/// 任一后续成功会清除门闩。失败不弹出队首，可在连接恢复后重试。
///
/// @warning 每帧热路径，循环和检查均有固定上限，不进行谱面物化。
///
/// @par 队列一致性
/// 网络提交前只复制队首，成功后重新加锁并比较 sequence 与 payload，确保观察
/// 线程的追加不会影响判断。队列只从尾部追加、头部弹出，因此匹配失败时保守
/// 保留原状态，不会错误确认其他操作。
///
/// @par 权限暂停
/// 未授权并不等同于无效 payload：操作保持在队首，后续也不能越过它提交，维持
/// 原始编辑顺序。只有 update 检测到权限实际收紧时，才按分类清理确定失权的操作。
void CollaborationRoom::submitQueuedLocalOperations()
{
    if ( !m_peer || !m_acceptLocalMutations.load(std::memory_order_acquire) ) {
        return;
    }
    for ( std::size_t index = 0; index < MAX_LOCAL_OPERATIONS_PER_UPDATE;
          ++index ) {
        // 锁内只复制队首快照，网络提交在锁外执行，避免阻塞观察线程入队。
        LocalOperation operation;
        {
            std::lock_guard lock(m_localOperationMutex);
            if ( m_localOperationQueue.empty() ) return;
            operation = m_localOperationQueue.front();
        }
        const auto patch = BeatmapDocumentCodec::inspect(operation.payload);
        if ( !patch || !permissionsAllowMutation(
                           m_localPermissions.load(std::memory_order_acquire),
                           patch->flags) ) {
            // 权限收紧时保留已经完成的本地编辑及其规范增量，不清空、不覆盖；
            // 房主重新授权后仍按原顺序提交。
            return;
        }
        // Peer 接受后再次核对队首序列和 payload，防止并发状态变化误移除其他项。
        const auto result = m_peer->submitOperation(operation.payload);
        if ( result == SubmitOperationResult::Accepted ) {
            std::lock_guard lock(m_localOperationMutex);
            if ( !m_localOperationQueue.empty() &&
                 m_localOperationQueue.front().sequence == operation.sequence &&
                 m_localOperationQueue.front().payload == operation.payload ) {
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

std::optional<CollaborationRoom::RebasedLocalBeatmap>
/// @brief 在最新权威文档上重放全部未确认本地操作并物化可见谱面。
/// @param committedLocalMutationSequence 本批权威提交已确认的最高本地序列。
/// @return 重基后的 BeatMap、Codec 文档与包含序列；任一步失败返回空。
///
/// @details 在锁内复制 in-flight 和 queued 两组操作并计算已确认水位线，随后锁外
/// 克隆权威 Codec。无待处理操作时直接物化克隆；否则严格按 in-flight 后 queued
/// 顺序 apply，每成功一项推进 included sequence，最后一次性物化。
///
/// @note 原队列不会被修改，重放只构造 UI 可见版本；权威回执仍负责正式移除。
///
/// @par 顺序语义
/// in-flight 已经早于 queued 发送，因此重放必须保持该拼接顺序。两组内部也按
/// 原 sequence 排列。includedLocalMutationSequence 取已提交水位线与每个成功重放
/// sequence 的最大值，供 UI 防止旧交付覆盖更新本地视图。
///
/// @par 失败原子性
/// 所有 apply 都作用于克隆 Codec，任何补丁或最终 materialize
/// 失败只销毁局部对象。 权威 m_documentCodec、本地队列和 UI 当前 BeatMap
/// 均保持不变，等待后续重同步。
CollaborationRoom::materializeRebasedLocalBeatmap(
    std::uint64_t committedLocalMutationSequence)
{
    std::vector<LocalOperation> pending;
    std::uint64_t               includedLocalMutationSequence =
        committedLocalMutationSequence;
    {
        std::lock_guard lock(m_localOperationMutex);
        includedLocalMutationSequence =
            std::max(includedLocalMutationSequence,
                     m_latestCommittedLocalMutationSequence);
        pending.reserve(m_inFlightLocalOperations.size() +
                        m_localOperationQueue.size());
        pending.insert(pending.end(),
                       m_inFlightLocalOperations.begin(),
                       m_inFlightLocalOperations.end());
        pending.insert(pending.end(),
                       m_localOperationQueue.begin(),
                       m_localOperationQueue.end());
    }

    // 即使无需重放也克隆 Codec，使 visibleDocument 与权威文档拥有独立所有权。
    if ( pending.empty() ) {
        auto document = m_documentCodec.cloneDocument();
        if ( !document ) return std::nullopt;
        auto beatmap = document->materialize();
        if ( !beatmap ) return std::nullopt;
        return RebasedLocalBeatmap{
            .beatmap                       = std::move(beatmap),
            .document                      = std::move(document),
            .includedLocalMutationSequence = includedLocalMutationSequence,
        };
    }

    auto localView = m_documentCodec.cloneDocument();
    if ( !localView ) return std::nullopt;
    // 任何本地补丁无法应用都取消整次交付，不能向 UI 暴露部分重放结果。
    for ( const auto& operation : pending ) {
        if ( !localView->apply(operation.payload).has_value() ) {
            return std::nullopt;
        }
        includedLocalMutationSequence =
            std::max(includedLocalMutationSequence, operation.sequence);
    }
    auto beatmap = localView->materialize();
    if ( !beatmap ) return std::nullopt;
    return RebasedLocalBeatmap{
        .beatmap                       = std::move(beatmap),
        .document                      = std::move(localView),
        .includedLocalMutationSequence = includedLocalMutationSequence,
    };
}

/// @brief 原子关闭观察线程入队并清空全部本地提交状态。
///
/// @details release store 先阻止无锁快速入口，随后在互斥锁下清除
/// queued、in-flight、 错误门闩、重基标志和序列水位线。onBeatmapMutated
/// 在取锁后会二次检查门闩， 因而不会在清理后重新入队。
void CollaborationRoom::stopAcceptingLocalMutations()
{
    m_acceptLocalMutations.store(false, std::memory_order_release);
    std::lock_guard lock(m_localOperationMutex);
    m_localOperationQueue.clear();
    m_inFlightLocalOperations.clear();
    m_localOperationSubmitBlocked          = false;
    m_localStateNeedsRebase                = false;
    m_remoteStateApplyPending              = false;
    m_nextLocalMutationSequence            = 1;
    m_latestCommittedLocalMutationSequence = 0;
}

/// @brief 在变化阈值和频率限制允许时发布最新本地视口。
///
/// @details 只保留 m_pendingLocalViewport 最新值。deferred 状态用于拖拽期间暂停
/// 网络发送但不延迟本地视觉；未达到变化阈值或截止时间时立即返回。发送成功后
/// 才更新 lastPublished 和下一截止时间，失败允许下一帧重试。
///
/// @warning 每帧热路径，仅做常数次比较和一次可选 Peer 入队，不阻塞等待网络。
void CollaborationRoom::flushLocalViewport()
{
    if ( !m_peer || !m_pendingLocalViewport ||
         m_localViewportPublishDeferred ) {
        return;
    }
    if ( m_lastPublishedLocalViewport &&
         !viewportChanged(*m_lastPublishedLocalViewport,
                          *m_pendingLocalViewport) ) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if ( now < m_nextViewportPublish ) return;

    // 复制最新值交给 Peer，发送失败不覆盖已发布基线。
    ParticipantViewport viewport = *m_pendingLocalViewport;
    if ( !m_peer->publishViewport(viewport) ) return;
    m_lastPublishedLocalViewport = viewport;
    const auto interval          = std::chrono::microseconds(
        1'000'000 / std::max<std::uint32_t>(1, m_viewportPublishRateHz));
    m_nextViewportPublish = now + interval;
}

/// @brief 按 variant 类型把 Peer 资源消息路由到 ResourceSync。
/// @param senderId 请求或分块的远端 peer ID。
/// @param message 已由 Peer 解码的资源协议消息。
/// @note Manifest/Chunk 由访客消费，Request 携带 senderId 由房主响应。
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

/// @brief 每帧有界消费资源同步器事件并经 Peer 发送或交付 bundle。
///
/// @details ManifestReady 缓存新清单并向所有远端参与者发送；SendRequest
/// 固定发给 房主；SendChunk 发给事件指定访客；BundleReady
/// 写日志后移动交付；Error 更新
/// lastError。发送失败记录稳定原因，但不在本函数阻塞重试。
///
/// @warning 每帧最多处理 256 项，资源文件 I/O 由 ResourceSync 后台路径承担。
///
/// @par ManifestReady
/// 房主保存新清单，遍历参与者并跳过本地 peer。成功接收者进入 recipients；同一
/// 清单不重复发送，新准备资源则会清空集合。
///
/// @par 请求与分块
/// SendRequest 固定发往 DEFAULT_HOST_ID。SendChunk 使用事件携带
/// peerId，允许房主 同时服务多个访客；发送失败分别记录 request/chunk
/// 稳定错误码。
///
/// @par 完成交付
/// BundleReady 表示摘要、分块和落盘全部结束，先记录完成日志，再移动给产品回调。
/// 没有回调时仍消费事件，不在房间中保留大型结果。
void CollaborationRoom::processResourceEvents()
{
    constexpr std::size_t MAX_RESOURCE_EVENTS_PER_UPDATE = 256U;
    for ( std::size_t index = 0; index < MAX_RESOURCE_EVENTS_PER_UPDATE;
          ++index ) {
        CollaborationResourceSyncEvent event;
        if ( !m_resourceSync.pollEvent(event) ) return;
        // 每种事件只做轻量路由，完整资源内容保持在消息或 bundle
        // 的移动所有权中。
        switch ( event.type ) {
        case CollaborationResourceSyncEvent::Type::ManifestReady: {
            const auto* manifest =
                std::get_if<ResourceManifest>(&event.message);
            if ( !manifest ) break;
            m_resourceManifest = *manifest;
            // 新清单发布给当前全部远端；后加入者由 PeerConnected 路径补发。
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

/// @brief 向单个访客幂等发送当前资源清单。
/// @param peerId 目标访客。
///
/// @details 只有房主、Peer 和 manifest 同时存在才发送。成功后记录接收者，避免
/// update 或重复连接事件重复发送；失败不记录，以便后续调用重试。
///
/// @note recipients 以当前会话 peerId 为键；参与者断开时会删除记录，槽位复用的
///       新连接仍可收到清单。
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

/// @brief 把不可恢复的房间启动或 Peer 错误转换为 Error 状态。
/// @param message 稳定错误标识。
///
/// @details 首先停止并清空本地提交，撤销待审批请求，再保存 lastError
/// 并追加日志。 连接所有权由调用路径或后续 disconnect
/// 清理，便于错误状态保留诊断信息。
///
/// @note fail 不立即 resetOnlineSessionState，因为 UI 需要 Error
/// 和会话日志；用户
///       显式 disconnect 后才完全释放并回到 Idle。
void CollaborationRoom::fail(std::string message)
{
    stopAcceptingLocalMutations();
    m_pendingJoinRequests.clear();
    m_state     = CollaborationRoomState::Error;
    m_lastError = message;
    appendLog(CollaborationLogEventType::Error, 0, {}, std::move(message));
}
}  // namespace MMM::Network::Collaboration
