#pragma once

#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/context/SessionContext.h"

namespace MMM::Logic
{

/// @brief 获取指定谱面物件领域的已选实体索引。
/// @param ctx 当前会话上下文。
/// @param kind 谱面物件领域。
/// @return 对应领域的已选实体集合。
/// @warning 交互热路径调用；只返回稳定容器引用，不遍历 Registry。
inline std::unordered_set<entt::entity>& selectedChartObjectIndex(
    SessionContext& ctx, ChartObjectKind kind)
{
    return kind == ChartObjectKind::AudioSample ? ctx.selectedSampleEntities
                                                : ctx.selectedNoteEntities;
}

/// @brief 获取指定谱面物件领域的 ECS Registry。
/// @param ctx 当前会话上下文。
/// @param kind 谱面物件领域。
/// @return 对应领域的 Registry。
/// @warning 交互热路径调用；只做常量分支。
inline entt::registry& chartObjectRegistry(SessionContext& ctx,
                                           ChartObjectKind kind)
{
    return kind == ChartObjectKind::AudioSample ? ctx.sampleRegistry
                                                : ctx.noteRegistry;
}

/// @brief 同步单个谱面物件的选中组件和已选实体索引。
/// @param ctx 当前会话上下文。
/// @param kind 谱面物件领域。
/// @param entity 目标实体。
/// @param selected 目标选中状态。
/// @warning 交互热路径调用；只访问单个实体及对应哈希索引。
inline void setChartObjectSelected(SessionContext& ctx, ChartObjectKind kind,
                                   entt::entity entity, bool selected)
{
    auto& registry = chartObjectRegistry(ctx, kind);
    auto& index    = selectedChartObjectIndex(ctx, kind);
    if ( !registry.valid(entity) ) {
        index.erase(entity);
        return;
    }
    if ( !registry.all_of<InteractionComponent>(entity) ) {
        registry.emplace<InteractionComponent>(entity);
    }
    registry.get<InteractionComponent>(entity).isSelected = selected;
    if ( selected ) {
        index.insert(entity);
    } else {
        index.erase(entity);
    }
}

/// @brief 在实体销毁前从已选索引移除其身份。
/// @param ctx 当前会话上下文。
/// @param kind 谱面物件领域。
/// @param entity 即将销毁的实体。
/// @warning 编辑动作低频路径；只执行一次哈希删除。
inline void forgetChartObjectSelection(SessionContext& ctx,
                                       ChartObjectKind kind,
                                       entt::entity    entity)
{
    selectedChartObjectIndex(ctx, kind).erase(entity);
}

/// @brief 清空两类谱面物件的选中状态。
/// @param ctx 当前会话上下文。
/// @warning 交互热路径调用；只遍历当前已选实体，不扫描完整 Registry。
inline void clearChartObjectSelection(SessionContext& ctx)
{
    const auto clearKind = [&](ChartObjectKind kind) {
        auto& registry = chartObjectRegistry(ctx, kind);
        auto& index    = selectedChartObjectIndex(ctx, kind);
        for ( const auto entity : index ) {
            if ( registry.valid(entity) &&
                 registry.all_of<InteractionComponent>(entity) ) {
                registry.get<InteractionComponent>(entity).isSelected = false;
            }
        }
        index.clear();
    };
    clearKind(ChartObjectKind::PlayerNote);
    clearKind(ChartObjectKind::AudioSample);
}

/// @brief 在整体重建 Registry 时清空对应领域的已选索引。
/// @param ctx 当前会话上下文。
/// @param kind 被整体重建的谱面物件领域。
inline void clearChartObjectSelectionIndex(SessionContext& ctx,
                                           ChartObjectKind kind)
{
    selectedChartObjectIndex(ctx, kind).clear();
}

}  // namespace MMM::Logic
