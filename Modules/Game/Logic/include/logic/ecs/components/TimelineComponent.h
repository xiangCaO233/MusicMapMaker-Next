#pragma once

#include "mmm/timing/Timing.h"

namespace MMM::Logic
{

/**
 * @brief 时间线组件，代表在特定时间点发生的速度或 BPM 变化事件
 *        大量这样的组件实体注册在 Timeline Registry
 * 中，便于进行高效的范围查询和积分计算。
 */
struct TimelineComponent {
    /// @brief 触发时间戳 (秒)
    double m_timestamp{ 0.0 };

    /// @brief 效果类型 (BPM 或 SCROLL 变速)
    ::MMM::TimingEffect m_effect{ ::MMM::TimingEffect::SCROLL };

    /// @brief 效果参数 (如 BPM 值 或 流速倍率/基础流速)
    double m_value{ 1.0 };
};

}  // namespace MMM::Logic