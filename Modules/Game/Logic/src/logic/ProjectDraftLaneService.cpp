#include "logic/ProjectDraftLaneService.h"

#include "logic/EditorClipboardProtocol.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectResourceService.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/session/NoteIdentity.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MMM::Logic
{
namespace
{

/// @brief 查找指定共享组。
ProjectDraftLaneGroup* findGroup(Project& project, std::string_view groupId)
{
    const auto iterator =
        std::find_if(project.m_draftLaneGroups.begin(),
                     project.m_draftLaneGroups.end(),
                     [groupId](const ProjectDraftLaneGroup& group) {
                         return group.m_mainAudioResourceId == groupId;
                     });
    return iterator == project.m_draftLaneGroups.end() ? nullptr : &*iterator;
}

/// @brief 解析当前会话实际使用的项目。
Project* resolveProject(SessionContext& ctx)
{
    if ( ctx.collaborationProject ) return ctx.collaborationProject.get();
    return EditorEngine::instance().getCurrentProject();
}

/// @brief 将持久化的草稿相对轨道换算为当前键数下的负轨道坐标。
void convertPersistedTracksToDraft(NoteComponent& note, int trackCount)
{
    note.m_trackIndex -= trackCount;
    note.m_isDraft = true;
    for ( auto& subNote : note.m_subNotes ) {
        subNote.trackIndex -= trackCount;
    }
}

/// @brief 将负轨道坐标换算为不依赖当前键数的草稿相对轨道。
void convertDraftTracksToPersisted(NoteComponent& note, int trackCount)
{
    note.m_trackIndex += trackCount;
    note.m_isDraft = false;
    for ( auto& subNote : note.m_subNotes ) {
        subNote.trackIndex += trackCount;
    }
}

/// @brief 判断草稿物件的全部轨道是否适用于当前键数。
bool hasValidPersistedDraftTracks(const NoteComponent& note, int trackCount)
{
    const auto validPart = [trackCount](
                               ::MMM::NoteType type, int track, int dtrack) {
        if ( track < 0 || track >= trackCount ) return false;
        if ( type != ::MMM::NoteType::FLICK ) return true;
        const int endTrack = track + dtrack;
        return endTrack >= 0 && endTrack < trackCount;
    };
    if ( !validPart(note.m_type, note.m_trackIndex, note.m_dtrack) ) {
        return false;
    }
    return std::all_of(
        note.m_subNotes.begin(),
        note.m_subNotes.end(),
        [&](const NoteComponent::SubNote& subNote) {
            return validPart(subNote.type, subNote.trackIndex, subNote.dtrack);
        });
}

/// @brief 将草稿条目编码为稳定的单条比较载荷。
std::string serializeDraftItem(const ClipboardItem& item)
{
    return EditorClipboardProtocol::serializeNotes(
        std::vector<ClipboardItem>{ item }, true);
}

/// @brief 编码草稿条目；空集合保持为空字符串。
std::string serializeDraftItems(const std::vector<ClipboardItem>& items)
{
    return items.empty() ? std::string{}
                         : EditorClipboardProtocol::serializeNotes(items, true);
}

/// @brief 解析草稿组载荷并丢弃非法的子实体条目。
std::vector<ClipboardItem> parseDraftItems(std::string_view payload)
{
    if ( payload.empty() ) return {};
    const auto parsed = EditorClipboardProtocol::parse(payload, true);
    if ( !parsed ) return {};

    auto items = parsed->notes;
    std::erase_if(
        items, [](const ClipboardItem& item) { return item.note.m_isSubNote; });
    return items;
}

/// @brief 为持久化组补齐稳定 ID，使跨画布刷新可以保留实体身份。
void canonicalizeGroupPayload(ProjectDraftLaneGroup& group)
{
    auto items = parseDraftItems(group.m_notePayload);
    for ( auto& item : items ) {
        ensureNoteCollaborationIdentity(item.note);
    }
    const auto canonical = serializeDraftItems(items);
    if ( canonical == group.m_notePayload ) return;
    group.m_notePayload = canonical;
    ++group.m_runtimeRevision;
}

/// @brief 按稳定 ID 将本会话相对基线的修改三方合并到最新共享载荷。
std::vector<ClipboardItem> mergeDraftItems(
    const std::vector<ClipboardItem>& base,
    const std::vector<ClipboardItem>& local, std::vector<ClipboardItem> latest)
{
    const auto findById = [](auto& items, std::string_view identity) {
        return std::find_if(items.begin(), items.end(), [identity](auto& item) {
            return item.note.m_collaborationId == identity;
        });
    };

    for ( const auto& baseItem : base ) {
        const auto& identity = baseItem.note.m_collaborationId;
        if ( identity.empty() || findById(local, identity) != local.end() ) {
            continue;
        }
        std::erase_if(latest, [&identity](const ClipboardItem& item) {
            return item.note.m_collaborationId == identity;
        });
    }

    for ( const auto& localItem : local ) {
        const auto& identity = localItem.note.m_collaborationId;
        const auto  baseIt   = findById(base, identity);
        const bool  locallyChanged =
            baseIt == base.end() ||
            serializeDraftItem(*baseIt) != serializeDraftItem(localItem);
        if ( !locallyChanged ) continue;

        const auto latestIt = findById(latest, identity);
        if ( latestIt == latest.end() ) {
            latest.push_back(localItem);
        } else {
            *latestIt = localItem;
        }
    }
    return latest;
}

/// @brief 判断当前会话是否正在直接编辑草稿物件。
bool hasActiveDraftInteraction(const SessionContext& ctx)
{
    if ( ctx.isDragging ) {
        if ( ctx.draggedObjectKind == ChartObjectKind::DraftNote ) return true;
        const auto isDraftEntity = [&](entt::entity entity) {
            const auto* note =
                ctx.noteRegistry.try_get<const NoteComponent>(entity);
            return note && note->m_isDraft;
        };
        if ( isDraftEntity(ctx.draggedEntity) ||
             std::ranges::any_of(ctx.dragRenderPinnedEntities,
                                 isDraftEntity) ) {
            return true;
        }
    }
    return (ctx.brushState.isActive && ctx.brushState.track < 0) ||
           (ctx.eraserState.isActive &&
            ctx.eraserState.targetObjectKind == ChartObjectKind::DraftNote);
}

/// @brief 确保草稿实体包含渲染和交互所需的轻量组件。
void ensureAuxiliaryComponents(entt::registry& registry, entt::entity entity)
{
    registry.emplace_or_replace<TransformComponent>(entity);
    if ( !registry.all_of<InteractionComponent>(entity) ) {
        registry.emplace<InteractionComponent>(entity);
    }
}

/// @brief 以稳定逻辑 ID 合并草稿载荷，并保留仍存在实体的身份。
std::string applyPayload(SessionContext&              ctx,
                         const ProjectDraftLaneGroup* group)
{
    std::vector<NoteComponent> desiredNotes;
    std::vector<ClipboardItem> visibleItems;
    if ( group ) {
        const auto items = parseDraftItems(group->m_notePayload);
        desiredNotes.reserve(items.size());
        visibleItems.reserve(items.size());
        for ( const auto& item : items ) {
            auto note = item.note;
            if ( !hasValidPersistedDraftTracks(note, ctx.trackCount) ) {
                continue;
            }
            ensureNoteCollaborationIdentity(note);
            visibleItems.push_back(ClipboardItem{ .note = note });
            convertPersistedTracksToDraft(note, ctx.trackCount);
            desiredNotes.push_back(std::move(note));
        }
    }

    std::unordered_map<std::string, entt::entity> existingById;
    std::vector<entt::entity>                     existingDraftEntities;
    const auto view = ctx.noteRegistry.view<const NoteComponent>();
    for ( const auto entity : view ) {
        const auto& note = view.get<const NoteComponent>(entity);
        if ( !note.m_isDraft ) continue;
        existingDraftEntities.push_back(entity);
        if ( !note.m_collaborationId.empty() ) {
            existingById.try_emplace(note.m_collaborationId, entity);
        }
    }

    std::unordered_set<entt::entity> retained;
    retained.reserve(existingDraftEntities.size() + desiredNotes.size());
    for ( auto& note : desiredNotes ) {
        entt::entity entity   = entt::null;
        const auto   existing = existingById.find(note.m_collaborationId);
        if ( existing != existingById.end() &&
             !retained.contains(existing->second) ) {
            entity = existing->second;
        } else {
            entity = ctx.noteRegistry.create();
        }
        retained.insert(entity);
        ctx.noteRegistry.emplace_or_replace<NoteComponent>(entity, note);
        ensureAuxiliaryComponents(ctx.noteRegistry, entity);

        for ( std::size_t index = 0; index < note.m_subNotes.size(); ++index ) {
            const auto&   subNote = note.m_subNotes[index];
            NoteComponent child;
            child.m_type            = subNote.type;
            child.m_timestamp       = subNote.timestamp;
            child.m_duration        = subNote.duration;
            child.m_trackIndex      = subNote.trackIndex;
            child.m_dtrack          = subNote.dtrack;
            child.m_isSubNote       = true;
            child.m_isDraft         = true;
            child.m_parentPolyline  = entity;
            child.m_subIndex        = static_cast<int>(index);
            child.m_metadata        = subNote.metadata;
            child.m_annotation      = subNote.annotation;
            child.m_sampleBinding   = subNote.sampleBinding;
            child.m_customColors    = subNote.customColors;
            child.m_collaborationId = subNote.collaborationId;

            entt::entity childEntity = entt::null;
            const auto   existingChild =
                existingById.find(child.m_collaborationId);
            if ( existingChild != existingById.end() &&
                 !retained.contains(existingChild->second) ) {
                childEntity = existingChild->second;
            } else {
                childEntity = ctx.noteRegistry.create();
            }
            retained.insert(childEntity);
            ctx.noteRegistry.emplace_or_replace<NoteComponent>(childEntity,
                                                               child);
            ensureAuxiliaryComponents(ctx.noteRegistry, childEntity);
        }
    }

    for ( const auto entity : existingDraftEntities ) {
        if ( retained.contains(entity) || !ctx.noteRegistry.valid(entity) ) {
            continue;
        }
        ctx.selectedNoteEntities.erase(entity);
        ctx.noteRegistry.destroy(entity);
    }

    if ( ctx.hoveredObjectKind == ChartObjectKind::DraftNote &&
         (ctx.hoveredEntity == entt::null ||
          !ctx.noteRegistry.valid(ctx.hoveredEntity)) ) {
        ctx.hoveredEntity     = entt::null;
        ctx.hoveredObjectKind = ChartObjectKind::PlayerNote;
    }
    ctx.isNoteOrderDirty      = true;
    ctx.isNotePruneDirty      = false;
    ctx.isTransformDirty      = true;
    ctx.isPreviewDensityDirty = true;
    ctx.isHitEventsDirty      = true;
    return serializeDraftItems(visibleItems);
}

}  // namespace

void ProjectDraftLaneService::load(SessionContext& ctx, Project* project)
{
    ctx.m_draftLaneGroupId.clear();
    ctx.m_draftLaneGroupRevision = 0U;
    ctx.m_draftLaneBasePayload.clear();
    if ( !project || !ctx.currentBeatmap || ctx.trackCount <= 0 ) {
        static_cast<void>(applyPayload(ctx, nullptr));
        return;
    }

    const auto* resource =
        ProjectResourceService::findDefaultBeatmapAudioResource(
            *project,
            *ctx.currentBeatmap,
            ctx.currentBeatmap->m_baseMapMetadata.map_path);
    if ( !resource || resource->m_id.empty() ) {
        static_cast<void>(applyPayload(ctx, nullptr));
        return;
    }

    ctx.m_draftLaneGroupId = resource->m_id;
    auto* group            = findGroup(*project, ctx.m_draftLaneGroupId);
    if ( group ) canonicalizeGroupPayload(*group);
    ctx.m_draftLaneBasePayload   = applyPayload(ctx, group);
    ctx.m_draftLaneGroupRevision = group ? group->m_runtimeRevision : 0U;
}

void ProjectDraftLaneService::refreshIfChanged(SessionContext& ctx)
{
    if ( ctx.m_draftLaneGroupId.empty() || !ctx.currentBeatmap ) return;
    auto* project = resolveProject(ctx);
    if ( !project ) return;
    auto* group    = findGroup(*project, ctx.m_draftLaneGroupId);
    auto  revision = group ? group->m_runtimeRevision : 0U;
    if ( revision == ctx.m_draftLaneGroupRevision ) return;
    if ( hasActiveDraftInteraction(ctx) ) return;
    if ( group ) {
        canonicalizeGroupPayload(*group);
        revision = group->m_runtimeRevision;
    }

    ctx.m_draftLaneBasePayload   = applyPayload(ctx, group);
    ctx.m_draftLaneGroupRevision = revision;
}

void ProjectDraftLaneService::sync(SessionContext& ctx)
{
    if ( !ctx.m_needsDraftNotesSync ) return;
    ctx.m_needsDraftNotesSync = false;
    if ( ctx.m_draftLaneGroupId.empty() || ctx.trackCount <= 0 ) return;

    auto* project = resolveProject(ctx);
    if ( !project ) return;

    std::vector<ClipboardItem> items;
    auto                       view = ctx.noteRegistry.view<NoteComponent>();
    items.reserve(view.size());
    for ( const auto entity : view ) {
        auto& source = view.get<NoteComponent>(entity);
        if ( source.m_isSubNote || !source.m_isDraft ) continue;
        ensureNoteCollaborationIdentity(source);
        ClipboardItem item;
        item.note = source;
        convertDraftTracksToPersisted(item.note, ctx.trackCount);
        items.push_back(std::move(item));
    }
    std::stable_sort(
        items.begin(), items.end(), [](const auto& lhs, const auto& rhs) {
            if ( lhs.note.m_timestamp != rhs.note.m_timestamp ) {
                return lhs.note.m_timestamp < rhs.note.m_timestamp;
            }
            return lhs.note.m_trackIndex < rhs.note.m_trackIndex;
        });

    auto* group = findGroup(*project, ctx.m_draftLaneGroupId);
    if ( !group ) {
        project->m_draftLaneGroups.push_back(ProjectDraftLaneGroup{
            .m_mainAudioResourceId = ctx.m_draftLaneGroupId,
        });
        group = &project->m_draftLaneGroups.back();
    } else {
        canonicalizeGroupPayload(*group);
    }
    const auto base   = parseDraftItems(ctx.m_draftLaneBasePayload);
    auto       latest = parseDraftItems(group->m_notePayload);
    auto       merged = mergeDraftItems(base, items, std::move(latest));
    std::stable_sort(
        merged.begin(), merged.end(), [](const auto& lhs, const auto& rhs) {
            if ( lhs.note.m_timestamp != rhs.note.m_timestamp ) {
                return lhs.note.m_timestamp < rhs.note.m_timestamp;
            }
            return lhs.note.m_trackIndex < rhs.note.m_trackIndex;
        });
    group->m_notePayload = serializeDraftItems(merged);
    ++group->m_runtimeRevision;
    ctx.m_draftLaneBasePayload   = applyPayload(ctx, group);
    ctx.m_draftLaneGroupRevision = group->m_runtimeRevision;
}

}  // namespace MMM::Logic
