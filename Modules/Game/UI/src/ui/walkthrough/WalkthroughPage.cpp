#include "ui/walkthrough/WalkthroughPage.h"

#include "audio/AudioManager.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "common/walkthrough/ComposeLessonNotes.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/markdown/MarkdownImageCache.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/ComposeLessonCatalog.h"
#include "ui/walkthrough/WalkthroughModel.h"
#include "ui/walkthrough/WalkthroughService.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include <algorithm>
#include <imgui.h>
#include <iterator>
#include <string>

/// @file WalkthroughPage.cpp
/// @brief 欢迎页演练主题正文、分支卡片和步骤操作的即时模式绘制。
/// @details 页面层遵循以下职责边界：
/// - 主题、章节和步骤内容来自已验证的 Walkthrough 模型；
/// - 完成状态、依赖可用性和持久化由 Walkthrough::Service 管理；
/// - Markdown 文本交给共享渲染器；
/// - 文档图片通过 UIManager 注册的 MarkdownImageCache 预热；
/// - 页面只保存当前主题、唯一展开分支和图片准备身份；
/// - 数据文件中的 action 只能调用服务预先注册的可信函数。
///
/// 绘制层级约定：
/// - 主题标题和整体说明位于正文顶部；
/// - 每个操作分支使用独立的自动高度卡片；
/// - 卡片标题行本身是透明的全宽反馈按钮；
/// - 左侧圆点表达分支完成状态；
/// - 右侧徽标表达已完成步骤数；
/// - 展开区域依次绘制步骤标题、Markdown 正文和操作按钮；
/// - 页面底部绘制目标完成提示、重置入口及服务错误。
///
/// 状态约定：
/// - 主题切换时默认展开第一分支；
/// - 再次点击已展开分支会折叠全部内容；
/// - 任一时刻至多展开一个分支；
/// - 已完成步骤不能重复手动确认；
/// - 自动与手动完成使用相同完成判定但展示不同来源；
/// - 有 action 的步骤必须同时满足依赖和注册条件才能执行；
/// - 重置只清理当前主题，不影响其他主题；
/// - 占位主题不渲染分支、进度或重置入口。
///
/// 突出引导约定：
/// - 每条分支显示一个“进入引导”，启动后按步骤顺序走完整条路线；
/// - 当前分支按钮切换为“结束引导”，避免用户无法主动退出；
/// - 引导启动时冻结目标 ID 与本地化提示，不跨帧借用模型字符串；
/// - 业务信号或突出目标完成后自动衔接同分支下一项 guide；
/// - 自动衔接只跳过没有 guide 的步骤，不跳过历史已完成步骤；
/// - 持久化完成度只用于徽标，本轮路线拥有独立的易失进度；
/// - 分支耗尽后停止突出层，不回退到先前已经操作的控件；
/// - 折叠、切换分支或更换主题都会结束旧引导；
/// - 欢迎标签被谱面标签遮住时，WelcomeView 仍每帧调用 updateGuide 续租；
/// - Spotlight 自身不持有页面指针，路线身份仍由本对象管理。
/// - 当前步骤仅以主题、分支和步骤 ID 标识，不缓存容器迭代器；
/// - 业务信号使用进程内单调序号，只接受当前步骤启动后的新事件；
/// - 已完成路线仍允许完整重放，以便用户重复熟悉全部操作；
/// - 目标暂不可见时进入等待态，等待菜单或向导在后续帧出现。
/// - 每次切换步骤都会重取信号基线，上一阶段的晚到事件不能跨步推进；
/// - 同帧目标完成与业务信号同时到达时只执行一次顺序衔接；
/// - Completed 仅表示步骤结束，只有路线耗尽或显式退出才调用 stop。
///
/// 配置与控件边界：
/// - 页面只解释 guide 的流程，不知道具体控件类型或所在窗口；
/// - 控件通过 UIManager 的 Spotlight 使用稳定语义 ID 上报矩形；
/// - target 列表顺序由配置作者定义，页面不猜测菜单或向导状态；
/// - 没有目标的键盘及外部应用步骤仍显示 prompt；
/// - JSON 不能通过突出层执行控件动作或注入输入；
/// - 路线步骤完成会同步为手动进度；已有业务事件的自动来源仍由 Progress 保留。
///
/// 性能约定：
/// - 每帧只遍历当前主题的分支和步骤；
/// - 不在绘制循环中读取教程文件；
/// - 图片准备只在主题或语言变化时提交；
/// - 进度文件只在确认、重置或业务事件改变状态时保存；
/// - 所有尺寸使用缓存的内容缩放比例；
/// - 长标题通过裁剪与悬停提示处理，不测量额外布局；
/// - ImGui 样式、字体、ID、缩进和子窗口必须严格成对恢复。
///
/// 项目上下文约定：
/// - 页面只读取 UIManager 已归约的生命周期状态，不直接观察逻辑线程项目指针；
/// - 项目切换或关闭会停止当前 Spotlight，并禁用操作与进入引导按钮；
/// - 正文和手动“已了解”仍可阅读与使用，不把环境门禁误作知识前置依赖。
///
/// CanonRock 创作路线约定：
/// - 批注段落展开为首播、练习和复播三个业务步骤；
/// - 切步时绑定当前非 Logo 会话与它的发布缓冲；
/// - 两处正式示例资源的文件身份决定目标会话；
/// - 每次进入基础或进阶路线时从独立基线还原正式主轨道物件；
/// - 草稿侧车、时间线、谱面元数据和其它标签不参与基线还原；
/// - 快照路径可能为相对路径，启动时使用注册表中的绝对文件身份；
/// - 欢迎页获得焦点不代表谱面 Session 不存在；启动时查找已打开的匹配标签；
/// - 多个候选中优先沿用活动标签，避免用户正在查看另一份示例时突然切换；
/// - 找到目标后通过工作区聚焦请求显示画布，才可上报可视聚光灯目标；
/// - 后续帧只比较已绑定键，不访问文件系统；
/// - 预览先暂停、定位，再播放，防止旧位置直接满足终点；
/// - 练习先暂停并定位，让草稿参考停在同一时间窗；
/// - 正式物件变化只提供修订号，不在 UI 直接锁会话扫描 ECS；
/// - 逻辑线程一次性查询段落物件，再发布结果给 UI；
/// - 结果修订落后于最新快照时丢弃，不沿用旧几何；
/// - 练习检查段内全部物件与草稿参考，不接受手动跳步；
/// - 同一对象修订只排一次查询，避免高帧率 UI 堆积逻辑命令；
/// - 页面销毁或主动退出后，尚在队列中的查询结果仍有共享所有权；
/// - 查询只读取正式轨道根物件，草稿仍由目录持有的参考提供；
/// - 复播再次从段首开始，播到终点才接续下一段；
/// - 退出或切换项目时停播并清理旧目标身份；
/// - 基础与进阶分支独立导航，不隐式串到另一分支。

namespace MMM::UI
{
/// @brief 页面退出时恢复引导期间的音频状态和项目路径限制。
WalkthroughPage::~WalkthroughPage()
{
    // 欢迎页通常先调用 stopGuide；析构仍兜底处理直接销毁页面的路径。
    if ( m_draftAreaMutedBeforeGuide.has_value() )
        Audio::AudioManager::instance().setDraftKeySoundAreaMuted(
            *m_draftAreaMutedBeforeGuide);
    Walkthrough::restrictOpenProjectGuideToCanonRock(false);
}

/// @brief 启动一个配置步骤的突出引导。
/// @param manager 提供 Spotlight、服务和当前语言。
/// @param topic 当前路线主题。
/// @param branch 当前路线分支。
/// @param step 必须包含 guide 的目标步骤。
/// @param reviewing 是否显式返回回看，不能被历史完成状态立即推走。
void WalkthroughPage::startGuide(UIManager*                 manager,
                                 const Walkthrough::Topic&  topic,
                                 const Walkthrough::Branch& branch,
                                 const Walkthrough::Step& step, bool reviewing)
{
    if ( !step.m_guide ) return;
    // 页面按钮之外的重放入口也必须重新检查项目身份。
    // 若用户在菜单或弹窗操作中切换了项目，不能启动指向别的项目的遮罩。
    if ( !Walkthrough::topicAvailableInProject(
             topic,
             manager->hasActiveProjectUiState() &&
                 !manager->isProjectTransitionInProgress(),
             manager->hasOpenBeatmapEditor(),
             manager->getActiveProjectRoot()) )
        return;
    m_guideError.clear();
    const auto& language =
        Config::AppConfig::instance().getEditorSettings().language;
    // 创作教程只控制当前 CanonRock 示例谱面；禁止把播放命令误发给其它标签。
    // 这些身份查询只发生在步骤交接时，不进入每帧高亮路径。
    std::shared_ptr<Logic::BeatmapSession>                composeSession;
    std::shared_ptr<Common::Render::RenderSnapshotBuffer> composeBuffer;
    std::shared_ptr<::MMM::BeatMap>                       composeBaseline;
    const Common::Render::RenderSnapshot* composeSnapshot = nullptr;
    std::string                           composeBeatmapKey;
    int32_t                               composeSessionIndex = -1;
    if ( step.m_composeLesson ) {
        // 欢迎页取得焦点后，活动 Session 不一定还是示例谱面；从已打开的
        // 标签中找精确的文件身份，优先使用此前活动的那个匹配项。
        // 列表仅在切步时复制，不能在每帧 UI 中等待注册表锁。
        auto&      engine  = Logic::EditorEngine::instance();
        const auto entries = engine.getSessionEntries();
        const auto allows  = [&](std::size_t index) {
            // 先排除空项和 Logo；这些项没有可播放的正式谱面。
            // 文件身份检查涉及磁盘，只能留在这个低频启动闭包内。
            return index < entries.size() && entries[index].session &&
                   !entries[index].isLogoPlaceholder &&
                   Walkthrough::canonRockComposeBeatmapAllows(
                       Config::utf8ToPath(entries[index].beatmapPathKey));
        };
        const auto activeIndex = engine.getActiveSessionIndex();
        if ( activeIndex >= 0 && allows(static_cast<std::size_t>(activeIndex)) )
            composeSessionIndex = activeIndex;
        else
            for ( std::size_t index = 0; index < entries.size(); ++index )
                if ( allows(index) ) {
                    composeSessionIndex = static_cast<int32_t>(index);
                    break;
                }
        if ( composeSessionIndex >= 0 ) {
            // 会话和相机来自同一注册表项，避免分别读取活动身份时发生错配。
            const auto& entry =
                entries[static_cast<std::size_t>(composeSessionIndex)];
            composeSession = entry.session;
            composeBuffer  = engine.getSyncBuffer(entry.cameraId);
            composeSnapshot =
                composeBuffer ? composeBuffer->getReadingSnapshot() : nullptr;
        }
        if ( !composeSession || !composeSnapshot ||
             !composeSnapshot->hasBeatmap ) {
            // 不启动遮罩，直接在欢迎页说明下一步应打开的资源。
            // 错误谱面被选中时必须保留编辑器正常输入。
            // 说明随设置语言显示，但谱面身份仍使用同一规范路径判定。
            m_guideError =
                Config::AppConfig::instance().getEditorSettings().language ==
                        "en_us"
                    ? "Open the CanonRock sample chart before starting the "
                      "composition walkthrough."
                    : "请先在 CanonRock "
                      "项目中打开《卡农-示例谱面.mmm》再进入创作引导。";
            return;
        }
        composeBeatmapKey = composeSnapshot->beatmapPathKey;
        if ( !branch.m_steps.empty() &&
             branch.m_steps.front().m_id == step.m_id ) {
            // 只在开始或重练路线时还原；段与段之间必须保留本轮已完成的修改。
            // 独立基线不受练习谱面保存影响，避免上次练习成为下次的标准答案。
            // 用户可能直接编辑源码示例，也可能编辑同步到配置目录的副本。
            // 因此基线跟随当前匹配的谱面目录，而不是硬编码其中一处路径。
            // 侧车目录不会被当成可编辑谱面标签，教学主画布和基线分离。
            // 此处只在路线入口读取文件，切到下一段时不再触发磁盘访问。
            const auto samplePath = Config::utf8ToPath(
                entries[static_cast<std::size_t>(composeSessionIndex)]
                    .beatmapPathKey);
            const auto baselinePath =
                samplePath.parent_path() / ".mmm" / "compose_baseline.mmm";
            auto baseline = ::MMM::BeatMap::loadFromFile(baselinePath);
            if ( baseline.m_allNotes.empty() ||
                 baseline.m_baseMapMetadata.track_count !=
                     composeSnapshot->trackCount ) {
                // 不完整基线不能用于整体替换；保留现有谱面并解释原因。
                // 轨道数不符时即使 Note 可解析，也无法保证后续教学位置正确。
                m_guideError =
                    language == "en_us"
                        ? "The CanonRock reference notes are missing or "
                          "invalid."
                        : "CanonRock 主轨道的原始示范物件缺失或无效。";
                return;
            }
            composeBaseline =
                std::make_shared<::MMM::BeatMap>(std::move(baseline));
        }
    }
    // 每次从分支入口重练打开项目，都先单独关闭旧项目；关闭确认仍可交互。
    // 关闭流程完成前不启动 Spotlight，也不排入新打开请求覆盖关闭意图。
    // 回看首步也遵守同一规则，不能保留上轮的活动项目继续演示打开。
    if ( topic.m_id == "mmm.open-project" && !branch.m_steps.empty() &&
         branch.m_steps.front().m_id == step.m_id &&
         manager->hasActiveProjectUiState() ) {
        // 已在别的步骤内返回首步时先释放遮罩，允许用户处理关闭确认。
        // 路径门禁也同步释放；关闭对话框可能访问最近项目和保存位置。
        // 待 UI 确认工作区清空后，才重新建立本轮引导的路径门禁。
        manager->walkthroughSpotlight().stop();
        m_activeGuide.reset();
        Walkthrough::restrictOpenProjectGuideToCanonRock(false);
        m_pendingOpenGuide = ActiveGuide{ .topicId   = topic.m_id,
                                          .branchId  = branch.m_id,
                                          .stepId    = step.m_id,
                                          .reviewing = reviewing };
        // 临时只读项目有专用关闭提示，不能走普通项目的关闭事件。
        if ( Logic::EditorEngine::instance().isTemporaryProjectOpen() )
            Event::EventBus::instance().publish(
                Event::TemporaryProjectClosePromptRequestedEvent{});
        else
            Event::EventBus::instance().publish(
                Event::ProjectCloseRequestedEvent{});
        return;
    }
    const auto& configuredPrompt = step.m_guide->m_prompt.get(language);
    // 路线顺序是前后导航的唯一依据，历史完成度不能删除回看入口。
    bool hasPrevious = false;
    // 没有 guide 的说明项不属于可执行导航节点，不能成为返回后的死路。
    for ( const auto& candidate : branch.m_steps ) {
        if ( candidate.m_id == step.m_id ) break;
        hasPrevious |= candidate.m_guide.has_value();
    }
    std::string prompt = configuredPrompt.empty() ? step.m_title.get(language)
                                                  : configuredPrompt;
    if ( topic.m_id == "mmm.open-project" ) {
        // 系统文件选择器不能由 ImGui 聚光灯定位，因此在提示中写出完整目录。
        // 此字符串只在步骤切换时生成，渲染热路径仅复用 Spotlight 的副本。
        // 绝对路径由配置根计算，兼容用户自定义的 MMM_CONFIG_ROOT。
        prompt += "\n";
        prompt += Config::pathToUtf8(Walkthrough::canonRockDirectory());
    }
    // 目标缺席或等待外部操作时也要给出明确的退出方式。
    prompt += "\n";
    prompt += TR("ui.walkthrough.escape_to_exit").toString();
    // 原生选择器阻塞 UI 时，唤起与打开成功事件可能在同一次 update 中到达。
    // 下一步骤的基线必须在进入分支时拍下，不能等选择器关闭才拍。
    // 基线采用服务内的递增修订号，已完成的旧练习不会被当成新事件。
    // 只为打开项目分支保留这个快照，其它演练仍按当前步骤启动时取样。
    // 文件夹或谱包拖放也可能在用户确认说明后立即完成，沿用同一契约。
    std::uint64_t nextSignalRevisionAtRunStart = 0;
    if ( topic.m_id == "mmm.open-project" && !branch.m_steps.empty() &&
         branch.m_steps.front().m_id == step.m_id ) {
        const auto next = std::find_if(std::next(branch.m_steps.begin()),
                                       branch.m_steps.end(),
                                       [](const Walkthrough::Step& candidate) {
                                           return candidate.m_guide.has_value();
                                       });
        if ( next != branch.m_steps.end() )
            nextSignalRevisionAtRunStart =
                manager->walkthroughService().latestSignalRevision(*next);
    }
    manager->walkthroughSpotlight().start(step.m_guide->m_targets,
                                          std::move(prompt),
                                          hasPrevious,
                                          reviewing,
                                          step.m_guide->m_requiresAction);
    m_activeGuide = ActiveGuide{
        .topicId  = topic.m_id,
        .branchId = branch.m_id,
        .stepId   = step.m_id,
        .signalRevisionAtStart =
            manager->walkthroughService().latestSignalRevision(step),
        .nextSignalRevisionAtRunStart = nextSignalRevisionAtRunStart,
        .reviewing                    = reviewing,
    };
    if ( !m_draftAreaMutedBeforeGuide.has_value() ) {
        // 区域总静音覆盖全部草稿轨道，不改写用户逐轨静音和音量。
        // 只在本轮首次进入时保存原值，切换教学步骤不重复覆盖。
        // 直接发布到线程安全控制库，避免命令被非活动谱面会话忽略。
        auto& audio                 = Audio::AudioManager::instance();
        m_draftAreaMutedBeforeGuide = audio.isDraftKeySoundAreaMuted();
        audio.setDraftKeySoundAreaMuted(true);
    }
    if ( step.m_composeLesson ) {
        // 会话队列按停播、定位、按阶段开播的顺序执行。
        // 练习阶段保持暂停，给用户稳定的拍位来绘制或调整物件。
        const auto& lesson            = *step.m_composeLesson;
        auto&       guide             = *m_activeGuide;
        guide.composeSession          = std::move(composeSession);
        guide.composeBuffer           = std::move(composeBuffer);
        guide.composeBeatmapKey       = std::move(composeBeatmapKey);
        guide.composeBaselineRevision = composeSnapshot->composeNoteRevision;
        const double begin            = lesson.m_beginMs / 1000.0;
        guide.composeSession->pushCommand(Logic::CmdSetPlayState{ false });
        if ( composeBaseline ) {
            // 逻辑队列依次停播、还原正式物件、定位、开播，首播不会闪过旧内容。
            // 对象替换沿用既有命令，草稿及非对象数据域保持原状。
            // 同一轮只提交一次还原；练习中绘制的 Note 会保留到后续段落。
            guide.composeSession->pushCommand(Logic::CmdReplaceBeatmapData{
                .sourceBeatmap  = std::move(composeBaseline),
                .replaceObjects = true,
            });
        }
        guide.composeSession->pushCommand(Logic::CmdSeek{ begin });
        if ( lesson.m_phase != Walkthrough::ComposeLessonPhase::Practice )
            guide.composeSession->pushCommand(Logic::CmdSetPlayState{ true });
        // 欢迎页与谱面常在同一 Dock 标签组；启动后必须让目标画布可见，
        // 否则聚光灯目标无法上报，用户只能看到仍停在原处的教程正文。
        Logic::EditorEngine::instance().requestSessionFocus(
            composeSessionIndex);
    }
    // 其它主题不继承项目白名单；本主题的所有入口共享同一资源身份。
    // 状态只影响用户触发的 UI 文件路径入口，不改变项目加载协议。
    // 这样普通主题的项目操作不会意外继承 CanonRock 路径过滤。
    Walkthrough::restrictOpenProjectGuideToCanonRock(topic.m_id ==
                                                     "mmm.open-project");
}

/// @brief 显式结束当前路线并清理本地身份。
/// @param manager 提供全局 Spotlight。
void WalkthroughPage::stopGuide(UIManager* manager)
{
    if ( m_activeGuide && m_activeGuide->composeSession )
        // 用户按 Esc 或主动结束时，不让自动预览继续在后台播放。
        // 此命令只在退出时排入队列，不进入每帧更新路径。
        m_activeGuide->composeSession->pushCommand(
            Logic::CmdSetPlayState{ false });
    if ( m_draftAreaMutedBeforeGuide.has_value() ) {
        // 本轮结束时恢复原值；预先已静音的用户不会被意外打开声音。
        // 控制库独立于谱面会话，关闭项目后也可还原进入引导前的值。
        Audio::AudioManager::instance().setDraftKeySoundAreaMuted(
            *m_draftAreaMutedBeforeGuide);
        m_draftAreaMutedBeforeGuide.reset();
    }
    // 取消路线同时取消等待关闭的请求；事件已发出时仍由原关闭流程处理。
    // 清理路径状态防止关闭欢迎标签后误限制普通打开项目操作。
    manager->walkthroughSpotlight().stop();
    m_activeGuide.reset();
    m_pendingOpenGuide.reset();
    m_guideError.clear();
    Walkthrough::restrictOpenProjectGuideToCanonRock(false);
}

/// @brief 推进当前整条引导路线并为前景层续租。
/// @param manager 提供目录、环境状态、信号序号和 Spotlight。
/// @warning UI 热路径：只在路线活动时线性查找一个主题、分支和步骤。
void WalkthroughPage::updateGuide(UIManager* manager)
{
    // 路线推进协议：
    // - m_activeGuide 是本轮身份，不读取历史完成度决定跳步；
    // - Spotlight Completed 是当前 UI 目标的明确终态；
    // - 业务信号只接受步骤启动后的新修订号；
    // - 每次衔接重新启动 Spotlight 并建立新的信号基线；
    // - Dock 切换不会清理身份，返回目录和关闭欢迎页则显式 stop；
    // - 项目或谱面标签消失会立即停止，避免遮罩指向失效窗口。
    // Esc 总能取消当前路线；待旧项目关闭的阶段还没有遮罩，也须清掉等待身份。
    // 只在演练活动时读取按键，不影响普通弹窗或编辑器的 Esc 行为。
    if ( (m_activeGuide || m_pendingOpenGuide) &&
         ImGui::IsKeyPressed(ImGuiKey_Escape, false) ) {
        stopGuide(manager);
        return;
    }
    // 只有 UI 已确认没有活动项目且切换结束，才启动先前排队的教学入口。
    // 不能只看逻辑线程对象指针：工作区标签清理也属于关闭过程。
    // 如果用户在关闭确认中取消，本分支保持待启动状态，仍可从页面停止。
    if ( m_pendingOpenGuide ) {
        if ( manager->hasActiveProjectUiState() ||
             manager->isProjectTransitionInProgress() )
            return;
        // 拿出身份后再查目录，避免目录变化时引用到已销毁的步骤对象。
        const auto pending = std::move(*m_pendingOpenGuide);
        m_pendingOpenGuide.reset();
        for ( const auto& topic : manager->walkthroughService().topics() ) {
            // 自定义目录重新加载后若对应步骤已不存在，不启动过期身份。
            if ( topic.m_id != pending.topicId ) continue;
            for ( const auto& branch : topic.m_branches ) {
                if ( branch.m_id != pending.branchId ) continue;
                for ( const auto& step : branch.m_steps )
                    if ( step.m_id == pending.stepId ) {
                        // 保留“上一步”回看语义，避免旧成功信号立即推走首步。
                        startGuide(
                            manager, topic, branch, step, pending.reviewing);
                        return;
                    }
            }
        }
        return;
    }
    auto& spotlight = manager->walkthroughSpotlight();
    if ( m_activeGuide && !spotlight.active() ) {
        stopGuide(manager);
        return;
    }
    if ( !m_activeGuide ) return;

    auto&       service = manager->walkthroughService();
    const auto& topics  = service.topics();
    const auto  topic   = std::find_if(
        topics.begin(), topics.end(), [&](const Walkthrough::Topic& candidate) {
            return candidate.m_id == m_activeGuide->topicId;
        });
    // 运行中切走 CanonRock 后立即撤掉引导，避免后续业务信号推进错误项目。
    // 这里只读取 UI 已归约的项目根；不等待逻辑线程项目锁。
    if ( topic == topics.end() ||
         !Walkthrough::topicAvailableInProject(
             *topic,
             manager->hasActiveProjectUiState() &&
                 !manager->isProjectTransitionInProgress(),
             manager->hasOpenBeatmapEditor(),
             manager->getActiveProjectRoot()) ) {
        // 环境或目录失效后立即撤掉遮罩，不能继续指向不存在的编辑区。
        stopGuide(manager);
        return;
    }
    const auto branch =
        std::find_if(topic->m_branches.begin(),
                     topic->m_branches.end(),
                     [&](const Walkthrough::Branch& candidate) {
                         return candidate.m_id == m_activeGuide->branchId;
                     });
    if ( branch == topic->m_branches.end() ) {
        stopGuide(manager);
        return;
    }
    const auto current =
        std::find_if(branch->m_steps.begin(),
                     branch->m_steps.end(),
                     [&](const Walkthrough::Step& candidate) {
                         return candidate.m_id == m_activeGuide->stepId;
                     });
    if ( current == branch->m_steps.end() ) {
        stopGuide(manager);
        return;
    }
    if ( current->m_composeLesson ) {
        // 只读画布已发布的数据，避免每帧等待会话 update 的长持锁区。
        // 快照消失或换谱面后，继续验收会污染另一张谱面的学习进度。
        const auto& lesson   = *current->m_composeLesson;
        auto&       guide    = *m_activeGuide;
        const auto* snapshot = guide.composeBuffer
                                   ? guide.composeBuffer->getReadingSnapshot()
                                   : nullptr;
        if ( !guide.composeSession || !snapshot || !snapshot->hasBeatmap ||
             snapshot->beatmapPathKey != guide.composeBeatmapKey ) {
            stopGuide(manager);
            return;
        }
        const double begin = lesson.m_beginMs / 1000.0;
        const double end   = lesson.m_endMs / 1000.0;
        if ( lesson.m_phase == Walkthrough::ComposeLessonPhase::Practice ) {
            // 先等新修改进入已发布快照，再请求一次性逻辑查询；普通帧不扫描
            // ECS。每次重新开始路线会先恢复示例物件，预设内容不算本轮练习。
            // 草稿修改可能触发修订，但最终只比较正式主轨道。
            if ( snapshot->composeNoteRevision >
                 guide.composeBaselineRevision ) {
                if ( guide.composeCapture && guide.composeCapture->ready.load(
                                                 std::memory_order_acquire) ) {
                    // acquire 后才可安全读取逻辑线程填充的完整数组。
                    // 版本落后于最新快照时，旧查询不能完成当前目标。
                    if ( guide.composeCaptureRevision ==
                         snapshot->composeNoteRevision ) {
                        if ( const auto* reference =
                                 service.composeLesson(lesson.m_lessonIndex);
                             reference &&
                             Walkthrough::matchesComposeLessonNotes(
                                 *reference, guide.composeCapture->notes) )
                            spotlight.completeTarget("compose.lesson.practice",
                                                     true);
                    }
                    guide.composeCapture.reset();
                }
                if ( !guide.composeCapture &&
                     guide.composeCaptureRevision !=
                         snapshot->composeNoteRevision ) {
                    // 同一修订只排一次查询，不随 UI 帧率重复扫描 ECS。
                    // 命令和页面共享结果所有权，页面退出也不会悬空。
                    guide.composeCaptureRevision =
                        snapshot->composeNoteRevision;
                    guide.composeCapture =
                        std::make_shared<Logic::ComposeLessonCapture>();
                    guide.composeSession->pushCommand(
                        Logic::CmdCaptureComposeLessonNotes{
                            begin, end, guide.composeCapture });
                }
            }
        } else {
            // 旧帧可能仍停留在上一段末尾；必须先看到本次播放从段首启动。
            // 用跨过终点而非恰好等于终点判断，避免离散帧跳过边界。
            // 回看时重新播放属于明确的新业务动作，符合强制步骤语义。
            guide.composePlaybackStarted |=
                snapshot->isPlaying && snapshot->playbackTime >= begin - 0.05 &&
                snapshot->playbackTime < end;
            if ( guide.composePlaybackStarted &&
                 snapshot->playbackTime >= end ) {
                // 停播先入队；下一步骤会重新定位自己的开始时间。
                guide.composeSession->pushCommand(
                    Logic::CmdSetPlayState{ false });
                spotlight.completeTarget("compose.lesson.playback", true);
            }
        }
    }
    // 返回请求优先于完成或业务信号；点击返回的同帧不能顺便完成当前步骤。
    if ( spotlight.consumePreviousStepRequest() ) {
        // 与向前遍历使用同一分支边界，不跨主题或跳进其它创建路线。
        auto previous = current;
        while ( previous != branch->m_steps.begin() ) {
            --previous;
            // 顺序查找只发生在返回请求被消费时，不建立逐帧历史副本。
            if ( !previous->m_guide ) continue;
            // 重建信号基线，不清空学习进度；Spotlight 消费目标登记的练习补偿。
            startGuide(manager, *topic, *branch, *previous, true);
            break;
        }
        // 返回后立即退出本次推进，避免消费业务完成信号后又向前跳转。
        spotlight.keepAlive();
        return;
    }
    const bool targetCompleted = spotlight.completed();
    if ( targetCompleted )
        // 高亮层的“知道了”和业务目标完成都应同步到主题进度。
        // acknowledge 对已经由业务信号自动完成的步骤保持幂等并保留来源。
        service.acknowledge(*topic, *current);
    if ( targetCompleted ||
         (!spotlight.reviewing() &&
          service.receivedSignalAfter(*current,
                                      m_activeGuide->signalRevisionAtStart)) ) {
        const auto next = std::find_if(std::next(current),
                                       branch->m_steps.end(),
                                       [](const Walkthrough::Step& candidate) {
                                           return candidate.m_guide.has_value();
                                       });
        if ( next == branch->m_steps.end() ) {
            // 最后一步已先同步进度，此处只结束易失路线，不清理完成记录。
            stopGuide(manager);
            return;
        }
        const auto nextSignalRevisionAtRunStart =
            m_activeGuide->nextSignalRevisionAtRunStart;
        // 当前五条打开路线均为“入口说明/选择器”和“打开成功”两步。
        // 只在首步交接时覆盖下一步基线，避免后续信号跨步骤串线。
        const bool fromOpenProjectFirstStep =
            topic->m_id == "mmm.open-project" &&
            current == branch->m_steps.begin();
        startGuide(manager, *topic, *branch, *next);
        if ( fromOpenProjectFirstStep && m_activeGuide )
            // 选择器阻塞期间已经到达的成功信号仍属于本轮打开操作。
            // 若没有新成功事件，下一步继续等待，不会凭旧进度跳过。
            m_activeGuide->signalRevisionAtStart = nextSignalRevisionAtRunStart;
    }
    // Dock 标签切走欢迎页后仍必须保留遮罩和路线状态机。
    // keepAlive 只控制本帧绘制，不改变当前步骤或重启目标状态机。
    // 目标窗口晚于欢迎页绘制时仍会在帧末 render 前补齐有效几何。
    if ( m_activeGuide ) spotlight.keepAlive();
}

/// @brief 在欢迎页正文区域绘制一个演练主题及其步骤分支。
/// @param manager 提供演练服务和 Markdown 图片缓存的 UI 管理器。
/// @param topicIndex 当前主题在已验证目录中的索引。
/// @warning UI 热路径：欢迎页可见时每帧执行；只遍历当前主题分支和步骤。
/// @details 页面不持有学习进度，所有状态判断和持久化委托给 Service；
/// 本地只保存展开分支及最近准备图片的主题/语言身份。
void WalkthroughPage::render(UIManager* manager, std::size_t topicIndex)
{
    // 主题目录由服务统一验证，索引失效时不绘制过期内容。
    auto&       service = manager->walkthroughService();
    const auto& topics  = service.topics();
    if ( topicIndex >= topics.size() ) return;
    // 主题引用只在本帧使用，不跨越目录可能重建的生命周期。
    const auto& topic = topics[topicIndex];
    // 项目条件同时约束页面内操作入口，处理用户停留教程时关闭项目的情况。
    // 主题正文与欢迎卡片共用门禁，防止已打开页面绕过目录限制。
    const bool canEnterTopic = Walkthrough::topicAvailableInProject(
        topic,
        manager->hasActiveProjectUiState() &&
            !manager->isProjectTransitionInProgress(),
        manager->hasOpenBeatmapEditor(),
        manager->getActiveProjectRoot());
    if ( !canEnterTopic && m_activeGuide ) {
        // 项目消失后立即结束旧目标遮罩，避免仍引导不可执行的菜单动作。
        stopGuide(manager);
    }
    // 标题、正文和步骤内容均按当前编辑器语言即时选择。
    const auto& language =
        Config::AppConfig::instance().getEditorSettings().language;
    // 固定视觉间距随内容缩放，保持高 DPI 下相同比例。
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    if ( m_currentTopic != topic.m_id ) {
        // 切换主题必须结束旧配置的目标流程，避免同名控件在新正文中被误突出。
        stopGuide(manager);
        // 切换主题时默认展开第一分支，不沿用上一主题的索引。
        m_currentTopic   = topic.m_id;
        m_expandedBranch = 0;
    }
    // 图片缓存是可选注册视图，缺失时 Markdown 仍可渲染纯文本。
    auto* images = manager->getView<MarkdownImageCache>("WalkthroughImages");
    if ( images &&
         (m_preparedTopic != topic.m_id || m_preparedLanguage != language) ) {
        // 仅主题或语言变化时扫描文档图片，避免每帧重复准备资源。
        images->prepareDocument(topic.m_description.get(language));
        for ( const auto& branch : topic.m_branches )
            for ( const auto& step : branch.m_steps )
                // 分支步骤正文也可能引用图片，需与主题说明一并预热。
                images->prepareDocument(step.m_body.get(language));
        // 两个身份都在准备完成后更新，避免半完成状态被视为有效缓存。
        m_preparedTopic    = topic.m_id;
        m_preparedLanguage = language;
    }
    // 当前页面所有 Markdown 块共享同一个图片缓存选项。
    MarkdownRenderOptions markdownOptions;
    markdownOptions.images = images;
    // 进度引用由服务拥有，只在本帧查询，不由页面修改。
    const auto& progress = service.progress();
    // 主题 ID 隔离分支、步骤和重置弹窗的 ImGui 内部标识。
    ImGui::PushID(topic.m_id.c_str());
    ImGui::Dummy({ 0, 24.0F * scale });
    // 使用当前字体族放大主题标题，不引入独立字体资源依赖。
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.75F);
    ImGui::TextWrapped("%s", topic.m_title.get(language).c_str());
    ImGui::PopFont();
    if ( topic.m_placeholder ) {
        // 占位主题只展示统一说明，不渲染尚未定义的分支和进度。
        ImGui::Dummy({ 0, ImGui::GetStyle().WindowPadding.y });
        ImGui::TextWrapped("%s", TR("ui.welcome.placeholder_body").data());
        ImGui::PopID();
        return;
    }
    ImGui::Dummy({ 0, 16.0F * scale });
    // 非占位主题先显示整体目标，再展示可独立完成的操作分支。
    renderMarkdown(topic.m_description.get(language), markdownOptions);
    if ( !canEnterTopic ) {
        ImGui::Spacing();
        ImGui::TextDisabled(
            "%s",
            TR((topic.m_id == "mmm.create-beatmap" ||
                topic.m_id == "mmm.create-beatmap-template")
                   ? "ui.walkthrough.requires_canonrock"
               : topic.m_requiresBeatmap ? "ui.walkthrough.requires_beatmap"
                                         : "ui.walkthrough.requires_project")
                .data());
    }
    ImGui::Dummy({ 0, 28.0F * scale });
    // 主题正文与欢迎目录统一沿用设置项的淡背景和中性描边。
    const auto& style      = ImGui::GetStyle();
    auto        background = style.Colors[ImGuiCol_FrameBg];
    auto        border     = style.Colors[ImGuiCol_Border];
    background.w *= 0.35F;
    border.w *= 0.6F;
    // 完成分支数量用于最终判断 all-of 或 any-of 主题目标。
    std::size_t finishedBranches = 0;
    for ( std::size_t index = 0; index < topic.m_branches.size(); ++index ) {
        // 每个分支卡片按自身稳定 ID 建立独立 ImGui 状态。
        const auto& branch = topic.m_branches[index];
        std::size_t count  = 0;
        for ( const auto& step : branch.m_steps )
            // completed 会同时解释手动确认和业务事件自动完成记录。
            count += progress.completed(topic, step) ? 1 : 0;
        // 空分支按 vacuous truth 视为完成，与模型目标判定保持一致。
        const bool done = count == branch.m_steps.size();
        if ( done ) ++finishedBranches;
        // 页面只允许一个分支展开，减少长教程的纵向占用。
        const bool expanded = m_expandedBranch == index;
        ImGui::PushID(branch.m_id.c_str());
        // 卡片圆角沿用普通 Frame，背景和边框使用降低透明度的主题色。
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, style.FrameRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize,
                            style.ChildBorderSize);
        ImGui::PushStyleColor(ImGuiCol_Border, border);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
        if ( ImGui::BeginChild("BranchCard",
                               { 0, 0 },
                               ImGuiChildFlags_AutoResizeY |
                                   ImGuiChildFlags_Borders |
                                   ImGuiChildFlags_AlwaysUseWindowPadding,
                               ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse) ) {
            // 卡片由内容自动决定高度，内部不建立第二层滚动区域。
            const float line      = ImGui::GetTextLineHeight();
            const float rowHeight = line + 16.0F * scale;
            const auto  row       = ImGui::GetCursorScreenPos();
            // 透明整行按钮只提供统一反馈和命中区，视觉由 DrawList 绘制。
            ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
            const bool clicked =
                FeedbackButton("##BranchHeader",
                               { ImGui::GetContentRegionAvail().x, rowHeight });
            ImGui::PopStyleColor();
            // 读取按钮实际矩形后定位状态圆、标题裁剪区和完成计数。
            const auto   end  = ImGui::GetItemRectMax();
            auto*        draw = ImGui::GetWindowDrawList();
            const ImVec2 center{ row.x + 10.0F * scale,
                                 row.y + rowHeight * 0.5F };
            // 展开或完成使用强调色，未完成折叠分支使用禁用文字色。
            const auto accent = ImGui::GetColorU32(
                expanded || done ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
            if ( done ) {
                // 完成状态绘制实心圆和简洁对勾，不依赖额外图标字体。
                draw->AddCircleFilled(center, 6.0F * scale, accent);
                const auto checkColor = ImGui::GetColorU32(ImGuiCol_WindowBg);
                draw->AddLine(
                    { center.x - 3.0F * scale, center.y },
                    { center.x - 1.0F * scale, center.y + 2.0F * scale },
                    checkColor,
                    1.5F * scale);
                draw->AddLine(
                    { center.x - 1.0F * scale, center.y + 2.0F * scale },
                    { center.x + 3.0F * scale, center.y - 2.0F * scale },
                    checkColor,
                    1.5F * scale);
            } else
                // 未完成状态保留空心圆，表达尚待操作。
                draw->AddCircle(center, 6.0F * scale, accent, 0, 1.0F * scale);
            // 计数按已完成步骤数/总步骤数展示。
            const auto badge = std::to_string(count) + "/" +
                               std::to_string(branch.m_steps.size());
            // 计数贴齐卡片右侧，标题区域在其左边单独裁剪。
            const float badgeX =
                end.x - ImGui::CalcTextSize(badge.c_str()).x - 6.0F * scale;
            draw->PushClipRect(
                { row.x + 28.0F * scale, row.y },
                { std::max(row.x + 28.0F * scale, badgeX - 10.0F * scale),
                  end.y },
                true);
            // 裁剪长标题，避免覆盖右侧完成计数。
            draw->AddText({ row.x + 28.0F * scale, row.y + 8.0F * scale },
                          ImGui::GetColorU32(ImGuiCol_Text),
                          branch.m_title.get(language).c_str());
            draw->PopClipRect();
            draw->AddText({ badgeX, row.y + 8.0F * scale },
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          badge.c_str());
            if ( ImGui::IsItemHovered() )
                // 悬停展示完整标题，补偿视觉裁剪造成的信息缺失。
                ImGui::SetTooltip("%s", branch.m_title.get(language).c_str());
            if ( clicked ) {
                if ( m_activeGuide || m_pendingOpenGuide ) {
                    // 折叠或切换分支会隐藏路线入口，应同步结束其引导。
                    stopGuide(manager);
                }
                if ( expanded )
                    // 点击已展开分支会折叠全部分支。
                    m_expandedBranch.reset();
                else
                    // 点击其他分支直接切换唯一展开索引。
                    m_expandedBranch = index;
            }
            if ( expanded ) {
                const auto firstGuide =
                    std::find_if(branch.m_steps.begin(),
                                 branch.m_steps.end(),
                                 [](const Walkthrough::Step& step) {
                                     return step.m_guide.has_value();
                                 });
                if ( firstGuide != branch.m_steps.end() ) {
                    const bool guidingThisBranch =
                        (m_activeGuide &&
                         m_activeGuide->topicId == topic.m_id &&
                         m_activeGuide->branchId == branch.m_id) ||
                        (m_pendingOpenGuide &&
                         m_pendingOpenGuide->topicId == topic.m_id &&
                         m_pendingOpenGuide->branchId == branch.m_id);
                    const auto guideLabel =
                        TR(guidingThisBranch ? "ui.walkthrough.stop_guide"
                                             : "ui.walkthrough.enter_guide")
                            .toString() +
                        "###WalkthroughBranchGuide";
                    // 分支入口总是从第一步开始，历史完成记录只保留展示意义。
                    ImGui::BeginDisabled(!canEnterTopic);
                    if ( FeedbackButton(guideLabel.c_str()) ) {
                        if ( guidingThisBranch ) {
                            stopGuide(manager);
                        } else {
                            startGuide(manager, topic, branch, *firstGuide);
                            // 点击帧立即续租，目标从下一帧开始按路线顺序跟随。
                            manager->walkthroughSpotlight().keepAlive();
                        }
                    }
                    ImGui::EndDisabled();
                    // 长路线的底部错误信息不可见；入口校验失败必须就地反馈。
                    if ( !m_guideError.empty() &&
                         topic.m_id == "mmm.compose-beatmap" )
                        ImGui::TextWrapped("%s", m_guideError.c_str());
                    ImGui::Spacing();
                }
                // 步骤内容缩进到状态圆之后，与分支标题文字对齐。
                ImGui::Indent(28.0F * scale);
                for ( const auto& step : branch.m_steps ) {
                    // 步骤 ID 隔离确认与执行业务按钮的内部状态。
                    ImGui::PushID(step.m_id.c_str());
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s",
                                       step.m_title.get(language).c_str());
                    // 步骤正文允许包含格式化说明和预热过的本地图片。
                    renderMarkdown(step.m_body.get(language), markdownOptions);
                    ImGui::Spacing();
                    // 已完成步骤禁用再次确认，来源标签仍可解释完成方式。
                    const bool completed = progress.completed(topic, step);
                    const auto acknowledge =
                        std::string(ICON_MMM_CHECK) + "  " +
                        TR("ui.walkthrough.acknowledge").toString();
                    // 确认按钮采用透明背景和主题勾选色，融入教程正文。
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4{ 0, 0, 0, 0 });
                    ImGui::PushStyleColor(
                        ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
                    // 必须执行的删除步骤只能由画布报告完成，页面按钮不可越过。
                    ImGui::BeginDisabled(completed ||
                                         step.m_guide &&
                                             step.m_guide->m_requiresAction);
                    if ( FeedbackButton(acknowledge.c_str()) )
                        // 手动确认由服务更新依赖进度并立即持久化。
                        service.acknowledge(topic, step);
                    ImGui::EndDisabled();
                    ImGui::PopStyleColor(2);
                    if ( completed ) {
                        // 完成来源区分业务事件自动达成与用户手动确认。
                        ImGui::SameLine();
                        ImGui::TextDisabled(
                            "%s",
                            TR(progress.source(topic, step) == "automatic"
                                   ? "ui.walkthrough.automatic"
                                   : "ui.walkthrough.manual")
                                .data());
                    }
                    if ( !step.m_action.empty() ) {
                        // 操作同时受步骤依赖可用性和可信 C++ 注册表限制。
                        ImGui::BeginDisabled(!canEnterTopic ||
                                             !progress.available(topic, step) ||
                                             !service.hasAction(step.m_action));
                        if ( FeedbackButton(
                                 TR("ui.walkthrough.perform").data()) )
                            // 数据文件只提供操作 ID，真正函数来自服务注册表。
                            service.execute(step.m_action);
                        ImGui::EndDisabled();
                    }
                    ImGui::Dummy({ 0, 14.0F * scale });
                    ImGui::PopID();
                }
                // 与进入步骤列表时的缩进严格配对。
                ImGui::Unindent(28.0F * scale);
            }
        }
        // BeginChild 无论返回值如何都必须结束，并恢复四项局部样式。
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::PopID();
        ImGui::Dummy({ 0, 6.0F * scale });
    }
    ImGui::Dummy({ 0, 12.0F * scale });
    // anyBranch 主题完成任一分支即可，否则要求所有分支完成。
    if ( topic.m_anyBranch ? finishedBranches > 0
                           : finishedBranches == topic.m_branches.size() )
        ImGui::TextWrapped("%s", TR("ui.walkthrough.goal_complete").data());
    ImGui::Dummy({ 0, 8.0F * scale });
    // 重置入口与步骤确认按钮使用一致的轻量文本样式。
    const auto resetLabel = std::string(ICON_MMM_REDO) + "  " +
                            TR("ui.walkthrough.reset").toString();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0, 0, 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    const bool resetClicked = FeedbackButton(resetLabel.c_str());
    ImGui::PopStyleColor(2);
    if ( resetClicked ) FeedbackOpenPopup("ResetWalkthrough");
    if ( ImGui::BeginPopup("ResetWalkthrough") ) {
        // 二次确认避免误删当前主题的全部学习记录。
        ImGui::TextWrapped("%s", TR("ui.walkthrough.reset_confirm").data());
        if ( FeedbackButton(TR("ui.common.confirm").data()) ) {
            service.reset(topic);
            // 重置完成后关闭弹窗，正文下一帧读取更新后的进度。
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( FeedbackButton(TR("ui.common.cancel").data()) )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // 加载、保存或自定义主题校验错误显示在当前页面底部。
    if ( !service.error().empty() )
        ImGui::TextWrapped("%s", service.error().c_str());
    // 恢复主题级 ImGui ID 作用域。
    ImGui::PopID();
}
}  // namespace MMM::UI
