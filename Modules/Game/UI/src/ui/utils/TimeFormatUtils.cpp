#include "ui/utils/TimeFormatUtils.h"
#include "common/ClockTimeText.h"
#include "common/render/BeatTimeIndex.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "ui/utils/CanvasTimeFormatContext.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace MMM::UI::Utils
{

namespace TimeFormatDetail
{

/// @brief 格式化累计分钟、秒及四位秒小数。
/// @param timeSeconds 可为负值的时间，单位秒。
/// @return MM:SS.ffff 文本，分钟可超过 60，负零在舍入后归零。
/// @note 使用共享整数拆分规则，保持状态栏、判定线和导出时间一致。
std::string formatClock(double timeSeconds)
{
    // UI 使用字符串接口，底层格式化仍与无分配的画布时间组件共用实现。
    return Common::formatClockTime(timeSeconds).data();
}

/// @brief 格式化纯毫秒。
/// @param timeSeconds 时间或时长，单位秒。
/// @return 四舍五入到整数毫秒并附加单位的文本。
std::string formatMilliseconds(double timeSeconds)
{
    // 直接对有符号秒数换算，保留负时间方向。
    return fmt::format(
        "{} ms", static_cast<int64_t>(std::llround(timeSeconds * 1000.0)));
}

namespace
{
/// @brief 将目标时间换算为已定位分段内的拍号和约分位置。
/// @param withinTime 从分段起点开始的相对秒数；首节点之前为负。
/// @param bpm 已归一化的分段 BPM。
/// @param completedBeats 段首前已经完成的整拍数。
/// @param beatDivisor 格线分拍数，非正配置使用四分拍。
/// @return 拍号和分拍，时间或拍号不可表示时结果无效。
/// @warning 画布热路径的常量时间运算，不读取注册表或分配节点。
CanvasBeatPosition calculateWithinBpm(double withinTime, double bpm,
                                      std::int64_t completedBeats,
                                      int          beatDivisor)
{
    if ( beatDivisor <= 0 ) beatDivisor = 4;
    const double beatFloat = withinTime / (60.0 / bpm);
    // epsilon 抑制理论整拍因浮点误差落到前一拍。
    const double offset = std::floor(beatFloat + 1e-6);
    // 用精确的 2^63 作为排除边界；直接把 INT64_MAX 转成 double 会向上舍入。
    if ( !std::isfinite(offset) || offset < -0x1p63 || offset >= 0x1p63 )
        return {};
    auto beatOffset = static_cast<std::int64_t>(offset);
    // 分拍也可能四舍五入到下一拍。保留到进位处理后再转成窄整数。
    const double roundedStep =
        std::round((beatFloat - offset) * static_cast<double>(beatDivisor));
    int step = 0;
    if ( roundedStep >= beatDivisor ) {
        ++beatOffset;
    } else {
        step = static_cast<int>(roundedStep);
    }

    const bool beforeFirstTiming = withinTime < 0.0;
    // 正常时间线从一编号，首节点之前保留零或负拍。
    if ( !beforeFirstTiming &&
         beatOffset >=
             std::numeric_limits<std::int64_t>::max() - completedBeats )
        return {};
    const auto beatNumber =
        beforeFirstTiming ? beatOffset : completedBeats + beatOffset + 1;
    if ( step == 0 ) return { beatNumber, beforeFirstTiming ? 0 : 1, 1, true };

    // 约分使同一个格线位置在不同分拍配置下具有相同的文字表示。
    const int gcd = std::gcd(step, beatDivisor);
    return { beatNumber, step / gcd, beatDivisor / gcd, true };
}

/// @brief 将已经校验的拍位置表示为 UI 时间文本。
/// @return 无可用拍位置时按四位小数显示秒。
/// @warning 每个可见时间标签调用一次；不重新查找或遍历 BPM 节点。
std::string formatBeatPosition(double timeSeconds, CanvasBeatPosition position)
{
    if ( !position.valid ) return fmt::format("{:.4f} s", timeSeconds);
    return fmt::format("{} + {}/{}",
                       position.beatNumber,
                       position.numerator,
                       position.denominator);
}
}  // namespace

/// @brief 使用已经归一化的 BPM 节点计算拍号与分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param points 已按时间排序且 BPM 有效的计时节点。
/// @param beatDivisor 当前分拍数，非正值回退为四。
/// @return 拍号与约分后的分拍；缺少适用节点时返回无效结果。
CanvasBeatPosition calculateBeatPositionWithPoints(
    double timeSeconds, const std::vector<CanvasTimeFormatBpmPoint>& points,
    int beatDivisor)
{
    // 没有 BPM 时间线时无法计算拍位置，由调用方决定占位或回退格式。
    if ( points.empty() || !std::isfinite(timeSeconds) ) return {};

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
                return calculateWithinBpm(timeSeconds - point.time,
                                          point.bpm,
                                          totalBeats,
                                          beatDivisor);
            }
            // 有序时间线中不应跳过中间区间，防御异常节点时退出循环。
            break;
        }
        if ( timeSeconds < nextBpmTime ) {
            // 目标落在当前 BPM 分段内，按已累计拍数格式化。
            return calculateWithinBpm(
                timeSeconds - point.time, point.bpm, totalBeats, beatDivisor);
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
std::string formatBeatTimeWithPoints(
    double timeSeconds, const std::vector<CanvasTimeFormatBpmPoint>& points,
    int beatDivisor)
{
    return formatBeatPosition(
        timeSeconds,
        calculateBeatPositionWithPoints(timeSeconds, points, beatDivisor));
}

/// @brief 使用渲染快照格式化拍号 + 分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param snapshot 当前渲染快照，可为空。
/// @return 基于快照 BPM 时间线和当前分拍数的文本。
/// @warning UI 热路径：借用当前 BPM 修订，仅作二分查询和格式化。
std::string formatBeatTime(double                                timeSeconds,
                           const Common::Render::RenderSnapshot* snapshot)
{
    // 预览和状态栏与时间线共用索引，即使快照不需要完整滚动分段也可换算。
    const auto* index = snapshot ? snapshot->m_beatTimeIndex.get() : nullptr;
    if ( !index || index->m_points.empty() || !std::isfinite(timeSeconds) )
        return formatBeatPosition(timeSeconds, {});

    const auto& points = index->m_points;
    // upper_bound 使下一节点起始时刻立即进入新 BPM，并保留首节点前的外推。
    const auto next = std::upper_bound(
        points.begin(),
        points.end(),
        timeSeconds,
        [](double time, const auto& point) { return time < point.m_time; });
    const auto& point =
        next == points.begin() ? points.front() : *std::prev(next);
    return formatBeatPosition(timeSeconds,
                              calculateWithinBpm(timeSeconds - point.m_time,
                                                 point.m_bpm,
                                                 point.m_completedBeats,
                                                 snapshot->currentBeatDivisor));
}

/// @brief 使用独立时间上下文格式化拍号 + 分拍位。
/// @param timeSeconds 待格式化时间，单位秒。
/// @param context 已缓存的 BPM 节点和分拍数。
/// @return 不依赖渲染快照的拍号分拍文本。
std::string formatBeatTime(double                         timeSeconds,
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
std::string formatTimeWithPreference(
    double timeSeconds, Config::TimeFormatPreference preference,
    const Common::Render::RenderSnapshot* snapshot)
{
    // 显式枚举分派使未知偏好回退为带四位小数的秒数。
    // 状态栏和各画布可逐帧调用；Beat 分支借用快照索引，只二分定位当前 BPM。
    switch ( preference ) {
    case Config::TimeFormatPreference::Clock: return formatClock(timeSeconds);
    case Config::TimeFormatPreference::Milliseconds:
        return formatMilliseconds(timeSeconds);
    case Config::TimeFormatPreference::Beat:
        return formatBeatTime(timeSeconds, snapshot);
    case Config::TimeFormatPreference::Seconds:
    default: return fmt::format("{:.4f} s", timeSeconds);
    }
}

}  // namespace TimeFormatDetail

/// @brief 从批注表等独立画布上下文计算拍号与分拍位置。
/// @param timeSeconds 待换算时间，单位秒。
/// @param context 已缓存且按时间排序的 BPM 节点与当前分拍数。
/// @return 可直接分列显示的拍号和约分分拍；无 BPM 时结果无效。
/// @warning 可在可见行绘制路径调用，不分配内存且不排序 BPM 节点。
CanvasBeatPosition calculateCanvasBeatPosition(
    double timeSeconds, const CanvasTimeFormatContext& context)
{
    return TimeFormatDetail::calculateBeatPositionWithPoints(
        timeSeconds, context.bpmPoints, context.beatDivisor);
}

/// @brief 按编辑器偏好格式化画布时间戳。
/// @param timeSeconds 时间戳，单位秒
/// @param snapshot 当前渲染快照，用于拍号格式查询 BPM 时间线
/// @return 已格式化的时间文本
/// @warning 画布每帧调用；拍号模式仅二分同一修订的索引。
std::string formatCanvasTime(double                                timeSeconds,
                             const Common::Render::RenderSnapshot* snapshot)
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
std::string formatCanvasTime(double                         timeSeconds,
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
/// @warning 状态栏每帧调用；两端共用一次偏好读取与同一份 BPM 索引。
std::string formatCanvasTimePair(double firstSeconds, double secondSeconds,
                                 const Common::Render::RenderSnapshot* snapshot)
{
    // 两端使用同一快照和当前偏好，保证范围文本格式一致。
    const auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    return fmt::format("{} / {}",
                       TimeFormatDetail::formatTimeWithPreference(
                           firstSeconds, preference, snapshot),
                       TimeFormatDetail::formatTimeWithPreference(
                           secondSeconds, preference, snapshot));
}

/// @brief 格式化画布时间长度。
/// @param durationSeconds 时间长度，单位秒
/// @return 已格式化的时间长度文本
std::string formatCanvasDuration(double durationSeconds)
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
