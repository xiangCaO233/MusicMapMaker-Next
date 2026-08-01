#include "font/AsciiFontRasterizer.h"

#include "config/Utf8Path.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace
{

/// @brief 验证时间文本需要的 ASCII 字形拥有可上传的 RGBA 位图。
/// @param font 已完成栅格化的字体。
/// @return 数字与时间分隔符均可用时返回 true。
bool hasTimeGlyphBitmaps(const MMM::Graphic::RasterizedAsciiFont& font)
{
    constexpr std::array<char, 12> requiredGlyphs{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.'
    };
    for ( const char character : requiredGlyphs ) {
        const auto* metrics = font.metrics.glyph(character);
        if ( !metrics || !metrics->available || !metrics->hasBitmap ) {
            return false;
        }

        const auto index =
            static_cast<std::uint32_t>(static_cast<unsigned char>(character)) -
            MMM::Common::ASCII_GLYPH_FIRST;
        const auto& glyph = font.glyphs[index];
        if ( glyph.width == 0U || glyph.height == 0U ||
             glyph.pixels.size() !=
                 static_cast<std::size_t>(glyph.width) * glyph.height * 4U ) {
            return false;
        }
    }
    return true;
}

}  // namespace

/// @brief 使用默认皮肤 ASCII 字体验证全部提示字号的独立 FreeType 栅格化结果。
/// @param argc 参数数量。
/// @param argv 第一个参数为字体文件 UTF-8 路径。
/// @return 全部字体度量、时间字形位图和小字号选择有效时返回 0。
int main(int argc, char** argv)
{
    if ( argc != 3 || !argv[1] || !argv[2] ) {
        return 1;
    }

    MMM::Common::AsciiFontAtlasMetrics atlas;
    const auto fontPath = MMM::Config::utf8ToPath(argv[1]);
    for ( std::size_t tierIndex = 0U;
          tierIndex < MMM::Common::ASCII_FONT_RASTER_TIER_COUNT;
          ++tierIndex ) {
        const auto rasterized = MMM::Graphic::AsciiFontRasterizer::rasterize(
            fontPath, MMM::Common::ASCII_FONT_RASTER_HEIGHTS[tierIndex]);
        if ( !rasterized || !rasterized->metrics.valid ||
             rasterized->metrics.lineHeight <= 0.0f ||
             rasterized->metrics.ascender <= 0.0f ) {
            return 2;
        }
        if ( !hasTimeGlyphBitmaps(*rasterized) ) {
            return 3;
        }
        if ( rasterized->metrics.glyph('\x01') != nullptr ) {
            return 4;
        }
        atlas.tiers[tierIndex] = rasterized->metrics;
    }
    atlas.valid = true;

    for ( std::size_t tierIndex = 0U; tierIndex < 5U; ++tierIndex ) {
        const auto selected = MMM::Common::selectAsciiFont(
            atlas,
            static_cast<float>(
                MMM::Common::ASCII_FONT_RASTER_HEIGHTS[tierIndex]));
        if ( !selected || selected.tierIndex != tierIndex ) {
            return 5;
        }
    }

    constexpr std::array<std::uint32_t, 10> cjkCodepoints{
        0x3074U, 0x3087U, 0x3093U, 0x30AFU, 0x30C6U,
        0x30C8U, 0x30DFU, 0x521DU, 0x91CDU, 0x97F3U
    };
    const auto unicodeFont =
        MMM::Graphic::AsciiFontRasterizer::rasterizeUnicode(
            MMM::Config::utf8ToPath(argv[2]),
            std::span<const std::uint32_t>(cjkCodepoints));
    if ( !unicodeFont || !unicodeFont->metrics.valid ||
         unicodeFont->metrics.glyphs.size() != cjkCodepoints.size() ||
         unicodeFont->glyphs.size() != cjkCodepoints.size() ) {
        return 6;
    }
    for ( std::size_t index = 0U; index < cjkCodepoints.size(); ++index ) {
        const auto* metrics = unicodeFont->metrics.glyph(cjkCodepoints[index]);
        const auto& glyph   = unicodeFont->glyphs[index];
        if ( !metrics || !metrics->available || !metrics->hasBitmap ||
             glyph.width == 0U || glyph.height == 0U ||
             glyph.pixels.size() !=
                 static_cast<std::size_t>(glyph.width) * glyph.height * 4U ) {
            return 7;
        }
    }
    return 0;
}
