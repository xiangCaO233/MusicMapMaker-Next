#pragma once

#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "ui/utils/CanvasTimeFormatContext.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace MMM::UI::Utils
{
namespace TimeFormatDetail
{

/// @brief 将无效 BPM 归一为可用于显示的 BPM。
inline double sanitizeBpm(double bpm)
{
    if ( bpm <= 0.0 ) return 120.0;
    return std::min(bpm, 10000.0);
}

/// @brief 格式化时:分:秒.毫秒。
inline std::string formatClock(double timeSeconds)
{
    bool    negative = timeSeconds < 0.0;
    double  absTime  = std::abs(timeSeconds);
    auto    totalMs  = static_cast<int64_t>(std::llround(absTime * 1000.0));
    int64_t ms       = totalMs % 1000;
    int64_t seconds  = (totalMs / 1000) % 60;
    int64_t minutes  = (totalMs / 60000) % 60;
    int64_t hours    = totalMs / 3600000;

    return fmt::format("{}{:02}:{:02}:{:02}.{:03}",
                       negative ? "-" : "",
                       hours,
                       minutes,
                       seconds,
                       ms);
}

/// @brief 格式化纯毫秒。
inline std::string formatMilliseconds(double timeSeconds)
{
    return fmt::format(
        "{} ms", static_cast<int64_t>(std::llround(timeSeconds * 1000.0)));
}

/// @brief 从快照滚动分段中提取完整 BPM 时间线。
inline std::vector<CanvasTimeFormatBpmPoint> collectBpmPoints(
    const Common::Render::RenderSnapshot* snapshot)
{
    std::vector<CanvasTimeFormatBpmPoint> points;
    if ( !snapshot ) return points;

    for ( const auto& segment : snapshot->scrollSegments ) {
        if ( segment.bpmEntity == entt::null ) continue;
        points.push_back({ segment.time, sanitizeBpm(segment.bpmValue) });
    }

    std::sort(
        points.begin(), points.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.time < rhs.time;
        });

    points.erase(std::unique(points.begin(),
                             points.end(),
                             [](const auto& lhs, const auto& rhs) {
                                 return std::abs(lhs.time - rhs.time) < 1e-6;
                             }),
                 points.end());
    return points;
}

/// @brief 使用已经归一化的 BPM 节点格式化拍号 + 分拍位。
inline std::string formatBeatTimeWithPoints(
    double timeSeconds, const std::vector<CanvasTimeFormatBpmPoint>& points,
    int beatDivisor)
{
    if ( points.empty() ) return fmt::format("{:.3f} s", timeSeconds);

    if ( beatDivisor <= 0 ) beatDivisor = 4;

    auto formatWithinBpm = [&](double  pointTime,
                               double  pointBpm,
                               int64_t totalBeats,
                               bool    beforeFirstTiming) {
        double  beatDuration = 60.0 / pointBpm;
        double  timeInBpm    = timeSeconds - pointTime;
        double  beatFloat    = timeInBpm / beatDuration;
        int64_t beatOffset = static_cast<int64_t>(std::floor(beatFloat + 1e-6));
        double  stepFloat  = (beatFloat - static_cast<double>(beatOffset)) *
                           static_cast<double>(beatDivisor);
        int step = static_cast<int>(std::round(stepFloat));
        if ( step >= beatDivisor ) {
            ++beatOffset;
            step = 0;
        }

        int beatNumber = beforeFirstTiming
                             ? static_cast<int>(beatOffset)
                             : static_cast<int>(totalBeats + beatOffset + 1);
        if ( step == 0 ) {
            return beforeFirstTiming ? fmt::format("{} + 0/1", beatNumber)
                                     : fmt::format("{} + 1/1", beatNumber);
        }

        int divisor = beatDivisor;
        int gcd     = std::gcd(step, divisor);
        return fmt::format("{} + {}/{}", beatNumber, step / gcd, divisor / gcd);
    };

    int64_t totalBeats = 0;
    for ( size_t i = 0; i < points.size(); ++i ) {
        const auto& point        = points[i];
        double      nextBpmTime  = (i + 1 < points.size())
                                       ? points[i + 1].time
                                       : std::numeric_limits<double>::infinity();
        double      beatDuration = 60.0 / point.bpm;

        if ( timeSeconds < point.time ) {
            if ( i == 0 ) {
                return formatWithinBpm(point.time, point.bpm, totalBeats, true);
            }
            break;
        }
        if ( timeSeconds < nextBpmTime ) {
            return formatWithinBpm(point.time, point.bpm, totalBeats, false);
        }

        double bpmDuration = nextBpmTime - point.time;
        totalBeats +=
            static_cast<int64_t>(std::round(bpmDuration / beatDuration));
    }

    return fmt::format("{:.3f} s", timeSeconds);
}

/// @brief 使用渲染快照格式化拍号 + 分拍位。
inline std::string formatBeatTime(
    double timeSeconds, const Common::Render::RenderSnapshot* snapshot)
{
    const auto points      = collectBpmPoints(snapshot);
    const int  beatDivisor = snapshot ? snapshot->currentBeatDivisor : 4;
    return formatBeatTimeWithPoints(timeSeconds, points, beatDivisor);
}

/// @brief 使用独立时间上下文格式化拍号 + 分拍位。
inline std::string formatBeatTime(double                         timeSeconds,
                                  const CanvasTimeFormatContext& context)
{
    return formatBeatTimeWithPoints(
        timeSeconds, context.bpmPoints, context.beatDivisor);
}

/// @brief 按指定偏好格式化时间。
inline std::string formatTimeWithPreference(
    double timeSeconds, Config::TimeFormatPreference preference,
    const Common::Render::RenderSnapshot* snapshot)
{
    switch ( preference ) {
    case Config::TimeFormatPreference::Clock: return formatClock(timeSeconds);
    case Config::TimeFormatPreference::Milliseconds:
        return formatMilliseconds(timeSeconds);
    case Config::TimeFormatPreference::Beat:
        return formatBeatTime(timeSeconds, snapshot);
    case Config::TimeFormatPreference::Seconds:
    default: return fmt::format("{:.3f} s", timeSeconds);
    }
}

}  // namespace TimeFormatDetail

/// @brief 按编辑器偏好格式化画布时间戳。
/// @param timeSeconds 时间戳，单位秒
/// @param snapshot 当前渲染快照，用于拍号格式查询 BPM 时间线
/// @return 已格式化的时间文本
inline std::string formatCanvasTime(
    double                                timeSeconds,
    const Common::Render::RenderSnapshot* snapshot = nullptr)
{
    auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    return TimeFormatDetail::formatTimeWithPreference(
        timeSeconds, preference, snapshot);
}

/// @brief 按编辑器偏好和独立数据上下文格式化画布时间戳。
/// @param timeSeconds 时间戳，单位秒。
/// @param context 不依赖渲染快照的 BPM 与分拍上下文。
/// @return 已格式化的时间文本。
inline std::string formatCanvasTime(double                         timeSeconds,
                                    const CanvasTimeFormatContext& context)
{
    const auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    if ( preference == Config::TimeFormatPreference::Beat ) {
        return TimeFormatDetail::formatBeatTime(timeSeconds, context);
    }
    return TimeFormatDetail::formatTimeWithPreference(
        timeSeconds, preference, nullptr);
}

/// @brief 按编辑器偏好格式化两个画布时间戳。
/// @param firstSeconds 第一个时间戳，单位秒
/// @param secondSeconds 第二个时间戳，单位秒
/// @param snapshot 当前渲染快照，用于拍号格式查询 BPM 时间线
/// @return 已格式化的时间范围文本
inline std::string formatCanvasTimePair(
    double firstSeconds, double secondSeconds,
    const Common::Render::RenderSnapshot* snapshot = nullptr)
{
    return fmt::format("{} / {}",
                       formatCanvasTime(firstSeconds, snapshot),
                       formatCanvasTime(secondSeconds, snapshot));
}

/// @brief 格式化画布时间长度。
/// @param durationSeconds 时间长度，单位秒
/// @return 已格式化的时间长度文本
inline std::string formatCanvasDuration(double durationSeconds)
{
    auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    if ( preference == Config::TimeFormatPreference::Beat ) {
        preference = Config::TimeFormatPreference::Seconds;
    }
    return TimeFormatDetail::formatTimeWithPreference(
        durationSeconds, preference, nullptr);
}

}  // namespace MMM::UI::Utils
