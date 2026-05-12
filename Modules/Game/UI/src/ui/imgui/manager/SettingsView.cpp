#include "ui/imgui/manager/SettingsView.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "imgui.h"
#include "ui/Icons.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"

namespace MMM::UI
{

SettingsView::SettingsView(const std::string& subViewName)
    : ISubView(subViewName)
{
    m_tabSubId =
        Event::EventBus::instance().subscribe<Event::UISettingsTabEvent>(
            [this](const Event::UISettingsTabEvent& e) {
                m_currentTab = e.tab;
            });
}

SettingsView::~SettingsView()
{
    if ( m_tabSubId != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::UISettingsTabEvent>(
            m_tabSubId);
    }
}

CLayHBox& SettingsView::getRow(size_t index)
{
    if ( index >= m_settingRows.size() ) {
        m_settingRows.emplace_back();
    }
    m_settingRows[index].clear();
    return m_settingRows[index];
}

CLayVBox& SettingsView::getSection(size_t index)
{
    if ( index >= m_sectionBoxes.size() ) {
        m_sectionBoxes.emplace_back();
    }
    m_sectionBoxes[index].clear();
    return m_sectionBoxes[index];
}

void SettingsView::onUpdate(LayoutContext& layoutContext,
                            UIManager*     sourceManager)
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    float sidebarWidth = std::stof(skinCfg.getLayoutConfig("side_bar.width"));

    // 1. 左侧图标侧边栏 (标准 ImGui)
    ImGui::BeginChild("SettingsCategories", { sidebarWidth, 0 }, false);
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

        auto DrawCategoryIcon = [&](Event::SettingsTab tab,
                                    const char*        iconStr,
                                    const char*        tooltip) {
            bool isActive = (m_currentTab == tab);
            if ( isActive ) {
                ImVec4 activeCol =
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
            } else {
                Utils::UIThemeUtils::pushTransparentButtonStyles();
            }

            ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if ( !isActive ) iconVec4.w *= 0.7f;
            ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

            ImFont* settingIconFont = skinCfg.getFont("setting_internal");
            if ( settingIconFont ) ImGui::PushFont(settingIconFont);

            std::string btnId = std::string(iconStr) + "##setting_tab_" +
                                std::to_string((int)tab);
            if ( ImGui::Button(btnId.c_str(),
                               { sidebarWidth, sidebarWidth }) ) {
                m_currentTab = tab;
            }

            if ( settingIconFont ) ImGui::PopFont();

            if ( ImGui::IsItemHovered() ) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(tooltip);
                ImGui::EndTooltip();
            }

            ImGui::PopStyleColor(1);
            if ( isActive )
                ImGui::PopStyleColor(3);
            else
                Utils::UIThemeUtils::popTransparentButtonStyles();
        };

        DrawCategoryIcon(Event::SettingsTab::Software,
                         ICON_MMM_DESKTOP,
                         TR_CACHE("ui.settings.software").data());
        DrawCategoryIcon(Event::SettingsTab::Visual,
                         ICON_MMM_EYE,
                         TR_CACHE("ui.settings.visual").data());
        DrawCategoryIcon(Event::SettingsTab::Project,
                         ICON_MMM_FOLDER,
                         TR_CACHE("ui.settings.project").data());
        DrawCategoryIcon(Event::SettingsTab::Beatmap,
                         ICON_MMM_FILE,
                         TR_CACHE("ui.settings.beatmap").data());
        DrawCategoryIcon(Event::SettingsTab::Editor,
                         ICON_MMM_PEN,
                         TR_CACHE("ui.settings.editor").data());

        ImGui::PopStyleVar(4);
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 0);

    // 2. 中间分割线
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float  h = ImGui::GetContentRegionAvail().y;
        ImGui::GetWindowDrawList()->AddLine(
            { p.x, p.y }, { p.x, p.y + h }, IM_COL32(80, 80, 80, 255));
        ImGui::Dummy({ 1.0f, h });
    }

    ImGui::SameLine(0, 0);

    // 3. 右侧内容区 (标准 ImGui，内部调用 Clay)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 25.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 15.0f));

        if ( ImGui::BeginChild("SettingsContent", { 0, 0 }, false) ) {
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);

            switch ( m_currentTab ) {
            case Event::SettingsTab::Software: drawSoftwareSettings(); break;
            case Event::SettingsTab::Visual: drawVisualSettings(); break;
            case Event::SettingsTab::Project: drawProjectSettings(); break;
            case Event::SettingsTab::Beatmap: drawBeatmapSettings(); break;
            case Event::SettingsTab::Editor: drawEditorSettings(); break;
            }

            ImGui::Dummy(ImVec2(0, 50));
            if ( contentFont ) ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }
}

}  // namespace MMM::UI
