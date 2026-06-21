#include "canvas/TimelineCanvas.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "logic/BeatmapSyncBuffer.h"
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <limits>

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

/// @brief Timeline 画布中按类型排列的 Timing 类型列表。
constexpr ::MMM::TimingEffect TIMELINE_EFFECT_ORDER[] = {
    ::MMM::TimingEffect::BPM,
    ::MMM::TimingEffect::JUMP,
    ::MMM::TimingEffect::HS,
    ::MMM::TimingEffect::SCROLL,
};

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

/// @brief 在指定画布 Y 坐标处准备并打开 Timing 创建弹窗。
/// @param size 当前 Timeline 画布尺寸。
/// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
/// @param useCurrentTime 是否使用当前播放时间而非鼠标命中时间。
void TimelineCanvas::openTimingCreatePopupAtY(const ImVec2& size,
                                              float         localMouseY,
                                              bool          useCurrentTime)
{
    if ( !m_currentSnapshot ) {
        return;
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

    targets.reserve(m_currentSnapshot->timelineElements.size());
    for ( const auto& element : m_currentSnapshot->timelineElements ) {
        for ( auto effect : TIMELINE_EFFECT_ORDER ) {
            if ( (element.effects & getTimingEffectMask(effect)) == 0 ) {
                continue;
            }
            entt::entity entity = getTimingEntity(element, effect);
            if ( entity == entt::null ) {
                continue;
            }

            targets.push_back({ entity,
                                effect,
                                element.time,
                                getTimingValue(element, effect),
                                element.y,
                                element.hasMarkerGeometry,
                                element.markerVertexOffset,
                                element.markerVertexCount,
                                element.markerIndexOffset,
                                element.markerIndexCount });
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

/// @brief 拾取鼠标附近的 Timing 目标。
/// @param canvasPos 画布左上角屏幕坐标。
/// @param size 当前 Timeline 画布尺寸。
/// @param localMouseY 鼠标相对画布左上角的 Y 坐标。
/// @return 命中的 Timing 目标；未命中时为空。
std::optional<TimelineCanvas::TimelineHitTarget>
TimelineCanvas::pickTimingTarget(const ImVec2& canvasPos, const ImVec2& size,
                                 float localMouseY) const
{
    if ( !m_currentSnapshot ) {
        return std::nullopt;
    }

    const ImVec2 mousePos  = ImGui::GetMousePos();
    float        bestScore = std::numeric_limits<float>::max();
    std::optional<TimelineHitTarget> bestTarget;
    for ( const auto& target : collectVisibleTimingTargets() ) {
        if ( !isTimingTargetSelectable(target) ) {
            continue;
        }
        float dy = std::abs(localMouseY - target.y);
        if ( dy > TIMING_PICK_RADIUS ) {
            continue;
        }

        float markerX = timingTargetCenterX(target, canvasPos, size);
        float dx      = std::abs(mousePos.x - markerX);
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
        if ( isTimingTargetSelectable(target) ) {
            snapshotEntities.insert(target.entity);
        }
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
    if ( hoveredTarget && isTimingTargetSelectable(*hoveredTarget) ) {
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

/// @brief 将当前选中的 Timing 复制到 Timeline 本地剪贴板。
/// @param cut 是否在复制后删除原 Timing。
void TimelineCanvas::copySelectedTimingEvents(bool cut)
{
    m_timingClipboard.clear();
    if ( m_selectedTimingEntities.empty() ) {
        return;
    }

    std::vector<TimelineHitTarget> selectedTargets;
    for ( const auto& target : collectVisibleTimingTargets() ) {
        if ( isTimingTargetSelectable(target) &&
             m_selectedTimingEntities.find(target.entity) !=
                 m_selectedTimingEntities.end() ) {
            selectedTargets.push_back(target);
        }
    }
    if ( selectedTargets.empty() ) {
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
    const double anchorTime = selectedTargets.front().time;
    m_timingClipboard.reserve(selectedTargets.size());
    for ( const auto& target : selectedTargets ) {
        m_timingClipboard.push_back(
            { target.time - anchorTime, target.effect, target.value });
    }

    if ( cut ) {
        deleteSelectedTimingEvents();
    }
}

/// @brief 将 Timeline 本地剪贴板粘贴到指定锚点时间。
/// @param anchorTime 粘贴锚点时间，单位秒。
void TimelineCanvas::pasteTimingClipboard(double anchorTime)
{
    if ( m_timingClipboard.empty() ) {
        return;
    }

    m_selectedTimingEntities.clear();
    Logic::CmdCreateTimelineEvents batch;
    batch.events.reserve(m_timingClipboard.size());
    for ( const auto& entry : m_timingClipboard ) {
        batch.events.push_back({ std::max(0.0, anchorTime + entry.relativeTime),
                                 entry.effect,
                                 entry.value });
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
    if ( m_isPopupOpen || m_isCreatePopupOpen || overMenuButton ||
         io.WantTextInput ) {
        if ( m_isTimingErasing &&
             !ImGui::IsMouseDown(ImGuiMouseButton_Right) ) {
            m_isTimingErasing = false;
            m_timingEraseTargetEntities.clear();
        }
        return;
    }

    const bool ctrl              = io.KeyCtrl;
    const bool shift             = io.KeyShift;
    const bool additiveSelection = ctrl || shift;
    if ( ImGui::IsKeyPressed(ImGuiKey_A) && ctrl ) {
        if ( m_currentSnapshot->currentTool == Logic::EditTool::Move ||
             m_currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            m_selectedTimingEntities.clear();
            for ( const auto& target : collectVisibleTimingTargets() ) {
                if ( isTimingTargetSelectable(target) ) {
                    m_selectedTimingEntities.insert(target.entity);
                }
            }
        }
    }
    if ( ImGui::IsKeyPressed(ImGuiKey_C) && ctrl ) {
        copySelectedTimingEvents(false);
    }
    if ( ImGui::IsKeyPressed(ImGuiKey_X) && ctrl ) {
        copySelectedTimingEvents(true);
    }
    if ( ImGui::IsKeyPressed(ImGuiKey_V) && ctrl ) {
        pasteTimingClipboard(std::max(0.0, m_currentSnapshot->currentTime));
    }
    if ( ImGui::IsKeyPressed(ImGuiKey_Delete) ||
         ImGui::IsKeyPressed(ImGuiKey_Backspace) ) {
        deleteSelectedTimingEvents();
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
             ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
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
            openTimingCreatePopupAtY(size, localMouseY, false);
            m_isTimingDrawPreviewing = false;
        }
        break;
    case Logic::EditTool::Move:
        if ( isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
            if ( hoveredTarget ) {
                if ( !additiveSelection &&
                     m_selectedTimingEntities.find(hoveredTarget->entity) ==
                         m_selectedTimingEntities.end() ) {
                    m_selectedTimingEntities.clear();
                }
                if ( additiveSelection &&
                     m_selectedTimingEntities.find(hoveredTarget->entity) !=
                         m_selectedTimingEntities.end() ) {
                    m_selectedTimingEntities.erase(hoveredTarget->entity);
                } else {
                    m_selectedTimingEntities.insert(hoveredTarget->entity);
                }

                m_isTimingDragging       = true;
                m_timingDragStartTime    = hoveredTarget->time;
                m_timingDragPreviewDelta = 0.0;
                m_timingDragEntries.clear();
                for ( const auto& target : collectVisibleTimingTargets() ) {
                    if ( isTimingTargetSelectable(target) &&
                         m_selectedTimingEntities.find(target.entity) !=
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
        auto addMarqueeTargets = [&]() {
            const float minY =
                std::min(m_timingMarqueeStartY, m_timingMarqueeEndY);
            const float maxY =
                std::max(m_timingMarqueeStartY, m_timingMarqueeEndY);
            for ( const auto& target : collectVisibleTimingTargets() ) {
                if ( isTimingTargetSelectable(target) && target.y >= minY &&
                     target.y <= maxY ) {
                    m_selectedTimingEntities.insert(target.entity);
                }
            }
        };

        if ( isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
            m_isTimingMarqueeSelecting = true;
            m_timingMarqueeStartY      = localMouseY;
            m_timingMarqueeEndY        = localMouseY;
            if ( !additiveSelection ) {
                m_selectedTimingEntities.clear();
            }
            addMarqueeTargets();
        }
        if ( m_isTimingMarqueeSelecting &&
             ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            m_timingMarqueeEndY = localMouseY;
            addMarqueeTargets();
        }
        if ( m_isTimingMarqueeSelecting &&
             !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            addMarqueeTargets();
            m_isTimingMarqueeSelecting = false;
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
        const float minY =
            canvasPos.y + std::min(m_timingMarqueeStartY, m_timingMarqueeEndY);
        const float maxY =
            canvasPos.y + std::max(m_timingMarqueeStartY, m_timingMarqueeEndY);
        drawList->AddRectFilled(ImVec2(canvasPos.x, minY),
                                ImVec2(canvasPos.x + size.x, maxY),
                                IM_COL32(120, 170, 255, 42));
        drawList->AddRect(ImVec2(canvasPos.x, minY),
                          ImVec2(canvasPos.x + size.x, maxY),
                          IM_COL32(120, 170, 255, 180),
                          0.0f,
                          0,
                          1.5f);
    }

    drawList->PopClipRect();
}

}  // namespace MMM::Canvas
