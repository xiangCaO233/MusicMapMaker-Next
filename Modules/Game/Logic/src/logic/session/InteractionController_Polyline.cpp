#include "common/LogicCommands.h"
#include "common/render/HoverRenderData.h"
#include "config/skin/translation/Translation.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/session/InteractionController.h"
#include "logic/session/NoteAction.h"
#include "logic/session/NoteIdentity.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/DrawTool.h"
#include <cstddef>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace MMM::Logic
{
/// @brief 用稳定子身份编辑折线，菜单不能把过期索引作用到相邻节点。
/// @param cmd 父实体与子令牌共同限定目标，插入和删除都是一次离散操作。
/// @details 删除复用桌面橡皮擦的断开语义，不自动连接断口。
/// 插入仅拆分已存在且合法的连接，保留前后端点与既有节点属性。
/// 新节点没有音频绑定、批注或旧身份，避免复制原节点的打击资源。
/// 所有结构变化通过 BatchNoteAction 提交，撤销同时恢复父列表和子实体。
/// @warning 仅用户确认节点操作时遍历旧子实体并准备批次，不得逐帧调用。
/// 此低频命令不等待其他线程，也不访问文件或通过 Undo 模拟失败恢复。
void InteractionController::handleCommand(const CmdEditPolylineNode& cmd)
{
    // 结构编辑不能接管未完成的拖动或画笔；播放权限与普通编辑一致。
    // 命令先过会话门禁，再解析节点，避免弹窗滞留期间越过播放状态。
    // 画笔、对象拖动和橡皮擦各有自己的事务，本入口不能中途接管它们。
    // 专业模式及草稿权限继续由公共可编辑性判断负责。
    if ( m_ctx.isPlaying || m_ctx.isDragging || m_ctx.brushState.isActive ||
         m_ctx.eraserState.isActive ||
         !m_ctx.lastConfig.settings.enablePolylineEditing ||
         (cmd.kind != ChartObjectKind::PlayerNote &&
          cmd.kind != ChartObjectKind::DraftNote) )
        return;
    auto& registry = m_ctx.noteRegistry;
    if ( !m_ctx.currentBeatmap || !registry.valid(cmd.entity) ) return;
    const auto* current = registry.try_get<const NoteComponent>(cmd.entity);
    if ( !current || current->m_type != ::MMM::NoteType::POLYLINE ||
         current->m_isSubNote ||
         current->m_isDraft != (cmd.kind == ChartObjectKind::DraftNote) ||
         !SessionUtils::isNoteEditable(*current, m_ctx.lastConfig.settings) )
        return;
    // 节点可被联机编辑或其它窗口替换；同一实体与相同下标不足以证明身份。
    // 失效命令不分配实体、不清重做栈，不留下半次历史事务。
    // 索引只负责定位候选，令牌确认其仍是用户看到的原节点。
    // 失效后要求重新选择，不自动搜索一个看起来接近的新位置。
    // 这是离散菜单命令；连续位移的锁存校验在 GrabTool 起笔处处理。
    // 令牌仅在同一进程比较，持久化仍使用原完整协作 ID。
    const int index = cmd.subIndex;
    if ( index < 0 || index >= static_cast<int>(current->m_subNotes.size()) ||
         cmd.nodeToken == 0 ||
         Common::Render::polylineTouchToken(
             current->m_subNotes[index].collaborationId) != cmd.nodeToken ) {
        m_ctx.lastActionMessage = TR("ui.mobile.node.stale").data();
        return;
    }
    if ( !cmd.insertAfter ) {
        // 首节点在旧橡皮擦中代表整条删除，节点菜单不伪装成局部操作。
        // 整条删除仍通过现有删除选中物件入口提供。
        if ( index == 0 ) return;
        // 借用的悬浮状态只用于调用既有局部分裂算法，调用后立即恢复。
        // 保存的是标量身份，不持有动作执行期间可能失效的组件引用。
        // 不临时改动选择集合，防止撤销边界外出现未记录的选区变化。
        const auto oldEntity    = m_ctx.hoveredEntity;
        const auto oldKind      = m_ctx.hoveredObjectKind;
        const auto oldPart      = m_ctx.hoveredPart;
        const auto oldIndex     = m_ctx.hoveredSubIndex;
        m_ctx.hoveredEntity     = cmd.entity;
        m_ctx.hoveredObjectKind = cmd.kind;
        m_ctx.hoveredPart =
            static_cast<int>(Common::Render::HoverPart::PolylineNode);
        m_ctx.hoveredSubIndex = index;
        // 工具对象不拥有笔画，真正的目标与预览状态由当前会话保存。
        // 此入口前面已排除活动擦除，所以不会覆盖另一笔预览。
        // 起笔与收尾在同一命令内完成，没有等待释放或迟到更新。
        DrawTool eraser;
        eraser.handleStartErase(m_ctx, {});
        // 限定显式节点，不能因为根已选中就把其余选区一起删掉。
        // 未选中路径原本就具备精确的分裂、降级与子实体历史处理。
        m_ctx.eraserState.limitToTarget = true;
        // 断口两侧的段类型、绑定与颜色继承全部由原橡皮擦实现决定。
        // 局部目标限制仅影响选区扩展，不改变任何分裂及降级规则。
        // 最终动作仍是一个 BatchNoteAction，联机和保存看到同一个结果。
        eraser.handleEndErase(m_ctx, {});
        m_ctx.eraserState.limitToTarget = false;
        // 被删根可能已经失效；不把旧悬浮句柄写回新注册表状态。
        m_ctx.hoveredEntity =
            registry.valid(oldEntity) ? oldEntity : entt::null;
        m_ctx.hoveredObjectKind = oldKind;
        m_ctx.hoveredPart       = oldPart;
        m_ctx.hoveredSubIndex   = oldIndex;
        return;
    }
    // 末尾续写已有绘制入口，插入只处理两个既有节点之间的连接。
    // 合法性与渲染能力标志共用同一判断，不能按任意猜测的中点修复异常谱面。
    if ( index + 1 >= static_cast<int>(current->m_subNotes.size()) ||
         !Common::Render::canSplitPolylineConnection(
             current->m_subNotes[index], current->m_subNotes[index + 1]) )
        return;
    // 全部校验在新实体分配之前完成，失败路径不能留下占位节点。
    // 完整 before 保存原属性，after 只用于这次低频确认。
    // 不把节点列表复制挂到每帧渲染或普通悬浮路径。
    const NoteComponent    before = *current;
    auto                   after  = before;
    auto&                  left   = after.m_subNotes[index];
    const auto             right  = after.m_subNotes[index + 1];
    NoteComponent::SubNote inserted;
    inserted.type = left.type;
    // 新节点延续连接段颜色，但不复制原节点音频绑定和批注。
    // 新增 ID 统一由身份助手补齐，左右原节点仍使用旧 ID。
    inserted.customColors = left.customColors;
    if ( left.type == ::MMM::NoteType::FLICK ) {
        // 同时间的横向连接只能按整数轨道拆分，两个方向使用同一有符号公式。
        inserted.timestamp  = left.timestamp;
        inserted.trackIndex = std::midpoint(left.trackIndex, right.trackIndex);
        inserted.dtrack     = right.trackIndex - inserted.trackIndex;
        left.dtrack         = inserted.trackIndex - left.trackIndex;
    } else {
        // 中点使用差值安全的标准算法，首尾时间和值域均不改变。
        inserted.timestamp = std::midpoint(left.timestamp, right.timestamp);
        inserted.trackIndex =
            left.type == ::MMM::NoteType::HOLD
                ? left.trackIndex
                : std::midpoint(left.trackIndex, right.trackIndex);
        if ( left.type == ::MMM::NoteType::HOLD ) {
            inserted.duration = right.timestamp - inserted.timestamp;
            left.duration     = inserted.timestamp - left.timestamp;
        }
    }
    // 插入后原向量引用可能失效，后面只使用最终 after 容器。
    // 根起点未改，所以父时间与轨道不必重新推导。
    // 后继与尾端不平移，插点不能偷偷改写原尾时刻。
    after.m_subNotes.insert(after.m_subNotes.begin() + index + 1,
                            std::move(inserted));
    // 先给新增节点建立稳定身份，父列表与接下来创建的子实体共享它。
    // 原节点身份、音频绑定和批注完整保留，不随下标平移重新编号。
    ensureNoteCollaborationIdentity(after);
    std::vector<BatchNoteAction::Entry> entries;
    const auto*                         interaction =
        registry.try_get<const InteractionComponent>(cmd.entity);
    const bool selected = interaction && interaction->isSelected;
    entries.push_back({ cmd.entity, before, after, selected, selected });
    // 结构变化需要重建父到子索引。旧子实体逐一进入同一批次，不能直接销毁。
    // 这样一次撤销可恢复原子句柄，避免拾取和导出仍读取上一个索引布局。
    // 旧子实体的选择状态属于历史快照，空 optional 不能伪造为选中。
    // 遍历只发生在明确结构确认边沿，不加入连续拖动预览。
    // 条目全部准备完才执行，渲染不能观察到半个结构。
    for ( const auto child : registry.view<NoteComponent>() ) {
        const auto& note = registry.get<const NoteComponent>(child);
        if ( !note.m_isSubNote || note.m_parentPolyline != cmd.entity )
            continue;
        const auto* state = registry.try_get<const InteractionComponent>(child);
        entries.push_back(
            { child,
              note,
              std::nullopt,
              state ? std::optional<bool>{ state->isSelected } : std::nullopt,
              std::nullopt });
    }
    // 子实体使用最终列表下标，不复用删除前的序号。
    // 父列表和子实体共享同一稳定 ID，防止保存或联机把它们视为两点。
    // 撤销移除新子实体并恢复旧句柄，重做复用同一条历史。
    for ( std::size_t i = 0; i < after.m_subNotes.size(); ++i ) {
        auto child = makeNoteComponentFromSubNote(
            after.m_subNotes[i], true, cmd.entity, static_cast<int>(i));
        child.m_isDraft    = after.m_isDraft;
        child.m_annotation = after.m_subNotes[i].annotation;
        entries.push_back({ registry.create(),
                            std::nullopt,
                            std::move(child),
                            std::nullopt,
                            false });
    }
    // 只有有效插入触及历史栈；取消菜单和过期目标不走到这里。
    // 父更新、子删除和子创建必须合为一笔 Undo。
    m_ctx.actionStack.pushAndExecute(
        std::make_unique<BatchNoteAction>(std::move(entries), "插入折线节点"),
        m_ctx);
}
}  // namespace MMM::Logic
