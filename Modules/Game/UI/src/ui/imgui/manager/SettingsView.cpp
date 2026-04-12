#include "ui/imgui/manager/SettingsView.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "ui/Icons.h"
#include "ui/layout/box/CLayBox.h"

namespace MMM::UI
{

SettingsView::SettingsView(const std::string& subViewName)
    : ISubView(subViewName)
{
}

void SettingsView::onUpdate(LayoutContext& layoutContext,
                            UIManager*     sourceManager)
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    float sidebarWidth = std::stof(skinCfg.getLayoutConfig("side_bar.width"));

    CLayHBox rootHBox;
    rootHBox.setPadding(0, 0, 0, 0).setSpacing(0);

    // 左侧图标侧边栏
    rootHBox.addElement(
        "SettingsCategoryList",
        Sizing::Fixed(sidebarWidth),
        Sizing::Grow(),
        [this, sidebarWidth, &skinCfg](Clay_BoundingBox r, bool isHovered) {
            ImGui::BeginChild(
                "SettingsCategories", { r.width, r.height }, false);

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

            auto DrawCategoryIcon = [&](SettingsTab tab,
                                        const char* iconStr,
                                        const char* tooltip) {
                bool isActive = (m_currentTab == tab);

                if ( isActive ) {
                    auto c1 = skinCfg.getColor("ui.button.sidebar_active");
                    auto c2 = skinCfg.getColor("ui.button.normal");
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(c1.r, c1.g, c1.b, c1.a));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(c1.r, c1.g, c1.b, c1.a));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(c2.r, c2.g, c2.b, c2.a));
                } else {
                    auto c1 = skinCfg.getColor("ui.button.transparent");
                    auto c2 = skinCfg.getColor("ui.button.transparent_hovered");
                    auto c3 = skinCfg.getColor("ui.button.transparent_active");
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(c1.r, c1.g, c1.b, c1.a));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(c2.r, c2.g, c2.b, c2.a));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(c3.r, c3.g, c3.b, c3.a));
                }

                Config::Color iconColor = skinCfg.getColor("icon");
                ImVec4        iconVec4(
                    iconColor.r, iconColor.g, iconColor.b, iconColor.a);
                if ( !isActive ) {
                    iconVec4.w *= 0.7f;
                }
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

                ImGui::PopStyleColor(4);
            };

            DrawCategoryIcon(SettingsTab::Software,
                             ICON_MMM_DESKTOP,
                             TR_CACHE("ui.settings.software").data());
            DrawCategoryIcon(SettingsTab::Visual,
                             ICON_MMM_EYE,
                             TR_CACHE("ui.settings.visual").data());
            DrawCategoryIcon(SettingsTab::Project,
                             ICON_MMM_FOLDER,
                             TR_CACHE("ui.settings.project").data());
            DrawCategoryIcon(SettingsTab::Editor,
                             ICON_MMM_PEN,
                             TR_CACHE("ui.settings.editor").data());

            ImGui::PopStyleVar(4);
            ImGui::EndChild();
        });

    // 中间分割线
    rootHBox.addElement("SettingsSeparator",
                        Sizing::Fixed(1.0f),
                        Sizing::Grow(),
                        [](Clay_BoundingBox r, bool) {
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            ImGui::GetWindowDrawList()->AddLine(
                                { p.x + 0.0f, p.y },
                                { p.x + 0.0f, p.y + r.height },
                                IM_COL32(80, 80, 80, 255));
                        });

    // 右侧内容区
    rootHBox.addElement(
        "SettingsContentArea",
        Sizing::Grow(),
        Sizing::Grow(),
        [this, &skinCfg](Clay_BoundingBox r, bool isHovered) {
            ImGui::BeginChild("SettingsContent", { r.width, r.height }, false);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));

            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);

            switch ( m_currentTab ) {
            case SettingsTab::Software: drawSoftwareSettings(); break;
            case SettingsTab::Visual: drawVisualSettings(); break;
            case SettingsTab::Project: drawProjectSettings(); break;
            case SettingsTab::Editor: drawEditorSettings(); break;
            }

            if ( contentFont ) ImGui::PopFont();

            ImGui::PopStyleVar();
            ImGui::EndChild();
        });

    rootHBox.render(layoutContext);
}

} // namespace MMM::UI
