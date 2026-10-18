#pragma once

#include "network/collaboration/CollaborationTypes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

namespace MMM::Network::Collaboration
{
/// @brief 返回本进程单调时钟秒数，不使用可能被校时修改的系统日期。
inline double playbackSteadySeconds()
{
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// @brief 一次往返探测得到的远端减本地时钟偏移及网络往返耗时。
struct PlaybackClockEstimate {
    /// @brief 远端单调时钟减本地单调时钟，允许设备启动时间完全不同。
    double offset = 0.0;
    /// @brief 去掉远端处理时间后的往返耗时，单位秒。
    double roundTrip = 0.0;
};

/// @brief 用四个单调时刻估计时钟偏移，不假定两台机器系统时间一致。
/// @note 对称链路假设只能近似补偿；拒绝超时和倒序样本，不能精确消除非对称延迟。
/// @param sent 本地发送请求时刻。
/// @param received 远端接收请求时刻。
/// @param replied 远端发送回复时刻。
/// @param returned 本地接收回复时刻。
/// @return 可用估计；网络往返超界或时钟次序非法时为空。
inline std::optional<PlaybackClockEstimate> estimatePlaybackClock(
    double sent, double received, double replied, double returned)
{
    // 先验证时间顺序，防止异常应答把延迟估计变成负值。
    if ( !std::isfinite(sent) || !std::isfinite(received) ||
         !std::isfinite(replied) || !std::isfinite(returned) ||
         returned < sent || replied < received )
        return std::nullopt;
    const double rtt = returned - sent - (replied - received);
    // 两秒以上的探测已失去交互参考价值，继续沿本地时钟播放等待下一轮。
    if ( rtt < 0.0 || rtt > 2.0 ) return std::nullopt;
    return PlaybackClockEstimate{
        ((received - sent) + (replied - returned)) * 0.5, rtt
    };
}

/// @brief 本次跟随需要交给逻辑线程的离散传输动作。
struct PlaybackFollowAction {
    /// @brief 是否需要定位；连续播放的小漂移不触发音频重新定位。
    bool seek = false;
    /// @brief 目标原始谱面时间，单位秒。
    double time = 0.0;
    /// @brief 跟随端应采用的传输状态。
    bool playing = false;
    /// @brief 跟随端应采用的播放倍率。
    double rate = 1.0;
};

/// @brief 对跟随音频的所有权、序号和漂移阈值进行常量成本判定。
/// @note 不调用音频或网络 API，便于使用确定性时钟验证状态转换。
class PlaybackFollowTracker
{
public:
    /// @brief 消费最新远端状态；退出、失联或样本过期时只停止本功能接管的播放。
    /// @warning 每帧路径只比较固定字段，不阻塞、不分配、不反复 Seek。
    /// @param peer 当前跟随对象，零表示取消或对象已离开。
    /// @param viewport 最新状态或按顺序消费的播放边界，指针仅在调用内借用。
    /// @param enabled 当前房间是否允许跟随音乐。
    /// @param targetTime 经过时钟校准并外推到当前时刻的远端播放位置。
    /// @param localTime 外推到同一时刻的本地播放位置。
    /// @param localPlaying 本地实际音频传输状态。
    /// @param now 接收端当前单调时间，用于判断样本过期。
    /// @return 必须按顺序发布的离散动作；没有变化时为空。
    std::optional<PlaybackFollowAction> update(
        PeerId peer, const ParticipantViewport* viewport, bool enabled,
        double targetTime, double localTime, bool localPlaying, double now)
    {
        // 播放状态超过两秒未刷新视为失联；暂停样本可无限保留视角位置。
        const bool stale =
            viewport && viewport->playing && now - viewport->receivedAt > 2.0;
        if ( !enabled || peer == 0 || !viewport || stale ) {
            const bool stop = m_owned;
            m_owned         = false;
            m_peer          = 0;
            m_sequence      = 0;
            if ( stop )
                return PlaybackFollowAction{ false, 0.0, false, m_rate };
            return std::nullopt;
        }
        // 切换目标或首次启用立即对齐；同一序号不能重复覆盖本地传输。
        const bool first = peer != m_peer || m_sequence == 0;
        // 先消费边界队列再读最新缓存时，两者可能指向相同序号。
        // 迟到的旧缓存不能使暂停重新变成播放，也不能降低消费水位。
        if ( !first && viewport->sequence <= m_sequence ) return std::nullopt;
        const bool transition  = first || viewport->playing != m_playing;
        const bool rateChanged = viewport->playbackRate != m_rate;
        m_peer                 = peer;
        m_sequence             = viewport->sequence;
        m_playing              = viewport->playing;
        m_rate                 = viewport->playbackRate;
        // 只在新状态到达时检查漂移；小于 150 毫秒的误差由本地连续播放吸收。
        const bool seek =
            transition || rateChanged ||
            (!viewport->playing && std::abs(targetTime - localTime) > 0.001) ||
            std::abs(targetTime - localTime) > 0.15;
        m_owned = viewport->playing;
        if ( seek || localPlaying != viewport->playing || rateChanged )
            return PlaybackFollowAction{
                seek, targetTime, viewport->playing, m_rate
            };
        return std::nullopt;
    }

private:
    /// @brief 当前跟随目标；零表示尚未接管。
    PeerId m_peer = 0;
    /// @brief 已消费序号，保证同一消息至多产生一次传输动作。
    std::uint64_t m_sequence = 0;
    /// @brief 本功能是否接管了正在播放的音频，用于退出时收尾。
    bool m_owned = false;
    /// @brief 上次远端播放状态，用于识别播放与暂停边界。
    bool m_playing = false;
    /// @brief 最近的播放倍率，停播收尾不改变倍率。
    double m_rate = 1.0;
};
}  // namespace MMM::Network::Collaboration
