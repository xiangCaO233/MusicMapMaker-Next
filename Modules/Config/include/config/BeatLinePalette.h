#pragma once

#include <array>
#include <cstddef>

namespace MMM::Config
{

/// @brief 分拍线调色盘中具有独立皮肤颜色的分母列表。
inline constexpr std::array<int, 8> BEAT_LINE_COLOR_PALETTE_DENOMINATORS = {
    1, 2, 3, 4, 6, 8, 12, 16
};

/// @brief 分拍线调色盘槽位数量，最后一个槽位用于其它分母的默认颜色。
inline constexpr std::size_t BEAT_LINE_COLOR_PALETTE_SLOT_COUNT =
    BEAT_LINE_COLOR_PALETTE_DENOMINATORS.size() + 1;

/// @brief 分拍线调色盘的持久化颜色数组。
/// 每个槽位保存 RGBA 四通道，数组尺寸与分母映射保持编译期一致。
using BeatLineColorPalette =
    std::array<std::array<float, 4>, BEAT_LINE_COLOR_PALETTE_SLOT_COUNT>;

/// @brief 查询指定分母对应的分拍线调色盘槽位。
/// @param denominator 分拍分母。
/// @return 已知分母的专用槽位，未知分母返回最后一个默认槽位。
[[nodiscard]] constexpr std::size_t beatLineColorPaletteSlot(int denominator)
{
    // 显式映射保持持久化槽位稳定，不能依赖数组查找顺序的隐式转换。
    switch ( denominator ) {
    // 整拍和常用二分拍占用前两个固定槽位。
    case 1: return 0;
    case 2: return 1;
    // 三、四、六分拍继续按公开调色盘顺序映射。
    case 3: return 2;
    case 4: return 3;
    case 6: return 4;
    // 高频细分保留独立颜色，便于密集网格保持层次。
    case 8: return 5;
    case 12: return 6;
    case 16: return 7;
    // 负数、零和非常用分母共享最后一个安全回退槽位。
    default: return BEAT_LINE_COLOR_PALETTE_SLOT_COUNT - 1;
    }
}

}  // namespace MMM::Config
