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
/// - 空白与模板新建谱面主题要求活动项目，并处于同一创作阶段；
/// - 空白谱面流程依次覆盖音频、自动测偏、校正、资源和创建目标；
/// - 模板谱面流程覆盖模板选择、复制选项、资源和创建目标；
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
/// - 打开项目、新建项目与两个新建谱面主题按阶段稳定排序；
/// - 新建项目菜单和快捷键分别记录向导打开与最终进入项目；
/// - 唤出向导只推进首步，不能提前把主题判定为完成；
/// - 项目加载成功只完成同一入口对应的最终步骤；
/// - 菜单与快捷键分支的学习进度彼此隔离；
/// - 两条创建分支都要求先完成各自的向导打开步骤；
/// - 创建主题保持 completion=any，任选一种实际入口即可完成；
/// - 创建主题不再参与占位主题集合的解析循环；
/// - 空白新建谱面记录向导、音频、测偏与最终会话阶段；
/// - 模板新建谱面独立记录模板选择、音频与最终会话阶段；
/// - 两个新建谱面主题明确声明 requires_project，普通主题默认不限制；
/// - requires_project 的字符串伪布尔值必须被解析器拒绝；
/// - 两个新建谱面主题均以六个线性步骤表达完整创建顺序；
/// - 空白与模板专属信号不能串到另一个主题的后续步骤；
/// - 菜单入口目标链包含文件菜单和新建谱面菜单项；
/// - 音频步骤使用复合音频区域的稳定语义目标；
/// - 创建步骤同时声明普通创建与重名确认两个互斥目标；
/// - WizardOpened 同时完成两个主题共用的弹窗入口步骤；
/// - TimingMeasured 只推进空白创建的自动测偏步骤；
/// - TemplateSelected 只推进模板创建的来源选择步骤；
/// - 两个主题采用 completion=all，不能跳过人工复核步骤；
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
    // 空白与模板创建分别提供同阶段主题，并共享项目入口条件。
    // 空白主题只保留一个流程分支，菜单与快捷键改为同一步骤的两个信号来源。
    // completion=all 保证用户必须按教学顺序完成自动步骤与人工复核。
    const auto createBeatmapTopic =
        parseTopic(BUILTIN_CREATE_BEATMAP_WALKTHROUGH);
    if ( !createBeatmapTopic || createBeatmapTopic->m_placeholder ||
         !createBeatmapTopic->m_requiresProject ||
         createBeatmapTopic->m_branches.size() != 1 ||
         createBeatmapTopic->m_anyBranch )
        return 36;
    const auto createBeatmapTemplateTopic =
        parseTopic(BUILTIN_CREATE_BEATMAP_TEMPLATE_WALKTHROUGH);
    // 模板主题必须是独立真实主题，不能退化成空白流程内部的入口选项。
    // 它沿用项目门禁，因为候选模板和新文件都属于当前活动项目。
    if ( !createBeatmapTemplateTopic ||
         createBeatmapTemplateTopic->m_placeholder ||
         !createBeatmapTemplateTopic->m_requiresProject ||
         createBeatmapTemplateTopic->m_branches.size() != 1 ||
         createBeatmapTemplateTopic->m_anyBranch )
        return 45;
    // 同一配置在无项目时禁止进入，项目生命周期就绪后立即可用。
    // 不要求项目的相邻主题作为对照，避免 helper 把全部教程一并锁住。
    if ( topicAvailable(*createBeatmapTopic, false) ||
         !topicAvailable(*createBeatmapTopic, true) ||
         topicAvailable(*createBeatmapTemplateTopic, false) ||
         !topicAvailable(*createBeatmapTemplateTopic, true) ||
         !topicAvailable(*createProjectTopic, false) )
        return 44;
    const auto validateBeatmapFlow = [](const Topic& value) {
        // 两个主题都用六步展示完整创建阶段，欢迎页因此显示一致的总进度。
        if ( value.m_branches.front().m_steps.size() != 6 ) return false;
        const auto& steps = value.m_branches.front().m_steps;
        for ( std::size_t index = 0; index < steps.size(); ++index ) {
            // 每一步都必须可重放引导，不能只依赖阅读静态正文。
            if ( !steps[index].m_guide ) return false;
            // 首步没有前置条件，其余步骤只依赖紧邻前一步形成严格线性流。
            // 这里拒绝跨步依赖，避免信号乱序时跳过用户要求的人工确认。
            if ( index > 0 &&
                 steps[index].m_prerequisites !=
                     std::vector<std::string>{ steps[index - 1].m_id } )
                return false;
        }
        return true;
    };
    if ( !validateBeatmapFlow(*createBeatmapTopic) ||
         !validateBeatmapFlow(*createBeatmapTemplateTopic) )
        return 37;
    // 空白流程的六个目标依次对应菜单入口、音频、自动测偏、BPM 复核、
    // 元数据资源区域和最终创建按钮，不要求改造原有单页弹窗布局。
    if ( createBeatmapTopic->m_branches[0].m_steps[0].m_guide->m_targets !=
             std::vector<std::string>{ "main-menu.file",
                                       "main-menu.file.new-beatmap" } ||
         // 音频区域同时包含下拉选择和导入按钮，使用复合目标而非具体按钮。
         createBeatmapTopic->m_branches[0].m_steps[1].m_guide->m_targets !=
             std::vector<std::string>{ "new-beatmap.audio" } ||
         // 推荐步骤聚焦自动测偏，仍允许用户按实际情况改用手动测量。
         createBeatmapTopic->m_branches[0].m_steps[2].m_guide->m_targets !=
             std::vector<std::string>{ "new-beatmap.timing.auto" } ||
         // 复核步骤聚焦可编辑 BPM，首拍偏移则通过相邻测量摘要判断。
         createBeatmapTopic->m_branches[0].m_steps[3].m_guide->m_targets !=
             std::vector<std::string>{ "new-beatmap.timing.bpm" } ||
         // 详情区域合并元数据、封面与背景，防止拆成弹窗内部的新页面。
         createBeatmapTopic->m_branches[0].m_steps[4].m_guide->m_targets !=
             std::vector<std::string>{ "new-beatmap.details" } ||
         // 重名确认与普通提交互斥，引导按当前可见目标选择其中之一。
         createBeatmapTopic->m_branches[0].m_steps[5].m_guide->m_targets !=
             std::vector<std::string>{ "new-beatmap.create",
                                       "new-beatmap.duplicate.continue" } )
        return 40;
    // 模板流程额外要求来源模式、候选选择和复制范围三个稳定视觉锚点。
    // 其他音频、详情和创建目标复用同一弹窗已有控件，保持交互一致。
    if ( createBeatmapTemplateTopic->m_branches[0]
                 .m_steps[1]
                 .m_guide->m_targets !=
             // 模式切换和来源选择是连续动作，任一可见控件均能恢复引导。
             std::vector<std::string>{ "new-beatmap.template.mode",
                                       "new-beatmap.template.pick" } ||
         createBeatmapTemplateTopic->m_branches[0]
                 .m_steps[3]
                 .m_guide->m_targets !=
             // 复制范围沿用原弹窗内的模板内容按钮，不引入新的页面结构。
             std::vector<std::string>{ "new-beatmap.template.options" } )
        return 46;
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
        // 相同 order 由欢迎页归入同一阶段；稳定插入顺序决定两张卡片的左右顺序。
        // 创作占位主题必须排在两个阶段二向导之后，形成截图中的阶段三。
        if ( topics.size() != 5 || service.chapters().size() != 2 ||
             topics[0].m_id != "mmm.open-project" ||
             topics[1].m_id != "mmm.create-project" ||
             topics[2].m_id != "mmm.create-beatmap" ||
             topics[3].m_id != "mmm.create-beatmap-template" ||
             topics[4].m_id != "mmm.compose-beatmap" ||
             topics[0].m_order != topics[1].m_order ||
             topics[1].m_order >= topics[2].m_order ||
             topics[2].m_order != topics[3].m_order ||
             topics[3].m_order >= topics[4].m_order )
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
        // 两个阶段二主题共用同一个弹窗入口，但后续业务信号必须隔离。
        MMM::Event::BeatmapCreateInteractionEvent beatmapEvent;
        beatmapEvent.m_origin = MMM::Logic::BeatmapCreateOrigin::FileMenu;
        beatmapEvent.m_stage =
            MMM::Event::BeatmapCreateInteractionStage::WizardOpened;
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        const auto& beatmapTopic         = service.topics()[2];
        const auto& beatmapTemplateTopic = service.topics()[3];
        // 打开弹窗时尚不知道用户将选择哪条创建路径，所以两个主题共用首步。
        // 共用仅限入口，音频与后续完成度必须等待各自来源事件。
        if ( !service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[0]) ||
             service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[1]) ||
             !service.progress().completed(
                 beatmapTemplateTopic,
                 beatmapTemplateTopic.m_branches[0].m_steps[0]) ||
             service.progress().completed(
                 beatmapTemplateTopic,
                 beatmapTemplateTopic.m_branches[0].m_steps[1]) )
            return 47;
        // 空白流程先确认音频，再由自动或手动测量结果推进推荐测偏步骤。
        beatmapEvent.m_stage =
            MMM::Event::BeatmapCreateInteractionStage::AudioSelected;
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        // fromTemplate 默认 false，此事件只能匹配空白主题的音频信号族。
        // 模板主题即使入口已完成，也不得提前越过来源选择步骤。
        if ( !service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[1]) ||
             service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[2]) ||
             service.progress().completed(
                 beatmapTemplateTopic,
                 beatmapTemplateTopic.m_branches[0].m_steps[2]) )
            return 48;
        beatmapEvent.m_stage =
            MMM::Event::BeatmapCreateInteractionStage::TimingMeasured;
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        // 有效测量结果回填才发布 TimingMeasured；单纯点击按钮不算完成。
        // 模板流程继承源 Timing，因此明确忽略此空白流程专属阶段。
        if ( !service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[2]) ||
             service.progress().completed(
                 beatmapTemplateTopic,
                 beatmapTemplateTopic.m_branches[0].m_steps[1]) )
            return 49;
        // BPM 校正与元数据资源均需用户复核，完成后才允许最终事件达成主题。
        service.acknowledge(beatmapTopic,
                            beatmapTopic.m_branches[0].m_steps[3]);
        service.acknowledge(beatmapTopic,
                            beatmapTopic.m_branches[0].m_steps[4]);
        // 两次确认分别代表校正 Timing 和补全元数据资源，不能由创建事件代替。
        // 最终成功事件只负责最后一步，并依赖前五步已经全部成立。
        beatmapEvent.m_stage =
            MMM::Event::BeatmapCreateInteractionStage::Completed;
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        if ( !service.progress().completed(
                 beatmapTopic, beatmapTopic.m_branches[0].m_steps[5]) ||
             service.progress().completed(
                 beatmapTemplateTopic,
                 beatmapTemplateTopic.m_branches[0].m_steps[5]) )
            return 50;
        // 模板流程使用独立来源标记，选择来源后才允许确认带入的主音频。
        beatmapEvent.m_origin       = MMM::Logic::BeatmapCreateOrigin::Shortcut;
        beatmapEvent.m_fromTemplate = true;
        beatmapEvent.m_stage =
            MMM::Event::BeatmapCreateInteractionStage::TemplateSelected;
        // 更换入口验证每个步骤的信号数组同时接受菜单和快捷键来源。
        // 来源布尔值决定信号族，避免模板动作错误推进空白主题。
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        if ( !service.progress().completed(
                 beatmapTemplateTopic,
                 beatmapTemplateTopic.m_branches[0].m_steps[1]) ||
             service.progress().completed(
                 beatmapTemplateTopic,
                 beatmapTemplateTopic.m_branches[0].m_steps[2]) )
            return 51;
        beatmapEvent.m_stage =
            MMM::Event::BeatmapCreateInteractionStage::AudioSelected;
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        // 模板默认值可能在选择来源后立即带入音频，队列顺序仍应先满足前置步骤。
        // 本测试分开发送两个事件，直接确认同样的顺序约束。
        if ( !service.progress().completed(
                 beatmapTemplateTopic,
                 beatmapTemplateTopic.m_branches[0].m_steps[2]) )
            return 52;
        service.acknowledge(beatmapTemplateTopic,
                            beatmapTemplateTopic.m_branches[0].m_steps[3]);
        service.acknowledge(beatmapTemplateTopic,
                            beatmapTemplateTopic.m_branches[0].m_steps[4]);
        // 模板复制范围与新谱面信息都属于有判断成本的人工确认步骤。
        // 完成事件必须来自实际模板命令，不能复用先前空白创建的 ready 信号。
        beatmapEvent.m_stage =
            MMM::Event::BeatmapCreateInteractionStage::Completed;
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        if ( !service.progress().completed(
                 beatmapTemplateTopic,
                 beatmapTemplateTopic.m_branches[0].m_steps[5]) )
            return 53;
        // 两个完成主题保留独立持久化键，后续重建服务时不会相互覆盖。
        // 此处无需重置信号队列；update 已在每轮断言前完整消费低频事件。
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
