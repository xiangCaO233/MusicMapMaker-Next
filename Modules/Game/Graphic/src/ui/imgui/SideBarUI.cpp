#include "ui/imgui/SideBarUI.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "ui/ITextureLoader.h"
#include "ui/layout/box/CLayBox.h"
#include <lunasvg.h>

namespace MMM::Graphic::UI
{

SideBarUI::SideBarUI(const std::string& name) : ITextureLoader(name) {}

SideBarUI::~SideBarUI()
{
    m_tabIcons.clear();
}

void SideBarUI::update()
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    static float         sidebarWidth =
        std::stof(skinCfg.getLayoutConfig("side_bar.width"));
    static float sidebarIconSize =
        std::stof(skinCfg.getLayoutConfig("side_bar.icon_size"));
    const ImGuiViewport* viewport      = ImGui::GetMainViewport();
    float                menuBarHeight = ImGui::GetFrameHeight();

    // ================== C. 左侧侧边栏窗口 ==================
    // 位置：X=0, Y=菜单高度
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + menuBarHeight));
    // 尺寸：宽=48, 高=总高 - 菜单高度
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
        [&](const char* label, SideBarTab tab, Clay_BoundingBox rect) {
            bool isActive = (m_activeTab == tab);
            // 获取纹理
            VKTexture* tex = nullptr;
            if ( m_tabIcons.count(tab) ) tex = m_tabIcons[tab].get();

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

            // 绘制透明按钮捕捉点击
            std::string btnId = "##tab_" + std::to_string((int)tab);
            if ( ImGui::Button(btnId.c_str(), { rect.width, rect.height }) ) {
                m_activeTab = (m_activeTab == tab) ? SideBarTab::None : tab;
            }

            // --- 核心：绘制图标 ---
            if ( tex ) {
                // 1. 触发自动注册并获取 ID
                ImTextureID imTexId = (ImTextureID)tex->getImTextureID();

                // 2. 计算居中位置
                float iconSize =
                    sidebarIconSize;  // 图标在 32px 按钮里的实际显示大小
                ImVec2 p_min = ImGui::GetItemRectMin();
                ImVec2 p_max = ImGui::GetItemRectMax();

                float offsetX = (rect.width - iconSize) * 0.5f;
                float offsetY = (rect.height - iconSize) * 0.5f;

                ImVec2 img_p1 = { p_min.x + offsetX, p_min.y + offsetY };
                ImVec2 img_p2 = { img_p1.x + iconSize, img_p1.y + iconSize };

                // 3. 绘制图标
                // 使用不同的着色（未激活时稍显灰色）
                ImU32 tint =
                    isActive ? IM_COL32_WHITE : IM_COL32(200, 200, 200, 255);

                ImGui::GetWindowDrawList()->AddImage(
                    imTexId, img_p1, img_p2, { 0, 0 }, { 1, 1 }, tint);
            }

            // 如果是激活状态，在左侧画一个蓝色的指示条（可选，VS Code 风格）
            if ( isActive ) {
                ImVec2 p_min = ImGui::GetItemRectMin();
                ImVec2 p_max = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    p_min,
                    ImVec2(p_min.x + 3.0f, p_max.y),
                    IM_COL32(0, 120, 215, 255));
            }

            // --- 清理状态栈 ---
            ImGui::PopStyleColor(3);  // 弹出 Button 和 ButtonHovered
        };

    CLayVBox vbox;
    vbox.setPadding(0, 0, 0, 0)
        .setSpacing(0)
        .addElement("FileExplorerButton",
                    Sizing::Fixed(sidebarWidth),
                    Sizing::Fixed(sidebarWidth),
                    [=](Clay_BoundingBox rect, bool isHovered) {
                        DrawSidebarButton(
                            "File", SideBarTab::FileExplorer, rect);
                    })
        .addElement("AudioExplorerButton",
                    Sizing::Fixed(sidebarWidth),
                    Sizing::Fixed(sidebarWidth),
                    [=](Clay_BoundingBox rect, bool isHovered) {
                        DrawSidebarButton(
                            "Audio", SideBarTab::AudioExplorer, rect);
                    })
        .addSpring();
    vbox.render(ctx);

    // --- 弹出样式变量 ---
    ImGui::PopStyleVar(6);
}

/// @brief 是否需要重载
bool SideBarUI::needReload()
{
    return std::exchange(m_needReload, false);
}

/// @brief 重载纹理
void SideBarUI::reloadTextures(vk::PhysicalDevice& physicalDevice,
                               vk::Device&         logicalDevice,
                               vk::CommandPool& cmdPool, vk::Queue& queue)
{
    auto&          skin     = Config::SkinManager::instance();
    const uint32_t iconSize = 32;

    // 直接调用基类生产接口，只需要给路径
    m_tabIcons[SideBarTab::FileExplorer] =
        loadTextureResource(skin.getAssetPath("side_bar.file_explorer_icon"),
                            iconSize,
                            physicalDevice,
                            logicalDevice,
                            cmdPool,
                            queue,
                            { { .83f, .83f, .83f, .83f } });

    m_tabIcons[SideBarTab::AudioExplorer] =
        loadTextureResource(skin.getAssetPath("side_bar.audio_explorer_icon"),
                            iconSize,
                            physicalDevice,
                            logicalDevice,
                            cmdPool,
                            queue,
                            { { .83f, .83f, .83f, .83f } });

    XINFO("SideBar textures reloaded.");
}

}  // namespace MMM::Graphic::UI
