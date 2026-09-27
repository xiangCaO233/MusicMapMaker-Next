#pragma once

namespace MMM::Canvas
{

/// @brief Timeline 时间点表格的轻量可见状态。
/// @warning UI 热路径状态：只允许 UI 线程读写，不得跨线程持有其字段引用。
struct TimelineAuxiliaryWindowState {
    /// @brief 时间点批量编辑窗口是否打开。
    bool timingPointsTableOpen{ false };
};

/// @brief 判断 Timeline 画布快照是否仍有自身消费者。
/// @param timelineVisible Timeline 主窗口是否可见。
/// @param state 时间点表格可见状态。
/// @return Timeline 主窗口或时间点表格可见时返回 true。
/// @warning UI 热路径：每帧准备阶段调用，只执行常量布尔判断。
constexpr bool shouldPrepareTimelineSnapshot(
    bool timelineVisible, const TimelineAuxiliaryWindowState& state)
{
    return timelineVisible || state.timingPointsTableOpen;
}

}  // namespace MMM::Canvas
