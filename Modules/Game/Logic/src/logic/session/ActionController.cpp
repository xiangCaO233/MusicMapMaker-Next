#include "log/colorful-log.h"
#include "logic/session/ActionController.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/SessionUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/NoteAction.h"
#include "logic/session/TimelineAction.h"

namespace MMM::Logic
{

// --- TimelineAction Implementation ---

void TimelineAction::execute(SessionContext& ctx)
{
    auto& reg = ctx.timelineRegistry;
    if ( m_type == Type::Create ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create();
        reg.emplace<TimelineComponent>(m_entity, *m_after);
        XINFO("[Action] Create Timeline: Time={:.3f}, Val={:.2f}",
              m_after->m_timestamp,
              m_after->m_value);
    } else if ( m_type == Type::Delete ) {
        if ( reg.valid(m_entity) ) {
            XINFO("[Action] Delete Timeline: Time={:.3f}, Val={:.2f}",
                  m_before->m_timestamp,
                  m_before->m_value);
            reg.destroy(m_entity);
        }
    } else if ( m_type == Type::Update ) {
        if ( reg.valid(m_entity) ) {
            XINFO(
                "[Action] Update Timeline: [{:.3f}, {:.2f}] -> [{:.3f}, "
                "{:.2f}]",
                m_before->m_timestamp,
                m_before->m_value,
                m_after->m_timestamp,
                m_after->m_value);
            reg.patch<TimelineComponent>(
                m_entity, [&](TimelineComponent& tl) { tl = *m_after; });
        }
    }
}

void TimelineAction::undo(SessionContext& ctx)
{
    auto& reg = ctx.timelineRegistry;
    XINFO("[Undo] TimelineAction Type={}", static_cast<int>(m_type));
    if ( m_type == Type::Create ) {
        if ( reg.valid(m_entity) ) reg.destroy(m_entity);
    } else if ( m_type == Type::Delete ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create();
        reg.emplace<TimelineComponent>(m_entity, *m_before);
    } else if ( m_type == Type::Update ) {
        if ( reg.valid(m_entity) ) {
            reg.patch<TimelineComponent>(
                m_entity, [&](TimelineComponent& tl) { tl = *m_before; });
        }
    }
}

void TimelineAction::redo(SessionContext& ctx)
{
    XINFO("[Redo] TimelineAction");
    execute(ctx);
}

// --- NoteAction Implementation ---

void NoteAction::execute(SessionContext& ctx)
{
    auto& reg = ctx.noteRegistry;
    if ( m_type == Type::Create ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create();
        reg.emplace<NoteComponent>(m_entity, *m_after);
        reg.emplace<TransformComponent>(m_entity);
        reg.emplace<InteractionComponent>(m_entity);
        XINFO("[Action] Create Note: Type={}, Time={:.3f}, Track={}",
              (int)m_after->m_type,
              m_after->m_timestamp,
              m_after->m_trackIndex);
    } else if ( m_type == Type::Delete ) {
        if ( reg.valid(m_entity) ) {
            XINFO("[Action] Delete Note: Time={:.3f}, Track={}",
                  m_before->m_timestamp,
                  m_before->m_trackIndex);
            reg.destroy(m_entity);
        }
    } else if ( m_type == Type::Update ) {
        if ( reg.valid(m_entity) ) {
            XINFO(
                "[Action] Update Note: Time [{:.3f} -> {:.3f}], Track [{} -> "
                "{}]",
                m_before->m_timestamp,
                m_after->m_timestamp,
                m_before->m_trackIndex,
                m_after->m_trackIndex);
            reg.patch<NoteComponent>(m_entity,
                                     [&](NoteComponent& n) { n = *m_after; });
        }
    }
    SessionUtils::rebuildHitEvents(ctx);
}

void NoteAction::undo(SessionContext& ctx)
{
    auto& reg = ctx.noteRegistry;
    XINFO("[Undo] NoteAction Type={}", static_cast<int>(m_type));
    if ( m_type == Type::Create ) {
        if ( reg.valid(m_entity) ) reg.destroy(m_entity);
    } else if ( m_type == Type::Delete ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create();
        reg.emplace<NoteComponent>(m_entity, *m_before);
        reg.emplace<TransformComponent>(m_entity);
        reg.emplace<InteractionComponent>(m_entity);
    } else if ( m_type == Type::Update ) {
        if ( reg.valid(m_entity) ) {
            reg.patch<NoteComponent>(m_entity,
                                     [&](NoteComponent& n) { n = *m_before; });
        }
    }
    SessionUtils::rebuildHitEvents(ctx);
}

void NoteAction::redo(SessionContext& ctx)
{
    XINFO("[Redo] NoteAction");
    execute(ctx);
}

// --- BatchNoteAction Implementation ---

void BatchNoteAction::execute(SessionContext& ctx)
{
    auto& reg = ctx.noteRegistry;
    XINFO("[Action] BatchNoteAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.after.has_value() ) {
            if ( !reg.valid(entry.entity) ) entry.entity = reg.create();
            reg.emplace_or_replace<NoteComponent>(entry.entity, *entry.after);
            reg.emplace_or_replace<TransformComponent>(entry.entity);
            reg.emplace_or_replace<InteractionComponent>(entry.entity);
        } else if ( entry.before.has_value() ) {
            if ( reg.valid(entry.entity) ) reg.destroy(entry.entity);
        }
    }
    SessionUtils::rebuildHitEvents(ctx);
}

void BatchNoteAction::undo(SessionContext& ctx)
{
    auto& reg = ctx.noteRegistry;
    XINFO("[Undo] BatchNoteAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.before.has_value() ) {
            if ( !reg.valid(entry.entity) ) entry.entity = reg.create();
            reg.emplace_or_replace<NoteComponent>(entry.entity, *entry.before);
            reg.emplace_or_replace<TransformComponent>(entry.entity);
            reg.emplace_or_replace<InteractionComponent>(entry.entity);
        } else if ( entry.after.has_value() ) {
            if ( reg.valid(entry.entity) ) reg.destroy(entry.entity);
        }
    }
    SessionUtils::rebuildHitEvents(ctx);
}

void BatchNoteAction::redo(SessionContext& ctx)
{
    XINFO("[Redo] BatchNoteAction");
    execute(ctx);
}

}  // namespace MMM::Logic
