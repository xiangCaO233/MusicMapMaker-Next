#include "ui/imgui/menu/actions/tools/BpmPlaybackRouting.h"

#include <string_view>

namespace
{
using MMM::UI::BpmPlaybackRoute;
using MMM::UI::isBpmMeasurementToolStableWindowId;
using MMM::UI::resolveBpmPlaybackRoute;
using MMM::UI::shouldDispatchBpmPlaybackToEditor;

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

/// @brief 覆盖同轨同步、异轨隔离、无活动谱面和无选择场景。
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
    ok &= !shouldDispatchBpmPlaybackToEditor(BpmPlaybackRoute::Audition);
    ok &= !shouldDispatchBpmPlaybackToEditor(BpmPlaybackRoute::Unavailable);
    ok &= isBpmMeasurementToolStableWindowId("BpmMeasurementTool");
    ok &= isBpmMeasurementToolStableWindowId(
        "BpmMeasurementTool/##BpmMeasureControlsChild_A1B2C3D4");
    ok &= !isBpmMeasurementToolStableWindowId("Canvas_0");
    ok &= !isBpmMeasurementToolStableWindowId("BpmMeasurementToolUnexpected");
    return ok ? 0 : 1;
}
