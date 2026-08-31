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

/// @brief 验证 BPM 编辑与新增保速 SV 合并为一次撤销。
/// @return 新建、撤销和重做均同时处理 BPM 与 SV 时返回 true。
bool testBpmKeepSpeedCreatesSvAtomically()
{
    MMM::Logic::SessionContext   context;
    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdCreateTimelineEvent{
        5.0, MMM::TimingEffect::BPM, 120.0 });
    const entt::entity bpmEntity =
        findTimelineEntity(context, MMM::TimingEffect::BPM);
    if ( bpmEntity == entt::null ) return false;

    controller.handleCommand(MMM::Logic::CmdUpdateBpmWithKeepSpeedSv{
        .bpmEntity   = bpmEntity,
        .newTime     = 6.0,
        .newBpm      = 180.0,
        .scrollValue = 2.0 / 3.0,
    });
    const entt::entity scrollEntity =
        findTimelineEntity(context, MMM::TimingEffect::SCROLL);
    if ( scrollEntity == entt::null ) {
        XERROR("BPM keep-speed command did not create an SV");
        return false;
    }
    const auto& bpm =
        context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
            bpmEntity);
    const auto& scroll =
        context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
            scrollEntity);
    if ( !near(bpm.m_timestamp, 6.0) || !near(bpm.m_value, 180.0) ||
         !near(scroll.m_timestamp, 6.0) || !near(scroll.m_value, 2.0 / 3.0) ) {
        XERROR("BPM keep-speed command produced incorrect BPM or SV fields");
        return false;
    }

    context.actionStack.undo(context);
    if ( !context.timelineRegistry.valid(bpmEntity) ||
         context.timelineRegistry.valid(scrollEntity) ||
         !near(context.timelineRegistry
                   .get<const MMM::Logic::TimelineComponent>(bpmEntity)
                   .m_timestamp,
               5.0) ||
         !near(context.timelineRegistry
                   .get<const MMM::Logic::TimelineComponent>(bpmEntity)
                   .m_value,
               120.0) ) {
        XERROR("One undo did not restore BPM and remove created keep-speed SV");
        return false;
    }

    context.actionStack.redo(context);
    return context.timelineRegistry.valid(scrollEntity) &&
           near(context.timelineRegistry
                    .get<const MMM::Logic::TimelineComponent>(bpmEntity)
                    .m_value,
                180.0) &&
           near(context.timelineRegistry
                    .get<const MMM::Logic::TimelineComponent>(scrollEntity)
                    .m_value,
                2.0 / 3.0);
}

/// @brief 验证 BPM 编辑与已有同时间戳 SV 更新合并为一次撤销。
/// @return 更新、撤销和重做均同时处理 BPM 与 SV 时返回 true。
bool testBpmKeepSpeedUpdatesSvAtomically()
{
    MMM::Logic::SessionContext          context;
    MMM::Logic::ActionController        controller(context);
    MMM::Logic::CmdCreateTimelineEvents createCommand;
    createCommand.events.push_back({ 5.0, MMM::TimingEffect::BPM, 120.0 });
    constexpr double NEAR_DESTINATION_SV_TIME = 7.0000005;
    createCommand.events.push_back(
        { NEAR_DESTINATION_SV_TIME, MMM::TimingEffect::SCROLL, 1.5 });
    controller.handleCommand(createCommand);

    const entt::entity bpmEntity =
        findTimelineEntity(context, MMM::TimingEffect::BPM);
    const entt::entity scrollEntity =
        findTimelineEntity(context, MMM::TimingEffect::SCROLL);
    controller.handleCommand(MMM::Logic::CmdUpdateBpmWithKeepSpeedSv{
        .bpmEntity   = bpmEntity,
        .newTime     = 7.0,
        .newBpm      = 240.0,
        .scrollValue = 0.5,
    });

    const auto fieldsMatch = [&](double bpmTime,
                                 double bpmValue,
                                 double scrollTime,
                                 double scrollValue) {
        const auto& bpm =
            context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
                bpmEntity);
        const auto& scroll =
            context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
                scrollEntity);
        return near(bpm.m_timestamp, bpmTime) && near(bpm.m_value, bpmValue) &&
               near(scroll.m_timestamp, scrollTime) &&
               near(scroll.m_value, scrollValue);
    };
    if ( !fieldsMatch(7.0, 240.0, 7.0, 0.5) ) {
        XERROR("BPM keep-speed command did not update existing destination SV");
        return false;
    }
    context.actionStack.undo(context);
    if ( !fieldsMatch(5.0, 120.0, NEAR_DESTINATION_SV_TIME, 1.5) ) {
        XERROR("One undo did not restore BPM and existing SV");
        return false;
    }
    context.actionStack.redo(context);
    return fieldsMatch(7.0, 240.0, 7.0, 0.5);
}

/// @brief 验证所有创建与修改命令都会拒绝负 BPM，并允许零 BPM。
/// @return 单项、批量与保速联动入口均保持非负 BPM 时返回 true。
bool testNegativeBpmMutationsAreRejected()
{
    // 使用独立空会话覆盖命令层，不依赖 ImGui 或渲染快照。
    // ActionController 是全部 BPM 编辑入口最终共享的持久化边界。
    MMM::Logic::SessionContext   context;
    MMM::Logic::ActionController controller(context);

    // 单项创建是最底层入口；负 BPM 不应生成实体或撤销记录。
    // 该断言同时确保非法命令不会把会话标记成可撤销变更。
    controller.handleCommand(MMM::Logic::CmdCreateTimelineEvent{
        1.0, MMM::TimingEffect::BPM, -120.0 });
    // 空 Registry 证明动作未执行，而非执行后又被数值计算回退。
    // 空撤销栈证明非法输入未产生隐藏的无效操作。
    if ( findTimelineEntity(context, MMM::TimingEffect::BPM) != entt::null ||
         context.actionStack.getUndoStackSize() != 0U ) {
        XERROR("Single Timeline creation accepted a negative BPM");
        return false;
    }

    // 批量创建混合负数与零值；只应保留合法的零 BPM。
    // 零值用于锁定用户要求的下边界，避免实现误改为严格大于零。
    MMM::Logic::CmdCreateTimelineEvents createCommand;
    createCommand.events.push_back({ 2.0, MMM::TimingEffect::BPM, -1.0 });
    createCommand.events.push_back({ 3.0, MMM::TimingEffect::BPM, 0.0 });
    controller.handleCommand(createCommand);
    // 查找结果必须对应零值项，因为负数项应在构造 BatchAction 前被过滤。
    // 单条撤销记录确认批量命令仍保持原子提交语义。
    const entt::entity bpmEntity =
        findTimelineEntity(context, MMM::TimingEffect::BPM);
    if ( bpmEntity == entt::null ||
         !near(context.timelineRegistry
                   .get<const MMM::Logic::TimelineComponent>(bpmEntity)
                   .m_value,
               0.0) ||
         context.actionStack.getUndoStackSize() != 1U ) {
        XERROR("Batch Timeline creation did not enforce the zero BPM boundary");
        return false;
    }

    // 先写入正常 BPM，随后验证表格使用的单项更新无法写入负数。
    // 非法更新必须保留原值，也不得额外压入撤销动作。
    controller.handleCommand(
        MMM::Logic::CmdUpdateTimelineEvent{ bpmEntity, 3.0, 120.0 });
    controller.handleCommand(
        MMM::Logic::CmdUpdateTimelineEvent{ bpmEntity, 3.0, -120.0 });
    // 组件值必须停留在前一条合法更新写入的 120。
    // 动作栈只包含批量创建与合法单项更新两条记录。
    if ( !near(context.timelineRegistry
                   .get<const MMM::Logic::TimelineComponent>(bpmEntity)
                   .m_value,
               120.0) ||
         context.actionStack.getUndoStackSize() != 2U ) {
        XERROR("Single Timeline update accepted a negative BPM");
        return false;
    }

    // 批量更新同样不得绕过约束；全非法批次应直接成为无操作。
    // 这覆盖时间线表格搜索替换最终下发的批量命令路径。
    MMM::Logic::CmdUpdateTimelineEvents updateCommand;
    updateCommand.events.push_back({ bpmEntity, 4.0, -60.0 });
    controller.handleCommand(updateCommand);
    // 全非法批次没有可执行条目，因此既不改值也不更新时间戳。
    // 撤销栈数量不变可证明未生成空 BatchAction。
    if ( !near(context.timelineRegistry
                   .get<const MMM::Logic::TimelineComponent>(bpmEntity)
                   .m_value,
               120.0) ||
         context.actionStack.getUndoStackSize() != 2U ) {
        XERROR("Batch Timeline update accepted a negative BPM");
        return false;
    }

    // 保速联动会同时修改 BPM 与 SV；负 BPM 必须在创建 SV 前整体拒绝。
    // 原 BPM、时间和动作栈数量均应保持不变。
    controller.handleCommand(MMM::Logic::CmdUpdateBpmWithKeepSpeedSv{
        .bpmEntity   = bpmEntity,
        .newTime     = 5.0,
        .newBpm      = -30.0,
        .scrollValue = 4.0,
    });
    // 保速命令必须在组装 BPM 与 Scroll 条目前整体返回。
    // 因此测试同时确认没有额外创建同时间点的 Scroll 实体。
    const auto& bpmAfterRejectedBinding =
        context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
            bpmEntity);
    if ( !near(bpmAfterRejectedBinding.m_timestamp, 3.0) ||
         !near(bpmAfterRejectedBinding.m_value, 120.0) ||
         findTimelineEntity(context, MMM::TimingEffect::SCROLL) != entt::null ||
         context.actionStack.getUndoStackSize() != 2U ) {
        XERROR("Keep-speed Timeline update accepted a negative BPM");
        return false;
    }

    // 最后写入零 BPM，确认所有负数拒绝逻辑没有误伤合法边界值。
    // 成功更新应只新增一条撤销记录。
    controller.handleCommand(
        MMM::Logic::CmdUpdateTimelineEvent{ bpmEntity, 3.0, 0.0 });
    // 零值应真实持久化到组件，而不是仅在输入框中暂存。
    // 第三条撤销记录证明边界值按正常更新流程提交。
    return near(context.timelineRegistry
                    .get<const MMM::Logic::TimelineComponent>(bpmEntity)
                    .m_value,
                0.0) &&
           context.actionStack.getUndoStackSize() == 3U;
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
                   testBpmKeepSpeedCreatesSvAtomically() &&
                   testBpmKeepSpeedUpdatesSvAtomically() &&
                   testNegativeBpmMutationsAreRejected() &&
                   testNonUndoableDirtyState()
               ? 0
               : 1;
}
