#include "logic/session/context/SessionContext.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/EditorAction.h"
#include "logic/BeatmapSession.h"

namespace MMM::Logic
{

void EditorActionStack::pushAndExecute(std::unique_ptr<IEditorAction> action,
                                       SessionContext&                ctx)
{
    action->execute(ctx);
    m_undoStack.push_back(std::move(action));
    m_redoStack.clear();
    SessionUtils::syncBeatmap(ctx);
}

void EditorActionStack::undo(SessionContext& ctx)
{
    if ( m_undoStack.empty() ) return;
    auto action = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    action->undo(ctx);
    m_redoStack.push_back(std::move(action));
    SessionUtils::syncBeatmap(ctx);
}

void EditorActionStack::redo(SessionContext& ctx)
{
    if ( m_redoStack.empty() ) return;
    auto action = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    action->redo(ctx);
    m_undoStack.push_back(std::move(action));
    SessionUtils::syncBeatmap(ctx);
}

void EditorActionStack::clear()
{
    m_undoStack.clear();
    m_redoStack.clear();
    m_saveIndex = 0;
}

bool EditorActionStack::isDirty() const
{
    return m_undoStack.size() != m_saveIndex;
}

void EditorActionStack::markSaved()
{
    m_saveIndex = m_undoStack.size();
}

}  // namespace MMM::Logic
