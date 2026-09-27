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
// 将协作协议类型限定在测试翻译单元，保持场景代码聚焦状态机语义。
using MMM::Network::Collaboration::ByteBuffer;
using MMM::Network::Collaboration::CollaborationChatMessage;
using MMM::Network::Collaboration::CollaborationPeer;
using MMM::Network::Collaboration::CollaborationPeerConfig;
using MMM::Network::Collaboration::CommittedOperation;
using MMM::Network::Collaboration::LoopbackTransportHub;
using MMM::Network::Collaboration::ParticipantIdentity;
using MMM::Network::Collaboration::ParticipantPermissions;
using MMM::Network::Collaboration::ParticipantViewport;
using MMM::Network::Collaboration::PeerId;
using MMM::Network::Collaboration::ProtocolError;
using MMM::Network::Collaboration::SubmitOperationResult;
using MMM::Network::Collaboration::SubmitChatMessageResult;
using MMM::Network::Collaboration::StateSnapshot;
using MMM::Network::Collaboration::decodeCollaborationMessage;
using MMM::Network::Collaboration::encodeCollaborationMessage;

/// @brief 测试中的客户端总数。
/// @note 一名房主加七名访客，覆盖协议允许的最大房间容量。
constexpr std::size_t PEER_COUNT = 8;
/// @brief 房主使用的固定路由槽位。
/// @note 访客从二开始编号，零值仍保留为无效身份。
constexpr PeerId HOST_ID = 1;

/// @brief 为测试 Peer 构造固定长度且可读的稳定标识。
/// @details
/// 线上身份要求使用固定长度的小写十六进制文本。测试不关心哈希算法，
/// 但必须保留同样的格式约束，否则构造失败会掩盖真正要验证的状态迁移。
/// discriminator 占用首字符，让同一 PeerId 的 ParticipantId、SessionId
/// 可被肉眼区分；剩余字符按 PeerId 的十六进制表示从右向左填充。
/// 生成过程完全确定，不依赖随机源，也不会让重连场景偶然复用错误身份。
/// @par 格式不变量
/// - 输出长度始终为 32 字符。
/// - 所有字符均属于协议接受的小写十六进制集合。
/// - 不同 discriminator 为同一 PeerId 建立不同身份域。
/// - 小 PeerId 保留前导零，便于测试日志和失败差异阅读。
/// @param peerId 测试连接槽位。
/// @param discriminator 区分参与者标识和操作会话标识的十六进制字符。
/// @return 满足线上协议格式的 32 字符小写十六进制标识。
[[nodiscard]] std::string makeTestStableId(PeerId peerId, char discriminator)
{
    // 固定查表只生成协议允许的小写十六进制字符。
    constexpr std::string_view DIGITS = "0123456789abcdef";
    // 先填满 32 字符，再把首字符留给身份类别区分。
    std::string identity(32U, '0');
    identity.front() = discriminator;
    // 从尾部向前写 peerId 的十六进制位，保留固定宽度和前导零。
    for ( std::size_t index = identity.size(); index > 1U; --index ) {
        identity[index - 1U] = DIGITS[peerId & 0xFU];
        peerId >>= 4U;
    }
    // 结果可直接通过稳定身份规范化校验，无需随机数或用户配置。
    return identity;
}

/// @brief 填充 Peer 测试所需的三层身份。
/// @details
/// 协作层同时维护临时 PeerId、稳定 ParticipantId 与单次加入 SessionId。
/// 大多数测试只需要一组互不冲突的默认映射，因此统一在这里构造，避免场景
/// 各自遗漏某层身份。模拟重连时，调用方会在此基础上只替换 SessionId，
/// 以明确验证“稳定协作者不变、操作会话更新”的协议语义。
/// @param config 待填充配置。
/// @param peerId 本次连接路由槽位。
/// @param hostPeerId 房主路由槽位。
void setTestPeerIdentity(CollaborationPeerConfig& config, PeerId peerId,
                         PeerId hostPeerId = HOST_ID)
{
    // PeerId 只在当前连接路由中使用，hostPeerId 固定指向权威节点。
    config.peerId     = peerId;
    config.hostPeerId = hostPeerId;
    // participant 使用 a 前缀跨重连稳定，session 使用 b 前缀区分加入流程。
    config.participantId = makeTestStableId(peerId, 'a');
    config.sessionId     = makeTestStableId(peerId, 'b');
}

/// @brief 为指定客户端和局部序号生成唯一规范化操作负载。
/// @details
/// CollaborationPeer 将操作正文视为不透明字节，本测试用三个可推导字节替代
/// 真实谱面命令。前两字节分别标识来源与本地顺序，末字节组合二者，用来降低
/// 仅比较长度或单字段时漏掉乱序、重复应用和来源串线的风险。
/// 返回值很短，也能在重复包和日志补发场景中避免负载上限干扰状态机判断。
/// @param peerId 发起客户端标识。
/// @param localIndex 客户端内的测试操作序号。
/// @return 可直接比较的短字节负载。
[[nodiscard]] ByteBuffer makeOperation(PeerId peerId, std::uint8_t localIndex)
{
    // 三字节同时编码来源、局部序号和校验式组合，便于逐项比较顺序。
    return {
        static_cast<std::uint8_t>(peerId),
        localIndex,
        static_cast<std::uint8_t>(peerId ^ localIndex),
    };
}

/// @brief 驱动所有 Peer 的有界非阻塞 update，直到消息队列收敛。
/// @details
/// LoopbackTransport 不启动后台线程，消息只有在对应 Peer 调用 update 时才会
/// 被消费。单轮采用“访客、房主、访客、房主”的顺序，完整覆盖请求上行、
/// 权威提交下行、确认上行，以及发现版本缺口后产生的补发请求。
/// rounds 由场景按消息链长度给定；本函数不会等待真实时间，也不会在条件
/// 未满足时无限循环，因此失败能够稳定复现而不是表现为测试挂起。
/// @par 推进约束
/// - 不根据容器顺序推断权威提交顺序。
/// - 不使用 sleep 或固定时长网络等待。
/// - 不在辅助函数内提前判断收敛，断言仍由具体场景负责。
/// - 每轮最后一次房主更新保证确认不会遗留到下一阶段。
/// @param host 房主 Peer。
/// @param guests 七个访客 Peer。
/// @param rounds 最多驱动轮数。
void pumpPeers(CollaborationPeer&                               host,
               std::vector<std::unique_ptr<CollaborationPeer>>& guests,
               std::size_t                                      rounds)
{
    // 每轮先让访客发送到房主，再由房主排序，随后访客接收广播和 Ack。
    for ( std::size_t round = 0; round < rounds; ++round ) {
        for ( auto& guest : guests ) {
            guest->update();
        }
        // 第一次房主 update 消费访客请求并产生权威提交。
        host.update();
        for ( auto& guest : guests ) {
            // 第二阶段访客应用提交并发送修订确认。
            guest->update();
        }
        // 最后让房主消费确认或访客在缺口时产生的补发请求。
        host.update();
    }
}

/// @brief 判断全部访客是否与房主模型及连续版本完全一致。
/// @details
/// 只比较 appliedRevision 会漏掉重复应用后“数量碰巧相等”的错误，只比较负载
/// 又无法发现水位更新遗漏。因此这里同时比较权威版本与完整应用序列。
/// 房主模型固定放在 models[0]，第 n 名访客对应 models[n + 1]；该映射与
/// 创建访客时捕获的数组索引保持一致，是并发收敛场景的核心不变量。
/// @par 失败含义
/// - revision 不同表示仍有缺口或确认链未完成。
/// - 模型不同表示发生丢失、乱序或重复应用。
/// - 任一访客失败都会使整体收敛检查立即失败。
/// @param host 房主 Peer。
/// @param guests 七个访客 Peer。
/// @param models 各 Peer 按提交顺序应用的操作负载。
/// @return 全部客户端收敛时返回 true。
[[nodiscard]] bool allPeersConverged(
    const CollaborationPeer&                               host,
    const std::vector<std::unique_ptr<CollaborationPeer>>& guests,
    const std::array<std::vector<ByteBuffer>, PEER_COUNT>& models)
{
    // models[0] 固定为房主应用顺序，访客模型从索引一对应 guests。
    for ( std::size_t index = 0; index < guests.size(); ++index ) {
        // 修订水位与实际负载序列必须同时一致才算真正收敛。
        if ( guests[index]->appliedRevision() != host.appliedRevision() ||
             models[index + 1] != models[0] ) {
            return false;
        }
    }
    // 空访客集合也按全称条件成立，当前调用场景始终包含七名访客。
    return true;
}

/// @brief 判断房主与全部访客是否持有一致的 Creator 身份表。
/// @details
/// Creator 是展示名称，ParticipantId 与 SessionId 则分别承担跨重连身份和
/// 单次操作会话去重。身份同步只有在这些字段及 PeerId 全部一致时才完整。
/// 检查以房主表为基准逐个查找，而不依赖 unordered_map 的迭代顺序；这样既
/// 能发现缺项，也不会把容器内部顺序差异误判为协议错误。
/// @par 身份层次
/// - PeerId 只标识本次传输连接的路由槽位。
/// - ParticipantId 标识可跨重连识别的协作者。
/// - SessionId 隔离每次加入产生的请求序号空间。
/// - Creator 是经规范化后向其他成员展示的名称。
/// @param host 房主 Peer。
/// @param guests 七个访客 Peer。
/// @return 全部身份映射与房主一致时返回 true。
[[nodiscard]] bool allCreatorIdentitiesConverged(
    const CollaborationPeer&                               host,
    const std::vector<std::unique_ptr<CollaborationPeer>>& guests)
{
    // 房主身份表应包含自身及七名已登记访客。
    if ( host.participantIdentities().size() != PEER_COUNT ) {
        return false;
    }
    // 每名访客必须与房主持有相同数量和逐字段相同的身份记录。
    return std::all_of(guests.begin(), guests.end(), [&](const auto& guest) {
        const auto& guestIdentities = guest->participantIdentities();
        if ( guestIdentities.size() != host.participantIdentities().size() ) {
            return false;
        }
        return std::all_of(
            host.participantIdentities().begin(),
            host.participantIdentities().end(),
            [&guestIdentities](const auto& entry) {
                // 先按 PeerId 查找，再核对稳定参与者、会话与 Creator。
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
/// @details
/// 视口是高频易失状态，不参与文档 revision，但仍由房主汇聚并广播。
/// 测试先要求成员数量一致，再按 PeerId 核对发布序号和代表性的时间、偏移值，
/// 避免某端保留已离开成员或把不同发布者的状态覆盖到同一槽位。
/// 浮点值由测试直接构造并经二进制协议往返，因此这里可使用精确比较。
/// @par 检查边界
/// - 集合大小先拦截缺少发布者和残留发布者。
/// - sequence 证明每名成员保留的是最新一次发布。
/// - playbackTime 验证纵向时间位置没有串线。
/// - horizontalOffsetRatio 验证横向画布位置没有串线。
/// @param host 房主 Peer。
/// @param guests 七个访客 Peer。
/// @return 全部客户端都收敛到八个参与者的最新状态时返回 true。
[[nodiscard]] bool allParticipantViewportsConverged(
    const CollaborationPeer&                               host,
    const std::vector<std::unique_ptr<CollaborationPeer>>& guests)
{
    // 房主先拥有八个最新视口，才有可能证明访客镜像收敛。
    if ( host.participantViewports().size() != PEER_COUNT ) return false;
    return std::all_of(guests.begin(), guests.end(), [&](const auto& guest) {
        const auto& guestViewports = guest->participantViewports();
        // 数量不等说明存在缺失或多余发布者，直接失败。
        if ( guestViewports.size() != PEER_COUNT ) return false;
        for ( const auto& [peerId, hostViewport] :
              host.participantViewports() ) {
            const auto viewport = guestViewports.find(peerId);
            // 核对序号和代表性浮点字段，确保不是只有身份集合一致。
            if ( viewport == guestViewports.end() ||
                 viewport->second.sequence != hostViewport.sequence ||
                 viewport->second.playbackTime != hostViewport.playbackTime ||
                 viewport->second.horizontalOffsetRatio !=
                     hostViewport.horizontalOffsetRatio ) {
                return false;
            }
        }
        // 当前访客逐项通过后继续检查下一名访客。
        return true;
    });
}

/// @brief 覆盖 8 Peer 并发提交、重复包去重和缺失版本日志补发。
/// @details
/// 本场景把房间填满到协议上限，先验证稳定身份和视口状态能广播到所有成员，
/// 再让八端各提交四条操作，形成 32 条连续权威 revision。传输层复制每个包，
/// 因而房主必须按 ParticipantId、SessionId 和 clientSequence 对请求去重，
/// 访客也必须按 revision 对提交去重，最终每端模型只能应用每条操作一次。
/// 随后关闭复制并定向丢失 revision 33；revision 34 到达时，落后访客应只发出
/// 一次补发请求，房主利用仍在日志中的两条提交恢复连续顺序，而不是使用快照。
/// 最后移除一个访客，确认成员身份和易失视口都从其余在线端清除。
/// 此测试刻意不断言不同发送者之间的具体排序，只要求所有端接受房主给出的
/// 同一总序，因为跨发送者顺序属于房主调度结果而非客户端可预知契约。
/// @par 阶段判据
/// - 空 Creator 与超出房间容量的成员必须在登记阶段拒绝。
/// - 身份广播完成后，八端应持有相同的 Creator 身份表。
/// - 视口广播完成后，八端应持有相同的发布者集合和最新值。
/// - 重复模式下，统计量应观察到重复，而业务模型不得出现重复项。
/// - 丢包模式下，只允许一次 resyncRequests，且无需快照回退。
/// - 房主自身不可被 removeParticipant 移除。
/// - 离开成员必须同时从身份表与视口表消失。
/// @par 回归风险
/// 该组合能发现只按 PeerId 去重、Ack 水位提前、补发顺序颠倒、离开广播遗漏
/// 等单端测试不易暴露的问题。固定 32 轮和 16 轮是消息链上界，不是时间等待。
/// @return 全部断言通过时返回 true。
[[nodiscard]] bool testEightPeerIncrementalConvergence()
{
    // 单个 Hub 提供可控重复和丢包故障注入，models 记录各端应用顺序。
    LoopbackTransportHub                            hub;
    std::array<std::vector<ByteBuffer>, PEER_COUNT> models;

    CollaborationPeerConfig hostConfig;
    // 房主使用固定三层身份和全容量默认限制。
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator = "Host Creator";
    hostConfig.isHost  = true;
    CollaborationPeer host(hostConfig,
                           hub.createEndpoint(HOST_ID),
                           [&models](const CommittedOperation& operation) {
                               // 房主回调只记录不透明负载，模拟文档应用顺序。
                               models[0].push_back(operation.payload);
                           });
    if ( !host.isValid() ) {
        // 基础房主必须在任何场景构造前满足角色与传输前置条件。
        return false;
    }
    // 仅含空白的 Creator 规范化为空，登记必须被拒绝。
    if ( host.addParticipant(99,
                             makeTestStableId(99, 'a'),
                             makeTestStableId(99, 'b'),
                             " \t") ) {
        return false;
    }

    {
        // 空 Creator 的独立房主配置应构造为无效 Peer。
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

    // 按 PeerId 2～8 创建七名访客，并立即登记到房主状态机。
    std::vector<std::unique_ptr<CollaborationPeer>> guests;
    guests.reserve(PEER_COUNT - 1);
    for ( std::size_t index = 1; index < PEER_COUNT; ++index ) {
        // models[index] 与当前访客保持同一数组槽位。
        const PeerId            peerId = static_cast<PeerId>(index + 1);
        CollaborationPeerConfig guestConfig;
        setTestPeerIdentity(guestConfig, peerId);
        guestConfig.creator = "Guest " + std::to_string(peerId);
        guestConfig.isHost  = false;
        auto guest          = std::make_unique<CollaborationPeer>(
            guestConfig,
            hub.createEndpoint(peerId),
            [&models, index](const CommittedOperation& operation) {
                // 每名访客独立记录实际应用的权威提交序列。
                models[index].push_back(operation.payload);
            });
        if ( !guest->isValid() ||
             !host.addParticipant(peerId,
                                  guestConfig.participantId,
                                  guestConfig.sessionId,
                                  guestConfig.creator) ) {
            return false;
        }
        // 登记成功后才转移 unique_ptr，保证 vector 中对象均有效在线。
        guests.push_back(std::move(guest));
    }
    // 第九个总成员超过上限，房主必须拒绝且不改变现有身份表。
    if ( host.addParticipant(static_cast<PeerId>(PEER_COUNT + 1),
                             makeTestStableId(PEER_COUNT + 1, 'a'),
                             makeTestStableId(PEER_COUNT + 1, 'b'),
                             "Overflow Guest") ) {
        return false;
    }
    // 驱动身份和权限广播到所有新访客。
    pumpPeers(host, guests, 4);
    if ( !allCreatorIdentitiesConverged(host, guests) ) {
        return false;
    }

    // 房主先发布一组有限且有代表性的画布时间与横向偏移。
    ParticipantViewport hostViewport;
    hostViewport.playbackTime          = 12.5;
    hostViewport.visualTime            = 12.6;
    hostViewport.visibleTimeStart      = 9.0;
    hostViewport.visibleTimeEnd        = 15.0;
    hostViewport.horizontalOffsetRatio = 0.125;
    if ( !host.publishViewport(hostViewport) ) return false;
    // 每名访客基于模板改写时间和偏移，确保状态表能区分发布者。
    for ( std::size_t index = 0; index < guests.size(); ++index ) {
        ParticipantViewport guestViewport = hostViewport;
        guestViewport.playbackTime        = 20.0 + static_cast<double>(index);
        guestViewport.visualTime          = guestViewport.playbackTime + 0.1;
        guestViewport.horizontalOffsetRatio =
            -0.05 * static_cast<double>(index + 1);
        if ( !guests[index]->publishViewport(guestViewport) ) return false;
    }
    // 房主汇聚后广播，八轮足以让全部状态镜像稳定。
    pumpPeers(host, guests, 8);
    if ( !allParticipantViewportsConverged(host, guests) ) return false;

    // 开启每包复制，验证请求去重和提交去重两层都不重复应用。
    hub.setDuplicatePackets(true);
    for ( std::uint8_t localIndex = 1; localIndex <= 4; ++localIndex ) {
        // 每轮房主提交一条，七名访客各提交一条，共八条权威操作。
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
    // 四轮八操作应最终产生 32 个连续 revision。
    pumpPeers(host, guests, 32);

    // 房主必须观察到重复请求，但模型只包含一次权威序列。
    if ( host.appliedRevision() != 32 || host.stats().duplicateRequests == 0 ||
         !allPeersConverged(host, guests, models) ) {
        return false;
    }
    // 每名访客也应收到并忽略至少一个重复提交广播。
    for ( const auto& guest : guests ) {
        if ( guest->stats().duplicateCommits == 0 ) {
            return false;
        }
    }

    // 关闭复制并定向丢弃房主发给 Peer 2 的下一条提交。
    hub.setDuplicatePackets(false);
    hub.dropNextPacket(HOST_ID, 2);
    const ByteBuffer missingOperation = makeOperation(HOST_ID, 5);
    if ( host.submitOperation(missingOperation) !=
         SubmitOperationResult::Accepted ) {
        return false;
    }
    host.update();
    // 首条广播被注入丢弃，访客此时仍停留在 revision 32。
    guests[0]->update();

    // 下一条 revision 34 到达后应触发从 33 开始的单次补发请求。
    const ByteBuffer followingOperation = makeOperation(HOST_ID, 6);
    if ( host.submitOperation(followingOperation) !=
         SubmitOperationResult::Accepted ) {
        return false;
    }
    host.update();
    guests[0]->update();
    // 房主处理补发请求，访客随后按 33、34 连续应用并确认。
    host.update();
    guests[0]->update();
    pumpPeers(host, guests, 16);

    // 日志仍覆盖缺口，因此无需快照回退，所有模型最终再次一致。
    if ( guests[0]->stats().resyncRequests != 1 ||
         host.stats().resyncUnavailable != 0 || host.appliedRevision() != 34 ||
         !allPeersConverged(host, guests, models) ) {
        return false;
    }

    // 尝试移除房主自身必须保持幂等无效，身份仍然存在。
    host.removeParticipant(HOST_ID);
    if ( !host.participantIdentities().contains(HOST_ID) ) {
        return false;
    }

    // 移除最后一名访客，并验证房主与其余访客同步清理身份和视口。
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
    // 最大规模、重复、丢包补发和成员离开场景全部完成。
    return true;
}

/// @brief 覆盖二进制帧往返、长度上限和截断消息拒绝。
/// @details
/// 本测试直接作用于 encodeCollaborationMessage 与 decodeCollaborationMessage，
/// 不经过 Peer 状态机，以便把字段规范化、长度边界和帧结构错误归因到协议层。
/// 成功路径覆盖编辑请求、身份、快照、视口、权限、聊天以及三类资源消息；
/// 失败路径覆盖超大操作、空 Creator、非有限浮点、未知权限位、非法 UTF-8、
/// 空白聊天、超长资源字段、非零保留位和截断帧。
/// 每个 expected 都同时检查是否失败及具体 ProtocolError，防止实现把不同格式
/// 错误合并为无意义的通用失败。固定宽度消息还要验证其不受操作正文上限影响。
/// 所有样本均为内存数据，不依赖外部文件、系统时钟或真实网络环境。
/// @par 成功路径字段
/// - EditRequest 保留两层稳定身份、客户端序号和不透明负载。
/// - ParticipantIdentity 对 Creator 去除边缘空白但不改稳定标识。
/// - StateSnapshot 保留权威 revision 和完整状态负载。
/// - ParticipantViewport 保留整数序号和有限 double 值。
/// - ParticipantPermissions 允许组合全部已知权限位。
/// - CollaborationChatMessage 保留合法多字节 UTF-8 文本。
/// - ResourceManifest、ResourceRequest 与 ResourceChunk 保留资源定位信息。
/// @par 拒绝路径字段
/// - 变长字段必须分别遵守操作、聊天、资源清单和分块上限。
/// - 字符串必须满足 UTF-8、可见内容和规范化身份约束。
/// - 浮点字段必须有限，权限与保留位不得携带未知语义。
/// - 解码器必须在读取越过帧尾之前报告 TruncatedFrame。
/// - 非法输入不得以部分成功的消息变体返回给上层状态机。
/// @return 全部协议断言通过时返回 true。
[[nodiscard]] bool testProtocolBounds()
{
    // 基础编辑请求使用恰好四字节上限，覆盖成功边界。
    MMM::Network::Collaboration::EditRequest request;
    request.participantId  = makeTestStableId(7, 'a');
    request.sessionId      = makeTestStableId(7, 'b');
    request.clientSequence = 11;
    request.payload        = { 1, 2, 3, 4 };

    // 先编码再用相同上限解码，验证完整二进制往返。
    auto encoded = encodeCollaborationMessage(request, 4);
    if ( !encoded.has_value() ) {
        return false;
    }
    // 解码结果必须是 EditRequest 变体而非仅返回任意成功类型。
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
        // 稳定身份、会话、序号和不透明负载均需逐字段一致。
        return false;
    }

    // 将运行时上限降为三，原四字节负载必须明确报 OperationTooLarge。
    auto oversized = encodeCollaborationMessage(request, 3);
    if ( oversized.has_value() ||
         oversized.error() != ProtocolError::OperationTooLarge ) {
        return false;
    }

    // Creator 两侧空格应由身份规范化去除，稳定 ID 保持不变。
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
        // 成功往返同时证明 Creator 使用规范化值而非原始空白文本。
        return false;
    }

    // 只有空白和换行的 Creator 规范化为空，编码阶段即应拒绝。
    ParticipantIdentity invalidIdentity{
        7, makeTestStableId(7, 'a'), makeTestStableId(7, 'b'), " \n"
    };
    auto invalidIdentityResult = encodeCollaborationMessage(invalidIdentity, 4);
    if ( invalidIdentityResult.has_value() ||
         invalidIdentityResult.error() !=
             ProtocolError::InvalidCreatorIdentity ) {
        return false;
    }

    // 快照用四字节负载覆盖 revision 与变长正文的边界成功路径。
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

    // 视口覆盖固定宽度浮点位模式的完整编码与解码。
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
    // 若编码失败，构造带预期错误的 expected 以保持统一检查表达。
    const auto viewportDecoded =
        viewportEncoded
            ? decodeCollaborationMessage(*viewportEncoded, 4U)
            : std::expected<MMM::Network::Collaboration::CollaborationMessage,
                            ProtocolError>(
                  std::unexpected(ProtocolError::InvalidViewportState));
    const auto* decodedViewport =
        viewportDecoded ? std::get_if<ParticipantViewport>(&*viewportDecoded)
                        : nullptr;
    if ( decodedViewport == nullptr ||
         decodedViewport->peerId != viewport.peerId ||
         decodedViewport->sequence != viewport.sequence ) {
        return false;
    }

    // 权限掩码组合 Edit 与 Objects 两个已知位，覆盖合法多位集合。
    const auto permissionMask =
        static_cast<MMM::Network::Collaboration::CollaborationPermissionMask>(
            MMM::Network::Collaboration::CollaborationPermission::Edit) |
        static_cast<MMM::Network::Collaboration::CollaborationPermissionMask>(
            MMM::Network::Collaboration::CollaborationPermission::Objects);
    const ParticipantPermissions permissions{ 7, permissionMask };
    // 权限消息为固定宽度，不受四字节操作负载上限影响。
    const auto permissionEncoded = encodeCollaborationMessage(permissions, 4U);
    const auto permissionDecoded =
        permissionEncoded
            ? decodeCollaborationMessage(*permissionEncoded, 4U)
            : std::expected<MMM::Network::Collaboration::CollaborationMessage,
                            ProtocolError>(
                  std::unexpected(ProtocolError::InvalidPermissions));
    const auto* decodedPermissions =
        permissionDecoded
            ? std::get_if<ParticipantPermissions>(&*permissionDecoded)
            : nullptr;
    if ( decodedPermissions == nullptr ||
         decodedPermissions->peerId != permissions.peerId ||
         decodedPermissions->permissions != permissions.permissions ) {
        return false;
    }
    // 高位未定义权限必须由编码器拒绝，不能静默透传给旧客户端。
    const ParticipantPermissions invalidPermissions{ 7, 1U << 31U };
    const auto                   invalidPermissionResult =
        encodeCollaborationMessage(invalidPermissions, 4U);
    if ( invalidPermissionResult || invalidPermissionResult.error() !=
                                        ProtocolError::InvalidPermissions ) {
        return false;
    }

    // 对视口剩余字段执行精确相等检查，确认 double 位模式未改变。
    if ( !decodedViewport || decodedViewport->peerId != viewport.peerId ||
         decodedViewport->sequence != viewport.sequence ||
         decodedViewport->playbackTime != viewport.playbackTime ||
         decodedViewport->visibleTimeStart != viewport.visibleTimeStart ||
         decodedViewport->horizontalOffsetRatio !=
             viewport.horizontalOffsetRatio ) {
        return false;
    }
    // 任一非有限视口值都应触发 InvalidViewportState。
    viewport.visualTime        = std::numeric_limits<double>::quiet_NaN();
    const auto invalidViewport = encodeCollaborationMessage(viewport, 4U);
    if ( invalidViewport ||
         invalidViewport.error() != ProtocolError::InvalidViewportState ) {
        return false;
    }

    // 合法中文文本覆盖多字节 UTF-8 聊天消息往返。
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
    // 仅含空格和制表符的聊天没有可见内容，编码必须拒绝。
    chat.text            = " \t";
    const auto emptyChat = encodeCollaborationMessage(chat, 4U);
    if ( emptyChat || emptyChat.error() != ProtocolError::InvalidChatMessage ) {
        return false;
    }
    // C0 AF 是过长 UTF-8 编码，验证严格序列校验不会接受。
    chat.text.assign("\xC0\xAF", 2U);
    const auto invalidUtf8Chat = encodeCollaborationMessage(chat, 4U);
    if ( invalidUtf8Chat ||
         invalidUtf8Chat.error() != ProtocolError::InvalidChatMessage ) {
        return false;
    }

    // 资源清单覆盖非零 generation 与小型不透明载荷往返。
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

    // 资源请求覆盖 64 位偏移和四字节请求长度字段。
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
        // 代次、索引、偏移和长度必须全部保持。
        return false;
    }

    // 资源分块在相同定位字段后追加恰好四字节 payload。
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
    // 分块和请求分别超过上限一字节，都必须报 OperationTooLarge。
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

    // 帧头第八字节是保留字段，非零值应在正文解析前拒绝。
    ByteBuffer invalidReserved = encoded.value();
    invalidReserved[7]         = 1;
    auto reservedResult        = decodeCollaborationMessage(invalidReserved, 4);
    if ( reservedResult.has_value() ||
         reservedResult.error() != ProtocolError::InvalidReservedField ) {
        return false;
    }

    // 删除最后一个 payload 字节，使头部声明长度与实际帧不一致。
    encoded->pop_back();
    auto truncated = decodeCollaborationMessage(encoded.value(), 4);
    // 该场景按完整帧长度错误分类，而非消息类型内部截断。
    return !truncated.has_value() &&
           truncated.error() == ProtocolError::InvalidMessageLength;
}

/// @brief 验证房主权限快照同步和 revision 前的强制授权不可被访客绕过。
/// @details
/// 权限判断分为房主持有的粗粒度位掩码和业务提供的负载级回调两层。
/// 第一阶段授予 Edit 与 Objects，但先提交首字节 0x41 的负载，证明细分回调
/// 能在权威 revision 分配前拒绝它；随后提交 0x42，证明两层均通过时正常提交。
/// 第二阶段撤销 Edit、保留 Objects，再次提交 0x42，证明业务回调的允许结果
/// 不能越过基础权限。访客本地只负责排队，最终授权始终由房主决定。
/// 断言同时覆盖权限镜像、unauthorizedEditRequests 计数、两端模型和 revision，
/// 从而确保被拒请求没有留下可见状态，也没有制造版本空洞。
/// @par 授权顺序
/// - 房主先验证发送者身份与当前 SessionId。
/// - Edit 位决定成员是否具备基础编辑资格。
/// - 业务回调再按具体负载决定细分对象权限。
/// - 两层授权全部通过后才能分配下一 revision。
/// - 权限变更由房主广播，访客镜像只用于界面反馈而非权威判断。
/// @par 失败保护
/// 被拒操作可以增加诊断统计，但不能触发应用回调、修改模型或占用版本号。
/// @return 越权请求不推进版本，恢复权限后合法请求正常收敛时返回 true。
[[nodiscard]] bool testParticipantPermissionsAreAuthoritative()
{
    // 单访客场景分别记录房主和访客应用结果，便于证明拒绝不推进模型。
    constexpr PeerId        GUEST_ID = 2;
    LoopbackTransportHub    hub;
    std::vector<ByteBuffer> hostModel;
    std::vector<ByteBuffer> guestModel;

    CollaborationPeerConfig hostConfig;
    // 房主安装细分授权回调，只允许 Peer 2 且首字节为 0x42 的操作。
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator = "Host";
    hostConfig.isHost  = true;
    CollaborationPeer host(
        hostConfig,
        hub.createEndpoint(HOST_ID),
        [&hostModel](const CommittedOperation& operation) {
            hostModel.push_back(operation.payload);
        },
        {},
        {},
        [](PeerId peerId, std::span<const std::uint8_t> payload) {
            // 回调条件与整体 Edit 权限共同决定最终授权。
            return peerId == GUEST_ID && !payload.empty() &&
                   payload.front() == 0x42U;
        });

    // 访客记录最终权威提交，不能在本地提交时提前修改模型。
    CollaborationPeerConfig guestConfig;
    setTestPeerIdentity(guestConfig, GUEST_ID);
    guestConfig.creator = "Guest";
    guestConfig.isHost  = false;
    auto guest          = std::make_unique<CollaborationPeer>(
        guestConfig,
        hub.createEndpoint(GUEST_ID),
        [&guestModel](const CommittedOperation& operation) {
            guestModel.push_back(operation.payload);
        });
    // 登记是后续权限广播和请求身份校验的前置条件。
    if ( !host.addParticipant(GUEST_ID,
                              guestConfig.participantId,
                              guestConfig.sessionId,
                              guestConfig.creator) ) {
        return false;
    }
    std::vector<std::unique_ptr<CollaborationPeer>> guests;
    guests.push_back(std::move(guest));
    // 初始泵送同步身份与默认权限表。
    pumpPeers(host, guests, 3U);

    // 第一阶段授予 Edit 与 Objects，整体权限允许进入细分回调。
    const auto objectEditPermissions =
        static_cast<MMM::Network::Collaboration::CollaborationPermissionMask>(
            MMM::Network::Collaboration::CollaborationPermission::Edit) |
        static_cast<MMM::Network::Collaboration::CollaborationPermissionMask>(
            MMM::Network::Collaboration::CollaborationPermission::Objects);
    if ( !host.setParticipantPermissions(GUEST_ID, objectEditPermissions) ) {
        return false;
    }
    pumpPeers(host, guests, 2U);
    // 访客镜像必须精确等于房主设置的完整掩码。
    const auto guestPermission =
        guests.front()->participantPermissions().find(GUEST_ID);
    if ( guestPermission == guests.front()->participantPermissions().end() ||
         guestPermission->second != objectEditPermissions ) {
        return false;
    }

    // 0x41 具有整体 Edit 权限，但被负载级回调拒绝。
    const ByteBuffer rejectedPayload{ 0x41U };
    if ( guests.front()->submitOperation(rejectedPayload) !=
         SubmitOperationResult::Accepted ) {
        return false;
    }
    pumpPeers(host, guests, 3U);
    // 拒绝发生在 revision 分配前，两侧模型与水位均保持初始状态。
    if ( host.appliedRevision() != 0U ||
         host.stats().unauthorizedEditRequests != 1U || !hostModel.empty() ||
         !guestModel.empty() ) {
        return false;
    }

    // 0x42 同时通过整体与细分授权，应产生第一条权威提交。
    const ByteBuffer acceptedPayload{ 0x42U };
    if ( guests.front()->submitOperation(acceptedPayload) !=
         SubmitOperationResult::Accepted ) {
        return false;
    }
    pumpPeers(host, guests, 3U);
    // 房主与访客各应用一次相同负载并收敛到 revision 1。
    if ( host.appliedRevision() != 1U ||
         guests.front()->appliedRevision() != 1U || hostModel.size() != 1U ||
         guestModel.size() != 1U || hostModel.front() != acceptedPayload ||
         guestModel.front() != acceptedPayload ) {
        return false;
    }

    // 第二阶段移除 Edit 位，仅保留 Objects，细分回调不应再被视为充分。
    const auto objectsOnly =
        static_cast<MMM::Network::Collaboration::CollaborationPermissionMask>(
            MMM::Network::Collaboration::CollaborationPermission::Objects);
    if ( !host.setParticipantPermissions(GUEST_ID, objectsOnly) ) return false;
    pumpPeers(host, guests, 2U);
    // 访客提交入口仍接受请求，权威拒绝必须发生在房主排序阶段。
    if ( guests.front()->submitOperation(acceptedPayload) !=
         SubmitOperationResult::Accepted ) {
        return false;
    }
    pumpPeers(host, guests, 3U);
    // 第二次越权计数增加，但 revision 和两侧模型大小不变。
    return host.appliedRevision() == 1U &&
           host.stats().unauthorizedEditRequests == 2U &&
           hostModel.size() == 1U && guestModel.size() == 1U;
}

/// @brief 验证聊天消息经房主验证转发、重复包去重并拒绝身份伪造。
/// @details
/// 房主与两名访客各自记录聊天回调，合法消息必须在三个收件箱中保持同一顺序。
/// 先开启 Loopback 重复包，分别由房主和 Guest A 发言，验证序号去重既作用于
/// 访客上行，也作用于房主广播。随后保留 Guest A 端点观察指针，绕过公开提交
/// API 注入一帧声称来自 Guest B 的消息，验证房主绑定传输来源和消息 peerId。
/// 伪造帧只能增加 invalidMessages，不能进入任意回调或被继续广播。
/// 场景末尾还从公开入口验证控制字符和超长正文被本地拒绝，区分内容校验与
/// 权威路由校验两类责任。中文合法样本同时覆盖多字节 UTF-8 正常路径。
/// @par 路由契约
/// - 房主本地消息先进入房主回调，再广播给全部访客。
/// - 访客消息先由房主验证，再以权威顺序回显给所有成员。
/// - 相同发送者和 sequence 的重复帧只能展示一次。
/// - 消息声明的 peerId 必须与底层传输来源一致。
/// - 无效帧不能消耗合法发送者的下一聊天序号。
/// @par 内容边界
/// 公开提交入口负责文本合法性，房主接收路径仍必须重复校验，不能信任访客。
/// @return 三端消息记录一致且伪造消息未进入回调时返回 true。
[[nodiscard]] bool testChatRoutingAndValidation()
{
    // 三端分别记录回调消息，验证房主回显和访客广播结果一致。
    constexpr PeerId                                     GUEST_A_ID = 2;
    constexpr PeerId                                     GUEST_B_ID = 3;
    LoopbackTransportHub                                 hub;
    std::array<std::vector<CollaborationChatMessage>, 3> messages;

    CollaborationPeerConfig hostConfig;
    // 房主不需要操作或资源回调，只安装聊天收件箱。
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator = "Host";
    hostConfig.isHost  = true;
    CollaborationPeer host(
        hostConfig,
        hub.createEndpoint(HOST_ID),
        nullptr,
        {},
        [&messages](const CollaborationChatMessage& message) {
            // messages[0] 固定为房主展示顺序。
            messages[0].push_back(message);
        });

    // Guest A 保留底层传输观察指针，用于后续直接注入伪造协议帧。
    CollaborationPeerConfig guestAConfig;
    setTestPeerIdentity(guestAConfig, GUEST_A_ID);
    guestAConfig.creator     = "Guest A";
    guestAConfig.isHost      = false;
    auto  guestATransport    = hub.createEndpoint(GUEST_A_ID);
    auto* rawGuestATransport = guestATransport.get();
    // unique_ptr 随后转移给 Peer，但观察指针在 Peer 生命周期内保持有效。
    auto guestA = std::make_unique<CollaborationPeer>(
        guestAConfig,
        std::move(guestATransport),
        nullptr,
        CollaborationPeer::ResourceMessageCallback{},
        [&messages](const CollaborationChatMessage& message) {
            // messages[1] 对应 Guest A 的权威聊天回调顺序。
            messages[1].push_back(message);
        });

    // Guest B 使用普通端点和独立收件箱，作为广播接收方。
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
            // messages[2] 对应 Guest B。
            messages[2].push_back(message);
        });

    // 两名访客都必须登记成功，房主才能验证其聊天 senderId。
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
    // 先同步三端身份表，聊天处理要求 chat.peerId 已知。
    pumpPeers(host, guests, 4U);

    // 重复模式使每条发送帧入队两次，覆盖聊天序号去重。
    hub.setDuplicatePackets(true);
    // 房主中文消息与访客 ASCII 消息都应由三端按相同顺序展示。
    if ( host.submitChatMessage("房主消息") !=
             SubmitChatMessageResult::Accepted ||
         guests[0]->submitChatMessage("Guest message") !=
             SubmitChatMessageResult::Accepted ) {
        return false;
    }
    pumpPeers(host, guests, 8U);
    // 每个收件箱恰有两条，重复统计在房主和发送访客侧均增加。
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

    // 关闭重复后，由 Guest A 传输伪造声称来自 Guest B 的聊天。
    hub.setDuplicatePackets(false);
    CollaborationChatMessage spoofed{ GUEST_B_ID, 99U, "spoofed" };
    const auto encodedSpoof = encodeCollaborationMessage(spoofed, 1024U);
    // 绕过 submitChatMessage 才能验证房主的传输身份绑定检查。
    if ( !encodedSpoof || !rawGuestATransport->send(HOST_ID, *encodedSpoof) ) {
        return false;
    }
    // 记录注入前计数，泵送后只允许增加一次 invalidMessages。
    const auto invalidMessagesBefore = host.stats().invalidMessages;
    pumpPeers(host, guests, 2U);
    if ( host.stats().invalidMessages != invalidMessagesBefore + 1U ||
         std::any_of(messages.begin(), messages.end(), [](const auto& inbox) {
             return inbox.size() != 2U;
         }) ) {
        return false;
    }

    // 最后验证本地提交入口直接拒绝换行控制字符和超长正文。
    return guests[0]->submitChatMessage("\n") ==
               SubmitChatMessageResult::InvalidMessage &&
           guests[1]->submitChatMessage(
               std::string(MMM::Network::Collaboration::
                                   MAX_COLLABORATION_CHAT_MESSAGE_BYTES +
                               1U,
                           'x')) == SubmitChatMessageResult::InvalidMessage;
}

/// @brief 验证资源消息只能按访客请求、房主响应的角色方向路由。
/// @details
/// 资源同步协议不是任意点对点通道：访客只能向房主发送 ResourceRequest，
/// 房主只能向访客发送 ResourceManifest 与 ResourceChunk。测试在同一连接上
/// 同时尝试三条合法消息和两个反向非法消息，验证公开发送入口先执行角色检查。
/// 双方回调分别保存通过验证的消息变体；推进队列后，房主应只收到请求，访客
/// 应按发送顺序收到清单和分块。这里不验证资源内容拼装，那属于独立的
/// CollaborationResourceSyncTest，本场景只约束 Peer 层消息方向和分派。
/// 使用相同 generation 与偏移可让断言聚焦消息类型而不引入资源状态机前提。
/// @par 角色矩阵
/// - Guest -> Host 的 ResourceRequest 合法。
/// - Guest -> Host 的 ResourceChunk 非法。
/// - Host -> Guest 的 ResourceManifest 合法。
/// - Host -> Guest 的 ResourceChunk 合法。
/// - Host -> Guest 的 ResourceRequest 非法。
/// @par 回调约束
/// 只有通过发送入口和接收身份检查的消息才能进入资源回调；回调中的 PeerId
/// 表示真实传输对端，消息变体则保留协议正文，两者职责不得互相替代。
/// @return 合法方向完整送达且反向发送被拒绝时返回 true。
[[nodiscard]] bool testResourceMessageRouting()
{
    // 单访客足以覆盖访客请求与房主清单/分块两个合法方向。
    constexpr PeerId     GUEST_ID = 2;
    LoopbackTransportHub hub;
    std::vector<MMM::Network::Collaboration::CollaborationMessage>
        hostResources;
    std::vector<MMM::Network::Collaboration::CollaborationMessage>
        guestResources;

    // 房主资源回调只记录已通过角色和发送方检查的消息。
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
            // 回调按值复制变体，便于 update 返回后检查具体类型。
            hostResources.push_back(message);
        });

    // 访客安装对称记录回调以接收房主资源响应。
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
    // 先让访客消费身份和权限广播，建立正常在线状态。
    guest.update();

    // 三种消息共享 generation 和资源定位字段，载荷保持最小确定性。
    const MMM::Network::Collaboration::ResourceRequest request{
        77U, 3U, 1024U, 4096U
    };
    const MMM::Network::Collaboration::ResourceManifest manifest{
        77U, { 9U, 8U, 7U }
    };
    const MMM::Network::Collaboration::ResourceChunk chunk{
        77U, 3U, 1024U, { 1U, 2U, 3U }
    };
    // 合法方向必须成功，访客发 chunk 与房主发 request 必须同步拒绝。
    if ( !guest.sendResourceMessage(HOST_ID, request) ||
         guest.sendResourceMessage(HOST_ID, chunk) ||
         !host.sendResourceMessage(GUEST_ID, manifest) ||
         !host.sendResourceMessage(GUEST_ID, chunk) ||
         host.sendResourceMessage(GUEST_ID, request) ) {
        return false;
    }
    // 双方各推进一次即可消费 Loopback 队列中的所有三条合法消息。
    host.update();
    guest.update();
    if ( hostResources.size() != 1U || guestResources.size() != 2U ||
         !std::holds_alternative<MMM::Network::Collaboration::ResourceRequest>(
             hostResources.front()) ||
         !std::holds_alternative<MMM::Network::Collaboration::ResourceManifest>(
             guestResources[0]) ||
         !std::holds_alternative<MMM::Network::Collaboration::ResourceChunk>(
             guestResources[1]) ) {
        // 房主只看到请求，访客按发送顺序看到清单和分块。
        return false;
    }
    return true;
}

/// @brief 验证稳定协作者重连时复用 PeerId 仍按新操作会话独立去重。
/// @details
/// 首次连接连续发送两条请求，房主的单轮处理上限刻意设为一，使第二条请求
/// 留在旧 SessionId 的传输队列。移除成员并销毁端点后，新 Peer 复用相同
/// PeerId 和 ParticipantId，但换用新的 SessionId，且 clientSequence 从一重启。
/// 房主必须清理旧会话待处理项和去重水位，不能把新请求误判为重复，也不能在
/// 重连后提交旧队列中的第二条操作。最终 revision 2 的来源应同时保留稳定的
/// ParticipantId 与新的 SessionId，身份表也必须原子替换到新会话。
/// 该场景区分“路由槽位复用”和“操作会话延续”，防止临时 PeerId 被错误地
/// 当作跨重连唯一身份。
/// @par 关键水位
/// - 旧会话第一条请求成为 revision 1。
/// - 旧会话第二条请求在成员移除时作废。
/// - 新会话 clientSequence 重新从一开始。
/// - 新请求成为 revision 2，不与旧会话序号冲突。
/// @par 生命周期
/// 旧 CollaborationPeer 必须先销毁以释放 Loopback 端点；房主移除成员则负责
/// 清理权威身份、待处理请求和会话级去重状态，两侧清理缺一不可。
/// @return 旧请求被清理且提交来源保留稳定身份和新会话时返回 true。
[[nodiscard]] bool testReusedPeerIdStartsFreshRequestSequence()
{
    // 房主每轮只处理一条请求，使第二条旧会话请求能留在队列中待清理。
    constexpr PeerId                GUEST_ID = 2;
    LoopbackTransportHub            hub;
    std::vector<CommittedOperation> hostOperations;

    CollaborationPeerConfig hostConfig;
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator                     = "Host";
    hostConfig.isHost                      = true;
    hostConfig.limits.maxRequestsPerUpdate = 1;
    // 记录房主真正提交的操作及其稳定身份和 SessionId。
    CollaborationPeer host(
        hostConfig,
        hub.createEndpoint(HOST_ID),
        [&hostOperations](const CommittedOperation& operation) {
            hostOperations.push_back(operation);
        });

    // 首次访客使用默认 b 类会话 ID，并由房主正常登记。
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

    // 连续提交两条请求，但房主本轮上限只会应用第一条。
    const auto firstOperation  = makeOperation(GUEST_ID, 1);
    const auto queuedOperation = makeOperation(GUEST_ID, 2);
    if ( guest->submitOperation(firstOperation) !=
             SubmitOperationResult::Accepted ||
         guest->submitOperation(queuedOperation) !=
             SubmitOperationResult::Accepted ) {
        return false;
    }
    host.update();
    // revision 1 必须来自第一条，第二条仍属于待处理旧会话。
    if ( host.appliedRevision() != 1 || hostOperations.size() != 1U ||
         hostOperations.front().payload != firstOperation ) {
        return false;
    }

    // 移除访客会清理旧 SessionId 的待处理请求和去重水位。
    host.removeParticipant(GUEST_ID);
    // 销毁旧 Peer 释放 Loopback 端点，允许复用相同临时 PeerId。
    guest.reset();
    // ParticipantId 保持稳定，只把 SessionId 改为 c 类新加入会话。
    guestConfig.sessionId = makeTestStableId(GUEST_ID, 'c');
    guest                 = std::make_unique<CollaborationPeer>(
        guestConfig, hub.createEndpoint(GUEST_ID), nullptr);
    if ( !host.addParticipant(GUEST_ID,
                              guestConfig.participantId,
                              guestConfig.sessionId,
                              guestConfig.creator) ) {
        return false;
    }

    // 新 Peer 的 clientSequence 从一开始，但不应被旧会话水位判重。
    const auto reconnectedOperation = makeOperation(GUEST_ID, 3);
    if ( guest->submitOperation(reconnectedOperation) !=
         SubmitOperationResult::Accepted ) {
        return false;
    }
    host.update();
    // 查询房主身份表，验证相同 PeerId 已整体替换为新 SessionId。
    const auto identity = host.participantIdentities().find(GUEST_ID);
    // 旧队列操作未提交，新操作成为 revision 2 并保留稳定参与者身份。
    return host.appliedRevision() == 2 && hostOperations.size() == 2U &&
           hostOperations.back().payload == reconnectedOperation &&
           hostOperations.back().participantId == guestConfig.participantId &&
           hostOperations.back().sessionId == guestConfig.sessionId &&
           identity != host.participantIdentities().end() &&
           identity->second.participantId == guestConfig.participantId &&
           identity->second.sessionId == guestConfig.sessionId;
}

/// @brief 验证房主拒绝同一稳定协作者或同一操作会话占用多个 PeerId。
/// @details
/// 首名访客正常登记后，完全相同的重复调用应作为幂等操作成功，便于信令重试。
/// 另一 PeerId 若复用 ParticipantId，会造成一个稳定协作者同时占据两个槽位；
/// 若复用 SessionId，则会破坏请求序号的会话级去重，两者都必须拒绝。
/// 已占用的原 PeerId 也不能被另一组身份原地改写，否则旧连接仍可能向该端点
/// 投递消息。所有失败尝试之后，身份表必须只保留房主和最初访客，且字段不变。
/// 本测试只检查房主本地登记不变量，不创建访客端点，以排除广播时序干扰。
/// @par 登记矩阵
/// - 同 PeerId、ParticipantId、SessionId 和 Creator 的重试幂等成功。
/// - 新 PeerId 复用已有 ParticipantId 时失败。
/// - 新 PeerId 复用已有 SessionId 时失败。
/// - 已有 PeerId 改写任一稳定身份时失败。
/// - 拒绝路径不得覆盖最初登记的 Creator 或身份字段。
/// @par 安全边界
/// 唯一性在房主登记时建立，避免冲突身份进入广播后才由各访客自行裁决。
/// @return 身份冲突均被拒绝且原身份保持不变时返回 true。
[[nodiscard]] bool testStableIdentityConflictsAreRejected()
{
    // 两个不同路由槽位用于分别尝试 ParticipantId 与 SessionId 冲突。
    constexpr PeerId     FIRST_GUEST_ID  = 2;
    constexpr PeerId     SECOND_GUEST_ID = 3;
    LoopbackTransportHub hub;

    CollaborationPeerConfig hostConfig;
    // 仅需房主身份状态机，不安装任何业务回调。
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator = "Host";
    hostConfig.isHost  = true;
    CollaborationPeer host(hostConfig, hub.createEndpoint(HOST_ID), nullptr);

    // 第一名访客的两个稳定标识作为后续冲突基准。
    const auto participantId = makeTestStableId(FIRST_GUEST_ID, 'a');
    const auto sessionId     = makeTestStableId(FIRST_GUEST_ID, 'b');
    if ( !host.addParticipant(
             FIRST_GUEST_ID, participantId, sessionId, "First Guest") ||
         // 完全相同的重复登记必须幂等成功。
         !host.addParticipant(
             FIRST_GUEST_ID, participantId, sessionId, "First Guest") ||
         host.addParticipant(SECOND_GUEST_ID,
                             participantId,
                             makeTestStableId(SECOND_GUEST_ID, 'b'),
                             "Duplicate Participant") ||
         // 不同 ParticipantId 但复用旧 SessionId 同样必须拒绝。
         host.addParticipant(SECOND_GUEST_ID,
                             makeTestStableId(SECOND_GUEST_ID, 'a'),
                             sessionId,
                             "Duplicate Session") ||
         // 已占用 PeerId 不能在原地改写为另一组身份。
         host.addParticipant(FIRST_GUEST_ID,
                             makeTestStableId(SECOND_GUEST_ID, 'a'),
                             makeTestStableId(SECOND_GUEST_ID, 'b'),
                             "Changed Identity") ) {
        return false;
    }

    // 所有拒绝后身份表仍只含房主和首名访客。
    const auto identity = host.participantIdentities().find(FIRST_GUEST_ID);
    return host.participantIdentities().size() == 2U &&
           identity != host.participantIdentities().end() &&
           identity->second.participantId == participantId &&
           identity->second.sessionId == sessionId;
}

/// @brief 验证迟加入客户端可用房主完整快照直接越过已裁剪的增量日志。
/// @details
/// 房主先独立提交五条操作，而日志容量只有两条，因此新访客无法从 revision 0
/// 依靠增量日志追到当前状态。房主依次尝试未来、合法、回退和当前 revision
/// 的快照设置，验证快照水位不能超过已应用版本，也不能倒退。
/// 最终保存的 revision 5 快照代表完整物化文档；访客加入后应只调用一次应用
/// 回调并直接把 appliedRevision 推到五，而不是错误重放仅存的 revision 4、5。
/// 房主在加入前发布的视口也必须随初始状态到达，证明快照恢复不会跳过旁路的
/// 成员易失状态同步。场景使用确定轮数推进，不依赖超时或后台线程。
/// @par 快照水位
/// - revision 6 高于房主当前水位，必须拒绝。
/// - revision 4 可作为第一份已物化快照保存。
/// - revision 3 低于已保存水位，必须拒绝回退。
/// - revision 5 可替换旧快照并成为加入同步基线。
/// @par 应用结果
/// 迟到访客无需知道早期五条操作的编码，只把完整快照作为一次状态替换应用；
/// Peer 层仍需把快照 revision 与业务回调结果一起提交，防止状态与水位分离。
/// @return 快照、版本和房主视口均被迟加入访客接收时返回 true。
[[nodiscard]] bool testLateJoinStateSnapshot()
{
    // 房主日志仅保留两条，而加入发生在五次提交后，迫使使用快照。
    constexpr PeerId        GUEST_ID = 2;
    LoopbackTransportHub    hub;
    std::vector<ByteBuffer> hostModel;
    std::vector<ByteBuffer> guestModel;

    CollaborationPeerConfig hostConfig;
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator                     = "Host";
    hostConfig.isHost                      = true;
    hostConfig.limits.maxJournalOperations = 2;
    // 房主模型记录五条普通操作，用于建立当前 revision 5。
    CollaborationPeer   host(hostConfig,
                             hub.createEndpoint(HOST_ID),
                             [&hostModel](const CommittedOperation& operation) {
                               hostModel.push_back(operation.payload);
                             });
    ParticipantViewport hostViewport;
    // 加入前发布视口，验证新访客初始同步不仅包含文档快照。
    hostViewport.playbackTime          = 4.0;
    hostViewport.visualTime            = 4.1;
    hostViewport.visibleTimeStart      = 1.0;
    hostViewport.visibleTimeEnd        = 8.0;
    hostViewport.horizontalOffsetRatio = 0.2;
    if ( !host.publishViewport(hostViewport) ) return false;
    // 逐条提交并立即 update，使日志最终只保留 revision 4 和 5。
    for ( std::uint8_t index = 1; index <= 5; ++index ) {
        const auto operation = makeOperation(HOST_ID, index);
        if ( host.submitOperation(operation) !=
             SubmitOperationResult::Accepted ) {
            return false;
        }
        host.update();
    }
    // partial 与 full 分别代表 revision 4 和 5 的完整物化内容。
    const ByteBuffer partialSnapshot{ 0xFA, 0xCE, 0x04 };
    const ByteBuffer fullSnapshot{ 0xFA, 0xCE, 0x05 };
    if ( host.appliedRevision() != 5 ||
         // 未来 revision 6 不得保存。
         host.setStateSnapshot(6, fullSnapshot) ||
         // 首次保存 revision 4 合法。
         !host.setStateSnapshot(4, partialSnapshot) ||
         // 快照不允许从 4 回退到 3。
         host.setStateSnapshot(3, partialSnapshot) ||
         // 最终用当前 revision 5 替换为最新完整快照。
         !host.setStateSnapshot(5, fullSnapshot) ) {
        return false;
    }

    // 迟加入访客只记录收到的状态应用回调。
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
    // 交替推进让身份、视口、权限和快照依次到达并确认。
    for ( std::size_t round = 0; round < 4; ++round ) {
        guest.update();
        host.update();
    }
    // 访客直接应用一次 fullSnapshot，水位追平而无需重放五条历史操作。
    return guest.appliedRevision() == host.appliedRevision() &&
           guest.participantViewports().contains(HOST_ID) &&
           guestModel.size() == 1U && guestModel.front() == fullSnapshot;
}

/// @brief 验证在线访客缺失的版本已经裁剪时自动回退房主最新快照。
/// @details
/// 与迟加入场景不同，此处访客已在线，但定向丢失第一条提交。房主继续推进到
/// revision 5 后，两条容量的日志只剩末尾版本，无法响应从 revision 1 开始的
/// 增量补发。访客收到未来提交后发起 resync，房主必须记录一次不可用并发送
/// 当前完整快照，而不是反复发送残缺日志或让访客永久停在缺口前。
/// 访客模型最终只包含快照负载，appliedRevision 与房主相等；这同时证明应用
/// 快照会替代积压的未来提交，并能正常回送确认。
/// 本测试覆盖在线故障恢复，与迟加入场景的初始同步职责互补。
/// @par 恢复链
/// - revision 1 被定向丢弃，访客仍保持初始水位。
/// - 后续未来提交使访客检测到连续性缺口。
/// - ResyncRequest 请求从下一期望版本开始补发。
/// - 房主发现日志首项已晚于请求起点，增加 resyncUnavailable。
/// - StateSnapshot 替代不可得增量并把访客直接推进到 revision 5。
/// - 访客确认新水位，房主可继续后续正常增量同步。
/// @return 房主记录日志不可用且访客通过快照追平时返回 true。
[[nodiscard]] bool testTrimmedJournalSnapshotFallback()
{
    // 在线慢访客先丢失 revision 1，之后房主日志滚动到无法覆盖该缺口。
    constexpr PeerId        GUEST_ID = 2;
    LoopbackTransportHub    hub;
    std::vector<ByteBuffer> guestModel;

    CollaborationPeerConfig hostConfig;
    setTestPeerIdentity(hostConfig, HOST_ID);
    hostConfig.creator                     = "Host";
    hostConfig.isHost                      = true;
    hostConfig.limits.maxJournalOperations = 2;
    // 房主无需记录本地模型，只关注补发不可用统计和当前 revision。
    CollaborationPeer host(hostConfig, hub.createEndpoint(HOST_ID), nullptr);

    // 访客模型用于证明最终只应用一份完整快照。
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
    // 先消费身份同步，确保后续快照能取得房主稳定身份。
    guest.update();

    // 定向丢弃房主发给访客的下一包，即第一条提交广播。
    hub.dropNextPacket(HOST_ID, GUEST_ID);
    // 连续五次提交让两条日志窗口不再包含 revision 1。
    for ( std::uint8_t index = 1; index <= 5; ++index ) {
        const auto operation = makeOperation(HOST_ID, index);
        if ( host.submitOperation(operation) !=
             SubmitOperationResult::Accepted ) {
            return false;
        }
        host.update();
    }
    // 房主在当前 revision 5 保存可直接恢复的完整快照。
    const ByteBuffer fullSnapshot{ 0xFA, 0xCE, 0x55 };
    if ( !host.setStateSnapshot(fullSnapshot) ) return false;

    // 访客看到未来提交请求补发，房主发现日志缺口后发送快照。
    guest.update();
    host.update();
    // 再次交替推进完成快照应用与 RevisionAck 返回。
    guest.update();
    host.update();
    return host.stats().resyncUnavailable > 0U &&
           guest.appliedRevision() == host.appliedRevision() &&
           guestModel.size() == 1U && guestModel.front() == fullSnapshot;
}
}  // namespace

/// @brief 运行协作增量同步回归测试。
/// @details
/// 各场景返回独立退出码，CTest 失败时无需额外日志即可快速定位协议层、消息
/// 路由、增量收敛、快照、重连身份或权限控制中的哪类契约被破坏。
/// 测试顺序先运行无状态协议边界，再运行较小消息路由，随后进入多 Peer 状态机
/// 和恢复流程；所有 Hub 与 Peer 都在场景内部构造，场景之间不会共享状态。
/// 本入口不捕获异常；项目禁用异常机制，场景以显式布尔结果报告失败。
/// @par 退出码分组
/// - 1～3 对应协议、聊天和资源消息边界。
/// - 4 对应最大房间的增量收敛。
/// - 5～6 对应迟加入与在线缺口的快照恢复。
/// - 7～8 对应重连会话和稳定身份唯一性。
/// - 9 对应房主权威权限判定。
/// @return 全部测试通过时返回 0。
int main()
{
    // 先验证纯协议边界，失败码一对应编解码层。
    if ( !testProtocolBounds() ) return 1;
    // 聊天和资源路由分别使用失败码二、三。
    if ( !testChatRoutingAndValidation() ) return 2;
    if ( !testResourceMessageRouting() ) return 3;
    // 最大规模增量收敛是核心状态机场景。
    if ( !testEightPeerIncrementalConvergence() ) return 4;
    // 两种快照路径分别覆盖迟加入和在线日志缺口。
    if ( !testLateJoinStateSnapshot() ) return 5;
    if ( !testTrimmedJournalSnapshotFallback() ) return 6;
    // 重连和身份冲突覆盖稳定身份生命周期边界。
    if ( !testReusedPeerIdStartsFreshRequestSequence() ) return 7;
    if ( !testStableIdentityConflictsAreRejected() ) return 8;
    // 权限场景最后运行，覆盖 revision 分配前授权顺序。
    if ( !testParticipantPermissionsAreAuthoritative() ) return 9;
    // 全部九组场景通过。
    return 0;
}
