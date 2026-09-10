#pragma once

#include "config/visual/BackgroundConfig.h"

#include <array>
#include <cstddef>

namespace MMM::Audio
{

/// @brief 背景频谱渲染使用的单帧立体声频段电平。
///
/// 数组容量固定为配置允许的最大频段数，bandCount 指示本帧有效前缀；这种布局
/// 避免每帧频谱更新产生动态分配，也让渲染侧可复用同一对象引用。
struct BackgroundSpectrumLevels {
    /// @brief 左声道由低频到高频的归一化电平。
    std::array<float, Config::BACKGROUND_SPECTRUM_MAX_BANDS> left{};
    /// @brief 右声道由低频到高频的归一化电平。
    std::array<float, Config::BACKGROUND_SPECTRUM_MAX_BANDS> right{};
    /// @brief 当前有效的单声道频段数。
    std::size_t bandCount{ 0U };
};

}  // namespace MMM::Audio
