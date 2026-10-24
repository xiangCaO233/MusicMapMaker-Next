#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace MMM::Common
{

/// @brief ASCII 字形范围起点。
inline constexpr std::uint32_t ASCII_GLYPH_FIRST = 0x20U;
/// @brief ASCII 字形范围终点。
inline constexpr std::uint32_t ASCII_GLYPH_LAST = 0x7EU;
/// @brief ASCII 字形数量。
inline constexpr std::size_t ASCII_GLYPH_COUNT =
    ASCII_GLYPH_LAST - ASCII_GLYPH_FIRST + 1U;
/// @brief 独立字体图集使用的 FreeType 提示字号档位。
inline constexpr std::array<std::uint32_t, 11> ASCII_FONT_RASTER_HEIGHTS{
    8U, 10U, 12U, 14U, 16U, 24U, 32U, 48U, 64U, 96U, 128U
};
/// @brief 独立字体图集字号档位数量。
inline constexpr std::size_t ASCII_FONT_RASTER_TIER_COUNT =
    ASCII_FONT_RASTER_HEIGHTS.size();
/// @brief 单次字体栅格化 API 的默认像素高度。
inline constexpr std::uint32_t ASCII_FONT_DEFAULT_RASTER_HEIGHT = 32U;

/// @brief 独立画布 Unicode 字形的固定栅格高度。
/// @note 所有字形度量都相对此高度归一化，避免快照携带字体像素尺寸。
inline constexpr std::uint32_t UNICODE_FONT_RASTER_HEIGHT = 32U;

/// @brief Unicode 替换字符码点。
/// @note 无效 UTF-8 序列统一解析为 U+FFFD，保证扫描始终向前推进。
inline constexpr std::uint32_t UNICODE_REPLACEMENT_CODEPOINT = 0xFFFDU;

}  // namespace MMM::Common
