#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace MMM
{

/// @brief Malody beat 三元组支持的最高分拍网格分母。
inline constexpr std::int32_t MALODY_MAX_BEAT_DENOMINATOR = 1920;

/// @brief Malody 导出优先尝试的固定分拍分母，与 MC 格式写出规则一致。
inline constexpr std::array<std::int32_t, 20> MALODY_BEAT_DENOMINATORS{
    1,  2,  3,  4,   6,   8,   12,  16,  24,  32,
    48, 64, 96, 192, 288, 384, 480, 768, 960, 1920
};

/// @brief 固定分拍候选判定使用的拟合容差。
inline constexpr double MALODY_BEAT_FIT_TOLERANCE = 1e-4;

/// @brief 连续拍位规整到整数拍时使用的误差阈值。
inline constexpr double MALODY_BEAT_BOUNDARY_EPSILON = 1e-6;

/// @brief Malody 连续拍位拟合后的最简分拍表示。
struct MalodyBeatFraction {
    /// @brief 整数拍号。
    std::int32_t beatIndex{ 0 };
    /// @brief 分拍分子。
    std::int32_t numerator{ 0 };
    /// @brief 分拍分母。
    std::int32_t denominator{ 1 };
    /// @brief 分拍小数值。
    double fraction{ 0.0 };
};

/// @brief 规整 Malody 分拍分子并输出最简分数。
/// @param beatIndex 整数拍号。
/// @param numerator 分拍分子。
/// @param denominator 分拍分母。
/// @return 已处理跨拍和约分的 Malody 分拍。
[[nodiscard]] inline MalodyBeatFraction normalizeMalodyBeatFraction(
    std::int32_t beatIndex, std::int32_t numerator, std::int32_t denominator)
{
    if ( denominator <= 0 ) return {};
    if ( numerator <= -denominator || numerator >= denominator ) {
        beatIndex += numerator / denominator;
        numerator %= denominator;
    }
    if ( numerator < 0 ) {
        --beatIndex;
        numerator += denominator;
    }
    if ( numerator == 0 ) return { beatIndex, 0, 1, 0.0 };

    const auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    return { beatIndex,
             numerator,
             denominator,
             static_cast<double>(numerator) /
                 static_cast<double>(denominator) };
}

/// @brief 按 MC 固定分拍候选拟合连续拍位，兜底使用 1/1920 网格。
/// @param beat 连续拍位置。
/// @return 规整后的拍号、分子、分母和分拍值。
/// @warning 有界纯数值计算；只遍历固定候选表，禁止引入分配、文件系统访问
/// 或阻塞操作。
[[nodiscard]] inline MalodyBeatFraction fitMalodyBeatFraction(double beat)
{
    if ( !std::isfinite(beat) ) return {};

    auto   beatIndex = static_cast<std::int32_t>(std::floor(beat));
    double fraction  = beat - static_cast<double>(beatIndex);
    if ( fraction < 0.0 ) {
        fraction += 1.0;
        --beatIndex;
    }
    if ( fraction < MALODY_BEAT_BOUNDARY_EPSILON ) {
        return { beatIndex, 0, 1, 0.0 };
    }
    if ( fraction > 1.0 - MALODY_BEAT_BOUNDARY_EPSILON ) {
        return { beatIndex + 1, 0, 1, 0.0 };
    }

    for ( const auto denominator : MALODY_BEAT_DENOMINATORS ) {
        const double scaled  = fraction * static_cast<double>(denominator);
        const auto numerator = static_cast<std::int32_t>(std::llround(scaled));
        if ( std::abs(scaled - static_cast<double>(numerator)) <
             MALODY_BEAT_FIT_TOLERANCE ) {
            return normalizeMalodyBeatFraction(
                beatIndex, numerator, denominator);
        }
    }

    const auto numerator = static_cast<std::int32_t>(std::llround(
        fraction * static_cast<double>(MALODY_MAX_BEAT_DENOMINATOR)));
    return normalizeMalodyBeatFraction(
        beatIndex, numerator, MALODY_MAX_BEAT_DENOMINATOR);
}

}  // namespace MMM
