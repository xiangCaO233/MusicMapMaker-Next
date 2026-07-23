#include "canvas/TrackLayoutEditing.h"

#include <cmath>
#include <limits>

namespace
{

/// @brief 判断两个布局浮点值是否足够接近。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 差值小于测试容差时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-6f;
}

/// @brief 判断布局是否满足轨道边界的全部不变量。
/// @param layout 待检查布局。
/// @return 四边范围、顺序和最小跨度均合法时返回 true。
bool isLegal(const MMM::Config::TrackLayout& layout)
{
    return layout.left >= 0.0f && layout.top >= 0.0f && layout.right <= 1.0f &&
           layout.bottom <= 1.0f &&
           layout.right - layout.left >=
               MMM::Canvas::TRACK_LAYOUT_MIN_SPAN - 1e-6f &&
           layout.bottom - layout.top >=
               MMM::Canvas::TRACK_LAYOUT_MIN_SPAN - 1e-6f;
}

/// @brief 验证越界和非有限配置会被规整为合法布局。
/// @return 规整结果合法时返回 true。
bool testSanitizeInvalidLayout()
{
    MMM::Config::TrackLayout layout;
    layout.left   = -2.0f;
    layout.top    = std::numeric_limits<float>::quiet_NaN();
    layout.right  = -1.0f;
    layout.bottom = 4.0f;
    return isLegal(MMM::Canvas::sanitizeTrackLayout(layout));
}

/// @brief 验证四条边在越过相邻边界或画布边缘时均会被限制。
/// @return 所有边界拖动结果保持合法时返回 true。
bool testBoundaryConstraints()
{
    const MMM::Config::TrackLayout start;
    const auto                     left = MMM::Canvas::resizeTrackLayout(
        start, MMM::Canvas::TrackLayoutDragHandle::Left, 0.95f);
    const auto top = MMM::Canvas::resizeTrackLayout(
        start, MMM::Canvas::TrackLayoutDragHandle::Top, 2.0f);
    const auto right = MMM::Canvas::resizeTrackLayout(
        start, MMM::Canvas::TrackLayoutDragHandle::Right, -1.0f);
    const auto bottom = MMM::Canvas::resizeTrackLayout(
        start, MMM::Canvas::TrackLayoutDragHandle::Bottom, -1.0f);
    return isLegal(left) && isLegal(top) && isLegal(right) && isLegal(bottom) &&
           near(left.right - left.left, MMM::Canvas::TRACK_LAYOUT_MIN_SPAN) &&
           near(top.bottom - top.top, MMM::Canvas::TRACK_LAYOUT_MIN_SPAN) &&
           near(right.right - right.left, MMM::Canvas::TRACK_LAYOUT_MIN_SPAN) &&
           near(bottom.bottom - bottom.top, MMM::Canvas::TRACK_LAYOUT_MIN_SPAN);
}

/// @brief 验证整体平移在触及四周时仍保持原始宽高。
/// @return 平移结果合法且尺寸不变时返回 true。
bool testMovePreservesSize()
{
    MMM::Config::TrackLayout start;
    start.left              = 0.2f;
    start.top               = 0.1f;
    start.right             = 0.7f;
    start.bottom            = 0.8f;
    const auto movedTopLeft = MMM::Canvas::moveTrackLayout(start, -3.0f, -2.0f);
    const auto movedBottomRight =
        MMM::Canvas::moveTrackLayout(start, 4.0f, 5.0f);

    return isLegal(movedTopLeft) && isLegal(movedBottomRight) &&
           near(movedTopLeft.left, 0.0f) && near(movedTopLeft.top, 0.0f) &&
           near(movedBottomRight.right, 1.0f) &&
           near(movedBottomRight.bottom, 1.0f) &&
           near(movedTopLeft.right - movedTopLeft.left, 0.5f) &&
           near(movedTopLeft.bottom - movedTopLeft.top, 0.7f) &&
           near(movedBottomRight.right - movedBottomRight.left, 0.5f) &&
           near(movedBottomRight.bottom - movedBottomRight.top, 0.7f);
}

/// @brief 验证中心把手优先于边界且四边均可命中。
/// @return 命中结果符合预期时返回 true。
bool testHandleHitTesting()
{
    const MMM::Config::TrackLayout layout;
    using Handle = MMM::Canvas::TrackLayoutDragHandle;
    return MMM::Canvas::hitTestTrackLayout(
               layout, 50.0f, 50.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Move &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 20.0f, 30.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Left &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 40.0f, 5.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Top &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 80.0f, 70.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Right &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 60.0f, 95.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Bottom &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 2.0f, 2.0f, 100.0f, 100.0f, 4.0f, 8.0f) == Handle::None;
}

}  // namespace

/// @brief 覆盖轨道布局边界调整、整体平移和命中测试。
/// @return 所有检查通过时返回 0。
int main()
{
    return testSanitizeInvalidLayout() && testBoundaryConstraints() &&
                   testMovePreservesSize() && testHandleHitTesting()
               ? 0
               : 1;
}
