#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "mmm/SafeParse.h"
#include "ui/UIManager.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/ToolbarDockLayout.h"
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
/// 默认布局只在首次进入且没有现有节点树时建立。项目工作区已经恢复时，
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
/// @param toolbarWidth 保留的历史参数；四边占位统一使用当前工具栏设置计算。
/// @warning UI 热路径：每帧创建宿主窗口；DockBuilder 重建及会话遍历只能发生在
/// 首次初始化的低频分支中；模式切换只调整工具栏。
void MainDockSpaceUI::renderDockingSpace(UIManager* sourceManager,
                                         float      menuBarHeight,
                                         float      statusBarHeight,
                                         float sidebarWidth, float toolbarWidth)
{
    // 布局坐标约定：
    // - WorkPos/WorkSize 已扣除平台视口保留区域；
    // - menuBarHeight 与 statusBarHeight 由同一帧的外层布局计算；
    // - sidebarWidth 表示独立侧栏占位；工具栏由四边矩形计算单独预留；
    // - floatGap 在各固定区域之间保留一致的视觉间隔。
    // 因此这里不能再次使用 DisplaySize，也不能叠加窗口装饰尺寸。
    // 所有几何量统一基于主视口和当前内容缩放，避免多显示器 DPI 下出现缝隙。
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    auto& toolbarSettings = Config::AppConfig::instance().getEditorSettings();
    // 拖动停靠和四边菜单请求共用入口，在计算宿主矩形前立即转成固定模式。
    // 只有偏好真正变化时保存，不能因为窗口尺寸逐帧变化而反复写配置。
    if ( updateToolbarFixedDockIntent(toolbarSettings.fixedToolWindow,
                                      toolbarSettings.toolbarHorizontal,
                                      toolbarSettings.toolbarDockEdge) )
        Config::AppConfig::instance().save();
    const bool fixedToolWindow = toolbarSettings.fixedToolWindow;
    float      floatGap        = std::floor(aesthetics.windowGap * dpiScale);
    // 固定启动或恢复项目时也移出旧工具节点，兼容历史 ini 的内部停靠带。
    if ( fixedToolWindow ) detachFixedToolbar();
    const auto fixedEdge =
        toolbarDockDirection(toolbarSettings.toolbarDockEdge);
    const bool horizontal =
        fixedEdge == ImGuiDir_Up || fixedEdge == ImGuiDir_Down;
    // 固定带的排布由保存边缘决定，不能沿用浮动阶段手动选择的另一方向。
    // 该赋值不触发配置 I/O；拖动停靠时的持久化仍由工具栏视图处理。
    if ( fixedToolWindow ) toolbarSettings.toolbarHorizontal = horizontal;
    // 无标题栏的厚度只包含一行/列图标和内边距，不保留标签栏高度。
    // 短标签仅增高横排按钮，竖排图标列宽仍保持 32 逻辑像素。
    const float toolbarThickness =
        std::floor(
            (horizontal && toolbarSettings.showToolLabels ? 46.0f : 32.0f) *
            dpiScale) +
        2.0f * std::floor(aesthetics.windowPadding * dpiScale);
    // 基础矩形只扣一次菜单和状态栏，左侧入口条也只在这里扣一次宽度。
    // 四边共用同一几何入口，顶部/左侧还会移动宿主原点。
    // 右侧/底部不改原点，工具栏从完整矩形末端定位。
    // 横向共扣三份间距：侧栏左侧、侧栏与宿主之间、工作区右侧。
    // 右侧外边距只能扣一份，工具栏和宿主不能各自再扣一次。
    auto* sideBarManager =
        sourceManager->getView<FloatingManagerUI>("SideBarManager");
    auto*         previousHost = ImGui::FindWindowByName("RightDockHost");
    const ImGuiID previousRoot =
        previousHost ? previousHost->GetID("MyMainDockSpace") : 0;
    // 左侧管理器展开时，固定带应从画布工作区扣除，而非放在管理器外侧。
    // 收起、浮动或移到其他边缘的管理器仍使用完整宿主外部固定带。
    const bool managerLeft = fixedToolWindow && fixedEdge == ImGuiDir_Left &&
                             sideBarManager && sideBarManager->isVisible() &&
                             toolbarManagerWorkNode(previousRoot);
    const auto geometry    = calculateToolbarWorkspaceGeometry(
        ImVec2(viewport->WorkPos.x + sidebarWidth + 2.0f * floatGap,
               viewport->WorkPos.y + menuBarHeight + floatGap),
        ImVec2(viewport->WorkSize.x - sidebarWidth - 3.0f * floatGap,
               viewport->WorkSize.y - menuBarHeight - statusBarHeight -
                   2.0f * floatGap),
        fixedToolWindow && !managerLeft,
        fixedEdge,
        toolbarThickness,
        floatGap);
    // 工具栏固定带直接从整个中心宿主扣除，顶部/底部扣高，左侧/右侧扣宽。
    // 宿主只改变矩形，已有设置页、双画布和其他分栏不重新构建。
    ImGui::SetNextWindowPos(geometry.m_dockPos);
    ImGui::SetNextWindowSize(geometry.m_dockSize);
    // 保留外层菜单调用签名，避免旧右侧宽度与四边占位发生二次扣除。
    (void)toolbarWidth;
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
    if ( sideBarManager ) {
        sideBarManager->applyDockResizeConstraintsBeforeDockSpace(
            sourceManager);
    }

    // 固定字符串保证 ImGui 配置能跨帧识别同一个根节点。
    ImGuiID dockspace_id = ImGui::GetID("MyMainDockSpace");
    // 静态状态仅描述此根 DockSpace 的上一帧模式，不持有任何窗口资源。
    static bool lastFixedToolWindow    = true;
    static bool hasLastFixedToolWindow = false;
    bool        toolbarModeChanged =
        !hasLastFixedToolWindow || lastFixedToolWindow != fixedToolWindow;
    bool projectLayoutLoaded =
        MainDockSpaceUI::consumeProjectWorkspaceLayoutLoaded();
    // 项目布局恢复具有更高优先级：恢复成功后绝不能紧接着重建默认节点树。
    if ( projectLayoutLoaded ) {
        lastFixedToolWindow    = fixedToolWindow;
        hasLastFixedToolWindow = true;
        toolbarModeChanged     = false;
    }

    // 没有节点树时才建立缺省布局；已有 ini 和项目布局都必须原样继承。
    if ( !ImGui::DockBuilderGetNode(dockspace_id) ) {
        // 默认节点树的不变量：
        // - 根节点始终对应 RightDockHost 内的 MyMainDockSpace；
        // - 侧栏只占据第一层拆分得到的边缘节点；
        // - 固定工具栏位于宿主外部，只有非固定工具栏可以拥有叶节点；
        // - 预览区位于剩余工作区右侧；
        // - 时间线位于画布区域右侧，中心节点留给所有主画布标签。
        // - 项目中的全部活动主画布共享中心节点，以标签页形式呈现；
        // - 浮动工具栏的节点 ID 为零，不能强制送回默认位置；
        // - 每次重建都从空节点树开始，不能继承上一模式的残余拆分；
        // - Finish 之前不允许其他路径观察并修改尚未完成的节点树。
        // 新窗口必须复用这些语义节点，不能依赖临时 DockNode 指针。
        lastFixedToolWindow    = fixedToolWindow;
        hasLastFixedToolWindow = true;

        // 欢迎页先解除旧节点依赖，避免重建期间保留悬空的停靠归属。
        // 此入口只属于缺失根节点的初始化，不会在工具栏模式切换时调用。
        // 设置页也因此不会重新收到中心节点的首次停靠请求。
        if ( auto* welcome = sourceManager->getView<WelcomeView>("Welcome") )
            welcome->prepareForDockLayoutChange();
        // DockBuilder 的重建顺序必须是移除、创建、定尺寸、拆分、停靠、完成。
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, geometry.m_dockSize);

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
        // 固定带位于宿主外部，非固定初次显示也保持浮动。
        // 默认创建停靠叶会被误判为用户主动固定，不能在此插入工具节点。
        MainDockSpaceUI::setToolDockId(0);
    } else if ( toolbarModeChanged ) {
        // 模式切换不能触碰设置页、并排画布或欢迎页的停靠归属。
        // 固定时只移出工具栏窗口，解除固定后保持浮动，其他窗口不重建。
        const float actualToolbarWidth =
            std::floor(32.0f * dpiScale) +
            2.0f * std::floor(aesthetics.windowPadding * dpiScale);
        MainDockSpaceUI::setToolDockId(updateToolbarDockLayout(
            dockspace_id,
            fixedToolWindow,
            actualToolbarWidth,
            ImVec2(viewport->WorkSize.x - sidebarWidth - toolbarWidth -
                       4.0f * floatGap,
                   viewport->WorkSize.y - menuBarHeight - statusBarHeight -
                       2.0f * floatGap)));
    } else if ( projectLayoutLoaded ) {
        // 项目加载后旧工具节点缓存可能属于被替换的树；只读取新窗口归属。
        // 不拆分保存的布局，让项目中的设置窗口和画布保持原位置。
        auto* toolbar = ImGui::FindWindowByName(" ###Toolbar");
        MainDockSpaceUI::setToolDockId(toolbar ? toolbar->DockId : 0);
    }
    lastFixedToolWindow    = fixedToolWindow;
    hasLastFixedToolWindow = true;

    // 用户的边缘请求已在宿主矩形计算前消费，不能在此再次创建工具叶。
    // 非固定阶段只剩浮动状态，历史节点约束入口仍用于旧布局过渡兼容。
    // 非固定停靠节点锁定厚度，内部长轴分隔条单独决定长度。
    // 固定工具栏不属于 DockSpace，因此不会提交工具节点或限制画布分隔条。
    // 旧 ini 的工具节点限制只在仍停靠的非固定状态清理，避免影响其他窗口。
    const float toolbarPadding =
        2.0f * std::floor(aesthetics.windowPadding * dpiScale);
    if ( !fixedToolWindow )
        applyToolbarDockConstraints(
            false,
            std::floor(32.0f * dpiScale) + toolbarPadding,
            std::floor((toolbarSettings.showToolLabels ? 46.0f : 32.0f) *
                       dpiScale) +
                toolbarPadding);

    // 工具栏长轴在非固定模式提供独立分隔条；厚度仍由外侧节点约束。
    // 固定工具栏已移出树，不再提交内部长度占位或停靠装饰。
    if ( !fixedToolWindow ) updateToolbarLengthDock(false);
    if ( const auto* toolbar = ImGui::FindWindowByName(" ###Toolbar") )
        MainDockSpaceUI::setToolDockId(toolbar->DockId);

    // 在管理器之后预留固定带；只增减专用占位，不改变已展开管理器的归属。
    // 隐藏管理器和解除固定时必须收回占位，让画布重新使用全部剩余空间。
    const ImGuiID managerSlot = updateToolbarManagerLeftSlot(
        dockspace_id, managerLeft, toolbarThickness);
    // 拖动固定条画布侧边界改变管理器宽度，厚度约束不应锁住工作区分栏。
    drawToolbarManagerResizeHandle(managerSlot);
    // 所有低频节点修改先完成，再提交本帧宿主和节点控制按钮。
    ImGui::DockSpace(
        dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    drawToolbarManagerLeftSlot(managerSlot);
    drawToolbarDockRemainder();
    FeedbackDockNodeControls(dockspace_id);
    if ( titleFont ) ImGui::PopFont();

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
