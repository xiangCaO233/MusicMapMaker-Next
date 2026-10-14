#include "network/collaboration/CollaborationPeer.h"

namespace MMM::Network::Collaboration
{
/// @brief 启动当前跟随目标的低频单调时钟探测。
/// @param peerId 已登记的远端参与者，零值清除校准。
/// @warning 每帧调用但每秒最多发一次消息，禁止阻塞等待响应。
/// @details 使用一秒截止时间限频，截止前线程继续推进普通协作消息。
/// 当前目标离开或跟随被关闭时，零值调用同步清空校准和边界缓存。
void CollaborationPeer::probePlaybackClock(PeerId peerId)
{
    // 只维护一个实际跟随目标，避免所有成员之间产生平方级探测流量。
    if ( peerId != m_playbackClockPeer ) {
        // PeerId 可复用：成员退出清理路径也必须调用本入口。
        // 清空请求时刻让旧目标迟到的回复无法匹配新目标请求。
        m_playbackClockPeer = peerId;
        m_playbackClockEstimate.reset();
        m_nextPlaybackClockProbe = 0.0;
        m_playbackClockSent      = 0.0;
        m_playbackTransitions.clear();
    }
    if ( !m_valid || peerId == 0 || peerId == m_config.peerId ||
         !m_participantIdentities.contains(peerId) )
        return;
    const double now = playbackSteadySeconds();
    if ( now < m_nextPlaybackClockProbe ) return;
    // 下一轮覆盖未完成请求，迟到回应因 sentAt 不匹配而被忽略。
    // 请求失败也限频，不能因连接断开在每帧重发。
    m_nextPlaybackClockProbe = now + 1.0;
    m_playbackClockSent      = now;
    const PlaybackClockProbe probe{ m_config.peerId, peerId, false, now };
    // 星型拓扑访客没有直连另一访客，端到端测量必须包含两跳路由。
    static_cast<void>(
        sendMessage(m_config.isHost ? peerId : m_config.hostPeerId, probe));
}

/// @brief 依次消费播放切换，避免一轮 update 内的播放暂停被最新视口覆盖。
/// @return 最早尚未消费的边界；没有边界时为空。
std::optional<ParticipantViewport> CollaborationPeer::popPlaybackTransition()
{
    if ( m_playbackTransitions.empty() ) return std::nullopt;
    const auto result = m_playbackTransitions.front();
    // 返回值持有完整轻量状态，不把 deque 元素指针暴露给后续帧。
    m_playbackTransitions.pop_front();
    return result;
}

/// @brief 根据校准时钟估计远端当前位置，避免传输和处理延迟累积。
/// @param viewport 最新已验证视口，receivedAt 属于本地单调时钟。
/// @return 外推后的原始谱面秒数；暂停时严格使用发布位置。
/// @warning UI 高频读取只访问已发布状态和单调时钟，不轮询或等待网络。
double CollaborationPeer::estimatePlaybackTime(
    const ParticipantViewport& viewport) const
{
    if ( !viewport.playing ) return viewport.playbackTime;
    // offset 定义为远端减本地，因此原始远端采样时刻要减去 offset。
    // receivedAt 是当前端重新记录的时刻，不随房主中继传给下一接收端。
    // 未校准前保持可用，从本地收包时刻外推而不假定系统时钟一致。
    double sampledLocally = viewport.receivedAt;
    if ( viewport.peerId == m_playbackClockPeer && m_playbackClockEstimate )
        sampledLocally = viewport.sampledAt - m_playbackClockEstimate->offset;
    // 异常时延不允许把音频外推到任意远处；过期停播由跟随状态机处理。
    const double elapsed =
        std::clamp(playbackSteadySeconds() - sampledLocally, 0.0, 2.0);
    return viewport.playbackTime + elapsed * viewport.playbackRate;
}

/// @brief 校验探测身份、路由目标和回应匹配，估计端到端时钟偏移。
/// @param senderId 可靠传输提供的直接发送方。
/// @param probe 固定大小请求或回复，不携带谱面修改。
/// @warning 网络 update 路径只处理固定字段，不建立后台等待任务。
/// @details 探测与文档 revision 解耦，任何回应都不产生谱面编辑或授权变更。
void CollaborationPeer::handlePlaybackClockProbe(
    PeerId senderId, const PlaybackClockProbe& probe)
{
    // 访客只接受房主中继，房主要求负载源与直接连接一致，防止跨成员伪造。
    const bool sourceValid =
        m_config.isHost
            ? m_participants.contains(senderId) && probe.source == senderId
            : senderId == m_config.hostPeerId &&
                  m_participantIdentities.contains(probe.source);
    if ( !sourceValid || probe.source == probe.target ||
         !m_participantIdentities.contains(probe.target) ) {
        ++m_stats.invalidMessages;
        return;
    }
    if ( probe.target != m_config.peerId ) {
        // 校验目标存在后才发送，防止把探测用于向任意路由槽投递内容。
        // 仅房主可转发给另一已知访客；访客不能替房主再次中继。
        if ( m_config.isHost )
            static_cast<void>(sendMessage(probe.target, probe));
        else
            ++m_stats.invalidMessages;
        return;
    }
    const double now = playbackSteadySeconds();
    // 回复交换源与目标，但原始 sentAt 必须完全保持以匹配请求。
    // 不广播探测回复，只有原请求者需要本轮时钟估计。
    if ( !probe.reply ) {
        // 收包与回包均在同一非阻塞处理轮；仍分别采样以剔除本地处理耗时。
        PlaybackClockProbe response{
            m_config.peerId, probe.source, true,
            probe.sentAt,    now,          playbackSteadySeconds()
        };
        static_cast<void>(sendMessage(
            m_config.isHost ? probe.source : m_config.hostPeerId, response));
        return;
    }
    // 原封不动回传发送时刻，只接受当前目标当前轮次，旧回复不能覆盖新估计。
    if ( probe.source != m_playbackClockPeer ||
         probe.sentAt != m_playbackClockSent )
        return;
    const auto estimate = estimatePlaybackClock(
        probe.sentAt, probe.receivedAt, probe.repliedAt, now);
    if ( !estimate ) return;
    // 失败保留旧偏移，不倒退音频，也不立刻重复探测。
    // 首次估计直接使用，后续平滑只对接近低延迟基线的样本开放。
    // 网络暂时排队时宁可保留旧偏移，不能把排队尖峰误认为设备时钟跳变。
    // 优先较低 RTT 的样本，排除网络排队尖峰；小幅更新抑制时钟估计抖动。
    if ( !m_playbackClockEstimate )
        m_playbackClockEstimate = estimate;
    else if ( estimate->roundTrip <=
              m_playbackClockEstimate->roundTrip + 0.01 ) {
        m_playbackClockEstimate->offset +=
            (estimate->offset - m_playbackClockEstimate->offset) * 0.2;
        m_playbackClockEstimate->roundTrip = estimate->roundTrip;
    }
}
}  // namespace MMM::Network::Collaboration
