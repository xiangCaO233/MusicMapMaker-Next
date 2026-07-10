#include "audio/AudioManager.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/TimeFormatUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <system_error>

namespace MMM::Canvas
{
namespace
{
/// @brief 连续拖动编辑命令的像素去重阈值。
constexpr float CONTINUOUS_EDIT_MOUSE_EPSILON = 0.75f;

/// @brief 画布悬浮信息相对鼠标的屏幕偏移。
constexpr float CANVAS_HOVER_OVERLAY_OFFSET = 15.0f;

/// @brief 将 ASCII 扩展名转换为小写。
/// @param value 输入扩展名。
/// @return 小写后的扩展名。
std::string toLowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 判断拖拽路径是否为 zip 兼容谱面包。
/// @param path 拖拽路径。
/// @return 扩展名匹配临时阅览包格式时返回 true。
bool isTemporaryPackagePath(const std::filesystem::path& path)
{
    const auto extension = toLowerAscii(Config::pathToUtf8(path.extension()));
    return extension == ".zip" || extension == ".7z" || extension == ".mcz" ||
           extension == ".osz" || extension == ".mpk";
}

/// @brief 用于主画布拖动吸附的 BPM 网格区间。
struct BpmSnapSpan {
    /// @brief BPM 段起始时间，单位秒。
    double time{ 0.0 };

    /// @brief 下一个 BPM 段起始时间，单位秒。
    double nextTime{ std::numeric_limits<double>::infinity() };

    /// @brief BPM 值。
    double bpm{ 120.0 };
};

/// @brief 规整用于分拍吸附的 BPM 值。
/// @param bpm 原始 BPM。
/// @param fallbackBpm 快照回退 BPM。
/// @return 可用于计算分拍间隔的 BPM。
/// @warning UI 热路径：拖动画布磁吸时调用；只做常量级数值规整。
double normalizedSnapBpm(double bpm, double fallbackBpm)
{
    double result = bpm;
    if ( result <= 0.0 || !std::isfinite(result) ) {
        result = fallbackBpm;
    }
    if ( result <= 0.0 || !std::isfinite(result) ) {
        result = 120.0;
    }
    return std::min(result, 10000.0);
}

/// @brief 查找指定时间所在的 BPM 分拍区间。
/// @param snapshot 当前渲染快照。
/// @param rawTime 目标显示时间，单位秒。
/// @param allowBeforeFirstTiming 是否允许在首个 BPM 前反推分拍网格。
/// @param outSpan 输出 BPM 分拍区间。
/// @return 找到可用区间时返回 true。
/// @warning UI 热路径：仅在磁铁开启且拖动画布时调用；线性扫描 Timing
/// 快照，不访问 ECS 或文件系统。
bool findBpmSnapSpan(const Logic::RenderSnapshot& snapshot, double rawTime,
                     bool allowBeforeFirstTiming, BpmSnapSpan& outSpan)
{
    constexpr double EPSILON = 1e-6;

    bool   hasAny      = false;
    bool   hasSelected = false;
    double firstTime   = 0.0;
    double firstBpm    = 120.0;

    for ( const auto& segment : snapshot.scrollSegments ) {
        if ( (segment.effects & Logic::System::SCROLL_EFFECT_BPM) == 0 ) {
            continue;
        }

        const double bpm =
            normalizedSnapBpm(segment.bpmValue, snapshot.fallbackBpm);
        if ( !hasAny ) {
            hasAny    = true;
            firstTime = segment.time;
            firstBpm  = bpm;
        }

        if ( segment.time <= rawTime + EPSILON ) {
            outSpan.time = segment.time;
            outSpan.bpm  = bpm;
            hasSelected  = true;
            continue;
        }

        if ( hasSelected ) {
            outSpan.nextTime = segment.time;
            return true;
        }

        break;
    }

    if ( hasSelected ) {
        outSpan.nextTime = std::numeric_limits<double>::infinity();
        return true;
    }

    if ( hasAny && rawTime < firstTime && allowBeforeFirstTiming ) {
        outSpan.time     = firstTime;
        outSpan.nextTime = std::numeric_limits<double>::infinity();
        outSpan.bpm      = firstBpm;
        for ( const auto& segment : snapshot.scrollSegments ) {
            if ( (segment.effects & Logic::System::SCROLL_EFFECT_BPM) == 0 ) {
                continue;
            }
            if ( std::abs(segment.time - firstTime) <= EPSILON ) {
                outSpan.bpm =
                    normalizedSnapBpm(segment.bpmValue, snapshot.fallbackBpm);
            } else if ( segment.time > firstTime + EPSILON ) {
                outSpan.nextTime = segment.time;
                break;
            }
        }
        return true;
    }

    if ( !hasAny ) {
        outSpan.time     = 0.0;
        outSpan.nextTime = std::numeric_limits<double>::infinity();
        outSpan.bpm      = normalizedSnapBpm(0.0, snapshot.fallbackBpm);
        return true;
    }

    return false;
}

/// @brief 磁铁开启时将拖动画布目标时间转换为已跨过的分拍线。
/// @param snapshot 当前渲染快照。
/// @param rawTargetTime 连续拖动换算出的目标显示时间，单位秒。
/// @param startTime 拖动开始时的当前显示时间，单位秒。
/// @param snapToWholeBeat 是否按整拍吸附；为 false 时按当前分拍数吸附。
/// @param outTime 输出吸附后的显示时间。
/// @return 成功换算时返回 true。
/// @warning UI 热路径：Move 工具空白拖动画布时每帧调用；读取当前编辑器配置，
/// 只扫描 Timing 快照并做常量级数学计算。
bool snapCanvasPanTargetTime(const Logic::RenderSnapshot& snapshot,
                             double rawTargetTime, double startTime,
                             bool snapToWholeBeat, double& outTime)
{
    if ( !std::isfinite(rawTargetTime) || !std::isfinite(startTime) ) {
        return false;
    }

    const auto& editorConfig =
        Logic::EditorEngine::instance().getEditorConfig();

    int beatDivisor = editorConfig.settings.beatDivisor;
    if ( beatDivisor <= 0 ) {
        beatDivisor = 4;
    }

    BpmSnapSpan span;
    if ( !findBpmSnapSpan(snapshot,
                          rawTargetTime,
                          editorConfig.visual.drawBeatLinesBeforeFirstTiming,
                          span) ) {
        return false;
    }

    const double beatDuration = 60.0 / span.bpm;
    const double stepDuration =
        snapToWholeBeat ? beatDuration
                        : beatDuration / static_cast<double>(beatDivisor);
    if ( stepDuration <= 1e-9 || !std::isfinite(stepDuration) ) {
        return false;
    }

    constexpr double EPSILON       = 1e-6;
    const bool       movingLater   = rawTargetTime > startTime + EPSILON;
    const bool       movingEarlier = rawTargetTime < startTime - EPSILON;
    if ( !movingLater && !movingEarlier ) {
        outTime = startTime;
        return true;
    }

    const double relativeTime = rawTargetTime - span.time;
    const double stepCount =
        movingLater ? std::floor(relativeTime / stepDuration + EPSILON)
                    : std::ceil(relativeTime / stepDuration - EPSILON);
    double candidate = span.time + stepCount * stepDuration;
    if ( candidate > span.nextTime ) {
        candidate = span.nextTime;
    }
    candidate = std::max(0.0, candidate);
    if ( !std::isfinite(candidate) ) {
        return false;
    }

    if ( movingLater && candidate <= startTime + EPSILON ) {
        outTime = startTime;
    } else if ( movingEarlier && candidate >= startTime - EPSILON ) {
        outTime = startTime;
    } else {
        outTime = candidate;
    }
    return true;
}

/// @brief 跳转到主画布当前悬浮时间点。
/// @param snapshot 当前渲染快照，时间字段使用视觉时间域。
/// @return 成功发送跳转命令时返回 true。
/// @warning UI 输入路径：只在点击导航手势触发时调用；不访问 ECS 或文件系统。
bool publishCanvasHoverSeek(const Logic::RenderSnapshot& snapshot)
{
    const double targetTime =
        snapshot.isSnapped ? snapshot.snappedTime : snapshot.hoveredTime;
    if ( !std::isfinite(targetTime) ||
         std::abs(targetTime - snapshot.currentTime) <= 1e-6 ) {
        return false;
    }

    const double visualOffset = Config::AppConfig::instance()
                                    .getVisualConfig()
                                    .getEffectiveVisualOffset();
    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdSeek{ targetTime - visualOffset }));
    return true;
}

/// @brief 获取无 ScrollSegment 快照下的默认绝对 Y 速度。
/// @return 默认绝对 Y 速度，单位像素/秒。
double defaultSnapshotAbsYSpeed()
{
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    return 500.0 * static_cast<double>(std::max(0.01f, visual.timelineZoom));
}

/// @brief 从 UI 快照估算指定显示时间对应的绝对 Y。
/// @param snapshot 当前渲染快照。
/// @param time 显示时间，单位秒。
/// @return 对应的绝对 Y。
/// @warning UI 热路径：Move 工具空白拖动画布时调用；只读取快照中的
/// ScrollSegment，不访问 ECS 或文件系统。
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

/// @brief 尝试在指定 ScrollSegment 内按绝对 Y 反算显示时间。
/// @param snapshot 当前渲染快照。
/// @param index 目标 ScrollSegment 索引。
/// @param absY 目标绝对 Y。
/// @param outTime 反算出的显示时间。
/// @return 该 segment 覆盖目标绝对 Y 时返回 true。
/// @warning UI 热路径：优先测试当前时间所在 segment，跨段时才由调用方扩展搜索。
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

/// @brief 从 UI 快照估算指定绝对 Y 对应的显示时间。
/// @param snapshot 当前渲染快照。
/// @param absY 目标绝对 Y。
/// @return 对应的显示时间，单位秒。
/// @warning UI 热路径：Move 工具空白拖动画布时调用；通常命中当前
/// ScrollSegment，跨段拖拽时才扫描快照分段。
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

/// @brief 计算框选拖出画布上下边缘时的自动滚动目标时间。
/// @param snapshot 当前渲染快照，时间字段使用视觉时间域。
/// @param viewportHeight 当前画布高度，单位像素。
/// @param mouseY 当前本地鼠标 Y 坐标，单位像素。
/// @param deltaTime 当前 UI 帧间隔，单位秒。
/// @param scrolled 输出是否需要执行自动滚动。
/// @return 自动滚动后的显示时间，单位秒。
/// @warning UI 热路径：框选拖动时每帧调用；只做数值换算并读取快照。
/// @param isAccelerated 是否应用 Shift 加速。
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

/// @brief 根据画布 Y 坐标计算鼠标下方的显示时间。
/// @param snapshot 当前渲染快照。
/// @param viewportHeight 当前画布高度，单位像素。
/// @param mouseY 当前鼠标 Y 坐标，单位像素。
/// @return 鼠标下方的显示时间。
/// @warning UI 热路径：Move 工具空白按下时调用；只读取当前快照。
double canvasTimeAtMouseY(const Logic::RenderSnapshot& snapshot,
                          float viewportHeight, float mouseY)
{
    if ( !std::isfinite(mouseY) || !std::isfinite(viewportHeight) ||
         viewportHeight <= 1.0f ) {
        return snapshot.currentTime;
    }

    const auto&  visual = Config::AppConfig::instance().getVisualConfig();
    const double scale  = std::abs(snapshot.renderScaleY) > 1e-6f
                              ? static_cast<double>(snapshot.renderScaleY)
                              : 1.0;
    const double judgmentLineY = static_cast<double>(viewportHeight) *
                                 static_cast<double>(visual.judgeline_pos);
    const double currentAbsY =
        snapshotAbsYAtTime(snapshot, snapshot.currentTime);
    return snapshotTimeAtAbsY(
        snapshot,
        currentAbsY + (judgmentLineY - static_cast<double>(mouseY)) / scale);
}

/// @brief 根据拖动画布锚点计算主画布滚动后的显示时间。
/// @param snapshot 当前渲染快照。
/// @param viewportHeight 当前画布高度，单位像素。
/// @param mouseY 当前鼠标 Y 坐标，单位像素。
/// @param startTime 拖动开始时的当前显示时间，单位秒。
/// @param anchorTime 拖动开始时鼠标抓住的显示时间。
/// @param anchorMouseY 拖动开始时鼠标所在的本地 Y 坐标，单位像素。
/// @param accelerate 是否使用快速拖动倍率。
/// @return 保持锚点贴合鼠标位置所需的当前显示时间。
/// @warning UI 热路径：Move 工具空白拖动画布时每帧调用；读取当前编辑器配置，
/// 只读取快照与做数值换算，不访问 ECS 或文件系统。
double canvasPanTargetTime(const Logic::RenderSnapshot& snapshot,
                           float viewportHeight, float mouseY, double startTime,
                           double anchorTime, float anchorMouseY,
                           bool accelerate)
{
    if ( !std::isfinite(mouseY) || !std::isfinite(anchorMouseY) ||
         !std::isfinite(viewportHeight) || viewportHeight <= 1.0f ||
         !std::isfinite(startTime) || !std::isfinite(anchorTime) ) {
        return snapshot.currentTime;
    }

    double multiplier = 1.0;
    if ( accelerate ) {
        multiplier = std::max(1.0f,
                              Logic::EditorEngine::instance()
                                  .getEditorConfig()
                                  .settings.scrollSpeedMultiplier);
    }
    const double effectiveMouseY =
        static_cast<double>(anchorMouseY) +
        (static_cast<double>(mouseY) - static_cast<double>(anchorMouseY)) *
            multiplier;

    const auto&  visual = Config::AppConfig::instance().getVisualConfig();
    const double scale  = std::abs(snapshot.renderScaleY) > 1e-6f
                              ? static_cast<double>(snapshot.renderScaleY)
                              : 1.0;
    const double judgmentLineY = static_cast<double>(viewportHeight) *
                                 static_cast<double>(visual.judgeline_pos);
    const double anchorAbsY    = snapshotAbsYAtTime(snapshot, anchorTime);
    const double targetCurrentAbsY =
        anchorAbsY - (judgmentLineY - effectiveMouseY) / scale;
    const double rawTargetTime =
        snapshotTimeAtAbsY(snapshot, targetCurrentAbsY);
    const auto& editorConfig =
        Logic::EditorEngine::instance().getEditorConfig();
    if ( editorConfig.settings.scrollSnap ) {
        double snappedTime = rawTargetTime;
        if ( snapCanvasPanTargetTime(snapshot,
                                     rawTargetTime,
                                     startTime,
                                     accelerate,
                                     snappedTime) ) {
            return snappedTime;
        }
    }
    return rawTargetTime;
}

/// @brief 开始一个固定在当前 ImGui viewport 内的画布悬浮信息窗口。
/// @param mousePos 当前鼠标屏幕坐标。
/// @return `ImGui::Begin` 的返回值；调用方必须始终调用 `ImGui::End`。
bool beginCanvasHoverOverlay(const ImVec2& mousePos)
{
    ImGuiViewport* viewport = ImGui::GetWindowViewport();
    ImVec2         pivot{ 0.0f, 0.0f };
    float          xOffset = CANVAS_HOVER_OVERLAY_OFFSET;
    float          yOffset = CANVAS_HOVER_OVERLAY_OFFSET;
    if ( viewport && mousePos.x > viewport->Pos.x + viewport->Size.x * 0.5f ) {
        pivot.x = 1.0f;
        xOffset = -CANVAS_HOVER_OVERLAY_OFFSET;
    }
    if ( viewport && mousePos.y > viewport->Pos.y + viewport->Size.y * 0.5f ) {
        pivot.y = 1.0f;
        yOffset = -CANVAS_HOVER_OVERLAY_OFFSET;
    }

    if ( viewport ) {
        ImGui::SetNextWindowViewport(viewport->ID);
    }
    ImGui::SetNextWindowPos(ImVec2(mousePos.x + xOffset, mousePos.y + yOffset),
                            ImGuiCond_Always,
                            pivot);
    ImGui::SetNextWindowBgAlpha(0.7f);

    constexpr ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoDocking;
    return ImGui::Begin("##CanvasHoverInspectOverlay", nullptr, overlayFlags);
}
}  // namespace

Basic2DCanvasInteraction::Basic2DCanvasInteraction(
    const std::string& canvasName, const std::string& cameraId)
    : m_canvasName(canvasName), m_cameraId(cameraId)
{
    m_dropSubId = Event::EventBus::instance().subscribe<Event::GLFWDropEvent>(
        [this](const Event::GLFWDropEvent& e) {
            XINFO("CanvasInteraction received GLFWDropEvent with {} paths",
                  e.paths.size());
            m_pendingDrops.push_back({ e.paths, e.pos });
        });
}

Basic2DCanvasInteraction::~Basic2DCanvasInteraction()
{
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(m_dropSubId);
}

/// @brief 判断连续拖动编辑命令是否需要发送，并更新缓存。
bool Basic2DCanvasInteraction::shouldSendContinuousEditCommand(
    LastContinuousEditCommand& last, glm::vec2 pos,
    const Logic::RenderSnapshot& snapshot, bool primaryModifier,
    bool secondaryModifier)
{
    constexpr double visualTimeEpsilon  = 1e-6;
    constexpr float  renderScaleEpsilon = 1e-5f;
    const bool       shouldSend =
        !last.valid ||
        std::abs(last.pos.x - pos.x) > CONTINUOUS_EDIT_MOUSE_EPSILON ||
        std::abs(last.pos.y - pos.y) > CONTINUOUS_EDIT_MOUSE_EPSILON ||
        std::abs(last.visualTime - snapshot.currentTime) > visualTimeEpsilon ||
        std::abs(last.visibleTimeStart - snapshot.visibleTimeStart) >
            visualTimeEpsilon ||
        std::abs(last.visibleTimeEnd - snapshot.visibleTimeEnd) >
            visualTimeEpsilon ||
        std::abs(last.renderScaleY - snapshot.renderScaleY) >
            renderScaleEpsilon ||
        last.beatDivisor != snapshot.currentBeatDivisor ||
        last.primaryModifier != primaryModifier ||
        last.secondaryModifier != secondaryModifier;
    if ( shouldSend ) {
        last.valid             = true;
        last.pos               = pos;
        last.visualTime        = snapshot.currentTime;
        last.visibleTimeStart  = snapshot.visibleTimeStart;
        last.visibleTimeEnd    = snapshot.visibleTimeEnd;
        last.renderScaleY      = snapshot.renderScaleY;
        last.beatDivisor       = snapshot.currentBeatDivisor;
        last.primaryModifier   = primaryModifier;
        last.secondaryModifier = secondaryModifier;
    }
    return shouldSend;
}

/// @brief 清空连续拖动编辑命令缓存。
void Basic2DCanvasInteraction::resetContinuousEditCommands()
{
    m_lastMarqueeUpdateCommand.valid = false;
    m_lastBrushUpdateCommand.valid   = false;
    m_lastMoveUpdateCommand.valid    = false;
    m_lastEraseUpdateCommand.valid   = false;
    m_rightEraseActive               = false;
}

void Basic2DCanvasInteraction::update(
    UI::UIManager* sourceManager, const Logic::RenderSnapshot* currentSnapshot,
    float targetWidth, float targetHeight)
{
    handleDrops(sourceManager);

    if ( currentSnapshot ) {
        handleHotkeys(currentSnapshot);
        handleInteractions(currentSnapshot, targetWidth, targetHeight);
    }

    updateTransientUi();
}

/// @brief 处理活动主画布上的 Ctrl/Command/Alt 修饰键滚轮。
bool Basic2DCanvasInteraction::handleModifierWheel(
    const Logic::RenderSnapshot* currentSnapshot, bool allowSelectionScroll)
{
    if ( !currentSnapshot ) {
        return false;
    }

    const auto& io               = ImGui::GetIO();
    const float wheel            = io.MouseWheel;
    const bool  isCommandPressed = io.KeyCtrl || io.KeySuper;
    if ( std::abs(wheel) <= 0.01f || (!isCommandPressed && !io.KeyAlt) ) {
        return false;
    }

    const bool isAltPressed   = io.KeyAlt;
    const bool isShiftPressed = io.KeyShift;
    auto&      engine         = Logic::EditorEngine::instance();
    const bool scrollsActiveSelection =
        allowSelectionScroll && isCommandPressed && !isAltPressed &&
        currentSnapshot->currentTool == Logic::EditTool::Marquee &&
        currentSnapshot->isSelecting;
    if ( !scrollsActiveSelection ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdScroll{
                m_cameraId,
                0.0f,
                false,
                Logic::ScrollCommandIntent::ModifierAdjustment }));
    }

    if ( isCommandPressed && isAltPressed ) {
        constexpr std::array<double, 4> presets = { 0.25, 0.50, 0.75, 1.0 };
        double                          currentSpeed =
            Audio::AudioManager::instance().getPlaybackSpeed();

        size_t bestIdx = 0;
        double minDiff = std::abs(currentSpeed - presets[0]);
        for ( size_t i = 1; i < presets.size(); ++i ) {
            double diff = std::abs(currentSpeed - presets[i]);
            if ( diff < minDiff ) {
                minDiff = diff;
                bestIdx = i;
            }
        }

        if ( wheel > 0.01f ) {
            if ( bestIdx < presets.size() - 1 ) bestIdx++;
        } else if ( wheel < -0.01f ) {
            if ( bestIdx > 0 ) bestIdx--;
        }

        double newSpeed = presets[bestIdx];
        if ( std::abs(newSpeed - currentSpeed) > 1e-4 ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetPlaybackSpeed{ newSpeed }));
            m_speedTooltipValue = static_cast<float>(newSpeed);
            m_speedTooltipTimer = 2.0f;
            ::MMM::UI::PlayInteractionMouseUpFeedback();
        }
        return true;
    }

    if ( isCommandPressed ) {
        if ( scrollsActiveSelection ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdScroll{ m_cameraId, -wheel, isShiftPressed }));
        } else {
            auto  editorCfg = engine.getEditorConfig();
            float step      = 0.1f;
            if ( isShiftPressed )
                step *= editorCfg.settings.scrollSpeedMultiplier;
            const float currentZoom = editorCfg.visual.timelineZoom;
            const float newZoom =
                std::clamp(currentZoom + wheel * step, 0.1f, 10.0f);
            if ( std::abs(newZoom - currentZoom) > 0.0001f ) {
                editorCfg.visual.timelineZoom = newZoom;
                engine.setEditorConfig(editorCfg);
                ::MMM::UI::PlayInteractionMouseUpFeedback();
            }
        }
        return true;
    }

    auto      editorCfg       = engine.getEditorConfig();
    const int originalDivisor = editorCfg.settings.beatDivisor;

    static std::unordered_map<std::string, float> wheelAccumulator;
    float& acc = wheelAccumulator[m_cameraId];
    acc += wheel;

    int steps = 0;
    if ( acc >= 1.0f ) {
        steps = static_cast<int>(acc);
        acc -= static_cast<float>(steps);
    } else if ( acc <= -1.0f ) {
        steps = static_cast<int>(acc);
        acc -= static_cast<float>(steps);
    }

    if ( steps != 0 ) {
        if ( isShiftPressed ) {
            constexpr std::array<int, 8> presets = { 1, 2, 3, 4, 6, 8, 12, 16 };
            int current = editorCfg.settings.beatDivisor;

            if ( steps > 0 ) {
                for ( int i = 0; i < steps; ++i ) {
                    auto it = std::upper_bound(
                        presets.begin(), presets.end(), current);
                    current = it != presets.end() ? *it : presets.back();
                }
            } else {
                for ( int i = 0; i < -steps; ++i ) {
                    auto it = std::lower_bound(
                        presets.begin(), presets.end(), current);
                    current = it != presets.begin() ? *std::prev(it)
                                                    : presets.front();
                }
            }
            editorCfg.settings.beatDivisor = current;
        } else {
            editorCfg.settings.beatDivisor += steps;
        }
        editorCfg.settings.beatDivisor =
            std::clamp(editorCfg.settings.beatDivisor, 1, 64);
        if ( editorCfg.settings.beatDivisor != originalDivisor ) {
            engine.setEditorConfig(editorCfg);
            ::MMM::UI::PlayInteractionMouseUpFeedback();
        }
    }

    return true;
}

/// @brief 推进并绘制交互层的临时 UI。
/// @warning UI 热路径：每帧最多绘制一个播放速度提示窗口。
void Basic2DCanvasInteraction::updateTransientUi()
{
    if ( m_speedTooltipTimer > 0.0f ) {
        m_speedTooltipTimer -= ImGui::GetIO().DeltaTime;

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2         mousePos = ImGui::GetMousePos();

        // 始终跟随鼠标，并根据屏幕位置自动调整对齐方式（边缘翻转）
        ImVec2 pivot = ImVec2(0.0f, 0.0f);
        if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f )
            pivot.x = 1.0f;
        if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f )
            pivot.y = 1.0f;

        float offsetX = (pivot.x == 0.0f) ? 20.0f : -20.0f;
        float offsetY = (pivot.y == 0.0f) ? 20.0f : -20.0f;

        ImGui::SetNextWindowPos(
            ImVec2(mousePos.x + offsetX, mousePos.y + offsetY),
            ImGuiCond_Always,
            pivot);
        ImGui::SetNextWindowBgAlpha(0.7f);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 10));
        if ( ImGui::Begin("##SpeedTooltip", nullptr, flags) ) {
            ImFont* font = Config::SkinManager::instance().getFont("content");
            if ( font ) ImGui::PushFont(font, font->LegacySize);
            ImGui::Text(TR("ui.toolbar.playback_speed_value").data(),
                        m_speedTooltipValue);
            if ( font ) ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

void Basic2DCanvasInteraction::handleDrops(UI::UIManager* sourceManager)
{
    if ( m_pendingDrops.empty() ) return;

    bool isHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    if ( isHovered ) {
        for ( const auto& drop : m_pendingDrops ) {
            if ( !drop.paths.empty() ) {
                std::filesystem::path p = Config::utf8ToPath(drop.paths[0]);
                if ( isTemporaryPackagePath(p) ) {
                    XINFO("Package dropped on Canvas: {}",
                          Config::pathToUtf8(p));

                    Event::OpenTemporaryProjectPackageEvent ev;
                    ev.m_packagePath = p;
                    Event::EventBus::instance().publish(ev);
                    continue;
                }

                std::error_code filesystemError;
                const bool      isDirectory =
                    std::filesystem::is_directory(p, filesystemError) &&
                    !filesystemError;
                std::filesystem::path projectPath =
                    isDirectory ? p : p.parent_path();
                auto ext = Config::pathToUtf8(p.extension());

                XINFO("File dropped on Canvas: {}, opening project: {}",
                      Config::pathToUtf8(p),
                      Config::pathToUtf8(projectPath));

                // 1. 打开项目
                Event::OpenProjectEvent ev;
                ev.m_projectPath = projectPath;
                Event::EventBus::instance().publish(ev);

                // 2. 跳转到谱面管理器
                Event::UISubViewToggleEvent evt;
                evt.sourceUiName           = m_canvasName;
                evt.uiManager              = sourceManager;
                evt.targetFloatManagerName = "SideBarManager";
                evt.subViewId =
                    UI::TabToSubViewId(UI::SideBarTab::BeatMapExplorer);
                evt.showSubView = true;
                Event::EventBus::instance().publish(evt);

                // 3. 如果是谱面文件，直接加载
                if ( ext == ".osu" || ext == ".imd" || ext == ".mc" ||
                     ext == ".mmm" ) {
                    XINFO("Auto-loading beatmap from drop: {}",
                          Config::pathToUtf8(p.filename()));
                    auto loadedMap = MMM::BeatMap::loadFromFile(p);
                    if ( loadedMap.m_baseMapMetadata.map_path.empty() ) {
                        XERROR("Failed to load dropped beatmap: {}",
                               Config::pathToUtf8(p));
                    } else {
                        auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
                            std::move(loadedMap));
                        Logic::EditorEngine::instance().createSession(
                            loadedBeatmap, Config::pathToUtf8(p.filename()));
                    }
                }
            }
        }
    }
    m_pendingDrops.clear();
}

/// @brief 处理主画布工具切换和编辑快捷键。
/// @param currentSnapshot 当前渲染快照。
/// @warning UI 热路径：每帧检查输入状态；禁止加入文件系统访问、ECS
/// 全量遍历或阻塞操作。
void Basic2DCanvasInteraction::handleHotkeys(
    const Logic::RenderSnapshot* currentSnapshot)
{
    // 如果键盘焦点不在主画布，跳过画布快捷键处理，避免穿透设置页或时间线窗口。
    if ( UI::ShortcutUtils::shouldBlockCanvasEditingShortcuts() ) return;
    if ( UI::ShortcutUtils::isShortcutRecordingActive() ) return;

    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    const std::array<Logic::EditTool, 5> editableTools{
        Logic::EditTool::Move,        Logic::EditTool::Marquee,
        Logic::EditTool::Draw,        Logic::EditTool::ColorBrush,
        Logic::EditTool::ColorEraser,
    };

    bool handledShortcut = false;
    for ( Logic::EditTool tool : editableTools ) {
        if ( UI::ShortcutUtils::isShortcutPressed(
                 UI::ShortcutUtils::getToolShortcut(settings, tool)) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdChangeTool{ tool }));
            handledShortcut = true;
            break;
        }
    }

    if ( !handledShortcut && UI::ShortcutUtils::isShortcutPressed(
                                 settings.shortcutConfig.deleteSelected) ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdDeleteSelected{}));
    }

    // 注意：Ctrl+C/V/X/Z/Y 和 Space (播放/暂停) 已由全局 MainMenuView 处理，
    // 在此处移除以防止重复触发。
}

/// @brief 处理主画布鼠标悬停、点击、拖拽和滚轮交互。
/// @param currentSnapshot 当前渲染快照。
/// @param targetWidth 画布宽度。
/// @param targetHeight 画布高度。
/// @warning UI 热路径约束如下。
/// 热路径：每帧执行并可能推送逻辑命令；禁止加入文件系统访问、完整排序或阻塞操作。
void Basic2DCanvasInteraction::handleInteractions(
    const Logic::RenderSnapshot* currentSnapshot, float targetWidth,
    float targetHeight)
{
    ImVec2     mousePos         = ImGui::GetMousePos();
    ImVec2     windowPos        = ImGui::GetCursorScreenPos();
    const bool hasValidMousePos = ImGui::IsMousePosValid(&mousePos) &&
                                  std::isfinite(mousePos.x) &&
                                  std::isfinite(mousePos.y);
    ImVec2     localMousePos{ 0.0f, 0.0f };
    if ( hasValidMousePos ) {
        localMousePos = { mousePos.x - windowPos.x, mousePos.y - windowPos.y };
    } else if ( m_lastMouseCommand.valid ) {
        localMousePos = { m_lastMouseCommand.pos.x, m_lastMouseCommand.pos.y };
    }

    const bool isInsideCanvas =
        hasValidMousePos && targetWidth > 0.0f && targetHeight > 0.0f &&
        localMousePos.x >= 0.0f && localMousePos.x <= targetWidth &&
        localMousePos.y >= 0.0f && localMousePos.y <= targetHeight;
    bool isHovered  = isInsideCanvas && ImGui::IsWindowHovered();
    bool isDragging = hasValidMousePos && ImGui::IsMouseDragging(0);

    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    const auto& layout = visual.trackLayout;
    const float normX =
        targetWidth > 0.0f ? localMousePos.x / targetWidth : 0.0f;
    const float normY =
        targetHeight > 0.0f ? localMousePos.y / targetHeight : 0.0f;
    const bool isMouseInTrackLayout =
        isHovered && normX >= layout.left && normX <= layout.right &&
        normY >= layout.top && normY <= layout.bottom;

    constexpr float mouseEpsilon = 0.1f;
    bool            shouldSendMouse =
        !m_lastMouseCommand.valid ||
        std::abs(m_lastMouseCommand.pos.x - localMousePos.x) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.pos.y - localMousePos.y) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportWidth - targetWidth) >
            mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportHeight - targetHeight) >
            mouseEpsilon ||
        m_lastMouseCommand.isHovering != isHovered ||
        m_lastMouseCommand.isDragging != isDragging;

    if ( shouldSendMouse ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetMousePosition{ .cameraId       = m_cameraId,
                                        .mouseX         = localMousePos.x,
                                        .mouseY         = localMousePos.y,
                                        .viewportWidth  = targetWidth,
                                        .viewportHeight = targetHeight,
                                        .isHovering     = isHovered,
                                        .isDragging     = isDragging }));
        m_lastMouseCommand.valid         = true;
        m_lastMouseCommand.pos           = { localMousePos.x, localMousePos.y };
        m_lastMouseCommand.viewportWidth = targetWidth;
        m_lastMouseCommand.viewportHeight = targetHeight;
        m_lastMouseCommand.isHovering     = isHovered;
        m_lastMouseCommand.isDragging     = isDragging;
    }

    // --- 交互：显示精确时间戳工具提示 ---
    if ( isHovered && currentSnapshot->isHoveringCanvas &&
         !currentSnapshot->isPlaying ) {
        if ( isMouseInTrackLayout ) {
            bool isEditTool =
                (currentSnapshot->currentTool != Logic::EditTool::Move &&
                 currentSnapshot->currentTool != Logic::EditTool::Marquee);

            if ( currentSnapshot->isSnapped || isEditTool ||
                 currentSnapshot->hoverInspect.show ) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(12, 12));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));

                const bool showHoverOverlay = beginCanvasHoverOverlay(mousePos);
                if ( showHoverOverlay ) {
                    if ( currentSnapshot->hoverInspect.show ) {
                        const auto& inspect = currentSnapshot->hoverInspect;
                        auto drawPoint = [currentSnapshot](
                                             const char* labelKey,
                                             const Logic::HoverBeatPoint& point,
                                             bool showTrack) {
                            if ( !point.show ) return;
                            const auto label = TR(labelKey);
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s %s: %d + %d/%d",
                                label.data(),
                                TR("ui.canvas.note_fraction").data(),
                                point.beatIndex,
                                point.numerator,
                                point.denominator);
                            const auto timeText =
                                formatCanvasTime(point.time, currentSnapshot);
                            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                               "%s %s: %s",
                                               label.data(),
                                               TR("ui.canvas.note_time").data(),
                                               timeText.c_str());
                            if ( showTrack ) {
                                ImGui::TextColored(
                                    ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                    "%s %s: %d",
                                    label.data(),
                                    TR("ui.canvas.track").data(),
                                    point.track + 1);
                            }
                        };

                        switch ( inspect.kind ) {
                        case Logic::HoverInspectKind::Note:
                            drawPoint("ui.canvas.hover.note",
                                      inspect.head,
                                      inspect.showTrack);
                            break;
                        case Logic::HoverInspectKind::HoldHead:
                            drawPoint(
                                "ui.canvas.hover.head", inspect.head, true);
                            break;
                        case Logic::HoverInspectKind::HoldEnd:
                        case Logic::HoverInspectKind::PolylineHoldEnd:
                            drawPoint(
                                "ui.canvas.hover.hold_end", inspect.end, true);
                            break;
                        case Logic::HoverInspectKind::FlickHead:
                            drawPoint("ui.canvas.hover.flick_head",
                                      inspect.head,
                                      true);
                            break;
                        case Logic::HoverInspectKind::FlickBody:
                        case Logic::HoverInspectKind::PolylineFlickBody:
                            drawPoint("ui.canvas.hover.flick_body",
                                      inspect.body,
                                      false);
                            break;
                        case Logic::HoverInspectKind::FlickEnd:
                        case Logic::HoverInspectKind::PolylineFlickEnd:
                            drawPoint(
                                "ui.canvas.hover.flick_end", inspect.end, true);
                            break;
                        case Logic::HoverInspectKind::PolylineHead:
                            drawPoint("ui.canvas.hover.polyline_head",
                                      inspect.body,
                                      inspect.showTrack);
                            break;
                        case Logic::HoverInspectKind::PolylineNode:
                            drawPoint("ui.canvas.hover.polyline_node",
                                      inspect.body,
                                      inspect.showTrack);
                            break;
                        case Logic::HoverInspectKind::HoldBody:
                        case Logic::HoverInspectKind::PolylineHoldBody:
                        case Logic::HoverInspectKind::None: break;
                        }

                        if ( inspect.showDuration ) {
                            const auto durationText =
                                formatCanvasDuration(inspect.duration);
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s: %s",
                                TR("ui.canvas.hover.duration").data(),
                                durationText.c_str());
                        }
                        if ( inspect.showDtrack ) {
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s: %d",
                                TR("ui.canvas.hover.dtrack").data(),
                                inspect.dtrack);
                        }
                        if ( inspect.showTrack &&
                             (inspect.kind ==
                                  Logic::HoverInspectKind::HoldBody ||
                              inspect.kind ==
                                  Logic::HoverInspectKind::PolylineHoldBody) ) {
                            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                               "%s: %d",
                                               TR("ui.canvas.track").data(),
                                               inspect.track + 1);
                        }

                        // 重叠数量沿用渲染遮罩的检测结果，避免回退到旧的包围盒计数。
                        int overlappingCount =
                            std::max(1, inspect.overlapCount);
                        for ( const auto& mask :
                              currentSnapshot->overlapMasks ) {
                            if ( localMousePos.x >= mask.x &&
                                 localMousePos.x <= mask.x + mask.w &&
                                 localMousePos.y >= mask.y &&
                                 localMousePos.y <= mask.y + mask.h ) {
                                overlappingCount = std::max(overlappingCount,
                                                            mask.objectCount);
                            }
                        }

                        if ( overlappingCount > 1 ) {
                            ImGui::TextColored(
                                ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                                "%s: %d%s",
                                TR("ui.canvas.overlapping_hitboxes").data(),
                                overlappingCount,
                                TR("ui.canvas.overlapping_warning").data());
                        } else {
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s: %d",
                                TR("ui.canvas.overlapping_hitboxes").data(),
                                overlappingCount);
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                    }

                    if ( currentSnapshot->isSnapped ) {
                        const auto timeText = formatCanvasTime(
                            currentSnapshot->snappedTime, currentSnapshot);
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                           "%s: %s",
                                           TR("ui.canvas.snap").data(),
                                           timeText.c_str());

                        if ( currentSnapshot->snappedNumerator == 1 &&
                             currentSnapshot->snappedDenominator == 1 ) {
                            ImGui::TextColored(
                                ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                                "%s (1/1)",
                                TR("ui.canvas.beat_fraction").data());
                        } else {
                            ImGui::TextColored(
                                ImVec4(0.8f, 0.9f, 1.0f, 1.0f),
                                "%s (%d/%d)",
                                TR("ui.canvas.beat_fraction").data(),
                                currentSnapshot->snappedNumerator,
                                currentSnapshot->snappedDenominator);
                        }
                    } else {
                        const auto timeText = formatCanvasTime(
                            currentSnapshot->hoveredTime, currentSnapshot);
                        ImGui::Text("%s: %s",
                                    TR("ui.canvas.time").data(),
                                    timeText.c_str());
                    }

                    if ( currentSnapshot->hoveredBeatIndex > 0 ) {
                        ImGui::Text("%s: %d",
                                    TR("ui.canvas.beat_index").data(),
                                    currentSnapshot->hoveredBeatIndex);
                    }

                    ImGui::Text("%s: %d",
                                TR("ui.canvas.track").data(),
                                currentSnapshot->hoveredTrack + 1);

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                                       "%s: %d",
                                       TR("ui.canvas.beat_divisor").data(),
                                       currentSnapshot->currentBeatDivisor);
                    if ( m_hoverLayerCount > 1 ) {
                        ImGui::TextColored(
                            ImVec4(0.8f, 0.9f, 1.0f, 1.0f),
                            "%s: %d/%d  %s",
                            TR("ui.canvas.hover.layer").data(),
                            m_hoverLayerIndex + 1,
                            m_hoverLayerCount,
                            TR("ui.canvas.hover.layer_hint").data());
                    }
                }

                ImGui::End();
                ImGui::PopStyleVar(2);
            }
        }
    }

    entt::entity hoveredEntity   = entt::null;
    uint8_t      hoveredPart     = 0;
    int          hoveredSubIndex = -1;

    struct HoverCandidate {
        entt::entity     entity{ entt::null };
        Logic::HoverPart part{ Logic::HoverPart::None };
        int              subIndex{ -1 };
    };

    std::vector<HoverCandidate> candidates;
    std::string                 layerSignature;
    if ( isHovered ) {
        for ( auto it = currentSnapshot->hitboxes.rbegin();
              it != currentSnapshot->hitboxes.rend();
              ++it ) {
            if ( localMousePos.x >= it->x && localMousePos.x <= it->x + it->w &&
                 localMousePos.y >= it->y &&
                 localMousePos.y <= it->y + it->h ) {
                candidates.push_back({ it->entity, it->part, it->subIndex });
                layerSignature +=
                    std::to_string(
                        static_cast<uint32_t>(entt::to_integral(it->entity))) +
                    ":" + std::to_string(static_cast<uint32_t>(it->part)) +
                    ":" + std::to_string(it->subIndex) + ";";
            }
        }
    }

    if ( layerSignature != m_hoverLayerSignature ) {
        m_hoverLayerSignature = layerSignature;
        m_hoverLayerIndex     = 0;
    }

    m_hoverLayerCount = static_cast<int>(candidates.size());
    if ( candidates.empty() ) {
        m_hoverLayerIndex = 0;
    } else {
        if ( m_hoverLayerIndex >= m_hoverLayerCount ) {
            m_hoverLayerIndex = m_hoverLayerCount - 1;
        }

        if ( m_hoverLayerCount > 1 && isHovered &&
             !ImGui::GetIO().WantTextInput ) {
            if ( ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ) {
                m_hoverLayerIndex = (m_hoverLayerIndex + 1) % m_hoverLayerCount;
            } else if ( ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ) {
                m_hoverLayerIndex =
                    (m_hoverLayerIndex + m_hoverLayerCount - 1) %
                    m_hoverLayerCount;
            }
        }

        const auto& candidate = candidates[m_hoverLayerIndex];
        hoveredEntity         = candidate.entity;
        hoveredPart           = static_cast<uint8_t>(candidate.part);
        hoveredSubIndex       = candidate.subIndex;
    }

    bool shouldSendHover = !m_hasLastHovered ||
                           m_lastHoveredEntity != hoveredEntity ||
                           m_lastHoveredPart != hoveredPart ||
                           m_lastHoveredSubIndex != hoveredSubIndex;
    if ( shouldSendHover ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdSetHoveredEntity{
                hoveredEntity, hoveredPart, hoveredSubIndex }));
        m_hasLastHovered      = true;
        m_lastHoveredEntity   = hoveredEntity;
        m_lastHoveredPart     = hoveredPart;
        m_lastHoveredSubIndex = hoveredSubIndex;
    }

    auto processColorToolTarget = [&](Logic::EditTool tool) {
        if ( currentSnapshot->isPlaying || hoveredEntity == entt::null ) return;
        if ( !m_colorStrokeEntities.insert(hoveredEntity).second ) return;

        if ( tool == Logic::EditTool::ColorBrush ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdApplyBrushPaletteToEntity{ hoveredEntity }));
        } else if ( tool == Logic::EditTool::ColorEraser ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdClearNoteColorOverrides{ hoveredEntity }));
        }
    };

    const bool leftClicked =
        ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
    const bool leftDoubleClicked =
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool leftSeekClicked =
        isMouseInTrackLayout && currentSnapshot->isHoveringCanvas &&
        currentSnapshot->hasBeatmap && !currentSnapshot->isPlaying &&
        currentSnapshot->currentTool != Logic::EditTool::Draw &&
        leftDoubleClicked;

    if ( leftSeekClicked ) {
        m_leftPressStartedOnCanvas      = false;
        m_leftPressStartedInTrackLayout = false;
        m_leftPressStartedOnEntity      = false;
        m_leftPressDragged              = false;
        m_isCanvasPanning               = false;
        m_canvasPanStartTime            = 0.0;
        m_canvasPanAnchorMouseY         = 0.0f;
        m_colorStrokeEntities.clear();
        resetContinuousEditCommands();
        publishCanvasHoverSeek(*currentSnapshot);
    } else if ( leftClicked ) {
        m_leftPressStartedOnCanvas      = isHovered;
        m_leftPressStartedInTrackLayout = isMouseInTrackLayout;
        m_leftPressStartedOnEntity      = hoveredEntity != entt::null;
        m_leftPressDragged              = false;
        m_isCanvasPanning               = false;
        m_colorStrokeEntities.clear();
        resetContinuousEditCommands();

        if ( isHovered ) {
            if ( currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
                if ( hoveredEntity != entt::null ) {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(Logic::CmdSelectEntity{
                            hoveredEntity, !ImGui::GetIO().KeyCtrl }));
                } else {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdStartMarquee{ m_cameraId,
                                                    localMousePos.x,
                                                    localMousePos.y,
                                                    ImGui::GetIO().KeyCtrl }));
                }
            } else if ( currentSnapshot->currentTool ==
                        Logic::EditTool::Move ) {
                if ( !currentSnapshot->isPlaying &&
                     hoveredEntity != entt::null ) {
                    // 抓取工具不再负责选中，只负责发起拖拽
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdStartDrag{ hoveredEntity,
                                                 m_cameraId,
                                                 ImGui::GetIO().KeyCtrl }));
                } else if ( !currentSnapshot->isPlaying ) {
                    m_isCanvasPanning       = true;
                    m_canvasPanStartTime    = currentSnapshot->currentTime;
                    m_canvasPanAnchorMouseY = localMousePos.y;
                    m_canvasPanAnchorTime   = canvasTimeAtMouseY(
                        *currentSnapshot, targetHeight, localMousePos.y);
                }
            } else if ( currentSnapshot->currentTool ==
                        Logic::EditTool::Draw ) {
                if ( !currentSnapshot->isPlaying ) {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdStartBrush{ m_cameraId,
                                                  localMousePos.x,
                                                  localMousePos.y,
                                                  ImGui::GetIO().KeyShift,
                                                  ImGui::GetIO().KeyCtrl }));
                }
            } else if ( currentSnapshot->currentTool ==
                        Logic::EditTool::ColorBrush ) {
                processColorToolTarget(Logic::EditTool::ColorBrush);
            } else if ( currentSnapshot->currentTool ==
                        Logic::EditTool::ColorEraser ) {
                processColorToolTarget(Logic::EditTool::ColorEraser);
            }
        }
    }

    if ( ImGui::IsMouseDragging(0) ) {
        m_leftPressDragged = true;

        if ( m_leftPressStartedOnCanvas &&
             currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            bool autoScrolled = false;
            if ( currentSnapshot->hasBeatmap && !currentSnapshot->isPlaying ) {
                const double autoScrollTargetTime =
                    marqueeAutoScrollTargetTime(*currentSnapshot,
                                                targetHeight,
                                                localMousePos.y,
                                                ImGui::GetIO().DeltaTime,
                                                ImGui::GetIO().KeyShift,
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

            const bool playbackScrolled = currentSnapshot->hasBeatmap &&
                                          currentSnapshot->isPlaying &&
                                          currentSnapshot->isSelecting;
            const bool shouldUpdateMarquee =
                shouldSendContinuousEditCommand(
                    m_lastMarqueeUpdateCommand,
                    { localMousePos.x, localMousePos.y },
                    *currentSnapshot,
                    ImGui::GetIO().KeyCtrl,
                    false) ||
                autoScrolled || playbackScrolled;
            if ( shouldUpdateMarquee ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUpdateMarquee{
                        localMousePos.x, localMousePos.y }));
            }
        } else if ( m_leftPressStartedOnCanvas &&
                    currentSnapshot->currentTool == Logic::EditTool::Draw &&
                    (!currentSnapshot->isPlaying ||
                     currentSnapshot->brush.isActive) ) {
            const bool playbackScrolled = currentSnapshot->hasBeatmap &&
                                          currentSnapshot->isPlaying &&
                                          currentSnapshot->brush.isActive;
            if ( shouldSendContinuousEditCommand(
                     m_lastBrushUpdateCommand,
                     { localMousePos.x, localMousePos.y },
                     *currentSnapshot,
                     ImGui::GetIO().KeyShift,
                     ImGui::GetIO().KeyCtrl) ||
                 playbackScrolled ) {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdUpdateBrush{ m_cameraId,
                                           localMousePos.x,
                                           localMousePos.y,
                                           ImGui::GetIO().KeyShift,
                                           ImGui::GetIO().KeyCtrl }));
            }
        } else if ( m_leftPressStartedOnEntity &&
                    currentSnapshot->currentTool == Logic::EditTool::Move ) {
            const bool playbackScrolled =
                currentSnapshot->hasBeatmap && currentSnapshot->isPlaying;
            const bool shouldUpdateMove =
                shouldSendContinuousEditCommand(
                    m_lastMoveUpdateCommand,
                    { localMousePos.x, localMousePos.y },
                    *currentSnapshot,
                    ImGui::GetIO().KeyCtrl,
                    false) ||
                playbackScrolled;
            if ( shouldUpdateMove ) {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdUpdateDrag{ m_cameraId,
                                          localMousePos.x,
                                          localMousePos.y,
                                          ImGui::GetIO().KeyCtrl }));
            }
        } else if ( m_leftPressStartedOnCanvas && !m_leftPressStartedOnEntity &&
                    m_isCanvasPanning && !currentSnapshot->isPlaying &&
                    currentSnapshot->currentTool == Logic::EditTool::Move ) {
            const double targetTime =
                canvasPanTargetTime(*currentSnapshot,
                                    targetHeight,
                                    localMousePos.y,
                                    m_canvasPanStartTime,
                                    m_canvasPanAnchorTime,
                                    m_canvasPanAnchorMouseY,
                                    ImGui::GetIO().KeyShift);
            if ( std::isfinite(targetTime) &&
                 std::abs(targetTime - currentSnapshot->currentTime) > 1e-6 ) {
                const double visualOffset = Config::AppConfig::instance()
                                                .getVisualConfig()
                                                .getEffectiveVisualOffset();
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdSeek{ targetTime - visualOffset }));
            }
        } else if ( m_leftPressStartedOnCanvas &&
                    currentSnapshot->currentTool ==
                        Logic::EditTool::ColorBrush ) {
            processColorToolTarget(Logic::EditTool::ColorBrush);
        } else if ( m_leftPressStartedOnCanvas &&
                    currentSnapshot->currentTool ==
                        Logic::EditTool::ColorEraser ) {
            processColorToolTarget(Logic::EditTool::ColorEraser);
        }
    }

    if ( ImGui::IsMouseReleased(0) ) {
        const bool shiftReleaseSeek =
            m_leftPressStartedOnCanvas && m_leftPressStartedInTrackLayout &&
            isMouseInTrackLayout && !m_leftPressDragged &&
            ImGui::GetIO().KeyShift && currentSnapshot->isHoveringCanvas &&
            currentSnapshot->hasBeatmap && !currentSnapshot->isPlaying &&
            currentSnapshot->currentTool == Logic::EditTool::Move;

        if ( m_leftPressStartedOnCanvas &&
             currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndMarquee{}));
        } else if ( m_leftPressStartedOnCanvas &&
                    currentSnapshot->currentTool == Logic::EditTool::Draw ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndBrush{ m_cameraId }));
        } else if ( m_leftPressStartedOnEntity &&
                    currentSnapshot->currentTool == Logic::EditTool::Move ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndDrag{ m_cameraId }));
        }

        if ( currentSnapshot->currentTool == Logic::EditTool::Move ) {
            if ( m_leftPressStartedOnCanvas && !m_leftPressStartedOnEntity &&
                 !m_leftPressDragged && !shiftReleaseSeek ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdSelectEntity{
                        entt::null, !ImGui::GetIO().KeyCtrl }));
            }
        }

        if ( shiftReleaseSeek ) {
            publishCanvasHoverSeek(*currentSnapshot);
        }

        m_leftPressStartedOnCanvas      = false;
        m_leftPressStartedInTrackLayout = false;
        m_leftPressStartedOnEntity      = false;
        m_leftPressDragged              = false;
        m_isCanvasPanning               = false;
        m_canvasPanStartTime            = 0.0;
        m_canvasPanAnchorMouseY         = 0.0f;
        m_colorStrokeEntities.clear();
        resetContinuousEditCommands();
    }

    const bool ctrlRightRemoveMarquee =
        isHovered && ImGui::IsMouseClicked(1) && ImGui::GetIO().KeyCtrl;

    // --- 右键交互：画笔工具下为擦除 ---
    if ( currentSnapshot->currentTool == Logic::EditTool::Draw ) {
        if ( !currentSnapshot->isPlaying && ImGui::IsMouseClicked(1) &&
             isHovered && !ctrlRightRemoveMarquee ) {
            m_lastEraseUpdateCommand.valid = false;
            m_rightEraseActive             = true;
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdStartErase{ m_cameraId, ImGui::GetIO().KeyShift }));
        }
        if ( m_rightEraseActive && !currentSnapshot->isPlaying &&
             ImGui::IsMouseDragging(1) ) {
            if ( shouldSendContinuousEditCommand(
                     m_lastEraseUpdateCommand,
                     { localMousePos.x, localMousePos.y },
                     *currentSnapshot,
                     ImGui::GetIO().KeyShift,
                     false) ) {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdUpdateErase{ m_cameraId,
                                           localMousePos.x,
                                           localMousePos.y,
                                           ImGui::GetIO().KeyShift }));
            }
        }
        if ( m_rightEraseActive && ImGui::IsMouseReleased(1) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndErase{ m_cameraId }));
            m_lastEraseUpdateCommand.valid = false;
            m_rightEraseActive             = false;
        }
    }

    // --- Ctrl+右键：移除框选框（全局可用） ---
    if ( ctrlRightRemoveMarquee ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdRemoveMarqueeAt{
                m_cameraId, localMousePos.x, localMousePos.y }));
    }

    // --- 交互：鼠标滚轮控制时间跳转与属性修改 ---
    const auto& io    = ImGui::GetIO();
    float       wheel = io.MouseWheel;
    const bool  isModifierWheelHovered =
        isInsideCanvas && (io.KeyCtrl || io.KeySuper || io.KeyAlt) &&
        !ImGui::IsAnyMouseDown() &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if ( (isHovered || isModifierWheelHovered) && std::abs(wheel) > 0.01f ) {
        if ( !handleModifierWheel(currentSnapshot) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdScroll{ m_cameraId, -wheel, io.KeyShift }));

            const bool canUpdateActiveBrush =
                currentSnapshot->currentTool == Logic::EditTool::Draw &&
                ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                (!currentSnapshot->isPlaying ||
                 currentSnapshot->brush.isActive);
            if ( canUpdateActiveBrush ) {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdUpdateBrush{ m_cameraId,
                                           localMousePos.x,
                                           localMousePos.y,
                                           io.KeyShift,
                                           io.KeyCtrl }));
            }
        }
    }
}

}  // namespace MMM::Canvas
