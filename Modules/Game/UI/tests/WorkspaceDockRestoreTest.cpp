#include "ui/imgui/WorkspaceDockRestore.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "mmm/project/ProjectSettings.h"
#include "ui/imgui/CanvasTabManager.h"
#include "ui/imgui/ToolbarDockLayout.h"

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
/// 共享工具节点场景保证固定工具栏只移出自身，不移除同节点的其他标签。

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
    // 外部工具栏占 50 像素；解除固定后宿主扩展同样宽度。
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(fixed ? 850 : 900, 600));
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
        ImGui::DockBuilderSetNodeSize(rootId, ImVec2(850, 600));
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
    }
    if ( transition ) {
        // 测试使用实际生产 helper，覆盖节点拆分和工具栏移出时的合并行为。
        (void)MMM::UI::updateToolbarDockLayout(
            rootId, fixed, 50, ImVec2(fixed ? 850 : 900, 600));
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
    // 固定工具栏每帧接收位置和尺寸；其他三个窗口不接收强制几何。
    // 这模拟真实 UI 的责任边界，避免测试主动纠正被破坏的设置窗口。
    if ( fixed ) {
        ImGui::SetNextWindowPos(ImVec2(850, 0));
        ImGui::SetNextWindowSize(ImVec2(50, 600));
    }
    ImGui::Begin(" ###Toolbar",
                 nullptr,
                 fixed ? ImGuiWindowFlags_NoDocking : ImGuiWindowFlags_None);
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
            // 固定工具栏必须脱离节点；解除固定必须获得独立窄栏。
            valid = valid && first->DockId == firstId &&
                    second->DockId == secondId &&
                    settings->DockId == settingsId &&
                    (fixed ? toolbar->DockId == 0 : toolbar->DockId != 0);
            // 停靠尺寸容许一个分隔条的舍入差，不容许被放大为主宿主宽度。
            // 浮动设置页没有节点分隔条影响，必须完全保持自身尺寸。
            const float sizeTolerance = floatingSettings ? 0.0f : 8.0f;
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
/// @return 设置页和并排画布保留，且工具栏成功移出时返回 true。
/// @details 用户可以把设置页拖入工具栏节点，这时节点不是工具栏独占资源。
/// 删除这个节点会误移出设置页；生产逻辑必须只解除工具栏窗口的关联。
bool checkSharedToolbarNode()
{
    createContext();
    drawToolbarFrame(true, false, true);
    const ImGuiID rootId = drawToolbarFrame(false, true);
    drawToolbarFrame(false, false);
    auto*         toolbar  = ImGui::FindWindowByName(" ###Toolbar");
    const ImGuiID sharedId = toolbar->DockId;
    // 在两个帧之间改变设置页归属，模拟用户主动合并标签，而非默认布局。
    // Finish 后推进窗口 Begin，确保共享状态实际接入 ImGui 的节点窗口列表。
    ImGui::DockBuilderDockWindow("###SettingsWindow", sharedId);
    ImGui::DockBuilderFinish(rootId);
    drawToolbarFrame(false, false);
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
}  // namespace

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
                   checkToolbarTransition(true) && checkSharedToolbarNode()
               ? 0
               : 1;
}
