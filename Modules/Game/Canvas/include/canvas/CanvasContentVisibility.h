#pragma once

namespace MMM::Canvas
{

/// @brief 判断当前快照是否允许显示谱面专属信息。
/// @param hasBeatmap 快照是否绑定了真实谱面。
/// @return 仅真实谱面返回 true；欢迎占位页返回 false。
/// @warning UI 热路径：每帧只执行常量布尔判断。
[[nodiscard]] constexpr bool shouldShowBeatmapDetails(bool hasBeatmap)
{
    return hasBeatmap;
}

/// @brief 判断主画布是否应显示鼠标悬浮检视层。
/// @param hasBeatmap 快照是否绑定了真实谱面。
/// @param overlayBlocksCanvas 其他浮层是否占用画布交互。
/// @param isWindowHovered 鼠标是否位于当前画布窗口。
/// @param isCanvasHovered 逻辑快照是否确认鼠标悬停画布。
/// @param isPlaying 当前谱面是否正在播放。
/// @return 所有悬浮检视条件满足时返回 true。
/// @warning UI 热路径：每帧只执行常量布尔判断。
[[nodiscard]] constexpr bool shouldShowCanvasHoverInspection(
    bool hasBeatmap, bool overlayBlocksCanvas, bool isWindowHovered,
    bool isCanvasHovered, bool isPlaying)
{
    return shouldShowBeatmapDetails(hasBeatmap) && !overlayBlocksCanvas &&
           isWindowHovered && isCanvasHovered && !isPlaying;
}

}  // namespace MMM::Canvas
