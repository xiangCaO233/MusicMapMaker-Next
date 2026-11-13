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
#include "logic/ecs/components/SampleComponent.h"
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
#include <string_view>
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
            snapshot.hitboxes[0].kind = Logic::ChartObjectKind::PlayerNote;
            snapshot.hitboxes[0].part = Common::Render::HoverPart::Head;
            // 同号采样与音符共存，不能把局部目标的领域退回默认玩家对象。
            if ( part == Common::Render::HoverPart::SampleAnchor ||
                 part == Common::Render::HoverPart::SampleOffset ) {
                snapshot.hitboxes[0].kind = Logic::ChartObjectKind::AudioSample;
                snapshot.hitboxes[0].part = part;
            }
            snapshot.hitboxes[0].subIndex    = -1;
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
/// @brief 采样左右目标经过生产触控解释器，再进入真实混合对象拖动事务。
/// @details 零偏移和正负偏移均保留锚点与触发时间的独立语义。
/// 同号玩家音符作为选中哨兵，偏移操作不得误改它或其注册表。
/// @note 普通点按只选择；拖动、撤销、重做及取消分别检查实际组件。
/// @warning 测试串行消费命令，不启动渲染或会话线程，也不等待音频回调。
void testSampleHandles()
{
    using namespace MMM::Logic;
    using Part = Common::Render::HoverPart;
    // 每种部位独立运行，不能用锚点成功代替触发点成功。
    // 两个图形即使同拍也属于不同的命令载荷。
    // 原生触点经容差分类后才发布 Start，不在按下时提前修改模型。
    // 这里不直接调用私有拾取函数，避免绕开扩大热区的生产接线。
    for ( const auto part : { Part::SampleAnchor, Part::SampleOffset } ) {
        const float x = part == Part::SampleAnchor ? 122 : 188;
        // 起点落在原小矩形之外，也必须能通过手指热区取得完整身份。
        // 快速释放前没有稳定拖动帧，必须补交最终坐标。
        // 与相邻对象相交不应重新解释本次起点的对象域。
        // 所有载荷先收集再用于逻辑测试，不借用 ImGui 上下文寿命。
        const auto events = run({ { { x, 155 }, true }, { { x, 220 }, false } },
                                true,
                                SelectionOperation::Replace,
                                true,
                                true,
                                part);
        check(
            events.m_starts.size() == 1 && events.m_ends == 1 &&
                events.m_starts.front().kind == ChartObjectKind::AudioSample &&
                events.m_starts.front().hitPart == static_cast<uint8_t>(part),
            "sample touch target retains registry and anchor/offset part");
        // 容差内轻点不能制造空编辑，否则会清除上一笔的重做分支。
        const auto tap = run({ { { x, 155 }, true }, { { x, 155 }, false } },
                             true,
                             SelectionOperation::Replace,
                             true,
                             true,
                             part);
        check(tap.m_starts.empty() && tap.m_ends == 0,
              "sample handle tap selects without edit history");
        // 初始负偏移用于覆盖提前触发，正偏移覆盖延后触发。
        // 零偏移是两类屏幕标记重合的关键边界，不能仅测试非零值。
        // 初始偏移只改变模型基线，不用测试代码替代触控目标分类。
        // 目标时间由生产相机与 ScrollCache 解释，不复制其反投影公式。
        // 同一原框外起点在经典方案中仍为空白框选，不扩展桌面拾取。
        // 负例必须真实移动，单纯没有命令不能证明仍保留原输入路径。
        // touchHandle 元数据可以共享，是否解释其扩展几何由平台入口决定。
        const auto desktop =
            run({ { { x, 155 }, true }, { { x, 220 }, false } },
                false,
                SelectionOperation::Replace,
                true,
                true,
                part);
        check(desktop.m_starts.empty() && desktop.m_marquees.size() == 1,
              "sample touch hit expansion does not change classic desktop");
        for ( const auto offset : { -125, 0, 125 } ) {
            // 成功和取消使用完全相同的起笔、路径及组件。
            // 对照只改变最后的事务收尾，不能用从未开始的空操作冒充取消。
            // 每轮重建上下文，历史与选择不能从上一种偏移泄漏。
            // 全部对象驻留内存，既不调用音频设备也不创建测试资源文件。
            for ( const bool cancel : { false, true } ) {
                // 会话提供正式业务门禁，不把渲染快照当可写注册表。
                // 专业模式允许玩家哨兵参与整体拖动，才能验证混合组隔离。
                // 相机只为该上下文创建，字段映射与捕获命令的 ID 相同。
                // 时间初值大于零，提前触发不会被绝对起点边界遮蔽。
                SessionContext ctx;
                ctx.currentBeatmap = std::make_shared<BeatMap>();
                ctx.currentTool    = EditTool::Marquee;
                ctx.lastConfig.settings.professionalMode = true;
                ctx.currentTime = ctx.animateTime = 1;
                ctx.trackCount = ctx.draftTrackCount = 4;
                ctx.bgmTrackCount                    = 1;
                // 测试的横坐标位于 BGM 区，排除跨域转换干扰局部时间语义。
                auto& layout          = ctx.lastConfig.visual.trackLayout;
                layout.left           = .5f;
                layout.right          = .9f;
                layout.bgmLanes.left  = .1f;
                layout.bgmLanes.width = .3f;
                ctx.lastConfig.visual.judgeline_pos = .5f;
                ctx.cameras.emplace(
                    "SelectionCamera",
                    CameraInfo{ "SelectionCamera", 1000, 600, 0 });
                auto& cache =
                    ctx.timelineRegistry.ctx().emplace<System::ScrollCache>();
                cache.rebuild(ctx.timelineRegistry,
                              ctx.lastConfig,
                              ctx.currentBeatmap.get());
                // 故意创建相同数值 ID，检出遗漏 ChartObjectKind 的误写。
                // 玩家音符和采样属于不同 Registry，真实选择也分别保存。
                // 音符时间不同于采样，不能用相同终点掩盖意外整体平移。
                // 不提供真实磁盘资源；测试只约束已有绑定字符串不被改变。
                const auto e = static_cast<entt::entity>(77);
                (void)ctx.sampleRegistry.create(e);
                (void)ctx.noteRegistry.create(e);
                // 锚点使用秒而偏移使用毫秒，两个字段不能共用同一换算。
                const SampleComponent before{ .m_timestamp       = 1,
                                              .m_offsetMs        = offset,
                                              .m_track           = 4,
                                              .m_audioResourceId = "effect.wav",
                                              .m_volume          = .4f };
                ctx.sampleRegistry.emplace<SampleComponent>(e, before);
                ctx.sampleRegistry.emplace<InteractionComponent>(
                    e, InteractionComponent{ .isSelected = true });
                ctx.noteRegistry.emplace<NoteComponent>(
                    e, NoteComponent{ .m_timestamp = 2, .m_trackIndex = 0 });
                ctx.noteRegistry.emplace<InteractionComponent>(
                    e, InteractionComponent{ .isSelected = true });
                // 控制器接收真实 CmdStartDrag 部位，不在测试中手设
                // draggedPart。 因而捕获层若丢失
                // SampleOffset，这里的独立偏移断言会失败。
                // 初始选区刻意包含哨兵，不能靠空选区让隔离测试通过。
                // 组件副本在命令执行前保存，避免引用随 Registry 更新而漂移。
                InteractionController controller(ctx);
                // 使用 UI 实际捕获载荷；只在取消对照中替换最后的事务收尾。
                for ( const auto& event : events.m_editCommands ) {
                    if ( const auto* c = std::get_if<CmdSelectEntity>(&event) )
                        controller.handleCommand(*c);
                    if ( const auto* c = std::get_if<CmdStartDrag>(&event) )
                        controller.handleCommand(*c);
                    if ( const auto* c = std::get_if<CmdUpdateDrag>(&event) )
                        controller.handleCommand(*c);
                    if ( const auto* c = std::get_if<CmdEndDrag>(&event) )
                        controller.handleCommand(
                            CmdEndDrag{ c->cameraId, cancel });
                }
                // 读取持久组件而非 UI 预览，检查实际命令的最终结果。
                // 结束时才计入历史，中间移动次数不能制造多份撤销动作。
                // after 是值副本，后面 Undo/Redo 不会同时改写预期结果。
                const auto after = ctx.sampleRegistry.get<SampleComponent>(e);
                // 资源、音量与锚点所属轨道不能因局部拖动发生隐式变更。
                check(after.m_audioResourceId == before.m_audioResourceId &&
                          after.m_volume == before.m_volume &&
                          after.m_track == 4,
                      "sample local drag preserves resource volume and lane");
                // 取消需要恢复两套 Registry；只恢复主采样会漏掉混合组预览。
                // 历史必须为空，不能先提交再靠 Undo 伪装取消成功。
                // 此分支不继续调用 Redo，避免把合法无动作状态当作失败。
                if ( cancel ) {
                    check(after.m_timestamp == 1 &&
                              after.m_offsetMs == offset &&
                              ctx.noteRegistry.get<NoteComponent>(e)
                                      .m_timestamp == 2 &&
                              ctx.actionStack.getUndoStackSize() == 0,
                          "cancel sample gesture restores both registries "
                          "without history");
                    continue;
                }
                // 锚点移动继续遵循原整组选中行为，不新增另一套位置算法。
                // 触发偏移则只能修改主要采样，不能改变锚点或玩家哨兵。
                // 检查字段独立性比仅检查屏幕图形发生变化更严格。
                if ( part == Part::SampleOffset )
                    check(after.m_timestamp == 1 &&
                              after.m_offsetMs != offset &&
                              ctx.noteRegistry.get<NoteComponent>(e)
                                      .m_timestamp == 2,
                          "offset handle changes only signed trigger offset");
                else
                    check(
                        after.m_timestamp != 1 && after.m_offsetMs == offset,
                        "anchor handle moves time and preserves signed offset");
                // 一次手势只建一个动作，撤销恢复 before，重做恢复最终值。
                check(ctx.actionStack.getUndoStackSize() == 1,
                      "sample gesture commits one action");
                // 一次撤销必须回到原始有符号偏移，不把负值夹成零。
                // 同时验证锚点恢复，避免偏移字段恢复但实体仍停在新时间。
                // 原资源属性在上面的共享断言中已核对。
                ctx.actionStack.undo(ctx);
                check(
                    ctx.sampleRegistry.get<SampleComponent>(e).m_timestamp ==
                            1 &&
                        ctx.sampleRegistry.get<SampleComponent>(e).m_offsetMs ==
                            offset,
                    "sample undo restores independent anchor and offset");
                // 重做比较之前保存的真实 after，而非再次计算目标像素。
                // 这会检出动作载荷引用了已被撤销改写的组件存储。
                // 对每种初始偏移分别执行，不从单个正例推断整个符号范围。
                ctx.actionStack.redo(ctx);
                check(
                    ctx.sampleRegistry.get<SampleComponent>(e).m_timestamp ==
                            after.m_timestamp &&
                        ctx.sampleRegistry.get<SampleComponent>(e).m_offsetMs ==
                            after.m_offsetMs,
                    "sample redo restores actual final component");
            }
        }
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
/// @brief 呈现策略在方案切换后立即分离触屏反馈和鼠标详情。
/// @details 同一移动实例还要服务经典与纯鼠标方案，不能按编译平台永久关闭。
void testInspectionPolicy()
{
    // 工厂对象只创建一次，再切换三种真实方案，暴露错误缓存的策略结果。
    auto mobile  = Canvas::createMobileCanvasInteractionPresentation();
    auto desktop = Canvas::createDesktopCanvasInteractionPresentation();
    for ( const auto* id :
          { "touch-gestures", "classic-desktop", "mouse-only" } ) {
        // 两次安全边界完成旧方案卸载和新方案激活，不沿用上一代策略。
        UI::Interaction::synchronizeProfile(id, true);
        UI::Interaction::synchronizeProfile(id, true);
        // 桌面对象始终保留旧详情；移动对象只在触屏方案使用简短反馈。
        // 选择集合和历史不参与显示策略，切换不能要求创建编辑事务。
        // 预期来自用例标识，不能用被测策略自己的能力值作为预期。
        check(desktop->allowsHoverInspection() &&
                  mobile->allowsHoverInspection() ==
                      (std::string_view(id) != "touch-gestures"),
              "hover inspection follows presentation without changing desktop");
    }
}
}  // namespace
/// @brief 返回所有语义断言结果，不把某个计数成功当作整组通过。
/// @post 所有场景均已解除订阅和销毁上下文，不保留设备或文件句柄。
int main()
{
    testInspectionPolicy();
    testObject();
    testHandles();
    testLocalParts();
    testDenseNodes();
    testSampleHandles();
    testBlank();
    testTapPreservesRedo();
    return failures == 0 ? 0 : 1;
}
