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
#include "mmm/beatmap/BeatMap.h"
#include "presentation/TimelineInteractionPresentation.h"
#include "presentation/mobile/MobileTimelineInteractionPresentation.h"
#include "ui/interaction/EditorInteraction.h"
#include <cmath>
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
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
    void frame(ImVec2 local, bool down = false, bool second = false)
    {
        // 输入先用绝对屏幕逻辑像素进入 ImGui，再归一化发布给原始触点层。
        // 两条输入流采用同一个位置和顺序，不能用已经合成的鼠标代替原始峰值。
        // 固定偏移来自测试窗口的真实内容原点，后面仍从窗口读正式原点。
        const ImVec2 point{ local.x + 8, local.y + 27 };
        using Graphic::TouchPhase;
        Graphic::publishMobileTouch(
            down ? (m_down ? TouchPhase::Move : TouchPhase::Down)
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
void begin()
{
    UI::Interaction::synchronizeProfile("touch-gestures", true);
    UI::Interaction::synchronizeProfile("touch-gestures", true);
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
    engine.closeSession(session, false);
    settings = saved;
    return failures ? 1 : 0;
}
