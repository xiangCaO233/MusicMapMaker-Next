#include "common/LogicCommands.h"
#include "config/VisualConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/AudioPlotTouch.h"
#include "ui/workspace/MobileTouchScroll.h"
#include "ui/workspace/TouchReadPress.h"
#include <cmath>
#include <imgui.h>
#include <memory>
#include <variant>
#include <vector>
namespace
{
using namespace MMM;
using Phase = Graphic::TouchPhase;
int failures{};  ///< 场景分别累计，不能由最后一个阳性覆盖此前失败。
/// @brief 容差仅容纳归一化坐标误差，不能接受可见时间偏移。
bool near(double a, double b)
{
    return std::abs(a - b) < .0001;
}
/// @brief 检查命令与正式控制器结果，不读解释器私有阶段。
void check(bool value, const char* message)
{
    if ( !value ) {
        XERROR("Mobile audio: {}", message);
        ++failures;
    }
}
/// @brief 真正的 ImGui 帧与原始触点组成可重复、无音频设备的集成夹具。
/// @details 正式事件交给真实 PlaybackController，不手工设置 Scrub 成功状态。
struct Fixture {
    Logic::SessionContext     m_ctx;  ///< 本用例独占谱面，不关联用户活动工程。
    Logic::PlaybackController m_playback{
        m_ctx
    };  ///< 正式定位、夹取与结束语义。
    UI::AudioPlotTouch          m_nav;     ///< 两个声道共享的生产解释器。
    UI::TouchReadPress          m_read;    ///< 资源行共用的轻点／长按生产判定。
    UI::MobileTouchScroll       m_scroll;  ///< 保留页面滚动与水平控件方向仲裁。
    std::vector<Logic::CmdSeek> m_seeks;   ///< 有序记录预览和最终提交。
    Event::SubscriptionID       m_subscription{};  ///< 析构收尾后才注销。
    float  m_zoom{ 1 }, m_scrollY{};  ///< 半窗与父页面滚动分别检查。
    double m_clock{};                 ///< 仅原始事件时钟，不执行真实睡眠。
    bool   m_available{ true }, m_shifted{},
        m_resourceMode{};  ///< 模态、重排与资源测试入口。
    int m_taps{}, m_holds{},
        m_forbidden{};  ///< 只允许导航与明确的资源阅读结果。
    /// @brief 固定非零窗口原点、非零视觉偏移及足够长的音轨边界。
    // 除真实播放控制器外，不初始化音频输出或图形后端。
    // 字体图集只保留 CPU 数据，测试不会上传 Vulkan 纹理。
    // 同一帧两个声道按生产顺序提交，检验共享归属而非两个独立分类器。
    // 较长 Dummy 使父页面确实可滚动，不能以无滚动页面掩盖方向冲突。
    // 事件订阅捕获所有正式命令，未知编辑或历史命令必须立即报错。
    // 预热帧建立窗口与滚动范围，不依赖第一次 Down 恰好创建窗口。
    Fixture()
    {
        // 每个场景串行拥有当前 UI 上下文，销毁前先完成导航收尾。
        // 字体、窗口和鼠标事件都在该上下文内，不复用另一场景的 ActiveID。
        // 配置不写个人目录，CTest 包装器已经提供隔离配置根。
        ImGui::CreateContext();
        auto& io                        = ImGui::GetIO();
        io.IniFilename                  = nullptr;
        io.DisplaySize                  = { 900, 900 };
        io.DeltaTime                    = .02f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels{};
        int            w{}, h{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        UI::Interaction::synchronizeProfile("touch-gestures", true);
        UI::Interaction::synchronizeProfile("touch-gestures", true);
        // 原始跟踪器也归本场景重置，不能借用上一测试保留的 Up 代际。
        // 正式控制器接收实例约束命令后才更新当前时间，断言不直接驱动时钟。
        Graphic::cancelMobileTouch();
        // 临时谱面不注册到后台引擎，避免其他线程改变测试时刻。
        // 正式范围检查仍基于真实对象，而不是只改一份预期时间。
        // 较长音轨边界避免错误映射都被同一个曲尾夹取掩盖。
        m_ctx.currentBeatmap         = std::make_shared<BeatMap>();
        m_ctx.audioTimelineTotalTime = 100;
        m_ctx.currentTime            = 10 - .25;
        m_ctx.lastConfig.visual.visualOffset =
            .25f - Config::VisualConfig::staticVisualOffset;
        // 事件订阅先于任何触点输入，记录顺序与正式命令队列一致。
        // 最后结束命令在析构时仍会被记录，不允许释放阶段藏起失败。
        // 除 Seek 外的动作全部计错，包括意外产生的鼠标预览命令。
        m_subscription =
            Event::EventBus::instance().subscribe<Event::LogicCommandEvent>(
                [&](const auto& e) {
                    if ( const auto* seek =
                             std::get_if<Logic::CmdSeek>(&e.command) ) {
                        m_seeks.push_back(*seek);
                        m_playback.handleCommand(*seek);
                    } else
                        ++m_forbidden;
                });
        frame();
        frame();
    }
    /// @brief 先收尾再拆订阅；避免通过销毁上下文掩盖未结束 Scrub。
    // 导航不能污染主画布时间线倍率，也不能依赖 Undo 来清理本测试。
    // 独占临时谱面在夹具退出时释放，所有配置由 CTest 隔离根接收。
    ~Fixture()
    {
        // 先收尾再注销订阅，使结束命令同样接受正式控制器检查。
        // 销毁上下文不能代替结束 Scrub，仍需明确检查业务状态。
        // 只读导航不会改写音符或项目模型，不用清理用户工程。
        m_nav.suspend();
        check(!m_ctx.isSeekScrubbing, "Scrub ends on destruction");
        check(!m_forbidden, "no editing, mouse-preview or history command");
        check(near(m_ctx.lastConfig.visual.timelineZoom, 1),
              "main canvas scale preserved");
        Event::EventBus::instance().unsubscribe<Event::LogicCommandEvent>(
            m_subscription);
        ImGui::DestroyContext();
    }
    /// @brief 事件可以单独发布，允许一个 UI 帧包含完整 Down／Up。
    void event(Phase p, int finger, float x, float y)
    {
        Graphic::publishMobileTouch(p,
                                    42,
                                    static_cast<std::uint64_t>(finger),
                                    x / 900,
                                    y / 900,
                                    m_clock += .02);
    }
    /// @brief 正式 NewFrame／滚动过滤／控件提交／恢复输入顺序。
    // 起点命中使用真实窗口悬浮，连续捕获允许离开矩形。
    // 资源测试仍提交实际 Selectable，但业务动作只采用原始阅读结果。
    // 不直接修改 ActiveID、点击计数或解释器的所有者字段。
    // 可见几何改变明确传给生产入口，不能通过换个预期值伪造取消。
    // 父页面滚动值单独读回，纵滑没有 Seek 仍不等于页面确实收到输入。
    void frame(float x = 200, float y = 220, bool down = false)
    {
        auto& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, down);
        ImGui::NewFrame();
        m_scroll.beginFrame();
        ImGui::SetNextWindowPos({ 50, 50 });
        ImGui::SetNextWindowSize({ 600, 600 });
        ImGui::Begin("AudioTouchFixture", nullptr, ImGuiWindowFlags_NoMove);
        // 显示中心由正式当前时间加通用偏移得到，不读取 Hover。
        // 两个声道使用同一帧中心，避免半窗改变后左右声道取不同数据。
        // 本测试的非零原点要求屏幕坐标先经过实际矩形归一化。
        const double center = m_nav.begin(
            m_ctx.currentTime + .25,
            reinterpret_cast<std::uintptr_t>(m_ctx.currentBeatmap.get()));
        // 两个固定矩形即可检验所有者争用，无需构建 PCM 或 FFT。
        // 同一窗口也有两个声道，因此窗口悬浮不足以授予目标归属。
        // 图间保留空隙，标题与控件之外的落点不能变成轻点。
        for ( int channel = 0; channel < 2; ++channel ) {
            ImVec2 min{ m_shifted ? 120.0f : 100.0f, 200.0f + 200 * channel },
                max{ min.x + 400, min.y + 140 };
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(channel);
            if ( m_resourceMode ) {
                (void)ImGui::Selectable("resource", false, 0, { 400, 140 });
                // 行身份采用稳定 ID，列表位置不参与动作识别。
                // 起点之外经过另一行不能转交已经锁存的手势。
                // 当前目标几何失效时由 m_available 明确否决剩余输入。
                const bool hit = UI::TouchReadPress::originInside(min, max);
                const auto result =
                    m_read.update(100 + channel,
                                  hit && ImGui::IsWindowHovered(),
                                  m_available && hit);
                m_taps += result == UI::TouchReadPress::Result::Tap;
                m_holds += result == UI::TouchReadPress::Result::Hold;
            } else {
                ImGui::InvisibleButton("plot", { 400, 140 });
                m_scroll.registerHorizontalDragItem();
                // 生产入口传入当前绘制范围，而非推算一个固定像素每秒系数。
                // 输入可超过一次 UI
                // 更新，原始跟踪器仍保留峰值与正常释放尾坐标。
                // 业务回调同步消费正式命令，使最终时间与 Scrub 标志能独立验证。
                m_nav.update(
                    { min,
                      max,
                      center,
                      100,
                      .25,
                      reinterpret_cast<std::uintptr_t>(
                          m_ctx.currentBeatmap.get()),
                      channel,
                      ImGui::IsWindowHovered(
                          ImGuiHoveredFlags_AllowWhenBlockedByActiveItem),
                      m_available },
                    m_zoom);
            }
            ImGui::PopID();
        }
        // 明确建立真正可滚动内容，仍由父窗口夹取上下边界。
        // 不手工设置预期滚动量，必须确认纵向意图实际送达父页面。
        // 读回值与无 Seek 同时成立，才说明方向路由符合设计。
        ImGui::SetCursorPosY(1500);
        ImGui::Dummy({ 1, 1 });
        m_scrollY = ImGui::GetScrollY();
        ImGui::End();
        m_scroll.endFrame();
        ImGui::Render();
    }
    /// @brief 单事件后推进一帧；多指状态始终来自正式跟踪器。
    void step(Phase p, int finger, float x, float y)
    {
        // 逐根状态由正式跟踪器维护，不根据预期反推手指数。
        // 正常尾事件保留，检查取消后的残留不会产生额外操作。
        // 按住标志取真实活跃数量，不能手工伪装成另一阶段。
        event(p, finger, x, y);
        frame(x, y, Graphic::mobileTouchSnapshot().m_count != 0);
    }
};
/// @brief 轻点按实际声道和偏移映射，空闲帧不能重复定位。
// 第二声道范围固定为 9～11 秒，四分之三落点扣偏移后为 10.25 秒。
// Down 保持无命令是阳性前提，按下即定位不能通过。
// 空闲帧检查跟踪器保留的 Up 不被后续 UI 重复消费。
// 快速拖动没有中间 Move，正常 Up 是唯一有效位移来源。
// 预览和最终命令顺序必须保持，Scrub 不能持续到下一笔。
// 后一笔以先前真实定位为中心，不能重新写回初始时间。
// 没有音频硬件推进时钟，结果只来自正式播放控制器。
// 两段输入共用对象，不能靠重新构造导航掩盖重复交付。
// 结果同时要求命令数量和准确时间，吞掉全部触点也不能通过。
// 偏移与缩放分开检查，避免错误相互抵消后最终数值碰巧正确。
void tapAndFastDrag()
{
    Fixture f;
    f.step(Phase::Down, 1, 400, 450);
    check(f.m_seeks.empty(), "Down does not seek");
    f.step(Phase::Up, 1, 400, 450);
    check(f.m_seeks.size() == 1 && near(f.m_ctx.currentTime, 10.25),
          "second-channel tap and visual offset");
    f.frame();
    check(f.m_seeks.size() == 1, "idle does not replay tap");
    // 合帧拖动必须先交付最终位置，再交付一次正式结束。
    // 本测试不依靠中间 Move，正常 Up 的末坐标是唯一有效位移来源。
    // 先只发布原始 Down，不画中间 UI 帧，模拟事件泵合批。
    // Up 的末坐标不同于起点，若丢失尾点就只会停在上一时间。
    // 随后唯一一帧必须同时交付预览和结束，不能等下一次真实触摸。
    f.event(Phase::Down, 2, 200, 220);
    f.event(Phase::Up, 2, 300, 220);
    f.frame(300, 220);
    check(f.m_seeks.size() == 3 && !f.m_ctx.isSeekScrubbing,
          "fast drag publishes preview and finish");
    check(near(f.m_ctx.currentTime, 9.75),
          "fast drag preserves final coordinate");
}
/// @brief 横向浏览与纵向滚动必须互斥，Cancel 不采纳自身异常坐标。
// 纵向输入从声道内起笔，父窗口具有非零滚动范围。
// 两轴差异明确超过死区，不把手抖当作滚动阳性。
// 横向输入属于新代际，前一笔拒绝不能永久锁住图表。
// 检查正式 isSeekScrubbing，不以命令数量代替业务收尾。
// Cancel 故意携带远离最后位置的坐标，发现误采纳取消落点。
// 后续无效 Up 仍由跟踪器发布，不手工清空分类器。
// 取消保留已显示浏览，不要求用户上一笔编辑产生 Undo。
// 父滚动和定位分别检查，吞掉输入不能使测试通过。
// 位置不变的尾事件也必须走结束路径，不能等待下一次触摸。
// 不改变系统手势开关，测试仅模拟应用可以收到的取消事件。
void directionAndCancel()
{
    Fixture f;
    f.step(Phase::Down, 1, 200, 300);
    f.step(Phase::Move, 1, 202, 220);
    f.step(Phase::Up, 1, 202, 220);
    f.frame();
    check(f.m_seeks.empty(), "vertical m_scroll does not seek");
    check(f.m_scrollY > 1, "vertical m_scroll reaches actual parent");
    f.step(Phase::Down, 2, 200, 220);
    f.step(Phase::Move, 2, 280, 220);
    const auto last = f.m_ctx.currentTime;
    check(f.m_ctx.isSeekScrubbing, "horizontal drag begins Scrub");
    f.step(Phase::Cancel, 2, 700, 700);
    check(near(f.m_ctx.currentTime, last) && !f.m_ctx.isSeekScrubbing,
          "Cancel retains last valid target");
    const auto count = f.m_seeks.size();
    f.step(Phase::Up, 2, 700, 700);
    check(f.m_seeks.size() == count, "cancelled Up is not a new tap");
}
/// @brief 双指只能来自同一声道，平移／缩放不发送 Undo 或鼠标命令。
// 第二指起初与第一指同声道，配对本身不能产生时间变化。
// 两次 Move 来自不同指针，第一指旧起点不能冒充中点。
// 缩放检查真实半窗，不以内部阶段变化代替几何已更新。
// 另一指仍按住时结束第二指，立即要求唯一的最终提交。
// 残留指移动再释放，数量不变，防止二次点击和导航接力。
// 跨声道保持第二指始终位于下方图表，不靠边界模糊判定。
// 两个夹具串行拥有当前 ImGui 上下文，不同时销毁全局上下文。
// 跨声道拒绝后完整抬手，不能把未释放当作无命令的原因。
// 两个声道共享同一实例，因此只检查实例不足以阻止错误接管。
// 夹具禁止 Undo 命令，两个手指不能意外复用主画布历史手势。
void pairAndBoundaries()
{
    {
        Fixture f;
        f.step(Phase::Down, 1, 200, 220);
        f.step(Phase::Down, 2, 400, 220);
        check(f.m_seeks.empty(), "second finger does not jump");
        f.step(Phase::Move, 1, 160, 220);
        f.step(Phase::Move, 2, 440, 220);
        check(f.m_zoom < .9f && f.m_zoom >= .1f,
              "pinch narrows the time window");
        const auto count = f.m_seeks.size();
        f.step(Phase::Up, 2, 440, 220);
        check(f.m_seeks.size() == count + 1 && !f.m_ctx.isSeekScrubbing,
              "first pair release finishes once");
        f.step(Phase::Move, 1, 300, 220);
        f.step(Phase::Up, 1, 300, 220);
        check(f.m_seeks.size() == count + 1, "remaining finger has no replay");
        // 另一声道即使属于同一音轨，也不能参与已经起笔的双指导航。
        // 两个图表区域之间保留空隙，测试不会用边界模糊来绕过归属。
    }
    Fixture g;
    g.step(Phase::Down, 1, 200, 220);
    g.step(Phase::Down, 2, 400, 450);
    g.step(Phase::Move, 2, 450, 450);
    g.step(Phase::Up, 2, 450, 450);
    g.step(Phase::Up, 1, 200, 220);
    check(g.m_seeks.empty(), "cross-channel pair rejected");
}
/// @brief 外部起点、重排、失效与方案切换分别封锁残留触摸。
// 起点在窗口外，最终抬手在图内也不能得到点击。
// 重排只改变几何，保留指针和实例以独立检查坐标口径失效。
// 模态先建立有效横向浏览，再撤去许可，覆盖已开始事务。
// 待定轻点与 Scrub 中断分别断言，不能统一补发定位。
// 经典方案由宿主原路径处理，触控解释器停止发布命令。
// 切回触控先空闲一帧，避免把切换期间残留输入当成新 Down。
// 最后只替换真实 BeatMap 对象，旧手势不得按窗口名接续。
// 切谱前没有移动，旧 Up 被拒绝时也没有需要收尾的预览。
// 测试不依赖文件路径改变，同路径重新加载也应具有新实例。
// 没有等待或线程暂停，所有中断在对应输入帧立即推进。
void interruption()
{
    Fixture f;
    f.step(Phase::Down, 1, 700, 220);
    f.step(Phase::Up, 1, 200, 220);
    check(f.m_seeks.empty(), "outside origin cannot enter");
    f.step(Phase::Down, 2, 200, 220);
    f.m_shifted = true;
    f.frame(200, 220, true);
    f.step(Phase::Up, 2, 200, 220);
    check(f.m_seeks.empty(), "geometry invalidates candidate");
    f.m_shifted = false;
    f.step(Phase::Down, 3, 200, 220);
    f.step(Phase::Move, 3, 250, 220);
    // 模态接管发生在已开始拖动之后，必须结束最后的 Scrub。
    // 撤去许可不替换图表几何，因此不会由其他检查碰巧满足取消断言。
    // 无效 Up 继续传入，防止重新关闭模态时旧点击意外生效。
    f.m_available = false;
    f.frame(250, 220, true);
    check(!f.m_ctx.isSeekScrubbing, "modal interruption ends Scrub");
    f.step(Phase::Up, 3, 250, 220);
    f.m_available    = true;
    const auto count = f.m_seeks.size();
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    f.step(Phase::Down, 4, 200, 220);
    f.step(Phase::Up, 4, 400, 220);
    check(f.m_seeks.size() == count,
          "classic profile uses original host input");
    UI::Interaction::synchronizeProfile("touch-gestures", true);
    f.frame();
    f.step(Phase::Down, 5, 200, 220);
    f.m_ctx.currentBeatmap = std::make_shared<BeatMap>();
    f.step(Phase::Up, 5, 400, 220);
    check(f.m_seeks.size() == count, "new chart rejects prior candidate");
}
/// @brief 资源长按只交付一次，滑动或多指后不能回放行点击。
// 两个真实 Selectable 与图表共用父页面滚动过滤流程。
// 业务只采用原始阅读结果，不从鼠标回放推断资源操作。
// DeltaTime 推进非阻塞时钟，测试不真实等待半秒。
// 超过阈值后继续按住一帧，菜单结果仍只能一次。
// Up 后再绘制空闲帧，长按不能附带一次控制器打开。
// 后续短按阳性必须成功，取消不能永久吞掉资源选择。
// 滑动使用另一代触点并超过峰值阈值，整段都不能交付动作。
// 两指逐根抬起，检查峰值不会在首指移除后遗失。
// 阅读资源不发布 Seek，析构时未知命令检查同时拒绝编辑泄漏。
// 这里验证动作分类，文件重命名和类型转换仍由各自原业务入口负责。
void resourceRead()
{
    Fixture f;
    // 资源列表模式保留同一个全局方向仲裁器，不用独立简化输入桩。
    // 长按行为与图表手势使用各自状态，资源菜单不能发布音频浏览命令。
    f.m_resourceMode = true;
    f.frame();
    f.step(Phase::Down, 1, 200, 220);
    // 只推进 ImGui 的非阻塞时间，不睡眠、不打开系统手势设置。
    ImGui::GetIO().DeltaTime = .2f;
    for ( int i = 0; i < 4; ++i ) f.frame(200, 220, true);
    check(f.m_holds == 1 && !f.m_taps, "stationary hold delivered once");
    f.step(Phase::Up, 1, 200, 220);
    f.frame();
    check(!f.m_taps, "hold Up does not open controller");
    f.step(Phase::Down, 2, 200, 450);
    f.step(Phase::Up, 2, 200, 450);
    check(f.m_taps == 1, "positive resource tap delivered");
    f.step(Phase::Down, 3, 200, 300);
    f.step(Phase::Move, 3, 200, 220);
    f.step(Phase::Up, 3, 200, 220);
    check(f.m_taps == 1 && f.m_holds == 1, "resource m_scroll has no action");
    f.step(Phase::Down, 4, 200, 220);
    f.step(Phase::Down, 5, 300, 220);
    f.step(Phase::Up, 5, 300, 220);
    f.step(Phase::Up, 4, 200, 220);
    check(f.m_taps == 1, "resource multi-touch rejected");
}
}  // namespace
/// @brief 执行图表导航和资源阅读的交叉模块回归，CTest 隔离全部配置输出。
// 各夹具独立创建谱面和上下文，用例间不传递浏览状态。
// 累计结果保留此前错误，不能因最后一个阳性成功而重置。
// 所有订阅均在退出时解除，不影响同一套件的其他测试入口。
int main()
{
    tapAndFastDrag();
    directionAndCancel();
    pairAndBoundaries();
    interruption();
    resourceRead();
    return failures ? 1 : 0;
}
