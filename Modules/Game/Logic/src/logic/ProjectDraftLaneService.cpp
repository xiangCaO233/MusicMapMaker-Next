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
/// @param project 持有共享草稿的项目。
/// @param groupId 主音频资源 ID，也是共享组的键。
/// @return 项目内的观察指针，未找到时为空。
/// @note 项目组容器增删后需重新取得指针，不跨容器修改缓存。
/// @details 组键取音频资源身份而非谱面路径，重命名谱面不改变草稿归属。
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
/// @param ctx 可能携带协作项目的会话。
/// @return 优先返回会话协作项目，否则返回编辑器本机项目。
/// @note 此处只观察现有项目，不复制或创建项目，也不承担跨线程生命周期保护。
Project* resolveProject(SessionContext& ctx)
{
    // 协作谱面的草稿不能误写入恰好在本机打开的另一个项目。
    if ( ctx.collaborationProject ) return ctx.collaborationProject.get();
    return EditorEngine::instance().getCurrentProject();
}

/// @brief 将持久化的草稿相对轨道换算为当前键数下的负轨道坐标。
/// @param note 待转换的根物件及其内嵌子物件。
/// @param trackCount 草稿轨宽度，不是物件自身的玩家轨道数。
/// @pre 输入已按同一宽度验证为非负持久化坐标。
/// @details 头节点与子节点统一平移，Flick 的相对终点偏移保持原值。
/// 调用后 note 仅供当前会话使用，不直接回写共享载荷。
void convertPersistedTracksToDraft(NoteComponent& note, int trackCount)
{
    // 编码列减去草稿宽度后落入 [-trackCount, -1]，与玩家区域保持分离。
    note.m_trackIndex -= trackCount;
    note.m_isDraft = true;
    for ( auto& subNote : note.m_subNotes ) {
        // 子节点必须使用同一原点，不能只移动折线头而改变内部几何关系。
        subNote.trackIndex -= trackCount;
    }
}

/// @brief 将负轨道坐标换算为不依赖当前键数的草稿相对轨道。
/// @param note 待编码的草稿根物件副本。
/// @param trackCount 当前草稿轨宽度。
/// @note 只用于序列化副本，不应把会话实体改回玩家物件。
/// @details 宽度与载荷共同定义原点；读取同一载荷时必须使用对应宽度反向换算。
void convertDraftTracksToPersisted(NoteComponent& note, int trackCount)
{
    // 载荷存非负列；草稿身份来自共享组归属，而非持久化 m_isDraft 标志。
    note.m_trackIndex += trackCount;
    note.m_isDraft = false;
    for ( auto& subNote : note.m_subNotes ) {
        subNote.trackIndex += trackCount;
    }
}

/// @brief 在持久化草稿轨道数量变化时保持既有负轨道坐标不变。
/// @param note 按旧宽度编码的物件。
/// @param oldTrackCount 原编码宽度。
/// @param newTrackCount 合并后采用的编码宽度。
/// @note 仅平移列，不修改 Flick 方向偏移或时间坐标。
/// @details 例如宽度从 4 增到 6 时，编码列 1 变为 3，可见列仍是 -3。
void rebasePersistedTracks(NoteComponent& note, int oldTrackCount,
                           int newTrackCount)
{
    // encoded - width 是可见负列，因此宽度增加多少，编码列也必须增加多少。
    const auto delta = newTrackCount - oldTrackCount;
    note.m_trackIndex += delta;
    for ( auto& subNote : note.m_subNotes ) {
        subNote.trackIndex += delta;
    }
}

/// @brief 计算持久化物件保持当前负轨道坐标所需的草稿轨道数量。
/// @param note 待测量的根物件及其子节点。
/// @param encodedTrackCount 当前载荷编码采用的宽度。
/// @return 覆盖最左起点及 Flick 终点所需的最小非负宽度。
/// @note 本函数负责左侧容量，不替代全部节点的轨道合法性检查。
int requiredTrackCountForPersistedNote(const NoteComponent& note,
                                       int                  encodedTrackCount)
{
    int required = 0;
    /// @brief 把一个节点的可见左边界并入宽度要求。
    /// @param type 决定是否还需计入 Flick 终点。
    /// @param track 持久化坐标中的节点起点。
    /// @param dtrack Flick 相对起点的方向偏移。
    const auto includePart = [&](::MMM::NoteType type, int track, int dtrack) {
        // encodedTrackCount - track 等于负列到玩家区边界的距离。
        required = std::max(required, encodedTrackCount - track);
        if ( type == ::MMM::NoteType::FLICK ) {
            // 向左的 Flick 终点可能比起点更远，单看起点会在缩轨后截断物件。
            required = std::max(required, encodedTrackCount - track - dtrack);
        }
    };
    includePart(note.m_type, note.m_trackIndex, note.m_dtrack);
    for ( const auto& subNote : note.m_subNotes ) {
        includePart(subNote.type, subNote.trackIndex, subNote.dtrack);
    }
    return required;
}

/// @brief 判断草稿物件的全部轨道是否适用于当前草稿轨数量。
/// @param note 使用非负持久化坐标的根物件。
/// @param trackCount 本次准备显示的草稿宽度。
/// @return 根节点、子节点及 Flick 终点均落在范围内时为真。
/// @details trackCount 为零时没有合法草稿列；不对越界节点夹取或删除部分子节点。
bool hasValidPersistedDraftTracks(const NoteComponent& note, int trackCount)
{
    /// @brief 验证一个节点的起点及必要的终点。
    /// @return 节点全部可见列都能映射到草稿区域时返回真。
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
    // 任一子节点越界就不显示整条物件，避免只保留折线的一部分。
    return std::all_of(
        note.m_subNotes.begin(),
        note.m_subNotes.end(),
        [&](const NoteComponent::SubNote& subNote) {
            return validPart(subNote.type, subNote.trackIndex, subNote.dtrack);
        });
}

/// @brief 将草稿条目编码为稳定的单条比较载荷。
/// @param item 使用同一轨道基准的条目。
/// @return 用于判定本地字段是否变更的完整协议文本。
/// @note 比较包含元数据、绑定等内容，不只比较位置和时刻。
/// @pre 输入已使用同一编码宽度，且本次处理期间其稳定身份不再变化。
std::string serializeDraftItem(const ClipboardItem& item)
{
    return EditorClipboardProtocol::serializeNotes(
        std::vector<ClipboardItem>{ item }, true);
}

/// @brief 编码草稿条目；空集合保持为空字符串。
/// @param items 已排序或需保持既有顺序的根条目。
/// @return 共享组载荷；此处不重新排列条目。
/// @note 空字符串与空草稿组配合使用，避免为无物件组生成仅含协议头的载荷。
std::string serializeDraftItems(const std::vector<ClipboardItem>& items)
{
    return items.empty() ? std::string{}
                         : EditorClipboardProtocol::serializeNotes(items, true);
}

/// @brief 解析草稿组载荷并丢弃非法的子实体条目。
/// @param payload 剪贴板协议形式的组载荷。
/// @return 根物件条目；空文本或解析失败返回空集合。
/// @note 这里不报告解析失败原因；空返回值也不能被解释为用户明确删除全部物件。
std::vector<ClipboardItem> parseDraftItems(std::string_view payload)
{
    if ( payload.empty() ) return {};
    const auto parsed = EditorClipboardProtocol::parse(payload, true);
    if ( !parsed ) return {};

    auto items = parsed->notes;
    // 子节点属于根物件的内嵌数据，独立子实体不能再次作为一个共享条目导入。
    std::erase_if(
        items, [](const ClipboardItem& item) { return item.note.m_isSubNote; });
    return items;
}

/// @brief 为持久化组补齐稳定 ID，使跨画布刷新可以保留实体身份。
/// @param group 要规范化的项目共享组。
/// @note 只在编码内容变化时推进运行时版本，不写磁盘配置。
/// @details
/// 会重编码解析结果，因此还会移除独立子实体等无法作为根条目保存的内容。
/// 调用方若已读取版本号，需要在规范化之后重新读取。
void canonicalizeGroupPayload(ProjectDraftLaneGroup& group)
{
    auto items = parseDraftItems(group.m_notePayload);
    for ( auto& item : items ) {
        // 身份补齐在载荷层完成，所有画布随后读取同一组稳定 ID。
        ensureNoteCollaborationIdentity(item.note);
    }
    const auto canonical = serializeDraftItems(items);
    // 避免每次读取都增加版本并触发其他会话无意义刷新。
    if ( canonical == group.m_notePayload ) return;
    group.m_notePayload = canonical;
    ++group.m_runtimeRevision;
}

/// @brief 按稳定 ID 将本会话相对基线的修改三方合并到最新共享载荷。
/// @param base 本会话上次应用的可见载荷。
/// @param local 本会话当前编辑后的载荷。
/// @param latest 最新项目载荷，按值接收并作为合并底稿。
/// @return 保留远端未冲突修改并应用本地增删改的条目。
/// @pre 三份输入已换算到同一草稿宽度，身份已规范化。
/// @note 同 ID 双方均修改时本地条目整体覆盖，不进行字段级合并。
/// @details 返回值以 latest
/// 的顺序为底稿，本地新增追加在后；调用方负责最终排序。
/// 身份空值不参与基线删除判断，正常载荷应在合并前补齐身份。
std::vector<ClipboardItem> mergeDraftItems(
    const std::vector<ClipboardItem>& base,
    const std::vector<ClipboardItem>& local, std::vector<ClipboardItem> latest)
{
    /// @brief 以逻辑身份查找条目，不依赖其数组位置。
    /// @param items 基线、本地或最新共享条目列表。
    /// @param identity 要定位的稳定物件 ID。
    /// @return 对应列表的迭代器，未找到时等于该列表末尾。
    const auto findById = [](auto& items, std::string_view identity) {
        return std::find_if(items.begin(), items.end(), [identity](auto& item) {
            return item.note.m_collaborationId == identity;
        });
    };

    // 只有基线曾有而本地已无的条目才是本地删除；不可删除本地从未见过的远端新增。
    for ( const auto& baseItem : base ) {
        const auto& identity = baseItem.note.m_collaborationId;
        // 基线仅代表本会话曾见的条目；latest 独有项不在这个删除循环中。
        if ( identity.empty() || findById(local, identity) != local.end() ) {
            continue;
        }
        std::erase_if(latest, [&identity](const ClipboardItem& item) {
            return item.note.m_collaborationId == identity;
        });
    }

    // 本地未变的条目沿用 latest，避免旧会话保存把其他画布的新内容覆盖回去。
    for ( const auto& localItem : local ) {
        const auto& identity = localItem.note.m_collaborationId;
        const auto  baseIt   = findById(base, identity);
        const bool  locallyChanged =
            baseIt == base.end() ||
            serializeDraftItem(*baseIt) != serializeDraftItem(localItem);
        // 不变条目即使已被其他画布删除，也不能由本次无关提交重新复活。
        if ( !locallyChanged ) continue;

        const auto latestIt = findById(latest, identity);
        if ( latestIt == latest.end() ) {
            // 本地新增或本地修改后远端已删除的条目，按本地提交意图重新加入。
            latest.push_back(localItem);
        } else {
            *latestIt = localItem;
        }
    }
    return latest;
}

/// @brief 判断当前会话是否正在直接编辑草稿物件。
/// @param ctx 待检查的会话交互状态。
/// @return 拖动、绘制或擦除正在涉及草稿时为真。
/// @note 用于推迟外部载荷应用，不阻塞线程或推迟本地交互反馈。
/// @details 不把普通玩家区交互视为草稿锁定，其他区域操作不必妨碍草稿刷新。
bool hasActiveDraftInteraction(const SessionContext& ctx)
{
    if ( ctx.isDragging ) {
        if ( ctx.draggedObjectKind == ChartObjectKind::DraftNote ) return true;
        /// @brief 从实体当前组件判断草稿身份，失效或无物件组件时不匹配。
        /// @param entity 拖动对象或被拖动固定的实体句柄。
        const auto isDraftEntity = [&](entt::entity entity) {
            const auto* note =
                ctx.noteRegistry.try_get<const NoteComponent>(entity);
            return note && note->m_isDraft;
        };
        // 组拖动的主实体未必是草稿，还需检查为拖动固定的其他实体。
        if ( isDraftEntity(ctx.draggedEntity) ||
             std::ranges::any_of(ctx.dragRenderPinnedEntities,
                                 isDraftEntity) ) {
            return true;
        }
    }
    // 新绘制物件可能尚未进入注册表，需从笔刷所在负轨道判断，而不能只查拖动实体。
    return (ctx.brushState.isActive && ctx.brushState.track < 0) ||
           (ctx.eraserState.isActive &&
            ctx.eraserState.targetObjectKind == ChartObjectKind::DraftNote);
}

/// @brief 确保草稿实体包含渲染和交互所需的轻量组件。
/// @param registry 当前会话的物件注册表。
/// @param entity 已创建或复用的有效实体。
/// @note 调用者另行置坐标脏标记；这里不立即计算几何，也不覆盖已有选择信息。
void ensureAuxiliaryComponents(entt::registry& registry, entt::entity entity)
{
    // 位置派生数据随新物件重算，但已有交互组件保留，避免刷新时丢失交互状态。
    registry.emplace_or_replace<TransformComponent>(entity);
    if ( !registry.all_of<InteractionComponent>(entity) ) {
        registry.emplace<InteractionComponent>(entity);
    }
}

/// @brief 以稳定逻辑 ID 合并草稿载荷，并保留仍存在实体的身份。
/// @param ctx 接收草稿实体的会话。
/// @param group 共享组；空指针表示清除本会话所有草稿实体。
/// @return 实际可见条目的持久化载荷，用作下一次三方合并基线。
/// @note 未显示的越界条目不计入基线，防止后续同步误认为用户删除它们。
/// @pre 草稿宽度已写入 ctx；调用期间会话注册表不可被其他线程同时修改。
/// @details 函数更新当前会话实体，不修改
/// group；共享组载荷是本次应用的只读输入。
std::string applyPayload(SessionContext&              ctx,
                         const ProjectDraftLaneGroup* group)
{
    /// @brief 使用负轨道坐标、将写入会话注册表的物件副本。
    std::vector<NoteComponent> desiredNotes;
    /// @brief 使用持久化坐标、将作为合并基线的可见根条目。
    std::vector<ClipboardItem> visibleItems;
    if ( group ) {
        // 先收集合法根物件，再触碰注册表，持久化副本与运行时负轨道副本分别保留。
        const auto items = parseDraftItems(group->m_notePayload);
        desiredNotes.reserve(items.size());
        visibleItems.reserve(items.size());
        for ( const auto& item : items ) {
            auto note = item.note;
            if ( !hasValidPersistedDraftTracks(note, ctx.draftTrackCount) ) {
                // 不改变共享数据，只从本会话显示集合剔除超出其草稿宽度的条目。
                continue;
            }
            ensureNoteCollaborationIdentity(note);
            // 基线保留编码坐标；后面的 ECS 副本才转换为画布所需负坐标。
            visibleItems.push_back(ClipboardItem{ .note = note });
            convertPersistedTracksToDraft(note, ctx.draftTrackCount);
            desiredNotes.push_back(std::move(note));
        }
    }

    // 仅索引草稿实体，不能让同名身份误替换正式谱面中的玩家物件。
    std::unordered_map<std::string, entt::entity> existingById;
    std::vector<entt::entity>                     existingDraftEntities;
    // 在创建新实体前收集旧句柄，避免边枚举边改变注册表影响清理边界。
    const auto view = ctx.noteRegistry.view<const NoteComponent>();
    for ( const auto entity : view ) {
        const auto& note = view.get<const NoteComponent>(entity);
        if ( !note.m_isDraft ) continue;
        existingDraftEntities.push_back(entity);
        if ( !note.m_collaborationId.empty() ) {
            // 重复身份只把首次实体作为复用候选，其余旧实体由清理阶段回收。
            existingById.try_emplace(note.m_collaborationId, entity);
        }
    }

    // retained 同时防止重复身份复用同一实体，并标识最终需要保留的旧实体。
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
        // 父实体先登记为保留项，再处理子节点，使每个句柄只承担一个对象。
        retained.insert(entity);
        // 复用句柄但完整替换物件值，使元数据、绑定和自定义颜色也随载荷刷新。
        ctx.noteRegistry.emplace_or_replace<NoteComponent>(entity, note);
        ensureAuxiliaryComponents(ctx.noteRegistry, entity);

        // 根载荷内嵌子节点，ECS 仍需独立子实体用于绘制和拾取。
        // 重建父句柄及序号，不能沿用其他会话的实体句柄。
        for ( std::size_t index = 0; index < note.m_subNotes.size(); ++index ) {
            const auto&   subNote = note.m_subNotes[index];
            NoteComponent child;
            child.m_type           = subNote.type;
            child.m_timestamp      = subNote.timestamp;
            child.m_duration       = subNote.duration;
            child.m_trackIndex     = subNote.trackIndex;
            child.m_dtrack         = subNote.dtrack;
            child.m_isSubNote      = true;
            child.m_isDraft        = true;
            child.m_parentPolyline = entity;
            child.m_subIndex       = static_cast<int>(index);
            // 子实体不仅用于几何拾取，属性编辑也读取它的注释、采样和配色副本。
            child.m_metadata        = subNote.metadata;
            child.m_annotation      = subNote.annotation;
            child.m_sampleBinding   = subNote.sampleBinding;
            child.m_customColors    = subNote.customColors;
            child.m_collaborationId = subNote.collaborationId;

            entt::entity childEntity = entt::null;
            // 子节点也按稳定身份复用，刷新同一折线时可保留其实体级交互关联。
            const auto existingChild =
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

    // 新载荷完整应用后才清理遗留实体，并先从选择集中移除失效句柄。
    for ( const auto entity : existingDraftEntities ) {
        if ( retained.contains(entity) || !ctx.noteRegistry.valid(entity) ) {
            continue;
        }
        ctx.selectedNoteEntities.erase(entity);
        ctx.noteRegistry.destroy(entity);
    }

    // 悬浮目标若随删除消失，重置种类和句柄；有效目标则继续保留。
    if ( ctx.hoveredObjectKind == ChartObjectKind::DraftNote &&
         (ctx.hoveredEntity == entt::null ||
          !ctx.noteRegistry.valid(ctx.hoveredEntity)) ) {
        ctx.hoveredEntity     = entt::null;
        ctx.hoveredObjectKind = ChartObjectKind::PlayerNote;
    }
    // 只发出派生缓存脏标记，让排序、坐标、预览和击打事件在各自流程刷新。
    // 草稿的遗留实体已在本函数清理，无需再请求一次剪枝。
    ctx.isNoteOrderDirty = true;
    ctx.isNotePruneDirty = false;
    // 根和子实体的 Transform 均需重算，不能沿用旧载荷的位置缓存。
    ctx.isTransformDirty = true;
    // 草稿增删也改变概览内容，预览密度不能只等待正式谱面物件变化。
    ctx.isPreviewDensityDirty = true;
    // 采样绑定可能随合并改变，击打事件缓存必须与新物件数据同步失效。
    ctx.isHitEventsDirty = true;
    // 返回实际应用的持久化快照，而非从 ECS 再收集，避免把子实体重复编码进基线。
    return serializeDraftItems(visibleItems);
}

}  // namespace

/// @brief 为载入谱面的会话选择共享组并建立草稿基线。
/// @param ctx 已设置当前谱面及玩家轨道数的会话。
/// @param project 对应项目；空指针时仅清除旧草稿状态。
/// @warning 低频加载入口，解析完整载荷并更新 ECS，不逐帧调用。
/// @details 无可解析音频或无有效轨道时清空草稿实体；不会为这种会话创建共享组。
void ProjectDraftLaneService::load(SessionContext& ctx, Project* project)
{
    // 切换谱面时丢弃旧组身份和基线，不能把前一首歌的草稿当成本次本地编辑。
    ctx.m_draftLaneGroupId.clear();
    ctx.m_draftLaneGroupRevision = 0U;
    ctx.m_draftLaneBasePayload.clear();
    ctx.draftTrackCount           = std::max(0, ctx.trackCount);
    ctx.m_draftLaneBaseTrackCount = ctx.draftTrackCount;
    // 初始化宽度也覆盖无项目分支，避免遗留前一个会话的负轨道范围。
    if ( !project || !ctx.currentBeatmap || ctx.trackCount <= 0 ) {
        static_cast<void>(applyPayload(ctx, nullptr));
        return;
    }

    // 共享键采用默认音频资源 ID；不同谱面指向同一音频时共享同一份草稿。
    const auto* resource =
        ProjectResourceService::findDefaultBeatmapAudioResource(
            *project,
            *ctx.currentBeatmap,
            ctx.currentBeatmap->m_baseMapMetadata.map_path);
    // 文件路径可作为显示提示，但没有资源身份就无法稳定选择项目共享组。
    if ( !resource || resource->m_id.empty() ) {
        static_cast<void>(applyPayload(ctx, nullptr));
        return;
    }

    ctx.m_draftLaneGroupId = resource->m_id;
    auto* group            = findGroup(*project, ctx.m_draftLaneGroupId);
    if ( group ) {
        // 旧配置未保存宽度时沿用当前玩家键数，已有宽度则保持跨谱面一致。
        canonicalizeGroupPayload(*group);
        ctx.draftTrackCount =
            group->m_trackCount > 0 ? group->m_trackCount : ctx.trackCount;
    }
    // 组不存在时仍应用空载荷清除旧实体，首次编辑后才由 sync 创建组。
    ctx.m_draftLaneBasePayload    = applyPayload(ctx, group);
    ctx.m_draftLaneBaseTrackCount = ctx.draftTrackCount;
    ctx.m_draftLaneGroupRevision  = group ? group->m_runtimeRevision : 0U;
}

/// @brief 共享组版本变化后刷新本会话可见草稿。
/// @param ctx 要与项目组对齐的会话。
/// @warning 每 update 检查组版本；无变化时不解析载荷或遍历实体。
/// 交互期间保留旧基线，结束后由版本差异再次触发刷新。
/// @note 版本是项目内的运行时通知，不是磁盘修订号或网络协议序号。
void ProjectDraftLaneService::refreshIfChanged(SessionContext& ctx)
{
    if ( ctx.m_draftLaneGroupId.empty() || !ctx.currentBeatmap ) return;
    auto* project = resolveProject(ctx);
    if ( !project ) return;
    auto* group = findGroup(*project, ctx.m_draftLaneGroupId);
    // 组消失时以零版本和空载荷刷新，使旧草稿不继续滞留在会话中。
    auto revision = group ? group->m_runtimeRevision : 0U;
    // 同版本快路不收集 ECS、不排序、不读取磁盘。
    if ( revision == ctx.m_draftLaneGroupRevision ) return;
    // 暂不更新已见版本，让下一轮仍能发现变化；不能在拖动中替换被操作的实体。
    if ( hasActiveDraftInteraction(ctx) ) return;
    if ( group ) {
        canonicalizeGroupPayload(*group);
        // 规范化可能补身份并增加版本，基线必须记住规范化后的版本。
        revision = group->m_runtimeRevision;
    }

    ctx.draftTrackCount =
        group && group->m_trackCount > 0 ? group->m_trackCount : ctx.trackCount;
    ctx.m_draftLaneBasePayload    = applyPayload(ctx, group);
    ctx.m_draftLaneBaseTrackCount = ctx.draftTrackCount;
    ctx.m_draftLaneGroupRevision  = revision;
}

/// @brief 将本会话草稿改动合并到共享组并刷新本地基线。
/// @param ctx 持有当前 ECS 草稿和上次共享快照的会话。
/// @warning 脏标记置位后完整收集和排序草稿；不用于无条件逐帧重建。
/// @note 只更新内存项目和运行时版本，磁盘保存由项目持久化流程负责。
/// @pre 会话的基线载荷与基线宽度来自同一次应用，不可独立重置其中一个。
/// @details
/// 合并前保留不可见远端条目，合并后必要时扩展宽度，使这些条目能够重新显示。
void ProjectDraftLaneService::sync(SessionContext& ctx)
{
    if ( !ctx.m_needsDraftNotesSync ) return;
    // 本轮消费请求；后续无共享组等情况不会让同一个无效请求每轮重复执行。
    ctx.m_needsDraftNotesSync = false;
    if ( ctx.m_draftLaneGroupId.empty() || ctx.trackCount <= 0 ) return;

    auto* project = resolveProject(ctx);
    if ( !project ) return;

    std::vector<ClipboardItem> items;
    auto                       view = ctx.noteRegistry.view<NoteComponent>();
    // view 包含正式物件和子实体，这只是容量上界，实际载荷由草稿过滤决定。
    items.reserve(view.size());
    for ( const auto entity : view ) {
        auto& source = view.get<NoteComponent>(entity);
        // 根物件已含完整子节点数据，独立子实体不可重复序列化。
        if ( source.m_isSubNote || !source.m_isDraft ) continue;
        // 补齐源组件身份后再复制，后续编辑和这次共享载荷使用同一个逻辑对象 ID。
        ensureNoteCollaborationIdentity(source);
        ClipboardItem item;
        // 复制完整属性后只转换副本坐标，不让画布实体立即跳回非负列。
        item.note = source;
        convertDraftTracksToPersisted(item.note, ctx.draftTrackCount);
        items.push_back(std::move(item));
    }
    // 固定输出顺序，减少注册表枚举次序变化导致的无意义载荷差异。
    std::stable_sort(
        items.begin(), items.end(), [](const auto& lhs, const auto& rhs) {
            if ( lhs.note.m_timestamp != rhs.note.m_timestamp ) {
                return lhs.note.m_timestamp < rhs.note.m_timestamp;
            }
            return lhs.note.m_trackIndex < rhs.note.m_trackIndex;
        });

    auto* group = findGroup(*project, ctx.m_draftLaneGroupId);
    if ( !group ) {
        // 仅真正有同步请求时创建组，载入空草稿不会提前增加项目配置条目。
        project->m_draftLaneGroups.push_back(ProjectDraftLaneGroup{
            .m_mainAudioResourceId = ctx.m_draftLaneGroupId,
        });
        // push_back 后重新取地址，不保留容器扩容前的观察指针。
        group = &project->m_draftLaneGroups.back();
    } else {
        canonicalizeGroupPayload(*group);
    }
    /// @brief 最新共享载荷的编码宽度；旧项目零值沿用当前玩家键数。
    const auto latestTrackCount =
        group->m_trackCount > 0 ? group->m_trackCount : ctx.trackCount;
    // 比较基线宽度区分用户主动改宽与共享组外部改宽，不能只比较两端当前值。
    const bool localTrackCountChanged =
        ctx.draftTrackCount != ctx.m_draftLaneBaseTrackCount;
    /// @brief 编辑期间是否有其他提交或身份规范化改变共享版本。
    const bool concurrentGroupChange =
        group->m_runtimeRevision != ctx.m_draftLaneGroupRevision;
    // 本地没改宽度时跟随最新共享宽度；双方并发改宽度时取较大值，避免截断。
    // 只有本地单独改宽度时允许采用本地缩轨结果。
    const auto targetTrackCount =
        std::max(0,
                 localTrackCountChanged
                     ? (concurrentGroupChange
                            ? std::max(ctx.draftTrackCount, latestTrackCount)
                            : ctx.draftTrackCount)
                     : latestTrackCount);

    // 三方内容必须先换算到相同编码原点，否则单纯扩轨会被误判成物件横向移动。
    if ( ctx.draftTrackCount != targetTrackCount ) {
        for ( auto& item : items ) {
            rebasePersistedTracks(
                item.note, ctx.draftTrackCount, targetTrackCount);
        }
    }
    // 基线使用上次应用的宽度，不能误用本地编辑后宽度去解释旧编码列。
    auto base = parseDraftItems(ctx.m_draftLaneBasePayload);
    if ( ctx.m_draftLaneBaseTrackCount != targetTrackCount ) {
        for ( auto& item : base ) {
            rebasePersistedTracks(
                item.note, ctx.m_draftLaneBaseTrackCount, targetTrackCount);
        }
    }
    // 最新组载荷有自己的编码宽度，独立换算后才能与本地和基线比较。
    auto latest = parseDraftItems(group->m_notePayload);
    if ( latestTrackCount != targetTrackCount ) {
        for ( auto& item : latest ) {
            rebasePersistedTracks(
                item.note, latestTrackCount, targetTrackCount);
        }
    }
    // 轨道基准对齐后再比较条目字段，保留其他画布相对于本地基线的独立修改。
    auto merged = mergeDraftItems(base, items, std::move(latest));
    /// @brief 容纳并发保留物件的最终宽度，至少保持已选目标宽度。
    auto mergedTrackCount = targetTrackCount;
    // 容量按所有合并结果计算，而不是只看本地可见项，保留并发新增的左侧物件。
    for ( const auto& item : merged ) {
        mergedTrackCount = std::max(
            mergedTrackCount,
            requiredTrackCountForPersistedNote(item.note, targetTrackCount));
    }
    // 合并可能保留缩轨范围外的远端物件，扩容后再次平移编码列，维持其可见位置。
    if ( mergedTrackCount != targetTrackCount ) {
        for ( auto& item : merged ) {
            rebasePersistedTracks(
                item.note, targetTrackCount, mergedTrackCount);
        }
    }
    std::stable_sort(
        merged.begin(), merged.end(), [](const auto& lhs, const auto& rhs) {
            if ( lhs.note.m_timestamp != rhs.note.m_timestamp ) {
                return lhs.note.m_timestamp < rhs.note.m_timestamp;
            }
            return lhs.note.m_trackIndex < rhs.note.m_trackIndex;
        });
    // 一次性发布组载荷、宽度和版本，再用合并结果刷新发起会话，建立下一次基线。
    group->m_notePayload = serializeDraftItems(merged);
    group->m_trackCount  = mergedTrackCount;
    // 同步请求即使产出相同文本也推进版本，让其他会话按统一发布机制重新对齐。
    ++group->m_runtimeRevision;
    ctx.draftTrackCount           = group->m_trackCount;
    ctx.m_draftLaneBasePayload    = applyPayload(ctx, group);
    ctx.m_draftLaneBaseTrackCount = ctx.draftTrackCount;
    ctx.m_draftLaneGroupRevision  = group->m_runtimeRevision;
}

}  // namespace MMM::Logic
