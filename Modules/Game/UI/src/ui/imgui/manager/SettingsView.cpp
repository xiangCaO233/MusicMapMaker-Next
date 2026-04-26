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

                ImGui::PopStyleColor(1);

                if ( isActive ) {
                    ImGui::PopStyleColor(3);
                } else {
                    Utils::UIThemeUtils::popTransparentButtonStyles();
                }
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
            // 在 BeginChild 之前推入样式变量，确保子窗口内部布局使用该边距
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(35.0f, 25.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(10.0f, 15.0f));

            if ( ImGui::BeginChild(
                     "SettingsContent", { r.width, r.height }, false) ) {
                ImFont* contentFont = skinCfg.getFont("content");
                if ( contentFont ) ImGui::PushFont(contentFont);

                // 强制全局缩进，确保即便某些组件忽略 WindowPadding
                // 也能有基础间距
                ImGui::Indent(10.0f);

                switch ( m_currentTab ) {
                case Event::SettingsTab::Software:
                    drawSoftwareSettings();
                    break;
                case Event::SettingsTab::Visual: drawVisualSettings(); break;
                case Event::SettingsTab::Project: drawProjectSettings(); break;
                case Event::SettingsTab::Beatmap: drawBeatmapSettings(); break;
                case Event::SettingsTab::Editor: drawEditorSettings(); break;
                }

                ImGui::Unindent(10.0f);

                // 底部增加大量留白，防止最后一个控件贴边
                ImGui::Dummy(ImVec2(0, 50));

                if ( contentFont ) ImGui::PopFont();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
        });

    rootHBox.render(layoutContext);
}

}  // namespace MMM::UI
