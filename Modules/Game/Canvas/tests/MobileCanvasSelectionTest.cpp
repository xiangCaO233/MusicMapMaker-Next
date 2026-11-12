#include "canvas/Basic2DCanvasInteraction.h"
#include "common/LogicCommands.h"
#include "common/SelectionOperation.h"
#include "common/render/RenderSnapshot.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/InteractionController.h"
#include "logic/session/NoteAction.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "presentation/CanvasInteractionPresentation.h"
#include "presentation/mobile/MobileCanvasInteractionPresentation.h"
#include "presentation/mobile/MobileNoteHandles.h"
#include "ui/interaction/EditorInteraction.h"
#include <entt/entity/entity.hpp>
#include <imgui.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <variant>
#include <vector>

namespace
{
using namespace MMM;
/// @brief 记录正式命令，不复制拾取算法或手动改变工具状态。
/// @details 实体身份、子部位和最终坐标分别验证，不能只比较命令总数。
struct Capture {
    /// @brief 按原顺序保留影响选择或拖动的正式命令，供真实历史栈回归重放。
    std::vector<Logic::LogicCommand> m_editCommands;
    /// @brief 保存最后悬浮身份，结束后的空闲帧必须清除触摸目标。
    entt::entity m_lastHover{ entt::null };
    /// @brief 包括错误的空白清空。
    std::vector<Logic::CmdSelectEntity> m_selections;
    /// @brief 起点应锁定原实体与子物件。
    std::vector<Logic::CmdStartDrag> m_starts;
    /// @brief 最后一次更新必须先于结束。
    std::vector<Logic::CmdUpdateDrag> m_moves;
    /// @brief 空白起点不能被沿途实体抢走。
    std::vector<Logic::CmdStartMarquee> m_marquees;
    /// @brief 保留最终矩形端点。
    std::vector<Logic::CmdUpdateMarquee> m_boxes;
    /// @brief 起笔与收尾必须一一配对。
    int m_ends{}, m_boxEnds{}, m_seeks{};
    /// @brief 检查事务结束后不存在迟到的预览。
    bool m_updateAfterEnd{};
    /// @brief 订阅只在当前 CPU 场景生效，不接触真实谱面或注册表。
    /// @param command 经操作方案派发的命令载荷，订阅期间只读借用。
    /// @details 选择与位移分别收集，点选允许起止事务但不允许位置更新。
    /// 记录结束之后再来的更新，用于检查正式提交的顺序边界。
    void record(const Logic::LogicCommand& command)
    {
        // Hover 不进入历史断言，但能检出释放后把手覆盖未撤销的残留。
        if ( const auto* hover =
                 std::get_if<Logic::CmdSetHoveredEntity>(&command) )
            m_lastHover = hover->entity;
        // 只保存本回归需要执行的业务命令，不把布局/悬浮反馈当作编辑。
        // 相同载荷随后送入正式控制器，不能只以 UI 计数推断历史栈未变化。
        if ( std::holds_alternative<Logic::CmdSelectEntity>(command) ||
             std::holds_alternative<Logic::CmdStartDrag>(command) ||
             std::holds_alternative<Logic::CmdUpdateDrag>(command) ||
             std::holds_alternative<Logic::CmdEndDrag>(command) )
            m_editCommands.push_back(command);
        // 空实体选择也必须记录；快速框选退化成空白单击会在这里暴露。
        // 不从最终高亮反推命令，以免中途清空后又选回掩盖副作用。
        if ( const auto* c = std::get_if<Logic::CmdSelectEntity>(&command) )
            m_selections.push_back(*c);
        if ( const auto* c = std::get_if<Logic::CmdStartDrag>(&command) )
            m_starts.push_back(*c);
        // 只接受结束之前的更新；异步消费不应改变 UI 发布顺序。
        // 完整保存坐标，避免只凭“至少有一次更新”误判终点正确。
        if ( const auto* c = std::get_if<Logic::CmdUpdateDrag>(&command) ) {
            m_updateAfterEnd |= m_ends != 0;
            m_moves.push_back(*c);
        }
        if ( const auto* c = std::get_if<Logic::CmdStartMarquee>(&command) )
            m_marquees.push_back(*c);
        // 物件位移与框选有不同命令，不能用一个移动计数混合验证。
        // 框选结束后额外的空实体选择会通过 m_selections 单独报告。
        if ( const auto* c = std::get_if<Logic::CmdUpdateMarquee>(&command) ) {
            m_updateAfterEnd |= m_boxEnds != 0;
            m_boxes.push_back(*c);
        }
        // 一次触摸只能结束一次；菜单与抖动不能额外增加历史动作。
        // Seek 独立计数，防止连续点选被桌面双击导航提前截走。
        m_ends += std::holds_alternative<Logic::CmdEndDrag>(command);
        m_boxEnds += std::holds_alternative<Logic::CmdEndMarquee>(command);
        m_seeks += std::holds_alternative<Logic::CmdSeek>(command);
    }
};
/// @brief 失败后继续执行其他场景，保留能定位语义的提示。
int failures{};
/// @brief 固定帧验证使用生产移动解释器，窗口和 IO 均从真实 ImGui 构建。
/// @param valid 当前场景的一条语义约束，不在失败时提前退出后续检查。
/// @param message 精确说明被破坏的输入行为，供无 GPU 的 CI 日志定位。
void check(bool valid, const char* message)
{
    if ( !valid ) {
        XERROR("{}", message);
        ++failures;
    }
}
/// @brief 一次采样的物理指针状态，坐标使用内容区局部逻辑像素。
/// @details 点位以画布内容为原点，不包含窗口标题栏和边距。
/// 原始触点使用归一化屏幕坐标，而 ImGui 队列使用屏幕像素。
struct Sample {
    /// @brief 内容区局部位置，固定窗口边距只在事件入队时添加。
    ImVec2 m_point;
    /// @brief 物理触点是否按住，释放样本不会省略最后坐标。
    bool m_down;
};
/// @brief 从无原始触点的桌面 CPU 环境运行同一移动生产路径。
/// @details 两个预热帧建立窗口，再逐帧把输入送进正式事件队列。
/// 字库只为 CPU 布局准备，不启动 Vulkan、音频或文件选择器。
/// 固定内容原点让所有断言使用与正式命令一致的局部坐标。
/// 释放后额外空帧用于暴露迟到的清空选区及重复结束。
/// 控制器先于 ImGui 上下文销毁，事件订阅不泄漏到下一场景。
/// 不直接调用私有拾取或命令处理方法，因此测试覆盖桥的真实接线。
/// 子物件用另一个注册表类型，防止只验证默认玩家音符身份。
/// 每个场景重置上下文，双击计数和拖动量不会来自前一测试。
/// 触摸抖动与真正位移分别提供，不能把所有释放都强制当拖动。
/// 这些测试不替代真机保存回读，只验证输入到命令的确定性边界。
/// @param samples 非空的完整触点序列，最后一个样本必须已抬手。
/// @param touch 为 false 时同一生产入口转交经典桌面解释器。
/// @param operation 触控点选和框选共用的集合意图，每个场景单独设置。
/// @param surfaceHovered 画布被另一窗口遮挡时拒绝把手归属。
/// @param handles 发布已选独立长条的首尾触控目标，默认保留旧折线场景。
/// 把手中心位于原始矩形外，可暴露只有图形内命中才生效的错误。
/// 两端使用同一个实体，断言必须同时检查部件，不能仅检查实体计数。
/// 测试不将 UI 捕获命令自动反馈到快照，避免用自证逻辑掩盖路由缺口。
/// 短点击不生成拖动，与已经通过触控容差的快速拖动分别断言。
/// @return 按发布顺序收集的编辑命令，不持有事件总线订阅。
Capture run(
    const std::vector<Sample>& samples, bool touch = true,
    Logic::SelectionOperation operation = Logic::SelectionOperation::Replace,
    bool handles = false, bool surfaceHovered = true,
    Common::Render::HoverPart part = Common::Render::HoverPart::HoldStart)
{
    // 两个安全边界符合生产切换契约；不能在旧画布仍持有手势时硬切。
    // 当前场景的控制器在切换之后创建，不继承上一方案的待定触点。
    UI::Interaction::synchronizeProfile(
        touch ? "touch-gestures" : "classic-desktop", true);
    UI::Interaction::synchronizeProfile(
        touch ? "touch-gestures" : "classic-desktop", true);
    ImGui::CreateContext();
    // 每个场景明确设置触控模式，避免上一次减选状态污染后续历史测试。
    // 经典方案不接收模式设置，继续从真实键盘状态解释集合意图。
    if ( touch )
        UI::Interaction::activeProfile().setSelectionOperation(operation);
    // 不读取本机 imgui.ini，测试尺寸与停靠状态不受开发者桌面影响。
    // 时钟用固定帧增量推进，快速释放无需等待实际墙钟时间。
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 900 };
    io.DeltaTime   = 1.0f / 60;
    unsigned char* pixels{};
    int            width{}, height{};
    // 构建 CPU 字库是 NewFrame 的前置条件，像素不上传到 GPU。
    // 该指针属于 ImGui，不能在上下文销毁后保存或使用。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Capture capture;
    {
        Canvas::Basic2DCanvasInteraction controller("SelectionFixture",
                                                    "SelectionCamera");
        auto presentation = Canvas::createMobileCanvasInteractionPresentation();
        // 控制器和解释器的生命周期都短于上下文，析构可安全访问 UI 状态。
        // 快照只有固定命中框，不伪造逻辑层的拖动或选择结果。
        Common::Render::RenderSnapshot snapshot;
        snapshot.hasBeatmap = snapshot.isHoveringCanvas = true;
        snapshot.currentTool = Logic::EditTool::Marquee;
        snapshot.trackCount  = 4;
        // 双击目标必须与播放头不同，否则通用路径会正确过滤原地 Seek。
        // 此值只验证派发，真实时间与轨道换算由真机保存回读覆盖。
        snapshot.hoveredTime       = 2.0;
        snapshot.beatmapPathKey    = "selection-fixture.mmm";
        snapshot.beatmapInstanceId = 1;
        // 第一块代表折线子节点，第二块代表拖动途中经过的其他物件。
        // 使用两个相隔的局部矩形，能区分“起点拾取”和“当前位置拾取”。
        snapshot.hitboxes.push_back(
            { .entity   = static_cast<entt::entity>(77),
              .part     = Common::Render::HoverPart::PolylineNode,
              .subIndex = 2,
              .x        = 140,
              .y        = 140,
              .w        = 30,
              .h        = 30,
              .kind     = Logic::ChartObjectKind::DraftNote });
        snapshot.hitboxes.push_back({ .entity = static_cast<entt::entity>(88),
                                      .x      = 290,
                                      .y      = 290,
                                      .w      = 30,
                                      .h      = 30 });
        if ( handles ) {
            // 头部在左侧显示局部首把手，尾部位于右侧；中间留给整体移动。
            snapshot.hitboxes[0].kind     = Logic::ChartObjectKind::PlayerNote;
            snapshot.hitboxes[0].part     = Common::Render::HoverPart::Head;
            snapshot.hitboxes[0].subIndex = -1;
            snapshot.hitboxes[0].touchHandle = part;
            if ( part == Common::Render::HoverPart::PolylineNode ) {
                snapshot.hitboxes[0].part           = part;
                snapshot.hitboxes[0].subIndex       = 2;
                snapshot.hitboxes[0].touchNodeToken = 123;
            }
            snapshot.hitboxes.push_back(
                { .entity      = static_cast<entt::entity>(77),
                  .part        = Common::Render::HoverPart::HoldEnd,
                  .x           = 140,
                  .y           = 60,
                  .w           = 30,
                  .h           = 30,
                  .touchHandle = Common::Render::HoverPart::HoldEnd });
        }
        // 总线只在控制器存活期间订阅，按引用捕获的结果不会跨场景悬空。
        // 事件不送入会话，因此用例不会修改文件或用户撤销栈。
        auto&      bus          = Event::EventBus::instance();
        const auto subscription = bus.subscribe<Event::LogicCommandEvent>(
            [&](const auto& e) { capture.record(e.command); });
        // 窗口位置与大小固定，但仍经过 Begin 建立真实命中环境。
        // 未按下的预热帧不允许产生选择、起笔或清空命令。
        bool wasDown{};
        // 触摸来源的坐标与按钮可能被 ImGui 队列拆帧，末尾空帧需完整排空回放。
        // 原始流先于 NewFrame 发布，保留 Android 后端与 ImGui 消费的顺序。
        // 发布实际 Down、Move、Up，验证峰值与所有权门闩不会吞掉快速单指。
        for ( int frame = 0; frame < static_cast<int>(samples.size()) + 8;
              ++frame ) {
            const auto sample = frame < 2 ? Sample{ { 150, 150 }, false }
                                : frame < static_cast<int>(samples.size()) + 2
                                    ? samples[frame - 2]
                                    : Sample{ samples.back().m_point, false };
            // 归一化使用整个显示尺寸，不能拿画布宽高当设备触点域。
            // 预热和收尾空帧不发布虚假的 Move，以免延长触点生命周期。
            if ( sample.m_down || wasDown ) {
                const auto phase = sample.m_down
                                       ? (wasDown ? Graphic::TouchPhase::Move
                                                  : Graphic::TouchPhase::Down)
                                       : Graphic::TouchPhase::Up;
                Graphic::publishMobileTouch(phase,
                                            1,
                                            1,
                                            (sample.m_point.x + 8) / 1000,
                                            (sample.m_point.y + 27) / 900,
                                            frame / 60.0);
            }
            wasDown = sample.m_down;
            // 坐标与按钮都进入真实队列，不直接覆盖内部 Clicked 或拖动距离。
            // 从而能捕获触控回放错误清零累计位移的实际接线问题。
            // 来源保持与生产触摸鼠标合成一致，旧触控终点不能被真实鼠标借用。
            io.AddMouseSourceEvent(touch ? ImGuiMouseSource_TouchScreen
                                         : ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(sample.m_point.x + 8, sample.m_point.y + 27);
            io.AddMouseButtonEvent(0, sample.m_down);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({ 0, 0 });
            ImGui::SetNextWindowSize({ 700, 800 });
            ImGui::Begin(
                "SelectionFixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
            // 原点断言保证终点误差来自交互实现，而非测试边距漂移。
            // 窗口仍通过正式 Begin/End 维护悬浮，不手动设置内部 HoveredWindow。
            const auto origin = ImGui::GetCursorScreenPos();
            check(origin.x == 8 && origin.y == 27,
                  "fixture content origin remains stable");
            UI::Interaction::SurfaceFrame surface{
                UI::Interaction::Surface::MainCanvas,
                "SelectionCamera",
                &snapshot,
                origin.x,
                origin.y,
                600,
                700,
                surfaceHovered,
                true,
                nullptr
            };
            // SurfaceFrame 与快照属于同一画布，相机身份不能借用其他窗口。
            // 调用完成后渲染空 CPU 帧，使释放回放按正式帧边界推进。
            presentation->update(
                controller, nullptr, &snapshot, surface, 600, 700);
            ImGui::End();
            ImGui::Render();
        }
        // 必须先退订再销毁 capture 的观察对象，避免后续场景重复记账。
        // 控制器离开作用域后才销毁上下文，保持指针守卫的生命周期约束。
        bus.unsubscribe<Event::LogicCommandEvent>(subscription);
    }
    ImGui::DestroyContext();
    return capture;
}
/// @brief 点选和连续拖动分别核验，子部位不能在跨过另一实体后漂移。
/// @details 轻点不更新位置，拖动恰有一个事务，最后端点不能在结束时丢失。
void testObject()
{
    // 八像素抖动高于默认鼠标门槛，但仍低于十像素触控容差。
    // 它必须只点选，不能因通用工具的较小阈值而产生微小数据位移。
    const auto tap = run({ { { 150, 150 }, true }, { { 158, 150 }, false } });
    check(tap.m_selections.size() == 1 &&
              tap.m_selections.front().entity == static_cast<entt::entity>(77),
          "jittering tap selects original object");
    // 空拖动即使没有 Update 也会写入普通动作栈并清掉 Redo。
    // 因而必须同时约束 Start 与 End，而非只检查最终音符位置。
    check(tap.m_starts.empty() && tap.m_moves.empty() && tap.m_ends == 0 &&
              tap.m_marquees.empty(),
          "tap selects without creating an editing transaction");
    // 快速抬手前没有一个完整的持续拖动帧，最易丢失最后位移。
    // 不能靠延长测试按住时间，让普通 Pass 路径掩盖收尾问题。
    // 触点坐标先于释放入队，允许同一事件泵内只产生一个终点 Move。
    const auto fast = run({ { { 150, 150 }, true }, { { 305, 305 }, false } });
    check(fast.m_starts.size() == 1 && fast.m_marquees.empty() &&
              fast.m_ends == 1,
          "fast drag has one object transaction");
    // 起点是草稿折线节点，终点落在另一玩家音符的包围盒内。
    // 实体、注册表与子索引三者一起锁存，才能保证拖动真正针对原节点。
    if ( !fast.m_starts.empty() ) {
        const auto& start = fast.m_starts.front();
        check(start.entity == static_cast<entt::entity>(77) &&
                  start.kind == Logic::ChartObjectKind::DraftNote &&
                  start.hitSubIndex == 2 &&
                  start.hitPart == static_cast<std::uint8_t>(
                                       Common::Render::HoverPart::PolylineNode),
              "drag locks origin entity, registry and subpart");
    }
    // 终点必须在 End 前更新，最终有事务并不等于位置已被保存。
    // 这里允许正常去重减少中间更新，但不能省略释放时的精确终点。
    check(!fast.m_moves.empty() &&
              std::abs(fast.m_moves.back().mouseX - 305) < .01f &&
              std::abs(fast.m_moves.back().mouseY - 305) < .01f,
          "fast drag commits final pointer before end");
    check(!fast.m_updateAfterEnd, "object updates never follow end");
    // 对照保留正常持续拖动路径，补快速释放不能破坏原有连续更新。
    // 最后抬手还包含新位移，检查它没有停在上一帧的位置。
    const auto drag = run({ { { 150, 150 }, true },
                            { { 200, 210 }, true },
                            { { 280, 290 }, true },
                            { { 305, 305 }, false } });
    check(drag.m_starts.size() == 1 && drag.m_ends == 1 &&
              !drag.m_moves.empty() &&
              std::abs(drag.m_moves.back().mouseY - 305) < .01f,
          "continuous drag submits release endpoint");
}
/// @brief 从图形外的可见首尾把手起笔，不能误发框选或整体移动。
/// @details 快速释放必须保留原部件；经典方案忽略移动专属把手。
void testHandles()
{
    // 每个场景重建输入与画布，防止上一个把手的锁存身份掩盖错误。
    // 所有坐标均位于同一内容区；真实窗口悬浮仍由 ImGui 判定。

    // Hold 首把手中心为 (116,155)，不在原始图形命中矩形中。
    // 原始矩形的左边为 140，因此 116 必须通过平台把手进入。
    // 结束点离开圆形热区后仍应保留 HoldStart，不能回退到空白框选。
    // 这里只检查命令时序，时间钳制及历史回放由真实工具测试负责。
    const auto start = run({ { { 116, 155 }, true }, { { 116, 220 }, false } },
                           true,
                           Logic::SelectionOperation::Replace,
                           true);
    check(start.m_starts.size() == 1 &&
              start.m_starts[0].hitPart ==
                  static_cast<uint8_t>(Common::Render::HoverPart::HoldStart) &&
              start.m_ends == 1 && start.m_marquees.empty(),
          "head handle starts local resize");
    // 旧热区外缘离中心 21 像素；改为胶囊后仍必须可以起笔。
    // 新胶囊附近的触点位于旧热区之外，用于确认新视觉也有直接入口。
    // 左外缘专门落在新胶囊区域之外，能够发现误把旧热区挪走的问题。
    // 内缘样本反过来只依靠新增区域，两条断言各自约束一个入口。
    // 每次仍以原手势起点锁定部件，后续位置不得重新选择另一物件。
    // 释放点跨过触控容差，避免只触发选中而掩盖无法开始拉伸。
    // 这里验证输入接线，真实时长与持久化结果由设备回读检查。
    for ( const ImVec2 point : { ImVec2{ 95, 155 }, ImVec2{ 145, 155 } } ) {
        const auto hit =
            run({ { point, true }, { { point.x, point.y + 65 }, false } },
                true,
                Logic::SelectionOperation::Replace,
                true);
        check(
            hit.m_starts.size() == 1 &&
                hit.m_starts[0].hitPart ==
                    static_cast<uint8_t>(Common::Render::HoverPart::HoldStart),
            "old touch radius and new capsule remain reachable");
    }
    // 尾把手的旧外缘也不能缩小，只改首端会遗漏另一侧热区。
    const auto tailEdge =
        run({ { { 215, 75 }, true }, { { 215, 130 }, false } },
            true,
            Logic::SelectionOperation::Replace,
            true);
    check(tailEdge.m_starts.size() == 1 &&
              tailEdge.m_starts[0].hitPart ==
                  static_cast<uint8_t>(Common::Render::HoverPart::HoldEnd),
          "old tail radius remains reachable");
    // 尾把手保持尾部语义，即使释放位置经过另一物件也只结束原事务。
    // 尾把手的右侧偏移不会变更轨道；局部工具只采纳纵向时间。
    // 最终坐标故意落在哨兵上，以检查释放不会重新拾取并替换目标。
    // 即时最终更新必须排在唯一 End 之前，结束后不可迟到修改。
    const auto end = run({ { { 194, 75 }, true }, { { 305, 305 }, false } },
                         true,
                         Logic::SelectionOperation::Replace,
                         true);
    check(end.m_starts.size() == 1 &&
              end.m_starts[0].entity == static_cast<entt::entity>(77) &&
              end.m_starts[0].hitPart ==
                  static_cast<uint8_t>(Common::Render::HoverPart::HoldEnd) &&
              end.m_ends == 1 && !end.m_updateAfterEnd,
          "tail handle retains target on release");
    // 触摸结束不存在鼠标式悬浮，释放后的空帧必须清除原部件身份。
    // 仅检查 End 计数不足以发现旧目标继续驱动高亮和检视的情况。
    check(end.m_lastHover == entt::null, "handle release clears touch hover");
    // 即使几何位置位于把手上，被外部窗口遮挡的起点也不属于画布。
    // 此对照不能只看没有编辑，旧把手身份也不能穿透到悬浮反馈。
    const auto blocked =
        run({ { { 116, 155 }, true }, { { 116, 220 }, false } },
            true,
            Logic::SelectionOperation::Replace,
            true,
            false);
    check(blocked.m_starts.empty() && blocked.m_lastHover == entt::null,
          "occluded canvas cannot acquire a handle");
    // 轻点把手只选中，不开始会影响重做分支的空历史事务。
    // 同位置抬手不跨过容差；即使目标部位可调整也不能创建编辑事务。
    // 这与一般图形轻点保持一致，避免消耗用户已有的重做分支。
    const auto tap = run({ { { 116, 155 }, true }, { { 116, 155 }, false } },
                         true,
                         Logic::SelectionOperation::Replace,
                         true);
    check(tap.m_starts.empty() && tap.m_ends == 0,
          "handle tap creates no drag");
    const auto desktop =
        run({ { { 116, 155 }, true }, { { 116, 220 }, false } },
            false,
            Logic::SelectionOperation::Replace,
            true);
    check(desktop.m_starts.empty(), "classic ignores touch-only handles");
}
/// @brief 局部节点与滑键箭头必须经过生产触控桥锁定部件。
/// @details 手指移动后不能把折线索引变成另一对象，也不能把尾部当整体移动。
void testLocalParts()
{
    // 捕获正式起笔载荷，不能用界面高亮代替索引检查。
    // 最后释放点离开起始节点，仍应只结束锁定的原事务。
    // 稳定令牌用于逻辑线程拒绝过期结构，不是另一个实体编号。

    // 本例绕过原几何框中心，仍从扩大的触控热区进入。
    // 节点索引和令牌必须同时出现，缺少任意一项都无法安全延迟派发。
    const auto node = run({ { { 150, 155 }, true }, { { 210, 220 }, false } },
                          true,
                          Logic::SelectionOperation::Replace,
                          true,
                          true,
                          Common::Render::HoverPart::PolylineNode);
    check(node.m_starts.size() == 1 && node.m_starts[0].hitSubIndex == 2 &&
              node.m_starts[0].hitNodeToken == 123 && node.m_ends == 1,
          "polyline touch drag freezes node identity and index");
    // 独立 Flick 尾部以右侧胶囊起笔，释放的横向位置传入原工具。
    // 箭头部位只用于局部长度修改，不应被降级为 Head 整体平移。
    // 起点与释放位置相隔较远，检验事务锁定而非偶然悬浮一致。
    const auto flick = run({ { { 194, 155 }, true }, { { 300, 155 }, false } },
                           true,
                           Logic::SelectionOperation::Replace,
                           true,
                           true,
                           Common::Render::HoverPart::FlickArrow);
    check(flick.m_starts.size() == 1 &&
              flick.m_starts[0].hitPart ==
                  static_cast<std::uint8_t>(
                      Common::Render::HoverPart::FlickArrow) &&
              flick.m_ends == 1,
          "flick touch target retains endpoint semantics");
}
/// @brief 相邻节点热区交叠时，显式选层优先且不跨越其实际热区。
/// @details 用正式移动命中 helper 检查目标选择，不复制最近距离算法。
/// 两个节点属于同一父并具有不同令牌，确保只比实体号会失败。
/// 固定窗口提供真实裁剪区，未选优先项时仍维持顶层拾取规则。
void testDenseNodes()
{
    // 使用真实 ImGui 上下文，让绘制及裁剪依赖与生产一致。
    // 不创建 Vulkan 或窗口设备，节点选择的回归可以在 CPU CI 执行。
    // 禁止读取开发者 imgui.ini，测试不受停靠和历史窗口位置影响。
    // 字库只为布局准备，像素不会上传 GPU，也不会写资源目录。

    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 800, 600 };
    io.DeltaTime   = 1.0f / 60;
    unsigned char* pixels{};
    int            w{}, h{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ 700, 500 });
    ImGui::Begin("DenseNodes");
    // 两个中心相隔十像素，目标半径仍为二十二像素。
    // 触点更接近后绘制节点，显式选择前一节点时不能被抢走。
    std::array<Common::Render::Hitbox, 2> boxes{
        { { .entity         = static_cast<entt::entity>(77),
            .part           = Common::Render::HoverPart::PolylineNode,
            .subIndex       = 1,
            .x              = 140,
            .y              = 140,
            .w              = 10,
            .h              = 10,
            .touchHandle    = Common::Render::HoverPart::PolylineNode,
            .touchNodeToken = 11 },
          { .entity         = static_cast<entt::entity>(77),
            .part           = Common::Render::HoverPart::PolylineNode,
            .subIndex       = 2,
            .x              = 150,
            .y              = 140,
            .w              = 10,
            .h              = 10,
            .touchHandle    = Common::Render::HoverPart::PolylineNode,
            .touchNodeToken = 22 } }
    };
    // 优先候选必须同时匹配父句柄、部位、子索引和稳定令牌。
    // 两节点共享父句柄，所以只比较 entity 的实现会给出错误结果。
    // 几何输入只是两个可见节点框，不含后台注册表或可变模型。
    // 新选区令牌在当前栈上存活，helper 不得保存其地址到下一帧。
    const Canvas::HoverLayerCandidate preferred{
        static_cast<entt::entity>(77),
        Logic::ChartObjectKind::PlayerNote,
        static_cast<uint8_t>(Common::Render::HoverPart::PolylineNode),
        1,
        11
    };
    // 无优先层先验证正常最近距离，证明没有把节点拾取整体关闭。
    // 相同触点再加优先层，只有目标规则改变，其他输入保持相同。
    // 返回结果须携带原令牌，不能只把显示索引改成期望值。
    // 既有抬手和门槛测试继续通过生产 routeInput 检查命令时序。
    const auto nearest = Canvas::MobileNoteHandles::render(
        boxes, { 0, 0 }, { 155, 145 }, 1, nullptr, 0);
    const auto pinned = Canvas::MobileNoteHandles::render(
        boxes, { 0, 0 }, { 155, 145 }, 1, nullptr, 0, nullptr, &preferred);
    check(nearest && nearest->subIndex == 2 && pinned && *pinned == preferred,
          "explicit overlapping node layer overrides nearest node");
    // 优先节点二十二像素范围之外，正常目标仍可获得输入。
    // 这防止菜单优先层永久吸附整个画布的下一根手指。
    // 该位置在优先节点之外但仍在相邻节点范围内。
    // 只能退回正常拾取，不能沿用上一次返回值冒充本次命中。
    // 这样显式选层不会变成跨整个画布的全局吸附状态。
    // 释放后是否清除悬浮由独立把手收尾用例验证。
    const auto outside = Canvas::MobileNoteHandles::render(
        boxes, { 0, 0 }, { 175, 145 }, 1, nullptr, 0, nullptr, &preferred);
    check(outside && outside->subIndex == 2,
          "preferred node cannot steal another touch region");
    // 提交这一帧再销毁上下文，遵守正式绘制生命周期。
    // 测试不保留命中列表引用，下一帧不能观察已销毁的局部数组。
    // 相邻物件的完整拖动事务另外由命令桥用例覆盖。
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
}
/// @brief 空白起点跨过物件仍维持框选，释放不能误当单击清空结果。
/// @details 快速框选与普通框选具有同样的正式起点、终点和收尾顺序。
void testBlank()
{
    // 原点在两个物件之外；终点进入物件也不能改成对象拖拽。
    // 只用 Down/Up 两个样本，确保最终矩形不依赖一个额外按住帧。
    const auto box = run({ { { 90, 90 }, true }, { { 305, 305 }, false } });
    check(box.m_starts.empty() && box.m_marquees.size() == 1 &&
              box.m_boxEnds == 1,
          "blank origin starts only marquee");
    check(!box.m_boxes.empty() &&
              std::abs(box.m_boxes.back().mouseX - 305) < .01f &&
              std::abs(box.m_boxes.back().mouseY - 305) < .01f,
          "fast marquee retains final rectangle");
    check(box.m_selections.empty() && !box.m_updateAfterEnd,
          "marquee release preserves selection result");
    // 空白轻点仍保留清空语义，与快速框选形成互补断言。
    // 不能通过禁止所有空实体选择来掩盖框选释放的错误清空。
    const auto blank = run({ { { 90, 90 }, true }, { { 94, 90 }, false } });
    check(blank.m_selections.size() == 1 &&
              blank.m_selections.front().entity == entt::null,
          "blank tap clears selection");
    // 空白轻点也不建立框选事务，避免结束回放覆盖已有集合运算基线。
    check(blank.m_marquees.empty() && blank.m_boxEnds == 0,
          "blank tap clears without a marquee transaction");
    // 同一物件连续轻点应始终点选，不能沿用桌面双击跳转播放头。
    // 经典方案仍要保留双击 seek，避免把移动规则扩散到桌面。
    const std::vector<Sample> twice{ { { 150, 150 }, true },
                                     { { 150, 150 }, false },
                                     { { 150, 150 }, false },
                                     { { 150, 150 }, true },
                                     { { 150, 150 }, false } };
    const auto                touch = run(twice);
    check(touch.m_selections.size() == 2 && touch.m_seeks == 0,
          "touch repeated taps select without desktop seek");
    // 同样的物理节奏交给经典方案，必须仍得到一次导航命令。
    // 这个对照能发现把全局双击计数永久清零的错误实现。
    const auto desktop = run(twice, false);
    check(desktop.m_seeks == 1, "classic double-click seek remains available");
    // 通过生产移动解释器检查模式接线，不直接构造集合操作命令。
    // 单击和从空白开始的框选必须携带同一个意图，不能只改按钮底色。
    for ( const auto mode : { Logic::SelectionOperation::Add,
                              Logic::SelectionOperation::Subtract } ) {
        const auto tap = run(
            { { { 150, 150 }, true }, { { 150, 150 }, false } }, true, mode);
        check(tap.m_selections.size() == 1 &&
                  tap.m_selections.front().selectionOperation == mode &&
                  tap.m_starts.empty(),
              "touch selection mode reaches tap without drag history");
        const auto box =
            run({ { { 90, 90 }, true }, { { 305, 305 }, false } }, true, mode);
        // 起点不命中物件，明确要求仍走可取消的正式框选事务。
        // 结束坐标与模式一起保留，重复框选才能累积或扣除原有集合。
        check(box.m_marquees.size() == 1 &&
                  box.m_marquees.front().selectionOperation == mode &&
                  box.m_boxEnds == 1,
              "touch selection mode reaches marquee");
        // 追加或减选中的空白轻点应保留集合，不沿用替换模式的清空行为。
        // 继续经过正式按下/抬手，防止只测模式查询而漏掉释放路径。
        // 命令为空才能排除先清空再选回造成的中途副作用。
        // 实际集合增减和重复框选结果另在模拟器中通过成组移动回读验证。
        // 此处只约束移动输入到正式命令的边界，不复制逻辑层的集合算法。
        const auto blank =
            run({ { { 90, 90 }, true }, { { 90, 90 }, false } }, true, mode);
        check(blank.m_selections.empty() && blank.m_marquees.empty(),
              "blank touch preserves add or subtract selection");
    }
}
/// @brief 将真实触控轻点派发结果送入逻辑控制器，验证重做分支与保存点。
/// @details 音符身份与 UI 命中一致；先产生可重做的真实位置动作。
/// 选择成功、重做深度、未保存状态与重做结果同时检查，空夹具不能通过。
/// 不初始化音频、文件或引擎线程，源游标与资源加载不参与选择历史断言。
void testTapPreservesRedo()
{
    using namespace MMM::Logic;
    SessionContext ctx;
    // 草稿身份匹配命中夹具；专业模式只开放编辑权限，不改变位置。
    // 使用正式选择工具，避免控制器因当前工具不匹配而直接忽略整段命令。
    // 只为可编辑性创建谱面对象，任何磁盘资源都不参与本测试。
    ctx.currentTool                          = EditTool::Marquee;
    ctx.currentBeatmap                       = std::make_shared<BeatMap>();
    ctx.lastConfig.settings.professionalMode = true;
    ctx.trackCount = ctx.draftTrackCount = 4;
    ctx.bgmTrackCount                    = 1;
    // 相机身份与捕获命令一致，旧拖动路径也必须能找到有效视口。
    // 旧行为不能因缺少相机而跳过历史提交，导致测试意外通过。
    ctx.cameras.emplace("SelectionCamera",
                        CameraInfo{ "SelectionCamera", 600, 700, 0 });
    auto& cache = ctx.timelineRegistry.ctx().emplace<System::ScrollCache>();
    // 滚动缓存为正式工具提供上下文，但轻点不应修改其积分。
    cache.rebuild(
        ctx.timelineRegistry, ctx.lastConfig, ctx.currentBeatmap.get());
    // 使用相同实体编号，防止选择命令因目标不存在而被忽略后假通过。
    const auto entity = ctx.noteRegistry.create(static_cast<entt::entity>(77));
    NoteComponent before{ .m_timestamp  = 1,
                          .m_trackIndex = -1,
                          .m_isDraft    = true };
    // 两份值快照独立，原动作载荷不随注册表的当前位置而变化。
    auto after        = before;
    after.m_timestamp = 2;
    ctx.noteRegistry.emplace<NoteComponent>(entity, before);
    ctx.noteRegistry.emplace<InteractionComponent>(entity);
    // 初始未选中，后面的选择断言证明捕获命令确实进入业务控制器。
    // 先建立、撤回并保存真实动作，重做栈不能由测试直接写入。
    ctx.actionStack.pushAndExecute(
        std::make_unique<NoteAction>(
            NoteAction::Type::Update, entity, before, after),
        ctx);
    ctx.actionStack.undo(ctx);
    // 保存撤回状态，既保留重做分支，又能检查轻点是否误置脏标志。
    ctx.actionStack.markSaved();
    InteractionController controller(ctx);
    const auto tap = run({ { { 150, 150 }, true }, { { 158, 150 }, false } });
    // UI 上下文先完整结束，再重放载荷，逻辑不借用任何 ImGui 状态。
    // 输入包含容差内抖动，不能只验证坐标完全不变的理想点击。
    // 保留发布顺序；若轻点再次产生空拖动，正式 End 会清空现有重做分支。
    // 命中和集合选择仍由生产控制器完成，不手动设置选中标志。
    for ( const auto& command : tap.m_editCommands ) {
        if ( const auto* c = std::get_if<CmdSelectEntity>(&command) )
            controller.handleCommand(*c);
        if ( const auto* c = std::get_if<CmdStartDrag>(&command) )
            controller.handleCommand(*c);
        if ( const auto* c = std::get_if<CmdUpdateDrag>(&command) )
            controller.handleCommand(*c);
        if ( const auto* c = std::get_if<CmdEndDrag>(&command) )
            controller.handleCommand(*c);
    }
    // 轻点应真实选中对象，同时保留已保存状态及原来的唯一重做动作。
    check(ctx.noteRegistry.get<InteractionComponent>(entity).isSelected &&
              ctx.actionStack.getUndoStackSize() == 0 &&
              ctx.actionStack.getRedoStackSize() == 1 &&
              !ctx.actionStack.isDirty(),
          "selection tap preserves real redo stack and saved state");
    // 继续执行重做，验证保留的动作仍有正确载荷，而非仅保留一个空栈条目。
    ctx.actionStack.redo(ctx);
    check(ctx.noteRegistry.get<NoteComponent>(entity).m_timestamp == 2,
          "redo after selection still restores actual object change");
}
}  // namespace
/// @brief 返回所有语义断言结果，不把某个计数成功当作整组通过。
/// @post 所有场景均已解除订阅和销毁上下文，不保留设备或文件句柄。
int main()
{
    testObject();
    testHandles();
    testLocalParts();
    testDenseNodes();
    testBlank();
    testTapPreservesRedo();
    return failures == 0 ? 0 : 1;
}
