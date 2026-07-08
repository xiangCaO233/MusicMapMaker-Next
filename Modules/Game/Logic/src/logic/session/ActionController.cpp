#include "logic/session/ActionController.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/TimelineAction.h"
#include "logic/session/context/SessionContext.h"

namespace MMM::Logic
{

namespace
{
/// @brief 确保音符实体拥有更新所需的辅助组件，并保留已有交互状态。
void ensureNoteAuxiliaryComponents(entt::registry& reg, entt::entity entity)
{
    if ( !reg.all_of<TransformComponent>(entity) ) {
        reg.emplace<TransformComponent>(entity);
    }
    if ( !reg.all_of<InteractionComponent>(entity) ) {
        reg.emplace<InteractionComponent>(entity);
    }
}

/// @brief 标记音符创建/更新后需要完整重建排序缓存。
void markNoteOrderDirty(SessionContext& ctx)
{
    ctx.isNoteOrderDirty = true;
    ctx.isNoteStatsDirty = true;
}

/// @brief 标记音符删除后只需从排序缓存中剔除失效实体。
void markNotePruneDirty(SessionContext& ctx)
{
    ctx.isNotePruneDirty = true;
    ctx.isNoteStatsDirty = true;
}
}  // namespace

// --- TimelineAction 实现 ---

void TimelineAction::execute(SessionContext& ctx)
{
    auto& reg = ctx.timelineRegistry;
    if ( m_type == Type::Create ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create(m_entity);
        reg.emplace_or_replace<TimelineComponent>(m_entity, *m_after);
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
    ctx.m_needsTimingsSync = true;
    ctx.isNoteStatsDirty   = true;
}

void TimelineAction::undo(SessionContext& ctx)
{
    auto& reg = ctx.timelineRegistry;
    XINFO("[Undo] TimelineAction Type={}", static_cast<int>(m_type));
    if ( m_type == Type::Create ) {
        if ( reg.valid(m_entity) ) reg.destroy(m_entity);
    } else if ( m_type == Type::Delete ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create(m_entity);
        reg.emplace_or_replace<TimelineComponent>(m_entity, *m_before);
    } else if ( m_type == Type::Update ) {
        if ( reg.valid(m_entity) ) {
            reg.patch<TimelineComponent>(
                m_entity, [&](TimelineComponent& tl) { tl = *m_before; });
        }
    }
    ctx.m_needsTimingsSync = true;
    ctx.isNoteStatsDirty   = true;
}

void TimelineAction::redo(SessionContext& ctx)
{
    XINFO("[Redo] TimelineAction");
    execute(ctx);
}

std::string TimelineAction::getName() const
{
    std::string typeStr;
    switch ( m_type ) {
    case Type::Create:
        typeStr = TR("ui.status.action.create_event").pStr;
        break;
    case Type::Delete:
        typeStr = TR("ui.status.action.delete_event").pStr;
        break;
    case Type::Update:
        typeStr = TR("ui.status.action.update_event").pStr;
        break;
    }
    if ( m_after )
        return fmt::format("{} ({}: {:.3f})",
                           typeStr,
                           TR("ui.status.info.time"),
                           m_after->m_timestamp);
    if ( m_before )
        return fmt::format("{} ({}: {:.3f})",
                           typeStr,
                           TR("ui.status.info.time"),
                           m_before->m_timestamp);
    return typeStr;
}

// --- BatchTimelineAction 实现 ---

void BatchTimelineAction::execute(SessionContext& ctx)
{
    auto& reg = ctx.timelineRegistry;
    XINFO("[Action] BatchTimelineAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.after.has_value() ) {
            if ( !reg.valid(entry.entity) ) {
                entry.entity = reg.create(entry.entity);
            }
            reg.emplace_or_replace<TimelineComponent>(entry.entity,
                                                      *entry.after);
        } else if ( entry.before.has_value() ) {
            if ( reg.valid(entry.entity) ) {
                reg.destroy(entry.entity);
            }
        }
    }
    ctx.m_needsTimingsSync = true;
    ctx.isNoteStatsDirty   = true;
}

void BatchTimelineAction::undo(SessionContext& ctx)
{
    auto& reg = ctx.timelineRegistry;
    XINFO("[Undo] BatchTimelineAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.before.has_value() ) {
            if ( !reg.valid(entry.entity) ) {
                entry.entity = reg.create(entry.entity);
            }
            reg.emplace_or_replace<TimelineComponent>(entry.entity,
                                                      *entry.before);
        } else if ( entry.after.has_value() ) {
            if ( reg.valid(entry.entity) ) {
                reg.destroy(entry.entity);
            }
        }
    }
    ctx.m_needsTimingsSync = true;
    ctx.isNoteStatsDirty   = true;
}

void BatchTimelineAction::redo(SessionContext& ctx)
{
    XINFO("[Redo] BatchTimelineAction");
    execute(ctx);
}

std::string BatchTimelineAction::getName() const
{
    const char* nameKey = "ui.status.action.batch_note";
    if ( m_name == "Paste" ) {
        nameKey = "ui.status.action.paste";
    }

    return fmt::format("{}: {} {}",
                       TR(nameKey),
                       m_entries.size(),
                       TR("ui.status.info.entries"));
}

// --- NoteAction 实现 ---

void NoteAction::execute(SessionContext& ctx)
{
    auto& reg = ctx.noteRegistry;
    if ( m_type == Type::Create ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create(m_entity);
        reg.emplace_or_replace<NoteComponent>(m_entity, *m_after);
        reg.emplace_or_replace<TransformComponent>(m_entity);
        reg.emplace_or_replace<InteractionComponent>(m_entity);
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
    ctx.m_needsNotesSync = true;
    SessionUtils::markHitEventsDirty(ctx);
    if ( m_type == Type::Delete ) {
        markNotePruneDirty(ctx);
    } else {
        markNoteOrderDirty(ctx);
    }
}

void NoteAction::undo(SessionContext& ctx)
{
    auto& reg = ctx.noteRegistry;
    XINFO("[Undo] NoteAction Type={}", static_cast<int>(m_type));
    if ( m_type == Type::Create ) {
        if ( reg.valid(m_entity) ) reg.destroy(m_entity);
    } else if ( m_type == Type::Delete ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create(m_entity);
        reg.emplace_or_replace<NoteComponent>(m_entity, *m_before);
        reg.emplace_or_replace<TransformComponent>(m_entity);
        reg.emplace_or_replace<InteractionComponent>(m_entity);
    } else if ( m_type == Type::Update ) {
        if ( reg.valid(m_entity) ) {
            reg.patch<NoteComponent>(m_entity,
                                     [&](NoteComponent& n) { n = *m_before; });
        }
    }
    ctx.m_needsNotesSync = true;
    SessionUtils::markHitEventsDirty(ctx);
    if ( m_type == Type::Create ) {
        markNotePruneDirty(ctx);
    } else {
        markNoteOrderDirty(ctx);
    }
}

void NoteAction::redo(SessionContext& ctx)
{
    XINFO("[Redo] NoteAction");
    execute(ctx);
}

std::string NoteAction::getName() const
{
    std::string typeStr;
    switch ( m_type ) {
    case Type::Create: typeStr = TR("ui.status.action.create_note").pStr; break;
    case Type::Delete: typeStr = TR("ui.status.action.delete_note").pStr; break;
    case Type::Update: typeStr = TR("ui.status.action.update_note").pStr; break;
    }
    if ( m_after )
        return fmt::format("{} ({}: {:.3f}, {}: {})",
                           typeStr,
                           TR("ui.status.info.time"),
                           m_after->m_timestamp,
                           TR("ui.status.info.track"),
                           m_after->m_trackIndex);
    if ( m_before )
        return fmt::format("{} ({}: {:.3f}, {}: {})",
                           typeStr,
                           TR("ui.status.info.time"),
                           m_before->m_timestamp,
                           TR("ui.status.info.track"),
                           m_before->m_trackIndex);
    return typeStr;
}

// --- BatchNoteAction 实现 ---

void BatchNoteAction::execute(SessionContext& ctx)
{
    auto& reg = ctx.noteRegistry;
    XINFO("[Action] BatchNoteAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.after.has_value() ) {
            if ( !reg.valid(entry.entity) )
                entry.entity = reg.create(entry.entity);
            reg.emplace_or_replace<NoteComponent>(entry.entity, *entry.after);
            ensureNoteAuxiliaryComponents(reg, entry.entity);
        } else if ( entry.before.has_value() ) {
            if ( reg.valid(entry.entity) ) reg.destroy(entry.entity);
        }
    }
    ctx.m_needsNotesSync = true;
    SessionUtils::markHitEventsDirty(ctx);
    bool needsOrderRebuild = false;
    bool needsPrune        = false;
    for ( const auto& entry : m_entries ) {
        if ( entry.after.has_value() ) {
            needsOrderRebuild = true;
        } else if ( entry.before.has_value() ) {
            needsPrune = true;
        }
    }
    if ( needsOrderRebuild ) {
        markNoteOrderDirty(ctx);
    } else if ( needsPrune ) {
        markNotePruneDirty(ctx);
    }
}

void BatchNoteAction::undo(SessionContext& ctx)
{
    auto& reg = ctx.noteRegistry;
    XINFO("[Undo] BatchNoteAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.before.has_value() ) {
            if ( !reg.valid(entry.entity) )
                entry.entity = reg.create(entry.entity);
            reg.emplace_or_replace<NoteComponent>(entry.entity, *entry.before);
            ensureNoteAuxiliaryComponents(reg, entry.entity);
        } else if ( entry.after.has_value() ) {
            if ( reg.valid(entry.entity) ) reg.destroy(entry.entity);
        }
    }
    ctx.m_needsNotesSync = true;
    SessionUtils::markHitEventsDirty(ctx);
    bool needsOrderRebuild = false;
    bool needsPrune        = false;
    for ( const auto& entry : m_entries ) {
        if ( entry.before.has_value() ) {
            needsOrderRebuild = true;
        } else if ( entry.after.has_value() ) {
            needsPrune = true;
        }
    }
    if ( needsOrderRebuild ) {
        markNoteOrderDirty(ctx);
    } else if ( needsPrune ) {
        markNotePruneDirty(ctx);
    }
}

void BatchNoteAction::redo(SessionContext& ctx)
{
    XINFO("[Redo] BatchNoteAction");
    execute(ctx);
}

std::string BatchNoteAction::getName() const
{
    const char* nameKey = "ui.status.action.batch_note";
    if ( m_name == "Delete Selected" )
        nameKey = "ui.status.action.delete_selected";
    else if ( m_name == "Paste" )
        nameKey = "ui.status.action.paste";
    else if ( m_name == "Mirror Paste" )
        nameKey = "ui.edit.mirror_paste";
    else if ( m_name == "Align Selected" )
        nameKey = "ui.tools.align_beats";

    return fmt::format("{}: {} {}",
                       TR(nameKey),
                       m_entries.size(),
                       TR("ui.status.info.entries"));
}

}  // namespace MMM::Logic
