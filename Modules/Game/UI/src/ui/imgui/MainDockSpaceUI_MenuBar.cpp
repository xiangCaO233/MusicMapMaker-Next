#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/ui/GLFWNativeEvent.h"
#include "event/ui/UpdateDragAreaEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/Icons.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/utils/UIThemeUtils.h"
#include <GLFW/glfw3.h>
#include <memory>

namespace MMM::UI
{

void MainDockSpaceUI::renderMenuBar(UIManager* sourceManager,
                                    float menuBarHeight, float sidebarWidth,
                                    float toolbarWidth, float dpiScale)
{
    dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, menuBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags menu_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBackground;

    ImGuiStyle& style             = ImGui::GetStyle();
    float       extraPaddingBaseY = 4.0f;
    float       extraPaddingY     = std::floor(extraPaddingBaseY * dpiScale);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(style.FramePadding.x, style.FramePadding.y + extraPaddingY));

    ImGui::Begin("TopMenuBarHost", nullptr, menu_flags);

    if ( ImGui::BeginMenuBar() ) {
        float  buttonSize          = menuBarHeight;
        ImVec2 defaultFramePadding = ImGui::GetStyle().FramePadding;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

        auto DrawIconButton = [&](const char*                          str_id,
                                  std::unique_ptr<Graphic::VKTexture>& tex,
                                  float                                btnSize,
                                  ImVec4 hoverColor) -> bool {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);

            bool clicked = ImGui::Button(str_id, ImVec2(btnSize, btnSize));

            if ( tex ) {
                ImTextureID imTexId  = (ImTextureID)tex->getImTextureID();
                float       iconSize = btnSize * 0.65f;
                ImVec2      p_min    = ImGui::GetItemRectMin();
                float       offsetX  = std::floor((btnSize - iconSize) * 0.5f);
                float       offsetY  = std::floor((btnSize - iconSize) * 0.5f);
                ImVec2      img_p1   = { p_min.x + offsetX, p_min.y + offsetY };
                ImVec2 img_p2 = { img_p1.x + iconSize, img_p1.y + iconSize };
                ImU32  tint   = ImGui::IsItemActive()
                                    ? IM_COL32(180, 180, 180, 255)
                                    : IM_COL32_WHITE;
                ImGui::GetWindowDrawList()->AddImage(
                    imTexId, img_p1, img_p2, { 0, 0 }, { 1, 1 }, tint);
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(1);
            return clicked;
        };

        auto DrawFontIconButton =
            [&](const char* icon, float btnSize, ImVec4 hoverColor) -> bool {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);

            ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

            bool clicked = ImGui::Button(icon, ImVec2(btnSize, btnSize));

            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(1);
            return clicked;
        };

        ImVec4 textCol   = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImVec4 hoverVec4 = ImVec4(textCol.x, textCol.y, textCol.z, 0.1f);

        ImGui::SetCursorPosX(0.0f);
        DrawIconButton("##logo", m_logo_texture, buttonSize, hoverVec4);

        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(std::floor(10.0f * dpiScale), defaultFramePadding.y));
        ImGui::SetCursorPosX(buttonSize + 4.0f);

        ImFont* menuFont = skinCfg.getFont("menu");
        if ( menuFont ) ImGui::PushFont(menuFont);
        m_mainMenuview.renderMenus(sourceManager);

        float dragStartX = ImGui::GetCursorPosX();

        m_mainMenuview.renderInfoText();
        if ( menuFont ) ImGui::PopFont();

        ImGui::PopStyleVar(1);

        float numberOfButtons = 3;
        float dragEndX =
            ImGui::GetWindowWidth() - (buttonSize * numberOfButtons);
        ImGui::SetCursorPosX(dragEndX);

        static float lastDragStartX = -1.0f;
        static float lastDragEndX   = -1.0f;
        if ( dragStartX != lastDragStartX || dragEndX != lastDragEndX ) {
            lastDragStartX = dragStartX;
            lastDragEndX   = dragEndX;
            Event::UpdateDragAreaEvent e;
            e.uiManager    = sourceManager;
            e.sourceUiName = "TopMenuBarHost";
            e.areas.push_back(
                { dragStartX, 0.0f, dragEndX - dragStartX, menuBarHeight });
            Event::EventBus::instance().publish(e);
        }

        ImGui::SetCursorPosX(dragStartX);
        ImGui::InvisibleButton("DragArea",
                               ImVec2(dragEndX - dragStartX, menuBarHeight));
        if ( ImGui::IsItemHovered() &&
             ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE });
        }
        ImGui::SetCursorPosX(dragEndX);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImFont* contentFont = skinCfg.getFont("content");

        if ( DrawFontIconButton(ICON_MMM_MINIMIZE, buttonSize, hoverVec4) ) {
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_ICONFY_WINDOW });
        }
        if ( contentFont ) ImGui::PushFont(contentFont);
        ImGui::SetItemTooltip("%s", TR("ui.window.minimize").data());
        if ( contentFont ) ImGui::PopFont();

        ImGui::SameLine();

        const char* maxIcon =
            m_isMaximized ? ICON_MMM_RESTORE : ICON_MMM_MAXIMIZE;
        const char* maxTip = m_isMaximized ? TR("ui.window.restore").data()
                                           : TR("ui.window.maximize").data();

        if ( DrawFontIconButton(maxIcon, buttonSize, hoverVec4) ) {
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE });
        }
        if ( contentFont ) ImGui::PushFont(contentFont);
        ImGui::SetItemTooltip("%s", maxTip);
        if ( contentFont ) ImGui::PopFont();

        ImGui::SameLine();

        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        if ( DrawFontIconButton(ICON_MMM_CLOSE, buttonSize, dangerCol) ) {
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_CLOSE_WINDOW });
        }
        if ( contentFont ) ImGui::PushFont(contentFont);
        ImGui::SetItemTooltip("%s", TR("ui.window.close").data());
        if ( contentFont ) ImGui::PopFont();

        ImGui::PopStyleVar(1);
        ImGui::PopStyleVar(2);
        ImGui::EndMenuBar();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

}  // namespace MMM::UI
