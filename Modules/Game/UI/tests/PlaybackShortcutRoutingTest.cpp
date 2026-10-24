#include "ui/imgui/menu/actions/edit/PlaybackShortcutRouting.h"
#include "config/EditorSettings.h"
#include "ui/imgui/ShortcutUtils.h"

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
/// @brief 验证独立实现仍返回原设置成员，并保持录制共享状态和格式化回退。
/// @return 引用身份、文字和跨函数状态全部一致时为 true。
/// @details 不注入真实键盘事件；已有导航回归继续验证 ImGui 路由上下文。
bool testShortcutUtilityBoundary()
{
    using namespace MMM::UI::ShortcutUtils;
    using MMM::Logic::EditTool;
    MMM::Config::EditorSettings settings;
    // 只读入口使用原对象的 const 视图，避免值拷贝掩盖引用身份错误。
    const auto& readOnly = settings;
    // 成员指针表固定五种有独立绑定的工具，避免只验证默认 Move 而遗漏字段映射。
    const std::array tools{ EditTool::Move,
                            EditTool::Marquee,
                            EditTool::Draw,
                            EditTool::ColorBrush,
                            EditTool::ColorEraser };
    // 期望地址直接取自配置字段，不以被测函数生成期望结果。
    const std::array bindings{ &settings.shortcutConfig.toolMove,
                               &settings.shortcutConfig.toolMarquee,
                               &settings.shortcutConfig.toolDraw,
                               &settings.shortcutConfig.toolColorBrush,
                               &settings.shortcutConfig.toolColorEraser };
    for ( std::size_t i = 0; i < tools.size(); ++i ) {
        // 可写和只读入口都必须借用同一成员，不能在源文件内返回临时或缓存副本。
        if ( &getToolShortcut(settings, tools[i]) != bindings[i] ||
             &getToolShortcut(readOnly, tools[i]) != bindings[i] )
            return false;
    }
    // Layout 沿用 Move 回退；通过返回引用写入后立即观察原设置。
    getToolShortcut(settings, EditTool::Layout).key = "FutureKey";
    if ( settings.shortcutConfig.toolMove.key != "FutureKey" ||
         &getToolShortcut(readOnly, EditTool::Layout) != bindings.front() )
        return false;
    // 未知未来按键仍显示原文字，禁用绑定不显示；不依赖当前 ImGui 上下文。
    // 展示测试复制绑定，避免覆盖工具映射测试刚写入的设置。
    auto binding = settings.shortcutConfig.mirror;
    // 保留默认镜像快捷键的 Ctrl 修饰，检查未知键仍能保留显示前缀。
    binding.key = "FutureKey";
    if ( formatShortcut(binding) != "Ctrl+FutureKey" ) return false;
    // 禁用优先于名称回退，旧配置中的未知键也不能显示已失效操作。
    binding.enabled = false;
    if ( !formatShortcut(binding).empty() ) return false;
    // 查找返回进程内稳定表元素，不能借用源文件函数的局部自动数组。
    // 稳定名称与界面显示文字 Left 不同，查找必须使用持久化名称。
    const auto* entry = findKeyEntry("LeftArrow");
    if ( !entry || entry != findKeyEntry("LeftArrow") ||
         entry->key != ImGuiKey_LeftArrow || findKeyEntry("FutureKey") )
        return false;
    // 录制标记必须在 setter、getter 与原引用入口之间共享同一个状态。
    setShortcutRecordingActive(true);
    // 先记录 setter 的可见结果，再通过引用入口清除以检验共享存储。
    const bool active              = isShortcutRecordingActive();
    shortcutRecordingActiveState() = false;
    // 无论检测结果如何都先恢复状态，避免影响同一测试进程的后续用例。
    return active && !isShortcutRecordingActive();
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
    // 工具函数移出头文件后，仍需通过实际链接验证引用与状态的单一定义。
    // 该检查不依赖上一组已经销毁的 ImGui 上下文，区分纯配置与输入查询职责。
    ok &= testShortcutUtilityBoundary();
    return ok ? 0 : 1;
}
