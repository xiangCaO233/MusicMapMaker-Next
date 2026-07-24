#include "font/AsciiFontRasterizer.h"

#include "log/colorful-log.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>

namespace MMM::Graphic
{

namespace
{

/// @brief 释放 FreeType 库对象。
struct FreeTypeLibraryReleaser {
    /// @brief 释放 FreeType 库对象。
    void operator()(std::remove_pointer_t<FT_Library>* library) const
    {
        if ( library ) FT_Done_FreeType(library);
    }
};

/// @brief 释放 FreeType 字体对象。
struct FreeTypeFaceReleaser {
    /// @brief 释放 FreeType 字体对象。
    void operator()(std::remove_pointer_t<FT_Face>* face) const
    {
        if ( face ) FT_Done_Face(face);
    }
};

using UniqueFreeTypeLibrary =
    std::unique_ptr<std::remove_pointer_t<FT_Library>, FreeTypeLibraryReleaser>;
using UniqueFreeTypeFace =
    std::unique_ptr<std::remove_pointer_t<FT_Face>, FreeTypeFaceReleaser>;

/// @brief 将文件系统路径转换为 FreeType 可读取的 UTF-8 字符串。
/// @param path 字体路径。
/// @return UTF-8 路径。
std::string fontPathUtf8(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return { reinterpret_cast<const char*>(value.data()), value.size() };
}

/// @brief 从 FreeType 灰度或单色位图取得覆盖率。
/// @param bitmap 字形位图。
/// @param x 像素横坐标。
/// @param y 像素纵坐标。
/// @return 0 至 255 的覆盖率。
unsigned char bitmapCoverage(const FT_Bitmap& bitmap, unsigned int x,
                             unsigned int y)
{
    const int   pitch = bitmap.pitch;
    const auto* row =
        pitch >= 0
            ? bitmap.buffer + static_cast<int>(y) * pitch
            : bitmap.buffer + static_cast<int>(bitmap.rows - 1U - y) * -pitch;
    if ( bitmap.pixel_mode == FT_PIXEL_MODE_GRAY ) {
        return row[x];
    }
    if ( bitmap.pixel_mode == FT_PIXEL_MODE_MONO ) {
        return (row[x / 8U] & (0x80U >> (x % 8U))) != 0U ? 255U : 0U;
    }
    return 0U;
}

}  // namespace

std::optional<RasterizedAsciiFont> AsciiFontRasterizer::rasterize(
    const std::filesystem::path& fontPath, std::uint32_t pixelHeight)
{
    if ( pixelHeight == 0U ) {
        XERROR("ASCII font raster height must be greater than zero");
        return std::nullopt;
    }

    FT_Library rawLibrary = nullptr;
    if ( FT_Init_FreeType(&rawLibrary) != 0 || !rawLibrary ) {
        XERROR("Failed to initialize FreeType for ASCII font atlas");
        return std::nullopt;
    }
    UniqueFreeTypeLibrary library(rawLibrary);

    const std::string pathUtf8 = fontPathUtf8(fontPath);
    FT_Face           rawFace  = nullptr;
    if ( FT_New_Face(library.get(), pathUtf8.c_str(), 0, &rawFace) != 0 ||
         !rawFace ) {
        XERROR("Failed to load ASCII font: {}", pathUtf8);
        return std::nullopt;
    }
    UniqueFreeTypeFace face(rawFace);

    if ( FT_Set_Pixel_Sizes(face.get(), 0U, pixelHeight) != 0 ) {
        XERROR("Failed to set ASCII font raster size: {}", pathUtf8);
        return std::nullopt;
    }

    RasterizedAsciiFont result;
    const float         inverseHeight = 1.0f / static_cast<float>(pixelHeight);
    result.metrics.valid              = true;
    result.metrics.ascender = static_cast<float>(face->size->metrics.ascender) /
                              64.0f * inverseHeight;
    result.metrics.lineHeight =
        std::max(1.0f, static_cast<float>(face->size->metrics.height) / 64.0f) *
        inverseHeight;

    for ( std::uint32_t code = Common::ASCII_GLYPH_FIRST;
          code <= Common::ASCII_GLYPH_LAST;
          ++code ) {
        const std::size_t index = code - Common::ASCII_GLYPH_FIRST;
        if ( FT_Load_Char(face.get(),
                          code,
                          FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0 ) {
            continue;
        }

        const FT_GlyphSlot slot    = face->glyph;
        const FT_Bitmap&   bitmap  = slot->bitmap;
        auto&              metrics = result.metrics.glyphs[index];
        metrics.available          = true;
        metrics.hasBitmap          = bitmap.width > 0U && bitmap.rows > 0U;
        metrics.width  = static_cast<float>(bitmap.width) * inverseHeight;
        metrics.height = static_cast<float>(bitmap.rows) * inverseHeight;
        metrics.bearingX =
            static_cast<float>(slot->bitmap_left) * inverseHeight;
        metrics.bearingY = static_cast<float>(slot->bitmap_top) * inverseHeight;
        metrics.advanceX =
            static_cast<float>(slot->advance.x) / 64.0f * inverseHeight;

        if ( !metrics.hasBitmap ) continue;
        if ( bitmap.pixel_mode != FT_PIXEL_MODE_GRAY &&
             bitmap.pixel_mode != FT_PIXEL_MODE_MONO ) {
            metrics.hasBitmap = false;
            continue;
        }

        auto& glyph  = result.glyphs[index];
        glyph.width  = bitmap.width;
        glyph.height = bitmap.rows;
        glyph.pixels.resize(static_cast<std::size_t>(glyph.width) *
                            static_cast<std::size_t>(glyph.height) * 4U);
        for ( std::uint32_t y = 0; y < glyph.height; ++y ) {
            for ( std::uint32_t x = 0; x < glyph.width; ++x ) {
                const auto alpha = bitmapCoverage(bitmap, x, y);
                const auto offset =
                    (static_cast<std::size_t>(y) * glyph.width + x) * 4U;
                glyph.pixels[offset + 0U] = 255U;
                glyph.pixels[offset + 1U] = 255U;
                glyph.pixels[offset + 2U] = 255U;
                glyph.pixels[offset + 3U] = alpha;
            }
        }
    }

    XDEBUG("Rasterized ASCII font atlas source: {} ({} px)",
           pathUtf8,
           pixelHeight);
    return result;
}

}  // namespace MMM::Graphic
