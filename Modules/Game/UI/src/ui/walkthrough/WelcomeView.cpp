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

namespace MMM::UI
{
WelcomeView::WelcomeView() : IUIView("Welcome") {}

void WelcomeView::prepareForDockLayoutChange()
{
    const auto* window = ImGui::FindWindowByName("###WelcomePage");
    const auto* node   = window ? window->DockNode : nullptr;
    while ( node && node->ParentNode ) node = node->ParentNode;
    if ( node && node->IsDockSpace() ) m_dockToCenter = true;
}

void WelcomeView::showHome()
{
    m_isOpen = true;
    m_topic.reset();
    m_focus          = true;
    m_scrollToTop    = true;
    m_restoreChapter = true;
}

void WelcomeView::renderHome(UIManager* manager)
{
    const auto& service = manager->walkthroughService();
    const auto& topics  = service.topics();
    const auto& language =
        Config::AppConfig::instance().getEditorSettings().language;
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    ImGui::Dummy({ 0, 28.0F * scale });
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.8F);
    ImGui::TextWrapped("MusicMapMaker-Next");
    ImGui::PopFont();
    ImGui::TextDisabled("%s", TR("ui.welcome.subtitle").data());
    ImGui::Dummy({ 0, 30.0F * scale });
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2F);
    ImGui::TextUnformatted(TR("ui.welcome.walkthroughs").data());
    ImGui::PopFont();
    ImGui::TextWrapped("%s", TR("ui.welcome.choose_topic").data());
    ImGui::Dummy({ 0, 10.0F * scale });
    if ( topics.empty() )
        ImGui::TextDisabled("%s", TR("ui.welcome.empty").data());

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
    auto selectedTab = ImLerp(
        style.Colors[ImGuiCol_WindowBg], style.Colors[ImGuiCol_Text], 0.12F);
    auto hoveredTab = ImLerp(
        style.Colors[ImGuiCol_WindowBg], style.Colors[ImGuiCol_Text], 0.18F);
    selectedTab.w = hoveredTab.w = 1.0F;
    ImGui::PushStyleColor(ImGuiCol_TabSelected, selectedTab);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, hoveredTab);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline,
                          style.Colors[ImGuiCol_CheckMark]);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 2.0F * scale);
    if ( ImGui::BeginTabBar("WelcomeChapters",
                            ImGuiTabBarFlags_FittingPolicyScroll |
                                ImGuiTabBarFlags_DrawSelectedOverline) ) {
        for ( const auto& chapter : service.chapters() ) {
            const auto label =
                chapter.m_title.get(language) + "###" + chapter.m_id;
            const auto flags = m_restoreChapter && m_chapter == chapter.m_id
                                   ? ImGuiTabItemFlags_SetSelected
                                   : ImGuiTabItemFlags_None;
            if ( !ImGui::BeginTabItem(label.c_str(), nullptr, flags) ) continue;
            if ( !m_restoreChapter || m_chapter.empty() ||
                 flags == ImGuiTabItemFlags_SetSelected )
                m_chapter = chapter.m_id;
            ImGui::Dummy({ 0, padding });
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
                const float width         = ImGui::GetContentRegionAvail().x;
                bool        hasTopics     = false;
                int         previousOrder = -1;
                int         stage         = 0;
                for ( std::size_t i = 0; i < topics.size(); ++i ) {
                    const auto& topic = topics[i];
                    if ( topic.m_chapter != chapter.m_id ) continue;
                    hasTopics = true;
                    if ( previousOrder != topic.m_order ) {
                        previousOrder = topic.m_order;
                        ++stage;
                        ImGui::Dummy({ 0, padding * 0.5F });
                        ImGui::TextDisabled(
                            "%s %d", TR("ui.welcome.stage").data(), stage);
                    }
                    std::size_t done = 0, total = 0;
                    for ( const auto& branch : topic.m_branches )
                        for ( const auto& step : branch.m_steps ) {
                            ++total;
                            done += service.progress().completed(topic, step)
                                        ? 1
                                        : 0;
                        }
                    const std::string badge =
                        topic.m_placeholder
                            ? TR("ui.welcome.coming_soon").toString()
                            : std::to_string(done) + " / " +
                                  std::to_string(total);
                    const auto& title = topic.m_title.get(language);
                    ImGui::PushID(topic.m_id.c_str());
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                                        style.FrameRounding);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,
                                        style.ChildBorderSize);
                    ImGui::PushStyleColor(ImGuiCol_Button, panelColor);
                    const bool chosen =
                        FeedbackButton("##TopicCard", { width, cardHeight });
                    ImGui::PopStyleColor();
                    ImGui::PopStyleVar(2);
                    const auto  pos    = ImGui::GetItemRectMin();
                    const auto  end    = ImGui::GetItemRectMax();
                    auto*       draw   = ImGui::GetWindowDrawList();
                    const auto  accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
                    const float iconWidth =
                        ImGui::CalcTextSize(ICON_MMM_BOOK).x;
                    const float textX = pos.x + padding + iconWidth + padding;
                    const float badgeWidth =
                        ImGui::CalcTextSize(badge.c_str()).x + padding;
                    const float badgeX = end.x - padding - badgeWidth;
                    draw->AddText({ pos.x + padding, pos.y + padding },
                                  accent,
                                  ICON_MMM_BOOK);
                    draw->PushClipRect(
                        { textX, pos.y },
                        { std::max(textX, badgeX - padding), end.y },
                        true);
                    draw->AddText({ textX, pos.y + padding },
                                  ImGui::GetColorU32(ImGuiCol_Text),
                                  title.c_str());
                    draw->PopClipRect();
                    draw->AddRectFilled(
                        { badgeX, pos.y + padding - 2.0F * scale },
                        { end.x - padding,
                          pos.y + padding + line + 2.0F * scale },
                        ImGui::GetColorU32(ImGuiCol_Header),
                        4.0F * scale);
                    draw->AddText({ badgeX + padding * 0.5F, pos.y + padding },
                                  ImGui::GetColorU32(ImGuiCol_Text),
                                  badge.c_str());
                    draw->PushClipRect(
                        { textX, pos.y }, { end.x - padding, end.y }, true);
                    draw->AddText({ textX, pos.y + padding + line * 1.5F },
                                  ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                  TR("ui.welcome.start_learning").data());
                    draw->PopClipRect();
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip("%s", title.c_str());
                    ImGui::PopID();
                    ImGui::Dummy({ 0, 4.0F * scale });
                    if ( chosen ) {
                        showTopic(i);
                    }
                }
                if ( !hasTopics )
                    ImGui::TextWrapped("%s",
                                       TR("ui.welcome.chapter_empty").data());
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            ImGui::EndTabItem();
        }
        m_restoreChapter = false;
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if ( !service.error().empty() )
        ImGui::TextWrapped("%s", service.error().c_str());
}

void WelcomeView::showTopic(std::size_t topicIndex)
{
    m_topic       = topicIndex;
    m_scrollToTop = true;
}

void WelcomeView::update(UIManager* manager)
{
    auto&       config   = Config::AppConfig::instance();
    const float scale    = config.getWindowContentScale();
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(
        { std::min(1000.0F * scale, viewport->WorkSize.x * 0.9F),
          std::min(780.0F * scale, viewport->WorkSize.y * 0.9F) },
        ImGuiCond_FirstUseEver);
    if ( m_dockToCenter ) {
        const auto dock = MainDockSpaceUI::getCenterDockId();
        if ( dock != 0 ) {
            ImGui::SetNextWindowDockID(dock, ImGuiCond_Always);
            m_dockToCenter = false;
        }
    }
    if ( m_focus ) {
        ImGui::SetNextWindowFocus();
        m_focus = false;
    }
    const auto& topics = manager->walkthroughService().topics();
    if ( m_topic && *m_topic >= topics.size() ) showHome();
    const auto title =
        (m_topic ? TR("ui.welcome.walkthroughs").toString() + ": " +
                       topics[*m_topic].m_title.get(
                           config.getEditorSettings().language)
                 : TR("ui.welcome.title").toString()) +
        "###WelcomePage";
    const bool  wasOpen = m_isOpen;
    const float padding = std::max(
        0.0F, config.getEditorSettings().aesthetics.windowPadding * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2{ padding, padding });
    const bool visible = ImGui::Begin(
        title.c_str(),
        &m_isOpen,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    FeedbackCurrentWindowCloseButton(wasOpen, &m_isOpen);
    if ( visible ) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2F);
        auto muted = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        muted.w *= 0.65F;
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, muted);
        if ( m_topic ) {
            const auto back = std::string(ICON_MMM_ARROW_LEFT) + "  " +
                              TR("ui.welcome.back").toString();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0, 0, 0, 0 });
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            if ( FeedbackButton(back.c_str()) ) showHome();
            ImGui::PopStyleColor(2);
        }
        const auto  available = ImGui::GetContentRegionAvail();
        const float footer =
            ImGui::GetFrameHeightWithSpacing() * (m_saveFailed ? 2.0F : 1.0F) +
            padding;
        if ( ImGui::BeginChild("WelcomeBody",
                               { 0, std::max(1.0F, available.y - footer) },
                               ImGuiChildFlags_None) ) {
            if ( m_scrollToTop ) {
                ImGui::SetScrollY(0);
                m_scrollToTop = false;
            }
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            const float margin =
                std::min(48.0F * scale, availableWidth * 0.06F);
            const float width =
                std::max(1.0F,
                         std::min((m_topic ? 620.0F : 760.0F) * scale,
                                  availableWidth - margin * 2.0F));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (availableWidth - width) * 0.5F);
            ImGui::BeginChild("WelcomeContent",
                              { width, 0 },
                              ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
            if ( m_topic ) {
                m_walkthrough.render(manager, *m_topic);
            } else {
                renderHome(manager);
            }
            // 尾部留白纳入滚动内容，统一采用用户的全局窗口内边距。
            ImGui::Dummy({ 0, ImGui::GetStyle().WindowPadding.y });
            ImGui::EndChild();
        }
        ImGui::EndChild();
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
            m_saveFailed = !config.save();
        if ( m_saveFailed )
            ImGui::TextWrapped("%s", TR("ui.welcome.save_failed").data());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
}  // namespace MMM::UI
