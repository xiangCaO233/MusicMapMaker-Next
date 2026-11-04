#include "presentation/mobile/MobileCanvasGesture.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"
#include "presentation/CanvasInteractionPresentation.h"
#include "presentation/mobile/MobileCanvasInteractionPresentation.h"
#include "ui/interaction/EditorInteraction.h"
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
void testControllerLongPress()
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
            int        edits{};
            auto&      bus = Event::EventBus::instance();
            const auto subscription =
                bus.subscribe<Event::LogicCommandEvent>([&](const auto& event) {
                    // 普通位置反馈不算编辑；选择和起笔都是需要防止的副作用。
                    // 先清空再结束框选并不能恢复原选择，必须从根源阻止此类命令。
                    edits += std::holds_alternative<Logic::CmdStartMarquee>(
                                 event.command) ||
                             std::holds_alternative<Logic::CmdStartBrush>(
                                 event.command) ||
                             std::holds_alternative<Logic::CmdSelectEntity>(
                                 event.command);
                });
            bool menu{};
            for ( int frame = 0; frame < 9; ++frame ) {
                // 前两帧建立真实窗口悬浮，后续按住超过正式阈值。
                // 输入经过事件队列与 NewFrame，不能直接伪造 Clicked 字段。
                io.AddMousePosEvent(150, 150);
                io.AddMouseButtonEvent(0, frame >= 2);
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
            // 菜单真的打开且没有编辑，两个条件必须同时成立。
            // 否则完全不处理输入的错误实现也会满足“没有副作用”。
            check(menu, "production controller opens touch menu");
            check(edits == 0, "production long press does not clear or draw");
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
    testControllerLongPress();
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
    return failures == 0 ? 0 : 1;
}
