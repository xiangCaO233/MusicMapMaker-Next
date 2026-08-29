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
/// @brief 获取没有 ScrollSegment 时使用的兜底绝对 Y 速度。
/// @return 兜底绝对 Y 速度，单位为像素/秒。
double defaultSnapshotAbsYSpeed()
{
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    return 500.0 * static_cast<double>(std::max(0.01f, visual.timelineZoom));
}

/// @brief 根据渲染快照中的显示时间估算绝对 Y 坐标。
/// @param snapshot 当前 UI 渲染快照。
/// @param time 显示时间，单位为秒。
/// @return 指定显示时间对应的绝对 Y 坐标。
/// @warning UI 热路径：只读取快照滚动分段缓存。
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

/// @brief 尝试在单个 ScrollSegment 内反解显示时间。
/// @param snapshot 当前 UI 渲染快照。
/// @param index 目标 ScrollSegment 索引。
/// @param absY 目标绝对 Y 坐标。
/// @param outTime 解析出的显示时间，单位为秒。
/// @return 目标绝对 Y 坐标位于该分段内时返回 true。
/// @warning UI 热路径：只做常量时间的分段数学计算。
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

/// @brief 根据渲染快照中的绝对 Y 坐标估算显示时间。
/// @param snapshot 当前 UI 渲染快照。
/// @param absY 目标绝对 Y 坐标。
/// @return 指定绝对 Y 坐标对应的显示时间，单位为秒。
/// @warning UI 热路径：通常先测试当前分段，只在跨分段时扫描快照分段。
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
    scrolled                = std::isfinite(targetTime) &&
               std::abs(targetTime - snapshot.currentTime) > 1e-6;
    return scrolled ? targetTime : snapshot.currentTime;
}

}  // namespace MMM::Canvas
