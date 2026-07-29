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

}  // namespace

/// @brief 覆盖主画布标签的标题和脏标志布局规则。
/// @return 所有标签规则满足时返回 0。
int main()
{
    return testDirtyLongTitleKeepsLeadingMarker() &&
                   testCleanTitleHasNoMarker() &&
                   testDirtyUnnamedBeatmapKeepsMarker() &&
                   testPlaceholderIgnoresDirtyState()
               ? 0
               : 1;
}
