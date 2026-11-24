#pragma once

#include "mmm/SafeParse.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace MMM::Internal
{

/// @file OsuManiaTracks.h
/// @brief osu!mania 的数字字段与谱面轨号之间的转换契约。
/// @details 导入接受闭区间上的数字；导出选择落在目标轨内的整数。
/// 所有转换均先验证范围，避免将不受信的浮点数直接交给整数转换。
///
/// @brief osu! 横坐标宽度；每个可往返的轨道至少分配一个整数坐标。
inline constexpr std::int32_t OSU_PLAYFIELD_WIDTH = 512;
/// @brief 未设置列数的纯时间轴使用常规 4K，仍能在 osu!mania 中打开。
inline constexpr std::int32_t OSU_DEFAULT_KEYS = 4;

/// @note 使用 SafeParse 的跨平台浮点入口，避免 macOS 旧部署目标缺少浮点
/// from_chars。
/// @brief 读取完整、有限的十进制数，拒绝平台特有的浮点扩展语法。
inline std::optional<double> parseOsuTrackNumber(std::string_view text)
{
    text = trimLeadingAsciiSpaces(text);
    while ( !text.empty() && (text.back() == ' ' || text.back() == '\t') )
        text.remove_suffix(1);
    // Apple 的浮点入口支持十六进制。先限制字符再检查实际消费长度，
    // 使残缺指数、非数、无穷以及十六进制在不同平台上一致失败。
    if ( text.empty() ||
         text.find_first_not_of("0123456789.-+eE") != std::string_view::npos )
        return std::nullopt;
    // 同一字段须被完整消费，防止数值前缀后附加任意字符仍通过范围检查。
    const auto result = parseFloatingPrefix(text);
    if ( result.error != std::errc{} || result.parsedLength != text.size() ||
         !std::isfinite(result.value) )
        return std::nullopt;
    return result.value;
}

/// @note 单键是合法极端，超过横坐标整数分辨率的模式无法给每列保留整数坐标。
/// @brief 校验键数，保留旧导入器对小数 CircleSize 的就近取整。
inline std::optional<std::int32_t> parseOsuManiaKeyCount(std::string_view text)
{
    const auto value = parseOsuTrackNumber(text);
    if ( !value ) return std::nullopt;
    // 先限制浮点的范围，再转为整数以防止无效或溢出的转换。
    const auto keys = std::round(*value);
    if ( keys < 1 || keys > OSU_PLAYFIELD_WIDTH ) return std::nullopt;
    return static_cast<std::int32_t>(keys);
}

/// @param x HitObject 的原始横坐标字段，可为整数或十进制浮点。
/// @param keys 本谱面玩家轨道数；直接调用也要检查范围。
/// @brief 单键和长条共用区间映射，非法输入返回空，由导入器跳过该物件。
inline std::optional<std::uint32_t> osuManiaTrack(std::string_view x,
                                                  std::int32_t     keys)
{
    if ( keys < 1 || keys > OSU_PLAYFIELD_WIDTH ) return std::nullopt;
    const auto coordinate = parseOsuTrackNumber(x);
    if ( !coordinate || *coordinate < 0 || *coordinate > OSU_PLAYFIELD_WIDTH )
        return std::nullopt;
    // 常规列使用左闭右开区间，整个游戏区的最右端也归入末轨。
    const auto track =
        static_cast<std::int32_t>(*coordinate * keys / OSU_PLAYFIELD_WIDTH);
    return static_cast<std::uint32_t>(std::min(track, keys - 1));
}

/// @brief 返回最接近旧写出坐标且实际位于该轨道的整数横坐标。
/// @details 轨道宽不足两个像素时，中点向下取整可能落入前一轨。
/// @param track 从零开始的内部玩家轨道号。
/// @param keys 整首谱面的玩家轨道数，不包含编辑器附加轨道。
/// @return 无法表达轨号或键数时返回空。
inline std::optional<std::int32_t> osuManiaCoordinate(std::uint32_t track,
                                                      std::int32_t  keys)
{
    if ( keys < 1 || keys > OSU_PLAYFIELD_WIDTH ||
         track >= static_cast<std::uint32_t>(keys) )
        return std::nullopt;
    // 键数上界也限制了下面的整型乘法，使密集模式的轨间边界可复算。
    const auto column = static_cast<std::int32_t>(track);
    // 至少一个整数网格点属于该列。用整除求左端上界和旧中点，避免浮点边界舍入。
    const auto first  = (column * OSU_PLAYFIELD_WIDTH + keys - 1) / keys;
    const auto middle = (2 * column + 1) * OSU_PLAYFIELD_WIDTH / (2 * keys);
    return std::max(first, middle);
}

}  // namespace MMM::Internal
