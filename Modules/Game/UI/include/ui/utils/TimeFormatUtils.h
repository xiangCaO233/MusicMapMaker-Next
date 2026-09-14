#pragma once

#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "mmm/timing/BpmNormalization.h"
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

/// @brief 画布时间对应的离散拍号与分拍位置。
struct CanvasBeatPosition {
    /// @brief 一基拍号；首个 BPM 节点之前保留零或负数偏移。
    int64_t beatNumber{ 0 };
    /// @brief 已约分的分拍分子；正常整拍使用一。
    int numerator{ 0 };
    /// @brief 已约分的分拍分母。
    int denominator{ 1 };
    /// @brief 是否存在可用于换算的 BPM 时间线。
    bool valid{ false };
};

namespace TimeFormatDetail
{

/// @brief 格式化时:分:秒.毫秒。
/// @param timeSeconds 可为负值的时间，单位秒。
/// @return 固定宽度 `HH:MM:SS.mmm` 文本，负值带前导负号。
/// @note 先对绝对毫秒总数舍入，再拆分字段，保证进位一致。
inline std::string formatClock(double timeSeconds)
{
    // 符号与绝对值分开处理，避免负数取模产生负字段。
    bool negative = timeSeconds < 0.0;
    // llround 使毫秒舍入可以正确进位到秒、分和小时。
    double absTime = std::abs(timeSeconds);
    auto   totalMs = static_cast<int64_t>(std::llround(absTime * 1000.0));
    // 由总毫秒依次拆出毫秒、秒、分，小时不限制位数。
    int64_t ms      = totalMs % 1000;
    int64_t seconds = (totalMs / 1000) % 60;
    int64_t minutes = (totalMs / 60000) % 60;
    int64_t hours   = totalMs / 3600000;

    // 秒、分和毫秒始终补零，小时至少两位。
    return fmt::format("{}{:02}:{:02}:{:02}.{:03}",
                       negative ? "-" : "",
                       hours,
                       minutes,
                       seconds,
                       ms);
}

/// @brief 格式化纯毫秒。
/// @param timeSeconds 时间或时长，单位秒。
/// @return 四舍五入到整数毫秒并附加单位的文本。
inline std::string formatMilliseconds(double timeSeconds)
{
    // 直接对有符号秒数换算，保留负时间方向。
    return fmt::format(
        "{} ms", static_cast<int64_t>(std::llround(timeSeconds * 1000.0)));
}

/// @brief 从快照滚动分段中提取完整 BPM 时间线。
/// @param snapshot 当前渲染快照，可为空。
/// @return 按时间升序排列且时间点去重的 BPM 节点值数组。
/// @warning 非每帧批量调用路径：函数会分配、排序并去重节点数组。
inline std::vector<CanvasTimeFormatBpmPoint> collectBpmPoints(
    const Common::Render::RenderSnapshot* snapshot)
{
    std::vector<CanvasTimeFormatBpmPoint> points;
    // 无快照时返回空时间线，调用方回退普通秒格式。
    if ( !snapshot ) return points;

    // 滚动分段只有关联 BPM 实体时才代表有效计时节点。
    for ( const auto& segment : snapshot->scrollSegments ) {
        if ( segment.bpmEntity == entt::null ) continue;
        // BPM 在进入格式化算法前统一修正非法或极端值。
        points.push_back(
            { segment.time, ::MMM::normalizeBpmValue(segment.bpmValue) });
    }

    // 渲染分段顺序不作为计时时间线有序性的隐含前提。
    std::sort(
        points.begin(), points.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.time < rhs.time;
        });

    // 同一时间的重复分段只保留排序后的首个 BPM 节点。
    // 一微秒容差吸收序列化和浮点计算产生的微小时间差。
    points.erase(std::unique(points.begin(),
                             points.end(),
                             [](const auto& lhs, const auto& rhs) {
                                 return std::abs(lhs.time - rhs.time) < 1e-6;
                             }),
                 points.end());
    return points;
}

/// @brief 使用已经归一化的 BPM 节点计算拍号与分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param points 已按时间排序且 BPM 有效的计时节点。
/// @param beatDivisor 当前分拍数，非正值回退为四。
/// @return 拍号与约分后的分拍；缺少适用节点时返回无效结果。
inline CanvasBeatPosition calculateBeatPositionWithPoints(
    double timeSeconds, const std::vector<CanvasTimeFormatBpmPoint>& points,
    int beatDivisor)
{
    // 没有 BPM 时间线时无法计算拍位置，由调用方决定占位或回退格式。
    if ( points.empty() ) return {};

    // 四分拍是编辑器无效配置的稳定回退值。
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    // 局部辅助函数把相对单个 BPM 节点的时间换算为结构化拍号和分拍。
    auto calculateWithinBpm = [&](double  pointTime,
                                  double  pointBpm,
                                  int64_t totalBeats,
                                  bool    beforeFirstTiming) {
        // BPM 已归一化为正数，可以安全计算单拍秒数。
        double beatDuration = 60.0 / pointBpm;
        // 首节点之前允许负相对时间，保持预卷时间可表达。
        double timeInBpm = timeSeconds - pointTime;
        double beatFloat = timeInBpm / beatDuration;
        // 小 epsilon 防止理论整数拍因浮点误差落到前一拍。
        int64_t beatOffset = static_cast<int64_t>(std::floor(beatFloat + 1e-6));
        // 小数拍乘当前分拍数后舍入到最近格线。
        double stepFloat = (beatFloat - static_cast<double>(beatOffset)) *
                           static_cast<double>(beatDivisor);
        int    step      = static_cast<int>(std::round(stepFloat));
        if ( step >= beatDivisor ) {
            // 舍入到分拍末端时进位到下一整拍。
            ++beatOffset;
            step = 0;
        }

        // 首节点前保留相对拍偏移，时间线内则加累计拍数并改为一基编号。
        const int64_t beatNumber =
            beforeFirstTiming ? beatOffset : totalBeats + beatOffset + 1;
        if ( step == 0 ) {
            // 整拍在首节点前显示 0/1，正常时间线显示 1/1。
            return CanvasBeatPosition{
                beatNumber, beforeFirstTiming ? 0 : 1, 1, true
            };
        }

        // 非整拍使用最大公约数约分，避免显示 2/4 等冗余分数。
        int divisor = beatDivisor;
        int gcd     = std::gcd(step, divisor);
        return CanvasBeatPosition{
            beatNumber, step / gcd, divisor / gcd, true
        };
    };

    // totalBeats 累计已完整越过的 BPM 分段拍数。
    int64_t totalBeats = 0;
    for ( size_t i = 0; i < points.size(); ++i ) {
        const auto& point = points[i];
        // 最后一个节点的结束时间视为正无穷。
        double nextBpmTime = (i + 1 < points.size())
                                 ? points[i + 1].time
                                 : std::numeric_limits<double>::infinity();
        // 每段独立计算拍长，供定位和累计使用。
        double beatDuration = 60.0 / point.bpm;

        if ( timeSeconds < point.time ) {
            if ( i == 0 ) {
                // 首个 BPM 前仍沿用该 BPM 向负方向外推拍位置。
                return calculateWithinBpm(
                    point.time, point.bpm, totalBeats, true);
            }
            // 有序时间线中不应跳过中间区间，防御异常节点时退出循环。
            break;
        }
        if ( timeSeconds < nextBpmTime ) {
            // 目标落在当前 BPM 分段内，按已累计拍数格式化。
            return calculateWithinBpm(point.time, point.bpm, totalBeats, false);
        }

        // 越过完整分段后，将其持续时间换算为整数拍加入累计值。
        double bpmDuration = nextBpmTime - point.time;
        totalBeats +=
            static_cast<int64_t>(std::round(bpmDuration / beatDuration));
    }

    // 节点时间线异常或无法覆盖目标时返回无效结果。
    return {};
}

/// @brief 使用已经归一化的 BPM 节点格式化拍号 + 分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param points 已按时间排序且 BPM 有效的计时节点。
/// @param beatDivisor 当前分拍数，非正值回退为四。
/// @return 拍号加约分分拍；缺少适用节点时返回秒格式。
inline std::string formatBeatTimeWithPoints(
    double timeSeconds, const std::vector<CanvasTimeFormatBpmPoint>& points,
    int beatDivisor)
{
    const auto position =
        calculateBeatPositionWithPoints(timeSeconds, points, beatDivisor);
    // 原有格式化接口在无法换算时继续回退秒数，保持所有既有调用行为。
    if ( !position.valid ) return fmt::format("{:.3f} s", timeSeconds);
    return fmt::format("{} + {}/{}",
                       position.beatNumber,
                       position.numerator,
                       position.denominator);
}

/// @brief 使用渲染快照格式化拍号 + 分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param snapshot 当前渲染快照，可为空。
/// @return 基于快照 BPM 时间线和当前分拍数的文本。
/// @warning 会提取、排序并去重 BPM 节点，不应在同帧对大量标签重复调用。
inline std::string formatBeatTime(
    double timeSeconds, const Common::Render::RenderSnapshot* snapshot)
{
    // 快照为空时仍使用默认四分拍，节点为空会回退秒格式。
    const auto points      = collectBpmPoints(snapshot);
    const int  beatDivisor = snapshot ? snapshot->currentBeatDivisor : 4;
    return formatBeatTimeWithPoints(timeSeconds, points, beatDivisor);
}

/// @brief 使用独立时间上下文格式化拍号 + 分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param context 已缓存的 BPM 节点和分拍数。
/// @return 不依赖渲染快照的拍号分拍文本。
inline std::string formatBeatTime(double                         timeSeconds,
                                  const CanvasTimeFormatContext& context)
{
    return formatBeatTimeWithPoints(
        timeSeconds, context.bpmPoints, context.beatDivisor);
}

/// @brief 按指定偏好格式化时间。
/// @param timeSeconds 待格式化时间或时长，单位秒。
/// @param preference 时钟、毫秒、拍号或秒格式偏好。
/// @param snapshot 拍号格式需要的渲染快照，其他格式忽略。
/// @return 与偏好对应的显示文本。
inline std::string formatTimeWithPreference(
    double timeSeconds, Config::TimeFormatPreference preference,
    const Common::Render::RenderSnapshot* snapshot)
{
    // 显式枚举分派确保未知值安全回退为三位秒数。
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

/// @brief 从批注表等独立画布上下文计算拍号与分拍位置。
/// @param timeSeconds 待换算时间，单位秒。
/// @param context 已缓存且按时间排序的 BPM 节点与当前分拍数。
/// @return 可直接分列显示的拍号和约分分拍；无 BPM 时结果无效。
/// @warning 可在可见行绘制路径调用，不分配内存且不排序 BPM 节点。
inline CanvasBeatPosition calculateCanvasBeatPosition(
    double timeSeconds, const CanvasTimeFormatContext& context)
{
    return TimeFormatDetail::calculateBeatPositionWithPoints(
        timeSeconds, context.bpmPoints, context.beatDivisor);
}

/// @brief 按编辑器偏好格式化画布时间戳。
/// @param timeSeconds 时间戳，单位秒
/// @param snapshot 当前渲染快照，用于拍号格式查询 BPM 时间线
/// @return 已格式化的时间文本
inline std::string formatCanvasTime(
    double                                timeSeconds,
    const Common::Render::RenderSnapshot* snapshot = nullptr)
{
    // 每次读取当前软件偏好，使设置更改立即反映到画布标签。
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
    // 独立上下文仅用于 Beat 格式，其余格式不需要 BPM 数据。
    const auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    if ( preference == Config::TimeFormatPreference::Beat ) {
        // 复用调用方缓存节点，避免从渲染快照重新收集和排序。
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
    // 两端使用同一快照和当前偏好，保证范围文本格式一致。
    return fmt::format("{} / {}",
                       formatCanvasTime(firstSeconds, snapshot),
                       formatCanvasTime(secondSeconds, snapshot));
}

/// @brief 格式化画布时间长度。
/// @param durationSeconds 时间长度，单位秒
/// @return 已格式化的时间长度文本
inline std::string formatCanvasDuration(double durationSeconds)
{
    // 时长没有绝对 BPM 时间线语义，因此 Beat 偏好回退为秒数。
    auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    if ( preference == Config::TimeFormatPreference::Beat ) {
        // 不尝试用当前拍速近似总时长，避免跨 BPM 项目产生误导。
        preference = Config::TimeFormatPreference::Seconds;
    }
    return TimeFormatDetail::formatTimeWithPreference(
        durationSeconds, preference, nullptr);
}

}  // namespace MMM::UI::Utils
