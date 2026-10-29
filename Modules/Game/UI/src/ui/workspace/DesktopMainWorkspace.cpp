#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/SkinConfig.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/ToolbarDockLayout.h"
#include <algorithm>
#include <cmath>
#include <imgui_internal.h>

namespace MMM::UI
{
/// @brief 桌面工作区原有外壳，移动端使用独立策略而不执行本路径。
/// @param sourceManager 当前工作区宿主，原生窗口仍由管理器持有。
/// @details 本次迁移只改变布局实现的归属，不重排原有窗口提交顺序。
/// 菜单和状态栏共享高度计算，固定工具栏复用原有四边布局算法。
/// 外壳不拥有项目会话，关闭或切换布局不会改变谱面生命周期。
/// 桌面工厂是本函数唯一的平台入口，移动目标不链接此实现。
/// @warning 每帧只进行原有桌面布局、事件处理和固定数量绘制。
void MainDockSpaceUI::renderDesktopWorkspace(UIManager* sourceManager)
{
    // 本帧复用皮肤、视口和 DPI 引用，避免各布局块采样到不同配置。
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    ImGuiViewport*       viewport = ImGui::GetMainViewport();
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    if ( auto* nativeWindow =
             sourceManager ? sourceManager->getNativeWindow() : nullptr ) {
        // 每帧从平台窗口校正状态，覆盖可能延迟或丢失的事件通知。
        m_isMaximized = nativeWindow->isWindowMaximized();
    }

    // 配置只在本帧借用，不引入新的配置拷贝或持久化入口。
    // 实际窗口缩放仍由统一的平台 DPI 来源决定。

    const auto& editorSettings =
        Config::AppConfig::instance().getEditorConfig().settings;
    const auto& aesthetics = editorSettings.aesthetics;

    float windowPaddingVal = std::floor(aesthetics.windowPadding * dpiScale);
    // 先取整再参与边缘计算，与原有桌面像素对齐方式一致。
    // 不在此处叠加移动触摸尺寸，避免桌面按钮无意变宽。

    // 独立侧栏宽度包含两侧窗口内边距，固定工具栏另按四边几何预留。
    float sidebarWidth =
        SideBarUI::GetSidebarWidth(dpiScale) + 2.0f * windowPaddingVal;
    // 四边工具栏在停靠宿主内统一扣除，菜单不再只为右侧栏单独扣宽度。
    // 固定几何与宿主共享计算入口，不能让两处布局产生重叠或重复预留。
    const float toolbarLayoutWidth = 0.0f;

    float       extraPaddingY = std::floor(4.0f * dpiScale);
    ImGuiStyle& style         = ImGui::GetStyle();
    // 每帧覆盖配置控制的样式，皮肤切换后无需重建工作区。
    // 工具栏和侧栏继续从这一份样式获得相同的外边距。

    // 把皮肤审美参数转换为整数像素并同步到本帧全局 ImGui 样式。
    float windowRound     = std::floor(aesthetics.windowRounding * dpiScale);
    float frameRound      = std::floor(aesthetics.frameRounding * dpiScale);
    style.WindowRounding  = windowRound;
    style.ChildRounding   = windowRound;
    style.FrameRounding   = frameRound;
    style.PopupRounding   = frameRound;
    style.TabRounding     = frameRound;
    style.ItemSpacing     = { std::floor(aesthetics.itemSpacing * dpiScale),
                              std::floor(aesthetics.itemSpacing * dpiScale) };
    style.WindowPadding   = { windowPaddingVal, windowPaddingVal };
    style.FrameBorderSize = 0.0f;

    // 菜单栏与状态栏等高，形成对称的顶部和底部固定区域。
    float menuBarHeight =
        ImGui::GetFontSize() + (style.FramePadding.y + extraPaddingY) * 2.0f;
    float statusBarHeight = menuBarHeight;
    // 顶底固定带只在桌面扣除，移动版可把完整可用区交给中心宿主。
    // NativeWindow 的边框交互只能出现在桌面路径。
    // 移动外壳不创建这些拖拽区域，也不模拟桌面标题栏。

    // 在提交窗口控件前设置边缘鼠标形状，避免被中央内容覆盖。
    handleNativeWindowFrameInteraction(sourceManager, dpiScale);

    // 顶部菜单栏占据主视口第一条固定带。
    renderMenuBar(sourceManager,
                  menuBarHeight,
                  sidebarWidth,
                  toolbarLayoutWidth,
                  dpiScale);

    // 中央 DockSpace 使用扣除四周固定区域后的剩余矩形。
    renderDockingSpace(sourceManager,
                       menuBarHeight,
                       statusBarHeight,
                       sidebarWidth,
                       toolbarLayoutWidth);

    // 状态栏在 DockSpace 之后绘制，以覆盖底部边界接缝。
    renderStatusBar(sourceManager, statusBarHeight, dpiScale);

    // 固定工具栏直接占据宿主留出的边缘矩形，不使用任何 Dock 节点。
    // renderDockingSpace 已在移出窗口前记录实际停靠边缘，不能用旧缓存定位。
    if ( editorSettings.fixedToolWindow ) {
        // 非固定模式保留原有 ImGui 窗口位置，不提交固定尺寸约束。
        // 此分支只处理固定工具栏，不能改变用户保存的浮动布局。
        const float gap  = std::floor(aesthetics.windowGap * dpiScale);
        const auto  edge = toolbarDockDirection(editorSettings.toolbarDockEdge);
        const bool  horizontal = edge == ImGuiDir_Up || edge == ImGuiDir_Down;
        const float thickness =
            std::floor(
                (horizontal && editorSettings.showToolLabels ? 46.0f : 32.0f) *
                dpiScale) +
            2.0f * windowPaddingVal;
        // 左侧包含侧栏外边距和栏间距，右侧仅保留一份外边距。
        // 与宿主使用同一基础矩形，避免固定工具栏右侧重复留白。
        const auto geometry = calculateToolbarWorkspaceGeometry(
            ImVec2(viewport->WorkPos.x + sidebarWidth + 2.0f * gap,
                   viewport->WorkPos.y + menuBarHeight + gap),
            ImVec2(viewport->WorkSize.x - sidebarWidth - 3.0f * gap,
                   viewport->WorkSize.y - menuBarHeight - statusBarHeight -
                       2.0f * gap),
            true,
            edge,
            thickness,
            gap);
        // 始终相对当前视口设定，主窗口缩放和 DPI 改变后仍紧贴同一条边。
        // 必须在 ToolbarView::Begin 之前提交，停靠标签宿主不再参与固定几何。
        // 非固定阶段不提交这些约束，用户拖动位置和长轴尺寸由 ImGui 保存。
        ImGui::SetNextWindowPos(geometry.m_toolbarPos);
        ImGui::SetNextWindowSize(geometry.m_toolbarSize);
        // 管理器展开时左侧固定带位于它与画布之间，以本帧占位叶几何定位。
        // 占位只提供矩形，实际工具窗口仍强制移出 DockSpace 并保持独立固定。
        if ( edge == ImGuiDir_Left ) {
            const auto* slotWindow =
                ImGui::FindWindowByName(TOOLBAR_MANAGER_SLOT_NAME);
            const auto* slot = slotWindow ? slotWindow->DockNode : nullptr;
            if ( slot ) {
                ImGui::SetNextWindowPos(slot->Pos);
                ImGui::SetNextWindowSize(ImVec2(thickness, slot->Size.y));
            }
        }
        ImGui::SetNextWindowDockID(0);
    }
    ImGui::SetNextWindowViewport(viewport->ID);
    m_toolbarView.update(sourceManager);
    // 原工具栏仍由相同对象更新，保留按钮状态和命令路由。
    // 移动工具条有独立实现，因此这里无需查询任何触摸模式。

    // 前景边框在所有主工作区窗口后绘制，确保无边框轮廓不被遮盖。
    renderNativeWindowFrameOverlay(sourceManager, dpiScale);
}
}  // namespace MMM::UI
