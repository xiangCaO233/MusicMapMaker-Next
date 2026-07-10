#include "canvas/TimelineCanvas.h"

#include "canvas/MarqueeAutoScroll.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "ui/imgui/ClipboardBridge.h"
#include "ui/imgui/ShortcutUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <optional>

namespace MMM::Canvas
{
namespace
{
/// @brief Timeline Timing 拾取半径，单位像素。
constexpr float TIMING_PICK_RADIUS = 16.0f;

/// @brief Timeline Timing 标记尺寸，单位像素。
constexpr float TIMING_MARKER_SIZE = 20.0f;

/// @brief Timeline Timing 标记边距，单位像素。
constexpr float TIMING_MARKER_PADDING = 5.0f;

/// @brief 将 Timing 类型转换为对应的快照效果掩码。
uint32_t getTimingEffectMask(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return Logic::System::SCROLL_EFFECT_BPM;
    case ::MMM::TimingEffect::SCROLL:
        return Logic::System::SCROLL_EFFECT_SCROLL;
    case ::MMM::TimingEffect::JUMP: return Logic::System::SCROLL_EFFECT_JUMP;
    case ::MMM::TimingEffect::HS: return Logic::System::SCROLL_EFFECT_HS;
    }
    return 0;
}

/// @brief 取 Timeline 元素中指定类型的实体。
entt::entity getTimingEntity(const Logic::TimelineInteractiveElement& element,
                             ::MMM::TimingEffect                      effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return element.bpmEntity;
    case ::MMM::TimingEffect::SCROLL: return element.scrollEntity;
    case ::MMM::TimingEffect::JUMP: return element.jumpEntity;
    case ::MMM::TimingEffect::HS: return element.hsEntity;
    }
    return entt::null;
}

/// @brief 取 Timeline 元素中指定类型的原始值。
double getTimingValue(const Logic::TimelineInteractiveElement& element,
                      ::MMM::TimingEffect                      effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return element.bpmValue;
    case ::MMM::TimingEffect::SCROLL: return element.scrollValue;
    case ::MMM::TimingEffect::JUMP: return element.jumpValue;
    case ::MMM::TimingEffect::HS: return element.hsValue;
    }
    return 0.0;
}

/// @brief 取 Timeline 元素中指定类型的 marker 几何范围。
const Logic::TimelineInteractiveElement::MarkerGeometry&
getTimingMarkerGeometry(const Logic::TimelineInteractiveElement& element,
                        ::MMM::TimingEffect                      effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return element.bpmMarker;
    case ::MMM::TimingEffect::SCROLL: return element.scrollMarker;
    case ::MMM::TimingEffect::JUMP: return element.jumpMarker;
    case ::MMM::TimingEffect::HS: return element.hsMarker;
    }
    return element.scrollMarker;
}

/// @brief Timeline 画布中按类型排列的 Timing 类型列表。
constexpr ::MMM::TimingEffect TIMELINE_EFFECT_ORDER[] = {
    ::MMM::TimingEffect::BPM,
    ::MMM::TimingEffect::JUMP,
    ::MMM::TimingEffect::HS,
    ::MMM::TimingEffect::SCROLL,
};

/// @brief 获取专业模式中指定 Timing 类型所属的轨道索引。
int professionalTimingLane(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return 1;
    case ::MMM::TimingEffect::SCROLL: return 2;
    case ::MMM::TimingEffect::JUMP: return 3;
    case ::MMM::TimingEffect::HS: return 4;
    }
    return 1;
}

/// @brief 将专业模式轨道位置转换为创建弹窗类型索引。
/// @param size 当前 Timeline 画布尺寸。
/// @param localMouseX 鼠标相对画布左上角的 X 坐标。
/// @return 可创建 Timing 的轨道返回类型索引；BGM 或越界轨道返回空。
std::optional<int> professionalCreateTypeAtX(const ImVec2& size,
                                             float         localMouseX)
{
    if ( size.x <= 1.0f || localMouseX < 0.0f || localMouseX > size.x ) {
        return std::nullopt;
    }

    constexpr int laneCount = 5;
    const float   laneWidth = size.x / static_cast<float>(laneCount);
    int           lane =
        static_cast<int>(std::floor(localMouseX / std::max(1.0f, laneWidth)));
    lane = std::clamp(lane, 0, laneCount - 1);
    switch ( lane ) {
    case 1: return 0;
    case 2: return 1;
    case 3: return 2;
    case 4: return 3;
    case 0:
    default: return std::nullopt;
    }
}

/// @brief Timeline 复制粘贴按 beat 换算使用的 BPM 锚点。
struct TimelineClipboardBeatPoint {
    double time{ 0.0 };   ///< BPM 时间点，单位秒
    double bpm{ 120.0 };  ///< 当前段 BPM
    double beat{ 0.0 };   ///< 该时间点对应的连续 beat
};

/// @brief Timeline 复制粘贴按 beat 换算使用的 BPM 时间线。
using TimelineClipboardBeatTimeline = std::vector<TimelineClipboardBeatPoint>;

/// @brief 规整 Timeline 复制粘贴换算用的 BPM 值。
double sanitizeTimelineClipboardBpm(double bpm, double fallbackBpm)
{
    if ( std::isfinite(bpm) && bpm > 0.0 ) {
        return std::min(bpm, 10000.0);
    }
    if ( std::isfinite(fallbackBpm) && fallbackBpm > 0.0 ) {
        return std::min(fallbackBpm, 10000.0);
    }
    return 120.0;
}

/// @brief 取得 Timeline 快照中的有效回退 BPM。
double timelineClipboardFallbackBpm(const Logic::RenderSnapshot& snapshot)
{
    return sanitizeTimelineClipboardBpm(snapshot.fallbackBpm, 120.0);
}

/// @brief 从 Timeline 当前快照构建可双向换算的连续 beat 时间线。
/// @param snapshot 当前渲染快照。
/// @return 按时间排序并去重后的 BPM/beat 锚点列表。
TimelineClipboardBeatTimeline buildTimelineClipboardBeatTimeline(
    const Logic::RenderSnapshot& snapshot)
{
    struct BpmEvent {
        double time{ 0.0 };   ///< BPM 时间点，单位秒
        double bpm{ 120.0 };  ///< BPM 值
    };

    const double          fallbackBpm = timelineClipboardFallbackBpm(snapshot);
    std::vector<BpmEvent> bpmEvents;
    bpmEvents.reserve(snapshot.scrollSegments.size());
    for ( const auto& segment : snapshot.scrollSegments ) {
        if ( (segment.effects & Logic::System::SCROLL_EFFECT_BPM) == 0 ||
             !std::isfinite(segment.time) ) {
            continue;
        }

        bpmEvents.push_back(
            { segment.time,
              sanitizeTimelineClipboardBpm(segment.bpmValue, fallbackBpm) });
    }
    std::stable_sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.time < rhs.time; });

    TimelineClipboardBeatTimeline timeline;
    timeline.reserve(bpmEvents.size());
    for ( const auto& event : bpmEvents ) {
        if ( !timeline.empty() &&
             std::abs(timeline.back().time - event.time) < 1e-6 ) {
            timeline.back().bpm = event.bpm;
            continue;
        }

        if ( timeline.empty() ) {
            timeline.push_back({ event.time, event.bpm, 0.0 });
            continue;
        }

        const auto&  previous = timeline.back();
        const double beat =
            previous.beat + (event.time - previous.time) * previous.bpm / 60.0;
        timeline.push_back({ event.time, event.bpm, beat });
    }
    return timeline;
}

/// @brief 将秒时间转换为连续 beat 位置。
double timelineClipboardTimeToBeat(
    const TimelineClipboardBeatTimeline& timeline, double time,
    double fallbackBpm)
{
    if ( !std::isfinite(time) ) return 0.0;

    const double bpm = sanitizeTimelineClipboardBpm(fallbackBpm, 120.0);
    if ( timeline.empty() ) {
        return time * bpm / 60.0;
    }

    auto it = std::upper_bound(
        timeline.begin(),
        timeline.end(),
        time,
        [](double value, const TimelineClipboardBeatPoint& point) {
            return value < point.time;
        });
    const auto& point = it == timeline.begin() ? timeline.front() : *(it - 1);
    return point.beat + (time - point.time) * point.bpm / 60.0;
}

/// @brief 将连续 beat 位置转换为秒时间。
double timelineClipboardBeatToTime(
    const TimelineClipboardBeatTimeline& timeline, double beat,
    double fallbackBpm)
{
    if ( !std::isfinite(beat) ) return 0.0;

    const double bpm = sanitizeTimelineClipboardBpm(fallbackBpm, 120.0);
    if ( timeline.empty() ) {
        return beat * 60.0 / bpm;
    }

    auto it = std::upper_bound(
        timeline.begin(),
        timeline.end(),
        beat,
        [](double value, const TimelineClipboardBeatPoint& point) {
            return value < point.beat;
        });
    const auto& point = it == timeline.begin() ? timeline.front() : *(it - 1);
    return point.time + (beat - point.beat) * 60.0 / point.bpm;
}

/// @brief 将创建弹窗索引转换为 Timing 类型。
/// @param createType 创建弹窗中的类型索引。
/// @return 对应的 Timing 类型。
::MMM::TimingEffect timingEffectFromCreateType(int createType)
{
    switch ( createType ) {
    case 0: return ::MMM::TimingEffect::BPM;
    case 2: return ::MMM::TimingEffect::JUMP;
    case 3: return ::MMM::TimingEffect::HS;
    case 1:
    default: return ::MMM::TimingEffect::SCROLL;
    }
}

/// @brief 获取指定 Timing 类型在创建弹窗中的默认参数。
/// @param effect Timing 类型。
/// @return 新建该类型 Timing 时使用的默认参数。
double defaultTimingCreateValue(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return 120.0;
    case ::MMM::TimingEffect::SCROLL: return 1.0;
    case ::MMM::TimingEffect::JUMP: return 1000.0;
    case ::MMM::TimingEffect::HS: return 1.0;
    }
    return 1.0;
}
}  // namespace

/// @brief 根据画布本地 Y 坐标换算谱面时间。
/// @param size 当前 Timeline 画布尺寸。
/// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
/// @return 换算出的谱面时间，单位秒。
double TimelineCanvas::canvasTimeAtLocalY(const ImVec2& size,
                                          float         localMouseY) const
{
    if ( !m_currentSnapshot ) {
        return 0.0;
    }

    auto&       visual        = Config::AppConfig::instance().getVisualConfig();
    float       judgmentLineY = size.y * visual.judgeline_pos;
    const float compensatedMouseY = localMouseY - m_lastAppliedYOffset;
    const auto& segments          = m_currentSnapshot->scrollSegments;
    if ( segments.empty() ) {
        float  zoom  = visual.timelineZoom;
        double speed = 500.0 * zoom;
        return (judgmentLineY - compensatedMouseY) / speed +
               m_currentSnapshot->currentTime;
    }

    auto it = std::upper_bound(
        segments.begin(),
        segments.end(),
        m_currentSnapshot->currentTime,
        [](double val, const Logic::System::ScrollSegment& seg) {
            return val < seg.time;
        });
    double currentAbsY = 0.0;
    if ( it == segments.begin() ) {
        currentAbsY = segments[0].absY +
                      (m_currentSnapshot->currentTime - segments[0].time) *
                          segments[0].speed;
    } else {
        auto prev = std::prev(it);
        currentAbsY =
            prev->absY +
            (m_currentSnapshot->currentTime - prev->time) * prev->speed;
    }

    double targetAbsY = currentAbsY + (judgmentLineY - compensatedMouseY);
    auto   itTime = std::lower_bound(segments.begin(),
                                     segments.end(),
                                     targetAbsY,
                                     [](const Logic::System::ScrollSegment& seg,
                                        double val) { return seg.absY < val; });

    if ( itTime == segments.begin() ) {
        if ( std::abs(segments[0].speed) < 1e-6 ) {
            return segments[0].time;
        }
        return segments[0].time +
               (targetAbsY - segments[0].absY) / segments[0].speed;
    }

    auto prev = std::prev(itTime);
    if ( std::abs(prev->speed) < 1e-6 ) {
        return prev->time;
    }
    return prev->time + (targetAbsY - prev->absY) / prev->speed;
}

/// @brief 根据谱面时间换算 Timeline 画布本地 Y 坐标。
/// @param size 当前 Timeline 画布尺寸。
/// @param time 谱面时间，单位秒。
/// @return Timeline 画布内 Y 坐标。
double TimelineCanvas::canvasYAtTime(const ImVec2& size, double time) const
{
    if ( !m_currentSnapshot ) {
        return 0.0;
    }

    auto&       visual        = Config::AppConfig::instance().getVisualConfig();
    float       judgmentLineY = size.y * visual.judgeline_pos;
    const auto& segments      = m_currentSnapshot->scrollSegments;
    if ( segments.empty() ) {
        float  zoom  = visual.timelineZoom;
        double speed = 500.0 * zoom;
        return judgmentLineY - (time - m_currentSnapshot->currentTime) * speed +
               m_lastAppliedYOffset;
    }

    auto resolveAbsYAndHs = [&](double queryTime) {
        auto it = std::upper_bound(
            segments.begin(),
            segments.end(),
            queryTime,
            [](double val, const Logic::System::ScrollSegment& seg) {
                return val < seg.time;
            });

        const auto* seg =
            it == segments.begin() ? &segments.front() : &(*std::prev(it));
        double absY = seg->absY;
        if ( std::abs(seg->speed) > 1e-9 ) {
            absY += (queryTime - seg->time) * seg->speed;
        }
        return std::pair{ absY, seg->hs };
    };

    const auto [currentAbsY, unusedHs] =
        resolveAbsYAndHs(m_currentSnapshot->currentTime);
    const auto [targetAbsY, targetHs] = resolveAbsYAndHs(time);
    (void)unusedHs;
    return judgmentLineY - (targetAbsY - currentAbsY) * targetHs +
           m_lastAppliedYOffset;
}

/// @brief 将时间吸附到附近已有 Timing 事件或分拍网格。
/// @param size 当前 Timeline 画布尺寸。
/// @param rawTime 未吸附的谱面时间，单位秒。
/// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
/// @param snapped 输出是否发生吸附。
/// @return 吸附后的谱面时间，单位秒。
double TimelineCanvas::snapTimingTime(const ImVec2& size, double rawTime,
                                      float localMouseY, bool& snapped) const
{
    snapped = false;
    if ( !m_currentSnapshot || !std::isfinite(rawTime) ) {
        return rawTime;
    }

    const auto& appConfig    = Config::AppConfig::instance();
    const auto& editorConfig = appConfig.getEditorConfig();
    const auto& visual       = appConfig.getVisualConfig();
    float       minItemDist  = visual.snapThreshold;
    double      result       = rawTime;
    for ( const auto& element : m_currentSnapshot->timelineElements ) {
        float distance = std::abs(element.y - localMouseY);
        if ( distance < minItemDist ) {
            result      = element.time;
            snapped     = true;
            minItemDist = distance;
        }
    }

    struct BpmSnapPoint {
        /// @brief BPM 段起始时间，单位秒。
        double time{ 0.0 };

        /// @brief BPM 值。
        double bpm{ 120.0 };
    };

    std::vector<BpmSnapPoint> bpmPoints;
    bpmPoints.reserve(m_currentSnapshot->scrollSegments.size());
    for ( const auto& segment : m_currentSnapshot->scrollSegments ) {
        if ( (segment.effects & Logic::System::SCROLL_EFFECT_BPM) == 0 ) {
            continue;
        }

        double bpm = segment.bpmValue;
        if ( bpm <= 0.0 || !std::isfinite(bpm) ) {
            bpm = 120.0;
        }
        bpm = std::min(bpm, 10000.0);

        if ( !bpmPoints.empty() &&
             std::abs(bpmPoints.back().time - segment.time) < 1e-6 ) {
            bpmPoints.back().bpm = bpm;
        } else {
            bpmPoints.push_back({ segment.time, bpm });
        }
    }

    if ( bpmPoints.empty() ) {
        return result;
    }

    int beatDivisor = editorConfig.settings.beatDivisor;
    if ( beatDivisor <= 0 ) {
        beatDivisor = 4;
    }

    const bool allowBeforeFirstTiming = visual.drawBeatLinesBeforeFirstTiming;
    if ( rawTime < bpmPoints.front().time && !allowBeforeFirstTiming ) {
        return result;
    }

    bool   hasBeatCandidate = false;
    double beatResult       = rawTime;
    float  beatDistance     = std::numeric_limits<float>::max();
    for ( size_t i = 0; i < bpmPoints.size(); ++i ) {
        const auto& point       = bpmPoints[i];
        double      nextBpmTime = (i + 1 < bpmPoints.size())
                                      ? bpmPoints[i + 1].time
                                      : std::numeric_limits<double>::infinity();

        if ( rawTime < point.time && i > 0 ) {
            continue;
        }
        if ( rawTime > nextBpmTime ) {
            continue;
        }

        double beatDuration = 60.0 / point.bpm;
        double stepDuration = beatDuration / static_cast<double>(beatDivisor);
        if ( stepDuration <= 1e-9 || !std::isfinite(stepDuration) ) {
            continue;
        }

        double relativeTime = rawTime - point.time;
        double stepCount = editorConfig.settings.snapFloor
                               ? std::floor(relativeTime / stepDuration + 1e-6)
                               : std::round(relativeTime / stepDuration);
        double nearestStepTime = point.time + stepCount * stepDuration;
        if ( nearestStepTime > nextBpmTime ) {
            nearestStepTime = nextBpmTime;
        }
        nearestStepTime = std::max(0.0, nearestStepTime);
        if ( !std::isfinite(nearestStepTime) ) {
            continue;
        }

        float y = static_cast<float>(canvasYAtTime(size, nearestStepTime));
        float distance = std::abs(y - localMouseY);
        if ( editorConfig.settings.scrollSnap ||
             distance <= visual.snapThreshold ) {
            if ( distance < beatDistance ) {
                hasBeatCandidate = true;
                beatResult       = nearestStepTime;
                beatDistance     = distance;
            }
        }
    }

    if ( hasBeatCandidate &&
         (editorConfig.settings.scrollSnap || beatDistance <= minItemDist) ) {
        result  = beatResult;
        snapped = true;
    }
    return result;
}

/// @brief 将显示时间按当前分拍规则吸附到拍线。
/// @param rawTime 未吸附的显示时间，单位秒。
/// @return 吸附后的显示时间；无可用 BPM 时返回原时间。
/// @warning UI 热路径：仅在 Shift 拖动总时间进度条时调用；只读取当前快照。
double TimelineCanvas::snapTimeToBeatLine(double rawTime) const
{
    if ( !m_currentSnapshot || !std::isfinite(rawTime) ) {
        return rawTime;
    }

    const auto& appConfig    = Config::AppConfig::instance();
    const auto& editorConfig = appConfig.getEditorConfig();
    const auto& visual       = appConfig.getVisualConfig();

    struct BpmSnapPoint {
        /// @brief BPM 段起始时间，单位秒。
        double time{ 0.0 };

        /// @brief BPM 值。
        double bpm{ 120.0 };
    };

    std::vector<BpmSnapPoint> bpmPoints;
    bpmPoints.reserve(m_currentSnapshot->scrollSegments.size());
    for ( const auto& segment : m_currentSnapshot->scrollSegments ) {
        if ( (segment.effects & Logic::System::SCROLL_EFFECT_BPM) == 0 ) {
            continue;
        }

        double bpm = segment.bpmValue;
        if ( bpm <= 0.0 || !std::isfinite(bpm) ) {
            bpm = m_currentSnapshot->fallbackBpm;
        }
        if ( bpm <= 0.0 || !std::isfinite(bpm) ) {
            bpm = 120.0;
        }
        bpm = std::min(bpm, 10000.0);

        if ( !bpmPoints.empty() &&
             std::abs(bpmPoints.back().time - segment.time) < 1e-6 ) {
            bpmPoints.back().bpm = bpm;
        } else {
            bpmPoints.push_back({ segment.time, bpm });
        }
    }

    if ( bpmPoints.empty() ) {
        return rawTime;
    }

    int beatDivisor = editorConfig.settings.beatDivisor;
    if ( beatDivisor <= 0 ) {
        beatDivisor = 4;
    }

    if ( rawTime < bpmPoints.front().time &&
         !visual.drawBeatLinesBeforeFirstTiming ) {
        return rawTime;
    }

    double nearestTime     = rawTime;
    double nearestDistance = std::numeric_limits<double>::max();
    for ( size_t i = 0; i < bpmPoints.size(); ++i ) {
        const auto& point       = bpmPoints[i];
        double      nextBpmTime = (i + 1 < bpmPoints.size())
                                      ? bpmPoints[i + 1].time
                                      : std::numeric_limits<double>::infinity();

        if ( rawTime < point.time && i > 0 ) {
            continue;
        }
        if ( rawTime > nextBpmTime ) {
            continue;
        }

        double beatDuration = 60.0 / point.bpm;
        double stepDuration = beatDuration / static_cast<double>(beatDivisor);
        if ( stepDuration <= 1e-9 || !std::isfinite(stepDuration) ) {
            continue;
        }

        double relativeTime = rawTime - point.time;
        double stepCount = editorConfig.settings.snapFloor
                               ? std::floor(relativeTime / stepDuration + 1e-6)
                               : std::round(relativeTime / stepDuration);
        double candidate = point.time + stepCount * stepDuration;
        if ( candidate > nextBpmTime ) {
            candidate = nextBpmTime;
        }
        candidate = std::max(0.0, candidate);
        if ( !std::isfinite(candidate) ) {
            continue;
        }

        double distance = std::abs(candidate - rawTime);
        if ( distance < nearestDistance ) {
            nearestTime     = candidate;
            nearestDistance = distance;
        }
    }

    return nearestTime;
}

/// @brief 在指定画布 Y 坐标处准备并打开 Timing 创建弹窗。
/// @param size 当前 Timeline 画布尺寸。
/// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
/// @param localMouseX 鼠标相对画布左上角的 X 坐标。
/// @param useCurrentTime 是否使用当前播放时间而非鼠标命中时间。
void TimelineCanvas::openTimingCreatePopupAtY(const ImVec2& size,
                                              float         localMouseY,
                                              float         localMouseX,
                                              bool          useCurrentTime)
{
    if ( !m_currentSnapshot ) {
        return;
    }

    if ( Config::AppConfig::instance()
             .getEditorSettings()
             .timelineProfessionalMode ) {
        auto createType = professionalCreateTypeAtX(size, localMouseX);
        if ( !createType ) {
            return;
        }
        m_createType = *createType;
    }

    m_createTimeRaw = canvasTimeAtLocalY(size, localMouseY);
    bool snapped    = false;
    m_createTimeSnapped =
        snapTimingTime(size, m_createTimeRaw, localMouseY, snapped);
    m_isTimeSnapped = snapped;

    m_createPosType    = useCurrentTime ? 1 : 0;
    m_createTimeManual = useCurrentTime ? m_currentSnapshot->currentTime
                                        : (m_isTimeSnapped ? m_createTimeSnapped
                                                           : m_createTimeRaw);
    m_createValue =
        defaultTimingCreateValue(timingEffectFromCreateType(m_createType));
    m_isCreatePopupOpen = true;
    ImGui::OpenPopup("TimelineCreateEvent");
}

/// @brief 收集当前快照中可交互的 Timing 目标。
/// @return 当前可见 Timing 目标列表。
std::vector<TimelineCanvas::TimelineHitTarget>
TimelineCanvas::collectVisibleTimingTargets() const
{
    std::vector<TimelineHitTarget> targets;
    if ( !m_currentSnapshot ) {
        return targets;
    }

    targets.reserve(m_currentSnapshot->timelineElements.size() * 2U);
    for ( const auto& element : m_currentSnapshot->timelineElements ) {
        for ( auto effect : TIMELINE_EFFECT_ORDER ) {
            if ( (element.effects & getTimingEffectMask(effect)) == 0 ) {
                continue;
            }
            entt::entity entity = getTimingEntity(element, effect);
            if ( entity == entt::null ) {
                continue;
            }

            const auto& markerGeometry =
                getTimingMarkerGeometry(element, effect);
            const bool hasMarkerGeometry =
                markerGeometry.hasMarkerGeometry || element.hasMarkerGeometry;
            targets.push_back({ entity,
                                effect,
                                element.time,
                                getTimingValue(element, effect),
                                element.y,
                                hasMarkerGeometry,
                                markerGeometry.hasMarkerGeometry
                                    ? markerGeometry.markerVertexOffset
                                    : element.markerVertexOffset,
                                markerGeometry.hasMarkerGeometry
                                    ? markerGeometry.markerVertexCount
                                    : element.markerVertexCount,
                                markerGeometry.hasMarkerGeometry
                                    ? markerGeometry.markerIndexOffset
                                    : element.markerIndexOffset,
                                markerGeometry.hasMarkerGeometry
                                    ? markerGeometry.markerIndexCount
                                    : element.markerIndexCount });
        }
    }
    return targets;
}

/// @brief 判断指定 Timing 目标是否允许被时间线选择操作选中。
/// @param target Timing 目标。
/// @return 允许选中时返回 true。
bool TimelineCanvas::isTimingTargetSelectable(
    const TimelineHitTarget& target) const
{
    if ( target.entity == entt::null ) {
        return false;
    }
    if ( target.effect != ::MMM::TimingEffect::BPM ) {
        return true;
    }
    return Config::AppConfig::instance()
        .getEditorSettings()
        .timelineSelectionIncludesBpm;
}

/// @brief 将单个 Timing 目标转换为显示用 X 坐标。
/// @param target Timing 目标。
/// @param canvasPos 画布左上角屏幕坐标。
/// @param size 当前 Timeline 画布尺寸。
/// @return 目标中心 X 坐标。
float TimelineCanvas::timingTargetCenterX(const TimelineHitTarget& target,
                                          const ImVec2&            canvasPos,
                                          const ImVec2&            size) const
{
    if ( Config::AppConfig::instance()
             .getEditorSettings()
             .timelineProfessionalMode ) {
        constexpr float laneCount = 5.0f;
        const int       lane      = professionalTimingLane(target.effect);
        return canvasPos.x +
               size.x * (static_cast<float>(lane) + 0.5f) / laneCount;
    }

    float x = canvasPos.x + TIMING_MARKER_PADDING + TIMING_MARKER_SIZE * 0.5f;
    if ( target.effect == ::MMM::TimingEffect::SCROLL ) {
        x = canvasPos.x + size.x - TIMING_MARKER_PADDING -
            TIMING_MARKER_SIZE * 0.5f;
    } else if ( target.effect == ::MMM::TimingEffect::JUMP ) {
        x = canvasPos.x + TIMING_MARKER_PADDING +
            (size.x - TIMING_MARKER_SIZE - 2.0f * TIMING_MARKER_PADDING) *
                0.33f +
            TIMING_MARKER_SIZE * 0.5f;
    } else if ( target.effect == ::MMM::TimingEffect::HS ) {
        x = canvasPos.x + TIMING_MARKER_PADDING +
            (size.x - TIMING_MARKER_SIZE - 2.0f * TIMING_MARKER_PADDING) *
                0.66f +
            TIMING_MARKER_SIZE * 0.5f;
    }
    return x;
}

/// @brief 计算 Timing 目标当前可视 marker 的屏幕空间 hitbox。
/// @param target Timing 目标。
/// @param canvasPos 画布左上角屏幕坐标。
/// @param size 当前 Timeline 画布尺寸。
/// @return 可用于拾取和框选的屏幕空间矩形。
/// @warning UI 热路径：拾取、hover
/// 和框选时调用；只读取当前快照顶点缓存并做常量矩形计算。
TimelineCanvas::TimingSelectionRect TimelineCanvas::timingTargetScreenRect(
    const TimelineHitTarget& target, const ImVec2& canvasPos,
    const ImVec2& size) const
{
    auto makeRect = [](float left, float top, float right, float bottom) {
        TimingSelectionRect rect;
        rect.left   = std::min(left, right);
        rect.right  = std::max(left, right);
        rect.top    = std::min(top, bottom);
        rect.bottom = std::max(top, bottom);
        rect.valid  = rect.right > rect.left && rect.bottom > rect.top;
        return rect;
    };
    auto mergeRect = [](TimingSelectionRect lhs, TimingSelectionRect rhs) {
        if ( !lhs.valid ) return rhs;
        if ( !rhs.valid ) return lhs;
        return TimingSelectionRect{
            std::min(lhs.left, rhs.left),
            std::min(lhs.top, rhs.top),
            std::max(lhs.right, rhs.right),
            std::max(lhs.bottom, rhs.bottom),
            true,
        };
    };

    const float centerX = timingTargetCenterX(target, canvasPos, size);
    const float centerY = canvasPos.y + target.y;
    const float halfSize =
        std::max(TIMING_MARKER_SIZE, TIMING_PICK_RADIUS) * 0.5f;
    const auto fallbackRect = makeRect(centerX - halfSize,
                                       centerY - halfSize,
                                       centerX + halfSize,
                                       centerY + halfSize);

    if ( m_currentSnapshot && target.hasMarkerGeometry &&
         target.markerVertexCount > 0U ) {
        const uint32_t startVertex = target.markerVertexOffset;
        const uint32_t endVertex =
            std::min(startVertex + target.markerVertexCount,
                     static_cast<uint32_t>(m_currentSnapshot->vertices.size()));
        if ( startVertex < endVertex ) {
            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float maxY = std::numeric_limits<float>::lowest();
            for ( uint32_t i = startVertex; i < endVertex; ++i ) {
                const auto& pos = m_currentSnapshot->vertices[i].pos;
                minX            = std::min(minX, pos.x);
                minY            = std::min(minY, pos.y);
                maxX            = std::max(maxX, pos.x);
                maxY            = std::max(maxY, pos.y);
            }

            auto rect = makeRect(canvasPos.x + minX,
                                 canvasPos.y + minY,
                                 canvasPos.x + maxX,
                                 canvasPos.y + maxY);
            if ( rect.valid ) {
                return mergeRect(rect, fallbackRect);
            }
        }
    }

    return fallbackRect;
}

/// @brief 拾取鼠标附近的 Timing 目标。
/// @param canvasPos 画布左上角屏幕坐标。
/// @param size 当前 Timeline 画布尺寸。
/// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
/// @return 命中的 Timing 目标；未命中时为空。
/// @warning UI 热路径：每帧 hover
/// 和点击时调用；只读取当前快照并做局部命中计算。
std::optional<TimelineCanvas::TimelineHitTarget>
TimelineCanvas::pickTimingTarget(const ImVec2& canvasPos, const ImVec2& size,
                                 float localMouseY) const
{
    (void)localMouseY;
    if ( !m_currentSnapshot ) {
        return std::nullopt;
    }

    const ImVec2 mousePos  = ImGui::GetMousePos();
    float        bestScore = std::numeric_limits<float>::max();
    std::optional<TimelineHitTarget> bestTarget;
    for ( const auto& target : collectVisibleTimingTargets() ) {
        const auto rect = timingTargetScreenRect(target, canvasPos, size);
        if ( !rect.valid || mousePos.x < rect.left || mousePos.x > rect.right ||
             mousePos.y < rect.top || mousePos.y > rect.bottom ) {
            continue;
        }

        float centerX = (rect.left + rect.right) * 0.5f;
        float centerY = (rect.top + rect.bottom) * 0.5f;
        float dx      = std::abs(mousePos.x - centerX);
        float dy      = std::abs(mousePos.y - centerY);
        float score   = dy + dx * 0.15f;
        if ( score < bestScore ) {
            bestScore  = score;
            bestTarget = target;
        }
    }
    return bestTarget;
}

/// @brief 将 Timing 类型转换为 ImGui 绘制颜色。
/// @param effect Timing 类型。
/// @param alpha 透明度。
/// @return ImGui 颜色。
ImU32 TimelineCanvas::timingEffectColor(::MMM::TimingEffect effect,
                                        int                 alpha) const
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return IM_COL32(255, 92, 92, alpha);
    case ::MMM::TimingEffect::SCROLL: return IM_COL32(78, 255, 104, alpha);
    case ::MMM::TimingEffect::JUMP: return IM_COL32(80, 135, 255, alpha);
    case ::MMM::TimingEffect::HS: return IM_COL32(255, 222, 72, alpha);
    }
    return IM_COL32(255, 255, 255, alpha);
}

/// @brief 清理选中集中已经不存在于当前快照的 Timing 实体。
void TimelineCanvas::pruneInvalidTimingSelection()
{
    std::unordered_set<entt::entity> snapshotEntities;
    for ( const auto& target : collectVisibleTimingTargets() ) {
        snapshotEntities.insert(target.entity);
    }

    std::erase_if(m_selectedTimingEntities, [&](entt::entity entity) {
        return entity == entt::null ||
               snapshotEntities.find(entity) == snapshotEntities.end();
    });
}

/// @brief 删除当前选中的 Timing 事件。
void TimelineCanvas::deleteSelectedTimingEvents()
{
    if ( m_selectedTimingEntities.empty() ) {
        return;
    }

    auto selected = m_selectedTimingEntities;
    for ( auto entity : selected ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdDeleteTimelineEvent{ entity }));
    }
    m_selectedTimingEntities.clear();
}

/// @brief 更新 Timeline 画笔右键擦除预览目标。
/// @param hoveredTarget 当前鼠标命中的 Timing 目标。
void TimelineCanvas::updateTimingEraseTarget(
    const std::optional<TimelineHitTarget>& hoveredTarget)
{
    m_timingEraseTargetEntities.clear();
    if ( hoveredTarget && hoveredTarget->entity != entt::null ) {
        m_timingEraseTargetEntities.insert(hoveredTarget->entity);
    }
}

/// @brief 提交当前 Timeline 画笔右键擦除目标。
void TimelineCanvas::commitTimingEraseTargets()
{
    if ( m_timingEraseTargetEntities.empty() ) {
        return;
    }

    bool targetIsSelected = false;
    for ( auto entity : m_timingEraseTargetEntities ) {
        if ( m_selectedTimingEntities.find(entity) !=
             m_selectedTimingEntities.end() ) {
            targetIsSelected = true;
            break;
        }
    }

    std::unordered_set<entt::entity> toDelete;
    if ( targetIsSelected ) {
        toDelete.insert(m_selectedTimingEntities.begin(),
                        m_selectedTimingEntities.end());
    }
    toDelete.insert(m_timingEraseTargetEntities.begin(),
                    m_timingEraseTargetEntities.end());

    for ( auto entity : toDelete ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdDeleteTimelineEvent{ entity }));
        m_selectedTimingEntities.erase(entity);
    }
}

/// @brief 将当前选中的 Timing 复制到编辑器级 Timeline 剪贴板。
/// @param cut 是否在复制后删除原 Timing。
void TimelineCanvas::copySelectedTimingEvents(bool cut)
{
    m_timingClipboard.clear();
    if ( m_selectedTimingEntities.empty() ) {
        Logic::EditorEngine::instance().setTimelineClipboard(
            {}, nullptr, false);
        return;
    }

    std::vector<TimelineHitTarget> selectedTargets;
    for ( const auto& target : collectVisibleTimingTargets() ) {
        if ( m_selectedTimingEntities.find(target.entity) !=
             m_selectedTimingEntities.end() ) {
            selectedTargets.push_back(target);
        }
    }
    if ( selectedTargets.empty() ) {
        Logic::EditorEngine::instance().setTimelineClipboard(
            {}, nullptr, false);
        return;
    }

    std::stable_sort(selectedTargets.begin(),
                     selectedTargets.end(),
                     [](const auto& lhs, const auto& rhs) {
                         if ( std::abs(lhs.time - rhs.time) > 1e-6 ) {
                             return lhs.time < rhs.time;
                         }
                         return static_cast<int>(lhs.effect) <
                                static_cast<int>(rhs.effect);
                     });
    const double anchorTime  = selectedTargets.front().time;
    const double fallbackBpm = timelineClipboardFallbackBpm(*m_currentSnapshot);
    const auto   beatTimeline =
        buildTimelineClipboardBeatTimeline(*m_currentSnapshot);
    const double anchorBeat =
        timelineClipboardTimeToBeat(beatTimeline, anchorTime, fallbackBpm);
    m_timingClipboard.reserve(selectedTargets.size());
    std::vector<Logic::TimelineClipboardItem> sharedClipboard;
    sharedClipboard.reserve(selectedTargets.size());
    for ( const auto& target : selectedTargets ) {
        TimelineClipboardEntry entry;
        entry.relativeTime    = target.time - anchorTime;
        entry.relativeBeat    = timelineClipboardTimeToBeat(
                                    beatTimeline, target.time, fallbackBpm) -
                                anchorBeat;
        entry.effect          = target.effect;
        entry.value           = target.value;
        entry.hasBeatPosition = true;
        m_timingClipboard.push_back(entry);

        Logic::TimelineClipboardItem sharedEntry;
        sharedEntry.timeline        = Logic::TimelineComponent{ target.time,
                                                                target.effect,
                                                                target.value };
        sharedEntry.relativeTime    = entry.relativeTime;
        sharedEntry.relativeBeat    = entry.relativeBeat;
        sharedEntry.hasBeatPosition = entry.hasBeatPosition;
        sharedClipboard.push_back(std::move(sharedEntry));
    }
    Logic::EditorEngine::instance().setTimelineClipboard(
        std::move(sharedClipboard), nullptr, false);

    if ( cut ) {
        deleteSelectedTimingEvents();
    }
}

/// @brief 将编辑器级 Timeline 剪贴板粘贴到指定锚点时间。
/// @param anchorTime 粘贴锚点时间，单位秒。
void TimelineCanvas::pasteTimingClipboard(double anchorTime)
{
    auto timingClipboard =
        Logic::EditorEngine::instance().getTimelineClipboard();
    if ( timingClipboard.empty() ) {
        return;
    }

    m_selectedTimingEntities.clear();
    Logic::CmdCreateTimelineEvents batch;
    batch.events.reserve(timingClipboard.size());
    const bool pasteByBeat =
        Config::AppConfig::instance().getEditorSettings().copyPasteTimeBasis ==
            Config::CopyPasteTimeBasis::Beat &&
        m_currentSnapshot &&
        std::all_of(timingClipboard.begin(),
                    timingClipboard.end(),
                    [](const auto& entry) { return entry.hasBeatPosition; });
    const double fallbackBpm =
        m_currentSnapshot ? timelineClipboardFallbackBpm(*m_currentSnapshot)
                          : 120.0;
    const auto beatTimeline =
        pasteByBeat ? buildTimelineClipboardBeatTimeline(*m_currentSnapshot)
                    : TimelineClipboardBeatTimeline{};
    const double anchorBeat =
        pasteByBeat
            ? timelineClipboardTimeToBeat(beatTimeline, anchorTime, fallbackBpm)
            : 0.0;
    for ( const auto& entry : timingClipboard ) {
        double targetTime = anchorTime + entry.relativeTime;
        if ( pasteByBeat ) {
            targetTime = timelineClipboardBeatToTime(
                beatTimeline, anchorBeat + entry.relativeBeat, fallbackBpm);
        }
        batch.events.push_back({ std::max(0.0, targetTime),
                                 entry.timeline.m_effect,
                                 entry.timeline.m_value });
    }
    if ( !batch.events.empty() ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent{ std::move(batch) });
    }
}

/// @brief 处理 Timeline 画布上的 Timing 选择、框选、拖动和快捷键。
/// @param canvasPos 画布左上角屏幕坐标。
/// @param size 当前 Timeline 画布尺寸。
/// @param isHovered 鼠标是否悬浮在画布 Image 上。
/// @param isFocused Timeline 窗口是否聚焦。
/// @warning UI 热路径：每帧处理 Timeline Timing
/// 交互；禁止引入文件系统访问或阻塞操作。
void TimelineCanvas::handleTimingCanvasInteraction(const ImVec2& canvasPos,
                                                   const ImVec2& size,
                                                   bool          isHovered,
                                                   bool          isFocused)
{
    if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ) {
        m_hoveredTimingEntity = entt::null;
        m_isTimingErasing     = false;
        m_timingEraseTargetEntities.clear();
        return;
    }

    pruneInvalidTimingSelection();

    ImGuiIO&    io          = ImGui::GetIO();
    const float localMouseY = io.MousePos.y - canvasPos.y;
    const float localMouseX = io.MousePos.x - canvasPos.x;
    const bool  overMenuButton =
        localMouseX >= size.x - 56.0f && localMouseY <= 56.0f;
    const bool professionalMode = Config::AppConfig::instance()
                                      .getEditorSettings()
                                      .timelineProfessionalMode;
    auto hoveredTarget    = isHovered && !overMenuButton
                                ? pickTimingTarget(canvasPos, size, localMouseY)
                                : std::nullopt;
    m_hoveredTimingEntity = hoveredTarget ? hoveredTarget->entity : entt::null;

    if ( isHovered && hoveredTarget ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%.3fs", hoveredTarget->time);
    }

    if ( !isFocused ) {
        if ( m_isTimingErasing &&
             !ImGui::IsMouseDown(ImGuiMouseButton_Right) ) {
            m_isTimingErasing = false;
            m_timingEraseTargetEntities.clear();
        }
        return;
    }
    const bool anyPopupOpen = ImGui::IsPopupOpen(
        nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if ( anyPopupOpen || m_isPopupOpen || m_isCreatePopupOpen ||
         overMenuButton || io.WantTextInput ||
         UI::ShortcutUtils::isShortcutRecordingActive() ) {
        if ( m_isTimingErasing &&
             !ImGui::IsMouseDown(ImGuiMouseButton_Right) ) {
            m_isTimingErasing = false;
            m_timingEraseTargetEntities.clear();
        }
        return;
    }

    const bool  ctrl              = io.KeyCtrl;
    const bool  shift             = io.KeyShift;
    const bool  additiveSelection = ctrl || shift;
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    bool        handledKeyboardCommand = false;
    if ( ctrl && ImGui::IsKeyPressed(ImGuiKey_Z) ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            shift ? Logic::LogicCommand{ Logic::CmdRedo{} }
                  : Logic::LogicCommand{ Logic::CmdUndo{} }));
        handledKeyboardCommand = true;
    }
    if ( ctrl && ImGui::IsKeyPressed(ImGuiKey_Y) ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdRedo{}));
        handledKeyboardCommand = true;
    }
    const std::array<Logic::EditTool, 5> editableTools{
        Logic::EditTool::Move,        Logic::EditTool::Marquee,
        Logic::EditTool::Draw,        Logic::EditTool::ColorBrush,
        Logic::EditTool::ColorEraser,
    };
    if ( !handledKeyboardCommand ) {
        for ( Logic::EditTool tool : editableTools ) {
            if ( UI::ShortcutUtils::isShortcutPressed(
                     UI::ShortcutUtils::getToolShortcut(settings, tool)) ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdChangeTool{ tool }));
                handledKeyboardCommand = true;
                break;
            }
        }
    }
    auto applyProfessionalCreateType = [&]() {
        if ( !professionalMode ) {
            return true;
        }

        auto createType = professionalCreateTypeAtX(size, localMouseX);
        if ( !createType ) {
            return false;
        }

        if ( m_createType != *createType ) {
            m_createType  = *createType;
            m_createValue = defaultTimingCreateValue(
                timingEffectFromCreateType(m_createType));
        }
        return true;
    };
    if ( !handledKeyboardCommand && ImGui::IsKeyPressed(ImGuiKey_A) && ctrl ) {
        if ( m_currentSnapshot->currentTool == Logic::EditTool::Move ||
             m_currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            m_selectedTimingEntities.clear();
            for ( const auto& target : collectVisibleTimingTargets() ) {
                if ( isTimingTargetSelectable(target) ) {
                    m_selectedTimingEntities.insert(target.entity);
                }
            }
        }
        handledKeyboardCommand = true;
    }
    if ( !handledKeyboardCommand && ImGui::IsKeyPressed(ImGuiKey_C) && ctrl ) {
        copySelectedTimingEvents(false);
        handledKeyboardCommand = true;
    }
    if ( !handledKeyboardCommand && ImGui::IsKeyPressed(ImGuiKey_X) && ctrl ) {
        copySelectedTimingEvents(true);
        handledKeyboardCommand = true;
    }
    if ( !handledKeyboardCommand && ImGui::IsKeyPressed(ImGuiKey_V) && ctrl ) {
        ::MMM::UI::ClipboardBridge::importEditorClipboardFromSystem();
        pasteTimingClipboard(std::max(0.0, m_currentSnapshot->currentTime));
        handledKeyboardCommand = true;
    }
    if ( !handledKeyboardCommand &&
         (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
          ImGui::IsKeyPressed(ImGuiKey_Backspace)) ) {
        deleteSelectedTimingEvents();
        handledKeyboardCommand = true;
    }

    if ( m_currentSnapshot->currentTool != Logic::EditTool::Draw ||
         m_currentSnapshot->isPlaying ) {
        m_isTimingErasing = false;
        m_timingEraseTargetEntities.clear();
    } else {
        if ( isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
            m_isTimingErasing = true;
            updateTimingEraseTarget(hoveredTarget);
        }
        if ( m_isTimingErasing && ImGui::IsMouseDown(ImGuiMouseButton_Right) ) {
            updateTimingEraseTarget(isHovered ? hoveredTarget : std::nullopt);
        }
        if ( m_isTimingErasing &&
             ImGui::IsMouseReleased(ImGuiMouseButton_Right) ) {
            updateTimingEraseTarget(isHovered ? hoveredTarget : std::nullopt);
            commitTimingEraseTargets();
            m_isTimingErasing = false;
            m_timingEraseTargetEntities.clear();
        }
    }

    switch ( m_currentSnapshot->currentTool ) {
    case Logic::EditTool::Draw:
        if ( !m_isTimingErasing && isHovered && !hoveredTarget &&
             ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
             applyProfessionalCreateType() ) {
            m_isTimingDrawPreviewing = true;
            bool snapped             = false;
            m_timingDrawPreviewTime =
                snapTimingTime(size,
                               canvasTimeAtLocalY(size, localMouseY),
                               localMouseY,
                               snapped);
            m_timingDrawPreviewY = static_cast<float>(
                canvasYAtTime(size, m_timingDrawPreviewTime));
        }
        if ( hoveredTarget && ImGui::IsMouseReleased(ImGuiMouseButton_Left) ) {
            m_isTimingDrawPreviewing = false;
        }
        if ( !m_isTimingErasing && m_isTimingDrawPreviewing &&
             ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            if ( !applyProfessionalCreateType() ) {
                m_isTimingDrawPreviewing = false;
                break;
            }
            bool snapped = false;
            m_timingDrawPreviewTime =
                snapTimingTime(size,
                               canvasTimeAtLocalY(size, localMouseY),
                               localMouseY,
                               snapped);
            m_timingDrawPreviewY = static_cast<float>(
                canvasYAtTime(size, m_timingDrawPreviewTime));
        }
        if ( !m_isTimingErasing && m_isTimingDrawPreviewing &&
             ImGui::IsMouseReleased(ImGuiMouseButton_Left) ) {
            openTimingCreatePopupAtY(size, localMouseY, localMouseX, false);
            m_isTimingDrawPreviewing = false;
        }
        break;
    case Logic::EditTool::Move:
        if ( isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
            if ( hoveredTarget ) {
                const bool wasSelected =
                    m_selectedTimingEntities.find(hoveredTarget->entity) !=
                    m_selectedTimingEntities.end();
                bool shouldBeginTimingDrag = true;
                if ( additiveSelection ) {
                    if ( wasSelected ) {
                        m_selectedTimingEntities.erase(hoveredTarget->entity);
                        shouldBeginTimingDrag = false;
                    } else {
                        m_selectedTimingEntities.insert(hoveredTarget->entity);
                    }
                } else {
                    if ( !wasSelected ) {
                        m_selectedTimingEntities.clear();
                    }
                    m_selectedTimingEntities.insert(hoveredTarget->entity);
                }

                if ( !shouldBeginTimingDrag ) {
                    m_isTimingDragging       = false;
                    m_timingDragPreviewDelta = 0.0;
                    m_timingDragEntries.clear();
                    break;
                }

                if ( m_selectedTimingEntities.empty() ) {
                    m_selectedTimingEntities.clear();
                    break;
                }

                m_isTimingDragging       = true;
                m_timingDragStartTime    = hoveredTarget->time;
                m_timingDragPreviewDelta = 0.0;
                m_timingDragEntries.clear();
                for ( const auto& target : collectVisibleTimingTargets() ) {
                    if ( m_selectedTimingEntities.find(target.entity) !=
                         m_selectedTimingEntities.end() ) {
                        m_timingDragEntries.push_back(
                            { target.entity, target.time, target.value });
                    }
                }
            } else if ( !additiveSelection ) {
                m_selectedTimingEntities.clear();
            }
        }

        if ( m_isTimingDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            double currentTime = canvasTimeAtLocalY(size, localMouseY);
            bool   snapped     = false;
            currentTime =
                snapTimingTime(size, currentTime, localMouseY, snapped);
            m_timingDragPreviewDelta = currentTime - m_timingDragStartTime;
        }
        if ( m_isTimingDragging &&
             !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            if ( std::abs(m_timingDragPreviewDelta) > 1e-6 ) {
                for ( const auto& entry : m_timingDragEntries ) {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
                            entry.entity,
                            std::max(
                                0.0,
                                entry.originalTime + m_timingDragPreviewDelta),
                            entry.value }));
                }
            }
            m_isTimingDragging       = false;
            m_timingDragPreviewDelta = 0.0;
            m_timingDragEntries.clear();
        }
        break;
    case Logic::EditTool::Marquee: {
        auto makeMarqueeRect = [&]() {
            TimingSelectionRect rect;
            rect.left   = canvasPos.x +
                          std::min(m_timingMarqueeStartX, m_timingMarqueeEndX);
            rect.right  = canvasPos.x +
                          std::max(m_timingMarqueeStartX, m_timingMarqueeEndX);
            rect.top    = canvasPos.y +
                          std::min(m_timingMarqueeStartY, m_timingMarqueeEndY);
            rect.bottom = canvasPos.y +
                          std::max(m_timingMarqueeStartY, m_timingMarqueeEndY);
            rect.valid =
                rect.right > rect.left + 0.5f && rect.bottom > rect.top + 0.5f;
            return rect;
        };

        auto rectIntersects = [](TimingSelectionRect lhs,
                                 TimingSelectionRect rhs) {
            if ( !lhs.valid || !rhs.valid ) return false;
            constexpr float EPS = 0.5f;
            return std::max(lhs.left, rhs.left) <=
                       std::min(lhs.right, rhs.right) + EPS &&
                   std::max(lhs.top, rhs.top) <=
                       std::min(lhs.bottom, rhs.bottom) + EPS;
        };

        auto rectContains = [](TimingSelectionRect outer,
                               TimingSelectionRect inner) {
            if ( !outer.valid || !inner.valid ) return false;
            constexpr float EPS = 0.5f;
            return inner.left >= outer.left - EPS &&
                   inner.right <= outer.right + EPS &&
                   inner.top >= outer.top - EPS &&
                   inner.bottom <= outer.bottom + EPS;
        };

        const auto selectionMode =
            Config::AppConfig::instance().getEditorSettings().selectionMode;
        auto selectionMatchesTarget = [&](TimingSelectionRect      selection,
                                          const TimelineHitTarget& target) {
            auto targetRect = timingTargetScreenRect(target, canvasPos, size);
            return selectionMode == Config::SelectionMode::Strict
                       ? rectContains(selection, targetRect)
                       : rectIntersects(selection, targetRect);
        };

        auto refreshMarqueeScreenY = [&]() {
            m_timingMarqueeStartY = static_cast<float>(
                canvasYAtTime(size, m_timingMarqueeStartTime));
            m_timingMarqueeEndY =
                static_cast<float>(canvasYAtTime(size, m_timingMarqueeEndTime));
        };

        auto refreshMarqueeTargets = [&]() {
            refreshMarqueeScreenY();
            m_selectedTimingEntities = m_timingMarqueeBaseSelection;
            const auto selectionRect = makeMarqueeRect();
            if ( !selectionRect.valid ) {
                return;
            }
            for ( const auto& target : collectVisibleTimingTargets() ) {
                if ( isTimingTargetSelectable(target) &&
                     selectionMatchesTarget(selectionRect, target) ) {
                    m_selectedTimingEntities.insert(target.entity);
                }
            }
        };

        if ( isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
            if ( hoveredTarget && isTimingTargetSelectable(*hoveredTarget) ) {
                const bool wasSelected =
                    m_selectedTimingEntities.find(hoveredTarget->entity) !=
                    m_selectedTimingEntities.end();
                if ( additiveSelection ) {
                    if ( wasSelected ) {
                        m_selectedTimingEntities.erase(hoveredTarget->entity);
                    } else {
                        m_selectedTimingEntities.insert(hoveredTarget->entity);
                    }
                } else {
                    m_selectedTimingEntities.clear();
                    m_selectedTimingEntities.insert(hoveredTarget->entity);
                }
                m_isTimingMarqueeSelecting = false;
                m_timingMarqueeBaseSelection.clear();
            } else {
                m_isTimingMarqueeSelecting = true;
                m_timingMarqueeStartX      = localMouseX;
                m_timingMarqueeEndX        = localMouseX;
                m_timingMarqueeStartTime =
                    canvasTimeAtLocalY(size, localMouseY);
                m_timingMarqueeEndTime = m_timingMarqueeStartTime;
                refreshMarqueeScreenY();
                m_timingMarqueeBaseSelection.clear();
                if ( additiveSelection ) {
                    m_timingMarqueeBaseSelection = m_selectedTimingEntities;
                }
                if ( !additiveSelection ) {
                    m_selectedTimingEntities.clear();
                }
                refreshMarqueeTargets();
            }
        }
        if ( m_isTimingMarqueeSelecting &&
             ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            bool autoScrolled = false;
            if ( !m_currentSnapshot->isPlaying ) {
                const double autoScrollTargetTime =
                    marqueeAutoScrollTargetTime(*m_currentSnapshot,
                                                size.y,
                                                localMouseY,
                                                io.DeltaTime,
                                                io.KeyShift,
                                                autoScrolled);
                if ( autoScrolled ) {
                    const double visualOffset = Config::AppConfig::instance()
                                                    .getVisualConfig()
                                                    .getEffectiveVisualOffset();
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(Logic::CmdSeek{
                            autoScrollTargetTime - visualOffset }));
                }
            }
            m_timingMarqueeEndX    = localMouseX;
            m_timingMarqueeEndTime = canvasTimeAtLocalY(size, localMouseY);
            refreshMarqueeTargets();
        }
        if ( m_isTimingMarqueeSelecting &&
             !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            refreshMarqueeTargets();
            m_isTimingMarqueeSelecting = false;
            m_timingMarqueeBaseSelection.clear();
        }
        break;
    }
    case Logic::EditTool::ColorBrush:
    case Logic::EditTool::ColorEraser: break;
    }
}

/// @brief 绘制 Timeline Timing 的 hover、选中、拖动和框选反馈。
/// @param canvasPos 画布左上角屏幕坐标。
/// @param size 当前 Timeline 画布尺寸。
/// @warning UI 热路径：每帧绘制交互覆盖层；只提交 ImGui 绘制命令。
void TimelineCanvas::renderTimingInteractionOverlay(const ImVec2& canvasPos,
                                                    const ImVec2& size)
{
    if ( !m_currentSnapshot ) {
        return;
    }

    ImDrawList*  drawList = ImGui::GetWindowDrawList();
    const ImVec2 clipMax(canvasPos.x + size.x, canvasPos.y + size.y);
    drawList->PushClipRect(canvasPos, clipMax, true);

    if ( m_isTimingDragging && std::abs(m_timingDragPreviewDelta) > 1e-6 ) {
        const ImVec2      mousePos = ImGui::GetMousePos();
        const std::string deltaText =
            fmt::format("{:+.3f}s", m_timingDragPreviewDelta);
        drawList->AddText(ImVec2(mousePos.x + 12.0f, mousePos.y + 12.0f),
                          IM_COL32(255, 255, 255, 230),
                          deltaText.c_str());
    }

    if ( m_isTimingMarqueeSelecting ) {
        const float minX =
            canvasPos.x + std::min(m_timingMarqueeStartX, m_timingMarqueeEndX);
        const float maxX =
            canvasPos.x + std::max(m_timingMarqueeStartX, m_timingMarqueeEndX);
        const float minY =
            canvasPos.y + std::min(m_timingMarqueeStartY, m_timingMarqueeEndY);
        const float maxY =
            canvasPos.y + std::max(m_timingMarqueeStartY, m_timingMarqueeEndY);
        drawList->AddRectFilled(ImVec2(minX, minY),
                                ImVec2(maxX, maxY),
                                IM_COL32(120, 170, 255, 42));
        drawList->AddRect(ImVec2(minX, minY),
                          ImVec2(maxX, maxY),
                          IM_COL32(120, 170, 255, 180),
                          0.0f,
                          0,
                          1.5f);
    }

    drawList->PopClipRect();
}

}  // namespace MMM::Canvas
