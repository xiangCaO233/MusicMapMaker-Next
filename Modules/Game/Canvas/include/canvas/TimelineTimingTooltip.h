#pragma once

#include "mmm/timing/Timing.h"

#include <string_view>

namespace MMM::Canvas
{

/// @brief Timeline Timing 悬浮提示使用的类型名与数值单位。
struct TimelineTimingTooltipDescriptor {
    /// @brief 面向编辑用户显示的 Timing 类型名。
    std::string_view label;

    /// @brief 追加到原始参数后的单位文本。
    std::string_view valueSuffix;
};

/// @brief 获取指定 Timing 类型的悬浮提示描述。
/// @param effect Timing 类型。
/// @return 对用户可读的类型名与数值单位。
/// @warning UI 热路径：Timing marker 悬浮时每帧调用；只返回静态字符串视图。
[[nodiscard]] inline constexpr TimelineTimingTooltipDescriptor
timelineTimingTooltipDescriptor(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return { "BPM", "" };
    case ::MMM::TimingEffect::SCROLL: return { "SV", "" };
    case ::MMM::TimingEffect::JUMP: return { "Jump", " ms" };
    case ::MMM::TimingEffect::HS: return { "HS", "" };
    }
    return { "Timing", "" };
}

}  // namespace MMM::Canvas
