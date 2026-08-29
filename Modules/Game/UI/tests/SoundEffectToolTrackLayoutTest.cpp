#include "ui/imgui/manager/SoundEffectToolTrackLayout.h"

namespace
{

/// @brief 验证草稿轨自动扩充后音效工具生成对应数量的控件行。
/// @return 草稿区行数与扩充后的轨道数量一致时返回 true。
constexpr bool testExpandedDraftTracksCreateControls()
{
    const auto layout = MMM::UI::calculateSoundEffectToolTrackLayout(4, 7, 1);
    return layout.playerRows == 4 && layout.draftRows == 7 &&
           layout.bgmRows == 1 && layout.totalRows == 22;
}

/// @brief 验证空区域仍保留一行状态提示。
/// @return 玩家、草稿和 BGM 区均保留单行时返回 true。
constexpr bool testEmptyAreasKeepPlaceholderRows()
{
    const auto layout = MMM::UI::calculateSoundEffectToolTrackLayout(0, 0, 0);
    return layout.playerRows == 1 && layout.draftRows == 1 &&
           layout.bgmRows == 1 && layout.totalRows == 13;
}

/// @brief 验证草稿控件数量独立于玩家轨道数量。
/// @return 草稿区不再错误复用玩家轨道数时返回 true。
constexpr bool testDraftRowsAreIndependentFromPlayerRows()
{
    const auto layout = MMM::UI::calculateSoundEffectToolTrackLayout(4, 9, 2);
    return layout.playerRows == 4 && layout.draftRows == 9 &&
           layout.bgmRows == 2 && layout.totalRows == 25;
}

}  // namespace

/// @brief 运行音效工具轨道控件布局回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    static_assert(testExpandedDraftTracksCreateControls());
    static_assert(testEmptyAreasKeepPlaceholderRows());
    static_assert(testDraftRowsAreIndependentFromPlayerRows());
    return 0;
}
