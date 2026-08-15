#pragma once

#include <algorithm>
#include <cmath>
#include <span>

namespace MMM::Canvas
{

/// @brief 单张批注详情卡片的纵向布局输入与结果。
struct AnnotationDetailCardPlacement {
    /// @brief 卡片期望对齐的批注标记中心 Y 坐标。
    float preferredCenterY{ 0.0F };
    /// @brief 卡片高度。
    float height{ 0.0F };
    /// @brief 避让完成后的卡片顶部 Y 坐标。
    float topY{ 0.0F };
};

/// @brief 按既有时间顺序排列批注详情卡片并避免相邻卡片重叠。
/// @param cards 已按画布 Y 坐标从上到下排列的卡片。
/// @param regionTop 可用区域顶部。
/// @param regionBottom 可用区域底部。
/// @param gap 相邻卡片间距。
/// @warning UI 热路径：只线性扫描当前可见卡片，不分配内存或排序。
inline void layoutAnnotationDetailCards(
    std::span<AnnotationDetailCardPlacement> cards, float regionTop,
    float regionBottom, float gap)
{
    if ( cards.empty() || !std::isfinite(regionTop) ||
         !std::isfinite(regionBottom) || regionBottom <= regionTop ) {
        return;
    }

    gap          = std::max(0.0F, std::isfinite(gap) ? gap : 0.0F);
    float cursor = regionTop;
    for ( auto& card : cards ) {
        card.height =
            std::max(1.0F, std::isfinite(card.height) ? card.height : 1.0F);
        const float preferredTop =
            std::isfinite(card.preferredCenterY)
                ? card.preferredCenterY - card.height * 0.5F
                : cursor;
        card.topY = std::max(cursor, preferredTop);
        cursor    = card.topY + card.height + gap;
    }

    cursor = regionBottom;
    for ( auto iterator = cards.rbegin(); iterator != cards.rend();
          ++iterator ) {
        iterator->topY = std::min(iterator->topY, cursor - iterator->height);
        cursor         = iterator->topY - gap;
    }

    if ( cards.front().topY < regionTop ) {
        const float shift = regionTop - cards.front().topY;
        for ( auto& card : cards ) card.topY += shift;
    }
}

}  // namespace MMM::Canvas
