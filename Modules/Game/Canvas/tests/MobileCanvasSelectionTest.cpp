#include "canvas/Basic2DCanvasInteraction.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "presentation/CanvasInteractionPresentation.h"
#include "presentation/mobile/MobileCanvasInteractionPresentation.h"
#include "ui/interaction/EditorInteraction.h"
#include <entt/entity/entity.hpp>
#include <imgui.h>

#include <cmath>
#include <cstdint>
#include <variant>
#include <vector>

namespace
{
using namespace MMM;
/// @brief 记录正式命令，不复制拾取算法或手动改变工具状态。
/// @details 实体身份、子部位和最终坐标分别验证，不能只比较命令总数。
struct Capture {
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
/// @return 按发布顺序收集的编辑命令，不持有事件总线订阅。
Capture run(const std::vector<Sample>& samples, bool touch = true)
{
    // 两个安全边界符合生产切换契约；不能在旧画布仍持有手势时硬切。
    // 当前场景的控制器在切换之后创建，不继承上一方案的待定触点。
    UI::Interaction::synchronizeProfile(
        touch ? "touch-gestures" : "classic-desktop", true);
    UI::Interaction::synchronizeProfile(
        touch ? "touch-gestures" : "classic-desktop", true);
    ImGui::CreateContext();
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
                true,
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
    check(tap.m_moves.empty() && tap.m_ends == 1 && tap.m_marquees.empty(),
          "tap does not move or marquee");
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
}
}  // namespace
/// @brief 返回所有语义断言结果，不把某个计数成功当作整组通过。
/// @post 所有场景均已解除订阅和销毁上下文，不保留设备或文件句柄。
int main()
{
    testObject();
    testBlank();
    return failures == 0 ? 0 : 1;
}
