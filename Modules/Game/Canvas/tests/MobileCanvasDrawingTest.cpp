#include "canvas/Basic2DCanvasInteraction.h"
#include "common/EditTool.h"
#include "common/LogicCommands.h"
#include "common/render/HoverRenderData.h"
#include "common/render/RenderSnapshot.h"
#include "config/EditorConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/EditorAction.h"
#include "logic/session/InteractionController.h"
#include "logic/session/NoteAction.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/NoteType.h"
#include "presentation/CanvasInteractionPresentation.h"
#include "presentation/mobile/MobileCanvasInteractionPresentation.h"
#include "ui/interaction/EditorInteraction.h"

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include <cmath>
#include <cstddef>
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
    ImVec2 m_point;     ///< 当前主触点的画布局部坐标。
    bool   m_down{};    ///< 主触点按住状态。
    bool   m_second{};  ///< 第二指接管，用于取消已经开始的画笔。
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
std::vector<Logic::LogicCommand> capture(
    const std::vector<Sample>& samples,
    std::string_view profile = "touch-gestures", bool simple = true,
    std::optional<Common::Render::Hitbox> hit = std::nullopt)
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
        snapshot.currentTool                            = Logic::EditTool::Draw;
        snapshot.trackCount                             = 4;
        // 末端拾取由正式控制器完成，夹具只提供渲染层本来会发布的命中盒。
        if ( hit ) snapshot.hitboxes.push_back(*hit);
        // 路径与实例同时固定，排除控制器以为谱面切换后提前取消笔画。
        // 有谱面且未播放，权限判断必须允许起笔；完全不派发输入不能假通过。
        // 不预置 brush.isActive，所有画笔状态必须由正式命令建立。
        // 命中身份不由测试替生产解释器伪造，绘制落点仍使用正常投影。
        snapshot.beatmapPathKey    = "drawing-fixture.mmm";
        snapshot.beatmapInstanceId = 1;
        auto& bus                  = Event::EventBus::instance();
        // 订阅只捕获正式命令；菜单、悬浮和测试自己的采样不创建编辑记录。
        const auto token =
            bus.subscribe<Event::LogicCommandEvent>([&](const auto& event) {
                const auto& command = event.command;
                if ( std::holds_alternative<Logic::CmdStartBrush>(command) ||
                     std::holds_alternative<Logic::CmdUpdateBrush>(command) ||
                     std::holds_alternative<Logic::CmdEndBrush>(command) ||
                     std::holds_alternative<Logic::CmdSetHoveredEntity>(
                         command) ||
                     std::holds_alternative<Logic::CmdSelectEntity>(command) )
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
    UI::Interaction::activeProfile().setNoteHoldBrush(true);
    return failures == 0 ? 0 : 1;
}
