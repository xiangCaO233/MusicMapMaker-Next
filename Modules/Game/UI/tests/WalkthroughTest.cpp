#include "BuiltinWalkthrough.h"
#include "ComposeLessonFixture.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapCreateInteractionEvent.h"
#include "event/project/ProjectOpenInteractionEvent.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/walkthrough/ComposeLessonCatalog.h"
#include "ui/walkthrough/WalkthroughModel.h"
#include "ui/walkthrough/WalkthroughService.h"
#include <algorithm>
#include <array>
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
/// - 编辑区简介要求活动项目和已打开谱面两个入口条件；
/// - 编辑区简介包含谱面标签、三类轨道、批注、时间线和工具栏；
/// - 草稿、批注与 BGM 区分别拥有独立的完整可见拖动步骤；
/// - 批注拖动与说明位于 BGM 拖动与说明之前；
/// - 时间线说明位于工具栏说明之前；
/// - 音频管理依次覆盖资源列表、全局设置和底部入口；
/// - 编辑区简介每一步都提供可重放 guide；
/// - 谱面标签目标位于整条编辑区路线首步；
/// - 音频资源入口目标位于整条编辑区路线末步；
/// - 软件个性化主题属于独立章节且无需活动项目；
/// - 软件个性化步骤包含主题、双字体、界面美化、光标与快捷键；
/// - 已有满意设置允许通过确认推进，不强制改动用户偏好；
/// - 创作谱面静态主题保留稳定阶段四身份，分支从项目资产生成；
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
/// - 阶段二继续使用内置路线，阶段四仅在参考资产有效时解除占位；
/// - 新建谱面事件订阅随 Service 生命周期建立和解除；
/// - 所有事件断言在 service.update 后读取，覆盖真实跨线程队列边界；
/// - 服务公开主题顺序保持欢迎页使用的稳定索引；
/// - 阶段三简介与阶段四创作主题都要求项目和真实谱面标签；
/// - 阶段四按批注生成基础和进阶两个独立分支；
/// - 两个分支分别按首播、编辑、复播三步展开每个段落；
/// - 编辑阶段不能靠“知道了”跳过几何验收；
/// - 草稿载荷与正式物件按类型、轨道、时间和数量比较；
/// - 折线父级缓存差异可通过，子段路径差异仍被拒绝；
/// - 额外或漏掉的物件不能被相似位置的另一个物件抵扣；
/// - 阶段四在资产有效时解除 placeholder 并提供完整路线；
/// - 阶段四 order 严格晚于编辑区简介；
/// - 阶段四在无项目、无谱面两种状态下均不可进入；
/// - 阶段四仅在项目和谱面标签都存在时可进入；
/// - 阶段四每一步都声明可重放的 guide；
/// - 阶段四动态步骤 ID 与阶段三旧目标保持主题级隔离；
/// - PackageDrop 只有只读项目完成时才推进；
/// - BeatmapDrop 只有真正打开谱面时才推进；
/// - 未注册 action 保持无操作；
/// - 已注册 action 恰好执行一次；
/// - 销毁并重建服务后进度仍可恢复；
/// - 重置后的状态再次重建服务仍保持清除。
/// - 打开项目演练只允许测试配置根内的 CanonRock 目录及指定文件。
/// - 指定目录以文件系统身份比较，不能把配置根误当成项目目录；
/// - 未列入引导的其它谱面文件不能因为父目录相同而放行；
/// - 结束演练后所有路径重新走正常打开流程，限制不能泄漏到其它主题。
/// - 资源占位只在测试输出配置根下建立，不要求真实 CanonRock 压缩包；
/// - 文件系统身份检查使用存在的临时文件，避免不存在路径的假阳性。
/// - 两个新建谱面引导只在 CanonRock 项目根下可进入，普通主题不受影响。

/// @brief 验证分支隔离、每步手动了解、信号判定、配置校验和无窗口持久化。
/// @param argc 必须包含测试输出根目录参数。
/// @param argv argv[1] 为测试隔离输出根目录。
/// @return 0 表示全部通过，非零值标识具体失败场景。
int main(int argc, char** argv)
{
    using namespace MMM::UI::Walkthrough;
    // 持久化测试必须由 CTest 提供隔离目录。
    if ( argc != 2 ) return 1;
    // 路径门禁测试只在隔离配置根下创建资源占位，绝不接触个人配置。
    // CanonRock 目录来自和生产代码相同的 AppPaths 配置根解析入口。
    const auto canonRock = canonRockDirectory();
    if ( canonRock.string().find(std::filesystem::path(argv[1]).string()) !=
         0U )
        return 101;
    std::error_code pathError;
    // 只需制造真实文件系统身份，无需复制大型谱面或音频资源。
    std::filesystem::create_directories(canonRock, pathError);
    if ( pathError ) return 102;
    const auto beatmap = canonRock / "卡农-示例谱面.mmm";
    const auto package = canonRock / "canonrock.zip";
    if ( !MMM::UI::Test::writeComposeLessonFixture(canonRock) ) return 102;
    std::ofstream(package).put('x');
    if ( !std::filesystem::exists(beatmap) ||
         !std::filesystem::exists(package) )
        return 103;
    // 先启用限制，分别验证目录、谱面、谱包与两种不允许的路径。
    restrictOpenProjectGuideToCanonRock(true);
    const bool pathsAllowed =
        openProjectGuideAllows(canonRock) && openProjectGuideAllows(beatmap) &&
        openProjectGuideAllows(package) &&
        !openProjectGuideAllows(std::filesystem::path(argv[1])) &&
        !openProjectGuideAllows(canonRock / "another.mmm");
    // 最后显式退出演练，确保后续持久化和事件测试不被全局状态污染。
    restrictOpenProjectGuideToCanonRock(false);
    if ( !pathsAllowed ||
         !openProjectGuideAllows(std::filesystem::path(argv[1])) )
        return 104;
    // 入口可由配置副本进入，也可直接打开仓库源码中的正式示例。
    // 另一个目录里的同名谱面不能仅凭文件名绕过启动身份校验。
    // 覆盖开发者直接从仓库练习与发布包从配置资源练习两条路径。
    const auto unrelatedDirectory = std::filesystem::path(argv[1]) / "other";
    std::filesystem::create_directories(unrelatedDirectory, pathError);
    if ( pathError ) return 108;
    const auto unrelatedBeatmap = unrelatedDirectory / "卡农-示例谱面.mmm";
    std::ofstream(unrelatedBeatmap).put('x');
    if ( !canonRockComposeBeatmapAllows(beatmap) ||
         !canonRockComposeBeatmapAllows(
             std::filesystem::path(MMM_COMPOSE_SAMPLE_FILE)) ||
         canonRockComposeBeatmapAllows(unrelatedBeatmap) ||
         canonRockComposeBeatmapAllows({}) )
        return 109;
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
            R"({"id":"bad","title":"Bad","requires_beatmap":"true","placeholder":true})") ||
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
    // 解析器仍支持将未来主题声明为占位，但当前内置阶段都已经提供真实流程。
    const auto placeholder = parseTopic(
        R"({"id":"future","title":"Future","chapter":"creation","placeholder":true})");
    if ( !placeholder || !placeholder->m_placeholder ||
         !placeholder->m_branches.empty() ||
         placeholder->m_chapter != "creation" )
        return 24;
    // 核心教程采用任一分支完成目标，并保持五种打开项目路径。
    const auto topic = parseTopic(BUILTIN_WALKTHROUGH);
    if ( !topic || topic->m_branches.size() != 5 || !topic->m_anyBranch )
        return 2;
    for ( const auto& branch : topic->m_branches )
        for ( const auto& step : branch.m_steps )
            // 每个已发布的小步骤都提供“进入引导”所需的配置。
            if ( !step.m_guide ) return 32;
    // 菜单、快捷键唤起和全部最终打开步骤都必须由实际业务事件推进。
    // 拖放准备说明保留“知道了”，供用户先切到系统文件管理器。
    // 这也防止原生选择器尚未选定目录时把第二步错误地当成完成。
    for ( std::size_t index = 0; index < topic->m_branches.size(); ++index ) {
        const auto& steps = topic->m_branches[index].m_steps;
        if ( steps.size() != 2 ||
             steps[0].m_guide->m_requiresAction != (index < 2) ||
             !steps[1].m_guide->m_requiresAction )
            return 105;
    }
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
    // 编辑区简介是阶段三真实主题，只在项目和谱面标签同时存在时可进入。
    const auto editorOverviewTopic =
        parseTopic(BUILTIN_EDITOR_OVERVIEW_WALKTHROUGH);
    if ( !editorOverviewTopic || editorOverviewTopic->m_placeholder ||
         !editorOverviewTopic->m_requiresProject ||
         !editorOverviewTopic->m_requiresBeatmap ||
         editorOverviewTopic->m_order != 30 ||
         editorOverviewTopic->m_branches.size() != 1 ||
         editorOverviewTopic->m_branches.front().m_steps.size() != 14 ||
         topicAvailable(*editorOverviewTopic, false, false) ||
         topicAvailable(*editorOverviewTopic, true, false) ||
         !topicAvailable(*editorOverviewTopic, true, true) )
        return 61;
    const auto& editorSteps = editorOverviewTopic->m_branches.front().m_steps;
    for ( std::size_t index = 0; index < editorSteps.size(); ++index ) {
        if ( !editorSteps[index].m_guide ) return 63;
        // 除入口外，每一步只依赖紧邻前一步，防止后续插入区域时再次越级。
        // 这同时验证批注介绍结束后只能进入 BGM 完整可见步骤。
        // 严格单链还保证新增步骤不会绕开既有草稿区或后续音频介绍。
        if ( index > 0 &&
             editorSteps[index].m_prerequisites !=
                 std::vector<std::string>{ editorSteps[index - 1].m_id } )
            return 64;
    }
    // 批注与 BGM 各自先完成完整可见拖动，再进入对应的只读介绍步骤。
    // 时间线仍须位于工具栏之前，后续音频管理顺序保持不变。
    if ( editorSteps[4].m_id != "reveal-annotation" ||
         editorSteps[4].m_guide->m_targets !=
             std::vector<std::string>{ "editor.canvas.pan-annotation" } ||
         editorSteps[5].m_id != "annotation-area" ||
         editorSteps[6].m_id != "reveal-bgm" ||
         editorSteps[6].m_guide->m_targets !=
             std::vector<std::string>{ "editor.canvas.pan-bgm" } ||
         editorSteps[7].m_id != "bgm-area" ||
         editorSteps[8].m_id != "timeline" ||
         editorSteps[9].m_id != "toolbar" ||
         editorSteps.front().m_guide->m_targets !=
             std::vector<std::string>{ "editor.beatmap-tab" } ||
         editorSteps.back().m_guide->m_targets !=
             std::vector<std::string>{ "editor.audio.actions" } )
        return 62;
    // 创作主题从资产批注生成两条路线，不再承袭固定的二十步操作脚本。
    auto composeBeatmapTopic  = parseTopic(BUILTIN_COMPOSE_BEATMAP_WALKTHROUGH);
    const auto composeLessons = loadComposeLessons(beatmap);
    // 源码中实际分发的 CanonRock 草稿侧车也必须能生成两阶段路线。
    // 夹具通过不能代替打包资源有效，尤其是隐藏的 .mmm 工作目录。
    // 这里只读取源码资产；隔离配置根仍用于服务进度和练习夹具写入。
    // 最后一段必须标记进阶，防止只分发基础教学也误报通过。
    const auto packagedLessons =
        loadComposeLessons(std::filesystem::path(MMM_COMPOSE_SAMPLE_FILE));
    if ( !packagedLessons || packagedLessons->empty() ||
         packagedLessons->front().m_reference.empty() ||
         !packagedLessons->back().m_advanced )
        return 107;
    // 真实目录的删除段是辅助撤销按钮的截止点。
    // 此检查贯穿后续进阶段落，避免只在删除段本身隐藏按钮。
    // 标题仅用于找到边界；按钮状态必须来自加载时的段落次序。
    // 段落次序来自时间戳批注，而不是静态 JSON 的教程列表。
    // 资源装载会剥离进阶前缀，删除标题比较的是规范化结果。
    // 因此检查资源包内实际分发的谱面，覆盖配置同步后的入口数据。
    // 删除段之前应维持原放置教学的辅助入口，不改变已有设计。
    // 删除段本身必须关闭，否则用户可点击按钮跳过右键手势。
    // 最后一个进阶段仍关闭，保证后续分支不会重新打开辅助操作。
    const auto deletePosition =
        std::ranges::find_if(*packagedLessons, [](const ComposeLesson& lesson) {
            return lesson.m_title == "删除物件教学";
        });
    if ( deletePosition == packagedLessons->end() ||
         deletePosition == packagedLessons->begin() ||
         std::any_of(packagedLessons->begin(),
                     deletePosition,
                     [](const ComposeLesson& lesson) {
                         return !lesson.m_allowUndoButton;
                     }) ||
         std::any_of(deletePosition,
                     packagedLessons->end(),
                     [](const ComposeLesson& lesson) {
                         return lesson.m_allowUndoButton;
                     }) )
        return 176;
    // 路线入口会用独立基线还原主轨，资源必须可被正式谱面读取器加载。
    // 首段初始为空，重练时才会要求重新绘制这六枚 Note。
    // 基线与可保存的示例谱面分开存放，后者在用户练习后可能发生变化。
    // 文件存在但解析成空谱面也必须失败，不能让入口清空全部目标物件。
    // 后续调整课程需要预置物件，所以基线总体必须包含正式 Note。
    // 入口只替换正式对象域，本测试聚焦替换命令的真实来源内容。
    // 首段空白是每次重新练习单键放置的起点，不受其它段落预置项影响。
    const auto baseline = MMM::BeatMap::loadFromFile(
        std::filesystem::path(MMM_COMPOSE_SAMPLE_FILE).parent_path() / ".mmm" /
        "compose_baseline.mmm");
    if ( baseline.m_baseMapMetadata.track_count != 4 ||
         baseline.m_allNotes.empty() ||
         std::any_of(baseline.m_allNotes.begin(),
                     baseline.m_allNotes.end(),
                     [&](const auto& note) {
                         const double timeMs = note.get().m_timestamp;
                         return timeMs >= packagedLessons->front().m_beginMs &&
                                timeMs <= packagedLessons->front().m_endMs;
                     }) )
        return 113;
    // 分发谱面的草稿有五列、主画布有四列；右侧四列对应主轨道 0–3。
    // 首段的六枚 Note 覆盖全部四条主轨，防止直接比较草稿编码而卡住练习。
    // 用实际资源验证，隔离夹具单独通过不能证明用户看到的谱面可继续。
    constexpr std::array<int, 6> firstLessonTracks{ 0, 2, 1, 3, 1, 2 };
    if ( packagedLessons->front().m_reference.size() !=
         firstLessonTracks.size() )
        return 111;
    for ( std::size_t index = 0; index < firstLessonTracks.size(); ++index )
        if ( packagedLessons->front().m_reference[index].track !=
             firstLessonTracks[index] )
            return 112;
    // 第四段使用真实草稿折线：父级缓存可因画笔收尾方式不同而变化，
    // 但任何子段长度变化都必须继续阻止进入复播。
    if ( packagedLessons->size() < 4 ||
         packagedLessons->at(3).m_reference.empty() ||
         packagedLessons->at(3).m_reference.front().subNotes.empty() )
        return 114;
    // 保留其它折线原样，专门验证单条折线父级缓存不会阻断整段验收。
    auto polylineGeometry = packagedLessons->at(3).m_reference;
    polylineGeometry.front().duration += 0.3;
    polylineGeometry.front().dtrack += 1;
    if ( !matchesComposeLessonNotes(packagedLessons->at(3), polylineGeometry) )
        return 115;
    // 子段多出 10 ms 已越过 2 ms 容差，路径变化仍须被完整比较捕获。
    polylineGeometry.front().subNotes.front().duration += 0.01;
    if ( matchesComposeLessonNotes(packagedLessons->at(3), polylineGeometry) )
        return 116;
    // 静态声明刻意是占位；只有真实批注和草稿载荷同时可读才生成分支。
    // 此处使用隔离夹具，不能依赖仓库中正在编辑的个人教学谱面。
    if ( !composeBeatmapTopic || !composeBeatmapTopic->m_placeholder ||
         !composeBeatmapTopic->m_requiresProject ||
         !composeBeatmapTopic->m_requiresBeatmap ||
         composeBeatmapTopic->m_order != 40 || !composeLessons ||
         composeLessons->size() != 5 ||
         topicAvailable(*composeBeatmapTopic, false, false) ||
         topicAvailable(*composeBeatmapTopic, true, false) ||
         !topicAvailable(*composeBeatmapTopic, true, true) )
        return 65;
    populateComposeLessonTopic(*composeBeatmapTopic, *composeLessons);
    if ( composeBeatmapTopic->m_placeholder ) return 73;
    // 隔离夹具同样采用五列草稿对应四列主轨，首列参考必须归一到零。
    if ( composeLessons->front().m_reference.front().track != 0 ) return 110;
    // 一次物件可视上接近但轨道或数量不对时，不能替代完整草稿目标。
    // 不同比较场景复用同一目标，确保容差不会意外放宽类型和数量约束。
    // 此处不涉及 UI 帧，几何比较仅在测试中显式调用。
    // 几何结果在每次变体后重置，断言失败能归因到单一字段。
    auto geometry = composeLessons->front().m_reference;
    if ( !matchesComposeLessonNotes(composeLessons->front(), geometry) )
        return 69;
    geometry.front().track += 1;
    if ( matchesComposeLessonNotes(composeLessons->front(), geometry) )
        return 70;
    geometry = composeLessons->front().m_reference;
    geometry.front().timestamp += 0.01;
    if ( matchesComposeLessonNotes(composeLessons->front(), geometry) )
        return 71;
    geometry = composeLessons->front().m_reference;
    geometry.push_back(geometry.front());
    if ( matchesComposeLessonNotes(composeLessons->front(), geometry) )
        return 72;
    // 错误提示必须与最终验收使用同一套一对一几何语义：正确物件不标红，
    // 多出的重叠物件仍为错误，遗漏目标仍有待操作框。
    // 匹配结果按资源参考和实际数组的原始下标输出，方便画布逐项定位。
    geometry = composeLessons->front().m_reference;
    const auto completeFeedback =
        compareComposeLessonNotes(composeLessons->front(), geometry, 17, 9);
    // 示例谱面实例令牌必须原样保留，供画布拒绝其它同路径标签页。
    // 正确完成时不应再有任何目标框或错误框。
    if ( completeFeedback.beatmapInstanceId != 17 ||
         completeFeedback.composeNoteRevision != 9 ||
         std::ranges::find(completeFeedback.expectedMatched, false) !=
             completeFeedback.expectedMatched.end() ||
         std::ranges::find(completeFeedback.actualMatched, false) !=
             completeFeedback.actualMatched.end() )
        return 117;
    geometry.push_back(geometry.front());
    const auto duplicateFeedback =
        compareComposeLessonNotes(composeLessons->front(), geometry, 17, 9);
    // 两个完全重叠的实际 Note 只能消费一份参考；多出的那个标红。
    // 数量错误不能靠渲染时合并同位置矩形来掩盖。
    if ( std::ranges::count(duplicateFeedback.actualMatched, false) != 1 )
        return 118;
    geometry.front().track += 1;
    const auto wrongTrackFeedback =
        compareComposeLessonNotes(composeLessons->front(), geometry, 17, 9);
    // 另一个重叠 Note 仍能覆盖参考；移到错误轨道的那颗独立标红。
    // 匹配算法不能因第一份实际物件先失败就放弃搜索后续对象。
    if ( std::ranges::count(wrongTrackFeedback.expectedMatched, false) != 0 ||
         std::ranges::count(wrongTrackFeedback.actualMatched, false) != 1 )
        return 119;
    // 移动段先排除完整匹配，再给不同位置的同结构物件分配独占目标。
    // 目标与实际枚举顺序相反，避免误把数组相同下标当作配对依据。
    // 两枚物件故意使用相同类型，单靠 NoteType 无法确定配对。
    // 两者仅偏移时间，必须显示蓝色目的地而非一键删除按钮。
    // 期望下标恰好与实际顺序交叉，能够抓出按索引直连的错误实现。
    // 这里没有 ECS 实体，比较器只应依赖捕获的几何和目录阶段。
    // 最近优先配对还须消费目标，不能让第二枚重复指向第一枚。
    ComposeLesson moveLesson;
    moveLesson.m_title     = "拖拽移动教学";
    moveLesson.m_reference = {
        { .type = ::MMM::NoteType::NOTE, .timestamp = 1.0, .track = 0 },
        { .type = ::MMM::NoteType::NOTE, .timestamp = 1.3, .track = 1 },
    };
    auto movedNotes = moveLesson.m_reference;
    std::swap(movedNotes[0], movedNotes[1]);
    movedNotes[0].timestamp += 0.08;
    movedNotes[1].timestamp += 0.05;
    const auto moveFeedback =
        compareComposeLessonNotes(moveLesson, movedNotes, 17, 10);
    // 完整验收仍为 false；配对不能把未到达目标的音符标成已完成。
    // 反馈保留实例令牌和修订号，画布后续据此拒绝陈旧红框。
    if ( moveFeedback.repairKind != ComposeLessonRepairKind::Move ||
         moveFeedback.repairTargetForActual != std::vector<int>{ 1, 0 } ||
         std::ranges::count(moveFeedback.actualMatched, true) != 0 ||
         moveFeedback.showUndoButton )
        return 170;
    // 滑键只调整箭头终轨；根时间和起始轨保持一致才能建立配对。
    // 从第一轨向第三轨的目标与当前向第二轨的箭头共享同一头部。
    // 仅改有向轨差即成为尾部修正候选，不应要求创建新的 Flick。
    // 此段没有额外子物件，目标端点可由根轨道加 dtrack 确定。
    ComposeLesson flickLesson;
    flickLesson.m_title     = "滑键拖拽调整教学";
    flickLesson.m_reference = {
        { .type      = ::MMM::NoteType::FLICK,
          .timestamp = 2.0,
          .track     = 0,
          .dtrack    = 2 },
    };
    auto wrongFlick           = flickLesson.m_reference;
    wrongFlick.front().dtrack = 1;
    const auto flickFeedback =
        compareComposeLessonNotes(flickLesson, wrongFlick, 17, 11);
    // 这个配对必须禁止一键撤销，让用户亲自拖动现有箭头。
    if ( flickFeedback.repairKind != ComposeLessonRepairKind::FlickTail ||
         flickFeedback.repairTargetForActual != std::vector<int>{ 0 } ||
         flickFeedback.showUndoButton )
        return 171;
    wrongFlick.front().track = 1;
    // 修改头轨后尾部拖动已无法修复整个几何；不应画误导性蓝点。
    // 对这种不兼容的根部错误只保留红框，供用户辨认其它操作需求。
    // 该断言防止“任何 Flick 都配到最近 Flick”的过度宽松提示。
    if ( compareComposeLessonNotes(flickLesson, wrongFlick, 17, 12)
             .repairTargetForActual != std::vector<int>{ -1 } )
        return 172;
    // 长条尾端与删除段同样不应提供一键撤销，但删除没有蓝色目标。
    // 长度从 0.6 秒缩到 0.3 秒时头部仍正确，唯一动作是拖动 HoldEnd。
    // 参考尾时间应由起点加持续时间计算，不能取长条矩形中心。
    ComposeLesson holdLesson;
    holdLesson.m_title     = "长条拖拽调整教学";
    holdLesson.m_reference = {
        { .type      = ::MMM::NoteType::HOLD,
          .timestamp = 3.0,
          .duration  = 0.6,
          .track     = 2 },
    };
    auto shortHold             = holdLesson.m_reference;
    shortHold.front().duration = 0.3;
    const auto holdFeedback =
        compareComposeLessonNotes(holdLesson, shortHold, 17, 13);
    // 当前长条仍须标红，直到用户把尾部调整到参考持续时间。
    if ( holdFeedback.repairKind != ComposeLessonRepairKind::HoldTail ||
         holdFeedback.repairTargetForActual != std::vector<int>{ 0 } ||
         holdFeedback.showUndoButton )
        return 173;
    ComposeLesson deleteLesson;
    deleteLesson.m_title = "删除物件教学";
    // 删除段没有草稿目标，不能为多余正式物件生成蓝色落点。
    // 红框仍存在，让用户知道应对哪个物件执行右键删除。
    const auto deleteFeedback =
        compareComposeLessonNotes(deleteLesson, shortHold, 17, 14);
    if ( deleteFeedback.repairKind != ComposeLessonRepairKind::Delete ||
         deleteFeedback.repairTargetForActual != std::vector<int>{ -1 } ||
         deleteFeedback.showUndoButton )
        return 174;
    // 删除之后即使再次遇到普通放置类型，也只提示，不自动提供删除按钮。
    // 用独立开关模拟加载器的顺序继承，避免后续标题被误认成早期放置段。
    // 保留非空参考可证明按钮开关与目标是否存在无关。
    moveLesson.m_title           = "后续普通教学";
    moveLesson.m_allowUndoButton = false;
    if ( compareComposeLessonNotes(moveLesson, movedNotes, 17, 15)
             .showUndoButton )
        return 175;
    // 后续进阶段即使包含未匹配的 Note，也只能由用户自行调整或删除。
    if ( composeBeatmapTopic->m_branches.size() != 2 ||
         composeBeatmapTopic->m_branches[0].m_steps.size() != 12 ||
         composeBeatmapTopic->m_branches[1].m_steps.size() != 3 )
        return 66;
    // 每段都是首播、练习、复播三步；任何一步都不能靠手动确认完成。
    // 前置依赖在分支内相邻连接，进阶首步不依赖基础末步。
    for ( const auto& branch : composeBeatmapTopic->m_branches )
        for ( std::size_t index = 0; index < branch.m_steps.size(); ++index ) {
            const auto& step = branch.m_steps[index];
            if ( !step.m_guide || !step.m_composeLesson ||
                 !step.m_guide->m_requiresAction ||
                 step.m_guide->m_targets !=
                     std::vector<std::string>{
                         index % 3 == 1 ? "compose.lesson.practice"
                                        : "compose.lesson.playback" } )
                return 67;
            if ( index > 0 &&
                 step.m_prerequisites != std::vector<std::string>{
                                             branch.m_steps[index - 1].m_id } )
                return 68;
        }
    // 软件个性化独立于项目和谱面，可从欢迎页直接进入。
    // 每步聚焦真实设置控件；保留现有偏好时允许明确确认继续。
    // 此流程不会修改磁盘设置，解析检查仅保证内置路线完整可达。
    // 不要求动作的阶段仍需有效目标，保证提示对应具体界面入口。
    const auto personalizationTopic =
        parseTopic(BUILTIN_SOFTWARE_PERSONALIZATION_WALKTHROUGH);
    if ( !personalizationTopic || personalizationTopic->m_placeholder ||
         personalizationTopic->m_chapter != "personalization" ||
         personalizationTopic->m_requiresProject ||
         personalizationTopic->m_requiresBeatmap ||
         personalizationTopic->m_order != 50 ||
         personalizationTopic->m_branches.size() != 1 ||
         !topicAvailable(*personalizationTopic, false, false) )
        return 69;
    constexpr std::array personalizationTargets{
        // 首步连接主侧栏，用户不需要先打开任何编辑器标签页。
        "personalization.sidebar.settings",
        // 常规分组中的主题和两种字体系不同配置键，逐项指向控件。
        "personalization.settings.theme",
        "personalization.settings.font-ascii",
        "personalization.settings.font-cjk",
        // 单个步骤定位整个美化分组，六项设置在同一片亮区展示。
        "personalization.settings.aesthetics",
        // 光标与美化分属可折叠分组，目标仍使用稳定语义标识。
        "personalization.settings.cursor",
        // 最后进入独立快捷键页并定位到一个可实际录制的绑定。
        "personalization.settings.shortcut-tab",
        "personalization.settings.shortcut-move",
    };
    const auto& personalizationSteps =
        personalizationTopic->m_branches.front().m_steps;
    if ( personalizationSteps.size() != personalizationTargets.size() )
        return 70;
    // 顺序由前置步骤形成单链，避免欢迎页展示无法完成的分支。
    for ( std::size_t index = 0; index < personalizationSteps.size();
          ++index ) {
        const auto& step = personalizationSteps[index];
        // 同一步只高亮一个语义目标，设置的可选性质由引导标志体现。
        // 首步没有前置条件，其余步骤必须紧跟上一操作。
        if ( !step.m_guide || step.m_guide->m_requiresAction ||
             step.m_guide->m_targets !=
                 std::vector<std::string>{ personalizationTargets[index] } ||
             (index > 0 && step.m_prerequisites !=
                               std::vector<std::string>{
                                   personalizationSteps[index - 1].m_id }) )
            return 71;
    }
    // 编辑器个性化必须与当前谱面的轨道数和可见组件关联。
    // 各步骤允许用户自行确认，不能要求拖到某个固定坐标才放行。
    // 与软件个性化不同，本主题在没有打开谱面时不得呈现为可执行。
    // order 必须排在软件主题之后，欢迎页才能稳定显示既有顺序。
    // 主标题的章节 ID 同时决定欢迎页卡片归属，不能混进创作章节。
    const auto editorPersonalizationTopic =
        parseTopic(BUILTIN_EDITOR_PERSONALIZATION_WALKTHROUGH);
    if ( !editorPersonalizationTopic ||
         editorPersonalizationTopic->m_chapter != "personalization" ||
         !editorPersonalizationTopic->m_requiresProject ||
         !editorPersonalizationTopic->m_requiresBeatmap ||
         editorPersonalizationTopic->m_order != 60 ||
         editorPersonalizationTopic->m_branches.size() != 1 )
        return 72;
    constexpr std::array editorPersonalizationTargets{
        // 先选择布局工具和了解边界，再逐项介绍实际可见组件。
        // 入口目标属于工具栏，画布目标来自当前谱面渲染快照。
        "personalization.editor.layout-tool",
        "personalization.editor.canvas-range",
        "personalization.editor.component.judgment-time",
        "personalization.editor.component.beat-number",
        "personalization.editor.component.beat-line-time",
        "personalization.editor.component.spectrum",
        "personalization.editor.component.kps",
        // 主轨道、草稿区和 BGM 区是独立的调整目标。
        // 这些 ID 不共用画布大框，否则会掩盖具体调整对象。
        "personalization.editor.judgment-line",
        "personalization.editor.player-lanes",
        "personalization.editor.draft-lanes",
        "personalization.editor.bgm-lanes",
        "personalization.editor.note-size",
    };
    const auto& editorPersonalizationSteps =
        editorPersonalizationTopic->m_branches.front().m_steps;
    if ( editorPersonalizationSteps.size() !=
         editorPersonalizationTargets.size() )
        return 73;
    // 每个目标均应有一个独立步骤，否则用户无法分别确认满意位置。
    // 单链前置条件同时阻止跳过尚未阅读的可调范围说明。
    // 最后一项仍需保留明确的 Note 尺寸目标以支持空谱面回退。
    for ( std::size_t index = 0; index < editorPersonalizationSteps.size();
          ++index ) {
        const auto& step = editorPersonalizationSteps[index];
        // 一个目标对应一个用户可自行结束的阶段，顺序不能被多目标抢占。
        // 特别不能误设 requires_action；个性化没有唯一正确的完成坐标。
        // 此处检查配置语义，不用修改真实谱面或个人视觉配置。
        if ( !step.m_guide || step.m_guide->m_requiresAction ||
             step.m_guide->m_targets !=
                 std::vector<std::string>{
                     editorPersonalizationTargets[index] } ||
             (index > 0 &&
              step.m_prerequisites !=
                  std::vector<std::string>{
                      editorPersonalizationSteps[index - 1].m_id }) )
            return 74;
    }
    // 后续原有内置主题断言继续执行，防止新增章节影响创作路线。
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
        // 编辑区简介占据阶段三，完整创作路线位于阶段四。
        // 个性化主题排在现有主题之后，保持旧测试与导航索引稳定。
        // 新主题作为第八张内置卡片，不能覆盖已完成主题的进度键。
        // 相同章节内按 order 排序，注册顺序不应隐式决定阅读顺序。
        if ( topics.size() != 8 || service.chapters().size() != 2 ||
             topics[0].m_id != "mmm.open-project" ||
             topics[1].m_id != "mmm.create-project" ||
             topics[2].m_id != "mmm.create-beatmap" ||
             topics[3].m_id != "mmm.create-beatmap-template" ||
             topics[4].m_id != "mmm.editor-overview" ||
             topics[5].m_id != "mmm.compose-beatmap" ||
             topics[6].m_id != "mmm.software-personalization" ||
             topics[7].m_id != "mmm.editor-personalization" ||
             topics[0].m_order != topics[1].m_order ||
             topics[1].m_order >= topics[2].m_order ||
             topics[2].m_order != topics[3].m_order ||
             topics[3].m_order >= topics[4].m_order ||
             topics[4].m_order >= topics[5].m_order ||
             topics[5].m_order >= topics[6].m_order ||
             topics[6].m_order >= topics[7].m_order )
            return 25;
        // 空白和模板入口共用 CanonRock 项目门禁，错误项目及无项目都不能启动。
        // 普通项目主题仍由原有项目条件决定，不继承这一教学限制。
        if ( !topicAvailableInProject(topics[2], true, false, canonRock) ||
             !topicAvailableInProject(topics[3], true, false, canonRock) ||
             topicAvailableInProject(topics[2], true, false, directory) ||
             topicAvailableInProject(topics[3], true, false, beatmap) ||
             topicAvailableInProject(topics[2], false, false, canonRock) ||
             !topicAvailableInProject(topics[1], true, false, directory) )
            return 105;
        // 创作教程只要求打开谱面，页面在开始步骤时核对示例文件身份。
        // 不能因为同一谱面通过临时工作区打开就禁用欢迎页入口。
        if ( !topicAvailableInProject(topics[5], true, true, directory) ||
             !topicAvailableInProject(topics[5], true, true, canonRock) ||
             topicAvailableInProject(topics[5], true, false, canonRock) ||
             topicAvailableInProject(topics[5], false, true, canonRock) )
            return 106;
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
        // 路线重放不能依赖“是否首次完成”：同一业务动作再次发生时，即使
        // 持久化步骤早已完成，也必须产生更大的易失信号序号供本轮引导衔接。
        const auto firstOpenRevision =
            service.latestSignalRevision(beatmapTopic.m_branches[0].m_steps[0]);
        MMM::Event::EventBus::instance().publish(beatmapEvent);
        service.update();
        if ( firstOpenRevision == 0 ||
             service.latestSignalRevision(
                 beatmapTopic.m_branches[0].m_steps[0]) <= firstOpenRevision )
            return 60;
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
