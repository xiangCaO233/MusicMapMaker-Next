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
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/interaction/EditorInteraction.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <entt/entity/entity.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
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
    /// @brief 只观察真实手势、弹窗草稿与投影，不直接调用业务创建或取消。
    /// @details 范围结果按值返回，测试不持有 UI 私有结构的跨帧引用。
    static bool dragging(const TimelineCanvas& c)
    {
        return c.m_isInterpolationDragging;
    }
    /// @brief 普通点装饰必须在按下时出现，不能等到释放才反推用户意图。
    /// @param c 只读本地手势状态，不复制快照或请求新一轮 UI 更新。
    /// @return 升级为段落、取消或释放之后必须为假。
    static bool pointPreview(const TimelineCanvas& c)
    {
        return c.m_isTimingDrawPreviewing;
    }
    /// @brief 单点草稿按起笔类型定位，横跨泳道不能改变本笔的参数种类。
    /// @return 创建类型的原有索引，不从鼠标释放位置重新计算。
    static int pointType(const TimelineCanvas& c) { return c.m_createType; }
    /// @brief 保留鼠标原始时间来源，不把轻微抖动后的释放时间当作点按起点。
    /// @return 单位为秒，保留普通创建弹窗中的原始位置来源。
    static double pointTime(const TimelineCanvas& c)
    {
        return c.m_createTimeRaw;
    }
    /// @brief 按值观察插值工作副本的类型和实体，区分新段与已有段编辑。
    /// @return 实际效果类型，不能仅靠弹窗开启状态推断泳道正确。
    static TimingEffect rangeType(const TimelineCanvas& c)
    {
        return c.m_interpolationEdit.effect;
    }
    /// @brief 编辑已有段落必须保留其稳定实体身份。
    /// @return 原实体句柄，新建与编辑混淆时此断言必须失败。
    static entt::entity editEntity(const TimelineCanvas& c)
    {
        return c.m_interpolationEdit.entity;
    }
    /// @brief 请求弹窗也属于准备完成，不要求在测试里渲染编辑器正文。
    static bool editor(const TimelineCanvas& c)
    {
        return c.m_isInterpolationEditorOpen || c.m_requestInterpolationEditor;
    }
    /// @brief 返回松手后工作副本的区间长度，单位秒。
    static double duration(const TimelineCanvas& c)
    {
        return c.m_interpolationEdit.interpolation.m_duration;
    }
    /// @brief 使用生产投影，隔离用户缩放与 DPI 对区间断言的影响。
    static double time(const TimelineCanvas& c, float y)
    {
        return c.canvasTimeAtLocalY({ 500, 500 }, y);
    }
    /// @brief 取消后不得意外进入普通创建弹窗；只读标志，不替测试清理状态。
    static bool pointEditor(const TimelineCanvas& c)
    {
        return c.m_isCreatePopupOpen;
    }
    /// @brief 检查 STOP 普通点弹窗，避免插值模式吞掉第五泳道点击。
    /// @return 只允许普通创建弹窗且创建类型仍为 STOP 时返回真。
    static bool stopPoint(const TimelineCanvas& c)
    {
        return c.m_isCreatePopupOpen && c.m_createType == 4;
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
/// @note 分隔线分别位于第 2、6、8、11 和 13 行之前，索引不含分隔线。
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
                              (row >= 8 ? 1 : 0) + (row >= 11 ? 1 : 0) +
                              (row >= 13 ? 1 : 0);
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
    // Timing 点拖自动识别，不再通过右键菜单切换绘制偏好。
    passed &= UI::Interaction::activeProfile().automaticTimingDraw();
    // 吸附开关来自菜单实际激活，而不是下面范围用例的准备 setter。
    // 再次打开菜单时必须使用新的落点策略，原菜单锚点仍保持已锁存的值。
    passed &= click(canvas, snapshot, point, 13);
    passed &= UI::Interaction::activeProfile().bypassSnapping();
    // 打开位置恰好命中 marker，但临时免吸附应采用真实投影而非 3.25 秒。
    passed &= open(canvas, snapshot, point) != nullptr;
    passed &=
        std::abs(Access::anchor(canvas) - Access::time(canvas, 150)) < 1e-9;
    frame(canvas, snapshot, { 700, 600 }, true);
    frame(canvas, snapshot, { 700, 600 });
    UI::Interaction::activeProfile().setBypassSnapping(false);
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
/// @brief 验证纯鼠标 Timing 点拖自动分流、取消及旧段编辑，保留经典 Shift 门禁。
/// @details 每个场景使用新视图，避免尚未渲染的编辑弹窗影响下一场景。
/// @return 无键盘即可画段，取消不创建单点，经典不改变时返回真。
/// @pre 使用 CPU ImGui 上下文和独立空谱面，不启动图形或音频后端。
/// @note 参数草稿是输出边界，未确认时不得产生谱面创建命令。
/// @note 每个鼠标边沿都经生产路由，不直接设置拖动标志或编辑状态。
/// @note STOP 没有连续区间定义，必须始终走原有普通时间点创建流程。
bool checkRangeDrawing()
{
    // 插值编辑器需要复制活动会话的冲突检查数据，快照本身不能替代真实会话。
    // 只创建内存空谱面，不启动引擎循环或音频播放，关闭时禁止写项目工作区。
    auto&      engine  = Logic::EditorEngine::instance();
    const auto session = engine.createSession(std::make_shared<BeatMap>(),
                                              "Timing range test",
                                              false,
                                              "TimingRangeTest",
                                              true);
    // 会话只提供冲突校验所需的空 Registry，输入仍由独立 RenderSnapshot 驱动。
    // 使用固定相机身份和恢复入口，避免把测试画布写入用户工作区。
    Common::Render::RenderSnapshot snapshot;
    snapshot.hasBeatmap  = true;
    snapshot.currentTool = Logic::EditTool::Draw;
    // 选择未来仍为正的显示区间，避免起点非负钳制使反向拖动断言退化。
    // marker 集合保持为空，使范围只由画布投影决定，不受已有时间点拾取干扰。
    snapshot.currentTime       = 4.0;
    snapshot.beatmapPathKey    = "range.mmm";
    snapshot.beatmapInstanceId = 1;
    // 本用例独立记录失败阶段，避免某个门禁未触发时被后面的结果覆盖。
    // 日志不读取 ECS 或控制器私有状态，所有断言仍观察正式手势产生的结果。
    bool passed = true;
    /// @brief 记录各阶段失败，不以最后一次成功覆盖先前错误。
    /// @param value 已经完成真实输入路由后的断言结果。
    /// @param stage 区分起笔、升级、取消与已有段编辑的诊断身份。
    auto expect = [&](bool value, const char* stage) {
        if ( !value ) XERROR("Timing range stage failed: {}", stage);
        passed &= value;
    };
    // 订阅不消费事件，仍可检测其他分支意外提前提交的创建命令。
    // 观察闭包在会话关闭前解除，避免局部计数被后续单例生命周期访问。
    int   created = 0;
    auto& bus     = Event::EventBus::instance();
    // 菜单和拖动不应直接创建时间点，只有后续编辑器确认才允许提交段落。
    const auto subscription =
        bus.subscribe<Event::LogicCommandEvent>([&](const auto& e) {
            created += std::holds_alternative<Logic::CmdCreateTimelineEvents>(
                e.command);
        });
    // 临时免吸附让 expected 能直接由相同投影计算，不预设拍线网格相位。
    // 帧辅助函数只注入左右键，证明无键盘参与也能进入持续拖动状态。
    UI::Interaction::activeProfile().setBypassSnapping(true);
    // 四种可插值泳道共享同一自动手势，不给某种效果另设点或段模式。
    const std::array effects{ TimingEffect::BPM,
                              TimingEffect::SCROLL,
                              TimingEffect::JUMP,
                              TimingEffect::HS };
    for ( int lane = 0; lane < 4; ++lane ) {
        // 点按、抖动、横移、释放帧拖动、正向拖动和反向拖动都从真实按下开始。
        // 场景之间使用新视图，未确认的参数草稿不能阻塞下一个独立用户动作。
        for ( int scenario = 0; scenario < 6; ++scenario ) {
            // 新画布只隔离未确认草稿，指针输入仍共用同一个真实窗口。
            // 唯一名称同时隔离守卫身份，不能把旧事务误算为本轮成功。
            Canvas::TimelineCanvas canvas(
                "AutoTiming" + std::to_string(lane * 6 + scenario),
                500,
                500,
                nullptr);
            const auto   origin = frame(canvas, snapshot, { 100, 100 });
            const ImVec2 first{ origin.x + (lane + 0.5F) * 100,
                                origin.y + 200 };
            // 起终点重合模拟点按，微小二维抖动不能绕过用户拖动阈值。
            ImVec2 last = first;
            if ( scenario == 1 ) {
                last.x += 1;
                last.y += 1;
            }
            // 纯横移虽越过阈值，却没有时间范围，仍应创建起笔泳道的点。
            if ( scenario == 2 ) last.x = origin.x + (lane == 3 ? 50 : 450);
            // 有效范围都横跨其它泳道，类型必须来自起笔而非释放帧。
            // 第三场景只在释放时移动，其余两种分别覆盖正向和反向时间。
            if ( scenario >= 3 ) {
                last.y = origin.y + (scenario == 5 ? 350 : 70);
                last.x = origin.x + (lane == 3 ? 50 : 450);
            }
            // Shift
            // 故意在部分单点和段落中保持按住，纯鼠标必须始终按移动量区分。
            // 原始时间与类型从起笔独立记录，断言不能照抄最后鼠标位置。
            ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, lane % 2 != 0);
            frame(canvas, snapshot, first, true);
            expect(Access::dragging(canvas) && Access::pointPreview(canvas),
                   "point preview at press");
            // 期望时间取原始投影，不从被测工作副本复制起点作为答案。
            const double startTime = Access::time(canvas, 200);
            if ( scenario != 3 ) frame(canvas, snapshot, last, true);
            frame(canvas, snapshot, last);
            if ( scenario < 3 ) {
                // 未越阈值及纯横移都没有有效时间范围，必须回到同一普通点创建流程。
                // 即使横移跨过多条泳道，也保留按下时的类型和时间。
                expect(
                    Access::pointEditor(canvas) && !Access::editor(canvas) &&
                        Access::pointType(canvas) == lane &&
                        std::abs(Access::pointTime(canvas) - startTime) < 1e-9,
                    "click or degenerate drag creates point");
            } else {
                // 时间跨度与横向轨迹无关，反向拖动仅改变起终点的顺序。
                const double expected = std::abs(
                    startTime - Access::time(canvas, scenario == 5 ? 350 : 70));
                expect(Access::editor(canvas) && !Access::pointEditor(canvas) &&
                           Access::rangeType(canvas) == effects[lane] &&
                           std::abs(Access::duration(canvas) - expected) < 1e-9,
                       "drag creates latched range");
            }
            // 测试只检视草稿，不渲染参数弹窗；清理全局非模态请求后销毁所属视图。
            // 真实手势与草稿结果已断言完毕，清理不能代替生产取消或改变被测结果。
            // 请求尚未 BeginPopup，Window
            // 为空，必须显式清理该测试草稿的请求层级。
            // 这些弹窗不属于真实用户环境，断言之后清理不会改变被测取消结果。
            if ( !ImGui::GetCurrentContext()->OpenPopupStack.empty() )
                ImGui::ClosePopupToLevel(0, true);
            // 消费真实修饰键释放，下一轮不能依赖上一轮按键缓存。
            ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
            frame(canvas, snapshot, last);
        }
    }
    // 取消既覆盖未拖动的点候选，也覆盖已经升级的有效插值范围。
    // 两类取消都必须消费后续释放，不能偷偷打开普通点窗口或删除旧实体。
    for ( int scenario = 0; scenario < 4; ++scenario ) {
        // 取消覆盖相同的非退化区间，不能因区间本来为空而碰巧没有提交。
        Canvas::TimelineCanvas canvas(
            "CancelAutoTiming" + std::to_string(scenario), 500, 500, nullptr);
        const auto   origin = frame(canvas, snapshot, { 100, 100 });
        const ImVec2 first{ origin.x + 150, origin.y + 200 },
            last{ origin.x + 150, origin.y + 350 };
        frame(canvas, snapshot, first, true);
        if ( scenario != 0 ) frame(canvas, snapshot, last, true);
        if ( scenario < 2 )
            frame(canvas, snapshot, scenario == 0 ? first : last, true, true);
        else if ( scenario == 2 ) {
            // 文件名相同的重新加载也使手势来源失效，不能将旧笔提交到新实例。
            snapshot.beatmapInstanceId = 2;
            frame(canvas, snapshot, last, true);
        } else {
            // 外部切换工具取消自动候选；不能在已经进入选择工具时释放出新时间点。
            snapshot.currentTool = Logic::EditTool::Marquee;
            frame(canvas, snapshot, last, true);
        }
        expect(!Access::dragging(canvas) && !Access::pointPreview(canvas),
               "cancel clears both previews");
        frame(canvas, snapshot, last);
        expect(!Access::editor(canvas) && !Access::pointEditor(canvas) &&
                   created == 0,
               "cancel release creates nothing");
        // 恢复的是下一笔的前置条件，当前取消结果已经在恢复之前完成检查。
        // 左右键仍通过帧释放，不直接清除控制器的按下标志。
        snapshot.beatmapInstanceId = 1;
        snapshot.currentTool       = Logic::EditTool::Draw;
        frame(canvas, snapshot, last);
    }
    // 已有时间点的点按与拖动也必须自动区分，不能因保护单击而失去起拖画段能力。
    // 这保持了原 Shift 范围绘制可从普通时间点起笔的能力，不新增重复事件。
    for ( bool drag : { false, true } ) {
        Canvas::TimelineCanvas canvas(
            drag ? "ExistingPointDrag" : "ExistingPointClick",
            500,
            500,
            nullptr);
        const auto origin = frame(canvas, snapshot, { 100, 100 });
        Common::Render::TimelineInteractiveElement marker{};
        marker.effects   = Common::Render::SCROLL_EFFECT_BPM;
        marker.bpmEntity = static_cast<entt::entity>(88);
        marker.bpmValue  = 120;
        // 稳定实体只用于 UI 拾取，不向空 Registry 插入虚假的用户时间点。
        // 参数保持为合法红线值，避免把数值校验失败误算成手势分流错误。
        // 时间和可见 Y 使用同一投影，避免把错误的命中夹具当成已存在的起点。
        marker.time = Access::time(canvas, 200);
        marker.y    = 200;
        snapshot.timelineElements.push_back(marker);
        const ImVec2 first{ origin.x + 50, origin.y + 200 },
            last{ origin.x + 50, origin.y + 70 };
        frame(canvas, snapshot, first);
        frame(canvas, snapshot, first, true);
        // 两个样本使用完全相同的已有起点，差别只能来自后续是否移动。
        // 按下只取得输入所有权，不能在判定为点按之前发布重复时间点。
        if ( drag ) frame(canvas, snapshot, last, true);
        frame(canvas, snapshot, drag ? last : first);
        // 单击保留原事件；拖动必须进入段落草稿，不能误建成普通点。
        // 检查两个弹窗类型互斥，不能用“任一窗口打开”代替起拖成功。
        expect(!Access::pointEditor(canvas) && Access::editor(canvas) == drag,
               "existing point click versus drag");
        frame(canvas, snapshot, drag ? last : first);
        // 快照只用于本次样本，不把该 marker 留给下一轮旧段落正文拾取。
        snapshot.timelineElements.clear();
    }
    {
        // 自动起笔不能抢走已有段落正文，单击正文保持原有的阻挡语义。
        // 双击编辑的是旧实体；同一位置的第二次按下不能被误认为新段起点。
        Canvas::TimelineCanvas canvas("ExistingAutoTiming", 500, 500, nullptr);
        const auto             origin = frame(canvas, snapshot, { 100, 100 });
        Common::Render::TimingInterpolationElement segment;
        segment.entity = static_cast<entt::entity>(99);
        segment.effect = TimingEffect::SCROLL;
        segment.time =
            std::min(Access::time(canvas, 200), Access::time(canvas, 350));
        segment.interpolation.m_duration =
            std::abs(Access::time(canvas, 200) - Access::time(canvas, 350));
        snapshot.timingInterpolations.push_back(segment);
        const ImVec2 body{ origin.x + 150, origin.y + 275 };
        const auto   doubleClickTime = ImGui::GetIO().MouseDoubleClickTime;
        // 仅恢复独立上下文的偏好，不伪造 MouseDoubleClicked 标志。
        // 第一击不得开启新点窗口，否则第二击将被错误的模态门禁拦截。
        ImGui::GetIO().MouseDoubleClickTime = 0.3F;
        frame(canvas, snapshot, body);
        frame(canvas, snapshot, body, true);
        frame(canvas, snapshot, body);
        expect(!Access::dragging(canvas) && !Access::pointEditor(canvas),
               "existing body blocks new point");
        frame(canvas, snapshot, body, true);
        expect(Access::editor(canvas) &&
                   Access::editEntity(canvas) == segment.entity,
               "existing body double click edits original");
        frame(canvas, snapshot, body);
        ImGui::GetIO().MouseDoubleClickTime = doubleClickTime;
        // 此旧段只属于本场景，不可泄漏到经典 Shift 的后续起笔区域。
        snapshot.timingInterpolations.clear();
    }
    // 所有物理按钮已释放，按正式两帧门禁切换，不能直接替换策略指针。
    // 临时开关不写用户配置，切出后的经典分支应继续使用原磁吸规则。
    UI::Interaction::activeProfile().setBypassSnapping(false);
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    Canvas::TimelineCanvas classic("ClassicRange", 500, 500, nullptr);
    const auto             origin = frame(classic, snapshot, { 100, 100 });
    // 经典输入仍用相同泳道和投影；唯一差异是方案与真实 Shift 状态。
    // 不切换 permanent snap 设置，因此能够检查方案状态未回写到磁吸配置。
    const ImVec2 first{ origin.x + 150, origin.y + 200 };
    frame(classic, snapshot, first, true);
    expect(!Access::dragging(classic), "classic no shift");
    // 普通点释放会打开创建弹窗；先模拟开始播放取消预览，隔离下一笔 Shift。
    // 不能让未渲染的普通点弹窗阻止 Shift 起笔，造成与新模式无关的假失败。
    snapshot.isPlaying = true;
    frame(classic, snapshot, first);
    snapshot.isPlaying = false;
    // 经典 Shift 仍可起段；松开 Shift 必须即时取消，不等待鼠标释放。
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    frame(classic, snapshot, first, true);
    expect(Access::dragging(classic), "classic shift start");
    // 左键继续保持，只有 Shift 释放；这能区分经典门禁与新右键取消路径。
    // 同帧的状态必须立即清理，不借助下一次释放事件补偿旧手势。
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    frame(classic, snapshot, first, true);
    expect(!Access::dragging(classic) && !Access::editor(classic),
           "classic release shift");
    frame(classic, snapshot, first);
    // 纯鼠标没有点模式开关；真实 Shift 不改变点按与拖动的区分。
    UI::Interaction::synchronizeProfile("mouse-only", true);
    UI::Interaction::synchronizeProfile("mouse-only", true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    Canvas::TimelineCanvas stopCanvas("StopRangeFallback", 500, 500, nullptr);
    const ImVec2           stop{ origin.x + 450, origin.y + 200 };
    // 先推进无按键帧，让第五泳道位置与上个场景的释放独立。
    // STOP 只验证正常点弹窗打开，不借由测试调用私有创建方法。
    frame(stopCanvas, snapshot, stop);
    frame(stopCanvas, snapshot, stop, true);
    expect(!Access::dragging(stopCanvas), "stop never interpolates");
    frame(stopCanvas, snapshot, stop);
    expect(Access::stopPoint(stopCanvas), "stop point fallback");
    // 以上所有操作只准备弹窗，尚未确认创建，不能产生任何谱面命令。
    // 普通点和范围弹窗都没有被确认，检查命令数量能发现过早写入谱面的实现。
    // 取消后的普通点弹窗另有显式断言，不仅依赖零创建命令推断取消正确。
    expect(created == 0, "no premature commit");
    bus.unsubscribe<Event::LogicCommandEvent>(subscription);
    // 先解除局部观察闭包再释放会话，销毁事件不能访问已结束的计数生命周期。
    // 会话索引来自实际创建结果，不关闭测试进程可能持有的其他条目。
    engine.closeSession(session, false);
    if ( !passed )
        XERROR(
            "Automatic Timing drawing, cancellation or classic isolation "
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
    bool passed = checkMenu();
    passed &= checkRangeDrawing();
    ImGui::DestroyContext();
    // 配置恢复放在上下文销毁之后，确保测试失败时也不会污染后续设置断言。
    settings = original;
    return passed ? 0 : 1;
}
