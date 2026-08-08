#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace MMM::UI::ProjectAudioToolLayout
{

/// @brief 项目音频工具画布中的逻辑矩形。
struct Rect {
    /// @brief 左边界逻辑坐标。
    float x{ 0.0F };

    /// @brief 上边界逻辑坐标。
    float y{ 0.0F };

    /// @brief 逻辑宽度。
    float width{ 0.0F };

    /// @brief 逻辑高度。
    float height{ 0.0F };

    /// @brief 获取矩形右边界。
    [[nodiscard]] float right() const { return x + width; }

    /// @brief 获取矩形下边界。
    [[nodiscard]] float bottom() const { return y + height; }
};

/// @brief 项目音频工具画布允许的最小相机倍率。
inline constexpr float MINIMUM_CAMERA_ZOOM = 0.5F;

/// @brief 项目音频工具画布允许的最大相机倍率。
inline constexpr float MAXIMUM_CAMERA_ZOOM = 4.0F;

/// @brief 单格滚轮对应的相机倍率。
inline constexpr float CAMERA_ZOOM_STEP = 1.2F;

/// @brief 围绕鼠标缩放项目音频画布后的相机状态。
struct CameraZoomResult {
    /// @brief 应用范围限制后的画布倍率。
    float zoom{ 1.0F };

    /// @brief 保持鼠标锚点所需的水平滚动像素。
    float scrollX{ 0.0F };

    /// @brief 保持鼠标锚点所需的垂直滚动像素。
    float scrollY{ 0.0F };
};

/// @brief 将画布相机缩放到指定倍率并保持鼠标锚点。
/// @param currentZoom 当前画布倍率。
/// @param targetZoom 目标画布倍率。
/// @param dpiScale 当前窗口内容缩放。
/// @param scrollX 当前水平滚动像素。
/// @param scrollY 当前垂直滚动像素。
/// @param pointerX 鼠标相对可见画布左边界的像素坐标。
/// @param pointerY 鼠标相对可见画布上边界的像素坐标。
/// @return 新倍率与保持鼠标下逻辑坐标所需的滚动位置。
/// @warning UI 热路径：仅在 Ctrl+滚轮或缩放滑条变化时执行常量数学运算，
/// 不得引入分配。
[[nodiscard]] inline CameraZoomResult zoomCameraToPointer(
    float currentZoom, float targetZoom, float dpiScale, float scrollX,
    float scrollY, float pointerX, float pointerY)
{
    const float safeZoom =
        std::isfinite(currentZoom)
            ? std::clamp(currentZoom, MINIMUM_CAMERA_ZOOM, MAXIMUM_CAMERA_ZOOM)
            : 1.0F;
    const float safeDpiScale =
        std::isfinite(dpiScale) ? std::max(0.01F, dpiScale) : 1.0F;
    const float safeScrollX =
        std::isfinite(scrollX) ? std::max(0.0F, scrollX) : 0.0F;
    const float safeScrollY =
        std::isfinite(scrollY) ? std::max(0.0F, scrollY) : 0.0F;
    const float nextZoom =
        std::isfinite(targetZoom)
            ? std::clamp(targetZoom, MINIMUM_CAMERA_ZOOM, MAXIMUM_CAMERA_ZOOM)
            : safeZoom;
    if ( std::abs(nextZoom - safeZoom) <= 1e-6F ) {
        return { safeZoom, safeScrollX, safeScrollY };
    }

    const float safePointerX = std::isfinite(pointerX) ? pointerX : 0.0F;
    const float safePointerY = std::isfinite(pointerY) ? pointerY : 0.0F;
    const float oldScale     = safeDpiScale * safeZoom;
    const float nextScale    = safeDpiScale * nextZoom;
    const float anchorX      = (safeScrollX + safePointerX) / oldScale;
    const float anchorY      = (safeScrollY + safePointerY) / oldScale;
    return {
        nextZoom,
        std::max(0.0F, anchorX * nextScale - safePointerX),
        std::max(0.0F, anchorY * nextScale - safePointerY),
    };
}

/// @brief 按滚轮增量围绕鼠标位置缩放画布相机。
/// @param currentZoom 当前画布倍率。
/// @param wheelDelta 本帧垂直滚轮增量，正值放大、负值缩小。
/// @param dpiScale 当前窗口内容缩放。
/// @param scrollX 当前水平滚动像素。
/// @param scrollY 当前垂直滚动像素。
/// @param pointerX 鼠标相对可见画布左边界的像素坐标。
/// @param pointerY 鼠标相对可见画布上边界的像素坐标。
/// @return 新倍率与保持鼠标下逻辑坐标所需的滚动位置。
/// @warning UI 热路径：仅在 Ctrl+滚轮触发时执行常量数学运算，不得引入分配。
[[nodiscard]] inline CameraZoomResult zoomCameraAtPointer(
    float currentZoom, float wheelDelta, float dpiScale, float scrollX,
    float scrollY, float pointerX, float pointerY)
{
    const float safeZoom =
        std::isfinite(currentZoom)
            ? std::clamp(currentZoom, MINIMUM_CAMERA_ZOOM, MAXIMUM_CAMERA_ZOOM)
            : 1.0F;
    if ( !std::isfinite(wheelDelta) || std::abs(wheelDelta) <= 1e-6F ) {
        return zoomCameraToPointer(
            safeZoom, safeZoom, dpiScale, scrollX, scrollY, pointerX, pointerY);
    }
    return zoomCameraToPointer(
        safeZoom,
        safeZoom * std::pow(CAMERA_ZOOM_STEP, wheelDelta),
        dpiScale,
        scrollX,
        scrollY,
        pointerX,
        pointerY);
}

/// @brief 一个下层方块及其已有上层遮挡，用于校验新增方块后的可见面积。
struct VisibilityConstraint {
    /// @brief 必须保留可见面积的下层方块。
    Rect base;

    /// @brief 已经位于基础方块上方且与其相交的不可移动遮挡。
    std::vector<Rect> fixedOccluders;

    /// @brief 扣除固定遮挡后互不重叠的可见矩形。
    std::vector<Rect> fixedVisibleCells;

    /// @brief 固定遮挡在基础方块内的并集面积。
    float fixedCoveredArea{ 0.0F };

    /// @brief 扣除固定遮挡后剩余的可见面积。
    float fixedVisibleArea{ 0.0F };
};

/// @brief 单轴吸附锁；拖过释放阈值前保持落在同一目标位置。
struct AxisSnapLock {
    /// @brief 当前锁定的方块左坐标或上坐标。
    std::optional<float> position;

    /// @brief 当前命中的目标参考线逻辑坐标。
    std::optional<float> targetLine;
};

/// @brief 二维吸附锁。
struct SnapLocks {
    /// @brief 水平吸附锁。
    AxisSnapLock x;

    /// @brief 垂直吸附锁。
    AxisSnapLock y;
};

/// @brief 单轴缩放方向。
enum class ResizeEdge {
    None,
    Minimum,
    Maximum,
};

/// @brief 根据文字宽度、按钮内边距和类型下限计算默认方块宽度。
[[nodiscard]] inline float calculateDefaultWidth(float textWidth,
                                                 float horizontalPadding,
                                                 float minimumWidth)
{
    return std::ceil(
        std::max(minimumWidth, textWidth + horizontalPadding * 2.0F + 2.0F));
}

/// @brief 根据固定按钮行计算不会压缩控件的最小方块宽度。
[[nodiscard]] inline float calculateControlMinimumWidth(float buttonSize,
                                                        float buttonSpacing,
                                                        float horizontalPadding,
                                                        std::size_t buttonCount)
{
    if ( buttonCount == 0 ) return horizontalPadding * 2.0F;
    return horizontalPadding * 2.0F +
           buttonSize * static_cast<float>(buttonCount) +
           buttonSpacing * static_cast<float>(buttonCount - 1U);
}

/// @brief 根据类型、文件名、进度条和按钮行计算最小方块高度。
[[nodiscard]] inline float calculateControlMinimumHeight(
    float textLineHeight, float progressHeight, float progressSpacing,
    float buttonSize, float verticalPadding, float itemSpacing)
{
    return verticalPadding * 2.0F + textLineHeight * 2.0F + itemSpacing * 2.0F +
           progressHeight + progressSpacing + buttonSize;
}

/// @brief 计算矩形面积。
[[nodiscard]] inline float area(const Rect& rect)
{
    return std::max(0.0F, rect.width) * std::max(0.0F, rect.height);
}

/// @brief 计算两个矩形的交集。
[[nodiscard]] inline std::optional<Rect> intersection(const Rect& lhs,
                                                      const Rect& rhs)
{
    const float left   = std::max(lhs.x, rhs.x);
    const float top    = std::max(lhs.y, rhs.y);
    const float right  = std::min(lhs.right(), rhs.right());
    const float bottom = std::min(lhs.bottom(), rhs.bottom());
    if ( right <= left || bottom <= top ) return std::nullopt;
    return Rect{ left, top, right - left, bottom - top };
}

/// @brief 计算多个遮挡矩形在基础矩形内的并集面积。
/// @warning 低频布局路径：仅在拖动或叠层变化时调用；会排序局部交点，
/// 禁止在未发生布局变化的普通绘制帧中调用。
[[nodiscard]] inline float coveredArea(const Rect&           base,
                                       std::span<const Rect> occluders)
{
    if ( area(base) <= 0.0F || occluders.empty() ) return 0.0F;

    std::vector<Rect>  clipped;
    std::vector<float> xCoordinates{ base.x, base.right() };
    clipped.reserve(occluders.size());
    xCoordinates.reserve(occluders.size() * 2 + 2);
    for ( const auto& occluder : occluders ) {
        const auto clippedRect = intersection(base, occluder);
        if ( !clippedRect ) continue;
        clipped.push_back(*clippedRect);
        xCoordinates.push_back(clippedRect->x);
        xCoordinates.push_back(clippedRect->right());
    }
    if ( clipped.empty() ) return 0.0F;

    std::ranges::sort(xCoordinates);
    xCoordinates.erase(std::unique(xCoordinates.begin(), xCoordinates.end()),
                       xCoordinates.end());

    float                                covered = 0.0F;
    std::vector<std::pair<float, float>> yIntervals;
    yIntervals.reserve(clipped.size());
    for ( std::size_t xIndex = 1; xIndex < xCoordinates.size(); ++xIndex ) {
        const float left  = xCoordinates[xIndex - 1];
        const float right = xCoordinates[xIndex];
        if ( right <= left ) continue;
        const float midpoint = (left + right) * 0.5F;

        yIntervals.clear();
        for ( const auto& clippedRect : clipped ) {
            if ( midpoint > clippedRect.x && midpoint < clippedRect.right() ) {
                yIntervals.emplace_back(clippedRect.y, clippedRect.bottom());
            }
        }
        if ( yIntervals.empty() ) continue;

        std::ranges::sort(yIntervals);
        float intervalStart = yIntervals.front().first;
        float intervalEnd   = yIntervals.front().second;
        float coveredY      = 0.0F;
        for ( std::size_t i = 1; i < yIntervals.size(); ++i ) {
            if ( yIntervals[i].first <= intervalEnd ) {
                intervalEnd = std::max(intervalEnd, yIntervals[i].second);
            } else {
                coveredY += intervalEnd - intervalStart;
                intervalStart = yIntervals[i].first;
                intervalEnd   = yIntervals[i].second;
            }
        }
        coveredY += intervalEnd - intervalStart;
        covered += (right - left) * coveredY;
    }
    return std::clamp(covered, 0.0F, area(base));
}

/// @brief 计算基础矩形扣除遮挡并集后的可见比例。
[[nodiscard]] inline float visibleRatio(const Rect&           base,
                                        std::span<const Rect> occluders)
{
    const float baseArea = area(base);
    if ( baseArea <= 0.0F ) return 0.0F;
    return std::clamp(
        1.0F - coveredArea(base, occluders) / baseArea, 0.0F, 1.0F);
}

/// @brief 将一个矩形扣除遮挡交集并追加为最多四个互不重叠矩形。
/// @param source 需要裁切的矩形。
/// @param occluder 遮挡矩形。
/// @param output 接收剩余可见矩形。
inline void appendSubtractedRect(const Rect& source, const Rect& occluder,
                                 std::vector<Rect>& output)
{
    const auto clipped = intersection(source, occluder);
    if ( !clipped ) {
        output.push_back(source);
        return;
    }

    const float sourceRight  = source.right();
    const float sourceBottom = source.bottom();
    if ( clipped->y > source.y ) {
        output.push_back(
            Rect{ source.x, source.y, source.width, clipped->y - source.y });
    }
    if ( clipped->bottom() < sourceBottom ) {
        output.push_back(Rect{ source.x,
                               clipped->bottom(),
                               source.width,
                               sourceBottom - clipped->bottom() });
    }
    if ( clipped->x > source.x ) {
        output.push_back(Rect{
            source.x, clipped->y, clipped->x - source.x, clipped->height });
    }
    if ( clipped->right() < sourceRight ) {
        output.push_back(Rect{ clipped->right(),
                               clipped->y,
                               sourceRight - clipped->right(),
                               clipped->height });
    }
}

/// @brief 将可能重叠的矩形预处理成互不重叠的并集单元。
/// @warning 批量拖动开始时的低频路径：允许分配和矩形裁切，禁止每帧调用。
[[nodiscard]] inline std::vector<Rect> buildUnionCells(
    std::span<const Rect> rects)
{
    std::vector<Rect> unionCells;
    std::vector<Rect> remainingCells;
    std::vector<Rect> nextRemainingCells;
    for ( const auto& rect : rects ) {
        if ( area(rect) <= 0.0F ) continue;
        remainingCells.clear();
        remainingCells.push_back(rect);
        for ( const auto& existing : unionCells ) {
            nextRemainingCells.clear();
            nextRemainingCells.reserve(remainingCells.size() * 2 + 2);
            for ( const auto& remaining : remainingCells ) {
                appendSubtractedRect(remaining, existing, nextRemainingCells);
            }
            remainingCells.swap(nextRemainingCells);
            if ( remainingCells.empty() ) break;
        }
        unionCells.insert(
            unionCells.end(), remainingCells.begin(), remainingCells.end());
    }
    return unionCells;
}

/// @brief 预处理一个下层方块的固定遮挡，过滤不相交方块并缓存并集面积。
/// @warning 拖动开始时的低频路径：允许分配和并集计算，禁止每帧重建。
[[nodiscard]] inline VisibilityConstraint prepareVisibilityConstraint(
    const Rect& base, std::span<const Rect> fixedOccluders)
{
    VisibilityConstraint constraint;
    constraint.base = base;
    constraint.fixedOccluders.reserve(fixedOccluders.size());
    for ( const auto& occluder : fixedOccluders ) {
        if ( intersection(base, occluder) ) {
            constraint.fixedOccluders.push_back(occluder);
        }
    }

    if ( area(base) > 0.0F ) {
        constraint.fixedVisibleCells.push_back(base);
    }
    std::vector<Rect> remainingCells;
    for ( const auto& occluder : constraint.fixedOccluders ) {
        remainingCells.clear();
        remainingCells.reserve(constraint.fixedVisibleCells.size() * 2 + 2);
        for ( const auto& visibleCell : constraint.fixedVisibleCells ) {
            appendSubtractedRect(visibleCell, occluder, remainingCells);
        }
        constraint.fixedVisibleCells.swap(remainingCells);
        if ( constraint.fixedVisibleCells.empty() ) break;
    }
    for ( const auto& visibleCell : constraint.fixedVisibleCells ) {
        constraint.fixedVisibleArea += area(visibleCell);
    }
    constraint.fixedCoveredArea =
        std::max(0.0F, area(base) - constraint.fixedVisibleArea);
    return constraint;
}

/// @brief 计算固定遮挡上再加入一个候选前景方块后的可见比例。
/// @warning 拖动热路径：只读取预切分的可见矩形，禁止分配、排序或复制遮挡。
[[nodiscard]] inline float visibleRatioWithCandidate(
    const VisibilityConstraint& constraint, const Rect& candidate)
{
    const float baseArea = area(constraint.base);
    if ( baseArea <= 0.0F ) return 0.0F;

    const auto candidateIntersection = intersection(constraint.base, candidate);
    if ( !candidateIntersection ) {
        return std::clamp(
            1.0F - constraint.fixedCoveredArea / baseArea, 0.0F, 1.0F);
    }
    if ( constraint.fixedVisibleCells.empty() ) {
        if ( !constraint.fixedOccluders.empty() ) return 0.0F;
        return std::clamp(
            1.0F - area(*candidateIntersection) / baseArea, 0.0F, 1.0F);
    }

    float newlyCoveredArea = 0.0F;
    for ( const auto& visibleCell : constraint.fixedVisibleCells ) {
        const auto newlyCovered =
            intersection(visibleCell, *candidateIntersection);
        if ( newlyCovered ) {
            newlyCoveredArea += area(*newlyCovered);
        }
    }
    return std::clamp(
        (constraint.fixedVisibleArea - newlyCoveredArea) / baseArea,
        0.0F,
        1.0F);
}

/// @brief 获取固定遮挡本身留下的可见比例。
/// @warning 拖动热路径：只读取预计算面积，禁止引入几何重建。
[[nodiscard]] inline float fixedVisibleRatio(
    const VisibilityConstraint& constraint)
{
    const float baseArea = area(constraint.base);
    if ( baseArea <= 0.0F ) return 0.0F;
    if ( constraint.fixedVisibleCells.empty() &&
         constraint.fixedOccluders.empty() ) {
        return 1.0F;
    }
    return std::clamp(
        1.0F - constraint.fixedCoveredArea / baseArea, 0.0F, 1.0F);
}

/// @brief 计算候选方块相对既有布局新增的可见比例缺口。
/// @warning 拖动热路径：只允许调用无分配的缓存查询。
[[nodiscard]] inline float visibilityDeficit(
    const VisibilityConstraint& constraint, const Rect& candidate,
    float minimumVisibleRatio)
{
    const float requiredRatio =
        std::min(minimumVisibleRatio, fixedVisibleRatio(constraint));
    return std::max(
        0.0F, requiredRatio - visibleRatioWithCandidate(constraint, candidate));
}

/// @brief 计算一组已去重矩形平移后对下层方块造成的可见比例。
/// @warning 批量拖动热路径：候选单元与固定可见单元均已缓存，禁止分配。
[[nodiscard]] inline float visibleRatioWithTranslatedCandidates(
    const VisibilityConstraint& constraint,
    std::span<const Rect> candidateUnionCells, float deltaX, float deltaY)
{
    const float baseArea = area(constraint.base);
    if ( baseArea <= 0.0F ) return 0.0F;

    float      newlyCoveredArea      = 0.0F;
    const auto accumulateCoveredArea = [&](const Rect& visibleCell) {
        for ( const auto& candidateCell : candidateUnionCells ) {
            const Rect translated{
                candidateCell.x + deltaX,
                candidateCell.y + deltaY,
                candidateCell.width,
                candidateCell.height,
            };
            const auto newlyCovered = intersection(visibleCell, translated);
            if ( newlyCovered ) {
                newlyCoveredArea += area(*newlyCovered);
            }
        }
    };

    float baselineVisibleArea = constraint.fixedVisibleArea;
    if ( constraint.fixedVisibleCells.empty() &&
         constraint.fixedOccluders.empty() ) {
        baselineVisibleArea = baseArea;
        accumulateCoveredArea(constraint.base);
    } else {
        for ( const auto& visibleCell : constraint.fixedVisibleCells ) {
            accumulateCoveredArea(visibleCell);
        }
    }
    return std::clamp(
        (baselineVisibleArea - newlyCoveredArea) / baseArea, 0.0F, 1.0F);
}

/// @brief 计算批量候选相对既有布局新增的可见比例缺口。
/// @warning 批量拖动热路径：只允许调用无分配的缓存查询。
[[nodiscard]] inline float translatedVisibilityDeficit(
    const VisibilityConstraint& constraint,
    std::span<const Rect> candidateUnionCells, float deltaX, float deltaY,
    float minimumVisibleRatio)
{
    const float requiredRatio =
        std::min(minimumVisibleRatio, fixedVisibleRatio(constraint));
    return std::max(
        0.0F,
        requiredRatio - visibleRatioWithTranslatedCandidates(
                            constraint, candidateUnionCells, deltaX, deltaY));
}

/// @brief 计算批量候选对全部下层方块造成的总可见比例缺口。
/// @warning 批量拖动热路径：每帧遍历缓存，不得复制或重建候选矩形。
[[nodiscard]] inline float translatedVisibilityDeficit(
    std::span<const VisibilityConstraint> constraints,
    std::span<const Rect> candidateUnionCells, float deltaX, float deltaY,
    float minimumVisibleRatio)
{
    float deficit = 0.0F;
    for ( const auto& constraint : constraints ) {
        deficit += translatedVisibilityDeficit(constraint,
                                               candidateUnionCells,
                                               deltaX,
                                               deltaY,
                                               minimumVisibleRatio);
    }
    return deficit;
}

/// @brief 将矩形限制在工具画布边界内。
[[nodiscard]] inline Rect clampToBounds(Rect rect, const Rect& bounds)
{
    rect.x = std::clamp(
        rect.x, bounds.x, std::max(bounds.x, bounds.right() - rect.width));
    rect.y = std::clamp(
        rect.y, bounds.y, std::max(bounds.y, bounds.bottom() - rect.height));
    return rect;
}

/// @brief 同尺寸方块堆叠后为下层方块保留的最小可见比例。
inline constexpr float STACK_MINIMUM_VISIBLE_RATIO = 0.35F;

/// @brief 同尺寸方块可自由组合的精确堆叠可见比例，对应覆盖 65%、50%、25%。
inline constexpr std::array<float, 3> STACK_VISIBLE_RATIOS{
    STACK_MINIMUM_VISIBLE_RATIO,
    0.50F,
    0.75F,
};

/// @brief 判断两个方块是否可视为同尺寸方块。
[[nodiscard]] inline bool hasMatchingSize(const Rect& lhs, const Rect& rhs)
{
    constexpr float SIZE_TOLERANCE = 0.5F;
    return std::abs(lhs.width - rhs.width) <= SIZE_TOLERANCE &&
           std::abs(lhs.height - rhs.height) <= SIZE_TOLERANCE;
}

/// @brief 收集矩形水平方向的边缘与中心锚点。
[[nodiscard]] inline std::array<float, 3> horizontalTargets(const Rect& rect)
{
    return {
        rect.x,
        rect.x + rect.width * 0.5F,
        rect.right(),
    };
}

/// @brief 收集矩形垂直方向的边缘与中心锚点。
[[nodiscard]] inline std::array<float, 3> verticalTargets(const Rect& rect)
{
    return {
        rect.y,
        rect.y + rect.height * 0.5F,
        rect.bottom(),
    };
}

/// @brief 对一个轴执行自身左中右或上中下到目标锚点的吸附。
[[nodiscard]] inline float snapAxis(float rawPosition, float size,
                                    std::span<const float> targetAnchors,
                                    float snapThreshold, float releaseThreshold,
                                    AxisSnapLock& lock)
{
    if ( lock.position ) {
        if ( std::abs(rawPosition - *lock.position) <= releaseThreshold ) {
            return *lock.position;
        }
        lock.position.reset();
        lock.targetLine.reset();
    }

    constexpr std::array<float, 3> OWN_ANCHOR_RATIOS{ 0.0F, 0.5F, 1.0F };
    float                          bestPosition = rawPosition;
    float                          bestDistance = snapThreshold;
    std::optional<float>           bestTarget;
    for ( const float target : targetAnchors ) {
        for ( const float ratio : OWN_ANCHOR_RATIOS ) {
            const float candidate = target - size * ratio;
            const float distance  = std::abs(rawPosition - candidate);
            if ( distance <= bestDistance ) {
                bestDistance = distance;
                bestPosition = candidate;
                bestTarget   = target;
            }
        }
    }
    if ( bestPosition != rawPosition ) {
        lock.position   = bestPosition;
        lock.targetLine = bestTarget;
    }
    return bestPosition;
}

/// @brief 将拖动方块吸附到同尺寸堆叠位置、其它方块和可见画布的锚点。
/// @warning 拖动热路径：按缓存的图层顺序检查方块，禁止查询项目资源或重建布局。
[[nodiscard]] inline Rect snapRect(Rect rawRect, const Rect& visibleCanvas,
                                   std::span<const Rect> otherRects,
                                   float snapThreshold, float releaseThreshold,
                                   SnapLocks& locks)
{
    const auto retainLock = [releaseThreshold](float         rawPosition,
                                               AxisSnapLock& lock) {
        if ( !lock.position ||
             std::abs(rawPosition - *lock.position) <= releaseThreshold ) {
            return;
        }
        lock.position.reset();
        lock.targetLine.reset();
    };
    retainLock(rawRect.x, locks.x);
    retainLock(rawRect.y, locks.y);

    const auto axisCanAttach = [snapThreshold](float rawPosition,
                                               float stackedPosition,
                                               const AxisSnapLock& lock) {
        if ( lock.position ) {
            return std::abs(*lock.position - stackedPosition) <= 1e-4F;
        }
        return std::abs(rawPosition - stackedPosition) <= snapThreshold;
    };
    for ( auto target = otherRects.rbegin(); target != otherRects.rend();
          ++target ) {
        if ( !hasMatchingSize(rawRect, *target) ) continue;

        const float stackedX = target->x;
        if ( !axisCanAttach(rawRect.x, stackedX, locks.x) ) {
            continue;
        }

        std::optional<float> stackedY;
        float                bestVerticalDistance = snapThreshold;
        for ( const float visibleRatio : STACK_VISIBLE_RATIOS ) {
            const float candidateY = target->y + target->height * visibleRatio;
            if ( !axisCanAttach(rawRect.y, candidateY, locks.y) ) continue;

            const float distance =
                locks.y.position ? 0.0F : std::abs(rawRect.y - candidateY);
            if ( distance > bestVerticalDistance ) continue;
            bestVerticalDistance = distance;
            stackedY             = candidateY;
        }
        if ( !stackedY ) continue;

        rawRect.x          = stackedX;
        rawRect.y          = *stackedY;
        locks.x.position   = stackedX;
        locks.x.targetLine = target->x;
        locks.y.position   = *stackedY;
        locks.y.targetLine = *stackedY;
        return rawRect;
    }

    if ( locks.x.position && locks.y.position ) {
        rawRect.x = *locks.x.position;
        rawRect.y = *locks.y.position;
        return rawRect;
    }

    std::vector<float> xTargets;
    std::vector<float> yTargets;
    xTargets.reserve(otherRects.size() * 3 + 3);
    yTargets.reserve(otherRects.size() * 3 + 3);
    xTargets.push_back(visibleCanvas.x);
    xTargets.push_back(visibleCanvas.x + visibleCanvas.width * 0.5F);
    xTargets.push_back(visibleCanvas.right());
    yTargets.push_back(visibleCanvas.y);
    yTargets.push_back(visibleCanvas.y + visibleCanvas.height * 0.5F);
    yTargets.push_back(visibleCanvas.bottom());
    for ( const auto& rect : otherRects ) {
        const auto horizontal = horizontalTargets(rect);
        const auto vertical   = verticalTargets(rect);
        xTargets.insert(xTargets.end(), horizontal.begin(), horizontal.end());
        yTargets.insert(yTargets.end(), vertical.begin(), vertical.end());
    }

    rawRect.x = snapAxis(rawRect.x,
                         rawRect.width,
                         xTargets,
                         snapThreshold,
                         releaseThreshold,
                         locks.x);
    rawRect.y = snapAxis(rawRect.y,
                         rawRect.height,
                         yTargets,
                         snapThreshold,
                         releaseThreshold,
                         locks.y);
    return rawRect;
}

/// @brief 收集当前画布和其它方块在一个轴上的全部吸附锚点。
[[nodiscard]] inline std::vector<float> collectAxisTargets(
    bool horizontal, const Rect& visibleCanvas,
    std::span<const Rect> otherRects)
{
    std::vector<float> targets;
    targets.reserve(otherRects.size() * 3 + 3);
    if ( horizontal ) {
        targets.push_back(visibleCanvas.x);
        targets.push_back(visibleCanvas.x + visibleCanvas.width * 0.5F);
        targets.push_back(visibleCanvas.right());
        for ( const auto& rect : otherRects ) {
            const auto anchors = horizontalTargets(rect);
            targets.insert(targets.end(), anchors.begin(), anchors.end());
        }
    } else {
        targets.push_back(visibleCanvas.y);
        targets.push_back(visibleCanvas.y + visibleCanvas.height * 0.5F);
        targets.push_back(visibleCanvas.bottom());
        for ( const auto& rect : otherRects ) {
            const auto anchors = verticalTargets(rect);
            targets.insert(targets.end(), anchors.begin(), anchors.end());
        }
    }
    return targets;
}

/// @brief 对正在缩放的单轴执行自身边缘或中心到目标锚点的吸附。
[[nodiscard]] inline float snapResizeAxis(
    float rawMinimum, float rawMaximum, ResizeEdge edge,
    std::span<const float> targetAnchors, float minimumSize,
    float snapThreshold, float releaseThreshold, AxisSnapLock& lock)
{
    if ( edge == ResizeEdge::None ) {
        lock.position.reset();
        lock.targetLine.reset();
        return edge == ResizeEdge::Minimum ? rawMinimum : rawMaximum;
    }

    const float rawEdge = edge == ResizeEdge::Minimum ? rawMinimum : rawMaximum;
    if ( lock.position ) {
        if ( std::abs(rawEdge - *lock.position) <= releaseThreshold ) {
            return *lock.position;
        }
        lock.position.reset();
        lock.targetLine.reset();
    }

    constexpr std::array<float, 3> OWN_ANCHOR_RATIOS{ 0.0F, 0.5F, 1.0F };
    float                          bestEdge     = rawEdge;
    float                          bestDistance = snapThreshold;
    std::optional<float>           bestTarget;
    for ( const float target : targetAnchors ) {
        for ( const float ratio : OWN_ANCHOR_RATIOS ) {
            float candidate = rawEdge;
            if ( edge == ResizeEdge::Minimum ) {
                if ( ratio >= 1.0F ) continue;
                candidate = (target - ratio * rawMaximum) / (1.0F - ratio);
                if ( rawMaximum - candidate < minimumSize ) continue;
            } else {
                if ( ratio <= 0.0F ) continue;
                candidate = (target - (1.0F - ratio) * rawMinimum) / ratio;
                if ( candidate - rawMinimum < minimumSize ) continue;
            }
            const float distance = std::abs(rawEdge - candidate);
            if ( distance <= bestDistance ) {
                bestDistance = distance;
                bestEdge     = candidate;
                bestTarget   = target;
            }
        }
    }
    if ( bestEdge != rawEdge ) {
        lock.position   = bestEdge;
        lock.targetLine = bestTarget;
    }
    return bestEdge;
}

/// @brief 将缩放中的活动边吸附到方块及可见画布的边缘和中心锚点。
[[nodiscard]] inline Rect snapResizeRect(
    Rect rawRect, ResizeEdge horizontalEdge, ResizeEdge verticalEdge,
    const Rect& visibleCanvas, std::span<const Rect> otherRects,
    float minimumWidth, float minimumHeight, float snapThreshold,
    float releaseThreshold, SnapLocks& locks)
{
    const auto xTargets = collectAxisTargets(true, visibleCanvas, otherRects);
    const auto yTargets = collectAxisTargets(false, visibleCanvas, otherRects);

    const float rawRight          = rawRect.right();
    const float rawBottom         = rawRect.bottom();
    const float snappedHorizontal = snapResizeAxis(rawRect.x,
                                                   rawRight,
                                                   horizontalEdge,
                                                   xTargets,
                                                   minimumWidth,
                                                   snapThreshold,
                                                   releaseThreshold,
                                                   locks.x);
    const float snappedVertical   = snapResizeAxis(rawRect.y,
                                                   rawBottom,
                                                   verticalEdge,
                                                   yTargets,
                                                   minimumHeight,
                                                   snapThreshold,
                                                   releaseThreshold,
                                                   locks.y);
    if ( horizontalEdge == ResizeEdge::Minimum ) {
        rawRect.x     = snappedHorizontal;
        rawRect.width = rawRight - snappedHorizontal;
    } else if ( horizontalEdge == ResizeEdge::Maximum ) {
        rawRect.width = snappedHorizontal - rawRect.x;
    }
    if ( verticalEdge == ResizeEdge::Minimum ) {
        rawRect.y      = snappedVertical;
        rawRect.height = rawBottom - snappedVertical;
    } else if ( verticalEdge == ResizeEdge::Maximum ) {
        rawRect.height = snappedVertical - rawRect.y;
    }
    return rawRect;
}

/// @brief 计算全部可见比例约束的总缺口。
/// @warning 拖动热路径：每帧遍历约束缓存，禁止复制、分配或重建固定遮挡。
[[nodiscard]] inline float visibilityDeficit(
    const Rect& candidate, std::span<const VisibilityConstraint> constraints,
    float minimumVisibleRatio)
{
    float deficit = 0.0F;
    for ( const auto& constraint : constraints ) {
        deficit +=
            visibilityDeficit(constraint, candidate, minimumVisibleRatio);
    }
    return deficit;
}

/// @brief 修正前景方块位置，使所有下层方块至少保留指定可见比例。
/// @warning 拖动热路径的低频几何分支：仅在方块位置变化时调用，最多执行固定
/// 轮数的局部约束检查，禁止在静止帧重复执行。
[[nodiscard]] inline Rect constrainVisibility(
    Rect candidate, const Rect& canvasBounds,
    std::span<const VisibilityConstraint> constraints,
    float                                 minimumVisibleRatio)
{
    candidate                = clampToBounds(candidate, canvasBounds);
    constexpr int MAX_PASSES = 8;
    for ( int pass = 0; pass < MAX_PASSES; ++pass ) {
        if ( visibilityDeficit(candidate, constraints, minimumVisibleRatio) <=
             1e-5F ) {
            break;
        }

        Rect  best = candidate;
        float bestDeficit =
            visibilityDeficit(candidate, constraints, minimumVisibleRatio);
        float bestDistance = std::numeric_limits<float>::max();
        for ( const auto& constraint : constraints ) {
            if ( visibilityDeficit(
                     constraint, candidate, minimumVisibleRatio) <= 1e-5F ) {
                continue;
            }

            const Rect&               base = constraint.base;
            const std::array<Rect, 4> alternatives{
                Rect{ base.x + base.width * minimumVisibleRatio,
                      candidate.y,
                      candidate.width,
                      candidate.height },
                Rect{ base.right() - base.width * minimumVisibleRatio -
                          candidate.width,
                      candidate.y,
                      candidate.width,
                      candidate.height },
                Rect{ candidate.x,
                      base.y + base.height * minimumVisibleRatio,
                      candidate.width,
                      candidate.height },
                Rect{ candidate.x,
                      base.bottom() - base.height * minimumVisibleRatio -
                          candidate.height,
                      candidate.width,
                      candidate.height },
            };
            for ( auto alternative : alternatives ) {
                alternative         = clampToBounds(alternative, canvasBounds);
                const float deficit = visibilityDeficit(
                    alternative, constraints, minimumVisibleRatio);
                const float deltaX   = alternative.x - candidate.x;
                const float deltaY   = alternative.y - candidate.y;
                const float distance = deltaX * deltaX + deltaY * deltaY;
                if ( deficit < bestDeficit - 1e-5F ||
                     (std::abs(deficit - bestDeficit) <= 1e-5F &&
                      distance < bestDistance) ) {
                    best         = alternative;
                    bestDeficit  = deficit;
                    bestDistance = distance;
                }
            }
        }
        if ( best.x == candidate.x && best.y == candidate.y ) break;
        candidate = best;
    }
    return candidate;
}

/// @brief 沿当前缩放轨迹限制候选矩形，使下层方块始终保留最小可见比例。
/// @warning 缩放交互路径：只在尺寸变化时执行固定轮数二分，禁止在静止帧调用。
[[nodiscard]] inline Rect constrainResizeVisibility(
    const Rect& previous, const Rect& candidate,
    std::span<const VisibilityConstraint> constraints,
    float                                 minimumVisibleRatio)
{
    const float previousDeficit =
        visibilityDeficit(previous, constraints, minimumVisibleRatio);
    const float candidateDeficit =
        visibilityDeficit(candidate, constraints, minimumVisibleRatio);
    if ( candidateDeficit <= 1e-5F ||
         (previousDeficit > 1e-5F &&
          candidateDeficit < previousDeficit - 1e-5F) ) {
        return candidate;
    }

    float         validAmount          = 0.0F;
    float         invalidAmount        = 1.0F;
    constexpr int BINARY_SEARCH_PASSES = 12;
    for ( int pass = 0; pass < BINARY_SEARCH_PASSES; ++pass ) {
        const float amount = (validAmount + invalidAmount) * 0.5F;
        const Rect  trial{
            previous.x + (candidate.x - previous.x) * amount,
            previous.y + (candidate.y - previous.y) * amount,
            previous.width + (candidate.width - previous.width) * amount,
            previous.height + (candidate.height - previous.height) * amount,
        };
        if ( visibilityDeficit(trial, constraints, minimumVisibleRatio) <=
             1e-5F ) {
            validAmount = amount;
        } else {
            invalidAmount = amount;
        }
    }
    return {
        previous.x + (candidate.x - previous.x) * validAmount,
        previous.y + (candidate.y - previous.y) * validAmount,
        previous.width + (candidate.width - previous.width) * validAmount,
        previous.height + (candidate.height - previous.height) * validAmount,
    };
}

/// @brief 沿批量移动轨迹限制组合外框，避免选中方块遮住固定下层方块。
/// @warning 批量拖动热路径：仅候选位置无效时执行固定轮数二分，不得分配。
[[nodiscard]] inline Rect constrainTranslatedVisibility(
    const Rect& previousBounds, Rect candidateBounds, const Rect& initialBounds,
    std::span<const Rect> candidateUnionCells, const Rect& canvasBounds,
    std::span<const VisibilityConstraint> constraints,
    float                                 minimumVisibleRatio)
{
    candidateBounds      = clampToBounds(candidateBounds, canvasBounds);
    const auto deficitAt = [&](const Rect& bounds) {
        return translatedVisibilityDeficit(constraints,
                                           candidateUnionCells,
                                           bounds.x - initialBounds.x,
                                           bounds.y - initialBounds.y,
                                           minimumVisibleRatio);
    };

    const float previousDeficit  = deficitAt(previousBounds);
    const float candidateDeficit = deficitAt(candidateBounds);
    if ( candidateDeficit <= 1e-5F ||
         (previousDeficit > 1e-5F &&
          candidateDeficit < previousDeficit - 1e-5F) ) {
        return candidateBounds;
    }

    float         validAmount          = 0.0F;
    float         invalidAmount        = 1.0F;
    constexpr int BINARY_SEARCH_PASSES = 12;
    for ( int pass = 0; pass < BINARY_SEARCH_PASSES; ++pass ) {
        const float amount = (validAmount + invalidAmount) * 0.5F;
        const Rect  trial{
            previousBounds.x + (candidateBounds.x - previousBounds.x) * amount,
            previousBounds.y + (candidateBounds.y - previousBounds.y) * amount,
            previousBounds.width,
            previousBounds.height,
        };
        if ( deficitAt(trial) <= 1e-5F ) {
            validAmount = amount;
        } else {
            invalidAmount = amount;
        }
    }
    return {
        previousBounds.x + (candidateBounds.x - previousBounds.x) * validAmount,
        previousBounds.y + (candidateBounds.y - previousBounds.y) * validAmount,
        previousBounds.width,
        previousBounds.height,
    };
}

/// @brief 查找基础方块未被上层方块遮挡的最大网格单元，用于放置文本标签。
/// @warning 低频布局路径：只在布局或叠层变化时重建标签裁切缓存。
[[nodiscard]] inline Rect largestVisibleCell(const Rect&           base,
                                             std::span<const Rect> occluders)
{
    std::vector<float> xCoordinates{ base.x, base.right() };
    std::vector<float> yCoordinates{ base.y, base.bottom() };
    for ( const auto& occluder : occluders ) {
        const auto clipped = intersection(base, occluder);
        if ( !clipped ) continue;
        xCoordinates.push_back(clipped->x);
        xCoordinates.push_back(clipped->right());
        yCoordinates.push_back(clipped->y);
        yCoordinates.push_back(clipped->bottom());
    }
    std::ranges::sort(xCoordinates);
    std::ranges::sort(yCoordinates);
    xCoordinates.erase(std::unique(xCoordinates.begin(), xCoordinates.end()),
                       xCoordinates.end());
    yCoordinates.erase(std::unique(yCoordinates.begin(), yCoordinates.end()),
                       yCoordinates.end());

    Rect best{};
    for ( std::size_t xIndex = 1; xIndex < xCoordinates.size(); ++xIndex ) {
        for ( std::size_t yIndex = 1; yIndex < yCoordinates.size(); ++yIndex ) {
            Rect cell{
                xCoordinates[xIndex - 1],
                yCoordinates[yIndex - 1],
                xCoordinates[xIndex] - xCoordinates[xIndex - 1],
                yCoordinates[yIndex] - yCoordinates[yIndex - 1],
            };
            const float midpointX = cell.x + cell.width * 0.5F;
            const float midpointY = cell.y + cell.height * 0.5F;
            const bool  covered =
                std::ranges::any_of(occluders, [&](const Rect& occluder) {
                    return midpointX > occluder.x &&
                           midpointX < occluder.right() &&
                           midpointY > occluder.y &&
                           midpointY < occluder.bottom();
                });
            if ( !covered && area(cell) > area(best) ) {
                best = cell;
            }
        }
    }
    return area(best) > 0.0F ? best : base;
}

/// @brief 从基础可见区域中扣除一个移动遮挡并选取最大标签矩形。
/// @warning 拖动热路径：固定执行至多四次面积比较，不分配、不排序。
[[nodiscard]] inline Rect largestVisibleCellWithOneOccluder(
    const Rect& base, const Rect& occluder)
{
    const auto clipped = intersection(base, occluder);
    if ( !clipped ) return base;

    const std::array candidates{
        Rect{ base.x, base.y, base.width, clipped->y - base.y },
        Rect{ base.x,
              clipped->bottom(),
              base.width,
              base.bottom() - clipped->bottom() },
        Rect{ base.x, clipped->y, clipped->x - base.x, clipped->height },
        Rect{ clipped->right(),
              clipped->y,
              base.right() - clipped->right(),
              clipped->height },
    };
    Rect best{};
    for ( const auto& candidate : candidates ) {
        if ( area(candidate) > area(best) ) {
            best = candidate;
        }
    }
    return area(best) > 0.0F ? best : base;
}

}  // namespace MMM::UI::ProjectAudioToolLayout
