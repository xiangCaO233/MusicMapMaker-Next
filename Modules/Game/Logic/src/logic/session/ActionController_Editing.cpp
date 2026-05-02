#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/ActionController.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/TimelineAction.h"
#include "logic/session/context/SessionContext.h"
#include <unordered_set>

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
            entries.push_back(
                { m_ctx.hoveredEntity,
                  m_ctx.noteRegistry.get<NoteComponent>(m_ctx.hoveredEntity),
                  std::nullopt });
        }
    }

    // 收集所有被删除实体的 ID（用于后续查找子物件）
    std::unordered_set<entt::entity> deletedEntities;
    for ( const auto& entry : entries ) {
        deletedEntities.insert(entry.entity);
    }

    // 同时删除被删除折线下所有子物件实体，防止孤儿子实体残留
    for ( entt::entity parentEntity : deletedEntities ) {
        if ( m_ctx.noteRegistry.valid(parentEntity) &&
             m_ctx.noteRegistry.all_of<NoteComponent>(parentEntity) ) {
            const auto& nc =
                m_ctx.noteRegistry.get<NoteComponent>(parentEntity);
            if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                 !nc.m_subNotes.empty() ) {
                for ( auto subEnt : m_ctx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == parentEntity ) {
                        entries.push_back({ subEnt, subNC, std::nullopt });
                    }
                }
            }
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

                // 同时删除 Polyline 的子物件实体
                if ( oldNote.m_type == ::MMM::NoteType::POLYLINE &&
                     !oldNote.m_subNotes.empty() ) {
                    for ( auto subEnt :
                          m_ctx.noteRegistry.view<NoteComponent>() ) {
                        const auto& subNC =
                            m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                        if ( subNC.m_isSubNote &&
                             subNC.m_parentPolyline == entity ) {
                            entries.push_back({ subEnt, subNC, std::nullopt });
                        }
                    }
                }
            }
        }
    }

    // 2. 粘贴到当前视觉时间 (判定线)
    double pasteTime = m_ctx.visualTime;

    // 尝试获取鼠标悬停处的时间作为基准 (如果有)
    // 注意：这里为了简化直接使用视觉时间。如果需要鼠标对齐，需要 UI 传入坐标。

    double timeOffset = pasteTime - minTime;

    for ( const auto& item : m_ctx.clipboard ) {
        auto newNote        = item.note;
        newNote.m_timestamp = item.note.m_timestamp + timeOffset;

        // 折线物件：同步偏移所有子物件的时间戳
        if ( newNote.m_type == ::MMM::NoteType::POLYLINE ) {
            for ( auto& sub : newNote.m_subNotes ) {
                sub.timestamp += timeOffset;
            }
        }

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
