#pragma once

#include "common/AsciiFontData.h"
#include "common/FontConstants.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace MMM::Common
{

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

}  // namespace MMM::Common
