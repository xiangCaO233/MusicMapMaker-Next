#pragma once

#include "mmm/timing/BpmNormalization.h"
#include "mmm/timing/Timing.h"
#include <optional>

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

    /// @brief 效果类型 (BPM、SCROLL、JUMP 或 HS)
    ::MMM::TimingEffect m_effect{ ::MMM::TimingEffect::SCROLL };

    /// @brief 效果参数 (如 BPM 值 或 流速倍率/基础流速)
    double m_value{ 1.0 };

    /// @brief 原始元数据备份
    ::MMM::TimingMetadata m_metadata;

    /// @brief 插值段落定义；长度保持秒单位，删除或撤销时作为整体处理。
    std::optional<::MMM::TimingInterpolation> m_interpolation;
};

/// @brief 从 BPM 锚点到指定时间的连续拍数；段尾后继承终值。
/// @warning 拍网格热路径，只有固定次数的曲线计算，没有 ECS 遍历。
inline double timelineBeatsAt(const TimelineComponent& timing, double time,
                              double fallbackBpm = 120.0)
{
    // 组件时间是秒，BPM 积分得到每分钟参数乘秒后除六十。
    // 曲线首点以前延用首值，保留已有负拍定位的行为。
    const double bpm     = normalizeBpmValue(timing.m_value, fallbackBpm);
    const double elapsed = time - timing.m_timestamp;
    return timing.m_interpolation ? integrateTimingInterpolation(
                                        *timing.m_interpolation, bpm, elapsed) /
                                        60.0
                                  : elapsed * bpm / 60.0;
}

/// @brief 反解指定连续拍位的时间；正 BPM 保证解唯一。
/// @warning 拍网格热路径；只在曲线内部进行固定上限的二分求解。
inline double timelineTimeAtBeat(const TimelineComponent& timing, double beat,
                                 double fallbackBpm = 120.0)
{
    const double bpm = normalizeBpmValue(timing.m_value, fallbackBpm);
    if ( !timing.m_interpolation || beat <= 0.0 )
        return timing.m_timestamp + beat * 60.0 / bpm;
    const auto&  curve = *timing.m_interpolation;
    const double whole =
        integrateTimingInterpolation(curve, bpm, curve.m_duration) / 60.0;
    // 曲线外可直接线性反解，长谱面定位不扩大迭代范围。
    if ( beat >= whole )
        return timing.m_timestamp + curve.m_duration +
               (beat - whole) * 60.0 / curve.m_endValue;
    // 正 BPM 使累计拍数严格单调，曲线内部只有一个对应时间。
    // 用完整时长夹住解，固定次数二分不会随谱面长度增加工作量。
    // 即使贝塞尔横轴局部斜率为零，也不依赖易发散的导数除法。
    double low = 0.0, high = curve.m_duration;
    for ( int iteration = 0; iteration < 40; ++iteration ) {
        const double middle = (low + high) * 0.5;
        if ( integrateTimingInterpolation(curve, bpm, middle) < beat * 60.0 )
            low = middle;
        else
            high = middle;
    }
    return timing.m_timestamp + (low + high) * 0.5;
}
}  // namespace MMM::Logic
