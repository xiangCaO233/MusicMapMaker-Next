#include "ui/imgui/manager/SettingsView.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "imgui.h"
#include "ui/Icons.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>

namespace MMM::UI
{

/// @brief 构造设置面板视图并订阅设置页切换事件。
/// @param viewName 视图名称。
SettingsView::SettingsView(const std::string& viewName) : IUIView(viewName)
{
    m_tabSubId =
        Event::EventBus::instance().subscribe<Event::UISettingsTabEvent>(
            [this](const Event::UISettingsTabEvent& e) { open(e.tab); });
}

/// @brief 析构设置面板视图并取消设置页切换事件订阅。
SettingsView::~SettingsView()
{
    if ( m_tabSubId != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::UISettingsTabEvent>(
            m_tabSubId);
    }
}

/// @brief 获取或创建指定索引的设置项行布局。
/// @param index 行布局缓存索引。
/// @return 已清空并可复用的横向行布局。
CLayHBox& SettingsView::getRow(size_t index)
{
    if ( index >= m_settingRows.size() ) {
        m_settingRows.emplace_back();
    }
    m_settingRows[index].clear();
    return m_settingRows[index];
}

/// @brief 获取或创建指定索引的设置段落布局。
/// @param index 段落布局缓存索引。
/// @return 已清空并可复用的纵向段落布局。
CLayVBox& SettingsView::getSection(size_t index)
{
    if ( index >= m_sectionBoxes.size() ) {
        m_sectionBoxes.emplace_back();
    }
    m_sectionBoxes[index].clear();
    return m_sectionBoxes[index];
}

/// @brief 打开设置窗口并切换到指定设置页。
/// @param tab 需要激活的设置页。
void SettingsView::open(Event::SettingsTab tab)
{
    m_currentTab            = tab;
    m_isOpen                = true;
    m_focusNextFrame        = true;
    m_dockToCenterNextFrame = true;
}

/// @brief 请求下一帧将设置窗口停靠到主编辑区中心标签页。
void SettingsView::requestDockToCenter()
{
    m_dockToCenterNextFrame = true;
}

/// @brief 请求下一帧聚焦设置窗口。
void SettingsView::requestFocus()
{
    m_focusNextFrame = true;
}

/// @brief 更新并渲染独立设置窗口。
/// @param sourceManager 当前 UI 管理器。
void SettingsView::update(UIManager* sourceManager)
{
    (void)sourceManager;

    std::string windowName =
        std::string(TR("title.settings_manager").data()) + "###SettingsWindow";

    ImGuiID dockId = 0;
    if ( m_dockToCenterNextFrame ) {
        dockId = MainDockSpaceUI::getCenterDockId();
    }
    if ( m_focusNextFrame ) {
        ImGui::SetNextWindowFocus();
        m_focusNextFrame = false;
    }

    LayoutContext layoutContext(m_layoutCtx,
                                windowName,
                                false,
                                ImGuiWindowFlags_None,
                                &m_isOpen,
                                dockId,
                                ImGuiCond_Always);
    (void)layoutContext;
    if ( dockId != 0 ) {
        m_dockToCenterNextFrame = false;
    }

    drawContent();
}

/// @brief 绘制设置视图内容。
void SettingsView::drawContent()
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    float sidebarBaseW = std::stof(skinCfg.getLayoutConfig("side_bar.width"));
    float btnSize      = std::floor(sidebarBaseW * dpiScale);

    auto GetCategoryShortLabel = [](Event::SettingsTab tab) -> const char* {
        switch ( tab ) {
        case Event::SettingsTab::Software:
            return TR_CACHE("ui.settings.software.short").data();
        case Event::SettingsTab::Visual:
            return TR_CACHE("ui.settings.visual.short").data();
        case Event::SettingsTab::Project:
            return TR_CACHE("ui.settings.project.short").data();
        case Event::SettingsTab::Beatmap:
            return TR_CACHE("ui.settings.beatmap.short").data();
        case Event::SettingsTab::Editor:
            return TR_CACHE("ui.settings.editor.short").data();
        }
        return "";
    };

    float maxLabelWidth = 0.0f;
    if ( ImFont* menuFont = skinCfg.getFont("menu");
         menuFont && ImGui::GetCurrentContext() ) {
        ImGui::PushFont(menuFont);
        const char* labels[] = {
            GetCategoryShortLabel(Event::SettingsTab::Software),
            GetCategoryShortLabel(Event::SettingsTab::Visual),
            GetCategoryShortLabel(Event::SettingsTab::Project),
            GetCategoryShortLabel(Event::SettingsTab::Beatmap),
            GetCategoryShortLabel(Event::SettingsTab::Editor)
        };
        for ( const char* label : labels ) {
            maxLabelWidth =
                std::max(maxLabelWidth, ImGui::CalcTextSize(label).x);
        }
        ImGui::PopFont();
    }
    if ( maxLabelWidth < 1.0f ) {
        maxLabelWidth = std::floor(48.0f * dpiScale);
    }

    float sepAreaW     = std::floor(12.0f * dpiScale);
    float labelPadding = std::floor(12.0f * dpiScale);
    float vboxPadding  = std::floor(12.0f * dpiScale);
    float sidebarWidth = std::floor(btnSize + sepAreaW + maxLabelWidth +
                                    labelPadding + vboxPadding);

    // 1. 左侧图标侧边栏 (Clay 布局)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::BeginChild("SettingsCategories", { sidebarWidth, 0 }, false);
    {
        auto& aesthetics =
            Config::AppConfig::instance().getEditorSettings().aesthetics;
        float rounding = std::floor(aesthetics.frameRounding * dpiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        Utils::pushFixedButtonStyleVars();

        auto DrawCategoryButton = [&](Event::SettingsTab tab,
                                      const char*        iconStr,
                                      const char*        tooltip,
                                      Clay_BoundingBox   rect) {
            ImGui::SetCursorScreenPos({ rect.x, rect.y });

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

            std::string btnId = "##setting_tab_" + std::to_string((int)tab);
            if ( ImGui::Button(btnId.c_str(), { rect.width, rect.height }) ) {
                m_currentTab = tab;
            }

            float iconAreaW = btnSize;
            float sepX      = rect.x + iconAreaW;

            ImFont* iconFont = skinCfg.getFont("pure_icons");
            if ( !iconFont ) {
                iconFont = skinCfg.getFont("setting_internal");
            }
            if ( iconFont ) ImGui::PushFont(iconFont);
            ImFont* drawIconFont = iconFont ? iconFont : ImGui::GetFont();
            ImVec2  iconSize     = ImGui::CalcTextSize(iconStr);
            ImVec2  iconPos = { rect.x + (iconAreaW - iconSize.x) * 0.5f,
                                rect.y + (rect.height - iconSize.y) * 0.5f };
            ImGui::GetWindowDrawList()->AddText(
                drawIconFont,
                ImGui::GetFontSize(),
                iconPos,
                ImGui::GetColorU32(ImGuiCol_Text),
                iconStr);
            if ( iconFont ) ImGui::PopFont();

            ImVec4 sepCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            sepCol.w *= 0.3f;
            ImGui::GetWindowDrawList()->AddLine(
                { sepX, rect.y + rect.height * 0.25f },
                { sepX, rect.y + rect.height * 0.75f },
                ImGui::GetColorU32(sepCol),
                std::floor(1.0f * dpiScale));

            std::string label    = GetCategoryShortLabel(tab);
            ImFont*     menuFont = skinCfg.getFont("menu");
            if ( menuFont ) {
                ImGui::PushFont(menuFont);
                ImVec2 labelSize       = ImGui::CalcTextSize(label.c_str());
                float  textLeftPadding = std::floor(8.0f * dpiScale);
                ImVec2 labelPos = { sepX + textLeftPadding,
                                    rect.y +
                                        (rect.height - labelSize.y) * 0.5f };
                ImGui::GetWindowDrawList()->AddText(
                    menuFont,
                    ImGui::GetFontSize(),
                    labelPos,
                    ImGui::GetColorU32(ImGuiCol_Text),
                    label.c_str());
                ImGui::PopFont();
            }

            Utils::renderTooltip(tooltip, Utils::TooltipDir::Right);

            ImGui::PopStyleColor(1);
            if ( isActive )
                ImGui::PopStyleColor(3);
            else
                Utils::UIThemeUtils::popTransparentButtonStyles();
        };

        CLayVBox vbox;
        vbox.setPadding(std::floor(6.0f * dpiScale),
                        std::floor(6.0f * dpiScale),
                        std::floor(8.0f * dpiScale),
                        std::floor(8.0f * dpiScale))
            .setSpacing(std::floor(aesthetics.itemSpacing * dpiScale));

        vbox.addElement("SoftwareTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Software,
                                ICON_MMM_DESKTOP,
                                TR_CACHE("ui.settings.software").data(),
                                rect);
                        });

        vbox.addElement("VisualTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Visual,
                                ICON_MMM_EYE,
                                TR_CACHE("ui.settings.visual").data(),
                                rect);
                        });

        vbox.addElement("ProjectTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Project,
                                ICON_MMM_FOLDER,
                                TR_CACHE("ui.settings.project").data(),
                                rect);
                        });

        vbox.addElement("BeatmapTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Beatmap,
                                ICON_MMM_FILE,
                                TR_CACHE("ui.settings.beatmap").data(),
                                rect);
                        });

        vbox.addElement("EditorTab",
                        Sizing::Grow(),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool) {
                            DrawCategoryButton(
                                Event::SettingsTab::Editor,
                                ICON_MMM_PEN,
                                TR_CACHE("ui.settings.editor").data(),
                                rect);
                        });

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        vbox.renderInCurrent(
            startPos, { sidebarWidth, ImGui::GetContentRegionAvail().y });

        Utils::popFixedButtonStyleVars();
        ImGui::PopStyleVar(2);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);  // WindowPadding, ChildRounding

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
