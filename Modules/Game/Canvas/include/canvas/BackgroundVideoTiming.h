#pragma once

#include <cmath>

namespace MMM::Canvas
{

/// @brief 根据谱面原始播放时钟计算当前应展示的视频时间。
/// @param playbackTime 快照中未包含视觉偏移的谱面时间。
/// @param videoStartTime 视频事件在谱面上的开始时间。
/// @param isPlaying 快照是否处于播放状态。
/// @param snapshotSysTime 快照生成时的 steady_clock 秒数。
/// @param playbackSpeed 当前播放速度倍率。
/// @param currentSysTime 渲染侧当前 steady_clock 秒数。
/// @return 视频内部时间；负值表示尚未到视频开始点。
/// @warning UI 每帧路径：只允许常数时间数值计算。
inline double calculateBackgroundVideoTime(
    double playbackTime, double videoStartTime, bool isPlaying,
    double snapshotSysTime, double playbackSpeed, double currentSysTime)
{
    double       extrapolatedTime = playbackTime;
    const double elapsed          = currentSysTime - snapshotSysTime;
    if ( isPlaying && snapshotSysTime > 0.0 && elapsed > 0.0 && elapsed < 0.1 &&
         std::isfinite(elapsed) && std::isfinite(playbackSpeed) ) {
        extrapolatedTime += elapsed * playbackSpeed;
    }
    return extrapolatedTime - videoStartTime;
}

}  // namespace MMM::Canvas
