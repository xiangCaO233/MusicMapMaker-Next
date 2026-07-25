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

/// @brief 验证同步字号缩放会让其他组件沿用当前把手的固定对角点。
/// @return 其他组件字号变化后左上角保持不动且中心随尺寸移动时返回 true。
bool testSynchronizedCanvasComponentResize()
{
    const MMM::Logic::CanvasComponentBounds region{
        0.0f, 0.0f, 1024.0f, 512.0f
    };
    MMM::Config::CanvasComponentPlacement placement;
    placement.visible       = true;
    placement.anchorX       = 0.375f;
    placement.anchorY       = 0.5f;
    placement.fontSizeRatio = 0.03125f;
    const auto startBounds  = MMM::Logic::canvasComponentBoundsInRegion(
        placement, region, 160.0f, 32.0f);
    const auto resized = MMM::Logic::resizeCanvasComponentToFontSizeInRegion(
        placement,
        MMM::Logic::CanvasComponentDragHandle::BottomRight,
        startBounds,
        0.0625f,
        region);
    const auto resizedBounds = MMM::Logic::canvasComponentBoundsInRegion(
        resized, region, 320.0f, 64.0f);
    const float startCenterX = (startBounds.left + startBounds.right) * 0.5f;
    const float resizedCenterX =
        (resizedBounds.left + resizedBounds.right) * 0.5f;
    return near(resized.fontSizeRatio, 0.0625f) &&
           near(resizedBounds.left, startBounds.left) &&
           near(resizedBounds.top, startBounds.top) &&
           !near(resizedCenterX, startCenterX);
}

/// @brief 验证同步移动会给其他组件应用相同位移并保留相对间距。
/// @return 两个组件移动后的中心差与移动前一致时返回 true。
bool testSynchronizedCanvasComponentMove()
{
    const MMM::Logic::CanvasComponentBounds region{
        0.0f, 0.0f, 1024.0f, 512.0f
    };
    MMM::Config::CanvasComponentPlacement first;
    first.visible                                = true;
    first.anchorX                                = 0.25f;
    first.anchorY                                = 0.5f;
    MMM::Config::CanvasComponentPlacement second = first;
    second.anchorX                               = 0.625f;
    const auto firstBounds =
        MMM::Logic::canvasComponentBoundsInRegion(first, region, 160.0f, 32.0f);
    const auto secondBounds = MMM::Logic::canvasComponentBoundsInRegion(
        second, region, 240.0f, 32.0f);

    constexpr float offsetX = 64.0f;
    constexpr float offsetY = 32.0f;
    const auto movedFirst   = MMM::Logic::moveCanvasComponentByOffsetInRegion(
        first, firstBounds, offsetX, offsetY, region);
    const auto movedSecond = MMM::Logic::moveCanvasComponentByOffsetInRegion(
        second, secondBounds, offsetX, offsetY, region);
    const auto movedFirstBounds = MMM::Logic::canvasComponentBoundsInRegion(
        movedFirst, region, 160.0f, 32.0f);
    const auto movedSecondBounds = MMM::Logic::canvasComponentBoundsInRegion(
        movedSecond, region, 240.0f, 32.0f);
    const float startCenterDistance =
        (secondBounds.left + secondBounds.right) * 0.5f -
        (firstBounds.left + firstBounds.right) * 0.5f;
    const float movedCenterDistance =
        (movedSecondBounds.left + movedSecondBounds.right) * 0.5f -
        (movedFirstBounds.left + movedFirstBounds.right) * 0.5f;
    return near(movedFirst.anchorX, 0.3125f) &&
           near(movedFirst.anchorY, 0.5625f) &&
           near(movedSecond.anchorX, 0.6875f) &&
           near(movedSecond.anchorY, 0.5625f) &&
           near(movedCenterDistance, startCenterDistance);
}

/// @brief 验证拍内组件移动和缩放不会越过所属整拍的垂直边界。
/// @return 垂直范围受整拍限制且横向仍可使用完整画布时返回 true。
bool testBeatRelativeComponentConstraints()
{
    const MMM::Logic::CanvasComponentBounds beatRegion{
        0.0f, 100.0f, 800.0f, 300.0f
    };
    MMM::Config::CanvasComponentPlacement placement =
        MMM::Config::DEFAULT_BEAT_NUMBER_PLACEMENT;
    placement.visible       = true;
    placement.anchorX       = 0.5f;
    placement.anchorY       = 0.5f;
    placement.fontSizeRatio = 0.1f;

    const auto moved = MMM::Logic::moveCanvasComponentInRegion(
        placement, 900.0f, 500.0f, beatRegion, 120.0f, 40.0f);
    const auto movedBounds = MMM::Logic::canvasComponentBoundsInRegion(
        moved, beatRegion, 120.0f, 40.0f);
    if ( !near(movedBounds.right, 800.0f) ||
         !near(movedBounds.bottom, 300.0f) ||
         movedBounds.left < beatRegion.left ||
         movedBounds.top < beatRegion.top ) {
        return false;
    }

    const auto startBounds = MMM::Logic::canvasComponentBoundsInRegion(
        placement, beatRegion, 200.0f, 20.0f);
    const auto resized = MMM::Logic::resizeCanvasComponentInRegion(
        placement,
        MMM::Logic::CanvasComponentDragHandle::BottomRight,
        startBounds,
        1200.0f,
        900.0f,
        beatRegion);
    const auto resizedBounds = MMM::Logic::canvasComponentBoundsInRegion(
        resized,
        beatRegion,
        startBounds.width() * (resized.fontSizeRatio / placement.fontSizeRatio),
        startBounds.height() *
            (resized.fontSizeRatio / placement.fontSizeRatio));
    return resizedBounds.left >= beatRegion.left &&
           resizedBounds.right <= beatRegion.right &&
           resizedBounds.top >= beatRegion.top &&
           resizedBounds.bottom <= beatRegion.bottom;
}

/// @brief 验证所有画布组件均可仅复位位置和尺寸。
/// @return 默认几何恢复、显示属性保留且 KPS 逐轨覆盖被清除时返回 true。
bool testCanvasComponentPlacementReset()
{
    MMM::Config::CanvasComponentLayoutConfig config;
    const auto                               verifyReset =
        [&](MMM::Config::CanvasComponentType             type,
            const MMM::Config::CanvasComponentPlacement& expected) {
            auto& placement         = config.placement(type);
            placement.visible       = true;
            placement.anchorX       = 0.91f;
            placement.anchorY       = 0.83f;
            placement.fontSizeRatio = 0.21f;
            placement.color         = { 0.2f, 0.3f, 0.4f, 0.5f };

            if ( type == MMM::Config::CanvasComponentType::Kps ) {
                auto& trackPlacement =
                    config.editablePlacement(type, 2, 4, 0.2f, 0.8f);
                trackPlacement.anchorX               = 0.72f;
                config.syncKpsTrackSizes             = true;
                config.syncKpsTrackRelativePositions = true;
                config.synchronizeKpsTrackFontSize(0.08f);
            }

            config.resetPlacementToDefault(type);
            const auto& reset = config.placement(type);
            return reset.visible && near(reset.anchorX, expected.anchorX) &&
                   near(reset.anchorY, expected.anchorY) &&
                   near(reset.fontSizeRatio, expected.fontSizeRatio) &&
                   near(reset.color[0], 0.2f) && near(reset.color[3], 0.5f);
        };

    const bool allPlacementsReset =
        verifyReset(MMM::Config::CanvasComponentType::JudgmentLineTime,
                    MMM::Config::DEFAULT_JUDGMENT_LINE_TIME_PLACEMENT) &&
        verifyReset(MMM::Config::CanvasComponentType::BeatNumber,
                    MMM::Config::DEFAULT_BEAT_NUMBER_PLACEMENT) &&
        verifyReset(MMM::Config::CanvasComponentType::BeatLineTime,
                    MMM::Config::DEFAULT_BEAT_LINE_TIME_PLACEMENT) &&
        verifyReset(MMM::Config::CanvasComponentType::Kps,
                    MMM::Config::DEFAULT_KPS_TOTAL_PLACEMENT);
    return allPlacementsReset && config.kpsTracks.empty() &&
           near(config.kpsTrackFontSizeRatio, 0.0f) &&
           config.syncKpsTrackSizes && config.syncKpsTrackRelativePositions;
}

/// @brief 验证画布组件布局配置可独立完成 JSON 往返。
/// @return 显隐、锚点、字号与颜色均保持时返回 true。
bool testCanvasComponentConfigRoundTrip()
{
    MMM::Config::CanvasComponentLayoutConfig source;
    auto& placement            = source.judgmentLineTime;
    placement.visible          = true;
    placement.anchorX          = 0.23f;
    placement.anchorY          = 0.76f;
    placement.fontSizeRatio    = 0.08f;
    placement.color            = { 0.1f, 0.3f, 0.7f, 0.8f };
    auto& beatNumber           = source.beatNumber;
    beatNumber.visible         = true;
    beatNumber.anchorX         = 0.91f;
    beatNumber.anchorY         = 0.34f;
    beatNumber.fontSizeRatio   = 0.16f;
    beatNumber.color           = { 0.8f, 0.2f, 0.3f, 0.7f };
    auto& beatLineTime         = source.beatLineTime;
    beatLineTime.visible       = true;
    beatLineTime.anchorX       = 0.42f;
    beatLineTime.anchorY       = 0.67f;
    beatLineTime.fontSizeRatio = 0.19f;
    beatLineTime.color         = { 0.2f, 0.9f, 0.6f, 0.75f };
    source.kps.visible         = true;
    source.kps.anchorX         = 0.61f;
    source.kps.anchorY         = 0.09f;
    source.kps.fontSizeRatio   = 0.06f;
    source.kps.color           = { 0.9f, 0.8f, 0.2f, 0.85f };
    auto& kpsTrack             = source.editablePlacement(
        MMM::Config::CanvasComponentType::Kps, 2, 4, 0.2f, 0.8f);
    kpsTrack.anchorX                     = 0.73f;
    kpsTrack.anchorY                     = 0.24f;
    kpsTrack.fontSizeRatio               = 0.045f;
    source.syncKpsTrackSizes             = true;
    source.syncKpsTrackRelativePositions = true;
    source.synchronizeKpsTrackFontSize(0.064f);

    const nlohmann::json encoded = source;
    const auto           decoded =
        encoded.get<MMM::Config::CanvasComponentLayoutConfig>();
    const auto& restoredTime         = decoded.judgmentLineTime;
    const auto& restoredBeat         = decoded.beatNumber;
    const auto& restoredBeatLineTime = decoded.beatLineTime;
    const auto  restoredKpsTrack     = decoded.resolvedPlacement(
        MMM::Config::CanvasComponentType::Kps, 2, 4, 0.2f, 0.8f);
    const auto defaultKpsTrack = decoded.resolvedPlacement(
        MMM::Config::CanvasComponentType::Kps, 1, 4, 0.2f, 0.8f);
    auto independentlyEditable              = decoded;
    independentlyEditable.syncKpsTrackSizes = false;
    const auto independentStoredKpsTrack =
        independentlyEditable.resolvedPlacement(
            MMM::Config::CanvasComponentType::Kps, 2, 4, 0.2f, 0.8f);
    const auto independentDefaultKpsTrack =
        independentlyEditable.resolvedPlacement(
            MMM::Config::CanvasComponentType::Kps, 1, 4, 0.2f, 0.8f);
    return restoredTime.visible && near(restoredTime.anchorX, 0.23f) &&
           near(restoredTime.anchorY, 0.76f) &&
           near(restoredTime.fontSizeRatio, 0.08f) &&
           near(restoredTime.color[0], 0.1f) &&
           near(restoredTime.color[1], 0.3f) &&
           near(restoredTime.color[2], 0.7f) &&
           near(restoredTime.color[3], 0.8f) && restoredBeat.visible &&
           near(restoredBeat.anchorX, 0.91f) &&
           near(restoredBeat.anchorY, 0.34f) &&
           near(restoredBeat.fontSizeRatio, 0.16f) &&
           near(restoredBeat.color[0], 0.8f) &&
           near(restoredBeat.color[1], 0.2f) &&
           near(restoredBeat.color[2], 0.3f) &&
           near(restoredBeat.color[3], 0.7f) && restoredBeatLineTime.visible &&
           near(restoredBeatLineTime.anchorX, 0.42f) &&
           near(restoredBeatLineTime.anchorY, 0.67f) &&
           near(restoredBeatLineTime.fontSizeRatio, 0.19f) &&
           near(restoredBeatLineTime.color[0], 0.2f) &&
           near(restoredBeatLineTime.color[1], 0.9f) &&
           near(restoredBeatLineTime.color[2], 0.6f) &&
           near(restoredBeatLineTime.color[3], 0.75f) && decoded.kps.visible &&
           near(decoded.kps.anchorX, 0.61f) &&
           near(decoded.kps.anchorY, 0.09f) &&
           near(decoded.kps.fontSizeRatio, 0.06f) &&
           near(decoded.kps.color[0], 0.9f) &&
           near(decoded.kps.color[3], 0.85f) &&
           decoded.kpsTracks.size() == 1U &&
           near(restoredKpsTrack.anchorX, 0.73f) &&
           near(restoredKpsTrack.anchorY, 0.24f) &&
           near(restoredKpsTrack.fontSizeRatio, 0.064f) &&
           near(defaultKpsTrack.anchorX, 0.425f) &&
           near(defaultKpsTrack.fontSizeRatio, 0.064f) &&
           decoded.syncKpsTrackSizes && decoded.syncKpsTrackRelativePositions &&
           near(decoded.kpsTrackFontSizeRatio, 0.064f) &&
           near(independentStoredKpsTrack.fontSizeRatio, 0.064f) &&
           near(independentDefaultKpsTrack.fontSizeRatio, 0.064f);
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
    if ( !testSynchronizedCanvasComponentResize() ) {
        return 8;
    }
    if ( !testSynchronizedCanvasComponentMove() ) {
        return 9;
    }
    if ( !testBeatRelativeComponentConstraints() ) {
        return 10;
    }
    if ( !testCanvasComponentPlacementReset() ) {
        return 11;
    }
    if ( !testCanvasComponentConfigRoundTrip() ) {
        return 12;
    }
    return 0;
}
