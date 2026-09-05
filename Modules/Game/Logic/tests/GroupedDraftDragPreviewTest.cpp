#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/GrabTool.h"
#include "mmm/beatmap/BeatMap.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace
{
/// @brief 配置四轨玩家区与四轨草稿区，复现多选拖动边界。
/// @param context 待配置会话。
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
bool transformXNear(const MMM::Logic::SessionContext& context,
                    entt::entity entity, float expectedX)
{
    const auto& transform =
        context.noteRegistry.get<const MMM::Logic::TransformComponent>(entity);
    return std::abs(transform.m_pos.x - expectedX) < 1e-4F;
}

/// @brief 创建横跨四条玩家轨道的选中折线。
/// @param context 当前会话。
/// @return 折线根实体。
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
bool testStandalonePolylineDoesNotBlockDraftPreview()
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
    tool.handleUpdateDrag(
        context,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 30.0F, 300.0F, true });
    if ( !standalonePreviewMatchesDelta(context, polylineEntity, -3) ||
         !transformXNear(context, polylineEntity, 10.0F) ) {
        XERROR("Standalone Polyline preview did not enter draft lanes");
        return false;
    }
    // 完整进入 Draft 后再松开，避免把临时跨域状态误当作可持久化谱面。
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    // 单物件提交仍只生成一条撤销记录，并清理全部即时渲染固定项。
    return standalonePreviewMatchesDelta(context, polylineEntity, -3) &&
           context.actionStack.getUndoStackSize() == 1U &&
           !context.isDragging && context.dragRenderPinnedEntities.empty();
}

/// @brief 验证宽折线不会阻塞以单键为焦点的跨区拖动虚影。
/// @return 玩家区、草稿区及返回玩家区的每次更新均连续时返回 true。
bool testWidePolylineDoesNotBlockDraftPreview()
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
    if ( !previewMatchesDelta(context, tapEntity, polylineEntity, -1) ||
         !transformXNear(context, tapEntity, 300.0F) ||
         !transformXNear(context, polylineEntity, 10.0F) ) {
        XERROR(
            "Wide Polyline blocked or misprojected the split-domain preview");
        return false;
    }

    // 30 对应独立布局最靠近玩家区的草稿轨 -1；整组此时完整进入草稿区。
    tool.handleUpdateDrag(
        context,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 30.0F, 300.0F, true });
    if ( !previewMatchesDelta(context, tapEntity, polylineEntity, -4) ) {
        XERROR("Grouped preview did not follow the Tap into draft lanes");
        return false;
    }

    // 返回玩家轨后必须恢复相同增量，不能在求解器切换点发生跳变。
    tool.handleUpdateDrag(
        context,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 350.0F, 300.0F, true });
    if ( !previewMatchesDelta(context, tapEntity, polylineEntity, -1) ) {
        XERROR("Grouped preview jumped after returning to player lanes");
        return false;
    }

    // 再次进入有效草稿位置并松开，整组应提交为一个撤销动作。
    tool.handleUpdateDrag(
        context,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 30.0F, 300.0F, true });
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });
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
    return context.noteRegistry.get<MMM::Logic::InteractionComponent>(tapEntity)
               .isSelected &&
           context.noteRegistry
               .get<MMM::Logic::InteractionComponent>(polylineEntity)
               .isSelected;
}
}  // namespace

/// @brief 运行单物件与多选组跨入草稿区的即时虚影回归测试。
/// @return 测试通过时返回 0。
int main()
{
    const bool standalonePassed =
        testStandalonePolylineDoesNotBlockDraftPreview();
    const bool groupedPassed = testWidePolylineDoesNotBlockDraftPreview();
    return standalonePassed && groupedPassed ? 0 : 1;
}
