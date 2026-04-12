#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/NoteAction.h"
#include "logic/session/TimelineAction.h"

namespace MMM::Logic
{

// --- Editing Handlers ---

void BeatmapSession::handleCommand(const CmdUndo& cmd)
{
    m_actionStack.undo(*this);
}

void BeatmapSession::handleCommand(const CmdRedo& cmd)
{
    m_actionStack.redo(*this);
}

void BeatmapSession::handleCommand(const CmdCopy& cmd)
{
    m_clipboard.clear();
    auto view = m_noteRegistry.view<NoteComponent, InteractionComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            const auto& note = view.get<NoteComponent>(entity);
            m_clipboard.push_back({ note });
        }
    }
    XINFO("Copied {} items to clipboard", m_clipboard.size());
}

void BeatmapSession::handleCommand(const CmdCut& cmd)
{
    handleCommand(CmdCopy{});
    auto view = m_noteRegistry.view<InteractionComponent>();
    for ( auto entity : view ) {
        auto& ic = m_noteRegistry.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            ic.isCut = true;
        }
    }
}

void BeatmapSession::handleCommand(const CmdPaste& cmd)
{
    if ( m_clipboard.empty() ) return;

    // 计算基准点 (目前取所有选中音符的最小时间)
    double minTime = m_clipboard[0].note.m_timestamp;
    for ( const auto& item : m_clipboard ) {
        minTime = std::min(minTime, item.note.m_timestamp);
    }

    std::vector<BatchNoteAction::Entry> entries;

    // 1. 如果之前有 Cut，需要删除那些 Cut 的物件
    auto view = m_noteRegistry.view<InteractionComponent>();
    for ( auto entity : view ) {
        auto& ic = m_noteRegistry.get<InteractionComponent>(entity);
        if ( ic.isCut ) {
            if ( m_noteRegistry.all_of<NoteComponent>(entity) ) {
                auto oldNote = m_noteRegistry.get<NoteComponent>(entity);
                entries.push_back({ entity, oldNote, std::nullopt });
            }
        }
    }

    // 2. 粘贴到当前视觉时间 (判定线)
    double pasteTime = m_visualTime;

    // 尝试获取鼠标悬停处的时间作为基准 (如果有)
    // 注意：这里为了简化直接使用视觉时间。如果需要鼠标对齐，需要 UI 传入坐标。

    for ( const auto& item : m_clipboard ) {
        auto newNote        = item.note;
        newNote.m_timestamp = pasteTime + (item.note.m_timestamp - minTime);
        entries.push_back({ entt::null, std::nullopt, newNote });
    }

    auto action = std::make_unique<BatchNoteAction>(std::move(entries));
    m_actionStack.pushAndExecute(std::move(action), *this);

    // 清除剪切状态
    for ( auto entity : view ) {
        m_noteRegistry.get<InteractionComponent>(entity).isCut = false;
    }
}

// --- Timeline Handlers ---

void BeatmapSession::handleCommand(const CmdUpdateTimelineEvent& cmd)
{
    if ( m_timelineRegistry.valid(cmd.entity) ) {
        auto oldTl = m_timelineRegistry.get<TimelineComponent>(cmd.entity);
        auto newTl = oldTl;
        newTl.m_timestamp = cmd.newTime;
        newTl.m_value     = cmd.newValue;

        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Update, cmd.entity, oldTl, newTl);
        m_actionStack.pushAndExecute(std::move(action), *this);
    }
}

void BeatmapSession::handleCommand(const CmdDeleteTimelineEvent& cmd)
{
    if ( m_timelineRegistry.valid(cmd.entity) ) {
        auto oldTl  = m_timelineRegistry.get<TimelineComponent>(cmd.entity);
        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Delete, cmd.entity, oldTl, std::nullopt);
        m_actionStack.pushAndExecute(std::move(action), *this);
    }
}

void BeatmapSession::handleCommand(const CmdCreateTimelineEvent& cmd)
{
    TimelineComponent newTl{ cmd.time, cmd.type, cmd.value };
    auto              action = std::make_unique<TimelineAction>(
        TimelineAction::Type::Create, entt::null, std::nullopt, newTl);
    m_actionStack.pushAndExecute(std::move(action), *this);
}

}  // namespace MMM::Logic
