#include "canvas/Basic2DCanvasInteraction.h"

#include "audio/AudioManager.h"
#include "canvas/AnnotationDetailLayout.h"
#include "canvas/AnnotationTargetHint.h"
#include "canvas/CanvasBlockedGesture.h"
#include "canvas/CanvasContentVisibility.h"
#include "canvas/HoverLayerSelection.h"
#include "canvas/TimeFormatUtils.h"
#include "common/AudioResourceDragPayload.h"
#include "common/CanvasComponentLayout.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/CreatorIdentity.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/session/CanvasCamera.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/audio/ProjectAudioPreviewControls.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace MMM::Canvas
{
namespace
{
/// @brief 连续拖动编辑命令的像素去重阈值。
constexpr float CONTINUOUS_EDIT_MOUSE_EPSILON = 0.75f;

/// @brief 画布悬浮信息相对鼠标的屏幕偏移。
constexpr float CANVAS_HOVER_OVERLAY_OFFSET = 15.0f;

/// @brief 画布悬浮信息窗口背景透明度。
constexpr float CANVAS_HOVER_OVERLAY_BACKGROUND_ALPHA = 0.70F;

/// @brief 画布悬浮信息窗口的内边距。
constexpr float CANVAS_HOVER_OVERLAY_PADDING = 12.0F;

/// @brief 画布悬浮信息窗口的横向元素间距。
constexpr float CANVAS_HOVER_OVERLAY_ITEM_SPACING_X = 8.0F;

/// @brief 画布悬浮信息窗口的纵向元素间距。
constexpr float CANVAS_HOVER_OVERLAY_ITEM_SPACING_Y = 6.0F;

/// @brief 绘制当前悬浮批注所指向物件的高对比几何提示。
/// @param bounds 批注目标在画布局部坐标中的提示边界。
/// @param canvasPosition 画布左上角屏幕坐标。
/// @param canvasWidth 画布可见宽度。
/// @param canvasHeight 画布可见高度。
/// @warning UI 热路径：悬浮批注详情卡片时每帧调用一次，只追加固定数量 ImGui
/// 几何。
void renderAnnotationTargetHint(const AnnotationTargetHintBounds& bounds,
                                ImVec2 canvasPosition, float canvasWidth,
                                float canvasHeight)
{
    const ImVec2 minimum{ canvasPosition.x + bounds.left,
                          canvasPosition.y + bounds.top };
    const ImVec2 maximum{ canvasPosition.x + bounds.right,
                          canvasPosition.y + bounds.bottom };

    auto& skin        = Config::SkinManager::instance();
    auto  accentColor = skin.getColor("preview.judgeline");
    if ( accentColor.r == 1.0F && accentColor.g == 0.0F &&
         accentColor.b == 1.0F ) {
        const ImVec4 fallback = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        accentColor = { fallback.x, fallback.y, fallback.z, fallback.w };
    }

    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(
        { accentColor.r, accentColor.g, accentColor.b, 1.0F });
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(
        { accentColor.r, accentColor.g, accentColor.b, 0.16F });
    constexpr ImU32 SHADOW   = IM_COL32(0, 0, 0, 230);
    constexpr float ROUNDING = 4.0F;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(
        canvasPosition,
        { canvasPosition.x + canvasWidth, canvasPosition.y + canvasHeight },
        true);
    drawList->AddRectFilled(minimum, maximum, fill, ROUNDING);
    drawList->AddRect(minimum, maximum, SHADOW, ROUNDING, 0, 6.0F);
    drawList->AddRect(minimum, maximum, accent, ROUNDING, 0, 2.5F);

    const float markerX = (minimum.x + maximum.x) * 0.5F;
    drawList->AddTriangleFilled({ markerX - 5.0F, minimum.y - 7.0F },
                                { markerX + 5.0F, minimum.y - 7.0F },
                                { markerX, minimum.y - 1.0F },
                                SHADOW);
    drawList->AddTriangleFilled({ markerX - 3.5F, minimum.y - 5.5F },
                                { markerX + 3.5F, minimum.y - 5.5F },
                                { markerX, minimum.y - 1.5F },
                                accent);
    drawList->PopClipRect();
}

/// @brief 组件布局拖动的基础吸附距离，单位逻辑像素。
constexpr float CANVAS_COMPONENT_SNAP_DISTANCE = 8.0f;

/// @brief 计算物件拖拽靠近单轴视口边缘时的内容平移量。
/// @param coordinate 指针在该轴上的局部坐标。
/// @param extent 视口在该轴上的尺寸。
/// @param deltaTime 当前 UI 帧间隔。
/// @param sensitivity 用户配置的边缘滚动灵敏度。
/// @return 内容应在本帧平移的逻辑像素；靠近起始边缘时为正。
/// @warning UI 热路径：物件拖动期间每帧调用，只做常量数值运算。
float objectDragAutoPanAxisDelta(float coordinate, float extent,
                                 float deltaTime, float sensitivity)
{
    if ( !std::isfinite(coordinate) || !std::isfinite(extent) ||
         extent <= 1.0F || !std::isfinite(sensitivity) ||
         sensitivity <= 0.0F ) {
        return 0.0F;
    }

    const float margin      = std::clamp(extent * 0.08F, 24.0F, 64.0F);
    float       penetration = 0.0F;
    float       direction   = 0.0F;
    if ( coordinate < margin ) {
        penetration = margin - coordinate;
        direction   = 1.0F;
    } else if ( coordinate > extent - margin ) {
        penetration = coordinate - (extent - margin);
        direction   = -1.0F;
    }
    if ( penetration <= 0.0F ) return 0.0F;

    const float frameSeconds = std::clamp(
        std::isfinite(deltaTime) && deltaTime > 0.0F ? deltaTime : 1.0F / 60.0F,
        1.0F / 240.0F,
        1.0F / 15.0F);
    const float     ramp = std::clamp(penetration / margin, 0.0F, 2.0F);
    constexpr float PIXELS_PER_SECOND = 900.0F;
    return direction * PIXELS_PER_SECOND * ramp * ramp * sensitivity *
           frameSeconds;
}

/// @brief 计算左键物件拖拽的二维边缘自动平移量。
/// @param mousePos 指针相对画布的位置。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @param deltaTime 当前 UI 帧间隔。
/// @param sensitivity 用户配置的边缘滚动灵敏度。
/// @return 直接传给 CmdPanCanvas 的二维内容位移。
/// @warning UI 热路径：物件拖动期间每帧调用，不分配内存。
glm::vec2 objectDragAutoPanDelta(glm::vec2 mousePos, float viewportWidth,
                                 float viewportHeight, float deltaTime,
                                 float sensitivity)
{
    return {
        objectDragAutoPanAxisDelta(
            mousePos.x, viewportWidth, deltaTime, sensitivity),
        objectDragAutoPanAxisDelta(
            mousePos.y, viewportHeight, deltaTime, sensitivity),
    };
}

/// @brief 从渲染快照实例取得实际文字内容边界。
/// @param instance 组件实例快照。
/// @return 与 Vulkan 字形几何一致的边界。
Logic::CanvasComponentBounds canvasComponentContentBounds(
    const Logic::CanvasComponentInstanceSnapshot& instance)
{
    return { instance.left, instance.top, instance.right, instance.bottom };
}

/// @brief 从渲染快照实例取得其允许布局的区域。
/// @param instance 组件实例快照。
/// @return 普通组件为整张画布，拍号组件为向下扩展文字半高的拍内区间。
Logic::CanvasComponentBounds canvasComponentLayoutRegion(
    const Logic::CanvasComponentInstanceSnapshot& instance)
{
    return { instance.regionLeft,
             instance.regionTop,
             instance.regionRight,
             instance.regionBottom };
}

/// @brief 将矩形两侧、中心和上下边界追加为二维组件吸附目标。
/// @param bounds 目标对象的像素边界。
/// @param targetsX 可写纵向目标线缓存。
/// @param targetsY 可写横向目标线缓存。
/// @warning UI 布局热路径：每个显示组件或轨道调用一次，只追加固定六个值。
void appendCanvasComponentSnapTargets(
    const Logic::CanvasComponentBounds& bounds, std::vector<float>& targetsX,
    std::vector<float>& targetsY)
{
    if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) return;
    targetsX.push_back(bounds.left);
    targetsX.push_back((bounds.left + bounds.right) * 0.5f);
    targetsX.push_back(bounds.right);
    targetsY.push_back(bounds.top);
    targetsY.push_back((bounds.top + bounds.bottom) * 0.5f);
    targetsY.push_back(bounds.bottom);
}

/// @brief 将组件边界按指定像素偏移平移。
/// @param bounds 原始组件边界。
/// @param offsetX 横向偏移。
/// @param offsetY 纵向偏移。
/// @return 平移后的组件边界。
[[nodiscard]] Logic::CanvasComponentBounds offsetCanvasComponentBounds(
    const Logic::CanvasComponentBounds& bounds, float offsetX, float offsetY)
{
    return {
        bounds.left + offsetX,
        bounds.top + offsetY,
        bounds.right + offsetX,
        bounds.bottom + offsetY,
    };
}

/// @brief 将有效组件边界并入外层包围框。
/// @param bounds 待并入的组件边界。
/// @param aggregate 可写外层包围框。
/// @param hasAggregate 是否已经写入首个有效边界。
/// @warning UI 布局热路径：只进行常量级边界比较。
void mergeCanvasComponentBounds(const Logic::CanvasComponentBounds& bounds,
                                Logic::CanvasComponentBounds&       aggregate,
                                bool& hasAggregate)
{
    if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) return;
    if ( !hasAggregate ) {
        aggregate    = bounds;
        hasAggregate = true;
        return;
    }
    aggregate.left   = std::min(aggregate.left, bounds.left);
    aggregate.top    = std::min(aggregate.top, bounds.top);
    aggregate.right  = std::max(aggregate.right, bounds.right);
    aggregate.bottom = std::max(aggregate.bottom, bounds.bottom);
}

/// @brief 将全部已显示自定义组件追加为吸附目标，同步 KPS 按组外框处理。
/// @param snapshot 当前画布渲染快照。
/// @param config 自定义组件布局配置。
/// @param targetsX 可写纵向目标线缓存。
/// @param targetsY 可写横向目标线缓存。
/// @warning UI 布局热路径：整体轨道移动时每帧遍历已缓存的组件快照一次。
void appendDisplayedCanvasComponentSnapTargets(
    const Logic::RenderSnapshot&               snapshot,
    const Config::CanvasComponentLayoutConfig& config,
    std::vector<float>& targetsX, std::vector<float>& targetsY)
{
    const bool groupAllKpsPositions =
        config.kps.visible && config.syncAllKpsComponentPositions;
    const bool groupKpsTrackPositions = config.kps.visible &&
                                        !groupAllKpsPositions &&
                                        config.syncKpsTrackRelativePositions;
    Logic::CanvasComponentBounds synchronizedKpsBounds;
    bool                         hasSynchronizedKpsBounds = false;

    for ( const auto& instance : snapshot.canvasComponentInstances ) {
        if ( !config.placement(instance.type).visible ) continue;

        const bool synchronizedKpsGroupMember =
            instance.type == Config::CanvasComponentType::Kps &&
            (groupAllKpsPositions ||
             (groupKpsTrackPositions && instance.instanceIndex >= 0));
        if ( synchronizedKpsGroupMember ) {
            mergeCanvasComponentBounds(canvasComponentContentBounds(instance),
                                       synchronizedKpsBounds,
                                       hasSynchronizedKpsBounds);
            continue;
        }
        appendCanvasComponentSnapTargets(
            canvasComponentContentBounds(instance), targetsX, targetsY);
    }

    if ( hasSynchronizedKpsBounds ) {
        appendCanvasComponentSnapTargets(
            synchronizedKpsBounds, targetsX, targetsY);
    }
}

/// @brief 判断组件任一横向基准是否与指定纵向目标线对齐。
/// @param bounds 组件最终像素边界。
/// @param targetX 纵向目标线横坐标。
/// @return 左边缘、中心或右边缘与目标线重合时返回 true。
bool canvasComponentAlignsWithX(const Logic::CanvasComponentBounds& bounds,
                                float                               targetX)
{
    constexpr float epsilon = 0.25f;
    return std::abs(bounds.left - targetX) <= epsilon ||
           std::abs((bounds.left + bounds.right) * 0.5f - targetX) <= epsilon ||
           std::abs(bounds.right - targetX) <= epsilon;
}

/// @brief 判断组件任一纵向基准是否与指定横向目标线对齐。
/// @param bounds 组件最终像素边界。
/// @param targetY 横向目标线纵坐标。
/// @return 上边缘、中心或下边缘与目标线重合时返回 true。
bool canvasComponentAlignsWithY(const Logic::CanvasComponentBounds& bounds,
                                float                               targetY)
{
    constexpr float epsilon = 0.25f;
    return std::abs(bounds.top - targetY) <= epsilon ||
           std::abs((bounds.top + bounds.bottom) * 0.5f - targetY) <= epsilon ||
           std::abs(bounds.bottom - targetY) <= epsilon;
}

/// @brief 绘制一条半透明虚线吸附参考线。
/// @param drawList 目标 ImGui 前景绘制列表。
/// @param start 参考线起点。
/// @param end 参考线终点。
/// @param color 半透明线条颜色。
/// @param thickness 线宽。
/// @param dashLength 单段虚线长度。
/// @param gapLength 相邻虚线间距。
/// @warning UI 布局热路径：仅吸附生效时调用，按画布单轴长度生成短线段。
void drawCanvasComponentSnapGuide(ImDrawList& drawList, const ImVec2& start,
                                  const ImVec2& end, ImU32 color,
                                  float thickness, float dashLength,
                                  float gapLength)
{
    const float deltaX = end.x - start.x;
    const float deltaY = end.y - start.y;
    const float length = std::sqrt(deltaX * deltaX + deltaY * deltaY);
    if ( length <= 0.0f ) return;

    dashLength       = std::max(1.0f, dashLength);
    gapLength        = std::max(1.0f, gapLength);
    const float dx   = deltaX / length;
    const float dy   = deltaY / length;
    const float step = dashLength + gapLength;
    for ( float distance = 0.0f; distance < length; distance += step ) {
        const float segmentEnd = std::min(distance + dashLength, length);
        drawList.AddLine(
            { start.x + dx * distance, start.y + dy * distance },
            { start.x + dx * segmentEnd, start.y + dy * segmentEnd },
            color,
            thickness);
    }
}

/// @brief 暗化组件实际可调区域以外的画布并标出区域边界。
/// @param drawList 目标 ImGui 前景绘制列表。
/// @param region 组件实例在画布局部坐标中的实际可调区域。
/// @param canvasScreenX 画布左上角屏幕横坐标。
/// @param canvasScreenY 画布左上角屏幕纵坐标。
/// @param canvasWidth 画布宽度。
/// @param canvasHeight 画布高度。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 布局热路径：悬停或拖动受限组件时每帧调用，只生成固定四块遮罩
/// 和一个边框。
void drawCanvasComponentEditableRegionMask(
    ImDrawList& drawList, const Logic::CanvasComponentBounds& region,
    float canvasScreenX, float canvasScreenY, float canvasWidth,
    float canvasHeight, float dpiScale)
{
    const float left =
        std::clamp(std::min(region.left, region.right), 0.0f, canvasWidth);
    const float top =
        std::clamp(std::min(region.top, region.bottom), 0.0f, canvasHeight);
    const float right =
        std::clamp(std::max(region.left, region.right), 0.0f, canvasWidth);
    const float bottom =
        std::clamp(std::max(region.top, region.bottom), 0.0f, canvasHeight);
    if ( right <= left || bottom <= top ) return;

    constexpr float edgeEpsilon  = 0.5f;
    const bool      coversCanvas = left <= edgeEpsilon && top <= edgeEpsilon &&
                                   right >= canvasWidth - edgeEpsilon &&
                                   bottom >= canvasHeight - edgeEpsilon;
    if ( coversCanvas ) return;

    const ImVec2    canvasMin{ canvasScreenX, canvasScreenY };
    const ImVec2    canvasMax{ canvasScreenX + canvasWidth,
                               canvasScreenY + canvasHeight };
    const ImVec2    allowedMin{ canvasScreenX + left, canvasScreenY + top };
    const ImVec2    allowedMax{ canvasScreenX + right, canvasScreenY + bottom };
    constexpr ImU32 maskColor = IM_COL32(0, 0, 0, 118);
    if ( allowedMin.y > canvasMin.y ) {
        drawList.AddRectFilled(
            canvasMin, { canvasMax.x, allowedMin.y }, maskColor);
    }
    if ( allowedMax.y < canvasMax.y ) {
        drawList.AddRectFilled(
            { canvasMin.x, allowedMax.y }, canvasMax, maskColor);
    }
    if ( allowedMin.x > canvasMin.x ) {
        drawList.AddRectFilled({ canvasMin.x, allowedMin.y },
                               { allowedMin.x, allowedMax.y },
                               maskColor);
    }
    if ( allowedMax.x < canvasMax.x ) {
        drawList.AddRectFilled({ allowedMax.x, allowedMin.y },
                               { canvasMax.x, allowedMax.y },
                               maskColor);
    }

    drawList.AddRect(allowedMin,
                     allowedMax,
                     IM_COL32(255, 218, 96, 210),
                     0.0f,
                     0,
                     std::max(1.5f, 2.0f * dpiScale));
}

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
    ImGui::SetNextWindowBgAlpha(CANVAS_HOVER_OVERLAY_BACKGROUND_ALPHA);

    constexpr ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoDocking;
    return ImGui::Begin("##CanvasHoverInspectOverlay", nullptr, overlayFlags);
}

/// @brief 读取批注 UI 皮肤颜色并在缺失时回退。
/// @param key 颜色键。
/// @param fallback 缺失颜色。
/// @return 可直接交给 ImDrawList 的颜色。
ImU32 annotationUiColor(std::string_view key, const ImVec4& fallback)
{
    const auto color =
        Config::SkinManager::instance().getColor(std::string(key));
    const bool missing = color.r == 1.0F && color.g == 0.0F &&
                         color.b == 1.0F && color.a == 1.0F;
    return ImGui::ColorConvertFloat4ToU32(
        missing ? fallback : ImVec4(color.r, color.g, color.b, color.a));
}

/// @brief 批注详情卡片单帧绘制数量上限。
constexpr std::size_t MAX_VISIBLE_ANNOTATION_DETAIL_CARDS = 48U;

/// @brief 单张详情卡片对应的只读批注数据。
struct AnnotationDetailCardEntry {
    /// @brief 卡片所在的时间戳分组。
    const Logic::AnnotationRenderMarker* marker{ nullptr };
    /// @brief 卡片展示的具体批注。
    const Logic::AnnotationRenderItem* item{ nullptr };
    /// @brief 批注在时间戳分组内的索引。
    std::size_t itemIndex{ 0U };
    /// @brief Markdown 正文完整排版高度。
    float contentHeight{ 0.0F };
};

/// @brief 批注详情卡片悬浮命中结果。
struct AnnotationDetailCardHit {
    /// @brief 被命中的时间戳分组。
    const Logic::AnnotationRenderMarker* marker{ nullptr };
    /// @brief 被命中的分组内批注索引。
    std::size_t itemIndex{ 0U };
    /// @brief 指针是否命中了卡片正文中的链接。
    bool linkHovered{ false };
    /// @brief 卡片是否消费了本帧滚轮输入。
    bool wheelConsumed{ false };
};

/// @brief 获取批注目标类型的翻译键。
const char* annotationTargetLabelKey(
    ::MMM::BeatmapAnnotationTargetKind targetKind)
{
    if ( targetKind == ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT ) {
        return "ui.annotation.target.player_object";
    }
    if ( targetKind == ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE ) {
        return "ui.annotation.target.audio_sample";
    }
    return "ui.annotation.target.timestamp";
}

/// @brief 计算一条物件批注连线在轨道区中的起点横坐标。
/// @param item 批注展示数据。
/// @param projection 当前画布横向投影。
/// @param fallbackX 无有效物件轨道时使用的批注栏中心。
/// @return 对应玩家物件、自动采样或批注栏中心的横坐标。
/// @warning UI 热路径：每张可见详情卡片调用一次，只执行常量级投影查询。
float annotationConnectorSourceX(const Logic::AnnotationRenderItem& item,
                                 const Logic::CanvasLaneProjection& projection,
                                 float                              fallbackX)
{
    if ( item.track < 0 ) return fallbackX;

    std::optional<Logic::CanvasLaneBounds> bounds;
    const auto track = static_cast<std::uint32_t>(item.track);
    if ( item.targetKind ==
         ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT ) {
        bounds = projection.bounds({ Logic::CanvasLaneKind::Player, track });
    } else if ( item.targetKind ==
                ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE ) {
        if ( track < projection.playerLaneCount ) {
            bounds =
                projection.bounds({ Logic::CanvasLaneKind::Player, track });
        } else {
            bounds = projection.bounds({ Logic::CanvasLaneKind::Bgm,
                                         track - projection.playerLaneCount });
        }
    }
    return bounds ? (bounds->leftX + bounds->rightX) * 0.5F : fallbackX;
}

/// @brief 绘制当前可见批注的避让卡片与物件连线。
/// @param markers 当前主画布已裁剪的批注时间戳分组。
/// @param projection 当前画布横向投影。
/// @param canvasScreenX 画布左上角屏幕横坐标。
/// @param canvasScreenY 画布左上角屏幕纵坐标。
/// @param targetWidth 画布宽度。
/// @param topY 轨道区顶部局部坐标。
/// @param bottomY 轨道区底部局部坐标。
/// @param pointerX 指针相对画布左侧的横坐标。
/// @param pointerY 指针相对画布顶部的纵坐标。
/// @param canvasHovered 指针是否位于画布窗口。
/// @param scrollItemId 当前保留滚动状态的批注 ID。
/// @param scrollY 当前批注卡片正文的纵向滚动偏移。
/// @return 指针命中的详情卡片及其分组内索引。
/// @warning UI 热路径：只线性遍历当前快照最多 48 条可见批注，不排序、不访问
/// ECS 或文件系统。
AnnotationDetailCardHit renderConnectedAnnotationDetails(
    const std::vector<Logic::AnnotationRenderMarker>& markers,
    const Logic::CanvasLaneProjection& projection, float canvasScreenX,
    float canvasScreenY, float targetWidth, float topY, float bottomY,
    float pointerX, float pointerY, bool canvasHovered,
    std::string& scrollItemId, float& scrollY)
{
    AnnotationDetailCardHit result;
    if ( markers.empty() || bottomY <= topY ) return result;

    constexpr float CARD_MARGIN     = 10.0F;
    constexpr float CARD_MIN_WIDTH  = 170.0F;
    constexpr float CARD_MAX_WIDTH  = 340.0F;
    constexpr float CARD_GAP        = 5.0F;
    constexpr float CARD_PADDING    = CANVAS_HOVER_OVERLAY_PADDING;
    constexpr float SCROLLBAR_SPACE = 7.0F;
    constexpr float CONNECTOR_ELBOW = 9.0F;
    const float     rightAvailable =
        targetWidth - projection.annotationRightX - CARD_MARGIN * 2.0F;
    const float leftAvailable = projection.annotationLeftX - CARD_MARGIN * 2.0F;
    const bool  placeRight =
        rightAvailable >= CARD_MIN_WIDTH || rightAvailable >= leftAvailable;
    const float available = placeRight ? rightAvailable : leftAvailable;
    if ( available < CARD_MIN_WIDTH ) return result;

    const float cardWidth = std::min(CARD_MAX_WIDTH, available);
    const float cardLeftX =
        placeRight ? projection.annotationRightX + CARD_MARGIN
                   : projection.annotationLeftX - CARD_MARGIN - cardWidth;
    const float cardRightX = cardLeftX + cardWidth;
    const float markerCenterX =
        (projection.annotationLeftX + projection.annotationRightX) * 0.5F;
    const float fontSize = ImGui::GetFontSize();
    const float contentWidth =
        std::max(1.0F, cardWidth - CARD_PADDING * 2.0F - SCROLLBAR_SPACE);

    const ImU32 connectorColor = annotationUiColor(
        "annotations.connector", ImVec4(0.42F, 0.72F, 0.96F, 0.86F));
    const auto& imguiStyle      = ImGui::GetStyle();
    ImVec4      background      = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    background.w                = CANVAS_HOVER_OVERLAY_BACKGROUND_ALPHA;
    const ImU32 backgroundColor = ImGui::ColorConvertFloat4ToU32(background);
    const ImU32 borderColor     = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 headerColor     = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 mutedColor      = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 scrollbarBackgroundColor =
        ImGui::GetColorU32(ImGuiCol_ScrollbarBg);
    const ImU32 scrollbarColor = ImGui::GetColorU32(ImGuiCol_ScrollbarGrab);
    const ImU32 scrollbarHoverColor =
        ImGui::GetColorU32(ImGuiCol_ScrollbarGrabHovered);
    const UI::MarkdownStyle         markdownStyle = UI::defaultMarkdownStyle();
    const UI::MarkdownRenderOptions markdownOptions{
        .wrapWidth        = contentWidth,
        .compact          = true,
        .interactiveLinks = true,
        .style            = &markdownStyle,
    };

    std::array<AnnotationDetailCardEntry, MAX_VISIBLE_ANNOTATION_DETAIL_CARDS>
        entries{};
    std::array<AnnotationDetailCardPlacement,
               MAX_VISIBLE_ANNOTATION_DETAIL_CARDS>
                placements{};
    std::size_t cardCount = 0U;

    auto appendMarker = [&](const Logic::AnnotationRenderMarker& marker) {
        if ( marker.canvasY < topY - 10.0F ||
             marker.canvasY > bottomY + 10.0F ) {
            return;
        }
        for ( std::size_t index = 0U; index < marker.items.size(); ++index ) {
            if ( cardCount >= entries.size() ) return;
            const auto& item = marker.items[index];
            const auto  contentLayout =
                UI::measureMarkdown(item.content, markdownOptions);
            const float visibleContentHeight =
                std::clamp(contentLayout.size.y, fontSize, fontSize * 5.0F);
            entries[cardCount] = {
                &marker,
                &item,
                index,
                std::max(fontSize, contentLayout.size.y),
            };
            placements[cardCount] = {
                marker.canvasY,
                CARD_PADDING * 2.0F +
                    CANVAS_HOVER_OVERLAY_ITEM_SPACING_Y * 2.0F +
                    fontSize * 2.0F + visibleContentHeight,
                0.0F,
            };
            ++cardCount;
        }
    };

    const bool ascending = markers.size() < 2U ||
                           markers.front().canvasY <= markers.back().canvasY;
    if ( ascending ) {
        for ( const auto& marker : markers ) appendMarker(marker);
    } else {
        for ( auto marker = markers.rbegin(); marker != markers.rend();
              ++marker ) {
            appendMarker(*marker);
        }
    }
    if ( cardCount == 0U ) return result;

    layoutAnnotationDetailCards(
        std::span<AnnotationDetailCardPlacement>(placements.data(), cardCount),
        topY + 4.0F,
        bottomY - 4.0F,
        CARD_GAP);

    auto* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(
        { canvasScreenX, canvasScreenY + topY },
        { canvasScreenX + targetWidth, canvasScreenY + bottomY },
        true);
    for ( std::size_t index = 0U; index < cardCount; ++index ) {
        const auto& entry     = entries[index];
        const auto& placement = placements[index];
        if ( !entry.marker || !entry.item ) continue;

        const float cardTopY    = placement.topY;
        const float cardBottomY = cardTopY + placement.height;
        const bool  hovered = canvasHovered && pointerX >= cardLeftX &&
                              pointerX <= cardRightX && pointerY >= cardTopY &&
                              pointerY <= cardBottomY;
        if ( hovered && !result.marker ) {
            result.marker    = entry.marker;
            result.itemIndex = entry.itemIndex;
        }

        const float sourceX =
            annotationConnectorSourceX(*entry.item, projection, markerCenterX);
        const float cardEdgeX   = placeRight ? cardLeftX : cardRightX;
        const float elbowX      = placeRight ? cardEdgeX - CONNECTOR_ELBOW
                                             : cardEdgeX + CONNECTOR_ELBOW;
        const float cardCenterY = (cardTopY + cardBottomY) * 0.5F;
        const std::array<ImVec2, 4> connector{
            ImVec2{ canvasScreenX + sourceX,
                    canvasScreenY + entry.marker->canvasY },
            ImVec2{ canvasScreenX + markerCenterX,
                    canvasScreenY + entry.marker->canvasY },
            ImVec2{ canvasScreenX + elbowX, canvasScreenY + cardCenterY },
            ImVec2{ canvasScreenX + cardEdgeX, canvasScreenY + cardCenterY },
        };
        drawList->AddPolyline(connector.data(),
                              static_cast<int>(connector.size()),
                              connectorColor,
                              ImDrawFlags_None,
                              hovered ? 2.5F : 1.5F);
        drawList->AddCircleFilled(
            connector.front(), hovered ? 4.0F : 3.0F, connectorColor);

        const ImVec2 cardMin{ canvasScreenX + cardLeftX,
                              canvasScreenY + cardTopY };
        const ImVec2 cardMax{ canvasScreenX + cardRightX,
                              canvasScreenY + cardBottomY };
        drawList->AddRectFilled(
            cardMin, cardMax, backgroundColor, imguiStyle.WindowRounding);
        if ( imguiStyle.WindowBorderSize > 0.0F ) {
            drawList->AddRect(cardMin,
                              cardMax,
                              borderColor,
                              imguiStyle.WindowRounding,
                              ImDrawFlags_None,
                              imguiStyle.WindowBorderSize);
        }

        const std::string_view author =
            entry.item->author.empty()
                ? TR("ui.annotation.unknown_author").view()
                : std::string_view(entry.item->author);
        drawList->AddText(
            { cardMin.x + CARD_PADDING, cardMin.y + CARD_PADDING },
            headerColor,
            author.data(),
            author.data() + author.size());

        char       metadata[512]{};
        const auto target =
            TR(annotationTargetLabelKey(entry.item->targetKind));
        if ( entry.item->track >= 0 ) {
            std::snprintf(metadata,
                          sizeof(metadata),
                          "%.3f s · %s #%d%s",
                          entry.marker->timestamp,
                          target.data(),
                          entry.item->track + 1,
                          entry.item->targetMissing ? " !" : "");
        } else {
            std::snprintf(metadata,
                          sizeof(metadata),
                          "%.3f s · %s%s",
                          entry.marker->timestamp,
                          target.data(),
                          entry.item->targetMissing ? " !" : "");
        }
        const ImVec2 metadataPos{ cardMin.x + CARD_PADDING,
                                  cardMin.y + CARD_PADDING + fontSize +
                                      CANVAS_HOVER_OVERLAY_ITEM_SPACING_Y };
        drawList->AddText(metadataPos, mutedColor, metadata);

        const ImVec2 contentMin{ cardMin.x + CARD_PADDING,
                                 metadataPos.y + fontSize +
                                     CANVAS_HOVER_OVERLAY_ITEM_SPACING_Y };
        const ImVec2 contentMax{ cardMax.x - CARD_PADDING - SCROLLBAR_SPACE,
                                 cardMax.y - CARD_PADDING };
        const float  visibleContentHeight =
            std::max(1.0F, contentMax.y - contentMin.y);
        const float maxScrollY =
            std::max(0.0F, entry.contentHeight - visibleContentHeight);
        float cardScrollY = entry.item->id == scrollItemId
                                ? std::clamp(scrollY, 0.0F, maxScrollY)
                                : 0.0F;
        if ( hovered ) {
            if ( scrollItemId != entry.item->id ) {
                scrollItemId = entry.item->id;
                scrollY      = 0.0F;
            }
            const auto wheelResult =
                updateAnnotationDetailWheel(ImGui::GetIO().MouseWheel,
                                            scrollY,
                                            maxScrollY,
                                            fontSize * 3.0F);
            scrollY              = wheelResult.scrollY;
            cardScrollY          = scrollY;
            result.wheelConsumed = wheelResult.consumed;
        }

        auto renderOptions           = markdownOptions;
        renderOptions.maxHeight      = visibleContentHeight;
        renderOptions.verticalOffset = cardScrollY;
        const auto markdownResult =
            UI::renderMarkdownToDrawList(*drawList,
                                         contentMin,
                                         contentMax,
                                         entry.item->content,
                                         renderOptions);
        if ( hovered && markdownResult.linkHovered ) result.linkHovered = true;

        if ( maxScrollY > 0.01F ) {
            constexpr float SCROLLBAR_WIDTH = 3.0F;
            const float     trackTop        = contentMin.y;
            const float     trackBottom     = contentMax.y;
            const float trackHeight = std::max(1.0F, trackBottom - trackTop);
            const float thumbHeight = std::clamp(
                trackHeight * visibleContentHeight / entry.contentHeight,
                12.0F,
                trackHeight);
            const float thumbTravel = std::max(0.0F, trackHeight - thumbHeight);
            const float thumbTop =
                trackTop + thumbTravel * (cardScrollY / maxScrollY);
            const float scrollbarX = cardMax.x - CARD_PADDING * 0.5F;
            drawList->AddRectFilled({ scrollbarX - SCROLLBAR_WIDTH, trackTop },
                                    { scrollbarX, trackBottom },
                                    scrollbarBackgroundColor,
                                    2.0F);
            drawList->AddRectFilled(
                { scrollbarX - SCROLLBAR_WIDTH, thumbTop },
                { scrollbarX, thumbTop + thumbHeight },
                hovered ? scrollbarHoverColor : scrollbarColor,
                2.0F);
        }
    }
    drawList->PopClipRect();
    return result;
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

/// @brief 在移动工具下绘制悬浮物件的项目音频试听按钮。
bool Basic2DCanvasInteraction::renderObjectAudioPreviewControls(
    const Logic::RenderSnapshot& currentSnapshot, float canvasScreenX,
    float canvasScreenY, float targetWidth, float targetHeight, float pointerX,
    float pointerY)
{
    const auto& inspect = currentSnapshot.hoverInspect;
    auto*       project = Logic::EditorEngine::instance().getCurrentProject();
    if ( currentSnapshot.currentTool != Logic::EditTool::Move ||
         m_leftPressStartedOnCanvas || m_leftPressStartedOnEntity ||
         m_rightEraseActive || !project || targetWidth <= 0.0F ||
         targetHeight <= 0.0F ) {
        m_audioPreviewOverlay = {};
        return false;
    }

    const auto& style            = ImGui::GetStyle();
    const float retentionPadding = std::max(6.0F, style.ItemSpacing.x * 0.5F);
    const bool  pointerInsideLockedRetention =
        m_audioPreviewOverlay.valid &&
        (m_audioPreviewOverlay.volumeEditorOpen ||
         (pointerX >= std::min(m_audioPreviewOverlay.left,
                               m_audioPreviewOverlay.controlsLeft) -
                          retentionPadding &&
          pointerX <= std::max(m_audioPreviewOverlay.right,
                               m_audioPreviewOverlay.controlsRight) +
                          retentionPadding &&
          pointerY >= std::min(m_audioPreviewOverlay.top,
                               m_audioPreviewOverlay.controlsTop) -
                          retentionPadding &&
          pointerY <= std::max(m_audioPreviewOverlay.bottom,
                               m_audioPreviewOverlay.controlsBottom) +
                          retentionPadding));

    entt::entity           targetEntity{ entt::null };
    Logic::ChartObjectKind targetObjectKind{
        Logic::ChartObjectKind::PlayerNote
    };
    std::string_view targetAudioResourceId;
    float            targetVolume{ 1.0F };
    std::int32_t     targetSampleBindingSubIndex{ -1 };
    if ( pointerInsideLockedRetention ) {
        targetEntity          = m_audioPreviewOverlay.entity;
        targetObjectKind      = m_audioPreviewOverlay.objectKind;
        targetAudioResourceId = m_audioPreviewOverlay.audioResourceId;
        targetVolume          = m_audioPreviewOverlay.volume;
        targetSampleBindingSubIndex =
            m_audioPreviewOverlay.sampleBindingSubIndex;
    } else if ( inspect.show && inspect.showAudioPreview &&
                inspect.entity != entt::null &&
                !inspect.audioResourceId.empty() ) {
        targetEntity                = inspect.entity;
        targetObjectKind            = inspect.objectKind;
        targetAudioResourceId       = inspect.audioResourceId;
        targetVolume                = inspect.volume;
        targetSampleBindingSubIndex = inspect.sampleBindingSubIndex;
    } else {
        m_audioPreviewOverlay = {};
        return false;
    }

    const bool sameObject =
        m_audioPreviewOverlay.valid &&
        m_audioPreviewOverlay.entity == targetEntity &&
        m_audioPreviewOverlay.objectKind == targetObjectKind &&
        m_audioPreviewOverlay.audioResourceId == targetAudioResourceId &&
        m_audioPreviewOverlay.sampleBindingSubIndex ==
            targetSampleBindingSubIndex;
    const float previousCenterX =
        (m_audioPreviewOverlay.left + m_audioPreviewOverlay.right) * 0.5F;
    const float previousCenterY =
        (m_audioPreviewOverlay.top + m_audioPreviewOverlay.bottom) * 0.5F;

    const Logic::Hitbox* anchor    = nullptr;
    float                bestScore = std::numeric_limits<float>::max();
    for ( const auto& hitbox : currentSnapshot.hitboxes ) {
        if ( hitbox.entity != targetEntity || hitbox.kind != targetObjectKind ||
             hitbox.w <= 0.0F || hitbox.h <= 0.0F ) {
            continue;
        }

        const bool pointerInside =
            pointerX >= hitbox.x && pointerX <= hitbox.x + hitbox.w &&
            pointerY >= hitbox.y && pointerY <= hitbox.y + hitbox.h;
        const float centerX = hitbox.x + hitbox.w * 0.5F;
        const float centerY = hitbox.y + hitbox.h * 0.5F;
        const float targetX = sameObject ? previousCenterX : pointerX;
        const float targetY = sameObject ? previousCenterY : pointerY;
        const float deltaX  = centerX - targetX;
        const float deltaY  = centerY - targetY;
        const float score =
            pointerInside ? -1.0F : deltaX * deltaX + deltaY * deltaY;
        if ( score < bestScore ) {
            bestScore = score;
            anchor    = &hitbox;
        }
    }
    if ( !anchor ) {
        m_audioPreviewOverlay = {};
        return false;
    }

    m_audioPreviewOverlay.valid      = true;
    m_audioPreviewOverlay.entity     = targetEntity;
    m_audioPreviewOverlay.objectKind = targetObjectKind;
    if ( !pointerInsideLockedRetention ) {
        m_audioPreviewOverlay.audioResourceId = targetAudioResourceId;
        m_audioPreviewOverlay.volume          = targetVolume;
        m_audioPreviewOverlay.sampleBindingSubIndex =
            targetSampleBindingSubIndex;
    }
    if ( !sameObject ) {
        const std::string previewInstanceId =
            "canvas/" + m_cameraId + "/" +
            std::to_string(static_cast<std::uint32_t>(targetObjectKind)) + "/" +
            std::to_string(
                static_cast<std::uint32_t>(entt::to_integral(targetEntity))) +
            "/" + std::to_string(targetSampleBindingSubIndex);
        m_audioPreviewOverlay.previewPoolKey =
            UI::makeProjectAudioPreviewPoolKey(previewInstanceId);
    }
    m_audioPreviewOverlay.left   = anchor->x;
    m_audioPreviewOverlay.top    = anchor->y;
    m_audioPreviewOverlay.right  = anchor->x + anchor->w;
    m_audioPreviewOverlay.bottom = anchor->y + anchor->h;

    const float buttonSize =
        std::ceil(std::max(20.0F, ImGui::GetFrameHeight()));
    const float spacing =
        std::max(2.0F, std::min(style.ItemInnerSpacing.x, 5.0F));
    const float progressHeight = std::clamp(buttonSize * 0.16F, 4.0F, 7.0F);
    const float progressSpacing =
        std::max(2.0F, std::min(style.ItemInnerSpacing.y, 4.0F));
    const float rowWidth    = buttonSize * 4.0F + spacing * 3.0F;
    const float panelHeight = progressHeight + progressSpacing + buttonSize;
    const float gap         = std::max(6.0F, style.ItemSpacing.y * 0.5F);

    float controlsX =
        (m_audioPreviewOverlay.left + m_audioPreviewOverlay.right - rowWidth) *
        0.5F;
    controlsX =
        std::clamp(controlsX, 0.0F, std::max(0.0F, targetWidth - rowWidth));
    float controlsY = m_audioPreviewOverlay.top - gap - panelHeight;
    controlsY =
        std::clamp(controlsY, 0.0F, std::max(0.0F, targetHeight - panelHeight));

    m_audioPreviewOverlay.controlsLeft   = controlsX;
    m_audioPreviewOverlay.controlsTop    = controlsY;
    m_audioPreviewOverlay.controlsRight  = controlsX + rowWidth;
    m_audioPreviewOverlay.controlsBottom = controlsY + panelHeight;

    const auto result = UI::renderProjectAudioPreviewControls(
        "CanvasObjectAudioPreview",
        *project,
        m_audioPreviewOverlay.audioResourceId,
        m_audioPreviewOverlay.previewPoolKey,
        m_audioPreviewOverlay.volume,
        &m_audioPreviewOverlay.volume,
        UI::ProjectAudioPreviewControlsLayout{
            .topLeft = { canvasScreenX + controlsX, canvasScreenY + controlsY },
            .width   = rowWidth,
            .buttonSize      = buttonSize,
            .buttonSpacing   = spacing,
            .progressHeight  = progressHeight,
            .progressSpacing = progressSpacing,
        });
    m_audioPreviewOverlay.volumeEditorOpen = result.volumeEditorOpen;
    if ( result.volumeChanged ) {
        Logic::EditorEngine::instance().pushCommand(
            Logic::LogicCommand(Logic::CmdUpdateObjectSampleVolume{
                .entity   = m_audioPreviewOverlay.entity,
                .kind     = m_audioPreviewOverlay.objectKind,
                .subIndex = m_audioPreviewOverlay.sampleBindingSubIndex,
                .volume   = m_audioPreviewOverlay.volume,
            }));
    }

    const bool  pointerInsideObject = pointerX >= m_audioPreviewOverlay.left &&
                                      pointerX <= m_audioPreviewOverlay.right &&
                                      pointerY >= m_audioPreviewOverlay.top &&
                                      pointerY <= m_audioPreviewOverlay.bottom;
    const float bridgePadding       = std::max(retentionPadding, gap);
    const float bridgeLeft =
        std::min(m_audioPreviewOverlay.left, controlsX) - bridgePadding;
    const float bridgeTop =
        std::min(m_audioPreviewOverlay.top, controlsY) - bridgePadding;
    const float bridgeRight =
        std::max(m_audioPreviewOverlay.right, controlsX + rowWidth) +
        bridgePadding;
    const float bridgeBottom =
        std::max(m_audioPreviewOverlay.bottom, controlsY + panelHeight) +
        bridgePadding;
    const bool pointerInsideBridge =
        pointerX >= bridgeLeft && pointerX <= bridgeRight &&
        pointerY >= bridgeTop && pointerY <= bridgeBottom;
    return result.hovered || result.volumeEditorOpen ||
           (pointerInsideBridge && !pointerInsideObject);
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
    const bool shouldPlayAdjustmentFeedback =
        engine.getEditorConfig().settings.stopPlaybackOnScroll;
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
            if ( shouldPlayAdjustmentFeedback ) {
                ::MMM::UI::PlayInteractionMouseUpFeedback();
            }
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
                if ( shouldPlayAdjustmentFeedback ) {
                    ::MMM::UI::PlayInteractionMouseUpFeedback();
                }
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
            if ( shouldPlayAdjustmentFeedback ) {
                ::MMM::UI::PlayInteractionMouseUpFeedback();
            }
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
    if ( Logic::EditorEngine::instance().getCurrentTool() ==
         Logic::EditTool::Layout ) {
        return;
    }

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

void Basic2DCanvasInteraction::rebuildNoteLayoutInstances(
    const Logic::RenderSnapshot& currentSnapshot)
{
    m_noteLayoutInstances.clear();
    m_noteLayoutIndexScratch.clear();
    m_noteLayoutInstances.reserve(currentSnapshot.hitboxes.size());
    m_noteLayoutIndexScratch.reserve(currentSnapshot.hitboxes.size());

    for ( const auto& hitbox : currentSnapshot.hitboxes ) {
        if ( hitbox.kind != Logic::ChartObjectKind::PlayerNote ||
             hitbox.entity == entt::null || !std::isfinite(hitbox.x) ||
             !std::isfinite(hitbox.y) || !std::isfinite(hitbox.w) ||
             !std::isfinite(hitbox.h) || hitbox.w <= 0.0f ||
             hitbox.h <= 0.0f ) {
            continue;
        }

        const Logic::CanvasComponentBounds bounds{
            hitbox.x, hitbox.y, hitbox.x + hitbox.w, hitbox.y + hitbox.h
        };
        const auto [it, inserted] = m_noteLayoutIndexScratch.try_emplace(
            hitbox.entity, m_noteLayoutInstances.size());
        if ( inserted ) {
            m_noteLayoutInstances.push_back({ hitbox.entity, bounds });
            continue;
        }

        bool hasBounds = true;
        mergeCanvasComponentBounds(
            bounds, m_noteLayoutInstances[it->second].bounds, hasBounds);
    }
}

void Basic2DCanvasInteraction::finishLayoutEditing()
{
    if ( m_layoutConfigurationChanged ) {
        Config::AppConfig::instance().save();
        ::MMM::UI::PlayInteractionMouseUpFeedback();
    }
    m_trackLayoutDragHandle = TrackLayoutDragHandle::None;
    m_noteScaleDragTarget.reset();
    m_noteScaleDragHandle = Logic::CanvasComponentDragHandle::None;
    m_canvasComponentDragTarget.reset();
    m_canvasComponentDragHandle        = Logic::CanvasComponentDragHandle::None;
    m_canvasComponentDragInstanceIndex = 0;
    m_canvasComponentSnapGuideX.reset();
    m_canvasComponentSnapGuideY.reset();
    m_canvasComponentSnapTargetsX.clear();
    m_canvasComponentSnapTargetsY.clear();
    m_layoutConfigurationChanged = false;
    m_synchronizedKpsTransformStarts.clear();
}

void Basic2DCanvasInteraction::handleLayoutEditing(
    float pointerX, float pointerY, float canvasScreenX, float canvasScreenY,
    float targetWidth, float targetHeight, bool isHovered,
    const Logic::RenderSnapshot& currentSnapshot)
{
    if ( targetWidth <= 0.0f || targetHeight <= 0.0f ) {
        if ( (m_trackLayoutDragHandle != TrackLayoutDragHandle::None ||
              m_noteScaleDragTarget.has_value() ||
              m_canvasComponentDragTarget.has_value()) &&
             !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            finishLayoutEditing();
        }
        return;
    }

    rebuildNoteLayoutInstances(currentSnapshot);

    auto&      appConfig = Config::AppConfig::instance();
    const auto keyCount  = currentSnapshot.hasBeatmap
                               ? std::max(currentSnapshot.trackCount, 1)
                               : 0;
    auto       layout    = sanitizeTrackLayout(
        appConfig.getVisualConfig().trackLayoutForKeyCount(keyCount));
    const float cameraOffsetX        = currentSnapshot.canvasHorizontalOffsetX;
    const float worldPointerX        = pointerX - cameraOffsetX;
    float       judgmentLinePosition = sanitizeJudgmentLinePosition(
        appConfig.getVisualConfig().judgmentLinePositionForKeyCount(keyCount));
    const float dpiScale                 = appConfig.getWindowContentScale();
    const float edgeHitRadius            = std::max(6.0f, 7.0f * dpiScale);
    const float moveHandleRadius         = std::max(12.0f, 15.0f * dpiScale);
    const float componentCornerHitRadius = std::max(6.0f, 7.0f * dpiScale);
    const float componentSnapDistance =
        std::max(CANVAS_COMPONENT_SNAP_DISTANCE,
                 CANVAS_COMPONENT_SNAP_DISTANCE * dpiScale);
    const bool movingCanvasComponent =
        m_canvasComponentDragTarget.has_value() &&
        m_canvasComponentDragHandle == Logic::CanvasComponentDragHandle::Move;
    const bool movingTrackLayout =
        !m_noteScaleDragTarget.has_value() &&
        !m_canvasComponentDragTarget.has_value() &&
        m_trackLayoutDragHandle == TrackLayoutDragHandle::Move;
    if ( (!movingCanvasComponent && !movingTrackLayout) ||
         !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        m_canvasComponentSnapGuideX.reset();
        m_canvasComponentSnapGuideY.reset();
    }

    std::optional<entt::entity>                 hoveredNote;
    std::optional<Logic::CanvasComponentBounds> hoveredNoteBounds;
    Logic::CanvasComponentDragHandle            hoveredNoteHandle =
        Logic::CanvasComponentDragHandle::None;
    if ( m_noteScaleDragTarget.has_value() ) {
        hoveredNote       = m_noteScaleDragTarget;
        hoveredNoteHandle = m_noteScaleDragHandle;
    } else if ( !m_canvasComponentDragTarget.has_value() && isHovered ) {
        for ( auto it = m_noteLayoutInstances.rbegin();
              it != m_noteLayoutInstances.rend();
              ++it ) {
            const auto hit = Logic::hitTestCanvasComponent(
                it->bounds, pointerX, pointerY, componentCornerHitRadius);
            if ( hit == Logic::CanvasComponentDragHandle::None ||
                 hit == Logic::CanvasComponentDragHandle::Move ) {
                continue;
            }
            hoveredNote       = it->entity;
            hoveredNoteBounds = it->bounds;
            hoveredNoteHandle = hit;
            break;
        }
    }

    std::optional<Config::CanvasComponentType> hoveredComponent;
    std::optional<Logic::CanvasComponentInstanceSnapshot>
                                     hoveredComponentInstance;
    Logic::CanvasComponentDragHandle hoveredComponentHandle =
        Logic::CanvasComponentDragHandle::None;
    if ( m_canvasComponentDragTarget.has_value() ) {
        hoveredComponent       = m_canvasComponentDragTarget;
        hoveredComponentHandle = m_canvasComponentDragHandle;
    } else if ( !hoveredNote.has_value() && isHovered ) {
        for ( auto it = currentSnapshot.canvasComponentInstances.rbegin();
              it != currentSnapshot.canvasComponentInstances.rend();
              ++it ) {
            const auto& placement = appConfig.getVisualConfig()
                                        .canvasComponentsForKeyCount(keyCount)
                                        .placement(it->type);
            if ( !placement.visible ) continue;

            const auto bounds = canvasComponentContentBounds(*it);
            if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) {
                continue;
            }
            const auto hit = Logic::hitTestCanvasComponent(
                bounds, pointerX, pointerY, componentCornerHitRadius);
            if ( hit != Logic::CanvasComponentDragHandle::None ) {
                hoveredComponent         = it->type;
                hoveredComponentInstance = *it;
                hoveredComponentHandle   = hit;
                break;
            }
        }
    }

    TrackLayoutDragHandle hoveredHandle = TrackLayoutDragHandle::None;
    if ( m_trackLayoutDragHandle != TrackLayoutDragHandle::None ) {
        hoveredHandle = m_trackLayoutDragHandle;
    } else if ( isHovered && !hoveredNote.has_value() &&
                !hoveredComponent.has_value() ) {
        hoveredHandle = hitTestTrackLayout(layout,
                                           judgmentLinePosition,
                                           worldPointerX,
                                           pointerY,
                                           targetWidth,
                                           targetHeight,
                                           edgeHitRadius,
                                           moveHandleRadius);
    }

    const Logic::CanvasComponentDragHandle hoveredResizeHandle =
        hoveredNoteHandle != Logic::CanvasComponentDragHandle::None
            ? hoveredNoteHandle
            : hoveredComponentHandle;
    if ( hoveredResizeHandle == Logic::CanvasComponentDragHandle::TopLeft ||
         hoveredResizeHandle ==
             Logic::CanvasComponentDragHandle::BottomRight ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    } else if ( hoveredResizeHandle ==
                    Logic::CanvasComponentDragHandle::TopRight ||
                hoveredResizeHandle ==
                    Logic::CanvasComponentDragHandle::BottomLeft ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    } else if ( hoveredResizeHandle ==
                Logic::CanvasComponentDragHandle::Move ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if ( hoveredHandle == TrackLayoutDragHandle::Left ||
                hoveredHandle == TrackLayoutDragHandle::Right ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else if ( hoveredHandle == TrackLayoutDragHandle::Top ||
                hoveredHandle == TrackLayoutDragHandle::Bottom ||
                hoveredHandle == TrackLayoutDragHandle::JudgmentLine ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    } else if ( hoveredHandle == TrackLayoutDragHandle::Move ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    if ( isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false) ) {
        if ( hoveredNote.has_value() && hoveredNoteBounds.has_value() ) {
            m_noteScaleDragTarget      = hoveredNote;
            m_noteScaleDragHandle      = hoveredNoteHandle;
            m_noteScaleDragStartBounds = *hoveredNoteBounds;
            const auto& visual         = appConfig.getVisualConfig();
            m_noteScaleDragStart = { visual.noteScaleX, visual.noteScaleY };
        } else if ( hoveredComponent.has_value() &&
                    hoveredComponentInstance.has_value() ) {
            m_canvasComponentDragTarget = hoveredComponent;
            m_canvasComponentDragHandle = hoveredComponentHandle;
            const auto placement =
                appConfig.getVisualConfig()
                    .canvasComponentsForKeyCount(keyCount)
                    .resolvedPlacement(*hoveredComponent,
                                       hoveredComponentInstance->instanceIndex,
                                       currentSnapshot.trackCount,
                                       layout.left,
                                       layout.right);
            m_canvasComponentDragStart = placement;
            m_canvasComponentDragStartBounds =
                canvasComponentContentBounds(*hoveredComponentInstance);
            m_canvasComponentDragRegion =
                canvasComponentLayoutRegion(*hoveredComponentInstance);
            m_canvasComponentDragInstanceIndex =
                hoveredComponentInstance->instanceIndex;
            m_synchronizedKpsTransformStarts.clear();
            const auto& canvasComponents =
                appConfig.getVisualConfig().canvasComponentsForKeyCount(
                    keyCount);
            const bool draggedKpsTrack =
                hoveredComponentInstance->instanceIndex >= 0;
            const bool captureAllKpsMove =
                hoveredComponentHandle ==
                    Logic::CanvasComponentDragHandle::Move &&
                canvasComponents.syncAllKpsComponentPositions;
            const bool captureKpsTrackMove =
                hoveredComponentHandle ==
                    Logic::CanvasComponentDragHandle::Move &&
                draggedKpsTrack &&
                canvasComponents.syncKpsTrackRelativePositions &&
                !captureAllKpsMove;
            const bool captureSynchronizedKpsResize =
                hoveredComponentHandle !=
                    Logic::CanvasComponentDragHandle::Move &&
                draggedKpsTrack && canvasComponents.syncKpsTrackSizes;
            if ( *hoveredComponent == Config::CanvasComponentType::Kps &&
                 (captureAllKpsMove || captureKpsTrackMove ||
                  captureSynchronizedKpsResize) ) {
                m_synchronizedKpsTransformStarts.reserve(
                    currentSnapshot.canvasComponentInstances.size());
                for ( const auto& instance :
                      currentSnapshot.canvasComponentInstances ) {
                    if ( instance.type != Config::CanvasComponentType::Kps ||
                         (!captureAllKpsMove && instance.instanceIndex < 0) ) {
                        continue;
                    }
                    m_synchronizedKpsTransformStarts.push_back(
                        { instance.instanceIndex,
                          canvasComponents.resolvedPlacement(
                              Config::CanvasComponentType::Kps,
                              instance.instanceIndex,
                              currentSnapshot.trackCount,
                              layout.left,
                              layout.right),
                          canvasComponentContentBounds(instance),
                          canvasComponentLayoutRegion(instance) });
                }
            }
            const float centerX = (m_canvasComponentDragStartBounds.left +
                                   m_canvasComponentDragStartBounds.right) *
                                  0.5f;
            const float centerY = (m_canvasComponentDragStartBounds.top +
                                   m_canvasComponentDragStartBounds.bottom) *
                                  0.5f;
            m_canvasComponentPointerOffset = {
                centerX - pointerX,
                centerY - pointerY,
            };
        } else if ( hoveredHandle != TrackLayoutDragHandle::None ) {
            m_trackLayoutDragHandle   = hoveredHandle;
            m_trackLayoutDragStart    = layout;
            m_trackLayoutPointerStart = {
                worldPointerX / targetWidth,
                pointerY / targetHeight,
            };
        }
    }

    if ( m_noteScaleDragTarget.has_value() &&
         ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        constexpr float scaleEpsilon = 1e-6f;
        const auto      candidate =
            Logic::resizeNoteRenderScale(m_noteScaleDragStart,
                                         m_noteScaleDragHandle,
                                         m_noteScaleDragStartBounds,
                                         pointerX,
                                         pointerY);
        const auto& visual = appConfig.getVisualConfig();
        if ( std::abs(visual.noteScaleX - candidate.x) > scaleEpsilon ||
             std::abs(visual.noteScaleY - candidate.y) > scaleEpsilon ) {
            appConfig.getVisualConfig().noteScaleX = candidate.x;
            appConfig.getVisualConfig().noteScaleY = candidate.y;
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateEditorConfig{ appConfig.getEditorConfig() }));
            m_layoutConfigurationChanged = true;
        }
    }

    if ( m_canvasComponentDragTarget.has_value() &&
         ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        constexpr float componentPositionEpsilon = 1e-6f;
        auto&           canvasComponents =
            appConfig.getVisualConfig().editableCanvasComponentsForKeyCount(
                keyCount);
        auto& component = canvasComponents.editablePlacement(
            *m_canvasComponentDragTarget,
            m_canvasComponentDragInstanceIndex,
            currentSnapshot.trackCount,
            layout.left,
            layout.right);
        Config::CanvasComponentPlacement candidate = component;
        if ( m_canvasComponentDragHandle ==
             Logic::CanvasComponentDragHandle::Move ) {
            candidate = Logic::moveCanvasComponentInRegion(
                component,
                pointerX + m_canvasComponentPointerOffset.x,
                pointerY + m_canvasComponentPointerOffset.y,
                m_canvasComponentDragRegion,
                m_canvasComponentDragStartBounds.width(),
                m_canvasComponentDragStartBounds.height());

            m_canvasComponentSnapGuideX.reset();
            m_canvasComponentSnapGuideY.reset();
            m_canvasComponentSnapTargetsX.clear();
            m_canvasComponentSnapTargetsY.clear();
            const auto safeTrackCount =
                std::max<std::int32_t>(currentSnapshot.trackCount, 0);
            const std::size_t targetObjectCount =
                currentSnapshot.canvasComponentInstances.size() +
                static_cast<std::size_t>(safeTrackCount) + 2U;
            m_canvasComponentSnapTargetsX.reserve(targetObjectCount * 3U + 1U);
            m_canvasComponentSnapTargetsY.reserve(targetObjectCount * 3U + 1U);

            const bool draggedKps = *m_canvasComponentDragTarget ==
                                    Config::CanvasComponentType::Kps;
            const bool synchronizeAllKpsComponentPositions =
                draggedKps && canvasComponents.syncAllKpsComponentPositions;
            const bool synchronizeKpsTrackRelativePositions =
                draggedKps && m_canvasComponentDragInstanceIndex >= 0 &&
                !synchronizeAllKpsComponentPositions &&
                canvasComponents.syncKpsTrackRelativePositions;
            const bool groupAllKpsPositions =
                canvasComponents.kps.visible &&
                canvasComponents.syncAllKpsComponentPositions;
            const bool groupKpsTrackPositions =
                canvasComponents.kps.visible && !groupAllKpsPositions &&
                canvasComponents.syncKpsTrackRelativePositions;
            const bool movingSynchronizedKpsGroup =
                synchronizeAllKpsComponentPositions ||
                synchronizeKpsTrackRelativePositions;
            Logic::CanvasComponentBounds synchronizedKpsTargetBounds;
            bool                         hasSynchronizedKpsTargetBounds = false;
            for ( const auto& instance :
                  currentSnapshot.canvasComponentInstances ) {
                if ( !canvasComponents.placement(instance.type).visible ) {
                    continue;
                }

                const bool synchronizedKpsGroupMember =
                    instance.type == Config::CanvasComponentType::Kps &&
                    (groupAllKpsPositions ||
                     (groupKpsTrackPositions && instance.instanceIndex >= 0));
                if ( synchronizedKpsGroupMember &&
                     !movingSynchronizedKpsGroup ) {
                    mergeCanvasComponentBounds(
                        canvasComponentContentBounds(instance),
                        synchronizedKpsTargetBounds,
                        hasSynchronizedKpsTargetBounds);
                    continue;
                }

                bool movesWithDraggedComponent = false;
                if ( instance.type == *m_canvasComponentDragTarget ) {
                    if ( instance.type != Config::CanvasComponentType::Kps ) {
                        movesWithDraggedComponent = true;
                    } else if ( synchronizeAllKpsComponentPositions ) {
                        movesWithDraggedComponent = true;
                    } else if ( synchronizeKpsTrackRelativePositions &&
                                instance.instanceIndex >= 0 ) {
                        movesWithDraggedComponent = true;
                    } else {
                        movesWithDraggedComponent =
                            instance.instanceIndex ==
                            m_canvasComponentDragInstanceIndex;
                    }
                }
                if ( movesWithDraggedComponent ) continue;

                appendCanvasComponentSnapTargets(
                    canvasComponentContentBounds(instance),
                    m_canvasComponentSnapTargetsX,
                    m_canvasComponentSnapTargetsY);
            }
            if ( hasSynchronizedKpsTargetBounds ) {
                appendCanvasComponentSnapTargets(synchronizedKpsTargetBounds,
                                                 m_canvasComponentSnapTargetsX,
                                                 m_canvasComponentSnapTargetsY);
            }

            const Logic::CanvasComponentBounds trackLayoutBounds{
                layout.left * targetWidth + cameraOffsetX,
                layout.top * targetHeight,
                layout.right * targetWidth + cameraOffsetX,
                layout.bottom * targetHeight,
            };
            appendCanvasComponentSnapTargets(trackLayoutBounds,
                                             m_canvasComponentSnapTargetsX,
                                             m_canvasComponentSnapTargetsY);
            if ( safeTrackCount > 0 ) {
                const float singleTrackWidth =
                    trackLayoutBounds.width() /
                    static_cast<float>(safeTrackCount);
                for ( std::int32_t trackIndex = 0; trackIndex < safeTrackCount;
                      ++trackIndex ) {
                    const float trackLeft =
                        trackLayoutBounds.left +
                        static_cast<float>(trackIndex) * singleTrackWidth;
                    appendCanvasComponentSnapTargets(
                        { trackLeft,
                          trackLayoutBounds.top,
                          trackLeft + singleTrackWidth,
                          trackLayoutBounds.bottom },
                        m_canvasComponentSnapTargetsX,
                        m_canvasComponentSnapTargetsY);
                }
            }
            m_canvasComponentSnapTargetsX.push_back(targetWidth * 0.5f);
            m_canvasComponentSnapTargetsY.push_back(targetHeight * 0.5f);

            const auto candidateBounds = Logic::canvasComponentBoundsInRegion(
                candidate,
                m_canvasComponentDragRegion,
                m_canvasComponentDragStartBounds.width(),
                m_canvasComponentDragStartBounds.height());
            Logic::CanvasComponentBounds synchronizedKpsGroupStartBounds;
            bool                         hasSynchronizedKpsGroupBounds = false;
            if ( synchronizeAllKpsComponentPositions ||
                 synchronizeKpsTrackRelativePositions ) {
                for ( const auto& transformStart :
                      m_synchronizedKpsTransformStarts ) {
                    mergeCanvasComponentBounds(transformStart.bounds,
                                               synchronizedKpsGroupStartBounds,
                                               hasSynchronizedKpsGroupBounds);
                }
            }

            Logic::CanvasComponentBounds snapSourceBounds = candidateBounds;
            if ( hasSynchronizedKpsGroupBounds ) {
                const float candidateOffsetX =
                    (candidateBounds.left + candidateBounds.right -
                     m_canvasComponentDragStartBounds.left -
                     m_canvasComponentDragStartBounds.right) *
                    0.5f;
                const float candidateOffsetY =
                    (candidateBounds.top + candidateBounds.bottom -
                     m_canvasComponentDragStartBounds.top -
                     m_canvasComponentDragStartBounds.bottom) *
                    0.5f;
                snapSourceBounds =
                    offsetCanvasComponentBounds(synchronizedKpsGroupStartBounds,
                                                candidateOffsetX,
                                                candidateOffsetY);
            }
            const auto snap =
                Logic::snapCanvasComponentBounds(snapSourceBounds,
                                                 m_canvasComponentSnapTargetsX,
                                                 m_canvasComponentSnapTargetsY,
                                                 componentSnapDistance);
            const float snapOffsetX =
                snap.center.x -
                (snapSourceBounds.left + snapSourceBounds.right) * 0.5f;
            const float snapOffsetY =
                snap.center.y -
                (snapSourceBounds.top + snapSourceBounds.bottom) * 0.5f;
            candidate = Logic::moveCanvasComponentInRegion(
                candidate,
                (candidateBounds.left + candidateBounds.right) * 0.5f +
                    snapOffsetX,
                (candidateBounds.top + candidateBounds.bottom) * 0.5f +
                    snapOffsetY,
                m_canvasComponentDragRegion,
                m_canvasComponentDragStartBounds.width(),
                m_canvasComponentDragStartBounds.height());
            const auto snappedBounds = Logic::canvasComponentBoundsInRegion(
                candidate,
                m_canvasComponentDragRegion,
                m_canvasComponentDragStartBounds.width(),
                m_canvasComponentDragStartBounds.height());
            Logic::CanvasComponentBounds finalSnapSourceBounds = snappedBounds;
            if ( hasSynchronizedKpsGroupBounds ) {
                const float appliedOffsetX =
                    (snappedBounds.left + snappedBounds.right -
                     m_canvasComponentDragStartBounds.left -
                     m_canvasComponentDragStartBounds.right) *
                    0.5f;
                const float appliedOffsetY =
                    (snappedBounds.top + snappedBounds.bottom -
                     m_canvasComponentDragStartBounds.top -
                     m_canvasComponentDragStartBounds.bottom) *
                    0.5f;
                finalSnapSourceBounds =
                    offsetCanvasComponentBounds(synchronizedKpsGroupStartBounds,
                                                appliedOffsetX,
                                                appliedOffsetY);
            }
            if ( snap.snappedX && canvasComponentAlignsWithX(
                                      finalSnapSourceBounds, snap.targetX) ) {
                m_canvasComponentSnapGuideX = snap.targetX;
            }
            if ( snap.snappedY && canvasComponentAlignsWithY(
                                      finalSnapSourceBounds, snap.targetY) ) {
                m_canvasComponentSnapGuideY = snap.targetY;
            }
        } else {
            candidate = Logic::resizeCanvasComponentInRegion(
                m_canvasComponentDragStart,
                m_canvasComponentDragHandle,
                m_canvasComponentDragStartBounds,
                pointerX,
                pointerY,
                m_canvasComponentDragRegion);
        }
        if ( std::abs(component.anchorX - candidate.anchorX) >
                 componentPositionEpsilon ||
             std::abs(component.anchorY - candidate.anchorY) >
                 componentPositionEpsilon ||
             std::abs(component.fontSizeRatio - candidate.fontSizeRatio) >
                 componentPositionEpsilon ) {
            component = candidate;
            const bool synchronizeAllKpsComponentPositions =
                *m_canvasComponentDragTarget ==
                    Config::CanvasComponentType::Kps &&
                m_canvasComponentDragHandle ==
                    Logic::CanvasComponentDragHandle::Move &&
                canvasComponents.syncAllKpsComponentPositions;
            const bool synchronizeKpsTrackRelativePositions =
                *m_canvasComponentDragTarget ==
                    Config::CanvasComponentType::Kps &&
                m_canvasComponentDragInstanceIndex >= 0 &&
                m_canvasComponentDragHandle ==
                    Logic::CanvasComponentDragHandle::Move &&
                !synchronizeAllKpsComponentPositions &&
                canvasComponents.syncKpsTrackRelativePositions;
            if ( synchronizeAllKpsComponentPositions ||
                 synchronizeKpsTrackRelativePositions ) {
                const float candidateCenterX =
                    m_canvasComponentDragRegion.left +
                    candidate.anchorX * m_canvasComponentDragRegion.width();
                const float candidateCenterY =
                    m_canvasComponentDragRegion.top +
                    candidate.anchorY * m_canvasComponentDragRegion.height();
                const float startCenterX =
                    (m_canvasComponentDragStartBounds.left +
                     m_canvasComponentDragStartBounds.right) *
                    0.5f;
                const float startCenterY =
                    (m_canvasComponentDragStartBounds.top +
                     m_canvasComponentDragStartBounds.bottom) *
                    0.5f;
                const float offsetX = candidateCenterX - startCenterX;
                const float offsetY = candidateCenterY - startCenterY;
                for ( const auto& transformStart :
                      m_synchronizedKpsTransformStarts ) {
                    if ( transformStart.instanceIndex ==
                         m_canvasComponentDragInstanceIndex ) {
                        continue;
                    }
                    auto synchronizedPlacement =
                        Logic::moveCanvasComponentByOffsetInRegion(
                            transformStart.placement,
                            transformStart.bounds,
                            offsetX,
                            offsetY,
                            transformStart.region);
                    auto& synchronizedComponent =
                        canvasComponents.editablePlacement(
                            Config::CanvasComponentType::Kps,
                            transformStart.instanceIndex,
                            currentSnapshot.trackCount,
                            layout.left,
                            layout.right);
                    synchronizedComponent = synchronizedPlacement;
                }
            }
            const bool synchronizeKpsTrackSize =
                *m_canvasComponentDragTarget ==
                    Config::CanvasComponentType::Kps &&
                m_canvasComponentDragInstanceIndex >= 0 &&
                m_canvasComponentDragHandle !=
                    Logic::CanvasComponentDragHandle::Move &&
                canvasComponents.syncKpsTrackSizes;
            if ( synchronizeKpsTrackSize ) {
                for ( const auto& transformStart :
                      m_synchronizedKpsTransformStarts ) {
                    if ( transformStart.instanceIndex ==
                         m_canvasComponentDragInstanceIndex ) {
                        continue;
                    }
                    auto synchronizedPlacement =
                        Logic::resizeCanvasComponentToFontSizeInRegion(
                            transformStart.placement,
                            m_canvasComponentDragHandle,
                            transformStart.bounds,
                            candidate.fontSizeRatio,
                            transformStart.region);
                    auto& synchronizedComponent =
                        canvasComponents.editablePlacement(
                            Config::CanvasComponentType::Kps,
                            transformStart.instanceIndex,
                            currentSnapshot.trackCount,
                            layout.left,
                            layout.right);
                    synchronizedComponent = synchronizedPlacement;
                }
                canvasComponents.synchronizeKpsTrackFontSize(
                    candidate.fontSizeRatio);
            }
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateEditorConfig{ appConfig.getEditorConfig() }));
            m_layoutConfigurationChanged = true;
        }
    }

    if ( !m_noteScaleDragTarget.has_value() &&
         !m_canvasComponentDragTarget.has_value() &&
         m_trackLayoutDragHandle != TrackLayoutDragHandle::None &&
         ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        const float         normalizedX = worldPointerX / targetWidth;
        const float         normalizedY = pointerY / targetHeight;
        Config::TrackLayout candidate   = m_trackLayoutDragStart;
        if ( m_trackLayoutDragHandle == TrackLayoutDragHandle::JudgmentLine ) {
            constexpr float positionEpsilon = 1e-6f;
            const float     candidatePosition =
                sanitizeJudgmentLinePosition(normalizedY);
            const float currentPosition =
                appConfig.getVisualConfig().judgmentLinePositionForKeyCount(
                    keyCount);
            if ( !std::isfinite(currentPosition) ||
                 std::abs(currentPosition - candidatePosition) >
                     positionEpsilon ) {
                appConfig.getVisualConfig()
                    .editableJudgmentLinePositionForKeyCount(keyCount) =
                    candidatePosition;
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                        appConfig.getEditorConfig() }));
                m_layoutConfigurationChanged = true;
                judgmentLinePosition         = candidatePosition;
            }
        } else {
            switch ( m_trackLayoutDragHandle ) {
            case TrackLayoutDragHandle::Left:
            case TrackLayoutDragHandle::Right:
                candidate = resizeTrackLayout(m_trackLayoutDragStart,
                                              m_trackLayoutDragHandle,
                                              normalizedX);
                break;
            case TrackLayoutDragHandle::Top:
            case TrackLayoutDragHandle::Bottom:
                candidate = resizeTrackLayout(m_trackLayoutDragStart,
                                              m_trackLayoutDragHandle,
                                              normalizedY);
                break;
            case TrackLayoutDragHandle::Move: {
                candidate =
                    moveTrackLayout(m_trackLayoutDragStart,
                                    normalizedX - m_trackLayoutPointerStart.x,
                                    normalizedY - m_trackLayoutPointerStart.y);
                m_canvasComponentSnapGuideX.reset();
                m_canvasComponentSnapGuideY.reset();
                m_canvasComponentSnapTargetsX.clear();
                m_canvasComponentSnapTargetsY.clear();
                const auto& canvasComponents =
                    appConfig.getVisualConfig().canvasComponentsForKeyCount(
                        keyCount);
                const std::size_t targetObjectCount =
                    currentSnapshot.canvasComponentInstances.size();
                m_canvasComponentSnapTargetsX.reserve(targetObjectCount * 3U +
                                                      1U);
                m_canvasComponentSnapTargetsY.reserve(targetObjectCount * 3U +
                                                      1U);
                appendDisplayedCanvasComponentSnapTargets(
                    currentSnapshot,
                    canvasComponents,
                    m_canvasComponentSnapTargetsX,
                    m_canvasComponentSnapTargetsY);
                m_canvasComponentSnapTargetsX.push_back(targetWidth * 0.5f);
                m_canvasComponentSnapTargetsY.push_back(targetHeight * 0.5f);

                const Logic::CanvasComponentBounds candidateBounds{
                    candidate.left * targetWidth + cameraOffsetX,
                    candidate.top * targetHeight,
                    candidate.right * targetWidth + cameraOffsetX,
                    candidate.bottom * targetHeight,
                };
                const auto snap = Logic::snapCanvasComponentBounds(
                    candidateBounds,
                    m_canvasComponentSnapTargetsX,
                    m_canvasComponentSnapTargetsY,
                    componentSnapDistance);
                candidate =
                    moveTrackLayoutToPixelCenter(candidate,
                                                 snap.center.x - cameraOffsetX,
                                                 snap.center.y,
                                                 targetWidth,
                                                 targetHeight);
                const Logic::CanvasComponentBounds snappedBounds{
                    candidate.left * targetWidth + cameraOffsetX,
                    candidate.top * targetHeight,
                    candidate.right * targetWidth + cameraOffsetX,
                    candidate.bottom * targetHeight,
                };
                if ( snap.snappedX &&
                     canvasComponentAlignsWithX(snappedBounds, snap.targetX) ) {
                    m_canvasComponentSnapGuideX = snap.targetX;
                }
                if ( snap.snappedY &&
                     canvasComponentAlignsWithY(snappedBounds, snap.targetY) ) {
                    m_canvasComponentSnapGuideY = snap.targetY;
                }
                break;
            }
            case TrackLayoutDragHandle::JudgmentLine: break;
            case TrackLayoutDragHandle::None: break;
            }

            constexpr float layoutEpsilon = 1e-6f;
            const auto&     current =
                appConfig.getVisualConfig().trackLayoutForKeyCount(keyCount);
            const bool changed =
                std::abs(current.left - candidate.left) > layoutEpsilon ||
                std::abs(current.top - candidate.top) > layoutEpsilon ||
                std::abs(current.right - candidate.right) > layoutEpsilon ||
                std::abs(current.bottom - candidate.bottom) > layoutEpsilon;
            if ( changed ) {
                appConfig.getVisualConfig().editableTrackLayoutForKeyCount(
                    keyCount) = candidate;
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                        appConfig.getEditorConfig() }));
                m_layoutConfigurationChanged = true;
                layout                       = candidate;
            }
        }
    }

    if ( (m_trackLayoutDragHandle != TrackLayoutDragHandle::None ||
          m_noteScaleDragTarget.has_value() ||
          m_canvasComponentDragTarget.has_value()) &&
         !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        finishLayoutEditing();
    }

    layout = sanitizeTrackLayout(
        appConfig.getVisualConfig().trackLayoutForKeyCount(keyCount));
    judgmentLinePosition = sanitizeJudgmentLinePosition(
        appConfig.getVisualConfig().judgmentLinePositionForKeyCount(keyCount));
    const ImVec2 canvasMin{ canvasScreenX, canvasScreenY };
    const ImVec2 canvasMax{ canvasScreenX + targetWidth,
                            canvasScreenY + targetHeight };
    const ImVec2 layoutMin{ canvasScreenX + layout.left * targetWidth +
                                cameraOffsetX,
                            canvasScreenY + layout.top * targetHeight };
    const ImVec2 layoutMax{ canvasScreenX + layout.right * targetWidth +
                                cameraOffsetX,
                            canvasScreenY + layout.bottom * targetHeight };
    const ImVec2 layoutCenter{ (layoutMin.x + layoutMax.x) * 0.5f,
                               (layoutMin.y + layoutMax.y) * 0.5f };
    const float  judgmentLineY =
        canvasScreenY + judgmentLinePosition * targetHeight;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->PushClipRect(canvasMin, canvasMax, true);
    drawList->AddRectFilled(
        canvasMin, { canvasMax.x, layoutMin.y }, IM_COL32(0, 0, 0, 72));
    drawList->AddRectFilled(
        { canvasMin.x, layoutMax.y }, canvasMax, IM_COL32(0, 0, 0, 72));
    drawList->AddRectFilled({ canvasMin.x, layoutMin.y },
                            { layoutMin.x, layoutMax.y },
                            IM_COL32(0, 0, 0, 72));
    drawList->AddRectFilled({ layoutMax.x, layoutMin.y },
                            { canvasMax.x, layoutMax.y },
                            IM_COL32(0, 0, 0, 72));
    drawList->AddRectFilled(layoutMin, layoutMax, IM_COL32(64, 180, 255, 24));

    std::optional<Logic::CanvasComponentBounds> editableComponentRegion;
    if ( m_canvasComponentDragTarget.has_value() ) {
        editableComponentRegion = m_canvasComponentDragRegion;
    } else if ( hoveredComponentInstance.has_value() &&
                hoveredComponentHandle !=
                    Logic::CanvasComponentDragHandle::None ) {
        editableComponentRegion =
            canvasComponentLayoutRegion(*hoveredComponentInstance);
    }
    if ( editableComponentRegion.has_value() ) {
        drawCanvasComponentEditableRegionMask(*drawList,
                                              *editableComponentRegion,
                                              canvasScreenX,
                                              canvasScreenY,
                                              targetWidth,
                                              targetHeight,
                                              dpiScale);
    }

    const ImU32 edgeColor        = IM_COL32(64, 190, 255, 230);
    const ImU32 highlightedColor = IM_COL32(255, 218, 96, 255);
    const float edgeThickness    = std::max(2.0f, 2.0f * dpiScale);
    auto        handleColor      = [&](TrackLayoutDragHandle handle) {
        return hoveredHandle == handle ? highlightedColor : edgeColor;
    };
    drawList->AddLine({ layoutMin.x, layoutMin.y },
                      { layoutMin.x, layoutMax.y },
                      handleColor(TrackLayoutDragHandle::Left),
                      edgeThickness);
    drawList->AddLine({ layoutMin.x, layoutMin.y },
                      { layoutMax.x, layoutMin.y },
                      handleColor(TrackLayoutDragHandle::Top),
                      edgeThickness);
    drawList->AddLine({ layoutMax.x, layoutMin.y },
                      { layoutMax.x, layoutMax.y },
                      handleColor(TrackLayoutDragHandle::Right),
                      edgeThickness);
    drawList->AddLine({ layoutMin.x, layoutMax.y },
                      { layoutMax.x, layoutMax.y },
                      handleColor(TrackLayoutDragHandle::Bottom),
                      edgeThickness);

    const ImU32 judgmentLineColor =
        hoveredHandle == TrackLayoutDragHandle::JudgmentLine
            ? highlightedColor
            : IM_COL32(255, 112, 190, 235);
    drawList->AddLine({ layoutMin.x, judgmentLineY },
                      { layoutMax.x, judgmentLineY },
                      judgmentLineColor,
                      edgeThickness);

    const float gripHalfLong  = std::max(12.0f, 15.0f * dpiScale);
    const float gripHalfShort = std::max(3.0f, 4.0f * dpiScale);
    const float gripRounding  = gripHalfShort;
    const float middleX       = layoutCenter.x;
    const float middleY       = layoutCenter.y;
    drawList->AddRectFilled(
        { layoutMin.x - gripHalfShort, middleY - gripHalfLong },
        { layoutMin.x + gripHalfShort, middleY + gripHalfLong },
        handleColor(TrackLayoutDragHandle::Left),
        gripRounding);
    drawList->AddRectFilled(
        { layoutMax.x - gripHalfShort, middleY - gripHalfLong },
        { layoutMax.x + gripHalfShort, middleY + gripHalfLong },
        handleColor(TrackLayoutDragHandle::Right),
        gripRounding);
    drawList->AddRectFilled(
        { middleX - gripHalfLong, layoutMin.y - gripHalfShort },
        { middleX + gripHalfLong, layoutMin.y + gripHalfShort },
        handleColor(TrackLayoutDragHandle::Top),
        gripRounding);
    drawList->AddRectFilled(
        { middleX - gripHalfLong, layoutMax.y - gripHalfShort },
        { middleX + gripHalfLong, layoutMax.y + gripHalfShort },
        handleColor(TrackLayoutDragHandle::Bottom),
        gripRounding);
    drawList->AddRectFilled(
        { layoutMax.x - gripHalfLong, judgmentLineY - gripHalfShort },
        { layoutMax.x + gripHalfLong, judgmentLineY + gripHalfShort },
        judgmentLineColor,
        gripRounding);

    const ImU32 moveColor = handleColor(TrackLayoutDragHandle::Move);
    drawList->AddCircleFilled(layoutCenter, moveHandleRadius, moveColor);
    const float arrowLength    = moveHandleRadius * 0.55f;
    const float arrowThickness = std::max(1.5f, 1.5f * dpiScale);
    drawList->AddLine({ layoutCenter.x - arrowLength, layoutCenter.y },
                      { layoutCenter.x + arrowLength, layoutCenter.y },
                      IM_COL32(20, 30, 40, 255),
                      arrowThickness);
    drawList->AddLine({ layoutCenter.x, layoutCenter.y - arrowLength },
                      { layoutCenter.x, layoutCenter.y + arrowLength },
                      IM_COL32(20, 30, 40, 255),
                      arrowThickness);

    const auto& canvasComponents =
        appConfig.getVisualConfig().canvasComponentsForKeyCount(keyCount);
    const bool groupAllKpsPositions =
        canvasComponents.kps.visible &&
        canvasComponents.syncAllKpsComponentPositions;
    const bool groupKpsTrackPositions =
        canvasComponents.kps.visible && !groupAllKpsPositions &&
        canvasComponents.syncKpsTrackRelativePositions;
    Logic::CanvasComponentBounds synchronizedKpsGroupBounds;
    bool                         hasSynchronizedKpsGroupBounds   = false;
    std::size_t                  synchronizedKpsGroupMemberCount = 0U;
    if ( groupAllKpsPositions || groupKpsTrackPositions ) {
        for ( const auto& instance :
              currentSnapshot.canvasComponentInstances ) {
            if ( instance.type != Config::CanvasComponentType::Kps ||
                 (groupKpsTrackPositions && instance.instanceIndex < 0) ) {
                continue;
            }
            const auto placement = canvasComponents.resolvedPlacement(
                Config::CanvasComponentType::Kps,
                instance.instanceIndex,
                currentSnapshot.trackCount,
                layout.left,
                layout.right);
            const auto contentBounds = canvasComponentContentBounds(instance);
            const auto bounds        = Logic::canvasComponentBoundsInRegion(
                placement,
                canvasComponentLayoutRegion(instance),
                contentBounds.width(),
                contentBounds.height());
            if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) continue;
            mergeCanvasComponentBounds(bounds,
                                       synchronizedKpsGroupBounds,
                                       hasSynchronizedKpsGroupBounds);
            ++synchronizedKpsGroupMemberCount;
        }
    }
    if ( hasSynchronizedKpsGroupBounds &&
         synchronizedKpsGroupMemberCount > 1U ) {
        const bool movingSynchronizedKps =
            m_canvasComponentDragTarget == Config::CanvasComponentType::Kps &&
            m_canvasComponentDragHandle ==
                Logic::CanvasComponentDragHandle::Move &&
            (groupAllKpsPositions || (groupKpsTrackPositions &&
                                      m_canvasComponentDragInstanceIndex >= 0));
        const ImU32 groupColor = movingSynchronizedKps
                                     ? highlightedColor
                                     : IM_COL32(90, 220, 255, 180);
        drawList->AddRect({ canvasScreenX + synchronizedKpsGroupBounds.left,
                            canvasScreenY + synchronizedKpsGroupBounds.top },
                          { canvasScreenX + synchronizedKpsGroupBounds.right,
                            canvasScreenY + synchronizedKpsGroupBounds.bottom },
                          groupColor,
                          0.0f,
                          0,
                          std::max(1.5f, 2.0f * dpiScale));
    }

    const ImU32 snapGuideColor     = IM_COL32(255, 218, 96, 150);
    const float snapGuideThickness = std::max(1.0f, 1.25f * dpiScale);
    const float snapGuideDash      = std::max(4.0f, 6.0f * dpiScale);
    const float snapGuideGap       = std::max(3.0f, 4.0f * dpiScale);
    if ( m_canvasComponentSnapGuideX.has_value() ) {
        const float guideX = canvasScreenX + *m_canvasComponentSnapGuideX;
        drawCanvasComponentSnapGuide(*drawList,
                                     { guideX, canvasMin.y },
                                     { guideX, canvasMax.y },
                                     snapGuideColor,
                                     snapGuideThickness,
                                     snapGuideDash,
                                     snapGuideGap);
    }
    if ( m_canvasComponentSnapGuideY.has_value() ) {
        const float guideY = canvasScreenY + *m_canvasComponentSnapGuideY;
        drawCanvasComponentSnapGuide(*drawList,
                                     { canvasMin.x, guideY },
                                     { canvasMax.x, guideY },
                                     snapGuideColor,
                                     snapGuideThickness,
                                     snapGuideDash,
                                     snapGuideGap);
    }

    const ImU32 noteBoundsColor = IM_COL32(255, 150, 96, 220);
    for ( const auto& instance : m_noteLayoutInstances ) {
        const auto& bounds = instance.bounds;
        if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) continue;
        const bool highlighted =
            (hoveredNote.has_value() && *hoveredNote == instance.entity) ||
            (m_noteScaleDragTarget.has_value() &&
             *m_noteScaleDragTarget == instance.entity);
        const ImU32 componentColor =
            highlighted ? highlightedColor : noteBoundsColor;
        const ImVec2 componentMin{ canvasScreenX + bounds.left,
                                   canvasScreenY + bounds.top };
        const ImVec2 componentMax{ canvasScreenX + bounds.right,
                                   canvasScreenY + bounds.bottom };
        drawList->AddRect(componentMin,
                          componentMax,
                          componentColor,
                          0.0f,
                          0,
                          std::max(1.0f, 1.25f * dpiScale));

        const float handleHalf = std::max(2.5f, 3.0f * dpiScale);
        const std::array<ImVec2, 4> corners{
            componentMin,
            ImVec2{ componentMax.x, componentMin.y },
            ImVec2{ componentMin.x, componentMax.y },
            componentMax,
        };
        for ( const auto& corner : corners ) {
            drawList->AddRectFilled(
                { corner.x - handleHalf, corner.y - handleHalf },
                { corner.x + handleHalf, corner.y + handleHalf },
                componentColor);
        }
    }

    for ( const auto& instance : currentSnapshot.canvasComponentInstances ) {
        const auto& placement = appConfig.getVisualConfig()
                                    .canvasComponentsForKeyCount(keyCount)
                                    .placement(instance.type);
        if ( !placement.visible ) continue;

        const auto bounds = canvasComponentContentBounds(instance);
        if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) continue;
        const bool highlighted =
            hoveredComponent.has_value() &&
            *hoveredComponent == instance.type &&
            ((hoveredComponentInstance.has_value() &&
              hoveredComponentInstance->instanceIndex ==
                  instance.instanceIndex) ||
             (m_canvasComponentDragTarget.has_value() &&
              m_canvasComponentDragInstanceIndex == instance.instanceIndex));
        const ImU32 componentColor =
            highlighted ? highlightedColor : IM_COL32(90, 220, 255, 240);
        const ImVec2 componentMin{ canvasScreenX + bounds.left,
                                   canvasScreenY + bounds.top };
        const ImVec2 componentMax{ canvasScreenX + bounds.right,
                                   canvasScreenY + bounds.bottom };
        drawList->AddRect(componentMin,
                          componentMax,
                          componentColor,
                          0.0f,
                          0,
                          std::max(1.0f, 1.5f * dpiScale));

        const float handleHalf = std::max(3.0f, 3.5f * dpiScale);
        const std::array<ImVec2, 4> corners{
            componentMin,
            ImVec2{ componentMax.x, componentMin.y },
            ImVec2{ componentMin.x, componentMax.y },
            componentMax,
        };
        for ( const auto& corner : corners ) {
            drawList->AddRectFilled(
                { corner.x - handleHalf, corner.y - handleHalf },
                { corner.x + handleHalf, corner.y + handleHalf },
                componentColor);
        }
    }
    drawList->PopClipRect();
}

bool Basic2DCanvasInteraction::renderAnnotationGutter(
    const Logic::RenderSnapshot& currentSnapshot, float canvasScreenX,
    float canvasScreenY, float targetWidth, float targetHeight, float pointerX,
    float pointerY, bool canvasHovered)
{
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    const auto& layout =
        visual.trackLayoutForKeyCount(currentSnapshot.trackCount);
    const auto projection = Logic::calculateCanvasLaneProjection(
        targetWidth,
        currentSnapshot.trackCount,
        currentSnapshot.bgmTrackCount,
        layout.left,
        layout.right,
        currentSnapshot.canvasHorizontalOffsetX,
        true,
        currentSnapshot.bmsEditingEnabled,
        currentSnapshot.draftLanesEnabled);
    const float topY          = layout.top * targetHeight;
    const float bottomY       = layout.bottom * targetHeight;
    const bool  gutterHovered = projection.valid && canvasHovered &&
                                pointerX >= projection.annotationLeftX &&
                                pointerX <= projection.annotationRightX &&
                                pointerY >= topY && pointerY <= bottomY;

    const Logic::AnnotationRenderMarker* hoveredMarker = nullptr;
    std::optional<std::size_t>           hoveredDetailIndex;
    bool                                 detailCardHovered   = false;
    bool                                 detailLinkHovered   = false;
    bool                                 detailWheelConsumed = false;
    if ( projection.valid && currentSnapshot.hasBeatmap ) {
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(
            { canvasScreenX, canvasScreenY + topY },
            { canvasScreenX + targetWidth, canvasScreenY + bottomY },
            true);
        const float centerX =
            (projection.annotationLeftX + projection.annotationRightX) * 0.5F;
        const ImU32 markerColor = annotationUiColor(
            "annotations.marker", ImVec4(0.42F, 0.72F, 0.96F, 0.98F));
        const ImU32 hoverColor = annotationUiColor(
            "annotations.marker_hover", ImVec4(0.68F, 0.86F, 1.0F, 1.0F));
        const ImU32 textColor = annotationUiColor(
            "annotations.marker_text", ImVec4(0.04F, 0.08F, 0.12F, 1.0F));

        if ( Config::AppConfig::instance()
                 .getEditorSettings()
                 .showAnnotationDetails ) {
            const auto detailHit = renderConnectedAnnotationDetails(
                currentSnapshot.annotationMarkers,
                projection,
                canvasScreenX,
                canvasScreenY,
                targetWidth,
                topY,
                bottomY,
                pointerX,
                pointerY,
                canvasHovered,
                m_annotationDetailScrollItemId,
                m_annotationDetailScrollY);
            if ( detailHit.marker ) {
                hoveredMarker       = detailHit.marker;
                hoveredDetailIndex  = detailHit.itemIndex;
                detailCardHovered   = true;
                detailLinkHovered   = detailHit.linkHovered;
                detailWheelConsumed = detailHit.wheelConsumed;
                if ( detailHit.itemIndex < detailHit.marker->items.size() ) {
                    const auto targetBounds = findAnnotationTargetHintBounds(
                        detailHit.marker->items[detailHit.itemIndex],
                        currentSnapshot.hitboxes);
                    if ( targetBounds ) {
                        renderAnnotationTargetHint(
                            *targetBounds,
                            { canvasScreenX, canvasScreenY },
                            targetWidth,
                            targetHeight);
                    }
                }
            }
        }

        for ( const auto& marker : currentSnapshot.annotationMarkers ) {
            const float markerY = marker.canvasY;
            if ( markerY < topY - 10.0F || markerY > bottomY + 10.0F ) {
                continue;
            }
            const bool markerHovered =
                gutterHovered && std::abs(pointerY - markerY) <= 10.0F;
            if ( markerHovered && !hoveredMarker ) hoveredMarker = &marker;

            const ImVec2 bubbleMin{ canvasScreenX + centerX - 8.0F,
                                    canvasScreenY + markerY - 6.5F };
            const ImVec2 bubbleMax{ canvasScreenX + centerX + 8.0F,
                                    canvasScreenY + markerY + 6.5F };
            drawList->AddRectFilled(bubbleMin,
                                    bubbleMax,
                                    markerHovered ? hoverColor : markerColor,
                                    3.0F);
            const std::array<ImVec2, 3> tail{
                ImVec2{ canvasScreenX + centerX - 3.0F,
                        canvasScreenY + markerY + 6.0F },
                ImVec2{ canvasScreenX + centerX + 1.0F,
                        canvasScreenY + markerY + 10.0F },
                ImVec2{ canvasScreenX + centerX + 3.0F,
                        canvasScreenY + markerY + 6.0F },
            };
            drawList->AddTriangleFilled(
                tail[0],
                tail[1],
                tail[2],
                markerHovered ? hoverColor : markerColor);
            if ( marker.items.size() > 1U ) {
                const std::string count = std::to_string(marker.items.size());
                const ImVec2      textSize = ImGui::CalcTextSize(count.c_str());
                drawList->AddText(
                    { canvasScreenX + centerX - textSize.x * 0.5F,
                      canvasScreenY + markerY - textSize.y * 0.5F },
                    textColor,
                    count.c_str());
            }
        }
        drawList->PopClipRect();
    }

    if ( hoveredMarker && !hoveredMarker->items.empty() ) {
        const std::string& markerId = hoveredMarker->items.front().id;
        bool               detailSelectionChanged = false;
        if ( markerId != m_annotationHoverMarkerId ) {
            m_annotationHoverMarkerId    = markerId;
            m_annotationHoverDetailIndex = hoveredDetailIndex.value_or(0U);
            detailSelectionChanged       = true;
        } else if ( hoveredDetailIndex ) {
            detailSelectionChanged =
                m_annotationHoverDetailIndex != *hoveredDetailIndex;
            m_annotationHoverDetailIndex = *hoveredDetailIndex;
        }
        m_annotationHoverDetailIndex = std::min(
            m_annotationHoverDetailIndex, hoveredMarker->items.size() - 1U);

        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0F, 0.0F),
                                            ImVec2(620.0F, 720.0F));
        ImGui::BeginTooltip();
        if ( detailSelectionChanged ) ImGui::SetScrollY(0.0F);
        const float tooltipMaxScrollY = ImGui::GetScrollMaxY();
        const float wheel             = ImGui::GetIO().MouseWheel;
        if ( !detailWheelConsumed && std::abs(wheel) > 0.01F ) {
            const auto wheelResult =
                updateAnnotationDetailWheel(wheel,
                                            ImGui::GetScrollY(),
                                            tooltipMaxScrollY,
                                            ImGui::GetFontSize() * 4.0F);
            if ( wheelResult.consumed ) {
                ImGui::SetScrollY(wheelResult.scrollY);
            }
        }
        if ( !detailCardHovered && hoveredMarker->items.size() > 1U &&
             !ImGui::GetIO().WantTextInput ) {
            int direction = 0;
            if ( ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ) {
                direction = -1;
            } else if ( ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ) {
                direction = 1;
            }
            if ( direction != 0 ) {
                m_annotationHoverDetailIndex =
                    stepAnnotationDetailItem(hoveredMarker->items.size(),
                                             m_annotationHoverDetailIndex,
                                             direction);
                ImGui::SetScrollY(0.0F);
            }
        }
        const auto timeText =
            formatCanvasTime(hoveredMarker->timestamp, &currentSnapshot);
        ImGui::Text("%s · %s · %zu",
                    TR("ui.annotation.marker_title").data(),
                    timeText.c_str(),
                    hoveredMarker->items.size());
        ImGui::Separator();
        for ( std::size_t index = 0U; index < hoveredMarker->items.size();
              ++index ) {
            const auto&            item = hoveredMarker->items[index];
            const std::string_view author =
                item.author.empty() ? TR("ui.annotation.unknown_author").view()
                                    : std::string_view(item.author);
            const ImVec4 color = index == m_annotationHoverDetailIndex
                                     ? ImVec4(0.45F, 0.78F, 1.0F, 1.0F)
                                     : ImVec4(0.72F, 0.74F, 0.78F, 1.0F);
            ImGui::TextColored(color,
                               "%zu. %.*s",
                               index + 1U,
                               static_cast<int>(author.size()),
                               author.data());
            const auto             firstLineEnd = item.content.find('\n');
            const std::string_view firstLine(item.content.data(),
                                             firstLineEnd == std::string::npos
                                                 ? item.content.size()
                                                 : firstLineEnd);
            ImGui::SameLine();
            ImGui::TextWrapped(
                "— %.*s", static_cast<int>(firstLine.size()), firstLine.data());
        }
        if ( hoveredMarker->items.size() > 1U ) {
            ImGui::TextDisabled("%s", TR("ui.annotation.wheel_hint").data());
        } else if ( tooltipMaxScrollY > 0.01F ) {
            ImGui::TextDisabled("%s", TR("ui.annotation.scroll_hint").data());
        }
        ImGui::Separator();
        const auto& detail = hoveredMarker->items[m_annotationHoverDetailIndex];
        const char* targetLabel = annotationTargetLabelKey(detail.targetKind);
        ImGui::Text("%s: %s",
                    TR("ui.annotation.target").data(),
                    TR(targetLabel).data());
        if ( detail.track >= 0 ) {
            ImGui::SameLine();
            ImGui::TextDisabled("#%d", detail.track + 1);
        }
        if ( detail.targetMissing ) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0F, 0.42F, 0.32F, 1.0F),
                               "%s",
                               TR("ui.annotation.target_missing").data());
        }
        ImGui::Text("%s: %s",
                    TR("ui.annotation.author").data(),
                    detail.author.empty()
                        ? TR("ui.annotation.unknown_author").data()
                        : detail.author.c_str());
        ImGui::Separator();
        UI::renderMarkdown(detail.content);
        ImGui::EndTooltip();

        if ( !currentSnapshot.isPlaying &&
             currentSnapshot.acceptsInteraction ) {
            if ( !detailLinkHovered &&
                 ImGui::IsMouseClicked(ImGuiMouseButton_Left, false) ) {
                const auto& selected =
                    hoveredMarker->items[m_annotationHoverDetailIndex];
                m_annotationEditor.annotationId = selected.id;
                m_annotationEditor.author       = selected.author;
                m_annotationEditor.timestamp    = hoveredMarker->timestamp;
                m_annotationEditor.content.fill('\0');
                const auto& content = selected.content;
                std::copy_n(content.data(),
                            std::min(content.size(),
                                     m_annotationEditor.content.size() - 1U),
                            m_annotationEditor.content.data());
                m_annotationEditor.requestOpen = true;
            } else if ( !detailLinkHovered &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Right, false) ) {
                m_annotationEditor.annotationId.clear();
                m_annotationEditor.author.clear();
                m_annotationEditor.timestamp = hoveredMarker->timestamp;
                m_annotationEditor.content.fill('\0');
                m_annotationEditor.requestOpen = true;
            }
        }
    } else if ( gutterHovered && !currentSnapshot.isPlaying &&
                currentSnapshot.acceptsInteraction &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right, false) ) {
        m_annotationEditor.annotationId.clear();
        m_annotationEditor.author.clear();
        const double hoveredTime     = currentSnapshot.isSnapped
                                           ? currentSnapshot.snappedTime
                                           : currentSnapshot.hoveredTime;
        m_annotationEditor.timestamp = std::max(0.0, hoveredTime);
        m_annotationEditor.content.fill('\0');
        m_annotationEditor.requestOpen = true;
    } else if ( gutterHovered && !hoveredMarker ) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(TR("ui.annotation.gutter_hint").data());
        ImGui::EndTooltip();
    }

    std::string popupLabel(TR("ui.annotation.editor_title").view());
    popupLabel += "###BeatmapAnnotationEditor";
    if ( m_annotationEditor.requestOpen ) {
        ImGui::OpenPopup(popupLabel.c_str());
        m_annotationEditor.requestOpen = false;
    }
    if ( ImGui::BeginPopupModal(
             popupLabel.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize) ) {
        const bool editingExisting = !m_annotationEditor.annotationId.empty();
        const auto creator         = Config::normalizeCreatorIdentity(
            Config::AppConfig::instance().getEditorSettings().defaultCreator);
        const auto timeText =
            formatCanvasTime(m_annotationEditor.timestamp, &currentSnapshot);
        ImGui::Text(
            "%s: %s", TR("ui.annotation.timestamp").data(), timeText.c_str());
        ImGui::Text(
            "%s: %s",
            TR("ui.annotation.author").data(),
            editingExisting
                ? (m_annotationEditor.author.empty()
                       ? TR("ui.annotation.unknown_author").data()
                       : m_annotationEditor.author.c_str())
                : (creator.empty() ? TR("ui.annotation.unknown_author").data()
                                   : creator.c_str()));
        if ( !editingExisting && creator.empty() ) {
            ImGui::TextColored(ImVec4(1.0F, 0.34F, 0.25F, 1.0F),
                               "%s",
                               TR("ui.annotation.creator_required").data());
        }
        ImGui::TextDisabled("%s", TR("ui.annotation.markdown_hint").data());
        ImGui::InputTextMultiline("##BeatmapAnnotationMarkdown",
                                  m_annotationEditor.content.data(),
                                  m_annotationEditor.content.size(),
                                  ImVec2(520.0F, 220.0F));

        const bool canSave = m_annotationEditor.content.front() != '\0' &&
                             (editingExisting || !creator.empty());
        ImGui::BeginDisabled(!canSave);
        if ( ::MMM::UI::FeedbackButton(editingExisting
                                           ? TR("ui.annotation.save").data()
                                           : TR("ui.annotation.add").data()) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdUpsertBeatmapAnnotation{
                    .annotationId = m_annotationEditor.annotationId,
                    .targetKind = ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
                    .timestamp  = m_annotationEditor.timestamp,
                    .author     = creator,
                    .content    = m_annotationEditor.content.data(),
                }));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if ( editingExisting ) {
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.annotation.delete").data()) ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdRemoveBeatmapAnnotation{
                        m_annotationEditor.annotationId }));
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(TR("ui.annotation.cancel").data()) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return gutterHovered || detailCardHovered ||
           ImGui::IsPopupOpen(popupLabel.c_str());
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
    bool       isHovered = isInsideCanvas && ImGui::IsWindowHovered();
    const bool middlePanStartHovered =
        isInsideCanvas &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool middleClicked =
        middlePanStartHovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle, false);
    if ( middleClicked ) {
        m_isMiddleCanvasPanning      = true;
        m_lastMiddlePanMousePosition = { localMousePos.x, localMousePos.y };
    }
    const bool isDragging = !m_isMiddleCanvasPanning && hasValidMousePos &&
                            ImGui::IsMouseDragging(ImGuiMouseButton_Left);

    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    const auto& layout =
        visual.trackLayoutForKeyCount(currentSnapshot->trackCount);
    if ( currentSnapshot->hasBeatmap && !currentSnapshot->isPlaying &&
         targetWidth > 0.0F && targetHeight > 0.0F ) {
        const auto projection = Logic::calculateCanvasLaneProjection(
            targetWidth,
            currentSnapshot->trackCount,
            currentSnapshot->bgmTrackCount,
            layout.left,
            layout.right,
            currentSnapshot->canvasHorizontalOffsetX,
            true,
            currentSnapshot->bmsEditingEnabled,
            currentSnapshot->draftLanesEnabled);
        const float dropLeft =
            std::clamp(projection.bgmLeftX, 0.0F, targetWidth);
        const float dropRight =
            std::clamp(projection.bgmRightX, 0.0F, targetWidth);
        const float dropTop =
            std::clamp(layout.top * targetHeight, 0.0F, targetHeight);
        const float dropBottom =
            std::clamp(layout.bottom * targetHeight, 0.0F, targetHeight);
        if ( projection.valid && dropRight > dropLeft &&
             dropBottom > dropTop ) {
            const ImRect dropRect{
                { windowPos.x + dropLeft, windowPos.y + dropTop },
                { windowPos.x + dropRight, windowPos.y + dropBottom },
            };
            const ImGuiID dropTargetId =
                ImGui::GetID("##Basic2DCanvasAudioResourceDropTarget");
            if ( ImGui::BeginDragDropTargetCustom(dropRect, dropTargetId) ) {
                if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                         Common::AUDIO_RESOURCE_DRAG_PAYLOAD_TYPE);
                     payload && payload->IsDelivery() && payload->Data &&
                     payload->DataSize ==
                         static_cast<int>(
                             sizeof(Common::AudioResourceDragPayload)) ) {
                    const auto& resourcePayload =
                        *static_cast<const Common::AudioResourceDragPayload*>(
                            payload->Data);
                    const std::string audioResourceId{
                        Common::audioResourceIdView(resourcePayload),
                    };
                    if ( !audioResourceId.empty() ) {
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdCreateAudioSample{
                                    .audioResourceId =
                                        std::move(audioResourceId),
                                    .cameraId   = m_cameraId,
                                    .mouseX     = localMousePos.x,
                                    .mouseY     = localMousePos.y,
                                    .isCtrlDown = ImGui::GetIO().KeyCtrl,
                                }));
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
    }

    const auto laneProjection = Logic::calculateCanvasLaneProjection(
        targetWidth,
        currentSnapshot->trackCount,
        currentSnapshot->bgmTrackCount,
        layout.left,
        layout.right,
        currentSnapshot->canvasHorizontalOffsetX,
        true,
        currentSnapshot->bmsEditingEnabled,
        currentSnapshot->draftLanesEnabled);
    const float trackLeftX  = laneProjection.draftLeftX;
    const float trackRightX = laneProjection.bgmRightX;
    const float normY =
        targetHeight > 0.0f ? localMousePos.y / targetHeight : 0.0f;
    const bool isMouseInTrackLayout =
        isHovered && localMousePos.x >= trackLeftX &&
        localMousePos.x <= trackRightX && normY >= layout.top &&
        normY <= layout.bottom;
    const bool isLayoutEditing =
        Logic::EditorEngine::instance().getCurrentTool() ==
        Logic::EditTool::Layout;

    if ( !isLayoutEditing &&
         (m_trackLayoutDragHandle != TrackLayoutDragHandle::None ||
          m_noteScaleDragTarget.has_value() ||
          m_canvasComponentDragTarget.has_value() ||
          m_layoutConfigurationChanged) ) {
        finishLayoutEditing();
    }

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

    if ( m_isMiddleCanvasPanning ) {
        if ( middleClicked ) {
            // 中键取得当前手势所有权前先结束已存在的左/右键编辑，避免工具状态悬空。
            if ( m_leftPressStartedOnCanvas && !m_leftPressStartedObjectDrag &&
                 currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdEndMarquee{}));
            } else if ( m_leftPressStartedOnCanvas &&
                        currentSnapshot->currentTool ==
                            Logic::EditTool::Draw ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdEndBrush{ m_cameraId }));
            } else if ( m_leftPressStartedObjectDrag ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdEndDrag{ m_cameraId }));
            }
            if ( m_rightEraseActive ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdEndErase{ m_cameraId }));
            }
            if ( m_trackLayoutDragHandle != TrackLayoutDragHandle::None ||
                 m_noteScaleDragTarget.has_value() ||
                 m_canvasComponentDragTarget.has_value() ||
                 m_layoutConfigurationChanged ) {
                finishLayoutEditing();
            }

            m_leftPressStartedOnCanvas      = false;
            m_leftPressStartedInTrackLayout = false;
            m_leftPressStartedOnEntity      = false;
            m_leftPressStartedObjectDrag    = false;
            m_leftPressDragged              = false;
            m_colorStrokeEntities.clear();
            resetContinuousEditCommands();
        }

        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if ( ImGui::IsMouseDown(ImGuiMouseButton_Middle) && hasValidMousePos ) {
            const glm::vec2 currentMousePosition{
                localMousePos.x,
                localMousePos.y,
            };
            const glm::vec2 delta =
                currentMousePosition - m_lastMiddlePanMousePosition;
            m_lastMiddlePanMousePosition = currentMousePosition;
            if ( std::abs(delta.x) > 0.001F || std::abs(delta.y) > 0.001F ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdPanCanvas{
                        .cameraId       = m_cameraId,
                        .deltaX         = delta.x,
                        .deltaY         = delta.y,
                        .viewportWidth  = targetWidth,
                        .viewportHeight = targetHeight,
                        .renderScaleY   = currentSnapshot->renderScaleY,
                    }));
            }
        }

        if ( !ImGui::IsMouseDown(ImGuiMouseButton_Middle) ) {
            m_isMiddleCanvasPanning = false;
        }
        return;
    }

    if ( isLayoutEditing ) {
        if ( !m_hasLastHovered || m_lastHoveredEntity != entt::null ||
             m_lastHoveredPart != 0 || m_lastHoveredSubIndex != -1 ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetHoveredEntity{ entt::null, 0, -1 }));
            m_hasLastHovered        = true;
            m_lastHoveredEntity     = entt::null;
            m_lastHoveredObjectKind = Logic::ChartObjectKind::PlayerNote;
            m_lastHoveredPart       = 0;
            m_lastHoveredSubIndex   = -1;
        }

        m_leftPressStartedOnCanvas      = false;
        m_leftPressStartedInTrackLayout = false;
        m_leftPressStartedOnEntity      = false;
        m_leftPressStartedObjectDrag    = false;
        m_leftPressDragged              = false;
        m_colorStrokeEntities.clear();
        resetContinuousEditCommands();
        handleLayoutEditing(localMousePos.x,
                            localMousePos.y,
                            windowPos.x,
                            windowPos.y,
                            targetWidth,
                            targetHeight,
                            isHovered,
                            *currentSnapshot);
        return;
    }

    const bool annotationGutterBlocksCanvas =
        renderAnnotationGutter(*currentSnapshot,
                               windowPos.x,
                               windowPos.y,
                               targetWidth,
                               targetHeight,
                               localMousePos.x,
                               localMousePos.y,
                               isHovered);
    if ( annotationGutterBlocksCanvas ) {
        if ( !m_hasLastHovered || m_lastHoveredEntity != entt::null ||
             m_lastHoveredPart != 0 || m_lastHoveredSubIndex != -1 ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetHoveredEntity{ entt::null, 0, -1 }));
            m_hasLastHovered        = true;
            m_lastHoveredEntity     = entt::null;
            m_lastHoveredObjectKind = Logic::ChartObjectKind::PlayerNote;
            m_lastHoveredPart       = 0;
            m_lastHoveredSubIndex   = -1;
        }

        const auto completion = resolveBlockedCanvasGestureCompletion(
            currentSnapshot->currentTool,
            ImGui::IsMouseReleased(ImGuiMouseButton_Left),
            ImGui::IsMouseReleased(ImGuiMouseButton_Right),
            m_leftPressStartedOnCanvas,
            m_leftPressStartedObjectDrag,
            m_rightEraseActive);
        switch ( completion.leftEnd ) {
        case BlockedCanvasLeftGestureEnd::Marquee:
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndMarquee{}));
            break;
        case BlockedCanvasLeftGestureEnd::Brush:
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndBrush{ m_cameraId }));
            break;
        case BlockedCanvasLeftGestureEnd::ObjectDrag:
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndDrag{ m_cameraId }));
            break;
        case BlockedCanvasLeftGestureEnd::None: break;
        }
        if ( completion.endErase ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndErase{ m_cameraId }));
            m_lastEraseUpdateCommand.valid = false;
            m_rightEraseActive             = false;
        }
        if ( completion.clearLeftState ) {
            m_leftPressStartedOnCanvas      = false;
            m_leftPressStartedInTrackLayout = false;
            m_leftPressStartedOnEntity      = false;
            m_leftPressStartedObjectDrag    = false;
            m_leftPressDragged              = false;
            m_colorStrokeEntities.clear();
            m_lastMarqueeUpdateCommand.valid = false;
            m_lastBrushUpdateCommand.valid   = false;
            m_lastMoveUpdateCommand.valid    = false;
        }
        return;
    }

    const bool audioPreviewOverlayBlocksCanvas =
        renderObjectAudioPreviewControls(*currentSnapshot,
                                         windowPos.x,
                                         windowPos.y,
                                         targetWidth,
                                         targetHeight,
                                         localMousePos.x,
                                         localMousePos.y);

    // --- 交互：显示精确时间戳工具提示 ---
    if ( shouldShowCanvasHoverInspection(currentSnapshot->hasBeatmap,
                                         audioPreviewOverlayBlocksCanvas,
                                         isHovered,
                                         currentSnapshot->isHoveringCanvas,
                                         currentSnapshot->isPlaying) ) {
        if ( isMouseInTrackLayout ) {
            bool isEditTool =
                (currentSnapshot->currentTool != Logic::EditTool::Move &&
                 currentSnapshot->currentTool != Logic::EditTool::Marquee);

            if ( currentSnapshot->isSnapped || isEditTool ||
                 currentSnapshot->hoverInspect.show ) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(CANVAS_HOVER_OVERLAY_PADDING,
                                           CANVAS_HOVER_OVERLAY_PADDING));
                ImGui::PushStyleVar(
                    ImGuiStyleVar_ItemSpacing,
                    ImVec2(CANVAS_HOVER_OVERLAY_ITEM_SPACING_X,
                           CANVAS_HOVER_OVERLAY_ITEM_SPACING_Y));

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
                                if ( point.track >=
                                     currentSnapshot->trackCount ) {
                                    ImGui::TextColored(
                                        ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                        "%s %s: %d",
                                        label.data(),
                                        TR("ui.canvas.bgm_track").data(),
                                        point.track -
                                            currentSnapshot->trackCount + 1);
                                } else {
                                    ImGui::TextColored(
                                        ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                        "%s %s: %d",
                                        label.data(),
                                        TR("ui.canvas.track").data(),
                                        point.track + 1);
                                }
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
                        case Logic::HoverInspectKind::AudioSampleAnchor:
                        case Logic::HoverInspectKind::AudioSampleTrigger:
                            drawPoint("ui.canvas.hover.sample_anchor",
                                      inspect.head,
                                      inspect.showTrack);
                            drawPoint("ui.canvas.hover.sample_trigger",
                                      inspect.end,
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
                        if ( inspect.showAudioPreview ) {
                            ImGui::TextWrapped(
                                "%s: %s",
                                TR("ui.canvas.hover.audio_resource").data(),
                                inspect.audioResourceId.c_str());
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s: %.1f%%",
                                TR("ui.canvas.hover.volume").data(),
                                inspect.volume * 100.0F);
                        }
                        if ( inspect.showAudioSample ) {
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s: %+lld ms",
                                TR("ui.canvas.hover.offset").data(),
                                static_cast<long long>(inspect.offsetMs));
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

                    if ( currentSnapshot->hoveredTrack >=
                         currentSnapshot->trackCount ) {
                        ImGui::Text("%s: %d",
                                    TR("ui.canvas.bgm_track").data(),
                                    currentSnapshot->hoveredTrack -
                                        currentSnapshot->trackCount + 1);
                    } else {
                        ImGui::Text("%s: %d",
                                    TR("ui.canvas.track").data(),
                                    currentSnapshot->hoveredTrack + 1);
                    }

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

    if ( audioPreviewOverlayBlocksCanvas ) {
        return;
    }

    entt::entity           hoveredEntity = entt::null;
    Logic::ChartObjectKind hoveredObjectKind{
        Logic::ChartObjectKind::PlayerNote
    };
    uint8_t hoveredPart     = 0;
    int     hoveredSubIndex = -1;

    std::vector<HoverLayerCandidate> candidates;
    std::string                      layerSignature;
    if ( isHovered ) {
        for ( auto it = currentSnapshot->hitboxes.rbegin();
              it != currentSnapshot->hitboxes.rend();
              ++it ) {
            const auto hitbox = Logic::scaleInteractionHitbox(
                *it,
                currentSnapshot->interactionHitboxScaleX,
                currentSnapshot->interactionHitboxScaleY);
            if ( localMousePos.x >= hitbox.x &&
                 localMousePos.x <= hitbox.x + hitbox.w &&
                 localMousePos.y >= hitbox.y &&
                 localMousePos.y <= hitbox.y + hitbox.h ) {
                const bool appended = appendHoverLayerCandidate(
                    candidates,
                    { hitbox.entity,
                      hitbox.kind,
                      static_cast<std::uint8_t>(hitbox.part),
                      hitbox.subIndex });
                if ( !appended ) continue;
                layerSignature +=
                    std::to_string(static_cast<uint32_t>(
                        entt::to_integral(hitbox.entity))) +
                    ":" + std::to_string(static_cast<uint32_t>(hitbox.kind)) +
                    ":" + std::to_string(static_cast<uint32_t>(hitbox.part)) +
                    ":" + std::to_string(hitbox.subIndex) + ";";
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
        hoveredObjectKind     = candidate.kind;
        hoveredPart           = candidate.part;
        hoveredSubIndex       = candidate.subIndex;
    }

    bool shouldSendHover = !m_hasLastHovered ||
                           m_lastHoveredEntity != hoveredEntity ||
                           m_lastHoveredObjectKind != hoveredObjectKind ||
                           m_lastHoveredPart != hoveredPart ||
                           m_lastHoveredSubIndex != hoveredSubIndex;
    if ( shouldSendHover ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetHoveredEntity{ hoveredEntity,
                                        hoveredPart,
                                        hoveredSubIndex,
                                        hoveredObjectKind }));
        m_hasLastHovered        = true;
        m_lastHoveredEntity     = hoveredEntity;
        m_lastHoveredObjectKind = hoveredObjectKind;
        m_lastHoveredPart       = hoveredPart;
        m_lastHoveredSubIndex   = hoveredSubIndex;
    }

    auto processColorToolTarget = [&](Logic::EditTool tool) {
        if ( currentSnapshot->isPlaying || hoveredEntity == entt::null ||
             (hoveredObjectKind != Logic::ChartObjectKind::PlayerNote &&
              hoveredObjectKind != Logic::ChartObjectKind::DraftNote) ) {
            return;
        }
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
        m_leftPressStartedObjectDrag    = false;
        m_leftPressDragged              = false;
        m_colorStrokeEntities.clear();
        resetContinuousEditCommands();
        publishCanvasHoverSeek(*currentSnapshot);
    } else if ( leftClicked ) {
        m_leftPressStartedOnCanvas      = isHovered;
        m_leftPressStartedInTrackLayout = isMouseInTrackLayout;
        m_leftPressStartedOnEntity      = hoveredEntity != entt::null;
        m_leftPressStartedObjectDrag    = false;
        m_leftPressDragged              = false;
        m_colorStrokeEntities.clear();
        resetContinuousEditCommands();

        if ( isHovered ) {
            if ( currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
                if ( hoveredEntity != entt::null ) {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdSelectEntity{ hoveredEntity,
                                                    !ImGui::GetIO().KeyCtrl,
                                                    hoveredObjectKind }));
                    if ( !currentSnapshot->isPlaying ) {
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdStartDrag{ hoveredEntity,
                                                     m_cameraId,
                                                     ImGui::GetIO().KeyCtrl,
                                                     hoveredObjectKind }));
                        m_leftPressStartedObjectDrag = true;
                    }
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
                                                 ImGui::GetIO().KeyCtrl,
                                                 hoveredObjectKind }));
                    m_leftPressStartedObjectDrag = true;
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

        if ( m_leftPressStartedOnCanvas && !m_leftPressStartedObjectDrag &&
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
        } else if ( m_leftPressStartedObjectDrag &&
                    (currentSnapshot->currentTool == Logic::EditTool::Move ||
                     currentSnapshot->currentTool ==
                         Logic::EditTool::Marquee) ) {
            const glm::vec2 autoPanDelta =
                currentSnapshot->hasBeatmap && !currentSnapshot->isPlaying
                    ? objectDragAutoPanDelta(
                          { localMousePos.x, localMousePos.y },
                          targetWidth,
                          targetHeight,
                          ImGui::GetIO().DeltaTime,
                          std::max(0.0F,
                                   visual.previewConfig.edgeScrollSensitivity))
                    : glm::vec2{ 0.0F, 0.0F };
            const bool autoPanned = std::abs(autoPanDelta.x) > 0.001F ||
                                    std::abs(autoPanDelta.y) > 0.001F;
            if ( autoPanned ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdPanCanvas{
                        .cameraId       = m_cameraId,
                        .deltaX         = autoPanDelta.x,
                        .deltaY         = autoPanDelta.y,
                        .viewportWidth  = targetWidth,
                        .viewportHeight = targetHeight,
                        .renderScaleY   = currentSnapshot->renderScaleY,
                    }));
            }
            const bool playbackScrolled =
                currentSnapshot->hasBeatmap && currentSnapshot->isPlaying;
            const bool shouldUpdateMove =
                shouldSendContinuousEditCommand(
                    m_lastMoveUpdateCommand,
                    { localMousePos.x, localMousePos.y },
                    *currentSnapshot,
                    ImGui::GetIO().KeyCtrl,
                    false) ||
                autoPanned || playbackScrolled;
            if ( shouldUpdateMove ) {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdUpdateDrag{ m_cameraId,
                                          localMousePos.x,
                                          localMousePos.y,
                                          ImGui::GetIO().KeyCtrl }));
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

        if ( m_leftPressStartedOnCanvas && !m_leftPressStartedObjectDrag &&
             currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndMarquee{}));
        } else if ( m_leftPressStartedOnCanvas &&
                    currentSnapshot->currentTool == Logic::EditTool::Draw ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndBrush{ m_cameraId }));
        } else if ( m_leftPressStartedObjectDrag ) {
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
        m_leftPressStartedObjectDrag    = false;
        m_leftPressDragged              = false;
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
