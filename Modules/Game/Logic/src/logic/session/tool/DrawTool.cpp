#include "logic/session/tool/DrawTool.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace MMM::Logic
{

namespace
{
/// @brief 最小允许放置物件的时间戳，单位秒。
constexpr double MIN_PLACEABLE_NOTE_TIME = 0.0;

/// @brief 判断绘制工具是否允许在指定时间创建物件。
/// @param time 待检查时间戳，单位秒。
/// @return 时间有限且不早于 0 秒时返回 true。
/// @note 明确拒绝 NaN 和无穷，单独判断非负不能排除正无穷。
/// @warning 创建与画笔更新调用链：固定字段判断，不访问外部资源。
/// @note 正零和负零都属于可放置起点；本判断不施加谱面结束时间上限。
bool isPlaceableNoteTime(double time)
{
    return std::isfinite(time) && time >= MIN_PLACEABLE_NOTE_TIME;
}

/// @brief 判断即将创建的物件及其子物件是否都位于可放置时间范围内。
/// @param note 待检查物件。
/// @return 所有时间戳均有效且不为负时返回 true。
/// @note 这里只校验起始时间，不代替持续长度、轨道域或资源绑定校验。
/// @note 仅 Polyline 类型遍历内嵌子项，其他类型按根时间判断。
/// @warning 提交阶段遍历当前画笔的局部列表，不遍历全部谱面。
/// @note 空子列表不会单独导致拒绝，折线是否需要降级由提交清理阶段决定。
bool isPlaceableNote(const NoteComponent& note)
{
    // 先验证根时间，再读取折线局部子项；任何一项非法都拒绝整个结果。
    // 不把负时间逐项钳制为零，以免改变已经画出的内部时间关系。
    if ( !isPlaceableNoteTime(note.m_timestamp) ) {
        return false;
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( const auto& subNote : note.m_subNotes ) {
            if ( !isPlaceableNoteTime(subNote.timestamp) ) {
                return false;
            }
        }
    }

    return true;
}

/// @brief 判断当前悬停对象是否为玩家物件注册表中的有效 Note。
/// @param ctx 会话上下文。
/// @return 悬停对象属于玩家物件域且实体有效时返回 true。
/// @note 函数名沿用玩家物件称呼，实际也接受同一 Registry 中的可编辑草稿。
/// @warning 输入路由热路径：检查单个身份及配置门禁，不遍历 ECS。
bool isHoveredPlayerNote(const SessionContext& ctx)
{
    // 先按领域解释实体号，不能用采样 Registry 中的同号实体读取 Note。
    // 实体有效仍不足以放行，还需存在目标组件并满足当前编辑模式。
    if ( (ctx.hoveredObjectKind != ChartObjectKind::PlayerNote &&
          ctx.hoveredObjectKind != ChartObjectKind::DraftNote) ||
         ctx.hoveredEntity == entt::null ||
         !ctx.noteRegistry.valid(ctx.hoveredEntity) ||
         !ctx.noteRegistry.all_of<NoteComponent>(ctx.hoveredEntity) ) {
        return false;
    }
    return SessionUtils::isNoteEditable(
        ctx.noteRegistry.get<const NoteComponent>(ctx.hoveredEntity),
        ctx.lastConfig.settings);
}

/// @brief 判断当前悬停对象是否为自动采样注册表中的有效物件。
/// @param ctx 会话上下文。
/// @return 悬停对象属于自动采样域且实体有效时返回 true。
/// @note 只判断对象领域与组件存在性，不检查采样引用的资源是否已加载。
/// @warning 输入路由热路径：固定身份查询，禁止加载音频。
bool isHoveredAudioSample(const SessionContext& ctx)
{
    return ctx.hoveredObjectKind == ChartObjectKind::AudioSample &&
           ctx.hoveredEntity != entt::null &&
           ctx.sampleRegistry.valid(ctx.hoveredEntity) &&
           ctx.sampleRegistry.all_of<SampleComponent>(ctx.hoveredEntity);
}

/// @brief 清理当前绘制笔刷状态。
/// @param ctx 会话上下文引用。
/// @note 清除当前手势的临时绑定，保留用户长期选定的资源与音量。
/// @note 不撤销已提交的替换删除，也不重置会话级拖动归属。
/// @warning 可从无效输入分支调用；不得加入等待或文件访问。
/// @note 已保存的屏幕参考和轨号不会归零；isActive 与负 holdStartTime
/// 控制其可用性。
void resetBrushState(SessionContext& ctx)
{
    // 临时段列表和活动资源只属于一次手势，失败后不能被下一笔继承。
    // 清空 active 字段而保留 selected 字段，使工具选择在取消后仍有效。
    ctx.brushState.polylineSegments.clear();
    ctx.brushState.activeAudioResourceId.clear();
    ctx.brushState.activeSampleBinding.reset();
    ctx.brushState.replacesExistingObject = false;
    ctx.brushState.hasPolylineGesture     = false;
    // 负起点是尚未建立持续段的哨兵，不是可提交时间。
    // isActive 最后置为 false，后续更新入口会停止处理该手势。
    ctx.brushState.holdStartTime      = -1.0;
    ctx.brushState.duration           = 0.0;
    ctx.brushState.dtrack             = 0;
    ctx.brushState.createsAudioSample = false;
    ctx.brushState.isActive           = false;
}

/// @brief 判断批量操作条目中是否已经包含指定实体。
/// @param entries 当前待提交批次，既可包含创建也可包含更新或删除。
/// @param entity 需要查重的本地 Note 身份。
/// @return 只要任一条目使用该身份就返回 true。
/// @note 不区分动作类型，避免给已有更新条目再次追加删除。
/// @warning 删除或合并的低频准备阶段线性查找，不用于连续画笔移动。
/// @note 比较的是实体代际完整值，不能只按裸索引识别删除记录。
bool hasBatchEntryForEntity(const std::vector<BatchNoteAction::Entry>& entries,
                            entt::entity                               entity)
{
    for ( const auto& entry : entries ) {
        if ( entry.entity == entity ) {
            return true;
        }
    }
    return false;
}

/// @brief 将指定折线父实体的所有子物件删除条目追加到批量操作中。
/// @param ctx 会话上下文引用。
/// @param parentEntity 折线父实体。
/// @param entries 待追加的批量操作条目列表。
/// @warning 逻辑热路径低频分支：只在删除/合并折线时执行一次完整 note
/// ECS 遍历，禁止在画笔移动更新中调用。
/// @note 只追加子删除，不为 parentEntity 自动创建父删除条目。
/// @note entries 的既有条目顺序保持不变，新增条目沿用当前 Registry 遍历顺序。
void appendPolylineChildDeleteEntries(
    SessionContext& ctx, entt::entity parentEntity,
    std::vector<BatchNoteAction::Entry>& entries)
{
    // 子实体必须通过父关系查找，父内嵌数组并不直接保存 ECS 身份。
    // 此处只构造删除记录，扫描期间不改变 Registry 结构。
    auto subNoteView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto subEnt : subNoteView ) {
        const auto& subNC = subNoteView.get<NoteComponent>(subEnt);
        if ( subNC.m_isSubNote && subNC.m_parentPolyline == parentEntity &&
             !hasBatchEntryForEntity(entries, subEnt) ) {
            // before 保留完整子组件，撤销父删除时可恢复子关系与属性。
            // nullopt after 表示删除，不是写回一个默认子物件。
            entries.push_back({ subEnt, subNC, std::nullopt });
        }
    }
}

/// @brief 将多个折线父实体的子物件删除条目通过一次 ECS 扫描追加到批量操作中。
/// @param ctx 会话上下文引用。
/// @param parentEntities 需要删除子物件的折线父实体集合。
/// @param entries 待追加的批量操作条目列表。
/// @warning 逻辑热路径低频分支：橡皮擦结束时最多执行一次完整 note ECS
/// 遍历，禁止在擦除拖动更新中调用。
/// @note 父集合以本地实体身份匹配，不按协作 ID 或当前轨道推测归属。
/// @note 若子身份已在 entries 中出现，则无论已有动作类型为何都不再追加删除。
void appendPolylineChildDeleteEntries(
    SessionContext& ctx, const std::unordered_set<entt::entity>& parentEntities,
    std::vector<BatchNoteAction::Entry>& entries)
{
    // 擦除集合没有折线父对象时不做全谱扫描。
    // 多个父对象共用一次查询，避免逐父重复走完整 ECS。
    if ( parentEntities.empty() ) return;

    // 将已有条目身份转为集合，后续对子实体的去重无需重复线性查找。
    // 集合只在本次提交准备期间存在，不维护跨手势的实体缓存。
    std::unordered_set<entt::entity> existingEntries;
    existingEntries.reserve(entries.size());
    for ( const auto& entry : entries ) {
        if ( entry.entity != entt::null ) {
            // 忽略空身份，防止无对象占位条目参与实体去重。
            existingEntries.insert(entry.entity);
        }
    }

    auto subNoteView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto subEnt : subNoteView ) {
        const auto& subNC = subNoteView.get<NoteComponent>(subEnt);
        // 同时筛选子关系、目标父集合和未记录身份。
        // insert 的结果将命中与去重合并，新增条目随即成为后续查重依据。
        if ( subNC.m_isSubNote &&
             parentEntities.find(subNC.m_parentPolyline) !=
                 parentEntities.end() &&
             existingEntries.insert(subEnt).second ) {
            entries.push_back({ subEnt, subNC, std::nullopt });
        }
    }
}
}  // namespace

/// @brief 从点击位置建立普通画笔，或从可续接对象恢复折线编辑。
/// @param ctx 持有配置、相机、活动画笔及历史栈的会话。
/// @param cmd 起始鼠标位置、相机和 Shift 状态。
/// @warning 按下低频路径：续接可复制折线并扫描关联子实体；禁止移入连续更新。
/// @note BGM 区创建采样，玩家与草稿区创建 Note，领域由起始命中确定。
/// @note 续接既有物件先提交删除历史，再由后续画笔结束提交新结果。
/// @pre 会话逻辑线程串行调用，构造删除快照期间组件须保持有效。
/// @note 命中范围以当前相机的投影解释，调用方应传入发起手势的相机标识。
/// @note 普通 Note 续接复制子段属性；新笔的颜色来自画笔设置，两种来源各自保留。
/// @note 返回后画笔可能仍未激活，调用方不能把命令到达视为创建成功。
void DrawTool::handleStartBrush(SessionContext& ctx, const CmdStartBrush& cmd)
{
    // 缺失相机时无法解释鼠标坐标，保持现有状态并忽略此命令。
    // 先解析实际命中领域，再激活画笔，避免空隙点击产生默认零轨物件。
    auto itCamera = ctx.cameras.find(cmd.cameraId);
    if ( itCamera == ctx.cameras.end() ) return;

    // 预览使用自己的像素边距，不采用主画布草稿和 BGM 分区。
    // 两个预览相机标识保持同一输入解释。
    const bool isPreview =
        cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas";
    const auto playerProjection =
        calculatePlayerTrackProjection(itCamera->second.viewportWidth,
                                       ctx.trackCount,
                                       ctx.lastConfig.visual.trackLayout.left,
                                       ctx.lastConfig.visual.trackLayout.right,
                                       itCamera->second.horizontalOffsetX);
    float                            leftX  = playerProjection.leftX;
    float                            rightX = playerProjection.rightX;
    std::optional<CanvasLaneAddress> targetLane;
    std::uint32_t                    projectedDraftLaneCount{ 0U };
    // 预览仅提供玩家轨号，右边缘包含在命中范围内。
    // floor 后钳制保证恰好点击最右边时仍落在最后一轨。
    if ( isPreview ) {
        leftX  = ctx.lastConfig.visual.previewConfig.margin.left;
        rightX = itCamera->second.viewportWidth -
                 ctx.lastConfig.visual.previewConfig.margin.right;
        if ( cmd.mouseX >= leftX && cmd.mouseX <= rightX &&
             ctx.trackCount > 0 ) {
            // 轨宽由预览可绘制区和玩家轨数决定，不能借用主画布单轨宽度。
            // 此分支依赖配置提供有效的非零预览宽度。
            const float laneWidth =
                (rightX - leftX) / static_cast<float>(ctx.trackCount);
            const auto laneIndex = static_cast<std::uint32_t>(std::clamp(
                static_cast<int>(std::floor((cmd.mouseX - leftX) / laneWidth)),
                0,
                ctx.trackCount - 1));
            targetLane = CanvasLaneAddress{ CanvasLaneKind::Player, laneIndex };
        }
    } else {
        // 主画布按独立领域几何严格命中，间隙和非物件区域不创建画笔。
        // 草稿轨数量随投影保存，后面绝对轨号换算必须使用同一份计数。
        const auto laneProjection = calculateCanvasLaneProjection(
            itCamera->second.viewportWidth,
            ctx.trackCount,
            ctx.bgmTrackCount,
            ctx.lastConfig.visual.trackLayout,
            itCamera->second.horizontalOffsetX,
            true,
            ctx.lastConfig.settings.enableBmsEditing,
            ctx.lastConfig.settings.professionalMode,
            ctx.draftTrackCount,
            true);
        projectedDraftLaneCount = laneProjection.draftLaneCount;
        targetLane              = laneProjection.laneAt(cmd.mouseX);
    }

    // 未命中可放置轨道便返回，不把最近轨道作为新的起笔位置。
    if ( !targetLane ) {
        return;
    }
    // 主音轨资源只能作为自动采样使用，不能附着到点击物件。
    // 门禁在激活之前执行，拒绝时只提供原因而不生成历史。
    const bool createsAudioSample = targetLane->kind == CanvasLaneKind::Bgm;
    if ( !createsAudioSample &&
         !ctx.brushState.selectedAudioResourceId.empty() &&
         ctx.brushState.selectedAudioTrackType ==
             ::MMM::AudioTrackType::Main ) {
        ctx.lastActionMessage = "主音轨不能绑定到玩家物件";
        return;
    }

    // 活动绑定在起笔时从工具选择复制，后续构造结果使用该手势快照。
    // 重置替换标志，只有成功续接并删除原对象后才重新置位。
    ctx.brushState.isActive           = true;
    ctx.brushState.hasPolylineGesture = false;
    ctx.brushState.createsAudioSample = createsAudioSample;
    ctx.brushState.duration           = 0.0;
    ctx.brushState.dtrack             = 0;
    ctx.brushState.activeAudioResourceId =
        createsAudioSample ? ctx.brushState.selectedAudioResourceId
                           : std::string{};
    ctx.brushState.activeSampleBinding.reset();
    ctx.brushState.replacesExistingObject = false;
    if ( !createsAudioSample &&
         !ctx.brushState.selectedAudioResourceId.empty() ) {
        // 音符绑定保留资源引用及实例音量，资源自身的公共音量不在此修改。
        // BGM 资源采用 activeAudioResourceId，避免同时保存两套绑定语义。
        ctx.brushState.activeSampleBinding = ::MMM::AudioSampleBinding{
            ctx.brushState.selectedAudioResourceId,
            ctx.brushState.selectedAudioVolume,
        };
    }

    // 获取 BPM 事件供磁吸使用
    SessionUtils::ensureBpmEvents(ctx);
    const auto& bpmEvents = ctx.bpmEvents;

    // 计算逻辑时间
    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    // 没有滚动映射不能从屏幕位置推导时间，撤回刚激活的画笔。
    // 这里沿用仅解除 isActive 的失败路径，后续新起笔会覆盖活动字段。
    if ( !cache ) {
        ctx.brushState.isActive = false;
        return;
    }

    // 以动画时间对应的绝对滚动位置为锚点，让起笔落在当前所见位置。
    // 鼠标在判定线上方时 deltaY 为正，时间由滚动缓存反解。
    float judgmentLineY =
        itCamera->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    double currentAbsY = cache->getAbsY(ctx.animateTime);
    double deltaY      = (judgmentLineY - cmd.mouseY);

    // 处理预览区缩放
    float renderScaleY = 1.0f;
    if ( isPreview ) {
        const auto* mainCamera =
            SessionUtils::findMainCanvasCamera(ctx.cameras);
        float mainViewportHeight = mainCamera ? mainCamera->viewportHeight
                                              : itCamera->second.viewportHeight;
        // 主画布轨道上下界是归一化比例，须先乘视口高度换算像素。
        // 预览边距则已经是像素，两种单位不可直接相减。
        float mainEffectiveH = (ctx.lastConfig.visual.trackLayout.bottom -
                                ctx.lastConfig.visual.trackLayout.top) *
                               mainViewportHeight;
        float previewDrawH =
            itCamera->second.viewportHeight -
            (ctx.lastConfig.visual.previewConfig.margin.top +
             ctx.lastConfig.visual.previewConfig.margin.bottom);
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
    }
    // 解除预览压缩后再进入滚动缓存，避免预览中同一像素被当作主画布距离。
    // 倍率由有效视口和 areaRatio 配置提供，当前入口不另行修正退化布局。
    deltaY /= renderScaleY;

    double rawTime = cache->getTime(currentAbsY + deltaY);

    auto snap = SessionUtils::getSnapResult(
        rawTime,
        cmd.mouseY,
        itCamera->second,
        ctx.lastConfig,
        bpmEvents,
        ctx.timelineRegistry,
        ctx.animateTime,
        ctx.cameras,
        ctx.currentBeatmap
            ? ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : 120.0);

    // 起笔沿用吸附结果；更新入口才根据 Ctrl 临时绕过吸附。
    // 无效或负起始时间直接取消，不能创建一个等待后续移动修正的非法物件。
    ctx.brushState.time = snap.isSnapped ? snap.snappedTime : rawTime;
    if ( !isPlaceableNoteTime(ctx.brushState.time) ) {
        resetBrushState(ctx);
        return;
    }

    // 绝对轨号在 Note 与采样间共用，草稿以负编号表达。
    // BGM 起笔固定为点击形态并提前返回，不进入音符续接与 Shift 形状逻辑。
    ctx.brushState.track = static_cast<int>(targetLane->absoluteTrack(
        static_cast<std::uint32_t>(ctx.trackCount), projectedDraftLaneCount));
    if ( createsAudioSample ) {
        ctx.brushState.type = ::MMM::NoteType::NOTE;
        return;
    }

    // 只有显式启用折线编辑并按住 Shift 才尝试从悬浮对象续接。
    // 普通起笔仍可发生在已有对象附近，不隐式删除悬浮物件。
    bool isResuming = false;
    if ( ctx.lastConfig.settings.enablePolylineEditing && cmd.isShiftDown &&
         isHoveredPlayerNote(ctx) ) {
        const auto& note =
            ctx.noteRegistry.get<NoteComponent>(ctx.hoveredEntity);
        // 悬浮子实体提升为父身份，续接必须替换完整折线而不是孤立子节点。
        // 后续末段判断仍使用拾取结果保存的子索引。
        entt::entity targetEntity = ctx.hoveredEntity;
        if ( note.m_isSubNote && note.m_parentPolyline != entt::null ) {
            targetEntity = note.m_parentPolyline;
        }

        // 续接使用提升后的父身份再次检查有效性，原悬浮子实体有效不代表父仍存在。
        // 后续 get 还依赖父身份确实持有 NoteComponent 的会话结构约束。
        if ( ctx.noteRegistry.valid(targetEntity) ) {
            const auto& parentNote =
                ctx.noteRegistry.get<NoteComponent>(targetEntity);
            if ( parentNote.m_type == ::MMM::NoteType::POLYLINE &&
                 !parentNote.m_subNotes.empty() ) {
                // 只允许从原折线最后一段续接，内部节点点击不截断或分叉原折线。
                // 末段还需命中允许延伸的可交互部件。
                int lastIdx =
                    static_cast<int>(parentNote.m_subNotes.size() - 1);
                // 续接既有折线保留全部已完成段，只允许修改其活动尾部。
                // 命中根体但没有末段索引时不会推测应该从哪一段继续。
                if ( ctx.hoveredSubIndex == lastIdx ) {
                    // 检查是否悬停在能够继续延伸的部分：Node、Body、HoldEnd
                    // 或 FlickArrow。
                    if ( ctx.hoveredPart ==
                             static_cast<uint8_t>(HoverPart::PolylineNode) ||
                         ctx.hoveredPart ==
                             static_cast<uint8_t>(HoverPart::HoldEnd) ||
                         ctx.hoveredPart ==
                             static_cast<uint8_t>(HoverPart::FlickArrow) ||
                         ctx.hoveredPart ==
                             static_cast<uint8_t>(HoverPart::HoldBody) ) {
                        // 进入恢复编辑模式
                        ctx.brushState.isActive = true;
                        ctx.brushState.type     = ::MMM::NoteType::POLYLINE;
                        // 先复制父内嵌段列表，随后的删除动作可能使 parentNote
                        // 引用失效。 画笔临时结构因而必须拥有自己的值副本。
                        ctx.brushState.polylineSegments = parentNote.m_subNotes;

                        // 如果最后一段是 Hold，则根据当前点击位置缩短/伸长它
                        if ( ctx.brushState.polylineSegments.back().type ==
                             ::MMM::NoteType::HOLD ) {
                            double snapTime =
                                snap.isSnapped ? snap.snappedTime : rawTime;
                            auto& lastSeg =
                                ctx.brushState.polylineSegments.back();
                            // 续接 Hold
                            // 时采用本次吸附点击作为新末端，允许缩短已有末段。
                            // 最小为零，暂时退化的末段交给后续形状或提交规则解释。
                            lastSeg.duration =
                                std::max(0.0, snapTime - lastSeg.timestamp);
                        }

                        // 恢复笔刷状态的各个参考值，以便 handleUpdateBrush
                        // 能够平滑接续
                        // 根时间与根轨保留原对象起点，不能改成点击末段时的位置。
                        // 这样后续折线降级为普通形状时仍能恢复正确的锚点。
                        ctx.brushState.holdStartTime = parentNote.m_timestamp;
                        ctx.brushState.startTrack    = parentNote.m_trackIndex;
                        ctx.brushState.activeSampleBinding =
                            parentNote.m_sampleBinding;

                        // 计算起始 Y 坐标，用于退化到普通音符时的参考
                        // 把原根时间正向投影回屏幕，恢复手势起始像素参照。
                        // 这里乘回预览倍率，与前面的鼠标逆映射成对。
                        double startAbsY =
                            cache->getAbsY(parentNote.m_timestamp);
                        ctx.brushState.startMouseY =
                            judgmentLineY -
                            static_cast<float>(startAbsY - currentAbsY) *
                                renderScaleY;

                        // 当前鼠标 Y 为新段参考点
                        // 局部垂直参照从本次接续点击开始，避免把原物件高度计为新手势。
                        // 全局 startMouseY
                        // 则保留根位置，两种参照服务不同的退化与增长规则。
                        ctx.brushState.segmentStartMouseY = cmd.mouseY;

                        // 删除原物件及其子物件实体 (通过
                        // BatchNoteAction，以便整体撤销)
                        std::vector<BatchNoteAction::Entry> deleteEntries;
                        deleteEntries.push_back(
                            { targetEntity, parentNote, std::nullopt });

                        appendPolylineChildDeleteEntries(
                            ctx, targetEntity, deleteEntries);

                        auto deleteAction = std::make_unique<BatchNoteAction>(
                            std::move(deleteEntries), "Resume Polyline Edit");
                        // 所有原组件快照均已复制，执行后不能继续读取原父引用。
                        // 本次删除独立进入历史，画笔最终替代结果尚未提交。
                        ctx.actionStack.pushAndExecute(std::move(deleteAction),
                                                       ctx);

                        // 设置拖拽状态
                        // 只有删除动作已入栈后才标记替换状态，取消恢复需要识别这一步历史。
                        // 普通未续接画笔不会携带该标志。
                        ctx.brushState.replacesExistingObject = true;
                        ctx.isDragging                        = true;
                        // 续接借用会话拖动归属保存输入来源，后续事件路由应保持相机一致。
                        // 这里设置的是交互状态，不把视口信息写入谱面物件。
                        ctx.dragCameraId = cmd.cameraId;
                        isResuming       = true;
                        XINFO("Resuming Polyline edit for entity {}",
                              static_cast<uint32_t>(targetEntity));
                    }
                }
            } else if ( !parentNote.m_isSubNote &&
                        (parentNote.m_type == ::MMM::NoteType::NOTE ||
                         parentNote.m_type == ::MMM::NoteType::HOLD ||
                         parentNote.m_type == ::MMM::NoteType::FLICK) ) {
                // 将普通的 Note/Hold/Flick 转换为包含单个 subNote 的
                // Polyline，并恢复编辑
                ctx.brushState.isActive = true;
                ctx.brushState.type     = ::MMM::NoteType::POLYLINE;

                // 把独立物件的几何、元数据、绑定和颜色装入唯一初始子段。
                // 这份值作为续接起点，不创建临时 ECS 子实体。
                NoteComponent::SubNote s;
                s.type                          = parentNote.m_type;
                s.timestamp                     = parentNote.m_timestamp;
                s.duration                      = parentNote.m_duration;
                s.trackIndex                    = parentNote.m_trackIndex;
                s.dtrack                        = parentNote.m_dtrack;
                s.metadata                      = parentNote.m_metadata;
                s.sampleBinding                 = parentNote.m_sampleBinding;
                s.customColors                  = parentNote.m_customColors;
                ctx.brushState.polylineSegments = { s };

                // 如果是 Hold，则根据当前点击位置缩短/伸长它
                // 独立 Hold 续接也按当前点击调整末端，与既有折线末段保持一致。
                // 普通 Note 和 Flick 不在这里改动它们的原跨度。
                if ( parentNote.m_type == ::MMM::NoteType::HOLD ) {
                    double snapTime =
                        snap.isSnapped ? snap.snappedTime : rawTime;
                    ctx.brushState.polylineSegments.back().duration =
                        std::max(0.0, snapTime - parentNote.m_timestamp);
                }

                ctx.brushState.holdStartTime       = parentNote.m_timestamp;
                ctx.brushState.startTrack          = parentNote.m_trackIndex;
                ctx.brushState.activeSampleBinding = parentNote.m_sampleBinding;

                double startAbsY = cache->getAbsY(parentNote.m_timestamp);
                ctx.brushState.startMouseY =
                    judgmentLineY -
                    static_cast<float>(startAbsY - currentAbsY) * renderScaleY;

                ctx.brushState.segmentStartMouseY = cmd.mouseY;

                std::vector<BatchNoteAction::Entry> deleteEntries;
                deleteEntries.push_back(
                    { targetEntity, parentNote, std::nullopt });

                auto deleteAction = std::make_unique<BatchNoteAction>(
                    std::move(deleteEntries), "Convert Note to Polyline");
                ctx.actionStack.pushAndExecute(std::move(deleteAction), ctx);

                ctx.brushState.replacesExistingObject = true;
                ctx.isDragging                        = true;
                ctx.dragCameraId                      = cmd.cameraId;
                isResuming                            = true;
                // 原对象删除后此日志仍读取
                // parentNote，存在观察引用生命周期风险。
                // 日志所需类型值应在删除前保存，不能依赖原组件存储继续有效。
                XINFO(
                    "Converting ordinary note (type {}) to Polyline and "
                    "resuming edit for entity {}",
                    static_cast<int>(parentNote.m_type),
                    static_cast<uint32_t>(targetEntity));
            }
        }
    }

    // 续接成功已恢复原始参照，不能再用鼠标按下位置覆盖。
    // 新 Shift 手势才记录持续段起点，形状升级由更新入口决定。
    if ( !isResuming ) {
        if ( cmd.isShiftDown ) {
            ctx.brushState.type = ::MMM::NoteType::NOTE;  // 初始为 Note
            ctx.brushState.holdStartTime = ctx.brushState.time;
            ctx.brushState.startTrack    = ctx.brushState.track;
            ctx.brushState.startMouseY   = cmd.mouseY;
        } else {
            ctx.brushState.type = ::MMM::NoteType::NOTE;
        }
    }
}

/// @brief 根据当前指针更新画笔位置及普通物件与折线的手势状态。
/// @param ctx 包含起笔参照和临时折线段的会话。
/// @param cmd 当前鼠标位置、相机及修饰键。
/// @warning
/// 连续输入热路径：即时更新本地预览，不得加入全谱扫描、文件访问或等待。
/// @note 画笔保持起笔领域，跨区域输入不隐式转换对象类别。
/// @pre 起笔参照与临时段由同一会话线程拥有，更新时不可并发重置。
/// @note 时间变化阈值用于形状判断，像素阈值用于识别吸附同拍内的垂直手势。
/// @note 普通形状和临时子列表共同表示状态，不能只改 type 而保留不兼容的段列表。
/// @note Ctrl 只绕过时间吸附，不解除轨道边界与对象域限制。
/// @note 一次输入最多增长或回退活动末段，连续事件推进整个折线路径。
void DrawTool::handleUpdateBrush(SessionContext& ctx, const CmdUpdateBrush& cmd)
{
    if ( !ctx.brushState.isActive ) return;
    // 记录手势曾进入 Flick 或 Polyline，而不只观察最后一帧类型。
    // 该历史标志可区分退化后的点击与从未延伸过的普通点击。
    ctx.brushState.hasPolylineGesture |=
        ctx.brushState.type == ::MMM::NoteType::FLICK ||
        ctx.brushState.type == ::MMM::NoteType::POLYLINE;

    // 同 StartBrush 的逻辑
    auto itCamera = ctx.cameras.find(cmd.cameraId);
    // 相机暂时不可用时保留现有画笔，不把最后有效位置归零。
    // 与非有限时间的取消分支不同，这里允许后续有效更新继续手势。
    if ( itCamera == ctx.cameras.end() ) return;

    SessionUtils::ensureBpmEvents(ctx);
    // 借用会话维护的 BPM 数据，吸附与屏幕反算共用本次动画锚点。
    // 不把 BPM 列表复制进每次鼠标更新的临时对象。
    const auto& bpmEvents = ctx.bpmEvents;

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;

    float judgmentLineY =
        itCamera->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    // 拖动时播放可以继续推进，当前屏幕锚点必须每次重新查询。
    // 固定使用起笔时的绝对 Y 会让画笔与正在滚动的画面分离。
    double currentAbsY = cache->getAbsY(ctx.animateTime);
    double deltaY      = (judgmentLineY - cmd.mouseY);

    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas" ) {
        const auto* mainCamera =
            SessionUtils::findMainCanvasCamera(ctx.cameras);
        // 缺少主相机时以当前视口高度兼容计算，避免解引用空观察指针。
        // 该回退只替代尺寸来源，不改变预览边距和覆盖比例的含义。
        float mainViewportHeight = mainCamera ? mainCamera->viewportHeight
                                              : itCamera->second.viewportHeight;
        float mainEffectiveH     = (ctx.lastConfig.visual.trackLayout.bottom -
                                    ctx.lastConfig.visual.trackLayout.top) *
                                   mainViewportHeight;
        float previewDrawH =
            itCamera->second.viewportHeight -
            (ctx.lastConfig.visual.previewConfig.margin.top +
             ctx.lastConfig.visual.previewConfig.margin.bottom);
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
    }
    deltaY /= renderScaleY;

    double rawTime = cache->getTime(currentAbsY + deltaY);
    auto   snap    = SessionUtils::getSnapResult(
        rawTime,
        cmd.mouseY,
        itCamera->second,
        ctx.lastConfig,
        bpmEvents,
        ctx.timelineRegistry,
        ctx.animateTime,
        ctx.cameras,
        ctx.currentBeatmap
            ? ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : 120.0);

    // Ctrl 切换仅影响这一轮时间目标，起笔参照仍保留原始锁定时间。
    // 恢复吸附后会直接按新目标更新几何，不等待额外稳定窗口。
    double currentPosTime =
        (snap.isSnapped && !cmd.isCtrlDown) ? snap.snappedTime : rawTime;
    // 非有限时间说明当前逆映射不可提交，清理整个活动画笔。
    // 有限负时间则钳制到零，允许既有手势贴住谱面起点继续移动。
    if ( !std::isfinite(currentPosTime) ) {
        resetBrushState(ctx);
        return;
    }
    currentPosTime = std::max(MIN_PLACEABLE_NOTE_TIME, currentPosTime);

    const bool isPreview =
        cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas";
    std::optional<CanvasLaneAddress> currentLane;
    std::uint32_t                    projectedDraftLaneCount{ 0U };
    if ( !isPreview ) {
        const auto laneProjection = calculateCanvasLaneProjection(
            itCamera->second.viewportWidth,
            ctx.trackCount,
            ctx.bgmTrackCount,
            ctx.lastConfig.visual.trackLayout,
            itCamera->second.horizontalOffsetX,
            true,
            ctx.lastConfig.settings.enableBmsEditing,
            ctx.lastConfig.settings.professionalMode,
            ctx.draftTrackCount,
            true);
        projectedDraftLaneCount = laneProjection.draftLaneCount;
        currentLane             = laneProjection.laneAt(cmd.mouseX);
        if ( !currentLane ) return;

        // 活动对象域以采样标志优先，其余由草稿轨负号区分。
        // 不能仅按 currentLane 判断域，否则鼠标跨界会悄然改变正在绘制的对象。
        const CanvasLaneKind brushLaneKind =
            ctx.brushState.createsAudioSample
                ? CanvasLaneKind::Bgm
                : (ctx.brushState.track < 0 ? CanvasLaneKind::Draft
                                            : CanvasLaneKind::Player);
        // 不同于移动工具，绘制过程中不支持跨域转换或跨域折线。
        // 鼠标离开原领域时保留上一有效预览，返回后可以继续同一笔。
        if ( currentLane->kind != brushLaneKind ) {
            return;
        }
    }

    // 采样只移动锚点和绝对轨号，不根据 Shift 生成 Hold 或折线段。
    // 没有 BGM 命中时仍结束此分支，不将采样送入普通音符状态机。
    if ( ctx.brushState.createsAudioSample ) {
        if ( currentLane && currentLane->kind == CanvasLaneKind::Bgm ) {
            ctx.brushState.track = static_cast<int>(currentLane->absoluteTrack(
                static_cast<std::uint32_t>(ctx.trackCount),
                projectedDraftLaneCount));
            ctx.brushState.time  = currentPosTime;
        }
        return;
    }

    const auto projection =
        calculatePlayerTrackProjection(itCamera->second.viewportWidth,
                                       ctx.trackCount,
                                       ctx.lastConfig.visual.trackLayout.left,
                                       ctx.lastConfig.visual.trackLayout.right,
                                       itCamera->second.horizontalOffsetX);
    float leftX  = projection.leftX;
    float rightX = projection.rightX;
    if ( cmd.cameraId == "Preview" ) {
        leftX  = ctx.lastConfig.visual.previewConfig.margin.left;
        rightX = itCamera->second.viewportWidth -
                 ctx.lastConfig.visual.previewConfig.margin.right;
    }
    float trackAreaW = rightX - leftX;
    // 没有离散领域地址的预览沿用连续玩家轨算式。
    // 主画布优先使用 currentLane，分区边距不能折算为玩家轨号。
    float singleTrackW = trackAreaW / static_cast<float>(ctx.trackCount);
    int   currentTrack =
        currentLane
            ? currentLane->absoluteTrack(
                  static_cast<std::uint32_t>(ctx.trackCount),
                  projectedDraftLaneCount)
            : static_cast<int>(std::floor((cmd.mouseX - leftX) / singleTrackW));
    // 草稿下界包含一个追加轨，玩家上界止于最后一条玩家轨。
    // 后续形状的起点和末端必须使用同一域的边界。
    const bool editsDraft   = ctx.brushState.track < 0;
    const int  minimumTrack = editsDraft ? -(ctx.draftTrackCount + 1) : 0;
    const int  maximumTrack = editsDraft ? -1 : ctx.trackCount - 1;
    currentTrack = std::clamp(currentTrack, minimumTrack, maximumTrack);

    // 关闭折线编辑时 Shift 仅控制持续长度，横向指针移动不生成 Flick。
    // 五像素阈值识别可见手势，时间容差则识别吸附后是否离开起拍。
    if ( cmd.isShiftDown && !ctx.lastConfig.settings.enablePolylineEditing ) {
        const float diffY = std::abs(cmd.mouseY - ctx.brushState.startMouseY);
        const bool  timeChanged =
            std::abs(currentPosTime - ctx.brushState.holdStartTime) > 1e-5;
        ctx.brushState.type  = timeChanged || diffY > 5.0F
                                   ? ::MMM::NoteType::HOLD
                                   : ::MMM::NoteType::NOTE;
        ctx.brushState.time  = ctx.brushState.holdStartTime;
        ctx.brushState.track = ctx.brushState.startTrack;
        // 固定根时间与根轨，只从当前指针时间计算非负时长。
        // 向起点之前拖动可保留零长度 Hold，不把负时长写入组件。
        ctx.brushState.duration =
            ctx.brushState.type == ::MMM::NoteType::HOLD
                ? std::max(0.0, currentPosTime - ctx.brushState.holdStartTime)
                : 0.0;
        ctx.brushState.dtrack = 0;
        ctx.brushState.polylineSegments.clear();
        return;
    }

    if ( cmd.isShiftDown ) {
        // 只有没有持续、横移和子段的纯点击才能重新建立 Shift 起点。
        // 保留中的 Hold 或 Flick 必须延续已有几何参照，不能被重新初始化。
        if ( ctx.brushState.type == ::MMM::NoteType::NOTE &&
             ctx.brushState.polylineSegments.empty() &&
             ctx.brushState.duration == 0.0 && ctx.brushState.dtrack == 0 ) {
            // 刚按下 Shift 或处于 Note 状态，锁定初始状态
            // 开始时未按 Shift 的普通笔也可在后续进入锁定模式。
            // 只在哨兵状态建立参照，持续按住 Shift 不反复移动根起点。
            if ( ctx.brushState.holdStartTime < 0.0 ) {
                ctx.brushState.holdStartTime      = currentPosTime;
                ctx.brushState.startTrack         = currentTrack;
                ctx.brushState.startMouseY        = cmd.mouseY;
                ctx.brushState.segmentStartMouseY = cmd.mouseY;
            }
        }

        // 该配置读数当前未用于下面的状态切换，既有逻辑仍采用固定像素判据。
        // 调整手势敏感度时需同时审视时间与像素条件，不能只修改此局部变量。
        float threshold = ctx.lastConfig.visual.snapThreshold;

        // --- Polyline 状态机 ---
        if ( ctx.brushState.polylineSegments.empty() ) {
            // [Phase 1] 决定初始物件 (HOLD or FLICK)
            float diffY = std::abs(cmd.mouseY - ctx.brushState.startMouseY);
            bool  timeChanged =
                std::abs(currentPosTime - ctx.brushState.holdStartTime) > 1e-5;

            // 已形成非零 Flick 才保留横移优先；尚未成形的对角拖动仍按初始分类。
            // 该条件保存手势方向顺序，不能简化成只检查当前鼠标位移。
            const bool extendsEstablishedFlick =
                ctx.brushState.type == ::MMM::NoteType::FLICK &&
                ctx.brushState.dtrack != 0 &&
                currentTrack != ctx.brushState.startTrack &&
                (timeChanged || diffY > 5.0F);

            // 已经明确横移成 Flick 后再纵向延伸时，必须保留横向优先顺序；
            // 否则重新分类会先改成 Hold，导致 L 形折线反转为钩子。
            if ( extendsEstablishedFlick ) {
                ctx.brushState.type = ::MMM::NoteType::FLICK;
                ctx.brushState.dtrack =
                    currentTrack - ctx.brushState.startTrack;
                ctx.brushState.duration = 0.0;
            } else if ( !timeChanged && diffY <= 5.0f ) {
                // 如果时间未改变（停留在同一拍）且垂直拖拽极小，则判断为普通音符或滑键
                // 同拍附近只解释横移，回到起轨时退回普通点击。
                // 显式检查终轨范围，避免箭头把已钳制的域边界解释为跨域连接。
                int dtrack = currentTrack - ctx.brushState.startTrack;
                if ( dtrack != 0 &&
                     ctx.brushState.startTrack + dtrack >= minimumTrack &&
                     ctx.brushState.startTrack + dtrack <= maximumTrack ) {
                    ctx.brushState.type   = ::MMM::NoteType::FLICK;
                    ctx.brushState.dtrack = dtrack;
                } else {
                    ctx.brushState.type   = ::MMM::NoteType::NOTE;
                    ctx.brushState.dtrack = 0;
                }
                ctx.brushState.duration = 0.0;
            } else {
                ctx.brushState.type     = ::MMM::NoteType::HOLD;
                ctx.brushState.duration = std::max(
                    0.0, currentPosTime - ctx.brushState.holdStartTime);
                ctx.brushState.dtrack = 0;
            }

            // 检查是否需要转化为 Polyline (状态转换点)
            if ( ctx.brushState.type == ::MMM::NoteType::HOLD &&
                 currentTrack != ctx.brushState.startTrack ) {
                // HOLD 偏离轨道 -> 开始 Polyline
                // HOLD 转折线时省略退化前段，避免以零持续段伪造一个额外节点。
                // 新 Flick 的时间仍按原起点加持续值构造，连接在真实末端。
                if ( ctx.brushState.duration > 1e-5 ) {
                    // 只有长度大于 0 才添加 Hold 段
                    NoteComponent::SubNote s1{ ::MMM::NoteType::HOLD,
                                               ctx.brushState.holdStartTime,
                                               ctx.brushState.duration,
                                               ctx.brushState.startTrack,
                                               0 };
                    ctx.brushState.polylineSegments.push_back(s1);
                }

                // 后续横移从 Hold 末端出发，起轨保留 Hold 轨道。
                // 轨差连接当前指针轨，不改变此前持续段的横向位置。
                NoteComponent::SubNote s2{
                    ::MMM::NoteType::FLICK,
                    ctx.brushState.holdStartTime + ctx.brushState.duration,
                    0.0,
                    ctx.brushState.startTrack,
                    currentTrack - ctx.brushState.startTrack
                };
                ctx.brushState.polylineSegments.push_back(s2);
                ctx.brushState.segmentStartMouseY = cmd.mouseY;
                ctx.brushState.type               = ::MMM::NoteType::POLYLINE;
            } else if ( ctx.brushState.type == ::MMM::NoteType::FLICK &&
                        (timeChanged || diffY > 5.0f) ) {
                // FLICK 垂直位移 -> 开始 Polyline: [FLICK, HOLD]
                // 先保存已形成的横移，再在箭头终轨追加持续段。
                // 两个子段共用转折时间，使 L 形顺序与输入先后保持一致。
                NoteComponent::SubNote s1{ ::MMM::NoteType::FLICK,
                                           ctx.brushState.holdStartTime,
                                           0.0,
                                           ctx.brushState.startTrack,
                                           ctx.brushState.dtrack };
                NoteComponent::SubNote s2{
                    ::MMM::NoteType::HOLD,
                    s1.timestamp,
                    std::max(0.0, currentPosTime - s1.timestamp),
                    s1.trackIndex + s1.dtrack,
                    0
                };
                ctx.brushState.polylineSegments.push_back(s1);
                ctx.brushState.polylineSegments.push_back(s2);
                ctx.brushState.segmentStartMouseY = cmd.mouseY;
                ctx.brushState.type               = ::MMM::NoteType::POLYLINE;
            }

            // 尚未升级为折线时，根字段直接表达普通物件，必须维持锁定起点。
            // 进入折线后实际起点由临时段列表表达，提交时重新推导根字段。
            if ( ctx.brushState.type != ::MMM::NoteType::POLYLINE ) {
                ctx.brushState.track = ctx.brushState.startTrack;
                ctx.brushState.time  = ctx.brushState.holdStartTime;
            }
        } else {
            // [Phase 2] Polyline 动态增长与退化
            // 连续编辑只改变活动末段，已经完成的前缀保持原值。
            // last 借用向量元素，push_back 或 pop_back 后不能再使用该引用。
            auto& last = ctx.brushState.polylineSegments.back();
            if ( last.type == ::MMM::NoteType::HOLD ) {
                // 指针回到当前 Hold 起点便撤掉这个未完成的末段。
                // 每次输入只回退一层，避免一次移动递归清空所有历史转折。
                if ( currentPosTime <= last.timestamp ) {
                    // 回退该 Hold 段
                    ctx.brushState.polylineSegments.pop_back();
                    if ( ctx.brushState.polylineSegments.size() == 1 ) {
                        // 只剩一段时把几何移回普通形状字段，再清空折线容器。
                        // 先按值复制段，防止 clear 使恢复来源失效。
                        auto s = ctx.brushState.polylineSegments[0];
                        ctx.brushState.type          = s.type;
                        ctx.brushState.holdStartTime = s.timestamp;
                        ctx.brushState.startTrack    = s.trackIndex;
                        ctx.brushState.duration      = s.duration;
                        ctx.brushState.dtrack        = s.dtrack;
                        ctx.brushState.polylineSegments.clear();
                    }
                    return;
                }

                // 末段增长从自身起点计算，不累计相邻帧时间差。
                // 若同时换轨，新增 Flick 应连接这个更新后的 Hold 尾部。
                last.duration = std::max(0.0, currentPosTime - last.timestamp);
                if ( currentTrack != last.trackIndex ) {
                    // 开启新的 Flick
                    ctx.brushState.polylineSegments.push_back(
                        { ::MMM::NoteType::FLICK,
                          last.timestamp + last.duration,
                          0.0,
                          last.trackIndex,
                          currentTrack - last.trackIndex });
                    ctx.brushState.segmentStartMouseY = cmd.mouseY;
                }
            } else if ( last.type == ::MMM::NoteType::FLICK ) {
                // 箭头实际终轨决定当前是否仍在横移；不能用末段起轨判断稳定。
                // 轨道稳定后才允许纵向动作开启下一持续段。
                int targetTrack = last.trackIndex + last.dtrack;
                // 从滑键头部或折线节点向更早时间拖动，只保留原滑键，不反向生成
                // Hold。
                if ( currentPosTime < last.timestamp &&
                     currentTrack == last.trackIndex && last.dtrack != 0 ) {
                    return;
                }
                // 当指针尚在横移时，优先修改当前 Flick 而非同时追加 Hold。
                // 纵向延伸的起点因而来自最后一次明确的横移位置。
                if ( currentTrack != targetTrack ) {
                    // 正在横移：更新
                    // dtrack，并重置垂直参考点以防止意外触发长按。
                    last.dtrack = currentTrack - last.trackIndex;
                    ctx.brushState.segmentStartMouseY = cmd.mouseY;

                    // 退化检查: 如果 Flick 回到了起始轨道
                    // 回到起轨会撤销活动横移段，前面的几何仍保留。
                    // 撤段后若只剩一段，恢复成普通物件，下一次更新可以再次升级。
                    if ( last.dtrack == 0 ) {
                        ctx.brushState.polylineSegments.pop_back();
                        if ( ctx.brushState.polylineSegments.size() == 1 ) {
                            auto s = ctx.brushState.polylineSegments[0];
                            ctx.brushState.type          = s.type;
                            ctx.brushState.holdStartTime = s.timestamp;
                            ctx.brushState.startTrack    = s.trackIndex;
                            ctx.brushState.duration      = s.duration;
                            ctx.brushState.dtrack        = s.dtrack;
                            ctx.brushState.polylineSegments.clear();
                        }
                    }
                } else {
                    // 轨道稳定：检查垂直移动
                    // 局部垂直参照随横移更新，横移时附带的鼠标抖动不会立即生成长条。
                    // 时间判据仍参考 Flick 时间，吸附跨拍可直接触发下一段。
                    float diffYLocal = std::abs(
                        cmd.mouseY - ctx.brushState.segmentStartMouseY);
                    bool timeChangedLocal =
                        std::abs(currentPosTime - last.timestamp) > 1e-5;

                    // 局部时间变化按绝对差判断，未在这里重新限制到前进方向。
                    // 新段初始为零持续，后续 Hold 分支负责延长或退回。
                    if ( timeChangedLocal || diffYLocal > 5.0f ) {
                        // 开启新的 Hold
                        ctx.brushState.polylineSegments.push_back(
                            // 新增 Hold 从 Flick 终轨开始且初始持续为零。
                            // 本轮只建立活动段，后续输入再更新长度或回退该段。
                            { ::MMM::NoteType::HOLD,
                              last.timestamp,
                              0.0,
                              last.trackIndex + last.dtrack,
                              0 });
                        ctx.brushState.segmentStartMouseY = cmd.mouseY;
                    }
                }
                // 从已有普通点击续接时，初始局部列表可能以 NOTE 结尾。
                // 这时就地升级当前节点，不额外保留一个重叠的零跨度点击。
            } else if ( last.type == ::MMM::NoteType::NOTE ) {
                float diffYLocal =
                    std::abs(cmd.mouseY - ctx.brushState.segmentStartMouseY);
                bool timeChangedLocal =
                    std::abs(currentPosTime - last.timestamp) > 1e-5;

                if ( currentTrack != last.trackIndex ) {
                    // 改变轨道 -> 变成 Flick 段
                    last.type   = ::MMM::NoteType::FLICK;
                    last.dtrack = currentTrack - last.trackIndex;
                    ctx.brushState.segmentStartMouseY = cmd.mouseY;
                } else if ( timeChangedLocal || diffYLocal > 5.0f ) {
                    // 垂直拖动 -> 变成 Hold 段
                    last.type = ::MMM::NoteType::HOLD;
                    last.duration =
                        std::max(0.0, currentPosTime - last.timestamp);
                    ctx.brushState.segmentStartMouseY = cmd.mouseY;
                }
            }
        }
    } else {
        // 切换回 Note，重置锁定状态
        ctx.brushState.type     = ::MMM::NoteType::NOTE;
        ctx.brushState.time     = currentPosTime;
        ctx.brushState.track    = currentTrack;
        ctx.brushState.duration = 0.0;
        ctx.brushState.dtrack   = 0;
        // 释放 Shift 解除形状锁定，下一次按 Shift 需重新采样起点。
        // hasPolylineGesture 不在这里清除，最终提交仍能识别曾形成横移的手势。
        ctx.brushState.holdStartTime = -1.0;
        ctx.brushState.polylineSegments.clear();
    }
    ctx.brushState.hasPolylineGesture |=
        ctx.brushState.type == ::MMM::NoteType::FLICK ||
        ctx.brushState.type == ::MMM::NoteType::POLYLINE;
}

/// @brief 将活动画笔清理为可提交物件，并合并尾部连接与删除记录。
/// @param ctx 提供临时画笔、实体 Registry 和动作历史的会话。
/// @param cmd 结束命令；当前实现使用最后一次更新留下的几何。
/// @warning
/// 释放低频路径：允许局部段清理、尾部全谱查找与批次分配，禁止移到连续更新。
/// @note 新建和合并删除进入同一批次；此前续接原对象的删除已经独立入栈。
/// @note 时间验证失败只取消当前画笔，不执行尚未提交的合并删除计划。
/// @pre 读取的临时几何是最后一次有效更新结果，结束命令本身不重算鼠标位置。
/// @note 连接候选按当前 Registry 顺序检查，不按距离重新排序或择优匹配。
/// @note 整个尾部搜索的匹配时间和轨道只计算一次，不递归沿新尾部追踪更多对象。
/// @note 批次保存完整 before/after，局部工作列表可以在函数返回时释放。
/// @note 可编辑性检查针对当前根，时间检查则在清理合并后覆盖最终子项。
void DrawTool::handleEndBrush(SessionContext& ctx, const CmdEndBrush& cmd)
{
    if ( !ctx.brushState.isActive ) return;

    // 采样有独立提交类型，不能借用 Note 的颜色、子结构或合并流程。
    // 锚点须有效且目标在 BGM 编号范围内，否则只清理手势。
    if ( ctx.brushState.createsAudioSample ) {
        if ( isPlaceableNoteTime(ctx.brushState.time) &&
             ctx.brushState.track >= ctx.trackCount ) {
            // 新画笔采样采用零毫秒偏移；资源引用取起笔保存的活动值。
            // 音量仍读取当前工具选择，未从临时折线状态推导。
            SampleComponent sample{
                .m_timestamp = ctx.brushState.time,
                .m_offsetMs  = 0,
                .m_track     = static_cast<std::uint32_t>(ctx.brushState.track),
                .m_audioResourceId = ctx.brushState.activeAudioResourceId,
                .m_volume          = ctx.brushState.selectedAudioVolume,
            };
            // 创建交给 SampleAction 分配身份，空 before
            // 明确表示没有待替换实体。
            // 成功与无效落点最后都清理画笔，避免下一帧重复提交。
            ctx.actionStack.pushAndExecute(
                std::make_unique<SampleAction>(SampleAction::Type::Create,
                                               entt::null,
                                               std::nullopt,
                                               std::move(sample)),
                ctx);
        }
        resetBrushState(ctx);
        return;
    }

    // 创建正式音符
    // 正式结果先按值组装，直到所有清理和时间检查完成才安装到 ECS。
    // 根草稿标志由有符号轨号推导，绑定与颜色使用当前手势设置。
    NoteComponent note;
    note.m_timestamp     = ctx.brushState.time;
    note.m_duration      = ctx.brushState.duration;
    note.m_trackIndex    = ctx.brushState.track;
    note.m_dtrack        = ctx.brushState.dtrack;
    note.m_type          = ctx.brushState.type;
    note.m_isDraft       = note.m_trackIndex < 0;
    note.m_sampleBinding = ctx.brushState.activeSampleBinding;
    applyNoteColorOverrides(note, ctx.brushState.customColors);

    // 释放时再次检查配置门禁，覆盖手势中途切换编辑模式的情况。
    // 拒绝不会生成新音符或执行本函数后续合并。
    if ( !SessionUtils::isNoteEditable(note, ctx.lastConfig.settings) ) {
        resetBrushState(ctx);
        return;
    }

    // 折线尾部结合所需的删除条目列表 (声明在外部以便后续使用)
    std::vector<BatchNoteAction::Entry> mergeDeleteEntries;

    // 普通点击不参与尾部连接搜索；具有持续、横移或子结构的物件才展开段表示。
    // 统一段表示仅用于这次提交，不修改活动画笔的原列表。
    bool isMergeableType = (note.m_type == ::MMM::NoteType::POLYLINE ||
                            note.m_type == ::MMM::NoteType::HOLD ||
                            note.m_type == ::MMM::NoteType::FLICK);

    if ( isMergeableType ) {
        // 独立 Hold 和 Flick 包装为单段，以便共用尾部几何计算。
        // 几何包装未复制所有父属性，最终根仍保留前面组装的绑定与颜色。
        std::vector<NoteComponent::SubNote> segments;
        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            segments = ctx.brushState.polylineSegments;
        } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
            segments.push_back({ ::MMM::NoteType::HOLD,
                                 note.m_timestamp,
                                 note.m_duration,
                                 note.m_trackIndex,
                                 0 });
        } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
            segments.push_back({ ::MMM::NoteType::FLICK,
                                 note.m_timestamp,
                                 0.0,
                                 note.m_trackIndex,
                                 note.m_dtrack });
        }

        // [深度清洗与递归简化] - 仅针对折线类型进行零值清洗与合并，普通 Hold /
        // Flick 无需深度清洗
        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            // 删除退化段后可能让同类段相邻，合并又可能产生零轨差。
            // 每次 changed 为真都减少列表长度，重复清理会在有限轮次内稳定。
            bool changed = true;
            while ( changed ) {
                changed = false;

                // 1. 过滤所有“零值”段：0长度Hold 或 0位移Flick
                // 普通 NOTE 是有效节点，不能因为没有 duration 或 dtrack
                // 就删除。 Hold 用秒容差，Flick
                // 用精确整数零轨差，两者退化判据不同。
                auto it = std::remove_if(
                    segments.begin(), segments.end(), [](const auto& s) {
                        if ( s.type == ::MMM::NoteType::HOLD )
                            return s.duration < 1e-5;
                        if ( s.type == ::MMM::NoteType::FLICK )
                            return s.dtrack == 0;
                        return false;
                    });
                // remove_if 只整理保留前缀，erase 才真正缩短工作列表。
                // 本轮改变会触发下一轮退化检查，源画笔列表不受影响。
                if ( it != segments.end() ) {
                    segments.erase(it, segments.end());
                    changed = true;
                }

                // 2. 合并连续的同类型物件
                if ( segments.size() > 1 ) {
                    for ( size_t i = 0; i < segments.size() - 1; ) {
                        auto& curr = segments[i];
                        auto& next = segments[i + 1];

                        if ( curr.type == next.type ) {
                            if ( curr.type == ::MMM::NoteType::HOLD ) {
                                // 合并长条持续时间
                                // 当前实现按连续段时长相加，依赖画笔生成的连接关系。
                                // 保留前段其他属性，后段仅贡献几何跨度。
                                curr.duration += next.duration;
                                segments.erase(segments.begin() + i + 1);
                                changed = true;
                                continue;  // 继续检查合并后的段
                            } else if ( curr.type == ::MMM::NoteType::FLICK ) {
                                // 合并滑键位移量
                                // 相反方向的轨差可能互相抵消，下一轮会删除新的零跨度段。
                                // 合并后停留原下标，继续检查是否还能吞并后一个同类段。
                                curr.dtrack += next.dtrack;
                                segments.erase(segments.begin() + i + 1);
                                changed = true;
                                continue;
                            }
                        }
                        i++;
                    }
                }
            }
        }

        // 3. 尾部结合检测 (清洗后仍有 >=1 段时执行)
        //    时间容差 3ms，轨道必须一致
        // 三毫秒只用于连接点匹配，不作为输入等待或延迟窗口。
        // 轨道仍要求精确相等，不能用时间接近替代横向连接。
        constexpr double MERGE_TIME_TOLERANCE = 0.003;

        // 清洗可能清空列表，先检查非空再读取末段。
        // 只有 Hold 和 Flick 在下面定义有效尾部，正常状态机应提供这两类尾段。
        if ( segments.size() >= 1 ) {
            // 计算尾部的时间和轨道
            // 这里保存工作列表末元素引用，后续分支会向同一向量追加元素。
            // 向量扩容会使该引用失效，追加后的后续匹配存在既有生命周期风险。
            const auto& lastSeg   = segments.back();
            double      tailTime  = 0.0;
            int         tailTrack = 0;

            if ( lastSeg.type == ::MMM::NoteType::HOLD ) {
                // subHold: 尾部 = 起始时间 + 持续时间, 轨道不变
                tailTime  = lastSeg.timestamp + lastSeg.duration;
                tailTrack = lastSeg.trackIndex;
            } else if ( lastSeg.type == ::MMM::NoteType::FLICK ) {
                // subFlick: 尾部 = 时间戳, 轨道 = 起始轨道 + dtrack
                tailTime  = lastSeg.timestamp;
                tailTrack = lastSeg.trackIndex + lastSeg.dtrack;
            }

            // 在注册表中搜索尾部位置附近的物件
            // 只检查根对象，子项由父折线匹配与级联删除统一处理。
            // 搜索阶段只收集删除计划，不能立即销毁迭代中的实体。
            auto noteView = ctx.noteRegistry.view<NoteComponent>();
            for ( auto entity : noteView ) {
                const auto& nc = noteView.get<NoteComponent>(entity);
                if ( nc.m_isSubNote ) continue;  // 跳过子物件

                // ===== 检测普通 Note（移除） =====
                // 重叠点击只作为冗余对象删除，不向路径追加一个新的 NOTE 段。
                // 所有命中共用本次尾部容差，删除仍与新物件创建原子地记录在批次中。
                if ( nc.m_type == ::MMM::NoteType::NOTE ) {
                    if ( nc.m_trackIndex == tailTrack &&
                         std::abs(nc.m_timestamp - tailTime) <=
                             MERGE_TIME_TOLERANCE ) {
                        // 移除与尾部重叠的普通 Note
                        mergeDeleteEntries.push_back(
                            { entity, nc, std::nullopt });
                        XINFO(
                            "Note merge: removing Note at t={:.3f} "
                            "track={}",
                            nc.m_timestamp,
                            nc.m_trackIndex);
                    }
                    continue;
                }

                // ===== 【1】结合普通物件 (Flick / Hold) =====
                if ( nc.m_type == ::MMM::NoteType::FLICK ) {
                    // 目标是一个独立的 Flick
                    if ( nc.m_trackIndex == tailTrack &&
                         std::abs(nc.m_timestamp - tailTime) <=
                             MERGE_TIME_TOLERANCE ) {
                        if ( lastSeg.type == ::MMM::NoteType::HOLD ) {
                            // 【1-1 不同类型】末尾 subHold + 目标 Flick
                            // → 将 Flick 作为最后一个 seg 加入
                            // 不同类型连接时保留目标的元数据、音效和颜色为独立子段属性。
                            // 首时间修正到连接点，但不修改目标实体本身，撤销仍用完整原值。
                            NoteComponent::SubNote flickSeg;
                            flickSeg.type       = ::MMM::NoteType::FLICK;
                            flickSeg.timestamp  = tailTime;  // 修复微小时间差
                            flickSeg.duration   = 0.0;
                            flickSeg.trackIndex = tailTrack;
                            flickSeg.dtrack     = nc.m_dtrack;
                            flickSeg.metadata   = nc.m_metadata;
                            flickSeg.sampleBinding = nc.m_sampleBinding;
                            flickSeg.customColors  = nc.m_customColors;
                            segments.push_back(flickSeg);

                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            XINFO(
                                "Note merge [1-1]: appended Flick from "
                                "entity {} as new seg",
                                static_cast<uint32_t>(entity));
                        } else if ( lastSeg.type == ::MMM::NoteType::FLICK ) {
                            // 【1-2 相同类型】末尾 subFlick + 目标 Flick
                            // → 延长最后 subFlick 的 dtrack
                            // 以当前 subFlick 的时间为准，将 Flick
                            // 的终点作为新终点
                            // 相同类型连接使用目标实际终轨，不直接累加可能带起轨偏差的几何。
                            // 尾段其他属性保留已有值，目标属性不会自动覆盖整个合并段。
                            int flickEnd = nc.m_trackIndex + nc.m_dtrack;
                            segments.back().dtrack =
                                flickEnd - segments.back().trackIndex;

                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            XINFO(
                                "Note merge [1-2]: extended last "
                                "subFlick to Flick end track={}",
                                flickEnd);
                        }
                        continue;
                    }
                }

                if ( nc.m_type == ::MMM::NoteType::HOLD ) {
                    // 目标是一个独立的 Hold
                    if ( nc.m_trackIndex == tailTrack &&
                         std::abs(nc.m_timestamp - tailTime) <=
                             MERGE_TIME_TOLERANCE ) {
                        if ( lastSeg.type == ::MMM::NoteType::FLICK ) {
                            // 【1-1 不同类型】末尾 subFlick + 目标 Hold
                            // → 将 Hold 作为最后一个 seg 加入
                            // 追加 Hold
                            // 采用目标持续值和修正后的起点，因此连接容差由首时间吸收。
                            // 目标的局部属性随新段保存，原实体留在待删除计划中。
                            NoteComponent::SubNote holdSeg;
                            holdSeg.type          = ::MMM::NoteType::HOLD;
                            holdSeg.timestamp     = tailTime;  // 修复微小时间差
                            holdSeg.duration      = nc.m_duration;
                            holdSeg.trackIndex    = tailTrack;
                            holdSeg.dtrack        = 0;
                            holdSeg.metadata      = nc.m_metadata;
                            holdSeg.sampleBinding = nc.m_sampleBinding;
                            holdSeg.customColors  = nc.m_customColors;
                            segments.push_back(holdSeg);

                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            XINFO(
                                "Note merge [1-1]: appended Hold from "
                                "entity {} as new seg",
                                static_cast<uint32_t>(entity));
                        } else if ( lastSeg.type == ::MMM::NoteType::HOLD ) {
                            // 【1-2 相同类型】末尾 subHold + 目标 Hold
                            // → 直接延长最后 subHold 的持续时间
                            // 延长已有 Hold
                            // 采用目标绝对结束时间，原尾段起点保持不变。
                            // 这与不同类型追加保留时长的规则不同，不应无条件互换两种算法。
                            double holdEnd = nc.m_timestamp + nc.m_duration;
                            segments.back().duration =
                                holdEnd - segments.back().timestamp;

                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            XINFO(
                                "Note merge [1-2]: extended last "
                                "subHold to Hold end t={:.3f}",
                                holdEnd);
                        }
                        continue;
                    }
                }

                // ===== 【2】结合另一个折线 =====
                if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                     !nc.m_subNotes.empty() ) {
                    // 目标折线的连接头以第一子项为准，不假定父基础字段一定代表所有子项。
                    // 目标内嵌列表只读，复制后的段才应用连接处时间修正。
                    const auto& targetFirst = nc.m_subNotes.front();
                    // 目标折线头部的时间和轨道
                    double targetHeadTime  = targetFirst.timestamp;
                    int    targetHeadTrack = targetFirst.trackIndex;

                    if ( targetHeadTrack == tailTrack &&
                         std::abs(targetHeadTime - tailTime) <=
                             MERGE_TIME_TOLERANCE ) {
                        if ( lastSeg.type != targetFirst.type ) {
                            // 【2-1 不同类型】直接追加目标折线的所有 seg
                            for ( size_t si = 0; si < nc.m_subNotes.size();
                                  ++si ) {
                                auto seg = nc.m_subNotes[si];
                                // 修复首段连接处的微小时间差
                                // 仅消除连接首段的微小时间偏差，后续段保持原始绝对时间。
                                // 不能把容差平移广播到整个目标折线，否则其余拍位也会改变。
                                if ( si == 0 ) {
                                    seg.timestamp = tailTime;
                                }
                                segments.push_back(seg);
                            }

                            // 删除目标折线及其子物件实体
                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            appendPolylineChildDeleteEntries(
                                ctx, entity, mergeDeleteEntries);
                            XINFO(
                                "Note merge [2-1]: appended {} segs "
                                "from Polyline entity {}",
                                nc.m_subNotes.size(),
                                static_cast<uint32_t>(entity));
                        } else {
                            // 【2-2 相同类型】延长尾段，追加剩余 seg
                            if ( targetFirst.type == ::MMM::NoteType::FLICK ) {
                                // 两个 subFlick 合并：
                                // 将目标首个 subFlick 的终点替换到用户绘制的
                                // subFlick 的终点
                                // 两个 Flick
                                // 首尾接合时保留用户段起点，并连接到目标首段箭头末端。
                                // 首段不再单独追加，否则会重复表达同一横移。
                                int targetFirstEnd =
                                    targetFirst.trackIndex + targetFirst.dtrack;
                                segments.back().dtrack =
                                    targetFirstEnd - segments.back().trackIndex;
                                // 以用户绘制的 subFlick 时间为准（不修改时间）
                            } else if ( targetFirst.type ==
                                        ::MMM::NoteType::HOLD ) {
                                // 两个 subHold 合并：
                                // 将目标首个 subHold 的结束时间作为新结束时间
                                // 两个 Hold
                                // 接合后时长由目标首段结束时间减现有尾段起点得到。
                                // 剩余目标子项从索引一开始追加，保持后续交替结构。
                                double targetFirstEnd = targetFirst.timestamp +
                                                        targetFirst.duration;
                                segments.back().duration =
                                    targetFirstEnd - segments.back().timestamp;
                            }

                            // 追加目标折线除首段外的所有 seg
                            for ( size_t si = 1; si < nc.m_subNotes.size();
                                  ++si ) {
                                segments.push_back(nc.m_subNotes[si]);
                            }

                            // 删除目标折线及其子物件实体
                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            appendPolylineChildDeleteEntries(
                                ctx, entity, mergeDeleteEntries);
                            XINFO(
                                "Note merge [2-2]: extended tail and "
                                "appended {} remaining segs from Polyline "
                                "entity {}",
                                nc.m_subNotes.size() - 1,
                                static_cast<uint32_t>(entity));
                        }
                        continue;
                    }
                }
            }
        }

        // 3.5. 移除折线路径上的物件 (如果设置开启)
        // 该选项当前按路径节点匹配物件起点，不做段内部连续几何相交检测。
        // 候选仅限独立 NOTE、HOLD、FLICK，完整折线不在这段清理中删除。
        if ( ctx.lastConfig.settings.removeObjectsOnPolylinePath ) {
            /// @brief 提交阶段用于起点重叠匹配的路径节点。
            struct Node {
                /// @brief 节点绝对轨号，草稿保留负号。
                int track;
                /// @brief 节点秒时间，用于三毫秒容差匹配。
                double time;
            };
            // 节点列表只用于本次删除匹配，不承担折线渲染或拾取缓存职责。
            // 它包含首起点及各段末端，因此首尾都能参与重叠检查。
            std::vector<Node> nodes;
            if ( !segments.empty() ) {
                // 首起点不能只从每段末端列表推导，否则路径头部重叠点击会遗漏。
                // 后续相邻节点允许重复，匹配结果通过实体去重而不是节点排序消重。
                nodes.push_back({ segments.front().trackIndex,
                                  segments.front().timestamp });
                for ( const auto& s : segments ) {
                    if ( s.type == ::MMM::NoteType::HOLD ) {
                        nodes.push_back(
                            { s.trackIndex, s.timestamp + s.duration });
                    } else if ( s.type == ::MMM::NoteType::FLICK ) {
                        nodes.push_back(
                            { s.trackIndex + s.dtrack, s.timestamp });
                    }
                }
            }

            // 尾部连接可能已准备删除同一对象，路径清理必须复用这份去重基础。
            // 避免一个批次对相同身份生成重复删除记录。
            std::unordered_set<entt::entity> alreadyQueued;
            // 同一实体可能既满足尾部连接也满足路径节点匹配。
            // 复制已有删除身份作为查重集合，保留两项设置共同生效时的一次删除语义。
            for ( const auto& entry : mergeDeleteEntries ) {
                if ( entry.entity != entt::null ) {
                    alreadyQueued.insert(entry.entity);
                }
            }

            auto noteView = ctx.noteRegistry.view<NoteComponent>();
            for ( auto entity : noteView ) {
                const auto& nc = noteView.get<NoteComponent>(entity);
                if ( nc.m_isSubNote ) continue;

                if ( nc.m_type == ::MMM::NoteType::NOTE ||
                     nc.m_type == ::MMM::NoteType::HOLD ||
                     nc.m_type == ::MMM::NoteType::FLICK ) {

                    // 逐个候选检查其起点是否落在任一路径节点。
                    // 命中即停止节点遍历，不按同一物件覆盖多个节点重复计数。
                    bool match = false;
                    for ( const auto& node : nodes ) {
                        if ( nc.m_trackIndex == node.track &&
                             std::abs(nc.m_timestamp - node.time) <= 0.003 ) {
                            match = true;
                            break;
                        }
                    }

                    // 节点命中只决定候选资格，实体是否已经排队另行判断。
                    // 匹配和提交分开，最终时间校验失败时仍可放弃整个未执行计划。
                    if ( match ) {
                        if ( alreadyQueued.find(entity) ==
                             alreadyQueued.end() ) {
                            mergeDeleteEntries.push_back(
                                { entity, nc, std::nullopt });
                            alreadyQueued.insert(entity);
                            XINFO(
                                "Polyline path clean: removing object {} at "
                                "t={:.3f} "
                                "track={}",
                                static_cast<uint32_t>(entity),
                                nc.m_timestamp,
                                nc.m_trackIndex);
                        }
                    }
                }
            }
        }

        // 纯长条反向拖绘允许零长度 Hold；折线及滑键手势的零值段仍须清理。
        // 纯持续手势允许反向拖到零长度，曾形成 Flick 或折线则不能套用该例外。
        // 保留条件同时要求单段 Hold，防止保留折线中的临时零段。
        const bool preserveZeroLengthHold =
            !ctx.brushState.hasPolylineGesture &&
            note.m_type == ::MMM::NoteType::HOLD && segments.size() == 1U &&
            segments.front().type == ::MMM::NoteType::HOLD &&
            segments.front().duration == 0.0;
        // 3.8 对合并/清洗后的最终段再次进行深度清洗与递归简化
        // 尾部合并可能再次产生同类邻段或零跨度，需要对最终结果再清理。
        // 处理的是合并后的工作列表，不重新搜索更多连接目标。
        // 第二轮清理还覆盖独立 Flick 经尾部连接后的抵消结果。
        // 保留零 Hold 的例外在循环外判定，循环内部不为任意零段开启特例。
        if ( !preserveZeroLengthHold ) {
            bool changed = true;
            while ( changed ) {
                changed = false;

                // 1. 过滤所有“零值”段：0长度Hold 或 0位移Flick
                auto it = std::remove_if(
                    segments.begin(), segments.end(), [](const auto& s) {
                        if ( s.type == ::MMM::NoteType::HOLD )
                            return s.duration < 1e-5;
                        if ( s.type == ::MMM::NoteType::FLICK )
                            return s.dtrack == 0;
                        return false;
                    });
                if ( it != segments.end() ) {
                    segments.erase(it, segments.end());
                    changed = true;
                }

                // 2. 合并连续的同类型物件
                // 少于两段时不做相邻比较，避免 size
                // 减一在空列表上产生无符号下溢。 每次 erase
                // 后立即重新进入循环，不继续使用已失效的 next 引用。
                if ( segments.size() > 1 ) {
                    for ( size_t i = 0; i < segments.size() - 1; ) {
                        auto& curr = segments[i];
                        auto& next = segments[i + 1];

                        // 类型相同仍只合并 Hold 与
                        // Flick，普通点击节点继续保留分段作用。
                        // 这些几何合并不会把后段绑定或颜色覆盖到前段。
                        if ( curr.type == next.type ) {
                            if ( curr.type == ::MMM::NoteType::HOLD ) {
                                // 合并长条持续时间
                                curr.duration += next.duration;
                                segments.erase(segments.begin() + i + 1);
                                changed = true;
                                continue;  // 继续检查合并后的段
                            } else if ( curr.type == ::MMM::NoteType::FLICK ) {
                                // 合并滑键位移量
                                curr.dtrack += next.dtrack;
                                segments.erase(segments.begin() + i + 1);
                                changed = true;
                                continue;
                            }
                        }
                        i++;
                    }
                }
            }
        }

        // 4. 根据最终清洗与合并结果进行降级或重构
        // 全部段消失时保留画笔起点为普通点击，清除持续和横移字段。
        // 不因几何退化静默丢弃整次画笔动作。
        if ( segments.empty() ) {
            note.m_type     = ::MMM::NoteType::NOTE;
            note.m_duration = 0.0;
            note.m_dtrack   = 0;
            // 空几何降级时清空父子结构标识的容器部分，防止 NOTE
            // 携带旧折线数据。 根基础时间在这个分支仍保留初始画笔值。
            note.m_subNotes.clear();
        } else if ( segments.size() == 1 ) {
            // 单段结果降级为独立物件，根几何改用清理后剩余段的位置。
            // 该分支只拷贝几何字段，根绑定和颜色仍使用前面准备的值。
            auto s            = segments[0];
            note.m_type       = s.type;
            note.m_timestamp  = s.timestamp;
            note.m_duration   = s.duration;
            note.m_trackIndex = s.trackIndex;
            note.m_dtrack     = s.dtrack;
            note.m_subNotes.clear();
        } else {
            // 多段保留完整局部属性，并以首段重新确定根时间和轨道。
            // ECS 子实体尚未创建，后续按最终顺序一次分配。
            note.m_subNotes = segments;
            if ( !note.m_subNotes.empty() ) {
                note.m_timestamp  = note.m_subNotes.front().timestamp;
                note.m_trackIndex = note.m_subNotes.front().trackIndex;
                note.m_type       = ::MMM::NoteType::POLYLINE;
            }
        }
    }

    // 最终检查在合并后进行，目标对象贡献的子时间也属于验证范围。
    // 失败时删除计划仍只是值记录，不会误删连接候选。
    if ( !isPlaceableNote(note) ) {
        XWARN("DrawTool: blocked note placement before 0s (time={:.3f})",
              note.m_timestamp);
        resetBrushState(ctx);
        return;
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        // 创建折线父实体及所有子物件实体
        // 先预留父身份，子组件可在同批次中引用它。
        // before 为空区分创建与更新，撤销时可删除新父及所有新子。
        entt::entity parentEnt = ctx.noteRegistry.create();
        mergeDeleteEntries.push_back({ parentEnt, std::nullopt, note });

        for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
            const auto&   s     = note.m_subNotes[i];
            NoteComponent subNC = makeNoteComponentFromSubNote(
                s, true, parentEnt, static_cast<int>(i));
            // 父域统一传递给子实体，不能让负轨折线的子项仍按玩家物件绘制。
            // 索引来自最终列表，清理前的下标不再适用于新结构。
            subNC.m_isDraft = note.m_isDraft;

            entt::entity subEnt = ctx.noteRegistry.create();
            mergeDeleteEntries.push_back({ subEnt, std::nullopt, subNC });
        }

        // 删除候选、创建父和创建子一起进入同一历史记录。
        // 动作取得条目所有权后，工具不再读取移动后的局部向量。
        auto action = std::make_unique<BatchNoteAction>(
            std::move(mergeDeleteEntries), "Polyline Create");
        ctx.actionStack.pushAndExecute(std::move(action), ctx);
    } else {
        // 非折线降级物件 (NOTE / HOLD / FLICK)
        // 有合并删除时，即使结果降级成普通物件也必须使用组合批次。
        // 这样一次撤销同时恢复旧对象并移除新结果。
        if ( !mergeDeleteEntries.empty() ) {
            mergeDeleteEntries.push_back(
                { ctx.noteRegistry.create(), std::nullopt, note });
            auto action = std::make_unique<BatchNoteAction>(
                std::move(mergeDeleteEntries), "Note Create & Merge");
            ctx.actionStack.pushAndExecute(std::move(action), ctx);
        } else {
            // 没有删除候选的普通新物件使用单对象创建动作。
            // 此路径由动作自行分配身份，保持空 before 的创建语义。
            auto action = std::make_unique<NoteAction>(
                NoteAction::Type::Create, entt::null, std::nullopt, note);
            ctx.actionStack.pushAndExecute(std::move(action), ctx);
        }
    }

    // 重置状态
    resetBrushState(ctx);
}

/// @brief 建立橡皮擦预览目标并记录当前 Shift 模式。
/// @param ctx 保存悬浮身份、对象领域和擦除手势的会话。
/// @param cmd 当前擦除输入及 Shift 状态。
/// @warning 输入路径只记录当前命中身份，完整选择扫描与级联删除推迟到释放。
/// @note Shift 将折线子实体提升为父目标，普通模式保留命中身份。
/// @note 无命中仍建立活动擦除手势，后续移动可取得第一个目标。
/// @note 预览集合只携带身份，不能直接作为跨线程的组件快照。
void DrawTool::handleStartErase(SessionContext& ctx, const CmdStartErase& cmd)
{
    // 目标集合描述当前预览，不是沿鼠标轨迹累计的擦除历史。
    // 起笔时清空旧目标，防止上一笔未完成状态混入本次释放。
    ctx.eraserState.isActive    = true;
    ctx.eraserState.isShiftDown = cmd.isShiftDown;
    ctx.eraserState.targetEntities.clear();
    // Note 和草稿共用 Note Registry，但领域标签仍保留命中类型。
    // 只读悬浮校验已经确认组件存在，后面才能安全解析父关系。
    if ( isHoveredPlayerNote(ctx) ) {
        ctx.eraserState.targetObjectKind = ctx.hoveredObjectKind;
        entt::entity target              = ctx.hoveredEntity;
        // Shift 模式：如果悬停在 Polyline 的子物件上，解析到父 Polyline 实体
        if ( cmd.isShiftDown ) {
            const auto& nc =
                ctx.noteRegistry.get<NoteComponent>(ctx.hoveredEntity);
            if ( nc.m_isSubNote && nc.m_parentPolyline != entt::null ) {
                target = nc.m_parentPolyline;
            }
        }
        // 只保存本地身份，擦除前快照在释放时从有效组件读取。
        // 这允许移动期间继续变化预览目标而不提前创建历史动作。
        ctx.eraserState.targetEntities.insert(target);
    } else if ( isHoveredAudioSample(ctx) ) {
        ctx.eraserState.targetObjectKind = ChartObjectKind::AudioSample;
        ctx.eraserState.targetEntities.insert(ctx.hoveredEntity);
    }
}

/// @brief 用当前悬浮对象替换橡皮擦的预览目标。
/// @param ctx 保存悬浮身份、对象领域和擦除手势的会话。
/// @param cmd 当前擦除输入及 Shift 状态。
/// @warning 输入路径只记录当前命中身份，完整选择扫描与级联删除推迟到释放。
/// @note Shift 将折线子实体提升为父目标，普通模式保留命中身份。
/// @note 当前目标集合每轮替换，重复经过同一对象不会增加删除次数。
/// @note Shift 松开后重新解释当前悬浮目标，不保留上轮整条删除模式。
void DrawTool::handleUpdateErase(SessionContext& ctx, const CmdUpdateErase& cmd)
{
    if ( !ctx.eraserState.isActive ) return;

    // Shift 在同一擦除手势内可以变化，释放采用最后更新的模式。
    // 每轮重新从悬浮身份解析目标，不沿用上轮提升后的父实体。
    ctx.eraserState.isShiftDown = cmd.isShiftDown;

    // 每帧只标记当前鼠标正下方的物件，移开就取消
    // 鼠标离开所有对象时留下空集合，释放不会删除此前经过的对象。
    // 空集合下的领域标签可能仍是旧值，结束入口必须允许空批次。
    ctx.eraserState.targetEntities.clear();
    if ( isHoveredPlayerNote(ctx) ) {
        ctx.eraserState.targetObjectKind = ctx.hoveredObjectKind;
        entt::entity target              = ctx.hoveredEntity;
        // Shift 模式：如果悬停在 Polyline 的子物件上，解析到父 Polyline 实体
        if ( cmd.isShiftDown ) {
            const auto& nc =
                ctx.noteRegistry.get<NoteComponent>(ctx.hoveredEntity);
            if ( nc.m_isSubNote && nc.m_parentPolyline != entt::null ) {
                target = nc.m_parentPolyline;
            }
        }
        ctx.eraserState.targetEntities.insert(target);
    } else if ( isHoveredAudioSample(ctx) ) {
        ctx.eraserState.targetObjectKind = ChartObjectKind::AudioSample;
        ctx.eraserState.targetEntities.insert(ctx.hoveredEntity);
    }
}

/// @brief 根据最终擦除目标提交采样删除或音符删除与折线分裂。
/// @param ctx 当前手势、最终悬浮部件以及两类实体 Registry。
/// @param cmd 结束命令；处理采用会话已保存的最终目标和模式。
/// @warning
/// 释放低频路径：可扫描选中集合、折线子实体并构造撤销批次，不得逐帧调用。
/// @note 擦中选中对象会扩展到该领域其他选中对象，不跨 Registry 删除。
/// @note 非 Shift 的折线局部擦除可重建左右部分；Shift 按完整对象删除。
/// @pre 释放期间 Registry 结构由会话串行管理，快照准备阶段不能并发删除组件。
/// @note 若目标已失效则跳过，不通过旧实体号重建缺失对象再删除。
/// @note 擦中未选中对象时不会因其他对象已选中而扩大范围。
/// @note 局部分裂仅处理当前悬浮的父身份，选中集合里的其他折线走普通删除。
/// @note 新余段先预留实体，再随批次安装组件；准备阶段不直接改写原折线。
void DrawTool::handleEndErase(SessionContext& ctx, const CmdEndErase& cmd)
{
    if ( !ctx.eraserState.isActive ) return;

    // 领域决定实体号的解释方式，采样分支结束后不得继续处理 Note Registry。
    // 即使当前目标集合为空，也需走完手势标志清理。
    if ( ctx.eraserState.targetObjectKind == ChartObjectKind::AudioSample ) {
        std::vector<BatchSampleAction::Entry> entries;
        // 只有擦中一个选中采样才扩展到所有选中采样。
        // 存在其他选中采样并不意味着擦除未选中目标时也应删除它们。
        bool targetIsSelected = false;
        for ( const auto entity : ctx.eraserState.targetEntities ) {
            // 缺失交互组件的采样可作为直接删除目标，但不能触发选中集合扩展。
            // 选择判断使用释放时状态，不使用擦除开始时的缓存副本。
            const auto* interaction =
                ctx.sampleRegistry.try_get<const InteractionComponent>(entity);
            if ( interaction && interaction->isSelected ) {
                targetIsSelected = true;
                break;
            }
        }

        std::unordered_set<entt::entity> toDelete;
        // 选择集合在释放时读取，预览移动不承担完整扫描。
        // 同号 Note 实体不参与本次采样删除计划。
        if ( targetIsSelected ) {
            const auto selectedView =
                ctx.sampleRegistry
                    .view<InteractionComponent, SampleComponent>();
            for ( const auto entity : selectedView ) {
                if ( selectedView.get<InteractionComponent>(entity)
                         .isSelected ) {
                    toDelete.insert(entity);
                }
            }
        }
        // 显式目标与选择扩展取并集，集合消除重复身份。
        // 目标在手势期间可能失效，读取删除快照前再次检查组件。
        for ( const auto entity : ctx.eraserState.targetEntities ) {
            if ( ctx.sampleRegistry.valid(entity) &&
                 ctx.sampleRegistry.all_of<SampleComponent>(entity) ) {
                toDelete.insert(entity);
            }
        }

        // 为最终有效身份预留批次容量，删除仍延迟到条目全部准备完成。
        // 采样可能在读取前失去组件，实际条目数可以小于预留数量。
        entries.reserve(toDelete.size());
        for ( const auto entity : toDelete ) {
            const auto* sample =
                ctx.sampleRegistry.try_get<const SampleComponent>(entity);
            if ( !sample ) continue;
            const auto* interaction =
                ctx.sampleRegistry.try_get<const InteractionComponent>(entity);
            // 保存完整采样 before，包括引用、音量与毫秒偏移。
            // 选择状态采用 optional 区分没有交互组件与明确未选中。
            entries.push_back({
                .entity = entity,
                .before = *sample,
                .after  = std::nullopt,
                .beforeSelected =
                    interaction ? std::optional<bool>{ interaction->isSelected }
                                : std::nullopt,
            });
        }
        // 没有有效目标时不创建空历史项，但后面仍重置擦除手势。
        // 批次统一执行删除并持有恢复所需的采样快照。
        if ( !entries.empty() ) {
            ctx.actionStack.pushAndExecute(
                std::make_unique<BatchSampleAction>(std::move(entries),
                                                    "橡皮擦删除自动采样"),
                ctx);
        }

        // 先结束活动状态，再清除目标与模式，后续更新不会继续复用已删除身份。
        // 对象种类恢复玩家默认值，但这不表示创建了新的玩家目标。
        ctx.eraserState.isActive         = false;
        ctx.eraserState.isShiftDown      = false;
        ctx.eraserState.targetObjectKind = ChartObjectKind::PlayerNote;
        ctx.eraserState.targetEntities.clear();
        return;
    }

    // Note 分支先跳过空预览，避免仅因场上有选中物件就触发删除。
    // 分裂所需子索引来自最终悬浮状态，不从旧起笔位置复用。
    if ( !ctx.eraserState.targetEntities.empty() ) {
        std::vector<BatchNoteAction::Entry> entries;
        bool                                targetIsSelected = false;

        // 1. 检查擦除目标中是否有物件处于选中状态
        for ( auto entity : ctx.eraserState.targetEntities ) {
            if ( ctx.noteRegistry.valid(entity) &&
                 ctx.noteRegistry.all_of<InteractionComponent>(entity) ) {
                if ( ctx.noteRegistry.get<InteractionComponent>(entity)
                         .isSelected ) {
                    targetIsSelected = true;
                    break;
                }
            }
        }

        if ( targetIsSelected ) {
            // 场景 A: 擦除的目标中包含选中物件 -> 删除所有选中的物件 +
            // 擦除的目标物件
            // 选择扩展和显式目标可能重叠，先按本地身份去重再准备条目。
            // 此分支沿用当前选择集合，不对每个选中物件重新执行绘制工具门禁。
            std::unordered_set<entt::entity> toDelete;

            // 添加所有选中的
            // 视图交集确保选择扩展得到的身份都有 Note 组件。
            // 仅有交互组件的非音符实体不会被误加入删除集合。
            auto view =
                ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
            for ( auto entity : view ) {
                if ( view.get<InteractionComponent>(entity).isSelected ) {
                    toDelete.insert(entity);
                }
            }

            // 添加所有本次擦除目标的（包括未选中的）
            for ( auto entity : ctx.eraserState.targetEntities ) {
                if ( ctx.noteRegistry.valid(entity) &&
                     ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                    toDelete.insert(entity);
                }
            }

            for ( auto entity : toDelete ) {
                const auto& nc = ctx.noteRegistry.get<NoteComponent>(entity);
                // handled
                // 表示该对象已被局部分裂方案覆盖，而非仅表示类型是折线。
                // 条件不满足时必须回到普通删除，避免目标没有对应历史条目。
                bool handled = false;

                // 特殊逻辑：如果是
                // Polyline，且当前悬停在它上面，则执行“分裂/缩减”而非“直接全部删除”
                // 选中组中的折线只有同时属于当前擦除目标才可能局部分裂。
                // 其他选中折线按完整删除处理，不能复用当前鼠标子索引。
                if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                     !nc.m_subNotes.empty() &&
                     ctx.eraserState.targetEntities.count(entity) &&
                     !ctx.eraserState.isShiftDown ) {
                    // 局部擦除只接受当前目标对应的有效子索引。
                    // 不能把另一对象的悬浮索引套用到本折线，即使两者子项数相同。
                    int k = ctx.hoveredSubIndex;
                    if ( isHoveredPlayerNote(ctx) &&
                         entity == ctx.hoveredEntity && k >= 0 &&
                         k < static_cast<int>(nc.m_subNotes.size()) ) {

                        // 1. 收集并删除该 Polyline 的所有旧子物件实体
                        appendPolylineChildDeleteEntries(ctx, entity, entries);

                        // 2. 删除原 Polyline 实体
                        entries.push_back({ entity, nc, std::nullopt });

                        // 3. 计算左右半段
                        // 只为内部索引生成左右余段，k
                        // 为零时保持当前整条删除语义。 被擦除的第 k
                        // 段不包含在任一余段中。
                        if ( k > 0 ) {
                            // 先复制原子项列表，左右部分的输入与即将删除的父组件独立。
                            // 撤销快照仍保留原父完整结构，新结果按各自余段重建。
                            auto subNotes = nc.m_subNotes;
                            // 左区间不包含被删段，右区间从下一段起一直保留到末尾。
                            // 这些半开区间保持原顺序，不重新排序时间或轨道。
                            std::vector<NoteComponent::SubNote> L(
                                subNotes.begin(), subNotes.begin() + k);
                            std::vector<NoteComponent::SubNote> R(
                                subNotes.begin() + k + 1, subNotes.end());

                            // 空部分不创建占位对象；单段降级，多段建立新的父子实体组。
                            // 左部分保留原头部语义，右部分不能重复继承原父的播放绑定。
                            /// @brief
                            /// 把一个保留部分追加为独立物件或新折线创建条目。
                            /// @param part 已排除擦除段的连续子项列表。
                            /// @param inheritsParentSound 是否包含原父头部。
                            /// @warning
                            /// 仅在释放时构造值与预留实体，不执行音频加载。
                            auto processPart =
                                [&](const std::vector<NoteComponent::SubNote>&
                                         part,
                                    bool inheritsParentSound) {
                                    // 擦除末段可使右部分为空，这是正常边界，无需创建空折线。
                                    if ( part.empty() ) return;
                                    if ( part.size() == 1 ) {
                                        // 单段恢复为根组件，清除父关联和子索引以参与普通物件渲染。
                                        // 几何、绑定和颜色优先来自保留下来的子项。
                                        auto          s = part[0];
                                        NoteComponent nextNC =
                                            makeNoteComponentFromSubNote(
                                                s, false, entt::null, -1);
                                        nextNC.m_isDraft = nc.m_isDraft;
                                        // 子项已有音效不覆盖；仅包含原头部的一侧才允许回退父绑定。
                                        // 否则分裂后的两侧会在不同时间重复播放原根音效。
                                        if ( !nextNC.m_sampleBinding &&
                                             inheritsParentSound ) {
                                            nextNC.m_sampleBinding =
                                                nc.m_sampleBinding;
                                        }
                                        // 颜色回退采用整组规则：子项没有任何覆盖时才复制父颜色。
                                        // 部分槽位已设置时不在这里逐槽补齐，保留现有样式继承行为。
                                        if ( !hasAnyNoteColorOverride(
                                                 nextNC.m_customColors) &&
                                             hasAnyNoteColorOverride(
                                                 nc.m_customColors) ) {
                                            nextNC.m_customColors =
                                                nc.m_customColors;
                                            // 同步颜色字段与元数据表示，避免保存后丢失刚继承的颜色。
                                            writeNoteColorOverridesToMetadata(
                                                nextNC);
                                        }

                                        // 余段使用新实体身份，旧实体完整留在删除
                                        // before 中。
                                        // 撤销按删除新对象并恢复旧对象重建分裂前状态。
                                        entt::entity newEnt =
                                            ctx.noteRegistry.create();
                                        entries.push_back(
                                            { newEnt, std::nullopt, nextNC });
                                    } else {
                                        // 多段部分创建新的根，基本时间和轨道取该部分首段。
                                        // 根不使用单段 duration 或
                                        // dtrack，完整几何由子列表表达。
                                        NoteComponent nextNC;
                                        nextNC.m_type =
                                            ::MMM::NoteType::POLYLINE;
                                        nextNC.m_timestamp =
                                            part.front().timestamp;
                                        nextNC.m_trackIndex =
                                            part.front().trackIndex;
                                        nextNC.m_duration = 0.0;
                                        nextNC.m_dtrack   = 0;
                                        // 父元数据沿用原折线，音效继承另受头部归属限制。
                                        // 不能因为元数据被复制就无条件复制父级音效绑定。
                                        nextNC.m_metadata = nc.m_metadata;
                                        if ( inheritsParentSound ) {
                                            nextNC.m_sampleBinding =
                                                nc.m_sampleBinding;
                                        }
                                        nextNC.m_customColors =
                                            nc.m_customColors;
                                        // 分裂出的每一部分都成为独立根，不再指向原来将被删除的父。
                                        // 旧父身份只能保留在撤销快照中，不能进入新根的关系字段。
                                        nextNC.m_isSubNote      = false;
                                        nextNC.m_isDraft        = nc.m_isDraft;
                                        nextNC.m_parentPolyline = entt::null;
                                        nextNC.m_subIndex       = -1;
                                        // 完整复制保留段的局部属性，随后按新列表顺序生成连续子索引。
                                        // 新根不沿用原父身份，子实体必须引用新分配的
                                        // parentEnt。
                                        nextNC.m_subNotes = part;

                                        // 为多段余部预留父身份后，所有新子项用该身份建立关系。
                                        // 子索引从零开始，不继续使用原折线被擦除位置之后的旧索引。
                                        entt::entity parentEnt =
                                            ctx.noteRegistry.create();
                                        entries.push_back({ parentEnt,
                                                            std::nullopt,
                                                            nextNC });

                                        for ( size_t i = 0; i < part.size();
                                              ++i ) {
                                            const auto&   s = part[i];
                                            NoteComponent subNC =
                                                makeNoteComponentFromSubNote(
                                                    s,
                                                    true,
                                                    parentEnt,
                                                    static_cast<int>(i));
                                            // 分裂不跨域，所有新子实体继承原父草稿标志。
                                            // 不可仅依赖子轨号而留下与根不同的编辑可见性。
                                            subNC.m_isDraft = nc.m_isDraft;

                                            entt::entity subEnt =
                                                ctx.noteRegistry.create();
                                            entries.push_back({ subEnt,
                                                                std::nullopt,
                                                                subNC });
                                        }
                                    }
                                };

                            // 左半包含原始头部，右半从断点之后开始，音效继承标志据此区分。
                            // 两半的创建条目与原父子删除同批提交，避免出现可单独撤销的半次分裂。
                            processPart(L, true);
                            processPart(R, false);
                        }

                        // 原对象已有删除记录，即便余段均为空也算处理完成。
                        // 阻止普通删除兜底再追加同一父身份。
                        handled = true;
                    }
                }

                // 局部分裂未接管的目标仍按整对象删除。
                // 对应旧子实体在最后的集中关联扫描中补齐，不在此重复扫描父关系。
                if ( !handled ) {
                    entries.push_back({ entity, nc, std::nullopt });
                }
            }
            XINFO("Eraser: Processing all {} items (selected + targets)",
                  entries.size());
        } else {
            // 场景 B: 擦除的目标全都是未选中物件 ->
            // 只处理这些目标物件（支持缩减/分裂）
            for ( auto entity : ctx.eraserState.targetEntities ) {
                if ( ctx.noteRegistry.valid(entity) &&
                     ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                    const auto& nc =
                        ctx.noteRegistry.get<NoteComponent>(entity);
                    bool handled = false;

                    if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                         !nc.m_subNotes.empty() &&
                         !ctx.eraserState.isShiftDown ) {
                        // 局部擦除只接受当前目标对应的有效子索引。
                        // 不能把另一对象的悬浮索引套用到本折线，即使两者子项数相同。
                        int k = ctx.hoveredSubIndex;
                        if ( isHoveredPlayerNote(ctx) &&
                             entity == ctx.hoveredEntity && k >= 0 &&
                             k < static_cast<int>(nc.m_subNotes.size()) ) {

                            // 1. 收集并删除该 Polyline 的所有旧子物件实体
                            appendPolylineChildDeleteEntries(
                                ctx, entity, entries);

                            // 2. 删除原 Polyline 实体
                            entries.push_back({ entity, nc, std::nullopt });

                            // 3. 计算左右半段
                            // 只为内部索引生成左右余段，k
                            // 为零时保持当前整条删除语义。 被擦除的第 k
                            // 段不包含在任一余段中。
                            if ( k > 0 ) {
                                // 先复制原子项列表，左右部分的输入与即将删除的父组件独立。
                                // 撤销快照仍保留原父完整结构，新结果按各自余段重建。
                                auto subNotes = nc.m_subNotes;
                                std::vector<NoteComponent::SubNote> L(
                                    subNotes.begin(), subNotes.begin() + k);
                                std::vector<NoteComponent::SubNote> R(
                                    subNotes.begin() + k + 1, subNotes.end());

                                // 空部分不创建占位对象；单段降级，多段建立新的父子实体组。
                                // 左部分保留原头部语义，右部分不能重复继承原父的播放绑定。
                                /// @brief
                                /// 把一个保留部分追加为独立物件或新折线创建条目。
                                /// @param part 已排除擦除段的连续子项列表。
                                /// @param inheritsParentSound
                                /// 是否包含原父头部。
                                /// @warning
                                /// 仅在释放时构造值与预留实体，不执行音频加载。
                                auto processPart = [&](const std::vector<
                                                           NoteComponent::
                                                               SubNote>& part,
                                                       bool
                                                           inheritsParentSound) {
                                    // 擦除末段可使右部分为空，这是正常边界，无需创建空折线。
                                    if ( part.empty() ) return;
                                    if ( part.size() == 1 ) {
                                        // 单段恢复为根组件，清除父关联和子索引以参与普通物件渲染。
                                        // 几何、绑定和颜色优先来自保留下来的子项。
                                        auto          s = part[0];
                                        NoteComponent nextNC =
                                            makeNoteComponentFromSubNote(
                                                s, false, entt::null, -1);
                                        nextNC.m_isDraft = nc.m_isDraft;
                                        // 子项已有音效不覆盖；仅包含原头部的一侧才允许回退父绑定。
                                        // 否则分裂后的两侧会在不同时间重复播放原根音效。
                                        if ( !nextNC.m_sampleBinding &&
                                             inheritsParentSound ) {
                                            nextNC.m_sampleBinding =
                                                nc.m_sampleBinding;
                                        }
                                        // 颜色回退采用整组规则：子项没有任何覆盖时才复制父颜色。
                                        // 部分槽位已设置时不在这里逐槽补齐，保留现有样式继承行为。
                                        if ( !hasAnyNoteColorOverride(
                                                 nextNC.m_customColors) &&
                                             hasAnyNoteColorOverride(
                                                 nc.m_customColors) ) {
                                            nextNC.m_customColors =
                                                nc.m_customColors;
                                            // 同步颜色字段与元数据表示，避免保存后丢失刚继承的颜色。
                                            writeNoteColorOverridesToMetadata(
                                                nextNC);
                                        }

                                        // 余段使用新实体身份，旧实体完整留在删除
                                        // before 中。
                                        // 撤销按删除新对象并恢复旧对象重建分裂前状态。
                                        entt::entity newEnt =
                                            ctx.noteRegistry.create();
                                        entries.push_back(
                                            { newEnt, std::nullopt, nextNC });
                                    } else {
                                        // 多段部分创建新的根，基本时间和轨道取该部分首段。
                                        // 根不使用单段 duration 或
                                        // dtrack，完整几何由子列表表达。
                                        NoteComponent nextNC;
                                        nextNC.m_type =
                                            ::MMM::NoteType::POLYLINE;
                                        nextNC.m_timestamp =
                                            part.front().timestamp;
                                        nextNC.m_trackIndex =
                                            part.front().trackIndex;
                                        nextNC.m_duration = 0.0;
                                        nextNC.m_dtrack   = 0;
                                        // 父元数据沿用原折线，音效继承另受头部归属限制。
                                        // 不能因为元数据被复制就无条件复制父级音效绑定。
                                        nextNC.m_metadata = nc.m_metadata;
                                        if ( inheritsParentSound ) {
                                            nextNC.m_sampleBinding =
                                                nc.m_sampleBinding;
                                        }
                                        nextNC.m_customColors =
                                            nc.m_customColors;
                                        nextNC.m_isSubNote      = false;
                                        nextNC.m_isDraft        = nc.m_isDraft;
                                        nextNC.m_parentPolyline = entt::null;
                                        nextNC.m_subIndex       = -1;
                                        // 完整复制保留段的局部属性，随后按新列表顺序生成连续子索引。
                                        // 新根不沿用原父身份，子实体必须引用新分配的
                                        // parentEnt。
                                        nextNC.m_subNotes = part;

                                        entt::entity parentEnt =
                                            ctx.noteRegistry.create();
                                        entries.push_back({ parentEnt,
                                                            std::nullopt,
                                                            nextNC });

                                        for ( size_t i = 0; i < part.size();
                                              ++i ) {
                                            const auto&   s = part[i];
                                            NoteComponent subNC =
                                                makeNoteComponentFromSubNote(
                                                    s,
                                                    true,
                                                    parentEnt,
                                                    static_cast<int>(i));
                                            // 分裂不跨域，所有新子实体继承原父草稿标志。
                                            // 不可仅依赖子轨号而留下与根不同的编辑可见性。
                                            subNC.m_isDraft = nc.m_isDraft;

                                            entt::entity subEnt =
                                                ctx.noteRegistry.create();
                                            entries.push_back({ subEnt,
                                                                std::nullopt,
                                                                subNC });
                                        }
                                    }
                                };

                                // 左半包含原始头部，右半从断点之后开始，音效继承标志据此区分。
                                // 两半的创建条目与原父子删除同批提交，避免出现可单独撤销的半次分裂。
                                processPart(L, true);
                                processPart(R, false);
                            }

                            // 原对象已有删除记录，即便余段均为空也算处理完成。
                            // 阻止普通删除兜底再追加同一父身份。
                            handled = true;
                        }
                    }

                    if ( !handled ) {
                        entries.push_back(
                            { entity,
                              ctx.noteRegistry.get<NoteComponent>(entity),
                              std::nullopt });
                    }
                }
            }
        }

        // 提交前统一补全剩余父对象的子删除，覆盖普通整条删除及选择扩展。
        // 新增部分此时仅预留身份，尚未安装
        // NoteComponent，不会被关联扫描误当旧子项。
        if ( !entries.empty() ) {
            // 同时删除被擦除折线下所有子物件实体，防止孤儿子实体残留
            // 这里收集批次已有有效身份作为父候选，非父身份不会匹配到子关系。
            // 后续助手同时对已有条目去重，避免局部分裂已记录的子删除重复入栈。
            std::unordered_set<entt::entity> erasedEntities;
            // 扫描的是当前准备批次的身份，包含已经创建但尚未安装组件的实体。
            // 真正子删除是否存在仍由父关系过滤决定，不能对集合所有身份盲目销毁。
            for ( const auto& entry : entries ) {
                if ( entry.entity != entt::null &&
                     ctx.noteRegistry.valid(entry.entity) ) {
                    erasedEntities.insert(entry.entity);
                }
            }
            // 集中追加的助手只读 Registry，条目扩容不会影响原组件视图。
            // 动作执行发生在这个完整扫描结束后，避免边扫描边删除导致漏项。
            appendPolylineChildDeleteEntries(ctx, erasedEntities, entries);

            // 同一动作同时表达原对象删除、余段新建及子关系替换。
            // 完成准备后才执行，避免构造后续条目时访问已删除的原组件。
            auto action = std::make_unique<BatchNoteAction>(std::move(entries));
            ctx.actionStack.pushAndExecute(std::move(action), ctx);
        }
    }

    ctx.eraserState.isActive         = false;
    ctx.eraserState.isShiftDown      = false;
    ctx.eraserState.targetObjectKind = ChartObjectKind::PlayerNote;
    ctx.eraserState.targetEntities.clear();
}

}  // namespace MMM::Logic
