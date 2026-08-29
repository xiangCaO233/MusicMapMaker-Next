#include "canvas/ObjectDragAutoPan.h"

#include <cmath>

namespace
{

/// @brief 使用小容差比较自动平移逻辑像素。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 验证默认边缘自动平移速度已降低且仍保留渐进响应。
/// @return 边缘速度与中心静止行为符合预期时返回 true。
bool testReducedEdgeSpeed()
{
    constexpr float extent    = 1000.0F;
    const float     leftDelta = MMM::Canvas::objectDragAutoPanAxisDelta(
        0.0F, extent, 1.0F / 60.0F, 1.0F);
    const float rightDelta = MMM::Canvas::objectDragAutoPanAxisDelta(
        extent, extent, 1.0F / 60.0F, 1.0F);
    const float centerDelta = MMM::Canvas::objectDragAutoPanAxisDelta(
        extent * 0.5F, extent, 1.0F / 60.0F, 1.0F);
    return near(leftDelta, 9.0F) && near(rightDelta, -9.0F) &&
           near(centerDelta, 0.0F);
}

/// @brief 验证横向自动平移止于草稿区和 BGM 区最外侧轨道边缘。
/// @return 两侧边缘均保留固定留白，已完整可见方向不再移动时返回 true。
bool testHorizontalTrackAreaBounds()
{
    const auto centered = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 2, 0.3F, 0.7F, 0.0F, true, true, true);
    if ( !centered.valid || !near(centered.player.singleTrackWidth, 100.0F) ||
         centered.draftLaneCount != 4U || centered.bgmLaneCount != 3U ||
         !near(centered.draftLeftX, -100.0F) ||
         !near(centered.bgmRightX, 1026.0F) ) {
        return false;
    }

    const float towardDraft =
        MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
            200.0F, 1000.0F, centered);
    const float towardBgm = MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
        -200.0F, 1000.0F, centered);
    if ( !near(towardDraft, 148.0F) || !near(towardBgm, -74.0F) ) {
        return false;
    }

    const auto draftAtBoundary = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 2, 0.3F, 0.7F, 148.0F, true, true, true);
    const auto bgmAtBoundary = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 2, 0.3F, 0.7F, -74.0F, true, true, true);
    return near(draftAtBoundary.draftLeftX, 48.0F) &&
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    20.0F, 1000.0F, draftAtBoundary),
                0.0F) &&
           near(bgmAtBoundary.bgmRightX, 952.0F) &&
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    -20.0F, 1000.0F, bgmAtBoundary),
                0.0F);
}

/// @brief 验证任一侧轨道已完整可见时不会继续滚入空白区。
/// @return 两侧继续越界均被阻止，返回轨道区方向仍可移动时返回 true。
bool testAlreadyVisibleSideDoesNotOverscroll()
{
    const auto draftVisible = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 2, 0.3F, 0.7F, 200.0F, true, true, true);
    const auto bgmVisible = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 2, 0.3F, 0.7F, -200.0F, true, true, true);
    return draftVisible.valid && draftVisible.draftLeftX > 48.0F &&
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    20.0F, 1000.0F, draftVisible),
                0.0F) &&
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    -20.0F, 1000.0F, draftVisible),
                -20.0F) &&
           bgmVisible.valid && bgmVisible.bgmRightX < 952.0F &&
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    -20.0F, 1000.0F, bgmVisible),
                0.0F) &&
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    20.0F, 1000.0F, bgmVisible),
                20.0F);
}

/// @brief 验证隐藏侧边轨道时以玩家区最外侧轨道作为边界。
/// @return 无草稿和 BGM 轨道时不会把玩家轨道自动滚入更大空白区。
bool testHiddenSideAreasFallBackToPlayerEdges()
{
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 0, 0.3F, 0.7F, 0.0F, true, false, false);
    return projection.valid && projection.draftLaneCount == 0U &&
           projection.bgmLaneCount == 0U &&
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    20.0F, 1000.0F, projection),
                0.0F) &&
           near(MMM::Canvas::clampObjectDragHorizontalAutoPanDelta(
                    -20.0F, 1000.0F, projection),
                0.0F);
}

}  // namespace

/// @brief 运行物件拖拽边缘自动平移回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testReducedEdgeSpeed() && testHorizontalTrackAreaBounds() &&
                   testAlreadyVisibleSideDoesNotOverscroll() &&
                   testHiddenSideAreasFallBackToPlayerEdges()
               ? 0
               : 1;
}
