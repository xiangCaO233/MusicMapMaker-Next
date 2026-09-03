#pragma once

#include <algorithm>
#include <cmath>

namespace MMM::Canvas
{

/// @brief Timeline 独立表格窗口的屏幕矩形。
struct TimelineTableWindowRect {
    /// @brief 左上角横坐标。
    float x{ 0.0F };
    /// @brief 左上角纵坐标。
    float y{ 0.0F };
    /// @brief 窗口宽度。
    float width{ 0.0F };
    /// @brief 窗口高度。
    float height{ 0.0F };
};

/// @brief 菜单再次激活独立表格窗口后的状态变化。
struct TimelineTableWindowActivation {
    /// @brief 激活后窗口是否保持打开。
    bool open{ false };
    /// @brief 是否请求将窗口聚焦到前台。
    bool requestFocus{ false };
    /// @brief 是否请求检查并恢复窗口位置。
    bool requestRecovery{ false };
};

/// @brief 解析 Timeline 独立表格菜单项的再次激活行为。
/// @param open 窗口当前是否打开。
/// @param focusedAndReachable 窗口是否同时聚焦且标题栏可从工作区访问。
/// @return 已聚焦且可访问时关闭，否则保持打开并请求恢复及聚焦。
/// @warning UI 菜单热路径：只执行常量布尔判断。
constexpr TimelineTableWindowActivation resolveTimelineTableWindowActivation(
    bool open, bool focusedAndReachable)
{
    if ( open && focusedAndReachable ) {
        return {};
    }
    return { true, true, true };
}

/// @brief 更新表格窗口供菜单切换使用的聚焦可访问状态。
/// @param previous 上一帧记录的状态。
/// @param reachable 当前标题栏是否可从显示器工作区访问。
/// @param focused 当前窗口是否聚焦。
/// @param popupOpen 菜单等弹窗是否正在临时接管焦点。
/// @return 弹窗接管焦点时保留原状态，其余情况使用当前窗口状态。
/// @warning UI 热路径：只执行常量布尔判断。
constexpr bool resolveTimelineTableWindowFocusedAndReachable(bool previous,
                                                             bool reachable,
                                                             bool focused,
                                                             bool popupOpen)
{
    if ( !reachable ) return false;
    if ( focused ) return true;
    return popupOpen && previous;
}

/// @brief 判断窗口标题栏是否仍能从指定显示器工作区访问。
/// @param window 待检查的窗口矩形。
/// @param workArea 显示器工作区矩形。
/// @param titleBarHeight 标题栏高度。
/// @param minimumVisibleWidth 标题栏至少需要露出的宽度。
/// @return 标题栏具有足够可点击区域时返回 true。
/// @warning UI 低频恢复路径：只在表格打开请求后执行常量浮点计算。
inline bool isTimelineTableWindowReachable(
    const TimelineTableWindowRect& window,
    const TimelineTableWindowRect& workArea, float titleBarHeight,
    float minimumVisibleWidth)
{
    const bool validWindow =
        std::isfinite(window.x) && std::isfinite(window.y) &&
        std::isfinite(window.width) && std::isfinite(window.height) &&
        window.width > 0.0F && window.height > 0.0F;
    const bool validWorkArea =
        std::isfinite(workArea.x) && std::isfinite(workArea.y) &&
        std::isfinite(workArea.width) && std::isfinite(workArea.height) &&
        workArea.width > 0.0F && workArea.height > 0.0F;
    if ( !validWindow || !validWorkArea ) return false;

    const float effectiveTitleBarHeight =
        std::clamp(titleBarHeight, 1.0F, window.height);
    const float requiredWidth =
        std::clamp(minimumVisibleWidth, 1.0F, window.width);
    const float requiredHeight = std::min(
        effectiveTitleBarHeight, std::max(1.0F, titleBarHeight * 0.5F));

    const float overlapWidth = std::max(
        0.0F,
        std::min(window.x + window.width, workArea.x + workArea.width) -
            std::max(window.x, workArea.x));
    const float overlapHeight =
        std::max(0.0F,
                 std::min(window.y + effectiveTitleBarHeight,
                          workArea.y + workArea.height) -
                     std::max(window.y, workArea.y));
    return overlapWidth >= requiredWidth && overlapHeight >= requiredHeight;
}

/// @brief 将不可访问的表格窗口缩放并居中到目标工作区。
/// @param window 当前窗口矩形。
/// @param workArea 目标显示器工作区矩形。
/// @param margin 窗口与工作区边缘之间的安全留白。
/// @return 可在目标工作区访问的窗口矩形。
/// @warning UI 低频恢复路径：只在表格窗口落到屏幕外时执行常量浮点计算。
inline TimelineTableWindowRect recoverTimelineTableWindowRect(
    const TimelineTableWindowRect& window,
    const TimelineTableWindowRect& workArea, float margin)
{
    const float safeMargin = std::max(0.0F, margin);
    const float availableWidth =
        std::max(1.0F, workArea.width - safeMargin * 2.0F);
    const float availableHeight =
        std::max(1.0F, workArea.height - safeMargin * 2.0F);
    const float sourceWidth = std::isfinite(window.width) && window.width > 0.0F
                                  ? window.width
                                  : availableWidth;
    const float sourceHeight =
        std::isfinite(window.height) && window.height > 0.0F ? window.height
                                                             : availableHeight;
    const float recoveredWidth  = std::min(sourceWidth, availableWidth);
    const float recoveredHeight = std::min(sourceHeight, availableHeight);

    return {
        workArea.x + (workArea.width - recoveredWidth) * 0.5F,
        workArea.y + (workArea.height - recoveredHeight) * 0.5F,
        recoveredWidth,
        recoveredHeight,
    };
}

}  // namespace MMM::Canvas
