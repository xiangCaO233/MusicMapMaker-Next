#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/PlanarObservationInput.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/ClipboardTypes.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/DrawTool.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/interaction/EditorInteraction.h"
#include <cmath>
#include <imgui.h>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>

namespace
{
namespace Interaction = MMM::UI::Interaction;
/// @brief 菜单锁存的吸附时间优先于播放头，整批粘贴保留相对时间。
/// @return 显式定位、默认定位及非法输入拒绝全部符合契约时返回真。
/// @details 只驱动正式粘贴控制器，不通过更改播放时间伪造粘贴位置。
/// 目标落点刻意不等于播放头或原始复制时间，以识别未传递锚点的回归。
/// 两个不同轨道的物件验证相对间隔，避免整批被压到同一拍。
bool testExplicitPasteAnchor()
{
    using namespace MMM::Logic;
    SessionContext context;
    // 播放头故意远离目标拍线，防止仍走默认定位却偶然通过。
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.trackCount     = 4;
    context.animateTime    = 10.0;
    context.lastConfig.settings.pastePosition =
        MMM::Config::PastePosition::JudgmentLine;
    // 两个相隔半秒的单键模拟同批复制，不改变其轨道排列。
    // 剪贴板保存来源绝对时间，粘贴控制器负责计算统一偏移。
    // 无拍位缓存时沿用秒间隔，不让节奏换算掩盖位置断言。
    // 本测试未执行全局 Copy，生产入口使用会话内剪贴板回退。
    context.clipboard = {
        ClipboardItem{
            .note = NoteComponent{ .m_timestamp = 2.0, .m_trackIndex = 0 } },
        ClipboardItem{
            .note = NoteComponent{ .m_timestamp = 2.5, .m_trackIndex = 1 } }
    };
    ActionController controller(context);
    // 显式时间来自菜单打开时的可见吸附线；逻辑层不再二次吸附。
    controller.handleCommand(CmdPaste{ .m_targetTime = 3.25 });
    const auto notes = context.noteRegistry.view<NoteComponent>();
    // 只接受完整两项结果，少建一个或重复创建均属于失败。
    if ( notes.size() != 2 ) return false;
    for ( auto entity : notes ) {
        const auto&  note     = notes.get<NoteComponent>(entity);
        const double expected = note.m_trackIndex == 0 ? 3.25 : 3.75;
        if ( std::abs(note.m_timestamp - expected) > 1e-9 ) return false;
    }
    // 显式位置只作用于本次命令，不能偷偷改用户的默认粘贴偏好。
    // 撤销清除上一批结果，再用无锚点命令验证兼容入口。
    // 第二次应对齐十秒播放头，不能复用上一次的菜单时间。
    context.actionStack.undo(context);
    controller.handleCommand(CmdPaste{});
    for ( auto entity : context.noteRegistry.view<NoteComponent>() ) {
        const auto&  note     = context.noteRegistry.get<NoteComponent>(entity);
        const double expected = note.m_trackIndex == 0 ? 10.0 : 10.5;
        if ( std::abs(note.m_timestamp - expected) > 1e-9 ) return false;
    }
    // 无效显式锚点必须拒绝，而非回退播放头产生难以发现的误放。
    // 同时检查动作栈和实体数，拒绝不能留下可撤销副作用。
    // 负值与 NaN 分别覆盖时间范围和有限性，两者都不允许替代落点。
    const auto history = context.actionStack.getUndoStackSize();
    controller.handleCommand(CmdPaste{ .m_targetTime = -1.0 });
    controller.handleCommand(
        CmdPaste{ .m_targetTime = std::numeric_limits<double>::quiet_NaN() });
    return context.noteRegistry.view<NoteComponent>().size() == 2 &&
           context.actionStack.getUndoStackSize() == history;
}
/// @brief 用纯鼠标方案的真实延伸策略验证单键、新建折线及末段续写。
/// @return 提交后只有完整根对象且撤销恢复原折线时返回真。
/// @details 不伪造键盘，复用 DrawTool 与动作栈；固定投影只用于稳定输入坐标。
/// 调用者已经激活纯鼠标方案，并准备可读取输入的 ImGui 上下文。
/// 第一次点按验证基本类型，第二次手势生成待续写的真实折线。
/// 续写从最后一段进入，沿用现有拾取许可，不扩展内部节点的可编辑范围。
/// 撤销沿用旧工具的删除与创建两步历史，不在输入方案中改写动作栈。
bool testMouseBrushContinuation()
{
    using namespace MMM::Logic;
    SessionContext context;
    // 最小谱面与四轨视口足以驱动正式画笔，不启动音频或渲染线程。
    // 元数据轨道数与运行态一致，排除布局查询使用不同来源造成的干扰。
    // 玩家区宽四百像素，使横移一百像素对应跨一轨。
    // 判定线位于三百像素处，向上移动对应向未来时间延伸。
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count = 4;
    context.trackCount                                    = 4;
    context.currentTime = context.animateTime         = 1.0;
    context.lastConfig.visual.trackLayout.left        = 0.1f;
    context.lastConfig.visual.trackLayout.right       = 0.5f;
    context.lastConfig.visual.judgeline_pos           = 0.5f;
    context.lastConfig.settings.enablePolylineEditing = true;
    context.cameras.emplace("Basic2DCanvas",
                            CameraInfo{ "Basic2DCanvas", 1000, 600, 0 });
    auto* cache = context.timelineRegistry.ctx().find<System::ScrollCache>();
    // 缓存由会话拥有，测试只按需创建，不保留跨会话引用。
    // 空时间点集合建立线性投影，不混入变速导致的形状差异。
    if ( !cache )
        cache = &context.timelineRegistry.ctx().emplace<System::ScrollCache>();
    cache->rebuild(context.timelineRegistry,
                   context.lastConfig,
                   context.currentBeatmap.get());
    const Interaction::SurfaceScope scope(Interaction::Surface::MainCanvas);
    const bool                      extend = Interaction::extendBrush();
    DrawTool                        tool;
    // 延伸语义必须从生产方案获取，不能写死 true 绕过输入策略。
    // Start 和 Update 使用相同策略，模拟无键盘参与的持续手势。
    // 单击立即得到 NOTE 草稿，释放后只有一个单键；不等待长按判定。
    tool.handleStartBrush(context,
                          { "Basic2DCanvas", 150, 300, extend, false });
    if ( !context.brushState.isActive ||
         context.brushState.type != MMM::NoteType::NOTE )
        return false;
    tool.handleEndBrush(context, { .cameraId = "Basic2DCanvas" });
    if ( context.noteRegistry.view<NoteComponent>().size() != 1 ) return false;
    // 清除该单键再绘制 L 形轨迹，避免尾部合并干扰续写基线。
    // 原会话和动作栈继续使用，后续新建会正常覆盖已撤销的重做分支。
    // 横向位移先生成 Flick，随后纵向位移再升级为 Polyline。
    context.actionStack.undo(context);
    tool.handleStartBrush(context,
                          { "Basic2DCanvas", 150, 300, extend, false });
    tool.handleUpdateBrush(context,
                           { "Basic2DCanvas", 250, 300, extend, false });
    tool.handleUpdateBrush(context,
                           { "Basic2DCanvas", 250, 200, extend, false });
    tool.handleEndBrush(context, { .cameraId = "Basic2DCanvas" });
    // 只查找根，子投影实体不能误当作另一条独立折线。
    entt::entity original = entt::null;
    for ( auto entity : context.noteRegistry.view<NoteComponent>() ) {
        const auto& note = context.noteRegistry.get<NoteComponent>(entity);
        if ( !note.m_isSubNote && note.m_type == MMM::NoteType::POLYLINE )
            original = entity;
    }
    if ( original == entt::null ) return false;
    const auto before = context.noteRegistry.get<NoteComponent>(original);
    // 起笔会删除旧组件，必须按值保存基线，不能保留该组件的借用引用。
    // 历史深度记录在续写之前，不假定前面的撤销操作清空了整个栈。
    const auto history = context.actionStack.getUndoStackSize();
    // 模拟正式拾取给出的末段 HoldEnd；内部节点不应被当作可续写尾部。
    context.hoveredEntity   = original;
    context.hoveredSubIndex = static_cast<int>(before.m_subNotes.size()) - 1;
    context.hoveredPart =
        static_cast<uint8_t>(MMM::Common::Render::HoverPart::HoldEnd);
    context.hoveredObjectKind = ChartObjectKind::PlayerNote;
    // 同值实体 ID 也可能存在于 Sample Registry，明确对象域避免串用。
    // Start 已经携带延伸意图，才能进入旧根恢复路径，而非另起新笔。
    tool.handleStartBrush(context,
                          { "Basic2DCanvas", 250, 200, extend, false });
    // 起笔必须恢复旧段和替换身份，不能在尾部另起一条不相干的折线。
    // 新段尚未追加，后续横向移动才应延伸原折线的活动末端。
    if ( !context.brushState.replacesExistingObject ||
         context.brushState.polylineSegments.size() !=
             before.m_subNotes.size() )
        return false;
    tool.handleUpdateBrush(context,
                           { "Basic2DCanvas", 350, 200, extend, false });
    tool.handleEndBrush(context, { .cameraId = "Basic2DCanvas" });
    // 保留原 Shift 续写的历史约定：删除旧根与创建新根各占一个动作。
    // 输入方案只改变手势解释，不能悄悄合并或重排现有撤销历史。
    if ( context.actionStack.getUndoStackSize() != history + 2 ) return false;
    context.actionStack.undo(context);
    context.actionStack.undo(context);
    if ( !context.noteRegistry.valid(original) ) return false;
    const auto& restored = context.noteRegistry.get<NoteComponent>(original);
    // 撤销恢复旧根身份和完整原始段数，不能留下续写后的替代根。
    // 时间和起始轨道同样回到基线，防止末端落点覆盖折线根锚点。
    return restored.m_type == before.m_type &&
           restored.m_timestamp == before.m_timestamp &&
           restored.m_trackIndex == before.m_trackIndex &&
           restored.m_subNotes.size() == before.m_subNotes.size();
}
/// @brief 在真实主画布控制器中替换整段操作，而非仅调用接口替身。
class SurfaceProfile final : public Interaction::IEditorInteractionProfile
{
public:
    /// @brief 测试方案身份只存在于独立测试进程。
    /// @warning 测试身份是静态字面量，注册期间不会失效或随显示文本改变。
    std::string_view id() const override { return "surface-test"; }
    /// @brief 展示名称与注册身份无关。
    /// @warning 只返回已有字符串，不在画布帧中构造描述对象。
    std::string_view name() const override { return "Surface test"; }
    /// @brief 指针手势在释放处理完毕之前阻止全局切换。
    /// @return 新控制器是否还拥有未处理完释放事件的手势。
    /// @warning 只读 UI 本地状态，不能等待旧逻辑工具完成一轮更新。
    bool hasActiveGesture() const override { return m_dragging; }
    /// @brief 当前主按钮开始选中并保持所有权，释放后才结束事务。
    /// @param frame 由实际 Basic2DCanvasInteraction 更新提供的视图上下文。
    /// @return 主画布独占时返回 true，其他表面交还原控制器。
    /// @warning 每帧只处理边沿和常量命令，不重新加载工程或创建新方案。
    bool handleSurface(const Interaction::SurfaceFrame& frame) override
    {
        if ( frame.m_surface != Interaction::Surface::MainCanvas ) return false;
        // 生产入口必须提供真实快照和投影，而不是测试自行拼装的替代坐标。
        m_receivedProjection =
            frame.m_projection != nullptr && frame.m_snapshot != nullptr;
        if ( Interaction::mouseClicked(0) && frame.m_hovered ) {
            m_dragging = true;
            Interaction::dispatchCommand(
                MMM::Event::LogicCommandEvent(MMM::Logic::CmdSelectAll{}));
        }
        // 返回 true 后不能再执行经典状态机，否则会重复发布 hover/brush 命令。
        if ( Interaction::mouseReleased(0) ) m_dragging = false;
        return true;
    }
    /// @brief 用例检查新控制器确实获得了坐标上下文。
    bool m_receivedProjection{};
    /// @brief 仅由测试 UI 线程维护，不访问会话锁。
    bool m_dragging{};
};
/// @brief 建立实际窗口并调用生产 update；窗口命中由 ImGui 计算。
/// @param interaction 绑定唯一相机身份的生产控制器，不是接口替身。
/// @param snapshot 用例拥有的不可变快照，生命周期覆盖整个输入帧。
/// @param down 是否保持物理主按钮按住，不直接修改方案自己的手势标志。
/// @details 窗口位置与尺寸固定，避免桌面窗口管理器或 DPI 改变输入命中。
/// 输入先排队再 NewFrame，保证方案与默认控制器看到同一份平台状态。
/// 调用后结束整个 ImGui 帧，不用直接调用私有 handleInteractions 模拟结果。
bool frame(MMM::Canvas::Basic2DCanvasInteraction&     interaction,
           const MMM::Common::Render::RenderSnapshot& snapshot, bool down,
           ImVec2 pointer = { 150, 150 })
{
    auto& io = ImGui::GetIO();
    // pointer 使用屏幕坐标，默认点按与后续拖动都经生产窗口转换到局部坐标。
    // 手势推进不自行派发 Brush 命令，避免跳过控制器的起笔和释放门禁。
    // 按下原点位于内容区，排除从标题栏或其他窗口拖入的情况。
    io.AddMousePosEvent(pointer.x, pointer.y);
    io.AddMouseButtonEvent(0, down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ 400, 400 });
    ImGui::Begin("SurfaceTest", nullptr, ImGuiWindowFlags_NoSavedSettings);
    // 焦点来自正式窗口栈，不能把测试硬编码的 focused 标志交给方案。
    // 已打开菜单时保留其焦点，强制聚焦父窗口会人为关闭标准弹出菜单。
    if ( !ImGui::IsPopupOpen(
             nullptr,
             ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) )
        ImGui::SetWindowFocus();
    interaction.update(nullptr, &snapshot, 300, 300);
    // 使用和生产画布相同的身份查询菜单，不通过私有成员强行开启。
    ImGui::PushID("Canvas_Probe");
    const bool menuOpen = ImGui::IsPopupOpen("SelectionEditMenu");
    ImGui::PopID();
    ImGui::End();
    ImGui::EndFrame();
    return menuOpen;
}
}  // namespace

/// @brief 确认新方案独占生产画布、释放收尾和经典操作恢复的完整链路。
/// @details 用例不创建 Vulkan 设备，字体图集只保留 CPU 数据。
/// 默认配置目录由 CTest 隔离，测试不会改变用户的编辑方案。
/// 第一次窗口帧只建立真实悬浮归属，第二帧才执行按下边沿。
/// 只有正式事件总线收到 CmdSelectAll 才算新方案的选择操作已经发布。
/// 同一帧不得再收到默认 CmdSetMousePosition，证明接管范围覆盖整段操作。
/// 新手势仍活跃时，即使调用者报告安全帧也不能立即切换方案。
/// 释放由生产更新送入当前方案，测试不直接清空其内部标志。
/// 连续安全帧后再切回默认方案，避免跳过手势交接协议。
/// 切回后的真实鼠标命令证明经典控制器恢复，而不是永久关闭编辑。
/// 画布在 ImGui 上下文之前析构，确保观察登记和订阅安全释放。
/// 所有测试输出限于构建树，不写入源码资源或实际谱面。
int main()
{
    ImGui::CreateContext();
    auto& io                        = ImGui::GetIO();
    io.IniFilename                  = nullptr;
    io.DisplaySize                  = { 800, 600 };
    io.DeltaTime                    = 1.0f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels{};
    int            width{}, height{};
    // 图集由 ImGui 拥有，不上传设备，也不由用例释放内部像素指针。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto  profile  = std::make_unique<SurfaceProfile>();
    auto* observer = profile.get();
    Interaction::registerProfile(std::move(profile));
    // 激活也走正式安全帧协议，不直接改注册表的活动指针。
    Interaction::synchronizeProfile("surface-test", true);
    Interaction::synchronizeProfile("surface-test", true);
    bool ok = true;
    {
        MMM::Canvas::Basic2DCanvasInteraction interaction("SurfaceTest",
                                                          "Canvas_Probe");
        MMM::Canvas::PlanarObservationInputProjection projection;
        interaction.setInputProjection(projection);
        MMM::Common::Render::RenderSnapshot snapshot;
        // 空工程会被生产入口拒绝，测试必须经过实际有谱面分支。
        snapshot.hasBeatmap = true;
        // 四键布局只提供足够的经典拾取上下文，不添加真实谱面实体。
        snapshot.trackCount       = 4;
        snapshot.isHoveringCanvas = true;
        // 观察正式事件出口，不能把私有状态变化当作命令已执行。
        int        selected = 0, hovered = 0;
        auto&      bus          = MMM::Event::EventBus::instance();
        const auto subscription = bus.subscribe<MMM::Event::LogicCommandEvent>(
            [&](const auto& event) {
                if ( std::holds_alternative<MMM::Logic::CmdSelectAll>(
                         event.command) )
                    ++selected;
                if ( std::holds_alternative<MMM::Logic::CmdSetMousePosition>(
                         event.command) )
                    ++hovered;
            });
        // 第一帧建立真实窗口命中，第二帧才开始方案自己的选择手势。
        frame(interaction, snapshot, false);
        // 按下只允许方案自己发布一次业务命令，经典鼠标跟踪应被抑制。
        frame(interaction, snapshot, true);
        ok &= observer->m_receivedProjection && observer->m_dragging &&
              selected == 1 && hovered == 0;
        // 即使调用者误报物理输入空闲，方案自己的事务 guard 也必须阻止切出。
        ok &= !Interaction::synchronizeProfile("classic-desktop", true);
        // 仍由当前方案接收释放，不能直接修改其私有拖拽状态促成切换。
        frame(interaction, snapshot, false);
        ok &= !observer->m_dragging;
        Interaction::synchronizeProfile("classic-desktop", true);
        ok &= Interaction::synchronizeProfile("classic-desktop", true);
        // 切回经典方案后原控制器重新发送鼠标状态，不能永久停留在被接管状态。
        frame(interaction, snapshot, false);
        ok &= hovered > 0 && selected == 1;
        // 在同一生产控制器中切入纯鼠标方案，不直接调用方案方法伪造结果。
        // Draw 工具不需要按键修饰；事件出口仍沿用既有画笔事务。
        Interaction::synchronizeProfile("mouse-only", true);
        Interaction::synchronizeProfile("mouse-only", true);
        snapshot.currentTool = MMM::Logic::EditTool::Draw;
        // 门禁仍要求有效谱面和停止播放，测试不能依靠绕过编辑许可起笔。
        snapshot.hasBeatmap = true;
        int  starts = 0, updates = 0, ends = 0;
        bool extended = true;
        // 记录所有画笔事件而非最后一次事件，起笔错误不能被后续更新掩盖。
        // 计数限定本次订阅生命周期，前面的方案接管测试不计入。
        const auto brushSubscription =
            bus.subscribe<MMM::Event::LogicCommandEvent>(
                [&](const auto& event) {
                    // 起笔与更新都必须携带相同延伸语义，释放只提交一次。
                    if ( const auto* start =
                             std::get_if<MMM::Logic::CmdStartBrush>(
                                 &event.command) ) {
                        ++starts;
                        extended &= start->isShiftDown;
                    }
                    if ( const auto* update =
                             std::get_if<MMM::Logic::CmdUpdateBrush>(
                                 &event.command) ) {
                        ++updates;
                        extended &= update->isShiftDown;
                    }
                    if ( std::holds_alternative<MMM::Logic::CmdEndBrush>(
                             event.command) )
                        ++ends;
                });
        // 无移动点按只有起笔与结束，不能为了纯鼠标绘制凭空生成持续段。
        // 原工具从 NOTE 类型开始，形状变化必须由后续移动命令驱动。
        // 释放不附带额外更新时，单击自然提交单键而不是零长持续段。
        frame(interaction, snapshot, false);
        frame(interaction, snapshot, true);
        frame(interaction, snapshot, false);
        ok &= starts == 1 && updates == 0 && ends == 1 && extended;
        // 持续拖动必须逐帧发布更新，不等待长按计时器或松开才反馈。
        // 两个不同落点覆盖超出拖动阈值后的连续采样，不只检查首次位移。
        // 释放位于最后落点，仍须走既有 EndBrush 事务收尾。
        // 物理 Shift 始终未注入，断言可以发现误改 ImGuiIO 的实现。
        // 快照中的活动画笔模拟逻辑线程已处理 Start，不改变物理 Shift。
        frame(interaction, snapshot, true);
        snapshot.brush.isActive = true;
        frame(interaction, snapshot, true, { 180, 200 });
        frame(interaction, snapshot, true, { 220, 250 });
        frame(interaction, snapshot, false, { 220, 250 });
        ok &= starts == 2 && updates >= 2 && ends == 2 && extended &&
              !io.KeyShift;
        // 仅移除本次观察者，后续经典模式检查继续使用相同主窗口。
        bus.unsubscribe<MMM::Event::LogicCommandEvent>(brushSubscription);
        // 画布命令正确后继续验证领域结果，避免只证明 Shift 字段被改写。
        ok &= testMouseBrushContinuation();
        ok &= testExplicitPasteAnchor();
        // 选中工具右键必须打开菜单，Draw 工具的擦除逻辑不能被同时触发。
        // 前面的绘制事务已释放，清理模拟快照后再切换工具。
        // 使用真实按钮事件，不调用 OpenPopup 跳过方案和工具门禁。
        snapshot.brush.isActive = false;
        snapshot.currentTool    = MMM::Logic::EditTool::Marquee;
        frame(interaction, snapshot, false);
        int        edits = 0;
        const auto menuSubscription =
            bus.subscribe<MMM::Event::LogicCommandEvent>(
                [&](const auto& event) {
                    // 菜单拥有输入期间，按下不应建立框选或擦除事务。
                    if ( std::holds_alternative<MMM::Logic::CmdStartErase>(
                             event.command) ||
                         std::holds_alternative<MMM::Logic::CmdStartMarquee>(
                             event.command) )
                        ++edits;
                });
        io.AddMouseButtonEvent(1, true);
        const bool opened = frame(interaction, snapshot, false);
        // 分开报告开启失败与输入泄漏，区分身份查询和焦点生命周期回归。
        if ( !opened ) XERROR("Selection menu did not open");
        ok &= opened;
        io.AddMouseButtonEvent(1, false);
        frame(interaction, snapshot, false);
        const int hoverBeforeMenu = hovered;
        // 未产生编辑也可能改变粘贴目标，所以悬浮更新同样视为失败。
        // 开菜单首帧允许发布落点，后续才要求悬浮命令停止。
        // 粘贴应使用右键位置，而不是上一帧残留的落点。
        // 指针在菜单内移动不能改变粘贴落点或当前选择。
        frame(interaction, snapshot, false, { 180, 170 });
        if ( hovered != hoverBeforeMenu || edits != 0 )
            XERROR("Selection menu leaked hover/edits: {} -> {}, edits {}",
                   hoverBeforeMenu,
                   hovered,
                   edits);
        ok &= hovered == hoverBeforeMenu && edits == 0;
        // 点击外部只关闭菜单，本次按下不得穿透到选中工具。
        // 外部位置仍在窗口内，可以发现仅依赖窗口悬浮造成的穿透。
        // 保持完整按下和释放，不通过销毁上下文掩盖悬挂手势。
        // 本场景不创建管理器，系统剪贴板访问不属于窗口命中测试。
        frame(interaction, snapshot, true, { 50, 250 });
        frame(interaction, snapshot, false, { 50, 250 });
        if ( edits != 0 ) XERROR("Selection menu close leaked {} edits", edits);
        ok &= edits == 0;
        bus.unsubscribe<MMM::Event::LogicCommandEvent>(menuSubscription);
        // 弹窗已完成一次外部点击关闭，后续画布帧应重新获得正常输入权。
        // 只撤销本用例的监听，不影响注册表或其他订阅者。
        bus.unsubscribe<MMM::Event::LogicCommandEvent>(subscription);
    }
    // 控制器已先析构并解除 guard，此时才释放底层 ImGui 输入快照。
    ImGui::DestroyContext();
    if ( !ok )
        XERROR(
            "EditorInteractionSurfaceTest: surface ownership or release "
            "handoff failed");
    return ok ? 0 : 1;
}
