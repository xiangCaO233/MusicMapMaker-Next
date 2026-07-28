#include "logic/session/EditorAction.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "logic/BeatmapSession.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"

namespace MMM::Logic
{

void EditorActionStack::pushAndExecute(std::unique_ptr<IEditorAction> action,
                                       SessionContext&                ctx)
{
    ctx.lastActionMessage = fmt::format(
        "{} {}", TR("ui.status.category.action").data(), action->getName());
    action->execute(ctx);
    m_undoStack.push_back(std::move(action));
    m_redoStack.clear();
    if ( ctx.m_needsTimingsSync || ctx.m_needsSamplesSync ) {
        SessionUtils::syncBeatmap(ctx);
    }
}

void EditorActionStack::undo(SessionContext& ctx)
{
    if ( m_undoStack.empty() ) return;
    auto action = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    ctx.lastActionMessage = fmt::format(
        "{} {}", TR("ui.status.category.undo").data(), action->getName());
    action->undo(ctx);
    m_redoStack.push_back(std::move(action));
    if ( ctx.m_needsTimingsSync || ctx.m_needsSamplesSync ) {
        SessionUtils::syncBeatmap(ctx);
    }
}

void EditorActionStack::redo(SessionContext& ctx)
{
    if ( m_redoStack.empty() ) return;
    auto action = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    ctx.lastActionMessage = fmt::format(
        "{} {}", TR("ui.status.category.redo").data(), action->getName());
    action->redo(ctx);
    m_undoStack.push_back(std::move(action));
    if ( ctx.m_needsTimingsSync || ctx.m_needsSamplesSync ) {
        SessionUtils::syncBeatmap(ctx);
    }
}

void EditorActionStack::clear()
{
    m_undoStack.clear();
    m_redoStack.clear();
    m_saveIndex             = 0;
    m_hasNonUndoableChanges = false;
}

bool EditorActionStack::isDirty() const
{
    return m_undoStack.size() != m_saveIndex || m_hasNonUndoableChanges;
}

void EditorActionStack::markSaved()
{
    m_saveIndex             = m_undoStack.size();
    m_hasNonUndoableChanges = false;
}

void EditorActionStack::markDirty()
{
    m_hasNonUndoableChanges = true;
}

void CompositeEditorAction::execute(SessionContext& ctx)
{
    for ( auto& action : m_actions ) {
        action->execute(ctx);
    }
}

void CompositeEditorAction::undo(SessionContext& ctx)
{
    for ( auto iterator = m_actions.rbegin(); iterator != m_actions.rend();
          ++iterator ) {
        (*iterator)->undo(ctx);
    }
}

void CompositeEditorAction::redo(SessionContext& ctx)
{
    for ( auto& action : m_actions ) {
        action->redo(ctx);
    }
}

std::string CompositeEditorAction::getName() const
{
    return m_name;
}

}  // namespace MMM::Logic
