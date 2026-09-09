#include "logic/session/EditorAction.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "logic/BeatmapSession.h"
#include "logic/ProjectDraftLaneService.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"

#include <fmt/format.h>

namespace MMM::Logic
{

/// @brief 执行新动作并建立新的撤销历史分支。
/// @param action 转交给历史栈的非空动作，其前置数据应已准备完成。
/// @param ctx 该历史栈所属会话，不应混用其他会话的实体注册表。
/// @warning 编辑提交路径，会同步脏数据并释放旧重做记录，不用于逐帧刷新。
void EditorActionStack::pushAndExecute(std::unique_ptr<IEditorAction> action,
                                       SessionContext&                ctx)
{
    // 名称在动作仍由局部指针持有时读取，状态栏记录此次操作的可读描述。
    ctx.lastActionMessage = fmt::format(
        "{} {}", TR("ui.status.category.action").data(), action->getName());
    action->execute(ctx);
    // 累积类别而非覆盖，允许会话在一次发布前执行多个不同领域的动作。
    m_pendingMutationFlags |= action->mutationFlags();
    m_undoStack.push_back(std::move(action));
    // 新编辑使原来的重做分支失效，旧动作及其保存的快照随之释放。
    m_redoStack.clear();
    if ( ctx.m_needsTimingsSync || ctx.m_needsSamplesSync ) {
        // 动作先改变 ECS；仅当时间点或样本需要回写时同步谱面模型。
        SessionUtils::syncBeatmap(ctx);
    }
    // 草稿属于项目级共享数据，不能仅依赖主谱面的同步标志。
    ProjectDraftLaneService::sync(ctx);
}

/// @brief 撤销最近一次动作，并将同一动作实例移入重做栈。
/// @param ctx 保存动作涉及实体的会话。
/// @note 空历史不改变状态提示，也不触发数据同步。
void EditorActionStack::undo(SessionContext& ctx)
{
    if ( m_undoStack.empty() ) return;
    // 移动所有权保留动作内的前后快照，不为重做复制或重新构造动作。
    auto action = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    ctx.lastActionMessage = fmt::format(
        "{} {}", TR("ui.status.category.undo").data(), action->getName());
    action->undo(ctx);
    // 撤销同样改变数据，必须向后续观察者发布该动作涉及的类别。
    m_pendingMutationFlags |= action->mutationFlags();
    m_redoStack.push_back(std::move(action));
    if ( ctx.m_needsTimingsSync || ctx.m_needsSamplesSync ) {
        SessionUtils::syncBeatmap(ctx);
    }
    ProjectDraftLaneService::sync(ctx);
}

/// @brief 重做最近撤销的动作，并恢复其在撤销历史中的位置。
/// @param ctx 保存动作涉及实体的会话。
/// @note 调用动作自己的 redo，允许它区别于首次 execute 的初始化过程。
void EditorActionStack::redo(SessionContext& ctx)
{
    if ( m_redoStack.empty() ) return;
    auto action = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    ctx.lastActionMessage = fmt::format(
        "{} {}", TR("ui.status.category.redo").data(), action->getName());
    action->redo(ctx);
    // 只移动当前栈顶，不清空其余重做记录，后续动作仍可依次恢复。
    m_pendingMutationFlags |= action->mutationFlags();
    m_undoStack.push_back(std::move(action));
    if ( ctx.m_needsTimingsSync || ctx.m_needsSamplesSync ) {
        SessionUtils::syncBeatmap(ctx);
    }
    ProjectDraftLaneService::sync(ctx);
}

/// @brief 丢弃全部历史并恢复初始保存状态。
/// @note 不调用动作的 undo；清空历史不会反向修改当前谱面内容。
void EditorActionStack::clear()
{
    m_undoStack.clear();
    m_redoStack.clear();
    // 新的历史基线不继承旧会话的保存位置、非撤销修改或待发布类别。
    m_saveIndex             = 0;
    m_hasNonUndoableChanges = false;
    m_pendingMutationFlags  = ::MMM::BeatmapMutationFlags::None;
}

/// @brief 按保存时的历史深度和非撤销修改标记判断未保存状态。
/// @return 历史深度偏离保存点或存在栈外修改时返回 true。
/// @note 这里只比较历史深度，不对动作身份或谱面内容进行等价比较。
bool EditorActionStack::isDirty() const
{
    return m_undoStack.size() != m_saveIndex || m_hasNonUndoableChanges;
}

/// @brief 将当前历史深度记录为已保存基线。
/// @note 由保存成功的调用方触发，本方法本身不执行文件写入。
void EditorActionStack::markSaved()
{
    // 保存确认涵盖栈内和栈外修改，但不消费仍需通知观察者的变更类别。
    m_saveIndex             = m_undoStack.size();
    m_hasNonUndoableChanges = false;
}

/// @brief 记录无法通过撤销栈深度表达的修改。
/// @note 该标记不创建撤销记录，也不推断需要发布的谱面变更类别。
void EditorActionStack::markDirty()
{
    m_hasNonUndoableChanges = true;
}

/// @brief 消费自上次读取以来累积的变更类别。
/// @return 多次执行、撤销和重做的类别并集；无新增变化时返回 None。
::MMM::BeatmapMutationFlags EditorActionStack::takePendingMutationFlags()
{
    // 先保留返回值再清空，使同一批变化只由会话消费一次。
    const auto flags       = m_pendingMutationFlags;
    m_pendingMutationFlags = ::MMM::BeatmapMutationFlags::None;
    return flags;
}

/// @brief 查看下一次撤销涉及的类别，不执行动作或消费累计变化。
/// @return 栈顶动作的类别；空栈返回 None。
::MMM::BeatmapMutationFlags EditorActionStack::undoMutationFlags() const
{
    return m_undoStack.empty() ? ::MMM::BeatmapMutationFlags::None
                               : m_undoStack.back()->mutationFlags();
}

/// @brief 查看下一次重做涉及的类别，不改变历史栈。
/// @return 栈顶动作的类别；空栈返回 None。
::MMM::BeatmapMutationFlags EditorActionStack::redoMutationFlags() const
{
    return m_redoStack.empty() ? ::MMM::BeatmapMutationFlags::None
                               : m_redoStack.back()->mutationFlags();
}

/// @brief 按组合时的顺序执行子动作。
/// @param ctx 全部子动作共用的会话。
/// @pre 子动作指针均非空；各动作所需的数据依赖应与组合顺序一致。
/// @note 组合的是一条撤销记录，不提供失败回滚事务。
void CompositeEditorAction::execute(SessionContext& ctx)
{
    // 后一个动作可能依赖前一个动作刚创建的数据，不能重排执行次序。
    for ( auto& action : m_actions ) {
        action->execute(ctx);
    }
}

/// @brief 逆序撤销子动作以还原组合之前的状态。
/// @param ctx 首次执行组合时所用的会话。
void CompositeEditorAction::undo(SessionContext& ctx)
{
    // 先撤销依赖方，再撤销提供数据的前序动作，保持实体生命周期有效。
    for ( auto iterator = m_actions.rbegin(); iterator != m_actions.rend();
          ++iterator ) {
        (*iterator)->undo(ctx);
    }
}

/// @brief 按原执行顺序重做所有子动作。
/// @param ctx 撤销该组合时所用的会话。
void CompositeEditorAction::redo(SessionContext& ctx)
{
    // 保留子动作自己的重做语义，不用 execute 替代其恢复逻辑。
    for ( auto& action : m_actions ) {
        action->redo(ctx);
    }
}

/// @brief 返回整组操作的展示名称，而非逐个拼接子动作名称。
/// @return 构造时保存的组合名称副本。
std::string CompositeEditorAction::getName() const
{
    return m_name;
}

/// @brief 汇总组合内所有子动作的数据影响范围。
/// @return 子动作类别的位并集；空组合返回 None。
::MMM::BeatmapMutationFlags CompositeEditorAction::mutationFlags() const
{
    // 同一类别出现多次只占一个标志位，供会话统一发布变更通知。
    auto flags = ::MMM::BeatmapMutationFlags::None;
    for ( const auto& action : m_actions ) flags |= action->mutationFlags();
    return flags;
}

}  // namespace MMM::Logic
