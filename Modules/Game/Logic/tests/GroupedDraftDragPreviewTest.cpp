#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/SelectionState.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/GrabTool.h"
#include "logic/session/tool/MarqueeTool.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace
{
/// @brief 配置四轨玩家区与四轨草稿区，复现多选拖动边界。
/// @param context 待配置会话。
/// @note 玩家区与草稿区刻意采用不同轨宽，防止错误复用玩家区投影也通过断言。
void configureDragContext(MMM::Logic::SessionContext& context)
{
    // 创建最小谱面对象，让工具链按真实会话路径检查可编辑状态。
    // 测试不加载文件，避免把资源解析引入纯拖动回归。
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count = 4;
    // 玩家轨与草稿轨同为四轨，便于验证整组完全跨区后的合法提交。
    // 保留一条 BGM 轨，确保统一轨道右边界与生产配置一致。
    context.trackCount      = 4;
    context.draftTrackCount = 4;
    context.bgmTrackCount   = 1;
    // 当前时间与动画时间保持一致，排除音频补间对纵向坐标的影响。
    // 鼠标固定在判定线上，因此所有预览帧只比较横向轨道变化。
    context.currentTime = 1.0;
    context.animateTime = 1.0;
    // 玩家区固定为 [100, 500)，每轨宽 100；Draft 独立为 [-150, 50)，每轨宽 40。
    // 两区保留 50 像素间隙，350 命中玩家轨 2，30 命中最右 Draft 轨 -1。
    // 判定线位于 300，和后续更新命令的 mouseY 保持一致。
    context.lastConfig.visual.trackLayout.left             = 0.1F;
    context.lastConfig.visual.trackLayout.right            = 0.5F;
    context.lastConfig.visual.trackLayout.draftLanes.left  = -0.15F;
    context.lastConfig.visual.trackLayout.draftLanes.width = 0.04F;
    context.lastConfig.visual.judgeline_pos                = 0.5F;
    // 同时启用草稿轨与折线编辑，确保两个选中物件都进入整组拖动状态。
    // 这里正是生产环境触发统一轨道求解器的配置组合。
    context.lastConfig.settings.professionalMode      = true;
    context.lastConfig.settings.enablePolylineEditing = true;
    // 主画布身份用于开启跨域预览，固定尺寸用于稳定轨道投影。
    // 横向偏移保持为零，避免测试结果依赖相机平移状态。
    // 纵向高度与判定线比例共同得到 300 的基准位置。
    context.cameras.emplace("Basic2DCanvas",
                            MMM::Logic::CameraInfo{
                                "Basic2DCanvas",
                                1000.0F,
                                600.0F,
                                0.0F,
                            });

    // 空谱面仍需滚动缓存，以便拖动命令把判定线坐标换算为时间。
    // 缓存只在初始化时构建，不参与每次横向拖动断言。
    // 使用当前谱面指针可保持与实际 Session 初始化行为一致。
    auto& cache = context.timelineRegistry.ctx()
                      .emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(context.timelineRegistry,
                  context.lastConfig,
                  context.currentBeatmap.get());
}

/// @brief 检查实体 Transform 是否位于统一投影给出的真实轨道左边界。
/// @param context 当前会话。
/// @param entity 待检查音符实体。
/// @param expectedX 预期横坐标。
/// @return 横向误差小于测试容差时返回 true。
/// @pre 实体已经经过一次拖动更新，Transform 组件存在。
bool transformXNear(const MMM::Logic::SessionContext& context,
                    entt::entity entity, float expectedX)
{
    const auto& transform =
        context.noteRegistry.get<const MMM::Logic::TransformComponent>(entity);
    // 检查即时渲染缓存而非重新投影组件，才能发现逻辑已移动但画面仍滞后的问题。
    return std::abs(transform.m_pos.x - expectedX) < 1e-4F;
}

/// @brief 创建横跨四条玩家轨道的选中折线。
/// @param context 当前会话。
/// @return 折线根实体。
/// @note 不生成派生子实体，测试焦点是父组件内嵌节点的整组移动。
entt::entity createSelectedWidePolyline(MMM::Logic::SessionContext& context)
{
    MMM::Logic::NoteComponent polyline{
        .m_type       = MMM::NoteType::POLYLINE,
        .m_timestamp  = 1.0,
        .m_trackIndex = 0,
    };
    // 末节点的 Flick 终点落在轨道 3，使整条折线横跨完整玩家区。
    // 根节点从轨道 0 开始，旧玩家域钳制会因此禁止任何向左预览。
    // 中间节点用于确认公共增量不会只更新首尾物件。
    // Flick 起点位于轨道 2，dtrack=1 将终点边界扩展到轨道 3。
    // 所有子物件时间不同，以确认本修复没有改写纵向结构。
    polyline.m_subNotes = {
        MMM::Logic::NoteComponent::SubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 1.25, .trackIndex = 0 },
        MMM::Logic::NoteComponent::SubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 1.50, .trackIndex = 1 },
        MMM::Logic::NoteComponent::SubNote{ .type       = MMM::NoteType::FLICK,
                                            .timestamp  = 1.75,
                                            .trackIndex = 2,
                                            .dtrack     = 1 },
    };
    // 根实体本身被选中，GrabTool 会从它的内嵌子物件计算整组轨道范围。
    // InteractionComponent 的选择态让该折线与焦点 Tap 一起进入初始快照。
    const auto entity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(entity, polyline);
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        entity, MMM::Logic::InteractionComponent{ .isSelected = true });
    // Transform 断言用于覆盖独立宽度区域间的即时坐标接缝。
    context.noteRegistry.emplace<MMM::Logic::TransformComponent>(entity);
    return entity;
}

/// @brief 创建作为鼠标拖动焦点的选中单键。
/// @param context 当前会话。
/// @return 玩家区最右轨上的单键实体。
entt::entity createSelectedAnchorTap(MMM::Logic::SessionContext& context)
{
    // Tap 放在玩家轨 3，鼠标移到轨 2 时请求的公共增量应恰好为 -1。
    // 该实体既是选中组成员，也是 CmdStartDrag 指定的主锚点。
    // 不添加持续时间或横向宽度，避免焦点自身引入额外边界。
    const auto entity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        entity,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::NOTE,
            .m_timestamp  = 1.0,
            .m_trackIndex = 3,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        entity, MMM::Logic::InteractionComponent{ .isSelected = true });
    // Transform 断言用于覆盖独立宽度区域间的即时坐标接缝。
    context.noteRegistry.emplace<MMM::Logic::TransformComponent>(entity);
    return entity;
}

/// @brief 创建头部在右侧、其余节点向左展开的单个宽折线。
/// @param context 当前会话。
/// @return 未预选中的折线根实体，用于触发单物件拖动分支。
entt::entity createStandaloneBoundaryPolyline(
    MMM::Logic::SessionContext& context)
{
    // 根节点位于玩家轨 2，向左的节点与 Flick 终点覆盖到玩家轨 0。
    // 将根节点拖到玩家轨 0 时，左侧端点必须先临时进入草稿区。
    MMM::Logic::NoteComponent polyline{
        .m_type       = MMM::NoteType::POLYLINE,
        .m_timestamp  = 1.0,
        .m_trackIndex = 2,
    };
    // 首节点与根位置一致，确保鼠标锚点使用折线头而非内部节点。
    // 末段向左 Flick 再扩一轨，用于触发旧玩家域的负端点钳制。
    polyline.m_subNotes = {
        MMM::Logic::NoteComponent::SubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 1.0, .trackIndex = 2 },
        MMM::Logic::NoteComponent::SubNote{
            .type = MMM::NoteType::NOTE, .timestamp = 1.25, .trackIndex = 1 },
        MMM::Logic::NoteComponent::SubNote{ .type       = MMM::NoteType::FLICK,
                                            .timestamp  = 1.5,
                                            .trackIndex = 1,
                                            .dtrack     = -1 },
    };

    // InteractionComponent 保持未选中，确保 handleStartDrag 进入 Mode B。
    const auto entity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(entity, polyline);
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    context.noteRegistry.emplace<MMM::Logic::TransformComponent>(entity);
    return entity;
}

/// @brief 检查单个宽折线是否完整应用指定统一轨道增量。
/// @param context 当前会话。
/// @param entity 折线根实体。
/// @param deltaTrack 相对初态的预期轨道增量。
/// @return 根、节点、Flick 终点与草稿标记均一致时返回 true。
/// @note 检查预览状态而非持久化合法性，跨域节点暂存于同一折线是允许的。
bool standalonePreviewMatchesDelta(const MMM::Logic::SessionContext& context,
                                   entt::entity entity, std::int32_t deltaTrack)
{
    const auto& polyline =
        context.noteRegistry.get<const MMM::Logic::NoteComponent>(entity);
    // 三个子节点的初始轨道为 2、1、1；dtrack 始终保持 -1。
    // 根轨控制 Draft 标记，子节点则允许在预览阶段分处两个区域。
    // 每帧都从初态应用统一增量，防止累计拖动误差掩盖钳制问题。
    if ( polyline.m_trackIndex != 2 + deltaTrack ||
         polyline.m_isDraft != (polyline.m_trackIndex < 0) ||
         polyline.m_subNotes.size() != 3U ||
         polyline.m_subNotes[0].trackIndex != 2 + deltaTrack ||
         polyline.m_subNotes[1].trackIndex != 1 + deltaTrack ||
         polyline.m_subNotes[2].trackIndex != 1 + deltaTrack ) {
        return false;
    }
    // 末端断言可防止只移动 Flick 起点而遗漏其横向覆盖范围。
    // 使用带符号的 dtrack 保留向左滑动方向，不能只验证覆盖宽度的绝对值。
    return polyline.m_subNotes[2].trackIndex + polyline.m_subNotes[2].dtrack ==
           deltaTrack;
}

/// @brief 检查多选组是否按单键锚点的统一轨道增量更新。
/// @param context 当前会话。
/// @param tapEntity 单键实体。
/// @param polylineEntity 折线实体。
/// @param deltaTrack 相对初始位置的预期轨道增量。
/// @return 根物件、子节点与 Flick 终点均保持统一增量时返回 true。
bool previewMatchesDelta(const MMM::Logic::SessionContext& context,
                         entt::entity tapEntity, entt::entity polylineEntity,
                         std::int32_t deltaTrack)
{
    const auto& tap =
        context.noteRegistry.get<const MMM::Logic::NoteComponent>(tapEntity);
    const auto& polyline =
        context.noteRegistry.get<const MMM::Logic::NoteComponent>(
            polylineEntity);
    // Tap 位置直接代表鼠标锚点，折线根位置代表整组公共增量。
    // m_isDraft 必须随根轨道符号同步，否则虚影会使用错误区域样式。
    // 两个根物件先通过后，再逐一验证内嵌折线节点。
    if ( tap.m_trackIndex != 3 + deltaTrack ||
         tap.m_isDraft != (tap.m_trackIndex < 0) ||
         polyline.m_trackIndex != deltaTrack ||
         polyline.m_isDraft != (deltaTrack < 0) ) {
        return false;
    }

    // 节点和 Flick 的 dtrack 不变，因此只需逐项校验起点轨道增量。
    // 初始子节点轨道恰好等于下标，可直接构造每一项的期望值。
    // 最后的独立断言额外覆盖 Flick 终点，而非只覆盖其起点。
    for ( std::size_t index = 0; index < polyline.m_subNotes.size(); ++index ) {
        if ( polyline.m_subNotes[index].trackIndex !=
             static_cast<std::int32_t>(index) + deltaTrack ) {
            return false;
        }
    }
    return polyline.m_subNotes.back().trackIndex +
               polyline.m_subNotes.back().dtrack ==
           3 + deltaTrack;
}

/// @brief 验证未预选中的单个宽折线不会被玩家域钳制。
/// @return 玩家区内跨边界预览和完整草稿区提交都连续时返回 true。
/// @note 与多选用例分别初始化，不能依赖前一个用例建立的选中状态。
bool testStandalonePolylineDoesNotBlockDraftPreview(bool cancel = false)
{
    // 独立 Session 隔离多选用例，保证实体从未预选状态进入 Mode B。
    MMM::Logic::SessionContext context;
    configureDragContext(context);
    const auto polylineEntity = createStandaloneBoundaryPolyline(context);

    // 模拟直接按住未选中折线的头部，生产路径会进入单物件 Mode B。
    context.hoveredEntity     = polylineEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::PolylineNode);
    context.hoveredSubIndex = 0;
    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             polylineEntity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::PlayerNote,
                         });

    // 玩家轨 0 的中心为 150；根节点从轨 2 左移两轨后，末端临时落到草稿轨 -2。
    tool.handleUpdateDrag(
        context,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 150.0F, 300.0F, true });
    if ( !standalonePreviewMatchesDelta(context, polylineEntity, -2) ||
         !transformXNear(context, polylineEntity, 100.0F) ) {
        XERROR("Standalone Polyline preview was clamped inside player lanes");
        return false;
    }

    // 继续移入最右草稿轨后，整条折线均位于草稿域，可以正常提交。
    // 根轨从 2 到 -1，总增量是 -3，而不是在上一帧的 -2 上再次叠加。
    tool.handleUpdateDrag(
        context,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 30.0F, 300.0F, true });
    if ( !standalonePreviewMatchesDelta(context, polylineEntity, -3) ||
         !transformXNear(context, polylineEntity, 10.0F) ) {
        XERROR("Standalone Polyline preview did not enter draft lanes");
        return false;
    }
    // 完整进入 Draft 后再松开，避免把临时跨域状态误当作可持久化谱面。
    tool.handleEndDrag(context,
                       MMM::Logic::CmdEndDrag{ "Basic2DCanvas", cancel });
    if ( cancel ) {
        // 整条折线的内嵌节点必须同时恢复，取消不能消耗任何撤销记录。
        return standalonePreviewMatchesDelta(context, polylineEntity, 0) &&
               context.actionStack.getUndoStackSize() == 0U &&
               !context.isDragging && context.dragRenderPinnedEntities.empty();
    }

    // 单物件提交仍只生成一条撤销记录，并清理全部即时渲染固定项。
    // 不在提交后重跑预览更新，让断言能发现结束命令将位置回滚的错误。
    return standalonePreviewMatchesDelta(context, polylineEntity, -3) &&
           context.actionStack.getUndoStackSize() == 1U &&
           !context.isDragging && context.dragRenderPinnedEntities.empty();
}

/// @brief 验证宽折线不会阻塞以单键为焦点的跨区拖动虚影。
/// @return 玩家区、草稿区及返回玩家区的每次更新均连续时返回 true。
/// @note 覆盖同一手势内的往返切换，不能用多次独立拖动替代这条序列。
bool testWidePolylineDoesNotBlockDraftPreview(bool cancel = false)
{
    // 每次测试使用全新 SessionContext，避免动作栈或选择状态跨用例泄漏。
    // 先创建宽折线，再创建 Tap，使主锚点身份完全由命令而非实体顺序决定。
    MMM::Logic::SessionContext context;
    configureDragContext(context);
    const auto polylineEntity = createSelectedWidePolyline(context);
    const auto tapEntity      = createSelectedAnchorTap(context);

    // 悬浮信息模拟用户按住组内 Tap 头部，而不是按住宽折线本身。
    // PlayerNote 类型确保旧实现会先尝试玩家区专用求解器。
    context.hoveredEntity     = tapEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::Head);
    context.hoveredSubIndex = -1;
    // StartDrag 会冻结所有选中物件初态，后续每帧都从同一初态求增量。
    // Ctrl 未按下，保证测试走普通整组拖动分支。
    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             tapEntity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::PlayerNote,
                         });

    // 350 对应玩家轨 2；旧逻辑会因折线触及轨 0 而把该次左移钳制为零。
    tool.handleUpdateDrag(
        context,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 350.0F, 300.0F, true });
    // 分域状态下组件轨号与两个根实体的缓存 Transform 必须同时正确。
    // 此时 Tap 仍在玩家区、折线根已进草稿区，两者不能共用同一轨宽。
    if ( !previewMatchesDelta(context, tapEntity, polylineEntity, -1) ||
         !transformXNear(context, tapEntity, 300.0F) ||
         !transformXNear(context, polylineEntity, 10.0F) ) {
        XERROR(
            "Wide Polyline blocked or misprojected the split-domain preview");
        return false;
    }

    // 30 对应独立布局最靠近玩家区的草稿轨 -1；整组此时完整进入草稿区。
    // 以 Tap 为锚点得到 -4 的公共增量，折线根应随之落到最左草稿轨。
    tool.handleUpdateDrag(
        context,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 30.0F, 300.0F, true });
    if ( !previewMatchesDelta(context, tapEntity, polylineEntity, -4) ) {
        XERROR("Grouped preview did not follow the Tap into draft lanes");
        return false;
    }

    // 返回玩家轨后必须恢复相同增量，不能在求解器切换点发生跳变。
    // 重用第一次的鼠标坐标，直接比较同一初态映射，排除运动方向依赖。
    tool.handleUpdateDrag(
        context,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 350.0F, 300.0F, true });
    if ( !previewMatchesDelta(context, tapEntity, polylineEntity, -1) ) {
        XERROR("Grouped preview jumped after returning to player lanes");
        return false;
    }

    // 再次进入有效草稿位置并松开，整组应提交为一个撤销动作。
    // 中间预览不应各自入栈，只有结束手势才形成可撤销的编辑边界。
    tool.handleUpdateDrag(
        context,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 30.0F, 300.0F, true });
    tool.handleEndDrag(context,
                       MMM::Logic::CmdEndDrag{ "Basic2DCanvas", cancel });
    if ( cancel ) {
        // 混合宽折线及单键组跨域预览后回滚，不能只恢复主锚点。
        return previewMatchesDelta(context, tapEntity, polylineEntity, 0) &&
               context.actionStack.getUndoStackSize() == 0U &&
               !context.isDragging && context.dragRenderPinnedEntities.empty();
    }
    // 提交后组件仍保留最终草稿坐标，并且拖动生命周期状态必须完全清理。
    // 单次整组移动只允许生成一条撤销记录，保持操作原子性。
    if ( !previewMatchesDelta(context, tapEntity, polylineEntity, -4) ||
         !transformXNear(context, tapEntity, 10.0F) ||
         !transformXNear(context, polylineEntity, -110.0F) ||
         context.actionStack.getUndoStackSize() != 1U || context.isDragging ||
         !context.dragRenderPinnedEntities.empty() ) {
        XERROR("Grouped draft drag did not commit atomically");
        return false;
    }
    // 提交不能顺带清除原有选择，否则后续连续拖动会退化为单物件操作。
    return context.noteRegistry.get<MMM::Logic::InteractionComponent>(tapEntity)
               .isSelected &&
           context.noteRegistry
               .get<MMM::Logic::InteractionComponent>(polylineEntity)
               .isSelected;
}
/// @brief 替换框选取消后恢复旧框和精确选择，不能把空替换结果当作基线。
/// @details 使用真实 MarqueeTool，取消行为不能仅由 UI 命令数量证明。
/// 原框包含非零时间区间，恢复空框或新框都不能通过。
/// 初始实体同时存在选择组件与索引，二者必须保持同步。
/// 显式 Replace 起框会先移除旧结果，形成确实需要回滚的状态。
/// 新框更新后仍不做正式释放，模拟第二根手指中途加入。
/// 测试检查旧运算基线，防止下一次追加选择回放错误集合。
/// 检查点只应保存一次，更新帧不能用已清空的选择覆盖它。
/// 取消不能产生空撤销动作，也不能消耗之前的历史。
/// 固定相机与滚动缓存复用拖动夹具，不读取图形或音频设备。
/// 新框的屏幕坐标故意不同于旧框，防止保留新框被误判成功。
/// 取消后 isSelecting 为 false，旧框仅是完成状态的可视结果。
/// 选择标记不是按框几何重新推导，而是精确恢复原索引。
/// 同一个工具以后仍可起框，不把检查点当作永久选择缓存。
/// 本用例覆盖 Replace，追加/减选仍共享相同保存和恢复路径。
/// 组件恢复通过生产选择入口执行，不直接写测试专属状态。
/// 容器交换后检查点立即失效，重复取消不能让选择再次反转。
/// 测试的实体只活在内存会话，不向测试资源目录写任何文件。
/// 拖动回归另行验证多实体及完整折线节点，不能用框选替代位置恢复。
bool testMarqueeCancellation()
{
    // 一次完整的开始、更新及取消均使用正式工具接口，保持真实状态转换。
    using namespace MMM::Logic;
    SessionContext context;
    configureDragContext(context);
    const auto entity = createSelectedAnchorTap(context);
    setChartObjectSelected(context, ChartObjectKind::PlayerNote, entity, true);
    context.hasMarqueeSelection = context.marqueeUsesOperations = true;
    context.marqueeBaselineNotes.insert(entity);
    MarqueeBox old;
    old.startTime = 0.25;
    old.endTime   = 0.75;
    old.cameraId  = "Basic2DCanvas";
    context.marqueeBoxes.push_back(old);
    MarqueeTool tool;
    // 新框尚未发布之前就复制检查点，Replace 会移除原选择及框序列。
    tool.handleStartMarquee(context,
                            CmdStartMarquee{ "Basic2DCanvas",
                                             150,
                                             300,
                                             false,
                                             SelectionOperation::Replace,
                                             true });
    const bool cleared = context.selectedNoteEntities.empty();
    tool.handleUpdateMarquee(context, CmdUpdateMarquee{ 350, 400 });
    tool.handleEndMarquee(context, CmdEndMarquee{ true });
    // 验证索引、组件、旧框以及运算基线，而不只验证选择数量。
    return cleared && context.selectedNoteEntities.contains(entity) &&
           context.noteRegistry.get<InteractionComponent>(entity).isSelected &&
           context.marqueeBoxes.size() == 1 &&
           context.marqueeBoxes.front().startTime == 0.25 &&
           context.marqueeBaselineNotes.contains(entity) &&
           context.hasMarqueeSelection && context.marqueeUsesOperations &&
           !context.isSelecting && context.actionStack.getUndoStackSize() == 0;
}
}  // namespace

/// @brief 运行单物件与多选组跨入草稿区的即时虚影回归测试。
/// @brief 长条首把手保留原尾时刻，连续更新以起笔快照为准。
/// @details 玩家与草稿分别执行，取消不产生历史，提交一次可完整撤销重做。
/// @param draft 使用真实负轨号检查局部编辑不会误入统一跨域平移。
/// @param tail 使用同一边界序列检查旧尾部拉伸，不允许跨域路径覆盖局部语义。
/// @param cancel 正常提交与中断恢复分别使用独立会话，不互相借用历史。
/// @return 时间、持续时长、轨道及历史边界全部保持契约时返回 true。
/// @note 首把手只调整时间，指针横移不应修改所属轨道。
/// @note 同时存在的已选哨兵检出错误收集整组或应用共同时间增量。
/// @note 重做校验使用最后有效目标，不只检查撤销栈的条数。
bool testHoldStart(bool draft, bool cancel, bool tail = false)
{
    MMM::Logic::SessionContext ctx;
    configureDragContext(ctx);
    // 场景复用生产相机与滚动缓存，只省略音频、文件解析及 GPU。
    // 原谱面对象保留在会话中，编辑权限检查与正式应用一致。
    using Note        = MMM::Logic::NoteComponent;
    const auto entity = ctx.noteRegistry.create();
    // 起点一秒、尾部三秒；同时选中的哨兵不应加入局部拖动。
    ctx.noteRegistry.emplace<Note>(entity,
                                   Note{ .m_type       = MMM::NoteType::HOLD,
                                         .m_timestamp  = 1.0,
                                         .m_duration   = 2.0,
                                         .m_trackIndex = draft ? -1 : 1,
                                         .m_isDraft    = draft });
    ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        entity, MMM::Logic::InteractionComponent{ .isSelected = true });
    // 哨兵不参与局部快照，取消也不能顺带更改其选中状态或时间。
    // 若只检查主物件，整组错误位移仍可能恰好通过端点断言。
    const auto sentinel = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<Note>(sentinel, Note{ .m_timestamp = 4.0 });
    ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        sentinel, MMM::Logic::InteractionComponent{ .isSelected = true });
    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(
        ctx,
        { entity,
          "Basic2DCanvas",
          true,
          draft ? MMM::Logic::ChartObjectKind::DraftNote
                : MMM::Logic::ChartObjectKind::PlayerNote,
          static_cast<uint8_t>(tail ? MMM::Logic::HoverPart::HoldEnd
                                    : MMM::Logic::HoverPart::HoldStart),
          -1 });
    const auto& cache =
        ctx.timelineRegistry.ctx().get<MMM::Logic::System::ScrollCache>();
    // 用实际滚动缓存投影目标时间，避免测试依赖固定像素速度。
    // 先前拉再后拉覆盖方向反转，持续时间不能按上一帧累计。
    // 越过尾部与负时间均是暂态预览，不在这些中间点结束手势。
    // 最后一帧回到合法范围，检查零长度预览仍可继续恢复。
    // 绕过吸附使边界断言只对应端点契约，吸附沿用公共目标求解器。
    for ( double target : { 0.5, 2.0, 4.0, -1.0, 1.5 } ) {
        const float y = static_cast<float>(300 + cache.getAbsY(1.0) -
                                           cache.getAbsY(target));
        tool.handleUpdateDrag(
            ctx, { "Basic2DCanvas", draft ? 30.0F : 250.0F, y, true });
        const auto&  note     = ctx.noteRegistry.get<Note>(entity);
        const double expected = tail ? 1.0 : std::clamp(target, 0.0, 3.0);
        // 尾把手保持原首点，首把手保持原尾点；两者边界并不对称。
        // 小于零的尾目标先钳到起点，不能产生负持续时间。
        const double expectedEnd = tail ? std::max(1.0, target) : 3.0;
        if ( std::abs(note.m_timestamp - expected) > 1e-5 ||
             std::abs(note.m_timestamp + note.m_duration - expectedEnd) >
                 1e-5 ||
             note.m_trackIndex != (draft ? -1 : 1) ||
             ctx.noteRegistry.get<Note>(sentinel).m_timestamp != 4.0 ) {
            XERROR(
                "Hold start preview draft={} cancel={} target={} actual={} "
                "duration={} track={}",
                draft,
                cancel,
                target,
                note.m_timestamp,
                note.m_duration,
                note.m_trackIndex);
            return false;
        }
    }
    // 仅在最后一个合法样本结束，之前越界样本均不应提交历史。
    // 两端都保留原对象类型，零时长仅为可继续编辑的中间状态。
    // 结束命令不携带时间，因此必须采用最后一次 Update 的结果。
    tool.handleEndDrag(ctx, { "Basic2DCanvas", cancel });
    // 取消直接恢复，不得借一次 Undo 消耗上一笔操作。
    if ( cancel )
        return ctx.actionStack.getUndoStackSize() == 0 &&
               ctx.noteRegistry.get<Note>(entity).m_timestamp == 1.0;
    // 连续五次预览只能合成一个操作，不能随方向反转拆分事务。
    // 这同时验证局部编辑没有把哨兵写入额外的历史条目。
    if ( ctx.actionStack.getUndoStackSize() != 1 ) return false;
    // 一个 Undo 必须同时恢复起点与时长，二者分成两条历史会在这里失败。
    // 后续 Redo 不重复发拖动命令，因此不能靠重新计算目标自证。
    ctx.actionStack.undo(ctx);
    if ( ctx.noteRegistry.get<Note>(entity).m_timestamp != 1.0 ||
         ctx.noteRegistry.get<Note>(entity).m_duration != 2.0 )
        return false;
    ctx.actionStack.redo(ctx);
    // 回放结果和手势结束前的最后一次即时预览保持一致。
    return std::abs(ctx.noteRegistry.get<Note>(entity).m_timestamp -
                    (tail ? 1.0 : 1.5)) < 1e-5 &&
           std::abs(ctx.noteRegistry.get<Note>(entity).m_duration -
                    (tail ? 0.5 : 1.5)) < 1e-5;
}
/// @return 测试通过时返回 0。
/// @note 两个分支分别执行，再汇总退出码，避免前一失败令后一分支被短路跳过。
int main()
{
    // 测试只构造内存会话并派发工具命令，不需要图形窗口或外部谱面资源。
    const bool standalonePassed =
        testStandalonePolylineDoesNotBlockDraftPreview();
    const bool groupedPassed = testWidePolylineDoesNotBlockDraftPreview();
    const bool standaloneCancelled =
        testStandalonePolylineDoesNotBlockDraftPreview(true);
    const bool groupedCancelled =
        testWidePolylineDoesNotBlockDraftPreview(true);
    const bool marqueeCancelled = testMarqueeCancellation();
    // 分别报告被破坏的事务边界，避免只看到聚合测试退出码。
    // 折线单体失败指向内嵌节点恢复，组拖动失败指向参与集合恢复。
    // 框选失败需要检查旧框、选择索引及集合运算基线是否一并恢复。
    if ( !standaloneCancelled )
        XERROR("Standalone polyline cancellation failed");
    if ( !groupedCancelled ) XERROR("Grouped drag cancellation failed");
    if ( !marqueeCancelled ) XERROR("Marquee checkpoint cancellation failed");
    // 每种轨道域分别覆盖首尾两端及提交/取消，不依赖其它场景先运行。
    // 取消场景从空历史开始，旧已完成操作的保护由既有取消用例共同覆盖。
    // 同一组负时间与越尾样本对两端给出不同期望，检查钳制方向。
    // 提交场景最后回到合法长度，再回放一次完整的撤销与重做。
    bool endpoints = true;
    for ( bool draft : { false, true } )
        for ( bool cancel : { false, true } ) {
            const bool passed = testHoldStart(draft, cancel);
            if ( !passed )
                XERROR(
                    "Hold endpoint draft={} cancel={} failed", draft, cancel);
            endpoints &= passed;
            // 不短路尾部用例，首端失败时也需报告既有尾端是否受影响。
            // 草稿和玩家使用不同轨道域，不能拿一个域的成功替代另一个。
            const bool tailPassed = testHoldStart(draft, cancel, true);
            if ( !tailPassed )
                XERROR("Hold tail draft={} cancel={} failed", draft, cancel);
            endpoints &= tailPassed;
        }
    if ( !endpoints ) XERROR("Hold start endpoint regression failed");
    return endpoints && standalonePassed && groupedPassed &&
                   standaloneCancelled && groupedCancelled && marqueeCancelled
               ? 0
               : 1;
}
