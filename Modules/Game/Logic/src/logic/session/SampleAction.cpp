#include "logic/session/SampleAction.h"

#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/context/SessionContext.h"

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
void ensureSampleAuxiliaryComponents(entt::registry& registry,
                                     entt::entity    entity)
{
    if ( !registry.all_of<InteractionComponent>(entity) ) {
        registry.emplace<InteractionComponent>(entity);
    }
}

/// @brief 标记自动采样排序索引需要重建。
/// @param ctx 会话上下文。
void markSampleOrderDirty(SessionContext& ctx)
{
    ctx.isSampleOrderDirty = true;
    ctx.isSamplePruneDirty = false;
}

/// @brief 标记自动采样排序索引只需剔除失效实体。
/// @param ctx 会话上下文。
void markSamplePruneDirty(SessionContext& ctx)
{
    if ( !ctx.isSampleOrderDirty ) {
        ctx.isSamplePruneDirty = true;
    }
}

/// @brief 计算容纳指定自动采样绝对轨道所需的持久化 BGM 轨道数。
/// @param ctx 会话上下文。
/// @param sample 自动采样。
/// @return 所需 BGM 轨道数量；物件不在 BGM 区时返回零。
std::int32_t requiredBgmTrackCount(const SessionContext&  ctx,
                                   const SampleComponent& sample)
{
    if ( ctx.trackCount <= 0 ||
         sample.m_track < static_cast<std::uint32_t>(ctx.trackCount) ) {
        return 0;
    }
    const auto required =
        sample.m_track - static_cast<std::uint32_t>(ctx.trackCount) + 1;
    return static_cast<std::int32_t>(std::min<std::uint32_t>(
        required,
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())));
}

}  // namespace

void SampleAction::execute(SessionContext& ctx)
{
    if ( m_after && !m_beforeBgmTrackCount ) {
        m_beforeBgmTrackCount = ctx.bgmTrackCount;
        m_afterBgmTrackCount =
            std::max(ctx.bgmTrackCount, requiredBgmTrackCount(ctx, *m_after));
    }
    if ( m_afterBgmTrackCount ) {
        ctx.bgmTrackCount = *m_afterBgmTrackCount;
    }

    auto& registry = ctx.sampleRegistry;
    if ( m_type == Type::Create && m_after ) {
        if ( !registry.valid(m_entity) ) {
            m_entity = registry.create(m_entity);
        }
        registry.emplace_or_replace<SampleComponent>(m_entity, *m_after);
        ensureSampleAuxiliaryComponents(registry, m_entity);
        markSampleOrderDirty(ctx);
    } else if ( m_type == Type::Delete && m_before ) {
        if ( registry.valid(m_entity) ) {
            registry.destroy(m_entity);
        }
        markSamplePruneDirty(ctx);
    } else if ( m_type == Type::Update && m_after &&
                registry.valid(m_entity) ) {
        registry.emplace_or_replace<SampleComponent>(m_entity, *m_after);
        ensureSampleAuxiliaryComponents(registry, m_entity);
        markSampleOrderDirty(ctx);
    }
    ctx.m_needsSamplesSync = true;
}

void SampleAction::undo(SessionContext& ctx)
{
    auto& registry = ctx.sampleRegistry;
    if ( m_type == Type::Create ) {
        if ( registry.valid(m_entity) ) {
            registry.destroy(m_entity);
        }
        markSamplePruneDirty(ctx);
    } else if ( m_before ) {
        if ( !registry.valid(m_entity) ) {
            m_entity = registry.create(m_entity);
        }
        registry.emplace_or_replace<SampleComponent>(m_entity, *m_before);
        ensureSampleAuxiliaryComponents(registry, m_entity);
        markSampleOrderDirty(ctx);
    }
    if ( m_beforeBgmTrackCount ) {
        ctx.bgmTrackCount = *m_beforeBgmTrackCount;
    }
    ctx.m_needsSamplesSync = true;
}

void SampleAction::redo(SessionContext& ctx)
{
    execute(ctx);
}

std::string SampleAction::getName() const
{
    switch ( m_type ) {
    case Type::Create: return "创建自动采样";
    case Type::Delete: return "删除自动采样";
    case Type::Update: return "更新自动采样";
    }
    return "自动采样操作";
}

void BatchSampleAction::execute(SessionContext& ctx)
{
    if ( !m_beforeBgmTrackCount ) {
        std::int32_t requiredCount = ctx.bgmTrackCount;
        bool         hasAfter      = false;
        for ( const auto& entry : m_entries ) {
            if ( !entry.after ) continue;
            hasAfter      = true;
            requiredCount = std::max(requiredCount,
                                     requiredBgmTrackCount(ctx, *entry.after));
        }
        if ( hasAfter ) {
            m_beforeBgmTrackCount = ctx.bgmTrackCount;
            m_afterBgmTrackCount  = requiredCount;
        }
    }
    if ( m_afterBgmTrackCount ) {
        ctx.bgmTrackCount = *m_afterBgmTrackCount;
    }

    auto& registry = ctx.sampleRegistry;
    for ( auto& entry : m_entries ) {
        if ( entry.after ) {
            if ( !registry.valid(entry.entity) ) {
                entry.entity = registry.create(entry.entity);
            }
            registry.emplace_or_replace<SampleComponent>(entry.entity,
                                                         *entry.after);
            ensureSampleAuxiliaryComponents(registry, entry.entity);
        } else if ( entry.before && registry.valid(entry.entity) ) {
            registry.destroy(entry.entity);
        }
    }
    ctx.m_needsSamplesSync = true;
    markSampleOrderDirty(ctx);
}

void BatchSampleAction::undo(SessionContext& ctx)
{
    auto& registry = ctx.sampleRegistry;
    for ( auto& entry : m_entries ) {
        if ( entry.before ) {
            if ( !registry.valid(entry.entity) ) {
                entry.entity = registry.create(entry.entity);
            }
            registry.emplace_or_replace<SampleComponent>(entry.entity,
                                                         *entry.before);
            ensureSampleAuxiliaryComponents(registry, entry.entity);
        } else if ( entry.after && registry.valid(entry.entity) ) {
            registry.destroy(entry.entity);
        }
    }
    if ( m_beforeBgmTrackCount ) {
        ctx.bgmTrackCount = *m_beforeBgmTrackCount;
    }
    ctx.m_needsSamplesSync = true;
    markSampleOrderDirty(ctx);
}

void BatchSampleAction::redo(SessionContext& ctx)
{
    execute(ctx);
}

std::string BatchSampleAction::getName() const
{
    return fmt::format("{}: {}", m_name, m_entries.size());
}

void TrackCountAction::apply(SessionContext& ctx, std::int32_t trackCount,
                             bool useAfterTrack)
{
    ctx.trackCount = trackCount;
    if ( ctx.currentBeatmap ) {
        ctx.currentBeatmap->m_baseMapMetadata.track_count = trackCount;
    }
    for ( const auto& change : m_sampleChanges ) {
        if ( !ctx.sampleRegistry.valid(change.entity) ||
             !ctx.sampleRegistry.all_of<SampleComponent>(change.entity) ) {
            continue;
        }
        auto& sample   = ctx.sampleRegistry.get<SampleComponent>(change.entity);
        sample.m_track = useAfterTrack ? change.afterTrack : change.beforeTrack;
    }
    ctx.m_needsSamplesSync = true;
}

void TrackCountAction::execute(SessionContext& ctx)
{
    XINFO("玩家轨道数从 {} 更新为 {}，迁移 {} 个自动采样",
          m_beforeTrackCount,
          m_afterTrackCount,
          m_sampleChanges.size());
    apply(ctx, m_afterTrackCount, true);
}

void TrackCountAction::undo(SessionContext& ctx)
{
    apply(ctx, m_beforeTrackCount, false);
}

void TrackCountAction::redo(SessionContext& ctx)
{
    apply(ctx, m_afterTrackCount, true);
}

std::string TrackCountAction::getName() const
{
    return fmt::format(
        "更新玩家轨道数: {} -> {}", m_beforeTrackCount, m_afterTrackCount);
}

}  // namespace MMM::Logic
