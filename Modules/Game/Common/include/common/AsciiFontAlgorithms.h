#pragma once

#include "common/AsciiFontData.h"
#include <cmath>
#include <cstddef>
#include <limits>

namespace MMM::Common
{

/// @brief 为目标逻辑像素字号选择最接近的 FreeType 物理栅格档位。
/// @param atlas 多档 ASCII 字体度量。
/// @param fontPixelHeight 目标字号逻辑像素高度。
/// @return 最接近且有效的字体档位。
/// @warning 热路径：组件绘制和布局命中时调用；只遍历固定十一个档位。
[[nodiscard]] inline AsciiFontSelection selectAsciiFont(
    const AsciiFontAtlasMetrics& atlas, float fontPixelHeight)
{
    // 无有效图集或字号时返回空观察结果，不选择任意默认字形。
    // 非有限字号无法参与距离比较，必须在循环前拒绝。
    if ( !atlas.valid || !std::isfinite(fontPixelHeight) ||
         fontPixelHeight <= 0.0f ) {
        return {};
    }

    // 结果借用 atlas 内的度量，调用方必须保证图集在使用期间存活。
    AsciiFontSelection selection;
    float              bestDistance = std::numeric_limits<float>::max();
    // 无效缩放回退到一倍，保持历史逻辑像素到栅格像素的换算规则。
    const float rasterScale =
        std::isfinite(atlas.rasterScale) && atlas.rasterScale > 0.0F
            ? atlas.rasterScale
            : 1.0F;
    // 在物理像素域比较距离，避免高 DPI 下选择偏小的提示档位。
    const float targetRasterHeight = fontPixelHeight * rasterScale;
    for ( std::size_t tierIndex = 0U; tierIndex < ASCII_FONT_RASTER_TIER_COUNT;
          ++tierIndex ) {
        const auto& metrics = atlas.tiers[tierIndex];
        // 缺失的栅格档位不参与候选，不能返回未初始化的字形表。
        if ( !metrics.valid ) continue;

        const float distance =
            std::abs(targetRasterHeight -
                     static_cast<float>(ASCII_FONT_RASTER_HEIGHTS[tierIndex]));
        // 只接受严格更小距离；等距时保留先遍历的较小档位。
        if ( distance < bestDistance ) {
            bestDistance        = distance;
            selection.metrics   = &metrics;
            selection.tierIndex = tierIndex;
        }
    }
    // 若所有档位都无效，保留初始空指针与零索引。
    return selection;
}

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
    // 无字体、空指针或非正字号均沿用零尺寸返回值。
    if ( !font.valid || !text || fontPixelHeight <= 0.0f ) {
        return result;
    }

    // 按空字符终止的输入只计算单行横向推进，不执行换行或整形。
    for ( const char* cursor = text; *cursor != '\0'; ++cursor ) {
        const auto* glyphMetrics = font.glyph(*cursor);
        // 超出 ASCII 范围或不可用字形保持跳过，不引入替代字符宽度。
        if ( glyphMetrics && glyphMetrics->available ) {
            result.width += glyphMetrics->advanceX * fontPixelHeight;
        }
    }
    // 行高取字体统一度量，与某个字形的位图边界无关。
    result.height = font.lineHeight * fontPixelHeight;
    return result;
}

}  // namespace MMM::Common
