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
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float                dpiScale = viewport->DpiScale;

    // 从逻辑引擎同步当前工具状态
    m_currentTool = Logic::EditorEngine::instance().getCurrentTool();

    // 1. 获取图标字体尺寸以计算固定宽度
    ImFont* toolFont = skinCfg.getFont("setting_internal");

    // 技巧：我们可以在 Begin 之外临时推入字体来获取其尺寸信息，或者直接从
    // SkinManager 配置中估算 这里我们假设 setting_internal 已经加载。
    float fontSize = 18.0f;  // 默认回退值
    if ( toolFont ) {
        fontSize = toolFont->LegacySize;
    }

    // 强制固定宽度为 32.0f (略微放宽以适配内容)
    float fixedBaseW = 32.0f;
    float fixedW     = std::floor(fixedBaseW * dpiScale);

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

    auto pushBtnStyle = [&](bool active) {
        if ( active ) {
            auto c1 = skinCfg.getColor("ui.button.normal");
            auto c2 = skinCfg.getColor("ui.button.normal_hovered");
            auto c3 = skinCfg.getColor("ui.button.normal_active");
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(c1.r, c1.g, c1.b, c1.a));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(c2.r, c2.g, c2.b, c2.a));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(c3.r, c3.g, c3.b, c3.a));
        } else {
            auto c1 = skinCfg.getColor("ui.button.transparent");
            auto c2 = skinCfg.getColor("ui.button.transparent_hovered");
            auto c3 = skinCfg.getColor("ui.button.transparent_active");
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(c1.r, c1.g, c1.b, c1.a));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(c2.r, c2.g, c2.b, c2.a));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(c3.r, c3.g, c3.b, c3.a));
        }
    };

    // 4. 使用 " ###Toolbar" 保持 ID 稳定
    if ( ImGui::Begin(" ###Toolbar", nullptr, flags) ) {
        // --- 绘制左侧分隔线 ---
        ImVec2 p1 = ImGui::GetWindowPos();
        // 向内偏移 1px 绘制 2px
        // 宽度的线，使其一半在边界外一半在边界内，视觉效果最稳重
        p1.x += 1.0f;
        ImVec2 p2 = ImVec2(p1.x, p1.y + ImGui::GetWindowHeight());
        // 使用标准的边框颜色，带 0.6 不透明度，2px 厚度
        ImU32 col = ImGui::GetColorU32(ImGuiCol_Border, 0.6f);
        ImGui::GetWindowDrawList()->AddLine(
            p1, p2, col, std::floor(2.0f * dpiScale));

        if ( toolFont ) ImGui::PushFont(toolFont);

        // 使用当前窗口的实际宽度作为按钮宽度，确保完全一致
        float drawW = ImGui::GetWindowWidth();

        // 1. 移动工具 (使用四向箭头图标 \uf047)
        ImGui::SetCursorPosX(0);
        drawToolButton(ICON_MMM_MOVE_ARROWS,
                       Logic::EditTool::Move,
                       TR("ui.toolbar.move"),
                       drawW);

        // 2. 矩形选取工具
        ImGui::SetCursorPosX(0);
        drawToolButton(ICON_MMM_SQUARE_SELECT,
                       Logic::EditTool::Marquee,
                       TR("ui.toolbar.marquee"),
                       drawW);

        // 3. 绘制工具 (铅笔)
        ImGui::SetCursorPosX(0);
        drawToolButton(
            ICON_MMM_PEN, Logic::EditTool::Draw, TR("ui.toolbar.draw"), drawW);

        // 4. 裁剪工具 (剪刀)
        ImGui::SetCursorPosX(0);
        drawToolButton(ICON_MMM_SCISSORS,
                       Logic::EditTool::Cut,
                       TR("ui.toolbar.cut"),
                       drawW);

        ImGui::Separator();

        // --- 鼠标滚动翻转开关 ---
        auto editorCfg = Logic::EditorEngine::instance().getEditorConfig();
        bool isReverse = editorCfg.settings.reverseScroll;

        pushBtnStyle(isReverse);

        ImGui::SetCursorPosX(0);
        if ( ImGui::Button(ICON_MMM_ARROWS_UP_DOWN, ImVec2(drawW, drawW)) ) {
            auto newConfig                   = editorCfg;
            newConfig.settings.reverseScroll = !isReverse;
            Logic::EditorEngine::instance().setEditorConfig(newConfig);
        }

        if ( ImGui::IsItemHovered() ) {
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);
            ImGui::SetTooltip("%s", TR("ui.toolbar.reverse_scroll").data());
            if ( contentFont ) ImGui::PopFont();
        }
        ImGui::PopStyleColor(3);

        // --- 滚动磁吸开关 ---
        bool isScrollSnap = editorCfg.settings.scrollSnap;
        pushBtnStyle(isScrollSnap);

        ImGui::SetCursorPosX(0);
        if ( ImGui::Button(ICON_MMM_MAGNET, ImVec2(drawW, drawW)) ) {
            auto newConfig                = editorCfg;
            newConfig.settings.scrollSnap = !isScrollSnap;
            Logic::EditorEngine::instance().setEditorConfig(newConfig);
        }

        if ( ImGui::IsItemHovered() ) {
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);
            ImGui::SetTooltip("%s", TR("ui.toolbar.scroll_snap").data());
            if ( contentFont ) ImGui::PopFont();
        }
        ImGui::PopStyleColor(3);

        // --- 线性滚轮映射开关 (SCROLLTIMING) ---
        // 选中: 使用SCROLLTIMING映射 (enableLinearScrollMapping = false)
        // 未选中: 纯线性映射 (enableLinearScrollMapping = true)
        bool isTimingMapped = !editorCfg.visual.enableLinearScrollMapping;
        pushBtnStyle(isTimingMapped);

        ImGui::SetCursorPosX(0);
        if ( ImGui::Button(ICON_MMM_EYE, ImVec2(drawW, drawW)) ) {
            auto newConfig = editorCfg;
            newConfig.visual.enableLinearScrollMapping =
                isTimingMapped;  // toggle it
            Logic::EditorEngine::instance().setEditorConfig(newConfig);
        }

        if ( ImGui::IsItemHovered() ) {
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);
            ImGui::SetTooltip("%s",
                              TR("ui.toolbar.scroll_timing_mapping").data());
            if ( contentFont ) ImGui::PopFont();
        }
        ImGui::PopStyleColor(3);

        ImGui::Separator();

        if ( toolFont ) ImGui::PopFont();

        // --- 分拍数量设置 (Beat Divisor) ---
        int currentDivisor = editorCfg.settings.beatDivisor;
        pushBtnStyle(m_showDivisorPopup);

        ImFont* contentFont = skinCfg.getFont("content");
        if ( contentFont ) ImGui::PushFont(contentFont);

        char divisorBuf[16];
        snprintf(divisorBuf, sizeof(divisorBuf), "%d", currentDivisor);

        ImGui::SetCursorPosX(0);
        if ( ImGui::Button(divisorBuf, ImVec2(drawW, drawW)) ) {
            m_showDivisorPopup = !m_showDivisorPopup;
        }

        // 记录按钮的Y坐标，用于悬浮窗对齐
        m_lastBtnY = ImGui::GetItemRectMin().y;

        if ( ImGui::IsItemHovered() ) {
            // 响应滚轮调整分拍
            float wheel = ImGui::GetIO().MouseWheel;
            if ( std::abs(wheel) > 0.1f ) {
                int delta = (wheel > 0) ? 1 : -1;
                if ( ImGui::GetIO().KeyShift )
                    delta *= static_cast<int>(
                        editorCfg.settings.scrollSpeedMultiplier);
                int newDivisor = std::clamp(currentDivisor + delta, 1, 64);
                if ( newDivisor != currentDivisor ) {
                    auto newConfig                 = editorCfg;
                    newConfig.settings.beatDivisor = newDivisor;
                    Logic::EditorEngine::instance().setEditorConfig(newConfig);
                }
            }

            ImGui::SetTooltip("%s", TR("ui.toolbar.beat_divisor").data());
        }
        if ( contentFont ) ImGui::PopFont();
        ImGui::PopStyleColor(3);

        // 如果之前弹出了 toolFont，这里就不需要再 Pop 了
        // 为了保持逻辑一致性，我们将最后的 PopFont 移到这个 if 外部或者调整逻辑
    }

    ImGui::End();

    // 我们必须在这个位置把上面为 Begin() 推入的5个样式弹出来
    ImGui::PopStyleVar(5);

    // --- 绘制分拍数量设置悬浮窗 ---
    if ( m_showDivisorPopup ) {
        // 在 Toolbar 窗口左侧显示悬浮窗

        ImVec2 toolbarPos = ImGui::FindWindowByName(" ###Toolbar")->Pos;
        // X = 工具栏左边缘往左 4px
        // Y = 按钮的顶部对齐
        ImVec2 popupPos =
            ImVec2(toolbarPos.x - std::floor(4.0f * dpiScale), m_lastBtnY);

        // Pivot(1.0, 0.0) 代表将弹窗的右上角对齐到 popupPos
        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));

        ImGuiWindowFlags popupFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                            std::floor(4.0f * dpiScale));
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2(std::floor(8.0f * dpiScale), std::floor(8.0f * dpiScale)));
        auto bgCol     = skinCfg.getColor("ui.button.disabled_bg");
        auto borderCol = skinCfg.getColor("ui.border");
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
                              ImVec4(bgCol.r, bgCol.g, bgCol.b, bgCol.a));
        ImGui::PushStyleColor(
            ImGuiCol_Border,
            ImVec4(borderCol.r, borderCol.g, borderCol.b, borderCol.a));

        if ( ImGui::Begin("##BeatDivisorPopup", nullptr, popupFlags) ) {
            auto editorCfg = Logic::EditorEngine::instance().getEditorConfig();
            int  currentDivisor = editorCfg.settings.beatDivisor;

            ImGui::TextUnformatted(TR("ui.toolbar.beat_divisor"));
            ImGui::Separator();

            ImGui::SetNextItemWidth(std::floor(120.0f * dpiScale));
            if ( ImGui::SliderInt("##DivisorSlider", &currentDivisor, 1, 64) ) {
                auto newConfig                 = editorCfg;
                newConfig.settings.beatDivisor = currentDivisor;
                Logic::EditorEngine::instance().setEditorConfig(newConfig);
            }

            // 可以加一些常用的快速设置按钮
            const int commonDivisors[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
            for ( int i = 0; i < 8; ++i ) {
                if ( i > 0 && i % 4 != 0 ) ImGui::SameLine();
                char buf[16];
                snprintf(buf, sizeof(buf), "1/%d", commonDivisors[i]);
                if ( ImGui::Button(buf,
                                   ImVec2(std::floor(35.0f * dpiScale),
                                          std::floor(24.0f * dpiScale))) ) {
                    auto newConfig                 = editorCfg;
                    newConfig.settings.beatDivisor = commonDivisors[i];
                    Logic::EditorEngine::instance().setEditorConfig(newConfig);
                }
            }
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}

void ToolbarView::drawToolButton(const char* icon, Logic::EditTool tool,
                                 const char* tooltip, float width)
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    bool                 isActive = (m_currentTool == tool);

    // 按钮高度与宽度保持一致，形成正方形
    float btnSize = width;


    if ( isActive ) {
        auto c1 = skinCfg.getColor("ui.button.normal");
        auto c2 = skinCfg.getColor("ui.button.normal_hovered");
        auto c3 = skinCfg.getColor("ui.button.normal_active");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c1.r, c1.g, c1.b, c1.a));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(c2.r, c2.g, c2.b, c2.a));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(c3.r, c3.g, c3.b, c3.a));
    } else {
        auto c1 = skinCfg.getColor("ui.button.transparent");
        auto c2 = skinCfg.getColor("ui.button.transparent_hovered");
        auto c3 = skinCfg.getColor("ui.button.transparent_active");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c1.r, c1.g, c1.b, c1.a));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(c2.r, c2.g, c2.b, c2.a));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(c3.r, c3.g, c3.b, c3.a));
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
        ImFont* contentFont = skinCfg.getFont("content");
        if ( contentFont ) ImGui::PushFont(contentFont);
        ImGui::SetTooltip("%s", tooltip);
        if ( contentFont ) ImGui::PopFont();
    }

    ImGui::PopStyleColor(4);
}

}  // namespace MMM::UI
