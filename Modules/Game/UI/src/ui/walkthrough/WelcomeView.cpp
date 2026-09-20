#include "ui/walkthrough/WelcomeView.h"

#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/translation/Translation.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughModel.h"
#include "ui/walkthrough/WalkthroughService.h"
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>

/// @file WelcomeView.cpp
/// @brief 欢迎标签页的章节目录、主题导航、Dock 恢复和固定页脚实现。
/// @details 欢迎页是普通 IUIView 标签页，不创建独立演练窗口；首页目录和
/// 主题正文共享相同 ImGui 窗口、滚动区、焦点与启动显示设置。
///
/// 导航状态约定：
/// - m_topic 为空表示章节目录，非空表示主题正文；
/// - m_chapter 保存稳定章节 ID，不保存本地化标签；
/// - m_restoreChapter 只在返回目录后的首个成功 TabBar 帧生效；
/// - m_scrollToTop 只在首页与主题之间切换后消费一次；
/// - m_focus 表示下一次 Begin 前请求窗口焦点；
/// - m_dockToCenter 表示主 Dock 节点可用后停靠一次；
/// - 学习进度完全由 Walkthrough::Service 拥有，不随页面导航重置。
///
/// 布局约定：
/// - 外层欢迎窗口禁用自身滚动条；
/// - WelcomeBody 是唯一纵向滚动区域；
/// - WelcomeContent 在可用宽度内居中并限制阅读行长；
/// - 首页目录最大宽度大于主题正文；
/// - “启动时显示”复选框固定在滚动区下方；
/// - 保存错误会扩展页脚预留高度而不覆盖正文；
/// - 动态窗口标题始终以 ###WelcomePage 固定内部 ID。
///
/// 章节目录约定：
/// - Service 提供的章节顺序直接决定标签顺序；
/// - 空章节仍显示标签和空状态说明；
/// - 主题只在 m_chapter 匹配的面板内绘制；
/// - 相同 m_order 的连续主题归入同一阶段；
/// - 主题卡片按服务目录索引进入正文；
/// - 占位主题显示“即将推出”而非步骤计数；
/// - 项目限定主题在生命周期未就绪时保留目录位置但禁用点击；
/// - 卡片标题裁剪时通过悬停提示提供完整文本。
///
/// 样式约定：
/// - 卡片背景和边框从当前 ImGui 主题色降低透明度得到；
/// - 章节标签选中和悬停色由背景向文字色插值；
/// - 标签上划线使用 CheckMark 强调色；
/// - 主题卡片使用 FeedbackButton 提供统一声音和悬停反馈；
/// - 书本图标、标题、副标题和进度徽标由 DrawList 叠加；
/// - 所有固定尺寸均乘当前内容缩放比例；
/// - 字体、颜色、样式、子窗口和 TabItem 调用必须保持栈平衡。
///
/// 持久化边界：
/// - 页面可见帧不扫描教程或图片目录；
/// - 切换首页、章节或主题不写配置文件；
/// - 只有用户改变“启动时显示”时调用 AppConfig::save；
/// - 保存失败只显示错误，不回滚用户在内存中的即时选择；
/// - 演练进度保存由 Service 在进度真正变化时独立处理。

namespace MMM::UI
{
/// @brief 创建欢迎视图并使用稳定名称注册基础视图状态。
/// @note 默认请求一次焦点和中心停靠，具体 Dock 节点在首次更新时解析。
WelcomeView::WelcomeView() : IUIView("Welcome") {}

/// @brief 在主 Dock 布局重建前记录欢迎页的重新停靠意图。
/// @warning 低频布局路径：读取 ImGui 内部 DockNode，只在布局切换前调用。
/// @details 只有当前窗口属于 DockSpace 树时才请求重新停靠；用户显式浮动的
/// 欢迎页保持浮动位置，不被布局恢复强制拉回中心。
void WelcomeView::prepareForDockLayoutChange()
{
    // 稳定 ### ID 与动态本地化标题无关，可直接查找现有窗口。
    const auto* window = ImGui::FindWindowByName("###WelcomePage");
    const auto* node   = window ? window->DockNode : nullptr;
    // 向上追溯到 Dock 树根，判断窗口是否确实属于主 DockSpace。
    while ( node && node->ParentNode ) node = node->ParentNode;
    if ( node && node->IsDockSpace() ) m_dockToCenter = true;
}

/// @brief 打开欢迎首页并重置页面级导航状态。
/// @post 保留学习进度和上次章节 ID，下一帧恢复章节并滚动到顶部。
void WelcomeView::showHome()
{
    // 显式显示入口同时重新打开可能已关闭的标签页。
    m_isOpen = true;
    // 空主题值是首页的唯一状态表示。
    m_topic.reset();
    m_focus       = true;
    m_scrollToTop = true;
    // 章节恢复只在返回首页后的首个 TabBar 帧执行一次。
    m_restoreChapter = true;
}

/// @brief 绘制欢迎首页的章节标签和主题卡片目录。
/// @param manager 提供演练模型、进度和错误状态的 UI 管理器。
/// @warning UI 热路径：首页可见时每帧遍历章节及其主题和步骤。
/// @details 主题按服务预排序结果分组到章节标签；相同 order 值归入同一阶段，
/// 卡片展示标题、入口提示及已完成步骤数，点击后在同一标签页进入正文。
void WelcomeView::renderHome(UIManager* manager)
{
    // 服务目录和进度由 UIManager 长期持有，本函数只借用只读引用。
    const auto& service = manager->walkthroughService();
    const auto& topics  = service.topics();
    // 目录标签和卡片标题即时响应当前语言设置。
    const auto& language =
        Config::AppConfig::instance().getEditorSettings().language;
    // 所有固定间距和图形半径按内容缩放换算。
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    // 首页顶部保留呼吸空间，再绘制产品名和引导副标题。
    ImGui::Dummy({ 0, 28.0F * scale });
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.8F);
    ImGui::TextWrapped("MusicMapMaker-Next");
    ImGui::PopFont();
    ImGui::TextDisabled("%s", TR("ui.welcome.subtitle").data());
    ImGui::Dummy({ 0, 30.0F * scale });
    // 演练目录标题使用次级字号，与产品主标题建立层次。
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2F);
    ImGui::TextUnformatted(TR("ui.welcome.walkthroughs").data());
    ImGui::PopFont();
    ImGui::TextWrapped("%s", TR("ui.welcome.choose_topic").data());
    ImGui::Dummy({ 0, 10.0F * scale });
    if ( topics.empty() )
        // 目录为空仍保留页面结构，并给出本地化提示。
        ImGui::TextDisabled("%s", TR("ui.welcome.empty").data());

    // 卡片高度容纳标题和入口提示两行文字及上下内边距。
    const float padding    = ImGui::GetStyle().WindowPadding.x;
    const float line       = ImGui::GetTextLineHeight();
    const float cardHeight = 2.0F * padding + 2.5F * line;
    // 与设置项装饰一致：淡化 FrameBg 和 Border，不额外增强明暗对比。
    const auto& style       = ImGui::GetStyle();
    auto        panelColor  = style.Colors[ImGuiCol_FrameBg];
    auto        borderColor = style.Colors[ImGuiCol_Border];
    panelColor.w *= 0.35F;
    borderColor.w *= 0.6F;
    // 仅增强章节标签的状态区分，内容区仍保留设置项的轻量装饰。
    // 标签选中和悬停色从窗口背景向文字色轻微插值，适配不同皮肤。
    auto selectedTab = ImLerp(
        style.Colors[ImGuiCol_WindowBg], style.Colors[ImGuiCol_Text], 0.12F);
    auto hoveredTab = ImLerp(
        style.Colors[ImGuiCol_WindowBg], style.Colors[ImGuiCol_Text], 0.18F);
    selectedTab.w = hoveredTab.w = 1.0F;
    // 四项标签局部样式在 TabBar 结束后统一恢复。
    ImGui::PushStyleColor(ImGuiCol_TabSelected, selectedTab);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, hoveredTab);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline,
                          style.Colors[ImGuiCol_CheckMark]);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 2.0F * scale);
    if ( ImGui::BeginTabBar("WelcomeChapters",
                            ImGuiTabBarFlags_FittingPolicyScroll |
                                ImGuiTabBarFlags_DrawSelectedOverline) ) {
        // 章节列表保留空章节，使未完成规划仍有稳定导航位置。
        for ( const auto& chapter : service.chapters() ) {
            // 可见标题后拼接稳定章节 ID，语言切换不破坏 ImGui 状态。
            const auto label =
                chapter.m_title.get(language) + "###" + chapter.m_id;
            // 返回首页后的首帧通过 SetSelected 恢复上次章节。
            const auto flags = m_restoreChapter && m_chapter == chapter.m_id
                                   ? ImGuiTabItemFlags_SetSelected
                                   : ImGuiTabItemFlags_None;
            if ( !ImGui::BeginTabItem(label.c_str(), nullptr, flags) ) continue;
            if ( !m_restoreChapter || m_chapter.empty() ||
                 flags == ImGuiTabItemFlags_SetSelected )
                // 用户切换或首次无记录时缓存当前章节稳定 ID。
                m_chapter = chapter.m_id;
            ImGui::Dummy({ 0, padding });
            // 章节面板采用轻量背景和边框，不建立独立滚动条。
            ImGui::PushStyleColor(ImGuiCol_ChildBg, panelColor);
            ImGui::PushStyleColor(ImGuiCol_Border, borderColor);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,
                                style.FrameRounding);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize,
                                style.ChildBorderSize);
            if ( ImGui::BeginChild("ChapterPanel",
                                   { 0, 0 },
                                   ImGuiChildFlags_AutoResizeY |
                                       ImGuiChildFlags_Borders |
                                       ImGuiChildFlags_AlwaysUseWindowPadding,
                                   ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse) ) {
                const float width = ImGui::GetContentRegionAvail().x;
                // hasTopics 用于为空章节显示明确占位说明。
                bool hasTopics = false;
                // order 变化划分新的学习阶段，同 order 主题并列。
                int previousOrder = -1;
                int stage         = 0;
                for ( std::size_t i = 0; i < topics.size(); ++i ) {
                    const auto& topic = topics[i];
                    // 主题目录已排序，此处只筛选当前章节，不重新分组容器。
                    if ( topic.m_chapter != chapter.m_id ) continue;
                    hasTopics = true;
                    if ( previousOrder != topic.m_order ) {
                        // 每遇到新的 order 值递增面向用户的一基阶段编号。
                        previousOrder = topic.m_order;
                        ++stage;
                        ImGui::Dummy({ 0, padding * 0.5F });
                        ImGui::TextDisabled(
                            "%s %d", TR("ui.welcome.stage").data(), stage);
                    }
                    // 卡片进度按主题所有分支的全部步骤聚合。
                    std::size_t done = 0, total = 0;
                    for ( const auto& branch : topic.m_branches )
                        for ( const auto& step : branch.m_steps ) {
                            ++total;
                            // 手动与自动完成都计入统一的已完成数量。
                            done += service.progress().completed(topic, step)
                                        ? 1
                                        : 0;
                        }
                    // 占位主题不显示误导性的 0/0，而显示“即将推出”。
                    const std::string badge =
                        topic.m_placeholder
                            ? TR("ui.welcome.coming_soon").toString()
                            : std::to_string(done) + " / " +
                                  std::to_string(total);
                    const auto& title = topic.m_title.get(language);
                    // 项目限定主题只读取 UI
                    // 已消费的生命周期状态，不跨线程访问控制器。
                    const bool canEnterTopic = Walkthrough::topicAvailable(
                        topic,
                        manager->hasActiveProjectUiState() &&
                            !manager->isProjectTransitionInProgress(),
                        manager->hasOpenBeatmapEditor());
                    // 主题 ID 隔离相同可见标题或相同阶段中的卡片状态。
                    ImGui::PushID(topic.m_id.c_str());
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                                        style.FrameRounding);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,
                                        style.ChildBorderSize);
                    ImGui::PushStyleColor(ImGuiCol_Button, panelColor);
                    ImGui::BeginDisabled(!canEnterTopic);
                    // 反馈按钮覆盖整张卡片，具体文本和徽标由 DrawList 绘制。
                    const bool chosen =
                        FeedbackButton("##TopicCard", { width, cardHeight });
                    // 使用真实按钮矩形定位图标、标题、副标题和进度徽标。
                    const auto  pos    = ImGui::GetItemRectMin();
                    const auto  end    = ImGui::GetItemRectMax();
                    auto*       draw   = ImGui::GetWindowDrawList();
                    const auto  accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
                    const float iconWidth =
                        ImGui::CalcTextSize(ICON_MMM_BOOK).x;
                    // 正文起点位于书本图标之后，右侧为徽标预留宽度。
                    const float textX = pos.x + padding + iconWidth + padding;
                    const float badgeWidth =
                        ImGui::CalcTextSize(badge.c_str()).x + padding;
                    const float badgeX = end.x - padding - badgeWidth;
                    draw->AddText({ pos.x + padding, pos.y + padding },
                                  accent,
                                  ICON_MMM_BOOK);
                    // 标题裁剪到徽标左侧，避免长本地化文本覆盖进度。
                    draw->PushClipRect(
                        { textX, pos.y },
                        { std::max(textX, badgeX - padding), end.y },
                        true);
                    draw->AddText({ textX, pos.y + padding },
                                  ImGui::GetColorU32(ImGuiCol_Text),
                                  title.c_str());
                    draw->PopClipRect();
                    // 徽标使用 Header 主题色形成轻量胶囊背景。
                    draw->AddRectFilled(
                        { badgeX, pos.y + padding - 2.0F * scale },
                        { end.x - padding,
                          pos.y + padding + line + 2.0F * scale },
                        ImGui::GetColorU32(ImGuiCol_Header),
                        4.0F * scale);
                    draw->AddText({ badgeX + padding * 0.5F, pos.y + padding },
                                  ImGui::GetColorU32(ImGuiCol_Text),
                                  badge.c_str());
                    // 第二行入口提示裁剪到卡片右侧内边距。
                    draw->PushClipRect(
                        { textX, pos.y }, { end.x - padding, end.y }, true);
                    draw->AddText({ textX, pos.y + padding + line * 1.5F },
                                  ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                  TR(canEnterTopic ? "ui.welcome.start_learning"
                                     : topic.m_requiresBeatmap
                                         ? "ui.welcome.requires_beatmap"
                                         : "ui.welcome.requires_project")
                                      .data());
                    draw->PopClipRect();
                    if ( ImGui::IsItemHovered() )
                        // 悬停显示完整标题，补偿卡片中的裁剪。
                        ImGui::SetTooltip("%s", title.c_str());
                    ImGui::EndDisabled();
                    ImGui::PopStyleColor();
                    ImGui::PopStyleVar(2);
                    ImGui::PopID();
                    ImGui::Dummy({ 0, 4.0F * scale });
                    if ( chosen ) {
                        // 主题索引来自服务当前快照，在同一帧切换页面状态。
                        showTopic(i);
                    }
                }
                if ( !hasTopics )
                    // 保留空章节入口并解释当前没有可用主题。
                    ImGui::TextWrapped("%s",
                                       TR("ui.welcome.chapter_empty").data());
            }
            // 章节面板与四项局部颜色/样式严格成对恢复。
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            ImGui::EndTabItem();
        }
        // 只在首次成功绘制 TabBar 后消费恢复请求，后续尊重用户点击。
        m_restoreChapter = false;
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    // 模型解析或进度持久化错误展示在目录底部。
    if ( !service.error().empty() )
        ImGui::TextWrapped("%s", service.error().c_str());
}

/// @brief 在当前欢迎标签页中打开指定主题正文。
/// @param topicIndex 主题在服务目录中的索引。
/// @post 下一帧正文滚动回顶部，学习进度保持不变。
void WelcomeView::showTopic(std::size_t topicIndex)
{
    // 索引合法性在 update 中用最新目录再次验证。
    m_topic       = topicIndex;
    m_scrollToTop = true;
}

/// @brief 绘制欢迎页窗口、可滚动正文和固定启动选项页脚。
/// @param manager 提供演练服务和正文依赖的 UI 管理器。
/// @warning UI 热路径：打开时每帧执行；配置文件只在用户切换启动选项时保存。
/// @details 窗口标题随首页或主题变化，但使用稳定 ### ID 保持 Dock；正文
/// 置于居中的定宽子区域，底部“启动时显示”选项保持在滚动区之外。
void WelcomeView::update(UIManager* manager)
{
    // 引导路线必须在欢迎标签被其它 Dock 标签遮住时继续推进和续租。
    // 该调用位于 Begin 之前，因此 Dock Tab 不可见时也不会被 ImGui 跳过。
    m_walkthrough.updateGuide(manager);
    // 当前帧共用配置引用和 DPI 比例，避免重复查询布局参数。
    auto&       config   = Config::AppConfig::instance();
    const float scale    = config.getWindowContentScale();
    const auto* viewport = ImGui::GetMainViewport();
    // 首次浮动尺寸受主视口工作区九成限制，避免窗口完全遮挡宿主。
    ImGui::SetNextWindowSize(
        { std::min(1000.0F * scale, viewport->WorkSize.x * 0.9F),
          std::min(780.0F * scale, viewport->WorkSize.y * 0.9F) },
        ImGuiCond_FirstUseEver);
    if ( m_dockToCenter ) {
        // 主 Dock 节点可能在早期帧尚未创建，保留请求直到取得有效 ID。
        const auto dock = MainDockSpaceUI::getCenterDockId();
        if ( dock != 0 ) {
            ImGui::SetNextWindowDockID(dock, ImGuiCond_Always);
            // 成功设置后只消费一次，不覆盖用户后续拖动布局。
            m_dockToCenter = false;
        }
    }
    if ( m_focus ) {
        // 焦点请求必须在 Begin 前发出，并在同帧消费。
        ImGui::SetNextWindowFocus();
        m_focus = false;
    }
    // 自定义目录或服务状态变化后，旧主题索引可能失效。
    const auto& topics = manager->walkthroughService().topics();
    if ( m_topic && *m_topic >= topics.size() ) showHome();
    // 可见标题显示当前主题，### 后固定 ID 确保停靠节点不随语言变化。
    const auto title =
        (m_topic ? TR("ui.welcome.walkthroughs").toString() + ": " +
                       topics[*m_topic].m_title.get(
                           config.getEditorSettings().language)
                 : TR("ui.welcome.title").toString()) +
        "###WelcomePage";
    // 保存 Begin 前状态用于统一原生关闭按钮反馈。
    const bool  wasOpen = m_isOpen;
    const float padding = std::max(
        0.0F, config.getEditorSettings().aesthetics.windowPadding * scale);
    // 欢迎页窗口内边距使用编辑器美学配置并按 DPI 缩放。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2{ padding, padding });
    const bool visible = ImGui::Begin(
        title.c_str(),
        &m_isOpen,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    FeedbackCurrentWindowCloseButton(wasOpen, &m_isOpen);
    if ( visible ) {
        // 欢迎页整体使用略大正文字号，局部标题可继续叠加倍率。
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2F);
        // 禁用文字降低透明度，用于副标题、阶段和进度辅助信息。
        auto muted = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        muted.w *= 0.65F;
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, muted);
        if ( m_topic ) {
            // 主题页顶部提供返回目录入口，使用强调色透明按钮样式。
            const auto back = std::string(ICON_MMM_ARROW_LEFT) + "  " +
                              TR("ui.welcome.back").toString();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0, 0, 0, 0 });
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            if ( FeedbackButton(back.c_str()) ) {
                // 返回目录表示用户主动退出当前路线，与切换 Dock 标签不同。
                m_walkthrough.stopGuide(manager);
                showHome();
            }
            ImGui::PopStyleColor(2);
        }
        // 页脚高度从正文滚动区中预留，保存错误出现时额外增加一行。
        const auto  available = ImGui::GetContentRegionAvail();
        const float footer =
            ImGui::GetFrameHeightWithSpacing() * (m_saveFailed ? 2.0F : 1.0F) +
            padding;
        if ( ImGui::BeginChild("WelcomeBody",
                               { 0, std::max(1.0F, available.y - footer) },
                               ImGuiChildFlags_None) ) {
            // 正文是唯一可滚动区域，窗口本身禁用滚动条。
            if ( m_scrollToTop ) {
                // 页面切换只重置一次滚动位置，不在后续帧争夺用户滚动。
                ImGui::SetScrollY(0);
                m_scrollToTop = false;
            }
            // 内容采用居中最大宽度，超宽窗口上保持舒适阅读行长。
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            const float margin =
                std::min(48.0F * scale, availableWidth * 0.06F);
            // 主题正文比卡片目录更窄，两个模式分别限制最大宽度。
            const float width =
                std::max(1.0F,
                         std::min((m_topic ? 620.0F : 760.0F) * scale,
                                  availableWidth - margin * 2.0F));
            // 通过移动当前 X 坐标将定宽内容子区水平居中。
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (availableWidth - width) * 0.5F);
            ImGui::BeginChild("WelcomeContent",
                              { width, 0 },
                              ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
            if ( m_topic ) {
                // 主题页委托给内嵌 WalkthroughPage，不创建新窗口。
                m_walkthrough.render(manager, *m_topic);
            } else {
                // 空主题状态绘制章节和主题目录。
                renderHome(manager);
            }
            // 尾部留白纳入滚动内容，统一采用用户的全局窗口内边距。
            ImGui::Dummy({ 0, ImGui::GetStyle().WindowPadding.y });
            ImGui::EndChild();
        }
        ImGui::EndChild();
        // 固定页脚复选框根据图标、内间距和文字宽度计算居中位置。
        const auto  label         = TR("ui.welcome.show_on_startup");
        const float checkboxWidth = ImGui::GetFrameHeight() +
                                    ImGui::GetStyle().ItemInnerSpacing.x +
                                    ImGui::CalcTextSize(label.data()).x;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(
                0.0F,
                (ImGui::GetContentRegionAvail().x - checkboxWidth) * 0.5F));
        if ( FeedbackCheckbox(
                 label.data(),
                 &config.getEditorSettings().m_showWelcomeOnStartup) )
            // 用户切换后立即保存；失败状态保持到下一次成功修改。
            m_saveFailed = !config.save();
        if ( m_saveFailed )
            ImGui::TextWrapped("%s", TR("ui.welcome.save_failed").data());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    // Begin 即使返回不可见也必须 End，并恢复窗口内边距样式。
    ImGui::End();
    ImGui::PopStyleVar();
    if ( !m_isOpen )
        // 关闭欢迎页不再拥有路线会话，立即清除可能仍在续租的遮罩。
        m_walkthrough.stopGuide(manager);
}
}  // namespace MMM::UI
