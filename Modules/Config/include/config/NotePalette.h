#pragma once

#include <array>
#include <cstddef>

namespace MMM::Config
{

/// @brief 音符调色盘中按部件语义排列的固定槽位数量。
/// @note 六个槽位对应 Tap、Head、Hold、End、FlickArrow 和 Node。
inline constexpr std::size_t NOTE_COLOR_PALETTE_SLOT_COUNT = 6;

/// @brief 音符调色盘的 RGBA 值数组，槽位顺序与物件颜色工具一致。
/// @note 配置层使用浮点数组，避免渲染层 glm 类型进入设置文件格式。
/// @note 运行时视觉配置持有相同值，但不会把活动方案写入用户视觉设置。
using NoteColorPalette =
    std::array<std::array<float, 4>, NOTE_COLOR_PALETTE_SLOT_COUNT>;

}  // namespace MMM::Config
