#include "ui/imgui/WorkspaceDockRestore.h"

#include "config/AppConfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "mmm/project/ProjectSettings.h"
#include "ui/imgui/CanvasTabManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/ToolbarDockLayout.h"
#include "ui/imgui/ToolbarPopupLayout.h"
#include "ui/imgui/manager/ToolbarView.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <string>
#include <vector>

/// @file WorkspaceDockRestoreTest.cpp
/// @brief 验证运行中从默认工作区加载项目布局后，已有窗口恢复原停靠节点。
/// @details 测试包含两个相互独立的阶段：先在一份上下文中生成真实 ImGui ini，
/// 再在已有默认 Dock 树的另一份上下文中模拟项目重新打开。
/// 画布标题故意使用 `###`，与应用中翻译后的窗口标题保持同样的稳定 ID 规则。
/// 两张画布分别占据左右叶节点；只检查它们已停靠不足以发现并排布局丢失。
/// 所有 ImGui 上下文均关闭 ini 文件写入，测试不得碰用户个人配置目录。
/// 测试还覆盖旧工作区与本次实际恢复会话不一致时的捕获门闩初始化。
/// 工具栏模式切换使用独立根节点，检查三次往返后的画布叶节点身份。
/// 设置页分别测试停靠和浮动几何，避免仅检查 DockId 漏掉全屏尺寸错误。
/// 共享工具节点场景保证移出固定工具栏时仍保留其他标签的节点归属。
/// 实际 ToolbarView 的尺寸约束另行验证，防止替代窗口漏掉视图自身的限制。
/// 非固定窗口只允许长轴变化，旧节点缓存也不能强制其归位。

namespace
{
/// @brief 测试宿主与项目窗口使用的稳定名称。
constexpr const char* HOST_NAME         = "WorkspaceDockRestoreHost";
constexpr const char* PANEL_NAME        = "WorkspaceDockRestorePanel";
constexpr const char* SECOND_PANEL_NAME = "WorkspaceDockRestoreSecondPanel";
constexpr const char* PANEL_TITLE       = "First###WorkspaceDockRestorePanel";
constexpr const char* SECOND_PANEL_TITLE =
    "Second###WorkspaceDockRestoreSecondPanel";

/// @brief 建立禁用用户配置文件的独立 ImGui 上下文。
/// @details 这里不创建 GLFW/Vulkan 后端，只检查 ImGui 自身 Dock 状态机。
/// 固定显示尺寸和帧时间让两份上下文的节点尺寸可比。
/// 字体图集需在 NewFrame 之前准备，否则无渲染后端的测试会触发断言。
void createContext()
{
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize        = ImVec2(900.0F, 600.0F);
    io.DeltaTime          = 1.0F / 60.0F;
    unsigned char* pixels = nullptr;
    int            width  = 0;
    int            height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
}

/// @brief 提交宿主和测试窗口的一帧，模拟应用启动时已经创建的默认工作区。
/// @param defaultDock 首次创建窗口时是否要求默认停靠。
/// @param secondPanel 是否提交项目恢复后才注册的第二张画布。
/// @param forceSecondFloating 是否模拟恢复时第二张画布错误地变成浮动窗口。
/// @return 本帧宿主 DockSpace 的稳定 ID。
/// @details 首帧可创建默认树，后续帧只提交同一 DockSpace；如果每帧重建，
/// 就无法检验项目 ini 对现有树的恢复能力。第二张画布可推迟到下一帧 Begin，
/// 对应实际 CanvasTabManager 注册新视图后 UIManager 隔帧绘制的流程。
/// @warning 测试 UI 热路径：每帧只提交两个窗口，不访问文件系统。
ImGuiID drawFrame(bool defaultDock, bool secondPanel = false,
                  bool forceSecondFloating = false)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(900.0F, 600.0F), ImGuiCond_Always);
    ImGui::Begin(HOST_NAME);
    const ImGuiID dockId = ImGui::GetID("WorkspaceDockRestoreRoot");
    // 宿主身份在两份上下文中相同，ini 的 DockSpace 根节点才能正确匹配。
    // 根 ID 由宿主窗口种子与固定字符串生成，不从保存的数据硬编码取得。
    ImGui::DockSpace(dockId);
    if ( defaultDock ) {
        // 启动时默认工作区已经有一棵活跃的预览/画布分栏树。
        // 默认树只包含第一张画布，制造与项目保存布局不同的叶节点结构。
        // 项目加载必须替换这棵树，而不能把旧叶节点误认为新布局的中心。
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockId, ImVec2(900.0F, 600.0F));
        ImGuiID canvasId = 0;
        ImGui::DockBuilderSplitNode(
            dockId, ImGuiDir_Right, 0.25F, nullptr, &canvasId);
        ImGui::DockBuilderDockWindow(PANEL_NAME, canvasId);
        ImGui::DockBuilderFinish(dockId);
    }
    ImGui::End();
    ImGui::Begin(PANEL_TITLE);
    ImGui::End();
    if ( secondPanel ) {
        // 仅在明确要求时破坏第二画布的 Dock 归属，用来验证生产补停靠入口。
        if ( forceSecondFloating )
            ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::Begin(SECOND_PANEL_TITLE);
        ImGui::End();
    }
    ImGui::Render();
    return dockId;
}

/// @brief 保存两个画布在主编辑区并排停靠的项目布局。
/// @param firstDockId 接收第一个画布的节点 ID。
/// @param secondDockId 接收第二个画布的节点 ID。
/// @return 完整 ImGui 工作区 ini 快照。
/// @details 在同一个中心画布区域再次左右拆分，得到两个不同叶节点。
/// 保存布局前推进额外两帧，确保窗口已真正接入节点，而非仅留下
/// DockBuilder 下一帧执行的挂起请求。保存的数据直接来自 ImGui，
/// 不手工构造 Docking 节，以覆盖真实格式及节点序列化行为。
std::string makeSavedLayout(ImGuiID& firstDockId, ImGuiID& secondDockId)
{
    createContext();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(900.0F, 600.0F), ImGuiCond_Always);
    ImGui::Begin(HOST_NAME);
    const ImGuiID rootId = ImGui::GetID("WorkspaceDockRestoreRoot");
    ImGui::DockBuilderRemoveNode(rootId);
    ImGui::DockBuilderAddNode(rootId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(rootId, ImVec2(900.0F, 600.0F));
    ImGuiID canvasRegionId = 0;
    // 第一层留给默认布局可类比的非画布区域；仅在画布区域形成并排布局。
    // 若仅拆分根节点，测试容易把画布分栏和工具窗口分栏混淆。
    ImGui::DockBuilderSplitNode(
        rootId, ImGuiDir_Right, 0.25F, nullptr, &canvasRegionId);
    // 保存两个叶节点的精确 ID，最终不能以同一中心节点冒充恢复成功。
    // 两个 ID 随 DockBuilder 生成，测试不假定其具体十六进制数值。
    secondDockId = ImGui::DockBuilderSplitNode(
        canvasRegionId, ImGuiDir_Right, 0.45F, nullptr, &firstDockId);
    ImGui::DockBuilderDockWindow(PANEL_NAME, firstDockId);
    ImGui::DockBuilderDockWindow(SECOND_PANEL_NAME, secondDockId);
    ImGui::DockBuilderFinish(rootId);
    // 提交宿主后才提交子窗口，让 ImGui 确认新旧节点的实际几何。
    // 只保存 DockId 而不 Begin 无法验证设置页是否突然填满整个工作区。
    ImGui::DockSpace(rootId);
    ImGui::End();
    ImGui::Begin(PANEL_TITLE);
    ImGui::End();
    ImGui::Begin(SECOND_PANEL_TITLE);
    ImGui::End();
    ImGui::Render();
    // 让 DockSpace 和两个窗口在已完成的树中稳定下来。
    drawFrame(false, true);
    drawFrame(false, true);
    std::string saved = ImGui::SaveIniSettingsToMemory();
    ImGui::DestroyContext();
    return saved;
}

/// @brief 验证捕获门闩只等待本次真正恢复的画布，不等待旧项目列表。
/// @param savedIni 用于生成两张画布的原节点请求。
/// @return 普通恢复会等待首帧，显式单谱面与缺失谱面不会误等待。
/// @details 同一份旧 ini 在显式打开谱面时仍可存在于项目设置里，
/// `restoreDockFromWorkspace` 是本次实际会话是否继承它的依据。
/// 该断言防止把旧工作区的画布计入等待集合，造成捕获一直冻结。
bool checkRestorePlan(const std::string& savedIni)
{
    createContext();
    MMM::ProjectWorkspaceState workspace;
    workspace.m_imguiIniData = savedIni;
    // 测试只使用低频准备入口，不需要启动逻辑引擎或项目 I/O。
    // 这样可以单独验证“本次打开哪些画布”这一恢复计划的判断。
    MMM::UI::CanvasTabManager tabs;

    // 显式打开谱面会跳过项目旧标签列表；它不应冻结工作区布局捕获。
    // 即使它的稳定窗口名恰好与旧项目记录相同，也不属于旧布局恢复任务。
    std::vector<MMM::UI::CanvasWorkspaceEntry> explicitEntries{
        { PANEL_NAME, false, false }
    };
    tabs.prepareProjectWorkspaceDockRestore(workspace, explicitEntries);
    const bool explicitCanCapture = !tabs.projectWorkspaceDockRestorePending();

    // 丢失谱面不会出现在逻辑层已发布的实际条目里；只等待成功创建的画布。
    // 两张实际恢复的画布则必须完成第一次 Begin 和原节点核对。
    // 不把工作区中的每个历史路径都加入 entries，模拟磁盘文件已被删除。
    std::vector<MMM::UI::CanvasWorkspaceEntry> restoredEntries{
        { PANEL_NAME, false, true }, { SECOND_PANEL_NAME, false, true }
    };
    tabs.prepareProjectWorkspaceDockRestore(workspace, restoredEntries);
    const bool restoredMustWait = tabs.projectWorkspaceDockRestorePending();
    ImGui::DestroyContext();
    return explicitCanCapture && restoredMustWait;
}

/// @brief 在已有默认工作区的上下文里加载项目布局并检查停靠节点。
/// @param savedIni 项目保存的 ini 数据。
/// @param firstDockId 第一个画布保存的节点。
/// @param secondDockId 并排画布保存的节点。
/// @return 窗口最终恢复到目标节点时返回 true。
/// @details 先让默认布局创建并绘制，再在一个 UI 帧中加载项目 ini。
/// 新画布隔帧创建后模拟浮动异常，生产补停靠函数应把它送回原叶节点。
/// 同进程再次加载相同项目布局，检查第二次打开也维持分栏结构。
/// 测试比较两个准确的 DockId；只有“都在主窗口”并不足以满足要求。
bool checkRestore(const std::string& savedIni, ImGuiID firstDockId,
                  ImGuiID secondDockId)
{
    createContext();
    // 默认树必须实际经历一帧，才能模拟应用已打开过无项目工作区。
    // 否则新上下文没有节点，项目布局恢复会退化为更容易的启动加载情形。
    drawFrame(true);
    drawFrame(false);
    auto* window = ImGui::FindWindowByName(PANEL_NAME);
    if ( !window || window->DockId == 0 ) {
        ImGui::DestroyContext();
        return false;
    }

    // 真实项目在 UI 帧开始后加载工作区；随后才提交 DockSpace 和各窗口。
    // LoadIniSettingsFromMemory 与默认 DockBuilder 使用同一稳定根 ID。
    // 加载时第一个窗口已经存在，第二个窗口尚未由管理器注册。
    ImGui::NewFrame();
    ImGui::LoadIniSettingsFromMemory(savedIni.data(), savedIni.size());
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(900.0F, 600.0F), ImGuiCond_Always);
    ImGui::Begin(HOST_NAME);
    ImGui::DockSpace(ImGui::GetID("WorkspaceDockRestoreRoot"));
    ImGui::End();
    // CanvasTabManager 在恢复帧注册第二张画布，实际 Begin 延后到下一帧。
    // 先绘制原有第一画布，故意跳过第二画布测试未完成的恢复阶段。
    // 这与 UIManager 在遍历开始时固定视图数量的行为相同。
    ImGui::Begin(PANEL_TITLE);
    ImGui::End();
    ImGui::Render();
    // ImGui 的早期快照仍应保留尚未 Begin 的第二张画布配置记录。
    // 实际应用另外使用捕获门闩，避免更复杂的异步载图覆盖该布局。
    const std::string earlyCapture = ImGui::SaveIniSettingsToMemory();
    const auto        earlySecondDockId =
        MMM::UI::savedWorkspaceWindowDockId(earlyCapture, SECOND_PANEL_NAME);
    if ( !earlySecondDockId || *earlySecondDockId != secondDockId ) {
        // 如果第二画布记录已经消失，后续补停靠就失去了准确目标。
        XERROR("Early workspace capture lost the second canvas dock");
        ImGui::DestroyContext();
        return false;
    }
    drawFrame(false, true, true);
    // 故意制造原问题的浮动画布表现；这一步不能由 ImGui 自动修复。
    // 如果测试未确认它真正浮动，后续断言可能只是 ImGui 自发恢复通过。
    auto* secondWindow = ImGui::FindWindowByName(SECOND_PANEL_NAME);
    if ( !secondWindow || secondWindow->DockId != 0 ) {
        ImGui::DestroyContext();
        return false;
    }
    const auto savedSecondDockId =
        MMM::UI::savedWorkspaceWindowDockId(savedIni, SECOND_PANEL_NAME);
    // 生产解析器必须从原 ini 读到右侧叶节点，而非返回默认中心节点。
    // 解析错误会让补停靠逻辑把左右画布合并成同一个标签组。
    if ( !savedSecondDockId || *savedSecondDockId != secondDockId ) {
        ImGui::DestroyContext();
        return false;
    }

    // 在 DockSpace 已提交、窗口 Begin 之前执行生产代码的补停靠操作。
    // 与 CanvasTabManager 在主 DockSpace 之后更新的顺序保持一致。
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(900.0F, 600.0F), ImGuiCond_Always);
    ImGui::Begin(HOST_NAME);
    ImGui::DockSpace(ImGui::GetID("WorkspaceDockRestoreRoot"));
    ImGui::End();
    (void)MMM::UI::reconcileSavedWorkspaceWindowDock(
        SECOND_PANEL_NAME, *savedSecondDockId, firstDockId);
    // DockBuilder 发出的请求要经过窗口 Begin 才能由 ImGui 状态机确认。
    // 额外再推进一帧，避免测试观察到的只是暂存 DockId。
    ImGui::Begin(PANEL_TITLE);
    ImGui::End();
    ImGui::Begin(SECOND_PANEL_TITLE);
    ImGui::End();
    ImGui::Render();
    drawFrame(false, true);
    // 同进程关闭并重新打开项目会再次加载布局，节点树仍必须保持并排。
    // 这同时覆盖“只在首次打开正确，下一次打开合并成一个标签组”的回归。
    ImGui::NewFrame();
    ImGui::LoadIniSettingsFromMemory(savedIni.data(), savedIni.size());
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(900.0F, 600.0F), ImGuiCond_Always);
    ImGui::Begin(HOST_NAME);
    ImGui::DockSpace(ImGui::GetID("WorkspaceDockRestoreRoot"));
    ImGui::End();
    ImGui::Begin(PANEL_TITLE);
    ImGui::End();
    ImGui::Begin(SECOND_PANEL_TITLE);
    ImGui::End();
    ImGui::Render();
    drawFrame(false, true);
    secondWindow = ImGui::FindWindowByName(SECOND_PANEL_NAME);
    // 两个准确叶节点都存在，才能证明并排布局和画布归属一起恢复。
    // 特别检查首张画布，防止修复第二张时又把第一张移出原位置。
    const bool restored = window->DockId == firstDockId && secondWindow &&
                          secondWindow->DockId == secondDockId;
    if ( !restored ) {
        XERROR(
            "Workspace dock restore failed: first={}, second={}, "
            "expected={}/{}",
            window->DockId,
            secondWindow ? secondWindow->DockId : 0,
            firstDockId,
            secondDockId);
    }
    ImGui::DestroyContext();
    return restored;
}
/// @brief 提交工具栏切换回归的一帧，不重建原画布与设置页布局。
/// @param fixed 工具栏是否在宿主外固定。
/// @param transition 是否调用生产模式切换入口。
/// @param initialize 是否创建三栏默认树。
/// @param floatingSettings 设置页是否作为独立浮动窗口测试。
/// @return 当前根节点 ID。
/// @warning 测试 UI 热路径：只在显式初始化/切换帧修改节点树。
ImGuiID drawToolbarFrame(bool fixed, bool transition, bool initialize = false,
                         bool floatingSettings = false)
{
    ImGui::NewFrame();
    // 固定模式使用真实外部占位，宿主缩小但不重建设置页和并排画布。
    // 与生产流程相同，先移出工具栏，再提交缩小后的宿主矩形。
    if ( fixed ) MMM::UI::detachFixedToolbar();
    const auto geometry = MMM::UI::calculateToolbarWorkspaceGeometry(
        ImVec2(0, 0), ImVec2(900, 600), fixed, ImGuiDir_Right, 50, 0);
    ImGui::SetNextWindowPos(geometry.m_dockPos);
    ImGui::SetNextWindowSize(geometry.m_dockSize);
    // 零内边距对应生产宿主，防止测试把宿主 padding 当作工具栏宽度变化。
    // 样式必须在 Begin 后弹出，否则设置页内容区会受到无关的布局影响。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(HOST_NAME, nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar();
    // 测试根与项目恢复测试根分开，避免一份上下文中的历史节点相互遮蔽。
    // 初始化和切换都先于 DockSpace，与生产路径采用同样的构建顺序。
    const ImGuiID rootId = ImGui::GetID("ToolbarTransitionRoot");
    if ( initialize ) {
        // 设置页和两张画布各占一个叶节点，重建中心布局会破坏这个关系。
        ImGui::DockBuilderAddNode(rootId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(rootId, ImVec2(900, 600));
        ImGuiID       canvasRegionId = 0;
        const ImGuiID settingsId     = ImGui::DockBuilderSplitNode(
            rootId, ImGuiDir_Right, 0.25f, nullptr, &canvasRegionId);
        ImGuiID       firstId  = 0;
        const ImGuiID secondId = ImGui::DockBuilderSplitNode(
            canvasRegionId, ImGuiDir_Right, 0.5f, nullptr, &firstId);
        ImGui::DockBuilderDockWindow(PANEL_NAME, firstId);
        ImGui::DockBuilderDockWindow(SECOND_PANEL_NAME, secondId);
        if ( !floatingSettings )
            ImGui::DockBuilderDockWindow("###SettingsWindow", settingsId);
        ImGui::DockBuilderFinish(rootId);
        // 固定时没有工具叶节点；非固定初始化用于共享标签节点边界测试。
        if ( !fixed )
            (void)MMM::UI::dockToolbarAtCanvasEdge(
                rootId, firstId, ImGuiDir_Right, 50);
    }
    if ( transition ) {
        // 测试使用实际生产 helper，覆盖节点拆分和工具栏移出时的合并行为。
        (void)MMM::UI::updateToolbarDockLayout(
            rootId, fixed, 50, ImVec2(900, 600));
    }
    // 提交宿主后才提交子窗口，让 ImGui 确认新旧节点的实际几何。
    // 只保存 DockId 而不 Begin 无法验证设置页是否突然填满整个工作区。
    ImGui::DockSpace(rootId);
    ImGui::End();
    ImGui::Begin(PANEL_TITLE);
    ImGui::End();
    ImGui::Begin(SECOND_PANEL_TITLE);
    ImGui::End();
    // 独立设置页只在初帧指定尺寸，后续切换不能再给它新的位置或大小。
    if ( initialize && floatingSettings ) {
        ImGui::SetNextWindowPos(ImVec2(100, 100));
        ImGui::SetNextWindowSize(ImVec2(300, 250));
    }
    // 使用与真实设置页相同的 ### 稳定后缀，标题文字不参与窗口身份。
    // 测试不能依靠重建一个新窗口绕过旧窗口 DockId 被清除的错误。
    ImGui::Begin("设置###SettingsWindow");
    ImGui::End();
    // 固定窗口使用预留矩形且禁止停靠，真实视图的装饰与约束另行验证。
    // 解除固定不强制重新停靠，避免把用户浮动位置送回旧的工具叶节点。
    if ( fixed ) {
        ImGui::SetNextWindowPos(geometry.m_toolbarPos);
        ImGui::SetNextWindowSize(geometry.m_toolbarSize);
    }
    ImGui::Begin(" ###Toolbar",
                 nullptr,
                 fixed ? ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoDocking
                       : ImGuiWindowFlags_None);
    ImGui::End();
    ImGui::Render();
    return rootId;
}

/// @brief 验证反复切换固定工具栏不会合并画布或改变设置页的窗口身份。
/// @param floatingSettings 是否同时检查浮动设置页的精确几何。
/// @return 两种模式往返后原叶节点和设置页尺寸仍保持时返回 true。
/// @details 断言发生在窗口 Begin 和 DockSpace 都推进后，不只检查挂起请求。
/// 停靠窗口允许一个分隔条造成的小幅变化；浮动窗口的位置和尺寸必须完全相同。
/// 上下文独立且关闭 ini 写入，测试不依赖或修改用户的个人工作区。
bool checkToolbarTransition(bool floatingSettings)
{
    createContext();
    // 初始化后再推进两帧，排除初次窗口自动适配内容导致的尺寸变化。
    // 几何基线从已经稳定的布局采集，不以 DockBuilder 的请求尺寸代替。
    drawToolbarFrame(true, false, true, floatingSettings);
    drawToolbarFrame(true, false);
    drawToolbarFrame(true, false);
    // 窗口对象在同一上下文内保持稳定；不缓存可能在树合并时失效的节点指针。
    // 每次断言读取窗口当前 DockId，能发现移出、合并或停靠到错误叶节点。
    auto* first    = ImGui::FindWindowByName(PANEL_NAME);
    auto* second   = ImGui::FindWindowByName(SECOND_PANEL_NAME);
    auto* settings = ImGui::FindWindowByName("###SettingsWindow");
    // 保存精确节点身份，不把左右画布仍在同一宿主误判为原布局未变。
    // 设置页既可在独立叶节点，也可在工作区外浮动，分别检查两种状态。
    const ImGuiID firstId      = first->DockId;
    const ImGuiID secondId     = second->DockId;
    const ImGuiID settingsId   = settings->DockId;
    const ImVec2  settingsSize = settings->Size;
    const ImVec2  settingsPos  = settings->Pos;
    bool          valid = firstId != 0 && secondId != 0 && firstId != secondId;
    // 多次往返会暴露未移除的空工具叶节点累积，以及被复用的失效节点缓存。
    // 不要求新工具节点每次使用相同 ID，它可以由 ImGui 在合并后重新生成。
    for ( int cycle = 0; cycle < 3; ++cycle ) {
        for ( bool fixed : { false, true } ) {
            // 切换帧只执行一次生产操作，之后两帧模拟常态 UI 的自然推进。
            // 若常态帧重复修正窗口归属，测试可能掩盖模式切换自身的错误。
            drawToolbarFrame(fixed, true);
            drawToolbarFrame(fixed, false);
            drawToolbarFrame(fixed, false);
            auto* toolbar = ImGui::FindWindowByName(" ###Toolbar");
            // 同一叶节点是比“窗口仍然停靠”更严格的条件，可以发现分栏被折叠。
            // 工具栏移出宿主，其他窗口仍保持原节点，不得把双画布合并为标签。
            valid = valid && first->DockId == firstId &&
                    second->DockId == secondId &&
                    settings->DockId == settingsId && toolbar->DockId == 0;
            // 四分之一宽的设置叶允许固定带造成的小幅缩放，不能放大为宿主全宽。
            // 浮动设置页没有节点分隔条影响，必须完全保持自身尺寸。
            const float sizeTolerance = floatingSettings ? 0.0f : 20.0f;
            valid =
                valid &&
                std::abs(settings->Size.x - settingsSize.x) <= sizeTolerance &&
                std::abs(settings->Size.y - settingsSize.y) <= sizeTolerance;
            if ( floatingSettings ) {
                // 浮动窗口没有理由随主宿主的宽度变化而移动或缩放。
                valid = valid && settings->Pos.x == settingsPos.x &&
                        settings->Pos.y == settingsPos.y;
            }
            // 失败信息同时输出几何和归属，区分尺寸回归与分栏关系回归。
            // 保留累计失败状态，后续往返即使恢复也不能掩盖中间错误。
            if ( !valid ) {
                XERROR(
                    "Toolbar transition changed layout: fixed={}, "
                    "docks={}/{}/{}, expected={}/{}/{}, size={}/{} "
                    "expected={}/{}",
                    fixed,
                    first->DockId,
                    second->DockId,
                    settings->DockId,
                    firstId,
                    secondId,
                    settingsId,
                    settings->Size.x,
                    settings->Size.y,
                    settingsSize.x,
                    settingsSize.y);
            }
        }
    }
    // 主动销毁上下文使下一场景从独立配置开始，不把浮动状态带到停靠测试。
    // Render 已结束所有 Begin/End，销毁时不会留下未平衡的窗口或样式栈。
    ImGui::DestroyContext();
    return valid;
}
/// @brief 验证固定工具栏不会删除与它共用叶节点的设置页。
/// @return 固定后仅工具栏移出，设置页仍在共享节点且画布分栏保持时返回 true。
/// @details 用户可以把设置页拖入工具栏节点，这时节点不是工具栏独占资源。
/// 删除这个节点会误移出设置页；生产逻辑必须保留整个节点及窗口关联。
bool checkSharedToolbarNode()
{
    createContext();
    drawToolbarFrame(false, false, true);
    const ImGuiID rootId = drawToolbarFrame(false, true);
    drawToolbarFrame(false, false);
    auto*         toolbar  = ImGui::FindWindowByName(" ###Toolbar");
    const ImGuiID sharedId = toolbar->DockId;
    // 在两个帧之间改变设置页归属，模拟用户主动合并标签，而非默认布局。
    // Finish 后推进窗口 Begin，确保共享状态实际接入 ImGui 的节点窗口列表。
    ImGui::DockBuilderDockWindow("###SettingsWindow", sharedId);
    ImGui::DockBuilderFinish(rootId);
    drawToolbarFrame(false, false);
    // 记录稳定后的叶身份，后续打开和收起管理器都应保持两张画布分开。
    // 不记录 DockNode 指针：占位的合并操作可能释放与重新分配其他节点。
    const ImGuiID firstId  = ImGui::FindWindowByName(PANEL_NAME)->DockId;
    const ImGuiID secondId = ImGui::FindWindowByName(SECOND_PANEL_NAME)->DockId;
    // 固定操作不能移除 sharedId，因为其中还有一个活跃的设置窗口。
    // 原画布分栏在此边界场景也应保持，不允许回退到全局默认布局。
    drawToolbarFrame(true, true);
    drawToolbarFrame(true, false);
    const bool valid =
        sharedId != 0 && toolbar->DockId == 0 &&
        ImGui::FindWindowByName("###SettingsWindow")->DockId == sharedId &&
        ImGui::FindWindowByName(PANEL_NAME)->DockId == firstId &&
        ImGui::FindWindowByName(SECOND_PANEL_NAME)->DockId == secondId;
    if ( !valid )
        XERROR("Pinning toolbar removed another window from its shared dock");
    ImGui::DestroyContext();
    return valid;
}
/// @brief 用真实工具栏视图检查非固定模式的尺寸约束和停靠自由度。
/// @return 横竖排分别开放长轴且下一帧不回弹，固定模式禁止缩放时返回 true。
/// @details 不能用替代窗口模拟 ToolbarView，否则会漏掉视图自身的 NoResize
/// 和每帧强制 DockId。这里只改变隔离配置内存，退出前恢复原设置字段。
bool checkToolbarResize()
{
    createContext();
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    const bool originalFixed      = settings.fixedToolWindow;
    const bool originalHorizontal = settings.toolbarHorizontal;
    const auto originalEdge       = settings.toolbarDockEdge;
    settings.toolbarHorizontal    = false;
    settings.fixedToolWindow      = false;
    MMM::UI::ToolbarView toolbar("Toolbar");
    // 非零旧节点缓存模拟用户从主工具分栏拖出；视图不能自行停靠回去。
    // 根节点仅供缓存指向合法节点，不引入平台窗口或图形后端。
    constexpr ImGuiID ROOT_ID    = 0x715AA;
    const ImGuiID originalToolId = MMM::UI::MainDockSpaceUI::getToolDockId();
    MMM::UI::MainDockSpaceUI::setToolDockId(ROOT_ID);
    // 同一视图跨帧绘制才能观察固定宽度约束和后续强制归位。
    // 指定较大的尺寸，避免首次自动适配内容掩盖用户期望的宽度。
    /// @brief 推进一个真实工具栏窗口帧，正数宽度表示外部尺寸请求。
    /// @param size 零向量表示不干预 ImGui 的用户尺寸状态。
    /// @details 每次先提交宿主，再提交工具栏，顺序与生产视图一致。
    /// 空根节点不接管工具栏，因此可以检查旧 ID 缓存是否错误恢复停靠。
    /// @warning 测试热路径：不写 ini，不创建平台窗口和图形后端。
    const auto drawFrame = [&](ImVec2 size) {
        ImGui::NewFrame();
        // DockSpace 根节点必须在活动宿主帧内建立，再提交真正的工具栏。
        // 空节点仍每帧保持存活，避免旧缓存指向被 ImGui 清理的测试节点。
        ImGui::SetNextWindowSize(ImVec2(900, 600));
        ImGui::Begin("ToolbarResizeHost");
        ImGui::DockSpace(ROOT_ID);
        ImGui::End();
        if ( size.x > 0 ) ImGui::SetNextWindowSize(size);
        toolbar.update(nullptr);
        ImGui::Render();
    };
    drawFrame(ImVec2(200, 400));
    drawFrame(ImVec2(200, 400));
    auto* window = ImGui::FindWindowByName(" ###Toolbar");
    // 竖排调大的高度必须生效，宽度应保持单列，而非错误开放两个轴。
    // 额外核对实际窗口标志，确保拖动角落时不会被 NoResize 拦截。
    bool        valid      = window && window->DockId == 0 &&
                             !(window->Flags & ImGuiWindowFlags_NoResize) &&
                             window->Size.x < 200 && window->Size.y == 400;
    const float fixedWidth = window->Size.x;
    drawFrame(ImVec2(280, 480));
    drawFrame(ImVec2(0, 0));
    // 常态帧不再指定尺寸；竖排高度必须保持，固定的列宽不能随请求变化。
    // 这个检查同时保证旧工具节点缓存不会强制把浮动窗口拉回去。
    valid = valid && window->DockId == 0 && window->Size.x == fixedWidth &&
            window->Size.y == 480;
    // 实际鼠标手势覆盖命中区和 ImGui 拖动状态机，不能只看 flags 或设置尺寸。
    // 点击右下角后同时改变两轴，约束必须只接受当前排布的长轴变化。
    /// @brief 用完整按下、移动、松开手势拖动真实右下角缩放握柄。
    /// @details 先推进悬浮帧让命中窗口完成更新，再发送按下事件。
    /// 鼠标移动同时包含横纵增量，因此两个轴都必须接受约束验证。
    /// 松开后额外推进一帧，检查尺寸是否被视图的常态逻辑回弹。
    /// @warning 使用 ImGui 输入队列，不调用后端或等待现实时间。
    const auto dragGrip = [&]() {
        auto&        io = ImGui::GetIO();
        const ImVec2 grip(window->Pos.x + window->Size.x - 2.0f,
                          window->Pos.y + window->Size.y - 2.0f);
        // 握柄靠近右下角的可见三角形，避开标题栏和工具按钮命中区域。
        // 这里不直接设置 ActiveId，否则会跳过真实鼠标命中判断。
        io.AddMousePosEvent(grip.x, grip.y);
        drawFrame(ImVec2(0, 0));
        drawFrame(ImVec2(0, 0));
        // 按下单独占一帧，保证后续移动是在持续按住的手势内处理。
        // 把所有事件塞进同一帧可能被输入队列合并为一次点击。
        io.AddMouseButtonEvent(0, true);
        drawFrame(ImVec2(0, 0));
        io.AddMousePosEvent(grip.x + 60.0f, grip.y + 30.0f);
        drawFrame(ImVec2(0, 0));
        // 松开提交最终尺寸，常态帧只能保留结果，不能强制回初始尺寸。
        // 两种方向共用手势，方向差异只能来自生产工具栏约束。
        io.AddMouseButtonEvent(0, false);
        drawFrame(ImVec2(0, 0));
        drawFrame(ImVec2(0, 0));
    };
    dragGrip();
    valid = valid && window->Size.x == fixedWidth && window->Size.y > 480;
    // 横排交换可调轴，宽度应立即生效而高度保持单行厚度。
    settings.toolbarHorizontal = true;
    drawFrame(ImVec2(200, 400));
    const float fixedHeight = window->Size.y;
    valid = valid && window->Size.x == 200 && fixedHeight < 400;
    drawFrame(ImVec2(280, 480));
    drawFrame(ImVec2(0, 0));
    valid = valid && window->Size.x == 280 && window->Size.y == fixedHeight;
    // 横排的宽度基线是显式请求后的 280，不能只比较上一次竖排宽度。
    // 同时比较高度，防止开放两轴的实现通过单轴变大的检查。
    dragGrip();
    valid = valid && window->Size.x > 280 && window->Size.y == fixedHeight;
    settings.fixedToolWindow = true;
    // 去标题栏会减少固定横排的厚度，先推进该外观变化再记录禁止缩放基线。
    drawFrame(ImVec2(0, 0));
    drawFrame(ImVec2(0, 0));
    const ImVec2 pinnedSize = window->Size;
    dragGrip();
    // 固定后的同一手势不能改变任何轴；这里不通过设置尺寸覆盖交互结果。
    valid = valid && window->Size.x == pinnedSize.x &&
            window->Size.y == pinnedSize.y;
    drawFrame(ImVec2(200, 400));
    // 固定模式继续使用主布局提供的窄栏，不能因解除通用限制而被用户缩放。
    // 标志与实际宽度一起检查，防止两种模式被误用同一套约束。
    valid = valid && (window->Flags & ImGuiWindowFlags_NoResize) &&
            window->Size.y == pinnedSize.y &&
            (window->Flags & ImGuiWindowFlags_NoTitleBar);
    if ( !valid )
        XERROR("Toolbar resize constraints or dock ownership regressed");
    // 共享测试套件可能继续运行其他 UI 用例，不能留下配置或工具节点缓存。
    // 上下文关闭 ini 保存，以上几何变化不会写入用户工作区。
    settings.fixedToolWindow   = originalFixed;
    settings.toolbarHorizontal = originalHorizontal;
    settings.toolbarDockEdge   = originalEdge;
    MMM::UI::MainDockSpaceUI::setToolDockId(originalToolId);
    ImGui::DestroyContext();
    return valid;
}
/// @brief 验证左侧固定带在展开管理器之后，收起后回到宿主外部左边缘。
/// @details 使用真实 DockSpace、管理器和两张并排画布，不用静态矩形替代布局。
/// 覆盖切换、管理器改宽、重复帧、主窗口缩放及工具栏解除固定。
/// 占位只能管理自身节点，画布叶 ID 和实际工具窗口的独立归属必须保留。
/// @return 展开顺序、局部归属、重复布局和收回几何均正确时返回 true。
/// 本用例复用生产占位 helper，避免测试和应用分别维护两套拆分实现。
/// 管理器可见状态在 DockSpace 之前发布，对应实际侧栏点击后的显示状态。
/// 隐藏窗口不提交 Begin，覆盖上一帧 DockNode 仍标记可见的过渡情况。
/// 两张画布使用不同内部 ID，合并成单标签组会导致身份断言失败。
/// 管理器宽度改变只影响自己的 SizeRef，测试不直接替工具栏改位置。
/// 每轮修改视口尺寸，用来检查占位不是一次性写死的屏幕坐标。
/// 没有图形后端，因此验证结果限于 ImGui 布局、归属及窗口尺寸。
bool checkToolbarAfterManager()
{
    createContext();
    // 使用与其他用例不同的根身份，防止静态布局缓存恰好命中旧树。
    // 内容厚度与视觉间距分开提供，断言不能把间距误认为图标列宽。
    constexpr ImGuiID ROOT_ID        = 0x715AC;
    constexpr float   THICKNESS      = 48;
    constexpr float   GAP            = 4;
    bool              managerVisible = true;
    bool              fixed          = true;
    ImGuiID           slotId         = 0;
    // 选用互不相同的颜色，确保悬浮测试不会把普通分隔条或活动色误认为成功。
    // 仅修改本用例自己的 ImGui 上下文，销毁时不会影响应用主题或其他测试。
    ImGui::GetStyle().Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.2f, 0.8f, 0.3f, 1);
    ImGui::GetStyle().Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.9f, 0.2f, 0.4f, 1);
    // 停靠分隔条使用 ResizeGrip 色，故意与普通分隔线设成不同颜色。
    // 原生左侧和自定义右侧都要通过同一预期，防止测试只复述实现选色。
    ImGui::GetStyle().Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.8f, 0.7f, 0.1f, 1);
    ImGui::GetStyle().Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.7f, 0.1f, 0.8f, 1);
    /// @brief 检查实际绘制顶点覆盖工具栏左右两侧的整条边界。
    /// @param color 预期主题状态，悬浮与活动分别验证。
    /// @param native 为 true 时检查左侧原生分隔条，否则检查右侧自定义分隔条。
    /// @return 状态颜色及矩形位置均正确时返回 true。
    /// @details 必须读取 Render 后的绘制数据，不能仅断言光标或命中状态。
    /// 前景层在 DockSpace 之后显示，才不会被原生普通分隔条盖住。
    /// 状态色只在命中区有效，鼠标移开后也要确认填充不再存在。
    const auto hasResizeHighlight = [&](ImGuiCol color, bool native = false) {
        const auto* slot     = ImGui::DockBuilderGetNode(slotId);
        const ImU32 expected = ImGui::GetColorU32(color);
        bool        found    = false;
        ImVec2      minimum(FLT_MAX, FLT_MAX);
        ImVec2      maximum(-FLT_MAX, -FLT_MAX);
        // 圆角填充包含透明抗锯齿顶点，只统计具有完整主题色的实际填充。
        // 检查全高覆盖，防止仅在鼠标位置画一个局部标记就误通过。
        // 原生分隔条属于宿主绘制通道，自定义分隔条位于视口前景层。
        // 分别检查实际输出，不能用自定义层的颜色代表原生主题效果。
        // DockSpace 的宿主是其内部子窗口，外层 Begin 窗口不持有这些顶点。
        const auto* drawList =
            native ? ImGui::DockBuilderGetNode(ROOT_ID)->HostWindow->DrawList
                   : ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
        for ( const auto& vertex : drawList->VtxBuffer ) {
            if ( vertex.col != expected ) continue;
            found     = true;
            minimum.x = std::min(minimum.x, vertex.pos.x);
            minimum.y = std::min(minimum.y, vertex.pos.y);
            maximum.x = std::max(maximum.x, vertex.pos.x);
            maximum.y = std::max(maximum.y, vertex.pos.y);
        }
        // 布局必须使用本帧节点坐标，拖动时不能残留在手势起点。
        // 容许半像素填充内缩，匹配 ImGui 圆角与抗锯齿的几何处理。
        const float edge =
            slot ? slot->Pos.x + (native
                                      ? -ImGui::GetStyle().DockingSeparatorSize
                                      : slot->Size.x)
                 : 0;
        return slot && found && std::abs(minimum.x - edge) < 1 &&
               std::abs(maximum.x - minimum.x -
                        ImGui::GetStyle().DockingSeparatorSize) < 1.5f &&
               std::abs(minimum.y - slot->Pos.y) < 1 &&
               std::abs(maximum.y - slot->Pos.y - slot->Size.y) < 1;
    };
    /// @brief 按生产顺序先计算宿主，再预留管理器之后的工具带。
    /// @warning 测试 UI 热路径：禁用 ini，不创建平台窗口、不读写个人配置。
    /// @details 此帧函数不使用 ToolDockId 缓存；专用占位只从实际树中读取。
    /// 工具窗口关闭固定后不再提交，确保回收不会依赖它继续可见。
    /// 管理器和画布名称与生产使用的稳定 ID 对齐，以覆盖窗口查找入口。
    /// 不设置节点的 IsVisible 标志，全部可见性由真实 Begin 和 DockSpace 更新。
    /// 背景与窗口装饰不参与坐标断言，避免主题颜色影响布局判断。
    const auto draw = [&]() {
        ImGui::NewFrame();
        // 判断顺序与生产宿主相同，不能只凭上一帧透明占位决定本帧位置。
        // 管理器收起后节点可能仍存活，可见状态必须优先排除这个旧身份。
        const bool inside =
            fixed && managerVisible && MMM::UI::toolbarManagerWorkNode(ROOT_ID);
        const auto geometry = MMM::UI::calculateToolbarWorkspaceGeometry(
            ImVec2(0, 0),
            ImGui::GetIO().DisplaySize,
            fixed && !inside,
            ImGuiDir_Left,
            THICKNESS,
            GAP);
        // 展开时宿主覆盖完整工作区，固定带从画布分支取得空间。
        // 收起时宿主起点向右移动，留出独立工具条，不能同时保留内部占位。
        ImGui::SetNextWindowPos(geometry.m_dockPos);
        ImGui::SetNextWindowSize(geometry.m_dockSize);
        // 宿主没有内容内边距，防止额外 padding 掩盖专用占位计算错误。
        // 样式在 Begin 后恢复，使管理器和画布仍使用自己的正常窗口样式。
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin(
            "ToolbarAfterManagerHost", nullptr, ImGuiWindowFlags_NoTitleBar);
        ImGui::PopStyleVar();
        if ( !ImGui::DockBuilderGetNode(ROOT_ID) ) {
            // 管理器单独占左侧，剩余工作区仍有两张独立画布，不能重建成标签组。
            ImGui::DockBuilderAddNode(ROOT_ID, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(ROOT_ID, geometry.m_dockSize);
            ImGuiID       work    = 0;
            const ImGuiID manager = ImGui::DockBuilderSplitNode(
                ROOT_ID, ImGuiDir_Left, 0.25f, nullptr, &work);
            ImGuiID       first  = 0;
            const ImGuiID second = ImGui::DockBuilderSplitNode(
                work, ImGuiDir_Right, 0.5f, nullptr, &first);
            ImGui::DockBuilderDockWindow("SideBarManager", manager);
            ImGui::DockBuilderDockWindow(PANEL_NAME, first);
            ImGui::DockBuilderDockWindow(SECOND_PANEL_NAME, second);
            ImGui::DockBuilderFinish(ROOT_ID);
        }
        // 先提交局部拆分，再让 DockSpace 计算真实位置和剩余画布尺寸。
        // 占位窗口随后提交，下一帧仍能参与节点可见性更新。
        slotId =
            MMM::UI::updateToolbarManagerLeftSlot(ROOT_ID, inside, THICKNESS);
        // 命中区在 DockSpace 重排前提交，移动帧立即反映到管理器尺寸。
        // 必须调用生产入口，避免仅操作节点尺寸而漏检真实鼠标行为。
        MMM::UI::drawToolbarManagerResizeHandle(slotId);
        ImGui::DockSpace(ROOT_ID);
        MMM::UI::drawToolbarManagerLeftSlot(slotId);
        ImGui::End();
        // 收起时不提交管理器，让 ImGui 真正执行隐藏节点的剩余空间分配。
        if ( managerVisible ) {
            ImGui::Begin("SideBarManager");
            ImGui::End();
        }
        ImGui::Begin(PANEL_TITLE);
        ImGui::End();
        ImGui::Begin(SECOND_PANEL_TITLE);
        ImGui::End();
        // 布局修改完成后才重新取观察指针，收回占位可能已经释放旧节点。
        // 工具窗口使用本帧叶矩形，禁止用管理器首次出现时的缓存坐标。
        auto* slot = ImGui::DockBuilderGetNode(slotId);
        ImGui::SetNextWindowPos(slot ? slot->Pos : geometry.m_toolbarPos);
        ImGui::SetNextWindowSize(slot ? ImVec2(THICKNESS, slot->Size.y)
                                      : geometry.m_toolbarSize);
        ImGui::SetNextWindowDockID(0);
        // 实际工具窗口禁止停靠；透明占位绝不能取代这个独立固定身份。
        if ( fixed ) {
            ImGui::Begin(
                " ###Toolbar",
                nullptr,
                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
            ImGui::End();
        }
        ImGui::Render();
    };
    // 首帧先建立真实窗口；管理器首次出现后的两帧足够让节点确认可见性。
    draw();
    draw();
    draw();
    draw();
    const ImGuiID firstId  = ImGui::FindWindowByName(PANEL_NAME)->DockId;
    const ImGuiID secondId = ImGui::FindWindowByName(SECOND_PANEL_NAME)->DockId;
    bool          passed   = true;
    for ( int round = 0; round < 3; ++round ) {
        // 多轮展开要复用当前树，不能只验证首次启动的缺省布局。
        // 稳定帧允许 ImGui 完成隐藏节点重新显现，不人为设置管理器位置。
        managerVisible = true;
        for ( int frame = 0; frame < 5; ++frame ) draw();
        auto* manager = ImGui::FindWindowByName("SideBarManager");
        auto* toolbar = ImGui::FindWindowByName(" ###Toolbar");
        auto* canvas  = ImGui::FindWindowByName(PANEL_NAME);
        auto* second  = ImGui::FindWindowByName(SECOND_PANEL_NAME);
        // 相邻顺序必须是管理器、工具栏、画布，且实际工具栏从未进入 Dock 树。
        passed &= slotId != 0 &&
                  manager->Pos.x + manager->Size.x <= toolbar->Pos.x &&
                  toolbar->Pos.x + toolbar->Size.x <= canvas->Pos.x &&
                  toolbar->DockId == 0 && canvas->DockId == firstId &&
                  second->DockId == secondId;
        // 从真实窗口边缘测量间距，不能仅检查占位请求的宽度。
        // 两侧都应只有一条分隔间距，额外 gap 会使画布侧更宽。
        // 多轮展开和视口缩放后的测量必须仍然相等。
        const float leftGap = toolbar->Pos.x - manager->Pos.x - manager->Size.x;
        const float rightGap = canvas->Pos.x - toolbar->Pos.x - toolbar->Size.x;
        passed &= leftGap > 0 && std::abs(leftGap - rightGap) < 0.5f;
        // 保存坐标值即可，不能把上一帧窗口矩形当作布局请求再次写回。
        // 后面的常态帧由生产 helper 独立推进，才能发现重复扣宽的累计误差。
        const float toolbarX = toolbar->Pos.x;
        for ( int frame = 0; frame < 20; ++frame ) draw();
        // 常态帧不能反复扣宽度，也不能累计移动管理器边界。
        passed &= std::abs(ImGui::FindWindowByName(" ###Toolbar")->Pos.x -
                           toolbarX) < 1;
        // 工具栏右侧是用户实际拖动的边界，不能以直接写 SizeRef 替代鼠标命中。
        // 两帧悬浮让 ImGui 确认透明命中窗口，再按下、移动、松开完整手势。
        // 鼠标移动期间保持按键，不要求再次点击左侧分隔条。
        // 默认分隔宽度只有两像素，命中窗口的边框和 padding 不得吃掉点击区。
        // 固定工具窗口已禁用边缘缩放，不能让其扩展热区覆盖这条分隔线。
        auto*        slot = ImGui::DockBuilderGetNode(slotId);
        const ImVec2 resizePoint(
            slot->Pos.x + slot->Size.x +
                ImGui::GetStyle().DockingSeparatorSize * 0.5f,
            slot->Pos.y + slot->Size.y * 0.6f);
        // 先悬浮左侧原生分隔条，经过原生短暂反馈延迟后检查其真正颜色。
        // 然后再悬浮自定义条，二者必须匹配同一个 ResizeGrip 主题色。
        ImGui::GetIO().AddMousePosEvent(
            slot->Pos.x - ImGui::GetStyle().DockingSeparatorSize * 0.5f,
            resizePoint.y);
        for ( int frame = 0; frame < 8; ++frame ) draw();
        passed &= hasResizeHighlight(ImGuiCol_ResizeGripHovered, true);
        ImGui::GetIO().AddMousePosEvent(resizePoint.x, resizePoint.y);
        draw();
        draw();
        // 悬浮必须绘制与原生分隔条一致的主题高亮，不能只切换缩放光标。
        passed &= hasResizeHighlight(ImGuiCol_ResizeGripHovered);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        draw();
        passed &= hasResizeHighlight(ImGuiCol_ResizeGripActive);
        ImGui::GetIO().AddMousePosEvent(resizePoint.x + 30, resizePoint.y);
        draw();
        // 保持按键移动后，活动高亮必须同步到本帧重排后的新边界。
        passed &= hasResizeHighlight(ImGuiCol_ResizeGripActive);
        // 本帧就必须更新位置，松开前即检查，避免把延迟到手势结束误判为成功。
        // 工具栏列宽继续为 THICKNESS，移动只发生在管理器和画布之间。
        passed &=
            ImGui::FindWindowByName(" ###Toolbar")->Pos.x > toolbarX + 20 &&
            ImGui::FindWindowByName(" ###Toolbar")->Size.x == THICKNESS;
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        draw();
        draw();
        // 松开后移开鼠标，避免后续布局循环把旧悬浮色当作持续有效状态。
        ImGui::GetIO().AddMousePosEvent(700, 300);
        draw();
        draw();
        passed &= !hasResizeHighlight(ImGuiCol_ResizeGripHovered) &&
                  !hasResizeHighlight(ImGuiCol_ResizeGripActive);
        // 释放后再次确认左右间距，拖拽不能把等宽留白变成单侧空白。
        // 每轮恢复都使用新的实际坐标，防止首次成功掩盖后续节点归属变化。
        manager = ImGui::FindWindowByName("SideBarManager");
        toolbar = ImGui::FindWindowByName(" ###Toolbar");
        canvas  = ImGui::FindWindowByName(PANEL_NAME);
        passed &=
            std::abs((toolbar->Pos.x - manager->Pos.x - manager->Size.x) -
                     (canvas->Pos.x - toolbar->Pos.x - toolbar->Size.x)) < 0.5f;
        // 模拟管理器分隔条扩大；固定带应随真实边界移动，而非使用首次宽度缓存。
        auto* managerNode = ImGui::FindWindowByName("SideBarManager")->DockNode;
        managerNode->SizeRef.x += 25;
        managerNode->Size.x += 25;
        managerNode->WantLockSizeOnce = true;
        draw();
        draw();
        passed &= ImGui::FindWindowByName(" ###Toolbar")->Pos.x > toolbarX;
        // 隐藏时先改变业务显示状态，模拟 SideBarManager 停止提交窗口。
        // 此刻旧叶仍可能存在，回收路径需要同时清理专用限制和空节点。
        managerVisible = false;
        draw();
        draw();
        draw();
        // 收起释放专用占位，工具栏退回左边缘；并排画布仍保持各自的叶身份。
        passed &=
            slotId == 0 && ImGui::FindWindowByName(" ###Toolbar")->Pos.x == 0 &&
            ImGui::FindWindowByName(PANEL_NAME)->DockId == firstId &&
            ImGui::FindWindowByName(SECOND_PANEL_NAME)->DockId == secondId;
        // 放大视口后再展开，固定条高度应继续由工作区确定。
        // 这个变化也覆盖 SizeRef 在不同根宽度下的剩余空间分配。
        ImGui::GetIO().DisplaySize = ImVec2(1000 + round * 30, 650);
    }
    // 最后一轮在管理器展开状态解除固定，区别于仅隐藏管理器的分支。
    // 业务开关关闭后不能仍把透明占位留在两张画布前面。
    managerVisible = true;
    draw();
    draw();
    draw();
    fixed = false;
    draw();
    draw();
    // 解除固定时只回收占位，管理器和画布继续使用原工作区。
    passed &= slotId == 0 &&
              ImGui::FindWindowByName(PANEL_NAME)->DockId == firstId &&
              ImGui::FindWindowByName(SECOND_PANEL_NAME)->DockId == secondId;
    if ( !passed )
        XERROR("Toolbar placement after FloatingManager regression failed");
    ImGui::DestroyContext();
    return passed;
}

/// @brief 验证原生边缘停靠和菜单选择均自动进入独立固定带，沿用主题圆角。
/// @return 四边的自动固定、中心占位、解除后的真实缩放全部正确时返回 true。
/// @details 原生路径使用 ImGui 的拖放停靠队列，菜单路径使用生产请求入口。
/// 新帧处理停靠请求后立刻计算固定带，不能要求用户另行勾选固定。
/// 使用真实 ToolbarView，不能以替代窗口掩盖视图自身的样式或尺寸限制。
/// 双画布同时提交，固定操作只回收工具叶，禁止重建整棵工作区。
/// 宿主矩形和工具窗口矩形都读实际 ImGui 结果，防止固定条只覆盖画布。
/// 顶部/左侧移动宿主原点，底部/右侧保持原点并缩小尺寸。
/// 固定后的主窗口缩放继续使用同一边缘和单行/单列厚度。
/// 常态帧不应再次触发偏好保存，只有新的用户停靠意图才返回变更。
/// 解除固定后仍保持浮动，中心区域恢复全部尺寸，不能再次自动固定。
/// 浮窗缩放手势同时请求改变两轴，只有长轴允许变化。
/// 底部固定带恢复标题栏后握柄仍须处于视口内，不人为移动窗口绕过缺陷。
/// 松开鼠标后推进稳定帧，防止固定位置逻辑覆盖回用户的长度。
/// 主题使用非零圆角基线，避免默认零圆角让没有应用样式的实现误通过。
/// 测试隔离 ini 文件并恢复设置，工具节点指针不跨树修改保存。
bool checkToolbarEdgeDocking()
{
    createContext();
    auto& config = MMM::Config::AppConfig::instance().getEditorSettings();
    // 本用例临时更改软件偏好和主题，必须逐字段恢复，避免影响后续共享测试。
    // 圆角也属于全局主题，不能因它与停靠逻辑无关就遗漏恢复。
    const bool  oldFixed             = config.fixedToolWindow;
    const bool  oldHorizontal        = config.toolbarHorizontal;
    const auto  oldEdge              = config.toolbarDockEdge;
    const float oldRounding          = config.aesthetics.windowRounding;
    config.fixedToolWindow           = false;
    config.aesthetics.windowRounding = 11.0f;
    MMM::UI::ToolbarView toolbar("Toolbar");
    constexpr ImGuiID    ROOT_ID = 0x715AB;
    // 只保留数值几何，节点在原生拖放拆分和空叶合并时可能立即释放。
    // changed 表示偏好变化，可用来检查常态帧不会反复发起配置保存。
    MMM::UI::ToolbarWorkspaceGeometry geometry{};
    bool                              changed = false;
    /// @brief 按生产顺序处理意图、宿主几何和真实工具窗口。
    /// @warning 测试只操作固定数量的窗口，关闭个人 ini 保存和图形后端。
    const auto drawFrame = [&]() {
        ImGui::NewFrame();
        // NewFrame 首先处理原生拖放队列，随后宿主读取新的真实节点归属。
        changed =
            MMM::UI::updateToolbarFixedDockIntent(config.fixedToolWindow,
                                                  config.toolbarHorizontal,
                                                  config.toolbarDockEdge);
        // 固定启动也可能读到旧 ini 的停靠归属；移出兼容路径需保持幂等。
        // 主动请求已移出的窗口再次检查时不应修改其他画布节点。
        if ( config.fixedToolWindow ) MMM::UI::detachFixedToolbar();
        const float dpi =
            MMM::Config::AppConfig::instance().getWindowContentScale();
        const float padding =
            2.0f * std::floor(config.aesthetics.windowPadding * dpi);
        // 内容厚度采用同一 DPI 和主题内边距，横排短标签会增加单行高度。
        // 固定窗口没有标题栏，不能把非固定的标题高度也扣进中心区域。
        const float thickness =
            std::floor((config.toolbarHorizontal && config.showToolLabels
                            ? 46.0f
                            : 32.0f) *
                       dpi) +
            padding;
        // 几何必须在本帧宿主 Begin 前确定，不能先提交全尺寸宿主再覆盖工具条。
        // 这里不通过改配置外的测试补偿位置，以免掩盖意图处理顺序错误。
        geometry = MMM::UI::calculateToolbarWorkspaceGeometry(
            ImVec2(0, 0),
            ImGui::GetIO().DisplaySize,
            config.fixedToolWindow,
            MMM::UI::toolbarDockDirection(config.toolbarDockEdge),
            thickness,
            4);
        ImGui::SetNextWindowPos(geometry.m_dockPos);
        ImGui::SetNextWindowSize(geometry.m_dockSize);
        // 零内边距与无标题宿主对应生产 DockSpace，不能把装饰当作占位误差。
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("ToolbarEdgesHost",
                     nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove);
        ImGui::PopStyleVar();
        if ( !ImGui::DockBuilderGetNode(ROOT_ID) ) {
            // 缺失根节点才建立默认布局，后面的八次固定操作必须复用这棵树。
            // 重建根节点会让所有归属断言退化成默认布局断言，失去回归意义。
            ImGui::DockBuilderAddNode(ROOT_ID, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(ROOT_ID, ImVec2(900, 600));
            ImGuiID       left  = 0;
            const ImGuiID right = ImGui::DockBuilderSplitNode(
                ROOT_ID, ImGuiDir_Right, 0.5f, nullptr, &left);
            // 两张画布各占一叶，固定切换不能把它们合并为同一标签组。
            ImGui::DockBuilderDockWindow(PANEL_NAME, left);
            ImGui::DockBuilderDockWindow(SECOND_PANEL_NAME, right);
            ImGui::DockBuilderFinish(ROOT_ID);
        }
        // 原生节点在宿主提交时确认几何，子窗口随后 Begin 接收新的实际布局。
        // 不能只检查 Builder 请求尺寸，否则还未完成的停靠也会被误判成功。
        ImGui::DockSpace(ROOT_ID);
        ImGui::End();
        ImGui::Begin(PANEL_TITLE);
        ImGui::End();
        ImGui::Begin(SECOND_PANEL_TITLE);
        ImGui::End();
        if ( config.fixedToolWindow ) {
            ImGui::SetNextWindowPos(geometry.m_toolbarPos);
            ImGui::SetNextWindowSize(geometry.m_toolbarSize);
        }
        // 工具条仍调用真实视图，以覆盖窗口类、圆角栈和浮动轴约束。
        // 固定带的 NextWindow 几何交给视图消费，不构造第二个模拟工具窗口。
        toolbar.update(nullptr);
        ImGui::Render();
    };
    // 首次浮动并无停靠意图，不能被初始化流程自动送回右侧并进入固定。
    // 两帧基线让首次窗口尺寸和字体图集状态稳定，再发原生或菜单请求。
    drawFrame();
    drawFrame();
    auto* window = ImGui::FindWindowByName(" ###Toolbar");
    bool  valid  = !config.fixedToolWindow && window->DockId == 0;
    // 两条入口各覆盖四个方向，不能只用菜单路径替代用户实际拖放的队列。
    // 每次从解除固定后的同一浮窗开始，避免上一边缘的 fixed 状态让断言假通过。
    for ( bool menu : { false, true } ) {
        for ( ImGuiDir edge :
              { ImGuiDir_Up, ImGuiDir_Down, ImGuiDir_Left, ImGuiDir_Right } ) {
            if ( menu ) {
                // 菜单选择无需先创建停靠叶；同帧直接得到外部固定带。
                MMM::UI::pendingToolbarDockRequest() = edge;
            } else {
                const auto* canvas = ImGui::FindWindowByName(PANEL_NAME);
                // 使用原生窗口拖放最终提交的队列，而非给 fixed 字段提前赋值。
                // 目标指针只传给队列，不在 NewFrame 拆分之后继续解引用。
                ImGui::DockContextQueueDock(ImGui::GetCurrentContext(),
                                            nullptr,
                                            canvas->DockNode,
                                            window,
                                            edge,
                                            0.15f,
                                            false);
                // 比例只模拟拖放生成的临时工具叶，最终固定带厚度不应沿用该比例。
                // split_outer 为 false
                // 表示画布局部停靠，固定时仍要预留整条中心边缘。
            }
            drawFrame();
            const bool horizontal =
                edge == ImGuiDir_Up || edge == ImGuiDir_Down;
            // 自动固定必须在宿主本帧计算几何前生效，不能依赖再次点击固定按钮。
            valid =
                valid && changed && config.fixedToolWindow &&
                config.toolbarHorizontal == horizontal &&
                MMM::UI::toolbarDockDirection(config.toolbarDockEdge) == edge;
            // 稳定帧必须保持固定而不再次报告变更，阻止逐帧重复保存配置。
            // 额外推进也检出工具栏仍被原生节点重新吸回的情况。
            drawFrame();
            drawFrame();
            const float dpi =
                MMM::Config::AppConfig::instance().getWindowContentScale();
            // 圆角值以视图实际应用的像素值为准，不能只检查全局主题设置。
            valid = valid && !changed && window->DockId == 0 &&
                    !window->DockNode &&
                    (window->Flags & ImGuiWindowFlags_NoDocking) &&
                    (window->Flags & ImGuiWindowFlags_NoTitleBar) &&
                    (window->Flags & ImGuiWindowFlags_NoResize) &&
                    window->WindowRounding == std::floor(11.0f * dpi) &&
                    window->Pos.x == geometry.m_toolbarPos.x &&
                    window->Pos.y == geometry.m_toolbarPos.y &&
                    window->Size.x == geometry.m_toolbarSize.x &&
                    window->Size.y == geometry.m_toolbarSize.y;
            // 原生拆分可能改变工具旁的叶 ID；根 ID 应始终保持稳定。
            // 从当前树重新取根，不能沿用发出拖放请求前的观察指针。
            const auto* root = ImGui::DockBuilderGetNode(ROOT_ID);
            // 检查真实根节点扣除占位后的矩形，工具条不得覆盖原来的中心面积。
            valid              = valid && root->Pos.x == geometry.m_dockPos.x &&
                                 root->Pos.y == geometry.m_dockPos.y &&
                                 root->Size.x == geometry.m_dockSize.x &&
                                 root->Size.y == geometry.m_dockSize.y;
            const auto* first  = ImGui::FindWindowByName(PANEL_NAME);
            const auto* second = ImGui::FindWindowByName(SECOND_PANEL_NAME);
            // 合并空工具叶后再取画布基线，允许 ImGui 正常重命名相邻叶。
            // 真正需要保持的是双画布独立性，以及后续宿主缩放不会再迁移它们。
            const ImGuiID firstId  = first->DockId;
            const ImGuiID secondId = second->DockId;
            valid = valid && firstId && secondId && firstId != secondId;
            // 固定后放大主窗口，不发新停靠请求，长轴和末端坐标仍应立即跟随。
            ImGui::GetIO().DisplaySize = ImVec2(1100, 700);
            drawFrame();
            drawFrame();
            valid = valid && !changed &&
                    window->Pos.x == geometry.m_toolbarPos.x &&
                    window->Pos.y == geometry.m_toolbarPos.y &&
                    window->Size.x == geometry.m_toolbarSize.x &&
                    window->Size.y == geometry.m_toolbarSize.y &&
                    first->DockId == firstId && second->DockId == secondId;
            // 先恢复原视口再解除固定，鼠标缩放结果不混入主窗口变大造成的变化。
            // 这里不发新的方向请求，保存的边缘偏好本身不能触发重新固定。
            ImGui::GetIO().DisplaySize = ImVec2(900, 600);
            drawFrame();
            config.fixedToolWindow = false;
            drawFrame();
            drawFrame();
            // 解除固定没有停靠意图，必须保持浮动，中心区域恢复完整矩形。
            valid = valid && !changed && !config.fixedToolWindow &&
                    window->DockId == 0 &&
                    !(window->Flags & ImGuiWindowFlags_NoResize) &&
                    geometry.m_dockSize.x == 900 &&
                    geometry.m_dockSize.y == 600;
            // 恢复标题栏后的真实厚度才是浮窗约束基线，不能沿用无标题的固定厚度。
            // 直接命中实际右下角握柄，底部条若未校正位置，手势应失败。
            const ImVec2 grip(window->Pos.x + window->Size.x - 2,
                              window->Pos.y + window->Size.y - 2);
            const float  length    = window->Size[horizontal ? 0 : 1];
            const float  thickness = window->Size[horizontal ? 1 : 0];
            auto&        io        = ImGui::GetIO();
            // 悬浮阶段刷新命中窗口，避免中心宿主覆盖后的旧 HoveredWindow 干扰。
            // 后续每帧只注入输入事件，不调用 SetNextWindowSize 来伪造缩放。
            io.AddMousePosEvent(grip.x, grip.y);
            drawFrame();
            drawFrame();
            io.AddMouseButtonEvent(0, true);
            drawFrame();
            // 两轴同时缩小，锁短轴和放开长轴必须同时成立。
            io.AddMousePosEvent(grip.x - 45, grip.y - 45);
            drawFrame();
            io.AddMouseButtonEvent(0, false);
            drawFrame();
            drawFrame();
            // 鼠标松开后的尺寸才是最终结果；只检查拖动瞬间会漏掉下一帧回弹。
            // 长轴必须真实缩短，短轴必须精确保持标题栏恢复后的单列或单行厚度。
            const bool resized = window->Size[horizontal ? 0 : 1] < length &&
                                 window->Size[horizontal ? 1 : 0] == thickness;
            valid              = valid && resized;
            if ( !valid )
                XERROR(
                    "Toolbar auto pin failed: menu={}, edge={}, fixed={}, "
                    "changed={}, dock={}, resized={}",
                    menu,
                    static_cast<int>(edge),
                    config.fixedToolWindow,
                    changed,
                    window->DockId,
                    resized);
        }
    }
    // 无方向请求来自用户选择浮动横竖排，不得被当成右侧固定请求。
    // None 是有值的消息，消费后要清空，不能与缺失 optional 的常态分支混淆。
    MMM::UI::pendingToolbarDockRequest() = ImGuiDir_None;
    drawFrame();
    valid = valid && !changed && !config.fixedToolWindow && window->DockId == 0;
    config.fixedToolWindow           = oldFixed;
    config.toolbarHorizontal         = oldHorizontal;
    config.toolbarDockEdge           = oldEdge;
    config.aesthetics.windowRounding = oldRounding;
    // 请求队列属于 UI 线程静态状态，不能让本用例的方向请求污染下一上下文。
    // 销毁之前清空队列，窗口和节点生命周期仍全部由 ImGui 上下文管理。
    MMM::UI::pendingToolbarDockRequest().reset();
    ImGui::DestroyContext();
    return valid;
}

}  // namespace

/// @brief 验证真实布局工具按钮在四边固定后打开朝内且可滚动的弹层。
/// @return 鼠标打开、按钮对齐、边界约束及内容增长均正确时返回 true。
/// @details 使用实际 ToolbarView，避免只检查计算函数而漏掉按钮锚点采集。
/// 横排按钮故意偏离视口左边缘，确保不是沿用工具栏左侧的旧定位。
/// 每个方向创建独立 ImGui 上下文，避免上次窗口位置与点击状态影响新方向。
/// 所有设置在退出前恢复，工具点击只投递逻辑命令，不启动后台会话。
/// 布局浮层含有多个折叠分组，实际尺寸由控件决定，不能强制指定成估计值。
/// 第一组断言负责生产视图的入口接线，第二组负责内容增长时的约束协议。
/// 方向判断同时覆盖工具条和弹层的实际边缘，不能只检查按钮附近的点。
/// 小视口中高度不足是合法情况，应产生可操作滚动区域而非隐藏末尾内容。
bool checkToolbarPopupPlacement()
{
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    // 保存直接修改的偏好字段，避免整个 EditorSettings 副本覆盖无关配置。
    // 本用例只改视觉排布，不改变音频设置、项目布局或快捷键配置。
    const bool oldFixed      = settings.fixedToolWindow;
    const bool oldHorizontal = settings.toolbarHorizontal;
    const bool oldLabels     = settings.showToolLabels;
    const auto oldEdge       = settings.toolbarDockEdge;
    const auto oldTools      = settings.toolbarVisibility.stateTools;
    // 只显示布局状态按钮，让测试可靠点击第一项；独立按钮保持原样。
    // 这仍经过真实反馈按钮与弹层渲染，不直接设置私有弹窗打开标志。
    settings.fixedToolWindow              = true;
    settings.showToolLabels               = false;
    settings.toolbarVisibility.stateTools = { false, false, false,
                                              false, false, true };
    bool passed                           = true;
    // 四边共享生产弹层入口，左右用例同时防止横排修复破坏竖排。
    // 不依赖用户现有固定方向，每个用例明确提供自身边缘。
    for ( ImGuiDir edge :
          { ImGuiDir_Up, ImGuiDir_Down, ImGuiDir_Left, ImGuiDir_Right } ) {
        createContext();
        // 独立上下文确保 Popup 的 Active 和 Size 不会从上个方向继承。
        // 偏好使用共享对象，因此恢复步骤不能由销毁上下文代替。
        settings.toolbarHorizontal =
            edge == ImGuiDir_Up || edge == ImGuiDir_Down;
        settings.toolbarDockEdge = edge == ImGuiDir_Up     ? "top"
                                   : edge == ImGuiDir_Down ? "bottom"
                                   : edge == ImGuiDir_Left ? "left"
                                                           : "right";
        MMM::UI::ToolbarView toolbar("Toolbar");
        const float          dpi =
            MMM::Config::AppConfig::instance().getWindowContentScale();
        // DPI 取生产配置，尺寸与点击位置必须使用同一个缩放来源。
        // 取整差异可能让窄按钮测试落到工具栏留白而不是按钮内。
        const float padding =
            std::floor(settings.aesthetics.windowPadding * dpi);
        const float thickness = std::floor(32 * dpi) + padding * 2;
        // 顶底工具栏从 X=120 起，竖排从 Y=80 起，检查完整二维按钮锚点。
        // 固定厚度使用生产 DPI 规则，避免 ImGui 窗口约束纠正测试矩形。
        const ImVec2 position =
            settings.toolbarHorizontal
                ? ImVec2(120, edge == ImGuiDir_Up ? 20 : 580 - thickness)
                : ImVec2(edge == ImGuiDir_Left ? 20 : 880 - thickness, 80);
        const ImVec2 size = settings.toolbarHorizontal ? ImVec2(700, thickness)
                                                       : ImVec2(thickness, 480);
        /// @brief 提交实际工具栏并获取自动尺寸后的弹层矩形。
        /// @warning 测试帧不创建图形后端或持久化个人 ini。
        const auto draw = [&]() {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(position);
            ImGui::SetNextWindowSize(size);
            toolbar.update(nullptr);
            ImGui::Render();
        };
        draw();
        draw();
        // 首两帧完成窗口创建与自动大小确认，后续仅注入真实输入事件。
        // 如果直接设置私有打开标志，会漏掉按钮仅保存 Y 坐标的原始缺陷。
        // 悬浮、按下、释放走真实鼠标事件，弹窗不得依赖特殊测试入口。
        // 按钮中心来自实际内边距与固定图标尺寸，短标签已在本用例关闭。
        ImGui::GetIO().AddMousePosEvent(position.x + padding + 16 * dpi,
                                        position.y + padding + 16 * dpi);
        draw();
        draw();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        draw();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        for ( int frame = 0; frame < 5; ++frame ) draw();
        const auto* popup = ImGui::FindWindowByName("##LayoutComponentsPopup");
        const auto* tool  = ImGui::FindWindowByName(" ###Toolbar");
        // 窗口可能仍在注册表中；必须同时检查 Active 才算真正打开。
        // 缺失或未激活时保留失败结果，禁止为检查矩形而解引用空指针。
        passed &= popup && popup->Active;
        if ( popup && popup->Active ) {
            // 工具条四边均朝内展开，主视口外侧安全留白不应被突破。
            // 比较真实 Begin 后的尺寸，而非比较准备阶段的估计高度。
            const int  axis    = settings.toolbarHorizontal ? 1 : 0;
            const bool leading = edge == ImGuiDir_Up || edge == ImGuiDir_Left;
            passed &=
                leading
                    ? popup->Pos[axis] >= tool->Pos[axis] + tool->Size[axis]
                    : popup->Pos[axis] + popup->Size[axis] <= tool->Pos[axis];
            passed &= popup->Pos.x >= 8 * dpi - 1 &&
                      popup->Pos.y >= 8 * dpi - 1 &&
                      popup->Pos.x + popup->Size.x <= 900 - 8 * dpi + 1 &&
                      popup->Pos.y + popup->Size.y <= 600 - 8 * dpi + 1;
            // 一像素容差只覆盖坐标截断，不允许工具条厚度量级的偏移。
            // 因而覆盖按钮或越过底边仍会让本用例失败。
            // 横排弹层应与触发按钮对齐，不能贴着视口左缘或管理器左缘。
            if ( settings.toolbarHorizontal )
                passed &= std::abs(popup->Pos.x - position.x - padding) < 1;
        }
        // 单独模拟内容大幅增长，使用相同定位与约束入口覆盖折叠分组展开。
        // 底部采用实际高度枢轴，上限内滚动，不能因旧高度缓存而盖住工具条。
        for ( int frame = 0; frame < 4; ++frame ) {
            ImGui::NewFrame();
            const auto geometry = MMM::UI::calculateToolbarPopupGeometry(
                position,
                size,
                ImVec2(position.x + padding, position.y + padding),
                ImVec2(260, 100),
                ImVec2(0, 0),
                ImVec2(900, 600),
                edge,
                4 * dpi,
                8 * dpi);
            MMM::UI::prepareToolbarPopup(geometry,
                                         ImGui::GetMainViewport()->ID);
            ImGui::Begin("##GrowingToolbarPopup",
                         nullptr,
                         ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize);
            // 固定内容宽度只用于测试，高度超过任一方向的可用工作区。
            // 实际窗口必须产生滚动范围，不能只裁剪掉末尾控件。
            ImGui::Dummy(ImVec2(240, 1400));
            ImGui::End();
            ImGui::Render();
        }
        const auto* growing = ImGui::FindWindowByName("##GrowingToolbarPopup");
        // ScrollMax 来自真实内容和窗口可用高度，正值证明受限内容仍可访问。
        // 结合边界检查，排除整窗越界却恰好生成滚动条的假通过。
        passed &= growing->ScrollMax.y > 0 && growing->Pos.y >= 8 * dpi - 1 &&
                  growing->Pos.y + growing->Size.y <= 600 - 8 * dpi + 1;
        if ( edge == ImGuiDir_Down )
            passed &= growing->Pos.y + growing->Size.y <= position.y;
        if ( edge == ImGuiDir_Up )
            passed &= growing->Pos.y >= position.y + size.y;
        // 所有矩形检查完成后才销毁，窗口观察指针不能带入下一方向。
        // 旧尺寸和鼠标状态随上下文释放，下个方向重新建立其初始状态。
        ImGui::DestroyContext();
    }
    // 这些偏好属于共享 AppConfig，任何失败路径也必须恢复原值。
    // 引擎命令在测试环境不执行，不改变用户谱面或配置持久化状态。
    settings.fixedToolWindow              = oldFixed;
    settings.toolbarHorizontal            = oldHorizontal;
    settings.showToolLabels               = oldLabels;
    settings.toolbarDockEdge              = oldEdge;
    settings.toolbarVisibility.stateTools = oldTools;
    if ( !passed )
        XERROR("Toolbar popup direction or viewport bounds regression failed");
    return passed;
}

/// @brief 独立运行默认工作区到项目工作区的停靠恢复回归场景。
/// @return 成功为 0，窗口未恢复为 1。
/// @details 先排除生成无效节点或重复 ID，再运行恢复流程。
int main()
{
    ImGuiID           firstDockId  = 0;
    ImGuiID           secondDockId = 0;
    const std::string savedIni     = makeSavedLayout(firstDockId, secondDockId);
    return !savedIni.empty() && firstDockId != 0 && secondDockId != 0 &&
                   firstDockId != secondDockId && checkRestorePlan(savedIni) &&
                   checkRestore(savedIni, firstDockId, secondDockId) &&
                   checkToolbarTransition(false) &&
                   checkToolbarTransition(true) && checkSharedToolbarNode() &&
                   checkToolbarResize() && checkToolbarEdgeDocking() &&
                   checkToolbarAfterManager() && checkToolbarPopupPlacement()
               ? 0
               : 1;
}
