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

namespace
{
/// @brief 验证旧配置默认开启、关闭偏好往返和错误类型回退。
bool testSettings()
{
    MMM::Config::EditorSettings settings;
    from_json(nlohmann::json::object(), settings);
    if ( !settings.m_showWelcomeOnStartup ) return false;
    settings.m_showWelcomeOnStartup = false;
    nlohmann::json encoded;
    to_json(encoded, settings);
    MMM::Config::EditorSettings restored;
    from_json(encoded, restored);
    if ( restored.m_showWelcomeOnStartup ) return false;
    encoded["showWelcomeOnStartup"] = "invalid";
    from_json(encoded, restored);
    if ( !restored.m_showWelcomeOnStartup ) return false;
    MMM::ProjectSettings project;
    project.m_editorOverride   = settings;
    nlohmann::json projectJson = project;
    return !projectJson["m_editorOverride"].contains("showWelcomeOnStartup");
}
/// @brief 创建无 GPU 帧，验证宽窄窗口、往返导航和独立学习记录。
bool testPages()
{
    MMM::UI::UIManager   manager;
    MMM::UI::WelcomeView welcome;
    ImGuiID              dockId            = 0;
    bool                 forceFloating     = false;
    bool                 renderPlaceholder = false;
    const auto           frame             = [&](float width) {
        ImGui::GetIO().DisplaySize = { width, 720 };
        ImGui::NewFrame();
        if ( dockId ) ImGui::DockSpaceOverViewport(dockId);
        if ( !dockId ) {
            ImGui::SetNextWindowPos({ 0, 0 });
            ImGui::SetNextWindowSize({ width, 700 });
            ImGui::SetWindowSize("###WelcomePage", { width, 700 });
        }
        if ( forceFloating ) ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        welcome.update(&manager);
        if ( renderPlaceholder ) {
            ImGui::SetNextWindowDockID(dockId, ImGuiCond_Always);
            ImGui::Begin("StartupPlaceholder",
                         nullptr,
                         ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::End();
        }
        ImGui::Render();
        return ImGui::GetDrawData()->TotalVtxCount > 0;
    };
    if ( !welcome.showingHome() ) return false;

    auto& aesthetics =
        MMM::Config::AppConfig::instance().getEditorSettings().aesthetics;
    const float originalPadding      = aesthetics.windowPadding;
    const auto  originalStylePadding = ImGui::GetStyle().WindowPadding;
    aesthetics.windowPadding         = 24.0F;
    frame(960);
    const auto* paddedWindow = ImGui::FindWindowByName("###WelcomePage");
    const float expectedPadding =
        24.0F * MMM::Config::AppConfig::instance().getWindowContentScale();
    if ( !paddedWindow || paddedWindow->WindowPadding.x != expectedPadding ||
         paddedWindow->WindowPadding.y != expectedPadding ||
         ImGui::GetStyle().WindowPadding.x != originalStylePadding.x ||
         ImGui::GetStyle().WindowPadding.y != originalStylePadding.y )
        return false;
    aesthetics.windowPadding = originalPadding;
    for ( int i = 0; i < 4; ++i )
        if ( !frame(960) ) return false;
    // 深浅色下章节均沿用设置项的淡背景、圆角和边框参数。
    const auto originalStyle = ImGui::GetStyle();
    for ( const bool light : { false, true } ) {
        if ( light )
            ImGui::StyleColorsLight();
        else
            ImGui::StyleColorsDark();
        auto& style      = ImGui::GetStyle();
        auto  panelColor = style.Colors[ImGuiCol_FrameBg];
        panelColor.w *= 0.35F;
        const auto expectedColor = ImGui::GetColorU32(panelColor);
        for ( int i = 0; i < 4; ++i ) frame(960);
        bool foundPanel = false;
        for ( const auto* window : ImGui::GetCurrentContext()->Windows ) {
            if ( !window->Active ||
                 std::string_view(window->Name).find("ChapterPanel") ==
                     std::string_view::npos )
                continue;
            if ( window->WindowRounding != style.FrameRounding ||
                 window->WindowBorderSize != style.ChildBorderSize )
                return false;
            for ( const auto& vertex : window->DrawList->VtxBuffer )
                if ( vertex.col == expectedColor ) foundPanel = true;
            // ImGui 可将子窗口装饰合并到父窗口绘制列表，避免额外绘制调用。
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
        welcome.showHome();
        for ( int i = 0; i < 4; ++i ) frame(960);
    }
    ImGui::GetStyle() = originalStyle;
    // 章节标签切换与正文往返不应丢失选择；空章节仍占有独立标签。
    ImGuiTabBar* chapters = nullptr;
    for ( const auto* window : ImGui::GetCurrentContext()->Windows ) {
        if ( std::string_view(window->Name).find("WelcomeContent") !=
             std::string_view::npos ) {
            chapters = ImGui::GetCurrentContext()->TabBars.GetByKey(
                ImHashStr("WelcomeChapters", 0, window->ID));
            if ( chapters ) break;
        }
    }
    if ( !chapters || chapters->Tabs.Size != 2 ) return false;
    if ( !(chapters->Flags & ImGuiTabBarFlags_DrawSelectedOverline) )
        return false;
    const auto creationTab        = chapters->Tabs[0].ID;
    const auto personalizationTab = chapters->Tabs[1].ID;
    chapters->NextSelectedTabId   = personalizationTab;
    for ( int i = 0; i < 4; ++i ) frame(360);
    if ( chapters->SelectedTabId != personalizationTab ) return false;
    welcome.showTopic(1);
    for ( int i = 0; i < 4; ++i ) frame(360);
    welcome.showHome();
    for ( int i = 0; i < 4; ++i ) frame(960);
    if ( chapters->SelectedTabId != personalizationTab ) return false;
    chapters->NextSelectedTabId = creationTab;
    for ( int i = 0; i < 4; ++i ) frame(960);
    welcome.showTopic(0);
    if ( welcome.showingHome() ) return false;
    for ( int i = 0; i < 4; ++i )
        if ( !frame(960) ) return false;
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
    auto&       service = manager.walkthroughService();
    const auto& topic   = service.topics().front();
    const auto& step    = topic.m_branches.front().m_steps.front();
    service.acknowledge(topic, step);
    welcome.showHome();
    if ( !welcome.showingHome() || !service.progress().completed(topic, step) )
        return false;
    for ( int i = 0; i < 4; ++i )
        if ( !frame(360) ) return false;
    welcome.showTopic(9999);
    frame(960);
    if ( !welcome.showingHome() ) return false;
    // 占位主题可独立进入和返回，不能残留上一主题的实际分支或修改已有进度。
    for ( std::size_t index = 1; index < service.topics().size(); ++index ) {
        welcome.showTopic(index);
        if ( welcome.showingHome() ) return false;
        for ( int i = 0; i < 4; ++i )
            if ( !frame(index == 1 ? 360 : 960) ) return false;
        for ( const auto* window : ImGui::GetCurrentContext()->Windows )
            if ( window->Active &&
                 std::string_view(window->Name).find("BranchCard") !=
                     std::string_view::npos )
                return false;
        welcome.showHome();
        if ( !welcome.showingHome() ||
             !service.progress().completed(topic, step) )
            return false;
        frame(960);
    }

    // 模拟项目恢复时销毁旧停靠树并生成新的中心节点，主题和学习记录必须保留。
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
    welcome.showHome();
    frame(960);
    renderPlaceholder = true;
    for ( int i = 0; i < 4; ++i ) frame(960);
    if ( !window->DockTabIsVisible ) {
        XERROR("Late startup placeholder stole the welcome tab");
        return false;
    }
    renderPlaceholder = false;
    welcome.showTopic(0);
    welcome.prepareForDockLayoutChange();
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
    return window->DockId == 0;
}
}  // namespace
/// @brief 欢迎页配置与无 GPU 渲染回归入口，个人配置由测试公共守卫隔离。
int main()
{
    if ( !testSettings() ) return 1;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io              = ImGui::GetIO();
    io.IniFilename        = nullptr;
    io.DeltaTime          = 1.0F / 60.0F;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const bool success = pixels && width > 0 && height > 0 && testPages();
    ImGui::DestroyContext();
    return success ? 0 : 2;
}
