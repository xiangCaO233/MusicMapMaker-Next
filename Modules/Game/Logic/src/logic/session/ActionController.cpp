#include "logic/session/ActionController.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/session/NoteAction.h"
#include "logic/session/NoteIdentity.h"
#include "logic/session/SelectionState.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/TimelineAction.h"
#include "logic/session/context/SessionContext.h"

#include <algorithm>
#include <fmt/format.h>
#include <optional>
#include <vector>

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

/// @brief 判断两个可选音符颜色是否相同。
bool sameOptionalNoteColor(const std::optional<glm::vec4>& lhs,
                           const std::optional<glm::vec4>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    return !lhs || (lhs->r == rhs->r && lhs->g == rhs->g && lhs->b == rhs->b &&
                    lhs->a == rhs->a);
}

/// @brief 判断两组音符颜色覆写是否相同。
bool sameNoteColors(const NoteColorOverrides& lhs,
                    const NoteColorOverrides& rhs)
{
    return sameOptionalNoteColor(lhs.tap, rhs.tap) &&
           sameOptionalNoteColor(lhs.head, rhs.head) &&
           sameOptionalNoteColor(lhs.hold, rhs.hold) &&
           sameOptionalNoteColor(lhs.end, rhs.end) &&
           sameOptionalNoteColor(lhs.flickArrow, rhs.flickArrow) &&
           sameOptionalNoteColor(lhs.node, rhs.node);
}

/// @brief 判断两个可选音符采样绑定是否相同。
bool sameNoteBinding(const std::optional<::MMM::AudioSampleBinding>& lhs,
                     const std::optional<::MMM::AudioSampleBinding>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    return !lhs || (lhs->m_audioResourceId == rhs->m_audioResourceId &&
                    lhs->m_volume == rhs->m_volume);
}

/// @brief 判断两个折线子物件是否完全相同。
bool sameSubNote(const NoteComponent::SubNote& lhs,
                 const NoteComponent::SubNote& rhs)
{
    return lhs.type == rhs.type && lhs.timestamp == rhs.timestamp &&
           lhs.duration == rhs.duration && lhs.trackIndex == rhs.trackIndex &&
           lhs.dtrack == rhs.dtrack &&
           lhs.metadata.note_properties == rhs.metadata.note_properties &&
           sameNoteBinding(lhs.sampleBinding, rhs.sampleBinding) &&
           sameNoteColors(lhs.customColors, rhs.customColors) &&
           lhs.collaborationId == rhs.collaborationId;
}

/// @brief 判断两组折线子物件是否完全相同。
bool sameSubNotes(const std::vector<NoteComponent::SubNote>& lhs,
                  const std::vector<NoteComponent::SubNote>& rhs)
{
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), sameSubNote);
}

/// @brief 仅当当前字段仍等于本动作的结果时应用反向或正向值。
template<typename Value, typename Equal>
void applyNoteFieldTransition(Value& current, const Value& expected,
                              const Value& replacement, Equal equal)
{
    if ( !equal(expected, replacement) && equal(current, expected) ) {
        current = replacement;
    }
}

/// @brief 在 Undo/Redo 时按字段合并音符，避免覆盖其他协作者的后续修改。
/// @param current 当前权威音符。
/// @param expected 本动作在该方向执行前预期看到的值。
/// @param replacement 本动作在该方向希望恢复的值。
void applySelectiveNoteTransition(NoteComponent&       current,
                                  const NoteComponent& expected,
                                  const NoteComponent& replacement)
{
    const auto equal = [](const auto& lhs, const auto& rhs) {
        return lhs == rhs;
    };
    applyNoteFieldTransition(
        current.m_type, expected.m_type, replacement.m_type, equal);
    applyNoteFieldTransition(current.m_timestamp,
                             expected.m_timestamp,
                             replacement.m_timestamp,
                             equal);
    applyNoteFieldTransition(
        current.m_duration, expected.m_duration, replacement.m_duration, equal);
    applyNoteFieldTransition(current.m_trackIndex,
                             expected.m_trackIndex,
                             replacement.m_trackIndex,
                             equal);
    applyNoteFieldTransition(
        current.m_dtrack, expected.m_dtrack, replacement.m_dtrack, equal);
    applyNoteFieldTransition(current.m_metadata.note_properties,
                             expected.m_metadata.note_properties,
                             replacement.m_metadata.note_properties,
                             equal);
    applyNoteFieldTransition(current.m_sampleBinding,
                             expected.m_sampleBinding,
                             replacement.m_sampleBinding,
                             sameNoteBinding);
    applyNoteFieldTransition(current.m_customColors,
                             expected.m_customColors,
                             replacement.m_customColors,
                             sameNoteColors);
    applyNoteFieldTransition(current.m_subNotes,
                             expected.m_subNotes,
                             replacement.m_subNotes,
                             sameSubNotes);
}

/// @brief 将当前实体的逻辑标识传播到动作快照。
void inheritNoteIdentity(const NoteComponent&          current,
                         std::optional<NoteComponent>& snapshot)
{
    if ( !snapshot ) return;
    snapshot->m_collaborationId = current.m_collaborationId;
    const auto count =
        std::min(snapshot->m_subNotes.size(), current.m_subNotes.size());
    for ( std::size_t index = 0; index < count; ++index ) {
        snapshot->m_subNotes[index].collaborationId =
            current.m_subNotes[index].collaborationId;
    }
}

/// @brief 判断实体是否仍表示动作记录中的同一逻辑音符。
bool isActionNoteEntity(const entt::registry& reg, entt::entity entity,
                        const NoteComponent& snapshot)
{
    if ( !reg.valid(entity) || !reg.all_of<NoteComponent>(entity) ) {
        return false;
    }
    return snapshot.m_collaborationId.empty() ||
           reg.get<const NoteComponent>(entity).m_collaborationId ==
               snapshot.m_collaborationId;
}

/// @brief 为首次执行的批量音符动作补齐并关联根物件、子物件逻辑标识。
void prepareBatchNoteIdentities(entt::registry&                      reg,
                                std::vector<BatchNoteAction::Entry>& entries)
{
    for ( auto& entry : entries ) {
        if ( entry.before &&
             isActionNoteEntity(reg, entry.entity, *entry.before) ) {
            auto& current = reg.get<NoteComponent>(entry.entity);
            ensureNoteCollaborationIdentity(current);
            inheritNoteIdentity(current, entry.before);
            inheritNoteIdentity(current, entry.after);
        } else if ( !entry.before && entry.after &&
                    !entry.after->m_isSubNote ) {
            ensureNoteCollaborationIdentity(*entry.after);
        }
    }

    for ( auto& entry : entries ) {
        if ( entry.before || !entry.after || !entry.after->m_isSubNote ) {
            continue;
        }
        const auto parent = std::find_if(
            entries.begin(), entries.end(), [&](const auto& candidate) {
                return candidate.entity == entry.after->m_parentPolyline &&
                       candidate.after && !candidate.after->m_isSubNote;
            });
        if ( parent != entries.end() && entry.after->m_subIndex >= 0 &&
             static_cast<std::size_t>(entry.after->m_subIndex) <
                 parent->after->m_subNotes.size() ) {
            entry.after->m_collaborationId =
                parent->after
                    ->m_subNotes[static_cast<std::size_t>(
                        entry.after->m_subIndex)]
                    .collaborationId;
        } else {
            ensureNoteCollaborationIdentity(*entry.after);
        }
    }
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
        typeStr = TR("ui.status.action.create_event").data();
        break;
    case Type::Delete:
        typeStr = TR("ui.status.action.delete_event").data();
        break;
    case Type::Update:
        typeStr = TR("ui.status.action.update_event").data();
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
    } else if ( m_name == "Batch Timeline Update" ) {
        nameKey = "ui.status.action.batch_timing_update";
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
        ensureNoteCollaborationIdentity(*m_after);
        if ( !reg.valid(m_entity) ) m_entity = reg.create(m_entity);
        reg.emplace_or_replace<NoteComponent>(m_entity, *m_after);
        reg.emplace_or_replace<TransformComponent>(m_entity);
        reg.emplace_or_replace<InteractionComponent>(m_entity);
        XINFO("[Action] Create Note: Type={}, Time={:.3f}, Track={}",
              (int)m_after->m_type,
              m_after->m_timestamp,
              m_after->m_trackIndex);
    } else if ( m_type == Type::Delete ) {
        if ( isActionNoteEntity(reg, m_entity, *m_before) ) {
            auto& current = reg.get<NoteComponent>(m_entity);
            ensureNoteCollaborationIdentity(current);
            inheritNoteIdentity(current, m_before);
            XINFO("[Action] Delete Note: Time={:.3f}, Track={}",
                  m_before->m_timestamp,
                  m_before->m_trackIndex);
            forgetChartObjectSelection(
                ctx, ChartObjectKind::PlayerNote, m_entity);
            reg.destroy(m_entity);
        }
    } else if ( m_type == Type::Update ) {
        if ( isActionNoteEntity(reg, m_entity, *m_before) ) {
            auto& current = reg.get<NoteComponent>(m_entity);
            ensureNoteCollaborationIdentity(current);
            inheritNoteIdentity(current, m_before);
            inheritNoteIdentity(current, m_after);
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
        if ( isActionNoteEntity(reg, m_entity, *m_after) ) {
            forgetChartObjectSelection(
                ctx, ChartObjectKind::PlayerNote, m_entity);
            reg.destroy(m_entity);
        }
    } else if ( m_type == Type::Delete ) {
        if ( !reg.valid(m_entity) ) {
            m_entity = reg.create(m_entity);
            reg.emplace<NoteComponent>(m_entity, *m_before);
            reg.emplace<TransformComponent>(m_entity);
            reg.emplace<InteractionComponent>(m_entity);
        }
    } else if ( m_type == Type::Update ) {
        if ( isActionNoteEntity(reg, m_entity, *m_after) ) {
            reg.patch<NoteComponent>(m_entity, [&](NoteComponent& note) {
                applySelectiveNoteTransition(note, *m_after, *m_before);
            });
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
    if ( m_type != Type::Update ) {
        execute(ctx);
        return;
    }

    auto& reg = ctx.noteRegistry;
    if ( isActionNoteEntity(reg, m_entity, *m_before) ) {
        reg.patch<NoteComponent>(m_entity, [&](NoteComponent& note) {
            applySelectiveNoteTransition(note, *m_before, *m_after);
        });
    }
    ctx.m_needsNotesSync = true;
    SessionUtils::markHitEventsDirty(ctx);
    markNoteOrderDirty(ctx);
}

std::string NoteAction::getName() const
{
    std::string typeStr;
    switch ( m_type ) {
    case Type::Create:
        typeStr = TR("ui.status.action.create_note").data();
        break;
    case Type::Delete:
        typeStr = TR("ui.status.action.delete_note").data();
        break;
    case Type::Update:
        typeStr = TR("ui.status.action.update_note").data();
        break;
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
    prepareBatchNoteIdentities(reg, m_entries);
    XINFO("[Action] BatchNoteAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.after.has_value() ) {
            if ( !reg.valid(entry.entity) )
                entry.entity = reg.create(entry.entity);
            reg.emplace_or_replace<NoteComponent>(entry.entity, *entry.after);
            ensureNoteAuxiliaryComponents(reg, entry.entity);
            if ( entry.afterSelected ) {
                setChartObjectSelected(ctx,
                                       ChartObjectKind::PlayerNote,
                                       entry.entity,
                                       *entry.afterSelected);
            }
        } else if ( entry.before.has_value() ) {
            if ( isActionNoteEntity(reg, entry.entity, *entry.before) ) {
                forgetChartObjectSelection(
                    ctx, ChartObjectKind::PlayerNote, entry.entity);
                reg.destroy(entry.entity);
            }
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
        if ( entry.before.has_value() && entry.after.has_value() ) {
            if ( isActionNoteEntity(reg, entry.entity, *entry.after) ) {
                reg.patch<NoteComponent>(
                    entry.entity, [&](NoteComponent& note) {
                        applySelectiveNoteTransition(
                            note, *entry.after, *entry.before);
                    });
            }
            if ( entry.beforeSelected && reg.valid(entry.entity) ) {
                setChartObjectSelected(ctx,
                                       ChartObjectKind::PlayerNote,
                                       entry.entity,
                                       *entry.beforeSelected);
            }
        } else if ( entry.before.has_value() ) {
            if ( !reg.valid(entry.entity) ) {
                entry.entity = reg.create(entry.entity);
                reg.emplace<NoteComponent>(entry.entity, *entry.before);
                ensureNoteAuxiliaryComponents(reg, entry.entity);
                if ( entry.beforeSelected ) {
                    setChartObjectSelected(ctx,
                                           ChartObjectKind::PlayerNote,
                                           entry.entity,
                                           *entry.beforeSelected);
                }
            }
        } else if ( entry.after.has_value() ) {
            if ( isActionNoteEntity(reg, entry.entity, *entry.after) ) {
                forgetChartObjectSelection(
                    ctx, ChartObjectKind::PlayerNote, entry.entity);
                reg.destroy(entry.entity);
            }
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
    auto& reg = ctx.noteRegistry;
    for ( auto& entry : m_entries ) {
        if ( entry.before && entry.after ) {
            if ( isActionNoteEntity(reg, entry.entity, *entry.before) ) {
                reg.patch<NoteComponent>(
                    entry.entity, [&](NoteComponent& note) {
                        applySelectiveNoteTransition(
                            note, *entry.before, *entry.after);
                    });
                if ( entry.afterSelected ) {
                    setChartObjectSelected(ctx,
                                           ChartObjectKind::PlayerNote,
                                           entry.entity,
                                           *entry.afterSelected);
                }
            }
        } else if ( entry.after ) {
            if ( !reg.valid(entry.entity) ) {
                entry.entity = reg.create(entry.entity);
                reg.emplace<NoteComponent>(entry.entity, *entry.after);
                ensureNoteAuxiliaryComponents(reg, entry.entity);
                if ( entry.afterSelected ) {
                    setChartObjectSelected(ctx,
                                           ChartObjectKind::PlayerNote,
                                           entry.entity,
                                           *entry.afterSelected);
                }
            }
        } else if ( entry.before &&
                    isActionNoteEntity(reg, entry.entity, *entry.before) ) {
            forgetChartObjectSelection(
                ctx, ChartObjectKind::PlayerNote, entry.entity);
            reg.destroy(entry.entity);
        }
    }
    ctx.m_needsNotesSync = true;
    SessionUtils::markHitEventsDirty(ctx);
    markNoteOrderDirty(ctx);
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
