#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

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

/// @brief 单个 ASCII 字形相对标准栅格高度的度量。
struct AsciiGlyphMetrics {
    /// @brief 字形是否可以使用。
    bool available{ false };
    /// @brief 字形是否拥有可绘制位图。
    bool hasBitmap{ false };
    /// @brief 位图宽度比例。
    float width{ 0.0f };
    /// @brief 位图高度比例。
    float height{ 0.0f };
    /// @brief 位图左侧相对笔位置的比例。
    float bearingX{ 0.0f };
    /// @brief 位图顶部相对基线的比例。
    float bearingY{ 0.0f };
    /// @brief 字形横向推进比例。
    float advanceX{ 0.0f };
};

/// @brief 一套固定 ASCII 字形的渲染度量。
struct AsciiFontMetrics {
    /// @brief 字体资源是否有效。
    bool valid{ false };
    /// @brief 基线以上高度比例。
    float ascender{ 0.0f };
    /// @brief 单行高度比例。
    float lineHeight{ 1.0f };
    /// @brief 固定 ASCII 范围的字形度量。
    std::array<AsciiGlyphMetrics, ASCII_GLYPH_COUNT> glyphs{};

    /// @brief 按 ASCII 字符取得字形度量。
    /// @param character ASCII 字符。
    /// @return 范围内返回字形指针，否则返回 nullptr。
    [[nodiscard]] const AsciiGlyphMetrics* glyph(char character) const
    {
        const auto code = static_cast<unsigned char>(character);
        if ( code < ASCII_GLYPH_FIRST || code > ASCII_GLYPH_LAST ) {
            return nullptr;
        }
        return &glyphs[code - ASCII_GLYPH_FIRST];
    }
};

/// @brief 同一 ASCII 字体的多档提示度量。
struct AsciiFontAtlasMetrics {
    /// @brief 是否至少有一个字号档位可用。
    bool valid{ false };
    /// @brief 与 `ASCII_FONT_RASTER_HEIGHTS` 一一对应的字体度量。
    std::array<AsciiFontMetrics, ASCII_FONT_RASTER_TIER_COUNT> tiers{};
};

/// @brief 一次 ASCII 字体档位选择结果。
struct AsciiFontSelection {
    /// @brief 选中的字体度量；没有可用档位时为空。
    const AsciiFontMetrics* metrics{ nullptr };
    /// @brief 选中的字号档位索引。
    std::size_t tierIndex{ 0U };

    /// @brief 判断选择结果是否有效。
    /// @return 已选中字体度量时返回 true。
    [[nodiscard]] explicit operator bool() const { return metrics != nullptr; }
};

/// @brief 为目标像素字号选择最接近的 FreeType 提示档位。
/// @param atlas 多档 ASCII 字体度量。
/// @param fontPixelHeight 目标字号像素高度。
/// @return 最接近且有效的字体档位。
/// @warning 热路径：组件绘制和布局命中时调用；只遍历固定十一个档位。
[[nodiscard]] inline AsciiFontSelection selectAsciiFont(
    const AsciiFontAtlasMetrics& atlas, float fontPixelHeight)
{
    if ( !atlas.valid || !std::isfinite(fontPixelHeight) ||
         fontPixelHeight <= 0.0f ) {
        return {};
    }

    AsciiFontSelection selection;
    float              bestDistance = std::numeric_limits<float>::max();
    for ( std::size_t tierIndex = 0U; tierIndex < ASCII_FONT_RASTER_TIER_COUNT;
          ++tierIndex ) {
        const auto& metrics = atlas.tiers[tierIndex];
        if ( !metrics.valid ) continue;

        const float distance =
            std::abs(fontPixelHeight -
                     static_cast<float>(ASCII_FONT_RASTER_HEIGHTS[tierIndex]));
        if ( distance < bestDistance ) {
            bestDistance        = distance;
            selection.metrics   = &metrics;
            selection.tierIndex = tierIndex;
        }
    }
    return selection;
}

/// @brief 纯 ASCII 文本的像素尺寸。
struct AsciiTextSize {
    /// @brief 横向推进总宽度。
    float width{ 0.0f };
    /// @brief 字体单行高度。
    float height{ 0.0f };
};

/// @brief 使用字体度量计算单行 ASCII 文本尺寸。
/// @param font 字体度量。
/// @param text 以空字符结尾的 ASCII 文本。
/// @param fontPixelHeight 目标字号像素高度。
/// @return 文本横向推进宽度与单行高度。
/// @warning 热路径：组件绘制和布局命中时调用；只允许线性扫描短文本。
[[nodiscard]] inline AsciiTextSize measureAsciiText(
    const AsciiFontMetrics& font, const char* text, float fontPixelHeight)
{
    AsciiTextSize result;
    if ( !font.valid || !text || fontPixelHeight <= 0.0f ) {
        return result;
    }

    for ( const char* cursor = text; *cursor != '\0'; ++cursor ) {
        const auto* glyphMetrics = font.glyph(*cursor);
        if ( glyphMetrics && glyphMetrics->available ) {
            result.width += glyphMetrics->advanceX * fontPixelHeight;
        }
    }
    result.height = font.lineHeight * fontPixelHeight;
    return result;
}

}  // namespace MMM::Common
