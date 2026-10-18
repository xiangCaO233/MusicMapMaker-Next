#include "ui/imgui/manager/ProjectAudioToolView.h"

#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "mmm/project/Project.h"
#include "ui/UIManager.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
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
/// @param pointer 指针位置，表格场景使用真实行坐标选择资源。
/// @param horizontalWheel 后端已转换的横轴滚轮增量，负值向右。
/// @note 横轴与纵轴分别注入，允许测试真实触控板事件而非只模拟传统鼠标。
/// @note Shift 状态仍独立进入队列，不能通过移除修饰键来绕过平台行为。
/// @note 本入口接收 ImGui 后端事件，不负责模拟操作系统的 Shift 轴转换。
/// @warning 测试帧入口：只推进 ImGui 内存状态，不等待真实时间。
void drawFrame(MMM::UI::UIManager& manager, MMM::UI::ProjectAudioToolView& view,
               bool docked, float wheel = 0.0F, bool focusOther = false,
               float height = 450.0F, bool shift = false,
               ImVec2 pointer         = ImVec2(200.0F, 250.0F),
               float  horizontalWheel = 0.0F)
{
    auto& io = ImGui::GetIO();
    // 指针放在画布空白处，避免点击音频预览或拖动资源。
    io.AddMousePosEvent(pointer.x, pointer.y);
    // 输入必须先于 NewFrame 排队，与真实 GLFW 后端的事件时序一致。
    io.AddMouseWheelEvent(horizontalWheel, wheel);
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
    // 活动弹层必须保留自身焦点，不能由测试夹具每帧强行聚焦背景窗口。
    if ( !focusOther && ImGui::GetCurrentContext()->OpenPopupStack.empty() )
        view.requestFocus();
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

/// @brief 通过真实鼠标操作验证音量松手、保存重建及移开指针均不关闭弹层。
/// @param manager 真实工具管理器，测试不调用视图私有状态。
/// @param view 与后续滚动测试共用的生产视图。
/// @return 只有明确点击外部才关闭时为真。
/// @details 使用实际 Popup 的窗口与激活状态断言，不能仅检查残留的打开栈。
/// 资源保存事件走真实订阅回调，覆盖缓存重建导致锚定 ID 被误清除的路径。
bool checkVolumePopup(MMM::UI::UIManager&            manager,
                      MMM::UI::ProjectAudioToolView& view)
{
    auto* project = MMM::Logic::ProjectController::instance().currentProject();
    if ( !project ) return false;
    project->m_settings.m_workspace.m_projectAudioToolPreviewEffectOnSelection =
        false;
    // 夹具第一张卡片固定为 140x100，位于逻辑坐标 24,24。
    for ( int frame = 0; frame < 4; ++frame ) drawFrame(manager, view, false);
    auto* canvas = findCanvas();
    if ( !canvas ) return false;
    const auto& style  = ImGui::GetStyle();
    const float button = ImGui::GetFrameHeight();
    const float spacing =
        std::max(1.0F, std::min(style.ItemInnerSpacing.x, 4.0F));
    // 固定四按钮居中排列，最后一个为音量按钮；不直接调用 OpenPopup。
    const ImVec2 buttonPoint{ canvas->DC.CursorStartPos.x + 24 +
                                  (140 - (button * 4 + spacing * 3)) * 0.5F +
                                  (button + spacing) * 3 + button * 0.5F,
                              canvas->DC.CursorStartPos.y + 24 + 100 -
                                  std::max(1.0F, style.FramePadding.y) -
                                  button * 0.5F };
    /// @brief 固定工具窗口几何，只推进指针位置及此前排队的输入事件。
    /// @param pointer 本帧鼠标屏幕坐标，弹层内外都通过真实窗口命中处理。
    /// @note 不直接设置私有锚定状态，确保缓存重建走完整生产交互路径。
    auto frameAt = [&](ImVec2 pointer) {
        drawFrame(manager, view, false, 0, false, 450, false, pointer);
    };
    frameAt(buttonPoint);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frameAt(buttonPoint);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frameAt(buttonPoint);
    auto& context = *ImGui::GetCurrentContext();
    if ( context.OpenPopupStack.empty() ||
         !context.OpenPopupStack.back().Window )
        return false;
    // 稳定 PopupId 表示同一次编辑上下文，不应关闭后自动重开一个新弹层。
    // 同时检查窗口 Active，避免把未被绘制的残留打开栈误判为可继续使用。
    const auto popupId = context.OpenPopupStack.back().PopupId;
    auto*      popup   = context.OpenPopupStack.back().Window;
    // 弹层先完成一帧自动测量，避免首帧尺寸影响滑块点击坐标。
    frameAt({ popup->Pos.x + 15, popup->Pos.y + 10 });
    if ( !popup->Active ) return false;
    // 输入区位于最后一行，点击七成位置可同时覆盖大于 100 的采样音量。
    // 不借用视图私有音量指针，确保测试经历真实控件编辑和提交路径。
    const ImVec2 sliderPoint{ popup->Pos.x + popup->Size.x * 0.7F,
                              popup->Pos.y + popup->Size.y -
                                  popup->WindowPadding.y - button * 0.5F };
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frameAt(sliderPoint);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frameAt(sliderPoint);
    // 释放后不再有任何按住的鼠标按钮，弹层持续显示不能依赖 active 滑块。
    // 保存事件是用户提交音量的正常后果，不代表用户要求结束编辑。
    // 松手提交音量后，额外触发保存通知，强制走方块缓存重建路径。
    // 移开鼠标不构成关闭意图，Popup 应仍每帧被真实提交。
    MMM::Event::EventBus::instance().publish(MMM::Event::ProjectSavedEvent{});
    // 指针移到画布空白区，排除仍悬浮在原方块附近而被动保持显示的可能。
    const ImVec2 outside{ 580, 400 };
    // 两个稳定帧覆盖事件消费与后续普通显示，均不进行真实时间等待。
    frameAt(outside);
    frameAt(outside);
    if ( context.OpenPopupStack.empty() ||
         context.OpenPopupStack.back().PopupId != popupId || !popup->Active )
        return false;
    // 只有在外部按下鼠标后才允许原生 Popup 规则关闭。
    // 按下和释放分别推进，避免同一帧两个边沿被 ImGui 输入队列合并。
    // 结束后继续复用工具实例，后面的滚动测试能检出遗留输入捕获。
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frameAt(outside);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frameAt(outside);
    return context.OpenPopupStack.empty() && !popup->Active;
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
    // 不同平台的后端事件不同：普通平台由 ImGui 处理 Shift，macOS 已由系统换轴。
    // 两种输入在每台测试机上都验证，避免仅在 Linux 上通过而漏掉 Apple
    // 默认行为。
    auto&      io                    = ImGui::GetIO();
    const bool originalMacOSBehavior = io.ConfigMacOSXBehaviors;
    bool       horizontalPassed      = true;
    for ( bool macOSBehavior : { false, true } ) {
        // 仅替换内存输入约定，不启动平台后端或修改用户持久配置。
        // 轮次末尾恢复宿主默认值，后续表格用例仍按原平台规则解释输入。
        io.ConfigMacOSXBehaviors = macOSBehavior;
        // 起点取真实滚动结果，不假设字体、主题与平台决定的每次步长相同。
        // 两个轮次顺序推进，第二轮不能借用第一轮产生的位移通过断言。
        const float before = canvas->Scroll.x;
        // Shift 先独立预热，保证修饰键已在滚轮到来前进入输入状态。
        // macOS 直接提供横向增量，不能再要求生产控件把同一事件换轴一次。
        drawFrame(manager, view, true, 0.0F, false, 3200.0F, true);
        drawFrame(manager,
                  view,
                  true,
                  macOSBehavior ? 0.0F : -1.0F,
                  false,
                  3200.0F,
                  true,
                  { 200.0F, 250.0F },
                  macOSBehavior ? -1.0F : 0.0F);
        // 后续无输入帧让原生滚动目标落地，断言真实子窗口位置而非请求值。
        drawFrame(manager, view, true, 0.0F, false, 3200.0F);
        if ( canvas->Scroll.x <= before ) {
            XERROR(
                "Project audio horizontal scroll failed: before={}, after={}, "
                "macOS={}",
                before,
                canvas->Scroll.x,
                macOSBehavior);
            horizontalPassed = false;
            break;
        }
    }
    // 失败分支也走这里，确保错误日志之后的正常析构不继承临时输入配置。
    // 只要求方向和非零位移，不将平台相关的滚动像素数硬编码为预期结果。
    io.ConfigMacOSXBehaviors = originalMacOSBehavior;
    return horizontalPassed;
}
/// @brief 验证真实表格替代方块绘制，选择资源不改写已有布局。
/// @return 模式切换、行选择和切回方块都保持工作区正确时返回 true。
/// @note 使用同一个工具实例和同一套项目资源，避免新建视图掩盖状态丢失。
/// @details 表格子窗口必须实际出现，不能只检查模式布尔值。
/// 方块窗口必须停止提交，证明切换式展示没有叠加两个交互区域。
/// 真实行点击验证共享画笔选择，而非直接修改工具私有状态。
/// 布局逐项比较包括资源身份和 Z 序，避免数量相同却顺序被改变。
bool checkTableView(MMM::UI::UIManager&            manager,
                    MMM::UI::ProjectAudioToolView& view)
{
    auto* project = MMM::Logic::ProjectController::instance().currentProject();
    if ( !project ) return false;
    auto& workspace = project->m_settings.m_workspace;
    // 测试不依赖音频文件；关闭选择自动试听以只验证表格操作契约。
    workspace.m_projectAudioToolPreviewEffectOnSelection = false;
    // 清空持久选择，不能让上次测试保存的值掩盖本次行点击没有生效。
    workspace.m_projectAudioToolSelectedResourceId.clear();
    const auto original = workspace.m_projectAudioToolPlacements;
    // 先保留布局，再切换；重建表格不能以新布局覆盖用来比较的基线。
    view.setTableView(true);
    for ( int frame = 0; frame < 3; ++frame ) drawFrame(manager, view, true);
    ImGuiWindow* table = nullptr;
    // 三帧覆盖首次创建、测量与稳定状态，不引入真实时间等待。
    for ( auto* window : ImGui::GetCurrentContext()->Windows ) {
        // 使用稳定内部 ID 找到表格滚动子窗口，不依赖窗口的本地化标题。
        if ( window->Active &&
             std::string_view(window->Name).find("ProjectAudioToolTable") !=
                 std::string_view::npos )
            table = window;
    }
    if ( !view.isTableView() || !table ||
         (findCanvas() && findCanvas()->Active) )
        return false;
    // 指针放在首行文件名列，按真实按下/松开时序激活公共 Selectable。
    // 使用实际表格几何而非固定坐标，避免顶部工具栏增加后点击到搜索框。
    // 先移动再按下，保持与 GLFW 后端逐帧投递输入的时序一致。
    const ImVec2 pointer(
        table->InnerRect.Min.x + 30.0F,
        table->InnerRect.Min.y + ImGui::GetFrameHeightWithSpacing() + 5.0F);
    drawFrame(manager, view, true, 0.0F, false, 450.0F, false, pointer);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame(manager, view, true, 0.0F, false, 450.0F, false, pointer);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame(manager, view, true, 0.0F, false, 450.0F, false, pointer);
    if ( workspace.m_projectAudioToolSelectedResourceId.empty() ) return false;
    // 选择只通过公共 UI 入口完成，工作区值证明提交已经发生。
    // 选择必须保存画笔资源，但不可像方块抓取那样提升 Z 序或挪动位置。
    const auto& current = workspace.m_projectAudioToolPlacements;
    if ( original.size() != current.size() ) return false;
    for ( std::size_t i = 0; i < current.size(); ++i ) {
        if ( original[i].m_audioResourceId != current[i].m_audioResourceId ||
             original[i].m_x != current[i].m_x ||
             original[i].m_y != current[i].m_y ||
             original[i].m_width != current[i].m_width ||
             original[i].m_height != current[i].m_height ||
             original[i].m_zOrder != current[i].m_zOrder )
            return false;
    }
    const auto selected = workspace.m_projectAudioToolSelectedResourceId;
    // 复用原实例切回，不能依靠析构重建恢复默认布局来通过测试。
    view.setTableView(false);
    for ( int frame = 0; frame < 3; ++frame ) drawFrame(manager, view, true);
    // 切回后方块恢复绘制，表格选中的资源仍然是当前音频画笔。
    return !view.isTableView() && findCanvas() && findCanvas()->Active &&
           workspace.m_projectAudioToolSelectedResourceId == selected;
}
}  // namespace

/// @brief 在隔离配置和项目目录中运行真实 UI 回归。
/// @return 全部滚动行为通过时返回零。
int main()
{
    // 单例按生产依赖顺序创建，避免事件订阅者晚于事件总线析构。
    MMM::Logic::EditorEngine::instance();
    auto&           controller = MMM::Logic::ProjectController::instance();
    const auto      root       = std::filesystem::current_path() /
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
        // 分阶段记录失败入口，平台差异不能只留下一个笼统的末尾错误。
        // 保留原有短路顺序：弹层失败时，不让后续滚动覆盖最先出现的输入异常。
        // 诊断只输出所属阶段，不跳过断言、不重试输入，也不降低通过条件。
        passed = checkVolumePopup(manager, view);
        if ( !passed ) XERROR("Project audio volume popup regression");
        if ( passed ) {
            passed = checkScrolling(manager, view);
            if ( !passed ) XERROR("Project audio scrolling regression");
        }
        if ( passed ) {
            passed = checkTableView(manager, view);
            if ( !passed ) XERROR("Project audio table view regression");
        }
    }
    // 先关闭生产项目数据源，再销毁 ImGui；控制器不保留工具视图的窗口指针。
    controller.closeProject();
    ImGui::DestroyContext();
    if ( !passed ) XERROR("Project audio tool docked scrolling regression");
    return passed ? 0 : 1;
}
