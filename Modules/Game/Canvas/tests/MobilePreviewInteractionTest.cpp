/// @file MobilePreviewInteractionTest.cpp
/// @brief 原始触点经过真实 ImGui 帧、页面仲裁与播放控制器的预览导航回归。
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "config/VisualConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "presentation/PreviewInteractionPresentation.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/MobileTouchScroll.h"
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <limits>
#include <memory>
#include <variant>
#include <vector>

namespace
{
using namespace MMM;
using P = Graphic::TouchPhase;
int failures{};  ///< 各场景独立累计，不以最后一次阳性覆盖此前失败。
/// @brief 断言外部可观察的命令、时间与范围，不读取分类器私有状态。
void check(bool value, const char* message)
{
    if ( !value ) {
        XERROR("Preview touch: {}", message);
        ++failures;
    }
}
/// @brief 容差只覆盖归一化浮点转换，不能容忍明显的时间漂移。
bool near(double a, double b)
{
    return std::abs(a - b) < .0001;
}

/// @brief 隔离谱面、相机与 UI；CTest 为本用例提供独立 MMM_CONFIG_ROOT。
/// @details 只对正式预览命令和 Seek 调用真实控制器，其余任何命令均报错。
/// 不启动音频或 Vulkan，不加载用户工程，不把截图存在当作业务成功。
// 夹具拥有自己的会话上下文，任何定位都不会发送到用户当前谱面。
// 真实播放控制器在同一线程消费捕获的正式事件，能检查有序提交和 Scrub 收尾。
// UI 仍经过标准 NewFrame 和全局页面仲裁，不直接调用分类器内部方法。
// 音频控制权保持关闭，因此时间夹取可以验证而不打开输出设备。
// 快照故意独立于逻辑状态，后续用例能观察滞后快照是否造成位置回跳。
// 命令计数区分密度轴直接 Seek 与内容区投影导航，不只检查最终时间相等。
// 上下文中的主相机横移设为非零，用来发现误路由到主画布的输入。
// 每个场景都有全新对象，前一笔完成不能掩盖下一笔没有真正开始。
// 测试配置根由 CTest 包装器隔离，释放时的布局保存不触碰个人配置。
struct Fixture {
    Logic::SessionContext     context;  ///< 当前测试独占的逻辑状态。
    Logic::PlaybackController playback{
        context
    };  ///< 正式定位、钳制和 Scrub 生命周期。
    Common::Render::RenderSnapshot
        snapshot;  ///< 本帧显示数据，不从全局引擎借用指针。
    std::unique_ptr<Canvas::PreviewInteractionPresentation>
                                    view;  ///< 正式移动输入策略。
    Canvas::PreviewInteractionFrame area;  ///< 与绘制相同的内容和密度轴边界。
    UI::MobileTouchScroll
        scroll;  ///< 保留全局页面滑动仲裁，不绕过鼠标延后路径。
    std::vector<Logic::LogicCommand> commands;        ///< 按真实发布顺序记录。
    Event::SubscriptionID            subscription{};  ///< 视图析构前解除订阅。
    double clock{};                      ///< 固定输入事件时钟，无真实等待。
    int    nav{}, seeks{}, forbidden{};  ///< 分开检测导航、密度定位与编辑泄漏。
    /// @brief 线性基线加上非零视口原点，使屏幕与局部坐标混用立即暴露。
    // 先创建 ImGui 上下文，再构建依赖字体尺寸的生产输入策略。
    // 事件分帧节流关闭，使一个测试帧对应明确的一组原始事件。
    // 原始跟踪器先取消上一上下文残留，不依赖默认手指编号碰巧不同。
    // 新 Down 会建立新代际，取消状态不能永久吞掉后续阳性手势。
    // 所有绘制都使用 CPU 字体图集，不创建纹理上传或物理窗口。
    // 谱面只提供实例身份和轨数；时间范围由独立的会话时长明确控制。
    // 上下边距清零是固定测试前置条件，不修改生产默认边距。
    // 有效视觉偏移设为零以便第一组算术检查，后续显示补偿另作非零验证。
    // 空时间线由正式 ScrollCache 构建，测试不手工修改积分数组。
    Fixture()
    {
        ImGui::CreateContext();
        auto& io                        = ImGui::GetIO();
        io.IniFilename                  = nullptr;
        io.DisplaySize                  = { 1000, 900 };
        io.DeltaTime                    = .02f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels{};
        int            w{}, h{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        UI::Interaction::synchronizeProfile("touch-gestures", true);
        UI::Interaction::synchronizeProfile("touch-gestures", true);
        Graphic::cancelMobileTouch();
        context.currentBeatmap = std::make_shared<BeatMap>();
        context.currentBeatmap->m_baseMapMetadata.track_count = 4;
        context.trackCount                                    = 4;
        context.audioTimelineTotalTime                        = 100;
        context.currentTime = context.animateTime = 10;
        auto& visual                              = context.lastConfig.visual;
        visual.trackLayout.top                    = 0;
        visual.trackLayout.bottom                 = 1;
        visual.judgeline_pos                      = .5f;
        visual.visualOffset = -Config::VisualConfig::staticVisualOffset;
        visual.previewConfig.areaRatio  = 10;
        visual.previewConfig.margin.top = visual.previewConfig.margin.bottom =
            0;
        // 预览高度六百、主画布有效高度六百、范围十，因此缩放恰为十分之一。
        // 这组尺寸能直接发现把预览像素当成主画布像素使用的错误。
        // 主画布横向偏移只作观察对照，预览命令不应触碰该值。
        // 滚速缓存由正式配置和注册表生成，支持随后添加真实变速事件再重建。
        // 时长足够容纳导航目标，不能因夹到曲尾让不同错误映射看起来相等。
        // 非零窗口原点要求输入先从屏幕坐标转为内容局部坐标。
        // 密度轴与内容区留有间隔，避免边界误差让两者同时认领触点。
        // 显示快照只提供绘制上下文，不作为逻辑控制器当前时间的权威。
        // 这些前置值可精确计算期望，不需要等待异步线程或观察私有标志。
        context.cameras.emplace(
            "Basic2DCanvas",
            Logic::CameraInfo{ "Basic2DCanvas", 1000, 600, 17 });
        auto& cache = context.timelineRegistry.ctx()
                          .emplace<Logic::System::ScrollCache>();
        cache.rebuild(context.timelineRegistry,
                      context.lastConfig,
                      context.currentBeatmap.get());
        // 主相机横移和时间线倍率是预览导航必须保持的对照值。
        Config::AppConfig::instance().getVisualConfig() = visual;
        snapshot.hasBeatmap                             = true;
        snapshot.beatmapInstanceId =
            reinterpret_cast<std::uintptr_t>(context.currentBeatmap.get());
        snapshot.currentTime             = 10;
        snapshot.renderScaleY            = .1f;
        snapshot.previewDensity.duration = 100;
        area                             = { &snapshot,
                                             { 100, 100 },
                                             { 400, 600 },
                                             { 520, 100 },
                                             { 560, 700 },
                                             0,
                                             1,
                                             true,
                                             true };
        view = Canvas::createMobilePreviewInteractionPresentation();
        // 订阅位于视图创建之后，但在任何测试触点发布之前生效。
        // 每个回调先记录原载荷，再由真实控制器消费，保留事件顺序。
        // 内容导航命令必须明确写 Preview，不能以空 ID 回退到活动主画布。
        // 未知命令不尝试自动处理，任何编辑或历史操作都计作污染。
        // 密度轴 Seek 同样经过真实时间范围和视觉偏移约束。
        // 回调不启动后台任务，退出帧后可以立刻检查上下文状态。
        // 固定数量的事件才进入向量，测试日志与生产热路径不共用数据结构。
        // 两次预热帧建立真实 ImGui 窗口，避免首帧布局和焦点影响阳性对照。
        // 订阅将在上下文销毁之前解除，析构收尾仍能被记录和验证。
        subscription =
            Event::EventBus::instance().subscribe<Event::LogicCommandEvent>(
                [&](const auto& e) {
                    commands.push_back(e.command);
                    if ( const auto* command =
                             std::get_if<Logic::CmdPanCanvas>(&e.command) ) {
                        check(command->cameraId == "Preview",
                              "navigation targets Preview only");
                        ++nav;
                        playback.handleCommand(*command);
                    } else if ( const auto* command =
                                    std::get_if<Logic::CmdSeek>(&e.command) ) {
                        ++seeks;
                        playback.handleCommand(*command);
                    } else
                        ++forbidden;
                });
        frame();
        frame();
    }
    /// @brief 先取消草稿和结束导航，再释放订阅和上下文，避免析构悬挂 Scrub。
    // 策略持有的最后有效导航必须先结束，不能让其访问已销毁的上下文。
    // 释放阶段如果还产生谱面编辑命令，同样需要报告失败。
    // 主相机横移和全局时间线倍率分别检查，不以谱面计数未变代替视图隔离。
    // 独立预览倍率允许改变，因此不把这种布局变化误判为全谱缩放。
    // 事件订阅比视图更长寿，保证结束命令仍走正式消费路径。
    // 视图销毁后才销毁 ImGui，避免释放函数读取失效输入上下文。
    // 每个用例的全局原始手势将在下一夹具初始化时取消。
    // 不调用 Undo 清理定位，导航本身没有创建或删除物件。
    // 本地配置保存由测试根接收，不要求修改用户的应用设置。
    ~Fixture()
    {
        view->suspend();
        check(forbidden == 0, "no editing or history command from Preview");
        check(near(context.cameras.at("Basic2DCanvas").horizontalOffsetX, 17),
              "main camera stays fixed");
        check(near(context.lastConfig.visual.timelineZoom, 1),
              "main timeline zoom stays fixed");
        Event::EventBus::instance().unsubscribe<Event::LogicCommandEvent>(
            subscription);
        view.reset();
        ImGui::DestroyContext();
    }
    /// @brief 单独发布事件，允许一帧内发生 Down、Move、Up 的完整手势。
    // 手指身份使用稳定设备和 finger 编号，不能根据数组当前位置合成事件。
    // 归一化只发生于平台输入边界，生产策略再按实际显示宽高还原。
    // 固定时钟只排序输入，不读取机器性能或墙钟。
    // 一次事件之后是否立即绘制由调用场景决定。
    // 普通手势逐事件推进，快速手势则把完整生命周期合在同一帧。
    // Up 与 Cancel 保留不同阶段，不能通过统一清空触点来替代。
    // 第二指和第三指也使用同一正式输入入口，不伪造鼠标右键。
    // 当前测试只使用一台虚拟设备，不声称覆盖厂商系统抢占行为。
    // 实际 Android 后端和界面几何将在独立设备验证中另行确认。
    void event(P phase, int finger, float x, float y)
    {
        Graphic::publishMobileTouch(
            phase, 91, finger, x / 1000, y / 900, clock);
        clock += .02;
    }
    /// @brief UI 输入与原始触点来自同一坐标，不直接篡改 ActiveId 或点击结果。
    // 原始输入可以比 UI 更快，这个函数只推进一次正式帧生命周期。
    // 鼠标来源明确标记为触屏，让页面仲裁处理真实的延后鼠标回放路径。
    // 父窗口禁止移动和滚动，画布内容应拥有连续导航输入。
    // 业务归属矩形独立于手指当前位置，因此拖出后可以继续收尾。
    // 外部覆盖用例改变持续许可，不能通过改变鼠标坐标冒充系统取消。
    // 实际策略只读当前输入和快照；本函数不手工设置它的阶段枚举。
    // 仲裁器的 begin/end 与 ImGui NewFrame/Render 成对执行。
    // 每帧完成后检查的是已发布命令和真实会话时间。
    // 没有 GPU、音频时钟或睡眠等待参与这些状态转换。
    void frame(float x = 200, float y = 300, bool down = false)
    {
        auto& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, down);
        ImGui::NewFrame();
        scroll.beginFrame(1);
        ImGui::SetNextWindowPos({ 50, 50 });
        ImGui::SetNextWindowSize({ 650, 700 });
        ImGui::Begin("PreviewFixture",
                     nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse);
        // 正式策略仍读取全局触点；窗口归属在边界场景由 area 明确设置。
        // 不把合成鼠标的 clicked/released 当作原始手势生命周期。
        view->update(area);
        ImGui::End();
        scroll.endFrame();
        ImGui::Render();
    }
    /// @brief 同时推进输入和 UI 的普通路径；合帧用例则显式分开调用。
    // 普通路径在输入发布之后立即绘制，模拟事件泵后的应用更新。
    // 按钮按下状态由当前活跃触点数量取得，不从预期命令结果反推。
    // 松手帧仍会执行策略，保证最后位置和最终提交都可被消费。
    // 快速合帧用例绕过的只有这个便利包装，不绕过生产跟踪器。
    // 取消也通过同一包装，不事后重置策略来让断言通过。
    // 多指逐根进入和释放，能够发现第二指先抬后剩余指的误回放。
    // 单指移动与多指导航使用同一窗口，不能靠销毁窗口隔离错误。
    // 空闲帧另外调用 frame，用来发现重复事件消费。
    // 因此吞掉所有输入、所有 Up 或全部取消都无法通过阳性对照。
    void step(P phase, int finger, float x, float y)
    {
        event(phase, finger, x, y);
        frame(x, y, Graphic::mobileTouchSnapshot().m_count != 0);
    }
};

/// @brief 点击只在 Up 定位，实际显示补偿与视觉偏移都进入正式反投影。
/// @details 故意把 snapshot.hoveredTime 设为错误值，防止重新依赖滞后 Hover。
/// 释放后空闲帧不得重复 Seek；主画布没有编辑命令也没有横移。
// 第一组先验证按下无副作用，再检查正常释放只有一个定位命令。
// 快照 Hover 时间故意偏离实际落点，沿用旧鼠标路径会得到明显错误结果。
// 视觉补偿采用非零值，以捕获漏减准备阶段 Y 偏移的问题。
// 点击后的空闲帧不产生新的 Seek，避免持久终态被每帧重新消费。
// 第二组把 Down 与 Up 合帧，验证跟踪器仍交付最终释放位置。
// 快速拖动从当前导航位置继续，不允许跳回旧显示快照的时间。
// 内容定位与密度轴 Seek 的计数分别断言，不能互相冒充。
// 正常结束后 Scrub 必须解除，音频与联机不能长期停留在中间状态。
// 本场景不执行 Undo，导航不应占用物件历史。
void tapAndFastRelease()
{
    Fixture f;
    f.snapshot.hoveredTime = 97;
    f.area.m_yOffset       = 20;
    f.step(P::Down, 1, 200, 400);
    check(f.commands.empty(), "touch Down never seeks");
    f.step(P::Up, 1, 200, 400);
    check(f.nav == 1 && f.seeks == 0, "tap publishes one dedicated navigation");
    check(near(f.context.currentTime, 10.4),
          "tap uses actual displayed geometry, not Hover");
    f.frame();
    f.frame();
    check(f.nav == 1, "idle frames never replay tap");
    // 同一帧内的完整拖动必须带最终 Up 坐标，不能因为没有 Move 帧而丢掉整笔。
    f.event(P::Down, 1, 200, 300);
    f.event(P::Up, 1, 200, 400);
    f.frame(200, 400);
    check(f.nav == 3 && !f.context.isSeekScrubbing,
          "fast drag previews and finishes once");
    check(f.context.currentTime > 11,
          "fast drag moves relative to current location");
}
/// @brief 原地抖动、往返拖动与取消分别验证，不能用吞掉所有输入假通过。
/// @details 单指超过阈值立即浏览；Cancel 只收尾最近有效值，不跳到取消坐标。
/// 上一笔时间导航不进入物件历史，因此无需借助 Undo 撤回未提交草稿。
// 抖动和有效拖动成对验证，避免将所有移动忽略也算取消安全。
// 取消位置故意远离最后有效位置，额外读取该位置会产生可检测跳转。
// 取消之后补发迟到 Up，检查是否被错误当成普通轻点。
// 外部落点先不归属预览，即使后续进入内容区也不得接管。
// 窗口几何变化属于输入坐标失效，不需要等待旋转超时。
// 恢复原尺寸不会复活被封锁的同一代际。
// 外部模态通过持续许可拒绝输入，不直接销毁生产解释器。
// 每个拒绝场景都检查命令数，避免曲尾夹取掩盖多余定位。
// 此前已显示的位置必须保留，取消不能撤销上一笔编辑。
void singleCancelAndBoundaries()
{
    Fixture f;
    f.step(P::Down, 1, 200, 300);
    f.step(P::Move, 1, 203, 304);
    check(f.commands.empty(), "small jitter remains pending");
    f.step(P::Move, 1, 200, 400);
    const double moved = f.context.currentTime;
    check(moved > 10 && f.context.isSeekScrubbing,
          "single drag starts continuous browsing");
    f.step(P::Cancel, 1, 200, 650);
    check(near(f.context.currentTime, moved) && !f.context.isSeekScrubbing,
          "Cancel keeps last valid navigation");
    const int count = f.nav;
    f.step(P::Up, 1, 200, 650);
    f.frame();
    check(f.nav == count, "late Up after Cancel cannot seek");
    // 外部首触点永远不能通过移入窗口获得归属。
    f.area.m_hovered = false;
    f.step(P::Down, 1, 800, 300);
    f.area.m_hovered = true;
    f.step(P::Move, 1, 200, 400);
    f.step(P::Up, 1, 200, 400);
    check(f.nav == count, "outside drag does not enter Preview");
    // 窗口几何变化会使当前代际失效；恢复原尺寸也不能重开旧轻点。
    f.step(P::Down, 1, 200, 300);
    f.area.m_size.y += 10;
    f.frame(200, 300, true);
    f.area.m_size.y -= 10;
    f.step(P::Up, 1, 200, 300);
    check(f.nav == count, "layout interruption cannot replay tap");
    f.step(P::Down, 1, 200, 300);
    f.area.m_available = false;
    f.step(P::Up, 1, 200, 300);
    check(f.nav == count, "external popup blocks pending tap");
    // 捕获后的手指可以越出窗口继续浏览，与外部起笔移入的拒绝规则不同。
    // 持续许可由平台 Cancel 或弹层收回，不依赖合成鼠标是否成功聚焦。
    f.area.m_available = f.area.m_hovered = true;
    f.step(P::Down, 1, 200, 300);
    f.area.m_hovered = false;
    f.step(P::Move, 1, 200, 800);
    check(f.context.currentTime > moved,
          "captured drag continues beyond content bounds");
    f.step(P::Up, 1, 200, 800);
    const int outsideCount = f.nav;
    // 隐藏两帧后重新出现，残留 Up 必须被吞掉而非恢复候选轻点。
    // 不重建策略对象，避免用析构清理掩盖生产中断检测遗漏。
    f.area.m_hovered = true;
    f.step(P::Down, 1, 200, 300);
    for ( int i = 0; i < 2; ++i ) {
        ImGui::NewFrame();
        ImGui::Render();
    }
    f.step(P::Up, 1, 200, 300);
    check(f.nav == outsideCount, "hidden frames discard a pending tap");
}
/// @brief 双指只导航，不接入主画布的二指撤销或三指重做。
/// @details 检查单指接管、纯平移死区、明确捏合和第一指释放的完整顺序。
/// 使用倍率十作为基线，绝对目标不能被滞后的 UI 快照反复叠乘。
// 先验证第二指加入时没有跳变，再让两指等距平移形成阳性对照。
// 纯平移不能因自然指距误差而改变预览范围。
// 明确张开时检查范围减小，防止把视觉缩放方向反转。
// 锚点不变量通过滚速积分坐标验证，不能只比较倍率数字。
// 动画或快照滞后不能作为下一段倍率基线参与逐帧叠乘。
// 第一指抬起就结束导航，剩余指继续移动也没有单指副作用。
// 三指用例使用新的完整原始代际，不复用此前已封锁的状态蒙混。
// Preview 不提供历史轻触，所有三指事件都不能发出撤销或重做。
// 主画布隔离在夹具析构中再次检查，贯穿整个导航生命周期。
void pairAndPinch()
{
    Fixture f;
    f.step(P::Down, 1, 200, 300);
    f.step(P::Down, 2, 400, 300);
    check(f.commands.empty(), "second finger arrival does not jump");
    f.event(P::Move, 1, 200, 350);
    f.event(P::Move, 2, 400, 350);
    f.frame(200, 350, true);
    check(f.context.currentTime > 10, "two-finger centroid pans");
    check(near(*f.context.m_pointerPreviewAreaRatio, 10),
          "pure pair pan keeps preview range");
    // 张开围绕同一中点，缩放锚点的滚动坐标必须保留。
    const auto& cache =
        f.context.timelineRegistry.ctx().get<Logic::System::ScrollCache>();
    const double anchor = cache.getAbsY(f.context.currentTime) + 50 / .1;
    f.event(P::Move, 1, 150, 350);
    f.event(P::Move, 2, 450, 350);
    f.frame(150, 350, true);
    const float ratio = *f.context.m_pointerPreviewAreaRatio;
    check(ratio < 10 && ratio >= 1, "spread zooms Preview in");
    check(
        near(cache.getAbsY(f.context.currentTime) + 50 / (1.0 / ratio), anchor),
        "pinch preserves content under centroid");
    f.step(P::Up, 1, 150, 350);
    check(!f.context.isSeekScrubbing && f.context.m_pointerPreviewFinished,
          "pair ends on first release");
    const int count = f.nav;
    f.step(P::Move, 2, 450, 500);
    f.step(P::Up, 2, 450, 500);
    check(f.nav == count, "remaining finger cannot replay single navigation");
    f.step(P::Down, 1, 200, 300);
    f.step(P::Down, 2, 400, 300);
    f.step(P::Down, 3, 300, 400);
    f.step(P::Up, 1, 200, 300);
    f.step(P::Up, 2, 400, 300);
    f.step(P::Up, 3, 300, 400);
    check(f.nav == count && f.forbidden == 0,
          "three fingers never become history commands");
    // 第二指从别的窗口落下不能组成预览捏合，两指全抬后也不补单击。
    // 本场景使用新代际，不能借此前三指造成的封锁状态得到假通过。
    f.step(P::Down, 1, 200, 300);
    f.step(P::Down, 2, 800, 300);
    f.step(P::Up, 1, 200, 300);
    f.step(P::Up, 2, 800, 300);
    check(f.nav == count, "cross-window pair cannot take over Preview");
}
/// @brief 密度轴是独立的全谱定位入口，多指加入只结束最后有效 Scrub。
/// @details 普通点击不走 Preview 滚速比例；两种时间语义不能混为一套像素比例。
/// 结束后本地密度游标必须释放，避免固定在旧目标遮盖后续播放。
// 密度轴用全谱时长映射，内容区则用滚速索引，两套坐标不能混用。
// 点击轴中部期望定位一半时长，与预览范围十倍完全独立。
// 连续拖动先开启 Scrub，第二指接管必须只提交最后有效值。
// 多指取消后释放全部手指，不能再补一次全谱跳转。
// 游标覆盖在普通点击后的下一帧释放，后续播放仍使用正式快照。
// 末尾直接替换谱面对象，模拟命令排队后才发生的切谱。
// 旧实例的密度 Seek 和内容导航分别送入正式控制器验证拒绝。
// 拒绝必须早于时间、音频和 Scrub 状态变化，不能事后补偿。
// 测试不会依赖两个不同谱面恰好使用不同路径或不同注释 ID。
void densityAndInstance()
{
    Fixture f;
    f.step(P::Down, 1, 540, 400);
    check(f.commands.empty(), "density Down waits for intent");
    f.step(P::Up, 1, 540, 400);
    check(f.seeks == 1 && near(f.context.currentTime, 50),
          "density tap maps full duration");
    f.frame();
    check(!f.view->densityTarget(), "tap does not freeze density cursor");
    f.step(P::Down, 1, 540, 400);
    f.step(P::Move, 1, 540, 250);
    check(near(f.context.currentTime, 75) && f.context.isSeekScrubbing,
          "density drag follows full axis");
    f.step(P::Down, 2, 400, 250);
    check(!f.context.isSeekScrubbing && near(f.context.currentTime, 75),
          "second finger cancels density without new target");
    f.step(P::Up, 1, 540, 250);
    f.step(P::Up, 2, 400, 250);
    // 发布后切谱也必须拒绝旧实例，而不是仅在 UI 起笔时检查。
    const auto oldId         = f.snapshot.beatmapInstanceId;
    f.context.currentBeatmap = std::make_shared<BeatMap>();
    f.playback.handleCommand(
        Logic::CmdSeek{ .time = 90, .beatmapInstanceId = oldId });
    f.playback.handleCommand(Logic::CmdPanCanvas{ .cameraId         = "Preview",
                                                  .deltaY           = 100,
                                                  .viewportWidth    = 400,
                                                  .viewportHeight   = 600,
                                                  .previewAreaRatio = 10,
                                                  .previewInstanceId = oldId });
    check(near(f.context.currentTime, 75),
          "stale queued commands reject replacement instance");
}
/// @brief 跨真实变速段定位，不能将预览内容当作线性全谱时间轴。
/// @details 通过已构建滚速缓存放置一个明确目标，再让正式轻点反投影回目标。
/// 第五秒后的 SV 为二，跨段距离与任一单一常速近似均不同。
// 快照当前时间和逻辑当前时间刻意在本场景重设到第六秒。
// 点击目标在第四秒，二者之间跨过第五秒的变速边界。
// 固定常速近似会产生不同答案，因此不能仅测试一个线性空谱面。
// 显示 Y 由正式缓存投影得到，点按必须通过正式控制器反算回来。
// 非有限输入在播放状态修改之前拒绝，不能用最终时间夹取掩盖错误。
// 方案切换发生在全部抬手之后，不以强制中断活动守卫替代真实切换。
// 切回触控的阳性轻点证明状态没有因为取消而永久失效。
// 该测试不注册自己的方案或改写生产分派函数。
void variableSpeedAndProfiles()
{
    Fixture f;
    auto&   registry = f.context.timelineRegistry;
    registry.emplace<Logic::TimelineComponent>(
        registry.create(), 0., TimingEffect::BPM, 120.);
    registry.emplace<Logic::TimelineComponent>(
        registry.create(), 5., TimingEffect::SCROLL, 2.);
    auto& cache = registry.ctx().get<Logic::System::ScrollCache>();
    cache.rebuild(
        registry, f.context.lastConfig, f.context.currentBeatmap.get());
    f.context.currentTime = f.context.animateTime = f.snapshot.currentTime = 6;
    const float targetY =
        400 - static_cast<float>((cache.getAbsY(4) - cache.getAbsY(6)) * .1);
    f.step(P::Down, 1, 200, targetY);
    f.step(P::Up, 1, 200, targetY);
    check(near(f.context.currentTime, 4),
          "cross-SV tap uses the real scroll index");
    const int count = f.nav;
    // 不可用投影不能用单位比例替代，也不允许坏数进入正式 Seek。
    Logic::CmdPanCanvas bad{ .cameraId       = "Preview",
                             .deltaY         = 100,
                             .viewportWidth  = 400,
                             .viewportHeight = 600,
                             .previewAreaRatio =
                                 std::numeric_limits<float>::quiet_NaN(),
                             .previewInstanceId =
                                 f.snapshot.beatmapInstanceId };
    f.playback.handleCommand(bad);
    check(near(f.context.currentTime, 4),
          "invalid ratio never changes transport");
    // 经典方案返回旧路径，而非让移动解释器吞掉鼠标；切回触控后仍可正常轻点。
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    f.step(P::Down, 1, 200, 400);
    f.step(P::Up, 1, 200, 400);
    check(f.nav == count, "classic profile produces no touch navigation");
    UI::Interaction::synchronizeProfile("touch-gestures", true);
    UI::Interaction::synchronizeProfile("touch-gestures", true);
    f.frame();
    f.step(P::Down, 1, 200, targetY);
    f.step(P::Up, 1, 200, targetY);
    check(f.nav == count + 1,
          "touch profile resumes on the next independent gesture");
}
/// @brief 最终范围配置被真实会话 update 采纳后解除覆盖，不重建缩放或等待确认。
/// @details 手势期间渲染有独立覆盖；这里验证最终发布和逻辑配置刷新之间的交接。
/// 会话不保存工作区，临时谱面也不写测试资源目录。
// 前两次更新走正式载入与派生缓存，不直接安装一个假的已加载上下文。
// 后续先提供不匹配的旧范围，覆盖必须保留以避免释放时视觉回跳。
// 再提供与手势相同的新范围，才能认为后台已经采纳最终 UI 配置。
// 解除覆盖只影响显示，不应再次调用 Seek 或改变当前曲目对象。
// 会话在检查之后关闭，测试不会把临时路径加入最近项目。
// 所有检查都发生在同一逻辑线程，不需要原子等待或睡眠。
void configAdoption()
{
    auto&      engine = Logic::EditorEngine::instance();
    auto       map    = std::make_shared<BeatMap>();
    const auto index  = engine.createSession(
        map, "PreviewConfigTest", false, "Preview config", true);
    const auto session = engine.getActiveSession();
    auto       config  = engine.getEditorConfig();
    // 固定 VSync 禁止测试同一时刻的多次 update 被空闲节流跳过。
    // 配置交接的通过条件不能依赖测试机快慢，也不使用 sleep 推进时间。
    config.settings.frameLimit = Config::FrameLimitPreference::VSync;
    session->update(0, config, true);
    session->update(0, config, true);
    auto& ctx = session->getContextMutable();
    // 注册真实相机后检查生产快照，不能只断言上下文里的可选值。
    // 主画布高度固定为 600，预览可绘制高度为 360，预期比例为 0.1。
    // 两个独立缓冲同时发布，可以发现预览倍率污染主画布的错误。
    config.visual.trackLayout.top             = 0;
    config.visual.trackLayout.bottom          = 1;
    config.visual.previewConfig.margin.top    = 20;
    config.visual.previewConfig.margin.bottom = 20;
    session->pushCommand(Logic::CmdUpdateViewport{ "Basic2DCanvas", 600, 600 });
    session->pushCommand(Logic::CmdUpdateViewport{ "Preview", 300, 400 });
    ctx.m_pointerPreviewAreaRatio         = 6;
    ctx.m_pointerPreviewFinished          = true;
    config.visual.previewConfig.areaRatio = 10;
    session->update(0, config, true);
    check(ctx.m_pointerPreviewAreaRatio.has_value(),
          "old configuration retains displayed range");
    // 旧持久配置仍为 10，但实际绘制必须已经使用触控值 6。
    // 读取真实渲染器产物，旧实现只更新反投影时会在此失败。
    // 缓冲只在本次同步更新之后读取，借用不越过下一次生产更新。
    // 比例来自布局生成器最终写入值，而非测试重新计算的上下文字段。
    // 未注册或未发布相机必须失败，不能以空指针跳过阳性检查。
    const auto  preview      = ctx.syncBuffers.find("Preview");
    const auto  main         = ctx.syncBuffers.find("Basic2DCanvas");
    const auto* rendered     = preview != ctx.syncBuffers.end()
                                   ? preview->second->pullLatestSnapshot()
                                   : nullptr;
    const auto* mainRendered = main != ctx.syncBuffers.end()
                                   ? main->second->pullLatestSnapshot()
                                   : nullptr;
    check(rendered && near(rendered->renderScaleY, .1),
          "live preview geometry uses the pending range before configuration "
          "adoption");
    check(mainRendered && near(mainRendered->renderScaleY, 1),
          "preview pinch leaves main canvas geometry unchanged");
    config.visual.previewConfig.areaRatio = 6;
    session->update(0, config, true);
    check(!ctx.m_pointerPreviewAreaRatio && !ctx.m_pointerPreviewFinished,
          "matching final configuration releases the range override");
    engine.closeSession(index, false);
}
}  // namespace
/// @brief 执行正式输入和播放控制器回归，所有文件输出由 CTest 限定到构建树。
int main()
{
    tapAndFastRelease();
    singleCancelAndBoundaries();
    pairAndPinch();
    densityAndInstance();
    variableSpeedAndProfiles();
    configAdoption();
    // 汇总保留所有场景的失败；配置交接成功不能覆盖此前触点或时间映射错误。
    // 测试结束不请求任何设备安装、公开发布或用户项目保存。
    return failures ? 1 : 0;
}
