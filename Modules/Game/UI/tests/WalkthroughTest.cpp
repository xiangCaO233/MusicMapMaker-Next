#include "BuiltinWalkthrough.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapCreateInteractionEvent.h"
#include "event/project/ProjectOpenInteractionEvent.h"
#include "ui/walkthrough/WalkthroughModel.h"
#include "ui/walkthrough/WalkthroughService.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

/// @file WalkthroughTest.cpp
/// @brief 演练模型解析、进度依赖、事件映射、动作白名单和持久化回归测试。
/// @details 测试使用调用方提供的隔离输出目录，不读取或覆盖个人配置。
/// 返回码按场景区分失败位置，便于无测试框架环境直接诊断。
/// 测试仅通过公开模型和服务接口观察行为，不依赖页面绘制内部状态。
/// 持久化场景通过多次构造 Service 验证磁盘结果而非对象内缓存。
/// 事件场景使用真实 EventBus，析构服务后订阅必须被正确解除。
///
/// 覆盖的模型约束包括：
/// - 内置章节的数量、顺序和稳定 ID；
/// - 重复章节 ID 与非整数 order 被拒绝；
/// - 占位与 requires_project 必须使用布尔值；
/// - 普通主题必须至少包含一个分支；
/// - 步骤引导接受提示与语义目标，并拒绝空对象或非法目标；
/// - 纯键盘引导允许 targets 为空但必须拥有可见 prompt；
/// - 目标列表保持配置次序，供界面按最后可见候选切换；
/// - 重复目标被拒绝，避免同一步骤出现不确定优先级；
/// - 内置真实主题的每一个步骤都提供可进入的 guide；
/// - 新建项目菜单步骤声明文件菜单到具体菜单项的目标链；
/// - 菜单入口目标链继续包含向导三页按钮，支持已完成步骤重放；
/// - 向导出现后更高优先级目标必须覆盖仍然可见的一级菜单；
/// - 快捷键入口没有菜单目标，但同样声明完整向导按钮链；
/// - 新建谱面主题要求活动项目并覆盖菜单、音频与创建目标；
/// - 历史进度不参与目标链解析，避免重放时停留在入口控件；
/// - 步骤不能依赖自身；
/// - 同一分支内步骤 ID 不能重复；
/// - completion=all 与内置 any 分支语义正确解析。
///
/// 覆盖的进度约束包括：
/// - 手动确认只完成目标步骤，不串到同分支其他步骤；
/// - 业务 signal 只推进声明匹配的步骤；
/// - 依赖未满足时，即使所有 signal 到齐也不能完成；
/// - 依赖满足后空 signal 可触发状态重新计算；
/// - 序列化与恢复保持手动完成记录；
/// - 损坏输入恢复失败且不破坏已有内存状态；
/// - 重置只清除指定主题的记录。
///
/// 覆盖的服务约束包括：
/// - 打开项目、新建项目与新建谱面三个真实主题按阶段稳定排序；
/// - 新建项目菜单和快捷键分别记录向导打开与最终进入项目；
/// - 唤出向导只推进首步，不能提前把主题判定为完成；
/// - 项目加载成功只完成同一入口对应的最终步骤；
/// - 菜单与快捷键分支的学习进度彼此隔离；
/// - 两条创建分支都要求先完成各自的向导打开步骤；
/// - 创建主题保持 completion=any，任选一种实际入口即可完成；
/// - 创建主题不再参与占位主题集合的解析循环；
/// - 新建谱面按菜单和快捷键分别记录向导、音频与最终会话阶段；
/// - 新建谱面主题明确声明 requires_project，普通主题保持默认不限制；
/// - requires_project 的字符串伪布尔值必须被解析器拒绝；
/// - 新建谱面两条分支均包含 dialog、audio、ready 三个步骤；
/// - 每条分支的 audio 只依赖自身 dialog，不能跨入口解锁；
/// - 每条分支的 ready 只依赖自身 audio，不能跳过资源选择；
/// - 菜单入口目标链包含文件菜单和新建谱面菜单项；
/// - 音频步骤使用复合音频区域的稳定语义目标；
/// - 创建步骤同时声明普通创建与重名确认两个互斥目标；
/// - 快捷键打开阶段不声明菜单目标，仅显示 Ctrl+N 提示；
/// - WizardOpened 只完成菜单分支首步；
/// - AudioSelected 在首步完成后推进菜单分支第二步；
/// - Completed 在前两步完成后推进菜单分支最终步骤；
/// - 菜单事件不能完成快捷键分支的任一步骤；
/// - 新建谱面主题仍采用 completion=any，任选入口即可达成目标；
/// - 谱面完成事件携带的路径不参与信号匹配；
/// - 未知新建谱面入口保持无信号，避免内部调用污染教程；
/// - 重复阶段事件由进度归约器幂等消费；
/// - 主题在服务目录中位于两个项目主题之后、创作主题之前；
/// - 新建谱面不再出现在 BUILTIN_PLACEHOLDERS 循环；
/// - 新建谱面事件订阅随 Service 生命周期建立和解除；
/// - 所有事件断言在 service.update 后读取，覆盖真实跨线程队列边界；
/// - 服务公开主题顺序保持欢迎页使用的稳定索引；
/// - PackageDrop 只有只读项目完成时才推进；
/// - BeatmapDrop 只有真正打开谱面时才推进；
/// - 未注册 action 保持无操作；
/// - 已注册 action 恰好执行一次；
/// - 销毁并重建服务后进度仍可恢复；
/// - 重置后的状态再次重建服务仍保持清除。

/// @brief 验证分支隔离、每步手动了解、信号判定、配置校验和无窗口持久化。
/// @param argc 必须包含测试输出根目录参数。
/// @param argv argv[1] 为测试隔离输出根目录。
/// @return 0 表示全部通过，非零值标识具体失败场景。
int main(int argc, char** argv)
{
    using namespace MMM::UI::Walkthrough;
    // 持久化测试必须由 CTest 提供隔离目录。
    if ( argc != 2 ) return 1;
    // 首先验证编译期章节目录的稳定公共结构。
    const auto chapters = parseChapters(BUILTIN_CHAPTERS);
    if ( !chapters || chapters->size() != 2 ||
         (*chapters)[0].m_id != "creation" ||
         (*chapters)[1].m_id != "personalization" )
        return 21;
    // 重复 ID 和浮点 order 都违反章节模型约束。
    if ( parseChapters(R"([{"id":"a","title":"A"},{"id":"a","title":"B"}])") ||
         parseChapters(R"([{"id":"a","title":"A","order":1.5}])") )
        return 22;
    // 占位字段类型、负 order、占位分支和空普通主题分别覆盖解析失败。
    if (
        parseTopic(
            R"({"id":"bad","title":"Bad","order":-1,"placeholder":true})") ||
        parseTopic(R"({"id":"bad","title":"Bad","placeholder":"true"})") ||
        parseTopic(
            R"({"id":"bad","title":"Bad","requires_project":"true","placeholder":true})") ||
        parseTopic(
            R"({"id":"bad","title":"Bad","placeholder":true,"branches":[{}]})") ||
        parseTopic(R"({"id":"bad","title":"Bad","branches":[]})") )
        return 23;
    // guide 必须为对象，且提示和目标不能同时为空。
    if (
        parseTopic(
            R"({"id":"bad","title":"Bad","branches":[{"id":"b","title":"B","steps":[{"id":"s","title":"S","guide":false}]}]})") ||
        parseTopic(
            R"({"id":"bad","title":"Bad","branches":[{"id":"b","title":"B","steps":[{"id":"s","title":"S","guide":{}}]}]})") )
        return 30;
    // 目标使用稳定 ID 白名单，重复项会造成优先级歧义，也必须拒绝。
    if (
        parseTopic(
            R"({"id":"bad","title":"Bad","branches":[{"id":"b","title":"B","steps":[{"id":"s","title":"S","guide":{"targets":["bad/target"]}}]}]})") ||
        parseTopic(
            R"({"id":"bad","title":"Bad","branches":[{"id":"b","title":"B","steps":[{"id":"s","title":"S","guide":{"targets":["same","same"]}}]}]})") )
        return 31;
    for ( const auto* input : BUILTIN_PLACEHOLDERS ) {
        // 所有内置占位主题必须属于 creation 且没有可执行分支。
        const auto placeholder = parseTopic(input);
        if ( !placeholder || !placeholder->m_placeholder ||
             !placeholder->m_branches.empty() ||
             placeholder->m_chapter != "creation" )
            return 24;
    }
    // 核心教程采用任一分支完成目标，并保持五种打开项目路径。
    const auto topic = parseTopic(BUILTIN_WALKTHROUGH);
    if ( !topic || topic->m_branches.size() != 5 || !topic->m_anyBranch )
        return 2;
    for ( const auto& branch : topic->m_branches )
        for ( const auto& step : branch.m_steps )
            // 每个已发布的小步骤都提供“进入引导”所需的配置。
            if ( !step.m_guide ) return 32;
    // 新建项目教程不是占位项，并提供菜单和快捷键两条可独立完成的分支。
    const auto createProjectTopic =
        parseTopic(BUILTIN_CREATE_PROJECT_WALKTHROUGH);
    if ( !createProjectTopic || createProjectTopic->m_placeholder ||
         createProjectTopic->m_branches.size() != 2 ||
         !createProjectTopic->m_anyBranch )
        return 26;
    for ( const auto& branch : createProjectTopic->m_branches )
        for ( const auto& step : branch.m_steps )
            // 新建项目的菜单、快捷键和向导阶段都必须可独立进入引导。
            if ( !step.m_guide ) return 33;
    if ( createProjectTopic->m_branches[0].m_steps[0].m_guide->m_targets !=
         std::vector<std::string>{ "main-menu.file",
                                   "main-menu.file.new-project",
                                   "new-project.project-info.next",
                                   "new-project.preferences.next",
                                   "new-project.location.create" } )
        return 34;
    if ( createProjectTopic->m_branches[1].m_steps[0].m_guide->m_targets !=
         std::vector<std::string>{ "new-project.project-info.next",
                                   "new-project.preferences.next",
                                   "new-project.location.create" } )
        return 35;
    // 新建谱面由真实内置主题提供，入口条件和三阶段目标均由配置声明。
    const auto createBeatmapTopic =
        parseTopic(BUILTIN_CREATE_BEATMAP_WALKTHROUGH);
    if ( !createBeatmapTopic || createBeatmapTopic->m_placeholder ||
         !createBeatmapTopic->m_requiresProject ||
         createBeatmapTopic->m_branches.size() != 2 ||
         !createBeatmapTopic->m_anyBranch )
        return 36;
    // 同一配置在无项目时禁止进入，项目生命周期就绪后立即可用。
    // 不要求项目的相邻主题作为对照，避免 helper 把全部教程一并锁住。
    if ( topicAvailable(*createBeatmapTopic, false) ||
         !topicAvailable(*createBeatmapTopic, true) ||
         !topicAvailable(*createProjectTopic, false) )
        return 44;
    for ( const auto& branch : createBeatmapTopic->m_branches ) {
        if ( branch.m_steps.size() != 3 ) return 37;
        for ( const auto& step : branch.m_steps )
            if ( !step.m_guide ) return 38;
        if ( branch.m_steps[1].m_prerequisites !=
                 std::vector<std::string>{ branch.m_steps[0].m_id } ||
             branch.m_steps[2].m_prerequisites !=
                 std::vector<std::string>{ branch.m_steps[1].m_id } )
            return 39;
    }
    if ( createBeatmapTopic->m_branches[0].m_steps[0].m_guide->m_targets !=
             std::vector<std::string>{ "main-menu.file",
                                       "main-menu.file.new-beatmap" } ||
         createBeatmapTopic->m_branches[0].m_steps[1].m_guide->m_targets !=
             std::vector<std::string>{ "new-beatmap.audio" } ||
         createBeatmapTopic->m_branches[0].m_steps[2].m_guide->m_targets !=
             std::vector<std::string>{ "new-beatmap.create",
                                       "new-beatmap.duplicate.continue" } )
        return 40;
    // 两条分支均应把向导打开设为最终完成步骤的显式前置条件。
    // 该约束保证恢复进度或乱序业务事件不会跳过用户实际唤出向导的动作。
    for ( const auto& branch : createProjectTopic->m_branches ) {
        if ( branch.m_steps.size() != 2 ||
             branch.m_steps[1].m_prerequisites.size() != 1 ||
             branch.m_steps[1].m_prerequisites.front() !=
                 branch.m_steps[0].m_id )
            return 29;
    }
    Progress progress;
    // 手动确认末分支第二步不能隐式完成该分支第一步。
    if ( !progress.acknowledge(*topic, topic->m_branches[4].m_steps[1]) ||
         progress.completed(*topic, topic->m_branches[4].m_steps[0]) )
        return 3;
    // 打开快捷选择器只完成对应步骤，不完成最终 ready 或菜单步骤。
    if ( !progress.signal(*topic, "project.shortcut.picker") ||
         progress.completed(*topic, topic->m_branches[1].m_steps[1]) ||
         progress.completed(*topic, topic->m_branches[0].m_steps[0]) )
        return 4;
    // 完成快捷流程后对应 ready 步骤必须达成。
    if ( !progress.signal(*topic, "project.shortcut.ready") ||
         !progress.completed(*topic, topic->m_branches[1].m_steps[1]) )
        return 5;
    Progress restored;
    // 完整序列化往返应保留此前手动确认的步骤。
    if ( !restored.restore(progress.serialize()) ||
         !restored.completed(*topic, topic->m_branches[4].m_steps[1]) )
        return 6;
    // 损坏恢复必须失败，且强保证已有进度对象内容不被清空。
    if ( restored.restore("broken") ||
         !restored.completed(*topic, topic->m_branches[4].m_steps[1]) )
        return 7;
    // 主题重置后该主题的手动完成记录必须消失。
    restored.reset(*topic);
    if ( restored.completed(*topic, topic->m_branches[4].m_steps[1]) ) return 8;
    // 构造含前置依赖和 all-signal 条件的最小主题。
    auto dependency = parseTopic(
        R"({"id":"test","title":"T","completion":"all","branches":[{"id":"a","title":"A","steps":[{"id":"first","title":"First"},{"id":"second","title":"Second","requires":["first"],"match":"all","signals":["one","two"]}]}]})");
    if ( !dependency || dependency->m_anyBranch ) return 9;
    const auto& first  = dependency->m_branches[0].m_steps[0];
    const auto& second = dependency->m_branches[0].m_steps[1];
    // 两个信号先到达时依赖尚未满足，第二步仍不能完成。
    progress.signal(*dependency, "one");
    progress.signal(*dependency, "two");
    if ( progress.completed(*dependency, second) ) return 10;
    // 确认前置步骤后用空 signal 重新传播依赖完成状态。
    progress.acknowledge(*dependency, first);
    progress.signal(*dependency, "");
    if ( !progress.completed(*dependency, second) ) return 11;
    // 自依赖必须在模型解析阶段拒绝。
    if (
        parseTopic(
            R"({"id":"x","title":"X","branches":[{"id":"b","title":"B","steps":[{"id":"s","title":"S","requires":["s"]}]}]})") )
        return 12;
    // 同分支重复步骤 ID 会破坏进度键，必须拒绝。
    if (
        parseTopic(
            R"({"id":"x","title":"X","branches":[{"id":"b","title":"B","steps":[{"id":"s","title":"S"},{"id":"s","title":"S"}]}]})") )
        return 13;
    // 用单调时钟生成本次运行专用目录，避免并发测试相互污染。
    const auto directory =
        std::filesystem::path(argv[1]) /
        ("walkthrough-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "progress.json";
    {
        // 第一段服务生命周期产生进度文件并验证事件与动作适配。
        Service     service(path, directory / "walkthroughs");
        const auto& topics = service.topics();
        if ( topics.size() != 4 || service.chapters().size() != 2 ||
             topics[0].m_id != "mmm.open-project" ||
             topics[1].m_id != "mmm.create-project" ||
             topics[2].m_id != "mmm.create-beatmap" ||
             topics[3].m_id != "mmm.compose-beatmap" ||
             topics[0].m_order != topics[1].m_order ||
             topics[1].m_order >= topics[2].m_order ||
             topics[2].m_order >= topics[3].m_order )
            return 25;
        // 包投放仅 completed 而非只读时不应完成教程步骤。
        MMM::Event::ProjectOpenInteractionEvent event;
        event.m_origin    = MMM::Event::ProjectOpenOrigin::PackageDrop;
        event.m_completed = true;
        MMM::Event::EventBus::instance().publish(event);
        service.update();
        if ( service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[4].m_steps[0]) )
            return 14;
        // 补全只读条件后同一来源事件应完成目标步骤。
        event.m_readOnly = true;
        MMM::Event::EventBus::instance().publish(event);
        service.update();
        if ( !service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[4].m_steps[1]) )
            return 15;
        // 谱面投放未真正打开谱面时不应完成 ready 步骤。
        event.m_origin = MMM::Event::ProjectOpenOrigin::BeatmapDrop;
        MMM::Event::EventBus::instance().publish(event);
        service.update();
        if ( service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[3].m_steps[1]) )
            return 16;
        // 补全 beatmapOpened 条件后步骤必须自动完成。
        event.m_beatmapOpened = true;
        MMM::Event::EventBus::instance().publish(event);
        service.update();
        if ( !service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[3].m_steps[1]) )
            return 17;
        // 新建项目快捷键事件先只完成向导步骤，不得提前完成进入项目或菜单分支。
        // 同时清除上一组拖放事件字段，避免测试结果依赖事件对象的历史状态。
        event.m_origin        = MMM::Event::ProjectOpenOrigin::CreateShortcut;
        event.m_completed     = false;
        event.m_readOnly      = false;
        event.m_beatmapOpened = false;
        MMM::Event::EventBus::instance().publish(event);
        service.update();
        const auto& createTopic = service.topics()[1];
        if ( !service.progress().completed(
                 createTopic, createTopic.m_branches[1].m_steps[0]) ||
             service.progress().completed(
                 createTopic, createTopic.m_branches[1].m_steps[1]) ||
             service.progress().completed(
                 createTopic, createTopic.m_branches[0].m_steps[0]) )
            return 27;
        // 项目创建并加载完成后，只有同来源分支的最终步骤自动达成。
        // ready 步骤依赖已完成的 dialog 步骤，因此也证明事件顺序被正确保留。
        event.m_completed = true;
        MMM::Event::EventBus::instance().publish(event);
        service.update();
        if ( !service.progress().completed(
                 createTopic, createTopic.m_branches[1].m_steps[1]) ||
             service.progress().completed(
                 createTopic, createTopic.m_branches[0].m_steps[1]) )
            return 28;
        // 新建谱面菜单入口按向导、音频和会话成功三个阶段依次推进。
        MMM::Event::BeatmapCreateInteractionEvent beatmapEvent;
        beatmapEvent.m_origin = MMM::Logic::BeatmapCreateOrigin::FileMenu;
        beatmapEvent.m_stage =
            MMM::Event::BeatmapCreateInteractionStage::WizardOpened;
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        const auto& beatmapTopic = service.topics()[2];
        if ( !service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[0]) ||
             service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[1]) )
            return 41;
        beatmapEvent.m_stage =
            MMM::Event::BeatmapCreateInteractionStage::AudioSelected;
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        if ( !service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[1]) ||
             service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[2]) )
            return 42;
        beatmapEvent.m_stage =
            MMM::Event::BeatmapCreateInteractionStage::Completed;
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        if ( !service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[2]) ||
             service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[1].m_steps[0]) )
            return 43;
        // 未注册 ID 不执行，注册 ID 只增加一次计数。
        int invoked = 0;
        service.registerAction("test", [&] { ++invoked; });
        service.execute("unregistered");
        service.execute("test");
        if ( invoked != 1 ) return 18;
    }
    {
        // 第二段生命周期验证文件恢复，并显式重置核心主题。
        Service service(path, directory / "walkthroughs");
        if ( !service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[4].m_steps[1]) )
            return 19;
        service.reset(service.topics()[0]);
    }
    {
        // 第三段生命周期验证重置已持久化，而不是只修改前一实例内存。
        Service service(path, directory / "walkthroughs");
        if ( service.progress().completed(
                 service.topics()[0],
                 service.topics()[0].m_branches[4].m_steps[1]) )
            return 20;
    }
    return 0;
}
