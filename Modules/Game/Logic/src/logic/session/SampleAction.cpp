#include "logic/session/SampleAction.h"

#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/session/NoteIdentity.h"
#include "logic/session/SelectionState.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <fmt/format.h>
#include <limits>

namespace MMM::Logic
{

namespace
{

/// @brief 确保自动采样实体具备交互组件。
/// @param registry 自动采样注册表。
/// @param entity 目标实体。
/// @pre entity 必须是该注册表中的有效实体。
void ensureSampleAuxiliaryComponents(entt::registry& registry,
                                     entt::entity    entity)
{
    // 撤销恢复可能重建实体；仅补缺失组件，已有选中与悬浮状态不能被覆盖。
    if ( !registry.all_of<InteractionComponent>(entity) ) {
        registry.emplace<InteractionComponent>(entity);
    }
}

/// @brief 标记自动采样排序索引需要重建。
/// @param ctx 会话上下文。
/// @note 与局部剔除标记互斥，后续缓存消费者负责实际维护工作。
void markSampleOrderDirty(SessionContext& ctx)
{
    // 新建和更新可能改变排序键，完整重建优先于只剔除删除项。
    ctx.isSampleOrderDirty = true;
    ctx.isSamplePruneDirty = false;
    // 标注投影也包含自动采样位置，需要随样本索引一起失效。
    ctx.isAnnotationRenderCacheDirty = true;
}

/// @brief 标记自动采样排序索引只需剔除失效实体。
/// @param ctx 会话上下文。
/// @pre 本次变化仅删除实体，没有改变其他样本的排序键。
void markSamplePruneDirty(SessionContext& ctx)
{
    // 删除不改变存活项之间的顺序；已有全量重建请求时无需再排队剔除。
    if ( !ctx.isSampleOrderDirty ) {
        ctx.isSamplePruneDirty = true;
    }
    ctx.isAnnotationRenderCacheDirty = true;
}

/// @brief 计算容纳指定自动采样绝对轨道所需的持久化 BGM 轨道数。
/// @param ctx 会话上下文。
/// @param sample 自动采样。
/// @return 所需 BGM 轨道数量；物件不在 BGM 区时返回零。
/// @note 只计算容纳该样本的最小数量，不扫描其他样本或修改布局。
std::int32_t requiredBgmTrackCount(const SessionContext&  ctx,
                                   const SampleComponent& sample)
{
    // 玩家区内的绝对轨道不扩展 BGM 区；无有效玩家宽度时不推导布局。
    if ( ctx.trackCount <= 0 ||
         sample.m_track < static_cast<std::uint32_t>(ctx.trackCount) ) {
        return 0;
    }
    // 零基轨道索引换成数量需要加一，再限制到持久化的有符号字段范围。
    const auto required =
        sample.m_track - static_cast<std::uint32_t>(ctx.trackCount) + 1;
    return static_cast<std::int32_t>(std::min<std::uint32_t>(
        required,
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())));
}

}  // namespace

/// @brief 应用单个样本变化，必要时扩展持久化 BGM 轨道区。
/// @param ctx 动作所属会话。
/// @pre 类型与快照匹配：新建需 after，删除需 before，更新需有效实体与 after。
/// @note 删除不自动缩减 BGM 区，空轨道仍属于用户持久化的布局。
/// @warning 编辑提交路径，只标记缓存失效，不在动作内部重建排序或写入文件。
void SampleAction::execute(SessionContext& ctx)
{
    // 只在首次执行捕获轨道数，重做不能用撤销后的状态重新定义历史基线。
    if ( m_after && !m_beforeBgmTrackCount ) {
        // optional 区分尚未记录与记录了零轨道，不能用数值零判断初始化状态。
        m_beforeBgmTrackCount = ctx.bgmTrackCount;
        m_afterBgmTrackCount =
            // 只扩展不足的布局，移动样本到较低轨道不会收回原有空轨道。
            std::max(ctx.bgmTrackCount, requiredBgmTrackCount(ctx, *m_after));
    }
    if ( m_afterBgmTrackCount ) {
        // 轨道扩展是动作的一部分，与样本一起撤销和重做。
        ctx.bgmTrackCount = *m_afterBgmTrackCount;
    }

    auto& registry = ctx.sampleRegistry;
    if ( m_type == Type::Create && m_after ) {
        // 协作身份写回动作快照，使后续重做继续使用同一物件身份。
        ensureSampleCollaborationIdentity(*m_after);
        if ( !registry.valid(m_entity) ) {
            // 创建时请求原实体编号，并保存注册表实际返回的实体供后续撤销。
            m_entity = registry.create(m_entity);
        }
        registry.emplace_or_replace<SampleComponent>(m_entity, *m_after);
        // 样本数据与交互组件分别恢复，实体才能继续参与拾取与选择。
        ensureSampleAuxiliaryComponents(registry, m_entity);
        markSampleOrderDirty(ctx);
    } else if ( m_type == Type::Delete && m_before ) {
        if ( registry.valid(m_entity) ) {
            // 销毁前移除选择记录，避免失效句柄残留在跨物件类型的选择集合中。
            forgetChartObjectSelection(
                ctx, ChartObjectKind::AudioSample, m_entity);
            registry.destroy(m_entity);
        }
        markSamplePruneDirty(ctx);
    } else if ( m_type == Type::Update && m_after &&
                registry.valid(m_entity) ) {
        // 更新不重建已失效实体；只有新建或撤销删除负责恢复实体生命周期。
        registry.emplace_or_replace<SampleComponent>(m_entity, *m_after);
        ensureSampleAuxiliaryComponents(registry, m_entity);
        markSampleOrderDirty(ctx);
    }
    // ECS 修改交由外层动作栈同步回谱面，动作本身不直接操作存储文件。
    ctx.m_needsSamplesSync = true;
}

/// @brief 恢复样本变化前的数据及首次执行前的 BGM 轨道数。
/// @param ctx 首次执行动作的会话。
/// @note 撤销创建删除实体；撤销更新和删除恢复 before 快照。
/// @pre 与该动作配对的快照仍属于当前会话，不能跨项目复用实体句柄。
void SampleAction::undo(SessionContext& ctx)
{
    auto& registry = ctx.sampleRegistry;
    if ( m_type == Type::Create ) {
        // 创建的逆操作只需剔除索引中的失效项，不改变其余样本排序。
        if ( registry.valid(m_entity) ) {
            forgetChartObjectSelection(
                ctx, ChartObjectKind::AudioSample, m_entity);
            registry.destroy(m_entity);
        }
        markSamplePruneDirty(ctx);
    } else if ( m_before ) {
        // 撤销删除时实体已不存在，恢复组件前先重新取得有效实体。
        if ( !registry.valid(m_entity) ) {
            m_entity = registry.create(m_entity);
        }
        registry.emplace_or_replace<SampleComponent>(m_entity, *m_before);
        // 恢复旧组件的完整内容，不只覆盖本次编辑所涉及的几个属性。
        ensureSampleAuxiliaryComponents(registry, m_entity);
        markSampleOrderDirty(ctx);
    }
    if ( m_beforeBgmTrackCount ) {
        // 回到动作原始布局，而不是按当前存活样本重新猜测轨道数量。
        ctx.bgmTrackCount = *m_beforeBgmTrackCount;
    }
    ctx.m_needsSamplesSync = true;
}

/// @brief 重用首次执行逻辑恢复 after 状态。
/// @param ctx 撤销动作时所用的会话。
/// @note 已保存的轨道数快照和协作身份在 execute 中不会重复初始化。
void SampleAction::redo(SessionContext& ctx)
{
    execute(ctx);
}

/// @brief 返回单样本操作类型对应的历史名称。
/// @return 可读名称；未识别的类型返回通用名称。
std::string SampleAction::getName() const
{
    switch ( m_type ) {
    case Type::Create: return "创建自动采样";
    case Type::Delete: return "删除自动采样";
    case Type::Update: return "更新自动采样";
    }
    return "自动采样操作";
}

/// @brief 应用批量样本快照，并按需要恢复选中状态。
/// @param ctx 持有样本注册表与轨道布局的会话。
/// @pre 条目的资源和协作身份已由构造批次的调用方准备完成。
/// @note before 为空表示创建，after 为空表示删除；两者均有值表示更新。
/// @note 选中状态是独立的可选快照，不从样本组件的内容推导。
/// @warning 低频批量编辑路径，遍历本批条目，不应逐帧重复执行。
void BatchSampleAction::execute(SessionContext& ctx)
{
    // 批次的轨道扩展取所有后态的最大需求，不能由最后一个条目决定。
    if ( !m_beforeBgmTrackCount ) {
        std::int32_t requiredCount = ctx.bgmTrackCount;
        bool         hasAfter      = false;
        for ( const auto& entry : m_entries ) {
            // 纯删除没有目标轨道，不因为删除物件而自动缩小用户保留的轨道区。
            if ( !entry.after ) continue;
            hasAfter      = true;
            requiredCount = std::max(requiredCount,
                                     requiredBgmTrackCount(ctx, *entry.after));
        }
        if ( hasAfter ) {
            // 仅存在后态时记录扩展快照，空批次和纯删除保持原布局。
            m_beforeBgmTrackCount = ctx.bgmTrackCount;
            m_afterBgmTrackCount  = requiredCount;
        }
    }
    if ( m_afterBgmTrackCount ) {
        ctx.bgmTrackCount = *m_afterBgmTrackCount;
    }

    auto& registry = ctx.sampleRegistry;
    for ( auto& entry : m_entries ) {
        // 修改条目内的句柄，后续撤销使用本次创建实际获得的实体身份。
        if ( entry.after ) {
            // 有 after 即恢复目标数据，覆盖更新与创建两种条目形态。
            if ( !registry.valid(entry.entity) ) {
                entry.entity = registry.create(entry.entity);
            }
            registry.emplace_or_replace<SampleComponent>(entry.entity,
                                                         *entry.after);
            // 已存在的交互组件保持不变，仅显式记录的选中值随后覆盖它。
            ensureSampleAuxiliaryComponents(registry, entry.entity);
            if ( entry.afterSelected ) {
                // optional 的存在性决定是否覆盖；显式 false 表示取消选中。
                setChartObjectSelected(ctx,
                                       ChartObjectKind::AudioSample,
                                       entry.entity,
                                       *entry.afterSelected);
            }
        } else if ( entry.before && registry.valid(entry.entity) ) {
            // 无后态的有效条目表示删除，先清理选择再释放实体。
            forgetChartObjectSelection(
                ctx, ChartObjectKind::AudioSample, entry.entity);
            registry.destroy(entry.entity);
        }
    }
    ctx.m_needsSamplesSync = true;
    // 混合批次可能同时创建、移动和删除，统一请求完整排序而非仅剔除。
    markSampleOrderDirty(ctx);
}

/// @brief 按各条目的前态恢复整批样本与可选选中状态。
/// @param ctx 执行批次时所用的会话。
/// @note 条目自身保存前后态，恢复不依赖当前组件仍保留后态内容。
/// @pre 批次中每条记录描述其目标实体的完整变化，而非依赖前条的增量操作。
void BatchSampleAction::undo(SessionContext& ctx)
{
    auto& registry = ctx.sampleRegistry;
    for ( auto& entry : m_entries ) {
        if ( entry.before ) {
            // 删除的逆操作需要重建实体，更新的逆操作直接替换当前组件。
            if ( !registry.valid(entry.entity) ) {
                entry.entity = registry.create(entry.entity);
            }
            registry.emplace_or_replace<SampleComponent>(entry.entity,
                                                         *entry.before);
            // 先恢复实体与交互能力，才能将历史选中状态重新加入选择索引。
            ensureSampleAuxiliaryComponents(registry, entry.entity);
            if ( entry.beforeSelected ) {
                // 未记录选中状态时保留已有交互状态，不把缺省值当作取消选择。
                setChartObjectSelected(ctx,
                                       ChartObjectKind::AudioSample,
                                       entry.entity,
                                       *entry.beforeSelected);
            }
        } else if ( entry.after && registry.valid(entry.entity) ) {
            // 没有前态说明实体由本批创建，撤销应移除它而非留下空组件。
            forgetChartObjectSelection(
                ctx, ChartObjectKind::AudioSample, entry.entity);
            registry.destroy(entry.entity);
        }
    }
    if ( m_beforeBgmTrackCount ) {
        // 布局与组件属于同一条历史记录，撤销必须恢复两者。
        ctx.bgmTrackCount = *m_beforeBgmTrackCount;
    }
    ctx.m_needsSamplesSync = true;
    markSampleOrderDirty(ctx);
}

/// @brief 使用批次已保存的后态重新应用变化。
/// @param ctx 撤销该批次时所用的会话。
/// @note 复用批次条目，不重新从当前选择集合收集对象。
void BatchSampleAction::redo(SessionContext& ctx)
{
    execute(ctx);
}

/// @brief 生成包含条目数的批量操作名称。
/// @return 名称中的数量是记录条目数，不是当前注册表中的存活实体数。
std::string BatchSampleAction::getName() const
{
    return fmt::format("{}: {}", m_name, m_entries.size());
}

/// @brief 应用玩家轨道宽度，同时迁移草稿坐标和已记录的样本绝对轨道。
/// @param ctx 动作所属会话。
/// @param trackCount 已由调用方验证的目标玩家轨道数。
/// @param useAfterTrack 选择样本迁移记录中的后态或前态。
/// @pre 目标宽度及迁移表应由命令构造方校验；这里不截断或删除越界正式音符。
/// @warning 轨道布局编辑路径，遍历音符与子节点，不用于普通 update。
void TrackCountAction::apply(SessionContext& ctx, std::int32_t trackCount,
                             bool useAfterTrack)
{
    // 草稿坐标编码依赖玩家区宽度，先保留旧宽度再换算到新编码。
    const std::int32_t previousTrackCount = ctx.trackCount;
    // 此处读取实际会话宽度，草稿编码差量必须基于应用前的布局。
    auto noteView = ctx.noteRegistry.view<NoteComponent>();
    for ( const auto entity : noteView ) {
        auto& note = noteView.get<NoteComponent>(entity);
        if ( !note.m_isDraft ) continue;
        // 正式音符轨道不迁移；草稿根节点和子节点需使用同一个宽度差。
        note.m_trackIndex = note.m_trackIndex + previousTrackCount - trackCount;
        for ( auto& subNote : note.m_subNotes ) {
            // 子节点使用绝对编码轨道而非根节点相对位移，需要逐个同步换算。
            subNote.trackIndex =
                subNote.trackIndex + previousTrackCount - trackCount;
        }
    }
    ctx.trackCount = trackCount;
    // 草稿已完成旧宽度换算后才发布新宽度，避免同一轮使用混合坐标基准。
    if ( ctx.currentBeatmap ) {
        // 会话布局立即生效，模型存在时同步元数据，保证后续保存使用新宽度。
        ctx.currentBeatmap->m_baseMapMetadata.track_count = trackCount;
    }
    for ( const auto& change : m_sampleChanges ) {
        // 迁移表可能包含已失效实体；轨道数操作不负责重建被删除的样本。
        if ( !ctx.sampleRegistry.valid(change.entity) ||
             !ctx.sampleRegistry.all_of<SampleComponent>(change.entity) ) {
            continue;
        }
        auto& sample = ctx.sampleRegistry.get<SampleComponent>(change.entity);
        // 使用构造动作时记录的绝对轨道，避免多次撤销重做累加位移误差。
        sample.m_track = useAfterTrack ? change.afterTrack : change.beforeTrack;
    }
    // 布局影响几何与排序，草稿和样本分别标记回写，不在这里全量重建。
    ctx.isTransformDirty      = true;
    ctx.isNoteOrderDirty      = true;
    ctx.m_needsDraftNotesSync = true;
    ctx.m_needsSamplesSync    = true;
}

/// @brief 首次提交玩家轨道变化并记录诊断日志。
/// @param ctx 动作所属会话。
/// @note 迁移清单在构造时固定，执行时不重新统计样本生成新清单。
void TrackCountAction::execute(SessionContext& ctx)
{
    XINFO("玩家轨道数从 {} 更新为 {}，迁移 {} 个自动采样",
          m_beforeTrackCount,
          m_afterTrackCount,
          m_sampleChanges.size());
    apply(ctx, m_afterTrackCount, true);
}

/// @brief 恢复原玩家宽度及样本迁移表中的原绝对轨道。
/// @param ctx 首次执行时的会话。
void TrackCountAction::undo(SessionContext& ctx)
{
    apply(ctx, m_beforeTrackCount, false);
}

/// @brief 重新应用目标宽度，不重复首次提交的诊断日志。
/// @param ctx 撤销时的会话。
void TrackCountAction::redo(SessionContext& ctx)
{
    apply(ctx, m_afterTrackCount, true);
}

/// @brief 生成原宽度到目标宽度的操作名称。
/// @return 使用历史快照而非会话当前宽度构造的名称。
std::string TrackCountAction::getName() const
{
    return fmt::format(
        "更新玩家轨道数: {} -> {}", m_beforeTrackCount, m_afterTrackCount);
}

/// @brief 应用持久化 BGM 区宽度并标记相关投影待更新。
/// @param ctx 动作所属会话。
/// @param bgmTrackCount 目标数量，负数钳制为零。
/// @note 不删除样本，也不改变样本的绝对轨道编号。
/// @details 仅改变 BGM 区数量，不移动玩家区边界，所以无需执行玩家宽度迁移。
void BgmTrackCountAction::apply(SessionContext& ctx, std::int32_t bgmTrackCount)
{
    // BGM 区可以为空，持久化数量不能为负。
    ctx.bgmTrackCount = std::max(0, bgmTrackCount);
    if ( ctx.currentBeatmap ) {
        // 模型可能尚未关联；会话仍需先保存布局，不能因缺少模型而忽略输入。
        ctx.currentBeatmap->m_baseMapMetadata.bgm_track_count =
            ctx.bgmTrackCount;
    }
    ctx.isTransformDirty = true;
    // 区域宽度参与样本投影，数量变化也需走样本同步入口。
    ctx.m_needsSamplesSync = true;
}

/// @brief 将 BGM 区设置为动作的目标数量。
/// @param ctx 动作所属会话。
/// @note execute、undo、redo 共用 apply，保持元数据回写与失效标记一致。
void BgmTrackCountAction::execute(SessionContext& ctx)
{
    apply(ctx, m_afterBgmTrackCount);
}

/// @brief 将 BGM 区恢复为动作前的数量。
/// @param ctx 首次执行时的会话。
/// @note 恢复存储的布局快照，不以当前样本占用的最大轨道替代原值。
void BgmTrackCountAction::undo(SessionContext& ctx)
{
    apply(ctx, m_beforeBgmTrackCount);
}

/// @brief 重新应用保存的 BGM 区目标数量。
/// @param ctx 撤销时的会话。
void BgmTrackCountAction::redo(SessionContext& ctx)
{
    apply(ctx, m_afterBgmTrackCount);
}

/// @brief 根据历史数量变化方向选择增加或移除轨道的翻译名称。
/// @return 当前语言下的操作名称副本。
/// @note 名称来自前后快照，因此撤销后仍描述原动作，而不是反向动作。
std::string BgmTrackCountAction::getName() const
{
    return m_afterBgmTrackCount > m_beforeBgmTrackCount
               ? TR("ui.action.bgm_track.add").data()
               : TR("ui.action.bgm_track.remove").data();
}

}  // namespace MMM::Logic
