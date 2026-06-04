#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/ui/GLFWNativeEvent.h"
#include "event/ui/UpdateDragAreaEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
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
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoDocking;

    ImGuiStyle& style             = ImGui::GetStyle();
    float       extraPaddingBaseY = 4.0f;
    float       extraPaddingY     = std::floor(extraPaddingBaseY * dpiScale);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(style.FramePadding.x, style.FramePadding.y + extraPaddingY));

    // 确保菜单栏背景色同步为 MenuBarBg
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg,
                          ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
    // 设置 WindowBg 以确保完全覆盖
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyle().Colors[ImGuiCol_TextLink]);

    ImGui::Begin(
        "TopMenuBarHost", nullptr, menu_flags & ~ImGuiWindowFlags_NoBackground);

    if ( ImGui::BeginMenuBar() ) {
        float  buttonSize          = menuBarHeight;
        ImVec2 defaultFramePadding = ImGui::GetStyle().FramePadding;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

        auto DrawIconButton = [&](const char*                          str_id,
                                  std::unique_ptr<Graphic::VKTexture>& tex,
                                  float                                btnSize,
                                  ImVec4 hoverColor) -> bool {
            Utils::pushFixedButtonStyleVars();
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
            Utils::popFixedButtonStyleVars();
            return clicked;
        };

        auto DrawFontIconButton =
            [&](const char* icon, float btnSize, ImVec4 hoverColor) -> bool {
            Utils::pushFixedButtonStyleVars();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);

            ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

            bool clicked = ImGui::Button(icon, ImVec2(btnSize, btnSize));

            ImGui::PopStyleColor(3);
            Utils::popFixedButtonStyleVars();
            return clicked;
        };

        ImVec4 textCol   = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImVec4 hoverVec4 = ImVec4(textCol.x, textCol.y, textCol.z, 0.1f);

        // 1. Logo (固定左侧)
        ImGui::SetCursorPosX(0.0f);
        DrawIconButton("##logo", m_logo_texture, buttonSize, hoverVec4);

        // 2. 菜单栏 (紧跟 Logo)
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(std::floor(10.0f * dpiScale), defaultFramePadding.y));
        ImGui::SetCursorPosX(buttonSize + 4.0f * dpiScale);

        ImFont* menuFont = skinCfg.getFont("menu");
        m_mainMenuview.renderMenus(sourceManager);
        ImGui::PopStyleVar(1);

        float barWidth  = ImGui::GetWindowWidth();
        float menusEndX = ImGui::GetCursorPosX();

        // 3. 标题：MusicMapMaker-Next (严格居中)
        const char* titleText = "MusicMapMaker-Next";
        if ( menuFont ) ImGui::PushFont(menuFont);
        float titleWidth = ImGui::CalcTextSize(titleText).x;
        float titleX     = (barWidth - titleWidth) * 0.5f;
        ImGui::SetCursorPosX(titleX);
        ImGui::TextUnformatted(titleText);
        float titleEndX = titleX + titleWidth;

        // 4. 帧信息 (FPS & UPS) - 靠右对齐到按钮左侧
        ImGuiIO&    io       = ImGui::GetIO();
        float       logicUps = Logic::EditorEngine::instance().getLogicUps();
        std::string fpsStr   = TR_FMT("ui.menu.frame_stats_fmt",
                                      1000.0f / io.Framerate,
                                      io.Framerate,
                                      logicUps);
        float       fpsWidth = ImGui::CalcTextSize(fpsStr.c_str()).x;

        float numberOfButtons  = 3;
        float buttonsAreaWidth = buttonSize * numberOfButtons;
        float buttonsStartX    = barWidth - buttonsAreaWidth;

        float fpsGap = std::floor(12.0f * dpiScale);
        float fpsX   = buttonsStartX - fpsGap - fpsWidth;
        ImGui::SetCursorPosX(fpsX);
        ImGui::TextUnformatted(fpsStr.c_str());
        float fpsEndX = fpsX + fpsWidth;
        if ( menuFont ) ImGui::PopFont();

        // 5. 拖拽区域 (Springs)
        // Area 1: MenusEnd -> TitleX
        // Area 2: TitleX -> TitleEnd (标题文字本身)
        // Area 3: TitleEnd -> ButtonsStartX (包含 FPS 信息)

        static std::vector<Event::DragArea> lastAreas;
        std::vector<Event::DragArea>        currentAreas;
        currentAreas.push_back(
            { menusEndX, 0.0f, titleX - menusEndX, menuBarHeight });
        currentAreas.push_back(
            { titleX, 0.0f, titleEndX - titleX, menuBarHeight });
        currentAreas.push_back(
            { titleEndX, 0.0f, buttonsStartX - titleEndX, menuBarHeight });

        bool areasChanged = (currentAreas.size() != lastAreas.size());
        if ( !areasChanged ) {
            for ( size_t i = 0; i < currentAreas.size(); ++i ) {
                if ( currentAreas[i].x != lastAreas[i].x ||
                     currentAreas[i].w != lastAreas[i].w ) {
                    areasChanged = true;
                    break;
                }
            }
        }

        if ( areasChanged ) {
            lastAreas = currentAreas;
            Event::UpdateDragAreaEvent e;
            e.uiManager    = sourceManager;
            e.sourceUiName = "TopMenuBarHost";
            e.areas        = currentAreas;
            Event::EventBus::instance().publish(e);
        }

        // 绘制透明按钮以捕获双击全屏事件
        for ( const auto& area : currentAreas ) {
            if ( area.w > 0 ) {
                ImGui::SetCursorPosX(area.x);
                ImGui::InvisibleButton(
                    fmt::format("DragArea##{}", area.x).c_str(),
                    ImVec2(area.w, area.h));
                if ( ImGui::IsItemHovered() &&
                     ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
                    Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                        .type = Event::NativeEventType::
                            GLFW_TOGGLE_WINDOW_MAXIMIZE });
                }
            }
        }

        // 6. 分隔线与窗口控制按钮
        float  lineX     = buttonsStartX - fpsGap * 0.5f;
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec4 sepCol    = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        sepCol.w *= 0.5f;  // 进一步增加不透明度
        ImGui::GetWindowDrawList()->AddLine(
            { windowPos.x + lineX, windowPos.y + menuBarHeight * 0.25f },
            { windowPos.x + lineX, windowPos.y + menuBarHeight * 0.75f },
            ImGui::GetColorU32(sepCol),
            1.5f);  // 稍微加粗

        ImGui::SetCursorPosX(buttonsStartX);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImFont* contentFont = skinCfg.getFont("content");

        if ( DrawFontIconButton(ICON_MMM_MINIMIZE, buttonSize, hoverVec4) ) {
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_ICONFY_WINDOW });
        }
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
            if ( contentFont ) ImGui::PushFont(contentFont);
            ImGui::SetTooltip("%s", TR("ui.window.minimize").data());
            if ( contentFont ) ImGui::PopFont();
        }

        ImGui::SameLine();

        const char* maxIcon =
            m_isMaximized ? ICON_MMM_RESTORE : ICON_MMM_MAXIMIZE;
        const char* maxTip = m_isMaximized ? TR("ui.window.restore").data()
                                           : TR("ui.window.maximize").data();

        if ( DrawFontIconButton(maxIcon, buttonSize, hoverVec4) ) {
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE });
        }
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
            if ( contentFont ) ImGui::PushFont(contentFont);
            ImGui::SetTooltip("%s", maxTip);
            if ( contentFont ) ImGui::PopFont();
        }

        ImGui::SameLine();

        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        if ( DrawFontIconButton(ICON_MMM_CLOSE, buttonSize, dangerCol) ) {
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_CLOSE_WINDOW });
        }
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
            if ( contentFont ) ImGui::PushFont(contentFont);
            ImGui::SetTooltip("%s", TR("ui.window.close").data());
            if ( contentFont ) ImGui::PopFont();
        }

        ImGui::PopStyleVar(1);
        ImGui::PopStyleVar(2);
        ImGui::EndMenuBar();
    }
    ImGui::End();
    ImGui::PopStyleColor(3);  // MenuBarBg, WindowBg, Text
    ImGui::PopStyleVar(3);
}

}  // namespace MMM::UI
