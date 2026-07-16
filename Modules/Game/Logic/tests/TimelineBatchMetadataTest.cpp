#include "logic/session/ActionController.h"

#include "log/colorful-log.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"

#include <cmath>
#include <optional>
#include <string>

namespace
{

/// @brief 使用小容差比较时间线数值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-9;
}

/// @brief 不抛异常地读取一个时间线元数据值。
/// @param metadata 待查询的时间线元数据。
/// @param source 元数据来源类型。
/// @param key 元数据键。
/// @return 找到时返回对应值，否则返回空。
std::optional<std::string> timingMetadataValue(
    const MMM::TimingMetadata& metadata, MMM::TimingMetadataType source,
    const std::string& key)
{
    const auto sourceIt = metadata.timing_properties.find(source);
    if ( sourceIt == metadata.timing_properties.end() ) {
        return std::nullopt;
    }
    const auto valueIt = sourceIt->second.find(key);
    if ( valueIt == sourceIt->second.end() ) {
        return std::nullopt;
    }
    return valueIt->second;
}

/// @brief 查找指定类型的 Timeline 组件。
/// @param context 当前测试会话上下文。
/// @param effect 要查找的 Timeline 类型。
/// @return 找到时返回组件观察指针，否则返回空。
const MMM::Logic::TimelineComponent* findTimeline(
    const MMM::Logic::SessionContext& context, MMM::TimingEffect effect)
{
    const auto view =
        context.timelineRegistry.view<const MMM::Logic::TimelineComponent>();
    for ( const auto entity : view ) {
        const auto& timeline =
            view.get<const MMM::Logic::TimelineComponent>(entity);
        if ( timeline.m_effect == effect ) {
            return &timeline;
        }
    }
    return nullptr;
}

/// @brief 查找指定类型的 Timeline 实体。
/// @param context 当前测试会话上下文。
/// @param effect 要查找的 Timeline 类型。
/// @return 找到时返回实体，否则返回 entt::null。
entt::entity findTimelineEntity(const MMM::Logic::SessionContext& context,
                                MMM::TimingEffect                 effect)
{
    const auto view =
        context.timelineRegistry.view<const MMM::Logic::TimelineComponent>();
    for ( const auto entity : view ) {
        if ( view.get<const MMM::Logic::TimelineComponent>(entity).m_effect ==
             effect ) {
            return entity;
        }
    }
    return entt::null;
}

/// @brief 验证批量创建、撤销和重做均保留 Timing metadata。
/// @return 行为符合预期时返回 true。
bool testBatchCreatePreservesMetadata()
{
    MMM::Logic::SessionContext   context;
    MMM::Logic::ActionController controller(context);

    MMM::TimingMetadata metadata;
    metadata.timing_properties[MMM::TimingMetadataType::MALODY]["beat"] =
        "[8,1,4]";

    MMM::Logic::CmdCreateTimelineEvents command;
    command.events.push_back({ 12.5, MMM::TimingEffect::BPM, 180.0, metadata });
    command.events.push_back({ 13.0, MMM::TimingEffect::SCROLL, 1.25 });
    controller.handleCommand(command);

    if ( context.timelineRegistry.storage<MMM::Logic::TimelineComponent>()
             .size() != 2 ) {
        XERROR("Timeline batch create did not create two events");
        return false;
    }

    const auto* bpm    = findTimeline(context, MMM::TimingEffect::BPM);
    const auto* scroll = findTimeline(context, MMM::TimingEffect::SCROLL);
    if ( !bpm || !scroll || !near(bpm->m_timestamp, 12.5) ||
         !near(bpm->m_value, 180.0) || !near(scroll->m_timestamp, 13.0) ||
         !near(scroll->m_value, 1.25) ) {
        XERROR("Timeline batch create changed core fields");
        return false;
    }

    const auto beat = timingMetadataValue(
        bpm->m_metadata, MMM::TimingMetadataType::MALODY, "beat");
    if ( !beat || *beat != "[8,1,4]" ) {
        XERROR("Timeline batch create dropped metadata");
        return false;
    }
    if ( !scroll->m_metadata.timing_properties.empty() ) {
        XERROR("Timeline batch create changed default metadata");
        return false;
    }

    context.actionStack.undo(context);
    if ( !context.timelineRegistry.view<const MMM::Logic::TimelineComponent>()
              .empty() ) {
        XERROR("Timeline batch undo did not remove created events");
        return false;
    }

    context.actionStack.redo(context);
    bpm = findTimeline(context, MMM::TimingEffect::BPM);
    if ( !bpm ) {
        XERROR("Timeline batch redo did not restore BPM event");
        return false;
    }
    const auto redoneBeat = timingMetadataValue(
        bpm->m_metadata, MMM::TimingMetadataType::MALODY, "beat");
    if ( !redoneBeat || *redoneBeat != "[8,1,4]" ) {
        XERROR("Timeline batch redo dropped metadata");
        return false;
    }
    return true;
}

/// @brief 验证批量更新合并为一次撤销并保留 Timing metadata。
/// @return 批量更新、撤销和重做均符合预期时返回 true。
bool testBatchUpdateIsAtomic()
{
    MMM::Logic::SessionContext   context;
    MMM::Logic::ActionController controller(context);

    MMM::TimingMetadata metadata;
    metadata.timing_properties[MMM::TimingMetadataType::MALODY]["beat"] =
        "[4,0,1]";
    MMM::Logic::CmdCreateTimelineEvents createCommand;
    createCommand.events.push_back(
        { 5.0, MMM::TimingEffect::BPM, 120.0, metadata });
    createCommand.events.push_back({ 6.0, MMM::TimingEffect::SCROLL, 10000.0 });
    controller.handleCommand(createCommand);

    const entt::entity bpmEntity =
        findTimelineEntity(context, MMM::TimingEffect::BPM);
    const entt::entity scrollEntity =
        findTimelineEntity(context, MMM::TimingEffect::SCROLL);
    if ( bpmEntity == entt::null || scrollEntity == entt::null ) {
        XERROR("Timeline batch update setup did not create entities");
        return false;
    }

    MMM::Logic::CmdUpdateTimelineEvents updateCommand;
    updateCommand.events.push_back({ bpmEntity, 5.0, 180.0 });
    updateCommand.events.push_back({ scrollEntity, 6.0, 10.0 });
    controller.handleCommand(updateCommand);

    const auto& updatedBpm =
        context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
            bpmEntity);
    const auto& updatedScroll =
        context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
            scrollEntity);
    const auto updatedBeat = timingMetadataValue(
        updatedBpm.m_metadata, MMM::TimingMetadataType::MALODY, "beat");
    if ( !near(updatedBpm.m_value, 180.0) ||
         !near(updatedScroll.m_value, 10.0) || !updatedBeat ||
         *updatedBeat != "[4,0,1]" ) {
        XERROR("Timeline batch update changed unexpected fields");
        return false;
    }

    context.actionStack.undo(context);
    if ( !near(context.timelineRegistry
                   .get<const MMM::Logic::TimelineComponent>(bpmEntity)
                   .m_value,
               120.0) ||
         !near(context.timelineRegistry
                   .get<const MMM::Logic::TimelineComponent>(scrollEntity)
                   .m_value,
               10000.0) ) {
        XERROR("One undo did not restore all batch-updated Timeline events");
        return false;
    }

    context.actionStack.redo(context);
    if ( !near(context.timelineRegistry
                   .get<const MMM::Logic::TimelineComponent>(bpmEntity)
                   .m_value,
               180.0) ||
         !near(context.timelineRegistry
                   .get<const MMM::Logic::TimelineComponent>(scrollEntity)
                   .m_value,
               10.0) ) {
        XERROR("One redo did not restore all batch-updated Timeline events");
        return false;
    }
    return true;
}

/// @brief 验证未进入撤销栈的元数据编辑仍会参与未保存状态判断。
/// @return 标脏、保存和清空语义符合预期时返回 true。
bool testNonUndoableDirtyState()
{
    MMM::Logic::SessionContext context;
    if ( context.actionStack.isDirty() ) {
        XERROR("A new action stack was unexpectedly dirty");
        return false;
    }

    context.actionStack.markDirty();
    if ( !context.actionStack.isDirty() ) {
        XERROR("Non-undoable metadata changes were not marked dirty");
        return false;
    }

    context.actionStack.markSaved();
    if ( context.actionStack.isDirty() ) {
        XERROR("Saving did not clear non-undoable metadata changes");
        return false;
    }

    context.actionStack.markDirty();
    context.actionStack.clear();
    if ( context.actionStack.isDirty() ) {
        XERROR("Clearing the action stack kept non-undoable changes dirty");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行批量 Timeline 创建元数据测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testBatchCreatePreservesMetadata() && testBatchUpdateIsAtomic() &&
                   testNonUndoableDirtyState()
               ? 0
               : 1;
}
