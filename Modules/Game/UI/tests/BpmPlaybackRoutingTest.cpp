#include "ui/imgui/menu/actions/tools/BpmPlaybackRouting.h"
#include "ui/imgui/WindowIdUtils.h"
#include "ui/imgui/menu/actions/tools/BpmAutomaticMeasurementPolicy.h"

#include <string_view>

/// @file BpmPlaybackRoutingTest.cpp
/// @brief BPM 测量工具的音轨播放、空格所有权和稳定窗口 ID 路由测试。
/// @details 场景覆盖与编辑器同轨同步、异轨试听、工具输入拦截、双重 `###`
/// 标题解析以及自动测量启动门槛，所有路径均为纯策略调用。

namespace
{
using MMM::UI::BpmPlaybackRoute;
using MMM::UI::BpmSpaceShortcutDisposition;
using MMM::UI::isBpmMeasurementToolStableWindowId;
using MMM::UI::resolveBpmPlaybackRoute;
using MMM::UI::resolveBpmSpaceShortcutDisposition;
using MMM::UI::shouldDirectlyControlBpmAudioTransport;
using MMM::UI::shouldDispatchBpmPlaybackToEditor;
using MMM::UI::shouldStartBpmAutomaticMeasurement;
using MMM::UI::shouldToggleBpmPlaybackFromSpace;
using MMM::UI::WindowIdUtils::stableWindowId;

/// @brief 检查播放路由是否符合预期。
/// @param selectedKey BPM 工具选中音轨键。
/// @param activeKey 活动谱面主音轨键。
/// @param expected 期望路由。
/// @return 路由符合预期时返回 true。
/// @note 相同非空键映射同步路由，不同键映射试听，空选择映射不可用。
bool checkRoute(std::string_view selectedKey, std::string_view activeKey,
                BpmPlaybackRoute expected)
{
    // 键值只表示逻辑音轨身份，不要求对应文件实际存在。
    return resolveBpmPlaybackRoute(selectedKey, activeKey) == expected;
}
}  // namespace

/// @brief 覆盖同轨同步、异轨隔离、聚焦空格、无活动谱面和无选择场景。
/// @return 所有断言通过时返回 0。
/// @details 运行时布尔断言与 constexpr 策略断言共同验证公开辅助函数契约。
int main()
{
    // ok 累积所有运行时条件，使后续策略在前项失败后仍得到执行。
    bool ok = true;
    // 工具与活动谱面选择同一音轨时复用编辑器 transport。
    ok &= checkRoute("/project/audio/main.ogg",
                     "/project/audio/main.ogg",
                     BpmPlaybackRoute::SynchronizedWithEditor);
    // 选择另一音轨时进入独立试听，不能改变编辑器播放状态。
    ok &= checkRoute("/project/audio/preview.ogg",
                     "/project/audio/main.ogg",
                     BpmPlaybackRoute::Audition);
    // 没有活动谱面仍可试听明确选择的音轨。
    ok &= checkRoute(
        "/project/audio/preview.ogg", {}, BpmPlaybackRoute::Audition);
    // 没有任何选中音轨时播放入口不可用。
    ok &= checkRoute(
        {}, "/project/audio/main.ogg", BpmPlaybackRoute::Unavailable);
    // 同步路由派发到编辑器，且不能由工具直接控制底层 transport。
    ok &= shouldDispatchBpmPlaybackToEditor(
        BpmPlaybackRoute::SynchronizedWithEditor);
    static_assert(!shouldDirectlyControlBpmAudioTransport(
        BpmPlaybackRoute::SynchronizedWithEditor));
    // 试听路由恰好相反，由 BPM 工具直接控制音频。
    static_assert(
        shouldDirectlyControlBpmAudioTransport(BpmPlaybackRoute::Audition));
    ok &= !shouldDispatchBpmPlaybackToEditor(BpmPlaybackRoute::Audition);
    // 不可用路由不会误派发给编辑器。
    ok &= !shouldDispatchBpmPlaybackToEditor(BpmPlaybackRoute::Unavailable);
    // 普通聚焦状态允许空格，文本输入或弹窗状态会拦截。
    ok &= shouldToggleBpmPlaybackFromSpace(false, false);
    ok &= !shouldToggleBpmPlaybackFromSpace(true, false);
    ok &= !shouldToggleBpmPlaybackFromSpace(false, true);
    // 自动测量只允许在没有正在运行的任务时启动。
    ok &= shouldStartBpmAutomaticMeasurement(false);
    ok &= !shouldStartBpmAutomaticMeasurement(true);

    // 真实窗口标题可能经过多层追加，解析必须使用最后一个 ### 标记。
    constexpr std::string_view DOUBLE_MARKER_WINDOW =
        "BPM测量工具###BpmMeasurementToolWindow###BpmMeasurementTool";
    // 子窗口稳定 ID 保留父窗口前缀，仍应归属于 BPM 工具。
    constexpr std::string_view DOUBLE_MARKER_CHILD =
        "BPM测量工具###BpmMeasurementToolWindow###BpmMeasurementTool/"
        "##BpmMeasureControlsChild_A1B2C3D4";
    // 先单独验证通用窗口 ID 解析，再交给 BPM 所有权判定。
    ok &= stableWindowId(DOUBLE_MARKER_WINDOW) == "BpmMeasurementTool";
    ok &=
        isBpmMeasurementToolStableWindowId(stableWindowId(DOUBLE_MARKER_CHILD));
    // 精确父 ID 和其子窗口有效，相似前缀不能误判为工具窗口。
    ok &= isBpmMeasurementToolStableWindowId("BpmMeasurementTool");
    ok &= isBpmMeasurementToolStableWindowId(
        "BpmMeasurementTool/##BpmMeasureControlsChild_A1B2C3D4");
    ok &= !isBpmMeasurementToolStableWindowId("Canvas_0");
    ok &= !isBpmMeasurementToolStableWindowId("BpmMeasurementToolUnexpected");
    // 工具聚焦且无输入冲突时，空格由工具切换自身播放。
    ok &= resolveBpmSpaceShortcutDisposition(true, false, false, true) ==
          BpmSpaceShortcutDisposition::ToggleTool;
    // 文本输入、弹窗或不可播放状态仍消费按键，但不切换 transport。
    ok &= resolveBpmSpaceShortcutDisposition(true, true, false, true) ==
          BpmSpaceShortcutDisposition::ConsumeOnly;
    ok &= resolveBpmSpaceShortcutDisposition(true, false, true, true) ==
          BpmSpaceShortcutDisposition::ConsumeOnly;
    ok &= resolveBpmSpaceShortcutDisposition(true, false, false, false) ==
          BpmSpaceShortcutDisposition::ConsumeOnly;
    // 工具未聚焦时完全放弃空格所有权，由上层编辑器继续路由。
    ok &= resolveBpmSpaceShortcutDisposition(false, false, false, true) ==
          BpmSpaceShortcutDisposition::NotOwned;
    // 异轨试听即使由空格启动，也绝不能派发到活动谱面的编辑器。
    const BpmPlaybackRoute focusedDifferentTrackRoute = resolveBpmPlaybackRoute(
        "/project/audio/preview.ogg", "/project/audio/main.ogg");
    // 组合断言保护焦点策略与音轨策略之间的边界。
    ok &= focusedDifferentTrackRoute == BpmPlaybackRoute::Audition;
    ok &= resolveBpmSpaceShortcutDisposition(true, false, false, true) ==
          BpmSpaceShortcutDisposition::ToggleTool;
    ok &= !shouldDispatchBpmPlaybackToEditor(focusedDifferentTrackRoute);
    // 单一退出码交给 CTest；失败项可通过调试器逐段定位。
    return ok ? 0 : 1;
}
