#include "canvas/CanvasTabTitle.h"

#include <string>

namespace
{

/// @brief 验证长标题的脏标志固定在最前方，不会成为尾部省略内容。
/// @return 标题保留前置脏标志且不追加尾部标志时返回 true。
bool testDirtyLongTitleKeepsLeadingMarker()
{
    // 使用足够长的真实风格名称，模拟停靠标签发生尾部截断的场景。
    const std::string beatmapName = "[mc] Monochrome City [4] 4K Inferno Lv.10";
    const std::string title       = MMM::Canvas::makeCanvasTabTitle(
        "canvas.editor", true, beatmapName, true);
    // 同时验证完整字符串、前缀和不存在旧版尾缀三项契约。
    return title == "* " + beatmapName && title.starts_with("* ") &&
           !title.ends_with(" *");
}

/// @brief 验证未修改谱面不显示脏标志。
/// @return 标题与谱面名称完全一致时返回 true。
bool testCleanTitleHasNoMarker()
{
    // hasBeatmap=true 且 isDirty=false 时不得引入任何装饰字符。
    return MMM::Canvas::makeCanvasTabTitle(
               "canvas.editor", true, "Clean Beatmap", false) ==
           "Clean Beatmap";
}

/// @brief 验证未命名脏谱面仍使用回退标题并保留前置标志。
/// @return 回退标题前存在脏标志时返回 true。
bool testDirtyUnnamedBeatmapKeepsMarker()
{
    // 空谱面名回退到稳定标题，但仍属于有谱面的脏会话。
    return MMM::Canvas::makeCanvasTabTitle("canvas.editor", true, "", true) ==
           "* canvas.editor";
}

/// @brief 验证欢迎占位标签不会继承无效的脏状态。
/// @return 不含谱面时只显示回退标题。
bool testPlaceholderIgnoresDirtyState()
{
    // hasBeatmap=false 的占位页不采信外部残留的 isDirty 标志。
    return MMM::Canvas::makeCanvasTabTitle("canvas.editor", false, "", true) ==
           "canvas.editor";
}

/// @brief 验证协作状态位于谱面名称前，脏标志仍固定在整个标签最前方。
/// @return 在线与离线标签均按预期组合时返回 true。
///
/// 两个状态标签使用不同脏状态，覆盖协作前缀与脏前缀的组合顺序。
bool testCollaborationStatusPrecedesBeatmapName()
{
    // 在线状态不带脏标志，离线状态与脏标志同时存在以验证前缀次序。
    return MMM::Canvas::makeCanvasTabTitle(
               "canvas.editor", true, "Online Map", false, "(在线)") ==
               "(在线) Online Map" &&
           MMM::Canvas::makeCanvasTabTitle(
               "canvas.editor", true, "Offline Map", true, "(离线)") ==
               "* (离线) Offline Map";
}

/// @brief 验证协作画布在欢迎页复用、断线和关闭过程中的标记生命周期。
/// @return 远端谱面正确标记、断线保留且回到欢迎页后清除时返回 true。
///
/// 状态输入模拟房间启动、Session 复用、断线和额外本地谱面四类来源。
bool testCollaborationCanvasStateLifecycle()
{
    // 回到欢迎占位页后必须清除历史协作标记。
    const bool offlinePlaceholder =
        MMM::Canvas::resolveCollaborationCanvasState(
            true, true, false, false, false, true);
    const bool joiningPlaceholder =
        // 房间正在加入但仍是占位页时不能提前显示协作标签。
        MMM::Canvas::resolveCollaborationCanvasState(
            false, true, true, true, false, true);
    const bool joinedThroughReusedPlaceholder =
        // 同一 Session 从欢迎页复用为远端谱面时建立协作标记。
        MMM::Canvas::resolveCollaborationCanvasState(
            false, false, true, true, true, true);
    const bool joinedDirectly = MMM::Canvas::resolveCollaborationCanvasState(
        // 非复用路径在房间生命周期首次激活时也应建立标记。
        false,
        false,
        false,
        true,
        false,
        true);
    const bool disconnectedBeatmap =
        // 房间断开后真实谱面仍保留协作来源，供 UI 展示离线状态。
        MMM::Canvas::resolveCollaborationCanvasState(
            true, false, false, false, true, true);
    const bool inactiveOfflineBeatmap =
        // 非活动画布同样保留已有标记，不依赖当前焦点。
        MMM::Canvas::resolveCollaborationCanvasState(
            true, false, false, false, true, false);
    const bool additionalLocalBeatmap =
        // 房间稳定期间新建的本地谱面不能被误标为协作画布。
        MMM::Canvas::resolveCollaborationCanvasState(
            false, false, false, false, false, true);
    // 七种生命周期状态共同约束建立、保留和清除三个方向。
    return !offlinePlaceholder && !joiningPlaceholder &&
           joinedThroughReusedPlaceholder && joinedDirectly &&
           disconnectedBeatmap && inactiveOfflineBeatmap &&
           !additionalLocalBeatmap;
}

}  // namespace

/// @brief 覆盖主画布标签的标题和脏标志布局规则。
/// @return 所有标签规则满足时返回 0。
///
/// 这里只检查可见标题；固定 ImGui ID 由实际窗口调用方负责。
int main()
{
    // 标题文本与协作状态归约分别测试，再组合为统一退出码。
    return testDirtyLongTitleKeepsLeadingMarker() &&
                   testCleanTitleHasNoMarker() &&
                   testDirtyUnnamedBeatmapKeepsMarker() &&
                   testPlaceholderIgnoresDirtyState() &&
                   testCollaborationStatusPrecedesBeatmapName() &&
                   testCollaborationCanvasStateLifecycle()
               // 任一标题或生命周期规则失败都返回非零。
               ? 0
               : 1;
}
