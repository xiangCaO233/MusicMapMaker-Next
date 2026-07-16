#include "ui/imgui/menu/actions/edit/PlaybackShortcutRouting.h"

namespace
{
using MMM::UI::shouldAllowPlaybackToggleWhileItemActive;
}  // namespace

/// @brief 覆盖活动控件期间允许播放切换的编辑交互组合。
/// @return 所有路由断言通过时返回 0。
int main()
{
    bool ok = true;
    ok &= shouldAllowPlaybackToggleWhileItemActive(
        false, true, false, false, false, false);
    ok &= shouldAllowPlaybackToggleWhileItemActive(
        false, false, true, false, false, false);
    ok &= shouldAllowPlaybackToggleWhileItemActive(
        false, false, false, false, true, false);
    ok &= shouldAllowPlaybackToggleWhileItemActive(
        false, false, false, false, false, true);
    ok &= shouldAllowPlaybackToggleWhileItemActive(
        true, false, false, true, false, false);

    ok &= !shouldAllowPlaybackToggleWhileItemActive(
        false, false, false, false, false, false);
    ok &= !shouldAllowPlaybackToggleWhileItemActive(
        true, false, false, false, true, false);
    ok &= !shouldAllowPlaybackToggleWhileItemActive(
        true, false, false, false, false, true);
    ok &= !shouldAllowPlaybackToggleWhileItemActive(
        true, true, false, false, false, false);
    return ok ? 0 : 1;
}
