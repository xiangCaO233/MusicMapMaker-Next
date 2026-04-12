#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/session/NoteAction.h"
#include "logic/session/TimelineAction.h"

namespace MMM::Logic
{

// --- TimelineAction Implementation ---

void TimelineAction::execute(BeatmapSession& session)
{
    auto& reg = session.getTimelineRegistry();
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

void TimelineAction::undo(BeatmapSession& session)
{
    auto& reg = session.getTimelineRegistry();
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

void TimelineAction::redo(BeatmapSession& session)
{
    XINFO("[Redo] TimelineAction");
    execute(session);
}

// --- NoteAction Implementation ---

void NoteAction::execute(BeatmapSession& session)
{
    auto& reg = session.getNoteRegistry();
    if ( m_type == Type::Create ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create();
        reg.emplace<NoteComponent>(m_entity, *m_after);
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
    session.rebuildHitEvents();
}

void NoteAction::undo(BeatmapSession& session)
{
    auto& reg = session.getNoteRegistry();
    XINFO("[Undo] NoteAction Type={}", static_cast<int>(m_type));
    if ( m_type == Type::Create ) {
        if ( reg.valid(m_entity) ) reg.destroy(m_entity);
    } else if ( m_type == Type::Delete ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create();
        reg.emplace<NoteComponent>(m_entity, *m_before);
    } else if ( m_type == Type::Update ) {
        if ( reg.valid(m_entity) ) {
            reg.patch<NoteComponent>(m_entity,
                                     [&](NoteComponent& n) { n = *m_before; });
        }
    }
    session.rebuildHitEvents();
}

void NoteAction::redo(BeatmapSession& session)
{
    XINFO("[Redo] NoteAction");
    execute(session);
}

// --- BatchNoteAction Implementation ---

void BatchNoteAction::execute(BeatmapSession& session)
{
    auto& reg = session.getNoteRegistry();
    XINFO("[Action] BatchNoteAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.after.has_value() ) {
            if ( !reg.valid(entry.entity) ) entry.entity = reg.create();
            reg.emplace_or_replace<NoteComponent>(entry.entity, *entry.after);
        } else if ( entry.before.has_value() ) {
            if ( reg.valid(entry.entity) ) reg.destroy(entry.entity);
        }
    }
    session.rebuildHitEvents();
}

void BatchNoteAction::undo(BeatmapSession& session)
{
    auto& reg = session.getNoteRegistry();
    XINFO("[Undo] BatchNoteAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.before.has_value() ) {
            if ( !reg.valid(entry.entity) ) entry.entity = reg.create();
            reg.emplace_or_replace<NoteComponent>(entry.entity, *entry.before);
        } else if ( entry.after.has_value() ) {
            if ( reg.valid(entry.entity) ) reg.destroy(entry.entity);
        }
    }
    session.rebuildHitEvents();
}

void BatchNoteAction::redo(BeatmapSession& session)
{
    XINFO("[Redo] BatchNoteAction");
    execute(session);
}

}  // namespace MMM::Logic
