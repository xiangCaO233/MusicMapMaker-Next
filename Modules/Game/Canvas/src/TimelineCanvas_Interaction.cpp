/// @file TimelineCanvas_Interaction.cpp
/// @brief 实现总时间线 Timing 的投影、吸附、选择、拖拽、擦除和剪贴板交互。
///
/// 本文件只消费 UI 可见的不可变 RenderSnapshot，所有 Timing 修改均发布
/// LogicCommandEvent 交给逻辑线程。时间与 Y 坐标换算显式处理 SV、HS、
/// 判定线位置和准备快照时应用的视觉偏移；交互状态只保存实体 ID 与数值。
///
/// 复制粘贴支持按秒保持间隔和按连续 beat 保持节奏两种语义，BPM 时间线
/// 仅从快照构建。框选、拖动和擦除以按下/更新/释放事务推进，不使用阻塞等待。
#include "canvas/TimelineCanvas.h"

#include "canvas/MarqueeAutoScroll.h"
#include "canvas/TimelineTimingTooltip.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "mmm/timing/BpmNormalization.h"
#include "ui/imgui/ClipboardBridge.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/utils/UIWidgetUtils.h"
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
/// @details 与 marker 最小尺寸共同保证细线或窄皮肤资源仍可被鼠标命中。
constexpr float TIMING_PICK_RADIUS = 16.0f;

/// @brief Timeline Timing 标记尺寸，单位像素。
/// @details 用于缺少真实 marker geometry 时构造稳定的回退矩形。
constexpr float TIMING_MARKER_SIZE = 20.0f;

/// @brief Timeline Timing 标记边距，单位像素。
/// @details 普通模式左右类型 marker 与画布边缘保持该最小距离。
constexpr float TIMING_MARKER_PADDING = 5.0f;

/// @brief 将 Timing 类型转换为对应的快照效果掩码。
/// @param effect Timing 类型。
/// @return 对应 `TimelineInteractiveElement::effects` 位；未知值返回零。
/// @details 掩码只用于判断快照元素是否包含该类型，不代表实体有效性。
uint32_t getTimingEffectMask(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return Common::Render::SCROLL_EFFECT_BPM;
    case ::MMM::TimingEffect::SCROLL:
        return Common::Render::SCROLL_EFFECT_SCROLL;
    case ::MMM::TimingEffect::JUMP: return Common::Render::SCROLL_EFFECT_JUMP;
    case ::MMM::TimingEffect::HS: return Common::Render::SCROLL_EFFECT_HS;
    }
    return 0;
}

/// @brief 取 Timeline 元素中指定类型的实体。
/// @param element 聚合同一时间位置多种效果的快照元素。
/// @param effect 需要读取的 Timing 类型。
/// @return 对应实体 ID；未知类型返回 `entt::null`。
/// @details 返回值只作为观察标识，不在 UI 线程解引用 registry。
entt::entity getTimingEntity(
    const Common::Render::TimelineInteractiveElement& element,
    ::MMM::TimingEffect                               effect)
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
/// @param element Timeline 交互元素。
/// @param effect 需要读取的 Timing 类型。
/// @return 对应参数值；未知类型返回零。
/// @details BPM、Scroll、Jump 与 HS 各自使用独立快照字段。
double getTimingValue(const Common::Render::TimelineInteractiveElement& element,
                      ::MMM::TimingEffect                               effect)
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
/// @param element Timeline 交互元素。
/// @param effect 需要读取的 Timing 类型。
/// @return 对应类型的 marker geometry 引用。
/// @details 未知类型回退 Scroll geometry，仅作为防御引用，调用方仍会
/// 通过零 effect mask 排除未知类型。
const Common::Render::TimelineInteractiveElement::MarkerGeometry&
getTimingMarkerGeometry(
    const Common::Render::TimelineInteractiveElement& element,
    ::MMM::TimingEffect                               effect)
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
/// @details 该顺序同时决定目标展开、等距拾取与同时间复制的稳定次序。
constexpr ::MMM::TimingEffect TIMELINE_EFFECT_ORDER[] = {
    ::MMM::TimingEffect::BPM,
    ::MMM::TimingEffect::JUMP,
    ::MMM::TimingEffect::HS,
    ::MMM::TimingEffect::SCROLL,
};

/// @brief 获取专业模式中指定 Timing 类型所属的轨道索引。
/// @param effect Timing 类型。
/// @return BPM、Scroll、Jump、HS 分别映射到零至三；未知值回退零。
/// @details 映射必须与专业覆盖层的四个可见标签顺序一致。
int professionalTimingLane(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return 0;
    case ::MMM::TimingEffect::SCROLL: return 1;
    case ::MMM::TimingEffect::JUMP: return 2;
    case ::MMM::TimingEffect::HS: return 3;
    }
    return 0;
}

/// @brief 将专业模式轨道位置转换为创建弹窗类型索引。
/// @details X 坐标先按四等分泳道取 floor，再钳制末端像素到最后泳道；
/// 退化宽度或画布外坐标返回空，避免隐式选择错误 Timing 类型。
/// @param size 当前 Timeline 画布尺寸。
/// @param localMouseX 鼠标相对画布左上角的 X 坐标。
/// @return 可创建 Timing 的轨道返回类型索引；越界轨道返回空。
std::optional<int> professionalCreateTypeAtX(const ImVec2& size,
                                             float         localMouseX)
{
    if ( size.x <= 1.0f || localMouseX < 0.0f || localMouseX > size.x ) {
        return std::nullopt;
    }

    constexpr int laneCount = 4;
    const float   laneWidth = size.x / static_cast<float>(laneCount);
    int           lane =
        static_cast<int>(std::floor(localMouseX / std::max(1.0f, laneWidth)));
    lane = std::clamp(lane, 0, laneCount - 1);
    switch ( lane ) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
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

/// @brief 按原值方向规整 Timeline 复制粘贴换算用的 BPM 值。
/// @details 复用全项目 BPM 规范：有限越界值钳制到最近边界，非有限值
/// 使用规范化回退，保证 beat 积分不会除零或产生无穷。
/// @param bpm 待规整 BPM。
/// @param fallbackBpm 原值为 NaN 时使用的回退 BPM。
/// @return 位于安全计算范围内的 BPM。
double sanitizeTimelineClipboardBpm(double bpm, double fallbackBpm)
{
    return ::MMM::normalizeBpmValue(bpm, fallbackBpm);
}

/// @brief 取得 Timeline 快照中的有效回退 BPM。
/// @param snapshot 当前 Timeline 渲染快照。
/// @return 规范化后的 fallback BPM；快照值不可用时以 120 为语义回退。
double timelineClipboardFallbackBpm(
    const Common::Render::RenderSnapshot& snapshot)
{
    return sanitizeTimelineClipboardBpm(snapshot.fallbackBpm, 120.0);
}

/// @brief 从 Timeline 当前快照构建可双向换算的连续 beat 时间线。
///
/// @details 构建规则：
/// - fallback BPM 先通过统一 BPM 规范化入口处理。
/// - 只收集带 BPM effect mask 的 ScrollSegment。
/// - 非有限事件时间被忽略。
/// - 每个事件 BPM 独立规范化到合法范围。
/// - 事件按时间升序稳定排序。
/// - 同一时间戳只保留最终出现的 BPM。
/// - 首个有效事件作为连续 beat 的零锚点。
/// - 后续累计 beat 使用前一段 BPM 积分。
/// - 每个锚点保存时间、当前段 BPM 和累计 beat。
/// - 秒到 beat 与 beat 到秒消费同一锚点数组。
/// - 构建结果不保存快照或 segment 指针。
/// - 空 BPM 列表返回空数组并由换算函数使用 fallback。
/// - 本函数只为剪贴板相对节奏换算服务。
/// - 本函数不修改谱面 Timing 数据。
/// - 中间计算使用 double 保留长谱面精度。
/// @param snapshot 当前渲染快照。
/// @return 按时间排序并去重后的 BPM/beat 锚点列表。
TimelineClipboardBeatTimeline buildTimelineClipboardBeatTimeline(
    const Common::Render::RenderSnapshot& snapshot)
{
    struct BpmEvent {
        double time{ 0.0 };   ///< BPM 时间点，单位秒
        double bpm{ 120.0 };  ///< BPM 值
    };

    const double          fallbackBpm = timelineClipboardFallbackBpm(snapshot);
    std::vector<BpmEvent> bpmEvents;
    bpmEvents.reserve(snapshot.scrollSegments.size());
    for ( const auto& segment : snapshot.scrollSegments ) {
        if ( (segment.effects & Common::Render::SCROLL_EFFECT_BPM) == 0 ||
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
/// @details 选择目标时间之前的最后一个 BPM 锚点，以该点累计 beat 加上
/// 局部秒差乘 BPM/60；空时间线使用规范化 fallback BPM 从零线性换算。
/// 时间早于首锚点时从首段向负方向外推，不在此钳制到零。
/// @warning 剪贴板低频路径：仅进行有序锚点查找和常量算术。
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
/// @details 选择目标 beat 之前的最后一个锚点，以该点时间加局部 beat 差
/// 乘 60/BPM；空时间线使用 fallback BPM 从零换算。负 beat 可向前外推，
/// 粘贴入口最终再按项目时间边界钳制事件时间。
/// @warning 剪贴板低频路径：只读取预构建锚点，不访问项目或 ECS。
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
/// @details 索引与专业模式泳道及创建弹窗选项保持一致；未知索引回退
/// Scroll，保证已有配置升级后仍落入可编辑的通用速度类型。
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
/// @details BPM 使用常用 120，Scroll/HS 使用单位倍率，Jump 使用毫秒级
/// 默认值；只初始化新建表单，不影响编辑已有事件。
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
///
/// @details 坐标换算约束：
/// - 输入 Y 相对 Timeline Image 左上角。
/// - 准备快照时应用的 Y 偏移先从输入坐标扣除。
/// - 判定线 Y 由当前键数对应的视觉配置计算。
/// - 当前播放时间在判定线位置作为投影原点。
/// - 无 ScrollSegment 时使用 500 像素每秒乘 timelineZoom。
/// - 有分段时先计算当前时间的累计绝对 Y。
/// - 当前段通过按时间 upper_bound 选择。
/// - 早于首段时使用首段外推。
/// - 目标绝对 Y 等于当前绝对 Y 加判定线到鼠标的像素差。
/// - 目标段通过累计 absY lower_bound 选择。
/// - 目标落在首段之前时从首段外推。
/// - 目标落在段内时从前一段锚点反算时间。
/// - 速度绝对值接近零时返回段起始时间。
/// - 本函数不修改鼠标位置或快照。
/// - 返回值属于显示时间域，调用方负责视觉偏移语义。
/// - 负时间是否允许由具体创建、拖动或 seek 调用方决定。
/// - 分段列表沿用渲染快照的稳定排序。
/// - 该换算与 `canvasYAtTime` 共同维持交互往返一致性。
/// @param size 当前 Timeline 画布尺寸。
/// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
/// @return 换算出的谱面时间，单位秒。
double TimelineCanvas::canvasTimeAtLocalY(const ImVec2& size,
                                          float         localMouseY) const
{
    // 没有当前快照时不存在投影上下文，返回安全的时间原点。
    if ( !m_currentSnapshot ) {
        return 0.0;
    }

    const auto& visual        = Config::AppConfig::instance().getVisualConfig();
    const float judgmentLineY = size.y * visual.judgmentLinePositionForKeyCount(
                                             m_currentSnapshot->trackCount);
    const float compensatedMouseY = localMouseY - m_lastAppliedYOffset;
    const auto& segments          = m_currentSnapshot->scrollSegments;
    if ( segments.empty() ) {
        // 无 SV 使用与时间线渲染一致的线性速度。
        float  zoom  = visual.timelineZoom;
        double speed = 500.0 * zoom;
        return (judgmentLineY - compensatedMouseY) / speed +
               m_currentSnapshot->currentTime;
    }

    auto it = std::upper_bound(
        segments.begin(),
        segments.end(),
        m_currentSnapshot->currentTime,
        [](double val, const Common::Render::ScrollSegment& seg) {
            return val < seg.time;
        });
    // 先确定当前播放时间对应的累计绝对 Y，作为鼠标反投影基准。
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
    // 再在累计距离域选择覆盖目标的分段并反算局部时间。
    auto itTime = std::lower_bound(segments.begin(),
                                   segments.end(),
                                   targetAbsY,
                                   [](const Common::Render::ScrollSegment& seg,
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
///
/// @details 正向投影约束：
/// - 输入时间与快照 `currentTime` 使用同一显示时间域。
/// - 判定线位置是当前时间的屏幕锚点。
/// - 无分段时使用线性 timelineZoom 速度。
/// - 有分段时分别解析当前时间和目标时间的累计 absY。
/// - 分段通过按时间 upper_bound 选择。
/// - 查询早于首段时使用首段外推。
/// - 非零速度段按锚点时间线性累积距离。
/// - 零速段保持锚点 absY。
/// - 目标段 HS 乘在当前与目标的绝对距离差上。
/// - 当前段 HS 不参与结果，只用于结构化 helper 返回。
/// - 目标时间绝对距离更大时通常投影到判定线上方。
/// - 最后重新加回准备快照阶段的 Y 偏移。
/// - 返回值是 Image 局部坐标，不包含屏幕窗口原点。
/// - 无当前快照时返回零。
/// - 本函数不钳制结果到视口，调用方可判断边缘可见性。
/// - 本函数不修改快照或配置。
/// - 与反投影函数共用同一分段选择规则。
/// @param size 当前 Timeline 画布尺寸。
/// @param time 谱面时间，单位秒。
/// @return Timeline 画布内 Y 坐标。
double TimelineCanvas::canvasYAtTime(const ImVec2& size, double time) const
{
    if ( !m_currentSnapshot ) {
        return 0.0;
    }

    const auto& visual        = Config::AppConfig::instance().getVisualConfig();
    const float judgmentLineY = size.y * visual.judgmentLinePositionForKeyCount(
                                             m_currentSnapshot->trackCount);
    const auto& segments = m_currentSnapshot->scrollSegments;
    if ( segments.empty() ) {
        float  zoom  = visual.timelineZoom;
        double speed = 500.0 * zoom;
        return judgmentLineY - (time - m_currentSnapshot->currentTime) * speed +
               m_lastAppliedYOffset;
    }

    auto resolveAbsYAndHs = [&](double queryTime) {
        // 同一 helper 保证当前与目标使用完全一致的分段外推规则。
        auto it = std::upper_bound(
            segments.begin(),
            segments.end(),
            queryTime,
            [](double val, const Common::Render::ScrollSegment& seg) {
                return val < seg.time;
            });

        const auto* seg =
            it == segments.begin() ? &segments.front() : &(*std::prev(it));
        double absY = seg->absY;
        if ( std::abs(seg->speed) > 1e-9 ) {
            // 静止段不执行除法或无意义累积，保持其固定绝对位置。
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
///
/// @details 吸附优先级：
/// - 非有限原始时间或空快照直接原样返回。
/// - 首先扫描可见 Timeline 元素的屏幕 Y 距离。
/// - 距离小于当前最近阈值时选择已有事件时间。
/// - 多个等距事件保留先出现者，保证结果稳定。
/// - 已有事件吸附和拍线吸附共享最小像素距离竞争。
/// - BPM 事件用于建立节奏网格分段。
/// - 当前分拍除数决定每拍细分数量。
/// - 分拍除数至少按一处理。
/// - BPM 使用统一 normalize 语义限制在合法范围。
/// - 非有限 BPM 回退到最接近其语义的有效值。
/// - 快照 fallback BPM 为没有事件时的基准。
/// - BPM 事件按时间排序后构建连续 beat 锚点。
/// - 同一时间的后续 BPM 覆盖该点节奏值。
/// - 当前 rawTime 所在 BPM 段用于时间到 beat 换算。
/// - 邻近整数细分位置再换算回候选时间。
/// - 候选时间通过正向投影转换为像素 Y。
/// - 像素距离必须小于当前最近候选才替换结果。
/// - 事件吸附因此可以优先于更远的网格点。
/// - 网格点更近时可以覆盖已有事件候选。
/// - 输出 `snapped` 只在某个候选实际命中时为 true。
/// - 原始时间本身不会因边界钳制被标记为吸附。
///
/// @details 分段构建：
/// - 只读取带 BPM effect mask 的 ScrollSegment。
/// - 非有限时间事件被忽略。
/// - 首段之前使用 fallback BPM。
/// - 每个 BPM 锚点记录时间、规范 BPM 和累计 beat。
/// - 累计 beat 使用上一段 BPM 积分。
/// - 时间相同的 BPM 不增加 beat 距离。
/// - 时间早于零的事件仍按快照顺序参与连续映射。
/// - 目标 beat 取当前连续 beat 乘 divisor 后四舍五入。
/// - 再除以 divisor 得到吸附后的连续 beat。
/// - beat 到时间换算使用目标 beat 所在 BPM 段。
/// - BPM 近零由规范化函数提前排除。
/// - 所有换算只使用快照数据，不查询 BeatMap。
///
/// @details 坐标与性能：
/// - `localMouseY` 是 Image 局部坐标。
/// - snapThreshold 使用逻辑像素并与渲染元素 Y 比较。
/// - `size` 只用于候选时间的正向投影。
/// - 准备快照偏移由正反投影函数内部统一处理。
/// - 函数只遍历当前可见元素和已缓存分段。
/// - 不访问文件系统或 ECS registry。
/// - 不发布逻辑命令。
/// - 不改变 beat divisor 或 BPM 数据。
/// - 调用方决定是否显示吸附反馈。
/// - 创建与拖动共用该入口，保持定位规则一致。
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
        if ( (segment.effects & Common::Render::SCROLL_EFFECT_BPM) == 0 ) {
            continue;
        }

        const double bpm = ::MMM::normalizeBpmValue(segment.bpmValue);

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

        double relativeTime    = rawTime - point.time;
        double stepCount       = editorConfig.settings.snapFloor
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
///
/// @details BPM 数据准备：
/// - 只读取当前快照 ScrollSegment 中带 BPM mask 的项。
/// - 每个 BPM 通过统一 normalize 入口规整。
/// - 非法有限值钳制到最接近自身的合法边界。
/// - 非有限值按 normalize 语义使用有效回退。
/// - 同一时间戳的后续 BPM 覆盖前一项。
/// - 快照分段顺序作为 BPM 点顺序，不在热路径重新排序。
/// - 没有 BPM 点时返回原始时间。
/// - beat divisor 非正时使用兼容默认值四。
///
/// @details 吸附计算：
/// - 可配置是否在首个 Timing 之前绘制并吸附拍线。
/// - 禁用首点前拍线时，早于首 BPM 的时间原样返回。
/// - 每个 BPM 段只处理落在其时间范围内的 rawTime。
/// - 拍长为 60/BPM。
/// - 细分步长为拍长除以 beat divisor。
/// - 非有限或接近零的步长被忽略。
/// - snapFloor 模式向前取整细分位置。
/// - 普通模式四舍五入到最近细分位置。
/// - floor 计算加入微小容差吸收边界浮点误差。
/// - 候选不得越过下一 BPM 时间点。
/// - 候选时间最小钳制到零。
/// - 非有限候选被忽略。
/// - 多个可用分段候选选择时间距离最近者。
/// - 返回值不受像素 snapThreshold 限制。
/// - 该入口用于 Shift 时间滑块的强制拍线定位。
/// - 函数不修改 `m_isTimeSnapped` 或创建弹窗状态。
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
        if ( (segment.effects & Common::Render::SCROLL_EFFECT_BPM) == 0 ) {
            continue;
        }

        const double bpm = ::MMM::normalizeBpmValue(
            segment.bpmValue, m_currentSnapshot->fallbackBpm);

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
        double stepCount    = editorConfig.settings.snapFloor
                                  ? std::floor(relativeTime / stepDuration + 1e-6)
                                  : std::round(relativeTime / stepDuration);
        double candidate    = point.time + stepCount * stepDuration;
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
/// @details 专业模式先按鼠标 X 选择 BPM、Scroll、Jump 或 HS 泳道；
/// 随后同时保存原始时间和吸附时间，位置来源可选择鼠标或当前播放头。
/// 默认参数按 TimingEffect 初始化，实际创建留给弹窗确认命令。
/// @warning UI 低频入口：只在创建手势触发时调用，不访问文件系统。
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

    if ( Config::AppConfig::instance().getEditorSettings().professionalMode ) {
        // 专业模式点击泳道之外时不推测类型，也不打开无目标弹窗。
        auto createType = professionalCreateTypeAtX(size, localMouseX);
        if ( !createType ) {
            return;
        }
        m_createType = *createType;
    }

    m_createTimeRaw = canvasTimeAtLocalY(size, localMouseY);
    // 原始与吸附值同时保留，弹窗可以让用户明确选择定位来源。
    bool snapped = false;
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
    ::MMM::UI::FeedbackOpenPopup("TimelineCreateEvent");
}

/// @brief 收集当前快照中可交互的 Timing 目标。
///
/// @details 展开规则：
/// - 一个 TimelineInteractiveElement 可同时包含多种 TimingEffect。
/// - effect mask 决定是否展开对应类型。
/// - 每种类型通过专用字段取得 entity 和参数值。
/// - null entity 不生成交互目标。
/// - 目标顺序先按快照元素，再按 `TIMELINE_EFFECT_ORDER`。
/// - 每个目标复制时间、Y、类型和值。
/// - 类型专属 marker geometry 优先于旧版公共 geometry。
/// - 专属 geometry 无效时兼容读取 element 公共范围。
/// - 顶点和索引 offset/count 只按值复制。
/// - 结果不保存 element 或快照内部指针。
/// - reserve 以两倍元素数作为常见容量估计。
/// - 空快照返回空向量。
/// - 本函数不筛选 BPM 的用户选择设置。
/// - 调用方可继续按可选择性、视口和命中矩形过滤。
/// - 本函数不排序、不查询 ECS、不发布事件。
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
/// @details 非空的 Scroll、Jump 和 HS 始终可选；BPM 是否进入时间线选择
/// 由 `timelineSelectionIncludesBpm` 显式控制，创建和直接编辑不受此开关影响。
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
/// @details 专业模式按四等分泳道居中；普通模式 BPM 靠左、Scroll 靠右，
/// Jump 与 HS 分别位于内部三分之一和三分之二处。结果为屏幕坐标。
/// @param target Timing 目标。
/// @param canvasPos 画布左上角屏幕坐标。
/// @param size 当前 Timeline 画布尺寸。
/// @return 目标中心 X 坐标。
float TimelineCanvas::timingTargetCenterX(const TimelineHitTarget& target,
                                          const ImVec2&            canvasPos,
                                          const ImVec2&            size) const
{
    if ( Config::AppConfig::instance().getEditorSettings().professionalMode ) {
        constexpr float laneCount = 4.0f;
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
///
/// @details 矩形来源：
/// - 始终先按类型中心和最小拾取半径建立 fallback rect。
/// - marker geometry 有效时扫描其顶点范围计算真实外框。
/// - 顶点结束索引钳制到快照 vertices 长度。
/// - 起始索引越界时忽略真实几何。
/// - 顶点局部坐标加 canvasPos 转为屏幕坐标。
/// - 任意倒置边界由 makeRect 规范为 min/max。
/// - 零面积真实外框被视为无效。
/// - 有效真实外框与 fallback rect 合并。
/// - 合并保证窄皮肤 marker 仍具备最低可点面积。
/// - fallback 同时让没有几何的旧快照仍可交互。
/// - 返回矩形供 hover、单击和框选复用。
/// - 该函数不修改顶点或交互状态。
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
/// @details 对每个展开目标计算屏幕矩形，只保留包含当前 ImGui 鼠标的目标；
/// 重叠时以纵向中心距离为主、横向距离乘 0.15 为辅选择最低分，
/// 使同一时间附近优先匹配视觉上最接近的 marker，同时保留泳道区分。
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
        // 无效矩形或指针不在矩形内的目标不参与重叠评分。
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
        // 纵向时间位置权重大于横向泳道差异，符合时间线主要阅读方向。
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
/// @details 先从可见交互目标构建当前实体集合，再删除 null 或已消失实体；
/// 逻辑删除、会话切换和快照更新后都不会保留悬空选择 ID。
/// @warning UI 热路径：只处理当前可见目标与本地选择集合，不访问 registry。
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
/// @details 复制选择集合后逐实体发布删除命令，避免发布过程中快照更新影响
/// 当前迭代；最后立即清空本地选择，视觉状态无需等待逻辑快照回流。
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
/// @details 擦除预览每帧只保留当前命中的单一实体；空命中清空红色预览，
/// 不在拖动经过过程中累积多个删除目标。
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
///
/// @details 删除集合语义：
/// - 当前没有擦除预览目标时不发布命令。
/// - 若目标属于当前多选集合，则删除整个选择集合。
/// - 若目标未选中，则只删除当前擦除目标。
/// - 擦除目标始终并入最终集合，避免选择状态竞争遗漏。
/// - unordered_set 对多来源实体 ID 去重。
/// - 每个实体发布一条 `CmdDeleteTimelineEvent`。
/// - 已提交实体同步从本地选择集合移除。
/// - 逻辑层负责实体有效性与撤销历史。
/// - 本函数不直接修改 Timeline 或 registry。
/// - 调用方在释放后清除擦除预览集合。
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
///
/// @details 剪贴板内容：
/// - 剪贴板按当前活动会话上下文隔离。
/// - 无活动会话时使用空上下文处理公共剪贴板。
/// - 每次复制先清空 TimelineCanvas 本地兼容缓存。
/// - 空选择会把编辑器级剪贴板更新为空。
/// - 只复制当前快照仍存在且位于选择集合的目标。
/// - 失效选择不会产生占位条目。
/// - 目标按时间升序稳定排序。
/// - 同时间目标按 TimingEffect 枚举顺序排序。
/// - 第一项时间作为相对秒锚点。
/// - 第一项连续 beat 作为相对 beat 锚点。
/// - 每项保存相对秒差。
/// - 每项保存相对连续 beat 差。
/// - 每项保存 effect 和原始参数值。
/// - 编辑器级条目还保存 TimelineComponent 元数据。
/// - beat 位置标志表示可按节奏基础粘贴。
/// - BPM 换算只读取当前快照构建的节奏时间线。
/// - setTimelineClipboard 按值接管共享条目数组。
/// - cut 在剪贴板建立成功后删除原选择。
/// - 普通 copy 不改变选择或谱面数据。
/// - 删除仍通过独立逻辑命令进入撤销历史。
/// @param cut 是否在复制后删除原 Timing。
void TimelineCanvas::copySelectedTimingEvents(bool cut)
{
    auto activeSession = Logic::EditorEngine::instance().getActiveSession();
    const auto* clipboardContext =
        activeSession ? &activeSession->getContext() : nullptr;
    m_timingClipboard.clear();
    if ( m_selectedTimingEntities.empty() ) {
        Logic::EditorEngine::instance().setTimelineClipboard(
            {}, clipboardContext, false);
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
            {}, clipboardContext, false);
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
        entry.relativeTime = target.time - anchorTime;
        entry.relativeBeat = timelineClipboardTimeToBeat(
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
        std::move(sharedClipboard), clipboardContext, false);

    if ( cut ) {
        deleteSelectedTimingEvents();
    }
}

/// @brief 将编辑器级 Timeline 剪贴板粘贴到指定锚点时间。
///
/// @details 粘贴基础：
/// - 从当前活动会话上下文读取编辑器级剪贴板。
/// - 空剪贴板不改变选择或谱面。
/// - 开始有效粘贴前清除本地旧选择。
/// - 所有新事件聚合为一个批量创建命令。
/// - 批量命令保持剪贴板条目顺序。
/// - 秒基础使用 anchorTime 加 relativeTime。
/// - beat 基础使用锚点连续 beat 加 relativeBeat。
/// - beat 结果再通过目标谱面 BPM 时间线换算为秒。
/// - copyPasteTimeBasis 明确控制秒或 beat 语义。
/// - 所有条目具有 beat 位置时才允许 beat 粘贴。
/// - 当前快照存在时才允许构建目标 BPM 时间线。
/// - 剪贴板包含 BPM 事件时强制回退相对秒。
/// - 原因是同批新 BPM 尚未应用，无法可靠反算后续目标拍位。
/// - fallback BPM 通过统一规范化入口处理。
/// - 每个最终目标时间最小钳制为零。
/// - effect、value 和 metadata 原样进入创建事件。
/// - 非空批次只发布一条 `CmdCreateTimelineEvents`。
/// - UI 不直接创建 entt entity。
/// - 新实体选择由逻辑快照回流后的交互处理决定。
/// @param anchorTime 粘贴锚点时间，单位秒。
void TimelineCanvas::pasteTimingClipboard(double anchorTime)
{
    auto activeSession = Logic::EditorEngine::instance().getActiveSession();
    const auto* clipboardContext =
        activeSession ? &activeSession->getContext() : nullptr;
    auto timingClipboard =
        Logic::EditorEngine::instance().getTimelineClipboard(clipboardContext);
    if ( timingClipboard.empty() ) {
        return;
    }

    m_selectedTimingEntities.clear();
    Logic::CmdCreateTimelineEvents batch;
    batch.events.reserve(timingClipboard.size());
    const bool containsPastedBpm = std::any_of(
        timingClipboard.begin(), timingClipboard.end(), [](const auto& entry) {
            return entry.timeline.m_effect == ::MMM::TimingEffect::BPM;
        });
    // 新 BPM 会改变其后事件的拍位时间映射。批量命令尚未应用时无法用目标
    // 谱面的旧 BPM 时间线可靠换算，因此保留来源相对时间，避免静默贴错拍位。
    const bool pasteByBeat =
        Config::AppConfig::instance().getEditorSettings().copyPasteTimeBasis ==
            Config::CopyPasteTimeBasis::Beat &&
        m_currentSnapshot && !containsPastedBpm &&
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
                                 entry.timeline.m_value,
                                 entry.timeline.m_metadata });
    }
    if ( !batch.events.empty() ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent{ std::move(batch) });
    }
}

/// @brief 处理 Timeline 画布上的 Timing 选择、框选、拖动和快捷键。
///
/// @details 输入前置：
/// - 无快照或无谱面时清除 hover 与擦除状态。
/// - 每帧先清理快照中已不存在的选择实体。
/// - 鼠标坐标从屏幕位置转换为 Image 局部位置。
/// - 右上菜单按钮区域不参与 Timing 拾取。
/// - 专业模式使用 X 所在泳道限定创建类型。
/// - hover 目标来自统一 marker 屏幕矩形拾取。
/// - hover 时显示参数、单位和秒时间 tooltip。
/// - hover marker 使用 Hand 光标。
/// - 文本输入、弹窗和快捷键录制会阻挡键盘动作。
/// - 播放中允许查看，但不开始修改事务。
///
/// @details 选择语义：
/// - Move 工具单击 marker 选择该实体。
/// - Ctrl/Command 单击切换实体选择状态。
/// - 未按修饰键时点击新实体替换原选择。
/// - BPM 是否可选由用户设置控制。
/// - 空白单击可清除选择。
/// - Marquee 工具空白拖动建立矩形选择。
/// - 框选按下时保存原选择集作为修饰键基线。
/// - Ctrl/Command 框选把命中项并入原选择。
/// - 普通框选以当前命中集合替换选择。
/// - 框选矩形同时比较 X 与 Y 范围。
/// - marker 使用真实几何与最小拾取矩形的并集。
/// - 框选越出视口时使用非阻塞自动滚动。
/// - 相机滚动后立即刷新矩形终点和命中集合。
///
/// @details 拖动语义：
/// - 在已选 marker 上按下可开始多选整体拖动。
/// - 在未选 marker 上按下先按选择规则更新集合。
/// - 拖动起始时间使用被抓取目标的原始时间。
/// - 每个受影响实体保存原始时间和 effect。
/// - 鼠标 Y 每帧反投影为候选时间。
/// - 候选时间通过统一 Timing 吸附入口规整。
/// - preview delta 是候选时间减抓取起始时间。
/// - 所有拖动条目共享同一个 preview delta。
/// - 预览阶段不写回逻辑 Timeline。
/// - 释放时逐实体发布更新命令。
/// - 每个最终时间最小钳制为零。
/// - 小于微秒容差的零位移不发布更新。
/// - 收尾后清空拖动条目和预览状态。
///
/// @details Draw 工具：
/// - 左键空白时间线开始 Timing 创建预览。
/// - 专业模式按泳道确定创建类型。
/// - 普通模式沿用创建弹窗当前类型。
/// - 按下时记录原始时间和吸附时间。
/// - 拖动期间更新创建预览位置。
/// - 释放时打开创建弹窗并保留最终候选。
/// - 双击或既有交互入口可以直接打开编辑弹窗。
/// - 右键按下开始擦除预览。
/// - 擦除拖动只跟随当前 hover 目标。
/// - 红色半透明 marker 表示待删除状态。
/// - 右键释放提交当前擦除集合。
/// - 擦除命中选中项时删除整个选择集合。
///
/// @details 快捷键与剪贴板：
/// - Delete 删除当前选择。
/// - Copy 建立编辑器级 Timeline 剪贴板。
/// - Cut 先复制再删除选择。
/// - Paste 以当前悬浮或播放时间作为锚点。
/// - Escape 取消活动拖动、框选、预览或擦除状态。
/// - 全局工具切换由上层菜单与快捷键系统负责。
/// - 快捷键只在时间线持有交互焦点时处理。
/// - 本函数不直接访问系统文件或剪贴板文本格式。
///
/// @details 手势收尾：
/// - 鼠标释放即提交最终状态，不等待固定时间窗口。
/// - 指针离开 Image 后已有手势仍可收尾。
/// - 窗口失焦时取消或完成不能继续的瞬态状态。
/// - 创建、拖动、框选和擦除状态互斥。
/// - 打开 modal 前清除其它手势状态。
/// - 所有谱面修改通过 LogicCommandEvent 发布。
/// - UI 本地集合只承担即时视觉反馈。
/// - 下一帧快照是逻辑结果的唯一事实来源。
///
/// @details 状态成员归属：
/// - `m_hoveredTimingEntity` 保存本帧单一 hover 实体。
/// - `m_selectedTimingEntities` 保存当前多选集合。
/// - `m_isTimingDragging` 表示已取得左键拖动所有权。
/// - `m_timingDragEntries` 保存拖动实体的冻结起始数据。
/// - `m_timingDragStartTime` 保存被抓取目标起始时间。
/// - `m_timingDragPreviewDelta` 保存所有目标共享的候选时间差。
/// - `m_isTimingMarqueeSelecting` 表示空白框选手势活动。
/// - `m_timingMarqueeBaseSelection` 保存按下时的选择基线。
/// - 四个 marquee 坐标保存局部矩形起止点。
/// - `m_isTimingErasing` 表示右键擦除事务活动。
/// - `m_timingEraseTargetEntities` 保存本帧待删除预览目标。
/// - `m_isTimingDrawPreviewing` 表示创建候选正在跟随鼠标。
/// - `m_timingDrawPreviewTime` 保存吸附后的创建候选时间。
/// - `m_timingDrawPreviewY` 由视觉装饰阶段更新。
/// - `m_isPopupOpen` 表示已有事件编辑 modal 活动。
/// - `m_isCreatePopupOpen` 表示创建事件 modal 活动。
/// - `m_editingEntity` 只在编辑弹窗生命周期内有效。
/// - `m_createTimeRaw` 保留鼠标反投影原始时间。
/// - `m_createTimeSnapped` 保留节奏吸附候选时间。
/// - `m_isTimeSnapped` 说明创建弹窗是否应展示吸附状态。
/// - 这些成员均属于当前 TimelineCanvas 实例，不跨谱面共享。
///
/// @details 互斥关系：
/// - 拖动活动时不能同时开始框选。
/// - 拖动活动时不能同时开始创建预览。
/// - 擦除活动时不能同时开始左键编辑事务。
/// - 编辑弹窗打开时不开始任何画布手势。
/// - 创建弹窗打开时不开始任何画布手势。
/// - 菜单按钮区域不开始 Timing 手势。
/// - 播放状态阻止创建、拖动、擦除和删除。
/// - hover 读取不受这些编辑互斥限制。
/// - 选择集合可在弹窗打开期间保留视觉上下文。
/// - Escape 清理瞬态手势但不删除持久 Timing。
/// - 工具切换后不属于该工具的活动状态会在对应分支收尾。
/// - 鼠标释放只提交取得该按键所有权的事务。
/// - 右键擦除与 Ctrl/Command 选择修饰语义分开处理。
/// - 创建弹窗的类型选择不修改专业模式泳道配置。
///
/// @details 命令边界：
/// - 选择集合变化通常只留在 UI 本地状态。
/// - 创建发布批量或单项 Timeline 创建命令。
/// - 拖动释放发布 Timeline 更新命令。
/// - 删除与擦除发布 Timeline 删除命令。
/// - 剪贴板复制通过 EditorEngine 的会话上下文接口。
/// - 粘贴使用批量创建命令形成单一逻辑操作。
/// - 本函数不直接调用 BeatmapSession 的可变 registry。
/// - 本函数不持有逻辑命令返回值或等待执行完成。
/// - 快照回流前的视觉连续性由本地预览状态保证。
/// - 快照回流后失效实体由 prune 统一清理。
/// - 命令中的时间统一使用 double，避免长谱面精度损失。
/// - UI 像素坐标统一使用 float，与 ImGui 几何保持一致。
/// - entity 只作为跨线程稳定标识传递，不在本函数解引用。
/// - 多选容器通过集合去重，同一实体不会重复提交。
/// - 每个结束分支都会清理对应瞬态状态，避免跨手势复用。
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
    const bool professionalMode =
        Config::AppConfig::instance().getEditorSettings().professionalMode;
    auto hoveredTarget    = isHovered && !overMenuButton
                                ? pickTimingTarget(canvasPos, size, localMouseY)
                                : std::nullopt;
    m_hoveredTimingEntity = hoveredTarget ? hoveredTarget->entity : entt::null;

    if ( isHovered && hoveredTarget ) {
        const auto descriptor =
            timelineTimingTooltipDescriptor(hoveredTarget->effect);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%.*s: %.6g%.*s\n%.3fs",
                          static_cast<int>(descriptor.label.size()),
                          descriptor.label.data(),
                          hoveredTarget->value,
                          static_cast<int>(descriptor.valueSuffix.size()),
                          descriptor.valueSuffix.data(),
                          hoveredTarget->time);
    }

    const auto updateTimingDragPreview = [&]() {
        double currentTime = canvasTimeAtLocalY(size, localMouseY);
        bool   snapped     = false;
        currentTime = snapTimingTime(size, currentTime, localMouseY, snapped);
        m_timingDragPreviewDelta = currentTime - m_timingDragStartTime;
    };
    const auto finishTimingDrag = [&]() {
        if ( std::abs(m_timingDragPreviewDelta) > 1e-6 ) {
            for ( const auto& entry : m_timingDragEntries ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
                        entry.entity,
                        std::max(0.0,
                                 entry.originalTime + m_timingDragPreviewDelta),
                        entry.value }));
            }
        }
        m_isTimingDragging       = false;
        m_timingDragPreviewDelta = 0.0;
        m_timingDragEntries.clear();
    };

    if ( m_currentSnapshot->isPlaying ) {
        // 播放时禁止开始新编辑，但保留播放前已开始的抓取或框选，使交互
        // 在时间线滚动期间持续更新，并允许按住鼠标再次切换播放状态。
        m_isTimingDrawPreviewing = false;
        m_isTimingErasing        = false;
        m_timingEraseTargetEntities.clear();
        if ( !m_isTimingDragging && !m_isTimingMarqueeSelecting ) {
            return;
        }
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
    bool        handledKeyboardCommand = m_currentSnapshot->isPlaying;
    if ( !handledKeyboardCommand && ctrl && ImGui::IsKeyPressed(ImGuiKey_Z) ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            shift ? Logic::LogicCommand{ Logic::CmdRedo{} }
                  : Logic::LogicCommand{ Logic::CmdUndo{} }));
        handledKeyboardCommand = true;
    }
    if ( !handledKeyboardCommand && ctrl && ImGui::IsKeyPressed(ImGuiKey_Y) ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdRedo{}));
        handledKeyboardCommand = true;
    }
    const std::array<Logic::EditTool, 5> editableTools{
        Logic::EditTool::Move,        Logic::EditTool::Marquee,
        Logic::EditTool::Draw,        Logic::EditTool::ColorBrush,
        Logic::EditTool::ColorEraser,
    };
    const bool isLayoutEditing =
        Logic::EditorEngine::instance().getCurrentTool() ==
        Logic::EditTool::Layout;
    if ( !handledKeyboardCommand && !isLayoutEditing ) {
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
            updateTimingDragPreview();
        }
        if ( m_isTimingDragging &&
             !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            finishTimingDrag();
        }
        break;
    case Logic::EditTool::Marquee: {
        auto makeMarqueeRect = [&]() {
            TimingSelectionRect rect;
            rect.left = canvasPos.x +
                        std::min(m_timingMarqueeStartX, m_timingMarqueeEndX);
            rect.right = canvasPos.x +
                         std::max(m_timingMarqueeStartX, m_timingMarqueeEndX);
            rect.top = canvasPos.y +
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
    case Logic::EditTool::ColorEraser:
    case Logic::EditTool::Layout: break;
    }
}

/// @brief 绘制 Timeline Timing 的 hover、选中、拖动和框选反馈。
///
/// @details 绘制边界：
/// - 覆盖层只在当前快照存在时绘制。
/// - 所有几何追加到当前 ImGui 窗口 DrawList。
/// - 外层 clip rect 严格限制在 Timeline Image。
/// - 覆盖层不创建 ImGui item，不参与命中。
/// - hover、选中和 marker glow 由快照装饰函数负责。
/// - 本函数只绘制无法放入 Vulkan 快照的即时 UI 提示。
///
/// @details 拖动提示：
/// - 只有 Timing 拖动活动且时间增量超过容差时显示。
/// - 文本使用带正负号的三位小数秒格式。
/// - 提示跟随当前鼠标屏幕位置。
/// - 十二像素偏移避免覆盖鼠标热点。
/// - 文字使用高对比半透明白色。
/// - 候选 marker 本体由 interaction decoration 绘制。
/// - 零位移不显示冗余 `+0.000s`。
///
/// @details 框选提示：
/// - 起点和终点均使用 Image 局部坐标存储。
/// - 绘制前分别对 X 与 Y 取 min/max。
/// - 因此可支持任意拖动方向。
/// - 局部边界叠加 canvasPos 转为屏幕矩形。
/// - 低 alpha 蓝色填充保留底层 marker 可见性。
/// - 较高 alpha 边框明确选择范围。
/// - 框选命中计算与该矩形使用相同起止状态。
/// - 自动滚动时终点持续更新，绘制不会滞后。
/// - 松开后状态机先关闭框选，本层随即停止绘制。
/// - PushClipRect 与 PopClipRect 在所有分支成对。
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
        // 时间文本属于屏幕空间，不受时间线局部 Y 偏移影响。
        const ImVec2      mousePos = ImGui::GetMousePos();
        const std::string deltaText =
            fmt::format("{:+.3f}s", m_timingDragPreviewDelta);
        drawList->AddText(ImVec2(mousePos.x + 12.0f, mousePos.y + 12.0f),
                          IM_COL32(255, 255, 255, 230),
                          deltaText.c_str());
    }

    if ( m_isTimingMarqueeSelecting ) {
        // 规范化四条边后再加屏幕原点，支持从任意角向反方向拖动。
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
    // 恢复窗口绘制列表裁剪栈，后续弹窗不受 Image 范围限制。
}

}  // namespace MMM::Canvas
