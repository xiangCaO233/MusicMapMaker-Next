#include "canvas/TimelineCanvas.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/ActionController.h"
#include "logic/session/EditorAction.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "presentation/TimelineInteractionPresentation.h"
#include "presentation/mobile/MobileTimelineInteractionPresentation.h"
#include "ui/interaction/EditorInteraction.h"
#include <cmath>
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

namespace MMM::Canvas
{
/// @brief 夹具只安装生产解释器并观察结果，不直接调用创建、选择或菜单动作。
/// @details 访问器没有提交命令的入口，期望结果必须由真实触摸流产生。
/// 生产投影可用于测量期望时长，但不能直接读取被测草稿作为期望值。
/// 私有观察只针对当前对象，不能在两轮输入之间替业务清理预览。
struct MobileTimingInteractionTestAccess {
    /// @brief 平台工厂不被替换，单个测试画布显式采用真实移动呈现。
    static void bind(TimelineCanvas& c, Common::Render::RenderSnapshot& s)
    {
        // 单个实例替换呈现对象，不改变整个测试进程链接的桌面工厂。
        // 与 Android 生产相同的移动代码负责输入分类，夹具不复制状态机。
        // 快照由 Fixture 拥有，在画布每一次调用期间都有效。
        c.m_timingPresentation = createMobileTimelineInteractionPresentation();
        c.m_currentSnapshot    = &s;
    }
    /// @brief 真正经过画布入口及 ImGui 边沿，不能直接调用内部业务函数。
    static void update(TimelineCanvas& c, ImVec2 origin)
    {
        c.handleTimingCanvasInteraction(origin, { 500, 500 }, true, true, true);
    }
    /// @brief 草稿结果区分普通点与插值窗口，不能仅以无异常判断成功。
    static bool point(const TimelineCanvas& c) { return c.m_isCreatePopupOpen; }
    /// @brief 新建点的类型必须锁存起始泳道，跨道抬手不改变语义。
    static int type(const TimelineCanvas& c) { return c.m_createType; }
    /// @brief 区间结果按值读取，不留存私有工作副本引用。
    static double duration(const TimelineCanvas& c)
    {
        return c.m_interpolationEdit.interpolation.m_duration;
    }
    /// @brief 请求参数窗口也表示区间创建完成，不要求启动图形设备。
    static bool range(const TimelineCanvas& c)
    {
        return c.m_requestInterpolationEditor || c.m_isInterpolationEditorOpen;
    }
    /// @brief 期望时间使用原生产投影，避免在测试复制吸附或坐标公式。
    static double time(const TimelineCanvas& c, float y)
    {
        return c.canvasTimeAtLocalY({ 500, 500 }, y);
    }
    /// @brief 端点从实际画布正向投影取得，测试不另写像素／时间换算公式。
    static float y(const TimelineCanvas& c, double time)
    {
        return static_cast<float>(c.canvasYAtTime({ 500, 500 }, time));
    }
    /// @brief 选择属于时间线自身，不借主画布选区作为成功证据。
    static std::size_t count(const TimelineCanvas& c)
    {
        return c.m_selectedTimingEntities.size();
    }
};
}  // namespace MMM::Canvas
namespace
{
using namespace MMM;
using Access = Canvas::MobileTimingInteractionTestAccess;
int failures{};  ///< 逐项报告，不因前一场景失败跳过后续取消覆盖。
/// @brief 保留各项失败说明，测试不调用控制台输出或抛异常。
void check(bool ok, const char* text)
{
    if ( !ok ) {
        XERROR("{}", text);
        ++failures;
    }
}
/// @brief 原始触点与鼠标表示一起发布，第二指只进入原始仲裁流。
/// @par 输入模型
/// 原始事件携带触点身份和代际，鼠标表示只携带主触点位置及边沿。
/// 双指接管必须来自原始层，不能在测试中直接切换内部 blocked 标志。
/// 不同用例共享后端的代际单调性，新的真实 Down 才能开始新一笔。
/// 释放之后继续推进四个空闲帧，可以观察晚到回放和重复提交。
/// 没有调用画布内部取消函数，因此取消失败不能被清理代码遮住。
/// @par 布局契约
/// 窗口内容原点为固定主题下的实际原点，局部样本只加一次偏移。
/// Timing 区域使用 500x500 逻辑单位，所有坐标位于可交互内容中。
/// 新绘制避开右上角工具菜单，测试不会因点中按钮而偶然没有创建。
/// 长度断言读取生产时间投影，不复制 ScrollCache 或缩放公式。
/// @par 数据与生命周期
/// Fixture 只拥有一个画布和一份快照，快照从不跨线程修改。
/// 画布中的观察指针仅在帧调用期间读取，本用例没有启动引擎循环。
/// 未确认草稿保留到场景结束，再随画布与上下文一起销毁。
/// 历史计数器只观察正式事件，初始夹具时间点没有创建或删除历史。
/// 不直接改变选中集合，确保轻点选择失败时能够被明确发现。
/// @par 边界覆盖
/// 五条泳道轻点覆盖类型锁存及 STOP 的普通点退路。
/// 小幅抖动覆盖手指松开时的自然偏移，不能变成意外插值区间。
/// 正向与反向拖动覆盖时间两端归一化，横跨泳道不能重解释类型。
/// 主指先松开与同时松开覆盖两种常见接管释放顺序。
/// 点选、真实移动和取消分别要求零、一、零条批量修改命令。
/// 场景结束后没有额外补偿 Undo，也没有重建谱面来掩盖副作用。
/// @details 预热位置与实际起点一致，避免不同位置触发输入队列拆帧。
/// 时间使用固定 UI 步长，长按分类无需真实等待或调度假设。
class Fixture
{
public:
    /// @brief 构造内存谱面快照；没有文件访问或真实音频播放。
    /// @details 本地快照包含固定路径和实例，不能依赖测试进程的活动标签。
    /// 不创建真实文件或监听器，避免结果受用户配置和目录状态影响。
    /// 画布可发布初始视口请求，但没有后台循环会消费本用例的指针预览。
    explicit Fixture(Logic::EditTool tool)
        : m_canvas("TouchTimingFixture", 500, 500, nullptr)
    {
        m_snapshot.hasBeatmap        = true;
        m_snapshot.beatmapPathKey    = "timing-touch.mmm";
        m_snapshot.beatmapInstanceId = 1;
        m_snapshot.currentTime       = 4;
        m_snapshot.currentTool       = tool;
        Access::bind(m_canvas, m_snapshot);
    }
    /// @brief 帧结束前仍推进控制器，快速抬手的延迟回放不能被测试省略。
    /// @warning 测试只做 CPU 布局与固定大小触点运算，不等待后台线程。
    void frame(ImVec2 local, bool down = false, bool second = false,
               bool cancelled = false)
    {
        // 输入先用绝对屏幕逻辑像素进入 ImGui，再归一化发布给原始触点层。
        // 两条输入流采用同一个位置和顺序，不能用已经合成的鼠标代替原始峰值。
        // 固定偏移来自测试窗口的真实内容原点，后面仍从窗口读正式原点。
        const ImVec2 point{ local.x + 8, local.y + 27 };
        using Graphic::TouchPhase;
        Graphic::publishMobileTouch(
            cancelled ? TouchPhase::Cancel
            : down    ? (m_down ? TouchPhase::Move : TouchPhase::Down)
                      : TouchPhase::Up,
            1,
            1,
            point.x / 1000,
            point.y / 900,
            m_tick * .05);
        // 第二指有独立稳定身份，数组位置和第一指鼠标表示均不随其变化。
        // 只在状态改变时发 Down/Up，持续按住不能反复创建新代际。
        // 主指先抬起的场景继续保留第二指，验证旧鼠标释放不会迟到提交。
        if ( second != m_second )
            Graphic::publishMobileTouch(
                second ? TouchPhase::Down : TouchPhase::Up,
                1,
                2,
                .5f,
                .5f,
                m_tick * .05);
        m_down   = down;
        m_second = second;
        ++m_tick;
        auto& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(point.x, point.y);
        io.AddMouseButtonEvent(0, down);
        // 原始触点先发布，再开始 UI 帧，保持与 Android 后端消费顺序一致。
        // 不直接写 MouseClicked 或 ActiveId，以免绕过真正的事件队列。
        // 每帧即使没有触点仍推进窗口，允许待定轻点正常排空回放。
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 700, 800 });
        ImGui::Begin(
            "TouchTimingFixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
        // 输入域必须与生产窗口一致，不能依靠 Application 默认域解释 Timing。
        const UI::Interaction::SurfaceScope scope(
            UI::Interaction::Surface::Timeline);
        Access::update(m_canvas, ImGui::GetCursorScreenPos());
        ImGui::End();
        ImGui::Render();
    }
    /// @brief 用多帧空闲排空延迟起笔与释放，再观察结果。
    /// @details 空闲不是跳过控制器，而是持续执行正式每帧入口。
    /// 输入层的待定 Begin/Release 因此必须按生产顺序完成。
    /// 四帧只是测试排空采样，不对应程序中的固定时长等待或阻塞。
    /// 没有借下一笔按下来强制提交上一笔，避免把两笔错误合并。
    void idle(ImVec2 p)
    {
        for ( int i = 0; i < 4; ++i ) frame(p);
    }
    Canvas::TimelineCanvas
        m_canvas;  ///< 先于 ImGui 上下文销毁，析构不访问资源文件。
    Common::Render::RenderSnapshot m_snapshot;  ///< 同一实例跨全部帧保持存活。
private:
    unsigned m_tick{};              ///< 单调合成时间不用于阻塞等待。
    bool     m_down{}, m_second{};  ///< 原始边沿只在真实状态变化时产生。
};
/// @brief 每个场景独立 ImGui 上下文，未确认参数草稿不跨场景污染。
/// @details 字体图集只供 CPU 排版，不上传 GPU，也不需要可见桌面窗口。
/// 固定 DeltaTime 覆盖真实长按计时，不执行 sleep 或依赖宿主帧率。
/// 禁用 ini 持久化，测试过程不能写入开发者的窗口布局。
/// 关闭输入分帧队列使每个样本边界可预测，生产分类仍采用真实 API。
/// 方案切换先经历两个安全帧，不能沿用上一画布尚未退出的解释器。
/// @details 测试只观察生产产生的草稿，不在夹具中替它提交。
void begin(std::string_view profile = "touch-gestures")
{
    UI::Interaction::synchronizeProfile(profile, true);
    UI::Interaction::synchronizeProfile(profile, true);
    ImGui::CreateContext();
    auto& io                        = ImGui::GetIO();
    io.IniFilename                  = nullptr;
    io.DisplaySize                  = { 1000, 900 };
    io.DeltaTime                    = .05f;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels{};
    int            w{}, h{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
}
/// @brief 覆盖五条泳道轻点与抖动，STOP 也必须能创建普通时间点。
/// @details 每轮仅一次真实点击，绝不通过私有方法打开创建弹窗。
/// 轻点期间既没有 Shift 也没有模拟双击，必须由触控方案自行决定类型。
/// 若只忽略所有输入，普通点标志不会成立，因此不能空通过。
/// 若误同时创建段落，互斥草稿断言会失败，不能由点窗口掩盖错误。
void points()
{
    for ( int lane = 0; lane < 5; ++lane ) {
        begin();
        {
            Fixture f(Logic::EditTool::Draw);
            // 每条泳道独立用例，防止前一轮未确认弹窗阻挡下一轮输入。
            // 同时包括 STOP；它没有插值能力，但普通时间点创建不能被吞掉。
            // 3px 抖动小于手指容差，不能被误判为需要参数区间的拖动。
            // 起点与释放位置不同，能够发现把释放坐标当作起点的回归。
            // 断言区分两种编辑草稿，不允许同时打开点和段落参数窗口。
            const ImVec2 p{ 50 + lane * 100.0f, 220 };
            f.idle(p);
            f.frame(p, true);
            f.frame({ p.x + 3, p.y + 3 }, true);
            f.frame({ p.x + 3, p.y + 3 });
            f.idle(p);
            check(Access::point(f.m_canvas) && !Access::range(f.m_canvas),
                  "timing tap creates point only");
            // 横向抖动不改变类型，类型必须从真正的起笔泳道锁存。
            // 这里不直接给 m_createType 赋值，所有五种结果都来自生产映射。
            check(Access::type(f.m_canvas) == lane,
                  "timing tap retains original lane");
        }
        ImGui::DestroyContext();
    }
}
/// @brief 正反向快速拖动均建立区间，第二指中断则不能产生任何草稿。
/// @details 有效拖动在第二次按住帧已越过容差，之后释放必须正确收尾。
/// 区间不等于像素长度，期望结果根据生产投影的秒时间差计算。
/// 接管时保留相同的有效范围，不能以零长度退化冒充取消成功。
/// 原始第二指不改变鼠标坐标，能够发现仅检查鼠标抬手的错误实现。
/// 取消之后再次推进空闲帧，任何迟到的点弹窗同样判定失败。
void ranges()
{
    for ( int scenario = 0; scenario < 4; ++scenario ) {
        begin();
        {
            Fixture f(Logic::EditTool::Draw);
            // 横向跨道与纵向拖动同时存在，类型仍应由原点锁定。
            // 正向与反向覆盖区间排序，两端比较不能只观察非零时长。
            // 下方取消场景采用同一非退化范围，不能因范围为空偶然通过。
            const ImVec2 first{ 150, 220 },
                last{ 350, scenario == 1 ? 350.0f : 90.0f };
            f.idle(first);
            f.frame(first, true);
            f.frame(last, true);
            // 第二指在已经开始预览之后进入，专门覆盖事务中途接管。
            // 其中一轮先抬主指再抬第二指，另一轮在同帧全部结束。
            // 不通过右键或 Escape 伪造取消，必须让原始仲裁识别多指峰值。
            if ( scenario >= 2 ) f.frame(last, true, true);
            f.frame(last, false, scenario == 2);
            f.frame(last);
            f.idle(last);
            if ( scenario < 2 ) {
                // 区间长度独立从两次生产投影求差，不读取工作副本的起终点。
                // 快速松手与持续帧不同，末端必须包含抬手前最后一个触点。
                // 容差仅用于浮点比较，不用来隐藏掉拍或时间单位错误。
                const double expected =
                    std::abs(Access::time(f.m_canvas, first.y) -
                             Access::time(f.m_canvas, last.y));
                check(Access::range(f.m_canvas) && !Access::point(f.m_canvas),
                      "timing drag opens interpolation editor");
                check(std::abs(Access::duration(f.m_canvas) - expected) < 1e-6,
                      "timing range includes final touch position");
            } else {
                // 取消没有提交命令，也不能把剩余单指解释成另一笔轻点。
                check(!Access::point(f.m_canvas) && !Access::range(f.m_canvas),
                      "second finger cancels timing draft");
            }
        }
        ImGui::DestroyContext();
    }
}
/// @brief 轻点只选中，不提交零位移修改；真实拖动只发布一次整批移动。
/// @details 时间点放在 BPM 泳道中心，配置明确允许红线参与选择。
/// marker 的显示位置与逻辑秒时间独立设置，命中由生产拾取完成。
/// 移动命令必须整批提交，逐帧发送多条更新同样会触发计数断言。
/// 轻点容差内没有移动命令，但必须观察到具体选择高亮集合。
/// 第二指取消既不能提交最后位置，也不能让下一空闲帧迟到提交。
/// 事件订阅在场景销毁前退订，后续测试不会写入悬空计数器。
void selections()
{
    for ( int scenario = 0; scenario < 3; ++scenario ) {
        begin();
        {
            Fixture f(Logic::EditTool::Marquee);
            Common::Render::TimelineInteractiveElement marker{};
            marker.time      = 3.25;
            marker.y         = 150;
            marker.effects   = Common::Render::SCROLL_EFFECT_BPM;
            marker.bpmEntity = static_cast<entt::entity>(42);
            marker.bpmValue  = 120;
            f.m_snapshot.timelineElements.push_back(marker);
            int   moves = 0;
            auto& bus   = Event::EventBus::instance();
            // 订阅只存活于当前场景，退出前显式退订，不能引用已销毁计数器。
            // 不修改事件载荷或阻止其他观察者，保持生产消息边界。
            // 其他视口命令不计入移动数量，测试必须明确核对目标命令种类。
            const auto token =
                bus.subscribe<Event::LogicCommandEvent>([&](const auto& e) {
                    if ( std::holds_alternative<Logic::CmdUpdateTimelineEvents>(
                             e.command) )
                        ++moves;
                });
            // 命中点由快照提供；本场景不伪造选区，也不直接触发移动命令。
            const ImVec2 first{ 50, 150 }, last{ 50, 250 };
            f.idle(first);
            f.frame(first, true);
            if ( scenario ) f.frame(last, true);
            if ( scenario == 2 ) f.frame(last, true, true);
            f.frame(scenario ? last : first);
            f.idle(last);
            check(moves == (scenario == 1 ? 1 : 0),
                  "tap and cancelled timing drag leave history untouched");
            // 轻点必须保留实际选择，否则完全吞掉输入也会满足零修改断言。
            // 拖动成功与取消分别要求一条和零条批量更新，不能只核对总命令数。
            // 场景销毁前已经排空尾帧，迟到的 End 不会逃过本断言。
            if ( scenario == 0 )
                check(Access::count(f.m_canvas) == 1,
                      "timing tap selects marker");
            bus.unsubscribe<Event::LogicCommandEvent>(token);
        }
        ImGui::DestroyContext();
    }
}
/// @brief 一条真实触点产生的范围命令经过原动作栈，撤销／重做保留完整定义。
/// @details 不调用画布私有提交函数，载荷只能来自上面的 EventBus 观察。
/// @param submitted 真实触点发出的命令，不能用测试自己计算的新时间替代。
/// @param source 起笔定义，作为 Undo 的独立期望值。
/// @note 模型夹具没有先建历史，撤销必须只恢复这次端点调整。
/// @note 参数值与函数定义按值比较，不以渲染后的像素相似作为正确性证据。
void verifyRangeHistory(
    const Logic::CmdUpdateTimelineEvent&              submitted,
    const Common::Render::TimingInterpolationElement& source)
{
    Logic::SessionContext ctx;
    // SessionContext 中的注册表独立于 UI 夹具，验证的确是跨边界命令消费。
    // 内存谱面没有源路径，测试不加载工程或覆盖调用机器的工作区。
    ctx.currentBeatmap              = std::make_shared<BeatMap>();
    const auto               entity = ctx.timelineRegistry.create();
    Logic::TimelineComponent original{ source.time,
                                       source.effect,
                                       source.value };
    original.m_interpolation = source.interpolation;
    ctx.timelineRegistry.emplace<Logic::TimelineComponent>(entity, original);
    // 画布和模型夹具使用各自的代数句柄，仅替换路由身份，不改时间或曲线载荷。
    auto command   = submitted;
    command.entity = entity;
    Logic::ActionController actions(ctx);
    actions.handleCommand(command);
    // 先保留实际 after，再撤销，Redo 不能只因返回了非空组件就判定成功。
    // 时长、曲线及控制点一起比较，避免把段落错误退化成普通时间点。
    const auto after =
        ctx.timelineRegistry.get<Logic::TimelineComponent>(entity);
    check(
        after.m_timestamp == command.newTime &&
            after.m_interpolation == command.interpolationOverride,
        "touch range uses original model update and interpolation definition");
    // 相同参数和曲线形状只改变范围，不应该因为重新采样而生成多个历史节点。
    actions.handleCommand(Logic::CmdUndo{});
    const auto& before =
        ctx.timelineRegistry.get<Logic::TimelineComponent>(entity);
    check(before.m_timestamp == original.m_timestamp &&
              before.m_interpolation == original.m_interpolation,
          "one undo restores the complete timing range");
    actions.handleCommand(Logic::CmdRedo{});
    // 读取原实体，不能因动作生成了另一个同时间事件而掩盖身份丢失。
    const auto& redo =
        ctx.timelineRegistry.get<Logic::TimelineComponent>(entity);
    check(redo.m_timestamp == after.m_timestamp &&
              redo.m_interpolation == after.m_interpolation,
          "one redo restores the exact adjusted range");
}
/// @brief 选择首标记后从左右端点起手，区分端点、标记、普通点与取消边界。
/// @details 四个方向都从真实输入开始；不直接填充选区或调用端点事务。
/// @par 场景矩阵
/// - 0／1：首端缩短与延长，原末端必须保持精确时间。
/// - 2／3：末端缩短与延长，原起点不能随手指整体平移。
/// - 4：把手轻点，没有实际移动，不能创建空历史。
/// - 5：第二指接管已开始的预览，不能把残留第一指当作提交。
/// - 6：平台 Cancel 不等于正常 Up，必须丢弃候选。
/// - 7：同实体被协作修改，旧定义不能覆盖新值。
/// - 8：同路径换实例，也不能复用相同实体编号提交。
/// - 9：播放开始后拒绝原编辑，不通过修改播放位置继续拖动。
/// - 10：移动后返回起点，精确恢复原范围，不留下吸附微调。
/// - 11：扩大范围包含同泳道普通点，本地拒绝且不生成更新。
/// - 12：没有持续移动帧，最终 Up 仍必须携带最后端点。
/// - 13：首端跨过末端，只夹到 1 ms，不交换起终值。
/// - 14：长按把手不打开正文菜单，释放也不制造编辑。
/// - 15：切换到绘制工具，旧范围手势必须立即失效。
/// - 16：经典桌面从同位置起手，仍走原整组移动命令。
/// - 17：触屏从标记中央起手，仍保留正文整体移动空间。
/// @note 初始范围使用曲线与非默认采样设置，线性默认值不能掩盖字段丢失。
/// @note 只统计最终编辑命令，导航或视口通知不算作一次模型提交。
void resizeRanges()
{
    for ( int scenario = 0; scenario < 18; ++scenario ) {
        begin(scenario == 16 ? "classic-desktop" : "touch-gestures");
        {
            Fixture f(Logic::EditTool::Marquee);
            // 先建立真实画布投影；后面按像素定义期望，但不复制其换算公式。
            f.idle({ 150, 250 });
            Common::Render::TimingInterpolationElement source;
            source.entity = static_cast<entt::entity>(42);
            source.effect = TimingEffect::SCROLL;
            source.time   = Access::time(f.m_canvas, 350);
            source.value  = 1.25;
            source.interpolation.m_duration =
                Access::time(f.m_canvas, 150) - source.time;
            source.interpolation.m_endValue = 3.5;
            // 非默认贝塞尔与密度提供不变量，只有 duration 允许随端点改变。
            // 即使该曲线的视觉值接近直线，也不能在命令中替换为默认 Linear。
            source.interpolation.m_curve            = TimingCurve::Bezier;
            source.interpolation.m_samplesPerSecond = 33;
            source.interpolation.m_controlX1        = .12;
            source.interpolation.m_controlY2        = .81;
            source.interpolation.m_beatNumerator    = 3;
            source.interpolation.m_beatDenominator  = 8;
            // 段落正文和首标记指向同一实体，是生产快照的两种描述。
            // 两者都提供，防止只在人工孤立正文上通过而真实 marker 会抢走输入。
            f.m_snapshot.timingInterpolations.push_back(source);
            Common::Render::TimelineInteractiveElement row;
            row.time          = source.time;
            row.y             = 350;
            row.scrollEntity  = source.entity;
            row.scrollValue   = source.value;
            row.effects       = Common::Render::SCROLL_EFFECT_SCROLL;
            row.interpolation = source.interpolation;
            // Timing 首点与正文都在同一滚动域，实体编号不同会让结果失去意义。
            // 行结构的秒时间与插值的秒时长一致，不能误用原生文件毫秒时间戳。
            f.m_snapshot.timelineElements.push_back(row);
            // 段首标记中央轻点只选择，不应建立编辑或借用插值创建参数窗口。
            f.frame({ 150, 350 }, true);
            f.frame({ 150, 350 });
            f.idle({ 150, 350 });
            check(Access::count(f.m_canvas) == 1,
                  "range marker tap selects the existing range");
            std::vector<Logic::CmdUpdateTimelineEvent> updates;
            // 单项范围更新与整组平移必须分开观察，不能只累计所有 Timing 命令。
            // 订阅生命周期只覆盖这一笔，不能将夹具最初的选择动作算进来。
            int        bodies{};
            auto&      bus = Event::EventBus::instance();
            const auto token =
                bus.subscribe<Event::LogicCommandEvent>([&](const auto& e) {
                    if ( const auto* cmd =
                             std::get_if<Logic::CmdUpdateTimelineEvent>(
                                 &e.command) )
                        updates.push_back(*cmd);
                    if ( std::holds_alternative<Logic::CmdUpdateTimelineEvents>(
                             e.command) )
                        ++bodies;
                });
            const bool tail = scenario == 2 || scenario == 3;
            // 左右锚点位于泳道四分之一处；中央特例证明热区没有吃掉整个标记。
            const ImVec2 from{ scenario >= 16 ? 150.F
                               : tail         ? 175.F
                                              : 125.F,
                               tail ? 150.F : 350.F };
            ImVec2       to{ from.x,
                             tail ? (scenario == 2 ? 200.F : 100.F)
                                  : (scenario == 1 ? 400.F : 300.F) };
            if ( scenario == 13 ) to.y = 100;
            if ( scenario == 11 ) {
                // 同泳道普通点位于扩大范围内部；边界外的其他效果不能误报冲突。
                auto other         = row;
                other.scrollEntity = static_cast<entt::entity>(43);
                other.interpolation.reset();
                other.y    = 400;
                other.time = Access::time(f.m_canvas, 400);
                f.m_snapshot.timelineElements.push_back(other);
                to.y = 450;
            }
            f.idle(from);
            // 空闲仍推进原控制器，保证上一轻点的回放不会混入下一笔。
            // 真正 Down 发生在最终锚点，不用移动之后的坐标代替起点。
            f.frame(from, true);
            if ( scenario == 14 )
                // 固定 UI 时间步超过菜单阈值，不执行真实 sleep 或线程同步。
                for ( int tick = 0; tick < 14; ++tick ) f.frame(from, true);
            if ( scenario != 4 && scenario != 14 && scenario != 12 )
                f.frame(to, true);
            // 第二指、系统取消和会话变化必须丢弃这次 UI 预览，不能靠 Undo
            // 回滚。 变更原定义的场景模拟协作更新，旧值不能在抬手时覆盖回来。
            if ( scenario == 5 ) f.frame(to, true, true);
            if ( scenario == 7 )
                // 保留身份只改内容，不能被单纯的实体存在性检查漏过。
                f.m_snapshot.timingInterpolations.front().value = 9;
            if ( scenario == 8 ) f.m_snapshot.beatmapInstanceId = 2;
            if ( scenario == 9 ) f.m_snapshot.isPlaying = true;
            if ( scenario == 15 )
                f.m_snapshot.currentTool = Logic::EditTool::Draw;
            // 工具切换本身不是一次新的按下，不能从残留触点开始创建段落。
            // 播放与换实例也不通过人为清理画布状态帮助被测实现取消。
            if ( scenario == 10 ) {
                // 先跨过移动门槛再回原点，区别于从未移动的把手轻点。
                f.frame(from, true);
                to = from;
            }
            f.frame(scenario == 4 || scenario == 14 ? from : to,
                    false,
                    false,
                    scenario == 6);
            f.idle(to);
            // 排空四帧后观察，延后的 Begin／Release 不能在断言之后才补发更新。
            // 成功场景必须一条，单纯吞掉所有触点也会被明确判错。
            const bool success =
                scenario < 4 || scenario == 12 || scenario == 13;
            check(updates.size() == (success ? 1 : 0),
                  "endpoint range commits once or cancels without history");
            check(bodies == ((scenario == 16 || scenario == 17) ? 1 : 0),
                  "endpoint never also dispatches whole-selection translation");
            // 两个整体移动阳性对照证明未把原正文输入一并吞掉。
            // 命令种类不同，端点更新与旧批量平移不能互相抵消计数错误。
            if ( success && updates.size() == 1 ) {
                const auto& cmd = updates.front();
                check(
                    cmd.entity == source.entity &&
                        cmd.newValue == source.value &&
                        cmd.interpolationOverride.has_value(),
                    "range update retains the original entity and start value");
                if ( cmd.interpolationOverride ) {
                    // 仅允许 duration
                    // 改变，其他函数与输出密度字段逐项语义相等。
                    // 相反端点通过原范围独立计算，不读取被测对象的预览状态。
                    auto expected       = source.interpolation;
                    expected.m_duration = cmd.interpolationOverride->m_duration;
                    check(*cmd.interpolationOverride == expected,
                          "range resize preserves curve, density, end value "
                          "and beat settings");
                    check(
                        tail ? cmd.newTime == source.time
                             : std::abs(cmd.newTime +
                                        cmd.interpolationOverride->m_duration -
                                        source.time -
                                        source.interpolation.m_duration) < 1e-9,
                        "opposite endpoint remains fixed");
                    if ( scenario == 13 )
                        check(std::abs(expected.m_duration - .001) < 1e-9,
                              "crossing the other endpoint clamps to one "
                              "millisecond");
                    verifyRangeHistory(cmd, source);
                    // 真正动作栈验证在订阅退出前完成，模型与 UI 身份各自独立。
                }
            }
            bus.unsubscribe<Event::LogicCommandEvent>(token);
        }
        ImGui::DestroyContext();
    }
}
}  // namespace
/// @brief 内存会话提供原参数校验所需环境，完成后不写项目工作区。
int main()
{
    auto& settings   = MMM::Config::AppConfig::instance().getEditorSettings();
    const auto saved = settings;
    settings.professionalMode             = true;
    settings.timelineSelectionIncludesBpm = true;
    auto&      engine  = MMM::Logic::EditorEngine::instance();
    const auto session = engine.createSession(std::make_shared<MMM::BeatMap>(),
                                              "TouchTiming",
                                              false,
                                              "TouchTiming",
                                              true);
    // 三组用例共用同一个内存会话，只提供原参数窗口需要的校验环境。
    // 触点与窗口在各用例重建，不借用用户磁盘项目或正在运行的应用。
    // 桌面和纯鼠标原有套件继续独立运行，不能用触控成功替代兼容性验证。
    // 会话关闭明确禁止持久化，进程结束前恢复本测试改动的配置。
    points();
    ranges();
    selections();
    resizeRanges();
    engine.closeSession(session, false);
    settings = saved;
    return failures ? 1 : 0;
}
