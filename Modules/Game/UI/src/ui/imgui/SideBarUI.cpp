#include "ui/imgui/SideBarUI.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "ui/Icons.h"
#include "ui/layout/box/CLayBox.h"
#include <lunasvg.h>

namespace MMM::UI
{

SideBarUI::SideBarUI(const std::string& name) : IUIView(name) {}

SideBarUI::~SideBarUI() {}

void SideBarUI::update(UIManager* sourceManager)
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float                dpiScale = viewport->DpiScale;

    float sidebarBaseWidth =
        std::stof(skinCfg.getLayoutConfig("side_bar.width"));
    float sidebarWidth = std::floor(sidebarBaseWidth * dpiScale);

    float       extraPaddingBaseY = 4.0f;
    float       extraPaddingY     = std::floor(extraPaddingBaseY * dpiScale);
    ImGuiStyle& style             = ImGui::GetStyle();
    float       menuBarHeight =
        ImGui::GetFontSize() + (style.FramePadding.y + extraPaddingY) * 2.0f;

    // ================== C. 左侧侧边栏窗口 ==================
    // 位置：X=0, Y=菜单高度 (使用 WorkPos 确保在多视口/缩放环境下坐标正确)
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + menuBarHeight));
    // 尺寸：宽=sidebarWidth, 高=总高 - 菜单高度
    ImGui::SetNextWindowSize(
        ImVec2(sidebarWidth, viewport->WorkSize.y - menuBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags sidebar_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;

    // --- 核心：进入窗口后，立即强制锁定所有“圆角”变量为 0 ---
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);   // 按钮圆角 -> 0
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);  // 窗口圆角 -> 0
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);   // 子窗口圆角 -> 0
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);  // 边框线 -> 0
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(0, 0));  // 项间距 -> 0
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(0, 0));  // 内部补白 -> 0

    LayoutContext ctx{ m_layoutCtx, "SideBarUI", true, sidebar_flags };
    // --- 样式锁定：全局无圆角、无边框 ---

    // lambda：绘制互斥按钮
    auto DrawSidebarButton =
        [&](const char* iconStr, SideBarTab tab, Clay_BoundingBox rect) {
            bool isActive = (m_activeTab == tab);

            // --- 样式处理 ---
            if ( isActive ) {
                // 激活态：深灰色背景（VS Code 风格）
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            } else {
                // 非激活态：全透明，仅悬停时有微弱反馈
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(1.0f, 1.0f, 1.0f, 0.1f));
            }

            // 使用不同的文字颜色（未激活时稍显灰色）
            Config::Color iconColor = skinCfg.getColor("icon");
            ImVec4 iconVec4(iconColor.r, iconColor.g, iconColor.b, iconColor.a);
            if ( !isActive ) {
                iconVec4.w *= 0.7f;  // 非激活状态稍微透明点
            }
            ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

            // 应用侧边栏专用字体大小
            ImFont* sideBarFont = skinCfg.getFont("side_bar");
            if ( sideBarFont ) ImGui::PushFont(sideBarFont);

            // 绘制按钮
            std::string btnId =
                std::string(iconStr) + "##tab_" + std::to_string((int)tab);
            if ( ImGui::Button(btnId.c_str(), { rect.width, rect.height }) ) {
                m_activeTab = (m_activeTab == tab) ? SideBarTab::None : tab;
                // 2. 发布事件通知 FloatingManagerUI
                using namespace MMM::Event;

                UISubViewToggleEvent evt;
                // 填充基类 UIEvent 信息
                evt.sourceUiName = m_name;
                evt.uiManager    = sourceManager;

                // 填充切换信息
                // 与注册 FloatingManagerUI
                // 时使用的名字一致
                evt.targetFloatManagerName = "SideBarManager";
                evt.subViewId              = TabToSubViewId(tab);

                if ( m_activeTab != SideBarTab::None ) {
                    evt.showSubView = true;
                }

                // 核心：发布到总线
                EventBus::instance().publish(evt);

                XINFO("SideBarUI: Published ToggleEvent for {}", evt.subViewId);
            }

            // --- 悬停提示 ---
            // 推入内容字体以保持 Tooltip 文字尺寸一致
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);
            ImGui::SetItemTooltip("%s", TabToTooltip(tab).c_str());
            if ( contentFont ) ImGui::PopFont();

            // --- 清理状态栈 ---
            if ( sideBarFont ) ImGui::PopFont();
            ImGui::PopStyleColor(4);  // 弹出 Button, Hovered, Active, Text
        };

    CLayVBox vbox;
    vbox.setPadding(0, 0, 0, 0)
        .setSpacing(0)
        .addElement("SearchButton",
                    Sizing::Fixed(sidebarWidth),
                    Sizing::Fixed(sidebarWidth),
                    [=](Clay_BoundingBox rect, bool isHovered) {
                        DrawSidebarButton(
                            ICON_MMM_SEARCH, SideBarTab::Search, rect);
                    })
        .addElement("FileExplorerButton",
                    Sizing::Fixed(sidebarWidth),
                    Sizing::Fixed(sidebarWidth),
                    [=](Clay_BoundingBox rect, bool isHovered) {
                        DrawSidebarButton(ICON_MMM_FOLDER_OPEN,
                                          SideBarTab::FileExplorer,
                                          rect);  // \uf15b file
                    })
        .addElement("AudioExplorerButton",
                    Sizing::Fixed(sidebarWidth),
                    Sizing::Fixed(sidebarWidth),
                    [=](Clay_BoundingBox rect, bool isHovered) {
                        DrawSidebarButton(ICON_MMM_MUSIC,
                                          SideBarTab::AudioExplorer,
                                          rect);  // \uf001 music
                    })
        .addElement("BeatMapExplorerButton",
                    Sizing::Fixed(sidebarWidth),
                    Sizing::Fixed(sidebarWidth),
                    [=](Clay_BoundingBox rect, bool isHovered) {
                        DrawSidebarButton(
                            ICON_MMM_FILE,
                            SideBarTab::BeatMapExplorer,
                            rect);  // \uf07c folder-open (打开文件)
                    })
        .addSpring()
        .addElement("SettingsButton",
                    Sizing::Fixed(sidebarWidth),
                    Sizing::Fixed(sidebarWidth),
                    [=](Clay_BoundingBox rect, bool isHovered) {
                        DrawSidebarButton(ICON_MMM_COG,
                                          SideBarTab::Settings,
                                          rect);  // \uf013 cog
                    });
    vbox.render(ctx);

    // --- 弹出样式变量 ---
    ImGui::PopStyleVar(6);
}

}  // namespace MMM::UI
