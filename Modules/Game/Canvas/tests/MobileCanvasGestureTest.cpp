#include "presentation/mobile/MobileCanvasGesture.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "presentation/CanvasInteractionPresentation.h"
#include "presentation/mobile/MobileCanvasInteractionPresentation.h"
#include "presentation/mobile/MobilePointerOwnership.h"
#include "ui/interaction/EditorInteraction.h"
#include <string_view>
#include <variant>

/// @par 回归契约
/// - 长按不能先清空选区再结束框选。
/// - 画笔长按不能先创建短音符再弹菜单。
/// - 移动超过阈值立即交给工具，不等待长按窗口结束。
/// - 拖动回到原点不会重新成为菜单候选。
/// - 候选没有编辑副作用，但仍持有指针。
/// - 触点抖动不改变起笔位置，释放终点独立保存。
/// - 轻点完整经过起笔及收尾，不能只发释放边沿。
/// - 连续轻点使用各自坐标，回放不能吃掉下一触点。
/// - 失去编辑条件只取消候选，不改用户撤销历史。
/// - 恢复窗口后等待新按下，不补交旧触点释放。
/// - 实际控制器测试使用事件队列，不能只依赖模型返回值。
/// - 普通位置反馈仍允许发布，不把所有事件一律当成编辑。
/// - 弹层必须真的打开，吞掉全部输入的错误实现不能通过。
/// - 不使用真实时间延迟，固定帧时钟保证可重现。
/// - 各工具独立创建上下文，弹层焦点不会跨场景污染。
/// - 不读取用户谱面，夹具提供固定身份与投影数据。
/// - 命令只观察不应用，不能写入用户正在编辑的文件。
/// - 观察订阅在用例结束前解除，不留下悬空捕获。
/// - 正式控制器先销毁，ImGui 上下文最后销毁。
/// - 模型等待状态与真实菜单均需验证，两个层次互为补充。
/// - 阈值前一帧和阈值后一帧分开推进，防止比较符边界退化。
/// - 指针轻微移动也覆盖纵横方向，不能只检查某一坐标轴。
/// - 下一笔按下可与上一笔收尾同帧，必须保留其身份。
/// - 起笔回放的坐标与结束回放坐标分别断言，避免只检查次数。
/// - 长按成功不需要 EndMarquee，因为编辑事务根本未开始。
/// - 测试运行不依赖 Android 后端，可在桌面持续回归。
/// - 移动生产策略显式链接，桌面生产工厂仍保持桌面版本。
/// - 任何起笔命令都视为回归，不以最终画面看似相同为通过条件。
/// - 菜单 UI 不执行选项，仅检查原始触点按住时的副作用。
namespace
{
/// @brief 输入真实意图边界；不直接写状态机私有标志或复制分类公式。
/// @details 测试只使用固定时钟，跨帧不调用 sleep，运行结果不依赖机器速度。
int failures{};
/// @brief 报告语义回归，保留完整场景后再返回失败码。
void check(bool value, const char* message)
{
    if ( !value ) {
        XERROR("{}", message);
        ++failures;
    }
}
/// @brief 从真实控制器命令验证长按不清空选择、不提前绘制。
/// @details 使用 CPU ImGui 帧，不用模型结果代替正式路由的副作用检查。
/// @pre 每种工具进入用例前没有活动画笔或框选事务。
/// @post 收到的命令只观察不应用，用户文件与逻辑会话均不改变。
/// @note 固定路径和实例身份只属于夹具，不共享应用活动谱面。
/// @note 阈值用固定 DeltaTime 推进，不依赖测试机负载。
/// @note 点击位于内容中部，避开页签和批注边缘的特殊命中。
/// @note 原始触点仍由正式策略负责释放保护，测试不改其门闩。
/// @note 同时统计直接选择命令，防止漏掉清空选区副作用。
/// @note 此层验证真实接线，轻点与拖动边界由后续模型用例覆盖。
/// @param multi 用真实多触点流覆盖已经起笔后的接管，而不是只测候选取消。
/// @details 输入模拟保留 SDL 先发布、ImGui 后消费的生产顺序。
/// 同一画布先绘制、再由第二指取消、全部抬起后再次轻点。
/// 新轻点仍需完整起笔，因此不能通过永久吞掉输入令用例通过。
/// 框选和画笔分别验证，取消必须使用各自业务命令。
/// 第一指先释放后第二指继续按住，覆盖最容易误落单键的阶段。
/// 全部抬起后的空帧用于检测迟到的合成鼠标释放。
/// 每个场景只统计命令，不应用到用户会话。
/// 取消数只统计 cancel=true，正常释放不能冒充取消。
/// 手势过程中不创建后台等待，时间推进完全由测试帧控制。
/// 再次起笔位于不同轨道，防止错误复用上次起点也满足计数。
/// 测试不要求特定帧率；10dp 阈值只通过明确越界触发。
/// 菜单是否打开独立断言，避免取消同时错误弹出编辑菜单。
/// 工具切换在两个上下文之间完成，不在手势中改变工具。
/// 原始输入的设备及手指身份固定，释放顺序与鼠标状态分别提供。
/// 合成鼠标不能取代原始峰值，因为一次轮询可能包含全部多指事件。
/// 下一次触点建立新代际，旧接管门闩必须自然复位。
/// 窗口在头两帧预热，避免首帧悬浮建立影响拾取结果。
/// 无触控方案仍执行旧断言，检测桌面输入是否被无意延迟。
void testControllerLongPress(bool touch, bool multi = false)
{
    using namespace MMM;
    for ( auto tool : { Logic::EditTool::Marquee, Logic::EditTool::Draw } ) {
        // 两种工具各用独立上下文，防止前一个弹层的焦点掩盖回归。
        // 字库只用于 CPU 文字布局，不创建音频或 Vulkan 设备。
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1000, 900 };
        io.DeltaTime   = 0.1f;
        unsigned char* pixels{};
        int            width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        {
            Canvas::Basic2DCanvasInteraction controller("TouchFixture",
                                                        "TouchCamera");
            auto                             presentation =
                Canvas::createMobileCanvasInteractionPresentation();
            Common::Render::RenderSnapshot snapshot;
            snapshot.hasBeatmap = snapshot.isHoveringCanvas = true;
            snapshot.currentTool                            = tool;
            snapshot.trackCount                             = 4;
            snapshot.beatmapPathKey    = "touch-fixture.mmm";
            snapshot.beatmapInstanceId = 1;
            int        edits{}, cancelled{};
            auto&      bus = Event::EventBus::instance();
            int        currentFrame{};
            bool       leakedSelection{};
            int        lateSelections{};  ///< 全部抬起后的新轻点必须仍可选择。
            const auto subscription =
                bus.subscribe<Event::LogicCommandEvent>([&](const auto& event) {
                    if ( const auto* end =
                             std::get_if<Logic::CmdEndBrush>(&event.command) )
                        cancelled += end->cancel;
                    if ( const auto* end =
                             std::get_if<Logic::CmdEndMarquee>(&event.command) )
                        cancelled += end->cancel;
                    // 普通位置反馈不算编辑；选择和起笔都是需要防止的副作用。
                    // 先清空再结束框选并不能恢复原选择，必须从根源阻止此类命令。
                    const bool selection =
                        std::holds_alternative<Logic::CmdSelectEntity>(
                            event.command);
                    // 正常空白轻点的释放允许额外清空选择，不能把它计作第三次起笔。
                    // 因此起笔次数与选择副作用按独立契约观察。
                    // 只在新轻点正常释放后允许清空选择，残留手指不能产生选择副作用。
                    leakedSelection |= multi && selection &&
                                       currentFrame >= 4 && currentFrame < 8;
                    if ( multi && selection && currentFrame >= 8 )
                        ++lateSelections;
                    edits += std::holds_alternative<Logic::CmdStartMarquee>(
                                 event.command) ||
                             std::holds_alternative<Logic::CmdStartBrush>(
                                 event.command) ||
                             (!multi && selection);
                });
            bool menu{};
            for ( int frame = 0; frame < (multi ? 12 : 9); ++frame ) {
                // 前两帧建立真实窗口悬浮，后续按住超过正式阈值。
                // 输入经过事件队列与 NewFrame，不能直接伪造 Clicked 字段。
                currentFrame = frame;
                // 阶段编号供事件订阅辨认取消后的禁用区间，不参与生产手势判断。
                if ( multi ) {
                    // 原始事件先于 ImGui
                    // 帧；第二指加入、第一指先抬和全部抬起分帧验收。
                    using Graphic::TouchPhase;
                    if ( frame == 2 )
                        Graphic::publishMobileTouch(
                            TouchPhase::Down, 1, 1, .15f, 1.0f / 6, .2);
                    if ( frame == 3 )
                        Graphic::publishMobileTouch(
                            TouchPhase::Move, 1, 1, .22f, 1.0f / 6, .3);
                    if ( frame == 4 )
                        Graphic::publishMobileTouch(
                            TouchPhase::Down, 1, 2, .35f, 1.0f / 6, .4);
                    if ( frame == 5 )
                        Graphic::publishMobileTouch(
                            TouchPhase::Up, 1, 1, .22f, 1.0f / 6, .5);
                    if ( frame == 6 )
                        Graphic::publishMobileTouch(
                            TouchPhase::Up, 1, 2, .35f, 1.0f / 6, .6);
                    if ( frame == 8 )
                        Graphic::publishMobileTouch(
                            TouchPhase::Down, 1, 3, .4f, 1.0f / 6, .8);
                    if ( frame == 9 )
                        Graphic::publishMobileTouch(
                            TouchPhase::Up, 1, 3, .4f, 1.0f / 6, .9);
                    io.AddMousePosEvent(frame < 3   ? 150
                                        : frame < 8 ? 220
                                                    : 400,
                                        150);
                    io.AddMouseButtonEvent(
                        0, (frame >= 2 && frame < 5) || frame == 8);
                } else {
                    io.AddMousePosEvent(150, 150);
                    io.AddMouseButtonEvent(0, frame >= 2);
                }
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({ 0, 0 });
                ImGui::SetNextWindowSize({ 700, 800 });
                ImGui::Begin(
                    "TouchFixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
                const auto origin = ImGui::GetCursorScreenPos();
                UI::Interaction::SurfaceFrame surface{
                    UI::Interaction::Surface::MainCanvas,
                    "TouchCamera",
                    &snapshot,
                    origin.x,
                    origin.y,
                    600,
                    700,
                    true,
                    true,
                    nullptr
                };
                presentation->update(
                    controller, nullptr, &snapshot, surface, 600, 700);
                ImGui::PushID("TouchCamera");
                menu |= ImGui::IsPopupOpen("SelectionEditMenu");
                ImGui::PopID();
                ImGui::End();
                ImGui::Render();
            }
            // 非触控方案相反：按住期间必须即时起笔，且不能打开触摸菜单。
            // 三种方案使用同一生产移动解释器，避免只测独立能力标志。
            // 菜单真的打开且没有编辑，两个条件必须同时成立。
            // 否则完全不处理输入的错误实现也会满足“没有副作用”。
            if ( multi ) {
                // 先起笔再被双指取消；所有旧触点抬起后，新轻点必须恢复正常编辑。
                // 一个取消只对应第一个已开始事务，新轻点按当前工具重新分类。
                // 菜单和残留选择都单独检查，不能用起笔计数掩盖其他副作用。
                // 选择工具的新轻点只选择，不再建立第二次空框选事务。
                // 绘制工具仍应起笔两次，不能因修复历史污染而吞掉新 Note。
                const int expectedEdits =
                    tool == Logic::EditTool::Marquee ? 1 : 2;
                const int expectedSelections =
                    tool == Logic::EditTool::Marquee ? 1 : 0;
                // 独立检查新轻点的选择，不能用少一次起笔掩盖触点被全部吞掉。
                // 旧触点阶段另由 leakedSelection 保证没有误清空基线。
                // 失败日志使用同一分类预期，正常纯选择不应留下错误日志。
                if ( menu || edits != expectedEdits || cancelled != 1 )
                    XERROR("tool={} menu={} starts={} cancelled={}",
                           static_cast<int>(tool),
                           menu,
                           edits,
                           cancelled);
                check(!menu && edits == expectedEdits && cancelled == 1 &&
                          !leakedSelection &&
                          lateSelections == expectedSelections,
                      "multi-touch cancels once, suppresses remaining finger "
                      "and accepts next tap");
            } else {
                check(menu == touch,
                      "only touch profile opens long press menu");
                check(touch ? edits == 0 : edits > 0,
                      "profile routes press through its actual editing path");
            }
            bus.unsubscribe<Event::LogicCommandEvent>(subscription);
        }
        // 正式控制器和策略先销毁，再结束其借用的 ImGui 上下文。
        ImGui::DestroyContext();
    }
}
}  // namespace
/// @brief 覆盖候选长按、立即轻点、连续拖动以及输入归属中断。
/// @details 尤其保证等待期间不产生 Begin，避免框选清空或画笔提前落笔。
int main()
{
    // 同一安卓解释器在经典和纯鼠标方案下不能截获长按或延迟普通起笔。
    // 逐个销毁画布上下文后切换方案，避免活动手势阻挡合法的安全帧切换。
    for ( const auto* id :
          { "classic-desktop", "mouse-only", "touch-gestures" } ) {
        MMM::UI::Interaction::synchronizeProfile(id, true);
        MMM::UI::Interaction::synchronizeProfile(id, true);
        const bool touch =
            MMM::UI::Interaction::activeProfile().touchGestures();
        check(touch == (std::string_view(id) == "touch-gestures"),
              "touch capability belongs only to touch profile");
        testControllerLongPress(touch);
    }
    using G = MMM::Canvas::MobileCanvasGesture;
    using O = G::Output;
    // 每个场景拥有独立实例，前一手势不能向后一场景遗留释放请求。
    // 测试只检查公开输出，内部阶段名称可在不改变语义时自由重构。
    G hold;
    // 选中工具与绘制工具共用前置仲裁，长按从未进入起笔状态。
    check(
        hold.update(true, true, false, { 20, 30 }, 0, 10, true) == O::Suppress,
        "press stays pending");
    check(hold.active(), "pending owns pointer");
    check(hold.update(false, true, false, { 23, 32 }, 0.44, 10, true) ==
              O::Suppress,
          "jitter does not edit");
    check(
        hold.update(false, true, false, { 23, 32 }, 0.46, 10, true) == O::Menu,
        "long press opens menu without edit");
    // 轻点最常见于单 Note 绘制，起笔晚于释放的事实不应丢失起点。
    // 释放完成后也必须解除输入归属，下一触点才能正常进入其他窗口。
    G tap;
    tap.update(true, true, false, { 20, 30 }, 0, 10, true);
    // 快速抬手只回放一次原起点与一次终点，保留两个边沿的先后顺序。
    check(
        tap.update(false, false, true, { 22, 31 }, 0.08, 10, true) == O::Begin,
        "tap begins on release");
    check(tap.replayPoint().x == 20, "tap retains original point");
    check(tap.update(false, false, false, { 50, 50 }, 0.1, 10, true) ==
              O::Release,
          "tap completes next frame");
    check(tap.replayPoint().x == 22 && !tap.active(),
          "release retains endpoint");
    // 拖动不受长按时间限制：一旦超过阈值便立即交给编辑器。
    // 后续帧必须 Pass，不能以重复 Begin 制造多条撤销事务。
    G drag;
    drag.update(true, true, false, { 20, 30 }, 0, 10, true);
    // 移动超出容差立即起笔，之后回到原点也不能转换成长按。
    check(
        drag.update(false, true, false, { 31, 30 }, 0.05, 10, true) == O::Begin,
        "drag begins beyond tolerance");
    check(drag.replayPoint().x == 20, "drag starts at original target");
    check(drag.update(false, true, false, { 20, 30 }, 0.6, 10, true) == O::Pass,
          "drag cannot become menu");
    check(
        drag.update(false, false, true, { 40, 40 }, 0.7, 10, true) == O::Pass &&
            !drag.active(),
        "drag release passes through");
    // 中断发生在尚未起笔阶段，应丢弃意图而非执行编辑器的撤销操作。
    // 恢复可编辑状态只接受新的原生按下，不复用旧触点的释放。
    G interrupted;
    interrupted.update(true, true, false, { 20, 30 }, 0, 10, true);
    // 播放、抽屉或窗口失焦取消候选，不得在恢复后补发原点击。
    check(interrupted.update(false, true, false, { 20, 30 }, 0.1, 10, false) ==
              O::Suppress,
          "blocked pending is cancelled");
    check(!interrupted.active(), "cancel releases pointer ownership");
    check(interrupted.update(false, false, true, { 20, 30 }, 0.2, 10, true) ==
              O::Suppress,
          "cancelled release does not edit");
    // 回放期间允许下一次快速轻点到达，上一轻点的释放仍优先完成。
    // 各自坐标必须隔离，防止第二次操作误用前一落点。
    G consecutive;
    consecutive.update(true, true, false, { 20, 30 }, 0, 10, true);
    consecutive.update(false, false, true, { 20, 30 }, 0.02, 10, true);
    // 上一轻点收尾与下一触点按下同帧时，新触点仍必须可完成。
    check(consecutive.update(true, true, false, { 60, 70 }, 0.04, 10, true) ==
              O::Release,
          "previous release wins current frame");
    check(consecutive.update(false, false, true, { 60, 70 }, 0.06, 10, true) ==
              O::Begin,
          "next rapid tap retained");
    check(consecutive.replayPoint().x == 60,
          "next rapid tap has independent origin");
    check(consecutive.update(false, false, false, { 60, 70 }, 0.08, 10, true) ==
              O::Release,
          "next rapid tap completes");
    // 原始输入用例最后执行，模拟设备代际不会污染前面的无设备夹具。
    // 正式控制器收到的取消命令需与工具开始次数一并匹配。
    MMM::UI::Interaction::synchronizeProfile("touch-gestures", true);
    testControllerLongPress(true, true);
    using namespace MMM;
    Canvas::MobilePointerOwnership owner;
    Graphic::MobileTouchTracker    tracker;
    tracker.update(Graphic::TouchPhase::Down, 1, 1, .2f, .3f, 0);
    check(!owner.update(tracker.snapshot(), false, false),
          "outside origin never owns canvas");
    tracker.update(Graphic::TouchPhase::Move, 1, 1, .4f, .3f, .1);
    owner.update(tracker.snapshot(), true, false);
    check(owner.blocksSingle() && !owner.ownsGesture(),
          "entering canvas cannot steal gesture");
    return failures == 0 ? 0 : 1;
}
