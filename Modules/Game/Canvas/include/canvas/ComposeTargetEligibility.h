#pragma once

#include <cmath>

namespace MMM::Canvas
{
/// @brief 教学起终点只允许使用逻辑画笔可放置的非负、有限秒时间。
/// @note 不使用浮点容差放宽零边界，也不钳制为零，否则会偏离实际分拍线。
/// @warning UI 热路径：纯数值检查，不分配或访问谱面实体。
inline bool isComposeTargetTime(double time)
{
    return std::isfinite(time) && time >= 0.0;
}

/// @brief 统一检查单键、滑键与长条候选拍位和完整 Note 框的可见性。
/// @param time 实际渲染拍线的精确时间。
/// @param y 与上下界位于同一坐标空间的拍线中心。
/// @param noteHeight 真实 Note 渲染高度。
/// @param top 玩家区可见上界。
/// @param bottom 玩家区可见下界。
/// @warning 候选生成和逐帧缓存复核共用；仅进行固定次数的数值运算。
inline bool isComposeTargetBeatLine(double time, float y, float noteHeight,
                                    float top, float bottom)
{
    // 可见负拍线只是渲染参考，不能作为画笔起点或终点。
    if ( !isComposeTargetTime(time) || !std::isfinite(y) ||
         !std::isfinite(noteHeight) || noteHeight <= 0.0F ||
         !std::isfinite(top) || !std::isfinite(bottom) )
        return false;
    // 中心在视野内仍可能裁掉半个物件，两侧均完整可见才生成教学框。
    return y - noteHeight * 0.5F >= top && y + noteHeight * 0.5F <= bottom;
}
}  // namespace MMM::Canvas
