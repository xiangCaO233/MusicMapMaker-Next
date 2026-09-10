#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace MMM::Canvas
{

/// @brief 根据指针到卡片边缘的距离连续降低透明度，卡片内保留四分之一可见度。
/// @warning UI 热路径：每张可见卡片每帧只做常量级计算，不分配内存。
inline float annotationDetailOpacity(float pointerX, float pointerY, float left,
                                     float top, float right, float bottom,
                                     bool canvasHovered, float proximity)
{
    if ( !canvasHovered ) return 1.0F;
    // 指针位于矩形投影内时两个轴距离均为零；位于外部时只累计越界轴。
    const float dx = std::max({ left - pointerX, 0.0F, pointerX - right });
    const float dy = std::max({ top - pointerY, 0.0F, pointerY - bottom });
    const float distance = std::sqrt(dx * dx + dy * dy);
    // proximity 至少为一个像素，避免配置为零时产生除零或突变。
    return 0.25F +
           0.75F * std::clamp(distance / std::max(1.0F, proximity), 0.0F, 1.0F);
}

/// @brief 卡片穿透画布输入；仅批注栏和编辑弹窗阻止画布交互。
/// @warning UI 热路径：每帧仅执行常量级布尔判断。
constexpr bool annotationBlocksCanvas(bool gutterHovered,
                                      bool detailCardHovered,
                                      bool editorPopupOpen)
{
    return (gutterHovered && !detailCardHovered) || editorPopupOpen;
}

/// @brief 单张批注详情卡片的纵向布局输入与结果。
struct AnnotationDetailCardPlacement {
    /// @brief 卡片期望对齐的批注标记中心 Y 坐标。
    float preferredCenterY{ 0.0F };
    /// @brief 卡片高度。
    float height{ 0.0F };
    /// @brief 避让完成后的卡片顶部 Y 坐标。
    float topY{ 0.0F };
};

/// @brief 批注详情滚轮处理结果。
struct AnnotationDetailWheelResult {
    /// @brief 更新后的纵向滚动偏移。
    float scrollY{ 0.0F };
    /// @brief 是否消费了本次滚轮输入。
    bool consumed{ false };
};

/// @brief 使用滚轮浏览长批注正文。
/// @param wheel ImGui 纵向滚轮增量。
/// @param scrollY 当前纵向滚动偏移。
/// @param maxScrollY 最大纵向滚动偏移。
/// @param scrollStep 单格滚轮对应的滚动距离。
/// @return 更新后的滚动状态。
/// @warning UI 热路径：每帧只执行常量级数值计算，不分配内存。
inline AnnotationDetailWheelResult updateAnnotationDetailWheel(float wheel,
                                                               float scrollY,
                                                               float maxScrollY,
                                                               float scrollStep)
{
    AnnotationDetailWheelResult result;
    // 非有限输入统一回到安全范围，避免 NaN 污染 ImGui 的滚动坐标。
    const float normalizedMaxScrollY =
        std::max(0.0F, std::isfinite(maxScrollY) ? maxScrollY : 0.0F);
    result.scrollY = std::clamp(
        std::isfinite(scrollY) ? scrollY : 0.0F, 0.0F, normalizedMaxScrollY);
    if ( std::abs(wheel) <= 0.01F ) return result;

    // 滚轮方向与内容偏移相反：向下滚动时增大正文的纵向偏移。
    const float step =
        std::max(1.0F, std::isfinite(scrollStep) ? scrollStep : 1.0F);
    const float nextScroll =
        std::clamp(result.scrollY - wheel * step, 0.0F, normalizedMaxScrollY);
    if ( std::abs(nextScroll - result.scrollY) > 0.01F ) {
        // 只有实际位置变化才消费输入；到达边界后允许画布继续处理滚轮。
        result.scrollY  = nextScroll;
        result.consumed = true;
    }
    return result;
}

/// @brief 判断批注交互层未消费的滚轮是否应继续传给画布。
/// @param annotationHovered 指针是否位于批注栏或详情卡片。
/// @param editorPopupOpen 批注编辑弹窗是否打开。
/// @param detailWheelConsumed 批注详情正文是否已消费滚轮。
/// @return 仅悬停批注交互层且没有弹窗或详情滚动消费时返回 true。
/// @warning UI 热路径：每帧只执行常量级布尔判断。
constexpr bool shouldPassAnnotationWheelToCanvas(bool annotationHovered,
                                                 bool editorPopupOpen,
                                                 bool detailWheelConsumed)
{
    return annotationHovered && !editorPopupOpen && !detailWheelConsumed;
}

/// @brief 按方向键方向循环切换同一时间戳上的批注。
/// @param itemCount 同一时间戳上的批注数量。
/// @param itemIndex 当前批注索引。
/// @param direction 小于零选择上一条，大于零选择下一条。
/// @return 切换后的批注索引；没有可切换项时返回安全索引。
/// @warning UI 热路径：每帧只执行常量级整数计算。
inline std::size_t stepAnnotationDetailItem(std::size_t itemCount,
                                            std::size_t itemIndex,
                                            int         direction)
{
    if ( itemCount == 0U ) return 0U;
    // 数据刷新可能缩短同时间戳列表，先把旧索引收敛到新范围内。
    itemIndex = std::min(itemIndex, itemCount - 1U);
    if ( direction < 0 ) return (itemIndex + itemCount - 1U) % itemCount;
    if ( direction > 0 ) return (itemIndex + 1U) % itemCount;
    return itemIndex;
}

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
    // 无效区域保持调用方原值，避免对尚未完成布局的画布产生伪坐标。
    if ( cards.empty() || !std::isfinite(regionTop) ||
         !std::isfinite(regionBottom) || regionBottom <= regionTop ) {
        return;
    }

    gap          = std::max(0.0F, std::isfinite(gap) ? gap : 0.0F);
    float cursor = regionTop;
    // 正向扫描优先保持时间顺序，并把每张卡片推到上一张卡片之后。
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
    // 反向扫描把底部溢出的卡片向上收紧，同时继续维持相邻间距。
    for ( auto iterator = cards.rbegin(); iterator != cards.rend();
          ++iterator ) {
        iterator->topY = std::min(iterator->topY, cursor - iterator->height);
        cursor         = iterator->topY - gap;
    }

    if ( cards.front().topY < regionTop ) {
        // 卡片总高超出区域时只能整体贴顶；相对次序和间距仍保持不变。
        const float shift = regionTop - cards.front().topY;
        for ( auto& card : cards ) card.topY += shift;
    }
}

}  // namespace MMM::Canvas
