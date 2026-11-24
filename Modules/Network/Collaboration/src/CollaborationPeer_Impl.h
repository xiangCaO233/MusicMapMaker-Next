#pragma once

#include "network/collaboration/CollaborationPeer.h"
#include "network/collaboration/CollaborationPlaybackSync.h"

#include <deque>

namespace MMM::Network::Collaboration
{
/// @brief Peer 私有状态由网络更新线程独占，跟随切换不改变编辑修订水位。
struct CollaborationPeer::Impl {
    /// @brief 跟随图、视口和目标的播放校准随同一 Peer 会话存活。
    struct FollowPlayback {
        /// @brief 启用新目标时清空校准及旧目标尚未消费的播放边界。
        /// @param peerId 零值主动结束跟随。
        /// @warning 每帧检查目标，只有目标切换会清理缓存。
        void selectClockPeer(PeerId peerId);

        /// @brief 当前单一跟随目标的时钟状态，切换目标后不复用旧偏移。
        PeerId m_playbackClockPeer = 0;
        /// @brief 本轮请求的发送时刻，用于拒绝伪造或过期回复。
        double m_playbackClockSent = 0.0;
        /// @brief 非阻塞探测下次允许发送的时间。
        double m_nextPlaybackClockProbe = 0.0;
        /// @brief 最近可用偏移估计；尚未校准时为空。
        std::optional<PlaybackClockEstimate> m_playbackClockEstimate;
        /// @brief
        /// 有界播放切换队列；仅缓存当前跟随目标，避免所有参与者队列增长。
        std::deque<ParticipantViewport> m_playbackTransitions;
        /// @brief 当前客户端下一个本地视口状态序号。
        std::uint64_t m_nextViewportSequence = 1;
        /// @brief 当前客户端下一条跟随请求序号，与视口序号互相独立。
        std::uint64_t m_nextFollowRequestSequence = 1;
        /// @brief 最新待确认跟随请求，零表示没有待处理请求。
        std::uint64_t m_pendingFollowRequestSequence = 0;
        /// @brief 最新匹配确认的身份与结果快照，不能被后续离开清理覆盖。
        std::optional<ParticipantFollowState> m_followConfirmation;
        /// @brief
        /// 最新已发送请求的完整目标身份，供等待期间展示选择且防止槽位复用。
        std::optional<ParticipantFollowState> m_pendingFollowRequest;
        /// @brief 房主权威跟随图；访客仅保存已确认边，零目标不占表项。
        std::unordered_map<PeerId, PeerId> m_participantFollowTargets;
        /// @brief 房主为每名访客保留的请求水位，拒绝重复和倒序请求。
        std::unordered_map<PeerId, std::uint64_t> m_lastFollowRequestSequence;
        /// @brief 各参与者最近一次通过序号校验的主画布视口状态。
        std::unordered_map<PeerId, ParticipantViewport> m_participantViewports;
    };

    /// @brief 待提交请求、回执与有界补发窗口由权威提交状态统一持有。
    struct CommitLog {
        /// @brief 保留最新的提交，淘汰超出补发窗口的历史记录。
        /// @param operation 已应用到本地的完整操作。
        /// @param limit 房主配置的日志长度上限。
        void record(const CommittedOperation& operation, std::size_t limit);

        /// @brief 当前客户端下一个本地请求序号。
        std::uint64_t m_nextClientSequence = 1;
        /// @brief 房主要分配的下一个全房间版本。
        std::uint64_t m_nextRevision = 1;
        /// @brief 当前客户端已经连续应用的最高版本。
        std::uint64_t m_appliedRevision = 0;
        /// @brief 当前补发流程期望追赶到的版本。
        std::optional<std::uint64_t> m_resyncTargetRevision;
        /// @brief 房主尚未排序提交的本地和远端请求。
        std::deque<EditRequest> m_pendingRequests;
        /// @brief 房主为每个加入会话接受的最高请求序号。
        std::unordered_map<OperationSessionId, std::uint64_t>
            m_lastAcceptedSequence;
        /// @brief 房主保留的有界已提交操作日志。
        std::deque<CommittedOperation> m_journal;
        /// @brief 房主记录的各访客最高连续确认版本。
        std::unordered_map<PeerId, std::uint64_t> m_lastAcknowledgedRevision;
        /// @brief 房主最近一次可独立恢复的完整谱面快照。
        std::optional<StateSnapshot> m_stateSnapshot;
    };

    /// @brief 当前 Peer 的视口与播放跟随状态。
    FollowPlayback m_followPlayback;
    /// @brief 当前 Peer 的提交日志和补发状态。
    CommitLog m_commits;
};
}  // namespace MMM::Network::Collaboration
