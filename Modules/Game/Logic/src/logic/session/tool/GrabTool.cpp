#include "logic/session/tool/GrabTool.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectResourceService.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/EditorAction.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MMM::Logic
{

namespace
{
/// @brief 折线子段拖拽结束时用于判断零长度 Hold 的容差。
constexpr double POLYLINE_SUB_DRAG_ZERO_DURATION = 1e-5;

/// @brief 带原始子实体索引的折线子段，用于清理后回写子实体。
/// @note 合并后的来源保留前段索引，不能用清理后位置代替。
/// @note sub 按值拥有属性，使删除工作列表元素不影响原始撤销快照。
struct CleanPolylineSubSegment {
    /// @brief 折线子段数据。
    NoteComponent::SubNote sub;
    /// @brief 该清理后子段优先复用的原始子实体索引。
    int sourceIndex{ -1 };
};

/// @brief 判断折线子段是否已经退化为应删除的零值段。
/// @param sub 待检测子段。
/// @return 需要清理时返回 true。
/// @note 只清理 Hold 的极短或负持续段以及 Flick 的零轨差段。
/// @note 普通 Note 即使没有时间或轨道跨度，也仍是有效节点。
bool isDegeneratePolylineSubSegment(const NoteComponent::SubNote& sub)
{
    // Hold 以秒容差判断退化，负持续值也进入清理范围。
    // Flick 没有时间持续段，应改按整数轨差判断。
    if ( sub.type == ::MMM::NoteType::HOLD ) {
        return sub.duration < POLYLINE_SUB_DRAG_ZERO_DURATION;
    }
    if ( sub.type == ::MMM::NoteType::FLICK ) {
        return sub.dtrack == 0;
    }
    return false;
}

/// @brief 判断两个相邻折线子段是否可在零值清理后合并。
/// @param lhs 前一个子段。
/// @param rhs 后一个子段。
/// @return 可合并时返回 true。
/// @note 调用方已按列表顺序选出相邻项，这里只判断类型，不重排时间。
/// @note 只有同类 Hold 或 Flick 可合并，不能跨越一个普通节点。
bool canMergeAdjacentPolylineSubSegments(const NoteComponent::SubNote& lhs,
                                         const NoteComponent::SubNote& rhs)
{
    // 不同类型保持分段，否则可能丢失时间与横向动作之间的边界。
    // 同类仅允许持续或横移主体，不合并普通点击节点。
    if ( lhs.type != rhs.type ) return false;
    return lhs.type == ::MMM::NoteType::HOLD ||
           lhs.type == ::MMM::NoteType::FLICK;
}

/// @brief 将后一个同类折线子段合并进前一个子段。
/// @param target 保留并扩展的子段。
/// @param source 被合并的子段。
/// @note 保留前段的起点和其他属性，将末端扩展到后段末端。
/// @note 调用前须确认两段同类；不会从 source 复制颜色、身份或音效绑定。
/// @pre target 与 source 是两个不同的列表元素；删除 source 由调用方完成。
void mergeAdjacentPolylineSubSegments(NoteComponent::SubNote&       target,
                                      const NoteComponent::SubNote& source)
{
    if ( target.type == ::MMM::NoteType::HOLD ) {
        // Hold 从目标原起点延伸至源段末端，不直接相加两段时长。
        // 取非负值处理末端落在目标起点之前的退化输入。
        double mergedEnd = source.timestamp + source.duration;
        target.duration  = std::max(0.0, mergedEnd - target.timestamp);
    } else if ( target.type == ::MMM::NoteType::FLICK ) {
        // Flick 合并取源段实际终轨，再相对目标起轨求轨差。
        // 不能直接相加 dtrack，因为两段记录的起轨可能不同。
        int mergedEndTrack = source.trackIndex + source.dtrack;
        target.dtrack      = mergedEndTrack - target.trackIndex;
    }
}

/// @brief 取颜色覆盖中的单个槽位。
/// @param colors 颜色覆盖集合。
/// @param slot 目标颜色槽位。
/// @return 对应槽位的颜色；未设置时为空。
std::optional<glm::vec4> getPolylineDragColorOverride(
    const NoteColorOverrides& colors, NoteColorSlot slot)
{
    return getNoteColorOverride(colors, slot);
}

/// @brief 合并子段和父折线的颜色覆盖，优先保留子段颜色。
/// @param childColors 保留下来的唯一子段颜色。
/// @param parentColors 被降级的父折线颜色。
/// @return 子段缺失槽位由父折线补齐后的颜色覆盖集合。
/// @note 子段显式覆盖优先，父折线仅补齐子段未设置的槽。
/// @note 返回值不再依赖父对象生命周期，适用于折线降级后的独立 Note。
NoteColorOverrides mergeStandalonePolylineColors(
    const NoteColorOverrides& childColors,
    const NoteColorOverrides& parentColors)
{
    // 先复制子段覆盖，后续仅填补未定义槽位。
    // 显式透明色仍是有效覆盖，不能按颜色数值判断是否缺失。
    NoteColorOverrides merged = childColors;
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<NoteColorSlot>(i);
        // has_value 区分未设置与显式颜色，保留子段设计意图。
        // 父槽同样为空时仍保持缺省，由正常皮肤规则提供颜色。
        if ( getPolylineDragColorOverride(merged, slot).has_value() ) {
            continue;
        }
        setNoteColorOverride(
            merged, slot, getPolylineDragColorOverride(parentColors, slot));
    }
    return merged;
}

/// @brief 清理折线子段中的零值段，并合并相邻同类段。
/// @param subNotes 拖拽结束后的折线子段列表。
/// @param changed 输出是否发生了清理或合并。
/// @return 清理后的子段与其复用的原始子实体索引。
/// @warning 拖动结束低频路径：允许局部列表复制和
/// erase；禁止移到每次拖动更新中。
/// @note sourceIndex 始终指向原始列表，删除和合并不重新编号。
/// @note 重复清理直到稳定，合并产生的零轨差仍需在下一轮删除。
/// @note 每次有效清理或合并都减少元素数，因此循环不会在相同列表上永久重复。
/// @note 不合并非相邻同类段，普通节点仍保留其分段作用。
std::vector<CleanPolylineSubSegment> cleanPolylineSubSegments(
    const std::vector<NoteComponent::SubNote>& subNotes, bool& changed)
{
    std::vector<CleanPolylineSubSegment> segments;
    // 创建局部工作列表并保留原下标，源子项数组不就地改变。
    // 后续提交可以据来源下标复用对应子实体，减少身份漂移。
    segments.reserve(subNotes.size());
    for ( std::size_t i = 0; i < subNotes.size(); ++i ) {
        segments.push_back({ subNotes[i], static_cast<int>(i) });
    }

    // changed 跨所有轮次累计，passChanged 仅控制是否需要下一轮。
    // 没有任何清理时返回原顺序与身份映射，调用方可以跳过结构调整。
    changed          = false;
    bool passChanged = true;
    while ( passChanged ) {
        passChanged = false;

        for ( std::size_t i = 0; i < segments.size(); ) {
            // 删除后不增加 i，下一个元素已移动到当前位置。
            // 避免连续两个退化段只删除第一个而漏掉第二个。
            if ( isDegeneratePolylineSubSegment(segments[i].sub) ) {
                segments.erase(segments.begin() +
                               static_cast<std::ptrdiff_t>(i));
                passChanged = true;
                changed     = true;
                continue;
            }
            ++i;
        }

        for ( std::size_t i = 0; i + 1 < segments.size(); ) {
            // 前一轮删除可能让两个同类段变为相邻，因此清理后再尝试合并。
            // 保留前段的来源索引，后段的索引随删除丢弃。
            if ( canMergeAdjacentPolylineSubSegments(segments[i].sub,
                                                     segments[i + 1].sub) ) {
                // 合并后停留在同一位置，继续检查扩展后的段能否吞并下一段。
                // 若相反方向 Flick 合并成零跨度，外层下一轮会再次执行退化清理。
                mergeAdjacentPolylineSubSegments(segments[i].sub,
                                                 segments[i + 1].sub);
                segments.erase(segments.begin() +
                               static_cast<std::ptrdiff_t>(i + 1));
                passChanged = true;
                changed     = true;
                continue;
            }
            ++i;
        }
    }

    return segments;
}

/// @brief 将单个折线子段转为独立音符组件。
/// @param target 被降级的父音符组件。
/// @param sub 保留下来的唯一子段。
/// @param inheritsParentSound 子段是否仍包含原折线头部。
/// @note 保留父实体，替换为唯一子项的独立物件数据。
/// @note 音效回退仅限保留下来的子项仍来自原头部，避免把父音效错误移到后段。
/// @note 保留父实体既有稳定身份，不用子项身份覆盖整个根对象。
/// @note metadata 取子项值，颜色则逐槽回退父值，两类属性继承规则不同。
/// @pre sub 来自独立清理结果；不得引用即将清空的 target.m_subNotes 存储。
void applyStandaloneSubSegment(NoteComponent&                target,
                               const NoteComponent::SubNote& sub,
                               bool inheritsParentSound)
{
    // 修改父组件之前保存它的可继承属性。
    // 结构降级会覆盖绑定和颜色，之后不能再从已改写字段取原父值。
    const NoteColorOverrides parentColors        = target.m_customColors;
    const auto               parentSampleBinding = target.m_sampleBinding;

    target.m_type       = sub.type;
    target.m_timestamp  = sub.timestamp;
    target.m_duration   = sub.duration;
    target.m_trackIndex = sub.trackIndex;
    target.m_dtrack     = sub.dtrack;
    // 降级后作为独立根物件，清除父引用和子索引。
    // 不能保留旧子身份，否则渲染和选择路径可能把它排除。
    target.m_isSubNote      = false;
    target.m_parentPolyline = entt::null;
    target.m_subIndex       = -1;
    target.m_metadata       = sub.metadata;
    target.m_sampleBinding  = sub.sampleBinding;
    // 子项已有绑定优先，父绑定只在原头部仍保留时补齐。
    // 这避免删除头段后，原父音效错误附着到另一个时间点。
    if ( !target.m_sampleBinding && inheritsParentSound ) {
        target.m_sampleBinding = parentSampleBinding;
    }
    applyNoteColorOverrides(
        target, mergeStandalonePolylineColors(sub.customColors, parentColors));
    // 独立物件不再拥有折线子项列表，清除旧结构以防后续重复同步。
    // 实体本身保留，调用方仍可用原父身份构造更新 Action。
    target.m_subNotes.clear();
}

/// @brief 将清理后的子段结果应用到父音符组件。
/// @param parentAfter 即将写入 Action 的父音符结果。
/// @param segments 清理后的子段列表。
/// @note 按零项、一项和多项选择点击退化、独立物件或继续保留折线。
/// @note 此处只修改待提交值对象，实际 ECS 与撤销记录由调用方安装。
/// @note 空结果保留父原起点，单结果改用子项位置，多结果采用第一项位置。
/// @note 所有结果都恢复根对象关系，不能将降级根留作某个旧子实体。
/// @pre segments 与 parentAfter 的子数组独立，重建父数组不会使输入失效。
void applyCleanedPolylineSubSegments(
    NoteComponent&                              parentAfter,
    const std::vector<CleanPolylineSubSegment>& segments)
{
    // 不论剩余子项数量多少，输出仍是原父实体的根组件。
    // 先统一解除子关系，再决定其最终物件类型。
    parentAfter.m_isSubNote      = false;
    parentAfter.m_parentPolyline = entt::null;
    parentAfter.m_subIndex       = -1;

    // 全部子段消失时保留原根位置为普通 Note，清除持续和横移语义。
    // 此分支不删除根实体，删除或更新历史由外层收尾决定。
    if ( segments.empty() ) {
        parentAfter.m_type     = ::MMM::NoteType::NOTE;
        parentAfter.m_duration = 0.0;
        parentAfter.m_dtrack   = 0;
        parentAfter.m_subNotes.clear();
        return;
    }

    // 单段无需继续使用折线容器，转换成该段对应的普通物件类型。
    // sourceIndex 为零才允许继承原父音效，清理后的新数组索引不能替代它。
    if ( segments.size() == 1 ) {
        applyStandaloneSubSegment(parentAfter,
                                  segments.front().sub,
                                  segments.front().sourceIndex == 0);
        return;
    }

    // 多段结果继续作为折线，父起点跟随清理后第一个子项。
    // 父层不保留单段 duration 或 dtrack，实际跨度由子列表表达。
    parentAfter.m_type       = ::MMM::NoteType::POLYLINE;
    parentAfter.m_timestamp  = segments.front().sub.timestamp;
    parentAfter.m_trackIndex = segments.front().sub.trackIndex;
    parentAfter.m_duration   = 0.0;
    parentAfter.m_dtrack     = 0;
    parentAfter.m_subNotes.clear();
    // 按清理后的顺序重建列表，不在这里重新排序或再次合并。
    // 来源索引留在外层清理结果中，供 ECS 子实体收尾使用。
    parentAfter.m_subNotes.reserve(segments.size());
    for ( const auto& segment : segments ) {
        parentAfter.m_subNotes.push_back(segment.sub);
    }
}

/// @brief 清除当前拖拽涉及实体的拖拽态。
/// @param ctx 会话上下文。
/// @param entities 拖拽开始时记录的实体集合。
/// @warning 手势结束时调用，只遍历参与集合，不完整遍历 ECS。
template<typename InitialStateMap>
void clearDraggingFlags(SessionContext& ctx, const InitialStateMap& entities)
{
    // 只遍历手势开始时记录的参与实体，不扫描整个谱面。
    // 拖动期间可能删除或替换对象，清理前检查有效性和交互组件。
    for ( const auto& [entity, state] : entities ) {
        (void)state;
        if ( ctx.noteRegistry.valid(entity) &&
             ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
            // 仅清除拖动标记，选中和悬浮状态交给各自输入流程管理。
            // 初始状态值在本函数中不恢复，恢复或提交由结束手势的事务负责。
            ctx.noteRegistry.get<InteractionComponent>(entity).isDragging =
                false;
        }
    }
}

/// @brief 清除当前拖拽涉及自动采样实体的拖拽态。
/// @param ctx 会话上下文。
/// @param entities 自动采样初始状态集合。
/// @warning 手势结束时调用，只清理本次采样集合，不复制其初始组件。
template<typename InitialStateMap>
void clearSampleDraggingFlags(SessionContext&        ctx,
                              const InitialStateMap& entities)
{
    // 只遍历手势开始时记录的参与实体，不扫描整个谱面。
    // 拖动期间可能删除或替换对象，清理前检查有效性和交互组件。
    for ( const auto& [entity, state] : entities ) {
        (void)state;
        if ( ctx.sampleRegistry.valid(entity) &&
             ctx.sampleRegistry.all_of<InteractionComponent>(entity) ) {
            // 仅清除拖动标记，选中和悬浮状态交给各自输入流程管理。
            // 初始状态值在本函数中不恢复，恢复或提交由结束手势的事务负责。
            ctx.sampleRegistry.get<InteractionComponent>(entity).isDragging =
                false;
        }
    }
}

/// @brief 查询实体当前是否被选中。
/// @warning 拖动开始低频路径：只读取单个实体的交互组件。
/// @param registry 目标对象所属注册表，不能混用音符与采样领域。
/// @param entity 要读取交互状态的实体。
/// @return 存在交互组件且其选中标志为 true 时返回 true。
bool isEntitySelected(const entt::registry& registry, entt::entity entity)
{
    const auto* interaction =
        registry.try_get<const InteractionComponent>(entity);
    return interaction && interaction->isSelected;
}

/// @brief 统一轨道拖动中的鼠标目标。
/// @note 像素几何与逻辑轨号一起返回，调用者不应混用另一相机的投影。
/// @note 对象只在当前输入处理期间使用；视口尺寸变化后须重新计算。
struct UnifiedDragTarget {
    /// @brief 目标锚点或实际触发时间，单位秒。
    double time{ 0.0 };
    /// @brief 草稿/玩家/BGM 共用的有符号轨道，负值表示草稿轨。
    std::int32_t absoluteTrack{ 0 };
    /// @brief 当前目标区域在线性轨号公式中的原点，未必是玩家区左边界。
    float leftX{ 0.0F };
    /// @brief 单轨逻辑宽度，供非主画布兼容连续玩家域。
    float singleTrackWidth{ 0.0F };
    /// @brief 本帧主画布的完整轨道投影，供每个实体独立解析横坐标。
    CanvasLaneProjection projection;
};

/// @brief 将当前鼠标位置换算为统一画布时间与绝对轨道。
/// @param ctx 会话上下文。
/// @param cmd 拖动更新命令。
/// @return 缺少相机或滚动缓存时返回空。
/// @warning
/// 拖动热路径：使用滚动缓存和公共吸附入口；禁止加入文件访问或阻塞等待。
/// @note 时间使用动画锚点逆映射，保证指针对应当前看到的画面。
/// @note 主画布按真实分区解析轨道，辅助视图沿用连续玩家轨兼容映射。
/// @note 返回空表示本轮不提供新目标，不能用默认零值替代上一有效位置。
std::optional<UnifiedDragTarget> calculateUnifiedDragTarget(
    SessionContext& ctx, const CmdUpdateDrag& cmd)
{
    // 先确认相机和滚动缓存存在，再读取尺寸与时间映射。
    // 初始化尚未完成时不创建临时缓存，避免拖动路径触发昂贵重建。
    const auto cameraIterator = ctx.cameras.find(cmd.cameraId);
    if ( cameraIterator == ctx.cameras.end() ) return std::nullopt;
    const auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return std::nullopt;

    const auto& camera = cameraIterator->second;
    const float judgmentLineY =
        camera.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    float renderScaleY = 1.0F;
    // Preview 需要解除相对主画布的压缩后才能反算逻辑时间。
    // 两个既有预览标识共用该规则，普通主画布倍率保持一。
    if ( cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas" ) {
        const auto* mainCamera =
            SessionUtils::findMainCanvasCamera(ctx.cameras);
        const float mainViewportHeight =
            mainCamera ? mainCamera->viewportHeight : camera.viewportHeight;
        const float mainEffectiveHeight =
            (ctx.lastConfig.visual.trackLayout.bottom -
             ctx.lastConfig.visual.trackLayout.top) *
            mainViewportHeight;
        const float previewTop = ctx.lastConfig.visual.previewConfig.margin.top;
        const float previewBottom =
            camera.viewportHeight -
            ctx.lastConfig.visual.previewConfig.margin.bottom;
        // 主画布使用归一化轨道高度，预览使用像素边距。
        // 两者换成像素后再结合 areaRatio，避免把整个窗口高度当作有效轨道高度。
        const float denominator =
            mainEffectiveHeight * ctx.lastConfig.visual.previewConfig.areaRatio;
        if ( std::abs(denominator) > 1e-6F ) {
            renderScaleY = (previewBottom - previewTop) / denominator;
        }
    }
    // 退化倍率不能作为逆映射除数，本轮返回无目标。
    // 判断后按画面动画时间取绝对滚动锚点，不使用未经平滑的播放时间。
    if ( std::abs(renderScaleY) < 1e-6F ) return std::nullopt;
    const double currentAbsY = cache->getAbsY(ctx.animateTime);
    const double targetAbsY =
        currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
    double targetTime = cache->getTime(targetAbsY);

    // 公共吸附助手使用缓存的 BPM 列表，按需维护由会话负责。
    // 不在鼠标命令中单独遍历、排序整条时间线。
    SessionUtils::ensureBpmEvents(ctx);
    const auto snap = SessionUtils::getSnapResult(
        targetTime,
        cmd.mouseY,
        camera,
        ctx.lastConfig,
        ctx.bpmEvents,
        ctx.timelineRegistry,
        ctx.animateTime,
        ctx.cameras,
        ctx.currentBeatmap
            ? ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : 120.0);
    // Ctrl 绕过本次吸附，保留原始鼠标时间。
    // 是否在最终物件层钳制时间由后续拖动更新决定，此处只计算目标。
    if ( snap.isSnapped && !cmd.isCtrlDown ) {
        targetTime = snap.snappedTime;
    }

    const bool isMainCanvas = SessionUtils::isMainCanvasCameraId(cmd.cameraId);
    // 主画布带水平平移以及草稿、BGM 开关，端点可跨真实区域。
    // 辅助视图不继承主画布平移，仍提供兼容玩家域投影。
    const auto projection = calculateCanvasLaneProjection(
        camera.viewportWidth,
        ctx.trackCount,
        ctx.bgmTrackCount,
        ctx.lastConfig.visual.trackLayout,
        isMainCanvas ? camera.horizontalOffsetX : 0.0F,
        isMainCanvas,
        isMainCanvas && ctx.lastConfig.settings.enableBmsEditing,
        isMainCanvas && ctx.lastConfig.settings.professionalMode,
        ctx.draftTrackCount,
        isMainCanvas);
    if ( !projection.valid ) return std::nullopt;

    CanvasLaneAddress address;
    if ( isMainCanvas ) {
        // 独立区域之间的空隙按真实几何吸附；批注区保持上一拖动目标。
        const auto lane = projection.nearestLane(cmd.mouseX);
        if ( !lane ) return std::nullopt;
        address = *lane;
    } else {
        address = {
            CanvasLaneKind::Player,
            static_cast<std::uint32_t>(
                projection.player.trackAt(cmd.mouseX, ctx.trackCount)),
        };
    }

    // 领域地址转为统一有符号轨号，供整个拖动组计算逻辑轨差。
    // 同时查询实际像素轨宽，不能用该轨差直接乘玩家宽度。
    const auto absoluteTrack = address.absoluteTrack(projection.playerLaneCount,
                                                     projection.draftLaneCount);
    const auto laneBounds    = projection.bounds(address);
    if ( !laneBounds ) return std::nullopt;
    // 返回与绝对轨道公式兼容的区域原点，草稿/BGM 拖动不会再借用玩家宽度。
    const float laneWidth = laneBounds->rightX - laneBounds->leftX;
    // 构造与旧线性公式兼容的区域原点，满足 left+absoluteTrack×width。
    // 该值不是整个画布固定左边界，而是当前目标区域的换算基准。
    const float domainOrigin =
        laneBounds->leftX - static_cast<float>(absoluteTrack) * laneWidth;
    return UnifiedDragTarget{
        .time             = targetTime,
        .absoluteTrack    = absoluteTrack,
        .leftX            = domainOrigin,
        .singleTrackWidth = laneWidth,
        .projection       = projection,
    };
}

/// @brief 判断音符及其折线子物件是否完整位于同一个可持久化轨道域。
/// @param note 待检查音符；根轨道决定草稿域或玩家域。
/// @param trackCount 玩家轨道数。
/// @return 所有节点和 Flick 终点均未跨越轨道域边界时返回 true。
/// @note 草稿域只要求轨号为负，玩家域同时要求小于玩家轨数。
/// @note 根轨决定目标域，所有折线子项及 Flick 终点必须留在同域。
/// @note 草稿域不在此按当前可见草稿轨数裁剪，范围扩展由拖动目标边界处理。
/// @note 只校验横向归属，不校验时间、持续长度或绑定资源类型。
bool noteFitsTrackDomain(const NoteComponent& note, std::int32_t trackCount)
{
    if ( trackCount <= 0 ) return false;
    // 根轨符号确定最终可持久化的域，子项不能各自选择不同域。
    // 玩家轨数非正时整体拒绝，避免后续玩家边界检查无意义。
    const bool draftDomain = note.m_trackIndex < 0;
    /// @brief 检查一个起轨及可选 Flick 终点是否留在根物件域。
    /// @param type 当前物件或子项类型。
    /// @param track 起始统一轨号。
    /// @param dtrack Flick 相对轨差。
    /// @return 起点及需要检查的终点都在目标域内时为 true。
    const auto fits = [trackCount, draftDomain](::MMM::NoteType type,
                                                std::int32_t    track,
                                                std::int32_t    dtrack) {
        // 用宽整数计算 Flick 终点，防止轨差相加在校验前溢出。
        // 对非 Flick 只检查起轨，duration 不参与横向域验证。
        const auto endTrack = static_cast<std::int64_t>(track) + dtrack;
        if ( draftDomain ) {
            return track < 0 &&
                   (type != ::MMM::NoteType::FLICK || endTrack < 0);
        }
        if ( track < 0 || track >= trackCount ) return false;
        return type != ::MMM::NoteType::FLICK ||
               (endTrack >= 0 && endTrack < trackCount);
    };
    if ( !fits(note.m_type, note.m_trackIndex, note.m_dtrack) ) return false;
    // 父起轨通过后继续验证每个子项，不能只靠父对象判断折线合法。
    // 空子列表自然通过，普通物件只受前面的根检查约束。
    return std::all_of(note.m_subNotes.begin(),
                       note.m_subNotes.end(),
                       [&](const NoteComponent::SubNote& sub) {
                           return fits(sub.type, sub.trackIndex, sub.dtrack);
                       });
}

/// @brief 判断拖动可变字段是否保持不变。
/// @warning 释放低频路径：仅遍历单个 Polyline 的局部子物件。
/// @note 只比较本工具连续拖动可能改变的位置、跨度和草稿状态。
/// @note 不等价于完整组件相等，不用于判断颜色、绑定或元数据变化。
/// @param lhs 按下时保存的初始组件。
/// @param rhs 释放时读取的当前组件。
/// @return 所有受检查的拖动字段均保持一致时为 true。
/// @note 子项的类型、颜色和绑定不在比较范围内，结构清理另有提交路径。
/// @note 时间字段使用精确相等；更新阶段的目标容差不用于抹去已发生的历史变化。
bool sameDraggedNoteState(const NoteComponent& lhs, const NoteComponent& rhs)
{
    // 先比较根字段和子列表大小，避免后续按下标访问不等长列表。
    // 任何可变字段不同都需要保留实际拖动结果。
    if ( lhs.m_timestamp != rhs.m_timestamp ||
         lhs.m_trackIndex != rhs.m_trackIndex ||
         lhs.m_duration != rhs.m_duration || lhs.m_dtrack != rhs.m_dtrack ||
         lhs.m_isDraft != rhs.m_isDraft ||
         lhs.m_subNotes.size() != rhs.m_subNotes.size() ) {
        return false;
    }
    // 子项按原顺序逐个比较，不按时间重新配对。
    // 拖动的结构清理在收尾另行处理，这里不是通用折线等价判定。
    for ( std::size_t index = 0; index < lhs.m_subNotes.size(); ++index ) {
        const auto& left  = lhs.m_subNotes[index];
        const auto& right = rhs.m_subNotes[index];
        if ( left.timestamp != right.timestamp ||
             left.trackIndex != right.trackIndex ||
             left.duration != right.duration || left.dtrack != right.dtrack ) {
            return false;
        }
    }
    return true;
}

/// @brief 判断自动采样拖动可变字段是否保持不变。
/// @note 锚点、偏移与轨道是拖动可变字段，资源和音量不参与比较。
/// @note 精确比较用于跳过未变手势，不引入额外的时间容差。
/// @param lhs 手势初始采样。
/// @param rhs 当前采样预览值。
/// @return 锚点秒时间、整数毫秒偏移与统一轨号完全相同时为 true。
/// @warning 释放阶段的固定字段比较，不查询资源或执行同步。
bool sameDraggedSampleState(const SampleComponent& lhs,
                            const SampleComponent& rhs)
{
    return lhs.m_timestamp == rhs.m_timestamp &&
           lhs.m_offsetMs == rhs.m_offsetMs && lhs.m_track == rhs.m_track;
}

/// @brief 按项目资源表解析谱面物件保存的音频引用。
/// @param ctx 会话上下文。
/// @param reference 资源 ID 或兼容路径。
/// @return 匹配项目资源；无项目或引用失效时返回空。
/// @note 查询已加载项目表，不做目录遍历或实际文件读取。
/// @note 返回项目拥有的观察指针，仅在项目资源表稳定期间使用。
const ::MMM::AudioResource* resolveReferencedAudioResource(
    const SessionContext& ctx, const std::string& reference)
{
    // 协作项目优先于编辑器当前本机项目，资源身份只能在对应项目解释。
    // 缺少项目或空引用直接返回空，不生成占位资源。
    const auto* project = ctx.collaborationProject
                              ? ctx.collaborationProject.get()
                              : EditorEngine::instance().getCurrentProject();
    if ( !project || reference.empty() ) return nullptr;
    // 谱面路径只作为兼容引用解析的上下文，不检查文件是否存在。
    // 没有当前谱面时传空路径，由资源表查找决定能否解析。
    const std::filesystem::path beatmapPath =
        ctx.currentBeatmap ? ctx.currentBeatmap->m_baseMapMetadata.map_path
                           : std::filesystem::path{};
    return ProjectResourceService::findAudioResourceForReference(
        *project, beatmapPath, reference);
}

/// @brief 清除会话级拖拽手势状态。
/// @param ctx 会话上下文。
/// @note 只解除会话手势归属，不清除工具保存的初始状态映射。
/// @note 参与实体的组件拖动标志由单独清理入口处理。
/// @note draggedEntity 与对象种类由各结束分支重置，不能只调用本助手完成释放。
void clearSessionDragState(SessionContext& ctx)
{
    // 清除部件、子索引和相机归属，防止下一次手势套用旧局部编辑目标。
    // 不在这里恢复位置，回退或提交必须先由结束事务完成。
    ctx.isDragging      = false;
    ctx.draggedPart     = HoverPart::None;
    ctx.draggedSubIndex = -1;
    ctx.dragCameraId.clear();
}
}  // namespace

/// @brief 将可转换的采样映射为玩家点击物件。
/// @param sample 保留锚点、资源与实例音量的源采样。
/// @param targetTrack 非负目标玩家轨，域上界由调用方校验。
/// @param resource 已解析的资源观察指针，可为空。
/// @return 可无损表达为点击时返回组件，否则为空。
/// @note 非零偏移无法由普通点击表达，必须拒绝而非丢弃偏移。
/// @note 本函数不复制源实体选择状态；选择继承由转换记录和 Action 管理。
/// @note 绑定引用沿用采样存储值，资源指针只用于验证类型。
std::optional<NoteComponent> makePlayerNoteFromSample(
    const SampleComponent& sample, std::int32_t targetTrack,
    const ::MMM::AudioResource* resource)
{
    // 偏移采样有独立触发点，普通 Note 无法保留这份语义。
    // 负目标轨也不能在此转换成玩家物件，草稿跨域由其他规则处理。
    if ( sample.m_offsetMs != 0 || targetTrack < 0 ) {
        return std::nullopt;
    }

    NoteComponent note;
    note.m_type       = ::MMM::NoteType::NOTE;
    note.m_timestamp  = sample.m_timestamp;
    note.m_trackIndex = targetTrack;
    // 无音频引用的采样可转成普通无绑定点击。
    // 有引用则必须验证资源类型，不能静默丢弃不支持的主音轨引用。
    if ( sample.m_audioResourceId.empty() ) {
        return note;
    }
    if ( !resource || resource->m_type != ::MMM::AudioTrackType::Effect ) {
        return std::nullopt;
    }
    // 音效绑定保留原引用与实例音量，不把音量乘入资源公共设置。
    // 转换不自动把兼容路径改为资源表 ID，身份规范化仍遵循调用方来源。
    note.m_sampleBinding =
        ::MMM::AudioSampleBinding{ sample.m_audioResourceId, sample.m_volume };
    return note;
}

/// @brief 将不含持续或子结构的玩家点击转换为采样。
/// @param note 要转换的独立普通 Note。
/// @param targetTrack 调用方确定的目标统一采样轨号。
/// @param resource 已解析的绑定资源，仅允许效果类型。
/// @return 成功时返回零偏移采样，否则为空。
/// @note 不执行 Registry 迁移，只准备供组合 Action 使用的值对象。
/// @note 不自动把父颜色映射到采样样式，两种对象使用各自渲染规则。
/// @note 返回的采样尚无 ECS 身份，只有正式提交时才分配目标实体。
std::optional<SampleComponent> makeAudioSampleFromPlayerNote(
    const NoteComponent& note, std::uint32_t targetTrack,
    const ::MMM::AudioResource* resource)
{
    // 只有独立点击可直接转换，Hold、Flick 和折线结构不能降格丢信息。
    // 即使类型是 NOTE，也检查残留持续、轨差和子列表。
    if ( note.m_isSubNote || note.m_type != ::MMM::NoteType::NOTE ||
         note.m_duration != 0.0 || note.m_dtrack != 0 ||
         !note.m_subNotes.empty() ) {
        return std::nullopt;
    }

    SampleComponent sample;
    sample.m_timestamp = note.m_timestamp;
    sample.m_offsetMs  = 0;
    sample.m_track     = targetTrack;
    // 无绑定或空资源引用保留为空采样，不构造虚假的音频资源。
    // 有绑定时再验证效果资源，并原样继承实例音量。
    if ( !note.m_sampleBinding ||
         note.m_sampleBinding->m_audioResourceId.empty() ) {
        return sample;
    }
    if ( !resource || resource->m_type != ::MMM::AudioTrackType::Effect ) {
        return std::nullopt;
    }
    sample.m_audioResourceId = note.m_sampleBinding->m_audioResourceId;
    sample.m_volume          = note.m_sampleBinding->m_volume;
    return sample;
}

/// @brief 开始物件移动或局部编辑拖拽。
/// @param ctx 会话上下文。
/// @param cmd 拖拽开始命令。
/// @warning 手势起始低频路径：收集选中组件并复制初始状态，折线关联收集
/// 包含 Registry 扫描；不得把这些初始化扫描移到连续更新中。
/// @note 主要对象决定整体或局部编辑模式，参与集合可同时含音符与采样。
/// @note 初始组件副本用于计算总位移与构造撤销，不能在连续更新中重置。
/// @pre 由会话逻辑线程串行调用，不能与 Registry 结构变更并发执行。
/// @note 初始集合冻结本次手势的参与者，后续选择变化不会自动增加参与对象。
/// @note 命令实体号须按 kind 在正确 Registry 中解释，不跨域尝试同号对象。
void GrabTool::handleStartDrag(SessionContext& ctx, const CmdStartDrag& cmd)
{
    // 开始新手势前清空上次模式标志、初始快照与渲染钉住集合。
    // 防止失败或结束后的旧参与者被下一次拖动重复更新。
    m_isPolylineSubDrag        = false;
    m_usesUnifiedObjectDrag    = false;
    m_isSampleOffsetDrag       = false;
    m_hasLastAppliedDragTarget = false;
    m_initialStates.clear();
    m_initialSampleStates.clear();
    ctx.dragRenderPinnedEntities.clear();
    ctx.dragSampleRenderPinnedEntities.clear();

    // 空实体在初始化清理后直接结束，不能建立没有对象的拖动基准。
    // 这里不替代控制器的领域校验，工具仍保留自己的局部前提检查。
    if ( cmd.entity == entt::null ) return;
    entt::entity draggedEntity = cmd.entity;
    // 音符与采样必须分领域校验，两个 Registry 的同号实体并非同一对象。
    // 草稿子实体可提升到父折线，后面据父对象保存初始状态。
    if ( cmd.kind == ChartObjectKind::PlayerNote ||
         cmd.kind == ChartObjectKind::DraftNote ) {
        const auto* note =
            ctx.noteRegistry.try_get<const NoteComponent>(draggedEntity);
        if ( cmd.kind == ChartObjectKind::DraftNote && note &&
             note->m_isSubNote && note->m_parentPolyline != entt::null &&
             ctx.noteRegistry.valid(note->m_parentPolyline) &&
             ctx.noteRegistry.all_of<NoteComponent>(note->m_parentPolyline) ) {
            // 草稿子实体提升到父后，重新取得父组件以检查当前编辑模式。
            // 不能继续用原子组件指针为父实体保存初始内容。
            draggedEntity = note->m_parentPolyline;
            note = ctx.noteRegistry.try_get<const NoteComponent>(draggedEntity);
        }
        if ( !note ||
             !SessionUtils::isNoteEditable(*note, ctx.lastConfig.settings) ) {
            return;
        }
    }

    ctx.draggedEntity     = draggedEntity;
    ctx.draggedObjectKind = cmd.kind;
    ctx.dragCameraId      = cmd.cameraId;
    // 从当前悬浮状态锁定命中部件和子索引，拖动中不随新的悬浮改变。
    // 命令领域和相机同步记录，供后续连续更新选择正确路径。
    ctx.draggedPart     = static_cast<HoverPart>(ctx.hoveredPart);
    ctx.draggedSubIndex = ctx.hoveredSubIndex;
    // 草稿从起始帧就使用统一有符号轨号，不能先进入只接受玩家轨的兼容路径。
    // 后面是否包含采样不会取消这一最初的跨域要求。
    if ( cmd.kind == ChartObjectKind::DraftNote ) {
        m_usesUnifiedObjectDrag = true;
    }

    if ( cmd.kind == ChartObjectKind::AudioSample ) {
        auto& registry = ctx.sampleRegistry;
        if ( !registry.valid(cmd.entity) ||
             !registry.all_of<SampleComponent>(cmd.entity) ) {
            return;
        }

        m_usesUnifiedObjectDrag    = true;
        m_isSampleOffsetDrag       = ctx.draggedPart == HoverPart::SampleOffset;
        const bool primarySelected = isEntitySelected(registry, cmd.entity);
        // 已选采样的整体移动可以带动混合选中组。
        // 偏移手柄只编辑触发偏移，不能把整组选中物件一起平移。
        const bool collectSelectedGroup =
            primarySelected && !m_isSampleOffsetDrag;

        // 以主要采样的选中状态决定是否收集整组，偏移手柄已被排除。
        // 仅在手势开始时复制初始组件，连续更新以这些不变副本为基准。
        if ( collectSelectedGroup ) {
            auto sampleView =
                registry.view<InteractionComponent, SampleComponent>();
            for ( const auto entity : sampleView ) {
                const auto& interaction =
                    sampleView.get<InteractionComponent>(entity);
                if ( !interaction.isSelected ) continue;
                // 每个已选采样记录完整 before
                // 和选中标记，拖动结束可按领域提交。
                // 实例偏移与音量随快照保存，不因整体移动重新推导。
                m_initialSampleStates.emplace(
                    entity,
                    SampleInitialState{
                        sampleView.get<SampleComponent>(entity),
                        true,
                    });
                registry.get<InteractionComponent>(entity).isDragging = true;
            }

            // 混合组中加入可编辑根音符，草稿也按当前模式检查。
            // 与采样分表存储，避免两个 Registry 的实体号碰撞。
            auto noteView =
                ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
            for ( const auto entity : noteView ) {
                const auto& interaction =
                    noteView.get<InteractionComponent>(entity);
                const auto& note = noteView.get<NoteComponent>(entity);
                // 混合组先收集可编辑根音符，子实体在后续关联扫描中补齐。
                // 这样草稿模式限制不会因采样作为主要对象而被绕过。
                if ( !interaction.isSelected || note.m_isSubNote ||
                     !SessionUtils::isNoteEditable(note,
                                                   ctx.lastConfig.settings) ) {
                    continue;
                }
                m_initialStates.emplace(entity, InitialState{ note, true });
                ctx.noteRegistry.get<InteractionComponent>(entity).isDragging =
                    true;
            }
            // 父折线加入初始集合后，再补充它的子实体初始状态。
            // 子实体未必独立选中，但必须随父移动并能在收尾恢复或提交。
            for ( const auto entity : ctx.noteRegistry.view<NoteComponent>() ) {
                const auto& note = ctx.noteRegistry.get<NoteComponent>(entity);
                if ( !note.m_isSubNote ||
                     !m_initialStates.contains(note.m_parentPolyline) ) {
                    continue;
                }
                m_initialStates.emplace(
                    entity,
                    InitialState{
                        note,
                        isEntitySelected(ctx.noteRegistry, entity),
                    });
                if ( ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
                    ctx.noteRegistry.get<InteractionComponent>(entity)
                        .isDragging = true;
                }
            }
        } else {
            const auto& sample = registry.get<SampleComponent>(cmd.entity);
            m_initialSampleStates.emplace(
                cmd.entity, SampleInitialState{ sample, primarySelected });
            // 单个未选采样也需要拖动反馈组件，按需创建而不改变选中状态。
            // 初始状态中的 primarySelected 保留真实选择，不能统一强设为选中。
            if ( !registry.all_of<InteractionComponent>(cmd.entity) ) {
                registry.emplace<InteractionComponent>(cmd.entity);
            }
            registry.get<InteractionComponent>(cmd.entity).isDragging = true;
        }

        // 只有主要采样已经进入初始集合，才建立会话的拖动起点副本。
        // 切换主要对象领域时重置另一份初始 Note，避免误读旧手势基准。
        const auto initialIterator = m_initialSampleStates.find(cmd.entity);
        if ( initialIterator == m_initialSampleStates.end() ) return;
        ctx.dragInitialSample = initialIterator->second.sample;
        ctx.dragInitialNote.reset();
        ctx.isDragging = true;
    } else if ( ctx.noteRegistry.valid(draggedEntity) &&
                ctx.noteRegistry.all_of<NoteComponent>(draggedEntity) ) {
        auto&      registry        = ctx.noteRegistry;
        const bool primarySelected = isEntitySelected(registry, draggedEntity);
        // 与旧整体移动判定保持一致；HoldEnd/FlickArrow 默认仍属于局部编辑。
        // 整体部件允许与采样组成统一移动组，Hold 尾与 Flick 箭头保留局部编辑。
        // 不能因为主要对象已选中就把所有端点编辑都当作整组平移。
        const bool groupCompatiblePart =
            ctx.draggedPart == HoverPart::None ||
            ctx.draggedPart == HoverPart::Head ||
            ctx.draggedPart == HoverPart::HoldBody ||
            ctx.draggedPart == HoverPart::PolylineNode;

        // 选中主对象时保存当前可编辑选中组，并记录原有选中状态。
        // 该快照用于最终撤销，不以连续拖动后的临时位置作为 before。
        if ( primarySelected ) {
            // 模式 A: 拖动整个选中组
            auto view = registry.view<InteractionComponent, NoteComponent>();
            for ( auto entity : view ) {
                const auto& note = view.get<NoteComponent>(entity);
                if ( view.get<InteractionComponent>(entity).isSelected &&
                     SessionUtils::isNoteEditable(note,
                                                  ctx.lastConfig.settings) ) {
                    m_initialStates[entity] = {
                        note,
                        true,
                    };
                    registry.get<InteractionComponent>(entity).isDragging =
                        true;
                }
            }

            // 额外收集所有选中 Polyline 的子物件实体
            // (确保它们跟随移动并能正确提交 Action)
            for ( auto entity : registry.view<NoteComponent>() ) {
                const auto& nc = registry.get<NoteComponent>(entity);
                if ( nc.m_isSubNote &&
                     m_initialStates.count(nc.m_parentPolyline) ) {
                    m_initialStates[entity] = {
                        nc,
                        isEntitySelected(registry, entity),
                    };
                    if ( registry.all_of<InteractionComponent>(entity) ) {
                        registry.get<InteractionComponent>(entity).isDragging =
                            true;
                    }
                }
            }

            // 只有兼容整体移动的部件才收集已选采样。
            // 混合组一旦包含采样，后续要进入支持多对象领域的统一目标路径。
            if ( groupCompatiblePart ) {
                auto sampleView =
                    ctx.sampleRegistry
                        .view<InteractionComponent, SampleComponent>();
                for ( const auto entity : sampleView ) {
                    const auto& interaction =
                        sampleView.get<InteractionComponent>(entity);
                    if ( !interaction.isSelected ) continue;
                    m_initialSampleStates.emplace(
                        entity,
                        SampleInitialState{
                            sampleView.get<SampleComponent>(entity),
                            true,
                        });
                    ctx.sampleRegistry.get<InteractionComponent>(entity)
                        .isDragging = true;
                }
                // 混合选中的自动采样仍要求整组进入统一轨道域。
                m_usesUnifiedObjectDrag = !m_initialSampleStates.empty();
            }
        } else {
            // 模式 B: 只拖动当前物件
            if ( auto* note = registry.try_get<NoteComponent>(draggedEntity) ) {
                m_initialStates[draggedEntity] = { *note, false };
                if ( !registry.all_of<InteractionComponent>(draggedEntity) ) {
                    registry.emplace<InteractionComponent>(draggedEntity);
                }
                registry.get<InteractionComponent>(draggedEntity).isDragging =
                    true;

                // 如果是 Polyline，也收集其子物件
                if ( note->m_type == ::MMM::NoteType::POLYLINE ) {
                    for ( auto subEnt : registry.view<NoteComponent>() ) {
                        const auto& subNC = registry.get<NoteComponent>(subEnt);
                        if ( subNC.m_isSubNote &&
                             subNC.m_parentPolyline == draggedEntity ) {
                            m_initialStates[subEnt] = {
                                subNC,
                                isEntitySelected(registry, subEnt),
                            };
                            if ( registry.all_of<InteractionComponent>(
                                     subEnt) ) {
                                registry.get<InteractionComponent>(subEnt)
                                    .isDragging = true;
                            }
                        }
                    }
                }
            }
        }

        // 未选中 Polyline 的各部件通常移动整条折线，仅内部 HoldBody
        // 保留子段编辑。
        // 未选中折线通常整体移动，但内部 Hold 身体仍可能单独调整子段。
        // 这个判定控制是否从首帧允许草稿边界预览，不直接修改实体。
        bool movesWholeObjects = groupCompatiblePart;
        if ( !primarySelected ) {
            const auto* note =
                registry.try_get<const NoteComponent>(draggedEntity);
            if ( note && note->m_type == ::MMM::NoteType::POLYLINE ) {
                const bool editsInternalBody =
                    ctx.draggedPart == HoverPart::HoldBody &&
                    ctx.draggedSubIndex > 0;
                movesWholeObjects = !editsInternalBody;
            }
        }
        // 主画布启用草稿轨后，整体单物件和多选组都从首帧使用统一轨道域。
        // 宽 Flick/Polyline 因而能在鼠标仍位于玩家区时产生跨边界虚影。
        // 局部端点和内部主体编辑不满足该门槛，继续走既有专用更新路径。
        const bool previewsAcrossDraftBoundary =
            movesWholeObjects &&
            SessionUtils::isMainCanvasCameraId(cmd.cameraId) &&
            ctx.lastConfig.settings.professionalMode;
        m_usesUnifiedObjectDrag =
            m_usesUnifiedObjectDrag || previewsAcrossDraftBoundary;

        // 兼容旧代码 (保留主拖拽物件的初始备份)
        // 主要 Note 必须已经纳入参与集合才能进入活动拖动。
        // 同时清空采样起点副本，避免之后按错误领域读取初始时间。
        if ( m_initialStates.count(draggedEntity) ) {
            ctx.dragInitialNote = m_initialStates[draggedEntity].note;
            ctx.dragInitialSample.reset();
            ctx.isDragging = true;
        }
    } else {
        ctx.draggedEntity = entt::null;
        return;
    }

    // 把参与实体加入本次拖动的渲染保留列表，避免旧排序窗口漏掉移动中的物件。
    // 列表只保存身份，组件初始快照继续由工具持有。
    ctx.dragRenderPinnedEntities.reserve(m_initialStates.size());
    for ( const auto& [entity, state] : m_initialStates ) {
        (void)state;
        ctx.dragRenderPinnedEntities.push_back(entity);
    }
    ctx.dragSampleRenderPinnedEntities.reserve(m_initialSampleStates.size());
    for ( const auto& [entity, state] : m_initialSampleStates ) {
        (void)state;
        ctx.dragSampleRenderPinnedEntities.push_back(entity);
    }
    // 采样渲染器通过 Registry 上下文借用会话持有的保留列表。
    // 首次建立槽位，后续手势复用；不能指向函数局部临时数组。
    auto* pinned = ctx.sampleRegistry.ctx().find<DragRenderPinnedEntities>();
    if ( !pinned ) {
        pinned = &ctx.sampleRegistry.ctx().emplace<DragRenderPinnedEntities>();
    }
    pinned->entities = &ctx.dragSampleRenderPinnedEntities;
}

/// @brief 更新跨草稿、玩家和采样领域的整体拖动或采样偏移。
/// @param ctx 正在编辑的会话状态。
/// @param cmd 本轮鼠标目标与修饰键。
/// @return 已接管本轮拖动时返回 true；false 表示继续普通局部编辑路径。
/// @warning 连续拖动热路径：主更新遍历参与集合，折线同步助手另有既有全表扫描。
/// 禁止增加全局排序、文件访问或阻塞等待；本地组件随有效目标立即更新。
/// @note 接管但无法解析本轮目标时保持当前位置，不回退到另一套坐标计算。
/// @note 横向可访问上界包含当前末尾追加轨位置，最终对象类型约束在释放阶段确认。
/// @note 预览会立即更新组件，撤销记录直到手势结束才生成。
bool GrabTool::handleUnifiedDragUpdate(SessionContext&      ctx,
                                       const CmdUpdateDrag& cmd)
{
    // 未进入统一模式的纯音符手势，先检查鼠标是否越出玩家区。
    // 仍在玩家区时交还普通更新，保持旧局部部件编辑语义。
    if ( !m_usesUnifiedObjectDrag &&
         (ctx.draggedObjectKind == ChartObjectKind::PlayerNote ||
          ctx.draggedObjectKind == ChartObjectKind::DraftNote) ) {
        if ( !SessionUtils::isMainCanvasCameraId(cmd.cameraId) ) return false;
        const auto cameraIterator = ctx.cameras.find(cmd.cameraId);
        if ( cameraIterator == ctx.cameras.end() ) return false;
        const auto playerProjection = calculatePlayerTrackProjection(
            cameraIterator->second.viewportWidth,
            ctx.trackCount,
            ctx.lastConfig.visual.trackLayout.left,
            ctx.lastConfig.visual.trackLayout.right,
            cameraIterator->second.horizontalOffsetX);
        if ( !playerProjection.valid ||
             (cmd.mouseX >= playerProjection.leftX &&
              cmd.mouseX < playerProjection.rightX) ) {
            return false;
        }
    }

    const auto target = calculateUnifiedDragTarget(ctx, cmd);
    // 统一模式已经接管时，缺少相机或合法轨道只跳过本次更新。
    // 不能返回 false 让旧路径把无效目标解释为其他玩家位置。
    if ( !target ) {
        return m_usesUnifiedObjectDrag ||
               ctx.draggedObjectKind == ChartObjectKind::AudioSample;
    }

    // 偏移拖动只改主要采样触发相对锚点的毫秒差。
    // 不移动锚点或整组选中对象，仍记录最后目标用于手势状态。
    if ( m_isSampleOffsetDrag ) {
        const auto initialIterator =
            m_initialSampleStates.find(ctx.draggedEntity);
        auto* sample =
            ctx.sampleRegistry.try_get<SampleComponent>(ctx.draggedEntity);
        // 主要采样消失时保持统一路径接管，但本轮不写入任何对象。
        // 返回 true 防止普通音符路径误用采样实体号继续处理。
        if ( initialIterator == m_initialSampleStates.end() || !sample ) {
            return true;
        }

        // 先在宽浮点中求秒差并换成毫秒，避免大时间值提前缩窄。
        // 明确检查整数上下界后才转换，极端偏移按可表示范围饱和。
        const long double offsetMs =
            (static_cast<long double>(target->time) -
             static_cast<long double>(
                 initialIterator->second.sample.m_timestamp)) *
            1000.0L;
        constexpr auto minimumOffset = std::numeric_limits<std::int64_t>::min();
        constexpr auto maximumOffset = std::numeric_limits<std::int64_t>::max();
        // 边界分支直接写整数极值，不对超范围浮点结果执行整数转换。
        // 两侧采用同一饱和策略，既支持提前触发也支持延后触发。
        if ( offsetMs <= static_cast<long double>(minimumOffset) ) {
            sample->m_offsetMs = minimumOffset;
        } else if ( offsetMs >= static_cast<long double>(maximumOffset) ) {
            sample->m_offsetMs = maximumOffset;
        } else {
            // 正常范围按最近毫秒取整，保持采样偏移字段的整数单位。
            // 不把每次鼠标增量累计到原偏移，避免舍入误差逐帧叠加。
            sample->m_offsetMs =
                static_cast<std::int64_t>(std::round(offsetMs));
        }
        m_hasLastAppliedDragTarget   = true;
        m_lastAppliedDragTargetTime  = target->time;
        m_lastAppliedDragTargetTrack = static_cast<int>(target->absoluteTrack);
        return true;
    }

    // 目标域判断以统一轨号为准，玩家区末端之后属于 BGM 范围。
    // 尚未接管且仍落在玩家轨的普通音符可继续使用专用更新。
    const bool targetIsBgm =
        target->absoluteTrack >=
        static_cast<std::int32_t>(std::max(0, ctx.trackCount));
    if ( !m_usesUnifiedObjectDrag &&
         ctx.draggedObjectKind == ChartObjectKind::PlayerNote && !targetIsBgm &&
         target->absoluteTrack >= 0 ) {
        return false;
    }
    // 一旦跨域路径接管，本手势后续即使鼠标回到玩家区也保持同一坐标语义。
    // 避免来回跨边界时在两套位移规则间切换。
    m_usesUnifiedObjectDrag = true;

    // 从主要对象的初始快照提取基准，而不是读取已被前几帧改变的组件。
    // 整组始终应用同一总位移，保证相对时间与轨差不漂移。
    double       primaryTime  = 0.0;
    std::int64_t primaryTrack = 0;
    if ( ctx.draggedObjectKind == ChartObjectKind::AudioSample ) {
        const auto initialIterator =
            m_initialSampleStates.find(ctx.draggedEntity);
        if ( initialIterator == m_initialSampleStates.end() ) return true;
        // 整体采样移动以锚点作为主要参考，触发偏移保留在组件中。
        // 偏移手柄的独立分支已经提前返回，不混入组平移。
        primaryTime  = initialIterator->second.sample.m_timestamp;
        primaryTrack = initialIterator->second.sample.m_track;
    } else {
        const auto initialIterator = m_initialStates.find(ctx.draggedEntity);
        if ( initialIterator == m_initialStates.end() ) return true;
        primaryTime  = initialIterator->second.note.m_timestamp;
        primaryTrack = initialIterator->second.note.m_trackIndex;
    }

    double       deltaTime = target->time - primaryTime;
    std::int64_t deltaTrack =
        static_cast<std::int64_t>(target->absoluteTrack) - primaryTrack;
    // 禁用纵向拖动时只把组时间位移归零，横向移动仍正常计算。
    // 吸附目标可以变化，但不应间接改变参与对象的时间。
    if ( ctx.lastConfig.settings.disableVerticalObjectDrag ) {
        deltaTime = 0.0;
    }

    // 收集整组最早时间和横向跨度，用于一次性限制共同位移。
    // 不能逐个对象分别钳制，否则多选对象的相对位置会被压缩。
    double       minimumTimestamp = std::numeric_limits<double>::infinity();
    std::int64_t minimumTrack     = std::numeric_limits<std::int64_t>::max();
    std::int64_t maximumTrack     = std::numeric_limits<std::int64_t>::min();
    // 起轨和终轨共用宽整数边界，避免长 Flick 的端点在收集时缩窄。
    // 这里只扩展跨度，最终可移动范围由所有参与者收集完后统一求交。
    const auto includeTrack = [&](std::int64_t track) {
        minimumTrack = std::min(minimumTrack, track);
        maximumTrack = std::max(maximumTrack, track);
    };
    /// @brief 将根和内嵌子项的起点及 Flick 终轨计入组边界。
    /// @param note 本次手势保存的初始组件。
    /// @warning 每次统一拖动仅遍历参与物件的局部子列表，不查询完整 Registry。
    const auto includeNote = [&](const NoteComponent& note) {
        minimumTimestamp = std::min(minimumTimestamp, note.m_timestamp);
        includeTrack(note.m_trackIndex);
        // Flick 终轨也属于横向跨度，鼠标根轨合法不代表箭头不越界。
        // 折线子项的 Flick 终点在下面按相同规则纳入。
        if ( note.m_type == ::MMM::NoteType::FLICK ) {
            includeTrack(static_cast<std::int64_t>(note.m_trackIndex) +
                         note.m_dtrack);
        }
        for ( const auto& sub : note.m_subNotes ) {
            minimumTimestamp = std::min(minimumTimestamp, sub.timestamp);
            includeTrack(sub.trackIndex);
            if ( sub.type == ::MMM::NoteType::FLICK ) {
                includeTrack(static_cast<std::int64_t>(sub.trackIndex) +
                             sub.dtrack);
            }
        }
    };

    for ( const auto& [entity, state] : m_initialStates ) {
        (void)entity;
        includeNote(state.note);
    }
    for ( const auto& [entity, state] : m_initialSampleStates ) {
        (void)entity;
        minimumTimestamp = std::min(minimumTimestamp, state.sample.m_timestamp);
        includeTrack(state.sample.m_track);
    }
    // 最早对象不能移动到负时间，必要时限制整组时间位移。
    // 其他对象仍保留与最早对象的间隔，不独立钳制各自时间。
    if ( std::isfinite(minimumTimestamp) &&
         minimumTimestamp + deltaTime < 0.0 ) {
        deltaTime = -minimumTimestamp;
    }

    // 上界按玩家数加持久 BGM 数构造，包含末尾追加轨的目标编号。
    // 整组跨度检查会结合所有 Flick 终点限制可达的共同轨差。
    const std::int64_t maximumAccessibleTrack =
        static_cast<std::int64_t>(std::max(0, ctx.trackCount)) +
        static_cast<std::int64_t>(std::max(0, ctx.bgmTrackCount));
    // 纯音符组可进入负草稿轨，混入采样后下界保持为零。
    // 采样轨字段无符号，不能把它们拖入负域后再强转存储。
    const std::int64_t minimumAccessibleTrack =
        m_initialSampleStates.empty()
            ? -static_cast<std::int64_t>(std::max(0, ctx.draftTrackCount) + 1)
            : 0;
    if ( minimumTrack != std::numeric_limits<std::int64_t>::max() &&
         maximumTrack != std::numeric_limits<std::int64_t>::min() ) {
        // 由组最小和最大轨反推出共同位移区间，而非只限制主要对象。
        // 没有可行区间时取消横移，避免在边界强行压缩整个组。
        const std::int64_t minimumDelta = minimumAccessibleTrack - minimumTrack;
        const std::int64_t maximumDelta = maximumAccessibleTrack - maximumTrack;
        if ( minimumDelta <= maximumDelta ) {
            deltaTrack = std::clamp(deltaTrack, minimumDelta, maximumDelta);
        } else {
            deltaTrack = 0;
        }
    }

    // 比较已经钳制后的有效目标，而不是原始鼠标输入。
    // 边缘外继续移动但实际目标不变时可以跳过重复组件写入。
    constexpr double TARGET_TIME_EPSILON = 1e-7;
    const auto       appliedTargetTrack  = static_cast<int>(
        std::clamp<std::int64_t>(primaryTrack + deltaTrack,
                                 std::numeric_limits<int>::min(),
                                 std::numeric_limits<int>::max()));
    const double appliedTargetTime = primaryTime + deltaTime;
    // 时间微小误差与相同轨道共同构成未变目标，直接返回已接管。
    // 这个去重不引入时间等待，下一次实际位移仍立即应用。
    if ( m_hasLastAppliedDragTarget &&
         std::abs(m_lastAppliedDragTargetTime - appliedTargetTime) <=
             TARGET_TIME_EPSILON &&
         m_lastAppliedDragTargetTrack == appliedTargetTrack ) {
        return true;
    }
    m_hasLastAppliedDragTarget = true;
    // 缓存钳制后的主要目标，后续输入即使不同也可能落到同一边界。
    // 缓存只用于跳过重复组件写入，不作为下一次位移的起点。
    m_lastAppliedDragTargetTime  = appliedTargetTime;
    m_lastAppliedDragTargetTrack = appliedTargetTrack;

    for ( const auto& [entity, state] : m_initialStates ) {
        auto* note = ctx.noteRegistry.try_get<NoteComponent>(entity);
        if ( !note ) continue;
        // 所有更新都用初始值加总位移，防止连续拖动累计浮点误差。
        // 对象已经删除时跳过，不尝试在拖动中重建丢失实体。
        note->m_timestamp = state.note.m_timestamp + deltaTime;
        const auto movedTrack =
            static_cast<std::int64_t>(state.note.m_trackIndex) + deltaTrack;
        note->m_trackIndex = static_cast<int>(
            std::clamp<std::int64_t>(movedTrack,
                                     std::numeric_limits<int>::min(),
                                     std::numeric_limits<int>::max()));
        // 跨过零轨边界时即时更新草稿标志，使本地视觉跟随当前域。
        // 最终能否持久化跨域结构由释放时的合法性与转换流程决定。
        note->m_isDraft = note->m_trackIndex < 0;
        // 父折线的内嵌子项使用相同总位移，保持内部时间和轨道关系。
        // 同时限制到当前与初始列表长度，防止结构变化后按旧下标越界。
        for ( std::size_t index = 0; index < note->m_subNotes.size() &&
                                     index < state.note.m_subNotes.size();
              ++index ) {
            note->m_subNotes[index].timestamp =
                state.note.m_subNotes[index].timestamp + deltaTime;
            // 对子项轨号先做宽整数加法，再缩窄到组件使用的整数范围。
            // 这层防护独立于整组域限制，防止表达类型边界导致算术溢出。
            const auto movedSubTrack =
                static_cast<std::int64_t>(
                    state.note.m_subNotes[index].trackIndex) +
                deltaTrack;
            note->m_subNotes[index].trackIndex = static_cast<int>(
                std::clamp<std::int64_t>(movedSubTrack,
                                         std::numeric_limits<int>::min(),
                                         std::numeric_limits<int>::max()));
        }
        if ( auto* transform =
                 ctx.noteRegistry.try_get<TransformComponent>(entity) ) {
            // 选中组可同时跨越 Draft/Player/BGM，不能广播鼠标目标域几何。
            // 每个根实体都按移动后的绝对轨道重新寻址，保证独立区域宽度生效。
            // 子节点继续由根折线数据同步，此处只维护实体自身的缓存坐标。
            const auto address = CanvasLaneAddress::fromAbsoluteTrack(
                note->m_trackIndex,
                target->projection.playerLaneCount,
                target->projection.draftLaneCount);
            if ( const auto bounds = target->projection.bounds(address) ) {
                transform->m_pos.x = bounds->leftX;
            } else {
                // Preview 等连续玩家域保持原公式，避免改变非主画布拖动语义。
                transform->m_pos.x =
                    target->leftX + static_cast<float>(note->m_trackIndex) *
                                        target->singleTrackWidth;
            }
        }
    }

    // unordered_map 不保证父子实体顺序；根折线全部移动后再以根数据覆盖子实体。
    // 此处调用的同步助手仍扫描完整 NoteComponent 视图，参与集合局部化
    // 并未消除这项既有开销；新增同步逻辑不得再叠加全谱遍历。
    for ( const auto& [entity, state] : m_initialStates ) {
        (void)state;
        const auto* note =
            ctx.noteRegistry.try_get<const NoteComponent>(entity);
        if ( note && note->m_type == ::MMM::NoteType::POLYLINE &&
             !note->m_isSubNote ) {
            syncPolylineSubEntities(ctx, entity, *note);
        }
    }

    for ( const auto& [entity, state] : m_initialSampleStates ) {
        auto* sample = ctx.sampleRegistry.try_get<SampleComponent>(entity);
        if ( !sample ) continue;
        // 自动采样同样以初始锚点加组位移，偏移字段保持不动。
        // 移动整个采样会让实际触发时间随锚点等量移动。
        sample->m_timestamp = state.sample.m_timestamp + deltaTime;
        const auto movedTrack =
            static_cast<std::int64_t>(state.sample.m_track) + deltaTrack;
        sample->m_track =
            static_cast<std::uint32_t>(std::max<std::int64_t>(0, movedTrack));
    }
    return true;
}

/// @brief 更新普通玩家域的整体位移或被抓取部件的局部编辑。
/// @param ctx 正在拖动的会话及初始组件快照。
/// @param cmd 当前鼠标目标和修饰键。
/// @warning 连续输入路径：优先交给统一跨域更新，不得添加文件访问或阻塞等待。
/// @note 位移按初始状态计算，局部尺寸按抓取部件规则更新。
/// @note 该兼容分支的玩家域限制不应用于已接管的跨区拖动。
/// @note 同一手势中父子结构须保持索引一致；结构清理推迟到结束入口。
/// @pre 开始命令已建立初始快照，更新期间不得从其他线程改写该快照。
/// @note 连续写入用于即时预览；外部读取者应遵循会话既有线程归属。
void GrabTool::handleUpdateDrag(SessionContext& ctx, const CmdUpdateDrag& cmd)
{
    if ( ctx.draggedEntity == entt::null ) return;
    // 统一路径已接管时直接结束，避免同一输入被两套规则重复应用。
    // 普通路径必须找到主对象初始快照，不能临时以当前值作为起点。
    if ( handleUnifiedDragUpdate(ctx, cmd) ) return;
    // 普通音符拖动必须有参与快照，没有状态时不试图重建手势。
    // 主要实体还需存在于映射中，避免下标访问插入默认组件。
    if ( m_initialStates.empty() ) return;
    if ( m_initialStates.find(ctx.draggedEntity) == m_initialStates.end() )
        return;

    auto it = ctx.cameras.find(cmd.cameraId);
    if ( it == ctx.cameras.end() ) return;

    // --- 1. 计算鼠标指向的目标位置 (Time, Track) ---
    // 以所属相机的判定线和动画时间逆映射，保持鼠标与可见物件对应。
    // Preview 额外解除压缩，主画布缩放已包含在滚动缓存中。
    float judgmentLineY =
        it->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" ) {
        const auto* mainCamera =
            SessionUtils::findMainCanvasCamera(ctx.cameras);
        float mainViewportHeight =
            mainCamera ? mainCamera->viewportHeight : it->second.viewportHeight;
        float mainEffectiveH = (ctx.lastConfig.visual.trackLayout.bottom -
                                ctx.lastConfig.visual.trackLayout.top) *
                               mainViewportHeight;
        float ty             = ctx.lastConfig.visual.previewConfig.margin.top;
        float by           = it->second.viewportHeight -
                             ctx.lastConfig.visual.previewConfig.margin.bottom;
        float previewDrawH = by - ty;
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
    }

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;

    double currentAbsY = cache->getAbsY(ctx.animateTime);
    double targetAbsY =
        currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
    double targetTime = cache->getTime(targetAbsY);

    // 磁吸处理
    SessionUtils::ensureBpmEvents(ctx);
    const auto& bpmEvents = ctx.bpmEvents;

    auto snap = SessionUtils::getSnapResult(
        targetTime,
        cmd.mouseY,
        it->second,
        ctx.lastConfig,
        bpmEvents,
        ctx.timelineRegistry,
        ctx.animateTime,
        ctx.cameras,
        ctx.currentBeatmap
            ? ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : 120.0);
    // 吸附只修改当前目标时间，初始时间始终保留按下时的值。
    // Ctrl 释放后可以重新吸附，不需要重新开始拖动。
    if ( snap.isSnapped && !cmd.isCtrlDown ) {
        targetTime = snap.snappedTime;
    }

    // 这一分支使用玩家域投影，跨草稿或采样的整体移动由统一入口处理。
    // 仅主画布带水平相机偏移，辅助视图保留自己的原点。
    const auto projection = calculatePlayerTrackProjection(
        it->second.viewportWidth,
        ctx.trackCount,
        ctx.lastConfig.visual.trackLayout.left,
        ctx.lastConfig.visual.trackLayout.right,
        SessionUtils::isMainCanvasCameraId(cmd.cameraId)
            ? it->second.horizontalOffsetX
            : 0.0F);
    const float leftX        = projection.leftX;
    const float singleTrackW = projection.singleTrackWidth;
    int         targetTrack  = projection.trackAt(cmd.mouseX, ctx.trackCount);

    // --- 2. 计算参考点的初始位置 ---
    // 以鼠标抓取的那个点作为参考，计算位移增量
    const auto& primaryInitialState = m_initialStates[ctx.draggedEntity];
    double      refInitialTime      = primaryInitialState.note.m_timestamp;
    int         refInitialTrack     = primaryInitialState.note.m_trackIndex;

    // 抓住折线节点时以该节点初始位置作参考，避免物件瞬间跳到根节点。
    // 先校验子索引范围，再读取初始子列表。
    if ( ctx.draggedPart == HoverPart::PolylineNode &&
         ctx.draggedSubIndex >= 0 &&
         ctx.draggedSubIndex <
             (int)primaryInitialState.note.m_subNotes.size() ) {
        refInitialTime =
            primaryInitialState.note.m_subNotes[ctx.draggedSubIndex].timestamp;
        refInitialTrack =
            primaryInitialState.note.m_subNotes[ctx.draggedSubIndex].trackIndex;
    }

    double deltaT     = targetTime - refInitialTime;
    int    deltaTrack = targetTrack - refInitialTrack;

    // --- 3. 限制增量，确保所有物件合法 ---
    // 注意：如果是拖拽特定部件且没有多选，则可能进入单点编辑模式
    // 整体部件可带动已保存参与组，尾部和箭头通常有独立编辑语义。
    // isMultiDrag 描述更新方式，并不等价于参与实体数量大于一。
    bool isMultiDrag = (ctx.draggedPart == HoverPart::None ||
                        ctx.draggedPart == HoverPart::Head ||
                        ctx.draggedPart == HoverPart::HoldBody ||
                        ctx.draggedPart == HoverPart::PolylineNode);

    // 如果点击的是特定部件 (如 HoldEnd)，且该物件未被选中，则强制进入单点编辑
    bool isPrimarySelected = false;
    if ( ctx.noteRegistry.all_of<InteractionComponent>(ctx.draggedEntity) ) {
        isPrimarySelected =
            ctx.noteRegistry.get<InteractionComponent>(ctx.draggedEntity)
                .isSelected;
    }

    // 对于未选中的 Polyline，所有部分的拖拽均视为整体移动（多选模式）
    // 确保拖拽头部、节点、尾端等任意部位时，折线及其所有子物件整体跟随移动
    if ( !isPrimarySelected ) {
        auto* draggedNote =
            ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
        if ( draggedNote && draggedNote->m_type == ::MMM::NoteType::POLYLINE ) {
            isMultiDrag = true;
        }
    }

    if ( !isPrimarySelected ) {
        if ( ctx.draggedPart == HoverPart::HoldEnd ||
             ctx.draggedPart == HoverPart::FlickArrow ) {
            // 仅非 Polyline 实体支持单节点编辑（调节长条长度或滑键方向）
            auto* draggedNote =
                ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
            if ( !draggedNote ||
                 draggedNote->m_type != ::MMM::NoteType::POLYLINE ) {
                isMultiDrag = false;
            }
        }
    }

    // --- 折线内部子段拖拽检测 ---
    // 未选中折线的内部 HoldBody 是整体移动规则中的特例。
    // 只允许正子索引，根段不按内部连接调整处理。
    bool isPolylineSubDrag = false;
    if ( !isPrimarySelected && ctx.draggedPart == HoverPart::HoldBody ) {
        auto* draggedNote =
            ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
        if ( draggedNote && draggedNote->m_type == ::MMM::NoteType::POLYLINE &&
             ctx.draggedSubIndex > 0 &&
             ctx.draggedSubIndex < (int)(draggedNote->m_subNotes.size()) ) {
            isPolylineSubDrag = true;
        }
    }
    // 把本轮判定保存给释放入口，决定是否尝试退化段清理。
    // 该标志不是实体持久属性，不随谱面序列化。
    m_isPolylineSubDrag = isPolylineSubDrag;

    // 禁用纵向对象拖动仅约束整体平移，不禁用改变局部持续时间。
    // 内部子段移动与末端拉伸继续遵守各自规则。
    const bool movesWholeObjects = isMultiDrag && !isPolylineSubDrag;
    if ( movesWholeObjects &&
         ctx.lastConfig.settings.disableVerticalObjectDrag ) {
        deltaT = 0.0;
    }

    // 相同吸附目标可跳过重复写入，不通过定时器延迟交互。
    // 记录的是当前更新规则实际使用的时间，纵向锁定时保持初始时间。
    if ( isPolylineSubDrag || isMultiDrag ) {
        constexpr double TARGET_TIME_EPSILON = 1e-7;
        const double     appliedTargetTime =
            movesWholeObjects ? refInitialTime + deltaT : targetTime;
        if ( m_hasLastAppliedDragTarget &&
             std::abs(m_lastAppliedDragTargetTime - appliedTargetTime) <=
                 TARGET_TIME_EPSILON &&
             m_lastAppliedDragTargetTrack == targetTrack ) {
            return;
        }
        m_hasLastAppliedDragTarget   = true;
        m_lastAppliedDragTargetTime  = appliedTargetTime;
        m_lastAppliedDragTargetTrack = targetTrack;
    }

    if ( isPolylineSubDrag ) {
        auto* note = ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
        if ( !note ) return;

        const auto& initState = m_initialStates[ctx.draggedEntity];
        // 局部编辑也以手势开始时的子列表为基准，避免反复累计位移。
        // 后续涉及当前与相邻段的联动，应使用同一份初始结构。
        const auto& initSubNotes = initState.note.m_subNotes;
        int         subIdx       = ctx.draggedSubIndex;
        const auto& initSub      = initSubNotes[subIdx];

        if ( initSub.type == ::MMM::NoteType::HOLD ) {
            // 拖动内部 Hold 身体改变该段及后续段的轨道，时间保持不动。
            // 限制共同轨差以保留后续折线的横向结构。
            int localDelta = targetTrack - initSub.trackIndex;

            for ( size_t j = subIdx; j < initSubNotes.size(); ++j ) {
                // 后缀使用同一个轨差，前缀不参加这次整体横移。
                // 边界检验针对玩家域；不能把此分支直接复用到有符号草稿轨。
                int newTrack = initSubNotes[j].trackIndex + localDelta;
                if ( newTrack < 0 ) localDelta = -initSubNotes[j].trackIndex;
                if ( newTrack >= ctx.trackCount )
                    localDelta =
                        ctx.trackCount - 1 - initSubNotes[j].trackIndex;
                if ( initSubNotes[j].type == ::MMM::NoteType::FLICK ) {
                    // Flick 的箭头端点也占轨道范围，不能只限制其起点。
                    // 此处保留现有逐段限制顺序，不把校验改成独立逐对象钳制。
                    int endTrack = newTrack + initSubNotes[j].dtrack;
                    if ( endTrack < 0 )
                        localDelta = -(initSubNotes[j].trackIndex +
                                       initSubNotes[j].dtrack);
                    if ( endTrack >= ctx.trackCount )
                        localDelta = ctx.trackCount - 1 -
                                     (initSubNotes[j].trackIndex +
                                      initSubNotes[j].dtrack);
                }
            }
            if ( subIdx > 0 &&
                 initSubNotes[subIdx - 1].type == ::MMM::NoteType::FLICK ) {
                // 若前段是 Flick，其连接终点会随当前 Hold 的轨道变化。
                // 把前段终点也纳入边界限制，避免连接处被推到玩家区外。
                int prevEnd = initSubNotes[subIdx - 1].trackIndex +
                              initSubNotes[subIdx - 1].dtrack + localDelta;
                if ( prevEnd < 0 )
                    localDelta = -(initSubNotes[subIdx - 1].trackIndex +
                                   initSubNotes[subIdx - 1].dtrack);
                if ( prevEnd >= ctx.trackCount )
                    localDelta = ctx.trackCount - 1 -
                                 (initSubNotes[subIdx - 1].trackIndex +
                                  initSubNotes[subIdx - 1].dtrack);
            }

            // 从当前子段起统一应用轨差，前面的节点位置保持不变。
            // 前一 Flick 只改变轨差，让它的末端继续接到移动后的段。
            for ( size_t j = subIdx; j < note->m_subNotes.size(); ++j ) {
                note->m_subNotes[j].trackIndex =
                    initSubNotes[j].trackIndex + localDelta;
            }
            if ( subIdx > 0 &&
                 note->m_subNotes[subIdx - 1].type == ::MMM::NoteType::FLICK ) {
                // 前一 Flick 起轨固定，仅延伸或缩回连接当前段的箭头。
                // 轨差可以暂时变为零，结束时再进行结构合并。
                note->m_subNotes[subIdx - 1].dtrack =
                    initSubNotes[subIdx - 1].dtrack + localDelta;
            }
        } else if ( initSub.type == ::MMM::NoteType::FLICK ) {
            // 内部 Flick 身体沿时间移动当前和后续子段，不改变横向轨差。
            // 位移与初始子项时间比较，避免拖动多帧累积误差。
            double localDelta = targetTime - initSub.timestamp;

            if ( subIdx > 0 &&
                 initSubNotes[subIdx - 1].type == ::MMM::NoteType::HOLD ) {
                // 当前段提前时会缩短前一个 Hold，不能把它缩成负持续时间。
                // 零持续允许暂存，拖动结束再由退化段清理流程处理。
                // 负前段持续值会反向限制允许位移；这里沿用输入组件的几何关系。
                // 正常非负前段允许最多提前其整个长度，连接点不能越过前段起点。
                double minDt = -initSubNotes[subIdx - 1].duration;
                if ( localDelta < minDt ) localDelta = minDt;
            }
            for ( size_t j = subIdx; j < initSubNotes.size(); ++j ) {
                if ( initSubNotes[j].timestamp + localDelta < 0.0 )
                    localDelta = -initSubNotes[j].timestamp;
            }

            for ( size_t j = subIdx; j < note->m_subNotes.size(); ++j ) {
                // 只改后缀时间，后缀各段持续值保持初始语义。
                // 相邻前段的长度调整在循环之后执行，不按后缀元素数重复累加。
                note->m_subNotes[j].timestamp =
                    initSubNotes[j].timestamp + localDelta;
            }
            if ( subIdx > 0 &&
                 note->m_subNotes[subIdx - 1].type == ::MMM::NoteType::HOLD ) {
                // 前一 Hold 末端须与已移动 Flick 起点衔接。
                // 基于初始时长加总时间差，可在指针反向移动时恢复原长度。
                note->m_subNotes[subIdx - 1].duration =
                    initSubNotes[subIdx - 1].duration + localDelta;
            }
        }

        // 内嵌子列表已修改后同步 ECS 子实体，保持渲染和拾取状态一致。
        // 不能只改父列表而让独立子实体继续留在旧坐标。
        syncPolylineSubEntities(ctx, ctx.draggedEntity, *note);

        if ( auto* trans = ctx.noteRegistry.try_get<TransformComponent>(
                 ctx.draggedEntity) ) {
            // 局部内部段编辑后仍更新父实体缓存 X，使后续渲染入口读取一致位置。
            // 子段具体几何来自刚同步的父列表及 ECS 子项。
            trans->m_pos.x = leftX + note->m_trackIndex * singleTrackW;
        }
    } else if ( isMultiDrag ) {
        // 预检查增量限制
        for ( const auto& [entity, state] : m_initialStates ) {
            // 先根据所有参与对象计算允许的共同位移，再统一写入。
            // 检查包括内嵌子项，确保整条折线的末梢也不越出玩家轨道。
            /// @brief 用一个初始几何范围收紧整组允许位移。
            /// @param type 决定是否计入 Flick 终点的物件类型。
            /// @param t 初始秒时间，用于限制组时间不得越过零。
            /// @param track 初始起轨，采用玩家域编号。
            /// @param dtrack 初始 Flick 轨差，其他类型忽略。
            /// @warning 每次普通组拖动调用；只更新捕获的增量，不分配资源。
            auto check =
                [&](::MMM::NoteType type, double t, int track, int dtrack) {
                    if ( t + deltaT < 0.0 ) deltaT = -t;

                    int trackL = track;
                    int trackR = track;
                    // Flick 左右范围取起点与终点的较小/较大值，兼容负轨差。
                    // 按这两个边界限制组位移，不假设箭头总是向右。
                    if ( type == ::MMM::NoteType::FLICK ) {
                        trackL = std::min(track, track + dtrack);
                        trackR = std::max(track, track + dtrack);
                    }

                    if ( trackL + deltaTrack < 0 ) deltaTrack = -trackL;
                    if ( trackR + deltaTrack >= ctx.trackCount )
                        deltaTrack = ctx.trackCount - 1 - trackR;
                };

            // 初始映射中的子实体也可能被单独检查，校验不写回组件。
            // 父内嵌列表仍需遍历，不能依赖所有子实体都存在于映射。
            const auto& n = state.note;
            check(n.m_type, n.m_timestamp, n.m_trackIndex, n.m_dtrack);
            for ( const auto& sub : n.m_subNotes ) {
                check(sub.type, sub.timestamp, sub.trackIndex, sub.dtrack);
            }
        }

        // 应用变更
        for ( auto& [entity, state] : m_initialStates ) {
            if ( auto* note =
                     ctx.noteRegistry.try_get<NoteComponent>(entity) ) {
                // 应用阶段使用初始组件加共同增量，保留组内相对排列。
                // 父折线内嵌子项也使用同一时间与轨道位移。
                note->m_timestamp  = state.note.m_timestamp + deltaT;
                note->m_trackIndex = state.note.m_trackIndex + deltaTrack;

                // 同步子物件 (Polylines 内部向量)
                for ( size_t i = 0; i < note->m_subNotes.size(); ++i ) {
                    note->m_subNotes[i].timestamp =
                        state.note.m_subNotes[i].timestamp + deltaT;
                    note->m_subNotes[i].trackIndex =
                        state.note.m_subNotes[i].trackIndex + deltaTrack;
                }

                // 更新 Transform
                if ( auto* trans = ctx.noteRegistry.try_get<TransformComponent>(
                         entity) ) {
                    trans->m_pos.x = leftX + note->m_trackIndex * singleTrackW;
                }
            }
        }
    } else {
        // 单个物件特殊部位拖拽 (维持原逻辑，并增加 PolylineNode 支持)
        if ( auto* note =
                 ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity) ) {
            if ( note->m_type == ::MMM::NoteType::HOLD &&
                 ctx.draggedPart == HoverPart::HoldEnd ) {
                // 拖 Hold 尾部只改持续时间，头部时间保持当前起点。
                // 鼠标早于头部时钳制为零，不自动翻转成负向长条。
                note->m_duration =
                    std::max(0.0, targetTime - note->m_timestamp);
            } else if ( note->m_type == ::MMM::NoteType::FLICK &&
                        ctx.draggedPart == HoverPart::FlickArrow ) {
                note->m_dtrack = targetTrack - note->m_trackIndex;
                // 指针仍在起轨时用相对轨道中心的左右位置决定暂时方向。
                // 随后仍按玩家边界钳制，所以边缘条件可能再次得到零轨差。
                if ( note->m_dtrack == 0 ) {
                    note->m_dtrack =
                        (cmd.mouseX > leftX +
                                          note->m_trackIndex * singleTrackW +
                                          singleTrackW / 2.0f)
                            ? 1
                            : -1;
                }
                // 允许的轨差由根轨到玩家左右边界推导，确保箭头不越界。
                // 此处不移动根节点，属于局部箭头编辑。
                note->m_dtrack =
                    std::clamp(note->m_dtrack,
                               -note->m_trackIndex,
                               ctx.trackCount - 1 - note->m_trackIndex);
            } else if ( ctx.draggedPart == HoverPart::PolylineNode &&
                        ctx.draggedSubIndex >= 0 &&
                        ctx.draggedSubIndex < (int)note->m_subNotes.size() ) {
                // 拖拽单个 Polyline 节点
                // 单节点分支直接修改所抓取节点，时间不得为负。
                // 该路径是否进入由前面的整体/局部判定决定，不等同于所有节点拖动。
                auto& sub      = note->m_subNotes[ctx.draggedSubIndex];
                sub.timestamp  = std::max(0.0, targetTime);
                sub.trackIndex = targetTrack;

                // 同步更新实际的子物件实体 (如果是单点拖拽)
                // 现有实现按父身份和子索引寻找对应 ECS 子实体，并在找到后停止。
                // 此路径当前未使用父到子实体索引，不能将这段关联查找视为常量开销。
                auto subView = ctx.noteRegistry.view<NoteComponent>();
                for ( auto subEnt : subView ) {
                    auto& subNC = subView.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == ctx.draggedEntity &&
                         subNC.m_subIndex == ctx.draggedSubIndex ) {
                        // 这里只同步被抓节点的位置字段，不把一次节点移动当作结构重建。
                        // 类型、绑定和子实体身份沿用原值。
                        subNC.m_timestamp  = sub.timestamp;
                        subNC.m_trackIndex = sub.trackIndex;
                        break;
                    }
                }

                // 注意：这里可能需要同步父 note 的基础位置，如果该节点是 Head
                // 根节点改变时父基础时间与轨道也必须跟随，保持排序和定位入口一致。
                // 非根节点编辑不能改写父起点。
                if ( ctx.draggedSubIndex == 0 ) {
                    note->m_timestamp  = sub.timestamp;
                    note->m_trackIndex = sub.trackIndex;
                }
            }
        }
    }
}

/// @brief 校验统一拖动落点，并整体提交跨域转换或恢复初始状态。
/// @param ctx 包含拖动预览结果的会话。
/// @warning
/// 释放低频路径：可遍历参与集合、解析内存资源表和构造撤销批次；不得逐帧调用。
/// @note 转换先准备值对象，所有参与对象都合法后才分配目标实体并提交。
/// @note 任一转换失败回退整组，不只取消失败对象。
/// @note 对已被外部删除的实体不重建；操作只覆盖仍存在的参与者。
/// @note 目标实体在校验后创建，跨 Registry 的删除与创建组成同一步撤销。
/// @pre 拖动预览和释放校验串行执行，准备计划后不得并发改变源组件。
/// @note 资源类型查询只用于决定转换许可，不在此加载或解码音频。
/// @note 转换不保证保留各领域独有样式；这里只搬运两类对象共有的播放字段。
void GrabTool::finishUnifiedDrag(SessionContext& ctx)
{
    /// @brief 一个 Note 删除与目标采样创建的准备记录。
    struct NoteToSampleConversion {
        /// @brief 源 Note Registry 的本地实体身份。
        entt::entity source{ entt::null };
        /// @brief 已验证的目标采样值，不引用源组件。
        SampleComponent target;
        /// @brief 原对象的选中状态，创建目标采样时恢复。
        bool selected{ false };
    };
    /// @brief 一个采样删除与目标 Note 创建的准备记录。
    struct SampleToNoteConversion {
        /// @brief 源 Sample Registry 的本地实体身份。
        entt::entity source{ entt::null };
        /// @brief 已验证的目标 Note 值，不引用源组件。
        NoteComponent target;
        /// @brief 原采样的选中状态，创建目标 Note 时恢复。
        bool selected{ false };
    };

    // 先记录两种方向的转换计划，校验阶段不修改 Registry 结构。
    // 转换身份集合让后面的原对象批次选择删除而非普通位置更新。
    std::vector<NoteToSampleConversion> noteConversions;
    std::vector<SampleToNoteConversion> sampleConversions;
    std::unordered_set<entt::entity>    convertedNotes;
    std::unordered_set<entt::entity>    convertedSamples;
    std::string                         rejectionReason;

    for ( const auto& [entity, state] : m_initialStates ) {
        const auto* current =
            ctx.noteRegistry.try_get<const NoteComponent>(entity);
        // 转换以根对象为单位，子实体不单独迁移到采样 Registry。
        // 不存在的参与者跳过，释放流程不回收或重新创建外部已删除对象。
        if ( !current || current->m_isSubNote ) continue;

        // 仍在音符域的根也需验证全部子项和终点，跨草稿/玩家结构不能直接保存。
        // 只有进入 BGM 域的根才继续尝试 Note 到采样转换。
        if ( current->m_trackIndex < ctx.trackCount ) {
            if ( !noteFitsTrackDomain(*current, ctx.trackCount) ) {
                rejectionReason =
                    "物件拖动结果跨越草稿与玩家轨道边界，已取消本次操作";
                break;
            }
            continue;
        }

        const std::string reference =
            current->m_sampleBinding
                ? current->m_sampleBinding->m_audioResourceId
                : std::string{};
        const auto* resource  = resolveReferencedAudioResource(ctx, reference);
        const auto  converted = makeAudioSampleFromPlayerNote(
            *current,
            static_cast<std::uint32_t>(current->m_trackIndex),
            resource);
        // 某个对象不能无损转换就拒绝整个手势，不允许组内一部分落地。
        // 此时尚未创建新领域实体，所以失败可以统一恢复初始组件。
        if ( !converted ) {
            rejectionReason =
                "只有未绑定音频或绑定 Effect 的普通 Tap 才能拖入 BGM 轨道区";
            break;
        }
        // 保存转换后的值和原选择标志，避免后续依赖源组件指针。
        // 源身份集合用于在原领域批次中记录删除。
        noteConversions.push_back({ entity, *converted, state.selected });
        convertedNotes.insert(entity);
    }

    // 音符方向通过后再验证采样方向，保留第一个明确拒绝原因。
    // 留在 BGM 域的采样无需转换，只有进入玩家轨的采样需要类型检查。
    if ( rejectionReason.empty() ) {
        for ( const auto& [entity, state] : m_initialSampleStates ) {
            const auto* current =
                ctx.sampleRegistry.try_get<const SampleComponent>(entity);
            if ( !current ||
                 current->m_track >=
                     static_cast<std::uint32_t>(std::max(0, ctx.trackCount)) ) {
                continue;
            }

            const auto* resource =
                resolveReferencedAudioResource(ctx, current->m_audioResourceId);
            const auto converted = makePlayerNoteFromSample(
                *current,
                static_cast<std::int32_t>(current->m_track),
                resource);
            if ( !converted ) {
                rejectionReason =
                    "自动采样仅能在空资源或 Effect 且 offset 为 0 "
                    "时拖入玩家轨道区";
                break;
            }
            // 反向转换计划同样只缓存结果，直到全部参与者验证通过才创建实体。
            // 源采样状态仍可在拒绝时由初始快照恢复。
            sampleConversions.push_back({ entity, *converted, state.selected });
            convertedSamples.insert(entity);
        }
    }

    // 失败回退覆盖参与对象的初始组件，但不复活已经失效的实体。
    // 恢复的是手势前数据，不能只把主要对象弹回而留下组内其他对象。
    /// @brief 将仍有效的参与对象回退到手势初始内容并标记派生缓存失效。
    /// @warning 只在释放拒绝分支调用；遍历参与集合，不阻塞等待其他线程。
    const auto restoreInitialStates = [&]() {
        bool restoredNotes = false;
        for ( const auto& [entity, state] : m_initialStates ) {
            if ( ctx.noteRegistry.valid(entity) &&
                 ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                // 回退恢复完整 Note 值，包括内嵌子项，而非只恢复根时间与轨道。
                // 因此临时跨域草稿标志和局部结构字段也能回到手势前状态。
                ctx.noteRegistry.replace<NoteComponent>(entity, state.note);
                restoredNotes = true;
            }
        }
        for ( const auto& [entity, state] : m_initialSampleStates ) {
            if ( ctx.sampleRegistry.valid(entity) &&
                 ctx.sampleRegistry.all_of<SampleComponent>(entity) ) {
                ctx.sampleRegistry.replace<SampleComponent>(entity,
                                                            state.sample);
            }
        }
        if ( restoredNotes ) {
            // 拖动预览期间可见性索引仍保留旧位置；回弹后必须令时间排序、
            // Polyline 包围区间和 AbsY 分桶一起失效，否则清除拖动固定列表后
            // 大折线可能继续按非法落点剔除，表现为整个物件消失。
            ctx.isNoteOrderDirty             = true;
            ctx.isNoteStatsDirty             = true;
            ctx.isAnnotationRenderCacheDirty = true;
        }
        // 组件回退后标记位置缓存重算，避免画面继续使用最后非法目标的坐标。
        // 即使没有有效 Note 被恢复，采样回退也需要后续更新路径看到状态变化。
        ctx.isTransformDirty = true;
    };

    // 无论提交还是回退都先清除参与者的拖动外观。
    // 组件快照恢复与 Action 执行负责内容，交互拖动态不应留给下一次手势。
    clearDraggingFlags(ctx, m_initialStates);
    clearSampleDraggingFlags(ctx, m_initialSampleStates);

    // 拒绝时只回退并显示原因，不往撤销栈追加一个失败操作。
    // 成功分支则读取当前预览作为 after，与手势初始 before 配对。
    if ( !rejectionReason.empty() ) {
        restoreInitialStates();
        ctx.lastActionMessage = std::move(rejectionReason);
    } else {
        std::vector<BatchNoteAction::Entry>   noteEntries;
        std::vector<BatchSampleAction::Entry> sampleEntries;
        // 容量同时考虑原音符更新/删除和从采样新建的音符。
        // 预先准备容量减少释放事务构造期间的重复扩容，不改变提交顺序。
        noteEntries.reserve(m_initialStates.size() + sampleConversions.size());
        sampleEntries.reserve(m_initialSampleStates.size() +
                              noteConversions.size());

        for ( const auto& [entity, state] : m_initialStates ) {
            const auto* current =
                ctx.noteRegistry.try_get<const NoteComponent>(entity);
            if ( !current ) continue;
            // 跨域转换的原 Note 表现为删除，before 保留原始数据与选择状态。
            // 目标采样在后续独立批次中创建，两者最终合并为同一撤销步骤。
            if ( convertedNotes.contains(entity) ) {
                noteEntries.push_back({
                    .entity         = entity,
                    .before         = state.note,
                    .after          = std::nullopt,
                    .beforeSelected = state.selected,
                });
                // 不转换的对象只在拖动字段有变化时加入更新批次。
                // 这样鼠标回到原处的统一拖动不会生成空历史操作。
            } else if ( !sameDraggedNoteState(state.note, *current) ) {
                noteEntries.push_back({
                    .entity = entity,
                    .before = state.note,
                    .after  = *current,
                });
            }
        }

        for ( const auto& [entity, state] : m_initialSampleStates ) {
            const auto* current =
                ctx.sampleRegistry.try_get<const SampleComponent>(entity);
            if ( !current ) continue;
            // 采样转 Note 同样先删除源领域对象，不能仅更换组件类型。
            // 两个 Registry 的身份空间独立，转换必须由成对删除和创建表达。
            if ( convertedSamples.contains(entity) ) {
                sampleEntries.push_back({
                    .entity         = entity,
                    .before         = state.sample,
                    .after          = std::nullopt,
                    .beforeSelected = state.selected,
                });
                // 采样同样按锚点、偏移和轨号判断是否需要更新。
                // 资源与音量由其他属性命令管理，不属于本手势的变化判断。
            } else if ( !sameDraggedSampleState(state.sample, *current) ) {
                sampleEntries.push_back({
                    .entity = entity,
                    .before = state.sample,
                    .after  = *current,
                });
            }
        }

        // 校验全部通过后才为新采样分配实体号，并继承原对象选择状态。
        // before 为空表示创建，不借用源 Note 的实体号。
        for ( const auto& conversion : noteConversions ) {
            // 此时只预留目标身份，采样组件由后续批次安装。
            // 创建时不复制 Note Registry 的交互组件，选择状态通过条目显式传递。
            const auto targetEntity = ctx.sampleRegistry.create();
            sampleEntries.push_back({
                .entity        = targetEntity,
                .before        = std::nullopt,
                .after         = conversion.target,
                .afterSelected = conversion.selected,
            });
        }
        // 反向转换为音符分配 Note Registry 实体。
        // afterSelected 保持跨域拖动后的选择连续性，供批次执行恢复。
        for ( const auto& conversion : sampleConversions ) {
            // 目标身份与源采样身份无对应数值约束。
            // 后续批次根据空 before 区分创建和更新，撤销时才能删除新对象。
            const auto targetEntity = ctx.noteRegistry.create();
            noteEntries.push_back({
                .entity        = targetEntity,
                .before        = std::nullopt,
                .after         = conversion.target,
                .afterSelected = conversion.selected,
            });
        }

        // 子动作在这里以独占所有权组装，提交后归历史栈持有。
        // 释放前的局部计划只携带值，避免 Action 执行导致源组件引用失效。
        std::vector<std::unique_ptr<IEditorAction>> actions;
        if ( !noteEntries.empty() ) {
            actions.push_back(std::make_unique<BatchNoteAction>(
                std::move(noteEntries), "统一画布物件移动"));
        }
        // 只有单采样原地更新且没有音符动作时使用单条 SampleAction。
        // 涉及删除、创建或多个领域时继续使用批次，避免丢失转换语义。
        if ( sampleEntries.size() == 1 && sampleEntries.front().before &&
             sampleEntries.front().after && actions.empty() ) {
            // 提取条目后不再访问其已移动的前后快照。
            // 动作取得快照所有权，工具清空手势缓存不会影响撤销数据。
            auto entry = std::move(sampleEntries.front());
            actions.push_back(
                std::make_unique<SampleAction>(SampleAction::Type::Update,
                                               entry.entity,
                                               std::move(entry.before),
                                               std::move(entry.after)));
        } else if ( !sampleEntries.empty() ) {
            actions.push_back(std::make_unique<BatchSampleAction>(
                std::move(sampleEntries), "统一画布自动采样移动"));
        }

        // 一个领域的动作直接入栈，多个领域用组合操作保持一次撤销。
        // 完全没有实际变化时不提交空 Action。
        if ( actions.size() == 1 ) {
            ctx.actionStack.pushAndExecute(std::move(actions.front()), ctx);
        } else if ( actions.size() > 1 ) {
            ctx.actionStack.pushAndExecute(
                std::make_unique<CompositeEditorAction>(
                    std::move(actions), "玩家物件与纯采样跨区转换"),
                ctx);
        }
    }

    // 最终内容确定后统一重建打击事件，回退结果也需要刷新播放派生状态。
    // 随后解除渲染保留列表和手势快照，不再以临时预览参与后续帧。
    SessionUtils::rebuildHitEvents(ctx);
    clearSessionDragState(ctx);
    ctx.draggedEntity     = entt::null;
    ctx.draggedObjectKind = ChartObjectKind::PlayerNote;
    ctx.dragInitialNote.reset();
    ctx.dragInitialSample.reset();
    ctx.dragRenderPinnedEntities.clear();
    ctx.dragSampleRenderPinnedEntities.clear();
    // 历史动作已持有必要 before/after，工具不再保留旧手势副本。
    // 同时重置去重目标，下一次拖动不能因坐标相同被误跳过。
    m_initialStates.clear();
    m_initialSampleStates.clear();
    m_usesUnifiedObjectDrag    = false;
    m_isSampleOffsetDrag       = false;
    m_hasLastAppliedDragTarget = false;
}

/// @brief 结束物件移动或局部编辑拖拽并提交动作。
/// @param ctx 会话上下文。
/// @param cmd 拖拽结束命令。
/// @warning 释放低频路径：汇总参与组件并构造历史动作，随后刷新打击事件。
/// @note cmd 当前不提供最终坐标，最后一次更新已写入的组件即为提交目标。
/// @note 普通位置提交分支没有精确未变过滤，行为不同于统一跨域提交。
/// @note 活动身份为空时直接返回；调用者不能用重复释放代替开始失败的清理。
void GrabTool::handleEndDrag(SessionContext& ctx, const CmdEndDrag& cmd)
{
    (void)cmd;
    if ( ctx.draggedEntity == entt::null ) return;

    // 跨域或采样手势由专用收尾统一校验并提交。
    // 返回后不再走普通音符批次，避免同一拖动重复进入历史。
    if ( m_usesUnifiedObjectDrag ||
         ctx.draggedObjectKind == ChartObjectKind::AudioSample ) {
        finishUnifiedDrag(ctx);
        return;
    }

    // 结构清理若已经提交，普通位置更新不能再覆盖新的父子关系。
    // 只补齐会话级手势清理，然后结束本次释放。
    if ( m_isPolylineSubDrag && tryPolylineSubDragMerge(ctx) ) {
        m_isPolylineSubDrag        = false;
        m_hasLastAppliedDragTarget = false;
        // 清除会话归属后释放主要对象初始副本和参与集合。
        // 结束后的下一次拖动必须重新采样起点，不能复用上一手势缓存。
        clearSessionDragState(ctx);
        ctx.dragRenderPinnedEntities.clear();
        ctx.dragSampleRenderPinnedEntities.clear();
        ctx.draggedObjectKind = ChartObjectKind::PlayerNote;
        ctx.dragInitialSample.reset();
        return;
    }

    std::vector<BatchNoteAction::Entry> entries;

    for ( auto& [entity, state] : m_initialStates ) {
        if ( ctx.noteRegistry.valid(entity) ) {
            if ( ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
                ctx.noteRegistry.get<InteractionComponent>(entity).isDragging =
                    false;
            }

            auto* currentNote = ctx.noteRegistry.try_get<NoteComponent>(entity);
            if ( currentNote ) {
                // 提交变更
                // before 始终来自按下时快照，after 取当前预览值。
                // 这一普通分支沿用已有行为收集有效参与者，不在这里另做结构合并。
                entries.push_back({ entity, state.note, *currentNote });
            }
        }
    }

    // 把整组位置编辑作为一个 BatchNoteAction 提交。
    // 参与对象已删除时跳过，无有效条目则不创建空批次。
    if ( !entries.empty() ) {
        auto action = std::make_unique<BatchNoteAction>(std::move(entries));
        ctx.actionStack.pushAndExecute(std::move(action), ctx);
    }

    SessionUtils::rebuildHitEvents(ctx);

    clearSessionDragState(ctx);
    ctx.draggedEntity     = entt::null;
    ctx.draggedObjectKind = ChartObjectKind::PlayerNote;
    ctx.dragInitialNote   = std::nullopt;
    ctx.dragInitialSample.reset();
    m_hasLastAppliedDragTarget = false;
    ctx.dragRenderPinnedEntities.clear();
    ctx.dragSampleRenderPinnedEntities.clear();
    m_initialStates.clear();
    m_initialSampleStates.clear();
}

/// @brief 将父折线内嵌子项覆盖到已有 ECS 子实体。
/// @param ctx 包含父子实体的会话。
/// @param parent 要同步的父实体身份。
/// @param note 已更新的父组件，调用期间保持有效。
/// @warning 拖动调用链中的既有扫描点：当前完整遍历 NoteComponent 查找子实体；
/// 该实现不具备常量查找成本，禁止再加入排序、文件访问或阻塞操作。
/// @note 不创建缺失子实体、不删除失效索引的子实体，结构维护由结束批次完成。
/// @note 同步属性不包含父子身份字段，既有 entity 与 subIndex 保持不变。
/// @pre 父组件内嵌列表在整个同步循环中保持结构稳定。
/// @note 子实体的 Transform 不在此写回，由相应位置更新流程处理。
void GrabTool::syncPolylineSubEntities(SessionContext& ctx, entt::entity parent,
                                       const NoteComponent& note)
{
    auto subView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto subEnt : subView ) {
        auto& subNC = subView.get<NoteComponent>(subEnt);
        if ( !subNC.m_isSubNote || subNC.m_parentPolyline != parent ) continue;
        // 旧子索引可能因结构变化不再有效，跳过而不越界访问父列表。
        // 本函数只同步现有合法子项，新增或删除实体由结构收尾负责。
        if ( subNC.m_subIndex < 0 ||
             subNC.m_subIndex >= (int)note.m_subNotes.size() )
            continue;

        // 父列表是几何和属性的权威来源，同步时连同草稿状态、绑定和颜色覆盖一起复制。
        // 子实体保留自己的父身份与索引，不重建本地实体号。
        // 读取父子属性期间不能插入或删除 NoteComponent，父观察引用需保持有效。
        // 本助手只赋值既有组件，新增和删除工作保留给释放阶段。
        const auto& sub       = note.m_subNotes[subNC.m_subIndex];
        subNC.m_type          = sub.type;
        subNC.m_timestamp     = sub.timestamp;
        subNC.m_duration      = sub.duration;
        subNC.m_trackIndex    = sub.trackIndex;
        subNC.m_dtrack        = sub.dtrack;
        subNC.m_isDraft       = note.m_isDraft;
        subNC.m_metadata      = sub.metadata;
        subNC.m_sampleBinding = sub.sampleBinding;
        subNC.m_customColors  = sub.customColors;
    }
}

/// @brief 为局部子段拖动收尾执行退化清理和子实体重组。
/// @param ctx 当前活动拖动会话。
/// @return 已清理并提交结构变化时返回 true，否则由普通收尾继续处理。
/// @warning 手势结束低频路径：包含子实体扫描、稳定排序和 Action
/// 构造，禁止连续更新调用。
/// @note 返回 true 后父与子结构已经通过批次提交，调用者仅做剩余手势清理。
/// @note 返回 false 不代表拖动失败，而是没有结构清理需要接管收尾。
/// @note 相同几何并不代表相同来源，子实体复用以 sourceIndex 为准。
/// @note 清理结果先保存在局部值中，生成完父子历史条目后才执行批次。
/// @note 资源与颜色继承由父降级助手决定，本函数负责把结果映射到实体历史。
bool GrabTool::tryPolylineSubDragMerge(SessionContext& ctx)
{
    if ( ctx.draggedEntity == entt::null ) return false;

    auto* note = ctx.noteRegistry.try_get<NoteComponent>(ctx.draggedEntity);
    if ( !note || note->m_type != ::MMM::NoteType::POLYLINE ) return false;

    int subIdx = ctx.draggedSubIndex;
    // 只有内部子段手势进入此结构清理，根部拖动留给普通提交。
    // 索引必须仍落在当前子列表中，不能根据旧悬浮索引越界。
    if ( subIdx <= 0 || subIdx >= static_cast<int>(note->m_subNotes.size()) ) {
        return false;
    }

    // 结构清理必须具备父 before，否则无法形成可靠撤销。
    // 找不到初始状态时返回未接管，由普通结束路径处理其余清理。
    auto initIt = m_initialStates.find(ctx.draggedEntity);
    if ( initIt == m_initialStates.end() ) return false;

    bool cleanedChanged = false;
    auto cleanedSegments =
        cleanPolylineSubSegments(note->m_subNotes, cleanedChanged);
    // 没有退化或合并就交还普通拖动收尾，不额外生成结构历史。
    // 清理函数返回独立结果，这之前的当前父组件仍是拖动预览。
    if ( !cleanedChanged ) return false;

    // 父 before 来自拖动前，after 从当前预览叠加结构清理。
    // 因此一次撤销能够同时恢复位置与被合并/删除的子段。
    NoteComponent parentBefore = initIt->second.note;
    NoteComponent parentAfter  = *note;
    applyCleanedPolylineSubSegments(parentAfter, cleanedSegments);

    /// @brief 清理前子实体及其撤销快照。
    struct ChildRecord {
        /// @brief 待复用或删除的子实体。
        entt::entity entity{ entt::null };
        /// @brief 清理前子索引，用于匹配保留段的来源。
        int oldSubIndex{ -1 };
        /// @brief 撤销时恢复的完整组件。
        NoteComponent before;
        /// @brief 本次重组是否复用该实体；未复用者进入删除批次。
        bool kept{ false };
    };

    std::vector<BatchNoteAction::Entry> entries;
    // 父更新先进入批次，后面的子实体变化与它共用一次提交。
    // 父降级时保留父实体号，旧子实体则在未复用分支中统一删除。
    entries.push_back({ ctx.draggedEntity, parentBefore, parentAfter });

    std::vector<ChildRecord> children;
    auto                     subView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto subEnt : subView ) {
        const auto& subNC = subView.get<NoteComponent>(subEnt);
        if ( !subNC.m_isSubNote || subNC.m_parentPolyline != ctx.draggedEntity )
            continue;

        // 优先使用手势前子实体快照；未被初始集合覆盖的实体采用当前组件。
        // 同时保留旧子索引，用来匹配清理结果的 sourceIndex。
        auto          initSubIt = m_initialStates.find(subEnt);
        NoteComponent before    = (initSubIt != m_initialStates.end())
                                      ? initSubIt->second.note
                                      : subNC;
        // 局部记录持有完整 before 值，后续排序仅移动这些记录。
        // Registry 组件不会随着工作列表排序改变顺序或身份。
        children.push_back(
            { subEnt, before.m_subIndex, std::move(before), false });
    }

    // 按旧索引排序使子实体复用顺序稳定，不依赖 Registry 遍历次序。
    // 排序仅在释放时进行，不进入连续拖动更新。
    // 同旧索引的记录保持收集顺序，比较器不额外按实体数值排序。
    // 该排序为后续来源查找提供稳定顺序，不承担修复重复索引的职责。
    std::stable_sort(children.begin(),
                     children.end(),
                     [](const ChildRecord& lhs, const ChildRecord& rhs) {
                         return lhs.oldSubIndex < rhs.oldSubIndex;
                     });

    // 仍有多个子项才继续保留子实体；父降级为独立物件时不复用任何子实体。
    // 新子索引按清理后的顺序写入，原始来源只用于选择可复用实体。
    if ( parentAfter.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( std::size_t newIndex = 0; newIndex < cleanedSegments.size();
              ++newIndex ) {
            // 来源下标指向清理前子项，newIndex 只描述清理后的排列。
            // 把两者混用会复用错误子实体，使撤销前状态与当前段错配。
            auto childIt =
                std::find_if(children.begin(),
                             children.end(),
                             [&](const ChildRecord& child) {
                                 return child.oldSubIndex ==
                                        cleanedSegments[newIndex].sourceIndex;
                             });

            // 从最终父子列表构造新子组件，写入新的连续 subIndex。
            // 草稿标志继承父域，不能继续沿用清理前旧子实体的域状态。
            NoteComponent after =
                makeNoteComponentFromSubNote(parentAfter.m_subNotes[newIndex],
                                             true,
                                             ctx.draggedEntity,
                                             static_cast<int>(newIndex));
            after.m_isDraft = parentAfter.m_isDraft;

            // 找到原来源实体就更新并标记保留，尽量维持本地身份。
            // 找不到时才分配新实体，以 before 为空表示创建。
            if ( childIt != children.end() ) {
                // 保留标记只属于本次工作列表，不写入实体组件。
                // 末尾删除阶段据此排除已复用身份，避免同一批次先更新又删除。
                childIt->kept = true;
                entries.push_back({ childIt->entity, childIt->before, after });
            } else {
                // 原列表可能有子段数据却没有对应 ECS 实体，此时补齐新身份。
                // before 为空保留这一差异，撤销无需伪造一个原本不存在的子实体。
                entt::entity newChild = ctx.noteRegistry.create();
                entries.push_back({ newChild, std::nullopt, after });
            }
        }
    }

    for ( const auto& child : children ) {
        // 未复用的旧子实体以删除条目进入同一批次。
        // 父降级或多个子段合并都通过这一出口清理多余实体。
        // 降级后没有任何子项被标记保留，所有旧子实体都进入删除记录。
        // 保留根实体与删除子实体一起提交，防止撤销只恢复半条折线。
        if ( !child.kept ) {
            entries.push_back({ child.entity, child.before, std::nullopt });
        }
    }

    if ( !entries.empty() ) {
        clearDraggingFlags(ctx, m_initialStates);
        // 父更新、子更新、创建和删除在一个批次提交，撤销可恢复完整结构。
        // 提交前清除拖动反馈，避免删除实体后再尝试清理其交互组件。
        auto action = std::make_unique<BatchNoteAction>(
            std::move(entries), "Polyline Sub-Drag Merge");
        ctx.actionStack.pushAndExecute(std::move(action), ctx);
    }

    SessionUtils::rebuildHitEvents(ctx);

    // 结构清理已经接管并完成提交，清除主身份避免外层再次生成普通位置批次。
    // 剩余会话相机和模式标志由调用方的成功分支清理。
    ctx.draggedEntity   = entt::null;
    ctx.dragInitialNote = std::nullopt;
    m_initialStates.clear();

    return true;
}

}  // namespace MMM::Logic
