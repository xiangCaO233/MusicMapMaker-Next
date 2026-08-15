#include "font/AsciiFontRasterizer.h"

#include "config/Utf8Path.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

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

/// @brief 在测试结束时清理精确的字体路径回归目录。
struct TestDirectoryCleanup {
    /// @brief 待清理目录。
    std::filesystem::path path;

    /// @brief 删除本测试创建的目录。
    ~TestDirectoryCleanup()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

}  // namespace

/// @brief 使用默认皮肤 ASCII 字体验证全部提示字号的独立 FreeType 栅格化结果。
/// @param argc 参数数量。
/// @param argv 依次提供 ASCII 字体、CJK 字体和测试输出目录的 UTF-8 路径。
/// @return 全部字体度量、时间字形位图和小字号选择有效时返回 0。
int main(int argc, char** argv)
{
    if ( argc != 4 || !argv[1] || !argv[2] || !argv[3] ) {
        return 1;
    }

    const auto unicodePathRoot = MMM::Config::utf8ToPath(argv[3]) /
                                 MMM::Config::utf8ToPath("字体栅格化测试") /
                                 MMM::Config::utf8ToPath("中文用户目录");
    TestDirectoryCleanup cleanup{ unicodePathRoot.parent_path() };
    std::error_code      fileError;
    std::filesystem::create_directories(unicodePathRoot, fileError);
    if ( fileError ) return 8;

    const auto fontPath =
        unicodePathRoot / MMM::Config::utf8ToPath("ASCII字体.ttf");
    const auto unicodeFontPath =
        unicodePathRoot / MMM::Config::utf8ToPath("中文字体.otf");
    std::filesystem::copy_file(
        MMM::Config::utf8ToPath(argv[1]),
        fontPath,
        std::filesystem::copy_options::overwrite_existing,
        fileError);
    if ( fileError ) return 9;
    std::filesystem::copy_file(
        MMM::Config::utf8ToPath(argv[2]),
        unicodeFontPath,
        std::filesystem::copy_options::overwrite_existing,
        fileError);
    if ( fileError ) return 10;

    MMM::Common::AsciiFontAtlasMetrics atlas;
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
            unicodeFontPath, std::span<const std::uint32_t>(cjkCodepoints));
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
