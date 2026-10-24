#pragma once

#include "common/FontConstants.h"
#include "common/UnicodeCodepoint.h"
#include "common/render/TextureID.h"
#include <cstddef>
#include <cstdint>

namespace MMM::Common::Render
{
/// @note 只依赖字形范围常量，不引入字体度量容器或整帧快照。
/// @warning 渲染热路径：保持 constexpr 内联，不分配、不查询图集。
/// @brief 将 ASCII 字号档位与字符转换为字体图集纹理 ID。
/// @param tierIndex 字号档位索引。
/// @param character ASCII 字符。
/// @return 字符对应纹理 ID；范围外返回 `TextureID::None`。
[[nodiscard]] inline constexpr TextureID asciiGlyphTextureId(
    std::size_t tierIndex, char character)
{
    // 先转换为无符号字节，避免实现相关的负 char 破坏范围判断。
    const auto code = static_cast<unsigned char>(character);
    // 字号档位或字符超出固定 ASCII 图集时显式返回无纹理。
    if ( tierIndex >= Common::ASCII_FONT_RASTER_TIER_COUNT ||
         code < Common::ASCII_GLYPH_FIRST || code > Common::ASCII_GLYPH_LAST ) {
        return TextureID::None;
    }
    // 每个字号档位占用连续区间，字符码点提供区间内偏移。
    return static_cast<TextureID>(
        static_cast<std::uint32_t>(TextureID::AsciiGlyphStart) +
        tierIndex * Common::ASCII_GLYPH_COUNT + code -
        Common::ASCII_GLYPH_FIRST);
}

/// @warning 按需字体请求与渲染路径共用，编号计算不得引入可变状态。
/// @brief 将 Unicode 码点转换为字体图集纹理 ID。
/// @param codepoint 合法且非 ASCII 的 Unicode 码点。
/// @return 字符对应纹理 ID；范围外返回 `TextureID::None`。
[[nodiscard]] inline constexpr TextureID unicodeGlyphTextureId(
    std::uint32_t codepoint)
{
    // ASCII 使用独立多档图集，非法 Unicode 标量也不得占用纹理编号。
    if ( codepoint <= Common::ASCII_GLYPH_LAST ||
         !Common::isValidUnicodeCodepoint(codepoint) ) {
        return TextureID::None;
    }
    // Unicode 高位保留区直接叠加码点，保证同一码点得到稳定 ID。
    return static_cast<TextureID>(
        static_cast<std::uint32_t>(TextureID::UnicodeGlyphStart) + codepoint);
}

}  // namespace MMM::Common::Render
