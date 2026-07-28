#include "logic/session/InteractionController.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/DrawTool.h"
#include "logic/session/tool/GrabTool.h"
#include "logic/session/tool/MarqueeTool.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MMM::Logic
{

namespace
{
/// @brief 屏幕空间选择矩形。
struct SelectionRect {
    /// @brief 左边界。
    float left{ 0.0f };

    /// @brief 上边界。
    float top{ 0.0f };

    /// @brief 右边界。
    float right{ 0.0f };

    /// @brief 下边界。
    float bottom{ 0.0f };

    /// @brief 当前矩形是否有效。
    bool valid{ false };
};

/// @brief 框选屏幕空间投影上下文。
struct SelectionScreenContext {
    /// @brief 当前框选所在视口的 ScrollCache。
    const System::ScrollCache* cache{ nullptr };

    /// @brief 当前视口图集 UV 表。
    const std::unordered_map<uint32_t, glm::vec4>* uvMap{ nullptr };

    /// @brief 当前视口判定线 Y 坐标。
    float judgmentLineY{ 0.0f };

    /// @brief 轨道区左边界。
    float leftX{ 0.0f };

    /// @brief 单轨宽度。
    float singleTrackW{ 0.0f };

    /// @brief 当前视口纵向渲染缩放。
    float renderScaleY{ 1.0f };

    /// @brief 当前视口的音符基础绘制宽度。
    float noteW{ 0.0f };

    /// @brief 当前视口的音符基础绘制高度。
    float noteH{ 0.0f };

    /// @brief 当前动画时间对应的绝对滚动坐标。
    double currentAbsY{ 0.0 };

    /// @brief 当前上下文是否有效。
    bool valid{ false };
};

/// @brief 已准备好的框选区域，缓存其屏幕矩形与投影上下文。
struct PreparedMarqueeBox {
    /// @brief 原始框选数据。
    MarqueeBox box;

    /// @brief 框选区域屏幕矩形。
    SelectionRect rect;

    /// @brief 该框选区域所属视口的投影上下文。
    SelectionScreenContext screen;
};

/// @brief 框选候选实体时间范围的保守扩展，覆盖音符纹理高度和特殊 SV 误差。
constexpr double MARQUEE_CANDIDATE_TIME_PADDING_SECONDS = 2.0;

/// @brief 获取指定谱面物件领域对应的 ECS 注册表。
/// @param ctx 会话上下文。
/// @param kind 谱面物件领域。
/// @return 对应的独立注册表。
entt::registry& registryForObjectKind(SessionContext& ctx, ChartObjectKind kind)
{
    return kind == ChartObjectKind::AudioSample ? ctx.sampleRegistry
                                                : ctx.noteRegistry;
}

/// @brief 清空实体选中状态和框选运行状态。
void clearSelection(SessionContext& ctx)
{
    ctx.isSelecting             = false;
    ctx.hasMarqueeSelection     = false;
    ctx.marqueeIsAdditive       = false;
    ctx.isMarqueeSelectionDirty = false;
    ctx.marqueeBoxes.clear();

    auto view = ctx.noteRegistry.view<InteractionComponent>();
    for ( auto entity : view ) {
        ctx.noteRegistry.get<InteractionComponent>(entity).isSelected = false;
    }
    auto sampleView = ctx.sampleRegistry.view<InteractionComponent>();
    for ( auto entity : sampleView ) {
        ctx.sampleRegistry.get<InteractionComponent>(entity).isSelected = false;
    }
}

/// @brief 停止使用框选框驱动选择状态，但保留当前实体选中集合。
void detachMarqueeSelection(SessionContext& ctx)
{
    ctx.isSelecting             = false;
    ctx.hasMarqueeSelection     = false;
    ctx.marqueeIsAdditive       = false;
    ctx.isMarqueeSelectionDirty = false;
    ctx.marqueeBoxes.clear();
}

/// @brief 清空已经存在的实体选中标记。
/// @param ctx 会话上下文。
/// @warning 逻辑热路径：框选重算时只遍历已创建 InteractionComponent 的实体，
/// 避免完整扫描 NoteComponent。
void clearSelectedEntityFlags(SessionContext& ctx)
{
    auto view = ctx.noteRegistry.view<InteractionComponent>();
    for ( auto entity : view ) {
        ctx.noteRegistry.get<InteractionComponent>(entity).isSelected = false;
    }
    auto sampleView = ctx.sampleRegistry.view<InteractionComponent>();
    for ( auto entity : sampleView ) {
        ctx.sampleRegistry.get<InteractionComponent>(entity).isSelected = false;
    }
}

/// @brief 判断屏幕矩形是否与另一个矩形相交。
/// @warning 逻辑热路径：框选重算时按可见框数量调用，只做常量比较。
bool rectIntersects(SelectionRect lhs, SelectionRect rhs)
{
    if ( !lhs.valid || !rhs.valid ) return false;
    constexpr float EPS = 0.5f;
    return std::max(lhs.left, rhs.left) <=
               std::min(lhs.right, rhs.right) + EPS &&
           std::max(lhs.top, rhs.top) <= std::min(lhs.bottom, rhs.bottom) + EPS;
}

/// @brief 判断一个屏幕矩形是否完整包含另一个矩形。
/// @warning 逻辑热路径：框选重算时按可见框数量调用，只做常量比较。
bool rectContains(SelectionRect outer, SelectionRect inner)
{
    if ( !outer.valid || !inner.valid ) return false;
    constexpr float EPS = 0.5f;
    return inner.left >= outer.left - EPS && inner.right <= outer.right + EPS &&
           inner.top >= outer.top - EPS && inner.bottom <= outer.bottom + EPS;
}

/// @brief 构造一个屏幕矩形。
/// @warning 逻辑热路径：只做边界归一化和尺寸合法性检查。
SelectionRect makeRect(float left, float top, float right, float bottom)
{
    SelectionRect rect;
    rect.left   = std::min(left, right);
    rect.right  = std::max(left, right);
    rect.top    = std::min(top, bottom);
    rect.bottom = std::max(top, bottom);
    rect.valid  = rect.right > rect.left && rect.bottom > rect.top;
    return rect;
}

/// @brief 将一个矩形合并进目标矩形。
/// @warning 逻辑热路径：包围盒生成时调用，只做常量比较。
void includeRect(SelectionRect& target, SelectionRect source)
{
    if ( !source.valid ) return;
    if ( !target.valid ) {
        target = source;
        return;
    }

    target.left   = std::min(target.left, source.left);
    target.top    = std::min(target.top, source.top);
    target.right  = std::max(target.right, source.right);
    target.bottom = std::max(target.bottom, source.bottom);
}

/// @brief 按当前框选模式判断两个屏幕矩形是否命中。
/// @warning 逻辑热路径：框选重算时按候选包围盒调用，只做常量比较。
bool selectionMatchesRect(SelectionRect selection, SelectionRect candidate,
                          Config::SelectionMode mode)
{
    return mode == Config::SelectionMode::Strict
               ? rectContains(selection, candidate)
               : rectIntersects(selection, candidate);
}

/// @brief 获取图集纹理宽高比。
/// @warning 逻辑热路径：框选重算时只读取当前视口已缓存 UV 表。
float getTextureAspect(const std::unordered_map<uint32_t, glm::vec4>& uvMap,
                       TextureID id, float fallback)
{
    auto it = uvMap.find(static_cast<uint32_t>(id));
    if ( it == uvMap.end() || std::abs(it->second.w) < 1e-6f ) {
        return fallback;
    }
    return it->second.z / it->second.w;
}

/// @brief 按渲染系统规则计算纹理实际绘制尺寸。
/// @warning 逻辑热路径：框选重算时只读取当前视口已缓存 UV 表。
glm::vec2 getTextureDrawSize(
    const std::unordered_map<uint32_t, glm::vec4>& uvMap, TextureID id,
    float baseW, float baseH)
{
    auto itBase = uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == uvMap.end() ) return { baseW, baseH };

    auto it = uvMap.find(static_cast<uint32_t>(id));
    if ( it == uvMap.end() ) return { baseW, baseH };

    return { baseW * (it->second.z / itBase->second.z),
             baseH * (it->second.w / itBase->second.w) };
}

/// @brief 计算指定视口的框选纵向缩放。
/// @warning 逻辑热路径：框选拖拽时调用，只读取当前缓存的视口尺寸和配置。
float calculateMarqueeRenderScaleY(const SessionContext& ctx,
                                   const CameraInfo&     camera,
                                   const std::string&    cameraId)
{
    if ( cameraId != "Preview" ) {
        return 1.0f;
    }

    const auto* mainCamera = SessionUtils::findMainCanvasCamera(ctx.cameras);
    const float mainViewportHeight =
        mainCamera ? mainCamera->viewportHeight : camera.viewportHeight;
    const float mainEffectiveH = (ctx.lastConfig.visual.trackLayout.bottom -
                                  ctx.lastConfig.visual.trackLayout.top) *
                                 mainViewportHeight;
    const float ty             = ctx.lastConfig.visual.previewConfig.margin.top;
    const float by = camera.viewportHeight -
                     ctx.lastConfig.visual.previewConfig.margin.bottom;
    const float previewDrawH = by - ty;

    if ( std::abs(mainEffectiveH) < 1e-6f ||
         std::abs(ctx.lastConfig.visual.previewConfig.areaRatio) < 1e-6f ) {
        return 1.0f;
    }
    return previewDrawH /
           (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
}

/// @brief 计算指定视口的轨道横向布局。
/// @warning 逻辑热路径：框选重算时调用，只读取当前缓存的视口尺寸和配置。
bool calculateMarqueeTrackLayout(const SessionContext& ctx,
                                 const CameraInfo&     camera,
                                 const std::string& cameraId, float& leftX,
                                 float& rightX)
{
    if ( ctx.trackCount <= 0 ) return false;

    if ( cameraId == "Preview" ) {
        leftX  = ctx.lastConfig.visual.previewConfig.margin.left;
        rightX = camera.viewportWidth -
                 ctx.lastConfig.visual.previewConfig.margin.right;
    } else {
        const auto projection = calculatePlayerTrackProjection(
            camera.viewportWidth,
            ctx.trackCount,
            ctx.lastConfig.visual.trackLayout.left,
            ctx.lastConfig.visual.trackLayout.right,
            camera.horizontalOffsetX);
        leftX  = projection.leftX;
        rightX = projection.rightX;
    }

    return rightX > leftX + 1.0f;
}

/// @brief 为框选判断准备屏幕投影上下文。
/// @warning 逻辑热路径：框选区域变化时调用；读取一次图集 UV
/// 缓存，不做文件访问。
SelectionScreenContext makeSelectionScreenContext(
    const SessionContext& ctx, const std::string& cameraId,
    const System::ScrollCache* cache)
{
    SelectionScreenContext screen;
    if ( !cache ) return screen;

    auto cameraIt = ctx.cameras.find(cameraId);
    if ( cameraIt == ctx.cameras.end() ) return screen;

    float leftX  = 0.0f;
    float rightX = 0.0f;
    if ( !calculateMarqueeTrackLayout(
             ctx, cameraIt->second, cameraId, leftX, rightX) ) {
        return screen;
    }

    const auto& uvMap      = EditorEngine::instance().getAtlasUVMap(cameraId);
    const float baseAspect = getTextureAspect(uvMap, TextureID::Note, 1.0f);
    const float singleTrackW =
        (rightX - leftX) / static_cast<float>(ctx.trackCount);
    if ( singleTrackW <= 0.0f || std::abs(baseAspect) < 1e-6f ) {
        return screen;
    }

    screen.cache = cache;
    screen.uvMap = &uvMap;
    screen.judgmentLineY =
        cameraIt->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    screen.leftX        = leftX;
    screen.singleTrackW = singleTrackW;
    screen.renderScaleY =
        calculateMarqueeRenderScaleY(ctx, cameraIt->second, cameraId);
    screen.noteW = singleTrackW * ctx.lastConfig.visual.noteScaleX;
    screen.noteH =
        (singleTrackW / baseAspect) * ctx.lastConfig.visual.noteScaleY;
    screen.currentAbsY = cache->getAbsY(ctx.animateTime);
    screen.valid       = screen.noteW > 0.0f && screen.noteH > 0.0f &&
                         std::abs(screen.renderScaleY) > 1e-6f;
    return screen;
}

/// @brief 将逻辑时间投影到屏幕 Y 坐标。
/// @warning 逻辑热路径：框选重算时只通过 ScrollCache 做一次坐标查询。
float timeToScreenY(const SelectionScreenContext& screen, double time,
                    double anchorTime)
{
    return screen.judgmentLineY -
           static_cast<float>(screen.cache->getDisplayDelta(
               time, screen.currentAbsY, anchorTime)) *
               screen.renderScaleY;
}

/// @brief 获取可交互主体末端时间。
/// @warning 逻辑热路径：框选重算时按候选物件调用；保持纯计算，不得分配。
double carrierEndTime(::MMM::NoteType type, double timestamp, double duration)
{
    if ( type == ::MMM::NoteType::HOLD ) {
        return timestamp + duration;
    }
    return timestamp;
}

/// @brief 获取可交互主体末端 HS 锚点时间。
/// @warning 逻辑热路径：框选重算时按候选物件调用；保持纯计算，不得访问 ECS。
double carrierEndAnchorTime(const SelectionScreenContext& screen,
                            ::MMM::NoteType type, double timestamp,
                            double duration)
{
    (void)screen;
    if ( type == ::MMM::NoteType::HOLD ) {
        return timestamp;
    }
    return carrierEndTime(type, timestamp, duration);
}

/// @brief 计算框选区域的屏幕矩形。
/// @warning 逻辑热路径：框选重算时只做两次时间投影和边界归一化。
SelectionRect makeMarqueeScreenRect(const MarqueeBox&             box,
                                    const SelectionScreenContext& screen)
{
    if ( !screen.valid ) return {};

    const float  x1 = screen.leftX + box.startTrack * screen.singleTrackW;
    const float  x2 = screen.leftX + box.endTrack * screen.singleTrackW;
    const double startAbsY = screen.cache->getAbsY(box.startTime);
    const double endAbsY   = screen.cache->getAbsY(box.endTime);
    const float  y1 = screen.judgmentLineY -
                      static_cast<float>(startAbsY - screen.currentAbsY) *
                          screen.renderScaleY;
    const float  y2 =
        screen.judgmentLineY -
        static_cast<float>(endAbsY - screen.currentAbsY) * screen.renderScaleY;
    return makeRect(x1, y1, x2, y2);
}

/// @brief 计算某个轨道时间点上的纹理矩形。
/// @warning 逻辑热路径：框选重算时只做坐标换算和 UV 尺寸读取。
SelectionRect makeTextureRect(const SelectionScreenContext& screen,
                              TextureID id, float track, double time,
                              double anchorTime)
{
    if ( !screen.valid || !screen.uvMap ) return {};

    const glm::vec2 size =
        getTextureDrawSize(*screen.uvMap, id, screen.noteW, screen.noteH);
    const float x = screen.leftX + track * screen.singleTrackW +
                    (screen.singleTrackW - size.x) * 0.5f;
    const float y = timeToScreenY(screen, time, anchorTime);
    return makeRect(x, y - size.y * 0.5f, x + size.x, y + size.y * 0.5f);
}

/// @brief 将 Hold/Flick 的主体矩形合并到屏幕包围盒。
/// @warning 逻辑热路径：框选重算时只根据单个物件局部参数计算。
void includeCarrierRect(SelectionRect&                target,
                        const SelectionScreenContext& screen,
                        ::MMM::NoteType type, double timestamp, double duration,
                        int trackIndex, int dtrack)
{
    if ( !screen.valid || !screen.uvMap ) return;

    if ( type == ::MMM::NoteType::HOLD && duration > 0.0 ) {
        const glm::vec2 bodySize =
            getTextureDrawSize(*screen.uvMap,
                               TextureID::HoldBodyVertical,
                               screen.noteW,
                               screen.noteH);
        const float x  = screen.leftX +
                         static_cast<float>(trackIndex) * screen.singleTrackW +
                         (screen.singleTrackW - bodySize.x) * 0.5f;
        const float sy = timeToScreenY(screen, timestamp, timestamp);
        const float ey = timeToScreenY(
            screen,
            carrierEndTime(type, timestamp, duration),
            carrierEndAnchorTime(screen, type, timestamp, duration));
        includeRect(target, makeRect(x, sy, x + bodySize.x, ey));
    } else if ( type == ::MMM::NoteType::FLICK && dtrack != 0 ) {
        const glm::vec2 bodySize =
            getTextureDrawSize(*screen.uvMap,
                               TextureID::HoldBodyHorizontal,
                               screen.noteW,
                               screen.noteH);
        const float startTrack =
            std::min(static_cast<float>(trackIndex),
                     static_cast<float>(trackIndex + dtrack));
        const float x = screen.leftX + startTrack * screen.singleTrackW +
                        screen.singleTrackW * 0.5f;
        const float y = timeToScreenY(screen, timestamp, timestamp);
        includeRect(target,
                    makeRect(x,
                             y - bodySize.y * 0.5f,
                             x + std::abs(dtrack) * screen.singleTrackW,
                             y + bodySize.y * 0.5f));
    }
}

/// @brief 合并 Polyline 相邻子物件之间的连接段屏幕包围盒。
/// @warning 逻辑热路径：框选重算时只处理当前 Polyline 的相邻子物件。
void includePolylineTransitionRect(SelectionRect&                target,
                                   const SelectionScreenContext& screen,
                                   const NoteComponent::SubNote& current,
                                   const NoteComponent::SubNote& next)
{
    if ( !screen.valid || !screen.uvMap ) return;

    const glm::vec2 bodySize = getTextureDrawSize(
        *screen.uvMap, TextureID::HoldBodyVertical, screen.noteW, screen.noteH);
    const float currentEndTrack = static_cast<float>(current.trackIndex) +
                                  (current.type == ::MMM::NoteType::FLICK
                                       ? static_cast<float>(current.dtrack)
                                       : 0.0f);
    const float currentX        = screen.leftX +
                                  currentEndTrack * screen.singleTrackW +
                                  (screen.singleTrackW - bodySize.x) * 0.5f;
    const float nextX =
        screen.leftX +
        static_cast<float>(next.trackIndex) * screen.singleTrackW +
        (screen.singleTrackW - bodySize.x) * 0.5f;
    const double currentEndTime =
        carrierEndTime(current.type, current.timestamp, current.duration);
    const double currentEndAnchorTime = carrierEndAnchorTime(
        screen, current.type, current.timestamp, current.duration);
    const float sy =
        timeToScreenY(screen, currentEndTime, currentEndAnchorTime);
    const float ey = timeToScreenY(screen, next.timestamp, next.timestamp);
    includeRect(target,
                makeRect(std::min(currentX, nextX),
                         std::min(sy, ey),
                         std::max(currentX, nextX) + bodySize.x,
                         std::max(sy, ey)));
}

/// @brief 计算普通物件的屏幕选择包围盒。
/// @warning 逻辑热路径：框选重算时按候选物件调用，只做局部几何计算。
SelectionRect makeNoteScreenRect(const NoteComponent&          note,
                                 const SelectionScreenContext& screen)
{
    SelectionRect rect;
    if ( !screen.valid ) return rect;

    if ( note.m_type == ::MMM::NoteType::NOTE ) {
        includeRect(rect,
                    makeTextureRect(screen,
                                    TextureID::Note,
                                    static_cast<float>(note.m_trackIndex),
                                    note.m_timestamp,
                                    note.m_timestamp));
        return rect;
    }

    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        includeRect(rect,
                    makeTextureRect(screen,
                                    TextureID::Note,
                                    static_cast<float>(note.m_trackIndex),
                                    note.m_timestamp,
                                    note.m_timestamp));
        includeCarrierRect(rect,
                           screen,
                           note.m_type,
                           note.m_timestamp,
                           note.m_duration,
                           note.m_trackIndex,
                           note.m_dtrack);
        includeRect(
            rect,
            makeTextureRect(
                screen,
                TextureID::HoldEnd,
                static_cast<float>(note.m_trackIndex),
                note.m_timestamp + note.m_duration,
                carrierEndAnchorTime(
                    screen, note.m_type, note.m_timestamp, note.m_duration)));
        return rect;
    }

    if ( note.m_type == ::MMM::NoteType::FLICK ) {
        includeRect(rect,
                    makeTextureRect(screen,
                                    TextureID::Note,
                                    static_cast<float>(note.m_trackIndex),
                                    note.m_timestamp,
                                    note.m_timestamp));
        includeCarrierRect(rect,
                           screen,
                           note.m_type,
                           note.m_timestamp,
                           note.m_duration,
                           note.m_trackIndex,
                           note.m_dtrack);
        if ( note.m_dtrack != 0 ) {
            const TextureID arrowId = note.m_dtrack < 0
                                          ? TextureID::FlickArrowLeft
                                          : TextureID::FlickArrowRight;
            includeRect(rect,
                        makeTextureRect(screen,
                                        arrowId,
                                        static_cast<float>(note.m_trackIndex +
                                                           note.m_dtrack),
                                        note.m_timestamp,
                                        note.m_timestamp));
        }
    }
    return rect;
}

/// @brief 判断 Polyline 是否与框选区域命中。
/// @warning 逻辑热路径：框选重算时只遍历当前 Polyline 的子物件列表。
bool polylineMatchesSelection(const NoteComponent&          note,
                              const SelectionScreenContext& screen,
                              SelectionRect                 selection,
                              Config::SelectionMode         mode)
{
    if ( !screen.valid || note.m_subNotes.empty() ) return false;

    for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
        const auto& sub = note.m_subNotes[i];
        if ( selectionMatchesRect(
                 selection,
                 makeTextureRect(screen,
                                 i == 0 ? TextureID::Note : TextureID::Node,
                                 static_cast<float>(sub.trackIndex),
                                 sub.timestamp,
                                 sub.timestamp),
                 mode) ) {
            return true;
        }

        SelectionRect carrierRect;
        includeCarrierRect(carrierRect,
                           screen,
                           sub.type,
                           sub.timestamp,
                           sub.duration,
                           sub.trackIndex,
                           sub.dtrack);
        if ( selectionMatchesRect(selection, carrierRect, mode) ) {
            return true;
        }

        if ( i + 1 < note.m_subNotes.size() ) {
            SelectionRect transitionRect;
            includePolylineTransitionRect(
                transitionRect, screen, sub, note.m_subNotes[i + 1]);
            if ( selectionMatchesRect(selection, transitionRect, mode) ) {
                return true;
            }
        }
    }

    const auto& last = note.m_subNotes.back();
    if ( last.type == ::MMM::NoteType::FLICK && last.dtrack != 0 ) {
        const TextureID arrowId = last.dtrack < 0 ? TextureID::FlickArrowLeft
                                                  : TextureID::FlickArrowRight;
        return selectionMatchesRect(
            selection,
            makeTextureRect(screen,
                            arrowId,
                            static_cast<float>(last.trackIndex + last.dtrack),
                            last.timestamp,
                            last.timestamp),
            mode);
    } else if ( last.type == ::MMM::NoteType::HOLD ) {
        return selectionMatchesRect(
            selection,
            makeTextureRect(
                screen,
                TextureID::HoldEnd,
                static_cast<float>(last.trackIndex),
                last.timestamp + last.duration,
                carrierEndAnchorTime(
                    screen, last.type, last.timestamp, last.duration)),
            mode);
    }

    return false;
}

/// @brief 根据物件类型判断是否与框选区域命中。
/// @warning 逻辑热路径：框选重算时按候选物件调用，不做 ECS 查询。
bool noteMatchesSelection(const NoteComponent&          note,
                          const SelectionScreenContext& screen,
                          SelectionRect selection, Config::SelectionMode mode)
{
    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        return polylineMatchesSelection(note, screen, selection, mode);
    }
    return selectionMatchesRect(
        selection, makeNoteScreenRect(note, screen), mode);
}

/// @brief 获取实体的主时间戳，失效实体排序到末尾。
/// @warning 逻辑热路径：框选候选二分时调用，只做 registry 有效性检查。
double getNoteStartTimeForSelection(const SessionContext& ctx,
                                    entt::entity          entity)
{
    if ( !ctx.noteRegistry.valid(entity) ||
         !ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
        return std::numeric_limits<double>::infinity();
    }
    return ctx.noteRegistry.get<const NoteComponent>(entity).m_timestamp;
}

/// @brief 收集所有主音符实体作为候选。
/// @warning 逻辑热路径兜底：只在排序缓存不可用时完整扫描 NoteComponent。
void collectAllPrimaryNoteCandidates(SessionContext&            ctx,
                                     std::vector<entt::entity>& candidates)
{
    auto view = ctx.noteRegistry.view<NoteComponent>();
    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);
        if ( !note.m_isSubNote ) {
            candidates.push_back(entity);
        }
    }
}

/// @brief 根据单个框选框的时间范围收集排序缓存中的候选实体。
/// @warning 逻辑热路径：框选更新时按框数量执行二分和局部线性扫描。
bool collectMarqueeBoxCandidates(SessionContext&                   ctx,
                                 const PreparedMarqueeBox&         box,
                                 std::vector<entt::entity>&        candidates,
                                 std::unordered_set<entt::entity>& seen)
{
    const auto& entities     = ctx.sortedNoteEntities;
    const auto& maxEndPrefix = ctx.sortedNoteMaxEndPrefix;
    if ( entities.empty() || maxEndPrefix.size() != entities.size() ||
         !box.screen.valid || !box.screen.cache || !box.rect.valid ) {
        return false;
    }

    auto collectTimeRangeCandidates = [&](double minTime, double maxTime) {
        minTime -= MARQUEE_CANDIDATE_TIME_PADDING_SECONDS;
        maxTime += MARQUEE_CANDIDATE_TIME_PADDING_SECONDS;
        if ( !std::isfinite(minTime) || !std::isfinite(maxTime) ||
             minTime > maxTime ) {
            return;
        }

        auto startIt =
            std::lower_bound(maxEndPrefix.begin(), maxEndPrefix.end(), minTime);
        std::size_t startIndex = static_cast<std::size_t>(
            std::distance(maxEndPrefix.begin(), startIt));
        if ( startIndex >= entities.size() ) {
            return;
        }

        auto endIt = std::upper_bound(
            entities.begin() + static_cast<std::ptrdiff_t>(startIndex),
            entities.end(),
            maxTime,
            [&ctx](double value, entt::entity entity) {
                return value < getNoteStartTimeForSelection(ctx, entity);
            });

        candidates.reserve(
            candidates.size() +
            static_cast<std::size_t>(std::distance(
                entities.begin() + static_cast<std::ptrdiff_t>(startIndex),
                endIt)));
        for ( auto it =
                  entities.begin() + static_cast<std::ptrdiff_t>(startIndex);
              it != endIt;
              ++it ) {
            const auto entity = *it;
            if ( !seen.insert(entity).second ) {
                continue;
            }
            if ( !ctx.noteRegistry.valid(entity) ||
                 !ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                continue;
            }

            const auto& note =
                ctx.noteRegistry.get<const NoteComponent>(entity);
            if ( note.m_isSubNote ) continue;
            candidates.push_back(entity);
        }
    };

    const double paddedTopY    = box.rect.top - box.screen.noteH;
    const double paddedBottomY = box.rect.bottom + box.screen.noteH;
    const double absA   = box.screen.currentAbsY +
                          (box.screen.judgmentLineY - paddedTopY) /
                              static_cast<double>(box.screen.renderScaleY);
    const double absB   = box.screen.currentAbsY +
                          (box.screen.judgmentLineY - paddedBottomY) /
                              static_cast<double>(box.screen.renderScaleY);
    auto         ranges = box.screen.cache->getTimeRangesForAbsYWindow(
        std::min(absA, absB), std::max(absA, absB));
    if ( ranges.empty() ) {
        collectTimeRangeCandidates(
            std::min(box.box.startTime, box.box.endTime),
            std::max(box.box.startTime, box.box.endTime));
        return true;
    }

    for ( const auto& [minTime, maxTime] : ranges ) {
        collectTimeRangeCandidates(minTime, maxTime);
    }
    return true;
}

/// @brief 收集当前框选框影响到的候选主音符实体。
/// @warning 逻辑热路径：优先使用已排序的时间段缓存；缓存不可用时才全量兜底。
void collectMarqueeSelectionCandidates(
    SessionContext& ctx, const std::vector<PreparedMarqueeBox>& boxes,
    std::vector<entt::entity>& candidates)
{
    std::unordered_set<entt::entity> seen;
    seen.reserve(256);

    bool usedIndexedCandidates = true;
    for ( const auto& box : boxes ) {
        if ( !collectMarqueeBoxCandidates(ctx, box, candidates, seen) ) {
            usedIndexedCandidates = false;
            break;
        }
    }

    if ( usedIndexedCandidates ) {
        return;
    }

    candidates.clear();
    collectAllPrimaryNoteCandidates(ctx, candidates);
}

/// @brief 准备所有有效框选区域的屏幕矩形。
/// @warning 逻辑热路径：框选区域变化时调用；按框数量读取视口和图集缓存。
std::vector<PreparedMarqueeBox> prepareMarqueeBoxes(
    const SessionContext& ctx, const System::ScrollCache* cache)
{
    std::vector<PreparedMarqueeBox> boxes;
    boxes.reserve(ctx.marqueeBoxes.size());
    for ( const auto& box : ctx.marqueeBoxes ) {
        PreparedMarqueeBox prepared;
        prepared.box    = box;
        prepared.screen = makeSelectionScreenContext(ctx, box.cameraId, cache);
        prepared.rect   = makeMarqueeScreenRect(box, prepared.screen);
        if ( prepared.rect.valid ) {
            boxes.push_back(std::move(prepared));
        }
    }
    return boxes;
}
}  // namespace

InteractionController::InteractionController(SessionContext& ctx) : m_ctx(ctx)
{
    m_tools[EditTool::Move]    = std::make_unique<GrabTool>();
    m_tools[EditTool::Marquee] = std::make_unique<MarqueeTool>();
    m_tools[EditTool::Draw]    = std::make_unique<DrawTool>();
}

// --- 交互命令处理 ---

void InteractionController::handleCommand(const CmdSetHoveredEntity& cmd)
{
    if ( (m_ctx.hoveredEntity != cmd.entity ||
          m_ctx.hoveredObjectKind != cmd.kind) &&
         m_ctx.hoveredEntity != entt::null ) {
        auto& previousRegistry =
            registryForObjectKind(m_ctx, m_ctx.hoveredObjectKind);
        if ( previousRegistry.valid(m_ctx.hoveredEntity) &&
             previousRegistry.all_of<InteractionComponent>(
                 m_ctx.hoveredEntity) ) {
            previousRegistry.get<InteractionComponent>(m_ctx.hoveredEntity)
                .isHovered = false;
            previousRegistry.get<InteractionComponent>(m_ctx.hoveredEntity)
                .hoveredPart = static_cast<uint8_t>(HoverPart::None);
        }
    }

    m_ctx.hoveredEntity = cmd.entity;
    m_ctx.hoveredObjectKind =
        cmd.entity == entt::null ? ChartObjectKind::PlayerNote : cmd.kind;
    m_ctx.hoveredPart     = cmd.part;
    m_ctx.hoveredSubIndex = cmd.subIndex;

    auto& registry = registryForObjectKind(m_ctx, m_ctx.hoveredObjectKind);
    if ( m_ctx.hoveredEntity != entt::null &&
         registry.valid(m_ctx.hoveredEntity) ) {
        if ( !registry.all_of<InteractionComponent>(m_ctx.hoveredEntity) ) {
            registry.emplace<InteractionComponent>(m_ctx.hoveredEntity);
        }
        auto& ic     = registry.get<InteractionComponent>(m_ctx.hoveredEntity);
        ic.isHovered = true;
        ic.hoveredPart     = cmd.part;
        ic.hoveredSubIndex = cmd.subIndex;
    }
}

void InteractionController::handleCommand(const CmdSelectEntity& cmd)
{
    if ( cmd.entity == entt::null ) {
        if ( cmd.clearOthers ) {
            clearSelection(m_ctx);
        }
        return;
    }

    // 只有在框选工具模式下才允许通过点击实体修改选中状态。
    if ( m_ctx.currentTool != EditTool::Marquee ) return;

    auto& registry = registryForObjectKind(m_ctx, cmd.kind);
    if ( !registry.valid(cmd.entity) ) return;
    if ( !registry.all_of<InteractionComponent>(cmd.entity) ) {
        registry.emplace<InteractionComponent>(cmd.entity);
    }
    auto& ic          = registry.get<InteractionComponent>(cmd.entity);
    bool  wasSelected = ic.isSelected;

    if ( !cmd.clearOthers ) {
        detachMarqueeSelection(m_ctx);
        ic.isSelected = !ic.isSelected;
        return;
    }

    if ( wasSelected ) {
        return;
    }

    clearSelection(m_ctx);
    if ( !registry.all_of<InteractionComponent>(cmd.entity) ) {
        registry.emplace<InteractionComponent>(cmd.entity);
    }
    registry.get<InteractionComponent>(cmd.entity).isSelected = true;
}

void InteractionController::handleCommand(const CmdSelectAll& cmd)
{
    detachMarqueeSelection(m_ctx);

    auto view = m_ctx.noteRegistry.view<NoteComponent>();
    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        if ( !m_ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
            m_ctx.noteRegistry.emplace<InteractionComponent>(entity);
        }
        m_ctx.noteRegistry.get<InteractionComponent>(entity).isSelected = true;
    }
    auto sampleView = m_ctx.sampleRegistry.view<SampleComponent>();
    for ( auto entity : sampleView ) {
        if ( !m_ctx.sampleRegistry.all_of<InteractionComponent>(entity) ) {
            m_ctx.sampleRegistry.emplace<InteractionComponent>(entity);
        }
        m_ctx.sampleRegistry.get<InteractionComponent>(entity).isSelected =
            true;
    }
}

void InteractionController::handleCommand(const CmdStartDrag& cmd)
{
    if ( cmd.kind == ChartObjectKind::AudioSample ) {
        return;
    }
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleStartDrag(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdUpdateDrag& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateDrag(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdEndDrag& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndDrag(m_ctx, cmd);
    }
}

/// @brief 处理视口鼠标位置、拖拽状态和边缘自动滚动速度。
/// @param cmd 鼠标位置更新指令。
/// @warning 逻辑热路径：UI 可每帧推送；只能更新交互状态和常量时间边缘滚动计算，
/// 禁止 ECS 遍历、文件系统访问和阻塞操作。
void InteractionController::handleCommand(const CmdSetMousePosition& cmd)
{
    const bool hasFiniteMouse =
        std::isfinite(cmd.mouseX) && std::isfinite(cmd.mouseY);
    const bool hasFiniteViewport =
        std::isfinite(cmd.viewportWidth) && std::isfinite(cmd.viewportHeight) &&
        cmd.viewportWidth > 0.0f && cmd.viewportHeight > 0.0f;
    const bool isInsideViewport =
        hasFiniteMouse &&
        (!hasFiniteViewport ||
         (cmd.mouseX >= 0.0f && cmd.mouseX <= cmd.viewportWidth &&
          cmd.mouseY >= 0.0f && cmd.mouseY <= cmd.viewportHeight));
    const bool isHovering = cmd.isHovering && isInsideViewport;

    bool canUpdate = false;
    if ( isHovering ) {
        if ( !m_ctx.isDragging || m_ctx.mouseCameraId == cmd.cameraId ||
             m_ctx.mouseCameraId == "" ) {
            m_ctx.mouseCameraId   = cmd.cameraId;
            m_ctx.isMouseInCanvas = true;
            canUpdate             = true;
        }
    } else if ( cmd.isDragging && hasFiniteMouse && m_ctx.isDragging &&
                m_ctx.mouseCameraId == cmd.cameraId ) {
        // 如果正在往外拖拽，依然允许更新坐标以便主画布跟随
        canUpdate = true;
    }

    if ( canUpdate ) {
        m_ctx.lastMousePos = { cmd.mouseX, cmd.mouseY };

        // 如果命令携带了直接的时间戳，优先使用它（用于音频视图等非空间映射视口）
        if ( cmd.hoverTime >= 0.0 && std::isfinite(cmd.hoverTime) ) {
            m_ctx.previewHoverTime = cmd.hoverTime;
            m_ctx.isDragging       = cmd.isDragging;
            m_ctx.dragCameraId     = cmd.cameraId;
        } else if ( cmd.cameraId == "AudioWaveform" ||
                    cmd.cameraId == "AudioSpectrum" ) {
            // 处理音频视图的释放操作
            m_ctx.isDragging = cmd.isDragging;
        } else if ( cmd.cameraId == "Preview" ) {
            // 传统的预览区交互
            m_ctx.isDragging = cmd.isDragging;
            if ( m_ctx.isDragging ) m_ctx.dragCameraId = cmd.cameraId;
        }

        // 边缘滚动逻辑
        m_ctx.previewEdgeScrollVelocity = 0.0;

        if ( cmd.isDragging && cmd.viewportWidth > 0 &&
             cmd.viewportHeight > 0 ) {
            float margin = 20.0f;
            float dist   = 0.0f;

            if ( cmd.cameraId == "Preview" ) {
                // 纵向边缘滚动 (Preview)
                if ( cmd.mouseY < margin )
                    dist = margin - cmd.mouseY;
                else if ( cmd.mouseY > cmd.viewportHeight - margin )
                    dist = (cmd.viewportHeight - margin) - cmd.mouseY;
            } else if ( cmd.cameraId == "AudioWaveform" ||
                        cmd.cameraId == "AudioSpectrum" ) {
                // 横向边缘滚动 (Audio Views)
                if ( cmd.mouseX < margin )
                    dist = cmd.mouseX - margin;
                else if ( cmd.mouseX > cmd.viewportWidth - margin )
                    dist = cmd.mouseX - (cmd.viewportWidth - margin);
            }

            if ( std::abs(dist) > 0.001f ) {
                float sensitivity =
                    m_ctx.lastConfig.visual.previewConfig.edgeScrollSensitivity;
                m_ctx.previewEdgeScrollVelocity =
                    static_cast<double>(dist) * sensitivity;
            }
        }
    } else if ( cmd.cameraId == m_ctx.mouseCameraId ||
                cmd.cameraId == m_ctx.dragCameraId ) {
        m_ctx.previewEdgeScrollVelocity = 0.0;
    }

    const bool commandOwnsDragState = cmd.cameraId == "Preview" ||
                                      cmd.cameraId == "AudioWaveform" ||
                                      cmd.cameraId == "AudioSpectrum";
    if ( commandOwnsDragState && !cmd.isDragging &&
         (cmd.cameraId == m_ctx.mouseCameraId ||
          cmd.cameraId == m_ctx.dragCameraId) ) {
        m_ctx.isDragging = false;
        if ( m_ctx.dragCameraId == cmd.cameraId ) {
            m_ctx.dragCameraId.clear();
        }
    }

    if ( !isHovering && !m_ctx.isDragging ) {
        if ( m_ctx.mouseCameraId == cmd.cameraId ) {
            m_ctx.mouseCameraId             = "";
            m_ctx.isMouseInCanvas           = false;
            m_ctx.previewEdgeScrollVelocity = 0.0;
        }
    }
}

void InteractionController::handleCommand(const CmdUpdateTrackCount& cmd)
{
    if ( cmd.trackCount <= 0 || cmd.trackCount == m_ctx.trackCount ) {
        return;
    }

    const auto oldTrackCount = m_ctx.trackCount;
    std::vector<TrackCountAction::SampleTrackChange> sampleChanges;
    const auto sampleView = m_ctx.sampleRegistry.view<const SampleComponent>();
    sampleChanges.reserve(sampleView.size());
    for ( auto entity : sampleView ) {
        const auto& sample = sampleView.get<const SampleComponent>(entity);
        const std::uint32_t bgmIndex =
            sample.m_track >= static_cast<std::uint32_t>(oldTrackCount)
                ? sample.m_track - static_cast<std::uint32_t>(oldTrackCount)
                : 0;
        const std::uint64_t afterTrack =
            static_cast<std::uint64_t>(cmd.trackCount) + bgmIndex;
        sampleChanges.push_back({
            .entity      = entity,
            .beforeTrack = sample.m_track,
            .afterTrack  = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                afterTrack, std::numeric_limits<std::uint32_t>::max())),
        });
    }

    auto action = std::make_unique<TrackCountAction>(
        oldTrackCount, cmd.trackCount, std::move(sampleChanges));
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
}

void InteractionController::handleCommand(const CmdChangeTool& cmd)
{
    m_ctx.currentTool = cmd.tool;
}

void InteractionController::handleCommand(const CmdSetBrushNoteColor& cmd)
{
    setNoteColorOverride(m_ctx.brushState.customColors, cmd.slot, cmd.color);
}

void InteractionController::handleCommand(const CmdSetBrushNotePalette& cmd)
{
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteColorOverride(
            m_ctx.brushState.customColors, slot, cmd.colors[i]);
    }
}

void InteractionController::handleCommand(const CmdStartMarquee& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleStartMarquee(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdUpdateMarquee& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateMarquee(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdEndMarquee& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndMarquee(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdRemoveMarqueeAt& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleRemoveMarqueeAt(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdStartBrush& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_ctx.isDragging   = true;
        m_ctx.dragCameraId = cmd.cameraId;
        m_tools[m_ctx.currentTool]->handleStartBrush(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdUpdateBrush& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateBrush(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdEndBrush& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndBrush(m_ctx, cmd);
    }
    m_ctx.isDragging = false;
}

void InteractionController::handleCommand(const CmdStartErase& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_ctx.isDragging   = true;
        m_ctx.dragCameraId = cmd.cameraId;
        m_tools[m_ctx.currentTool]->handleStartErase(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdUpdateErase& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleUpdateErase(m_ctx, cmd);
    }
}

void InteractionController::handleCommand(const CmdEndErase& cmd)
{
    if ( m_tools.count(m_ctx.currentTool) ) {
        m_tools[m_ctx.currentTool]->handleEndErase(m_ctx, cmd);
    }
    m_ctx.isDragging = false;
}

/// @brief 根据当前框选区域重新计算实体选中状态。
/// @warning 逻辑热路径：框选框变化时执行；使用已缓存的排序/视口/图集数据，
/// 禁止文件系统访问、阻塞等待或完整无关 ECS 遍历。
void InteractionController::updateMarqueeSelection(bool forceFullSync)
{
    if ( !m_ctx.isMarqueeSelectionDirty && !forceFullSync ) return;
    m_ctx.isMarqueeSelectionDirty = false;
    if ( m_ctx.marqueeBoxes.empty() ) return;

    const auto* cache =
        m_ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    const auto preparedBoxes = prepareMarqueeBoxes(m_ctx, cache);
    if ( preparedBoxes.empty() ) return;

    auto mode = m_ctx.lastConfig.settings.selectionMode;
    if ( forceFullSync || !m_ctx.marqueeIsAdditive ) {
        clearSelectedEntityFlags(m_ctx);
    }

    std::vector<entt::entity> candidates;
    collectMarqueeSelectionCandidates(m_ctx, preparedBoxes, candidates);

    for ( auto entity : candidates ) {
        if ( !m_ctx.noteRegistry.valid(entity) ||
             !m_ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
            continue;
        }

        const auto& note = m_ctx.noteRegistry.get<const NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;
        bool isSelectedInAny = false;
        for ( const auto& box : preparedBoxes ) {
            if ( noteMatchesSelection(note, box.screen, box.rect, mode) ) {
                isSelectedInAny = true;
                break;
            }
        }

        if ( !isSelectedInAny ) continue;

        if ( !m_ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
            m_ctx.noteRegistry.emplace<InteractionComponent>(entity);
        }
        m_ctx.noteRegistry.get<InteractionComponent>(entity).isSelected = true;
    }
}

}  // namespace MMM::Logic
