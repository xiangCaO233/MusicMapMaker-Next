#include "canvas/AnnotationDetailLayout.h"

#include <array>
#include <cmath>

namespace
{
/// @brief 判断两个布局坐标是否近似相等。
/// @param lhs 待比较的左值。
/// @param rhs 待比较的右值。
/// @return 误差小于布局测试容差时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 0.001F;
}

/// @brief 验证相近批注会按顺序错开且整体留在可用区域内。
/// @return 三张卡片保持间距且未越界时返回 true。
bool testCardsAvoidOverlap()
{
    // 前两张卡片的期望中心刻意重叠，第三张靠近区域底部。
    std::array<MMM::Canvas::AnnotationDetailCardPlacement, 3> cards{
        MMM::Canvas::AnnotationDetailCardPlacement{ 25.0F, 30.0F, 0.0F },
        MMM::Canvas::AnnotationDetailCardPlacement{ 30.0F, 30.0F, 0.0F },
        MMM::Canvas::AnnotationDetailCardPlacement{ 95.0F, 30.0F, 0.0F },
    };
    // 在 120 像素区域内要求相邻卡片保留 4 像素间隔。
    MMM::Canvas::layoutAnnotationDetailCards(cards, 0.0F, 120.0F, 4.0F);

    // 同时检查顶部、逐项间距和底部，覆盖正向与反向两次扫描结果。
    return cards.front().topY >= 0.0F &&
           cards[1].topY >= cards[0].topY + cards[0].height + 4.0F &&
           cards[2].topY >= cards[1].topY + cards[1].height + 4.0F &&
           cards.back().topY + cards.back().height <= 120.0F;
}

/// @brief 验证底部拥挤时会整体上移而不破坏卡片间距。
/// @return 末张卡片贴底且首张卡片仍在区域内时返回 true。
bool testCardsShiftAwayFromBottom()
{
    // 两个期望中心都靠近底部，正向布局必然产生下边界溢出。
    std::array<MMM::Canvas::AnnotationDetailCardPlacement, 2> cards{
        MMM::Canvas::AnnotationDetailCardPlacement{ 90.0F, 30.0F, 0.0F },
        MMM::Canvas::AnnotationDetailCardPlacement{ 100.0F, 30.0F, 0.0F },
    };
    // 反向收紧应把最后一张贴到 110，并保持 5 像素间隔。
    MMM::Canvas::layoutAnnotationDetailCards(cards, 10.0F, 110.0F, 5.0F);

    // 三项断言分别约束贴底位置、卡片间距以及上边界。
    return near(cards[1].topY + cards[1].height, 110.0F) &&
           cards[1].topY >= cards[0].topY + cards[0].height + 5.0F &&
           cards[0].topY >= 10.0F;
}

/// @brief 验证滚轮只浏览长文，不再负责切换批注。
/// @return 可滚动时消费输入、到达边界时不消费输入则返回 true。
bool testWheelOnlyScrollsMarkdown()
{
    // 向下滚动一格应按给定步长把正文偏移从零推进到 36。
    const auto scrolled =
        MMM::Canvas::updateAnnotationDetailWheel(-1.0F, 0.0F, 120.0F, 36.0F);
    const auto atBottom =
        // 已在最大偏移时同方向滚轮不能再改变状态，也不应被标记为消费。
        MMM::Canvas::updateAnnotationDetailWheel(-1.0F, 120.0F, 120.0F, 36.0F);

    // consumed 必须与坐标是否真正变化保持一致，供调用方决定事件传播。
    return scrolled.consumed && near(scrolled.scrollY, 36.0F) &&
           !atBottom.consumed && near(atBottom.scrollY, 120.0F);
}

/// @brief 验证批注栏只拦截已由详情或弹窗消费的滚轮。
/// @return 四种悬停、弹窗和消费组合均符合传递规则时返回 true。
bool testUnconsumedGutterWheelPassesToCanvas()
{
    // 未消费的批注栏滚轮传给画布，其余三种情况均不得继续传播。
    // 第一项是唯一允许传递的组合，后续分别隔离详情消费、弹窗和未悬停。
    return MMM::Canvas::shouldPassAnnotationWheelToCanvas(true, false, false) &&
           !MMM::Canvas::shouldPassAnnotationWheelToCanvas(true, false, true) &&
           !MMM::Canvas::shouldPassAnnotationWheelToCanvas(true, true, false) &&
           !MMM::Canvas::shouldPassAnnotationWheelToCanvas(false, false, false);
}

/// @brief 验证方向键选择会循环经过同一时间戳的全部批注。
/// @return 前后步进和两端环绕结果均正确时返回 true。
bool testDirectionKeysCycleItems()
{
    // 中间位置验证普通步进，两端位置验证模运算环绕。
    // 四个结果同时约束向前、向后以及首尾两个边界。
    return MMM::Canvas::stepAnnotationDetailItem(3U, 1U, -1) == 0U &&
           MMM::Canvas::stepAnnotationDetailItem(3U, 1U, 1) == 2U &&
           MMM::Canvas::stepAnnotationDetailItem(3U, 0U, -1) == 2U &&
           MMM::Canvas::stepAnnotationDetailItem(3U, 2U, 1) == 0U;
}
/// @brief 验证卡片悬停穿透、编辑弹窗拦截以及靠近时连续淡化。
/// @return 输入拦截与四个透明度采样点均符合约定时返回 true。
bool testCardInputAndProximity()
{
    using namespace MMM::Canvas;
    // 详情卡片本身允许操作画布；只有空白批注栏和编辑弹窗阻止输入。
    // 画布未悬停时透明度保持完整，不应用局部指针淡化。
    // 透明度测试使用 20 像素衰减距离，便于精确验证中点插值结果。
    return !annotationBlocksCanvas(false, true, false) &&
           !annotationBlocksCanvas(true, true, false) &&
           annotationBlocksCanvas(true, false, false) &&
           annotationBlocksCanvas(false, true, true) &&
           // 指针从卡片内部向外移动时，透明度应由 0.25 线性恢复到 1.0。
           near(annotationDetailOpacity(50, 50, 0, 0, 100, 100, true, 20),
                0.25F) &&
           near(annotationDetailOpacity(110, 50, 0, 0, 100, 100, true, 20),
                0.625F) &&
           near(annotationDetailOpacity(120, 50, 0, 0, 100, 100, true, 20),
                1.0F) &&
           near(annotationDetailOpacity(50, 50, 0, 0, 100, 100, false, 20),
                1.0F);
}
}  // namespace

/// @brief 运行批注详情卡片布局回归测试。
/// @return 全部布局断言通过时返回 0。
int main()
{
    // 每项测试无外部状态依赖，短路组合能直接映射到进程退出状态。
    return testCardInputAndProximity() && testCardsAvoidOverlap() &&
                   testCardsShiftAwayFromBottom() &&
                   testWheelOnlyScrollsMarkdown() &&
                   testUnconsumedGutterWheelPassesToCanvas() &&
                   testDirectionKeysCycleItems()
               ? 0
               : 1;
}
