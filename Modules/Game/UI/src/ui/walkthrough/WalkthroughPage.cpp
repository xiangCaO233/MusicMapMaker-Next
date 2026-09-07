#include "ui/walkthrough/WalkthroughPage.h"

#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/translation/Translation.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/markdown/MarkdownImageCache.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughModel.h"
#include "ui/walkthrough/WalkthroughService.h"
#include <algorithm>
#include <imgui.h>
#include <string>

namespace MMM::UI
{
void WalkthroughPage::render(UIManager* manager, std::size_t topicIndex)
{
    auto&       service = manager->walkthroughService();
    const auto& topics  = service.topics();
    if ( topicIndex >= topics.size() ) return;
    const auto& topic = topics[topicIndex];
    const auto& language =
        Config::AppConfig::instance().getEditorSettings().language;
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    if ( m_currentTopic != topic.m_id ) {
        m_currentTopic   = topic.m_id;
        m_expandedBranch = 0;
    }
    auto* images = manager->getView<MarkdownImageCache>("WalkthroughImages");
    if ( images &&
         (m_preparedTopic != topic.m_id || m_preparedLanguage != language) ) {
        images->prepareDocument(topic.m_description.get(language));
        for ( const auto& branch : topic.m_branches )
            for ( const auto& step : branch.m_steps )
                images->prepareDocument(step.m_body.get(language));
        m_preparedTopic    = topic.m_id;
        m_preparedLanguage = language;
    }
    MarkdownRenderOptions markdownOptions;
    markdownOptions.images = images;
    const auto& progress   = service.progress();
    ImGui::PushID(topic.m_id.c_str());
    ImGui::Dummy({ 0, 24.0F * scale });
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.75F);
    ImGui::TextWrapped("%s", topic.m_title.get(language).c_str());
    ImGui::PopFont();
    ImGui::Dummy({ 0, 16.0F * scale });
    renderMarkdown(topic.m_description.get(language), markdownOptions);
    ImGui::Dummy({ 0, 28.0F * scale });
    std::size_t finishedBranches = 0;
    for ( std::size_t index = 0; index < topic.m_branches.size(); ++index ) {
        const auto& branch = topic.m_branches[index];
        std::size_t count  = 0;
        for ( const auto& step : branch.m_steps )
            count += progress.completed(topic, step) ? 1 : 0;
        const bool done = count == branch.m_steps.size();
        if ( done ) ++finishedBranches;
        const bool expanded = m_expandedBranch == index;
        ImGui::PushID(branch.m_id.c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0F * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize,
                            expanded ? 1.0F * scale : 0.0F);
        ImGui::PushStyleColor(ImGuiCol_Border,
                              ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              expanded
                                  ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBg)
                                  : ImVec4{ 0, 0, 0, 0 });
        if ( ImGui::BeginChild(
                 "BranchCard",
                 { 0, 0 },
                 ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders,
                 ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse) ) {
            const float line      = ImGui::GetTextLineHeight();
            const float rowHeight = line + 16.0F * scale;
            const auto  row       = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
            const bool clicked =
                FeedbackButton("##BranchHeader",
                               { ImGui::GetContentRegionAvail().x, rowHeight });
            ImGui::PopStyleColor();
            const auto   end  = ImGui::GetItemRectMax();
            auto*        draw = ImGui::GetWindowDrawList();
            const ImVec2 center{ row.x + 10.0F * scale,
                                 row.y + rowHeight * 0.5F };
            const auto   accent = ImGui::GetColorU32(
                expanded || done ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
            if ( done ) {
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
                draw->AddCircle(center, 6.0F * scale, accent, 0, 1.0F * scale);
            const auto badge = std::to_string(count) + "/" +
                               std::to_string(branch.m_steps.size());
            const float badgeX =
                end.x - ImGui::CalcTextSize(badge.c_str()).x - 6.0F * scale;
            draw->PushClipRect(
                { row.x + 28.0F * scale, row.y },
                { std::max(row.x + 28.0F * scale, badgeX - 10.0F * scale),
                  end.y },
                true);
            draw->AddText({ row.x + 28.0F * scale, row.y + 8.0F * scale },
                          ImGui::GetColorU32(ImGuiCol_Text),
                          branch.m_title.get(language).c_str());
            draw->PopClipRect();
            draw->AddText({ badgeX, row.y + 8.0F * scale },
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          badge.c_str());
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip("%s", branch.m_title.get(language).c_str());
            if ( clicked ) {
                if ( expanded )
                    m_expandedBranch.reset();
                else
                    m_expandedBranch = index;
            }
            if ( expanded ) {
                ImGui::Indent(28.0F * scale);
                for ( const auto& step : branch.m_steps ) {
                    ImGui::PushID(step.m_id.c_str());
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s",
                                       step.m_title.get(language).c_str());
                    renderMarkdown(step.m_body.get(language), markdownOptions);
                    ImGui::Spacing();
                    const bool completed = progress.completed(topic, step);
                    const auto acknowledge =
                        std::string(ICON_MMM_CHECK) + "  " +
                        TR("ui.walkthrough.acknowledge").toString();
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4{ 0, 0, 0, 0 });
                    ImGui::PushStyleColor(
                        ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
                    ImGui::BeginDisabled(completed);
                    if ( FeedbackButton(acknowledge.c_str()) )
                        service.acknowledge(topic, step);
                    ImGui::EndDisabled();
                    ImGui::PopStyleColor(2);
                    if ( completed ) {
                        ImGui::SameLine();
                        ImGui::TextDisabled(
                            "%s",
                            TR(progress.source(topic, step) == "automatic"
                                   ? "ui.walkthrough.automatic"
                                   : "ui.walkthrough.manual")
                                .data());
                    }
                    if ( !step.m_action.empty() ) {
                        ImGui::BeginDisabled(!progress.available(topic, step) ||
                                             !service.hasAction(step.m_action));
                        if ( FeedbackButton(
                                 TR("ui.walkthrough.perform").data()) )
                            service.execute(step.m_action);
                        ImGui::EndDisabled();
                    }
                    ImGui::Dummy({ 0, 14.0F * scale });
                    ImGui::PopID();
                }
                ImGui::Unindent(28.0F * scale);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::PopID();
        ImGui::Dummy({ 0, 6.0F * scale });
    }
    ImGui::Dummy({ 0, 12.0F * scale });
    if ( topic.m_anyBranch ? finishedBranches > 0
                           : finishedBranches == topic.m_branches.size() )
        ImGui::TextWrapped("%s", TR("ui.walkthrough.goal_complete").data());
    ImGui::Dummy({ 0, 8.0F * scale });
    const auto resetLabel = std::string(ICON_MMM_REDO) + "  " +
                            TR("ui.walkthrough.reset").toString();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0, 0, 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    const bool resetClicked = FeedbackButton(resetLabel.c_str());
    ImGui::PopStyleColor(2);
    if ( resetClicked ) FeedbackOpenPopup("ResetWalkthrough");
    if ( ImGui::BeginPopup("ResetWalkthrough") ) {
        ImGui::TextWrapped("%s", TR("ui.walkthrough.reset_confirm").data());
        if ( FeedbackButton(TR("ui.common.confirm").data()) ) {
            service.reset(topic);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( FeedbackButton(TR("ui.common.cancel").data()) )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if ( !service.error().empty() )
        ImGui::TextWrapped("%s", service.error().c_str());
    ImGui::PopID();
}
}  // namespace MMM::UI
