#include "ui/imgui/manager/ProjectAudioToolView.h"

#include "config/AppConfig.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "mmm/project/Project.h"
#include "ui/UIManager.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>

/// @file ProjectAudioToolScrollTest.cpp
/// @brief 使用真实工具视图验证停靠、标签切换和双轴滚动。
/// @details 不启动 GPU 或音频设备；资源只有内存 ID，不读取用户媒体文件。
/// 所有项目写入都位于构建树，ImGui 禁止写入个人工作区 ini。
namespace
{
/// @brief 查找真实工具创建的画布子窗口。
/// @return 已创建的子窗口；首次提交前返回空指针。
/// @note 翻译只改变前缀，子窗口名称中的稳定画布 ID 不变。
ImGuiWindow* findCanvas()
{
    // 不依赖具体语言，避免翻译缓存影响测试识别。
    for ( auto* window : ImGui::GetCurrentContext()->Windows ) {
        if ( std::string_view(window->Name).find("/ProjectAudioToolCanvas_") !=
             std::string_view::npos ) {
            return window;
        }
    }
    // 查找失败属于夹具或视图创建异常，不回退到宿主滚动条。
    return nullptr;
}

/// @brief 提交一帧真实工具和共享停靠标签。
/// @param manager 工具依赖的 UI 管理器。
/// @param view 本次测试唯一的工具实例。
/// @param docked 是否停靠；浮动阶段验证同一实例的原有滚动行为。
/// @param wheel 纵向滚轮增量，负值向下。
/// @param focusOther 是否激活同节点中的另一个标签。
/// @param height 工具所在宿主的高度。
/// @param shift 是否按住 Shift，显式请求横向滚动。
/// @warning 测试帧入口：只推进 ImGui 内存状态，不等待真实时间。
void drawFrame(MMM::UI::UIManager& manager, MMM::UI::ProjectAudioToolView& view,
               bool docked, float wheel = 0.0F, bool focusOther = false,
               float height = 450.0F, bool shift = false)
{
    auto& io = ImGui::GetIO();
    // 指针放在画布空白处，避免点击音频预览或拖动资源。
    io.AddMousePosEvent(200.0F, 250.0F);
    // 输入必须先于 NewFrame 排队，与真实 GLFW 后端的事件时序一致。
    io.AddMouseWheelEvent(0.0F, wheel);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    ImGui::NewFrame();
    // 固定宿主几何，隔离系统 DPI、用户布局及动画时钟。
    ImGui::SetNextWindowPos({ 0.0F, 0.0F }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ 600.0F, height }, ImGuiCond_Always);
    ImGui::Begin("AudioToolScrollHost", nullptr, ImGuiWindowFlags_NoTitleBar);
    const auto dockId = ImGui::GetID("AudioToolScrollDock");
    // 每帧沿用同一个节点，不重建 Dock 树掩盖窗口迁移或滚动状态丢失。
    ImGui::DockSpace(dockId);
    ImGui::End();
    // 同一工具先浮动再停靠，覆盖运行中窗口身份迁移。
    ImGui::SetNextWindowDockID(docked ? dockId : 0, ImGuiCond_Always);
    if ( !docked ) {
        ImGui::SetNextWindowPos({ 0.0F, 0.0F }, ImGuiCond_Always);
        ImGui::SetNextWindowSize({ 600.0F, height }, ImGuiCond_Always);
    }
    // 测试显式选择工具标签，避免新建的辅助标签默认抢走初始焦点。
    // focusOther 分支故意不给工具焦点，以真实经过隐藏内容帧。
    if ( !focusOther ) view.requestFocus();
    view.update(&manager);
    // 真实标签切换会隐藏工具内容；重新激活后必须仍可滚动。
    if ( docked ) {
        ImGui::SetNextWindowDockID(dockId, ImGuiCond_Always);
        if ( focusOther ) ImGui::SetNextWindowFocus();
        ImGui::Begin("OtherAudioToolTab");
        ImGui::End();
    }
    // 即使无 GPU，也要结束帧并完成滚动范围及窗口悬停状态的发布。
    ImGui::Render();
}

/// @brief 构造超过视口的资源列，并保留横向溢出的独立资源。
/// @param project 测试项目，禁止借用用户当前项目。
/// @details 最后一张方块位于视口外，滚到末端才能查看；额外远端方块验证横轴。
void prepareResources(MMM::Project& project)
{
    // 清除上次运行的磁盘恢复状态，资源布局只由本次夹具决定。
    project.m_audioResources.clear();
    auto& placements =
        project.m_settings.m_workspace.m_projectAudioToolPlacements;
    // 避免同名旧 placement 合并后改变首帧资源位置和 Z 序。
    placements.clear();
    for ( int index = 0; index < 24; ++index ) {
        // ID 与布局记录一一对应；文件名外观无需真实 wav 文件支持。
        const auto id = "pf1_" + std::to_string(index);
        project.m_audioResources.push_back(MMM::AudioResource{
            .m_id = id, .m_type = MMM::AudioTrackType::Effect });
        // 固定宽高保证溢出来自位置，而不是字体或主题自动尺寸。
        placements.push_back(
            MMM::ProjectAudioToolItemPlacement{ .m_audioResourceId = id,
                                                .m_x               = 24.0F,
                                                .m_y = 24.0F + index * 110.0F,
                                                .m_width  = 140.0F,
                                                .m_height = 100.0F,
                                                .m_zOrder = index });
    }
    project.m_audioResources.push_back(MMM::AudioResource{
        .m_id = "horizontal", .m_type = MMM::AudioTrackType::Effect });
    // 高宿主可容纳整列，只留下横向溢出，Shift 滚轮可查看右侧资源。
    placements.push_back(
        MMM::ProjectAudioToolItemPlacement{ .m_audioResourceId = "horizontal",
                                            .m_x               = 1000.0F,
                                            .m_y               = 24.0F,
                                            .m_width           = 140.0F,
                                            .m_height          = 100.0F,
                                            .m_zOrder          = 24 });
}

/// @brief 验证浮动、停靠、后台标签恢复及横向溢出的实际滚动结果。
/// @return 所有场景均可查看视口外资源时返回 true。
bool checkScrolling(MMM::UI::UIManager&            manager,
                    MMM::UI::ProjectAudioToolView& view)
{
    // 多帧预热让 ImGui 完成首次内容测量和悬停窗口更新。
    for ( int frame = 0; frame < 4; ++frame ) drawFrame(manager, view, false);
    // 窗口由同一 ImGui context 持有，跨帧借用不涉及资源所有权转移。
    auto* canvas = findCanvas();
    if ( !canvas || !canvas->ScrollbarY || canvas->ScrollMax.y < 2000.0F )
        return false;
    // 下滚与随后无输入的一帧配对，检查延迟滚动目标确实被应用。
    // 仅查看当帧局部相机偏移会漏掉下一帧被 ImGui 夹回零的错误。
    drawFrame(manager, view, false, -1.0F);
    drawFrame(manager, view, false);
    // 浮动阶段先确认输入夹具有效，防止停靠失败实际源于未注入滚轮。
    if ( canvas->Scroll.y <= 0.0F ) return false;

    // 停靠后持续滚到末端，不能仅凭出现滚动条就判定资源可访问。
    for ( int frame = 0; frame < 4; ++frame ) drawFrame(manager, view, true);
    // 固定数量只表示测试输入次数，不构成运行时等待或交互消抖窗口。
    for ( int frame = 0; frame < 60; ++frame )
        drawFrame(manager, view, true, -1.0F);
    drawFrame(manager, view, true);
    // 使用原生 ScrollMax 校验终点，避免自算范围与生产错误恰好一致。
    // 最后一张资源之后保留操作留白，滚到终点才保证下方方块可访问。
    if ( !canvas->ScrollbarY ||
         std::abs(canvas->Scroll.y - canvas->ScrollMax.y) > 1.0F )
        return false;

    // 在另一个标签中停留，再恢复工具；布局变化不能清空滚动范围。
    for ( int frame = 0; frame < 4; ++frame )
        drawFrame(manager, view, true, 0.0F, true);
    for ( int frame = 0; frame < 4; ++frame ) drawFrame(manager, view, true);
    // 恢复后的上滚必须改变位置，防止测试只确认旧滚动值仍然存在。
    const float previousY = canvas->Scroll.y;
    // 正增量反向移动，覆盖到达边界后重新离开边界的状态转换。
    drawFrame(manager, view, true, 1.0F);
    drawFrame(manager, view, true);
    if ( canvas->Scroll.y >= previousY || canvas->ScrollMax.y < 2000.0F )
        return false;

    // 模拟停靠宿主比平台可见区域更高，画布仍必须按被裁剪后的高度滚动。
    // 旧实现会用宿主完整高度计算范围，把下方资源错误地视为已经可见。
    ImGui::GetIO().DisplaySize.y = 700.0F;
    for ( int frame = 0; frame < 4; ++frame )
        drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    // 此断言约束原生子窗口范围，不能只依赖绘制层的临时相机偏移。
    if ( canvas->ScrollMax.y < 2000.0F ) return false;
    // 先离开下边界，避免下滚没有位移被误判为滚动轴失效。
    // 实际滚动目标在后续帧落地，因此再提交一次无输入帧。
    drawFrame(manager, view, true, 1.0F, false, 3200.0F);
    drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    const float clippedY = canvas->Scroll.y;
    // 横轴必须保持不变，单纯观察纵轴有移动不足以发现双轴串扰。
    const float clippedX = canvas->Scroll.x;
    // 普通滚轮必须保持纵轴，不得因宿主尺寸或裁剪状态转向右侧资源。
    drawFrame(manager, view, true, -1.0F, false, 3200.0F);
    drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    if ( canvas->Scroll.y <= clippedY || canvas->Scroll.x != clippedX )
        return false;

    // 扩大平台区域，让整列资源真正可见；此时普通滚轮也不能偷偷换轴。
    // 横向溢出仍存在，只有 Shift 或水平滚轮允许移动横轴。
    ImGui::GetIO().DisplaySize.y = 3400.0F;
    for ( int frame = 0; frame < 4; ++frame )
        drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    if ( canvas->ScrollMax.y > 1.0F || !canvas->ScrollbarX ) return false;
    const float previousX = canvas->Scroll.x;
    drawFrame(manager, view, true, -1.0F, false, 3200.0F);
    drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    // 无纵向溢出意味着纵轮在边界停止，不能隐式重解释为横向请求。
    if ( canvas->Scroll.x != previousX ) return false;
    // 单独按 Shift 再滚轮，验证横向入口保留而默认方向没有歧义。
    // 修饰键先独立预热，隔离 ImGui 输入队列的跨帧事件合并策略。
    // 这样测试失败只表示横向请求未生效，而不是 Shift 尚未进入状态。
    drawFrame(manager, view, true, 0.0F, false, 3200.0F, true);
    drawFrame(manager, view, true, -1.0F, false, 3200.0F, true);
    drawFrame(manager, view, true, 0.0F, false, 3200.0F);
    return canvas->Scroll.x > previousX;
}
}  // namespace

/// @brief 在隔离配置和项目目录中运行真实 UI 回归。
/// @return 全部滚动行为通过时返回零。
int main()
{
    // 单例按生产依赖顺序创建，避免事件订阅者晚于事件总线析构。
    MMM::Logic::EditorEngine::instance();
    auto&      controller = MMM::Logic::ProjectController::instance();
    const auto root       = std::filesystem::current_path() /
                      "test_output/ProjectAudioToolScrollTest";
    std::error_code error;
    std::filesystem::create_directories(root, error);
    // 目录失败时不进入控制器；打开失败时不解引用当前项目指针。
    if ( error || !controller.openProject(root).m_opened ) return 1;
    // 开放控制器接口提供生产视图的数据源，不改私有状态或使用测试替身。
    prepareResources(*controller.currentProject());
    // CTest 已隔离 AppConfig 根；这里只修改本进程测试偏好。
    MMM::Config::AppConfig::instance().getEditorSettings().professionalMode =
        true;
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    // 大显示区域容纳最后的高宿主场景，确保画布仍处于可见剪裁区。
    io.DisplaySize = { 900.0F, 3400.0F };
    io.DeltaTime   = 1.0F / 60.0F;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    // 无平台后端也需要字体图集，以满足 NewFrame 的前置条件。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 失败结果仍经过正常析构和项目关闭，不能用提前退出绕过资源释放。
    bool passed = false;
    {
        // UI 对象先于 ImGui context 释放，保持 Clay 和窗口的生命周期配对。
        MMM::UI::UIManager            manager;
        MMM::UI::ProjectAudioToolView view("AudioToolScrollTest");
        passed = checkScrolling(manager, view);
    }
    // 先关闭生产项目数据源，再销毁 ImGui；控制器不保留工具视图的窗口指针。
    controller.closeProject();
    ImGui::DestroyContext();
    if ( !passed ) XERROR("Project audio tool docked scrolling regression");
    return passed ? 0 : 1;
}
