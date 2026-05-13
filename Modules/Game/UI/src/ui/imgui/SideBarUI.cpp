#include "ui/imgui/SideBarUI.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "ui/Icons.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include <lunasvg.h>

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
                        m_activeTab = tab;
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

void SideBarUI::update(UIManager* sourceManager)
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    float sidebarBaseW = std::stof(skinCfg.getLayoutConfig("side_bar.width"));
    float sidebarWidth = std::floor((sidebarBaseW + 12.0f) * dpiScale);
    float btnSize      = std::floor(sidebarBaseW * dpiScale);

    float       extraPaddingY = std::floor(4.0f * dpiScale);
    ImGuiStyle& style         = ImGui::GetStyle();
    float       menuBarHeight =
        ImGui::GetFontSize() + (style.FramePadding.y + extraPaddingY) * 2.0f;
    float statusBarHeight = menuBarHeight;

    // ================== C. 左侧侧边栏窗口 ==================
    float floatGap    = std::floor(4.0f * dpiScale);
    float windowRound = std::floor(8.0f * dpiScale);

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + floatGap,
               viewport->WorkPos.y + menuBarHeight + floatGap));
    ImGui::SetNextWindowSize(ImVec2(sidebarWidth,
                                    viewport->WorkSize.y - menuBarHeight -
                                        statusBarHeight - 2.0f * floatGap));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags sidebar_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    if ( ImGui::Begin("SideBarUI", nullptr, sidebar_flags) ) {
        CLayWrapperCore::instance().makeCurrent(m_layoutCtx.context);
        // --- 核心：进入窗口后，立即强制锁定所有“圆角”变量 ---
        float rounding = std::floor(6.0f * dpiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

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

            // 应用侧边栏专用字体大小
            ImFont* sideBarFont = skinCfg.getFont("pure_icons");
            if ( sideBarFont ) ImGui::PushFont(sideBarFont);

            // 绘制按钮
            std::string btnId =
                std::string(iconStr) + "##tab_" + std::to_string((int)tab);

            ImGui::SetCursorScreenPos({ rect.x, rect.y });
            if ( ImGui::Button(btnId.c_str(), { rect.width, rect.height }) ) {
                m_activeTab = (m_activeTab == tab) ? SideBarTab::None : tab;
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

            // --- 悬停提示 ---
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);
            ImGui::SetItemTooltip("%s", TabToTooltip(tab).c_str());
            if ( contentFont ) ImGui::PopFont();

            // --- 清理状态栈 ---
            if ( sideBarFont ) ImGui::PopFont();
            ImGui::PopStyleColor(1);
            if ( isActive ) {
                ImGui::PopStyleColor(3);
            } else {
                Utils::UIThemeUtils::popTransparentButtonStyles();
            }
        };

        CLayVBox vbox;
        vbox.setPadding(std::floor(6.0f * dpiScale),
                        std::floor(6.0f * dpiScale),
                        std::floor(8.0f * dpiScale),
                        std::floor(8.0f * dpiScale))
            .setSpacing(std::floor(4.0f * dpiScale))
            .addElement("SearchButton",
                        Sizing::Fixed(btnSize),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            DrawSidebarButton(
                                ICON_MMM_SEARCH, SideBarTab::Search, rect);
                        })
            .addElement("FileExplorerButton",
                        Sizing::Fixed(btnSize),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            DrawSidebarButton(ICON_MMM_FOLDER_OPEN,
                                              SideBarTab::FileExplorer,
                                              rect);
                        })
            .addElement("AudioExplorerButton",
                        Sizing::Fixed(btnSize),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            DrawSidebarButton(ICON_MMM_MUSIC,
                                              SideBarTab::AudioExplorer,
                                              rect);
                        })
            .addElement("BeatMapExplorerButton",
                        Sizing::Fixed(btnSize),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            DrawSidebarButton(ICON_MMM_FILE,
                                              SideBarTab::BeatMapExplorer,
                                              rect);
                        })
            .addSpring()
            .addElement("SettingsButton",
                        Sizing::Fixed(btnSize),
                        Sizing::Fixed(btnSize),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            DrawSidebarButton(
                                ICON_MMM_COG, SideBarTab::Settings, rect);
                        });

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        float  availH   = ImGui::GetContentRegionAvail().y;
        ImVec2 sz = vbox.renderInCurrent(startPos, { sidebarWidth, availH });
        ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

        // --- 弹出样式变量 ---
        ImGui::PopStyleVar(5);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);  // WindowPadding, WindowBorderSize, WindowRounding
}

}  // namespace MMM::UI
