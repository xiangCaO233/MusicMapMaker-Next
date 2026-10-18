#include "network/collaboration/CollaborationPlaybackSync.h"
#include "network/collaboration/CollaborationPeer.h"
#include "network/collaboration/CollaborationRoom.h"
#include "network/collaboration/LoopbackTransport.h"

#include <cmath>
#include <limits>
#include <string>

namespace
{
using namespace MMM::Network::Collaboration;

/// @brief 验证设备单调时钟相差很大时，往返估计仍能得到正确偏移。
/// @return 偏移、网络时间和非法样本边界全部符合预期时返回 true。
/// @note 所有时刻由测试给定，不通过 sleep 模拟传播延迟。
bool checkClock()
{
    // 去程 50 毫秒、回程 50 毫秒、远端处理 20 毫秒。
    // 远端单调时钟比本地快九百秒，不能直接相减当作网络延迟。
    const auto estimate =
        estimatePlaybackClock(100.0, 1000.05, 1000.07, 100.12);
    // 两端绝对数值相差九百秒，结果应仍为一百毫秒网络往返。
    // 减去二十毫秒处理时间，避免把远端回调排队算入链路往返。
    if ( !estimate || std::abs(estimate->offset - 900.0) > 1e-9 ||
         std::abs(estimate->roundTrip - 0.1) > 1e-9 )
        return false;
    // 拒绝处理时刻倒序、超时与非有限数据，不让异常样本污染校准。
    return !estimatePlaybackClock(1.0, 2.0, 1.9, 1.2) &&
           !estimatePlaybackClock(1.0, 2.0, 2.0, 4.0) &&
           !estimatePlaybackClock(
               1.0, 2.0, 2.0, std::numeric_limits<double>::infinity());
}

/// @brief 验证连续播放暂停全部产生边界，连续播放的小漂移不反复定位。
/// @return 边界顺序、重复过滤、漂移阈值与退出收尾都成立时返回 true。
/// @details 相反的本地状态模拟跟随端尚未处理新的传输命令。
/// 无音频设备也可验证是否生成正确动作，实际命令由画布桥接入口发送。
bool checkFollow()
{
    PlaybackFollowTracker tracker;
    ParticipantViewport   viewport;
    viewport.peerId     = 2;
    viewport.receivedAt = 10.0;
    // 目标身份与序号独立，只更新序号，不通过重复构造对象重置状态机。
    // 用户没有启用跟随音乐时，远端播放不接管本地音频。
    viewport.sequence = 1;
    viewport.playing  = true;
    if ( tracker.update(2, &viewport, false, 5.0, 0.0, false, 10.0) )
        return false;
    // 同一目标连续切换四次，每次都必须返回对应播放状态。
    for ( const bool playing : { true, false, true, false } ) {
        // 每个状态使用不同序号，不能被重复包过滤分支吞掉。
        viewport.playing = playing;
        const auto action =
            tracker.update(2, &viewport, true, 5.0, 5.0, !playing, 10.0);
        if ( !action || action->playing != playing || !action->seek )
            return false;
        ++viewport.sequence;
    }
    // 恢复播放后微小时间误差无需 Seek，同一序号也不能重复处理。
    viewport.playing = true;
    if ( !tracker.update(2, &viewport, true, 5.0, 5.0, false, 10.0) )
        return false;
    ++viewport.sequence;
    // 四十毫秒漂移低于允许窗口，持续播放不能返回重定位动作。
    if ( tracker.update(2, &viewport, true, 5.04, 5.0, true, 10.0) )
        return false;
    // 同一序号即使重新外推时间，也不能再应用为新的远端操作。
    if ( tracker.update(2, &viewport, true, 6.0, 5.0, true, 10.0) )
        return false;
    // 新序号的大漂移应校正；关闭跟随只能停止本功能接管的播放一次。
    ++viewport.sequence;
    if ( !tracker.update(2, &viewport, true, 6.0, 5.0, true, 10.0) )
        return false;
    const auto stopped =
        tracker.update(0, nullptr, false, 0.0, 0.0, true, 10.0);
    // 收尾暂停不携带 Seek，否则退出跟随会把本地跳回零秒。
    // 本地参数仍为播放中，确保收尾不是依靠本地先暂停才通过。
    if ( !stopped || stopped->playing || stopped->seek ) return false;
    // 第二次关闭为空，不会每帧把用户后续手动播放再次暂停。
    if ( tracker.update(0, nullptr, false, 0.0, 0.0, true, 10.0) ) return false;
    // 目标突然断流时使用非阻塞过期判定，不让音乐无限继续播放。
    if ( !tracker.update(2, &viewport, true, 6.0, 5.0, false, 10.0) )
        return false;
    const auto stale = tracker.update(2, &viewport, true, 6.0, 6.0, true, 12.1);
    // 时间推进由参数表达，不阻塞等待真实两秒超时。
    return stale && !stale->playing;
}

/// @brief 构造遵循线上身份格式的确定性 Peer 参数。
/// @param peer 一到三的房间路由槽位。
/// @return 角色、房主路由和两类稳定身份都完整的测试配置。
/// @note 本地 Creator 必须与房主登记值一致，身份保护不允许伪造自身。
CollaborationPeerConfig config(PeerId peer)
{
    CollaborationPeerConfig result;
    result.peerId     = peer;
    result.hostPeerId = 1;
    result.isHost     = peer == 1;
    result.creator = peer == 1 ? "Host" : (peer == 2 ? "Leader" : "Follower");
    // 不同连接使用不同稳定身份，字符串仍满足三十二位十六进制约束。
    result.participantId = std::string(32, static_cast<char>('0' + peer));
    result.sessionId     = std::string(32, static_cast<char>('a' + peer));
    // 两类稳定标识使用不同字符域，避免混用仍恰好相等。
    // 测试只使用一到三，两类标识始终位于十六进制字符集合。
    return result;
}

/// @brief 验证访客间时钟探测和同一收包轮中的快速播放暂停序列。
/// @return 状态切换按序送达且远端时间补偿包含采样年龄时返回 true。
/// @details 两个访客只有房主可转发，不能通过绕过房主的直连通过测试。
/// 此场景不提交文档操作，播放边界不应需要成员编辑权限。
bool checkRouting()
{
    LoopbackTransportHub hub;
    const auto           apply = [](const CommittedOperation&) {};
    // 空回调只满足文档接口，传输测试不依赖任何谱面实体。
    // 所有端点绑定同一个 Hub，房主中继与产品的星型拓扑一致。
    CollaborationPeer host(config(1), hub.createEndpoint(1), apply);
    CollaborationPeer leader(config(2), hub.createEndpoint(2), apply);
    CollaborationPeer follower(config(3), hub.createEndpoint(3), apply);
    if ( !host.isValid() || !leader.isValid() || !follower.isValid() )
        return false;
    // 先登记成员并完整泵出身份消息，保证时钟路由只能指向已知成员。
    if ( !host.addParticipant(
             2, config(2).participantId, config(2).sessionId, "Leader") ||
         !host.addParticipant(
             3, config(3).participantId, config(3).sessionId, "Follower") )
        return false;
    leader.update();
    follower.update();
    // 访客三跟随访客二；请求和回复必须各经过房主，而非假定直连。
    follower.probePlaybackClock(2);
    // 第一轮房主转发请求，第二轮转发回复，两个方向都参与计时。
    // 不通过睡眠推进时间，真实单调采样在消息处理轮之间自然增长。
    host.update();
    leader.update();
    host.update();
    follower.update();
    // 发布播放倍率和采样时刻，随后确认新增字段完整往返。
    ParticipantViewport viewport;
    viewport.playbackTime = 3.0;
    viewport.playbackRate = 1.5;
    viewport.sampledAt    = playbackSteadySeconds() - 0.05;
    // 提前采样刻意覆盖网络入队之前就已存在的 UI 处理延迟。
    // 一点五倍速会将五十毫秒采样年龄换算为七十五毫秒音乐进度。
    for ( const bool playing : { true, false, true, false } ) {
        viewport.playing = playing;
        if ( !leader.publishViewport(viewport) ) return false;
    }
    // 一次处理全部四次切换：最新视口为暂停，但事件队列仍保留每一个边界。
    host.update();
    follower.update();
    std::uint64_t sequence = 0;
    // 不在四次发布之间消费，复现同一收包轮多个边界被覆盖的场景。
    // 初始缓存不存在，第一条播放同样必须成为可消费边界。
    // 后三条反复改变 playing，不能只验证最终的暂停结果。
    for ( const bool playing : { true, false, true, false } ) {
        const auto transition = follower.popPlaybackTransition();
        if ( !transition || transition->playing != playing ||
             transition->sequence <= sequence ||
             transition->playbackRate != 1.5 ||
             transition->sampledAt != viewport.sampledAt )
            return false;
        sequence = transition->sequence;
        // 只对播放检查外推；暂停位置固定，不能额外乘以采样年龄。
        // 每条都核对原采样时间，证明房主没有用转发时间替换它。
        if ( playing && follower.estimatePlaybackTime(*transition) < 3.04 )
            return false;
    }
    if ( follower.popPlaybackTransition() ) return false;
    // 耗尽后必须为空，不能把普通位置缓存重复解释成播放边界。
    // 停止跟随后清空目标状态，重新进入不能复用旧时钟与旧事件。
    follower.probePlaybackClock(0);
    // 无效计数为零证明校准回复通过身份验证，而不是被丢弃后使用回退时间。
    // 清除目标后不得残留旧目标的暂停，避免污染下一次跟随。
    return !follower.popPlaybackTransition() &&
           follower.stats().invalidMessages == 0;
}
}  // namespace

/// @brief 运行时钟、跟随边界与房主中继回归，不依赖真实音频设备或公网。
/// @return 独立退出码区分开关默认、时钟公式、状态机和真实协议路由失败。
int main()
{
    // 房间开关默认关闭；选择只保存在房间，不修改用户全局音频设置。
    MMM::Network::Collaboration::CollaborationRoom room;
    if ( room.followPlaybackEnabled() ) return 1;
    room.setFollowPlaybackEnabled(true);
    if ( !room.followPlaybackEnabled() ) return 2;
    if ( !checkClock() ) return 3;
    if ( !checkFollow() ) return 4;
    if ( !checkRouting() ) return 5;
    return 0;
}
