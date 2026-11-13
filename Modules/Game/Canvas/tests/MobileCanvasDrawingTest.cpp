#include "canvas/Basic2DCanvasInteraction.h"
#include "common/EditTool.h"
#include "common/LogicCommandMutationClassification.h"
#include "common/LogicCommands.h"
#include "common/NoteColor.h"
#include "common/NotePaletteStroke.h"
#include "common/ObjectDeletion.h"
#include "common/PolylineGeometry.h"
#include "common/render/HoverRenderData.h"
#include "common/render/RenderSnapshot.h"
#include "config/EditorConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/EditorAction.h"
#include "logic/session/InteractionController.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/NoteType.h"
#include "presentation/CanvasInteractionPresentation.h"
#include "presentation/mobile/MobileCanvasInteractionPresentation.h"
#include "ui/interaction/EditorInteraction.h"

#include <entt/entity/registry.hpp>
#include <glm/vec4.hpp>
#include <imgui.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace
{
using namespace MMM;
/// @brief 失败计数仅在本用例进程内累计，不借用应用的日志或保存状态。
// 本用例分为两个边界：原始触点到正式命令，以及正式命令到音符与历史。
// 不在测试中复制手势分类、屏幕反投影、吸附或形状状态机。
// 设备实测负责布局和真实事件差异，CPU 场景专门覆盖帧合并与命令顺序。
// 初始旧对象没有创建历史，后续 Undo 不应把它当作本笔的一部分。
int failures{};
/// @brief 保留首个失败之后的其他检查，避免一条命令断言掩盖几何错误。
void check(bool passed, const char* message)
{
    if ( !passed ) {
        XERROR("{}", message);
        ++failures;
    }
}
/// @brief 一个屏幕触点样本，坐标处于画布内容域。
/// @details 固定 UI 时间推进，长按与快速释放不依赖宿主调度或真实 sleep。
struct Sample {
    ImVec2 m_point;      ///< 当前主触点的画布局部坐标。
    bool   m_down{};     ///< 主触点按住状态。
    bool   m_second{};   ///< 第二指接管，用于取消已经开始的画笔。
    bool   m_cancel{};   ///< 原始系统取消，不模拟普通释放。
    bool   m_playing{};  ///< 播放接管不允许提交旧擦除。
    bool   m_scene{};    ///< 切换实例，旧目标不得用于新谱面。
    bool   m_palette{};  ///< 普通调色浮层同样必须阻断旧画布手势。
};
/// @brief 通过真实 ImGui 帧和生产移动解释器取得正式笔画命令。
/// @param samples 起笔至全部抬起的非空序列。
/// @param profile 触控与桌面对照使用相同输入，不修改实际键盘状态。
/// @param simple 触控绘制意图；为假时启用折线，桌面对照忽略此模式。
/// @param hit 渲染层发布的可选命中盒，只用于已有对象续写与轻点选择。
/// @return 画笔、悬停及选择命令，顺序与生产事件发布顺序一致。
/// @details 不复制手势分类、轨道投影或音符创建算法。
/// 测试输出保存在内存，不能触碰用户配置或资源目录。
/// @pre samples 非空且最终全部抬手；追加的空闲帧仅用于排空回放。
/// @note 初始预热帧不带触点，窗口布局必须先于第一次按下建立。
/// @note 第二指采用另一稳定身份，不能用数组下标代替真实手指身份。
/// @note 主触点的来源与物理按下边沿分开发布，遵循后端的事件队列契约。
/// @note 当前输入快照不代替逻辑层可变状态；命令保存后才做独立回放。
/// @note 没有注册 UIManager，不执行文件对话框、保存或其他系统动作。
/// @note 任何菜单副作用都作为失败，不能只以音符数量断言绘制正确。
/// @param tool 本场景实际工具，配色与绘制共用同一生产输入链。
/// 配色夹具仍保留音符／采样同号候选，用于验证真正的领域过滤。
/// 桌面对照使用相同几何，避免把缺少目标误判为保留旧逻辑。
std::vector<Logic::LogicCommand> capture(
    const std::vector<Sample>& samples,
    std::string_view profile = "touch-gestures", bool simple = true,
    std::optional<Common::Render::Hitbox> hit = std::nullopt,
    bool erase = false, Logic::EditTool tool = Logic::EditTool::Draw)
{
    // 两个安全边界完成方案切换，旧画布和待定笔画不能跨场景残留。
    UI::Interaction::synchronizeProfile(profile, true);
    UI::Interaction::synchronizeProfile(profile, true);
    // 场景开始先解除原始触点；新 Down 建立新代际，不能沿用上一峰值。
    // 方案切换在创建画布前完成，活动事务从不被强制迁移到另一方案。
    // 后续采样全部走事件生产入口，不能直接写入触点快照中的成员。
    // 取消原始状态并不代表取消业务；本场景此时尚未创建任何画笔。
    Graphic::cancelMobileTouch();
    UI::Interaction::activeProfile().setNoteHoldBrush(simple);
    UI::Interaction::activeProfile().setObjectEraser(erase);
    ImGui::CreateContext();
    // 教程与绘制共用方案能力，触控不能仍被旧 Shift 条件挡住。
    // 经典方案必须继续要求延伸时按 Shift；此处所有物理修饰键均未按下。
    const bool freeBrush = profile != "classic-desktop";
    check(UI::Interaction::modifierFreeBrush() == freeBrush,
          "lesson recognizes modifier-free drawing");
    check(UI::Interaction::lessonBrushModifiersMatch(true) == freeBrush,
          "extended lesson respects profile");
    check(UI::Interaction::lessonBrushModifiersMatch(false),
          "tap lesson accepts unmodified drawing");
    auto& io = ImGui::GetIO();
    // 禁用 ini 读写，开发者工作区与停靠布局不能改变固定坐标。
    // DisplaySize 使用逻辑像素，原始归一化坐标必须与窗口命中处于同一空间。
    // 固定 DeltaTime 只用于可重复的手势分类，不模拟设备实际帧率。
    // 夹具不用 sleep 等待起笔，延迟回放由后续正常 CPU 帧推进。
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 900 };
    io.DeltaTime   = .05f;
    unsigned char* pixels{};
    int            width{}, height{};
    // CPU 夹具仍建立有效字体图集，避免窗口测量依赖图形后端。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    std::vector<Logic::LogicCommand> commands;
    {
        // 窗口与控制器先于 ImGui 上下文销毁，不留下字体或输入观察指针。
        Canvas::Basic2DCanvasInteraction canvas("DrawingFixture",
                                                "DrawingCamera");
        auto presentation = Canvas::createMobileCanvasInteractionPresentation();
        Common::Render::RenderSnapshot snapshot;
        snapshot.hasBeatmap = snapshot.isHoveringCanvas = true;
        snapshot.currentTool                            = tool;
        snapshot.trackCount                             = 4;
        // 末端拾取由正式控制器完成，夹具只提供渲染层本来会发布的命中盒。
        if ( hit ) snapshot.hitboxes.push_back(*hit);
        snapshot.brush.customColors.head = glm::vec4(1, 0, 0, 1);
        snapshot.brush.customColors.hold = glm::vec4(1, 0, 0, 1);
        if ( erase || tool == Logic::EditTool::ColorBrush ||
             tool == Logic::EditTool::ColorEraser ) {
            // 三个固定可见对象覆盖短帧跨越、空白释放和混合领域同号身份。
            // 不预置选择，正式移动解释器必须自行从扫过路径收集目标。
            for ( int i = 0; i < 3; ++i )
                snapshot.hitboxes.push_back(
                    { entt::entity{ static_cast<unsigned>(i) },
                      Common::Render::HoverPart::Head,
                      -1,
                      100.f + 100 * i,
                      300,
                      40,
                      20 });
            snapshot.hitboxes.push_back(
                { entt::entity{ 0 },
                  Common::Render::HoverPart::SampleAnchor,
                  -1,
                  400,
                  300,
                  40,
                  20,
                  Logic::ChartObjectKind::AudioSample });
        }
        // 路径与实例同时固定，排除控制器以为谱面切换后提前取消笔画。
        // 有谱面且未播放，权限判断必须允许起笔；完全不派发输入不能假通过。
        // 不预置 brush.isActive，所有画笔状态必须由正式命令建立。
        // 命中身份不由测试替生产解释器伪造，绘制落点仍使用正常投影。
        snapshot.beatmapPathKey    = "drawing-fixture.mmm";
        snapshot.beatmapInstanceId = 1;
        auto& bus                  = Event::EventBus::instance();
        // 订阅只捕获正式命令；菜单、悬浮和测试自己的采样不创建编辑记录。
        const auto token = bus.subscribe<
            Event::LogicCommandEvent>([&](const auto& event) {
            const auto& command = event.command;
            if ( std::holds_alternative<Logic::CmdStartBrush>(command) ||
                 std::holds_alternative<Logic::CmdUpdateBrush>(command) ||
                 std::holds_alternative<Logic::CmdEndBrush>(command) ||
                 std::holds_alternative<Logic::CmdSetHoveredEntity>(command) ||
                 std::holds_alternative<Logic::CmdSelectEntity>(command) ||
                 std::holds_alternative<Logic::CmdApplyBrushPaletteToEntity>(
                     event.command) ||
                 std::holds_alternative<Logic::CmdClearNoteColorOverrides>(
                     event.command) ||
                 std::holds_alternative<Logic::CmdDeleteSelected>(command) ||
                 (erase &&
                  std::holds_alternative<Logic::CmdSetMousePosition>(command)) )
                commands.push_back(command);
        });
        // 只捕获笔画命令而非相机悬浮反馈，命令数量不能被无关事件污染。
        // 载荷按值保存，不保留事件对象或事件总线内部存储的引用。
        // 订阅只在当前控制器生命周期内有效，场景之间不能重复记账。
        // 控制器与解释器销毁之前退订，后续逻辑回放不依赖 ImGui 上下文。
        bool down{}, second{};
        for ( std::size_t frame = 0; frame < samples.size() + 10; ++frame ) {
            // 两帧预热建立真实窗口命中，八帧收尾排空延迟起笔和释放。
            // 预热与本笔起点一致，避免不同来源的位置跳变触发 ImGui 队列分帧。
            // 原始触点与 ImGui 边沿因此在同一位置开始，不用固定坐标偏袒某一轨。
            const Sample sample = frame < 2 ? Sample{ samples.front().m_point }
                                  : frame < samples.size() + 2
                                      ? samples[frame - 2]
                                      : Sample{ samples.back().m_point };
            const ImVec2 point{ sample.m_point.x + 8, sample.m_point.y + 27 };
            if ( profile == "touch-gestures" ) {
                using Graphic::TouchPhase;
                // 平台流先于 NewFrame 发布，与 Android 鼠标合成的消费顺序相同。
                // 第二指不改变主指位置；它只能接管并取消，不能成为提交终点。
                Graphic::publishMobileTouch(
                    sample.m_down ? (down ? TouchPhase::Move : TouchPhase::Down)
                                  : TouchPhase::Up,
                    1,
                    1,
                    point.x / 1000,
                    point.y / 900,
                    frame * .05);
                if ( sample.m_second != second )
                    Graphic::publishMobileTouch(
                        sample.m_second ? TouchPhase::Down : TouchPhase::Up,
                        1,
                        2,
                        .5f,
                        .5f,
                        frame * .05);
            }
            if ( sample.m_cancel ) Graphic::cancelMobileTouch();
            snapshot.isPlaying = sample.m_playing;
            if ( sample.m_scene ) snapshot.beatmapInstanceId = 2;
            down   = sample.m_down;
            second = sample.m_second;
            // 主触点的鼠标表示只对应真实事件，不能模拟 Shift 或切换工具。
            // 桌面对照使用真实 Mouse 来源，避免错误借用触控终点补偿。
            // 原始多指流与鼠标合成按同一顺序推进，第二指只取得导航所有权。
            // 主指抬起后仍留有第二指时，不能提交原来的未完成画笔。
            io.AddMouseSourceEvent(profile == "touch-gestures"
                                       ? ImGuiMouseSource_TouchScreen
                                       : ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(0, down);
            ImGui::NewFrame();
            // 通过正式 Begin/End 建立窗口命中，不能手动写 hovered window。
            // 内容原点偏移只做坐标换算，不应参与计算笔画移动距离。
            // 每帧仍调用解释器，即使原始触点已抬起，也要排空起笔和释放回放。
            // 额外空闲帧可以发现迟到的 Update 或重复
            // End，不能到抬手便停止测试。
            ImGui::SetNextWindowPos({ 0, 0 });
            ImGui::SetNextWindowSize({ 700, 800 });
            ImGui::Begin(
                "DrawingFixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
            const auto origin = ImGui::GetCursorScreenPos();
            check(origin.x == 8 && origin.y == 27, "drawing fixture origin");
            UI::Interaction::SurfaceFrame surface{
                UI::Interaction::Surface::MainCanvas,
                "DrawingCamera",
                &snapshot,
                origin.x,
                origin.y,
                600,
                700,
                true,
                true,
                nullptr
            };
            // 与正式主画布一致地限定输入表面，不能借默认 Application
            // 域测试画笔。
            // 作用域离开后恢复外层方案解释，不让本用例污染后续窗口。
            const UI::Interaction::SurfaceScope scope(
                UI::Interaction::Surface::MainCanvas);
            // 普通工具窗口不在 ImGui Popup 栈中，专门检查这条接管边界。
            // 浮层放在画布之外，不能靠几何遮住触点来使取消断言偶然通过。
            // 外观与输入来自真实 Begin/End，而非测试伪造一个 overlay 布尔值。
            if ( sample.m_palette ) {
                ImGui::SetNextWindowPos({ 720, 30 });
                ImGui::SetNextWindowSize({ 200, 150 });
                ImGui::Begin("##ColorPalettePopup",
                             nullptr,
                             ImGuiWindowFlags_NoSavedSettings);
                ImGui::End();
            }
            presentation->update(canvas, nullptr, &snapshot, surface, 600, 700);
            ImGui::PushID("DrawingCamera");
            // 按相机固定 ID 查询菜单，不能只检查全局是否存在任意弹窗。
            // 绘制时静止按住不应被选择菜单夺走；没有编辑副作用还不够。
            // 菜单未出现和实际创建单键分开断言，忽略所有输入也不能通过。
            // Render 只完成 CPU 帧，不调用图形设备或等待后台线程。
            check(!ImGui::IsPopupOpen("SelectionEditMenu"),
                  "drawing hold never opens selection menu");
            ImGui::PopID();
            ImGui::End();
            ImGui::Render();
        }
        // 必须在观察容器与控制器销毁前退订，下一场景不能收到旧回调。
        bus.unsubscribe<Event::LogicCommandEvent>(token);
    }
    UI::Interaction::activeProfile().setObjectEraser(false);
    ImGui::DestroyContext();
    return commands;
}
/// @brief 用真实逻辑工具和历史栈解释捕获的生产命令。
/// @param commands UI 已经分类的完整笔画，不在逻辑回放时修改载荷。
/// @param cancelled 接管场景不应创建音符或撤销节点。
/// @return 新音符的值快照；失败时默认值仅供后续比较，不代表测试成功。
/// @details 初始对象位于起拍，且折线能力开启，用于发现误续写或隐式替换。
/// @pre commands 来自本文件的生产输入捕获，不接受省略起笔的更新片段。
/// @note 正常结束与取消都需要完整生命周期，取消不能省略 End。
/// @note 起点对象带有可交互组件，权限检查不能因空对象而提前忽略它。
/// @note 用户配置在会话中按值建立，不借用或覆盖应用级默认配置。
/// @note 正式提交前的临时状态不等于已保存音符，必须检查 Registry。
/// @note 两个对象重叠仍具有不同身份，撤销应保留最初那个实体。
/// @note 可撤销节点数按笔画计算，不能为每个移动事件产生独立节点。
/// @note 结果按值返回，用例结束不暴露 Registry 或工具的生命周期。
Logic::NoteComponent replay(const std::vector<Logic::LogicCommand>& commands,
                            bool cancelled = false)
{
    Logic::SessionContext ctx;
    // 只创建最小领域对象，正式工具自行处理临时画笔、提交和撤销。
    // 本用例没有音频回调或后台更新线程，结果在命令返回后即可检查。
    // 配置线性空时间线，测试不猜测每像素对应多少毫秒。
    // 正反向用同一套生产投影回读，才能发现终点或起点使用错误。
    ctx.currentBeatmap = std::make_shared<BeatMap>();
    ctx.currentTool    = Logic::EditTool::Draw;
    ctx.trackCount = ctx.draftTrackCount = 4;
    ctx.bgmTrackCount                    = 1;
    ctx.currentTime = ctx.animateTime             = 1;
    ctx.lastConfig.visual.trackLayout.left        = .1f;
    ctx.lastConfig.visual.trackLayout.right       = .5f;
    ctx.lastConfig.visual.judgeline_pos           = .5f;
    ctx.lastConfig.settings.professionalMode      = true;
    ctx.lastConfig.settings.enablePolylineEditing = true;
    // 折线能力保持开启，以证明简单绘制不是临时关闭用户全局配置。
    // 相机身份和视口大小匹配输入夹具，不能因缺少相机而跳过命令。
    // 玩家区域固定，横向移动跨过其他轨道时仍应保持最初轨道。
    // 滚动缓存只在初始化时建立，连续输入不得反复重建时间积分。
    ctx.cameras.emplace("DrawingCamera",
                        Logic::CameraInfo{ "DrawingCamera", 600, 700, 0 });
    auto& cache =
        ctx.timelineRegistry.ctx().emplace<Logic::System::ScrollCache>();
    cache.rebuild(
        ctx.timelineRegistry, ctx.lastConfig, ctx.currentBeatmap.get());
    // 旧对象没有任何历史动作，必须在提交、撤销和重做后完整保留。
    const auto                 old = ctx.noteRegistry.create();
    const Logic::NoteComponent existing{ .m_timestamp = 1, .m_trackIndex = 0 };
    ctx.noteRegistry.emplace<Logic::NoteComponent>(old, existing);
    ctx.noteRegistry.emplace<Logic::InteractionComponent>(old);
    // 旧对象位于起拍，必须在提交、撤销和重做之后保持原样。
    // 它不进入任何历史节点，后面检测的是对既有谱面的意外改写。
    // 生产控制器拥有正式工具实例，测试不能直接写 brushState 模拟结果。
    // 取消需要保持历史深度，不能用一次普通 Undo 假装取消预览。
    Logic::InteractionController controller(ctx);
    // 生命周期计数与几何检查分开，一次触摸只能拥有一个开始和结束。
    // End 本身不根据指针重算几何，最后一次 Update 必须先于它。
    // Start 携带的策略应在逻辑层锁存，后续 UI 切换不能修改同一笔语义。
    // 抓取的是生产命令，测试不在回放前更改标志来制造期望形状。
    int  starts{}, ends{}, cancels{};
    bool ended{};
    for ( const auto& command : commands ) {
        if ( const auto* c = std::get_if<Logic::CmdStartBrush>(&command) ) {
            check(!ended && c->m_noteHoldOnly,
                  "one touch stroke locks note/hold policy");
            ++starts;
            controller.handleCommand(*c);
        }
        if ( const auto* c = std::get_if<Logic::CmdUpdateBrush>(&command) ) {
            check(!ended, "no update after end");
            controller.handleCommand(*c);
        }
        if ( const auto* c = std::get_if<Logic::CmdEndBrush>(&command) ) {
            ended = true;
            ++ends;
            cancels += c->cancel;
            controller.handleCommand(*c);
        }
    }
    check(starts == 1 && ends == 1 && cancels == int(cancelled),
          "one complete drawing transaction");
    check(!ctx.brushState.isActive && !ctx.brushState.m_noteHoldOnly,
          "drawing state clears at end");
    const auto notes = ctx.noteRegistry.view<Logic::NoteComponent>();
    check(notes.size() == (cancelled ? 1U : 2U),
          "only one independent note created");
    const auto& retained = ctx.noteRegistry.get<Logic::NoteComponent>(old);
    check(retained.m_type == existing.m_type &&
              retained.m_timestamp == existing.m_timestamp &&
              retained.m_duration == 0 && retained.m_trackIndex == 0,
          "existing note never replaced or merged");
    // 场景中只有独立根对象，数量变化可发现误删旧对象或重复提交。
    // 类型、轨道和时长仍需独立断言，数量正确不代表笔画正确。
    // 取消不应留下可重做的中间删除动作；这与普通撤销有不同契约。
    // 新笔画完成后临时模式必须重置，否则下一次桌面起笔会继承策略。
    if ( cancelled ) {
        check(ctx.actionStack.getRedoStackSize() == 0,
              "cancel preserves redo history");
        check(ctx.actionStack.getUndoStackSize() == 0,
              "cancel preserves history");
        return {};
    }
    // 只在测试低频回读中遍历，不把 ECS 扫描引入生产连续绘制路径。
    Logic::NoteComponent result;
    for ( const auto e : notes )
        if ( e != old ) result = notes.get<Logic::NoteComponent>(e);
    check(ctx.actionStack.getUndoStackSize() == 1,
          "one stroke creates one undo step");
    // 只有测试的低频回读遍历 Note，不能把完整 ECS 扫描带入连续绘制。
    // 返回组件副本，不借用即将析构的 Registry 存储或实体指针。
    // 撤销只能移除新物件，重做沿用正式 Action 中保存的完整载荷。
    // 不能在 Undo 后重新按当前指针创建物件，这样会漏掉身份与历史错误。
    ctx.actionStack.undo(ctx);
    check(ctx.noteRegistry.view<Logic::NoteComponent>().size() == 1U,
          "undo removes only new note");
    ctx.actionStack.redo(ctx);
    check(ctx.noteRegistry.view<Logic::NoteComponent>().size() == 2U,
          "redo restores independent stroke");
    return result;
}
/// @brief 数值比较仅容忍投影浮点误差，不能用宽时间窗口遮掩终点丢失。
bool near(double a, double b)
{
    return std::abs(a - b) < 1e-7;
}
/// @brief 检查事务续写的真实输入派发、完整父子替换以及取消历史边界。
/// @param sourceType 原物件种类；空值表示空白处新画折线。
/// @param cancel 第二指接管时应保留原对象和既有重做分支。
/// @param stale 模拟另一编辑插入，本笔过期快照不得覆盖新内容。
/// @details 屏幕端点从生产滚动缓存计算，轨道中心与夹具内容区保持一致。
/// 测试不执行自己的续写算法，几何和撤销均由正式工具及动作实现。
/// @note 本组不创建 Vulkan 设备，测试失败不会被渲染初始化成败掩盖。
/// @note 夹具没有播放线程，状态回读发生在正式命令返回后。
/// @note 源类型为 Polyline 时检查完整父子数量，不能只检查根列表。
/// @note Note、Hold 和 Flick 分别覆盖三种转换入口，不能互相替代。
/// @note 所有场景的初始 redo 都非空，因此取消保留历史的检查有效。
/// @note 新画场景也经过相同种子历史，正式创建允许清除旧 redo 分支。
/// @note stale 只用于已有来源，空白绘制没有待替换快照可过期。
/// @note 拖动先纵向再横向再纵向，结果必须存在至少一次真实转折。
/// @note 命中盒只描述原末端，沿途空白位置不会误拾取另一来源。
/// @note 源根注释非空，成功续写和取消都要维持这个用户内容。
/// @note 原几何不由测试预先删除，生产路径必须自行完成原子替换。
/// @note 撤销及重做调用正式动作栈，不通过反向编辑模拟历史。
/// @note 这些 CPU 断言不代替手机和平板的真实事件及保存回读验收。
/// @note 夹具中的常量只定义固定测试视口，不进入生产触控阈值。
/// @note 临时持有的组件值副本在会话销毁前结束，不跨线程共享。
/// @note 任一断言失败继续记录其它边界，便于一次定位整笔事务的问题。
void verifyPolyline(std::optional<NoteType> sourceType, bool cancel = false,
                    bool stale = false)
{
    // 本夹具只拥有单线程会话，不依赖后台音频时钟或项目扫描完成时间。
    // 所有生产命令按发布顺序执行，不能在测试中先删原物件来模拟续写。
    // 原始输入来自 capture，测试不替换 CmdStartBrush 中的策略标志。
    // 没有真实平台窗口，屏幕坐标与逻辑相机都使用固定的内容区域。
    // 因而测试失败时可区分输入派发、几何状态机和历史提交三个边界。
    Logic::SessionContext ctx;
    ctx.currentBeatmap = std::make_shared<BeatMap>();
    ctx.currentTool    = Logic::EditTool::Draw;
    ctx.trackCount = ctx.draftTrackCount = 4;
    ctx.bgmTrackCount                    = 1;
    ctx.currentTime = ctx.animateTime       = 1;
    ctx.lastConfig.visual.trackLayout.left  = .1f;
    ctx.lastConfig.visual.trackLayout.right = .5f;
    ctx.lastConfig.visual.judgeline_pos     = .5f;
    // 缩放为跨拍转折保留足够视野，不能让固定像素位移被吸附成同拍零长度。
    ctx.lastConfig.visual.timelineZoom            = .25f;
    ctx.lastConfig.settings.professionalMode      = true;
    ctx.lastConfig.settings.enablePolylineEditing = true;
    // 相机键必须与捕获夹具一致，否则工具可能早退而掩盖续写失败。
    // 视口尺寸用于真实屏幕反投影，不按期望时长手工构造逻辑更新。
    // 原对象类型只影响来源和末端，后续拖动统一经过同一套触控序列。
    // 换方向时相邻两个样本都仍按住主指，只有最后样本负责结束。
    ctx.cameras.emplace("DrawingCamera",
                        Logic::CameraInfo{ "DrawingCamera", 600, 700, 0 });
    auto& cache =
        ctx.timelineRegistry.ctx().emplace<Logic::System::ScrollCache>();
    cache.rebuild(
        ctx.timelineRegistry, ctx.lastConfig, ctx.currentBeatmap.get());
    // 原根身份固定为首个分配实体，命中夹具与真实会话使用同一身份。
    // 原物件不进入历史，取消或撤销不能把它误当作本次新建对象删除。
    // 原根使用完整代际句柄，撤销恢复不能被新实体复用相同裸索引冒充。
    // 协作身份与本地实体身份分开核对，后续保存不能依赖易变的句柄。
    // 非空注释用于发现根替换丢失属性，默认空值不能覆盖这个回归。
    // 无来源的场景把原对象放到旁轨，提交也必须保留这个无关物件。
    const auto           old = ctx.noteRegistry.create();
    Logic::NoteComponent original;
    original.m_type            = sourceType.value_or(NoteType::NOTE);
    original.m_timestamp       = 1;
    original.m_trackIndex      = sourceType ? 0 : 3;
    original.m_collaborationId = "polyline-source";
    original.m_annotation      = "preserve source annotation";
    if ( original.m_type == NoteType::HOLD ) original.m_duration = .2;
    if ( original.m_type == NoteType::FLICK ) original.m_dtrack = 1;
    // 三段源折线包含纵、横、纵三种局部顺序，续写必须保留已有前缀。
    // 仅含一个段的样例不能暴露原子恢复时漏掉部分子实体的问题。
    // 子项保存在根内和独立 ECS 实体中，两份结构应由正式批次一起维护。
    // 普通 Hold 与 Flick 则作为单段来源，覆盖类型升级的另一入口。
    if ( original.m_type == NoteType::POLYLINE ) {
        original.m_subNotes = { { NoteType::HOLD, 1, .2, 0, 0 },
                                { NoteType::FLICK, 1.2, 0, 0, 1 },
                                { NoteType::HOLD, 1.2, .2, 1, 0 } };
    }
    ctx.noteRegistry.emplace<Logic::NoteComponent>(old, original);
    ctx.noteRegistry.emplace<Logic::InteractionComponent>(old);
    // 已有折线再带教学身份，检查复合提交没有遮蔽步骤返回所需的补偿入口。
    // 普通撤销恢复来源，教程返回则只移除本步骤延伸后的完整产物，两者不同。
    if ( original.m_type == NoteType::POLYLINE )
        ctx.walkthroughPracticeNotes[3] = { 123,
                                            old,
                                            original.m_collaborationId };
    // 组件值转交注册表后不再借用局部副本，测试观察只发生在命令返回以后。
    // 子实体必须同批替换和恢复，只比较根对象数会漏掉孤立旧子项。
    for ( std::size_t i = 0; i < original.m_subNotes.size(); ++i ) {
        const auto child = ctx.noteRegistry.create();
        auto       nc    = Logic::makeNoteComponentFromSubNote(
            original.m_subNotes[i], true, old, int(i));
        ctx.noteRegistry.emplace<Logic::NoteComponent>(child, std::move(nc));
        ctx.noteRegistry.emplace<Logic::InteractionComponent>(child);
    }
    // 原对象总数包括父子实体，这个基线在历史种子动作之前捕获。
    // 临时笔画不应生成正式子实体，只有 EndBrush 可以改变此数量。
    // 后续取消同时检查数量和源属性，不能只因原根仍在就视为成功。
    // 新结果的根与旧根身份分别跟踪，避免实体复用造成假阳性。
    const auto originalCount =
        ctx.noteRegistry.view<Logic::NoteComponent>().size();
    // 先制造真实非空重做分支，取消必须保留它，空重做栈不能证明历史安全。
    ctx.actionStack.pushAndExecute(
        std::make_unique<Logic::NoteAction>(
            Logic::NoteAction::Type::Create,
            entt::null,
            std::nullopt,
            Logic::NoteComponent{ .m_timestamp = 5, .m_trackIndex = 3 }),
        ctx);
    ctx.actionStack.undo(ctx);
    // 撤销种子后 redo 非空，而 undo 为空，适合发现隐式删除清空重做的回归。
    // 预览期间不能增修订号，保存和联机观察者不应读到半条被删除的路径。
    // 正式提交后清除旧重做分支是正常编辑行为，只有取消必须完整保留。
    // 过期场景单独推进修订，验证冲突保护并非只检查实体是否仍有效。
    const auto revision = ctx.actionStack.captureSaveRevision();
    const auto y        = [&](double time) {
        return 350.f - float(cache.getAbsY(time) - cache.getAbsY(1));
    };
    double       tail  = original.m_type == NoteType::HOLD       ? 1.2
                         : original.m_type == NoteType::POLYLINE ? 1.4
                                                                 : 1;
    int          track = original.m_type == NoteType::FLICK ||
                                 original.m_type == NoteType::POLYLINE
                             ? 1
                             : 0;
    const ImVec2 start{ 90.f + 60 * track, y(tail) };
    // 新画和续写都经过横移再纵移的转折；不能只得到一个普通 Hold 也算通过。
    // 以真实时间增量定义跨拍转折，兼容用户磁吸默认开启的配置。
    // 第一腿与末腿各跨半拍，避免把合法的同拍 Flick 合并误报为漏段。
    const float         turnY = y(tail + .25), endY = y(tail + .5);
    std::vector<Sample> samples{ { start, true },
                                 { { start.x, turnY }, true },
                                 { { start.x + 60, turnY }, true },
                                 { { start.x + 60, endY }, true } };
    if ( cancel ) {
        samples.push_back({ samples.back().m_point, true, true });
        samples.push_back({ samples.back().m_point, false, true });
    }
    samples.push_back({ samples.back().m_point, false });
    // 末端命中盒模拟渲染输出，不直接写入逻辑层 hoveredEntity。
    // 生产控制器仍要发出悬停命令，再根据同一帧的原始落点启动绘制。
    // HoldEnd 和 FlickArrow 分开设置，身体起笔不能冒充合法续写末端。
    // 已有折线还必须命中最后子索引，内部节点不能截断整条路径。
    std::optional<Common::Render::Hitbox> hit;
    if ( sourceType ) {
        const auto part = original.m_type == NoteType::NOTE
                              ? Common::Render::HoverPart::Head
                          : original.m_type == NoteType::FLICK
                              ? Common::Render::HoverPart::FlickArrow
                              : Common::Render::HoverPart::HoldEnd;
        hit =
            Common::Render::Hitbox{ old,
                                    part,
                                    original.m_type == NoteType::POLYLINE ? 2
                                                                          : -1,
                                    start.x - 20,
                                    start.y - 12,
                                    40,
                                    24 };
    }
    Logic::InteractionController controller(ctx);
    // 起笔之后立即观察正式数据，不等结束再推测中间是否曾经误删。
    // Begin 产生的策略必须来自触控方案，不由逻辑回放临时补写。
    // 第二指取消和过期提交都要从真实控制器入口结束临时状态。
    // 每个场景拥有独立事件订阅，上一轮的触摸不能污染下一轮命令。
    // 每组记录来源与真实命令几何，失败输出可区分输入漏点和吸附后的合法合段。
    XINFO("Touch polyline source={} cancel={} stale={}",
          int(original.m_type),
          cancel,
          stale);
    bool began{};
    for ( const auto& command :
          capture(samples, "touch-gestures", false, hit) ) {
        std::visit(
            [&](const auto& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr ( std::is_same_v<T, Logic::CmdStartBrush> ) {
                    check(!c.m_noteHoldOnly && c.m_transactionalResume,
                          "polyline mode supplies transactional intent");
                    controller.handleCommand(c);
                    began = true;
                    check(ctx.actionStack.captureSaveRevision() == revision &&
                              ctx.actionStack.getRedoStackSize() == 1 &&
                              ctx.noteRegistry.view<Logic::NoteComponent>()
                                      .size() == originalCount,
                          "resume preview leaves original data and redo "
                          "unchanged");
                } else if constexpr ( std::is_same_v<T, Logic::CmdEndBrush> ) {
                    if ( stale ) ctx.actionStack.markDirty();
                    controller.handleCommand(c);
                } else if constexpr (
                    std::is_same_v<T, Logic::CmdUpdateBrush> ||
                    std::is_same_v<T, Logic::CmdSetHoveredEntity> ||
                    std::is_same_v<T, Logic::CmdSelectEntity> ) {
                    controller.handleCommand(c);
                }
            },
            command);
    }
    check(began && !ctx.brushState.isActive && !ctx.brushState.m_pendingResume,
          "polyline gesture completes and clears temporary state");
    // 两类拒绝共享不提交约束：多指主动取消与历史修订过期。
    // 它们都不能留下空操作，也不能生成仅恢复删除动作的中间 redo 节点。
    // 源根和子实体需要同时存在，单独恢复其中一份会产生孤立或重复数据。
    // 取消之后不继续执行成功路径，避免用后续 Undo 掩盖取消产生的副作用。
    if ( cancel || stale ) {
        check(ctx.noteRegistry.valid(old) &&
                  ctx.noteRegistry.view<Logic::NoteComponent>().size() ==
                      originalCount &&
                  ctx.actionStack.getUndoStackSize() == 0 &&
                  ctx.actionStack.getRedoStackSize() == 1,
              "cancel or stale resume preserves original parent children and "
              "history");
        check(ctx.noteRegistry.get<Logic::NoteComponent>(old).m_annotation ==
                  original.m_annotation,
              "cancel preserves source attributes");
        return;
    }
    // 完整路径、根父子结构与一次撤销独立检查，数量相同不能替代形状正确。
    // 最后一个样本已经抬手，生产桥必须在 End 之前补交这个真实终点。
    // 新 Hold 的初始持续若仍为零，会被提交清洗删除，故必须检查最后一段。
    // 根候选与子实体区分开，子数量增加不等于真的创建了折线根。
    // 最少两段保证转向成功，普通单键或长条不能冒充本组的期望结果。
    // 原根只有续写场景才被替换，空白新画仍要保留旁轨的原有单键。
    // 一笔只允许一个历史节点，删除和创建分开记录会在这里被发现。
    entt::entity created = entt::null;
    for ( const auto entity : ctx.noteRegistry.view<Logic::NoteComponent>() ) {
        const auto& nc = ctx.noteRegistry.get<Logic::NoteComponent>(entity);
        if ( entity != old && !nc.m_isSubNote ) {
            created = entity;
            if ( sourceType )
                check(nc.m_annotation == original.m_annotation,
                      "resume retains source annotation");
            // 最后一帧的纵向转折不能丢失；仅检查 Polyline
            // 类型会放过少一段的错误。
            // 真实末端应落在下一轨并向前延伸，而不是保留零长度活动 Hold。
            check(nc.m_type == NoteType::POLYLINE && nc.m_subNotes.size() >= 3,
                  "turning touch path creates real polyline");
            if ( !nc.m_subNotes.empty() ) {
                const auto& end = nc.m_subNotes.back();
                check(end.type == NoteType::HOLD && end.duration > 0 &&
                          end.trackIndex == track + 1 &&
                          end.timestamp + end.duration > tail,
                      "release preserves final vertical leg and endpoint");
            }
        }
    }
    check(created != entt::null && ctx.actionStack.getUndoStackSize() == 1,
          "drawing or resume is exactly one undo step");
    if ( sourceType )
        check(!ctx.noteRegistry.valid(old),
              "resume replaces original root at commit");
    ctx.actionStack.undo(ctx);
    check(
        ctx.noteRegistry.valid(old) &&
            ctx.noteRegistry.view<Logic::NoteComponent>().size() ==
                originalCount &&
            ctx.noteRegistry.get<Logic::NoteComponent>(old).m_subNotes.size() ==
                original.m_subNotes.size(),
        "one undo restores original root and children");
    // 再次执行正式 redo，验证复合动作在撤销后仍持有完整创建载荷。
    // 子动作的执行顺序须与首次提交相同，不能依赖已经销毁的预览状态。
    // 本地笔刷已清空，所以重做不能从当前指针重新推算或补齐路径。
    // 最终状态不会写入用户工程，设备验收另行覆盖文件保存回读。
    ctx.actionStack.redo(ctx);
    check(ctx.actionStack.getUndoStackSize() == 1,
          "one redo restores complete polyline transaction");
    // 补偿仍通过正式动作栈执行，不能在测试中直接销毁创建的子实体。
    // 原来源也属于同一练习槽，返回步骤不能把已经替代的旧根复活。
    if ( original.m_type == NoteType::POLYLINE ) {
        ctx.actionStack.rollbackWalkthrough(123, ctx);
        check(ctx.noteRegistry.view<Logic::NoteComponent>().size() == 0,
              "resumed tutorial polyline retains targeted rollback");
    }
}

/// @brief 配色触点复用真实生产输入，连续拖涂只能在释放交付一个批次。
/// @details 与擦除共用固定命中框，采样同号目标必须被忽略。
/// 颜色断言使用命令里的起笔值，而不是测试自己重演取色或命中算法。
/// @warning 同步 CPU 回归，不连接设备、不等待真实时钟。
void testTouchColors()
{
    // 用生产解释器建立从 Down 到尾部空闲的完整事件序列。
    // 不直接调用移动局部收集器，避免测试与实现共享命中结果。
    // 快照只提供渲染器本来会发布的几何，不能预先标记待涂对象。
    // 两种配色工具共用相同夹具，消除输入来源不同造成的假对照。
    const auto run = [&](std::vector<Sample> points,
                         int                 count,
                         Logic::EditTool     tool) {
        const auto commands =
            capture(points, "touch-gestures", true, std::nullopt, false, tool);
        // 统计完整生命周期内的全部提交，不以最后一个命令代替整笔计数。
        // 捕获集同时保留桌面立即染色命令，保证能发现意外穿透。
        // 目标身份是带代数的实体值，不以坐标或颜色相似性代替身份。
        int batches = 0;
        for ( const auto& command : commands ) {
            // 桌面逐实体路径在触屏必须完全被屏蔽，否则取消会留下已染物件。
            check(!std::holds_alternative<Logic::CmdClearNoteColorOverrides>(
                      command),
                  "touch clear never uses desktop immediate command");
            check(!std::holds_alternative<Logic::CmdStartBrush>(command),
                  "color stroke never draws notes");
            if ( const auto* color =
                     std::get_if<Logic::CmdApplyBrushPaletteToEntity>(
                         &command) ) {
                // 桌面逐实体命令也使用同一类型，因此必须检查显式批次字段。
                // 有命令但缺批次不能仅凭总数相同被当作正确触屏行为。
                // 错误时继续检查后续命令，不让首个失败掩盖重复提交。
                ++batches;
                check(color->m_stroke.has_value(),
                      "color stroke always has explicit batch");
                if ( !color->m_stroke ) continue;
                const auto& stroke = *color->m_stroke;
                // 同号采样不能重复记录；领域过滤发生在收集而非后台猜测。
                check(stroke.m_targets.size() ==
                              static_cast<std::size_t>(count) &&
                          stroke.m_instance == 1,
                      "color targets and instance preserved");
                // 清除与设置携带同样的目标，却具有不同的业务含义。
                // 不用空颜色模拟清除，否则尚未选择画笔也会擦掉旧配色。
                // 颜色本身锁在载荷里，取消之后不应再查询变化中的画笔。
                check(stroke.m_clear == (tool == Logic::EditTool::ColorEraser),
                      "clear and paint remain distinct");
                check(stroke.m_colors.head ==
                          std::optional{ glm::vec4(1, 0, 0, 1) },
                      "stroke locks brush color");
            }
        }
        // 包含所有尾部空闲帧，防止系统取消之后补发迟到的轻点。
        // 没有目标时要求完全不发命令，不能发送一个空批次再依靠后台吞掉。
        // 有目标时只接受一个批次，不接受按物件数量拆分成多条历史。
        // 尾部帧保留同一指针位置，专门发现抬手后误复用悬浮的提交。
        check(batches == int(count > 0), "one color action only on release");
    };
    // 两种工具逐项使用同一组 Down／Move／Up，防止仅适配了染色入口。
    // 系统取消和多指接管均要阻止清除旧色，不能只阻止添加新色。
    // 运行之间重新建立输入呈现状态，不共享上一笔的候选或导航归属。
    for ( auto tool :
          { Logic::EditTool::ColorBrush, Logic::EditTool::ColorEraser } ) {
        // 阳性轻点必须真正命中，不能靠屏蔽所有触点让取消用例通过。
        // 三个框之间留空白，连续扫动验证跨越而非只检查停留的终点。
        // 同路径返回验证去重，采样与首个音符同号验证领域过滤。
        run({ { { 120, 310 }, true }, { { 120, 310 }, false } }, 1, tool);
        run({ { { 80, 310 }, true },
              { { 460, 310 }, true },
              { { 80, 310 }, true },
              { { 80, 310 }, false } },
            3,
            tool);
        // 快速移动和松手同帧仍补交经过的薄框，不能只涂起点。
        run({ { { 80, 310 }, true }, { { 460, 310 }, false } }, 3, tool);
        run({ { { 80, 310 }, true }, { { 80, 310 }, false } }, 0, tool);
        // 空白轻点保留空目标，不允许回退当前选中或悬停物件。
        // 快速释放用不同终点，防止只回放起点的旧缺陷再次出现。
        // 多指接管与系统 Cancel 分别覆盖已经有候选的活动笔画。
        run({ { { 80, 310 }, true },
              { { 250, 310 }, true },
              { { 460, 310 }, true, true },
              { { 460, 310 }, false } },
            0,
            tool);
        run({ { { 80, 310 }, true },
              { { 250, 310 }, true },
              { { 460, 310 }, true, false, true },
              { { 460, 310 }, false } },
            0,
            tool);
        run({ { { 120, 310 }, true },
              { { 120, 310 }, true, false, false, false, true },
              { { 120, 310 }, false } },
            0,
            tool);
        run({ { { 80, 310 }, true },
              { { 250, 310 }, true },
              { { 460, 310 }, true, false, false, true },
              { { 460, 310 }, false } },
            0,
            tool);
        // 切谱发生在尚未开始拖动的轻点候选阶段，同样不能延后提交。
        // 播放场景先产生有效候选，再切换状态，不以无目标解释拒绝。
        // 双指场景让第一指已经移动，避免只测尚未起笔的空取消。
        // 已经扫过物件后打开普通调色浮层，旧候选必须作废。
        // 浮层出现的下一帧就抬手，验证关闭后不会补交迟到释放。
        // 与系统取消分开，避免仅覆盖原始触点 Cancel 而遗漏普通窗口。
        run({ { { 80, 310 }, true },
              { { 250, 310 }, true },
              { { 460, 310 }, true, false, false, false, false, true },
              { { 460, 310 }, false } },
            0,
            tool);
        // 静止长按不是选择菜单，不通过右键或键盘实现涂色。
        std::vector<Sample> held(14, Sample{ { 120, 310 }, true });
        held.push_back({ { 120, 310 }, false });
        run(held, 1, tool);
    }
    // 经典方案仍由原桌面控制器逐目标即时提交，不承接移动批次状态。
    // 阳性点击使用同一命中框，确保桌面对照不是简单忽略输入。
    // 本断言不要求桌面采用触屏的释放时机，保留原工具的连续行为。
    const auto desktop =
        capture({ { { 120, 310 }, true }, { { 120, 310 }, false } },
                "classic-desktop",
                true,
                std::nullopt,
                false,
                Logic::EditTool::ColorBrush);
    bool desktopPaint = false;
    for ( const auto& command : desktop ) {
        if ( const auto* paint =
                 std::get_if<Logic::CmdApplyBrushPaletteToEntity>(&command) ) {
            desktopPaint = true;
            check(!paint->m_stroke,
                  "classic color keeps immediate single-entity command");
        }
    }
    // 实际观测到旧单目标命令后才判通过，不能以没有新批次作为唯一证据。
    // 未修改物理修饰键，也未用触控方案的临时状态替代经典控制器。
    check(desktopPaint, "classic desktop color still responds to click");
}
/// @brief 配色批次、选区填色及清除都复用正式历史栈验证可逆性。
/// @details 根和投影命中合并，属性与其它领域不能随配色改变。
/// 无变化、空列表、过期实例、播放拒绝必须保留已有重做。
void testColorActions()
{
    // 历史模型回归与触点捕获分开：一个检查意图，一个检查可逆业务状态。
    // 会话只在本用例存活，不借用真实编辑器、配置文件或用户谱面。
    // 原数据不是通过历史创建，Undo 不会顺带撤销测试夹具的搭建。
    // 颜色字段与几何、批注及采样分别作为哨兵，避免只检查屏幕视觉。
    Logic::SessionContext ctx;
    ctx.currentBeatmap                       = std::make_shared<BeatMap>();
    ctx.lastConfig.settings.professionalMode = true;
    // 专业模式与折线能力在原正式配置上开启，而不是绕过权限分支。
    // 后续关闭许可用例会复用同一组有效目标，保证门禁存在阳性对照。
    // 未开启能力的草稿不能因为画面上仍然可见就被意外改色。
    ctx.lastConfig.settings.enablePolylineEditing = true;
    const auto root                               = ctx.noteRegistry.create(),
               child                              = ctx.noteRegistry.create(),
               other                              = ctx.noteRegistry.create();
    // 根使用合法三段正交结构；颜色修改不能重写其类型、接缝和元数据。
    Logic::NoteComponent before{ .m_type       = NoteType::POLYLINE,
                                 .m_timestamp  = 1,
                                 .m_trackIndex = 1 };
    // Hold 尾与 Flick 同时刻，Flick 末轨与后 Hold 同轨，满足原正交契约。
    // 测试只读生产颜色助手的结果，不自行把节点着色后再声称命令正确。
    // 原根批注是独立保存字段，颜色事务不能只重建外观而丢失它。
    before.m_subNotes = { { .type       = NoteType::HOLD,
                            .timestamp  = 1,
                            .duration   = .5,
                            .trackIndex = 1 },
                          { .type       = NoteType::FLICK,
                            .timestamp  = 1.5,
                            .trackIndex = 1,
                            .dtrack     = 1 },
                          { .type       = NoteType::HOLD,
                            .timestamp  = 1.5,
                            .duration   = .5,
                            .trackIndex = 2 } };
    // 原几何首尾与根锚点一致；没有用不合法子列表掩盖根级传播行为。
    // 一个子投影足以验证解析父根，权威保存结构仍是内嵌三段列表。
    // 不调用结构修复器，配色不能凭空修改或规范化任何谱面几何。
    before.m_annotation = "keep annotation";
    ctx.noteRegistry.emplace<Logic::NoteComponent>(root, before);
    ctx.noteRegistry.emplace<Logic::NoteComponent>(
        child,
        Logic::NoteComponent{ .m_type           = NoteType::HOLD,
                              .m_timestamp      = 1,
                              .m_duration       = .5,
                              .m_trackIndex     = 1,
                              .m_isSubNote      = true,
                              .m_parentPolyline = root,
                              .m_subIndex       = 0 });
    ctx.noteRegistry.emplace<Logic::NoteComponent>(
        other, Logic::NoteComponent{ .m_timestamp = 9 });
    ctx.noteRegistry.emplace<Logic::InteractionComponent>(other).isSelected =
        true;
    // 样本实体与根恰好同号，两个 Registry 的领域边界不能靠数值区分。
    // 负偏移选为非零哨兵，染色后不能被错误重置成默认触发点。
    // 该对象不参与音符命令目标，断言其原值可发现跨域写入。
    const auto sample = ctx.sampleRegistry.create();
    ctx.sampleRegistry.emplace<Logic::SampleComponent>(
        sample, Logic::SampleComponent{ .m_timestamp = 3, .m_offsetMs = -24 });
    const auto instance =
        reinterpret_cast<std::uintptr_t>(ctx.currentBeatmap.get());
    Common::Render::NoteColorOverrides red;
    red.head = red.hold = glm::vec4(1, 0, 0, 1);
    Logic::ActionController                   controller(ctx);
    const Logic::CmdApplyBrushPaletteToEntity paint{
        entt::null,
        Logic::NotePaletteStroke{ instance, { child, root, child }, red, false }
    };
    // 输入刻意同时列出根和重复子项，不能由测试先去重再交给业务端。
    // 正式处理器应解析父根后合并，最终仅占一个可逆历史槽位。
    // 配色不临时取消选区，无关选中音符必须保留自己的原始颜色。
    controller.handleCommand(paint);
    // 投影和根只形成一个历史槽，无关选区与同号采样仍保持原值。
    const auto& colored = ctx.noteRegistry.get<Logic::NoteComponent>(root);
    check(colored.m_customColors.head == red.head &&
              colored.m_annotation == before.m_annotation,
          "stroke applies colors and retains annotation");
    // 几何合法性独立于颜色相等，颜色正确不能抵扣原生折线结构被破坏。
    // 同一根带多个投影目标时，后台只应替换权威根而非拆出新物件。
    // 不使用实体数量变化作为填色成功条件，配色本身不应创建或删除实体。
    check(Common::isCanonicalPolyline(colored.m_subNotes, 1, 1, 0, 3),
          "color preserves canonical geometry");
    check(ctx.actionStack.getUndoStackSize() == 1 &&
              !ctx.noteRegistry.get<Logic::NoteComponent>(other)
                   .m_customColors.head,
          "stroke leaves unrelated selected note unchanged");
    check(ctx.sampleRegistry.get<Logic::SampleComponent>(sample).m_offsetMs ==
              -24,
          "color ignores sample registry");
    controller.handleCommand(paint);
    // 重复涂相同颜色必须在真实已染色数据上执行，不能用空命中代替。
    // 不创建空动作可保证用户撤销一次就回到本次实际编辑前的状态。
    // 后台不能为避免空动作而先执行一次修改再用 Undo 回滚。
    check(ctx.actionStack.getUndoStackSize() == 1,
          "same color does not create empty history");
    // 清除前已经有真实覆写，必须产生有效动作；这不是空结果阳性测试。
    // 使用完整清除标志，不逐槽提交而制造多次撤销才能恢复的结果。
    // 撤销清除应回到红色，再撤销原笔画应恢复无覆写状态。
    controller.handleCommand(Logic::CmdApplyBrushPaletteToEntity{
        entt::null, Logic::NotePaletteStroke{ instance, { root }, {}, true } });
    check(!ctx.noteRegistry.get<Logic::NoteComponent>(root).m_customColors.head,
          "color eraser clears only overrides");
    ctx.actionStack.undo(ctx);
    check(
        ctx.noteRegistry.get<Logic::NoteComponent>(root).m_customColors.head ==
            red.head,
        "undo clear restores exact colors");
    ctx.actionStack.undo(ctx);
    // 无效请求执行在已有重做上，不能通过清历史让断言假通过。
    // 空载荷不借用当前已选物件；播放中也不补交之前排队的笔画。
    controller.handleCommand(Logic::CmdApplyBrushPaletteToEntity{
        entt::null, Logic::NotePaletteStroke{ instance, {}, red, false } });
    controller.handleCommand(Logic::CmdApplyBrushPaletteToEntity{
        entt::null,
        Logic::NotePaletteStroke{ instance + 1, { root }, red, false } });
    ctx.isPlaying = true;
    controller.handleCommand(paint);
    ctx.isPlaying = false;
    check(ctx.actionStack.getUndoStackSize() == 0 &&
              ctx.actionStack.getRedoStackSize() == 2,
          "invalid color requests preserve redo");
    // 上述无效批次均执行在重做哨兵存在时，才能发现错误的清栈行为。
    // 再实际重做一次，避免仅检查栈深却没有验证动作内容仍然可执行。
    // 重做结果取实际组件，不取测试缓存的期望对象冒充恢复状态。
    ctx.actionStack.redo(ctx);
    check(
        ctx.noteRegistry.get<Logic::NoteComponent>(root).m_customColors.head ==
            red.head,
        "redo restores palette batch");
    // 选区填色使用既有命令，只有显式移动清除标志改变操作含义。
    std::array<glm::vec4, Logic::NOTE_COLOR_SLOT_COUNT> blue;
    // 选区填色使用另一种明显颜色，与先前笔画目标区分开。
    // 完整六槽数组能发现只更新当前可见部件而漏掉其它槽位的问题。
    // 非选中根继续保持红色，选区身份不由 UI 可见范围截断。
    blue.fill({ 0, 0, 1, 1 });
    controller.handleCommand(
        Logic::CmdApplyNotePaletteToSelection{ blue, false, true });
    check(
        ctx.noteRegistry.get<Logic::NoteComponent>(other).m_customColors.tap ==
            std::optional{ glm::vec4(0, 0, 1, 1) },
        "explicit selection fill changes selected object");
    check(
        ctx.noteRegistry.get<Logic::NoteComponent>(root).m_customColors.head ==
            red.head,
        "selection fill does not recolor unselected root");
    controller.handleCommand(
        Logic::CmdApplyNotePaletteToSelection{ blue, true, true });
    // 清除全部配色恢复继承语义，不把某个当前皮肤颜色写回物件。
    // 这与设置透明色不同，因此检查 optional 为空而不只检查 RGBA 值。
    // 填色与清除分别占一条真实动作，撤销只恢复最近一次确认。
    check(!ctx.noteRegistry.get<Logic::NoteComponent>(other).m_customColors.tap,
          "selection clear removes all overrides");
    ctx.actionStack.undo(ctx);
    check(
        ctx.noteRegistry.get<Logic::NoteComponent>(other).m_customColors.tap ==
            std::optional{ glm::vec4(0, 0, 1, 1) },
        "one undo restores selection colors");
    // 相同填色不能丢失刚才清除操作的重做。
    controller.handleCommand(
        Logic::CmdApplyNotePaletteToSelection{ blue, false, true });
    // 刚撤销的清除仍应能重做，相同填色请求不应该剪断这条历史分支。
    // 再次确认是无变化离散操作，与活动笔画取消的路径分别验证。
    // 此处不调用保存标记或清理历史，结果完全来自正式动作栈。
    check(ctx.actionStack.getRedoStackSize() == 1,
          "no-op selection fill preserves redo");
    // 播放期间保留有效选区与已有颜色，拒绝必须来自活动交互门禁。
    // 无效确认不能清空之前的重做，也不能悄悄更改选区或已有颜色。
    // 普通物件填色不额外要求专业模式，继续复用桌面的类型权限。
    ctx.lastConfig.settings.professionalMode = false;
    ctx.isPlaying                            = true;
    controller.handleCommand(
        Logic::CmdApplyNotePaletteToSelection{ blue, true, true });
    check(ctx.actionStack.getRedoStackSize() == 1,
          "playing mobile fill preserves redo");
    ctx.isPlaying = false;
    // 停止播放后同一普通物件可以确认清色，不能因关闭专业模式而变成死按钮。
    // 这里具有真实已染色选区，与空选择的无操作用例明确区分。
    // 保持原选择组件不变，不通过重选或换工具绕过普通模式的能力检查。
    controller.handleCommand(
        Logic::CmdApplyNotePaletteToSelection{ blue, true, true });
    check(
        !ctx.noteRegistry.get<Logic::NoteComponent>(other).m_customColors.tap,
        "ordinary selection colors remain editable without professional mode");
}

/// @brief 真实触点擦除只在释放发一次显式删除，不启动画笔或桌面右键擦除。
/// @details 空白起笔、短帧扫过、往返去重、混合域同号以及全部取消边界独立断言。
/// 不向 UI 伪造已擦除的快照，预览期间模型始终保持原样。
/// @warning 仅同步 CPU 输入，不操作真实设备或用户工程。
/// 测试边界：
/// - 原始触点和 ImGui 合成鼠标仍按后端正常顺序推进。
/// - 不直接构造擦除意图输出，也不绕开 SurfaceScope。
/// - 固定命中框仅替代渲染发布的数据，不包含删除或选择结果。
/// - 空闲尾帧继续推进，检查取消后是否补发延迟的轻点。
/// - 同号 Note 与 Sample 必须各留一个显式目标。
/// - 取消不依赖关闭窗口或销毁整个编辑器。
/// - 经典方案接到相同触控偏好设置仍忽略该标量。
/// - 绘制和擦除不能在同一序列中同时提交。
/// - 命令数量按完整笔画统计，不能按移动帧统计。
/// - 移动阈值以下的轻点必须有阳性删除对照。
/// - 手势跨过薄框而停在空白时仍需保留已扫过对象。
/// - 系统取消用原始快照表达，不能伪装成普通抬手。
/// - 播放与切谱分别验证，不能合并为一个提前返回场景。
/// - 本测试不保存 APK 或用户配置，设备注入另行验收。
void testTouchEraser()
{
    const auto run = [&](std::vector<Sample> samples,
                         int                 targets,
                         std::string_view    profile = "touch-gestures") {
        const auto commands =
            capture(samples, profile, true, std::nullopt, true);
        // 累计的是整个输入序列的提交，尾部空闲帧也包括在内。
        // 模式选择只改变触控呈现，桌面对照不使用此计数替代画笔阳性检查。
        int deletions = 0;
        // 仅捕获擦除场景的指针状态，不扩大普通绘制用例的序列。
        // 抬手后必须撤去悬浮，不能留下需要鼠标停留的吸附反馈。
        std::optional<bool> hovering;
        for ( const auto& command : commands ) {
            if ( const auto* pointer =
                     std::get_if<Logic::CmdSetMousePosition>(&command) )
                hovering = pointer->isHovering;
            if ( const auto* deletion =
                     std::get_if<Logic::CmdDeleteSelected>(&command) ) {
                // 命令必须携带原实例及明确列表，不能发送默认的删除当前选择。
                // 数量检查包括领域相同才去重的要求，返程不会再追加同一对象。
                ++deletions;
                check(deletion->m_objects &&
                          deletion->m_objects->m_beatmapInstanceId == 1 &&
                          deletion->m_objects->m_targets.size() ==
                              static_cast<std::size_t>(targets),
                      "erase preserves explicit domain targets and original "
                      "instance");
            }
            // 已有画笔命令同样必须为零，否则擦除会边删边画。
            if ( profile == "touch-gestures" )
                check(!std::holds_alternative<Logic::CmdStartBrush>(command),
                      "eraser never starts drawing");
        }
        // 尾部空闲帧已排空释放回放，最后指针状态必须表示手指离开。
        // 经典鼠标仍允许停在窗口内悬浮，不能强加触控的收尾约束。
        if ( profile == "touch-gestures" )
            check(hovering && !*hovering,
                  "finger release clears erase hover feedback");
        check(deletions == int(targets > 0),
              "one batch only on normal erase release");
    };
    // 每个场景重新创建解释器，避免上一笔的容量或实体身份遮掩状态错误。
    // 阳性对照证明并非简单屏蔽所有输入导致后面的取消测试通过。
    // 点按命中一个；空白轻点不能删除当前选择或悬浮残留。
    run({ { { 120, 310 }, true }, { { 120, 310 }, false } }, 1);
    run({ { { 80, 310 }, true }, { { 80, 310 }, false } }, 0);
    // 两个位置之间一帧扫过四个对象，释放空白仍保留整笔目标。
    // 同一路径回扫不能重复删除，Note 与 Sample 的同号身份都应存在。
    run({ { { 80, 310 }, true },
          { { 460, 310 }, true },
          { { 80, 310 }, true },
          { { 80, 310 }, false } },
        4);
    // 快速移动与抬手同帧到达，也必须补交整个扫动线段。
    run({ { { 80, 310 }, true }, { { 460, 310 }, false } }, 4);
    // 起笔后和开始前的第二指、系统取消、播放与切谱都不发迟到删除。
    // 各序列留有真正移动，防止仅因为没有目标而使取消测试偶然通过。
    run({ { { 80, 310 }, true },
          { { 250, 310 }, true },
          { { 460, 310 }, true, true },
          { { 460, 310 }, false } },
        0);
    run({ { { 120, 310 }, true, true }, { { 120, 310 }, false } }, 0);
    run({ { { 80, 310 }, true },
          { { 250, 310 }, true },
          { { 460, 310 }, true, false, true },
          { { 460, 310 }, false } },
        0);
    run({ { { 80, 310 }, true },
          { { 250, 310 }, true },
          { { 460, 310 }, true, false, false, true },
          { { 460, 310 }, false } },
        0);
    run({ { { 80, 310 }, true },
          { { 250, 310 }, true },
          { { 460, 310 }, true, false, false, false, true },
          { { 460, 310 }, false } },
        0);
    // 还未跨过拖动门槛的轻点也不能跨实例回放，原点命中有效物件。
    // 与上面已开始扫动后切谱分开，防止只检查活动擦除标志。
    run({ { { 120, 310 }, true },
          { { 120, 310 }, true, false, false, false, true },
          { { 120, 310 }, false } },
        0);
    // 长按绘制模式不弹选择菜单；释放后仍仅删除一笔中的一个目标。
    // 固定帧时钟超过长按阈值，不以真正 sleep 影响宿主或测试可重复性。
    std::vector<Sample> held(14, Sample{ { 120, 310 }, true });
    held.push_back({ { 120, 310 }, false });
    run(held, 1);
    // 设置触控偏好不能改变经典方案，经典输入仍走原画笔，不发显式擦除。
    run({ { { 120, 310 }, true }, { { 120, 310 }, false } },
        0,
        "classic-desktop");
}
/// @brief 显式批次沿用既有删除动作，验证根子级联、领域隔离和一次精确历史。
/// @details 选中的无关 Note 保留；空、过期和受限目标不能偷用选择或清重做。
/// 这里直接回放语义命令，不在测试中重写删除、级联和撤销算法。
/// 模型验收边界：
/// - 原根、子投影及采样均在输入之前已存在，不属于新绘制历史。
/// - 选中的无关物件作为范围哨兵，不能随目标根一起被误删。
/// - 显式列表包含重复根及重复采样，必须在领域内去重。
/// - 子实体命中提升到根，避免仅删除投影后留下不可见父内容。
/// - 一次 Undo 同时恢复两个 Registry 的原始实体句柄。
/// - 样本负偏移和根批注用于发现仅重建几何而遗失属性。
/// - 过期实例命令不允许使用当前选择作为替代目标。
/// - 空列表与空 optional 的默认桌面含义不同。
/// - 领域错误的有效句柄也必须拒绝，不能仅验证索引存在。
/// - 停止播放后旧取消笔画不能自动补提交。
/// - 每种无效请求都在已有重做栈的情况下执行。
/// - 专业模式关闭时采样仍可展示，但不允许本命令编辑。
/// - 使用生产 ActionController 和 EditorActionStack，不手写撤销结果。
void testEraseDeletion()
{
    Logic::SessionContext ctx;
    ctx.currentBeatmap                            = std::make_shared<BeatMap>();
    ctx.lastConfig.settings.professionalMode      = true;
    ctx.lastConfig.settings.enablePolylineEditing = true;
    const auto           parent                   = ctx.noteRegistry.create(),
                         child                    = ctx.noteRegistry.create(),
                         other                    = ctx.noteRegistry.create();
    Logic::NoteComponent root{ .m_type       = NoteType::POLYLINE,
                               .m_timestamp  = 1,
                               .m_trackIndex = 1 };
    // 使用合法 Hold→Flick→Hold，不能用空子列表冒充可级联的折线。
    // 原根时间、轨道与首段一致；两个接缝分别保持时间和轨道连续。
    root.m_subNotes = { { .type       = NoteType::HOLD,
                          .timestamp  = 1,
                          .duration   = .5,
                          .trackIndex = 1 },
                        { .type       = NoteType::FLICK,
                          .timestamp  = 1.5,
                          .trackIndex = 1,
                          .dtrack     = 1 },
                        { .type       = NoteType::HOLD,
                          .timestamp  = 1.5,
                          .duration   = .5,
                          .trackIndex = 2 } };
    check(Common::isCanonicalPolyline(root.m_subNotes, 1, 1, 0, 3),
          "erase fixture is canonical");
    root.m_annotation = "retained-history";
    ctx.noteRegistry.emplace<Logic::NoteComponent>(parent, root);
    // 三个投影子实体分别对应根数组，不复用无关已选物件作为假子项。
    // 显式只命中一个子实体，后续删除必须通过父级联覆盖其它两个。
    // 子投影只携带父句柄与各自序号，根的内嵌列表才是保存的几何来源。
    // 两种表示须同时保留到 before 快照，不能只检查其中一种恢复。
    // 首段不是普通 Note；用 Hold 类型确保与合法路径契约一致。
    const std::array children{ child,
                               ctx.noteRegistry.create(),
                               ctx.noteRegistry.create() };
    for ( std::size_t i = 0; i < children.size(); ++i ) {
        const auto& part = root.m_subNotes[i];
        ctx.noteRegistry.emplace<Logic::NoteComponent>(
            children[i],
            Logic::NoteComponent{ .m_type           = part.type,
                                  .m_timestamp      = part.timestamp,
                                  .m_duration       = part.duration,
                                  .m_trackIndex     = part.trackIndex,
                                  .m_dtrack         = part.dtrack,
                                  .m_isSubNote      = true,
                                  .m_parentPolyline = parent,
                                  .m_subIndex       = static_cast<int>(i) });
    }
    ctx.noteRegistry.emplace<Logic::NoteComponent>(
        other, Logic::NoteComponent{ .m_timestamp = 9 });
    ctx.noteRegistry.emplace<Logic::InteractionComponent>(other).isSelected =
        true;
    // 样本与根编号相同，足以发现只按 entity 而忽略 Registry 的去重错误。
    const auto sample = ctx.sampleRegistry.create();
    check(sample == parent, "mixed deletion fixture shares entity numbers");
    ctx.sampleRegistry.emplace<Logic::SampleComponent>(
        sample,
        Logic::SampleComponent{
            .m_timestamp = 3, .m_offsetMs = -24, .m_track = 1 });
    // 使用目标的指针宽度保存进程内实例，不能把设备地址截成固定 32 位编号。
    const auto instance =
        reinterpret_cast<std::uintptr_t>(ctx.currentBeatmap.get());
    Logic::ActionController        controller(ctx);
    const Logic::CmdDeleteSelected command{ Logic::ObjectDeletion{
        instance,
        { { child, Logic::ChartObjectKind::PlayerNote },
          { parent, Logic::ChartObjectKind::PlayerNote },
          { sample, Logic::ChartObjectKind::AudioSample },
          { sample, Logic::ChartObjectKind::AudioSample } } } };
    // 明确列表在入队之前即可精确分类，不能借用当前选区漏掉采样权限。
    // 默认桌面命令仍延后到会话细化，空显式列表不要求无关领域权限。
    check(Logic::requiredBeatmapMutationFlags(Logic::LogicCommand{ command }) ==
              (BeatmapMutationFlags::Objects |
               BeatmapMutationFlags::AudioSamples),
          "mixed erase is classified before queueing");
    check(Logic::requiredBeatmapMutationFlags(Logic::LogicCommand{
              Logic::CmdDeleteSelected{} }) == BeatmapMutationFlags::None,
          "default desktop deletion retains dynamic classification");
    // 释放之前没有调用删除命令，批次执行后才检查两个领域的最终状态。
    // 级联子项与显式重复根必须共用同一个 before 快照。
    controller.handleCommand(command);
    check(!ctx.noteRegistry.all_of<Logic::NoteComponent>(parent) &&
              !ctx.noteRegistry.all_of<Logic::NoteComponent>(child) &&
              !ctx.sampleRegistry.all_of<Logic::SampleComponent>(sample) &&
              ctx.noteRegistry.all_of<Logic::NoteComponent>(other) &&
              ctx.actionStack.getUndoStackSize() == 1,
          "stroke deletes only explicit roots and samples in one action");
    for ( const auto e : children )
        check(!ctx.noteRegistry.all_of<Logic::NoteComponent>(e),
              "all projections deleted with root");
    ctx.actionStack.undo(ctx);
    // 撤销不只是重新出现一个父实体，还要恢复合法子列表及每个投影关系。
    // 几何契约独立校验，不能以实体数量正确代替结构正确。
    const auto& restored = ctx.noteRegistry.get<Logic::NoteComponent>(parent);
    check(Common::isCanonicalPolyline(restored.m_subNotes,
                                      restored.m_timestamp,
                                      restored.m_trackIndex,
                                      0,
                                      3),
          "undo restores canonical polyline geometry");
    for ( const auto e : children )
        check(ctx.noteRegistry.get<Logic::NoteComponent>(e).m_parentPolyline ==
                  parent,
              "undo restores every child relationship");
    check(
        ctx.noteRegistry.get<Logic::NoteComponent>(parent).m_annotation ==
                root.m_annotation &&
            ctx.noteRegistry.get<Logic::NoteComponent>(child)
                    .m_parentPolyline == parent &&
            ctx.sampleRegistry.get<Logic::SampleComponent>(sample).m_offsetMs ==
                -24,
        "one undo restores root child and full signed sample offset");
    // 级联检查以三个子项为阳性对照，局部命中不能只删一个投影实体。
    // 保留下来的完整子列表继续满足原生折线契约，包含时间和轨道接缝。
    // 测试没有调用修复器把非法几何改成合法结构，before 即是有效谱面。
    // 删除的作用是移除指定根，不重写任何子段的类型或关联。
    // 撤销时逐个验证原子实体句柄，不能用同样几何的新实体冒充恢复。
    // 原根、投影子项和样本由同一个复合动作恢复，而非测试补建。
    // 撤销后真实注册表仍包含完整负偏移，证明恢复的是 before 快照。
    // 旧实体代数随 before 保留，过期目标不得通过创建新实体而重新变有效。
    // 混合域一笔只有一个历史槽，不允许分两次撤销才恢复完整谱面。
    // 同一批次的所有目标都在执行前准备，删除顺序不影响父子覆盖。
    // 样本与根恰好同号，历史仍必须分别恢复两个独立 Registry。
    // 后续无效请求都复用这一状态，不能各自创建空会话掩盖历史丢失。
    // 空目标不应把上方选中的无关音符带入删除范围。
    // 过期实例也不能通过相同实体编号获得当前谱面的删除权限。
    // 错误草稿领域句柄不能绕过玩家与草稿的分离。
    // 播放测试故意带有效目标，保证拒绝源于编辑状态而不是目标为空。
    // 重做深度检查必须在这些请求之后，不能只在各自调用之前验证。
    // 后续 Redo 应重放最初正确笔画，不能重放某个失败请求。
    // 不调用 clear 或 markSaved 来人为制造预期的历史深度。
    // 已有重做作为哨兵，所有无效批次不能新增空历史或丢失上一笔。
    controller.handleCommand(
        Logic::CmdDeleteSelected{ Logic::ObjectDeletion{ instance, {} } });
    controller.handleCommand(Logic::CmdDeleteSelected{
        Logic::ObjectDeletion{ instance + 1, command.m_objects->m_targets } });
    controller.handleCommand(Logic::CmdDeleteSelected{ Logic::ObjectDeletion{
        instance, { { other, Logic::ChartObjectKind::DraftNote } } } });
    ctx.isPlaying = true;
    controller.handleCommand(command);
    ctx.isPlaying = false;
    check(ctx.actionStack.getUndoStackSize() == 0 &&
              ctx.actionStack.getRedoStackSize() == 1,
          "empty stale wrong-domain and interrupted batches preserve redo");
    ctx.actionStack.redo(ctx);
    check(!ctx.noteRegistry.all_of<Logic::NoteComponent>(parent) &&
              !ctx.sampleRegistry.all_of<Logic::SampleComponent>(sample) &&
              ctx.noteRegistry.all_of<Logic::NoteComponent>(other),
          "one redo repeats exact deletion");
    ctx.actionStack.undo(ctx);
    ctx.lastConfig.settings.professionalMode = false;
    // 玩家折线仍按原能力开放，草稿与采样由专业模式各自限制。
    // 本轮只提交采样，不能因为已有 Note 选择而扩大删除领域。
    controller.handleCommand(Logic::CmdDeleteSelected{ Logic::ObjectDeletion{
        instance, { { sample, Logic::ChartObjectKind::AudioSample } } } });
    check(ctx.sampleRegistry.all_of<Logic::SampleComponent>(sample) &&
              ctx.actionStack.getRedoStackSize() == 1,
          "sample edit capability is enforced");
}
/// @brief 从真实会话发布空闲颜色笔快照，补上纯输入夹具与模型间的连接验证。
/// @details 不直接填写 RenderSnapshot
/// 颜色；必须经命令、会话更新与正式缓冲取得。
/// 活动几何仍为空，调色后的六槽来源不能依赖桌面 StartBrush 才存在。
/// Alpha 与 RGB 一起检查，避免只有取色 UI 变化而实际笔画仍是空载荷。
void testIdleColorSnapshot()
{
    Logic::BeatmapSession session;
    auto&                 ctx = session.getContextMutable();
    // 使用内存谱面和本地快照池，不读取设备文件、音频或其它会话的缓冲。
    // 相机固定为主视口，避免辅助相机的播放限频影响一次更新的发布断言。
    // 缓冲属于本测试会话，测试结束后随会话一起销毁，无跨用例悬空引用。
    Logic::SessionUtils::loadBeatmap(ctx, std::make_shared<BeatMap>());
    ctx.cameras.emplace(
        "Basic2DCanvas",
        Logic::CameraInfo{ "Basic2DCanvas", 1000.0F, 600.0F, 0.0F });
    ctx.syncBuffers.emplace(
        "Basic2DCanvas",
        std::make_shared<Common::Render::RenderSnapshotBuffer>());
    constexpr glm::vec4 color{ .91F, .22F, .13F, .47F };
    // 正式命令先更新画笔再切换工具，始终不发送 StartBrush。
    session.pushCommand(
        Logic::CmdSetBrushNoteColor{ Logic::NoteColorSlot::Head, color });
    session.pushCommand(Logic::CmdChangeTool{ Logic::EditTool::ColorBrush });
    // 配色工具沿用原专业模式权限，夹具须显式开启而非依赖宿主用户配置。
    auto config                      = ctx.lastConfig;
    config.settings.professionalMode = true;
    // 零步长只消费离散命令，不用固定时长等待后台线程或等待预览变为活动。
    // 发布数据从只读端拉取，不能检查写入端暂存对象来掩盖遗漏的提交。
    session.update(0.0, config, true);
    const auto* snapshot =
        ctx.syncBuffers.at("Basic2DCanvas")->pullLatestSnapshot();
    // 确认测到实际发布的颜色工具，而不是缓冲中残留的默认工作对象。
    check(snapshot && snapshot->currentTool == Logic::EditTool::ColorBrush,
          "color tool publishes a real session snapshot");
    if ( !snapshot ) return;
    // 不能为了提供颜色把几何笔画标为活动，否则会与触控本地事务冲突。
    // 检查完整 optional，未发布不能被默认黑色或皮肤继承误判为有效 RGBA。
    // 与独立手势用例组合，约束实际生产源和输入消费端使用同一颜色契约。
    check(!ctx.brushState.isActive && !snapshot->brush.isActive,
          "idle color snapshot never starts a geometry brush");
    check(snapshot->brush.customColors.head &&
              *snapshot->brush.customColors.head == color,
          "idle color snapshot carries exact RGBA from the palette command");
}
}  // namespace

/// @brief 组合覆盖轻点、静止按住、快速终点、双向长条和第二指接管。
/// @details 最终形状由真实 DrawTool 决定，测试不根据手势自己制造音符。
/// @note 轻点含容差内抖动，起点坐标不能被最后一次移动覆盖。
/// @note 静止按住刻意超过菜单阈值，区别于仅测试短按释放。
/// @note 快速序列没有持续移动帧，终点补偿必须在释放路径生效。
/// @note 连续序列跨过其他轨道，检验横向漂移不会生成滑键或折线。
/// @note 正反向对照交换落点时间，不能用非负时长检查代替范围检查。
/// @note 桌面场景最后执行，触控状态必须已经完全清理。
/// @return
/// 任一输入、几何或历史断言失败均返回非零，不以某个场景成功代替整组通过。
int main()
{
    const auto tap =
        replay(capture({ { { 90, 350 }, true }, { { 96, 354 }, false } }));
    check(tap.m_type == NoteType::NOTE && tap.m_duration == 0 &&
              tap.m_trackIndex == 0,
          "tap jitter creates one note at original lane");
    // 静止超过菜单阈值仍是候选，抬手只创建一个单键。
    std::vector<Sample> hold(14, { { 90, 350 }, true });
    hold.push_back({ { 90, 350 }, false });
    check(replay(capture(hold)).m_type == NoteType::NOTE,
          "stationary hold stays note");
    // 位移与释放在同帧到达，仍必须提交最终时长，不能只生成起点单键。
    const auto forward =
        replay(capture({ { { 90, 350 }, true }, { { 220, 200 }, false } }));
    check(forward.m_type == NoteType::HOLD && forward.m_duration > 0 &&
              forward.m_trackIndex == 0 && forward.m_dtrack == 0 &&
              forward.m_subNotes.empty(),
          "fast diagonal drawing is fixed-lane hold");
    // 快速抬手与连续拖动有同一有效起点和终点，应该产生一致几何。
    // 反向只交换时间端点，归一化后仍固定原轨道并拥有正时长。
    // 这里只比较生产提交结果，不用测试自己的屏幕到时间公式。
    // 终点丢失会退回单键，因此同时检查类型、时间和持续长度。
    const auto backward =
        replay(capture({ { { 90, 200 }, true }, { { 220, 350 }, false } }));
    check(backward.m_type == NoteType::HOLD &&
              near(backward.m_timestamp, forward.m_timestamp) &&
              near(backward.m_duration, forward.m_duration),
          "reverse drawing normalizes same time range");
    // 连续更新与快速释放使用同一端点规则，横向漂移不能变成折线。
    const auto slow = replay(capture({ { { 90, 350 }, true },
                                       { { 140, 310 }, true },
                                       { { 180, 250 }, true },
                                       { { 220, 200 }, false } }));
    check(slow.m_type == NoteType::HOLD &&
              near(slow.m_timestamp, forward.m_timestamp) &&
              near(slow.m_duration, forward.m_duration),
          "continuous and fast endpoints agree");
    replay(capture({ { { 90, 350 }, true },
                     { { 90, 250 }, true },
                     { { 90, 250 }, true, true },
                     { { 90, 250 }, false, true },
                     { { 90, 250 }, false, false } }),
           true);
    // 桌面对照经过同一移动工厂的回退路径，不能带入新绘制策略。
    // 第二指接管后必须全部抬起，残留触点不能自动提交取消的旧笔画。
    // 最后使用经典桌面和纯鼠标作为对照，检查触控策略没有泄漏。
    // 对照仍经同一移动工厂的桌面回退，不能只测方案能力标志。
    // 桌面键盘状态没有被测试改写，经典 Shift 绘制由既有套件继续覆盖。
    for ( const auto profile : { "classic-desktop", "mouse-only" } ) {
        const auto commands = capture({ { { 90, 350 }, true },
                                        { { 220, 200 }, true },
                                        { { 220, 200 }, false } },
                                      profile);
        int        starts{};
        for ( const auto& command : commands )
            if ( const auto* c = std::get_if<Logic::CmdStartBrush>(&command) ) {
                ++starts;
                check(!c->m_noteHoldOnly,
                      "desktop brush policy remains unchanged");
            }
        check(starts == 1, "desktop draw still starts normally");
    }
    // 先从空白起笔，确认新增模式不是只会替换已有对象。
    // 四种来源分别做成功提交与第二指取消，保证两个分支不共享假设。
    // 三段来源额外插入修订变化，验证过期快照不会覆盖期间的新编辑。
    // 最后的轻点只检查选择命令，任何 Start 都意味着误进入续写事务。
    // 简单模式恢复在测试结束执行，不能污染同套件其它画布用例。
    // 桌面对照在模式切换之前已运行，原物理修饰键始终未被模拟写入。
    // 实际设备另行验证中文和英文布局，CPU 测试只约束操作语义。
    // 本组不保存资源，所有测试生成内容只存在于本进程内存。
    // 源物件前缀由生产状态机继承，测试只选择命中末端而不拼接新段。
    // 快速抬手的最终位置仍由相同输入桥补交，后续设备验收覆盖真实事件。
    verifyPolyline(std::nullopt);
    for ( const auto type : { NoteType::NOTE,
                              NoteType::HOLD,
                              NoteType::FLICK,
                              NoteType::POLYLINE } ) {
        verifyPolyline(type);
        verifyPolyline(type, true);
    }
    verifyPolyline(NoteType::POLYLINE, false, true);
    // 原末端轻点只发选择；没有 Start/End 就没有误替换或清除重做的中间事务。
    const auto tapped =
        capture({ { { 90, 350 }, true }, { { 92, 352 }, false } },
                "touch-gestures",
                false,
                Common::Render::Hitbox{ entt::entity{ 0 },
                                        Common::Render::HoverPart::Head,
                                        -1,
                                        70,
                                        338,
                                        40,
                                        24 });
    bool selected{};
    for ( const auto& command : tapped ) {
        selected |= std::holds_alternative<Logic::CmdSelectEntity>(command);
        check(!std::holds_alternative<Logic::CmdStartBrush>(command),
              "endpoint tap never begins resume");
    }
    check(selected, "endpoint tap selects original object");
    testIdleColorSnapshot();
    testTouchColors();
    testColorActions();
    testTouchEraser();
    testEraseDeletion();
    UI::Interaction::activeProfile().setNoteHoldBrush(true);
    return failures == 0 ? 0 : 1;
}
