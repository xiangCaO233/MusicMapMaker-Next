#include "common/LogicCommandMutationClassification.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/InteractionController.h"
#include "logic/session/NoteIdentity.h"
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
    // Cancel 从正在预览的反向值回滚，不是原地轻点的空事务。
    // 正常分支则将两次更新合并，证明中途跨越根轨没有提前提交。
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
/// @brief 创建包含斜向、长条和双轨滑键连接的折线及其子实体。
/// @details 各段稳定 ID 和元数据用于识别插入后是否错误覆盖相邻节点。
/// @param ctx 已配置最小谱面、相机与草稿范围的会话。
/// @param draft 使用负轨号，确保结构编辑不偷偷把草稿变成玩家物件。
entt::entity makeNodeFixture(MMM::Logic::SessionContext& ctx, bool draft)
{
    // 夹具不加载磁盘文件，避免混入解析或解码错误。
    // 仍保留真实谱面、相机和滚动缓存以走完整权限路径。
    // 正负轨号使用同样的局部连接，只改变所属域。
    using namespace MMM;
    Logic::NoteComponent note{ .m_type       = NoteType::POLYLINE,
                               .m_timestamp  = 1.0,
                               .m_trackIndex = draft ? -4 : 0,
                               .m_isDraft    = draft };
    const int            base = draft ? -4 : 0;
    // 四个节点包含斜向、竖向和瞬时横向连接。
    // 首尾几何明确，便于检查后缀是否意外移动。
    // 重复时刻在 Flick 接点合法，不能误当时间倒退。
    // 每个连接由生产规则判定，不在测试里模拟插点求解。
    note.m_subNotes = {
        { .type = NoteType::NOTE, .timestamp = 1, .trackIndex = base },
        { .type       = NoteType::HOLD,
          .timestamp  = 2,
          .duration   = 1,
          .trackIndex = base + 1 },
        { .type       = NoteType::FLICK,
          .timestamp  = 3,
          .trackIndex = base + 1,
          .dtrack     = 2 },
        { .type = NoteType::NOTE, .timestamp = 3, .trackIndex = base + 3 }
    };
    // 不同节点的局部属性不能因下标平移迁移到新增节点。
    note.m_subNotes[1].annotation = "保留原批注";
    note.m_subNotes[1]
        .metadata.note_properties[NoteMetadataType::MMM]["fixture"] = "keep";
    // 由生产助手生成稳定 ID，而非手工把索引当身份。
    // 父与子分别编号，才能发现误把一个标识复制给所有节点。
    // 新增点必须保留空白来源，原点身份则不能变化。
    Logic::ensureNoteCollaborationIdentity(note);
    const auto root = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<Logic::NoteComponent>(root, note);
    ctx.noteRegistry.emplace<Logic::InteractionComponent>(
        root, Logic::InteractionComponent{ .isSelected = true });
    // 独立子实体与父内嵌列表同时构造，避免只测模型容器。
    // 撤销以后旧句柄仍须恢复，不能只恢复父列表长度。
    // 子域跟随父域，不因结构操作偷偷转成正式物件。
    for ( int i = 0; i < 4; ++i ) {
        auto child = Logic::makeNoteComponentFromSubNote(
            note.m_subNotes[i], true, root, i);
        child.m_isDraft   = draft;
        const auto entity = ctx.noteRegistry.create();
        ctx.noteRegistry.emplace<Logic::NoteComponent>(entity,
                                                       std::move(child));
        ctx.noteRegistry.emplace<Logic::InteractionComponent>(entity);
    }
    return root;
}
/// @brief 三类连接分别插入，撤销重做必须同时恢复根与子实体索引。
/// @param draft 玩家/草稿独立构造，不让一个域的成功掩盖另一个。
/// @param index 指定被拆分的 NOTE、HOLD 或 FLICK 段。
/// @return 全部几何、身份和一次历史记录保持契约时返回真。
bool testNodeInsert(bool draft, int index)
{
    // 每个连接用独立会话，不继承前一用例的历史或身份。
    // 三种段类型分别测试，不能只用普通斜线代替。
    // 调用真实控制器与动作栈，不手工生成预期 after。
    using namespace MMM;
    Logic::SessionContext ctx;
    configureDragContext(ctx);
    const auto root   = makeNodeFixture(ctx, draft);
    const auto before = ctx.noteRegistry.get<Logic::NoteComponent>(root);
    Logic::InteractionController     controller(ctx);
    const Logic::CmdEditPolylineNode cmd{
        root,
        draft ? Logic::ChartObjectKind::DraftNote
              : Logic::ChartObjectKind::PlayerNote,
        index,
        Common::Render::polylineTouchToken(
            before.m_subNotes[index].collaborationId),
        true
    };
    // 命令必须进入物件权限分类，不能绕过联机只读谱面的限制。
    if ( Logic::requiredBeatmapMutationFlags(Logic::LogicCommand{ cmd }) !=
         BeatmapMutationFlags::Objects )
        return false;
    // 载荷明确父领域、索引与稳定令牌，不借用未来 hover。
    // 权限分类须与实际物件变更一致，联机只读门闩才能正确工作。
    // 执行以后继续验证注册表与历史，不能只检查命令返回。
    controller.handleCommand(cmd);
    const auto& after = ctx.noteRegistry.get<Logic::NoteComponent>(root);
    // 有效插入只增一个点和一条历史。
    // 离散操作不能建立空拖动事务。
    // 数量相同仍不足以证明目标正确，后续继续核对身份与几何。
    if ( after.m_subNotes.size() != 5 ||
         ctx.actionStack.getUndoStackSize() != 1 )
        return false;
    // 新增身份不复用左右节点，原节点顺序和后继终点保持不变。
    // 对照插入前后相同的后继节点，而不是比较当前相邻两个索引。
    // 这样能区分正确下标迁移与错误地给整条路径重新分配身份。
    if ( after.m_subNotes[index + 1].collaborationId.empty() ||
         after.m_subNotes[index + 1].collaborationId ==
             before.m_subNotes[index].collaborationId ||
         after.m_subNotes[index + 2].collaborationId !=
             before.m_subNotes[index + 1].collaborationId )
        return false;
    // 长条两半时长之和保持原值，左右端点均不能平移。
    // 批注仍附在原节点，不应转移到新增点。
    // 横段两半轨差之和保持原跨度，时间必须保持瞬时。
    if ( index == 1 && (after.m_subNotes[1].duration != 0.5 ||
                        after.m_subNotes[2].duration != 0.5 ||
                        after.m_subNotes[1].annotation != "保留原批注") )
        return false;
    if ( index == 2 &&
         (after.m_subNotes[2].dtrack != 1 || after.m_subNotes[3].dtrack != 1) )
        return false;
    // 子实体数量及索引必须与父列表一致，否则画面和下一次拾取会过期。
    // 逐项检查子实体与父列表的最终索引。
    // 只数实体会漏掉旧下标残留导致的下一次误拾取。
    // 草稿版本不能丢失负轨域标志。
    int children{};
    for ( const auto entity : ctx.noteRegistry.view<Logic::NoteComponent>() ) {
        const auto& child = ctx.noteRegistry.get<Logic::NoteComponent>(entity);
        if ( child.m_parentPolyline != root || !child.m_isSubNote ) continue;
        ++children;
        if ( child.m_isDraft != draft || child.m_subIndex < 0 ||
             child.m_subIndex >= 5 ||
             child.m_collaborationId !=
                 after.m_subNotes[child.m_subIndex].collaborationId )
            return false;
    }
    // 根实体不参与 children 计数，旧子投影若未删除便会超过五个。
    // 子身份对应父列表同一索引，检查结果可以直接用于下一次拾取。
    if ( children != 5 ) return false;
    // 一次撤销同时恢复父列表与子实体布局。
    // 重做不再次发送插入命令，所以不能靠重新计算伪装回放。
    // 新增节点 ID 不按回放次数重新生成。
    ctx.actionStack.undo(ctx);
    if ( ctx.noteRegistry.get<Logic::NoteComponent>(root).m_subNotes.size() !=
         4 )
        return false;
    ctx.actionStack.redo(ctx);
    if ( ctx.noteRegistry.get<Logic::NoteComponent>(root).m_subNotes.size() !=
         5 )
        return false;
    // 插入后原索引可能已经指向新增点；旧令牌必须拒绝，而不是修改邻点。
    // 故意使用插入前下一点的旧下标。
    // 该处如今是新增点，原稳定令牌必须令命令失败。
    // 失败不改变历史大小，也不清掉已有重做分支。
    if ( index < 2 ) {
        const auto size = ctx.actionStack.getUndoStackSize();
        controller.handleCommand(
            { root,
              cmd.kind,
              index + 1,
              Common::Render::polylineTouchToken(
                  before.m_subNotes[index + 1].collaborationId),
              true });
        if ( ctx.actionStack.getUndoStackSize() != size ) return false;
    }
    return true;
}
/// @brief 节点删除沿用断开语义，已选中的其它物件不能被选区扩展误删。
/// @details 无效首节点与播放中请求不改变历史，正常删除一次即可完整撤销。
bool testNodeDelete()
{
    // 根目标和另一个哨兵都选中，验证不会扩展删除集合。
    // 不靠先清空选择来规避风险。
    // 删除采用真实橡皮擦断开路径，左右部分不自动连接。
    using namespace MMM;
    Logic::SessionContext ctx;
    configureDragContext(ctx);
    const auto root     = makeNodeFixture(ctx, false);
    const auto original = ctx.noteRegistry.get<Logic::NoteComponent>(root);
    const auto sentinel = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<Logic::NoteComponent>(
        sentinel, Logic::NoteComponent{ .m_timestamp = 9 });
    ctx.noteRegistry.emplace<Logic::InteractionComponent>(
        sentinel, Logic::InteractionComponent{ .isSelected = true });
    Logic::InteractionController controller(ctx);
    Logic::CmdEditPolylineNode   command{
        root,
        Logic::ChartObjectKind::PlayerNote,
        0,
        Common::Render::polylineTouchToken(
            original.m_subNotes[0].collaborationId),
        false
    };
    // 首节点代表整条，局部入口拒绝它。
    // 整物件删除由原菜单提供，不能悄悄换成另一操作。
    // 无效请求必须保持空历史。
    controller.handleCommand(command);
    if ( ctx.actionStack.getUndoStackSize() != 0 ) return false;
    // 中间删除既不自动连接左右部分，也不删除另一个已选根物件。
    command.subIndex  = 1;
    command.nodeToken = Common::Render::polylineTouchToken(
        original.m_subNotes[1].collaborationId);
    // 弹窗可能跨过播放状态切换，业务层不能只信 UI 禁用态。
    // 相同载荷先验证门禁，再执行合法删除。
    // 一次拒绝不得污染下一次合法请求。
    ctx.isPlaying = true;
    controller.handleCommand(command);
    ctx.isPlaying = false;
    if ( ctx.actionStack.getUndoStackSize() != 0 ) return false;
    // 合法删除与前面的拒绝使用同一个控制器，避免重建掩盖状态泄漏。
    // 原根消失而哨兵存在，是断开重建与扩大选区删除的区别。
    controller.handleCommand(command);
    if ( ctx.actionStack.getUndoStackSize() != 1 ||
         ctx.noteRegistry.valid(root) || !ctx.noteRegistry.valid(sentinel) )
        return false;
    // 断开可能创建两个新根，撤销仍要恢复原父句柄。
    // 哨兵在执行、撤销、重做后都必须存在。
    // 不从屏幕高亮推断对象是否真实被删除。
    ctx.actionStack.undo(ctx);
    if ( !ctx.noteRegistry.valid(root) ||
         ctx.noteRegistry.get<Logic::NoteComponent>(root).m_subNotes.size() !=
             4 )
        return false;
    ctx.actionStack.redo(ctx);
    return !ctx.noteRegistry.valid(root) && ctx.noteRegistry.valid(sentinel);
}
/// @brief Flick 尾部只改方向/长度，草稿域不能进入整体跨域搬移。
/// @param draft 使用负轨号版本，与玩家版本复用相同长度变化和反向逻辑。
/// @param cancel 第二次反向后取消，应恢复整笔起始几何且不建历史。
bool testFlickEndpoint(bool draft, bool cancel)
{
    // 独立 Flick 不进入折线分支，需要单独验证。
    // 方向翻转必须经过根轨，不能只测同向拉长。
    // 草稿版本检出旧玩家域钳制对负轨的错误限制。
    using namespace MMM;
    Logic::SessionContext ctx;
    configureDragContext(ctx);
    const auto entity = ctx.noteRegistry.create();
    const int  track  = draft ? -3 : 1;
    ctx.noteRegistry.emplace<Logic::NoteComponent>(
        entity,
        Logic::NoteComponent{ .m_type       = NoteType::FLICK,
                              .m_timestamp  = 1,
                              .m_trackIndex = track,
                              .m_dtrack     = 1,
                              .m_isDraft    = draft });
    ctx.noteRegistry.emplace<Logic::InteractionComponent>(
        entity, Logic::InteractionComponent{ .isSelected = true });
    // 真实工具保存初始组件，测试不手工恢复 before。
    // 起笔明确是箭头，不能读取上一帧的悬浮部位。
    // 纵坐标固定，排除时间插值对结果的影响。
    Logic::GrabTool tool;
    tool.handleStartDrag(ctx,
                         { entity,
                           "Basic2DCanvas",
                           true,
                           draft ? Logic::ChartObjectKind::DraftNote
                                 : Logic::ChartObjectKind::PlayerNote,
                           static_cast<uint8_t>(Logic::HoverPart::FlickArrow),
                           -1 });
    // 先拉长两轨，再越过根轨反向；两个过程都保持根轨与时间不变。
    // 玩家与草稿像素的零点不同，期望的相对轨差却相同。
    // 两套坐标显式给出，不能让测试跟随被测投影函数一起算错。
    tool.handleUpdateDrag(
        ctx, { "Basic2DCanvas", draft ? 30.0f : 450.0f, 300, true });
    if ( ctx.noteRegistry.get<Logic::NoteComponent>(entity).m_dtrack != 2 ) {
        XERROR("Flick length actual={} track={} dragging={}",
               ctx.noteRegistry.get<Logic::NoteComponent>(entity).m_dtrack,
               ctx.noteRegistry.get<Logic::NoteComponent>(entity).m_trackIndex,
               ctx.isDragging);
        return false;
    }
    tool.handleUpdateDrag(
        ctx, { "Basic2DCanvas", draft ? -90.0f : 150.0f, 300, true });
    const auto& preview = ctx.noteRegistry.get<Logic::NoteComponent>(entity);
    if ( preview.m_dtrack != -1 || preview.m_timestamp != 1 ||
         preview.m_trackIndex != track )
        return false;
    // 取消与提交各用独立会话，不互相借用历史。
    // 方向翻转不能拆成两笔 Undo。
    // 撤销恢复原长度，重做恢复最终反向值。
    tool.handleEndDrag(ctx, { "Basic2DCanvas", cancel });
    // Cancel 从正在预览的反向值回滚，不是原地轻点的空事务。
    // 正常分支则将两次更新合并，证明中途跨越根轨没有提前提交。
    if ( cancel )
        return ctx.actionStack.getUndoStackSize() == 0 &&
               ctx.noteRegistry.get<Logic::NoteComponent>(entity).m_dtrack == 1;
    if ( ctx.actionStack.getUndoStackSize() != 1 ) return false;
    ctx.actionStack.undo(ctx);
    if ( ctx.noteRegistry.get<Logic::NoteComponent>(entity).m_dtrack != 1 )
        return false;
    ctx.actionStack.redo(ctx);
    return ctx.noteRegistry.get<Logic::NoteComponent>(entity).m_dtrack == -1;
}
/// @brief 起笔前已经被结构编辑替换的节点，不允许借旧下标开始拖动。
/// @details UI 合成回放与逻辑队列不同步，因此必须在真实 GrabTool 验证。
/// 原实体仍有效，专门排除只检查根句柄就通过的错误实现。
/// 有效令牌对照必须能起笔，不能通过禁用所有局部拖动来让拒绝用例通过。
/// 取消后原节点几何、身份及历史同时保持原样。
bool testNodeDragToken()
{
    // 此测试只验证起笔身份与取消，不复写局部拖动的几何算法。
    // 实际几何联动由既有 PolylineFirstBodyDragTest 继续覆盖。
    // 令牌错误时根仍有效，区别于实体已经删除的简单失败情况。
    // 因而只做 registry.valid 判断的实现会被这个用例检出。

    using namespace MMM;
    Logic::SessionContext ctx;
    configureDragContext(ctx);
    const auto      root   = makeNodeFixture(ctx, false);
    const auto      before = ctx.noteRegistry.get<Logic::NoteComponent>(root);
    Logic::GrabTool tool;
    // 错误令牌来自同一父内另一个节点，根句柄与下标都仍合法。
    // 失败应发生在建立拖动事务之前，不得需要事后 Undo。
    Logic::CmdStartDrag start{
        root,
        "Basic2DCanvas",
        true,
        Logic::ChartObjectKind::PlayerNote,
        static_cast<uint8_t>(Logic::HoverPart::PolylineNode),
        1,
        Common::Render::polylineTouchToken(before.m_subNotes[2].collaborationId)
    };
    tool.handleStartDrag(ctx, start);
    if ( ctx.isDragging || ctx.actionStack.getUndoStackSize() != 0 )
        return false;
    // 用正确身份重复完全相同的输入，证明局部编辑仍可正常开始。
    // 此对照也检验错误请求没有残留旧参与集合。
    // 正确令牌取相同父列表中的目标节点，不从当前 Hover 状态取得。
    // 重复起笔前没有重新构造会话，验证失败路径清理完整。
    // 原数组、索引和目标轨道完全相同，只有身份参数改变。
    start.hitNodeToken = Common::Render::polylineTouchToken(
        before.m_subNotes[1].collaborationId);
    tool.handleStartDrag(ctx, start);
    if ( !ctx.isDragging || ctx.draggedSubIndex != 1 ) return false;
    // 先产生可见位移再取消，确保恢复路径真正经过发生变化的组件。
    // 锁定的节点索引与令牌在整笔期间不能由后续 Hover 替换。
    tool.handleUpdateDrag(ctx, { "Basic2DCanvas", 350, 250, true });
    tool.handleEndDrag(ctx, { "Basic2DCanvas", true });
    // Cancel 恢复原快照，不能把有效对照变成一条空编辑历史。
    // 组件引用在 End 之后重新取得，避免读取已被 action 替换的旧存储。
    // 取消应保留原批注和稳定身份，不能用删除重建模拟恢复。
    // 确认没有历史条目，也防止一次无位移拖动清掉用户重做分支。
    const auto& after = ctx.noteRegistry.get<Logic::NoteComponent>(root);
    return !ctx.isDragging && ctx.actionStack.getUndoStackSize() == 0 &&
           after.m_subNotes[1].timestamp == before.m_subNotes[1].timestamp &&
           after.m_subNotes[1].collaborationId ==
               before.m_subNotes[1].collaborationId;
}
/// @brief 普通折线锚点和 Flick 后的显式末节点均能完成真实局部移动。
/// @param draft 玩家与草稿使用各自投影，保持负轨号所属域。
/// @param flickEnd 末节点是前一 Flick 的同时间终点，应同时改变其跨度。
/// @param cancel 取消恢复原节点，正常提交只生成一次可回放历史。
/// @details 旧局部分支只识别 Hold/Flick，NOTE 会命中却不修改数据。
/// 这里直接验证模型变化，不能用星号或手指悬浮代替执行结果。
/// @pre 已载入谱面并开放折线编辑，不能通过权限拒绝伪装测试成功。
/// @note Ctrl 输入只跳过磁吸，不跳过稳定身份或草稿负轨域校验。
/// @return 预览、历史深度与撤销重做后的轨道均符合契约时返回真。
/// @warning 仅构造 CPU 会话，不初始化图形设备、音频线程或磁盘资源。
bool testPlainNodeDrag(bool draft, bool flickEnd, bool cancel)
{
    using namespace MMM;
    Logic::SessionContext ctx;
    configureDragContext(ctx);
    const auto root   = makeNodeFixture(ctx, draft);
    auto&      parent = ctx.noteRegistry.get<Logic::NoteComponent>(root);
    const int  index  = flickEnd ? 3 : 1;
    if ( !flickEnd ) {
        // 将中间段改为普通锚点，前后时刻为一秒和三秒。
        // 夹具同时更新对应子投影，起始父子数据必须一致。
        // 保留原批注和身份，只改变段类型以隔离 NOTE 的缺失处理。
        // 不重建父实体，确保锁定命令仍引用原根。
        parent.m_subNotes[1].type     = NoteType::NOTE;
        parent.m_subNotes[1].duration = 0;
        for ( auto e : ctx.noteRegistry.view<Logic::NoteComponent>() ) {
            auto& child = ctx.noteRegistry.get<Logic::NoteComponent>(e);
            if ( child.m_isSubNote && child.m_parentPolyline == root &&
                 child.m_subIndex == 1 ) {
                child.m_type     = NoteType::NOTE;
                child.m_duration = 0;
            }
        }
    }
    // 判定线上输入直接映射到期望时间，不从被测工具反推坐标。
    // Flick 末端即使收到其它时间也须保持前段的瞬时关系。
    ctx.currentTime = ctx.animateTime = 2.5;
    const auto      before            = parent;
    Logic::GrabTool tool;
    tool.handleStartDrag(ctx,
                         { root,
                           "Basic2DCanvas",
                           true,
                           draft ? Logic::ChartObjectKind::DraftNote
                                 : Logic::ChartObjectKind::PlayerNote,
                           static_cast<uint8_t>(Logic::HoverPart::PolylineNode),
                           index,
                           Common::Render::polylineTouchToken(
                               before.m_subNotes[index].collaborationId) });
    // 两个域的目标均为相对第三轨，几何与根的原始位置无关。
    // 原终点位于相对第四轨，所以该输入真实缩短 Flick 而不是空拖动。
    tool.handleUpdateDrag(
        ctx, { "Basic2DCanvas", draft ? -10.0f : 350.0f, 300, true });
    const auto& preview = ctx.noteRegistry.get<Logic::NoteComponent>(root);
    const int   expectedTrack = draft ? -2 : 2;
    // 同一个输入时间在两类锚点具有不同含义，避免用一套期望掩盖分支遗漏。
    // 普通点接受时间移动，瞬时横段的终点则保持原前段时刻。
    const double expectedTime = flickEnd ? 3.0 : 2.5;
    if ( preview.m_subNotes[index].trackIndex != expectedTrack ||
         preview.m_subNotes[index].timestamp != expectedTime ||
         preview.m_subNotes[index].collaborationId !=
             before.m_subNotes[index].collaborationId )
        return false;
    // 前缀的根和独立批注不跟着点移动，避免误走整条位移。
    // 横段终点的附属 NOTE 必须与前段轨差保持一致。
    if ( preview.m_trackIndex != before.m_trackIndex ||
         preview.m_timestamp != before.m_timestamp ||
         (flickEnd && preview.m_subNotes[2].dtrack != 1) )
        return false;
    tool.handleEndDrag(ctx, { "Basic2DCanvas", cancel });
    // 子实体同步由正式工具完成，测试不直接写入期待的 after 状态。
    // 释放后的回放还会再同步一次，所以继续检查逻辑模型而非截屏。
    // 释放后重新取组件，历史动作可以替换原存储。
    // 取消不建立历史，正常提交不能把横向与纵向变化拆成两笔。
    const auto& finished = ctx.noteRegistry.get<Logic::NoteComponent>(root);
    if ( cancel )
        return ctx.actionStack.getUndoStackSize() == 0 &&
               finished.m_subNotes[index].trackIndex ==
                   before.m_subNotes[index].trackIndex;
    if ( ctx.actionStack.getUndoStackSize() != 1 ) return false;
    ctx.actionStack.undo(ctx);
    // 一次撤销恢复节点及相邻 Flick，不靠额外位置命令回滚。
    // 重做必须沿用同一稳定身份，不再生成一次新的插点动作。
    const auto& undone = ctx.noteRegistry.get<Logic::NoteComponent>(root);
    if ( undone.m_subNotes[index].trackIndex !=
             before.m_subNotes[index].trackIndex ||
         (flickEnd && undone.m_subNotes[2].dtrack != 2) )
        return false;
    ctx.actionStack.redo(ctx);
    return ctx.noteRegistry.get<Logic::NoteComponent>(root)
               .m_subNotes[index]
               .trackIndex == expectedTrack;
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
    // 删除、插入和拖动共享注册表模型，但各自使用独立夹具。
    // 每项都执行真实历史回放，失败日志仅用于区分事务来源。
    bool       nodes       = testNodeDelete();
    const bool tokenPassed = testNodeDragToken();
    if ( !tokenPassed ) XERROR("node drag token failed");
    nodes &= tokenPassed;
    for ( bool draft : { false, true } )
        for ( bool cancel : { false, true } ) {
            const bool result = testFlickEndpoint(draft, cancel);
            if ( !result )
                XERROR("Flick draft={} cancel={} failed", draft, cancel);
            nodes &= result;
        }
    // 普通锚点与横段显式终点分别覆盖提交和取消，避免只测试原有 Hold 分支。
    for ( bool draft : { false, true } )
        for ( bool end : { false, true } )
            for ( bool cancel : { false, true } ) {
                const bool passed = testPlainNodeDrag(draft, end, cancel);
                if ( !passed )
                    XERROR("Plain node draft={} end={} cancel={} failed",
                           draft,
                           end,
                           cancel);
                nodes &= passed;
            }
    // 三个连接在玩家与草稿上各执行一次，不能只测一个正轨样本。
    // 同时保持原有跨域整组选中测试，防止局部修复禁用整体搬移。
    for ( bool draft : { false, true } )
        for ( int i = 0; i < 3; ++i ) {
            const bool result = testNodeInsert(draft, i);
            if ( !result )
                XERROR("insert draft={} segment={} failed", draft, i);
            nodes &= result;
        }
    if ( !nodes ) XERROR("Polyline node structural editing regression failed");
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
    return nodes && endpoints && standalonePassed && groupedPassed &&
                   standaloneCancelled && groupedCancelled && marqueeCancelled
               ? 0
               : 1;
}
