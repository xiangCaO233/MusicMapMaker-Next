#pragma once

#include <algorithm>

namespace MMM::UI
{

/// @brief 音效工具各轨道区域的控件行数。
struct SoundEffectToolTrackLayout {
    /// @brief 玩家轨道控件行数，空谱面时仍保留一行提示。
    int playerRows{ 1 };
    /// @brief 草稿轨道控件行数，使用动态扩充后的持久化轨道数量。
    int draftRows{ 1 };
    /// @brief BGM 轨道控件行数，空区域时仍保留一行提示。
    int bgmRows{ 1 };
    /// @brief 包含分类标题和总控件的全部行数。
    int totalRows{ 13 };
};

/// @brief 根据各区域实际轨道数量计算音效工具控件布局。
/// @param playerTrackCount 玩家轨道数量。
/// @param draftTrackCount 动态扩充后的草稿轨道数量。
/// @param bgmTrackCount BGM 轨道数量。
/// @return 每个区域与整个弹层的控件行数。
[[nodiscard]] constexpr SoundEffectToolTrackLayout
calculateSoundEffectToolTrackLayout(int playerTrackCount, int draftTrackCount,
                                    int bgmTrackCount) noexcept
{
    SoundEffectToolTrackLayout layout;
    layout.playerRows = std::max(1, playerTrackCount);
    layout.draftRows  = std::max(1, draftTrackCount);
    layout.bgmRows    = std::max(1, bgmTrackCount);
    layout.totalRows =
        10 + layout.playerRows + layout.draftRows + layout.bgmRows;
    return layout;
}

}  // namespace MMM::UI
