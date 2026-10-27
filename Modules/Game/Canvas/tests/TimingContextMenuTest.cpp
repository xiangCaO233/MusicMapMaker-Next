#include "canvas/TimelineCanvas.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "common/render/ScrollRenderData.h"
#include "common/render/TimelineInteractiveElement.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/VisualConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"
#include "ui/interaction/EditorInteraction.h"
#include <cmath>
#include <cstddef>
#include <entt/entity/entity.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <variant>
#include <vector>

namespace MMM::Canvas
{
/// @brief 测试仅绑定输入快照并读取即时 UI 状态，不伪造菜单动作。
/// @details 访问器只建立用户操作前置条件，后续状态变化由生产代码完成。
/// 不直接调用复制、粘贴、删除函数，能够发现菜单行没有正确绑定的回归。
struct TimingContextMenuTestAccess {
    /// @brief 调用正式交互路由；按钮、弹窗和菜单项均由真实 ImGui 帧处理。
    /// @param canvas 独立的 CPU 时间线实例，不关联逻辑会话。
    /// @param snapshot 调用方拥有且在全部菜单帧中保持存活的快照。
    /// @param origin 父窗口本帧提供的图像原点。
    /// @note profileAlreadyChecked 为真，模拟生产 update 已通过表面门禁。
    static void interact(TimelineCanvas&                 canvas,
                         Common::Render::RenderSnapshot& snapshot,
                         ImVec2                          origin)
    {
        // 不保存临时拷贝，菜单中的身份检查必须看到下一帧更新后的实例号。
        canvas.m_currentSnapshot = &snapshot;
        canvas.handleTimingCanvasInteraction(origin, { 500, 500 }, true, true);
    }
    /// @brief 给定已有多选前置条件，后续复制与删除必须自行维护该集合。
    /// @param canvas 仅在用例开始时建立多选，不在菜单动作之间反复重置。
    /// @note 与快照实体匹配；若菜单误替换多选，后续复制条数会失败。
    static void select(TimelineCanvas& canvas)
    {
        canvas.m_selectedTimingEntities = { static_cast<entt::entity>(42),
                                            static_cast<entt::entity>(43) };
    }
    /// @brief 观察正式本地选区，不能用命令数量代替清理和全选结果。
    /// @param canvas 当前菜单所属的时间线实例。
    /// @return 即时选择数量，无需等待逻辑线程发布新快照。
    static std::size_t count(const TimelineCanvas& canvas)
    {
        return canvas.m_selectedTimingEntities.size();
    }
    /// @brief 检查具体成员身份，避免数量相同但替换错对象的假通过。
    /// @param canvas 由真实鼠标动作维护选择的时间线。
    /// @param id 仅用于固定快照实体的句柄比较，不访问 Registry。
    /// @return 同一选区中存在指定实体时返回真。
    static bool contains(const TimelineCanvas& canvas, unsigned id)
    {
        return canvas.m_selectedTimingEntities.contains(
            static_cast<entt::entity>(id));
    }
    /// @brief 核对右键起点的吸附时间，后续移动到菜单不能改变该值。
    /// @param canvas 刚完成右键打开流程的时间线实例。
    /// @return 与画布时间轴一致的秒时间，不是播放头位置。
    static double anchor(const TimelineCanvas& canvas)
    {
        return canvas.m_timingContextTime;
    }
};
}  // namespace MMM::Canvas

namespace
{
using namespace MMM;
using Access = Canvas::TimingContextMenuTestAccess;

/// @brief 驱动独立 ImGui 窗口，不创建 GPU、音频设备或谱面会话。
/// @param canvas 本轮菜单所属的正式时间线控件。
/// @param snapshot 在输入帧之间可变更的逻辑只读快照。
/// @param mouse 屏幕坐标，菜单展开后与原右键坐标故意不同。
/// @param left 当前左键状态，不直接伪造 clicked 或 released 标记。
/// @param right 当前右键状态，用于打开菜单或验证画笔擦除边界。
/// @return 固定窗口的内容原点，避免依赖用户的布局和 DPI。
ImVec2 frame(Canvas::TimelineCanvas&         canvas,
             Common::Render::RenderSnapshot& snapshot, ImVec2 mouse,
             bool left = false, bool right = false)
{
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, left);
    io.AddMouseButtonEvent(1, right);
    ImGui::NewFrame();
    // 禁用持久布局后，每次运行都从相同几何和相同弹窗相对位置开始。
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ 600, 600 });
    // 弹窗拥有焦点时不强制聚焦父窗口，否则会掩盖菜单点击所有权错误。
    ImGui::Begin("TimingMenuTest", nullptr, ImGuiWindowFlags_NoSavedSettings);
    const auto origin = ImGui::GetCursorScreenPos();
    // 即便弹窗拥有焦点仍要推进控制器，让菜单关闭帧执行输入防穿透逻辑。
    Access::interact(canvas, snapshot, origin);
    ImGui::End();
    ImGui::EndFrame();
    return origin;
}

/// @brief 右键完整按下并释放，等第二帧布局稳定后才能点击菜单项。
/// @param canvas 不重建视图，保留上次弹窗关闭状态和选区。
/// @param snapshot 同一谱面的时间点几何与编辑工具。
/// @param point 画布中用于吸附的右键起点。
/// @return 当前弹窗窗口；测试只观察真实布局，不调用菜单业务函数。
ImGuiWindow* open(Canvas::TimelineCanvas&         canvas,
                  Common::Render::RenderSnapshot& snapshot, ImVec2 point)
{
    frame(canvas, snapshot, point);
    // 第一帧确保上一笔释放，随后才产生独立的右键按下边沿。
    frame(canvas, snapshot, point, false, true);
    frame(canvas, snapshot, point);
    const auto& stack = ImGui::GetCurrentContext()->OpenPopupStack;
    // 弹窗不存在就显式失败，不能回退到父窗口坐标继续点击。
    return stack.empty() ? nullptr : stack.back().Window;
}

/// @brief 按实际弹窗位置点击菜单项，覆盖菜单激活而非私有业务辅助函数。
/// @param canvas 接收菜单项激活的时间线控件。
/// @param snapshot 所有点击共用的只读输入快照。
/// @param point 新一轮菜单打开时的画布吸附坐标。
/// @param row 菜单中不含分隔线的零起始行号。
/// @note 四条分隔线分别位于第 2、第 6、第 8 和第 11 行之前。
/// @return 真实菜单行被激活并关闭后返回真。
/// @details 不按翻译文本搜索行，避免测试依赖桌面语言或字体配置。
/// @warning 仅测试读取 ImGui 内部窗口几何，生产代码不依赖该结构。
bool click(Canvas::TimelineCanvas&         canvas,
           Common::Render::RenderSnapshot& snapshot, ImVec2 point, int row)
{
    const auto* popup = open(canvas, snapshot, point);
    if ( !popup ) return false;
    // Selectable 行含 ItemSpacing；分隔线还增加一个像素高度。
    const auto&  style      = ImGui::GetStyle();
    const float  line       = ImGui::GetTextLineHeightWithSpacing();
    const int    separators = (row >= 2 ? 1 : 0) + (row >= 6 ? 1 : 0) +
                              (row >= 8 ? 1 : 0) + (row >= 11 ? 1 : 0);
    const ImVec2 target{ popup->Pos.x + popup->Size.x * 0.5F,
                         popup->DC.CursorStartPos.y + row * line +
                             separators * (1.0F + style.ItemSpacing.y) +
                             ImGui::GetTextLineHeight() * 0.5F };
    frame(canvas, snapshot, target, true);
    // ImGui 菜单在释放时激活；仅移动或按下都不能作为完成依据。
    frame(canvas, snapshot, target);
    const bool closed = ImGui::GetCurrentContext()->OpenPopupStack.empty();
    // 关闭帧仍属于菜单；下一次独立按下前要让输入状态正常收尾。
    frame(canvas, snapshot, point);
    return closed;
}

/// @brief 验证 Timing 菜单的独立选区、吸附粘贴和会话身份检查。
/// @details 第一时间点故意远离播放头；复制包含 BPM，粘贴应保留相对秒差。
/// @details 所有副作用仅观察事件总线，不创建真实音符或写入用户文件。
/// @details 系统剪贴板读取由上下文级空回调替换，不接触用户内容。
/// @return 菜单业务、吸附位置和旧方案隔离都满足契约时返回真。
/// @pre 测试独占 ImGui 上下文和事件总线监听生命周期。
/// @post 所有订阅在局部变量析构之前移除，避免引用悬空。
/// @note 菜单激活使用鼠标释放边沿，不发送任何键盘快捷键模拟编辑。
/// @note 每一轮都保留相同视图，能够检出上次菜单未完成收尾的状态残留。
/// @note 快照显式给出效果位，不依赖解析真实谱面或生成 marker 的工作线程。
/// @note 所有实体句柄仅作值比较，测试不要求 Registry 中实际创建对应对象。
/// @note 只有两个固定时间点，断言同时覆盖条目数量、时间与效果类型。
/// @note 故意使用同路径新实例，区分会话代数验证和文件名验证。
bool checkMenu()
{
    UI::Interaction::synchronizeProfile("mouse-only", true);
    UI::Interaction::synchronizeProfile("mouse-only", true);
    Common::Render::RenderSnapshot snapshot;
    snapshot.hasBeatmap        = true;
    snapshot.currentTool       = Logic::EditTool::Marquee;
    snapshot.currentTime       = 1.0;
    snapshot.trackCount        = 4;
    snapshot.beatmapPathKey    = "menu.mmm";
    snapshot.beatmapInstanceId = 1;
    // marker 的可见 Y 与逻辑秒时间分别固定，让吸附无需依赖窗口缩放。
    // 第一项使用 BPM，使原粘贴路径保留相对秒差，避免拍位配置干扰锚点断言。
    Common::Render::TimelineInteractiveElement bpm{};
    bpm.time                  = 3.25;
    bpm.y                     = 150;
    bpm.effects               = Common::Render::SCROLL_EFFECT_BPM;
    bpm.bpmEntity             = static_cast<entt::entity>(42);
    bpm.bpmValue              = 120;
    auto scroll               = bpm;
    scroll.time               = 3.75;
    scroll.y                  = 200;
    scroll.effects            = Common::Render::SCROLL_EFFECT_SCROLL;
    scroll.bpmEntity          = entt::null;
    scroll.scrollEntity       = static_cast<entt::entity>(43);
    scroll.scrollValue        = 1.5;
    snapshot.timelineElements = { bpm, scroll };
    // 画布只绑定快照，事件总线仅观察，不应用命令或触发真实音频设备。
    Canvas::TimelineCanvas canvas("TimingMenuCanvas", 500, 500, nullptr);
    const auto             origin = frame(canvas, snapshot, { 200, 150 });
    const ImVec2           point{ origin.x + 200, origin.y + 150 };
    frame(canvas, snapshot, point);
    Access::select(canvas);
    // 选择只初始化一次；之后全选、清空、删除都必须通过鼠标菜单完成。
    bool        passed  = true;
    std::size_t deleted = 0, undo = 0, redo = 0, noteEdits = 0;
    std::vector<Logic::CmdCreateTimelineEvents::Entry> created;
    std::vector<double>                                seeks;
    std::string                                        navigationOrder;
    auto&      bus = Event::EventBus::instance();
    const auto subscription =
        bus.subscribe<Event::LogicCommandEvent>([&](const auto& event) {
            // 订阅生产路由，能够发现菜单误发主画布音符动作和重复批量命令。
            if ( const auto* batch =
                     std::get_if<Logic::CmdCreateTimelineEvents>(
                         &event.command) )
                created = batch->events;
            if ( const auto* batch =
                     std::get_if<Logic::CmdDeleteTimelineEvents>(
                         &event.command) )
                deleted += batch->entities.size();
            undo += std::holds_alternative<Logic::CmdUndo>(event.command);
            redo += std::holds_alternative<Logic::CmdRedo>(event.command);
            if ( const auto* command =
                     std::get_if<Logic::CmdSeek>(&event.command) ) {
                seeks.push_back(command->time);
                navigationOrder += 's';
            }
            if ( const auto* command =
                     std::get_if<Logic::CmdSetPlayState>(&event.command) )
                navigationOrder += command->isPlaying ? 'p' : 'x';
            noteEdits +=
                std::holds_alternative<Logic::CmdSelectEntity>(event.command) ||
                std::holds_alternative<Logic::CmdDeleteSelected>(
                    event.command) ||
                std::holds_alternative<Logic::CmdPaste>(event.command);
        });
    // 复制不删除选区；已有多选不会被右键所在的单个 marker 替换。
    // 菜单行点击位置与画布 Y 不同，能够检出粘贴使用实时鼠标位置的错误。
    passed &= click(canvas, snapshot, point, 2);
    passed &= Access::count(canvas) == 2;
    passed &= std::abs(Access::anchor(canvas) - 3.25) < 1e-9;
    passed &= click(canvas, snapshot, point, 4);
    passed &= created.size() == 2;
    // 先校验数量再索引，失败时报告测试失败而不是因越界终止进程。
    if ( created.size() == 2 ) {
        passed &= std::abs(created[0].time - 3.25) < 1e-9;
        passed &= std::abs(created[1].time - 3.75) < 1e-9;
        passed &= created[0].type == TimingEffect::BPM &&
                  created[1].type == TimingEffect::SCROLL;
    }
    // 全选、清空与删除分别检查本地状态，不能只判断弹窗消失。
    // 全选遵循 BPM 可选配置，测试入口显式开启该开关并在退出时恢复。
    passed &= click(canvas, snapshot, point, 6);
    passed &= Access::count(canvas) == 2;
    passed &= click(canvas, snapshot, point, 7);
    passed &= Access::count(canvas) == 0;
    passed &= click(canvas, snapshot, point, 6);
    passed &= click(canvas, snapshot, point, 5);
    passed &= deleted == 2 && Access::count(canvas) == 0;
    // 历史菜单直接观察语义命令，防止错误转入独立音符或 BPM 工具动作。
    passed &= click(canvas, snapshot, point, 0);
    passed &= click(canvas, snapshot, point, 1);
    passed &= undo == 1 && redo == 1 && noteEdits == 0;
    // 剪切需要走与复制相同的内部剪贴板，同时一次删除整个多选集合。
    // 先由菜单全选恢复集合，避免直接访问器重置掩盖菜单全选失效。
    passed &= click(canvas, snapshot, point, 6);
    passed &= click(canvas, snapshot, point, 3);
    passed &= deleted == 4 && Access::count(canvas) == 0;
    // 时间线锚点位于 3.25 秒，不能误用 1 秒播放头或菜单中的实时坐标。
    // 导航单独计数，不把之前的复制、剪切和撤销命令混入播放顺序。
    // 观察的是正式 CmdSeek 的播放时间，不能只检查 UI 本地保存的锚点。
    // 测试不应用命令，快照仍为暂停，不模拟音频线程的异步状态回流。
    // 单独定位保持暂停，从此播放应严格先发 Seek，再发开启播放命令。
    // 每次新开菜单都重新采集落点，两轮顺序字符串必须精确为 s、s、p。
    // 偏移包含用户与平台固定分量，换算口径与主画布导航一致。
    // 所有捕获的 Seek 都要核对时间，不能只检查最后一次结果。
    // 导航不负责改变选择，后续多选断言继续使用剪切后的本地状态。
    passed &= click(canvas, snapshot, point, 11);
    passed &= seeks.size() == 1 && navigationOrder == "s";
    passed &= click(canvas, snapshot, point, 12);
    passed &= seeks.size() == 2 && navigationOrder == "ssp";
    const auto offset = Config::AppConfig::instance()
                            .getVisualConfig()
                            .getEffectiveVisualOffset();
    for ( const auto time : seeks )
        passed &= std::abs(time - (3.25 - offset)) < 1e-9;
    // 三种模式都从真实菜单选择，随后只用鼠标点选和框选，不伪造 Ctrl。
    // 模式是应用级状态，菜单退出后主画布也应读到同一个值。
    using Operation = Logic::SelectionOperation;
    const ImVec2 first{ origin.x + 50, origin.y + 150 };
    const ImVec2 second{ origin.x + 150, origin.y + 200 };
    auto         tap = [&](ImVec2 at) {
        // 按下和释放都经过正式控制器，不直接调用选择辅助函数。
        // 释放后才能再次打开菜单，避免把一笔手势拆成多次操作。
        frame(canvas, snapshot, at, true);
        frame(canvas, snapshot, at);
    };
    passed &= click(canvas, snapshot, point, 8);
    passed &= UI::Interaction::activeProfile().selectionOperation() ==
              Operation::Replace;
    // 默认替换先建立单项集合，后续追加必须保留这一项。
    tap(first);
    passed &= Access::count(canvas) == 1 && Access::contains(canvas, 42);
    passed &= click(canvas, snapshot, point, 9);
    tap(second);
    tap(second);
    // 重复追加已选项不能切换移除；这是与经典 Ctrl 点击不同的关键契约。
    passed &= Access::count(canvas) == 2;
    passed &= click(canvas, snapshot, point, 10);
    tap(first);
    tap(first);
    passed &= Access::count(canvas) == 1 && Access::contains(canvas, 43);
    // 在非 marker 位置开始框选；几何覆盖两个泳道，但基线只有 Scroll。
    // 减选同一目标两次仍是差集，不能恢复第二次命中的目标。
    // 减选框扩大时移除命中项，缩回时必须从起笔基线恢复该项。
    const ImVec2 corner{ origin.x + 5, origin.y + 120 };
    const ImVec2 all{ origin.x + 180, origin.y + 230 };
    const ImVec2 narrow{ origin.x + 80, origin.y + 175 };
    // 起点与全部终点都远离自动滚动边缘，选区差异只由集合语义决定。
    frame(canvas, snapshot, corner, true);
    frame(canvas, snapshot, all, true);
    passed &= Access::count(canvas) == 0;
    // 直接改应用模式模拟外部设置，当前笔仍必须使用锁存的减选操作。
    UI::Interaction::activeProfile().setSelectionOperation(Operation::Add);
    // 当前活动手势不通过菜单切换；这次设置故意模拟外部应用状态变更。
    frame(canvas, snapshot, narrow, true);
    frame(canvas, snapshot, narrow);
    passed &= Access::count(canvas) == 1 && Access::contains(canvas, 43);
    // 下一笔才采用追加，先框选 BPM，随后多次框选同一区域仍是幂等并集。
    for ( int i = 0; i < 2; ++i ) {
        // 第一笔追加 BPM，第二笔命中相同目标；成员数量不应反向减少。
        frame(canvas, snapshot, corner, true);
        frame(canvas, snapshot, narrow, true);
        frame(canvas, snapshot, narrow);
        passed &= Access::count(canvas) == 2;
    }
    passed &= click(canvas, snapshot, point, 8);
    frame(canvas, snapshot, corner, true);
    frame(canvas, snapshot, narrow, true);
    frame(canvas, snapshot, narrow);
    passed &= Access::count(canvas) == 1 && Access::contains(canvas, 42);
    // 抓取工具的追加保留组；减选不能启动拖动或发布移动命令。
    // 上一笔替换框不保留多选基线，框外 Scroll 应已从集合中移除。
    snapshot.currentTool = Logic::EditTool::Move;
    // 使用固定 marker 时间零位移抓取，不依赖异步更新快照完成状态收尾。
    passed &= click(canvas, snapshot, point, 9);
    tap(second);
    passed &= Access::count(canvas) == 2;
    passed &= click(canvas, snapshot, point, 10);
    tap(first);
    passed &= Access::count(canvas) == 1 && Access::contains(canvas, 43);
    snapshot.currentTool = Logic::EditTool::Marquee;
    // 还原工具后再检查旧菜单失效，防止工具变化本身提前关闭菜单造成假通过。
    // 同路径重载后的新实例不能接收旧菜单，关闭时也不得穿透出编辑命令。
    // 在未产生任何菜单项点击时切换代数，生产校验应主动关闭旧菜单。
    passed &= open(canvas, snapshot, point) != nullptr;
    ++snapshot.beatmapInstanceId;
    // 指针不点击任何菜单行，仍需通过新快照更新检测会话变更。
    frame(canvas, snapshot, point);
    passed &= ImGui::GetCurrentContext()->OpenPopupStack.empty();
    // 失效菜单关闭不得提交剪切、粘贴或撤销，现有计数应原样保留。
    passed &= deleted == 4 && undo == 1 && redo == 1 && noteEdits == 0;
    frame(canvas, snapshot, point);
    // 经典桌面方案与画笔擦除均不提供本菜单，保持原工具职责。
    // 使用同一个 canvas 保留菜单历史，能够发现关闭帧标记遗留造成的误接管。
    // 方案同步采用正常两次安全帧流程，不直接修改活动策略指针。
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    passed &= open(canvas, snapshot, point) == nullptr;
    // 经典无修饰点击仍然替换，Ctrl 点击已选项仍然切出，不读取纯鼠标模式。
    tap(first);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    // 经典分支读取真实物理修饰键，不能被上次的纯鼠标减选模式覆盖。
    tap(second);
    passed &= Access::count(canvas) == 2;
    tap(first);
    passed &= Access::count(canvas) == 1 && Access::contains(canvas, 43);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    // 在切回纯鼠标方案前释放修饰键，确保最后的画笔用例独立于本断言。
    frame(canvas, snapshot, point);
    UI::Interaction::synchronizeProfile("mouse-only", true);
    UI::Interaction::synchronizeProfile("mouse-only", true);
    snapshot.currentTool = Logic::EditTool::Draw;
    // Draw 的右键仍由擦除状态机处理，本用例只断言没有误开选择菜单。
    passed &= open(canvas, snapshot, point) == nullptr;
    bus.unsubscribe<Event::LogicCommandEvent>(subscription);
    // 订阅闭包引用局部计数；必须先解除，再让后续上下文析构或执行其他用例。
    if ( !passed )
        XERROR(
            "Timing context menu selection, clipboard anchor or ownership "
            "failed");
    return passed;
}
}  // namespace

/// @brief 使用 CPU ImGui 帧验证鼠标菜单，不初始化 Vulkan 或操作系统窗口。
/// @return 所有业务和输入所有权断言通过时返回零。
/// @note 临时专业模式和系统剪贴板回调都只影响独立测试进程。
/// @note 时间步长只驱动控件状态，不引入 sleep 或任何阻塞同步。
/// @pre 测试在独立进程中运行，不共用应用的窗口或字体图集。
/// @post 配置完整恢复；临时剪贴板回调随本上下文销毁。
int main()
{
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    const auto original       = settings;
    settings.professionalMode = true;
    // 这些配置只影响当前测试进程，不能调用保存入口写入个人设置。
    settings.timelineSelectionIncludesBpm = true;
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 800, 700 };
    io.DeltaTime   = 1.0F / 60.0F;
    // 本用例只测试单击集合操作，重复点击用于验证追加和减选的幂等性。
    // 禁用双击识别，避免短帧间隔让单击用例意外转入编辑弹窗。
    io.MouseDoubleClickTime         = 0.0F;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels           = nullptr;
    int            width = 0, height = 0;
    // CPU 控件布局需要字体度量，像素数据不上传纹理，也不需要显示器。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 避免读取桌面剪贴板，保留复制动作写入的编辑器内部 Timeline 数据。
    ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext*) {
        return "";
    };
    const bool passed = checkMenu();
    ImGui::DestroyContext();
    // 配置恢复放在上下文销毁之后，确保测试失败时也不会污染后续设置断言。
    settings = original;
    return passed ? 0 : 1;
}
