#include "logic/session/ActionController.h"

#include "log/colorful-log.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"

#include <cmath>
#include <optional>
#include <string>

namespace
{

// 通过会话命令和动作栈验证时间线编辑，不启动 UI，也不保存谱面文件。
// “一次撤销”描述历史记录分组，不表示跨线程原子操作或失败事务的完整回滚保证。

/// @brief 使用小容差比较时间线数值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
/// @note 严格差值比较拒绝非有限结果，不将 NaN 当作有效时间或效果值。
bool near(double lhs, double rhs)
{
    // 时间与数值使用同一确定容差，不让近似 SV 匹配窗口成为断言本身的宽容差。
    return std::abs(lhs - rhs) < 1e-9;
}

/// @brief 不抛异常地读取一个时间线元数据值。
/// @param metadata 待查询的时间线元数据。
/// @param source 元数据来源类型。
/// @param key 元数据键。
/// @return 找到时返回对应值，否则返回空。
/// @note 返回字符串副本，可在实体被撤销删除之后保留断言所需的值。
/// @note 空字符串与缺少键不同：存在但为空仍返回有值 optional。
std::optional<std::string> timingMetadataValue(
    const MMM::TimingMetadata& metadata, MMM::TimingMetadataType source,
    const std::string& key)
{
    const auto sourceIt = metadata.timing_properties.find(source);
    if ( sourceIt == metadata.timing_properties.end() ) {
        // 缺少来源分组与缺少组内键都表示未找到，不通过下标访问补建空条目。
        return std::nullopt;
    }
    const auto valueIt = sourceIt->second.find(key);
    // 只读查询不能改变被测元数据，否则测试助手可能掩盖实际丢字段的问题。
    if ( valueIt == sourceIt->second.end() ) {
        // 不能返回默认空字符串，否则“字段丢失”会与合法空值混淆。
        return std::nullopt;
    }
    // 不从其他格式分组寻找后备键，也不解析或规范化字段中的分拍 JSON 文本。
    return valueIt->second;
}

/// @brief 查找指定类型的 Timeline 组件。
/// @param context 当前测试会话上下文。
/// @param effect 要查找的 Timeline 类型。
/// @return 找到时返回组件观察指针，否则返回空。
/// @pre 夹具中每种被查询效果至多一项，否则返回哪项取决于 Registry 遍历顺序。
/// @note 指针只在组件继续存在时有效，撤销删除后必须重新查询。
/// @note 只用于小型测试夹具，不把遍历 Registry 的助手放入生产每帧查询路径。
const MMM::Logic::TimelineComponent* findTimeline(
    const MMM::Logic::SessionContext& context, MMM::TimingEffect effect)
{
    // 查询只观察组件，不创建缺少的时间点，也不改变选中状态。
    const auto view =
        context.timelineRegistry.view<const MMM::Logic::TimelineComponent>();
    for ( const auto entity : view ) {
        // 按效果种类定位，不依赖实体编号或创建顺序在撤销重做后保持一致。
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
/// @note 返回的是当前 Registry 身份，不代表持久化时间点 ID。
/// @pre 场景为每种效果构造至多一项，助手不按时间进一步筛选同类型事件。
entt::entity findTimelineEntity(const MMM::Logic::SessionContext& context,
                                MMM::TimingEffect                 effect)
{
    // 句柄允许跨更新保留；跨删除后只能先检查 valid，不能直接解引用旧组件。
    const auto view =
        context.timelineRegistry.view<const MMM::Logic::TimelineComponent>();
    for ( const auto entity : view ) {
        if ( view.get<const MMM::Logic::TimelineComponent>(entity).m_effect ==
             effect ) {
            return entity;
        }
    }
    // 未找到用空句柄表示，调用方不可用整数零代替 entt::null。
    return entt::null;
}

/// @brief 验证批量创建、撤销和重做均保留 Timing metadata。
/// @return 行为符合预期时返回 true。
/// @note 初次创建检查两项核心字段，重做后这里只检查 BPM 及其 beat 元数据。
bool testBatchCreatePreservesMetadata()
{
    // 撤销以实体视图为空判定，不以 undo 栈长度减少代替真正删除时间点。
    // 上下文先于控制器创建，借用上下文的控制器会先析构，不跨会话保留引用。
    MMM::Logic::SessionContext   context;
    MMM::Logic::ActionController controller(context);

    // 来源分组与 beat 键都要保留；只保留字符串而丢失所属格式也不能视为兼容。
    MMM::TimingMetadata metadata;
    metadata.timing_properties[MMM::TimingMetadataType::MALODY]["beat"] =
        "[8,1,4]";

    MMM::Logic::CmdCreateTimelineEvents command;
    command.events.push_back({ 12.5, MMM::TimingEffect::BPM, 180.0, metadata });
    // 两项采用不同时间和数值，避免调换条目后仍碰巧满足核心字段检查。
    command.events.push_back({ 13.0, MMM::TimingEffect::SCROLL, 1.25 });
    // 同批次混合有元数据和无元数据两项，检测前一条 metadata
    // 被错误复用到后一条。
    controller.handleCommand(command);

    if ( context.timelineRegistry.storage<MMM::Logic::TimelineComponent>()
             .size() != 2 ) {
        XERROR("Timeline batch create did not create two events");
        return false;
    }

    // 分别按 BPM 和 SCROLL 查找，不能只靠创建数量证明正确类型的时间点已经生成。
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
    // 原始 beat 字符串是格式元数据，不根据秒时间重新计算出另一种等价写法。
    if ( !beat || *beat != "[8,1,4]" ) {
        XERROR("Timeline batch create dropped metadata");
        return false;
    }
    if ( !scroll->m_metadata.timing_properties.empty() ) {
        // 空元数据项不能继承同批 BPM 的 MALODY 分组，检查整个属性表为空。
        XERROR("Timeline batch create changed default metadata");
        return false;
    }

    context.actionStack.undo(context);
    // 只撤销一次，整个创建批次应消失，不能要求对每个时间点分别撤销。
    if ( !context.timelineRegistry.view<const MMM::Logic::TimelineComponent>()
              .empty() ) {
        XERROR("Timeline batch undo did not remove created events");
        return false;
    }

    context.actionStack.redo(context);
    // 旧 bpm 观察指针已因删除失效，重新查找后才能读取恢复出的组件。
    bpm = findTimeline(context, MMM::TimingEffect::BPM);
    if ( !bpm ) {
        XERROR("Timeline batch redo did not restore BPM event");
        return false;
    }
    const auto redoneBeat = timingMetadataValue(
        bpm->m_metadata, MMM::TimingMetadataType::MALODY, "beat");
    if ( !redoneBeat || *redoneBeat != "[8,1,4]" ) {
        // 按完整字符串比较，重做不能丢失分拍分子、分母或把元数据转成另一表示。
        XERROR("Timeline batch redo dropped metadata");
        return false;
    }
    return true;
}

/// @brief 验证批量更新合并为一次撤销并保留 Timing metadata。
/// @return 批量更新、撤销和重做均符合预期时返回 true。
/// @note 更新命令不显式提供 metadata，应保持原组件中的格式扩展信息。
bool testBatchUpdateIsAtomic()
{
    // 更新后元数据检查针对 BPM，SCROLL 在这个场景中没有非空 metadata。
    // “Atomic”在此仅指批量历史分组，测试未注入中途失败，也未建立并发写入场景。
    // 新上下文使创建和更新历史独立，第一次 undo
    // 应针对更新而不是其他场景的动作。
    MMM::Logic::SessionContext   context;
    MMM::Logic::ActionController controller(context);

    MMM::TimingMetadata metadata;
    metadata.timing_properties[MMM::TimingMetadataType::MALODY]["beat"] =
        "[4,0,1]";
    MMM::Logic::CmdCreateTimelineEvents createCommand;
    createCommand.events.push_back(
        { 5.0, MMM::TimingEffect::BPM, 120.0, metadata });
    createCommand.events.push_back({ 6.0, MMM::TimingEffect::SCROLL, 10000.0 });
    // 初始 SV 与 BPM 数值明显不同，能检查批量条目是否被错误套用同一个新值。
    controller.handleCommand(createCommand);

    const entt::entity bpmEntity =
        findTimelineEntity(context, MMM::TimingEffect::BPM);
    const entt::entity scrollEntity =
        findTimelineEntity(context, MMM::TimingEffect::SCROLL);
    if ( bpmEntity == entt::null || scrollEntity == entt::null ) {
        // 在使用 Registry::get 之前验证夹具创建成功，避免断言访问无效句柄。
        XERROR("Timeline batch update setup did not create entities");
        return false;
    }

    MMM::Logic::CmdUpdateTimelineEvents updateCommand;
    updateCommand.events.push_back({ bpmEntity, 5.0, 180.0 });
    // 使用已创建实体的句柄进行更新，不通过效果类型重新创建一批替代实体。
    updateCommand.events.push_back({ scrollEntity, 6.0, 10.0 });
    // 两项保持各自时间不变，只更新效果数值，隔离批量修改与元数据保留规则。
    controller.handleCommand(updateCommand);

    const auto& updatedBpm =
        context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
            bpmEntity);
    const auto& updatedScroll =
        context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
            scrollEntity);
    const auto updatedBeat = timingMetadataValue(
        updatedBpm.m_metadata, MMM::TimingMetadataType::MALODY, "beat");
    // beat 在本测试中是应原样保留的格式扩展，不能因 BPM 数值变化而清空它。
    // 读取执行后的真实组件，不检查 updateCommand
    // 本身，确保断言覆盖命令应用过程。
    if ( !near(updatedBpm.m_value, 180.0) ||
         !near(updatedScroll.m_value, 10.0) || !updatedBeat ||
         *updatedBeat != "[4,0,1]" ) {
        XERROR("Timeline batch update changed unexpected fields");
        // 元数据 optional
        // 的存在性已在解引用之前检查，丢字段应报告失败而非崩溃。
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

    // 若两项各压入一条历史，一次 undo 只恢复最后一项，这组双值断言会失败。
    // 不再提交任何新命令，直接重做应恢复同一批新值，而不是重新计算或重新创建事件。
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
        // 重做也只调用一次，不能用循环 redo 隐藏批次拆成多条动作的问题。
        return false;
    }
    return true;
}

/// @brief 验证 BPM 编辑与新增保速 SV 合并为一次撤销。
/// @return 新建、撤销和重做均同时处理 BPM 与 SV 时返回 true。
/// @note 本场景目标处没有 SV，联动操作需要同时包含一个更新和一个创建。
bool testBpmKeepSpeedCreatesSvAtomically()
{
    // 重做末尾只核对实体有效性与两项数值，不逐项重复初次执行的时间断言。
    // 初始创建本身是一条历史；后续 undo 应只撤销联动修改，不删除原来的 BPM。
    MMM::Logic::SessionContext   context;
    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdCreateTimelineEvent{
        5.0, MMM::TimingEffect::BPM, 120.0 });
    const entt::entity bpmEntity =
        findTimelineEntity(context, MMM::TimingEffect::BPM);
    if ( bpmEntity == entt::null ) return false;

    // 从 120 改为 180，传入保速倍率 2/3；本测试检查联动应用，不负责计算该倍率。
    // 同时将时间从 5 移到 6，新增 SV 必须跟随新时间而不是停留在旧 BPM 处。
    controller.handleCommand(MMM::Logic::CmdUpdateBpmWithKeepSpeedSv{
        .bpmEntity   = bpmEntity,
        .newTime     = 6.0,
        .newBpm      = 180.0,
        .scrollValue = 2.0 / 3.0,
    });
    // 传给命令的是确定的目标倍率，渲染投影和速度保持的像素效果不在此验证。
    const entt::entity scrollEntity =
        findTimelineEntity(context, MMM::TimingEffect::SCROLL);
    if ( scrollEntity == entt::null ) {
        // 查找新 SV 的身份供撤销后验证删除；不能只看 BPM 变化就认为联动完成。
        XERROR("BPM keep-speed command did not create an SV");
        return false;
    }
    const auto& bpm =
        context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
            bpmEntity);
    const auto& scroll =
        context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
            scrollEntity);
    // BPM 与 SV 的时间和值都检查，避免新增点存在但放错时刻或保留默认倍率。
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
        // BPM 存活且新增 SV 消失才正确，删除两个实体不是合法的联动撤销。
        return false;
    }

    // 撤销已删除新增 SV，不能继续使用前面缓存的 scroll 组件引用读取恢复结果。
    // 重做复用动作记录中的实体身份；检查原 scrollEntity
    // 再次有效而非随便找到一个 SV。
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
/// @note SV 与目标时间相差半微秒，覆盖近似同刻匹配，不要求原始 double
/// 完全相等。
bool testBpmKeepSpeedUpdatesSvAtomically()
{
    // 夹具只有一个目标 SV，不验证多个近邻候选之间的优先级选择。
    // 与新增分支不同，撤销时已有 SV 必须继续存在，并恢复其原值和原时刻。
    MMM::Logic::SessionContext          context;
    MMM::Logic::ActionController        controller(context);
    MMM::Logic::CmdCreateTimelineEvents createCommand;
    createCommand.events.push_back({ 5.0, MMM::TimingEffect::BPM, 120.0 });
    constexpr double NEAR_DESTINATION_SV_TIME = 7.0000005;
    // 该差值大于断言 near 的 1e-9，能检测撤销是否真实恢复了原始 SV 时间。
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

    /// @brief 检查本场景两个已知实体的时间与数值。
    /// @param bpmTime BPM 的预期秒时间。
    /// @param bpmValue BPM 的预期数值。
    /// @param scrollTime 同一 SV 实体的预期秒时间。
    /// @param scrollValue SV 的预期倍率。
    /// @pre 两个实体仍有效；本场景要求更新原实体，而非删旧建新。
    const auto fieldsMatch = [&](double bpmTime,
                                 double bpmValue,
                                 double scrollTime,
                                 double scrollValue) {
        // 执行、撤销与重做共用同一助手，三个阶段保持相同字段检查口径。
        // 每次调用重新从 Registry 读取，不捕获上一次更新前的组件副本。
        const auto& bpm =
            context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
                bpmEntity);
        const auto& scroll =
            context.timelineRegistry.get<const MMM::Logic::TimelineComponent>(
                scrollEntity);
        // 直接读取原 scrollEntity，能够识别错误地新增替代点而未更新原点的实现。
        return near(bpm.m_timestamp, bpmTime) && near(bpm.m_value, bpmValue) &&
               near(scroll.m_timestamp, scrollTime) &&
               near(scroll.m_value, scrollValue);
    };
    if ( !fieldsMatch(7.0, 240.0, 7.0, 0.5) ) {
        // 命中近似同刻 SV 后应对齐到精确目标 7.0，不能保留那半微秒偏差。
        XERROR("BPM keep-speed command did not update existing destination SV");
        return false;
    }
    context.actionStack.undo(context);
    if ( !fieldsMatch(5.0, 120.0, NEAR_DESTINATION_SV_TIME, 1.5) ) {
        // 撤销恢复原 BPM 和原 SV 的不同时间，不能把两者都设回同一个时刻。
        XERROR("One undo did not restore BPM and existing SV");
        return false;
    }
    context.actionStack.redo(context);
    return fieldsMatch(7.0, 240.0, 7.0, 0.5);
}

/// @brief 验证所有创建与修改命令都会拒绝负 BPM，并允许零 BPM。
/// @return 单项、批量与保速联动入口均保持非负 BPM 时返回 true。
/// @note 本测试针对命令层 BPM
/// 下界，不推广为其他数值入口或所有时间线效果的范围规则。
bool testNegativeBpmMutationsAreRejected()
{
    // 只测试有限负数和零，不覆盖 NaN、无穷或上界超限的其他数值策略。
    // 使用独立空会话覆盖命令层，不依赖 ImGui 或渲染快照。
    // ActionController 是这些 BPM
    // 编辑入口共享的命令处理边界，不在此执行磁盘保存。
    MMM::Logic::SessionContext   context;
    MMM::Logic::ActionController controller(context);

    // 单项创建是最底层入口；负 BPM 不应生成实体或撤销记录。
    // 该断言同时确保非法命令不会把会话标记成可撤销变更。
    controller.handleCommand(MMM::Logic::CmdCreateTimelineEvent{
        1.0, MMM::TimingEffect::BPM, -120.0 });
    // 先从空栈开始测非法单项创建，后续栈大小才能精确区分各次合法提交。
    // 空 Registry 证明动作未执行，而非执行后又被数值计算回退。
    // 空撤销栈证明非法输入未产生隐藏的无效操作。
    if ( findTimelineEntity(context, MMM::TimingEffect::BPM) != entt::null ||
         context.actionStack.getUndoStackSize() != 0U ) {
        XERROR("Single Timeline creation accepted a negative BPM");
        return false;
    }

    // 批量创建混合负数与零值；只应保留合法的零 BPM。
    // 零值覆盖命令层的非负下边界，避免实现误改为严格大于零。
    MMM::Logic::CmdCreateTimelineEvents createCommand;
    createCommand.events.push_back({ 2.0, MMM::TimingEffect::BPM, -1.0 });
    // -1 接近下界，与 -120 配合，避免仅针对特定非法数值的处理碰巧通过。
    createCommand.events.push_back({ 3.0, MMM::TimingEffect::BPM, 0.0 });
    // 混合合法与非法项验证按条目过滤，不要求一个非法项否决整个创建批次。
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
    // 先建立 120 的有效旧值，非法更新不能通过“原本就是默认值”碰巧保持正确。
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

    // 后面继续使用同一个合法 BPM，任何一次非法更新都可能污染后续检查基线。
    // 批量更新同样不得绕过约束；全非法批次应直接成为无操作。
    // 这覆盖时间线表格搜索替换最终下发的批量命令路径。
    MMM::Logic::CmdUpdateTimelineEvents updateCommand;
    updateCommand.events.push_back({ bpmEntity, 4.0, -60.0 });
    // 非法条目还试图移动时间，后续保速拒绝断言会检查原时间仍为 3.0。
    controller.handleCommand(updateCommand);
    // 全非法批次没有可执行条目；这里直接检查数值和撤销栈数量。
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
    // 从真实组件读取拒绝后的状态，不用命令对象中的 newTime/newBpm 代替结果。
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
    // 从正值改为零才是可观察的成功更新，直接重复写入零可能被无变化策略跳过。
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
/// @note markSaved 仅更新内存中的保存基线，不调用文件写入或验证磁盘内容。
/// @note 不可撤销的脏标记与 undo/redo 历史索引是不同状态，空栈也可以未保存。
bool testNonUndoableDirtyState()
{
    // 仅验证显式状态操作，不据此断言所有业务编辑命令都已正确发送脏通知。
    // 不实例化 ActionController，这个场景只检查动作栈自身的脏状态协议。
    MMM::Logic::SessionContext context;
    // 初始空栈应为清洁状态，后续标脏必须能与这个正向基线区分。
    if ( context.actionStack.isDirty() ) {
        XERROR("A new action stack was unexpectedly dirty");
        return false;
    }

    context.actionStack.markDirty();
    // 不推入可撤销命令，直接验证独立脏状态能参与 isDirty 判断。
    if ( !context.actionStack.isDirty() ) {
        // markDirty
        // 后仍清洁会让元数据修改遗漏保存提示，即使没有可撤销动作也应失败。
        XERROR("Non-undoable metadata changes were not marked dirty");
        return false;
    }

    context.actionStack.markSaved();
    // 不模拟文件保存成功或失败，只检查动作栈接受“已保存”通知后的状态。
    // 保存基线应清除不可撤销变更的脏标记，不能仅比较 undo 栈长度。
    if ( context.actionStack.isDirty() ) {
        XERROR("Saving did not clear non-undoable metadata changes");
        return false;
    }

    // 保存和清空是两条独立重置入口，再标脏一次才能确保 clear
    // 不是借用已清洁状态。
    context.actionStack.markDirty();
    context.actionStack.clear();
    // 清空后恢复新会话语义，不能继续保留旧项目的不可撤销变更标记。
    // 再次标脏后清空，覆盖会话关闭或重新载入时不能遗留未保存提示的语义。
    if ( context.actionStack.isDirty() ) {
        XERROR("Clearing the action stack kept non-undoable changes dirty");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行批量 Timeline 创建元数据测试。
/// @return 全部测试通过时返回 0。
/// @note 包含创建、更新、BPM/SV 联动、负值拒绝与独立脏状态，不只检查 metadata。
int main()
{
    // 日志区分夹具建立、命令执行和历史恢复阶段，整体退出码仍是最终通过依据。
    // 本测试无需真实 ImGui 上下文，元数据与动作恢复直接走共享逻辑入口。
    // 测试不接收外部路径，所有实体和元数据都在本次进程内按确定顺序构造。
    // 各场景独立创建上下文，短路失败不会影响下一次运行的动作栈初态。
    // 退出零才表示全部场景均已运行，不能只凭没有错误日志判断每项都执行过。
    return testBatchCreatePreservesMetadata() && testBatchUpdateIsAtomic() &&
                   testBpmKeepSpeedCreatesSvAtomically() &&
                   testBpmKeepSpeedUpdatesSvAtomically() &&
                   testNegativeBpmMutationsAreRejected() &&
                   testNonUndoableDirtyState()
               ? 0
               : 1;
}
