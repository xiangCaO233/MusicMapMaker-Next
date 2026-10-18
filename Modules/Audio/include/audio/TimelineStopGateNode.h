#pragma once
#include "audio/AudioTimelineMixerNode.h"
#include <ice/core/IAudioNode.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <memory>
#include <utility>

namespace MMM::Audio
{
/// @brief 在统一拉伸器之前冻结整棵谱面音频图。
/// @details STOP 时不拉取上游，因此 BGM、Key
/// 音和自定义采样均保留源游标与效果器状态。 静音仍交给统一拉伸器消费，使 STOP
/// 持续时间遵循同一播放倍率。
class TimelineStopGateNode final : public ice::IAudioNode
{
public:
    /// @brief 构造常驻图节点；时间线的生命周期必须覆盖本节点。
    /// @warning 共享所有权仅在图建立时转移，音频回调不复制所有权。
    TimelineStopGateNode(std::shared_ptr<ice::IAudioNode> input,
                         AudioTimelineMixerNode&          timeline)
        : m_input(std::move(input)), m_timeline(timeline)
    {
    }

    /// @brief 消费已由边界回调固定的区间；暂停段只输出静音。
    /// @warning 每音频块调用；不分配、不等待、不扫描事件表。
    void process(ice::AudioBuffer& buffer) override
    {
        // 不调用子节点才能真正暂停音频，而非只把继续播放的结果静音。
        if ( m_timeline.consumeStopFrames(buffer.num_frames()) )
            buffer.clear();
        else
            m_input->process(buffer);
    }

    // 图生命周期由 AudioManager 管理，引用目标比该 Gate 更长寿。
private:
    /// @brief 低频构造时固定的上游混音图所有者。
    std::shared_ptr<ice::IAudioNode> m_input;
    /// @brief 稳定时间线观察引用，提供同一输入块的暂停决策。
    AudioTimelineMixerNode& m_timeline;
};
}  // namespace MMM::Audio
