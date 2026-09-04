#pragma once

#include "canvas/AuxiliaryWindowState.h"

namespace MMM::Canvas
{

/// @brief Timeline 时间点表格窗口矩形的兼容别名。
using TimelineTableWindowRect = AuxiliaryWindowRect;

/// @brief Timeline 时间点表格激活结果的兼容别名。
using TimelineTableWindowActivation = AuxiliaryWindowActivation;

/// @brief 解析 Timeline 时间点表格菜单项的再次激活行为。
/// @param open 窗口当前是否打开。
/// @param focusedAndReachable 窗口是否同时聚焦且标题栏可访问。
/// @return 通用辅助窗口激活结果。
/// @warning UI 菜单热路径：只转发常量布尔判断。
constexpr TimelineTableWindowActivation resolveTimelineTableWindowActivation(
    bool open, bool focusedAndReachable)
{
    return resolveAuxiliaryWindowActivation(open, focusedAndReachable);
}

/// @brief 更新 Timeline 时间点表格的聚焦可访问状态。
/// @param previous 上一帧记录的状态。
/// @param reachable 当前标题栏是否可访问。
/// @param focused 当前窗口是否聚焦。
/// @param popupOpen 菜单等弹窗是否正在临时接管焦点。
/// @return 通用辅助窗口聚焦可访问状态。
/// @warning UI 热路径：只转发常量布尔判断。
constexpr bool resolveTimelineTableWindowFocusedAndReachable(bool previous,
                                                             bool reachable,
                                                             bool focused,
                                                             bool popupOpen)
{
    return resolveAuxiliaryWindowFocusedAndReachable(
        previous, reachable, focused, popupOpen);
}

/// @brief 判断 Timeline 时间点表格标题栏是否仍可访问。
/// @param window 待检查的窗口矩形。
/// @param workArea 显示器工作区矩形。
/// @param titleBarHeight 标题栏高度。
/// @param minimumVisibleWidth 标题栏至少需要露出的宽度。
/// @return 标题栏具有足够可点击区域时返回 true。
/// @warning UI 低频恢复路径：只转发常量浮点计算。
inline bool isTimelineTableWindowReachable(
    const TimelineTableWindowRect& window,
    const TimelineTableWindowRect& workArea, float titleBarHeight,
    float minimumVisibleWidth)
{
    return isAuxiliaryWindowReachable(
        window, workArea, titleBarHeight, minimumVisibleWidth);
}

/// @brief 将不可访问的 Timeline 时间点表格恢复到目标工作区。
/// @param window 当前窗口矩形。
/// @param workArea 目标显示器工作区矩形。
/// @param margin 窗口与工作区边缘之间的安全留白。
/// @return 可在目标工作区访问的窗口矩形。
/// @warning UI 低频恢复路径：只转发常量浮点计算。
inline TimelineTableWindowRect recoverTimelineTableWindowRect(
    const TimelineTableWindowRect& window,
    const TimelineTableWindowRect& workArea, float margin)
{
    return recoverAuxiliaryWindowRect(window, workArea, margin);
}

}  // namespace MMM::Canvas
