#include "ui/imgui/manager/SoundEffectToolTrackLayout.h"

/// @file SoundEffectToolTrackLayoutTest.cpp
/// @brief 音效工具按玩家、草稿和 BGM 轨道计算控件行数的编译期测试。
/// @details 三个 constexpr 场景覆盖扩容、空区占位以及不同区域独立计数。

namespace
{

/// @brief 验证草稿轨自动扩充后音效工具生成对应数量的控件行。
/// @return 草稿区行数与扩充后的轨道数量一致时返回 true。
constexpr bool testExpandedDraftTracksCreateControls()
{
    // 每个草稿轨都必须生成独立控件，不能截断到默认轨道数。
    const auto layout = MMM::UI::calculateSoundEffectToolTrackLayout(4, 7, 1);
    return layout.playerRows == 4 && layout.draftRows == 7 &&
           layout.bgmRows == 1 && layout.totalRows == 24;
}

/// @brief 验证空区域仍保留一行状态提示。
/// @return 玩家、草稿和 BGM 区均保留单行时返回 true。
constexpr bool testEmptyAreasKeepPlaceholderRows()
{
    // 空轨道区仍保留一行，供界面显示“无轨道”状态。
    const auto layout = MMM::UI::calculateSoundEffectToolTrackLayout(0, 0, 0);
    return layout.playerRows == 1 && layout.draftRows == 1 &&
           layout.bgmRows == 1 && layout.totalRows == 15;
}

/// @brief 验证草稿控件数量独立于玩家轨道数量。
/// @return 草稿区不再错误复用玩家轨道数时返回 true。
constexpr bool testDraftRowsAreIndependentFromPlayerRows()
{
    // 特意让草稿轨多于玩家轨，捕获错误复用 playerRows 的实现。
    const auto layout = MMM::UI::calculateSoundEffectToolTrackLayout(4, 9, 2);
    return layout.playerRows == 4 && layout.draftRows == 9 &&
           layout.bgmRows == 2 && layout.totalRows == 27;
}

}  // namespace

/// @brief 运行音效工具轨道控件布局回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    // 编译期断言保证纯布局函数保持 constexpr 可用性。
    static_assert(testExpandedDraftTracksCreateControls());
    static_assert(testEmptyAreasKeepPlaceholderRows());
    static_assert(testDraftRowsAreIndependentFromPlayerRows());
    return 0;
}
