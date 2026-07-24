#include "canvas/TrackLayoutEditing.h"
#include "common/CanvasComponentLayout.h"

#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

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

/// @brief 验证判定线位置会被限制在画布范围内。
/// @return 有限值、越界值和非有限值均得到合法结果时返回 true。
bool testJudgmentLineConstraints()
{
    return near(MMM::Canvas::sanitizeJudgmentLinePosition(0.42f), 0.42f) &&
           near(MMM::Canvas::sanitizeJudgmentLinePosition(-2.0f), 0.0f) &&
           near(MMM::Canvas::sanitizeJudgmentLinePosition(3.0f), 1.0f) &&
           near(MMM::Canvas::sanitizeJudgmentLinePosition(
                    std::numeric_limits<float>::quiet_NaN()),
                0.85f);
}

/// @brief 验证中心把手优先于边界且四边和判定线把手均可命中。
/// @return 命中结果符合预期时返回 true。
bool testHandleHitTesting()
{
    const MMM::Config::TrackLayout layout;
    using Handle = MMM::Canvas::TrackLayoutDragHandle;
    return MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 50.0f, 50.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Move &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 20.0f, 30.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Left &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 40.0f, 5.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Top &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 80.0f, 70.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Right &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 60.0f, 95.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::Bottom &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 80.0f, 85.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::JudgmentLine &&
           MMM::Canvas::hitTestTrackLayout(
               layout, 0.85f, 2.0f, 2.0f, 100.0f, 100.0f, 4.0f, 8.0f) ==
               Handle::None;
}

/// @brief 验证组件锚点规整与边缘拖动会保持组件完整可见。
/// @return 锚点、边界和移动结果均合法时返回 true。
bool testCanvasComponentPlacement()
{
    MMM::Config::CanvasComponentPlacement invalid;
    invalid.anchorX = std::numeric_limits<float>::quiet_NaN();
    invalid.anchorY = 4.0f;
    invalid.color   = {
        std::numeric_limits<float>::quiet_NaN(), -1.0f, 2.0f, 0.5f
    };
    const auto sanitized =
        MMM::Logic::sanitizeCanvasComponentPlacement(invalid);
    if ( !near(sanitized.anchorX, 0.5f) || !near(sanitized.anchorY, 1.0f) ||
         !near(sanitized.color[0], 1.0f) || !near(sanitized.color[1], 0.0f) ||
         !near(sanitized.color[2], 1.0f) || !near(sanitized.color[3], 0.5f) ) {
        return false;
    }

    MMM::Config::CanvasComponentPlacement placement;
    placement.visible = true;
    const auto moved  = MMM::Logic::moveCanvasComponent(
        placement, -100.0f, 1000.0f, 800.0f, 600.0f, 140.0f, 27.0f);
    const auto bounds =
        MMM::Logic::canvasComponentBounds(moved, 800.0f, 600.0f, 140.0f, 27.0f);
    return near(bounds.left, 0.0f) && near(bounds.bottom, 600.0f) &&
           bounds.right <= 800.0f && bounds.top >= 0.0f &&
           bounds.contains(bounds.left, bounds.top);
}

/// @brief 验证四角包围框会等比调整字号并保持对角点。
/// @return 命中、字号和移动后的中心均符合预期时返回 true。
bool testCanvasComponentResize()
{
    MMM::Config::CanvasComponentPlacement placement;
    placement.visible       = true;
    placement.anchorX       = 0.5f;
    placement.anchorY       = 0.5f;
    placement.fontSizeRatio = 0.04f;
    const auto bounds       = MMM::Logic::canvasComponentBounds(
        placement, 800.0f, 600.0f, 200.0f, 24.0f);
    if ( MMM::Logic::hitTestCanvasComponent(
             bounds, bounds.right, bounds.bottom, 6.0f) !=
         MMM::Logic::CanvasComponentDragHandle::BottomRight ) {
        return false;
    }

    const auto resized = MMM::Logic::resizeCanvasComponent(
        placement,
        MMM::Logic::CanvasComponentDragHandle::BottomRight,
        bounds,
        bounds.left + bounds.width() * 2.0f,
        bounds.top + bounds.height() * 2.0f,
        800.0f,
        600.0f);
    return near(resized.fontSizeRatio, 0.08f) &&
           near(resized.anchorX, 0.625f) && near(resized.anchorY, 0.52f);
}

/// @brief 验证画布组件布局配置可独立完成 JSON 往返。
/// @return 显隐、锚点、字号与颜色均保持时返回 true。
bool testCanvasComponentConfigRoundTrip()
{
    MMM::Config::CanvasComponentLayoutConfig source;
    auto& placement         = source.judgmentLineTime;
    placement.visible       = true;
    placement.anchorX       = 0.23f;
    placement.anchorY       = 0.76f;
    placement.fontSizeRatio = 0.08f;
    placement.color         = { 0.1f, 0.3f, 0.7f, 0.8f };

    const nlohmann::json encoded = source;
    const auto           decoded =
        encoded.get<MMM::Config::CanvasComponentLayoutConfig>();
    const auto& restored = decoded.judgmentLineTime;
    return restored.visible && near(restored.anchorX, 0.23f) &&
           near(restored.anchorY, 0.76f) &&
           near(restored.fontSizeRatio, 0.08f) &&
           near(restored.color[0], 0.1f) && near(restored.color[1], 0.3f) &&
           near(restored.color[2], 0.7f) && near(restored.color[3], 0.8f);
}

}  // namespace

/// @brief 覆盖轨道、判定线与可选画布组件的布局编辑。
/// @return 所有检查通过时返回 0。
int main()
{
    if ( !testSanitizeInvalidLayout() ) {
        return 1;
    }
    if ( !testBoundaryConstraints() ) {
        return 2;
    }
    if ( !testMovePreservesSize() ) {
        return 3;
    }
    if ( !testJudgmentLineConstraints() ) {
        return 4;
    }
    if ( !testHandleHitTesting() ) {
        return 5;
    }
    if ( !testCanvasComponentPlacement() ) {
        return 6;
    }
    if ( !testCanvasComponentResize() ) {
        return 7;
    }
    if ( !testCanvasComponentConfigRoundTrip() ) {
        return 8;
    }
    return 0;
}
