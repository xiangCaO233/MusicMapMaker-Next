#pragma once

#include "mmm/timing/BpmNormalization.h"
#include <cmath>

namespace MMM::Canvas
{
/// @brief 判断 STOP 草稿是否能作为停顿时长提交。
/// @param seconds 内部保存的秒数，可在输入期间暂时为零或负数。
/// @return 只有有限正时长才允许创建或更新事件。
/// @warning UI 热路径：只做标量判断，不钳制或重写正在输入的草稿。
inline bool isValidStopDuration(double seconds)
{
    // 零作为清空后重输的中间态保留，不能每帧强制改成最小正数。
    return std::isfinite(seconds) && seconds > 0.0;
}

/// @brief 把内部秒数换为 STOP 编辑器的显示值。
/// @param seconds 停顿时长，不包含后续物件的音乐时间。
/// @param bpm STOP 所在时刻的有效 BPM，同刻红线先于 STOP 生效。
/// @param bmsUnits true 使用 iBMSC 的 1/48 拍，否则直接使用秒。
/// @warning UI 热路径：只执行常量时间换算，不读取谱面或查询音频设备。
inline double stopDurationDisplayValue(double seconds, double bpm,
                                       bool bmsUnits)
{
    // iBMSC 的 #STOP 保存原始数值；192 单位是一标准四拍小节。
    // 内部仍按秒保存，切换显示模式不得改变实际播放等待时间。
    return bmsUnits ? seconds * normalizeBpmValue(bpm) * (48.0 / 60.0)
                    : seconds;
}

/// @brief 将用户输入换回统一的 STOP 秒数。
/// @param value 秒数或 BMS 单位值，草稿允许暂时不满足提交条件。
/// @param bpm STOP 所在时刻的有效 BPM。
/// @param bmsUnits 输入是否采用 iBMSC 的 STOP 单位。
/// @warning UI 热路径：不改变音频路由、源游标或播放倍速。
inline double stopDurationSeconds(double value, double bpm, bool bmsUnits)
{
    // BPM 只用于单位换算；播放层始终消费固定秒数，无需第二套等待调度。
    // 除以每秒单位数，避免先乘六十造成不必要的中间溢出。
    return bmsUnits ? value / (normalizeBpmValue(bpm) * (48.0 / 60.0)) : value;
}
}  // namespace MMM::Canvas
