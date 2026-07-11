#pragma once

#include <span>
#include <string_view>

namespace MMM::UI
{

/// @brief 已打开谱面用于计算打包默认选择的轻量状态。
struct PackageOpenBeatmapState {
    /// @brief 与候选文件使用相同规则生成的稳定绝对路径键。
    std::string_view beatmapPathKey{};

    /// @brief 对应主画布是否为所属停靠组当前显示的前台标签。
    bool isCanvasVisible{ false };

    /// @brief 对应 Session 是否为当前活动 Session。
    bool isActive{ false };

    /// @brief 是否为未加载谱面的 Logo 占位画布。
    bool isLogoPlaceholder{ false };
};

/// @brief 判断谱面候选是否应在打开打包窗口时默认选中。
/// @param candidatePathKey 候选谱面的稳定绝对路径键。
/// @param openBeatmaps 当前已打开谱面的前台、活动与占位状态。
/// @return 存在有效前台画布时只匹配全部前台画布；没有前台画布时只匹配活动画布。
constexpr bool shouldDefaultSelectPackageBeatmap(
    std::string_view                         candidatePathKey,
    std::span<const PackageOpenBeatmapState> openBeatmaps)
{
    if ( candidatePathKey.empty() ) {
        return false;
    }

    bool hasVisibleBeatmap = false;
    for ( const auto& beatmap : openBeatmaps ) {
        if ( beatmap.isLogoPlaceholder || beatmap.beatmapPathKey.empty() ||
             !beatmap.isCanvasVisible ) {
            continue;
        }
        hasVisibleBeatmap = true;
        if ( beatmap.beatmapPathKey == candidatePathKey ) {
            return true;
        }
    }
    if ( hasVisibleBeatmap ) {
        return false;
    }

    for ( const auto& beatmap : openBeatmaps ) {
        if ( !beatmap.isLogoPlaceholder && beatmap.isActive &&
             beatmap.beatmapPathKey == candidatePathKey ) {
            return true;
        }
    }
    return false;
}

}  // namespace MMM::UI
