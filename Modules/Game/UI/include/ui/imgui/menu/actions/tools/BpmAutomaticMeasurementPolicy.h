#pragma once

namespace MMM::UI
{

/// @brief 判断是否应启动新的 BPM 后台自动测量请求。
/// @param backgroundMeasurementActive 当前是否已有后台自动测量请求。
/// @return 没有后台自动测量任务时返回 true，防止重复点击改变窗口可见状态。
constexpr bool shouldStartBpmAutomaticMeasurement(
    bool backgroundMeasurementActive)
{
    return !backgroundMeasurementActive;
}

}  // namespace MMM::UI
