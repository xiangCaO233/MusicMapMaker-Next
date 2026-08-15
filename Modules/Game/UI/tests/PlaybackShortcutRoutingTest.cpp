#include "ui/imgui/menu/actions/edit/PlaybackShortcutRouting.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace
{
using MMM::UI::consumePlaybackShortcutNavigationActivation;
using MMM::UI::shouldAllowPlaybackToggleWhileItemActive;

/// @brief 验证播放快捷键会清除同一空格产生的控件导航激活。
/// @return 导航激活 ID 被清空且空上下文可安全处理时返回 true。
bool testPlaybackShortcutConsumesNavigationActivation()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    GImGui->NavActivateId        = 42;
    GImGui->NavActivateDownId    = 42;
    GImGui->NavActivatePressedId = 42;
    GImGui->NavActivateFlags     = ImGuiActivateFlags_PreferInput;
    consumePlaybackShortcutNavigationActivation(GImGui);
    const bool cleared = GImGui->NavActivateId == 0 &&
                         GImGui->NavActivateDownId == 0 &&
                         GImGui->NavActivatePressedId == 0 &&
                         GImGui->NavActivateFlags == ImGuiActivateFlags_None;
    consumePlaybackShortcutNavigationActivation(nullptr);
    ImGui::DestroyContext();
    return cleared;
}
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
    ok &= testPlaybackShortcutConsumesNavigationActivation();
    return ok ? 0 : 1;
}
