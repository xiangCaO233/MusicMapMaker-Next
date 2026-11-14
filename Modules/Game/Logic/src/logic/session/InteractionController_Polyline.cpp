#include "common/LogicCommands.h"
#include "common/PolylineGeometry.h"
#include "common/render/HoverRenderData.h"
#include "config/skin/translation/Translation.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/session/InteractionController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/DrawTool.h"

namespace MMM::Logic
{
/// @brief 用稳定子身份断开删除折线节点，不把过期索引作用到相邻节点。
/// @param cmd 父实体与子令牌共同限定要删除的合法连接段。
/// @details 删除复用桌面橡皮擦的断开语义，不自动连接断口。
/// 新增转折由原绘制/续写状态机处理，不能用相邻同类段伪造节点。
/// 所有结构变化通过 BatchNoteAction 提交，撤销同时恢复父列表和子实体。
/// @warning 仅用户确认节点操作时遍历旧子实体并准备批次，不得逐帧调用。
/// 此低频命令不等待其他线程，也不访问文件或通过 Undo 模拟失败恢复。
void InteractionController::handleCommand(const CmdDeletePolylineNode& cmd)
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
    // 结构编辑只处理原生正交路径，不把畸形导入数据当作自由折线编辑。
    if ( !Common::isCanonicalPolyline(
             current->m_subNotes,
             current->m_timestamp,
             current->m_trackIndex,
             current->m_isDraft ? -m_ctx.draftTrackCount : 0,
             current->m_isDraft ? -1 : m_ctx.trackCount - 1) )
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
    m_ctx.hoveredEntity = registry.valid(oldEntity) ? oldEntity : entt::null;
    m_ctx.hoveredObjectKind = oldKind;
    m_ctx.hoveredPart       = oldPart;
    m_ctx.hoveredSubIndex   = oldIndex;
    return;
}
}  // namespace MMM::Logic
