#include "ui/imgui/SideBarUI.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/ui/UISettingsTabEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "logic/ProjectController.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <limits>

namespace MMM::UI
{

SideBarUI::SideBarUI(const std::string& name) : IUIView(name)
{
    m_subId =
        Event::EventBus::instance().subscribe<Event::UISubViewToggleEvent>(
            [this](const Event::UISubViewToggleEvent& e) {
                if ( e.targetFloatManagerName == "SideBarManager" ) {
                    auto tab = SubViewIdToTab(e.subViewId);
                    if ( tab != SideBarTab::None ) {
                        if ( e.showSubView ) {
                            m_activeTab = tab;
                            if ( e.sourceUiName != m_name ) {
                                persistWorkspaceActiveTab(m_activeTab);
                            }
                        } else if ( m_activeTab == tab ) {
                            m_activeTab = SideBarTab::None;
                            if ( e.sourceUiName != m_name ) {
                                persistWorkspaceActiveTab(m_activeTab);
                            }
                        }
                    }
                }
            });
}

SideBarUI::~SideBarUI()
{
    if ( m_subId != 0 ) {
        Event::EventBus::instance().unsubscribe<Event::UISubViewToggleEvent>(
            m_subId);
    }
}

std::string SideBarUI::workspaceNameFromTab(SideBarTab tab)
{
    switch ( tab ) {
    case SideBarTab::Search: return "Search";
    case SideBarTab::FileExplorer: return "FileExplorer";
    case SideBarTab::AudioExplorer: return "AudioExplorer";
    case SideBarTab::BeatMapExplorer: return "BeatMapExplorer";
    case SideBarTab::Settings:
    case SideBarTab::None:
    default: return "None";
    }
}

SideBarTab SideBarUI::workspaceNameToTab(const std::string& name)
{
    if ( name == "Search" ) return SideBarTab::Search;
    if ( name == "FileExplorer" ) return SideBarTab::FileExplorer;
    if ( name == "AudioExplorer" ) return SideBarTab::AudioExplorer;
    if ( name == "BeatMapExplorer" ) return SideBarTab::BeatMapExplorer;
    return SideBarTab::None;
}

SideBarTab SideBarUI::getActiveTab() const
{
    return m_activeTab;
}

void SideBarUI::setActiveTab(SideBarTab tab)
{
    m_activeTab = tab;
}

void SideBarUI::persistWorkspaceActiveTab(SideBarTab tab) const
{
    auto* project = Logic::ProjectController::instance().currentProject();
    if ( !project ) {
        return;
    }

    project->m_settings.m_workspace.m_sidebarActiveTab =
        SideBarUI::workspaceNameFromTab(tab);
    Logic::ProjectController::instance().saveProject();
}

void SideBarUI::update(UIManager* sourceManager)
{
    if ( auto* sideBarManager =
             sourceManager->getView<FloatingManagerUI>("SideBarManager") ) {
        SideBarTab managerTab = SideBarTab::None;
        if ( sideBarManager->isVisible() ) {
            managerTab = SubViewIdToTab(sideBarManager->getCurrentSubViewId());
        }
        if ( managerTab != m_activeTab ) {
            m_activeTab = managerTab;
        }
    }

    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    float sidebarBaseW = std::stof(skinCfg.getLayoutConfig("side_bar.width"));
    float sidebarWidth = GetSidebarWidth(dpiScale);
    float btnSize      = std::floor(sidebarBaseW * dpiScale);
    bool  showManagerLabels =
        Config::AppConfig::instance().getEditorSettings().showManagerLabels;
    float btnHeight =
        showManagerLabels ? std::floor(48.0f * dpiScale) : btnSize;
    float expandedBtnW = sidebarWidth;

    float       extraPaddingY = std::floor(4.0f * dpiScale);
    ImGuiStyle& style         = ImGui::GetStyle();
    float       menuBarHeight =
        ImGui::GetFontSize() + (style.FramePadding.y + extraPaddingY) * 2.0f;
    float statusBarHeight = menuBarHeight;

    // ================== C. 左侧侧边栏窗口 ==================
    auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    float floatGap    = std::floor(aesthetics.windowGap * dpiScale);
    float windowRound = std::floor(aesthetics.windowRounding * dpiScale);

    float windowPaddingVal  = std::floor(aesthetics.windowPadding * dpiScale);
    float totalSidebarWidth = sidebarWidth + 2.0f * windowPaddingVal;

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + floatGap,
               viewport->WorkPos.y + menuBarHeight + floatGap),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(totalSidebarWidth,
                                    viewport->WorkSize.y - menuBarHeight -
                                        statusBarHeight - 2.0f * floatGap));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags sidebar_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;

    float windowPadding = std::floor(aesthetics.windowPadding * dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(windowPadding, windowPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    if ( ImGui::Begin("SideBarUI", nullptr, sidebar_flags) ) {
        CLayWrapperCore::instance().makeCurrent(m_layoutCtx.context);
        // --- 核心：进入窗口后，立即强制锁定所有“圆角”变量 ---
        float rounding = std::floor(aesthetics.frameRounding * dpiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        Utils::pushFixedButtonStyleVars();

        // lambda：绘制互斥按钮
        auto DrawSidebarButton = [&](const char*      iconStr,
                                     SideBarTab       tab,
                                     Clay_BoundingBox rect) {
            bool isActive = (m_activeTab == tab);

            // --- 样式处理 ---
            if ( isActive ) {
                ImVec4 activeCol =
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
            } else {
                Utils::UIThemeUtils::pushTransparentButtonStyles();
            }

            // 使用不同的文字颜色（未激活时稍显透明）
            ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if ( !isActive ) {
                iconVec4.w *= 0.7f;  // 非激活状态稍微透明点
            }
            ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

            // 1. 绘制按钮交互背景
            ImGui::SetCursorScreenPos({ rect.x, rect.y });
            std::string btnId = "##tab_btn_" + std::to_string((int)tab);
            if ( ::MMM::UI::FeedbackButton(btnId.c_str(),
                                           { rect.width, rect.height }) ) {
                if ( tab == SideBarTab::Settings ) {
                    sourceManager->openSettingsWindow(
                        Event::SettingsTab::Software);
                } else {
                    m_activeTab = (m_activeTab == tab) ? SideBarTab::None : tab;
                    persistWorkspaceActiveTab(m_activeTab);
                    // 2. 发布事件通知 FloatingManagerUI
                    using namespace MMM::Event;

                    UISubViewToggleEvent evt;
                    evt.sourceUiName           = m_name;
                    evt.uiManager              = sourceManager;
                    evt.targetFloatManagerName = "SideBarManager";
                    evt.subViewId              = TabToSubViewId(tab);

                    if ( m_activeTab != SideBarTab::None ) {
                        evt.showSubView = true;
                    }

                    EventBus::instance().publish(evt);
                }
            }

            // --- 绘制内容 (图标 + 可选短标签) ---
            float iconAreaW = rect.width;

            // [A] 绘制图标
            ImFont* sideBarFont = skinCfg.getFont("pure_icons");
            if ( sideBarFont ) {
                ImGui::PushFont(sideBarFont, sideBarFont->LegacySize);
            }
            ImVec2 iconSize = ImGui::CalcTextSize(iconStr);
            float  iconY    = rect.y + (rect.height - iconSize.y) * 0.5f;
            if ( showManagerLabels ) {
                iconY = rect.y + std::floor(5.0f * dpiScale);
            }
            ImVec2 iconPos = { rect.x + (iconAreaW - iconSize.x) * 0.5f,
                               iconY };
            ImGui::GetWindowDrawList()->AddText(
                sideBarFont,
                ImGui::GetFontSize(),
                iconPos,
                ImGui::GetColorU32(ImGuiCol_Text),
                iconStr);
            if ( sideBarFont ) ImGui::PopFont();

            if ( showManagerLabels ) {
                std::string label    = TabToShortLabel(tab);
                ImFont*     menuFont = skinCfg.getFont("menu");
                if ( menuFont ) {
                    float labelFontSize = std::floor(
                        std::min(ImGui::GetFontSize(), rect.width * 0.42f));
                    ImVec2 labelSize = menuFont->CalcTextSizeA(
                        labelFontSize,
                        std::numeric_limits<float>::max(),
                        0.0f,
                        label.c_str());
                    ImVec2 labelPos = {
                        rect.x + (rect.width - labelSize.x) * 0.5f,
                        rect.y + rect.height - labelSize.y -
                            std::floor(4.0f * dpiScale),
                    };
                    ImGui::GetWindowDrawList()->AddText(
                        menuFont,
                        labelFontSize,
                        labelPos,
                        ImGui::GetColorU32(ImGuiCol_Text),
                        label.c_str());
                }
            }

            // --- 悬停提示 (保留以增强可用性) ---
            Utils::renderTooltip(TabToTooltip(tab).c_str(),
                                 Utils::TooltipDir::Right);

            // --- 清理状态栈 ---
            ImGui::PopStyleColor(1);
            if ( isActive ) {
                ImGui::PopStyleColor(3);
            } else {
                Utils::UIThemeUtils::popTransparentButtonStyles();
            }
        };

        CLayVBox vbox;
        vbox.setPadding(0, 0, 0, 0)
            .setSpacing(std::floor(aesthetics.itemSpacing * dpiScale))
            .addElement("SearchButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            DrawSidebarButton(
                                ICON_MMM_SEARCH, SideBarTab::Search, rect);
                        })
            .addElement("FileExplorerButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            DrawSidebarButton(ICON_MMM_FOLDER_OPEN,
                                              SideBarTab::FileExplorer,
                                              rect);
                        })
            .addElement("AudioExplorerButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            DrawSidebarButton(ICON_MMM_MUSIC,
                                              SideBarTab::AudioExplorer,
                                              rect);
                        })
            .addElement("BeatMapExplorerButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            DrawSidebarButton(ICON_MMM_FILE,
                                              SideBarTab::BeatMapExplorer,
                                              rect);
                        })
            .addSpring()
            .addElement("SettingsButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            DrawSidebarButton(
                                ICON_MMM_COG, SideBarTab::Settings, rect);
                        });

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        float  availH   = ImGui::GetContentRegionAvail().y;
        float  availW   = ImGui::GetContentRegionAvail().x;
        ImVec2 sz       = vbox.renderInCurrent(startPos, { availW, availH });
        ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

        // --- 弹出样式变量 ---
        Utils::popFixedButtonStyleVars();
        ImGui::PopStyleVar(4);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);  // WindowPadding, WindowBorderSize, WindowRounding
}

}  // namespace MMM::UI
