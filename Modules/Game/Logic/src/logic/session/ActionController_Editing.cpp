#include "log/colorful-log.h"
#include "logic/session/ActionController.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/SessionUtils.h"
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

void ActionController::handleCommand(const CmdUndo& cmd)
{
    m_ctx.actionStack.undo(m_ctx);
}

void ActionController::handleCommand(const CmdRedo& cmd)
{
    m_ctx.actionStack.redo(m_ctx);
}

void ActionController::handleCommand(const CmdCopy& cmd)
{
    m_ctx.clipboard.clear();
    auto view = m_ctx.noteRegistry.view<NoteComponent, InteractionComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            const auto& note = view.get<NoteComponent>(entity);
            m_ctx.clipboard.push_back({ note });
        }
    }
    XINFO("Copied {} items to clipboard", m_ctx.clipboard.size());
}

void ActionController::handleCommand(const CmdCut& cmd)
{
    handleCommand(CmdCopy{});
    auto view = m_ctx.noteRegistry.view<InteractionComponent>();
    for ( auto entity : view ) {
        auto& ic = m_ctx.noteRegistry.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            ic.isCut = true;
        }
    }
}

void ActionController::handleCommand(const CmdDeleteSelected& cmd)
{
    std::vector<BatchNoteAction::Entry> entries;

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( ic.isSelected ) {
            entries.push_back(
                { entity, view.get<NoteComponent>(entity), std::nullopt });
        }
    }

    // 如果没有任何选中的，但有悬停的，也删除悬停的 (符合习惯)
    if ( entries.empty() && m_ctx.hoveredEntity != entt::null ) {
        if ( m_ctx.noteRegistry.valid(m_ctx.hoveredEntity) &&
             m_ctx.noteRegistry.all_of<NoteComponent>(m_ctx.hoveredEntity) ) {
            entries.push_back({ m_ctx.hoveredEntity,
                                m_ctx.noteRegistry.get<NoteComponent>(
                                    m_ctx.hoveredEntity),
                                std::nullopt });
        }
    }

    if ( !entries.empty() ) {
        auto action = std::make_unique<BatchNoteAction>(std::move(entries));
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        XINFO("Deleted {} selected/hovered items", entries.size());
    }
}

void ActionController::handleCommand(const CmdPaste& cmd)
{
    if ( m_ctx.clipboard.empty() ) return;

    // 计算基准点 (目前取所有选中音符的最小时间)
    double minTime = m_ctx.clipboard[0].note.m_timestamp;
    for ( const auto& item : m_ctx.clipboard ) {
        minTime = std::min(minTime, item.note.m_timestamp);
    }

    std::vector<BatchNoteAction::Entry> entries;

    // 1. 如果之前有 Cut，需要删除那些 Cut 的物件
    auto view = m_ctx.noteRegistry.view<InteractionComponent>();
    for ( auto entity : view ) {
        auto& ic = m_ctx.noteRegistry.get<InteractionComponent>(entity);
        if ( ic.isCut ) {
            if ( m_ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                auto oldNote = m_ctx.noteRegistry.get<NoteComponent>(entity);
                entries.push_back({ entity, oldNote, std::nullopt });
            }
        }
    }

    // 2. 粘贴到当前视觉时间 (判定线)
    double pasteTime = m_ctx.visualTime;

    // 尝试获取鼠标悬停处的时间作为基准 (如果有)
    // 注意：这里为了简化直接使用视觉时间。如果需要鼠标对齐，需要 UI 传入坐标。

    for ( const auto& item : m_ctx.clipboard ) {
        auto newNote        = item.note;
        newNote.m_timestamp = pasteTime + (item.note.m_timestamp - minTime);
        entries.push_back({ entt::null, std::nullopt, newNote });
    }

    auto action = std::make_unique<BatchNoteAction>(std::move(entries));
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);

    // 清除剪切状态
    for ( auto entity : view ) {
        m_ctx.noteRegistry.get<InteractionComponent>(entity).isCut = false;
    }
}

// --- Timeline Handlers ---

void ActionController::handleCommand(const CmdUpdateTimelineEvent& cmd)
{
    if ( m_ctx.timelineRegistry.valid(cmd.entity) ) {
        auto oldTl = m_ctx.timelineRegistry.get<TimelineComponent>(cmd.entity);
        auto newTl = oldTl;
        newTl.m_timestamp = cmd.newTime;
        newTl.m_value     = cmd.newValue;

        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Update, cmd.entity, oldTl, newTl);
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    }
}

void ActionController::handleCommand(const CmdDeleteTimelineEvent& cmd)
{
    if ( m_ctx.timelineRegistry.valid(cmd.entity) ) {
        auto oldTl  = m_ctx.timelineRegistry.get<TimelineComponent>(cmd.entity);
        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Delete, cmd.entity, oldTl, std::nullopt);
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    }
}

void ActionController::handleCommand(const CmdCreateTimelineEvent& cmd)
{
    TimelineComponent newTl{ cmd.time, cmd.type, cmd.value };
    auto              action = std::make_unique<TimelineAction>(
        TimelineAction::Type::Create, entt::null, std::nullopt, newTl);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
}

}  // namespace MMM::Logic
