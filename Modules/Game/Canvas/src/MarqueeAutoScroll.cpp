#include "canvas/MarqueeAutoScroll.h"
#include "config/AppConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>

namespace MMM::Canvas
{
namespace
{
/// @brief Returns the fallback absolute Y speed when no ScrollSegment exists.
/// @return Fallback absolute Y speed in pixels per second.
double defaultSnapshotAbsYSpeed()
{
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    return 500.0 * static_cast<double>(std::max(0.01f, visual.timelineZoom));
}

/// @brief Estimates absolute Y from a display time in a render snapshot.
/// @param snapshot Current UI render snapshot.
/// @param time Display time in seconds.
/// @return Absolute Y at the requested display time.
/// @warning UI hot path: reads only the snapshot scroll segment cache.
double snapshotAbsYAtTime(const Logic::RenderSnapshot& snapshot, double time)
{
    if ( snapshot.scrollSegments.empty() ) {
        return time * defaultSnapshotAbsYSpeed();
    }

    auto it = std::upper_bound(
        snapshot.scrollSegments.begin(),
        snapshot.scrollSegments.end(),
        time,
        [](double val, const Logic::System::ScrollSegment& segment) {
            return val < segment.time;
        });

    const auto& segment = it == snapshot.scrollSegments.begin()
                              ? snapshot.scrollSegments.front()
                              : *std::prev(it);
    return segment.absY + (time - segment.time) * segment.speed;
}

/// @brief Attempts to resolve a display time inside one ScrollSegment.
/// @param snapshot Current UI render snapshot.
/// @param index Target ScrollSegment index.
/// @param absY Target absolute Y.
/// @param outTime Resolved display time in seconds.
/// @return True when the target absolute Y belongs to the segment.
/// @warning UI hot path: constant-time segment math only.
bool trySnapshotTimeAtSegmentAbsY(const Logic::RenderSnapshot& snapshot,
                                  size_t index, double absY, double& outTime)
{
    constexpr double EPSILON  = 1e-6;
    const auto&      segments = snapshot.scrollSegments;
    if ( index >= segments.size() ) {
        return false;
    }

    const auto& segment = segments[index];
    if ( std::abs(segment.speed) <= EPSILON ) {
        if ( std::abs(absY - segment.absY) <= EPSILON ) {
            outTime = segment.time;
            return true;
        }
        return false;
    }

    const bool   hasNext  = index + 1 < segments.size();
    const double nextTime = hasNext ? segments[index + 1].time
                                    : std::numeric_limits<double>::infinity();
    const double endAbsY =
        hasNext
            ? segment.absY + (nextTime - segment.time) * segment.speed
            : (segment.speed > 0.0 ? std::numeric_limits<double>::infinity()
                                   : -std::numeric_limits<double>::infinity());
    const double minAbsY = std::min(segment.absY, endAbsY) - EPSILON;
    const double maxAbsY = std::max(segment.absY, endAbsY) + EPSILON;
    if ( absY < minAbsY || absY > maxAbsY ) {
        return false;
    }

    outTime = segment.time + (absY - segment.absY) / segment.speed;
    return outTime >= segment.time - EPSILON && outTime <= nextTime + EPSILON;
}

/// @brief Estimates display time from absolute Y in a render snapshot.
/// @param snapshot Current UI render snapshot.
/// @param absY Target absolute Y.
/// @return Display time in seconds at the requested absolute Y.
/// @warning UI hot path: usually tests the current segment first, scanning all
/// snapshot segments only when crossing segment boundaries.
double snapshotTimeAtAbsY(const Logic::RenderSnapshot& snapshot, double absY)
{
    if ( snapshot.scrollSegments.empty() ) {
        const double speed = defaultSnapshotAbsYSpeed();
        return std::abs(speed) > 1e-9 ? absY / speed : snapshot.currentTime;
    }

    auto currentIt = std::upper_bound(
        snapshot.scrollSegments.begin(),
        snapshot.scrollSegments.end(),
        snapshot.currentTime,
        [](double val, const Logic::System::ScrollSegment& segment) {
            return val < segment.time;
        });
    const size_t currentIndex =
        currentIt == snapshot.scrollSegments.begin()
            ? 0
            : static_cast<size_t>(std::distance(snapshot.scrollSegments.begin(),
                                                std::prev(currentIt)));

    double outTime = snapshot.currentTime;
    if ( trySnapshotTimeAtSegmentAbsY(snapshot, currentIndex, absY, outTime) ) {
        return outTime;
    }

    for ( size_t i = 0; i < snapshot.scrollSegments.size(); ++i ) {
        if ( i == currentIndex ) {
            continue;
        }
        if ( trySnapshotTimeAtSegmentAbsY(snapshot, i, absY, outTime) ) {
            return outTime;
        }
    }

    const auto& first = snapshot.scrollSegments.front();
    const auto& last  = snapshot.scrollSegments.back();
    const auto& edge =
        std::abs(absY - first.absY) < std::abs(absY - last.absY) ? first : last;
    if ( std::abs(edge.speed) <= 1e-9 ) {
        return edge.time;
    }
    return edge.time + (absY - edge.absY) / edge.speed;
}
}  // namespace

double marqueeAutoScrollTargetTime(const Logic::RenderSnapshot& snapshot,
                                   float viewportHeight, float mouseY,
                                   float deltaTime, bool isAccelerated,
                                   bool& scrolled)
{
    scrolled = false;
    if ( !std::isfinite(mouseY) || !std::isfinite(viewportHeight) ||
         viewportHeight <= 1.0f ||
         (mouseY >= 0.0f && mouseY <= viewportHeight) ) {
        return snapshot.currentTime;
    }

    const double direction = mouseY < 0.0f ? 1.0 : -1.0;
    const float  outsidePixels =
        mouseY < 0.0f ? -mouseY : mouseY - viewportHeight;
    if ( outsidePixels <= 0.0f ) {
        return snapshot.currentTime;
    }

    const auto&  visual = Config::AppConfig::instance().getVisualConfig();
    const double sensitivity =
        std::max(0.0f, visual.previewConfig.edgeScrollSensitivity);
    if ( sensitivity <= 1e-6 ) {
        return snapshot.currentTime;
    }

    const double dt = std::clamp(std::isfinite(deltaTime) && deltaTime > 0.0f
                                     ? static_cast<double>(deltaTime)
                                     : 1.0 / 60.0,
                                 1.0 / 240.0,
                                 1.0 / 15.0);
    const double ramp =
        std::max(0.0,
                 static_cast<double>(outsidePixels) /
                     std::max(1.0, static_cast<double>(viewportHeight) * 0.18));
    const double     acceleratedRamp                = ramp * ramp;
    constexpr double SHIFT_AUTO_SCROLL_ACCELERATION = 3.0;
    const double     acceleration =
        isAccelerated ? SHIFT_AUTO_SCROLL_ACCELERATION : 1.0;
    const double pixelsPerSecond =
        (6000.0 + 24000.0 * acceleratedRamp) * sensitivity * acceleration;
    const double scale = std::abs(snapshot.renderScaleY) > 1e-6f
                             ? static_cast<double>(snapshot.renderScaleY)
                             : 1.0;
    const double currentAbsY =
        snapshotAbsYAtTime(snapshot, snapshot.currentTime);
    const double targetAbsY =
        currentAbsY + direction * pixelsPerSecond * dt / scale;
    const double targetTime = snapshotTimeAtAbsY(snapshot, targetAbsY);
    scrolled = std::isfinite(targetTime) &&
               std::abs(targetTime - snapshot.currentTime) > 1e-6;
    return scrolled ? targetTime : snapshot.currentTime;
}

}  // namespace MMM::Canvas
