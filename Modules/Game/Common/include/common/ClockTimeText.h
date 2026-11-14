#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace MMM::Common
{
/// @brief 将秒数显示为累计分钟、秒与四位秒小数，不引入小时字段。
/// @param timeSeconds 播放位置或时长，负时间保留整体负号。
/// @return 空字符结尾的 MM:SS.ffff 文本，分钟只补最少两位而不按小时回卷。
/// @details 统一舍入后拆字段，避免 59.99996 秒出现非法的 00:60.0000。
/// 非有限输入显示零；超出整数换算范围的输入只限制显示，不修改调用方数据。
/// @warning 时间组件热路径可调用；只使用栈上固定数组，不分配或访问外部状态。
[[nodiscard]] inline std::array<char, 32> formatClockTime(double timeSeconds)
{
    // 非有限数不能流入整数转换，原播放或编辑数据仍由业务校验负责。
    const double finite = std::isfinite(timeSeconds) ? timeSeconds : 0.0;
    const double scaled = std::abs(finite) * 10000.0;
    // 先检查上界再 llround，避免极大时间或乘法溢出导致未定义整数转换。
    // double 不能精确表示 INT64_MAX，达到该边界时直接采用整数最大值。
    // 分钟只设最小显示宽度，不添加小时字段或按日截断长时长。
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto ticks = scaled >= static_cast<double>(maximum)
                           ? maximum
                           : static_cast<std::int64_t>(std::llround(scaled));
    // 四位秒小数的一个单位为 0.1 ms；所有字段来自同一个已舍入整数。
    const auto minutes  = ticks / 600000;
    const auto seconds  = (ticks / 10000) % 60;
    const auto fraction = ticks % 10000;
    // 舍入为零后不再显示负号，保持正常归零附近的读数稳定。
    const bool           negative = finite < 0.0 && ticks != 0;
    std::array<char, 32> result{};
    // 最大整数时间的累计分钟也能完整放入该缓冲，包括负号和结束符。
    // printf 的宽度是最小宽度，因此分钟超过两位时不会被截断。
    std::snprintf(result.data(),
                  result.size(),
                  "%s%02lld:%02lld.%04lld",
                  negative ? "-" : "",
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds),
                  static_cast<long long>(fraction));
    return result;
}
}  // namespace MMM::Common
