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
}  // namespace

/// @brief 运行批注详情卡片布局回归测试。
/// @return 全部布局断言通过时返回 0。
int main()
{
    return testCardsAvoidOverlap() && testCardsShiftAwayFromBottom() ? 0 : 1;
}
