#include "ui/imgui/menu/actions/tools/BpmPlaybackRouting.h"
#include "ui/imgui/WindowIdUtils.h"

#include <string_view>

namespace
{
using MMM::UI::BpmPlaybackRoute;
using MMM::UI::BpmSpaceShortcutDisposition;
using MMM::UI::isBpmMeasurementToolStableWindowId;
using MMM::UI::resolveBpmPlaybackRoute;
using MMM::UI::resolveBpmSpaceShortcutDisposition;
using MMM::UI::shouldDirectlyControlBpmAudioTransport;
using MMM::UI::shouldDispatchBpmPlaybackToEditor;
using MMM::UI::shouldToggleBpmPlaybackFromSpace;
using MMM::UI::WindowIdUtils::stableWindowId;

/// @brief 检查播放路由是否符合预期。
/// @param selectedKey BPM 工具选中音轨键。
/// @param activeKey 活动谱面主音轨键。
/// @param expected 期望路由。
/// @return 路由符合预期时返回 true。
bool checkRoute(std::string_view selectedKey, std::string_view activeKey,
                BpmPlaybackRoute expected)
{
    return resolveBpmPlaybackRoute(selectedKey, activeKey) == expected;
}
}  // namespace

/// @brief 覆盖同轨同步、异轨隔离、聚焦空格、无活动谱面和无选择场景。
/// @return 所有断言通过时返回 0。
int main()
{
    bool ok = true;
    ok &= checkRoute("/project/audio/main.ogg",
                     "/project/audio/main.ogg",
                     BpmPlaybackRoute::SynchronizedWithEditor);
    ok &= checkRoute("/project/audio/preview.ogg",
                     "/project/audio/main.ogg",
                     BpmPlaybackRoute::Audition);
    ok &= checkRoute(
        "/project/audio/preview.ogg", {}, BpmPlaybackRoute::Audition);
    ok &= checkRoute(
        {}, "/project/audio/main.ogg", BpmPlaybackRoute::Unavailable);

    ok &= shouldDispatchBpmPlaybackToEditor(
        BpmPlaybackRoute::SynchronizedWithEditor);
    static_assert(!shouldDirectlyControlBpmAudioTransport(
        BpmPlaybackRoute::SynchronizedWithEditor));
    static_assert(
        shouldDirectlyControlBpmAudioTransport(BpmPlaybackRoute::Audition));
    ok &= !shouldDispatchBpmPlaybackToEditor(BpmPlaybackRoute::Audition);
    ok &= !shouldDispatchBpmPlaybackToEditor(BpmPlaybackRoute::Unavailable);
    ok &= shouldToggleBpmPlaybackFromSpace(false, false);
    ok &= !shouldToggleBpmPlaybackFromSpace(true, false);
    ok &= !shouldToggleBpmPlaybackFromSpace(false, true);

    constexpr std::string_view DOUBLE_MARKER_WINDOW =
        "BPM测量工具###BpmMeasurementToolWindow###BpmMeasurementTool";
    constexpr std::string_view DOUBLE_MARKER_CHILD =
        "BPM测量工具###BpmMeasurementToolWindow###BpmMeasurementTool/"
        "##BpmMeasureControlsChild_A1B2C3D4";
    ok &= stableWindowId(DOUBLE_MARKER_WINDOW) == "BpmMeasurementTool";
    ok &=
        isBpmMeasurementToolStableWindowId(stableWindowId(DOUBLE_MARKER_CHILD));

    ok &= isBpmMeasurementToolStableWindowId("BpmMeasurementTool");
    ok &= isBpmMeasurementToolStableWindowId(
        "BpmMeasurementTool/##BpmMeasureControlsChild_A1B2C3D4");
    ok &= !isBpmMeasurementToolStableWindowId("Canvas_0");
    ok &= !isBpmMeasurementToolStableWindowId("BpmMeasurementToolUnexpected");

    ok &= resolveBpmSpaceShortcutDisposition(true, false, false, true) ==
          BpmSpaceShortcutDisposition::ToggleTool;
    ok &= resolveBpmSpaceShortcutDisposition(true, true, false, true) ==
          BpmSpaceShortcutDisposition::ConsumeOnly;
    ok &= resolveBpmSpaceShortcutDisposition(true, false, true, true) ==
          BpmSpaceShortcutDisposition::ConsumeOnly;
    ok &= resolveBpmSpaceShortcutDisposition(true, false, false, false) ==
          BpmSpaceShortcutDisposition::ConsumeOnly;
    ok &= resolveBpmSpaceShortcutDisposition(false, false, false, true) ==
          BpmSpaceShortcutDisposition::NotOwned;

    const BpmPlaybackRoute focusedDifferentTrackRoute = resolveBpmPlaybackRoute(
        "/project/audio/preview.ogg", "/project/audio/main.ogg");
    ok &= focusedDifferentTrackRoute == BpmPlaybackRoute::Audition;
    ok &= resolveBpmSpaceShortcutDisposition(true, false, false, true) ==
          BpmSpaceShortcutDisposition::ToggleTool;
    ok &= !shouldDispatchBpmPlaybackToEditor(focusedDifferentTrackRoute);
    return ok ? 0 : 1;
}
