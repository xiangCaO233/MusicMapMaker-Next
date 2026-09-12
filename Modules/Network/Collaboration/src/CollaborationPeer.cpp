#include "network/collaboration/CollaborationPeer.h"
#include "config/CreatorIdentity.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace MMM::Network::Collaboration
{
/// @brief 创建房主或访客的权威增量同步状态机。
/// @param config 身份、角色和所有有界处理参数。
/// @param transport 当前 Peer 独占的可靠有序传输端点。
/// @param applyCallback 已提交操作的本地应用入口。
/// @param resourceCallback 已校验资源消息的产品层入口。
/// @param chatCallback 已校验聊天消息的产品层入口。
/// @param authorizeEditCallback 房主提交前的细分编辑授权入口。
CollaborationPeer::CollaborationPeer(
    CollaborationPeerConfig                  config,
    std::unique_ptr<ICollaborationTransport> transport,
    ApplyOperationCallback                   applyCallback,
    ResourceMessageCallback resourceCallback, ChatMessageCallback chatCallback,
    AuthorizeEditCallback authorizeEditCallback)
    : m_config(std::move(config))
    , m_transport(std::move(transport))
    , m_applyCallback(std::move(applyCallback))
    , m_resourceCallback(std::move(resourceCallback))
    , m_chatCallback(std::move(chatCallback))
    , m_authorizeEditCallback(std::move(authorizeEditCallback))
{
    // 房间容量先钳制到协议支持范围，避免配置绕过 2～8 人约束。
    m_config.maxParticipants = std::clamp(m_config.maxParticipants,
                                          MIN_COLLABORATION_PARTICIPANTS,
                                          MAX_COLLABORATION_PARTICIPANTS);
    // 单操作上限同时约束编辑、快照和资源协议帧。
    // 所有每次更新及队列上限至少为一，防止状态机永久停止推进。
    m_config.limits.maxOperationBytes =
        std::max<std::size_t>(1, m_config.limits.maxOperationBytes);
    // 每轮消息上限限制传输接收和解码工作量。
    m_config.limits.maxMessagesPerUpdate =
        std::max<std::size_t>(1, m_config.limits.maxMessagesPerUpdate);
    // 每轮请求上限限制房主授权、应用和广播工作量。
    m_config.limits.maxRequestsPerUpdate =
        std::max<std::size_t>(1, m_config.limits.maxRequestsPerUpdate);
    // 待处理上限为接收洪泛提供固定内存边界。
    m_config.limits.maxPendingRequests =
        std::max<std::size_t>(1, m_config.limits.maxPendingRequests);
    // 日志上限决定最远可通过纯增量补发恢复的窗口。
    m_config.limits.maxJournalOperations =
        std::max<std::size_t>(1, m_config.limits.maxJournalOperations);
    // 展示身份和两个稳定标识在进入任何状态表前统一规范化。
    m_config.creator = Config::normalizeCreatorIdentity(m_config.creator);
    m_config.participantId =
        Config::normalizeCollaborationStableId(m_config.participantId);
    m_config.sessionId =
        Config::normalizeCollaborationStableId(m_config.sessionId);

    // 身份完整性与角色拓扑分别验证，便于保持 hostPeerId 不变量。
    const bool identityValid =
        m_config.peerId != 0 && m_config.hostPeerId != 0 &&
        !m_config.participantId.empty() && !m_config.sessionId.empty() &&
        !m_config.creator.empty();
    const bool roleValid = m_config.isHost
                               ? m_config.peerId == m_config.hostPeerId
                               : m_config.peerId != m_config.hostPeerId;
    // 有效 Peer 还必须拥有传输端点；回调可按功能选择为空。
    m_valid = identityValid && roleValid && m_transport != nullptr;
    if ( m_valid ) {
        // 本地身份始终进入镜像身份表，后续消息可统一按 peerId 查询。
        m_participantIdentities.emplace(
            m_config.peerId,
            ParticipantIdentity{ m_config.peerId,
                                 m_config.participantId,
                                 m_config.sessionId,
                                 m_config.creator });
        // 房主固定拥有全部权限；访客等待房主广播其正式权限快照。
        m_participantPermissions.emplace(
            m_config.peerId,
            m_config.isHost ? COLLABORATION_PERMISSION_ALL : 0U);
    }
}

/// @brief 释放 Peer 及其独占传输端点和回调对象。
CollaborationPeer::~CollaborationPeer() = default;

/// @brief 返回构造时身份、角色和传输是否满足前置条件。
/// @return 可处理消息时返回 true。
/// @warning 逻辑热路径可调用，只读取固定布尔值。
bool CollaborationPeer::isValid() const
{
    return m_valid;
}

/// @brief 返回本 Peer 是否承担房主权威排序职责。
/// @return 构造配置中的角色值。
/// @warning 逻辑热路径可调用，只读取不可变配置。
bool CollaborationPeer::isHost() const
{
    return m_config.isHost;
}

/// @brief 返回本连接的临时路由槽位。
/// @return 非零 PeerId；无效对象可能保留原配置值。
PeerId CollaborationPeer::localPeerId() const
{
    return m_config.peerId;
}

/// @brief 返回规范化的持久参与者标识。
/// @return 与当前 Peer 生命周期相同的只读引用。
const ParticipantId& CollaborationPeer::localParticipantId() const
{
    return m_config.participantId;
}

/// @brief 返回规范化的本次操作会话标识。
/// @return 用于客户端序号去重的只读会话 ID。
const OperationSessionId& CollaborationPeer::localSessionId() const
{
    return m_config.sessionId;
}

/// @brief 返回已经连续应用的最高权威修订号。
/// @return 初始为零，提交或快照应用后单调增加。
/// @warning 逻辑热路径可调用，不执行同步或容器访问。
std::uint64_t CollaborationPeer::appliedRevision() const
{
    return m_appliedRevision;
}

/// @brief 返回累计的协议与传输诊断计数。
/// @return 由当前逻辑线程维护的只读统计引用。
const CollaborationPeerStats& CollaborationPeer::stats() const
{
    return m_stats;
}

/// @brief 返回 PeerId 到规范化协作者身份的当前镜像。
/// @return 只读身份表引用。
/// @warning 引用仅在下一次可能修改身份表的 update 或成员操作前稳定。
const std::unordered_map<PeerId, ParticipantIdentity>&
CollaborationPeer::participantIdentities() const
{
    return m_participantIdentities;
}

/// @brief 返回每名参与者最近接受的视口状态。
/// @return 以 PeerId 索引的只读状态表。
/// @warning 引用仅供当前逻辑帧观察，不得跨 update 缓存元素地址。
const std::unordered_map<PeerId, ParticipantViewport>&
CollaborationPeer::participantViewports() const
{
    return m_participantViewports;
}

/// @brief 返回房主发布并由访客镜像的权限状态。
/// @return 以 PeerId 索引的只读权限表。
/// @warning 引用会在成员变更或权限消息处理时失效。
const std::unordered_map<PeerId, CollaborationPermissionMask>&
CollaborationPeer::participantPermissions() const
{
    return m_participantPermissions;
}

/// @brief 房主登记新访客并同步既有身份、视口、权限和快照。
/// @param peerId 新连接的临时路由槽位。
/// @param participantId 跨重连稳定的参与者标识。
/// @param sessionId 本次加入流程独有的操作会话标识。
/// @param creator 访客展示身份。
/// @return 身份合法、容量允许且不存在冲突时返回 true。
bool CollaborationPeer::addParticipant(PeerId             peerId,
                                       ParticipantId      participantId,
                                       OperationSessionId sessionId,
                                       std::string        creator)
{
    // 所有外部身份先规范化，比较与存储都使用唯一表示。
    creator       = Config::normalizeCreatorIdentity(creator);
    participantId = Config::normalizeCollaborationStableId(participantId);
    sessionId     = Config::normalizeCollaborationStableId(sessionId);
    // 仅有效房主能登记非自身、非零且身份完整的访客。
    if ( !m_valid || !m_config.isHost || peerId == 0 ||
         peerId == m_config.peerId || participantId.empty() ||
         sessionId.empty() || creator.empty() ) {
        return false;
    }
    // 相同 PeerId 的重复登记只有在全部身份字段一致时才幂等成功。
    if ( m_participants.contains(peerId) ) {
        const auto identity = m_participantIdentities.find(peerId);
        return identity != m_participantIdentities.end() &&
               identity->second.participantId == participantId &&
               identity->second.sessionId == sessionId &&
               identity->second.creator == creator;
    }
    // m_participants 只含访客，因此加一计入房主自身后检查总容量。
    if ( (m_participants.size() + 1) >= m_config.maxParticipants ) {
        return false;
    }
    // ParticipantId 与 SessionId 都必须在当前房间内唯一。
    const bool stableIdentityConflict =
        std::any_of(m_participantIdentities.begin(),
                    m_participantIdentities.end(),
                    [&participantId, &sessionId](const auto& entry) {
                        // 任一稳定维度重复都可能破坏来源识别或请求去重。
                        return entry.second.participantId == participantId ||
                               entry.second.sessionId == sessionId;
                    });
    if ( stableIdentityConflict ) return false;

    // 在把访客加入广播集合前，先向它发送所有既有身份。
    for ( const auto& [knownPeerId, identity] : m_participantIdentities ) {
        // map 键与消息内 peerId 一致，此循环只需发送值对象。
        static_cast<void>(knownPeerId);
        static_cast<void>(sendMessage(peerId, identity));
    }
    // 同步发送失败会进入 sendFailures，但成员登记仍继续保持拓扑收敛。
    // 已发布视口作为初始在线状态一并补给新访客。
    for ( const auto& [knownPeerId, viewport] : m_participantViewports ) {
        static_cast<void>(knownPeerId);
        static_cast<void>(sendMessage(peerId, viewport));
    }
    // 未发布视口的成员不会产生占位消息。
    // 权限快照必须先于访客开始提交编辑，避免短暂采用错误默认值。
    for ( const auto& [knownPeerId, permissions] : m_participantPermissions ) {
        static_cast<void>(sendMessage(
            peerId, ParticipantPermissions{ knownPeerId, permissions }));
    }
    // 初始同步顺序固定为身份、视口、权限，最后才可能发送状态快照。

    // 完成初始单播后再登记参与者，使后续广播包含新访客。
    m_participants.insert(peerId);
    // 新访客尚未确认任何修订，从零开始跟踪。
    m_lastAcknowledgedRevision.try_emplace(peerId, 0);
    // 移动规范化字符串进入权威身份记录，避免额外分配。
    const ParticipantIdentity identity{ peerId,
                                        std::move(participantId),
                                        std::move(sessionId),
                                        std::move(creator) };
    m_participantIdentities.emplace(peerId, identity);
    // 当前协议默认新访客拥有全部已知权限，房主可随后收窄。
    m_participantPermissions.emplace(peerId, COLLABORATION_PERMISSION_ALL);
    // 向所有访客广播新身份与权限，新访客也会收到自身权威镜像。
    for ( const PeerId participantId : m_participants ) {
        static_cast<void>(sendMessage(participantId, identity));
        static_cast<void>(sendMessage(
            participantId,
            ParticipantPermissions{ peerId, COLLABORATION_PERMISSION_ALL }));
    }
    // 两条广播各自独立，部分发送失败不会撤销已发送或本地登记。
    // 若房主已有可独立恢复快照，新访客可跳过日志起点限制直接追平。
    if ( m_stateSnapshot ) {
        static_cast<void>(sendMessage(peerId, *m_stateSnapshot));
    }
    // 单次发送失败由统计记录，不回滚已建立的房间成员身份。
    return true;
}

/// @brief 用当前已应用修订保存房主状态快照。
/// @param payload 可独立恢复的完整文档字节。
/// @return 转发到显式修订重载后的结果。
bool CollaborationPeer::setStateSnapshot(ByteBuffer payload)
{
    // 捕获调用时的权威修订，避免快照声明未来版本。
    return setStateSnapshot(m_appliedRevision, std::move(payload));
}

/// @brief 保存指定权威修订对应的最新完整状态快照。
/// @param revision 快照实际物化时的修订号。
/// @param payload 可独立恢复且受操作上限约束的文档字节。
/// @return 仅有效房主、非回退修订和合法负载时返回 true。
bool CollaborationPeer::setStateSnapshot(std::uint64_t revision,
                                         ByteBuffer    payload)
{
    // 快照不能来自访客、未来修订、旧于已有快照或空/超限负载。
    if ( !m_valid || !m_config.isHost || revision == 0 ||
         revision > m_appliedRevision ||
         (m_stateSnapshot && revision < m_stateSnapshot->revision) ||
         payload.empty() ||
         payload.size() > m_config.limits.maxOperationBytes ) {
        return false;
    }
    // 按值移动替换旧快照，使新加入和日志缺口访客只收到最新恢复点。
    m_stateSnapshot = StateSnapshot{ revision, std::move(payload) };
    return true;
}

/// @brief 房主移除访客并清理其会话相关的全部状态。
/// @param peerId 待移除访客的临时路由槽位。
/// @warning 成员生命周期低频路径，会线性清理待处理请求并向访客广播。
void CollaborationPeer::removeParticipant(PeerId peerId)
{
    // 非房主或未知访客不产生广播和状态变化。
    if ( !m_config.isHost || !m_participants.contains(peerId) ) {
        return;
    }
    // 先从广播集合移除，离开通知不会再尝试发送给已退役端点。
    m_participants.erase(peerId);
    const auto identity = m_participantIdentities.find(peerId);
    if ( identity != m_participantIdentities.end() ) {
        // 清除该加入会话尚未排序的请求，避免离线访客操作稍后被提交。
        const auto sessionId = identity->second.sessionId;
        std::erase_if(m_pendingRequests,
                      [&sessionId](const EditRequest& request) {
                          // 只移除精确会话请求，不影响同一人未来重连的新会话。
                          return request.sessionId == sessionId;
                      });
        // 去重水位随会话退役，新的唯一 SessionId 从零开始。
        m_lastAcceptedSequence.erase(sessionId);
    }
    // 确认、视口、聊天序号和权限均以临时 PeerId 为生命周期边界。
    m_lastAcknowledgedRevision.erase(peerId);
    m_participantViewports.erase(peerId);
    m_lastChatSequence.erase(peerId);
    m_participantPermissions.erase(peerId);
    // 身份已经缺失说明状态不完整，此时不再广播重复离开通知。
    if ( m_participantIdentities.erase(peerId) == 0 ) {
        return;
    }
    // 只向仍在线访客广播已离开的路由槽位。
    const ParticipantLeft participantLeft{ peerId };
    for ( const PeerId participantId : m_participants ) {
        // 离开通知发送失败不恢复已删除成员，其他客户端可由连接层重同步。
        static_cast<void>(sendMessage(participantId, participantLeft));
    }
}

/// @brief 房主替换在线访客权限并广播完整掩码。
/// @param peerId 目标访客。
/// @param permissions 只含当前协议已知位的新权限集合。
/// @return 目标与掩码合法且广播已启动时返回 true。
/// @warning 管理操作低频路径，广播量受最多八名参与者限制。
bool CollaborationPeer::setParticipantPermissions(
    PeerId peerId, CollaborationPermissionMask permissions)
{
    // 房主自身权限固定为全部，不允许通过访客管理入口修改。
    if ( !m_valid || !m_config.isHost || peerId == m_config.peerId ||
         !m_participants.contains(peerId) ||
         !isCollaborationPermissionMaskValid(permissions) ) {
        return false;
    }
    // 先更新权威表，再构造广播消息，确保本地查询立即一致。
    m_participantPermissions.insert_or_assign(peerId, permissions);
    const ParticipantPermissions message{ peerId, permissions };
    // 广播消息携带完整值，访客无需依赖旧权限计算增量。
    for ( const PeerId participantId : m_participants ) {
        // 广播包含目标本人，所有访客镜像相同完整权限表。
        static_cast<void>(sendMessage(participantId, message));
    }
    return true;
}

/// @brief 把本地增量操作提交给房主权威排序路径。
/// @param payload 与 UI/ECS 状态解耦的规范操作字节。
/// @return 入队、发送或输入失败的精确结果。
/// @warning 交互提交路径调用；只复制有界负载并非阻塞入队或发送。
SubmitOperationResult CollaborationPeer::submitOperation(
    std::span<const std::uint8_t> payload)
{
    // 构造无效对象不能产生序号或传输副作用。
    if ( !m_valid ) {
        return SubmitOperationResult::InvalidPeer;
    }
    // 空操作没有可应用语义，单独报告给调用方。
    if ( payload.empty() ) {
        return SubmitOperationResult::EmptyOperation;
    }
    // 在复制前应用配置上限，避免交互输入驱动无界分配。
    if ( payload.size() > m_config.limits.maxOperationBytes ) {
        return SubmitOperationResult::OperationTooLarge;
    }

    // 身份和会话来自已规范化配置，序号只在接受成功后递增。
    EditRequest request;
    request.participantId  = m_config.participantId;
    request.sessionId      = m_config.sessionId;
    request.clientSequence = m_nextClientSequence;
    // span 仅在调用期间有效，进入队列或传输前复制到消息所有权。
    request.payload.assign(payload.begin(), payload.end());

    if ( m_config.isHost ) {
        // 房主本地操作也走相同有界请求队列，保持统一 revision 顺序。
        if ( !enqueueHostRequest(std::move(request)) ) {
            return SubmitOperationResult::QueueFull;
        }
    } else if ( !sendMessage(m_config.hostPeerId, request) ) {
        // 访客只向权威房主发送，失败时保留当前序号供调用方重试。
        return SubmitOperationResult::TransportUnavailable;
    }

    // 只有请求已进入房主队列或传输层后才消费本地序号。
    ++m_nextClientSequence;
    return SubmitOperationResult::Accepted;
}

/// @brief 校验并提交本地单行聊天消息。
/// @param text 用户输入的 UTF-8 正文。
/// @return 接受、无效或传输失败结果。
/// @warning 用户提交路径；仅执行一次有界协议编码和发送。
SubmitChatMessageResult CollaborationPeer::submitChatMessage(std::string text)
{
    // 无效 Peer 不触发协议编码，保持聊天序号不变。
    if ( !m_valid ) return SubmitChatMessageResult::InvalidPeer;

    // 本地身份与当前待消费序号覆盖调用方不可控字段。
    CollaborationChatMessage chat;
    chat.peerId   = m_config.peerId;
    chat.sequence = m_nextChatSequence;
    chat.text     = std::move(text);
    // 复用正式编码器执行 UTF-8、控制字符和长度的完整校验。
    if ( !encodeCollaborationMessage(chat,
                                     m_config.limits.maxOperationBytes) ) {
        return SubmitChatMessageResult::InvalidMessage;
    }

    if ( m_config.isHost ) {
        // 房主消息直接进入统一处理器，再由处理器回调并广播。
        handleChatMessage(m_config.peerId, chat);
        // 本地处理器会验证身份表并通过相同回调/广播路径发布。
    } else if ( !sendMessage(m_config.hostPeerId, chat) ) {
        // 发送失败时不消费序号，允许上层用同一语义重试。
        return SubmitChatMessageResult::TransportUnavailable;
    }
    // 接受后递增，保证同一 Peer 的聊天序号单调。
    ++m_nextChatSequence;
    return SubmitChatMessageResult::Accepted;
}

/// @brief 发布本地最新视口，并按房主星型拓扑转发。
/// @param viewport 不含可信 peerId 和 sequence 的本地视口值。
/// @return 输入有限且目标发送均成功时返回 true。
/// @warning 高频交互路径；禁止增加等待、文件 I/O 或全表排序。
bool CollaborationPeer::publishViewport(ParticipantViewport viewport)
{
    // 本地入口先拒绝所有非有限浮点值，范围细校验由协议编码器完成。
    if ( !m_valid || !std::isfinite(viewport.playbackTime) ||
         !std::isfinite(viewport.visualTime) ||
         !std::isfinite(viewport.visibleTimeStart) ||
         !std::isfinite(viewport.visibleTimeEnd) ||
         !std::isfinite(viewport.horizontalOffsetRatio) ) {
        return false;
    }

    // 路由身份和单调序号始终由 Peer 覆盖，调用方不能伪造。
    viewport.peerId   = m_config.peerId;
    viewport.sequence = m_nextViewportSequence++;
    m_participantViewports.insert_or_assign(m_config.peerId, viewport);
    if ( m_config.isHost ) {
        // 房主向全部访客广播，并累计而不因单个失败停止其他发送。
        bool sent = true;
        for ( const PeerId participantId : m_participants ) {
            sent = sendMessage(participantId, viewport) && sent;
        }
        return sent;
    }
    // 访客只把视口发给房主，由房主验证来源并转发。
    return sendMessage(m_config.hostPeerId, viewport);
}

/// @brief 按角色白名单发送已经由资源状态机构造的消息。
/// @param recipientId 资源消息目标。
/// @param message 允许的清单、请求或分块变体。
/// @return 角色、目标、类型、编码与发送均通过时返回 true。
/// @warning 资源后台路径调用，函数本身不执行文件 I/O。
bool CollaborationPeer::sendResourceMessage(PeerId recipientId,
                                            const CollaborationMessage& message)
{
    if ( !m_valid ) return false;
    if ( m_config.isHost ) {
        // 房主只能向已登记访客发送清单或资源分块。
        if ( !m_participants.contains(recipientId) ||
             (!std::holds_alternative<ResourceManifest>(message) &&
              !std::holds_alternative<ResourceChunk>(message)) ) {
            return false;
        }
    } else if ( recipientId != m_config.hostPeerId ||
                !std::holds_alternative<ResourceRequest>(message) ) {
        // 访客只能向房主发送资源请求，不能伪造清单或分块。
        return false;
    }
    return sendMessage(recipientId, message);
}

/// @brief 有界接收消息并在房主端有界提交待处理请求。
/// @warning 每次逻辑 update 调用；禁止等待、文件系统访问和无界容器遍历。
void CollaborationPeer::update()
{
    // 无效构造对象保持完全惰性，不访问空传输端点。
    if ( !m_valid ) {
        return;
    }

    // 每轮最多消费配置数量的传输包，限制网络洪泛占用逻辑帧。
    for ( std::size_t index = 0; index < m_config.limits.maxMessagesPerUpdate;
          ++index ) {
        TransportPacket packet;
        // 非阻塞传输无可用消息时立即结束本轮。
        if ( !m_transport->receive(packet) ) {
            break;
        }
        // 线协议解析在分派前应用统一操作负载上限。
        auto message = decodeCollaborationMessage(
            packet.payload, m_config.limits.maxOperationBytes);
        if ( !message.has_value() ) {
            // 无效消息只累计诊断并丢弃，不改变房间状态。
            ++m_stats.invalidMessages;
            continue;
        }
        // 分派器再根据当前角色和真实 senderId 验证消息方向。
        handleMessage(packet.senderId, message.value());
    }

    if ( m_config.isHost ) {
        // 房主在接收完成后提交有界请求，形成确定的本轮全局顺序。
        processHostRequests();
    }
}

/// @brief 将已验证编辑请求加入房主有界队列。
/// @param request 拥有负载的请求值。
/// @return 队列尚有容量时返回 true。
bool CollaborationPeer::enqueueHostRequest(EditRequest request)
{
    // 队列只由逻辑线程访问，不需要锁或跨线程所有权同步。
    // 入队前检查硬上限，避免接收速度超过处理速度时无界增长。
    if ( m_pendingRequests.size() >= m_config.limits.maxPendingRequests ) {
        return false;
    }
    // 移动负载所有权，避免大型操作额外复制。
    m_pendingRequests.push_back(std::move(request));
    return true;
}

/// @brief 根据本地角色和传输确认的发送者分派消息。
/// @param senderId 不能由消息负载伪造的直接发送方。
/// @param message 已通过线协议解码的类型安全消息。
void CollaborationPeer::handleMessage(PeerId                      senderId,
                                      const CollaborationMessage& message)
{
    // 分派过程不复制消息变体，可变负载仅在后续入队时按需复制。
    if ( m_config.isHost ) {
        // 房主只接受请求、确认、补发、视口、聊天和访客资源请求。
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
            // 房主处理器负责把最新视口继续广播给其他访客。
        } else if ( const auto* chat =
                        std::get_if<CollaborationChatMessage>(&message) ) {
            handleChatMessage(senderId, *chat);
            // 房主处理器在去重后统一触发聊天回调与广播。
        } else if ( std::holds_alternative<ResourceRequest>(message) &&
                    m_participants.contains(senderId) && m_resourceCallback ) {
            // 资源请求必须来自已登记访客且产品层已安装处理回调。
            m_resourceCallback(senderId, message);
        } else {
            // 方向错误、未知参与者或缺少必需回调均计为无效消息。
            ++m_stats.invalidMessages;
        }
        return;
    }

    // 访客只接受房主提交、成员状态、快照和房主资源数据。
    if ( const auto* committed = std::get_if<CommittedOperation>(&message) ) {
        // 访客提交应用器会验证房主来源与连续 revision。
        handleCommittedOperation(senderId, *committed);
    } else if ( const auto* identity =
                    std::get_if<ParticipantIdentity>(&message) ) {
        handleParticipantIdentity(senderId, *identity);
    } else if ( const auto* participantLeft =
                    std::get_if<ParticipantLeft>(&message) ) {
        handleParticipantLeft(senderId, *participantLeft);
    } else if ( const auto* snapshot = std::get_if<StateSnapshot>(&message) ) {
        // 快照仅用于日志不足时直接追平，不进入普通增量队列。
        handleStateSnapshot(senderId, *snapshot);
    } else if ( const auto* viewport =
                    std::get_if<ParticipantViewport>(&message) ) {
        handleParticipantViewport(senderId, *viewport);
    } else if ( const auto* chat =
                    std::get_if<CollaborationChatMessage>(&message) ) {
        handleChatMessage(senderId, *chat);
    } else if ( const auto* permissions =
                    std::get_if<ParticipantPermissions>(&message) ) {
        handleParticipantPermissions(senderId, *permissions);
    } else if ( (std::holds_alternative<ResourceManifest>(message) ||
                 std::holds_alternative<ResourceChunk>(message)) &&
                senderId == m_config.hostPeerId && m_resourceCallback ) {
        // 清单与分块必须直接来自权威房主，随后交给资源同步器。
        m_resourceCallback(senderId, message);
    } else {
        // 访客收到请求类消息或非房主资源消息时丢弃并计数。
        ++m_stats.invalidMessages;
    }
}

/// @brief 房主核对传输身份与稳定身份后接收编辑请求。
/// @param senderId 传输层确认的访客 PeerId。
/// @param request 待进入权威队列的请求。
void CollaborationPeer::handleEditRequest(PeerId             senderId,
                                          const EditRequest& request)
{
    // 本函数不执行权限检查；授权留到队列出队，保持统一请求顺序。
    // senderId 必须同时存在于访客集合和权威身份表。
    const auto identity = m_participantIdentities.find(senderId);
    if ( m_participants.find(senderId) == m_participants.end() ||
         identity == m_participantIdentities.end() ||
         request.participantId != identity->second.participantId ||
         request.sessionId != identity->second.sessionId ||
         request.clientSequence == 0 || request.payload.empty() ||
         request.payload.size() > m_config.limits.maxOperationBytes ) {
        // 负载身份不匹配可防止访客冒用另一会话的去重序号。
        ++m_stats.invalidMessages;
        return;
    }
    // 队列满是有效请求的容量丢弃，与协议无效消息分开统计。
    if ( !enqueueHostRequest(request) ) {
        ++m_stats.droppedRequests;
    }
}

/// @brief 访客只按连续修订应用房主提交，并在缺口时请求补发。
/// @param senderId 直接发送方。
/// @param committed 待验证的权威提交。
void CollaborationPeer::handleCommittedOperation(
    PeerId senderId, const CommittedOperation& committed)
{
    // 接收路径不缓存未来提交，可靠补发负责按连续顺序重新送达。
    // 来源、修订、稳定身份、序号和非空负载构成提交基本合法性。
    if ( senderId != m_config.hostPeerId || committed.revision == 0 ||
         Config::normalizeCollaborationStableId(committed.participantId) !=
             committed.participantId ||
         Config::normalizeCollaborationStableId(committed.sessionId) !=
             committed.sessionId ||
         committed.clientSequence == 0 || committed.payload.empty() ) {
        ++m_stats.invalidMessages;
        return;
    }
    // 已应用或更旧提交属于可靠重传重复，不再次调用业务回调。
    if ( committed.revision <= m_appliedRevision ) {
        ++m_stats.duplicateCommits;
        return;
    }
    // 观察到未来修订时不缓存乱序操作，只请求从当前下一版补发。
    if ( committed.revision != m_appliedRevision + 1 ) {
        requestResync(committed.revision);
        return;
    }

    // 唯一连续提交先更新本地模型与修订水位。
    applyCommittedOperation(committed);
    // 追到触发缺口时观察的目标修订后结束当前补发抑制窗口。
    if ( m_resyncTargetRevision.has_value() &&
         m_appliedRevision >= m_resyncTargetRevision.value() ) {
        m_resyncTargetRevision.reset();
    }
    // 每次连续应用后向房主确认最新水位。
    sendRevisionAck();
}

/// @brief 房主单调更新访客已确认的连续修订水位。
/// @param senderId 发送确认的访客。
/// @param ack 访客当前已应用修订。
void CollaborationPeer::handleRevisionAck(PeerId             senderId,
                                          const RevisionAck& ack)
{
    // Ack 仅用于诊断和后续清理策略，不影响当前 appliedRevision。
    // 只接受在线访客且不能确认房主尚未提交的未来修订。
    const auto participantIt = m_participants.find(senderId);
    if ( participantIt == m_participants.end() ||
         ack.revision > m_appliedRevision ) {
        ++m_stats.invalidMessages;
        return;
    }
    // 迟到 Ack 不得降低已有确认水位。
    auto& acknowledged = m_lastAcknowledgedRevision[senderId];
    acknowledged       = std::max(acknowledged, ack.revision);
}

/// @brief 房主从有界日志补发访客缺失修订或回退到完整快照。
/// @param senderId 请求补发的访客。
/// @param request 从哪一修订开始补发。
void CollaborationPeer::handleResyncRequest(PeerId               senderId,
                                            const ResyncRequest& request)
{
    // 补发只读取有界日志，不修改房主自身修订水位。
    // 起点必须处于 [1, applied+1]，且请求方仍在房间内。
    if ( m_participants.find(senderId) == m_participants.end() ||
         request.fromRevision == 0 ||
         request.fromRevision > m_appliedRevision + 1 ) {
        ++m_stats.invalidMessages;
        return;
    }
    // applied+1 表示访客已经追平，无需发送任何历史数据。
    if ( request.fromRevision == m_appliedRevision + 1 ) {
        return;
    }
    // 日志为空或起点早于最老保留项时无法增量补发。
    if ( m_journal.empty() ||
         request.fromRevision < m_journal.front().revision ) {
        ++m_stats.resyncUnavailable;
        if ( m_stateSnapshot ) {
            // 有快照时发送最近恢复点，让访客直接跳过已淘汰日志。
            static_cast<void>(sendMessage(senderId, *m_stateSnapshot));
        }
        return;
    }

    // 日志按 revision 排序，向请求方发送起点及之后全部保留操作。
    for ( const auto& committed : m_journal ) {
        if ( committed.revision >= request.fromRevision ) {
            // 保留日志顺序逐条发送，使访客能继续执行连续性检查。
            static_cast<void>(sendMessage(senderId, committed));
        }
    }
}

/// @brief 访客接受房主广播并维护稳定身份镜像。
/// @param senderId 必须为权威房主。
/// @param identity 待校验参与者身份。
void CollaborationPeer::handleParticipantIdentity(
    PeerId senderId, const ParticipantIdentity& identity)
{
    // 身份表包含本地、房主和所有已知访客，是后续权限/聊天校验基础。
    // 所有文本字段重新规范化，防止绕过本地身份唯一表示。
    const auto creator = Config::normalizeCreatorIdentity(identity.creator);
    const auto participantId =
        Config::normalizeCollaborationStableId(identity.participantId);
    const auto sessionId =
        Config::normalizeCollaborationStableId(identity.sessionId);
    // 本地自身身份若被广播，必须与构造配置完全一致。
    if ( senderId != m_config.hostPeerId || identity.peerId == 0 ||
         participantId.empty() || sessionId.empty() || creator.empty() ||
         (identity.peerId == m_config.peerId &&
          (participantId != m_config.participantId ||
           sessionId != m_config.sessionId || creator != m_config.creator)) ) {
        ++m_stats.invalidMessages;
        return;
    }
    // 不同 PeerId 不能共享 ParticipantId 或 SessionId。
    const bool stableIdentityConflict =
        std::any_of(m_participantIdentities.begin(),
                    m_participantIdentities.end(),
                    [&identity, &participantId, &sessionId](const auto& entry) {
                        return entry.first != identity.peerId &&
                               (entry.second.participantId == participantId ||
                                entry.second.sessionId == sessionId);
                    });
    if ( stableIdentityConflict ) {
        ++m_stats.invalidMessages;
        return;
    }
    // 同一 PeerId 的更新按房主权威值整体替换。
    m_participantIdentities.insert_or_assign(
        identity.peerId,
        ParticipantIdentity{
            identity.peerId, participantId, sessionId, creator });
}

/// @brief 访客根据房主通知删除参与者的临时状态。
/// @param senderId 通知直接发送方。
/// @param participantLeft 待删除的 PeerId。
void CollaborationPeer::handleParticipantLeft(
    PeerId senderId, const ParticipantLeft& participantLeft)
{
    // 未知 PeerId 的合法房主通知仍可幂等清理各镜像表。
    // 只能由房主移除其他访客，不能通过消息移除房主或当前本地 Peer。
    if ( senderId != m_config.hostPeerId || participantLeft.peerId == 0 ||
         participantLeft.peerId == m_config.hostPeerId ||
         participantLeft.peerId == m_config.peerId ) {
        ++m_stats.invalidMessages;
        return;
    }
    // 身份、视口、聊天水位和权限共享同一 PeerId 生命周期。
    m_participantIdentities.erase(participantLeft.peerId);
    m_participantViewports.erase(participantLeft.peerId);
    m_lastChatSequence.erase(participantLeft.peerId);
    m_participantPermissions.erase(participantLeft.peerId);
    // 访客不维护 m_participants 或请求队列，因此无需额外清理。
}

/// @brief 访客接受房主发布的完整参与者权限掩码。
/// @param senderId 必须为权威房主。
/// @param permissions 目标 PeerId 与完整权限集合。
void CollaborationPeer::handleParticipantPermissions(
    PeerId senderId, const ParticipantPermissions& permissions)
{
    // 权限消息只改变镜像掩码，不 retroactively 应用或撤销旧操作。
    // 目标必须已有身份，掩码只能含已知位，房主权限不可被收窄。
    if ( senderId != m_config.hostPeerId || permissions.peerId == 0 ||
         !m_participantIdentities.contains(permissions.peerId) ||
         !isCollaborationPermissionMaskValid(permissions.permissions) ||
         (permissions.peerId == m_config.hostPeerId &&
          permissions.permissions != COLLABORATION_PERMISSION_ALL) ) {
        ++m_stats.invalidMessages;
        return;
    }
    // 完整快照按 PeerId 插入或替换，不执行增量位运算。
    m_participantPermissions.insert_or_assign(permissions.peerId,
                                              permissions.permissions);
}

/// @brief 验证并收敛最新视口，再由房主转发给其他访客。
/// @param senderId 直接发送方。
/// @param viewport 声明原始发布者和序号的视口状态。
/// @warning 高频消息路径；只做固定校验、哈希查找和有界参与者广播。
void CollaborationPeer::handleParticipantViewport(
    PeerId senderId, const ParticipantViewport& viewport)
{
    // 视口状态与谱面 revision 解耦，只按每个发布者自己的 sequence 收敛。
    // 协议编码器已做范围检查，此处再次拒绝非有限值保护直接调用边界。
    const bool valuesValid = viewport.peerId != 0 && viewport.sequence != 0 &&
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
        // 访客只能发布自己的视口，senderId 与 payload peerId 必须一致。
        if ( !m_participants.contains(senderId) ||
             viewport.peerId != senderId ) {
            ++m_stats.invalidMessages;
            return;
        }
    } else if ( senderId != m_config.hostPeerId ||
                !m_participantIdentities.contains(viewport.peerId) ||
                viewport.peerId == m_config.peerId ) {
        // 访客只接受房主转发的其他已知成员视口。
        ++m_stats.invalidMessages;
        return;
    }

    // 每名参与者只接受严格更大的序号，迟到和重复状态静默丢弃。
    const auto existing = m_participantViewports.find(viewport.peerId);
    if ( existing != m_participantViewports.end() &&
         viewport.sequence <= existing->second.sequence ) {
        return;
    }
    // 最新状态整体替换，避免历史视口队列增长。
    m_participantViewports.insert_or_assign(viewport.peerId, viewport);

    if ( m_config.isHost ) {
        // 房主转发给除原发送方外的所有访客，避免无意义回显。
        for ( const PeerId participantId : m_participants ) {
            if ( participantId != senderId ) {
                static_cast<void>(sendMessage(participantId, viewport));
            }
        }
    }
}

/// @brief 校验聊天来源与序号，通知产品层并由房主广播。
/// @param senderId 直接发送方。
/// @param chat 声明原始发送者、序号和正文的消息。
void CollaborationPeer::handleChatMessage(PeerId senderId,
                                          const CollaborationChatMessage& chat)
{
    // 聊天正文已由线协议验证，本层只验证拓扑来源和去重水位。
    // 房主可处理自身或在线访客消息；访客只接受房主转发。
    const bool senderValid =
        m_config.isHost
            ? (senderId == m_config.peerId || m_participants.contains(senderId))
            : senderId == m_config.hostPeerId;
    if ( !senderValid || (m_config.isHost && chat.peerId != senderId) ||
         !m_participantIdentities.contains(chat.peerId) ) {
        // 房主要求 payload 身份等于传输发送方，阻止冒用他人聊天身份。
        ++m_stats.invalidMessages;
        return;
    }

    // 序号水位按原始 chat.peerId 跟踪，可靠重传不重复展示。
    const auto lastSequence = m_lastChatSequence.find(chat.peerId);
    if ( lastSequence != m_lastChatSequence.end() &&
         chat.sequence <= lastSequence->second ) {
        ++m_stats.duplicateChatMessages;
        return;
    }
    // 在调用 UI 回调前更新水位，重入路径也不会重复接受同一消息。
    m_lastChatSequence.insert_or_assign(chat.peerId, chat.sequence);
    if ( m_chatCallback ) m_chatCallback(chat);
    // 回调按同步逻辑线程执行，不跨线程保留 chat 引用。

    if ( m_config.isHost ) {
        // 广播包括原发送访客，使其也以房主权威顺序显示消息。
        for ( const PeerId participantId : m_participants ) {
            static_cast<void>(sendMessage(participantId, chat));
        }
    }
}

/// @brief 访客用房主完整快照跳过无法补齐的日志缺口。
/// @param senderId 必须为权威房主。
/// @param snapshot 非回退修订与完整文档负载。
void CollaborationPeer::handleStateSnapshot(PeerId               senderId,
                                            const StateSnapshot& snapshot)
{
    // 快照应用会跃迁 appliedRevision，因此必须保证不低于当前水位。
    // 快照必须来自房主、非空、受限且不能回退已应用修订。
    if ( senderId != m_config.hostPeerId || snapshot.revision == 0 ||
         snapshot.payload.empty() ||
         snapshot.payload.size() > m_config.limits.maxOperationBytes ||
         snapshot.revision < m_appliedRevision ) {
        ++m_stats.invalidMessages;
        return;
    }
    // 相同修订已在本地体现，重复快照无需再次应用或确认。
    if ( snapshot.revision == m_appliedRevision ) return;

    // 复用提交应用回调承载完整负载，并以快照修订直接推进水位。
    CommittedOperation committed;
    committed.revision      = snapshot.revision;
    const auto hostIdentity = m_participantIdentities.find(m_config.hostPeerId);
    if ( hostIdentity == m_participantIdentities.end() ) {
        // 缺少房主稳定身份时无法构造可追踪的提交来源。
        ++m_stats.invalidMessages;
        return;
    }
    // 快照归因于房主身份，clientSequence 使用修订号构造稳定非零值。
    committed.participantId  = hostIdentity->second.participantId;
    committed.sessionId      = hostIdentity->second.sessionId;
    committed.clientSequence = snapshot.revision;
    committed.payload        = snapshot.payload;
    // 应用成功语义由回调负责，状态机随后结束补发并确认新水位。
    applyCommittedOperation(committed);
    m_resyncTargetRevision.reset();
    sendRevisionAck();
}

/// @brief 房主有界去重、授权并为请求分配全局修订号。
/// @warning 每次逻辑 update 调用；最多处理 maxRequestsPerUpdate 个请求。
void CollaborationPeer::processHostRequests()
{
    // 队列顺序就是房主全局排序输入，不执行额外排序或全表扫描。
    // 双重条件同时限制单帧工作量并在队列提前耗尽时结束。
    for ( std::size_t index = 0; index < m_config.limits.maxRequestsPerUpdate &&
                                 !m_pendingRequests.empty();
          ++index ) {
        // 先移动队首再弹出，后续所有分支都消费该请求一次。
        EditRequest request = std::move(m_pendingRequests.front());
        m_pendingRequests.pop_front();

        // 去重键使用 SessionId，重连后的新会话可从序号一重新开始。
        auto& lastSequence = m_lastAcceptedSequence[request.sessionId];
        if ( request.clientSequence <= lastSequence ) {
            // 重复或倒序请求不分配 revision，也不调用业务授权。
            ++m_stats.duplicateRequests;
            continue;
        }
        // 先推进去重水位，即使请求越权也不能用同序号重复尝试。
        lastSequence = request.clientSequence;

        // 权限与产品层细分授权在 revision 分配前执行，拒绝不产生缺口。
        if ( !isEditRequestAuthorized(request) ) {
            ++m_stats.unauthorizedEditRequests;
            continue;
        }

        // 仅授权请求获得下一个连续全房间 revision。
        CommittedOperation committed;
        committed.revision       = m_nextRevision++;
        committed.participantId  = request.participantId;
        committed.sessionId      = request.sessionId;
        committed.clientSequence = request.clientSequence;
        // 移动操作负载进入提交，待处理请求不再保留副本。
        committed.payload = std::move(request.payload);

        // 房主先在本地应用，再写入补发日志并广播相同提交。
        applyCommittedOperation(committed);
        m_journal.push_back(committed);
        // 日志只保留配置数量的最近操作，旧缺口改由快照恢复。
        while ( m_journal.size() > m_config.limits.maxJournalOperations ) {
            m_journal.pop_front();
        }
        broadcastCommittedOperation(committed);
        // 广播完成与否不改变 revision 已提交事实，失败通过统计暴露。
    }
}

/// @brief 判断请求是否具有整体编辑权限和可选的负载级授权。
/// @param request 已通过来源身份校验的队首请求。
/// @return 房主自身请求或远端权限及回调均允许时返回 true。
/// @warning 房主逻辑更新路径调用，回调不得阻塞或修改 Peer 容器。
bool CollaborationPeer::isEditRequestAuthorized(
    const EditRequest& request) const
{
    // 调用时请求身份已经由接收或本地构造路径完成规范化校验。
    // 房主本地会话天然具有编辑权限，不需要在访客表中查找。
    if ( request.participantId == m_config.participantId &&
         request.sessionId == m_config.sessionId ) {
        return true;
    }
    // 远端请求用 ParticipantId 与 SessionId 组合定位当前在线 PeerId。
    const auto identity = std::find_if(
        m_participantIdentities.begin(),
        m_participantIdentities.end(),
        [&request](const auto& entry) {
            return entry.second.participantId == request.participantId &&
                   entry.second.sessionId == request.sessionId;
        });
    if ( identity == m_participantIdentities.end() ) return false;

    // 缺少权限快照或 Edit 位未开启时直接拒绝，不调用产品回调。
    const auto permissions = m_participantPermissions.find(identity->first);
    if ( permissions == m_participantPermissions.end() ||
         !hasCollaborationPermission(permissions->second,
                                     CollaborationPermission::Edit) ) {
        return false;
    }
    // 未提供细分回调时整体 Edit 权限即足够，否则交由产品检查负载。
    // 回调只观察有界 payload span，不取得请求所有权。
    return !m_authorizeEditCallback ||
           m_authorizeEditCallback(identity->first, request.payload);
}

/// @brief 更新连续修订水位并通知本地文档应用层。
/// @param committed 已证明可应用的权威提交或快照包装。
/// @warning 逻辑更新路径同步调用，应用回调不得阻塞或递归推进网络状态。
void CollaborationPeer::applyCommittedOperation(
    const CommittedOperation& committed)
{
    // 状态机不解释操作负载，业务层回调负责文档语义和持久化边界。
    // 先发布水位，再调用回调，使回调查询状态时观察到新修订。
    m_appliedRevision = committed.revision;
    if ( m_applyCallback ) {
        // 回调借用 committed 仅限调用期间，不能跨帧保存引用。
        m_applyCallback(committed);
    }
}

/// @brief 编码并通过独占传输端点发送一条协议消息。
/// @param recipientId 目标临时路由槽位。
/// @param message 已由调用分支进行角色校验的消息。
/// @return 编码和传输均成功时返回 true。
/// @warning 逻辑热路径调用，传输实现必须非阻塞并立即复制帧字节。
bool CollaborationPeer::sendMessage(PeerId                      recipientId,
                                    const CollaborationMessage& message)
{
    // encoded 缓冲只活到同步 send 返回，传输实现必须复制或立即消费。
    // 所有消息共用相同操作负载上限，避免不同入口产生不一致帧。
    auto encoded =
        encodeCollaborationMessage(message, m_config.limits.maxOperationBytes);
    if ( !encoded.has_value() ||
         !m_transport->send(recipientId, encoded.value()) ) {
        // 编码失败和传输拒绝统一计入发送失败，但不会改变本地提交状态。
        ++m_stats.sendFailures;
        return false;
    }
    return true;
}

/// @brief 房主向当前全部访客广播同一权威提交。
/// @param committed 已在房主本地应用并写入日志的操作。
/// @warning 广播最多覆盖协议允许的七名访客，不得扩展为无界目标集合。
void CollaborationPeer::broadcastCommittedOperation(
    const CommittedOperation& committed)
{
    // m_participants 不包含房主自身，本地应用已经在广播前完成。
    // 单个发送失败不会阻止其他访客，失败计数由 sendMessage 累计。
    for ( const PeerId participantId : m_participants ) {
        static_cast<void>(sendMessage(participantId, committed));
    }
}

/// @brief 访客确认当前已经连续应用的最高修订。
/// @warning 每次连续提交后调用，只允许一次非阻塞发送。
void CollaborationPeer::sendRevisionAck()
{
    // m_appliedRevision 在连续提交或快照应用后已经更新。
    // Ack 只发给权威房主；传输失败由统一发送统计记录。
    static_cast<void>(
        sendMessage(m_config.hostPeerId, RevisionAck{ m_appliedRevision }));
}

/// @brief 访客在观察到修订缺口时请求从下一连续版本补发。
/// @param observedRevision 触发缺口的未来修订。
/// @warning 缺口路径低频调用，已有覆盖请求时必须保持静默。
void CollaborationPeer::requestResync(std::uint64_t observedRevision)
{
    // observedRevision 只定义抑制范围，实际请求起点始终来自本地水位。
    // 已有请求覆盖相同或更早观察目标时不重复发送。
    if ( m_resyncTargetRevision.has_value() &&
         observedRevision <= m_resyncTargetRevision.value() ) {
        return;
    }
    // 保存最新追赶目标用于抑制重复请求，并累计实际请求次数。
    m_resyncTargetRevision = observedRevision;
    ++m_stats.resyncRequests;
    // 始终从当前连续水位的下一修订开始，避免留下本地缺口。
    static_cast<void>(sendMessage(m_config.hostPeerId,
                                  ResyncRequest{ m_appliedRevision + 1 }));
}
}  // namespace MMM::Network::Collaboration
