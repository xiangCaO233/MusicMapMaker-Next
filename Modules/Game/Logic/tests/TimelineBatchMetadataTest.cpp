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

}  // namespace

/// @brief 运行批量 Timeline 创建元数据测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testBatchCreatePreservesMetadata() ? 0 : 1;
}
