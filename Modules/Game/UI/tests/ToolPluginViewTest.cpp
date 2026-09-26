#include "ui/plugin/ToolPluginView.h"
#include "config/AppPaths.h"
#include "log/colorful-log.h"
#include "ui/imgui/MainDockSpaceUI.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

/// @file ToolPluginViewTest.cpp
/// @brief 验证工具脚本加载与其生成窗口的主视口停靠契约。
/// @details 普通脚本检查无需图形上下文；停靠检查另建无后端 ImGui 上下文。
/// 启用多视口标志以模拟应用配置，但不创建 GLFW 或 Vulkan 平台窗口。
/// 检查首次两个插件进入同一节点、之后能拆成左右节点，以及解除停靠后
/// 仍由主视口承载，防止工具面板变成独立系统窗口。
/// 所有临时脚本只写隔离配置根，ImGui ini 也关闭磁盘保存。

namespace
{
// 这个测试不创建图形设备：加载器应先构造声明式缓存，窗口只有在 UI
// update 中才提交 ImGui 绘制。目录由测试注入的 MMM_CONFIG_ROOT 隔离。
// 内置脚本与外置脚本经过同一份清单校验，不依赖应用实际用户配置。
/// @brief 在内存清单中查找稳定插件 ID。
/// @param plugins 最近一次加载的工具插件快照。
/// @param id 待查找插件标识。
/// @return 清单包含且成功构建该插件时为 true。
bool contains(const std::vector<MMM::UI::ToolPluginInfo>& plugins,
              const std::string&                          id)
{
    // 只用稳定 ID 匹配身份，且检查 build 未留下声明错误。
    // 可见名称可能本地化，不能作为插件成功加载的判据。
    return std::any_of(
        plugins.begin(), plugins.end(), [&id](const auto& plugin) {
            return plugin.id == id && plugin.available && plugin.error.empty();
        });
}

/// @brief 验证工具窗口首次进入主 DockSpace，之后仍可被拆分停靠。
/// @return 插件能与编辑器面板停靠、拆分且始终由主视口承载时为 true。
/// @warning 测试只驱动 ImGui 自身，不创建平台或图形后端。
bool testDockablePluginWindows()
{
    // 平台窗口创建属于后端，测试只需验证 ImGui 对每个面板分配的 Viewport ID。
    // 视图构造放在上下文之后，使后续新增控件也可安全查询 ImGui 状态。
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // 多视口模式与真实应用一致；禁用 ini 落盘，避免写入个人工作区。
    io.IniFilename = nullptr;
    io.ConfigFlags |=
        ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
    io.DisplaySize = ImVec2(900.0F, 600.0F);
    io.DeltaTime   = 1.0F / 60.0F;
    // 无渲染后端时显式构建字体图集，满足 NewFrame 的内部前置条件。
    unsigned char* pixels = nullptr;
    int            width  = 0;
    int            height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    constexpr const char* audioWindow   = "###ToolPlugin_mmm.audio_transcoder";
    constexpr const char* beatmapWindow = "###ToolPlugin_mmm.beatmap_converter";
    constexpr const char* editorWindow  = "EditorCanvasPanel";
    // 用户配置中的旧窗口仅有 Pos/Size，无 DockId，也没有 ViewportId。
    // 同时保留独立平台视口案例，验证两种旧布局都能进入主停靠树。
    ImGui::LoadIniSettingsFromMemory(
        "[Window][###ToolPlugin_mmm.audio_transcoder]\n"
        "Pos=100,100\n"
        "Size=400,400\n"
        "\n"
        "[Window][###ToolPlugin_mmm.beatmap_converter]\n"
        "Pos=200,120\n"
        "Size=400,400\n"
        "ViewportPos=1200,1000\n"
        "ViewportId=0xAABBCCDD\n");
    const auto* audioSettings =
        ImGui::FindWindowSettingsByID(ImHashStr(audioWindow));
    const auto* beatmapSettings =
        ImGui::FindWindowSettingsByID(ImHashStr(beatmapWindow));
    // 加载结果也进入最终断言：如果 ImGui 更改 ini 解析规则，不能让模拟的
    // 旧平台窗口悄悄变成没有设置的新窗口而使迁移测试虚假通过。
    // 音频窗口对应用户报告的普通内部浮动窗口；只有窗口位置和尺寸。
    // 谱面转换窗口模拟此前可能被提升成独立平台窗口的另一种旧状态。
    // 两个无 DockId 的窗口都必须从已保存布局恢复，而非假装是新窗口。
    const bool legacyFloatingLoaded =
        audioSettings && audioSettings->DockId == 0 &&
        audioSettings->ViewportId == 0 && beatmapSettings &&
        beatmapSettings->DockId == 0 &&
        beatmapSettings->ViewportId == 0xAABBCCDD;

    // 插件视图拥有两个独立 Lua 状态，窗口身份由稳定 ### ID 区分。
    MMM::UI::ToolPluginView view;
    bool                    opened = view.openPlugin("mmm.audio_transcoder") &&
                                     view.openPlugin("mmm.beatmap_converter");
    // 查找只用 ### 后稳定 ID；可见中文标题更换不应改变窗口归属。

    /// @brief 提交一帧主 DockSpace 与两个插件窗口。
    /// @param split 是否在已创建的根节点中重建左右分栏。
    /// @param undock 是否移除所有窗口的停靠归属并清除持久化引用。
    /// @param pluginWidth 浮动插件窗口的目标宽度；零表示不覆盖当前尺寸。
    /// @return 当前主 DockSpace 的稳定节点 ID。
    const auto drawFrame =
        [&](bool split, bool undock = false, float pluginWidth = 0.0F) {
            ImGui::NewFrame();
            // 固定宿主几何让重新分栏前后比较的是节点归属，而非窗口尺寸抖动。
            ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(900.0F, 600.0F), ImGuiCond_Always);
            ImGui::Begin("PluginDockHost");
            const ImGuiID dockId = ImGui::GetID("PluginDockSpace");
            ImGui::DockSpace(dockId);
            if ( split ) {
                // 模拟用户把两个标签拆到不同节点，检查插件不会每帧拉回中心。
                ImGui::DockBuilderRemoveNode(dockId);
                ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockId, ImVec2(900.0F, 600.0F));
                ImGuiID left = 0, right = 0;
                ImGui::DockBuilderSplitNode(
                    dockId, ImGuiDir_Right, 0.4F, &right, &left);
                // 普通编辑器窗口与工具插件共用节点，覆盖只能停靠插件列表的回归。
                ImGui::DockBuilderDockWindow(editorWindow, left);
                ImGui::DockBuilderDockWindow(audioWindow, left);
                ImGui::DockBuilderDockWindow(beatmapWindow, right);
                ImGui::DockBuilderFinish(dockId);
            }
            if ( undock ) {
                // 清除两个插件的 Dock 引用，模拟用户把面板从标签组拖出。
                // 不能通过销毁节点代替，否则测试只会覆盖整棵布局重建。
                ImGui::DockBuilderRemoveNodeDockedWindows(dockId);
            }
            ImGui::End();
            // 编辑器窗口与插件窗口由相同 DockSpace 管理，不参与插件专用逻辑。
            // 它每帧保持存在，才能验证插件停靠后实际共享编辑器节点。
            ImGui::Begin(editorWindow);
            ImGui::End();
            MMM::UI::MainDockSpaceUI::setCenterDockId(dockId);
            // NextWindowSize 只作用于接下来第一个工具窗口，即音频插件。
            // 停靠解除后可比较同一声明布局在宽窄两个浮动面板中的高度。
            if ( pluginWidth > 0.0F )
                ImGui::SetNextWindowSize(ImVec2(pluginWidth, 600.0F),
                                         ImGuiCond_Always);
            view.update(nullptr);
            ImGui::Render();
            return dockId;
        };

    // 两帧让首次 Begin 与 DockNode 归属稳定；首帧的请求不算实际停靠。
    drawFrame(false);
    drawFrame(false);
    auto*         audio        = ImGui::FindWindowByName(audioWindow);
    auto*         beatmap      = ImGui::FindWindowByName(beatmapWindow);
    const ImGuiID mainViewport = ImGui::GetMainViewport()->ID;
    // 同一 DockId 表示两个插件是内部标签，而非各自拥有一个平台视口。
    // 主视口 ID 再约束它们所处的真正宿主窗口。
    const bool initiallyDocked = opened && legacyFloatingLoaded && audio &&
                                 beatmap && audio->DockId != 0 &&
                                 audio->DockId == beatmap->DockId &&
                                 audio->Viewport->ID == mainViewport &&
                                 beatmap->Viewport->ID == mainViewport;

    // 后续分栏必须由用户操作维持，不能被首次停靠迁移逻辑反复覆盖。
    drawFrame(true);
    drawFrame(false);
    audio                   = ImGui::FindWindowByName(audioWindow);
    beatmap                 = ImGui::FindWindowByName(beatmapWindow);
    auto*      editor       = ImGui::FindWindowByName(editorWindow);
    const bool splitRemains = audio && beatmap && audio->DockId != 0 &&
                              beatmap->DockId != 0 &&
                              audio->DockId != beatmap->DockId && editor &&
                              editor->DockId == audio->DockId &&
                              audio->Viewport->ID == mainViewport &&
                              beatmap->Viewport->ID == mainViewport;
    // 拆分后的节点仍各自属于主视口；再次绘制验证插件没有自动拉回中心。
    // 解除停靠后保留窗口可见，检查它们是内部浮动 ImGui 面板。
    drawFrame(false, true);
    drawFrame(false);
    // 此时两个窗口已经历再次 Begin，旧节点的过期 DockId 不应留在窗口状态。
    // 如果任一窗口被多视口提升为平台窗口，其 Viewport ID 会不同于主视口。
    // 两种条件都要检查，单独的 DockId == 0 不能证明仍在应用内部。
    audio                         = ImGui::FindWindowByName(audioWindow);
    beatmap                       = ImGui::FindWindowByName(beatmapWindow);
    const bool floatingInsideMain = audio && beatmap && audio->DockId == 0 &&
                                    beatmap->DockId == 0 &&
                                    audio->Viewport->ID == mainViewport &&
                                    beatmap->Viewport->ID == mainViewport;
    // 两组 row 在宽窗口各占一行，窄窗口改为每个输入项单独一行。
    // 内容高度随布局变化，而不是把右列挤出可视范围。
    // ImGui 在下一次 Begin 时才把上帧内容尺寸写回窗口状态，故各绘两帧。
    // 先解除停靠再设置宽度，避免 DockNode 用自身尺寸覆盖测试输入。
    // 700 足以容纳两列，300 小于两列的最小宽度之和。
    // 两次测量复用同一音频插件实例，确保差异只来自窗口尺寸。
    drawFrame(false, false, 700.0F);
    drawFrame(false, false, 700.0F);
    audio                  = ImGui::FindWindowByName(audioWindow);
    const float wideHeight = audio ? audio->ContentSize.y : 0.0F;
    drawFrame(false, false, 300.0F);
    drawFrame(false, false, 300.0F);
    audio = ImGui::FindWindowByName(audioWindow);
    const bool responsiveLayout =
        audio && audio->ContentSize.y > wideHeight + ImGui::GetFontSize();
    // 失败时保留每项判据，避免把停靠回归误诊为行布局宽度问题。
    if ( !initiallyDocked || !splitRemains || !floatingInsideMain ||
         !responsiveLayout )
        XERROR(
            "Plugin UI check: initial={} split={} floating={} responsive={} "
            "wideHeight={} narrowHeight={}",
            initiallyDocked,
            splitRemains,
            floatingInsideMain,
            responsiveLayout,
            wideHeight,
            audio ? audio->ContentSize.y : 0.0F);
    // 静态中心 ID 只在本测试有效，销毁上下文前清零以免影响后续测试。
    // 视图对象晚于上下文销毁；其析构只释放 Lua 和内存状态，不调用 ImGui。
    MMM::UI::MainDockSpaceUI::setCenterDockId(0);
    ImGui::DestroyContext();
    return initiallyDocked && splitRemains && floatingInsideMain &&
           responsiveLayout;
}
}  // namespace

/// @brief 验证两个内置工具和用户脚本使用同一声明式加载流程。
/// @return 失败时返回非零，交给 CTest 报告。
int main()
{
    // CTest 为当前套件注入独立 MMM_CONFIG_ROOT，测试只写该目录。
    // 不使用用户本机插件目录，避免无关脚本或最近目录影响结果。
    const auto      root = MMM::Config::AppPaths::pluginsRootPath() / "tools";
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if ( error ) return 1;

    MMM::UI::ToolPluginView view;
    // 内置脚本经过 sol::safe_script、类型检查和 build 回调构造。
    // 两个工具应在没有 assets 文件复制到配置目录时仍能加载。
    if ( !contains(view.plugins(), "mmm.audio_transcoder") ||
         !contains(view.plugins(), "mmm.beatmap_converter") ||
         !view.openPlugin("mmm.audio_transcoder") ||
         view.openPlugin("missing.plugin") ) {
        // 不存在的 ID 不能意外打开上一次的窗口。
        return 2;
    }

    // 用户脚本从隔离目录读取；重复 ID 不得替换内置工具实例。
    // 此处还检查 Lua base 库里的文件读取函数确实被移除。
    // 若沙箱意外暴露 dofile/loadfile，脚本会在入口阶段主动失败。
    const auto    path = root / "test-tool.lua";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "assert(io == nil and os == nil and package == nil and "
              "dofile == nil and loadfile == nil); "
              "return { type='tool', id='test.tool', name='测试工具', "
              "build=function(api) return {{type='row', id='fields', "
              "min_column_width=230, children={{type='input', id='first', "
              "label='第一项', value='1'}, {type='input', id='second', "
              "label='第二项', value='2'}}}} end, "
              "on_action=function(id, value, api) end }";
    output.close();
    if ( !output ) return 3;
    view.reload();
    // 显式重载后重新扫描测试目录；没有轮询或逐帧文件检测。
    // 自定义工具使用两个输入框的 row，走与内置转码工具相同的解析入口。
    // 回调未执行时控件树也必须先构建完成，错误会保留在清单快照中。
    const auto tool = std::find_if(
        view.plugins().begin(), view.plugins().end(), [](const auto& info) {
            return info.id == "test.tool";
        });
    // 只存在列表项不足以证明 row 已解析；必须确认 build 没有留下错误。
    const bool loaded = tool != view.plugins().end() && tool->error.empty() &&
                        view.openPlugin("test.tool") &&
                        contains(view.plugins(), "mmm.audio_transcoder");
    // 外置 row 声明成功解析，且不移除内置脚本；两种来源共用宿主。
    // 语法错误必须留在列表中供开发者定位，且不能生成空操作窗口。
    // 这项诊断避免用户必须查看终端日志才能发现插件没有出现的原因。
    const auto    invalidPath = root / "invalid-tool.lua";
    std::ofstream invalid(invalidPath, std::ios::binary | std::ios::trunc);
    invalid << "return {";
    invalid.close();
    if ( !invalid ) return 4;
    view.reload();
    // 失败项只存在于列表快照，available=false 表示不能打开窗口。
    const bool diagnosed = std::any_of(
        view.plugins().begin(), view.plugins().end(), [](const auto& info) {
            return !info.available && !info.error.empty() &&
                   info.name.find("invalid-tool.lua") != std::string::npos;
        });
    // 错误项名称含文件路径，便于定位并区分多个同时失败的脚本。
    const bool removedValid = std::filesystem::remove(path, error) && !error;
    // 两个清理结果分别判断，不让第二次成功掩盖第一次的残留文件。
    error.clear();
    const bool removedInvalid =
        std::filesystem::remove(invalidPath, error) && !error;
    // 清理失败与脚本未加载都作为失败，避免后续测试看到残留插件。
    // CTest 隔离目录会在套件结束后删除，但本测试仍负责自身临时脚本。
    if ( !loaded || !diagnosed || !removedValid || !removedInvalid ) return 5;
    return testDockablePluginWindows() ? 0 : 6;
}
