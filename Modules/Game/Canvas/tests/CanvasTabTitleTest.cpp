#include "canvas/CanvasTabTitle.h"

#include <string>

namespace
{

/// @brief 验证长标题的脏标志固定在最前方，不会成为尾部省略内容。
/// @return 标题保留前置脏标志且不追加尾部标志时返回 true。
bool testDirtyLongTitleKeepsLeadingMarker()
{
    const std::string beatmapName = "[mc] Monochrome City [4] 4K Inferno Lv.10";
    const std::string title       = MMM::Canvas::makeCanvasTabTitle(
        "canvas.editor", true, beatmapName, true);
    return title == "* " + beatmapName && title.starts_with("* ") &&
           !title.ends_with(" *");
}

/// @brief 验证未修改谱面不显示脏标志。
/// @return 标题与谱面名称完全一致时返回 true。
bool testCleanTitleHasNoMarker()
{
    return MMM::Canvas::makeCanvasTabTitle(
               "canvas.editor", true, "Clean Beatmap", false) ==
           "Clean Beatmap";
}

/// @brief 验证未命名脏谱面仍使用回退标题并保留前置标志。
/// @return 回退标题前存在脏标志时返回 true。
bool testDirtyUnnamedBeatmapKeepsMarker()
{
    return MMM::Canvas::makeCanvasTabTitle("canvas.editor", true, "", true) ==
           "* canvas.editor";
}

/// @brief 验证欢迎占位标签不会继承无效的脏状态。
/// @return 不含谱面时只显示回退标题。
bool testPlaceholderIgnoresDirtyState()
{
    return MMM::Canvas::makeCanvasTabTitle("canvas.editor", false, "", true) ==
           "canvas.editor";
}

/// @brief 验证协作状态位于谱面名称前，脏标志仍固定在整个标签最前方。
/// @return 在线与离线标签均按预期组合时返回 true。
bool testCollaborationStatusPrecedesBeatmapName()
{
    return MMM::Canvas::makeCanvasTabTitle(
               "canvas.editor", true, "Online Map", false, "(在线)") ==
               "(在线) Online Map" &&
           MMM::Canvas::makeCanvasTabTitle(
               "canvas.editor", true, "Offline Map", true, "(离线)") ==
               "* (离线) Offline Map";
}

/// @brief 验证协作画布在欢迎页复用、断线和关闭过程中的标记生命周期。
/// @return 远端谱面正确标记、断线保留且回到欢迎页后清除时返回 true。
bool testCollaborationCanvasStateLifecycle()
{
    const bool offlinePlaceholder =
        MMM::Canvas::resolveCollaborationCanvasState(
            true, true, false, false, false, true);
    const bool joiningPlaceholder =
        MMM::Canvas::resolveCollaborationCanvasState(
            false, true, true, true, false, true);
    const bool joinedThroughReusedPlaceholder =
        MMM::Canvas::resolveCollaborationCanvasState(
            false, false, true, true, true, true);
    const bool joinedDirectly = MMM::Canvas::resolveCollaborationCanvasState(
        false, false, false, true, false, true);
    const bool disconnectedBeatmap =
        MMM::Canvas::resolveCollaborationCanvasState(
            true, false, false, false, true, true);
    const bool inactiveOfflineBeatmap =
        MMM::Canvas::resolveCollaborationCanvasState(
            true, false, false, false, true, false);
    const bool additionalLocalBeatmap =
        MMM::Canvas::resolveCollaborationCanvasState(
            false, false, false, false, false, true);
    return !offlinePlaceholder && !joiningPlaceholder &&
           joinedThroughReusedPlaceholder && joinedDirectly &&
           disconnectedBeatmap && inactiveOfflineBeatmap &&
           !additionalLocalBeatmap;
}

}  // namespace

/// @brief 覆盖主画布标签的标题和脏标志布局规则。
/// @return 所有标签规则满足时返回 0。
int main()
{
    return testDirtyLongTitleKeepsLeadingMarker() &&
                   testCleanTitleHasNoMarker() &&
                   testDirtyUnnamedBeatmapKeepsMarker() &&
                   testPlaceholderIgnoresDirtyState() &&
                   testCollaborationStatusPrecedesBeatmapName() &&
                   testCollaborationCanvasStateLifecycle()
               ? 0
               : 1;
}
