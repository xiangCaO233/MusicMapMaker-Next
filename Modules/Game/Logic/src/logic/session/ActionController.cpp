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

// 动作记录保留前后值，实体组件是执行时的权威状态；两者不能在协作更新后混为一谈。
// 本文件负责应用记录并标记派生数据失效，不在每个动作内执行完整缓存重建。
namespace
{
/// @brief 确保音符实体拥有更新所需的辅助组件，并保留已有交互状态。
/// @param reg 音符所属注册表。
/// @param entity 已存在的目标实体。
/// @pre entity 有效，调用期间注册表不被其他线程修改。
/// @note 只补齐辅助组件，不创建实体，也不补写 NoteComponent。
/// @warning 可能向 ECS
/// 组件池分配存储，应在编辑动作执行阶段调用，不用于逐帧补齐。
void ensureNoteAuxiliaryComponents(entt::registry& reg, entt::entity entity)
{
    // 只补缺失组件，不覆盖已有变换或选择、拖动状态。
    if ( !reg.all_of<TransformComponent>(entity) ) {
        reg.emplace<TransformComponent>(entity);
    }
    if ( !reg.all_of<InteractionComponent>(entity) ) {
        reg.emplace<InteractionComponent>(entity);
    }
}

/// @brief 标记音符创建/更新后需要完整重建排序缓存。
/// @param ctx 接收排序、统计与批注显示失效标记的会话。
/// @note 脏标记仅合并置位，不能在这里清除前一个动作提出的更新需求。
void markNoteOrderDirty(SessionContext& ctx)
{
    // 音符时间或内容改变可同时影响显示顺序、统计及绑定批注，需一起失效。
    ctx.isNoteOrderDirty             = true;
    ctx.isNoteStatsDirty             = true;
    ctx.isAnnotationRenderCacheDirty = true;
}

/// @brief 标记音符删除后只需从排序缓存中剔除失效实体。
/// @param ctx 需要清除失效音符引用的会话。
/// @note 若会话已经要求完整排序，该标记不撤销更强的完整更新请求。
void markNotePruneDirty(SessionContext& ctx)
{
    // 删除不打乱剩余元素的时间顺序，保留剔除标记与完整重排标记的区别。
    ctx.isNotePruneDirty             = true;
    ctx.isNoteStatsDirty             = true;
    ctx.isAnnotationRenderCacheDirty = true;
}

/// @brief 尝试把单个音符动作直接合并进已构建缓存。
/// @param ctx 持有派生缓存的会话。
/// @param entity 本次变更的实体身份。
/// @param before 变更前值，创建动作为空。
/// @param after 变更后值，删除动作为空。
/// @return 缓存已经更新或动作未实际改变实体时返回 true。
/// @note 失败时由调用方标记完整重建，不在此重复实现缓存更新算法。
/// @pre before/after 描述本次实际写入的状态，不是尚未通过身份检查的动作意图。
/// @note 空前后值表示无需更新，返回 true 不代表实体必然存在。
/// @note span 和 mutation 都是栈上借用对象，增量入口不得异步保留其地址。
bool applySingleNoteCacheMutation(SessionContext& ctx, entt::entity entity,
                                  const std::optional<NoteComponent>& before,
                                  const std::optional<NoteComponent>& after)
{
    if ( !before && !after ) return true;
    // 视图仅借用两个 optional 的内容，在本次同步调用内立即消费。
    const SessionUtils::NoteCacheMutationView mutation{
        .entity = entity,
        .before = before ? &*before : nullptr,
        .after  = after ? &*after : nullptr,
    };
    return SessionUtils::applyNoteCacheMutationsIncrementally(
        ctx,
        std::span<const SessionUtils::NoteCacheMutationView>(&mutation, 1U));
}

/// @brief 标记一次 Note 动作实际涉及的正式谱面与项目草稿数据域。
/// @param ctx 待累积持久化脏标记的会话。
/// @param before 变更前音符，可为空。
/// @param after 变更后音符，可为空。
/// @return 动作是否涉及正式谱面物件。
/// @note 返回值仅描述正式数据域，草稿脏标记通过 ctx 单独传出。
bool markNoteStorageDirty(SessionContext&                     ctx,
                          const std::optional<NoteComponent>& before,
                          const std::optional<NoteComponent>& after)
{
    // 跨正式区和草稿区移动时，两侧数据域都要保存，不能只看变更后的类型。
    const bool touchesDraft =
        (before && before->m_isDraft) || (after && after->m_isDraft);
    const bool touchesFormal =
        (before && !before->m_isDraft) || (after && !after->m_isDraft);
    ctx.m_needsDraftNotesSync = ctx.m_needsDraftNotesSync || touchesDraft;
    // 合并而非覆盖脏标记，保留同一批中先前动作留下的待同步状态。
    ctx.m_needsNotesSync = ctx.m_needsNotesSync || touchesFormal;
    return touchesFormal;
}

/// @brief 标记批量 Note 动作实际涉及的数据域。
/// @param ctx 接收整批持久化标记的会话。
/// @param entries 批量动作记录，允许混合创建、修改和删除。
/// @return 动作是否涉及正式谱面物件。
/// @note 空批次返回 false，且不清除 ctx 上已存在的任何脏标记。
bool markBatchNoteStorageDirty(
    SessionContext& ctx, const std::vector<BatchNoteAction::Entry>& entries)
{
    bool touchesFormal = false;
    for ( const auto& entry : entries ) {
        // 标记函数放在或运算左侧，已有正式改动也不能跳过后续草稿项的副作用。
        touchesFormal = markNoteStorageDirty(ctx, entry.before, entry.after) ||
                        touchesFormal;
    }
    return touchesFormal;
}

/// @brief 计算完整容纳草稿物件所需的持久化草稿轨道数量。
/// @param note 根音符及其折线子段。
/// @return 非草稿为零，草稿取涉及的最远负轨道编号绝对值。
/// @note 只计算该物件的需求，不代表会话中其他物件所需的总轨道数。
/// @pre 轨道及跨度来自有效编辑范围，取负及相加不能溢出 int32。
/// @warning 编辑动作低频路径：仅遍历单个物件自身的 Polyline 子段。
std::int32_t requiredDraftTrackCount(const NoteComponent& note)
{
    if ( !note.m_isDraft ) return 0;
    // 初值为零，位于非负轨道的部分不会把所需草稿数量减成负值。
    std::int32_t required = 0;
    /// @brief 将单段起点与滑键尾轨合并到当前草稿容量需求。
    /// @param type 当前段的类型，仅滑键需要额外检查横向跨度。
    /// @param track 当前段起始轨道，草稿采用负数编号。
    /// @param dtrack 滑键相对起点的轨道位移。
    /// @note 捕获的 required 保存最大需求，不累计各段宽度。
    const auto includePart =
        [&](::MMM::NoteType type, std::int32_t track, std::int32_t dtrack) {
            // 草稿轨道向负方向延伸，Flick 尾部可能比起点更靠外。
            required = std::max(required, -track);
            if ( type == ::MMM::NoteType::FLICK ) {
                required = std::max(required, -(track + dtrack));
            }
        };
    includePart(note.m_type, note.m_trackIndex, note.m_dtrack);
    // 根与子段一并参与边界计算，不能只用折线头部决定轨道容量。
    for ( const auto& subNote : note.m_subNotes ) {
        includePart(subNote.type, subNote.trackIndex, subNote.dtrack);
    }
    return required;
}

/// @brief 应用动作记录的草稿轨道数量并标记共享项目数据脏。
/// @param ctx 需要恢复轨道数的会话。
/// @param count 动作快照中的轨道数，负值规整为零。
/// @note 轨道数从动作记录恢复；这里不扫描全谱重新推导，避免改写撤销语义。
/// @pre 调用方已完成布局冲突决策，本函数不验证缩小轨数后现有物件是否仍可容纳。
void applyDraftTrackCount(SessionContext& ctx, std::int32_t count)
{
    const auto normalized = std::max(0, count);
    if ( normalized == ctx.draftTrackCount ) return;
    // 相同数量不重复置位变换标记，重做或空动作不会凭空触发布局更新。
    // 轨道数变化同时影响持久化与几何布局，不必触发音符时间排序。
    ctx.draftTrackCount       = normalized;
    ctx.m_needsDraftNotesSync = true;
    ctx.isTransformDirty      = true;
}

/// @brief 判断两个可选音符颜色是否相同。
/// @param lhs 当前或预期的颜色覆写。
/// @param rhs 待比较的颜色覆写。
/// @return 存在性一致且四通道精确相等时为 true。
/// @note 这是动作值匹配，不是视觉近似比较，不能用误差容忍吞掉实际编辑。
bool sameOptionalNoteColor(const std::optional<glm::vec4>& lhs,
                           const std::optional<glm::vec4>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    // 未覆写和显式设置为皮肤当前颜色仍是不同状态，不能只比较最终显示颜色。
    return !lhs || (lhs->r == rhs->r && lhs->g == rhs->g && lhs->b == rhs->b &&
                    lhs->a == rhs->a);
}

/// @brief 判断两组音符颜色覆写是否相同。
/// @param lhs 第一组按部位保存的颜色覆写。
/// @param rhs 第二组颜色覆写。
/// @return 全部颜色槽的存在性与数值均相等时返回 true。
/// @note 颜色组整体参与选择性回退，任一槽变化都会阻止整组被旧动作覆盖。
bool sameNoteColors(const NoteColorOverrides& lhs,
                    const NoteColorOverrides& rhs)
{
    // 暂未用于当前类型的颜色槽也需比较，防止切换物件类型后丢失已保存覆写。
    return sameOptionalNoteColor(lhs.tap, rhs.tap) &&
           sameOptionalNoteColor(lhs.head, rhs.head) &&
           sameOptionalNoteColor(lhs.hold, rhs.hold) &&
           sameOptionalNoteColor(lhs.end, rhs.end) &&
           sameOptionalNoteColor(lhs.flickArrow, rhs.flickArrow) &&
           sameOptionalNoteColor(lhs.node, rhs.node);
}

/// @brief 判断两个可选音符采样绑定是否相同。
/// @param lhs 第一份可选绑定。
/// @param rhs 第二份可选绑定。
/// @return 绑定存在性、资源身份和事件音量均一致时返回 true。
/// @note 同一资源但音量不同仍是不同绑定，空绑定也不等价于零音量。
bool sameNoteBinding(const std::optional<::MMM::AudioSampleBinding>& lhs,
                     const std::optional<::MMM::AudioSampleBinding>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    return !lhs || (lhs->m_audioResourceId == rhs->m_audioResourceId &&
                    // 按存储的资源 ID 比较，不解析路径或访问音频资源管理器。
                    lhs->m_volume == rhs->m_volume);
}

/// @brief 判断两个折线子物件是否完全相同。
/// @param lhs 第一份子物件值。
/// @param rhs 第二份子物件值。
/// @return 此处参与动作回退的所有字段均相等时返回 true。
/// @note 同时比较几何、格式元数据、音色及协作身份，不仅比较屏幕位置。
bool sameSubNote(const NoteComponent::SubNote& lhs,
                 const NoteComponent::SubNote& rhs)
{
    // 路径中的逻辑 ID 同样参与比较，位置相同的新节点不能冒充历史节点。
    return lhs.type == rhs.type && lhs.timestamp == rhs.timestamp &&
           lhs.duration == rhs.duration && lhs.trackIndex == rhs.trackIndex &&
           lhs.dtrack == rhs.dtrack &&
           lhs.metadata.note_properties == rhs.metadata.note_properties &&
           lhs.annotation == rhs.annotation &&
           sameNoteBinding(lhs.sampleBinding, rhs.sampleBinding) &&
           sameNoteColors(lhs.customColors, rhs.customColors) &&
           lhs.collaborationId == rhs.collaborationId;
}

/// @brief 判断两组折线子物件是否完全相同。
/// @param lhs 第一条折线路径的有序子段。
/// @param rhs 第二条折线路径的有序子段。
/// @return 节点数一致且各对应节点相等时返回 true。
/// @note 子物件顺序属于折线路径语义，不能通过排序或集合比较忽略顺序。
bool sameSubNotes(const std::vector<NoteComponent::SubNote>& lhs,
                  const std::vector<NoteComponent::SubNote>& rhs)
{
    // 先检查长度再逐项比较，避免第二个序列较短时越界访问。
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), sameSubNote);
}

/// @brief 仅当当前字段仍等于本动作的结果时应用反向或正向值。
/// @tparam Value 可复制赋值的字段值类型。
/// @tparam Equal 对该字段定义相等语义的比较器类型。
/// @param current 当前权威字段，满足预期时才修改。
/// @param expected 动作在当前执行方向预期的旧值。
/// @param replacement 希望写入的新值。
/// @param equal 比较器，不修改输入字段。
/// @note 这是值条件合并，不是带版本号的冲突解决；值相同的后续编辑无法区分来源。
/// @note 不满足条件时静默保留当前值，由上层继续处理同一动作中的其他字段。
template<typename Value, typename Equal>
void applyNoteFieldTransition(Value& current, const Value& expected,
                              const Value& replacement, Equal equal)
{
    // 例如动作把时间从 A 改为 B，撤销时只接受当前仍为 B，不覆盖后来改成的 C。
    // 正向重做只需交换 expected 与 replacement，保持同一套冲突判断。
    if ( !equal(expected, replacement) && equal(current, expected) ) {
        // 当前值被后续操作改过时不强行覆盖，无变化字段也不制造一次赋值。
        current = replacement;
    }
}

/// @brief 在 Undo/Redo 时按字段合并音符，避免覆盖其他协作者的后续修改。
/// @param current 当前权威音符。
/// @param expected 本动作在该方向执行前预期看到的值。
/// @param replacement 本动作在该方向希望恢复的值。
/// @note 只合并此处列举的业务字段，实体及协作身份由专门流程维护。
/// @pre current、expected、replacement 在本次同步合并期间没有外部并发写入。
/// @note 每个标量字段独立判断，冲突不导致整条音符回滚，也不会强制覆盖当前值。
/// @note 颜色组、元数据表和子节点序列分别作为整体字段，不在内部进行二次合并。
void applySelectiveNoteTransition(NoteComponent&       current,
                                  const NoteComponent& expected,
                                  const NoteComponent& replacement)
{
    /// @brief 比较普通字段的存储值，供条件合并统一调用。
    /// @param lhs 当前值或预期值。
    /// @param rhs 要比较的预期值或目标值。
    /// @return 类型自身定义的相等结果。
    const auto equal = [](const auto& lhs, const auto& rhs) {
        // 使用字段自身的值相等运算，不执行显示层归一化。
        return lhs == rhs;
    };
    // 类型与时间等标量按各自前后值匹配，保留未被本动作修改的当前字段。
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
    applyNoteFieldTransition(
        current.m_isDraft, expected.m_isDraft, replacement.m_isDraft, equal);
    // 元数据表按整体比较，不逐键合并并发修改。
    applyNoteFieldTransition(current.m_metadata.note_properties,
                             expected.m_metadata.note_properties,
                             replacement.m_metadata.note_properties,
                             equal);
    applyNoteFieldTransition(current.m_annotation,
                             expected.m_annotation,
                             replacement.m_annotation,
                             equal);
    applyNoteFieldTransition(current.m_sampleBinding,
                             expected.m_sampleBinding,
                             replacement.m_sampleBinding,
                             sameNoteBinding);
    // 绑定按资源和音量整体恢复，不把旧资源与后来编辑的音量组合成新绑定。
    applyNoteFieldTransition(current.m_customColors,
                             expected.m_customColors,
                             replacement.m_customColors,
                             sameNoteColors);
    // 子节点序列整体回退，避免把两代折线路径拼成混合结构。
    applyNoteFieldTransition(current.m_subNotes,
                             expected.m_subNotes,
                             replacement.m_subNotes,
                             sameSubNotes);
}

/// @brief 将当前实体的逻辑标识传播到动作快照。
/// @param current 当前有效实体的音符状态。
/// @param snapshot 待更新身份的旧值或新值快照；为空时不创建。
/// @note 只修正逻辑标识，不把当前几何、颜色或批注覆盖进历史值。
/// @note 子节点按共同索引继承身份，不根据时间或轨道猜测节点对应关系。
/// @pre current 的身份已由调用者补齐，本函数只传播，不生成新的 ID。
void inheritNoteIdentity(const NoteComponent&          current,
                         std::optional<NoteComponent>& snapshot)
{
    if ( !snapshot ) return;
    snapshot->m_collaborationId = current.m_collaborationId;
    const auto count =
        std::min(snapshot->m_subNotes.size(), current.m_subNotes.size());
    // 只传播双方共有索引，不改变动作记录的子节点数量或补出新节点。
    // 身份继承不修改根实体引用，运行期 entt 编号仍由创建或恢复流程维护。
    // 历史路径可能比当前路径长，超出共同部分的历史节点保留各自已有身份。
    for ( std::size_t index = 0; index < count; ++index ) {
        snapshot->m_subNotes[index].collaborationId =
            current.m_subNotes[index].collaborationId;
    }
}

/// @brief 判断实体是否仍表示动作记录中的同一逻辑音符。
/// @param reg 执行动作的注册表。
/// @param entity 动作记录的运行期实体标识。
/// @param snapshot 用于核验稳定逻辑身份的历史值。
/// @return 实体有效、组件存在且协作标识符合记录时为 true。
/// @note 旧动作未记录协作 ID 时保留兼容行为，仅验证实体与组件有效性。
bool isActionNoteEntity(const entt::registry& reg, entt::entity entity,
                        const NoteComponent& snapshot)
{
    // 先验证注册表访问前提，再读取 ID；旧记录指向已销毁实体是正常的跳过条件。
    if ( !reg.valid(entity) || !reg.all_of<NoteComponent>(entity) ) {
        return false;
    }
    return snapshot.m_collaborationId.empty() ||
           // 非空稳定 ID 防止实体槽位复用后把旧动作应用到另一个逻辑音符。
           reg.get<const NoteComponent>(entity).m_collaborationId ==
               snapshot.m_collaborationId;
}

/// @brief 为首次执行的批量音符动作补齐并关联根物件、子物件逻辑标识。
/// @param reg 当前音符注册表，用于核对现存实体身份。
/// @param entries 可写动作记录，补齐身份后供首次执行和后续重放共用。
/// @note 先处理根物件再处理独立子实体，避免条目排列顺序影响父子身份关联。
/// @note 此处修改动作快照中的 ID，后续重做必须沿用这些已确定的身份。
/// @warning
/// 父条目查找会遍历本批记录，仅在批量动作准备阶段调用，不能逐帧重复执行。
void prepareBatchNoteIdentities(entt::registry&                      reg,
                                std::vector<BatchNoteAction::Entry>& entries)
{
    for ( auto& entry : entries ) {
        if ( entry.before &&
             isActionNoteEntity(reg, entry.entity, *entry.before) ) {
            auto& current = reg.get<NoteComponent>(entry.entity);
            ensureNoteCollaborationIdentity(current);
            inheritNoteIdentity(current, entry.before);
            // 前后快照沿用同一逻辑身份，更新不能被当作创建另一个音符。
            inheritNoteIdentity(current, entry.after);
        } else if ( !entry.before && entry.after &&
                    !entry.after->m_isSubNote ) {
            // 新根物件没有现存实体可借用身份，直接在创建快照上准备稳定 ID。
            ensureNoteCollaborationIdentity(*entry.after);
        }
    }

    for ( auto& entry : entries ) {
        // 第二遍只补新建子实体，既有实体的身份已在第一遍处理。
        // 子实体的父引用用于关联本批记录，不通过遍历整个注册表寻找相似根节点。
        if ( entry.before || !entry.after || !entry.after->m_isSubNote ) {
            continue;
        }
        // 父子通过动作内实体引用关联，不假设父条目恰好排在子条目前一项。
        const auto parent = std::find_if(
            entries.begin(), entries.end(), [&](const auto& candidate) {
                return candidate.entity == entry.after->m_parentPolyline &&
                       candidate.after && !candidate.after->m_isSubNote;
            });
        if ( parent != entries.end() && entry.after->m_subIndex >= 0 &&
             static_cast<std::size_t>(entry.after->m_subIndex) <
                 parent->after->m_subNotes.size() ) {
            // 独立子实体和根组件内的同一子段共用
            // ID，防止协作同步把它们视为两项。
            entry.after->m_collaborationId =
                parent->after
                    ->m_subNotes[static_cast<std::size_t>(
                        entry.after->m_subIndex)]
                    .collaborationId;
        } else {
            // 缺父条目或子索引无效时仍生成身份，避免新实体携带空 ID。
            ensureNoteCollaborationIdentity(*entry.after);
        }
    }
}
}  // namespace

// --- TimelineAction 实现 ---

/// @brief 按创建、删除或更新类型应用单个时间点动作。
/// @param ctx 接收时间点与派生状态失效标记的会话。
/// @pre 创建具有 after，删除具有 before，更新同时具有二者。
/// @note 更新使用完整组件替换，不采用音符动作的选择性字段回退。
/// @note 时间点沿用实体有效性检查，不具备音符稳定 ID 的额外身份校验。
/// @pre 更新目标若有效则须带有 TimelineComponent，valid 检查不代替组件检查。
/// @warning 编辑命令执行路径会修改注册表并写日志，不可在渲染线程直接调用。
void TimelineAction::execute(SessionContext& ctx)
{
    auto& reg = ctx.timelineRegistry;
    if ( m_type == Type::Create ) {
        // 恢复时尝试沿用记录的实体提示，并保存实际返回身份以便后续撤销。
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
            // patch 保留组件更新通知入口，不绕过注册表直接覆盖外部快照。
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
    // 即便目标已失效而跳过实体修改，当前实现仍保留同步与统计失效请求。
    // 节奏变化也会影响长条等物件的统计，不能只标记时间点持久化。
    ctx.isNoteStatsDirty = true;
}

/// @brief 以动作保存的旧值撤销时间点创建、删除或更新。
/// @param ctx 动作所属会话。
/// @note 不恢复删除前的外部选择状态，只恢复时间线组件。
/// @pre 历史记录中的 before/after 仍满足对应动作类型的存在性约束。
/// @warning
/// 撤销在会话逻辑执行方串行应用，不与时间线编辑或渲染读取并发修改组件。
void TimelineAction::undo(SessionContext& ctx)
{
    auto& reg = ctx.timelineRegistry;
    XINFO("[Undo] TimelineAction Type={}", static_cast<int>(m_type));
    if ( m_type == Type::Create ) {
        // 撤销创建不要求 after 再次写入，只移除仍然有效的目标实体。
        if ( reg.valid(m_entity) ) reg.destroy(m_entity);
    } else if ( m_type == Type::Delete ) {
        if ( !reg.valid(m_entity) ) m_entity = reg.create(m_entity);
        // 撤销删除恢复旧组件；已有有效实体时按现有重放规则覆盖组件。
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

/// @brief 重新应用时间点动作，复用首次执行的状态更新路径。
/// @param ctx 动作所属会话。
/// @pre 动作此前已执行并撤销，实体提示及前后快照继续归属于该会话。
void TimelineAction::redo(SessionContext& ctx)
{
    // 复用 execute 的完整替换语义，而不是音符重做的逐字段条件合并。
    XINFO("[Redo] TimelineAction");
    execute(ctx);
}

/// @brief 生成本地化的时间点动作名称，附带可用快照中的时间。
/// @return 有 after 时优先显示新时间，只有 before 时显示旧时间。
/// @note 名称仅用于展示，不作为动作类型或重放目标的身份标识。
/// @note 查询时读取当前翻译，历史动作不固定保存创建当时的界面语言。
/// @warning 名称格式化会构造字符串，供历史列表或状态反馈使用，不用于实体查找。
std::string TimelineAction::getName() const
{
    // 时间点的效果数值不参与名称定位，名称保持简短，详细值由动作记录保存。
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
    // 删除没有 after，改用 before 的位置，避免名称显示空时间或默认零。
    if ( m_before )
        return fmt::format("{} ({}: {:.3f})",
                           typeStr,
                           TR("ui.status.info.time"),
                           m_before->m_timestamp);
    return typeStr;
}

// --- BatchTimelineAction 实现 ---

/// @brief 将一组时间点变更作为一个动作应用。
/// @param ctx 接收变更的会话。
/// @note 有 after 的条目创建或覆盖，无 after 而有 before 的条目删除。
/// @note before 与 after 都为空的条目不修改实体，但仍计入动作条目总数。
/// @warning 每次提交遍历本批条目，重建请求合并到批次末，不应在逐帧刷新中重放。
void BatchTimelineAction::execute(SessionContext& ctx)
{
    // entry.entity 保存实际创建身份后留给下一次
    // undo/redo，不只是一次性创建提示。
    auto& reg = ctx.timelineRegistry;
    XINFO("[Action] BatchTimelineAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        // 每个条目保存自身实际实体，重放不能只依赖批次中的位置索引。
        if ( entry.after.has_value() ) {
            if ( !reg.valid(entry.entity) ) {
                entry.entity = reg.create(entry.entity);
            }
            reg.emplace_or_replace<TimelineComponent>(entry.entity,
                                                      *entry.after);
            // 创建和更新在批量路径共用覆盖分支，不通过条目排列顺序区分类型。
        } else if ( entry.before.has_value() ) {
            if ( reg.valid(entry.entity) ) {
                reg.destroy(entry.entity);
            }
        }
    }
    ctx.m_needsTimingsSync = true;
    ctx.isNoteStatsDirty   = true;
}

/// @brief 按每个条目的 before 恢复整批时间点。
/// @param ctx 动作所属会话。
/// @note 沿记录顺序处理独立实体，并在批次末统一标记同步与统计失效。
/// @pre 条目表示独立实体的前后值，不依赖相反顺序才能撤销的中间步骤。
/// @warning 注册表修改必须在所属会话的逻辑执行方完成，条目循环不提供内部同步。
void BatchTimelineAction::undo(SessionContext& ctx)
{
    // before 存储的是业务旧值，不从 after 反推 BPM、SV 等不同效果的逆运算。
    auto& reg = ctx.timelineRegistry;
    XINFO("[Undo] BatchTimelineAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.before.has_value() ) {
            // 有旧值时恢复旧值；它可能是撤销更新，也可能是恢复已删除实体。
            if ( !reg.valid(entry.entity) ) {
                entry.entity = reg.create(entry.entity);
            }
            reg.emplace_or_replace<TimelineComponent>(entry.entity,
                                                      *entry.before);
        } else if ( entry.after.has_value() ) {
            // 只有新值的条目原本是创建，撤销后应使实体不再存在。
            if ( reg.valid(entry.entity) ) {
                reg.destroy(entry.entity);
            }
        }
    }
    ctx.m_needsTimingsSync = true;
    ctx.isNoteStatsDirty   = true;
}

/// @brief 重做整批时间点变更，不创建新的动作记录。
/// @param ctx 动作所属会话。
/// @pre m_entries 是此前撤销的记录，不应重新根据当前注册表生成 before 值。
void BatchTimelineAction::redo(SessionContext& ctx)
{
    XINFO("[Redo] BatchTimelineAction");
    execute(ctx);
}

/// @brief 按批次用途和条目数生成本地化显示名称。
/// @return 已知用途使用对应翻译键，其余使用通用批量动作名称。
/// @note 展示的是记录条目数，不是本次重放实际成功修改的实体数量。
/// @note 名称不验证条目是否仍有效，不能根据显示数量判断是否还存在可操作实体。
std::string BatchTimelineAction::getName() const
{
    const char* nameKey = "ui.status.action.batch_note";
    // m_name 是内部用途标记，不直接作为界面文本输出。
    // 用途映射不改变条目的执行方式，实际创建、更新或删除仍由前后值决定。
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

/// @brief 应用单个音符动作并优先增量更新模型与渲染查询缓存。
/// @param ctx 动作所属会话，持有正式谱面与项目草稿的同步标记。
/// @pre before/after 与动作类型匹配，注册表由逻辑执行方独占修改。
/// @note 模型增量写入与查询缓存增量写入是两条独立成功路径。
/// @note 持久化脏标记根据动作记录的数据域合并，缓存差量则根据实际修改值构造。
/// @note 创建路径重置辅助组件，更新路径仅替换音符组件；二者不能无条件合并。
/// @warning 编辑动作路径可能复制折线子节点及元数据，不作为空闲 update
/// 的周期任务。
void NoteAction::execute(SessionContext& ctx)
{
    if ( !m_beforeDraftTrackCount ) {
        // optional 的存在性区分“尚未捕获”和“原轨数为零”，不能用数值真假替代。
        // 只在首次执行捕获轨道数，重放继续使用动作原有的前后状态。
        m_beforeDraftTrackCount = ctx.draftTrackCount;
        m_afterDraftTrackCount =
            m_after ? std::max(ctx.draftTrackCount,
                               requiredDraftTrackCount(*m_after))
                    : ctx.draftTrackCount;
    }
    if ( m_afterDraftTrackCount ) {
        // 先更新布局容量再应用物件，目标草稿轨应在物件出现时已经存在。
        applyDraftTrackCount(ctx, *m_afterDraftTrackCount);
    }

    auto&      reg                = ctx.noteRegistry;
    const bool hadPendingNoteSync = ctx.m_needsNotesSync;
    // 记录进入动作时是否已有待同步内容，避免单个创建增量掩盖此前未保存的变更。
    std::optional<NoteComponent> cacheBefore;
    std::optional<NoteComponent> cacheAfter;
    // optional 拷贝保留修改前值，不能持有随后 patch 或 destroy
    // 会改变的组件引用。
    bool beatmapUpdatedIncrementally = false;
    if ( m_type == Type::Create ) {
        ensureNoteCollaborationIdentity(*m_after);
        // 写入注册表前准备身份，使新组件和动作快照从创建起保持相同逻辑 ID。
        if ( !reg.valid(m_entity) ) m_entity = reg.create(m_entity);
        reg.emplace_or_replace<NoteComponent>(m_entity, *m_after);
        reg.emplace_or_replace<TransformComponent>(m_entity);
        reg.emplace_or_replace<InteractionComponent>(m_entity);
        // 创建使用新的辅助状态，不从动作快照恢复旧悬浮或拖动标记。
        cacheAfter = reg.get<NoteComponent>(m_entity);
        if ( !hadPendingNoteSync ) {
            // 模型原本同步时才尝试追加创建项，否则保留完整同步以覆盖全部改动。
            beatmapUpdatedIncrementally =
                SessionUtils::syncCreatedNoteToBeatmap(ctx, *cacheAfter);
        }
        XINFO("[Action] Create Note: Type={}, Time={:.3f}, Track={}",
              (int)m_after->m_type,
              m_after->m_timestamp,
              m_after->m_trackIndex);
    } else if ( m_type == Type::Delete ) {
        if ( isActionNoteEntity(reg, m_entity, *m_before) ) {
            auto& current = reg.get<NoteComponent>(m_entity);
            ensureNoteCollaborationIdentity(current);
            inheritNoteIdentity(current, m_before);
            cacheBefore = current;
            // 补身份只影响标识，不把当前几何覆盖进用于撤销的历史 before。
            // 缓存差量使用实际删除前的当前值，不只依赖可能较旧的动作快照。
            XINFO("[Action] Delete Note: Time={:.3f}, Track={}",
                  m_before->m_timestamp,
                  m_before->m_trackIndex);
            forgetChartObjectSelection(ctx,
                                       m_before->m_isDraft
                                           ? ChartObjectKind::DraftNote
                                           : ChartObjectKind::PlayerNote,
                                       m_entity);
            reg.destroy(m_entity);
            // 先解除选择再销毁实体，避免选择集合残留已失效的音符引用。
        }
    } else if ( m_type == Type::Update ) {
        if ( isActionNoteEntity(reg, m_entity, *m_before) ) {
            auto& current = reg.get<NoteComponent>(m_entity);
            ensureNoteCollaborationIdentity(current);
            inheritNoteIdentity(current, m_before);
            inheritNoteIdentity(current, m_after);
            cacheBefore = current;
            XINFO(
                "[Action] Update Note: Time [{:.3f} -> {:.3f}], Track [{} -> "
                "{}]",
                m_before->m_timestamp,
                m_after->m_timestamp,
                m_before->m_trackIndex,
                m_after->m_trackIndex);
            reg.patch<NoteComponent>(m_entity,
                                     [&](NoteComponent& n) { n = *m_after; });
            // 首次执行更新采用完整 after；选择性合并由撤销和重做路径另行处理。
            cacheAfter = reg.get<NoteComponent>(m_entity);
        }
    }
    const bool formalMutation = markNoteStorageDirty(ctx, m_before, m_after);
    if ( formalMutation && beatmapUpdatedIncrementally ) {
        // 增量模型写入只会在进入动作时无待同步项的创建分支成功。
        // 只有模型确实完成增量写入才撤销本次正式音符同步标记。
        ctx.m_needsNotesSync = false;
    }
    const bool cacheUpdated =
        applySingleNoteCacheMutation(ctx, m_entity, cacheBefore, cacheAfter);
    // 查询缓存与持久化分开提交：缓存成功不代表磁盘保存已经完成。
    if ( formalMutation && !cacheUpdated ) {
        // 正式物件的命中事件需与音符缓存同代，增量失败则要求后续重建。
        SessionUtils::markHitEventsDirty(ctx);
    }
    if ( cacheUpdated ) {
        // 查询缓存已按实际前后值更新，不再追加完整重排或失效剔除工作。
        // 不清除其他动作已经置位的标记，只是不为当前动作追加回退请求。
        return;
    }
    if ( m_type == Type::Delete ) {
        markNotePruneDirty(ctx);
    } else {
        markNoteOrderDirty(ctx);
    }
}

/// @brief 撤销单个音符动作，恢复草稿轨道数并保留后续协作字段修改。
/// @param ctx 动作所属会话。
/// @note 撤销创建和更新需核对逻辑身份；恢复删除只在目标实体无效时重新创建。
/// @note 缓存前后值来自本次实际操作，不直接把历史 before 当作当前缓存状态。
/// @pre 本动作已经执行；前后快照以及草稿轨数记录未被另一个动作复用。
/// @warning 撤销期间同步修改实体、选择和缓存标记，必须保持会话写入串行化。
void NoteAction::undo(SessionContext& ctx)
{
    // 历史业务值仍保留在动作对象内，本次恢复不会消耗或移动走 before 快照。
    // 轨道数属于动作保存的布局状态，先恢复它再处理音符本体。
    if ( m_beforeDraftTrackCount ) {
        applyDraftTrackCount(ctx, *m_beforeDraftTrackCount);
    }

    auto&                        reg = ctx.noteRegistry;
    std::optional<NoteComponent> cacheBefore;
    std::optional<NoteComponent> cacheAfter;
    XINFO("[Undo] NoteAction Type={}", static_cast<int>(m_type));
    // 撤销后的新增或删除方向与动作原类型相反，后面的缓存回退也要按反向处理。
    if ( m_type == Type::Create ) {
        if ( isActionNoteEntity(reg, m_entity, *m_after) ) {
            // 撤销创建只删除该动作创建的逻辑物件，不删除复用同槽位的其他实体。
            cacheBefore = reg.get<NoteComponent>(m_entity);
            forgetChartObjectSelection(ctx,
                                       m_after->m_isDraft
                                           ? ChartObjectKind::DraftNote
                                           : ChartObjectKind::PlayerNote,
                                       m_entity);
            reg.destroy(m_entity);
        }
    } else if ( m_type == Type::Delete ) {
        if ( !reg.valid(m_entity) ) {
            // 目标仍有效时不擅自覆盖；恢复动作不是重新宣告对已占用实体的所有权。
            m_entity = reg.create(m_entity);
            reg.emplace<NoteComponent>(m_entity, *m_before);
            // 撤销删除恢复业务快照，辅助变换和交互组件从新状态开始。
            reg.emplace<TransformComponent>(m_entity);
            reg.emplace<InteractionComponent>(m_entity);
            cacheAfter = reg.get<NoteComponent>(m_entity);
        }
    } else if ( m_type == Type::Update ) {
        if ( isActionNoteEntity(reg, m_entity, *m_after) ) {
            cacheBefore = reg.get<NoteComponent>(m_entity);
            reg.patch<NoteComponent>(m_entity, [&](NoteComponent& note) {
                applySelectiveNoteTransition(note, *m_after, *m_before);
                // 当前字段偏离 after 时保留后续改动，不整份覆盖为 before。
            });
            cacheAfter = reg.get<NoteComponent>(m_entity);
        }
    }
    const bool formalMutation = markNoteStorageDirty(ctx, m_before, m_after);
    const bool cacheUpdated =
        applySingleNoteCacheMutation(ctx, m_entity, cacheBefore, cacheAfter);
    if ( formalMutation && !cacheUpdated ) {
        SessionUtils::markHitEventsDirty(ctx);
    }
    if ( cacheUpdated ) return;
    if ( m_type == Type::Create ) {
        // 撤销创建实际删除了实体，剩余排序可保留，只请求剔除失效引用。
        markNotePruneDirty(ctx);
    } else {
        // 恢复删除或撤销更新可能改变排列位置，不能只做失效实体剔除。
        markNoteOrderDirty(ctx);
    }
}

/// @brief 重做音符动作，更新类动作按字段从 before 合并到 after。
/// @param ctx 动作所属会话。
/// @note 创建和删除沿用 execute，更新不能沿用首次执行的整组件覆盖。
/// @note 重做沿用已捕获的草稿轨数，不把当前布局重新记为动作的初始状态。
/// @warning 更新分支复制实际组件作为缓存差量，仅在重做请求到来时执行。
void NoteAction::redo(SessionContext& ctx)
{
    // 不新增历史条目；重做栈的移动由调用者管理，本函数只应用原动作状态。
    if ( m_afterDraftTrackCount ) {
        applyDraftTrackCount(ctx, *m_afterDraftTrackCount);
    }
    XINFO("[Redo] NoteAction");
    if ( m_type != Type::Update ) {
        // 只有更新需要与首次执行区分的协作合并逻辑。
        execute(ctx);
        return;
    }

    auto&                        reg = ctx.noteRegistry;
    std::optional<NoteComponent> cacheBefore;
    std::optional<NoteComponent> cacheAfter;
    if ( isActionNoteEntity(reg, m_entity, *m_before) ) {
        cacheBefore = reg.get<NoteComponent>(m_entity);
        // 同一音符的其他字段可能已由协作者更新，重做只恢复仍符合预期的字段。
        reg.patch<NoteComponent>(m_entity, [&](NoteComponent& note) {
            applySelectiveNoteTransition(note, *m_before, *m_after);
            // 此方向预期当前仍为 before，只恢复未被后续操作改变的字段。
        });
        cacheAfter = reg.get<NoteComponent>(m_entity);
    }
    const bool formalMutation = markNoteStorageDirty(ctx, m_before, m_after);
    const bool cacheUpdated =
        applySingleNoteCacheMutation(ctx, m_entity, cacheBefore, cacheAfter);
    if ( formalMutation && !cacheUpdated ) {
        SessionUtils::markHitEventsDirty(ctx);
    }
    if ( !cacheUpdated ) markNoteOrderDirty(ctx);
    // 更新没有纯删除的剔除分支，增量失败时保守请求完整排序缓存更新。
}

/// @brief 按动作类型和可用快照生成包含时间、轨道的本地化名称。
/// @return 优先展示 after，删除类动作使用 before。
/// @note 轨道按动作存储值显示，不在这里转换草稿轨道编号。
/// @note 展示读取动作快照而非实时实体，被删除的音符仍能保留历史名称。
/// @warning 名称生成会分配格式化字符串，不应替代缓存的动作身份或协议字段。
std::string NoteAction::getName() const
{
    // 动作类型决定翻译键，物件类型不会改变创建、删除或更新的历史分类。
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

/// @brief 首次执行一组音符变更，并统一维护身份、选择与派生缓存标记。
/// @param ctx 批量动作所属会话。
/// @note 条目按记录顺序执行；批量路径采用重排或剔除失效标记而非逐项增量更新。
/// @note 创建与更新共用 after 覆盖分支，首次执行的语义不同于后续 redo。
/// @note 整批共用一对草稿轨数快照，不为每个条目保存和恢复中间布局。
/// @pre 调用方负责批次的合法性和执行排他；本函数不提供逐项失败回滚事务。
/// @warning 批次大小决定同步执行成本，不得作为每帧的状态同步替代方案。
void BatchNoteAction::execute(SessionContext& ctx)
{
    // 容量准备先于实体写入，整批中较远的草稿段无需等待它所在条目才扩展布局。
    if ( !m_beforeDraftTrackCount ) {
        // 只在批次首次执行时扫描容量，重放不能扩大或改写历史记录的边界。
        m_beforeDraftTrackCount = ctx.draftTrackCount;
        auto requiredCount      = ctx.draftTrackCount;
        // 取所有 after 所需轨数的最大值，不因批量删除而自动缩小现有草稿区。
        for ( const auto& entry : m_entries ) {
            if ( entry.after ) {
                // 删除没有目标几何，不参与新增草稿容量的计算。
                requiredCount = std::max(requiredCount,
                                         requiredDraftTrackCount(*entry.after));
            }
        }
        m_afterDraftTrackCount = requiredCount;
    }
    if ( m_afterDraftTrackCount ) {
        applyDraftTrackCount(ctx, *m_afterDraftTrackCount);
    }

    auto& reg = ctx.noteRegistry;
    prepareBatchNoteIdentities(reg, m_entries);
    // 身份准备只调整记录中的关联，真正的组件写入由随后的条目循环完成。
    // 在创建子实体前准备父子稳定标识，不能等全部实体写入后再补身份。
    XINFO("[Action] BatchNoteAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.after.has_value() ) {
            if ( !reg.valid(entry.entity) )
                entry.entity = reg.create(entry.entity);
            reg.emplace_or_replace<NoteComponent>(entry.entity, *entry.after);
            // 批量写入保留已有辅助组件；选择状态由下面的可选字段单独决定。
            ensureNoteAuxiliaryComponents(reg, entry.entity);
            // 选择操作只改交互状态，不写回 entry.after 中的音符业务值。
            if ( entry.afterSelected ) {
                // optional 的有值表示显式恢复选择，false
                // 也是需要应用的目标状态。
                setChartObjectSelected(ctx,
                                       entry.after->m_isDraft
                                           ? ChartObjectKind::DraftNote
                                           : ChartObjectKind::PlayerNote,
                                       entry.entity,
                                       *entry.afterSelected);
            }
        } else if ( entry.before.has_value() ) {
            if ( isActionNoteEntity(reg, entry.entity, *entry.before) ) {
                // 删除前按历史对象类别清理选择集合，避免失效引用留到后续交互。
                forgetChartObjectSelection(ctx,
                                           entry.before->m_isDraft
                                               ? ChartObjectKind::DraftNote
                                               : ChartObjectKind::PlayerNote,
                                           entry.entity);
                reg.destroy(entry.entity);
            }
        }
    }
    if ( markBatchNoteStorageDirty(ctx, m_entries) ) {
        // 正式物件变更影响打击事件缓存；纯草稿批次不要求正式播放事件重建。
        SessionUtils::markHitEventsDirty(ctx);
    }
    bool needsOrderRebuild = false;
    bool needsPrune        = false;
    // 混合批次只要有创建或更新就需要完整重排，纯删除才走剔除路径。
    // 此处按记录保守判断，即使部分实体因冲突跳过，也不缩小批次失效范围。
    for ( const auto& entry : m_entries ) {
        if ( entry.after.has_value() ) {
            needsOrderRebuild = true;
        } else if ( entry.before.has_value() ) {
            needsPrune = true;
        }
    }
    if ( needsOrderRebuild ) {
        // 重排已经涵盖失效项处理，混合批次不必再单独请求一次剔除。
        markNoteOrderDirty(ctx);
    } else if ( needsPrune ) {
        markNotePruneDirty(ctx);
    }
}

/// @brief 撤销整批音符变更，恢复记录中的旧值和可选选择状态。
/// @param ctx 批量动作所属会话。
/// @note 更新按字段合并，删除的恢复只在记录实体当前无效时创建。
/// @note 选择状态是动作单独记录的可选值，不从音符业务组件推导。
/// @note 缺省选择值表示不主动恢复，显式 false 则表示恢复未选中状态。
/// @note before/after 的几何记录和 beforeSelected/afterSelected
/// 是两套独立快照。
/// @pre 动作记录仍属于当前会话；不支持把一组实体引用移到另一注册表重放。
/// @warning
/// 批次撤销在逻辑执行方串行处理，选择恢复与业务值更新之间没有线程屏障。
void BatchNoteAction::undo(SessionContext& ctx)
{
    // 不重新调用身份准备，否则撤销可能把原记录关联到后来出现的实体。
    if ( m_beforeDraftTrackCount ) {
        // 恢复批次前的区域容量，和音符业务值一起形成这次撤销的布局结果。
        applyDraftTrackCount(ctx, *m_beforeDraftTrackCount);
    }

    auto& reg = ctx.noteRegistry;
    XINFO("[Undo] BatchNoteAction: {} entries", m_entries.size());
    for ( auto& entry : m_entries ) {
        if ( entry.before.has_value() && entry.after.has_value() ) {
            // 前后都有值表示更新，不把它当作删除后重新创建而丢失当前辅助状态。
            if ( isActionNoteEntity(reg, entry.entity, *entry.after) ) {
                reg.patch<NoteComponent>(
                    entry.entity, [&](NoteComponent& note) {
                        applySelectiveNoteTransition(
                            note, *entry.after, *entry.before);
                    });
            }
            if ( entry.beforeSelected && reg.valid(entry.entity) ) {
                // 选择恢复采用本分支现有的有效性判断，独立于上面的字段合并结果。
                // 因此这里并不保证上面的身份条件成立，不能把两个判断描述成同一条件。
                setChartObjectSelected(ctx,
                                       entry.before->m_isDraft
                                           ? ChartObjectKind::DraftNote
                                           : ChartObjectKind::PlayerNote,
                                       entry.entity,
                                       *entry.beforeSelected);
            }
        } else if ( entry.before.has_value() ) {
            // 只有 before 表示撤销删除，实体已被占用时不覆盖现有内容。
            if ( !reg.valid(entry.entity) ) {
                entry.entity = reg.create(entry.entity);
                reg.emplace<NoteComponent>(entry.entity, *entry.before);
                ensureNoteAuxiliaryComponents(reg, entry.entity);
                if ( entry.beforeSelected ) {
                    // 新建辅助组件后再恢复选择，避免组件默认值抹掉记录中的选中状态。
                    // 使用 before
                    // 的对象类别，保证恢复的是动作前的数据域选择状态。
                    setChartObjectSelected(ctx,
                                           entry.before->m_isDraft
                                               ? ChartObjectKind::DraftNote
                                               : ChartObjectKind::PlayerNote,
                                           entry.entity,
                                           *entry.beforeSelected);
                }
            }
        } else if ( entry.after.has_value() ) {
            // 只有 after 表示撤销创建，解除对应对象类别的选择后再销毁。
            if ( isActionNoteEntity(reg, entry.entity, *entry.after) ) {
                forgetChartObjectSelection(ctx,
                                           entry.after->m_isDraft
                                               ? ChartObjectKind::DraftNote
                                               : ChartObjectKind::PlayerNote,
                                           entry.entity);
                reg.destroy(entry.entity);
            }
        }
    }
    if ( markBatchNoteStorageDirty(ctx, m_entries) ) {
        SessionUtils::markHitEventsDirty(ctx);
    }
    bool needsOrderRebuild = false;
    bool needsPrune        = false;
    for ( const auto& entry : m_entries ) {
        // 撤销方向有 before 即可能恢复或移动实体；只有 after
        // 才是单纯移除创建项。
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

/// @brief 重做整批音符变更，保留更新字段中已发生的后续协作改动。
/// @param ctx 批量动作所属会话。
/// @note 不直接复用 execute，避免更新类条目整份覆盖当前组件。
/// @note 更新、恢复创建和删除分别检查实体状态，跳过冲突条目不会中止其余条目。
/// @pre 使用原动作的稳定身份和历史快照，不因重做而重新捕获当前值作为起点。
/// @warning 重做同步遍历整个动作批次，完整缓存更新由后续脏标记消费流程负责。
void BatchNoteAction::redo(SessionContext& ctx)
{
    // 与单条更新相同，预期值和目标值的方向从撤销时的 after→before 反转。
    if ( m_afterDraftTrackCount ) {
        applyDraftTrackCount(ctx, *m_afterDraftTrackCount);
    }
    XINFO("[Redo] BatchNoteAction");
    auto& reg = ctx.noteRegistry;
    for ( auto& entry : m_entries ) {
        if ( entry.before && entry.after ) {
            // 重做更新仍要求逻辑身份匹配，而不是仅检查注册表里存在这个实体。
            if ( isActionNoteEntity(reg, entry.entity, *entry.before) ) {
                reg.patch<NoteComponent>(
                    entry.entity, [&](NoteComponent& note) {
                        applySelectiveNoteTransition(
                            note, *entry.before, *entry.after);
                    });
                if ( entry.afterSelected ) {
                    // 此重做分支的选择恢复位于身份检查内，身份不符时不修改选择。
                    setChartObjectSelected(ctx,
                                           entry.after->m_isDraft
                                               ? ChartObjectKind::DraftNote
                                               : ChartObjectKind::PlayerNote,
                                           entry.entity,
                                           *entry.afterSelected);
                }
            }
        } else if ( entry.after ) {
            // 重做创建只恢复已消失的实体，不覆盖当前占用记录槽位的物件。
            if ( !reg.valid(entry.entity) ) {
                entry.entity = reg.create(entry.entity);
                reg.emplace<NoteComponent>(entry.entity, *entry.after);
                // 恢复业务值后补齐新的辅助状态，不从已销毁组件借用旧引用。
                ensureNoteAuxiliaryComponents(reg, entry.entity);
                if ( entry.afterSelected ) {
                    setChartObjectSelected(ctx,
                                           entry.after->m_isDraft
                                               ? ChartObjectKind::DraftNote
                                               : ChartObjectKind::PlayerNote,
                                           entry.entity,
                                           *entry.afterSelected);
                }
            }
        } else if ( entry.before &&
                    isActionNoteEntity(reg, entry.entity, *entry.before) ) {
            // 重做删除仍要核对稳定身份，不能把撤销期间复用槽位的新物件一并删除。
            forgetChartObjectSelection(ctx,
                                       entry.before->m_isDraft
                                           ? ChartObjectKind::DraftNote
                                           : ChartObjectKind::PlayerNote,
                                       entry.entity);
            reg.destroy(entry.entity);
        }
    }
    if ( markBatchNoteStorageDirty(ctx, m_entries) ) {
        SessionUtils::markHitEventsDirty(ctx);
    }
    // 批量重做统一要求重排，不复用首次执行中针对纯删除的剔除优化。
    // 重排只请求派生数据更新，不代表重新执行历史动作中的组件修改。
    // 即使记录为空也维持现有的保守失效行为，不把这个入口当作查询函数。
    markNoteOrderDirty(ctx);
}

/// @brief 将批量用途标记映射为本地化名称并附带条目数。
/// @return 已知操作专名或通用批量音符名称。
/// @note 条目数是动作记录数，不保证等于实际修改成功的实体数量。
/// @note 未识别的内部用途使用通用名称，不把内部英文标记直接暴露为界面文本。
/// @warning 此查询构造本地化字符串，不能在渲染物件遍历中逐物件重复调用。
std::string BatchNoteAction::getName() const
{
    // 专名只覆盖预定义用途，新增用途若需独立显示应在此登记翻译键。
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
