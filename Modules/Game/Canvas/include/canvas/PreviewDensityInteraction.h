#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace MMM::Canvas
{

/// @brief 将全谱时间投影为密度栏垂直坐标。
/// @param time 待投影的谱面时间，单位秒。
/// @param topY 密度时间轴顶部屏幕纵坐标，对应谱面末尾。
/// @param bottomY 密度时间轴底部屏幕纵坐标，对应谱面开头。
/// @param duration 密度时间轴覆盖的总时长，单位秒。
/// @return 经过边界限制的屏幕纵坐标；参数无效时返回空。
/// @warning UI 热路径纯计算：每个协作者每帧调用，不得引入分配或阻塞操作。
[[nodiscard]] inline std::optional<double> previewDensityYAtTime(
    double time, double topY, double bottomY, double duration)
{
    if ( !std::isfinite(time) || !std::isfinite(topY) ||
         !std::isfinite(bottomY) || !std::isfinite(duration) ||
         bottomY <= topY || duration <= 0.0 ) {
        return std::nullopt;
    }

    const double progress = std::clamp(time / duration, 0.0, 1.0);
    return bottomY - progress * (bottomY - topY);
}

/// @brief 将密度栏垂直坐标映射为全谱时间。
/// @param pointerY 指针当前屏幕纵坐标。
/// @param topY 密度时间轴顶部屏幕纵坐标，对应谱面末尾。
/// @param bottomY 密度时间轴底部屏幕纵坐标，对应谱面开头。
/// @param duration 密度时间轴覆盖的总时长，单位秒。
/// @return 经过边界限制的谱面时间；参数无效时返回空。
/// @warning UI 热路径纯计算：拖动密度栏时每帧调用，不得引入分配或阻塞操作。
[[nodiscard]] inline std::optional<double> previewDensityTimeAtY(
    double pointerY, double topY, double bottomY, double duration)
{
    if ( !std::isfinite(pointerY) || !std::isfinite(topY) ||
         !std::isfinite(bottomY) || !std::isfinite(duration) ||
         bottomY <= topY || duration <= 0.0 ) {
        return std::nullopt;
    }

    const double progress =
        std::clamp((bottomY - pointerY) / (bottomY - topY), 0.0, 1.0);
    return progress * duration;
}

}  // namespace MMM::Canvas
