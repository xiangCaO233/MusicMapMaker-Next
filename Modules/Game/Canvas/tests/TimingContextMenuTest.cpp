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
    /// @brief 核对普通点编辑副本，区分右键目标与播放头、唯一选中项和旧值。
    /// @param c 菜单动作后仍存活的画布，不直接修改其弹窗状态。
    /// @return 参数编辑器请求、原实体、精确时间和值均匹配时为真。
    /// @note 不把“有任意弹窗”视为成功，创建窗口与普通点编辑窗口必须区分。
    /// @note 保持速度的复选框默认清除，不能继承上次编辑 BPM 的联动状态。
    static bool pointEdit(const TimelineCanvas& c, entt::entity entity,
                          double time, double value, const char* type)
    {
        return c.m_isPopupOpen && c.m_editingEntity == entity &&
               c.m_editTime == time && c.m_editValue == value &&
               c.m_editType == type && !c.m_keepSpeedOnBpmEdit;
    }
    /// @brief 已有点参数窗口与新建点弹窗拥有不同状态，不能混用断言。
    /// @return 不读取新建请求或插值请求，只观察原有事件详情窗口。
    static bool eventEditor(const TimelineCanvas& c) { return c.m_isPopupOpen; }
    /// @brief 在父窗口层级绘制生产模态窗口，验证菜单关闭后的 ID 栈衔接。
    /// @param c 已由真实菜单准备好参数的画布，不直接创建或修改 Timing。
    /// @note 只用于普通点；段落测试继续观察原编辑器保留的完整定义。
    /// @warning 仅由独立 CPU 测试帧调用，不初始化音频设备或后台逻辑循环。
    static void renderEventEditor(TimelineCanvas& c)
    {
        c.renderEventEditorPopup();
    }
    /// @brief 在 Begin 前执行实际窗口聚焦流程，不绕过生产侧的弹窗门禁。
    /// @param c 保留跨帧焦点请求的时间线实例。
    static void prepareWindow(TimelineCanvas& c)
    {
        c.applyPendingWindowFocus();
    }
    /// @brief 重现画布点击后既有的当前帧及下一帧聚焦请求。
    /// @note 测试只建立输入前提，延迟请求仍由生产函数消费。
    static void focusClickedCanvas(TimelineCanvas& c)
    {
        ImGui::SetWindowFocus();
        c.m_shouldFocusNextFrame = true;
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
/// @param showEditor
/// 仅在菜单参数测试中绘制普通事件模态，其他手势用例保持无弹窗正文。
/// @return 固定窗口的内容原点，避免依赖用户的布局和 DPI。
ImVec2 frame(Canvas::TimelineCanvas&         canvas,
             Common::Render::RenderSnapshot& snapshot, ImVec2 mouse,
             bool left = false, bool right = false, bool showEditor = false)
{
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, left);
    io.AddMouseButtonEvent(1, right);
    ImGui::NewFrame();
    // 禁用持久布局后，每次运行都从相同几何和相同弹窗相对位置开始。
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ 600, 600 });
    // 延迟聚焦必须先于 Begin，复现正式窗口可能关闭上一帧右键菜单的顺序。
    Access::prepareWindow(canvas);
    ImGui::Begin("TimingMenuTest", nullptr, ImGuiWindowFlags_NoSavedSettings);
    const auto origin = ImGui::GetCursorScreenPos();
    // 只在真实父窗口内部点击时排队聚焦；点击菜单不属于画布点击。
    // 不伪造 clicked 标记，菜单开启和下一帧存活都由真实按下/释放边沿验证。
    const bool inside = mouse.x >= origin.x && mouse.y >= origin.y &&
                        mouse.x < origin.x + 500 && mouse.y < origin.y + 500;
    if ( inside &&
         ImGui::IsWindowHovered(
             ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
         (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) ||
          ImGui::IsMouseClicked(2)) )
        Access::focusClickedCanvas(canvas);
    // 即便弹窗拥有焦点仍要推进控制器，让菜单关闭帧执行输入防穿透逻辑。
    Access::interact(canvas, snapshot, origin);
    // 与正式时间线保持同一父窗口层级；不能在 TimingEditMenu 内绘制目标编辑器。
    if ( showEditor ) Access::renderEventEditor(canvas);
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
    // 正式点击已排队下一帧聚焦，此次释放帧会先执行生产窗口的聚焦入口。
    // 若入口只排除模态而未保护普通菜单，Begin 会清空弹窗，下面必须返回空。
    // 因而所有菜单点击用例同时验证跨帧存活，不能只凭按下帧打开判定成功。
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
/// @note 分隔线分别位于第 2、6、8、11、13 和 14 行之前，索引不含分隔线。
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
                              (row >= 13 ? 1 : 0) + (row >= 14 ? 1 : 0);
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
/// @brief 验证段落正文的完整鼠标编辑链，而非仅检查参数编辑器是否打开。
/// @return
/// 复制、粘贴、剪切、删除、全选、清空与历史入口均按时间线语义运行时为真。
/// @details 同时覆盖可见首标记与没有首标记两种快照，禁止重复或遗漏段落实体。
/// @note 观察正式创建批次中的曲线参数，不允许粘贴退化为普通时间点。
/// @note 所有副作用限制在当前测试进程，既不读用户剪贴板，也不写入项目文件。
bool checkSegmentClipboard()
{
    bool passed = true;
    // 两种快照共用正式菜单路径，模式和 BPM 选择门禁保持同一初始配置。
    // 不直接调用控制器的私有复制方法，避免绕过菜单禁用和选区失效检查。
    // 模式只影响后续点击；右键已有对象必须能建立可执行复制的普通选区。
    // 用同一套真实菜单坐标驱动，不能由访问器直接调用复制或删除函数。
    for ( bool hasMarker : { false, true } ) {
        Common::Render::RenderSnapshot snapshot;
        snapshot.hasBeatmap        = true;
        snapshot.currentTool       = Logic::EditTool::Marquee;
        snapshot.currentTime       = 4;
        snapshot.beatmapPathKey    = "segment-clipboard.mmm";
        snapshot.beatmapInstanceId = 1;
        Common::Render::TimingInterpolationElement segment;
        segment.entity                   = static_cast<entt::entity>(101);
        segment.effect                   = TimingEffect::SCROLL;
        segment.time                     = 3;
        segment.value                    = 1.25;
        segment.interpolation.m_duration = 1.5;
        segment.interpolation.m_endValue = 2.5;
        segment.interpolation.m_curve    = TimingCurve::EaseOut;
        segment.interpolation.m_samplesPerSecond = 32;
        // 使用非默认曲线与密度，单独复制段首或重建默认插值都不能通过比较。
        // 终值也不同于起值，排除长度正确但实际变成恒定倍率的假成功。
        snapshot.timingInterpolations.push_back(segment);
        // 独立 BPM 与段落混合复制，保持原有相对秒差，不能全贴在同一锚点。
        // BPM 不位于正文泳道，右键段落时只应选中原段落实体。
        Common::Render::TimelineInteractiveElement point{};
        point.effects   = Common::Render::SCROLL_EFFECT_BPM;
        point.bpmEntity = static_cast<entt::entity>(42);
        point.bpmValue  = 120;
        point.time      = 4.5;
        point.y         = 150;
        snapshot.timelineElements.push_back(point);
        if ( hasMarker ) {
            // 段首与正文引用同一个实体，复制和全选不能因此产生两个条目。
            // 单独的首标记仍远离正文落点，测试不能依靠误命中段首而通过。
            auto marker         = point;
            marker.effects      = Common::Render::SCROLL_EFFECT_SCROLL;
            marker.bpmEntity    = entt::null;
            marker.scrollEntity = segment.entity;
            marker.scrollValue  = segment.value;
            marker.time         = segment.time;
            marker.y            = 50;
            snapshot.timelineElements.push_back(marker);
        }
        Canvas::TimelineCanvas canvas(
            hasMarker ? "SegmentCopyWithMarker" : "SegmentCopyNoMarker",
            500,
            500,
            nullptr);
        const auto  origin = frame(canvas, snapshot, { 100, 100 });
        const float bodyY  = static_cast<float>((200 + 350) * .5F);
        // 起终投影取实际画布，确保正文位于可见区，不依赖默认缩放的偶然结果。
        // 只调整本测试的时间范围；类型和曲线定义始终由同一个段落实体提供。
        snapshot.timingInterpolations[0].time =
            std::min(Access::time(canvas, 200), Access::time(canvas, 350));
        snapshot.timingInterpolations[0].interpolation.m_duration =
            std::abs(Access::time(canvas, 350) - Access::time(canvas, 200));
        const auto& source = snapshot.timingInterpolations[0];
        // source 借用本轮固定快照，后续不再增删段落，避免引用因扩容失效。
        // 可选首标记与段落时间一致，而其屏幕 Y 故意不在正文点击范围内。
        if ( hasMarker ) snapshot.timelineElements[1].time = source.time;
        snapshot.timelineElements[0].time = source.time + 1.5;
        const ImVec2 body{ origin.x + 150, origin.y + bodyY },
            blank{ origin.x + 400, origin.y + 400 };
        std::vector<Logic::CmdCreateTimelineEvents::Entry> created;
        // 本轮捕获创建结果的值副本；下一次粘贴会整体替换，不累计旧批次。
        // 对数量和相对位置分别断言，防止重复发布也碰巧满足非空检查。
        std::size_t deleted = 0, undo = 0, redo = 0;
        auto&       bus = Event::EventBus::instance();
        const auto  subscription =
            bus.subscribe<Event::LogicCommandEvent>([&](const auto& event) {
                // 所有批量操作都经正式命令通路；只记录载荷，不伪造逻辑快照回流。
                // 这能检出菜单把段落拆成采样点、重复创建或误发主画布命令的回归。
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
            });
        /// @brief 检查完整定义随剪贴板跨入口保留，不仅检查生成条目数量。
        /// @return 起始值、终值、曲线、时长与采样密度同时保持时为真。
        auto copiedRange = [&]() {
            // 插值字段本身必须存在，不能因普通点起始值相同而误判复制成功。
            // 类型、倍率与连续定义分开比较，检出泳道或字段映射错误。
            if ( created.empty() || !created[0].interpolation ) return false;
            const auto& range = *created[0].interpolation;
            return created[0].type == source.effect &&
                   created[0].value == source.value &&
                   range.m_duration == source.interpolation.m_duration &&
                   range.m_endValue == source.interpolation.m_endValue &&
                   range.m_curve == source.interpolation.m_curve &&
                   range.m_samplesPerSecond ==
                       source.interpolation.m_samplesPerSecond;
        };
        // 第一次右键正文即有可复制选区，关闭菜单后不能被 marker 清理流程抹去。
        // 粘贴以打开菜单时的吸附位置为锚点，移入菜单不会改用播放头或菜单坐标。
        passed &=
            click(canvas, snapshot, body, 2) && Access::count(canvas) == 1;
        passed &= click(canvas, snapshot, blank, 4) && created.size() == 1 &&
                  copiedRange();
        if ( !created.empty() )
            passed &= std::abs(created[0].time - Access::anchor(canvas)) < 1e-9;
        // 全选必须同时包含没有首标记的段落与普通点；有首标记时仍然去重。
        // 全选后的复制从空白处激活，右键不能悄悄替换为附近单个 marker。
        // 剪贴板包含 BPM 时延续既有相对秒语义，避免新红线改变后续拍位换算。
        // 混合粘贴只产生两个实体，并保留普通点相对段首的一点五秒偏移。
        passed &=
            click(canvas, snapshot, blank, 6) && Access::count(canvas) == 2;
        passed &= click(canvas, snapshot, blank, 2);
        passed &= click(canvas, snapshot, blank, 4) && created.size() == 2 &&
                  copiedRange();
        if ( created.size() == 2 )
            passed &= std::abs(created[1].time - created[0].time - 1.5) < 1e-9;
        // 剪切先保留完整定义，再删除一个原段落；本地选区立即清空。
        // 点击粘贴前不清理或重建选区，跨帧保留必须由正式控制器负责。
        // 复制与粘贴分属独立菜单事务，目标位置不沿用上次右键的正文时间。
        // 后续粘贴仍使用剪切结果，不能因删除后选择为空而丢失曲线。
        passed &= click(canvas, snapshot, body, 3) && deleted == 1 &&
                  Access::count(canvas) == 0;
        passed &= click(canvas, snapshot, blank, 4) && created.size() == 1 &&
                  copiedRange();
        // 快照刻意未应用删除结果，使下一次正文右键能独立验证删除入口。
        // 实际逻辑批量命令与撤销恢复由既有 Timing 命令测试覆盖。
        // 历史入口操作当前时间线会话，不能转发成主画布的音符选择或粘贴。
        // 本测试只核对一次命令；撤销栈实际恢复由逻辑层测试验证。
        passed &= click(canvas, snapshot, body, 5) && deleted == 2;
        passed &= click(canvas, snapshot, blank, 0);
        passed &= click(canvas, snapshot, blank, 1);
        passed &= undo == 1 && redo == 1;
        // 清空选择只更改本地集合，不能追加删除指令或改变剪贴板。
        // 快照刻意保留原段落与普通点，全选后清空必须覆盖两种实体来源。
        // 选择变化不构成谱面编辑，删除总数在清空前后应保持不变。
        // 右键空白不会自动重新选中段落，因此此处能检出选区清理失效。
        passed &= click(canvas, snapshot, blank, 6);
        passed &= click(canvas, snapshot, blank, 7) &&
                  Access::count(canvas) == 0 && deleted == 2;
        // 解除监听后才销毁捕获目标，避免下一组会话事件访问已结束的计数生命周期。
        bus.unsubscribe<Event::LogicCommandEvent>(subscription);
    }
    if ( !passed )
        XERROR("Timing segment context clipboard or selection failed");
    return passed;
}
/// @brief 通过真实右键菜单验证普通参数编辑，不借用画笔齿轮或双击入口。
/// @return 五种效果、唯一选中回退及多选拒绝均保持原实体与参数时为真。
/// @details 各场景使用独立画布，避免未渲染的模态请求影响下一次菜单。
/// @note 本用例不确认参数，打开菜单和准备草稿不能生成谱面修改命令。
/// @pre 两个工具共用参数弹窗；不通过切换到绘制工具来获得编辑权限。
/// @post 普通点只读取快照，测试无需创建虚假的逻辑会话或时间线 Registry。
/// @warning 模态输入拥有焦点时仍推进父控制器，验证释放事件不会穿透画布。
/// @pre 专业模式已由主函数启用，所有五类 marker 均采用固定泳道。
/// @post 本函数解除命令监听；BPM 的选择配置恢复到调用前的测试设定。
/// @note 多选只影响参数入口的回退规则，命中目标不会清空既有集合。
/// @note 空白处多选没有确定的单个参数，不允许以容器迭代顺序选取目标。
/// @note 编辑窗口显示使用既有模态组件，不需要 Vulkan 纹理或音频输出。
bool checkPointEditing()
{
    // 方案切换通过安全帧完成，不能为了测试绕过物理按钮的释放门禁。
    // 前一组菜单测试已经释放左右键，因此这里无需伪造按键缓存。
    UI::Interaction::synchronizeProfile("mouse-only", true);
    UI::Interaction::synchronizeProfile("mouse-only", true);
    // 集合模式跨方案切换保留，上一组减选测试不能成为本用例的隐含初始条件。
    // 这里只设下一笔的正常替换语义；具体对象仍经真实点击建立唯一选择。
    UI::Interaction::activeProfile().setSelectionOperation(
        Logic::SelectionOperation::Replace);
    bool passed = true;
    // 草稿打开不是数据修改；监听整条命令通路，可检出菜单误触删除或更新。
    // 订阅生命周期严格短于本函数，所有引用在解除监听前保持存活。
    int        commands     = 0;
    auto&      bus          = Event::EventBus::instance();
    const auto subscription = bus.subscribe<Event::LogicCommandEvent>(
        [&](const auto&) { ++commands; });
    const std::array<const char*, 5> types{
        "BPM", "Scroll", "Jump", "HS", "STOP"
    };
    // 两个工具均可编辑；BPM 的多选关闭也不能屏蔽直接参数编辑。
    // 其他场景包括已有多选、空白唯一选择、空白多选及打开菜单后的目标删除。
    for ( int scenario = 0; scenario < 10; ++scenario ) {
        bool scenarioPassed = true;
        // 每轮绑定独立快照，避免上个实体草稿或菜单身份令牌残留。
        // 专业模式固定五泳道，类型断言不依赖字体或翻译后的标签宽度。
        const int                      lane = scenario < 5 ? scenario : 1;
        Common::Render::RenderSnapshot snapshot;
        snapshot.hasBeatmap = true;
        snapshot.currentTool =
            scenario % 2 ? Logic::EditTool::Move : Logic::EditTool::Marquee;
        snapshot.currentTime = 4;
        // 播放头故意不同于 marker 时间；参数编辑必须取对象时间而非导航位置。
        // 固定路径与实例号仅标识本测试快照，不关联磁盘上的同名谱面文件。
        snapshot.beatmapPathKey    = "point-edit.mmm";
        snapshot.beatmapInstanceId = 1;
        Common::Render::TimelineInteractiveElement marker{};
        marker.time = 3.25;
        marker.y    = 150;
        // 实体是稳定值标识；菜单不能通过空测试 Registry 重新查找普通点参数。
        // 只有插值编辑器的冲突检查需要真实会话，本组普通点无需创建会话。
        const auto entity =
            static_cast<entt::entity>(scenario >= 5 ? 42 : 50 + lane);
        // 专用 effect 位和字段共同决定拾取，不能只改文字标签来模拟五种类型。
        // 各类型使用不相同的非默认参数，保证菜单确实从当前快照取值。
        const double value = 120.0 + lane;
        switch ( lane ) {
        case 0:
            // 红线使用原始 BPM 参数，不能误读绿线的默认倍率字段。
            marker.effects   = Common::Render::SCROLL_EFFECT_BPM;
            marker.bpmEntity = entity;
            marker.bpmValue  = value;
            break;
        case 1:
            // 绿线取存储倍率，打开窗口时不重新乘除参考 BPM。
            marker.effects      = Common::Render::SCROLL_EFFECT_SCROLL;
            marker.scrollEntity = entity;
            marker.scrollValue  = value;
            break;
        case 2:
            // Jump 的存储值直接送入现有编辑器，由其统一展示单位。
            marker.effects    = Common::Render::SCROLL_EFFECT_JUMP;
            marker.jumpEntity = entity;
            marker.jumpValue  = value;
            break;
        case 3:
            // HS 必须保持独立类型，不能回退到普通 Scroll 编辑分支。
            marker.effects  = Common::Render::SCROLL_EFFECT_HS;
            marker.hsEntity = entity;
            marker.hsValue  = value;
            break;
        case 4:
            // STOP 的类型字符串需与原弹窗严格一致，避免错误显示倍率控件。
            marker.effects    = Common::Render::SCROLL_EFFECT_STOP;
            marker.stopEntity = entity;
            marker.stopValue  = value;
            break;
        }
        // 效果位与对应实体字段缺一不可，这里只发布当前类型的 marker。
        // 测试无需构建 Vulkan 几何，生产拾取的最小命中矩形即可提供入口。
        snapshot.timelineElements.push_back(marker);
        auto other         = marker;
        other.time         = 3.75;
        other.y            = 220;
        other.scrollEntity = static_cast<entt::entity>(43);
        // 第二个事件与右键目标位于不同 Y，避免重复几何造成无意重叠。
        // 42、43 与已有选区访问器一致，使多选断言确实有两个有效实体。
        if ( scenario >= 5 ) snapshot.timelineElements.push_back(other);
        Canvas::TimelineCanvas canvas(
            "PointEdit" + std::to_string(scenario), 500, 500, nullptr);
        const auto origin = frame(canvas, snapshot, { 100, 100 });
        ImVec2     point{ origin.x + 50 + lane * 100, origin.y + 150 };
        // 上一轮模态可能自动聚焦数值框，关闭后推进一帧清理文本输入所有权。
        // 这里不直接篡改 WantTextInput，否则会掩盖生产菜单的录入防穿透门禁。
        frame(canvas, snapshot, point);
        if ( scenario == 0 )
            Config::AppConfig::instance()
                .getEditorSettings()
                .timelineSelectionIncludesBpm = false;
        if ( scenario >= 5 ) {
            // 选择与编辑是独立事务；右键打开编辑器不会改变已经建立的多选集合。
            // 场景 5 命中 42，同时保留 43，避免编辑入口先替换整个选区。
            // 独立 marker 与右键命中对象不同，已有多选不能把编辑目标换成首项。
            // 唯一选择通过真实左键建立，测试只为多选场景初始化两个成员。
            if ( scenario == 5 || scenario == 7 )
                Access::select(canvas);
            else {
                frame(canvas, snapshot, point, true);
                frame(canvas, snapshot, point);
            }
        }
        // 400 像素处远离两个 marker，只能走唯一选择回退，不能直接命中。
        // 场景 7 保留两项选择，因此必须拒绝猜测一个对象作为参数编辑目标。
        if ( scenario == 6 || scenario == 7 || scenario == 9 )
            point.y = origin.y + 400;
        bool activated = false;
        if ( scenario == 8 || scenario == 9 ) {
            // 菜单停留期间删除或更新目标，点击须从当前快照重新解析而非使用旧副本。
            const auto* popup = open(canvas, snapshot, point);
            scenarioPassed &= popup != nullptr;
            if ( popup ) {
                const auto& style = ImGui::GetStyle();
                // 使用真实弹窗的起点和行间距，菜单为避屏幕边缘而上移也不影响点击。
                // 目标位置与右键锚点分离，可以检出菜单激活时重新拾取鼠标的错误。
                const ImVec2 row{
                    popup->Pos.x + popup->Size.x * .5F,
                    popup->DC.CursorStartPos.y +
                        14 * ImGui::GetTextLineHeightWithSpacing() +
                        6 * (1 + style.ItemSpacing.y) +
                        ImGui::GetTextLineHeight() * .5F
                };
                if ( scenario == 8 )
                    snapshot.timelineElements.clear();
                else
                    snapshot.timelineElements[0].scrollValue = value + 1;
                // 该更新不改变第二个事件，断言可区分锁存的目标与选区内其他对象。
                // 使用整型可精确表示的 double 增量，排除文本格式化与舍入误差。
                // 删除后仍点击原菜单行，拒绝编辑的原因必须来自目标失效检查。
                // 更新场景只改参数，实体身份不变，必须读到新的精确值。
                frame(canvas, snapshot, row, true);
                frame(canvas, snapshot, row);
                activated = ImGui::GetCurrentContext()->OpenPopupStack.empty();
            }
        } else
            activated = click(canvas, snapshot, point, 14);
        const bool shouldEdit = scenario != 7 && scenario != 8;
        // 删除后的快照没有目标，与已有目标但空白处多选不唯一属于两种失败来源。
        // 两种情形都不得意外打开新建点弹窗，也不得沿用上一轮参数编辑状态。
        // 多选空白的条目保持禁用，菜单不关闭；其余有效目标必须准确匹配。
        // 错误实体、错误字段或 STOP 大小写错误都会使参数断言失败。
        if ( shouldEdit )
            scenarioPassed &=
                activated &&
                Access::pointEdit(canvas,
                                  entity,
                                  marker.time,
                                  scenario == 9 ? value + 1 : value,
                                  types[lane]);
        else
            scenarioPassed &= !Access::eventEditor(canvas);
        if ( shouldEdit ) {
            // 请求标志正确还不够：正式模态必须在菜单关闭后于父窗口层级可见。
            // 只绘制正文，不按应用或删除按钮，因此不会向空测试会话提交数据。
            frame(canvas, snapshot, point, false, false, true);
            const auto& stack = ImGui::GetCurrentContext()->OpenPopupStack;
            // 菜单若未正确退出，参数窗口会成为其子弹窗，堆栈深度就不再为一。
            // 同时检查实际窗口与 Modal 标志，避免仅有打开请求造成假通过。
            scenarioPassed &=
                stack.size() == 1 && stack.back().Window &&
                (stack.back().Window->Flags & ImGuiWindowFlags_Modal);
        }
        if ( !scenarioPassed )
            XERROR(
                "Timing point context edit failed: scenario={}, activated={}, "
                "selected={}, editor={}",
                scenario,
                activated,
                Access::count(canvas),
                Access::eventEditor(canvas));
        passed &= scenarioPassed;
        // 配置只临时限制红线集合参与，不改变后续场景的选择范围。
        // 参数编辑与集合门禁是两种权限，恢复操作发生在断言之后。
        if ( scenario == 0 )
            Config::AppConfig::instance()
                .getEditorSettings()
                .timelineSelectionIncludesBpm = true;
        // 未渲染的菜单请求不一定具有 Window，必须在画布销毁前显式收尾。
        // 这里仅清除当前独立场景的弹窗，不改变生产编辑控制器状态。
        if ( !ImGui::GetCurrentContext()->OpenPopupStack.empty() )
            ImGui::ClosePopupToLevel(0, true);
        frame(canvas, snapshot, point);
    }
    // 打开参数窗口、失效目标与禁用多选入口均不得提前修改谱面。
    // 窗口内的应用和删除仍由既有弹窗测试覆盖，不在此重写其业务命令。
    passed &= commands == 0;
    // 先解除捕获局部计数的闭包，再进入后续需要真实会话的段落测试。
    bus.unsubscribe<Event::LogicCommandEvent>(subscription);
    if ( !passed ) XERROR("Timing context point parameter editing failed");
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
    // 选择工具菜单必须能从首标记不可见的段落正文打开原编辑器。
    // 用同一真实会话取得冲突检查上下文，曲线和采样定义不能退化成普通时间点。
    for ( bool professional : { true, false } ) {
        Config::AppConfig::instance().getEditorSettings().professionalMode =
            professional;
        snapshot.currentTool = Logic::EditTool::Marquee;
        Canvas::TimelineCanvas canvas(
            professional ? "RangeMenuPro" : "RangeMenuNormal",
            500,
            500,
            nullptr);
        const auto origin = frame(canvas, snapshot, { 100, 100 });
        Common::Render::TimingInterpolationElement segment;
        segment.entity = static_cast<entt::entity>(101);
        segment.effect = TimingEffect::SCROLL;
        segment.value  = 1.25;
        segment.time =
            std::min(Access::time(canvas, 200), Access::time(canvas, 350));
        segment.interpolation.m_duration =
            std::abs(Access::time(canvas, 200) - Access::time(canvas, 350));
        snapshot.timingInterpolations.push_back(segment);
        const ImVec2 body{ origin.x + 150, origin.y + 275 };
        expect(click(canvas, snapshot, body, 14),
               "range menu closes before editor");
        expect(Access::editor(canvas) &&
                   Access::editEntity(canvas) == segment.entity &&
                   Access::rangeType(canvas) == segment.effect &&
                   Access::duration(canvas) == segment.interpolation.m_duration,
               "range context edits original definition");
        snapshot.timingInterpolations.clear();
        frame(canvas, snapshot, body);
    }
    Config::AppConfig::instance().getEditorSettings().professionalMode = true;
    snapshot.currentTool = Logic::EditTool::Draw;
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
    passed &= checkSegmentClipboard();
    passed &= checkPointEditing();
    passed &= checkRangeDrawing();
    ImGui::DestroyContext();
    // 配置恢复放在上下文销毁之后，确保测试失败时也不会污染后续设置断言。
    settings = original;
    return passed ? 0 : 1;
}
