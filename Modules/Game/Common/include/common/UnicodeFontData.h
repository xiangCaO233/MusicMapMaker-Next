#pragma once

#include "common/AsciiFontData.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace MMM::Common
{

/// @brief 独立画布 Unicode 字形的固定栅格高度。
/// @note 所有字形度量都相对此高度归一化，避免快照携带字体像素尺寸。
inline constexpr std::uint32_t UNICODE_FONT_RASTER_HEIGHT = 32U;

/// @brief Unicode 替换字符码点。
/// @note 无效 UTF-8 序列统一解析为 U+FFFD，保证扫描始终向前推进。
inline constexpr std::uint32_t UNICODE_REPLACEMENT_CODEPOINT = 0xFFFDU;

/// @brief 单个 Unicode 字形及其归一化度量。
/// @note glyphs 容器按 codepoint 排序后可直接对该结构执行二分查找。
struct UnicodeGlyphMetrics {
    /// @brief Unicode 码点。
    std::uint32_t codepoint{ 0U };

    /// @brief 相对固定栅格高度的字形度量。
    AsciiGlyphMetrics metrics;
};

/// @brief 独立画布按需加载的 Unicode 字体度量。
struct UnicodeFontMetrics {
    /// @brief 字体资源是否有效。
    bool valid{ false };

    /// @brief 基线以上高度比例。
    float ascender{ 0.0F };

    /// @brief 单行高度比例。
    float lineHeight{ 1.0F };

    /// @brief 按码点升序排列的已加载字形。
    std::vector<UnicodeGlyphMetrics> glyphs;

    /// @brief 按 Unicode 码点查找字形度量。
    /// @param codepoint 待查找码点。
    /// @return 已加载时返回字形度量，否则返回 nullptr。
    /// @warning 主画布文本热路径：仅执行二分查找，不得修改字形集合。
    [[nodiscard]] const AsciiGlyphMetrics* glyph(std::uint32_t codepoint) const
    {
        // 排序不变量允许 lower_bound 在对数时间定位首个不小于目标的条目。
        const auto iterator = std::lower_bound(
            glyphs.begin(),
            glyphs.end(),
            codepoint,
            [](const UnicodeGlyphMetrics& entry, std::uint32_t value) {
                // 比较器只读取码点，字形纹理与度量不参与排序。
                return entry.codepoint < value;
            });
        // lower_bound 可能指向下一个更大码点，返回前必须再次验证相等。
        return iterator != glyphs.end() && iterator->codepoint == codepoint
                   ? &iterator->metrics
                   : nullptr;
    }
};

/// @brief 判断码点是否属于合法 Unicode 标量值。
/// @param codepoint 待判断码点。
/// @return 合法且不属于代理项范围时返回 true。
[[nodiscard]] inline constexpr bool isValidUnicodeCodepoint(
    std::uint32_t codepoint)
{
    // Unicode 上限以外及 UTF-16 代理项范围都不是合法标量值。
    return codepoint <= 0x10FFFFU &&
           !(codepoint >= 0xD800U && codepoint <= 0xDFFFU);
}

/// @brief 从 UTF-8 文本解码下一个 Unicode 码点。
/// @param text UTF-8 文本。
/// @param offset 输入与输出字节偏移；无效序列至少前进一个字节。
/// @return 解码码点；无效序列返回替换字符。
/// @warning 主画布文本热路径：只读取至多四个字节，不分配内存。
[[nodiscard]] inline std::uint32_t decodeNextUtf8Codepoint(
    std::string_view text, std::size_t& offset)
{
    // 到达文本末尾返回零且不移动偏移，调用循环以 size 条件终止。
    if ( offset >= text.size() ) return 0U;

    const auto first = static_cast<unsigned char>(text[offset]);
    // ASCII 单字节路径直接返回，避免进入多字节状态机。
    if ( first < 0x80U ) {
        ++offset;
        return first;
    }

    std::size_t   sequenceLength = 0U;
    std::uint32_t codepoint      = 0U;
    std::uint32_t minimumValue   = 0U;
    // 首字节范围同时决定序列长度、初始有效位和防过长编码的最小值。
    if ( first >= 0xC2U && first <= 0xDFU ) {
        sequenceLength = 2U;
        codepoint      = first & 0x1FU;
        minimumValue   = 0x80U;
    } else if ( first >= 0xE0U && first <= 0xEFU ) {
        sequenceLength = 3U;
        codepoint      = first & 0x0FU;
        minimumValue   = 0x800U;
    } else if ( first >= 0xF0U && first <= 0xF4U ) {
        sequenceLength = 4U;
        codepoint      = first & 0x07U;
        minimumValue   = 0x10000U;
    } else {
        // 非法首字节只消费一个字节，使后续有效序列仍有机会恢复解析。
        ++offset;
        return UNICODE_REPLACEMENT_CODEPOINT;
    }

    if ( text.size() - offset < sequenceLength ) {
        // 截断序列同样只前进一个字节，维持“每次至少推进一字节”的契约。
        ++offset;
        return UNICODE_REPLACEMENT_CODEPOINT;
    }
    for ( std::size_t index = 1U; index < sequenceLength; ++index ) {
        // 每个后续字节必须以二进制 10 开头，否则当前序列立即失败。
        const auto continuation =
            static_cast<unsigned char>(text[offset + index]);
        if ( (continuation & 0xC0U) != 0x80U ) {
            // 偏移只跳过首字节，异常后续字节由下一次调用重新解释。
            ++offset;
            return UNICODE_REPLACEMENT_CODEPOINT;
        }
        // 累积六位有效载荷，最多执行三次且不会超出 uint32_t。
        codepoint = (codepoint << 6U) | (continuation & 0x3FU);
    }

    offset += sequenceLength;
    // 最小值排除过长编码，标量检查排除代理项和 Unicode 上限外值。
    if ( codepoint < minimumValue || !isValidUnicodeCodepoint(codepoint) ) {
        return UNICODE_REPLACEMENT_CODEPOINT;
    }
    return codepoint;
}

/// @brief 收集 UTF-8 文本中的非 ASCII Unicode 码点。
/// @param target 接收码点的低频工作缓冲区。
/// @param text 待扫描 UTF-8 文本。
/// @warning 字体图集重载路径：允许追加并在调用方统一排序去重，禁止在每帧调用。
inline void appendNonAsciiCodepoints(std::vector<std::uint32_t>& target,
                                     std::string_view            text)
{
    std::size_t offset = 0U;
    // 解码器保证有效输入和无效输入都推进，循环不会停留在坏字节上。
    while ( offset < text.size() ) {
        const auto codepoint = decodeNextUtf8Codepoint(text, offset);
        // ASCII 已由固定字形集覆盖，仅收集需要动态载入的非 ASCII 码点。
        if ( codepoint > ASCII_GLYPH_LAST ) {
            target.push_back(codepoint);
        }
    }
}

}  // namespace MMM::Common
