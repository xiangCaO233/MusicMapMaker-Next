#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>

/// @file DockTabCloseHoverTest.cpp
/// @brief Dock 标签关闭按钮同时显示标签悬浮色与危险操作色的绘制回归测试。
/// @details 测试建立无平台后端的最小 DockSpace，使用 ImGui 内部标签几何解析
/// 关闭按钮中心，并通过宿主 DrawList 的顶点颜色验证视觉反馈。
/// @note 测试禁用交互音效，只检查绘制与 HoveredId 状态。

namespace
{
/// @brief 回归测试中的 Dock 宿主窗口名称。
/// @note 固定名称保证跨帧复用同一个 ImGuiWindow 和 DockNode。
constexpr const char* DOCK_HOST_NAME = "DockTabCloseHoverHost";

/// @brief 回归测试中的第一个 Dock 窗口名称。
/// @note 该窗口作为非活动标签，确保测试存在真实标签栏。
constexpr const char* FIRST_WINDOW_NAME = "First Beatmap";

/// @brief 回归测试中的第二个 Dock 窗口名称。
/// @note 该窗口的关闭按钮是最终悬停目标。
constexpr const char* SECOND_WINDOW_NAME = "Second Beatmap";

/// @brief 关闭按钮悬浮时必须出现的固定危险操作色。
/// @note 数值与 FeedbackCurrentWindowCloseButton 的危险色契约一致。
constexpr ImU32 EXPECTED_CLOSE_HOVER_COLOR = IM_COL32(222, 48, 62, 255);

/// @brief 绘制一帧包含两个可关闭窗口的 DockSpace。
/// @param mousePosition 本帧鼠标屏幕坐标。
/// @details 宿主和两个标签窗口每帧按稳定顺序提交，两个 open 标记均在本帧
/// 保持 true，避免测试鼠标移动期间意外关闭窗口。
/// @warning 测试 UI 路径：每帧只提交一个 DockSpace 与两个窗口。
void drawDockFrame(const ImVec2& mousePosition)
{
    // 通过事件队列注入坐标，遵循 ImGui 当前输入 API。
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mousePosition.x, mousePosition.y);
    ImGui::NewFrame();

    // 宿主固定覆盖测试显示区域，消除 ini 布局和系统窗口位置影响。
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(640.0F, 360.0F), ImGuiCond_Always);
    // 宿主本身不显示标题栏，也不参与导航或前景层级变化。
    ImGui::Begin(DOCK_HOST_NAME,
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus);
    // 稳定 DockSpace ID 让两个子窗口跨帧保持在同一标签栏。
    const ImGuiID dockspaceId = ImGui::GetID("DockTabCloseHoverDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0F, 0.0F));
    ImGui::End();

    // 第一个窗口建立初始标签并执行统一关闭反馈辅助函数。
    bool firstOpen = true;
    ImGui::SetNextWindowDockID(dockspaceId, ImGuiCond_Always);
    ImGui::Begin(FIRST_WINDOW_NAME, &firstOpen);
    MMM::UI::FeedbackCurrentWindowCloseButton(true, &firstOpen);
    ImGui::End();

    // 第二个窗口建立测试目标标签，调用时传入 Begin 前的打开状态。
    bool secondOpen = true;
    ImGui::SetNextWindowDockID(dockspaceId, ImGuiCond_Always);
    ImGui::Begin(SECOND_WINDOW_NAME, &secondOpen);
    MMM::UI::FeedbackCurrentWindowCloseButton(true, &secondOpen);
    ImGui::End();

    // Render 完成标签栏几何与颜色顶点生成，供后续检查读取。
    ImGui::Render();
}

/// @brief 计算绘制列表中使用指定颜色的顶点数。
/// @param drawList 待检查的绘制列表。
/// @param color 目标打包颜色。
/// @return 颜色完全匹配的顶点数。
int countVerticesWithColor(const ImDrawList* drawList, ImU32 color)
{
    if ( !drawList ) {
        // Dock 宿主尚无绘制列表时按零匹配处理，而不是解引用空指针。
        return 0;
    }

    // 精确比较 ImGui 已打包的 RGBA 值，不依赖顶点位置或索引顺序。
    int count = 0;
    for ( const ImDrawVert& vertex : drawList->VtxBuffer ) {
        if ( vertex.col == color ) {
            // 一个填充矩形通常贡献至少四个具有相同颜色的顶点。
            ++count;
        }
    }
    return count;
}

/// @brief 获取指定 Dock 窗口关闭按钮的中心坐标。
/// @param windowName 目标窗口名称。
/// @param center 输出关闭按钮中心坐标。
/// @return Dock 标签与关闭按钮几何可用时返回 true。
/// @details 计算方式与 ImGui 标签栏布局使用相同的 Offset、ScrollingAnim、
/// FramePadding 和字体尺寸，避免硬编码绝对坐标。
bool resolveCloseButtonCenter(const char* windowName, ImVec2* center)
{
    // 只有已停靠且标签栏生成完成的窗口才具有关闭按钮几何。
    ImGuiWindow* window = ImGui::FindWindowByName(windowName);
    if ( !window || !window->DockNode || !window->DockNode->TabBar ||
         !center ) {
        return false;
    }

    // DockNode 的 TabBar 保存窗口到标签项的映射。
    ImGuiTabBar*  tabBar = window->DockNode->TabBar;
    ImGuiTabItem* target = nullptr;
    for ( auto& tab : tabBar->Tabs ) {
        if ( tab.Window == window ) {
            // 窗口指针比可见标题更稳定，也避免本地化名称比较。
            target = &tab;
            break;
        }
    }
    if ( !target ) {
        // 窗口存在但尚未完成 Dock 布局时等待下一帧重试。
        return false;
    }

    // 标签起点包含滚动动画偏移，并按 ImGui 内部规则截断到像素。
    const float tabX = tabBar->BarRect.Min.x +
                       std::trunc(target->Offset - tabBar->ScrollingAnim);
    // 关闭按钮使用当前字体高度作为正方形边长。
    const float buttonSize = ImGui::GetFontSize();
    // max 保证窄标签中的按钮不会落到标签左边界之外。
    const float buttonX = std::max(
        tabX, tabX + target->Width - tabBar->FramePadding.x - buttonSize);
    const float buttonY = tabBar->BarRect.Min.y + tabBar->FramePadding.y;
    // 输出按钮几何中心，使鼠标事件稳定落入命中区域内部。
    *center = ImVec2(buttonX + buttonSize * 0.5F, buttonY + buttonSize * 0.5F);
    return true;
}

/// @brief 验证关闭按钮悬浮同时补齐标签高亮与红色危险高亮。
/// @return 成功时返回 0；否则返回可定位失败阶段的非零编码。
/// @details 前两帧建立 Dock 布局，随后多帧重复向解析出的按钮中心注入鼠标，
/// 等待 ImGui 输入队列、悬停状态和标签绘制全部稳定。
int testCloseHoverRendersTabAndDangerHighlights()
{
    // 使用鲜明自定义标签悬浮色，避免与其他主题颜色误匹配。
    ImGuiStyle& style                 = ImGui::GetStyle();
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.12F, 0.78F, 0.44F, 1.0F);
    const ImU32 expectedTabHoverColor =
        ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_TabHovered]);

    // 前两帧把鼠标放在视口外，仅用于建立并稳定 DockNode 与标签栏。
    drawDockFrame(ImVec2(-1000.0F, -1000.0F));
    drawDockFrame(ImVec2(-1000.0F, -1000.0F));

    // 第一次解析获得当前标签栏中目标关闭按钮的位置。
    ImVec2 closeCenter;
    if ( !resolveCloseButtonCenter(SECOND_WINDOW_NAME, &closeCenter) ) {
        return 1;
    }
    // 输入坐标一帧后重新解析，覆盖标签滚动动画可能造成的位置变化。
    drawDockFrame(closeCenter);
    if ( !resolveCloseButtonCenter(SECOND_WINDOW_NAME, &closeCenter) ) {
        return 1;
    }
    // 再推进一帧，使 ImGui 的 HoveredId 与最终绘制使用相同稳定坐标。
    drawDockFrame(closeCenter);
    if ( !resolveCloseButtonCenter(SECOND_WINDOW_NAME, &closeCenter) ) {
        return 1;
    }
    // 最终帧的 DrawList 和 HoveredId 是本场景的权威观察结果。
    drawDockFrame(closeCenter);

    // 目标窗口、DockNode 或标签栏缺失分别归入布局建立失败。
    ImGuiWindow* window = ImGui::FindWindowByName(SECOND_WINDOW_NAME);
    if ( !window || !window->DockNode || !window->DockNode->TabBar ) {
        return 2;
    }
    // 关闭按钮 ID 按辅助函数使用的 #CLOSE 标签与窗口种子重建。
    const ImGuiID closeButtonId = ImHashStr("#CLOSE", 0, window->ID);
    if ( GImGui->HoveredId != closeButtonId ) {
        return 3;
    }
    // 标签实际绘制在 Dock 宿主窗口，而不是内容窗口的 DrawList。
    const ImDrawList* hostDrawList =
        window->DockNode->TabBar->Window
            ? window->DockNode->TabBar->Window->DrawList
            : nullptr;
    // 标签悬浮背景至少应形成一个四色顶点矩形。
    if ( countVerticesWithColor(hostDrawList, expectedTabHoverColor) < 4 ) {
        return 4;
    }
    // 关闭按钮危险高亮同样至少需要四个目标颜色顶点。
    if ( countVerticesWithColor(hostDrawList, EXPECTED_CLOSE_HOVER_COLOR) <
         4 ) {
        return 5;
    }
    return 0;
}
}  // namespace

/// @brief 运行 Dock 标签关闭按钮悬浮视觉回归测试。
/// @return 测试通过时返回 0。
/// @note 返回码 1 至 5 对应场景内部阶段，6 表示字体图集准备失败。
int main()
{
    // 测试访问 imgui_internal 数据，先确保编译与运行版本匹配。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // 固定无后端显示参数并禁用 ini 文件，隔离用户停靠布局。
    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0F, 360.0F);
    io.DeltaTime   = 1.0F / 60.0F;
    io.IniFilename = nullptr;
    // Docking 标志必须在创建 DockSpace 前启用。
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // 无渲染后端仍需构建字体图集，才能合法调用 NewFrame。
    unsigned char* fontPixels = nullptr;
    int            fontWidth  = 0;
    int            fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    // 字体指针和尺寸共同确认图集数据可供当前上下文使用。
    const bool fontReady = fontPixels && fontWidth > 0 && fontHeight > 0;

    // 禁用声音和动画反馈，避免测试关注点超出关闭按钮视觉状态。
    MMM::UI::SetInteractionFeedbackEnabled(false);
    const int result =
        fontReady ? testCloseHoverRendersTabAndDangerHighlights() : 6;
    // 所有内部对象读取完毕后统一销毁 ImGui 上下文。
    ImGui::DestroyContext();
    return result;
}
