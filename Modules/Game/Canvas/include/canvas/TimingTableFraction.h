#pragma once

#include "mmm/beatmap/BeatFraction.h"

namespace MMM::Canvas
{

/// @brief 表格拍位换算使用的误差阈值。
inline constexpr double TIMING_TABLE_BEAT_EPSILON =
    MALODY_BEAT_BOUNDARY_EPSILON;

/// @brief 时间点表格中展示和保存的分拍拟合结果。
struct TimingTableFractionFit {
    /// @brief 拟合后的拍号。
    std::int32_t beatIndex{ 0 };
    /// @brief 分拍分子。
    std::int32_t numerator{ 0 };
    /// @brief 分拍分母。
    std::int32_t denominator{ 1 };
    /// @brief 分拍小数值。
    double fraction{ 0.0 };
    /// @brief 拟合到该分数后的时间误差，单位毫秒。
    double errorMs{ 0.0 };
};

/// @brief 按 MC 导出规则的固定分拍候选拟合时间点表格拍位。
/// @param beat 连续拍位置。
/// @return 规整后的拍号、分子、分母和分拍值。
/// @warning UI 热路径：每个可见 Timing 行每帧调用；只遍历固定候选表，
/// 禁止引入分配、文件系统访问或阻塞操作。
[[nodiscard]] inline TimingTableFractionFit fitTimingTableFraction(double beat)
{
    const auto fit = fitMalodyBeatFraction(beat);
    return { fit.beatIndex, fit.numerator, fit.denominator, fit.fraction, 0.0 };
}

}  // namespace MMM::Canvas
