#include "ui/imgui/manager/ToolbarView.h"
#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace MMM::UI
{

ToolbarView::ToolbarView(const std::string& name) : IUIView(name) {}

void ToolbarView::update(UIManager* sourceManager)
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();

    // 1. 获取图标字体尺寸以计算固定宽度
    ImFont* toolFont = skinCfg.getFont("setting_internal");

    // 技巧：我们可以在 Begin 之外临时推入字体来获取其尺寸信息，或者直接从
    // SkinManager 配置中估算 这里我们假设 setting_internal 已经加载。
    float fontSize = 18.0f;  // 默认回退值
    if ( toolFont ) {
        fontSize = toolFont->LegacySize;
    }

    float btnSize  = fontSize + 8.0f;
    float paddingX = 4.0f;
    // 强制固定宽度为 26.0f (更紧凑)
    float fixedW = 26.0f;

    // 2. 锁定窗口尺寸约束
    ImGui::SetNextWindowSizeConstraints(ImVec2(fixedW, -1), ImVec2(fixedW, -1));

    // 3. 核心标志：禁用标题栏、禁用手动调宽、禁用折叠、禁止滚动、禁止移动
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove;

    // 样式锁定：将 WindowPadding 设为 0，让按钮无缝填满窗口宽度
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // ItemSpacing 设为 0，按钮之间完全无间隙
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    // 4. 使用 " ###Toolbar" 保持 ID 稳定
    if ( ImGui::Begin(" ###Toolbar", nullptr, flags) ) {
        // --- [绘制左侧分隔线] ---
        ImVec2 p1 = ImGui::GetWindowPos();
        // 向内偏移 1px 绘制 2px
        // 宽度的线，使其一半在边界外一半在边界内，视觉效果最稳重
        p1.x += 1.0f;
        ImVec2 p2 = ImVec2(p1.x, p1.y + ImGui::GetWindowHeight());
        // 使用标准的边框颜色，带 0.6 不透明度，2px 厚度
        ImU32 col = ImGui::GetColorU32(ImGuiCol_Border, 0.6f);
        ImGui::GetWindowDrawList()->AddLine(p1, p2, col, 2.0f);

        if ( toolFont ) ImGui::PushFont(toolFont);

        // 使用固定宽度 26px 绘制按钮
        float drawW = fixedW;

        // 1. 移动工具 (使用四向箭头图标 \uf047)
        drawToolButton(ICON_MMM_MOVE_ARROWS,
                       Logic::EditTool::Move,
                       "Move Tool (V)",
                       drawW);

        // 2. 矩形选取工具
        drawToolButton(ICON_MMM_SQUARE_SELECT,
                       Logic::EditTool::Marquee,
                       "Marquee Tool (M)",
                       drawW);

        // 3. 绘制工具 (铅笔)
        drawToolButton(
            ICON_MMM_PEN, Logic::EditTool::Draw, "Draw Tool (P)", drawW);

        // 4. 裁剪工具 (剪刀)
        drawToolButton(
            ICON_MMM_SCISSORS, Logic::EditTool::Cut, "Cut Tool (C)", drawW);

        if ( toolFont ) ImGui::PopFont();
    }

    ImGui::End();

    ImGui::PopStyleVar(5);
}

void ToolbarView::drawToolButton(const char* icon, Logic::EditTool tool,
                                 const char* tooltip, float width)
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    bool                 isActive = (m_currentTool == tool);

    // 按钮高度与宽度保持一致，形成正方形
    float btnSize = width;


    if ( isActive ) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(1.0f, 1.0f, 1.0f, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
    }

    Config::Color iconColor = skinCfg.getColor("icon");
    ImVec4        iconVec4(iconColor.r, iconColor.g, iconColor.b, iconColor.a);
    if ( !isActive ) iconVec4.w *= 0.8f;
    ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

    if ( ImGui::Button(icon, ImVec2(btnSize, btnSize)) ) {
        if ( m_currentTool != tool ) {
            m_currentTool = tool;
            Logic::EditorEngine::instance().pushCommand(
                Logic::CmdChangeTool{ tool });
        }
    }

    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", tooltip);
    }

    ImGui::PopStyleColor(4);
}

}  // namespace MMM::UI
