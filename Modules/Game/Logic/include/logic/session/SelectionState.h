#pragma once

#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/context/SessionContext.h"

namespace MMM::Logic
{
// 不同注册表可能出现相同数值的实体句柄，索引必须按物件领域隔离。

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
    // 与选择索引使用同一领域路由，禁止用音符注册表验证样本实体。
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
        // 延迟输入可能引用已删除实体，只清掉旧索引，不为选择操作重建物件。
        index.erase(entity);
        return;
    }
    if ( !registry.all_of<InteractionComponent>(entity) ) {
        // 恢复或导入的实体可能尚无交互组件，在首次选择时补齐。
        registry.emplace<InteractionComponent>(entity);
    }
    registry.get<InteractionComponent>(entity).isSelected = selected;
    // 组件服务绘制与拾取，哈希索引服务选择集操作，两者必须同时更新。
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
    // 销毁流程只清索引；若实体仍要保留，应使用 setChartObjectSelected。
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
            // 清理期间容忍失效实体，不因历史索引残留而访问已销毁的组件。
            if ( registry.valid(entity) &&
                 registry.all_of<InteractionComponent>(entity) ) {
                registry.get<InteractionComponent>(entity).isSelected = false;
            }
        }
        index.clear();
        // 统一清空也会移除未能访问组件的失效句柄。
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
    // 注册表由调用方整体重建，这里不逐个回写即将失效的交互组件。
    selectedChartObjectIndex(ctx, kind).clear();
}

}  // namespace MMM::Logic
