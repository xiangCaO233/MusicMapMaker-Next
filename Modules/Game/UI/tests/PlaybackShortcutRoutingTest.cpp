#include "ui/imgui/menu/actions/edit/PlaybackShortcutRouting.h"

#include "imgui.h"
#include "imgui_internal.h"

/// @file PlaybackShortcutRoutingTest.cpp
/// @brief 编辑器播放快捷键与 ImGui 键盘导航激活之间的路由回归测试。
/// @details 测试建立最小 ImGui 上下文，验证空格触发播放后会消费同键产生的
/// 控件激活，同时覆盖不同活动控件组合的允许和拒绝策略。

namespace
{
using MMM::UI::consumePlaybackShortcutNavigationActivation;
using MMM::UI::shouldAllowPlaybackToggleWhileItemActive;

/// @brief 验证播放快捷键会清除同一空格产生的控件导航激活。
/// @return 导航激活 ID 被清空且空上下文可安全处理时返回 true。
bool testPlaybackShortcutConsumesNavigationActivation()
{
    // 内部导航字段只有在有效 ImGui 上下文中才能安全访问。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // 四个字段模拟同一次空格键生成的完整导航激活状态。
    GImGui->NavActivateId        = 42;
    GImGui->NavActivateDownId    = 42;
    GImGui->NavActivatePressedId = 42;
    GImGui->NavActivateFlags     = ImGuiActivateFlags_PreferInput;
    // 播放路由取得空格所有权后必须一次清空全部激活字段。
    consumePlaybackShortcutNavigationActivation(GImGui);
    const bool cleared = GImGui->NavActivateId == 0 &&
                         GImGui->NavActivateDownId == 0 &&
                         GImGui->NavActivatePressedId == 0 &&
                         GImGui->NavActivateFlags == ImGuiActivateFlags_None;
    // 空上下文用于验证调用方在无 ImGui 环境下可以安全跳过。
    consumePlaybackShortcutNavigationActivation(nullptr);
    // 结果在销毁上下文前完成计算，避免访问失效的 GImGui。
    ImGui::DestroyContext();
    return cleared;
}
}  // namespace

/// @brief 覆盖活动控件期间允许播放切换的编辑交互组合。
/// @return 所有路由断言通过时返回 0。
/// @details 第一组覆盖允许路由，第二组覆盖普通活动控件、冲突输入及文本编辑
/// 对播放快捷键的拦截。
int main()
{
    // 无活动控件时，画布和工具窗口的播放入口允许响应。
    // ok 不短路，确保每个纯策略组合在单次运行中都被求值。
    bool ok = true;
    // 特殊的可共存交互组合仍允许编辑器处理播放切换。
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
    // 普通活动控件没有明确路由时必须保留空格给 ImGui。
    ok &= !shouldAllowPlaybackToggleWhileItemActive(
        false, false, false, false, false, false);
    // 文本输入及与工具冲突的活动控件不得触发播放。
    ok &= !shouldAllowPlaybackToggleWhileItemActive(
        true, false, false, false, true, false);
    ok &= !shouldAllowPlaybackToggleWhileItemActive(
        true, false, false, false, false, true);
    ok &= !shouldAllowPlaybackToggleWhileItemActive(
        true, true, false, false, false, false);
    // 最后验证获准播放时同步消费 ImGui 导航状态。
    ok &= testPlaybackShortcutConsumesNavigationActivation();
    return ok ? 0 : 1;
}
