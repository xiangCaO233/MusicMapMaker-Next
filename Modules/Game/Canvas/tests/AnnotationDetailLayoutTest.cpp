#include "canvas/AnnotationDetailLayout.h"

#include <array>
#include <cmath>

namespace
{
/// @brief 判断两个布局坐标是否近似相等。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 0.001F;
}

/// @brief 验证相近批注会按顺序错开且整体留在可用区域内。
bool testCardsAvoidOverlap()
{
    std::array<MMM::Canvas::AnnotationDetailCardPlacement, 3> cards{
        MMM::Canvas::AnnotationDetailCardPlacement{ 25.0F, 30.0F, 0.0F },
        MMM::Canvas::AnnotationDetailCardPlacement{ 30.0F, 30.0F, 0.0F },
        MMM::Canvas::AnnotationDetailCardPlacement{ 95.0F, 30.0F, 0.0F },
    };
    MMM::Canvas::layoutAnnotationDetailCards(cards, 0.0F, 120.0F, 4.0F);

    return cards.front().topY >= 0.0F &&
           cards[1].topY >= cards[0].topY + cards[0].height + 4.0F &&
           cards[2].topY >= cards[1].topY + cards[1].height + 4.0F &&
           cards.back().topY + cards.back().height <= 120.0F;
}

/// @brief 验证底部拥挤时会整体上移而不破坏卡片间距。
bool testCardsShiftAwayFromBottom()
{
    std::array<MMM::Canvas::AnnotationDetailCardPlacement, 2> cards{
        MMM::Canvas::AnnotationDetailCardPlacement{ 90.0F, 30.0F, 0.0F },
        MMM::Canvas::AnnotationDetailCardPlacement{ 100.0F, 30.0F, 0.0F },
    };
    MMM::Canvas::layoutAnnotationDetailCards(cards, 10.0F, 110.0F, 5.0F);

    return near(cards[1].topY + cards[1].height, 110.0F) &&
           cards[1].topY >= cards[0].topY + cards[0].height + 5.0F &&
           cards[0].topY >= 10.0F;
}

/// @brief 验证滚轮只浏览长文，不再负责切换批注。
bool testWheelOnlyScrollsMarkdown()
{
    const auto scrolled =
        MMM::Canvas::updateAnnotationDetailWheel(-1.0F, 0.0F, 120.0F, 36.0F);
    const auto atBottom =
        MMM::Canvas::updateAnnotationDetailWheel(-1.0F, 120.0F, 120.0F, 36.0F);

    return scrolled.consumed && near(scrolled.scrollY, 36.0F) &&
           !atBottom.consumed && near(atBottom.scrollY, 120.0F);
}

/// @brief 验证方向键选择会循环经过同一时间戳的全部批注。
bool testDirectionKeysCycleItems()
{
    return MMM::Canvas::stepAnnotationDetailItem(3U, 1U, -1) == 0U &&
           MMM::Canvas::stepAnnotationDetailItem(3U, 1U, 1) == 2U &&
           MMM::Canvas::stepAnnotationDetailItem(3U, 0U, -1) == 2U &&
           MMM::Canvas::stepAnnotationDetailItem(3U, 2U, 1) == 0U;
}
}  // namespace

/// @brief 运行批注详情卡片布局回归测试。
/// @return 全部布局断言通过时返回 0。
int main()
{
    return testCardsAvoidOverlap() && testCardsShiftAwayFromBottom() &&
                   testWheelOnlyScrollsMarkdown() &&
                   testDirectionKeysCycleItems()
               ? 0
               : 1;
}
