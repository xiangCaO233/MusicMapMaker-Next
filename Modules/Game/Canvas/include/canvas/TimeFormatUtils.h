#pragma once

#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "logic/BeatmapSyncBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace MMM::Canvas
{
namespace TimeFormatDetail
{

/// @brief BPM 时间线格式化段。
struct BpmFormatPoint {
    /// @brief 段起始时间，单位秒。
    double time{ 0.0 };
    /// @brief 段 BPM。
    double bpm{ 120.0 };
};

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
inline std::vector<BpmFormatPoint> collectBpmPoints(
    const Logic::RenderSnapshot* snapshot)
{
    std::vector<BpmFormatPoint> points;
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

/// @brief 格式化拍号 + 分拍位。
inline std::string formatBeatTime(double                       timeSeconds,
                                  const Logic::RenderSnapshot* snapshot)
{
    auto points = collectBpmPoints(snapshot);
    if ( points.empty() ) return fmt::format("{:.3f} s", timeSeconds);

    int beatDivisor = snapshot ? snapshot->currentBeatDivisor : 4;
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    int64_t totalBeats = 0;
    for ( size_t i = 0; i < points.size(); ++i ) {
        const auto& point       = points[i];
        double      nextBpmTime = (i + 1 < points.size())
                                      ? points[i + 1].time
                                      : std::numeric_limits<double>::infinity();
        double      beatDuration = 60.0 / point.bpm;

        if ( timeSeconds < point.time ) break;
        if ( timeSeconds < nextBpmTime ) {
            double  timeInBpm = timeSeconds - point.time;
            double  beatFloat = timeInBpm / beatDuration;
            int64_t beatOffset =
                static_cast<int64_t>(std::floor(beatFloat + 1e-6));
            double stepFloat = (beatFloat - static_cast<double>(beatOffset)) *
                               static_cast<double>(beatDivisor);
            int    step      = static_cast<int>(std::round(stepFloat));
            if ( step >= beatDivisor ) {
                ++beatOffset;
                step = 0;
            }

            int beatNumber = static_cast<int>(totalBeats + beatOffset + 1);
            if ( step == 0 ) return fmt::format("{} + 1/1", beatNumber);

            int divisor = beatDivisor;
            int gcd     = std::gcd(step, divisor);
            return fmt::format(
                "{} + {}/{}", beatNumber, step / gcd, divisor / gcd);
        }

        double bpmDuration = nextBpmTime - point.time;
        totalBeats +=
            static_cast<int64_t>(std::round(bpmDuration / beatDuration));
    }

    return fmt::format("{:.3f} s", timeSeconds);
}

/// @brief 按指定偏好格式化时间。
inline std::string formatTimeWithPreference(
    double timeSeconds, Config::TimeFormatPreference preference,
    const Logic::RenderSnapshot* snapshot)
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
    double timeSeconds, const Logic::RenderSnapshot* snapshot = nullptr)
{
    auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    return TimeFormatDetail::formatTimeWithPreference(
        timeSeconds, preference, snapshot);
}

/// @brief 按编辑器偏好格式化两个画布时间戳。
/// @param firstSeconds 第一个时间戳，单位秒
/// @param secondSeconds 第二个时间戳，单位秒
/// @param snapshot 当前渲染快照，用于拍号格式查询 BPM 时间线
/// @return 已格式化的时间范围文本
inline std::string formatCanvasTimePair(
    double firstSeconds, double secondSeconds,
    const Logic::RenderSnapshot* snapshot = nullptr)
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

}  // namespace MMM::Canvas
