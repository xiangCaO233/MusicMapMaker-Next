#define IMGUI_DEFINE_MATH_OPERATORS
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/MainMenuView.h"

#include <imgui.h>
#include <string>

namespace MMM::UI
{

/// @brief 渲染保存快捷提示气泡。
/// @warning UI 热路径：每帧执行；仅在保存提示计时器有效时绘制前景气泡。
void MainMenuView::renderSaveTooltip()
{
    if ( m_saveTooltipTimer <= 0.0f ) return;

    m_saveTooltipTimer -= ImGui::GetIO().DeltaTime;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2         mousePos = ImGui::GetMousePos();
    const float    dpiScale =
        Config::AppConfig::instance().getWindowContentScale();

    ImVec2 pivot = ImVec2(0.0f, 0.0f);
    if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f )
        pivot.x = 1.0f;
    if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f )
        pivot.y = 1.0f;

    float offsetX = (pivot.x == 0.0f) ? 20.0f * dpiScale : -20.0f * dpiScale;
    float offsetY = (pivot.y == 0.0f) ? 20.0f * dpiScale : -20.0f * dpiScale;

    std::string message = m_saveTooltipMessage.empty()
                              ? TR("ui.status.beatmap.saved").data()
                              : m_saveTooltipMessage;
    std::string text    = std::string(ICON_MMM_SAVE) + "  " + message;

    ImVec2 padding{ 16.0f * dpiScale, 10.0f * dpiScale };
    ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    ImVec2 size{ textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f };
    ImVec2 pos{ mousePos.x + offsetX, mousePos.y + offsetY };
    ImVec2 rectMin{ pos.x - size.x * pivot.x, pos.y - size.y * pivot.y };
    ImVec2 rectMax{ rectMin.x + size.x, rectMin.y + size.y };

    ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
    ImU32 bgColor   = ImGui::GetColorU32(ImVec4(0.04f, 0.05f, 0.07f, 0.88f));
    ImU32 textColor = ImGui::GetColorU32(
        m_saveTooltipSuccess ? ImVec4(0.45f, 1.0f, 0.48f, 1.0f)
                             : ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
    drawList->AddRectFilled(rectMin, rectMax, bgColor, 8.0f * dpiScale);
    drawList->AddText(ImVec2(rectMin.x + padding.x, rectMin.y + padding.y),
                      textColor,
                      text.c_str());
}

}  // namespace MMM::UI
