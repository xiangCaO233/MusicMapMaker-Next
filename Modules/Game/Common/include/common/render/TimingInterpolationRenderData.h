#pragma once

#include "mmm/timing/Timing.h"
#include <entt/entt.hpp>

namespace MMM::Common::Render
{
/// @brief 一个插值段落的不可变画布描述，不包含导出采样事件。
/// @details 由脏时间线缓存收集，UI 只借用当前快照的数据。
struct TimingInterpolationElement {
    /// @brief 段落实体，用于提交编辑或删除指令。
    entt::entity entity{ entt::null };
    /// @brief 段首时间，单位秒；时长在 interpolation 内。
    double time{ 0.0 };
    /// @brief 段首参数，与普通 Timing 使用相同单位。
    double value{ 1.0 };
    /// @brief 段落所控制的独立效果泳道。
    TimingEffect effect{ TimingEffect::SCROLL };
    /// @brief 曲线和输出密度的值语义副本。
    TimingInterpolation interpolation;
};
}  // namespace MMM::Common::Render
