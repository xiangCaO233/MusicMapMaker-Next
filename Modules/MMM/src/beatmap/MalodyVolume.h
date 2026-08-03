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
    if ( !std::isfinite(gainPercent) ) return 1.0F;

    const double volume = 1.0 + gainPercent / 100.0;
    if ( volume <= 0.0 ) return 0.0F;
    return static_cast<float>(std::min(
        volume, static_cast<double>(std::numeric_limits<float>::max())));
}

/// @brief 将内部非负音量倍率转换为 Malody `.mc` 的有符号增益百分比。
/// @param volume 内部音量倍率；`1` 表示原音量。
/// @return 可写入 Malody `vol` 字段的整数增益百分比。
inline std::int64_t volumeToMalodyGainPercent(float volume)
{
    const long double safeVolume =
        std::isfinite(volume) ? static_cast<long double>(std::max(0.0F, volume))
                              : 1.0L;
    const long double gainPercent = (safeVolume - 1.0L) * 100.0L;
    const long double minimum =
        static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    const long double maximum =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if ( gainPercent <= minimum ) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if ( gainPercent >= maximum ) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(std::llround(gainPercent));
}

}  // namespace MMM::Internal
