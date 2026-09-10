#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace MMM::Internal
{

/// @brief 将 Malody `.mc` 中以零为中性的有符号增益百分比转换为音量倍率。
/// @param gainPercent Malody `vol` 字段；`0` 表示原音量，负值衰减，正值增益。
/// @return 非负且有限的内部音量倍率。
inline float malodyGainPercentToVolume(double gainPercent)
{
    // 非有限来源值无法表达方向，按 Malody 的中性增益回退。
    if ( !std::isfinite(gainPercent) ) return 1.0F;

    // Malody 百分比零点对应内部一倍音量，负百分比表示衰减。
    const double volume = 1.0 + gainPercent / 100.0;
    // 小于或等于静音的输入统一收敛为零，不传播负音量。
    if ( volume <= 0.0 ) return 0.0F;
    // 极端增益夹取到 float 上限，避免窄化转换产生无穷。
    return static_cast<float>(std::min(
        volume, static_cast<double>(std::numeric_limits<float>::max())));
}

/// @brief 将内部非负音量倍率转换为 Malody `.mc` 的有符号增益百分比。
/// @param volume 内部音量倍率；`1` 表示原音量。
/// @return 可写入 Malody `vol` 字段的整数增益百分比。
inline std::int64_t volumeToMalodyGainPercent(float volume)
{
    // 非有限内部值按中性音量处理，负值先收敛到静音。
    const long double safeVolume =
        std::isfinite(volume) ? static_cast<long double>(std::max(0.0F, volume))
                              : 1.0L;
    // 使用 long double 保留范围检查前的中间精度。
    const long double gainPercent = (safeVolume - 1.0L) * 100.0L;
    const long double minimum =
        static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    const long double maximum =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if ( gainPercent <= minimum ) {
        // 饱和到可序列化整数边界，避免转换超出定义范围。
        return std::numeric_limits<std::int64_t>::min();
    }
    if ( gainPercent >= maximum ) {
        // 正向极端增益同样使用整数上界表达。
        return std::numeric_limits<std::int64_t>::max();
    }
    // 正常范围按最近整数百分比写出，保持常见倍率的直观值。
    return static_cast<std::int64_t>(std::llround(gainPercent));
}

}  // namespace MMM::Internal
