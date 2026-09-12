#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "mmm/SafeParse.h"
#include "ui/UIManager.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WelcomeView.h"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 本文件集中维护主工作区的 DockBuilder 默认布局。
///
/// 默认布局只在首次进入或固定工具栏模式切换时重建。项目工作区已经恢复时，
/// 必须保留项目记录的节点树，避免默认比例覆盖用户保存的停靠关系。
/// 布局中的窗口名称同时是 ImGui 持久化标识，修改时需要同步窗口创建方。

/// @brief 无异常解析停靠布局比例配置。
/// @param value 配置字符串。
/// @param fallback 解析失败时的默认值。
/// @return 解析成功的有限浮点数或默认值。
float parseDockLayoutFloat(std::string_view value, float fallback)
{
    // 空字符串代表皮肤未声明该项，直接采用调用方提供的稳定默认值。
    if ( value.empty() ) return fallback;

    // SafeParse 允许无异常地处理用户可编辑配置，避免渲染路径抛出异常。
    const auto  result = Internal::parseFloatingPrefix(value);
    const float parsed = static_cast<float>(result.value);
    // 除了解析状态，还需排除未消费字符和非有限值，防止污染节点尺寸。
    if ( result.error == std::errc{} && result.parsedLength != 0 &&
         std::isfinite(parsed) ) {
        return parsed;
    }
    // 非法皮肤配置不阻断界面创建，回退值保证主画布仍然可见。
    return fallback;
}

/// @brief 将当前活动的所有主画布窗口停靠到指定中心节点。
/// @param dockId 目标中心 Dock 节点 ID。
/// @warning 低频 UI 路径：仅在 DockBuilder
/// 重建默认布局时执行；遍历当前打开会话列表。
void dockActiveMainCanvasWindows(ImGuiID dockId)
{
    // 零 ID 表示中心节点尚未建立，此时 DockBuilder 调用没有有效目标。
    if ( dockId == 0 ) return;

    // 会话快照在低频布局重建期获取，不把会话容器访问带入逐帧绘制分支。
    auto entries = Logic::EditorEngine::instance().getSessionEntries();
    for ( const auto& entry : entries ) {
        // cameraId 同时充当画布窗口的稳定 ImGui ID；空值无法用于定位窗口。
        if ( entry.cameraId.empty() ) continue;
        ImGui::DockBuilderDockWindow(entry.cameraId.c_str(), dockId);
    }
}
}  // namespace

/// @brief 创建主视口中央的停靠宿主，并按需恢复默认节点布局。
/// @param sourceManager UI 管理器，用于协调浮动管理器和欢迎页状态。
/// @param menuBarHeight 顶部菜单栏占用高度。
/// @param statusBarHeight 底部状态栏占用高度。
/// @param sidebarWidth 固定侧栏占用宽度。
/// @param toolbarWidth 固定工具栏占用宽度。
/// @warning UI 热路径：每帧创建宿主窗口；DockBuilder 重建及会话遍历只能发生在
/// 首帧或布局模式发生变化的低频分支中。
void MainDockSpaceUI::renderDockingSpace(UIManager* sourceManager,
                                         float      menuBarHeight,
                                         float      statusBarHeight,
                                         float sidebarWidth, float toolbarWidth)
{
    // 布局坐标约定：
    // - WorkPos/WorkSize 已扣除平台视口保留区域；
    // - menuBarHeight 与 statusBarHeight 由同一帧的外层布局计算；
    // - sidebarWidth 与 toolbarWidth 仅表示固定模式下的外部占位；
    // - floatGap 在各固定区域之间保留一致的视觉间隔。
    // 因此这里不能再次使用 DisplaySize，也不能叠加窗口装饰尺寸。
    // 所有几何量统一基于主视口和当前内容缩放，避免多显示器 DPI 下出现缝隙。
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    const bool fixedToolWindow =
        Config::AppConfig::instance().getEditorSettings().fixedToolWindow;
    float floatGap = std::floor(aesthetics.windowGap * dpiScale);

    // 宿主窗口位于四条固定 UI 带之间，间距分别留给皮肤定义的窗口空隙。
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + sidebarWidth + 2.0f * floatGap,
               viewport->WorkPos.y + menuBarHeight + floatGap));
    ImGui::SetNextWindowSize(ImVec2(
        viewport->WorkSize.x - sidebarWidth - toolbarWidth - 4.0f * floatGap,
        viewport->WorkSize.y - menuBarHeight - statusBarHeight -
            2.0f * floatGap));
    ImGui::SetNextWindowViewport(viewport->ID);

    // 宿主仅承载 DockSpace，本身不能响应移动、缩放或再次被停靠。
    ImGuiWindowFlags dock_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking;

    float windowRound = std::floor(aesthetics.windowRounding * dpiScale);
    float frameRound  = std::floor(aesthetics.frameRounding * dpiScale);

    // 圆角由宿主向停靠窗口统一提供，确保固定区与浮动区观感一致。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    float separatorSize =
        std::min(8.0f * dpiScale, std::max(3.0f * dpiScale, floatGap));
    // 分隔条宽度受上下限约束，兼顾高 DPI 可操作性和紧凑皮肤布局。
    ImGui::PushStyleVar(ImGuiStyleVar_DockingSeparatorSize, separatorSize);

    // --- 宿主窗口使用 0 内边距以撑满容器 ---
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

    ImFont* titleFont = skinCfg.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

    ImGui::Begin("RightDockHost", nullptr, dock_flags);
    // 立即弹出 WindowPadding，防止该临时宿主样式泄漏到停靠子窗口。
    ImGui::PopStyleVar(1);

    // 调整约束必须先于 DockSpace 提交，否则本帧拖动会使用过期节点尺寸。
    auto* sideBarManager =
        sourceManager->getView<FloatingManagerUI>("SideBarManager");
    if ( sideBarManager ) {
        sideBarManager->applyDockResizeConstraintsBeforeDockSpace(
            sourceManager);
    }

    // 固定字符串保证 ImGui 配置能跨帧识别同一个根节点。
    ImGuiID dockspace_id = ImGui::GetID("MyMainDockSpace");
    s_mainDockId         = dockspace_id;
    ImGui::DockSpace(
        dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    FeedbackDockNodeControls(dockspace_id);

    if ( titleFont ) ImGui::PopFont();

    // 静态状态仅描述此根 DockSpace 的上一帧模式，不持有任何窗口资源。
    static bool lastFixedToolWindow    = true;
    static bool hasLastFixedToolWindow = false;
    static bool is_first_time          = true;
    bool        shouldResetLayout =
        !hasLastFixedToolWindow || lastFixedToolWindow != fixedToolWindow;
    bool projectLayoutLoaded =
        MainDockSpaceUI::consumeProjectWorkspaceLayoutLoaded();
    // 项目布局恢复具有更高优先级：恢复成功后绝不能紧接着重建默认节点树。
    if ( projectLayoutLoaded ) {
        is_first_time          = false;
        lastFixedToolWindow    = fixedToolWindow;
        hasLastFixedToolWindow = true;
        shouldResetLayout      = false;
    }

    // 首帧负责建立缺省树；之后只有固定工具栏模式切换才允许重建。
    if ( is_first_time || shouldResetLayout ) {
        // 默认节点树的不变量：
        // - 根节点始终对应 RightDockHost 内的 MyMainDockSpace；
        // - 侧栏只占据第一层拆分得到的边缘节点；
        // - 非固定工具栏只占工作区最右侧的窄节点；
        // - 预览区位于剩余工作区右侧；
        // - 时间线位于画布区域右侧，中心节点留给所有主画布标签。
        // - 项目中的全部活动主画布共享中心节点，以标签页形式呈现；
        // - 不存在可停靠工具栏时，工具节点 ID 必须保持为零；
        // - 每次重建都从空节点树开始，不能继承上一模式的残余拆分；
        // - Finish 之前不允许其他路径观察并修改尚未完成的节点树。
        // 新窗口必须复用这些语义节点，不能依赖临时 DockNode 指针。
        is_first_time          = false;
        lastFixedToolWindow    = fixedToolWindow;
        hasLastFixedToolWindow = true;

        // 欢迎页先解除旧节点依赖，避免重建期间保留悬空的停靠归属。
        if ( auto* welcome = sourceManager->getView<WelcomeView>("Welcome") )
            welcome->prepareForDockLayoutChange();
        // DockBuilder 的重建顺序必须是移除、创建、定尺寸、拆分、停靠、完成。
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        const float dockspaceWidth = viewport->WorkSize.x - sidebarWidth -
                                     toolbarWidth - 4.0f * floatGap;
        ImGui::DockBuilderSetNodeSize(
            dockspace_id,
            ImVec2(dockspaceWidth,
                   viewport->WorkSize.y - menuBarHeight - statusBarHeight -
                       2.0f * floatGap));

        ImGuiID dock_id_left;
        ImGuiID dock_id_right;
        // 皮肤侧栏比例属于外部输入，限制范围防止任一工作区被完全挤出。
        float sidebarRatio = parseDockLayoutFloat(
            skinCfg.getLayoutConfig("floating_windows.window1.initial_ratio"),
            0.22f);
        sidebarRatio = std::clamp(sidebarRatio, 0.05f, 0.95f);
        auto dir =
            skinCfg.getLayoutConfig("floating_windows.window1.initial_side");
        // 未识别的方向按左侧处理，保持旧皮肤的默认布局兼容性。
        ImGuiDir sidebarDir = (dir == "right") ? ImGuiDir_Right : ImGuiDir_Left;

        // 第一次拆分保留另一侧作为后续画布、预览和时间线的工作区域。
        dock_id_left = ImGui::DockBuilderSplitNode(
            dockspace_id, sidebarDir, sidebarRatio, nullptr, &dock_id_right);

        ImGuiID dock_id_work = dock_id_right;
        if ( !fixedToolWindow ) {
            // 可停靠工具栏的比例由实际像素宽度换算，并限制到合理窄栏范围。
            const float toolNodeRatio = std::clamp(
                (std::floor(32.0f * dpiScale) +
                 2.0f * std::floor(aesthetics.windowPadding * dpiScale)) /
                    std::max(dockspaceWidth, 1.0f),
                0.035f,
                0.12f);
            ImGuiID dock_id_tool = 0;
            dock_id_tool         = ImGui::DockBuilderSplitNode(dock_id_work,
                                                               ImGuiDir_Right,
                                                               toolNodeRatio,
                                                               nullptr,
                                                               &dock_id_work);
            ImGui::DockBuilderDockWindow("Toolbar", dock_id_tool);
            // 缓存节点 ID 供工具栏约束逻辑使用，不缓存节点指针。
            MainDockSpaceUI::setToolDockId(dock_id_tool);
        } else {
            // 固定工具栏位于 DockSpace 外部，因此清除历史工具节点标识。
            MainDockSpaceUI::setToolDockId(0);
        }

        // 工作区右侧先切出预览，再从剩余区域切出时间线与中心画布。
        ImGuiID dock_id_center_canvas;
        ImGuiID dock_id_preview;
        dock_id_preview = ImGui::DockBuilderSplitNode(dock_id_work,
                                                      ImGuiDir_Right,
                                                      0.20f,
                                                      nullptr,
                                                      &dock_id_center_canvas);

        ImGuiID dock_id_center;
        ImGuiID dock_id_timeline;
        // 此处的比例相对于上一步剩余节点，而不是根窗口的绝对宽度。
        // 保持拆分顺序即可让 ImGui 在窗口缩放时按层级重新分配空间。
        dock_id_timeline = ImGui::DockBuilderSplitNode(dock_id_center_canvas,
                                                       ImGuiDir_Right,
                                                       0.28f,
                                                       nullptr,
                                                       &dock_id_center);

        // 固定窗口名是各视图注册时使用的 ID，必须与创建方保持一致。
        ImGui::DockBuilderDockWindow("SideBarManager", dock_id_left);
        ImGui::DockBuilderDockWindow("Timeline", dock_id_timeline);
        ImGui::DockBuilderDockWindow("Basic2DCanvas", dock_id_center);
        dockActiveMainCanvasWindows(dock_id_center);
        ImGui::DockBuilderDockWindow("PreviewWindow", dock_id_preview);

        // 中心 ID 供新建画布标签定位；在 Finish 前写入也只保存数值 ID。
        MainDockSpaceUI::setCenterDockId(dock_id_center);

        // 所有拆分和窗口归属一次性提交，避免暴露半构建节点树。
        ImGui::DockBuilderFinish(dockspace_id);
    }

    // 用户拖动节点后中心节点可能变化，每帧以 ImGui 当前节点树校正缓存。
    if ( ImGuiDockNode* centerNode =
             ImGui::DockBuilderGetCentralNode(dockspace_id) ) {
        // CentralNode 是用户调整布局后的权威结果，可能不同于初建时的局部变量。
        MainDockSpaceUI::setCenterDockId(centerNode->ID);
    } else {
        // 根节点暂不可用时清零，调用方会等待下一帧而非使用旧 ID。
        MainDockSpaceUI::setCenterDockId(0);
    }

    // 样式栈平衡关系：
    // - WindowPadding 已在 Begin 后提前弹出；
    // - 此处恢复透明背景颜色；
    // - 剩余六项依次对应圆角、边框和 DockingSeparatorSize。
    // 样式栈按 Push 的逆序完整恢复，避免影响同帧后续独立窗口。
    ImGui::End();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(6);
}

}  // namespace MMM::UI
