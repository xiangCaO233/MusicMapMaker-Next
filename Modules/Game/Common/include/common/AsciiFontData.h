#pragma once

#include "common/FontConstants.h"
#include <array>
#include <cstddef>

namespace MMM::Common
{

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
    /// @brief 逻辑像素字号转换为字体图集物理栅格字号的倍率。
    float rasterScale{ 1.0F };
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

/// @brief 纯 ASCII 文本的像素尺寸。
struct AsciiTextSize {
    /// @brief 横向推进总宽度。
    float width{ 0.0f };
    /// @brief 字体单行高度。
    float height{ 0.0f };
};

}  // namespace MMM::Common
