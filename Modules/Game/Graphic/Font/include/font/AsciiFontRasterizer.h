#pragma once

#include "common/AsciiFontData.h"
#include "common/UnicodeFontData.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace MMM::Graphic
{

/// @brief 单个 ASCII 字形的 RGBA 位图。
struct RasterizedAsciiGlyph {
    /// @brief RGBA8 像素。
    std::vector<unsigned char> pixels;
    /// @brief 位图宽度。
    std::uint32_t width{ 0 };
    /// @brief 位图高度。
    std::uint32_t height{ 0 };
};

/// @brief 可上传到 Vulkan 图集的 ASCII 字体资源。
struct RasterizedAsciiFont {
    /// @brief 字体排版度量。
    Common::AsciiFontMetrics metrics;
    /// @brief 固定 ASCII 范围的字形位图。
    std::array<RasterizedAsciiGlyph, Common::ASCII_GLYPH_COUNT> glyphs;
};

/// @brief 可上传到 Vulkan 图集的按需 Unicode 字体资源。
struct RasterizedUnicodeFont {
    /// @brief Unicode 字体排版与字形度量。
    Common::UnicodeFontMetrics metrics;

    /// @brief 与 `metrics.glyphs` 一一对应的字形位图。
    std::vector<RasterizedAsciiGlyph> glyphs;
};

/// @brief 使用 FreeType 将字体文件栅格化为固定 ASCII 字形。
class AsciiFontRasterizer final
{
public:
    /// @brief 栅格化指定字体的 `U+0020` 至 `U+007E`。
    /// @param fontPath 字体文件路径。
    /// @param pixelHeight 标准栅格高度。
    /// @return 成功时返回字形位图与度量，失败时返回空值。
    [[nodiscard]] static std::optional<RasterizedAsciiFont> rasterize(
        const std::filesystem::path& fontPath,
        std::uint32_t pixelHeight = Common::ASCII_FONT_DEFAULT_RASTER_HEIGHT);

    /// @brief 按需栅格化指定 Unicode 码点，避免预加载完整 CJK 字库。
    /// @param fontPath 字体文件路径。
    /// @param codepoints 按升序去重后的 Unicode 码点。
    /// @param pixelHeight 标准栅格高度。
    /// @return 成功时返回可用字形位图与度量，失败时返回空值。
    [[nodiscard]] static std::optional<RasterizedUnicodeFont> rasterizeUnicode(
        const std::filesystem::path&   fontPath,
        std::span<const std::uint32_t> codepoints,
        std::uint32_t pixelHeight = Common::UNICODE_FONT_RASTER_HEIGHT);
};

}  // namespace MMM::Graphic
