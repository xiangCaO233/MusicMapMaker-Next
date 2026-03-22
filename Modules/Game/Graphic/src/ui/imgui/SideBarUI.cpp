#include "ui/imgui/SideBarUI.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "ui/layout/box/CLayBox.h"
#include <lunasvg.h>

namespace MMM::Graphic::UI
{

SideBarUI::SideBarUI(const std::string& name) : IUIView(name)
{
    auto loadSvg = [&](SideBarTab tab, const std::string& assetKey) {
        auto path = Config::SkinManager::instance().getAssetPath(assetKey);

        // 1. 使用 lunasvg 加载并渲染
        auto doc = lunasvg::Document::loadFromFile(path.string());
        if ( !doc ) {
            XWARN("Failed to load SVG: {}", path.string());
            return;
        }

        // 侧边栏按钮通常是 48px，我们栅格化为 48x48 (或者 96x96
        // 以获得更好的清晰度)
        uint32_t size   = 48;
        auto     bitmap = doc->renderToBitmap(size, size);
        bitmap.convertToRGBA();

        // 2. 调用你的 VKTexture 内存构造函数
        // 这里会把像素上传到显存，但还不会注册给 ImGui
        m_tabIcons[tab] = std::make_unique<VKTexture>(
            bitmap.data(), size, size, phys, device, pool, queue);
    };

    // 读取图标路径
    auto file_icon = Config::SkinManager::instance().getAssetPath(
        "side_bar.file_explorer_icon");
    auto audio_icon = Config::SkinManager::instance().getAssetPath(
        "side_bar.audio_explorer_icon");
}

void SideBarUI::update()
{
    const ImGuiViewport* viewport      = ImGui::GetMainViewport();
    float                sidebarWidth  = 48.0f;
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

            // --- 核心交互逻辑 ---
            if ( ImGui::Button(label, { rect.width, rect.height }) ) {
                if ( m_activeTab == tab ) {
                    // 如果点的是已经选中的按钮 -> 取消选中
                    m_activeTab = SideBarTab::None;
                } else {
                    // 如果点的是未选中的按钮 -> 切换到该按钮
                    m_activeTab = tab;
                }
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
                    Sizing::Fixed(48),
                    Sizing::Fixed(48),
                    [=](Clay_BoundingBox rect, bool isHovered) {
                        DrawSidebarButton(
                            "File", SideBarTab::FileExplorer, rect);
                    })
        .addElement("AudioExplorerButton",
                    Sizing::Fixed(48),
                    Sizing::Fixed(48),
                    [=](Clay_BoundingBox rect, bool isHovered) {
                        DrawSidebarButton(
                            "Audio", SideBarTab::AudioExplorer, rect);
                    })
        .addSpring();
    vbox.render(ctx.m_avail.x, ctx.m_avail.y, ctx.m_startPos);

    // --- 弹出样式变量 ---
    ImGui::PopStyleVar(6);
}

}  // namespace MMM::Graphic::UI
