#pragma once

#include "config/FrameLimitPreference.h"

namespace MMM::Config
{

/// @brief 帧率计算缺少有效设备刷新率时使用的回退值。
inline constexpr int FRAME_LIMIT_FALLBACK_REFRESH_RATE = 60;

/// @brief 根据帧率限制偏好计算目标循环频率。
/// @param preference 当前帧率限制偏好。
/// @param deviceRefreshRate 当前设备刷新率；无效值会回退到 60 Hz。
/// @return 固定限制模式对应的目标频率；Unlimited 返回 0。
/// @warning 渲染与逻辑热路径：只执行常量级算术，禁止加入系统查询或分配。
constexpr double frameLimitTargetRate(FrameLimitPreference preference,
                                      int                  deviceRefreshRate)
{
    const double refreshRate = static_cast<double>(
        deviceRefreshRate > 0 ? deviceRefreshRate
                              : FRAME_LIMIT_FALLBACK_REFRESH_RATE);
    switch ( preference ) {
    case FrameLimitPreference::VSync: return refreshRate;
    case FrameLimitPreference::Refresh2x: return refreshRate * 2.0;
    case FrameLimitPreference::Refresh4x: return refreshRate * 4.0;
    case FrameLimitPreference::Refresh8x: return refreshRate * 8.0;
    case FrameLimitPreference::Unlimited:
    default: return 0.0;
    }
}

/// @brief 根据帧率限制偏好计算目标循环间隔。
/// @param preference 当前帧率限制偏好。
/// @param deviceRefreshRate 当前设备刷新率；无效值会回退到 60 Hz。
/// @return 固定限制模式对应的秒间隔；Unlimited 返回 0。
/// @warning 渲染与逻辑热路径：只执行常量级算术，禁止加入系统查询或分配。
constexpr double frameLimitTargetInterval(FrameLimitPreference preference,
                                          int deviceRefreshRate)
{
    const double targetRate =
        frameLimitTargetRate(preference, deviceRefreshRate);
    return targetRate > 0.0 ? 1.0 / targetRate : 0.0;
}

}  // namespace MMM::Config
