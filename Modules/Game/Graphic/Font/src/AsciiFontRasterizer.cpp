#include "font/AsciiFontRasterizer.h"

#include "log/colorful-log.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

/// @brief 保持字体文件字节在 FreeType Face 生命周期内有效。
struct LoadedFreeTypeFace {
    /// @brief 通过原生文件系统路径读取的完整字体文件。
    std::vector<FT_Byte> bytes;
    /// @brief 引用 bytes 的 FreeType Face；成员逆序析构确保先释放 Face。
    UniqueFreeTypeFace face;
};

/// @brief 通过 std::filesystem 原生路径读取字体并创建内存 Face。
/// @param library 已初始化的 FreeType 库。
/// @param fontPath 字体文件路径。
/// @return 文件完整可读且 FreeType 可以解析时返回持有对象。
std::optional<LoadedFreeTypeFace> loadFreeTypeFace(
    FT_Library library, const std::filesystem::path& fontPath)
{
    std::error_code fileSizeError;
    const auto fileSize = std::filesystem::file_size(fontPath, fileSizeError);
    if ( fileSizeError || fileSize == 0U ||
         fileSize >
             static_cast<std::uintmax_t>(std::numeric_limits<FT_Long>::max()) ||
         fileSize > static_cast<std::uintmax_t>(
                        std::numeric_limits<std::streamsize>::max()) ) {
        return std::nullopt;
    }

    std::vector<FT_Byte> bytes(static_cast<std::size_t>(fileSize));
    std::ifstream        input(fontPath, std::ios::binary);
    if ( !input ) return std::nullopt;
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if ( !input ||
         input.gcount() != static_cast<std::streamsize>(bytes.size()) ) {
        return std::nullopt;
    }

    FT_Face rawFace = nullptr;
    if ( FT_New_Memory_Face(library,
                            bytes.data(),
                            static_cast<FT_Long>(bytes.size()),
                            0,
                            &rawFace) != 0 ||
         !rawFace ) {
        return std::nullopt;
    }
    return LoadedFreeTypeFace{ std::move(bytes), UniqueFreeTypeFace(rawFace) };
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

/// @brief 将当前 FreeType 字形槽转换为归一化度量与 RGBA 位图。
/// @param slot 已加载的 FreeType 字形槽。
/// @param inverseHeight 标准栅格高度倒数。
/// @param metrics 接收归一化字形度量。
/// @param glyph 接收 RGBA 字形位图。
void copyLoadedGlyph(FT_GlyphSlot slot, float inverseHeight,
                     Common::AsciiGlyphMetrics& metrics,
                     RasterizedAsciiGlyph&      glyph)
{
    const FT_Bitmap& bitmap = slot->bitmap;
    metrics.available       = true;
    metrics.hasBitmap       = bitmap.width > 0U && bitmap.rows > 0U;
    metrics.width           = static_cast<float>(bitmap.width) * inverseHeight;
    metrics.height          = static_cast<float>(bitmap.rows) * inverseHeight;
    metrics.bearingX = static_cast<float>(slot->bitmap_left) * inverseHeight;
    metrics.bearingY = static_cast<float>(slot->bitmap_top) * inverseHeight;
    metrics.advanceX =
        static_cast<float>(slot->advance.x) / 64.0F * inverseHeight;

    if ( !metrics.hasBitmap ) return;
    if ( bitmap.pixel_mode != FT_PIXEL_MODE_GRAY &&
         bitmap.pixel_mode != FT_PIXEL_MODE_MONO ) {
        metrics.hasBitmap = false;
        return;
    }

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

    const std::string pathUtf8   = fontPathUtf8(fontPath);
    auto              loadedFace = loadFreeTypeFace(library.get(), fontPath);
    if ( !loadedFace ) {
        XERROR("Failed to load ASCII font: {}", pathUtf8);
        return std::nullopt;
    }
    auto& face = loadedFace->face;

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

        copyLoadedGlyph(face->glyph,
                        inverseHeight,
                        result.metrics.glyphs[index],
                        result.glyphs[index]);
    }

    XDEBUG("Rasterized ASCII font atlas source: {} ({} px)",
           pathUtf8,
           pixelHeight);
    return result;
}

std::optional<RasterizedUnicodeFont> AsciiFontRasterizer::rasterizeUnicode(
    const std::filesystem::path&   fontPath,
    std::span<const std::uint32_t> codepoints, std::uint32_t pixelHeight)
{
    if ( pixelHeight == 0U ) {
        XERROR("Unicode font raster height must be greater than zero");
        return std::nullopt;
    }

    FT_Library rawLibrary = nullptr;
    if ( FT_Init_FreeType(&rawLibrary) != 0 || !rawLibrary ) {
        XERROR("Failed to initialize FreeType for Unicode font atlas");
        return std::nullopt;
    }
    UniqueFreeTypeLibrary library(rawLibrary);

    const std::string pathUtf8   = fontPathUtf8(fontPath);
    auto              loadedFace = loadFreeTypeFace(library.get(), fontPath);
    if ( !loadedFace ) {
        XERROR("Failed to load Unicode font: {}", pathUtf8);
        return std::nullopt;
    }
    auto& face = loadedFace->face;

    if ( FT_Set_Pixel_Sizes(face.get(), 0U, pixelHeight) != 0 ) {
        XERROR("Failed to set Unicode font raster size: {}", pathUtf8);
        return std::nullopt;
    }

    RasterizedUnicodeFont result;
    const float inverseHeight = 1.0F / static_cast<float>(pixelHeight);
    result.metrics.valid      = true;
    result.metrics.ascender = static_cast<float>(face->size->metrics.ascender) /
                              64.0F * inverseHeight;
    result.metrics.lineHeight =
        std::max(1.0F, static_cast<float>(face->size->metrics.height) / 64.0F) *
        inverseHeight;
    result.metrics.glyphs.reserve(codepoints.size());
    result.glyphs.reserve(codepoints.size());

    std::uint32_t previousCodepoint = 0U;
    bool          hasPrevious       = false;
    for ( const std::uint32_t codepoint : codepoints ) {
        if ( !Common::isValidUnicodeCodepoint(codepoint) ||
             codepoint <= Common::ASCII_GLYPH_LAST ||
             (hasPrevious && codepoint <= previousCodepoint) ) {
            continue;
        }
        previousCodepoint = codepoint;
        hasPrevious       = true;

        if ( FT_Get_Char_Index(face.get(), codepoint) == 0U ||
             FT_Load_Char(face.get(),
                          codepoint,
                          FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0 ) {
            continue;
        }

        Common::UnicodeGlyphMetrics metrics;
        metrics.codepoint = codepoint;
        RasterizedAsciiGlyph glyph;
        copyLoadedGlyph(face->glyph, inverseHeight, metrics.metrics, glyph);
        result.metrics.glyphs.push_back(metrics);
        result.glyphs.push_back(std::move(glyph));
    }

    XDEBUG("Rasterized Unicode font atlas source: {} ({} px, {} glyphs)",
           pathUtf8,
           pixelHeight,
           result.metrics.glyphs.size());
    return result;
}

}  // namespace MMM::Graphic
