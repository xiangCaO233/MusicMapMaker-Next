#pragma once

namespace MMM::Canvas
{

/// @brief 将已解析到当前 UI 帧的谱面播放时间换算为视频内部时间。
/// @param resolvedPlaybackTime 已完成亚帧与谱面末尾钳制的播放时间。
/// @param videoStartTime 视频事件在谱面上的开始时间。
/// @return 视频内部时间；负值表示尚未到视频开始点。
/// @warning UI 每帧路径：只允许常数时间数值计算。
inline double calculateBackgroundVideoTime(double resolvedPlaybackTime,
                                           double videoStartTime)
{
    return resolvedPlaybackTime - videoStartTime;
}

}  // namespace MMM::Canvas
