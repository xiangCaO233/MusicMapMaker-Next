#include "ui/walkthrough/WelcomeView.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"
#include "mmm/project/ProjectSettings.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/walkthrough/WalkthroughModel.h"
#include "ui/walkthrough/WalkthroughService.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <string_view>

/// @file WelcomeViewTest.cpp
/// @brief 欢迎页配置兼容性、无 GPU 布局、样式、导航和 Dock 恢复回归测试。
/// @details 测试创建纯 ImGui 上下文并检查生成的窗口与顶点数据，不需要
/// Vulkan 设备；个人 AppConfig 由测试公共环境设置的隔离配置根保护。
///
/// 配置场景覆盖：
/// - 旧配置缺少 showWelcomeOnStartup 时默认开启；
/// - 显式关闭值可完成序列化往返；
/// - 错误 JSON 类型回退安全默认值；
/// - 软件级欢迎页偏好不写入项目编辑器覆盖配置。
///
/// 页面场景覆盖：
/// - 初始状态显示主题目录；
/// - 用户窗口内边距按 DPI 应用于欢迎页且不污染全局样式；
/// - 深色和浅色主题都绘制章节面板背景；
/// - 章节与分支卡片沿用 Frame 圆角和 Child 边框；
/// - 章节标签使用选中上划线；
/// - 窄窗口下章节切换和主题往返保持稳定；
/// - 首个分支默认展开；
/// - 新建项目主题渲染菜单与快捷键两条真实分支；
/// - 新建项目主题在窄窗口中仍保留可见分支卡片；
/// - 两个新建谱面主题是项目限定的真实主题，不再按占位页处理；
/// - 无项目时直接渲染主题仅供布局测试，不代表目录按钮可以点击；
/// - 项目限定属性来自内置配置，页面不会按固定主题索引硬编码门禁；
/// - 空白与模板谱面正文均生成真实分支卡片，确保配置未退化为静态说明；
/// - 编辑区简介是要求谱面标签的阶段三真实主题；
/// - 学习进度在首页与正文之间保持；
/// - 越界主题索引安全回退首页；
/// - 占位主题不残留真实分支窗口。
///
/// Dock 场景覆盖：
/// - 首次出现时进入当前中心 Dock 节点；
/// - 晚创建的启动占位标签不会夺走欢迎页可见性；
/// - 项目布局重建后恢复到新的中心节点；
/// - 布局重建保持当前主题和学习进度；
/// - ini 中缺少欢迎页记录时仍保持既有停靠；
/// - 用户显式浮动后布局重建不会强制重新停靠。
///
/// 帧驱动约定：
/// - 每个状态切换后运行数帧，让 ImGui 完成窗口和 Dock 状态迁移；
/// - DisplaySize 直接模拟宽屏和窄屏工作区；
/// - DockSpaceOverViewport 只在测试已指定 Dock ID 时创建；
/// - 返回值用总顶点数确认页面产生可见绘制内容；
/// - 测试结束前恢复全局 ImGuiStyle，避免场景互相污染。
///
/// 内部状态检查约定：
/// - 使用稳定 ###WelcomePage ID 查找动态标题窗口；
/// - 使用 WelcomeContent 窗口 ID 查找章节 TabBar；
/// - 直接设置 NextSelectedTabId 模拟用户选择章节；
/// - 通过 Active 和名称后缀筛选当前帧子窗口；
/// - 通过 WindowRounding 和 WindowBorderSize 检查卡片装饰；
/// - 通过 DrawList 顶点颜色确认透明面板背景实际提交；
/// - 同时检查子窗口和父窗口以兼容 ImGui 装饰合并；
/// - 通过 BranchCard 高度确认默认展开正文已布局。
/// - 通过 Active BranchCard 确认真实主题没有退化为占位页面。
///
/// 隔离约定：
/// - io.IniFilename 设为空，禁止读写用户布局文件；
/// - 测试配置根由 CTest 环境设置，不使用个人 AppConfig；
/// - 临时修改 aesthetics.windowPadding 后立即恢复；
/// - 深浅主题循环结束后恢复完整 ImGuiStyle；
/// - MainDockSpaceUI 的中心 ID 在结束前清零；
/// - ImGui 上下文只在本测试进程存在；
/// - 不创建渲染后端、窗口系统或 GPU 纹理；
/// - 字体图集只用于生成可检查的文本顶点。
/// - 所有窗口断言均在 ImGui::Render 完成对应帧后读取。

namespace
{
/// @brief 验证旧配置默认开启、关闭偏好往返和错误类型回退。
/// @return 所有配置兼容性约束成立时返回 true。
bool testSettings()
{
    // 空对象模拟升级前没有欢迎页字段的旧配置。
    MMM::Config::EditorSettings settings;
    from_json(nlohmann::json::object(), settings);
    if ( !settings.m_showWelcomeOnStartup ) return false;
    // 显式关闭后进行完整 JSON 序列化和新对象恢复。
    settings.m_showWelcomeOnStartup = false;
    nlohmann::json encoded;
    to_json(encoded, settings);
    MMM::Config::EditorSettings restored;
    from_json(encoded, restored);
    if ( restored.m_showWelcomeOnStartup ) return false;
    // 错误类型不能传播异常值，读取器应恢复安全默认开启。
    encoded["showWelcomeOnStartup"] = "invalid";
    from_json(encoded, restored);
    if ( !restored.m_showWelcomeOnStartup ) return false;
    // 该偏好属于软件设置，项目级 editorOverride 序列化必须排除。
    MMM::ProjectSettings project;
    project.m_editorOverride   = settings;
    nlohmann::json projectJson = project;
    return !projectJson["m_editorOverride"].contains("showWelcomeOnStartup");
}
/// @brief 创建无 GPU 帧，验证宽窄窗口、往返导航和独立学习记录。
/// @return 所有绘制、导航、样式和 Dock 场景通过时返回 true。
/// @warning 测试内部创建 UIManager，要求调用方已建立 ImGui 上下文和字体图集。
bool testPages()
{
    // UIManager 提供真实演练服务，WelcomeView 作为被测页面单独构造。
    MMM::UI::UIManager   manager;
    MMM::UI::WelcomeView welcome;
    ImGuiID              dockId            = 0;
    bool                 forceFloating     = false;
    bool                 renderPlaceholder = false;
    // 帧闭包统一驱动无 GPU ImGui 帧，并按开关模拟 Dock 与占位窗口。
    const auto frame = [&](float width) {
        // 固定高度只改变宽度，用于覆盖响应式目录布局。
        ImGui::GetIO().DisplaySize = { width, 720 };
        ImGui::NewFrame();
        // 非零 ID 时创建当前测试阶段的主 DockSpace。
        if ( dockId ) ImGui::DockSpaceOverViewport(dockId);
        if ( !dockId ) {
            // 未启用 Dock 的早期场景固定浮动窗口位置和尺寸。
            ImGui::SetNextWindowPos({ 0, 0 });
            ImGui::SetNextWindowSize({ width, 700 });
            ImGui::SetWindowSize("###WelcomePage", { width, 700 });
        }
        // forceFloating 模拟用户把已停靠欢迎页拖出工作区。
        if ( forceFloating ) ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        welcome.update(&manager);
        if ( renderPlaceholder ) {
            // 占位标签后创建，用于验证其不会抢走欢迎页当前 Dock Tab。
            ImGui::SetNextWindowDockID(dockId, ImGuiCond_Always);
            ImGui::Begin("StartupPlaceholder",
                         nullptr,
                         ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::End();
        }
        ImGui::Render();
        // 无 GPU 环境以生成顶点数量确认页面确实参与绘制。
        return ImGui::GetDrawData()->TotalVtxCount > 0;
    };
    if ( !welcome.showingHome() ) return false;

    // 保存并临时修改窗口内边距，验证局部应用和样式栈恢复。
    auto& aesthetics =
        MMM::Config::AppConfig::instance().getEditorSettings().aesthetics;
    const float originalPadding      = aesthetics.windowPadding;
    const auto  originalStylePadding = ImGui::GetStyle().WindowPadding;
    aesthetics.windowPadding         = 24.0F;
    frame(960);
    const auto* paddedWindow = ImGui::FindWindowByName("###WelcomePage");
    // WelcomeView 应把逻辑像素配置乘当前内容缩放。
    const float expectedPadding =
        24.0F * MMM::Config::AppConfig::instance().getWindowContentScale();
    if ( !paddedWindow || paddedWindow->WindowPadding.x != expectedPadding ||
         paddedWindow->WindowPadding.y != expectedPadding ||
         ImGui::GetStyle().WindowPadding.x != originalStylePadding.x ||
         ImGui::GetStyle().WindowPadding.y != originalStylePadding.y )
        return false;
    // 后续场景恢复应用配置，避免改变其他布局测量。
    aesthetics.windowPadding = originalPadding;
    for ( int i = 0; i < 4; ++i )
        if ( !frame(960) ) return false;
    // 深浅色下章节均沿用设置项的淡背景、圆角和边框参数。
    // 保存完整样式，在深浅主题循环结束后整体恢复。
    const auto originalStyle = ImGui::GetStyle();
    for ( const bool light : { false, true } ) {
        if ( light )
            // 第二轮覆盖浅色主题的透明面板混合结果。
            ImGui::StyleColorsLight();
        else
            // 第一轮覆盖默认深色主题。
            ImGui::StyleColorsDark();
        auto& style      = ImGui::GetStyle();
        auto  panelColor = style.Colors[ImGuiCol_FrameBg];
        panelColor.w *= 0.35F;
        // 被测实现将 FrameBg alpha 乘 0.35，测试复现同一期望颜色。
        const auto expectedColor = ImGui::GetColorU32(panelColor);
        for ( int i = 0; i < 4; ++i ) frame(960);
        // 查找活跃 ChapterPanel 并验证窗口装饰与绘制颜色。
        bool foundPanel = false;
        for ( const auto* window : ImGui::GetCurrentContext()->Windows ) {
            if ( !window->Active ||
                 std::string_view(window->Name).find("ChapterPanel") ==
                     std::string_view::npos )
                continue;
            if ( window->WindowRounding != style.FrameRounding ||
                 window->WindowBorderSize != style.ChildBorderSize )
                // 章节卡片必须沿用设置项同款圆角和边框宽度。
                return false;
            for ( const auto& vertex : window->DrawList->VtxBuffer )
                if ( vertex.col == expectedColor ) foundPanel = true;
            // ImGui 可将子窗口装饰合并到父窗口绘制列表，避免额外绘制调用。
            // ImGui 可能把子窗口装饰合并进父 DrawList。
            if ( window->ParentWindow )
                for ( const auto& vertex :
                      window->ParentWindow->DrawList->VtxBuffer )
                    if ( vertex.col == expectedColor &&
                         window->Rect().Contains(vertex.pos) )
                        foundPanel = true;
        }
        if ( !foundPanel ) {
            XERROR("Chapter panel background missing for light={}", light);
            return false;
        }
        // 进入真实主题后验证分支卡片使用相同装饰规则。
        welcome.showTopic(0);
        for ( int i = 0; i < 4; ++i ) frame(960);
        bool foundBranch = false;
        for ( const auto* window : ImGui::GetCurrentContext()->Windows ) {
            if ( !window->Active ||
                 std::string_view(window->Name).find("BranchCard") ==
                     std::string_view::npos )
                continue;
            if ( window->WindowRounding != style.FrameRounding ||
                 window->WindowBorderSize != style.ChildBorderSize )
                return false;
            // 同时搜索子窗口和父窗口绘制列表，兼容装饰合并策略。
            for ( const auto* decorated :
                  { window,
                    static_cast<const ImGuiWindow*>(window->ParentWindow) } ) {
                if ( !decorated ) continue;
                for ( const auto& vertex : decorated->DrawList->VtxBuffer )
                    if ( vertex.col == expectedColor &&
                         window->Rect().Contains(vertex.pos) )
                        foundBranch = true;
            }
        }
        if ( !foundBranch ) {
            XERROR("Walkthrough branch background missing for light={}", light);
            return false;
        }
        // 每种主题颜色验证完后回到目录准备下一轮。
        welcome.showHome();
        for ( int i = 0; i < 4; ++i ) frame(960);
    }
    // 深浅主题切换是全局状态，必须在后续导航测试前恢复。
    ImGui::GetStyle() = originalStyle;
    // 章节标签切换与正文往返不应丢失选择；空章节仍占有独立标签。
    // 从居中内容子窗口取得章节 TabBar 的内部状态。
    ImGuiTabBar* chapters = nullptr;
    for ( const auto* window : ImGui::GetCurrentContext()->Windows ) {
        if ( std::string_view(window->Name).find("WelcomeContent") !=
             std::string_view::npos ) {
            chapters = ImGui::GetCurrentContext()->TabBars.GetByKey(
                ImHashStr("WelcomeChapters", 0, window->ID));
            if ( chapters ) break;
        }
    }
    // 两个内置章节都应存在，包括没有实际主题的个性化章节。
    if ( !chapters || chapters->Tabs.Size != 2 ) return false;
    if ( !(chapters->Flags & ImGuiTabBarFlags_DrawSelectedOverline) )
        return false;
    const auto creationTab        = chapters->Tabs[0].ID;
    const auto personalizationTab = chapters->Tabs[1].ID;
    // 直接请求选择空章节，随后在窄窗口帧中确认状态落地。
    chapters->NextSelectedTabId = personalizationTab;
    for ( int i = 0; i < 4; ++i ) frame(360);
    if ( chapters->SelectedTabId != personalizationTab ) return false;
    // 从空章节状态进入新建项目主题，菜单和快捷键分支都应正常渲染。
    welcome.showTopic(1);
    for ( int i = 0; i < 4; ++i ) frame(360);
    bool createProjectBranchVisible = false;
    for ( const auto* window : ImGui::GetCurrentContext()->Windows ) {
        if ( window->Active &&
             std::string_view(window->Name).find("BranchCard") !=
                 std::string_view::npos ) {
            createProjectBranchVisible = true;
            break;
        }
    }
    if ( !createProjectBranchVisible ) return false;
    // 阶段二同时包含空白创建和模板创建，两张卡片保持独立主题与相同排序值。
    // 即使测试未打开项目，直接渲染也应保留可读正文；模型入口条件供首页
    // 和引导按钮禁用实际操作。
    const auto& createBeatmapTopic   = manager.walkthroughService().topics()[2];
    const auto& templateBeatmapTopic = manager.walkthroughService().topics()[3];
    // ID 校验防止测试仅因索引碰巧可渲染而通过。
    // 两个主题共享项目门禁和 order，但分别持有自己的分支与进度键。
    if ( createBeatmapTopic.m_id != "mmm.create-beatmap" ||
         createBeatmapTopic.m_placeholder ||
         !createBeatmapTopic.m_requiresProject ||
         templateBeatmapTopic.m_id != "mmm.create-beatmap-template" ||
         templateBeatmapTopic.m_placeholder ||
         !templateBeatmapTopic.m_requiresProject ||
         createBeatmapTopic.m_order != templateBeatmapTopic.m_order )
        return false;
    welcome.showTopic(2);
    for ( int i = 0; i < 4; ++i ) frame(360);
    bool createBeatmapBranchVisible = false;
    for ( const auto* window : ImGui::GetCurrentContext()->Windows )
        if ( window->Active &&
             std::string_view(window->Name).find("BranchCard") !=
                 std::string_view::npos )
            createBeatmapBranchVisible = true;
    if ( !createBeatmapBranchVisible ) return false;
    // 第二张阶段二卡片同样应能进入独立的模板创建演练正文。
    welcome.showTopic(3);
    for ( int i = 0; i < 4; ++i ) frame(360);
    bool templateBeatmapBranchVisible = false;
    // BranchCard 来自真实演练模型；静态占位正文不会生成这一子窗口。
    for ( const auto* window : ImGui::GetCurrentContext()->Windows )
        if ( window->Active &&
             std::string_view(window->Name).find("BranchCard") !=
                 std::string_view::npos )
            templateBeatmapBranchVisible = true;
    if ( !templateBeatmapBranchVisible ) return false;
    // 返回首页时仍须恢复进入主题前选择的个性化章节。
    welcome.showHome();
    for ( int i = 0; i < 4; ++i ) frame(960);
    if ( chapters->SelectedTabId != personalizationTab ) return false;
    // 切回创建章节后进入真实教程，验证默认展开第一分支。
    chapters->NextSelectedTabId = creationTab;
    for ( int i = 0; i < 4; ++i ) frame(960);
    welcome.showTopic(0);
    if ( welcome.showingHome() ) return false;
    for ( int i = 0; i < 4; ++i )
        if ( !frame(960) ) return false;
    // 展开分支的自动高度应明显高于折叠卡片。
    bool expandedVisible = false;
    for ( const auto* window : ImGui::GetCurrentContext()->Windows ) {
        if ( std::string_view(window->Name).find("BranchCard") !=
                 std::string_view::npos &&
             window->Active && window->Size.y > 120.0F )
            expandedVisible = true;
    }
    if ( !expandedVisible ) return false;
    for ( int i = 0; i < 4; ++i )
        if ( !frame(360) ) return false;
    // 使用真实服务确认第一步，随后页面导航不能清除学习记录。
    auto&       service = manager.walkthroughService();
    const auto& topic   = service.topics().front();
    const auto& step    = topic.m_branches.front().m_steps.front();
    service.acknowledge(topic, step);
    welcome.showHome();
    if ( !welcome.showingHome() || !service.progress().completed(topic, step) )
        return false;
    for ( int i = 0; i < 4; ++i )
        if ( !frame(360) ) return false;
    // 越界索引在 update 中应自动回到安全首页。
    welcome.showTopic(9999);
    frame(960);
    if ( !welcome.showingHome() ) return false;
    // 阶段三和阶段四都是真实教程；直接渲染仍应有分支卡片，项目与谱面
    // 门禁只控制首页入口和引导按钮能否执行，不把正文退化成静态占位。
    const auto& editorOverview = service.topics()[4];
    if ( editorOverview.m_id != "mmm.editor-overview" ||
         editorOverview.m_placeholder || !editorOverview.m_requiresProject ||
         !editorOverview.m_requiresBeatmap )
        return false;
    const auto& composeBeatmap = service.topics()[5];
    // 创作主题必须保持独立主题身份，不能靠阶段三分支卡片残留形成假阳性。
    // requiresProject 与 requiresBeatmap 同时存在，保证目录入口只在编辑上下文
    // 就绪后开放；这里直接 showTopic 只用于验证正文模型仍然完整可渲染。
    if ( composeBeatmap.m_id != "mmm.compose-beatmap" ||
         composeBeatmap.m_placeholder || !composeBeatmap.m_requiresProject ||
         !composeBeatmap.m_requiresBeatmap )
        return false;
    welcome.showTopic(5);
    if ( welcome.showingHome() ) return false;
    for ( int i = 0; i < 4; ++i )
        if ( !frame(360) ) return false;
    bool composeBranchVisible = false;
    // 窄窗口覆盖长中文说明换行后的自动高度；只要真实 BranchCard 活动，
    // 即可证明单键与长条流程完整显示且没有复用上一主题的隐藏子窗口。
    for ( const auto* candidate : ImGui::GetCurrentContext()->Windows )
        if ( candidate->Active &&
             std::string_view(candidate->Name).find("BranchCard") !=
                 std::string_view::npos )
            composeBranchVisible = true;
    if ( !composeBranchVisible ) return false;
    welcome.showHome();
    if ( !welcome.showingHome() || !service.progress().completed(topic, step) )
        return false;
    frame(960);

    // 模拟项目恢复时销毁旧停靠树并生成新的中心节点，主题和学习记录必须保留。
    // 后续场景启用 Dock，并提供第一个项目布局中心节点。
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    dockId = ImHashStr("WelcomeDockBeforeProject");
    MMM::UI::MainDockSpaceUI::setCenterDockId(dockId);
    for ( int i = 0; i < 4; ++i ) frame(960);
    auto* window = ImGui::FindWindowByName("###WelcomePage");
    if ( !window || window->DockId != dockId ) {
        XERROR("Welcome initial dock mismatch: actual={}, expected={}",
               window ? window->DockId : 0,
               dockId);
        return false;
    }
    // 回到首页后晚创建占位标签，欢迎页仍应保持当前可见 Tab。
    welcome.showHome();
    frame(960);
    renderPlaceholder = true;
    for ( int i = 0; i < 4; ++i ) frame(960);
    if ( !window->DockTabIsVisible ) {
        XERROR("Late startup placeholder stole the welcome tab");
        return false;
    }
    renderPlaceholder = false;
    // 布局重建前停留在主题页并显式记录重新停靠意图。
    welcome.showTopic(0);
    welcome.prepareForDockLayoutChange();
    // 删除旧树并发布新的中心 ID，模拟打开项目后的布局恢复。
    ImGui::DockBuilderRemoveNode(dockId);
    dockId = ImHashStr("WelcomeDockAfterProject");
    MMM::UI::MainDockSpaceUI::setCenterDockId(dockId);
    for ( int i = 0; i < 4; ++i ) frame(960);
    if ( window->DockId != dockId || welcome.showingHome() ||
         !service.progress().completed(topic, step) ) {
        XERROR("Welcome rebuild state mismatch: actual={}, expected={}",
               window->DockId,
               dockId);
        return false;
    }

    // 项目 ini 没有欢迎页记录时，同样不能被整体布局加载解除停靠。
    // 空 ini 没有欢迎页窗口记录，重新加载也不应解除当前停靠。
    welcome.prepareForDockLayoutChange();
    ImGui::LoadIniSettingsFromMemory("[Docking][Data]\n");
    for ( int i = 0; i < 4; ++i ) frame(960);
    if ( window->DockId != dockId || welcome.showingHome() ) {
        XERROR("Welcome ini restore mismatch: actual={}, expected={}",
               window->DockId,
               dockId);
        return false;
    }

    // 用户主动浮动后再替换停靠树，不能被自动停靠策略吸回工作区。
    // 最终场景先显式解除停靠，再重建树验证用户意图优先。
    forceFloating = true;
    frame(960);
    forceFloating = false;
    if ( window->DockId != 0 ) {
        XERROR("Welcome explicit undock failed: {}", window->DockId);
        return false;
    }
    welcome.prepareForDockLayoutChange();
    ImGui::DockBuilderRemoveNode(dockId);
    for ( int i = 0; i < 4; ++i ) frame(960);
    MMM::UI::MainDockSpaceUI::setCenterDockId(0);
    // 清空共享中心 ID，并以窗口保持浮动作为最终断言。
    return window->DockId == 0;
}
}  // namespace
/// @brief 欢迎页配置与无 GPU 渲染回归入口，个人配置由测试公共守卫隔离。
/// @return 0 表示全部场景通过，1 表示配置失败，2 表示 UI 场景失败。
int main()
{
    // 配置测试不依赖 ImGui，应在创建上下文前先运行。
    if ( !testSettings() ) return 1;
    // 建立最小无后端 ImGui 上下文供窗口、Dock 和 DrawList 测试。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    // 禁止测试读写用户 imgui.ini，并固定帧时间为 60 FPS。
    io.IniFilename        = nullptr;
    io.DeltaTime          = 1.0F / 60.0F;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    // 构建字体图集，使文本绘制在无渲染后端环境下仍能生成顶点。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const bool success = pixels && width > 0 && height > 0 && testPages();
    // 所有断言完成后释放全局 ImGui 状态。
    ImGui::DestroyContext();
    return success ? 0 : 2;
}
