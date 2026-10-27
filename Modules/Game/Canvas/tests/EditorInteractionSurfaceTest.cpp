#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/PlanarObservationInput.h"
#include "common/LogicCommands.h"
#include "common/SelectionOperation.h"
#include "common/render/HoverRenderData.h"
#include "common/render/RenderSnapshot.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/ClipboardTypes.h"
#include "logic/session/InteractionController.h"
#include "logic/session/SelectionState.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/DrawTool.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/interaction/EditorInteraction.h"
#include <array>
#include <cmath>
#include <imgui.h>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>

namespace MMM::Canvas
{
/// @brief 从真实右键采集的候选触发生产选择入口，不构造虚假的悬浮优先级。
struct EditorInteractionSurfaceTestAccess {
    /// @brief 菜单点击与测试共用身份验证和命令发布，不绕过新快照校验。
    /// @param controller 通过真实鼠标输入打开过右键菜单的控制器。
    /// @param index 对应右键落点冻结的候选位置。
    /// @param snapshot 激活时的快照，可能已删除节点或切换谱面实例。
    /// @return 命令是否真实发布，不能仅用菜单存在与否判断成功。
    /// @pre 菜单目标已由真实右键冻结，访问器不得自行填充候选。
    /// @note 激活不执行拖动，拖动身份必须由下一次真实左键另行验证。
    /// @note 测试独占事件总线观察窗口，不跨异步线程读取控制器私有状态。
    static bool choose(Basic2DCanvasInteraction& controller, std::size_t index,
                       const Common::Render::RenderSnapshot& snapshot)
    {
        return controller.selectHoverLayer(index, snapshot);
    }
};
}  // namespace MMM::Canvas

namespace
{
namespace Interaction = MMM::UI::Interaction;
/// @brief 验证显式选择集合操作、重复框选、缩框及移框后的有序重算。
/// @return 同号跨领域对象互不干扰且经典 Ctrl 语义仍成立时返回真。
/// @details 通过正式命令入口和选择索引检查结果，不直接写选中组件模拟成功。
/// 选择属于非破坏性交互，不应往谱面编辑历史写入新动作。
/// 固定投影只提供确定的命中位置，不替代生产几何算法。
/// 操作按输入顺序运行，不能仅测试最终框的并集。
/// 缩框检查关注按住期间的反馈，不能只在释放后修正结果。
/// 移框从重叠区域开始，验证最后创建的框拥有移除优先级。
/// 同号 Note 与 Sample 刻意共存，防止跨 Registry 清错选择。
/// 点选与矩形选择混合，验证基线不会被旧框重新覆盖。
/// 末尾回到经典命令，检查 Ctrl 行为没有被新字段接管。
bool testSelectionOperations()
{
    using namespace MMM::Logic;
    SessionContext ctx;
    ctx.currentBeatmap = std::make_shared<MMM::BeatMap>();
    ctx.currentBeatmap->m_baseMapMetadata.track_count = 4;
    ctx.trackCount                                    = 4;
    ctx.bgmTrackCount                                 = 1;
    ctx.currentTool                                   = EditTool::Marquee;
    ctx.animateTime = ctx.currentTime        = 1.0;
    ctx.lastConfig.settings.professionalMode = true;
    ctx.lastConfig.settings.selectionMode =
        MMM::Config::SelectionMode::Intersection;
    ctx.lastConfig.visual.trackLayout.left  = 0.1f;
    ctx.lastConfig.visual.trackLayout.right = 0.5f;
    ctx.lastConfig.visual.judgeline_pos     = 0.5f;
    // 每轨一百像素，中心分别在 150、250、350，避免边界包含规则混入断言。
    // 元数据与运行态均为四轨，排除布局来源不一致的干扰。
    // BGM 轨提供独立采样域，不和玩家 Note 共用物件身份。
    // 一秒视野对应一秒候选，时间粗筛会同时覆盖多个不同横向位置。
    // 因此可检查未命中矩形的基线成员仍保持选中。
    ctx.cameras.emplace("Basic2DCanvas",
                        CameraInfo{ "Basic2DCanvas", 1000, 600, 0 });
    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache )
        cache = &ctx.timelineRegistry.ctx().emplace<System::ScrollCache>();
    cache->rebuild(
        ctx.timelineRegistry, ctx.lastConfig, ctx.currentBeatmap.get());
    std::array<entt::entity, 3> notes;
    for ( int i = 0; i < 3; ++i ) {
        notes[i] = ctx.noteRegistry.create();
        ctx.noteRegistry.emplace<NoteComponent>(
            notes[i], NoteComponent{ .m_timestamp = 1.0, .m_trackIndex = i });
        // 提供正常会话的排序缓存，候选查询不需要全量 Registry 兜底。
        // Note 分属不同轨道，窄框一次只能命中一项。
        // 不使用重叠 Note，避免重叠层策略干扰集合运算。
        ctx.sortedNoteEntities.push_back(notes[i]);
        ctx.sortedNoteMaxEndPrefix.push_back(1.0);
    }
    const auto sample = ctx.sampleRegistry.create();
    // 独立 Registry 从首个实体分配，同值 ID 也不能共用选择索引。
    // 采样触发时间与锚点一致，不额外扩大本场景的命中几何。
    ctx.sampleRegistry.emplace<SampleComponent>(
        sample, SampleComponent{ .m_timestamp = 1.0, .m_track = 4 });
    ctx.sortedSampleEntities     = { sample };
    ctx.sortedSampleMaxEndPrefix = { 1.0 };
    InteractionController controller(ctx);
    /// @brief 模拟一次完整框选，每次更新均经过生产的几何命中和集合同步。
    // 起笔冻结模式，后续命令只携带终点，与真实画布协议一致。
    // 先刷新预览再结束，覆盖连续更新与最终同步两种状态。
    const auto box =
        [&](SelectionOperation operation, float left, float right) {
            controller.handleCommand(CmdStartMarquee{
                "Basic2DCanvas", left, 400, false, operation });
            controller.handleCommand(CmdUpdateMarquee{ right, 100 });
            controller.updateMarqueeSelection();
            controller.handleCommand(CmdEndMarquee{});
            controller.updateMarqueeSelection();
        };
    /// @brief 校验音符索引中的目标身份，组件一致性由统一选择入口保证。
    const auto selected = [&](int i) {
        return ctx.selectedNoteEntities.contains(notes[i]);
    };
    // 多次框选必须保留先前结果，减选只移除命中的成员。
    // 第一笔选择 A，第二笔追加 B，不要求两个区域相邻。
    // 第三笔覆盖 A，原加选框继续存在也不能把 A 重新选入。
    box(SelectionOperation::Replace, 120, 180);
    if ( !selected(0) || selected(1) ) return false;
    box(SelectionOperation::Add, 220, 280);
    if ( !selected(0) || !selected(1) || ctx.marqueeBoxes.size() != 2 )
        return false;
    box(SelectionOperation::Subtract, 120, 180);
    if ( selected(0) || !selected(1) ) return false;
    // 后来的加选可以恢复前面减掉的 A，运算不能按类型统一分组执行。
    // 随后移除这次加选，前面的减选又应生效，B 在两次操作中始终保留。
    box(SelectionOperation::Add, 120, 180);
    if ( !selected(0) || !selected(1) ) return false;
    controller.handleCommand(CmdRemoveMarqueeAt{ "Basic2DCanvas", 150, 300 });
    controller.updateMarqueeSelection();
    if ( selected(0) || !selected(1) ) return false;
    // 强制刷新不能把减选框误当作加选框，或丢弃前面操作的顺序。
    // 最终同步等低频请求必须与普通脏刷新产生相同集合。
    controller.updateMarqueeSelection(true);
    if ( selected(0) || !selected(1) ) return false;
    // 拖动减选框先覆盖 B，再缩回起点；被临时排除的 B 必须即时恢复。
    // A 已由前一笔排除，缩框只能恢复 B，不能回退整段历史。
    // 新框归零后仍保留之前的有效框，结束不应丢弃整个序列。
    controller.handleCommand(CmdStartMarquee{
        "Basic2DCanvas", 220, 400, false, SelectionOperation::Subtract });
    controller.handleCommand(CmdUpdateMarquee{ 280, 100 });
    controller.updateMarqueeSelection();
    if ( selected(1) ) return false;
    controller.handleCommand(CmdUpdateMarquee{ 220, 400 });
    controller.updateMarqueeSelection();
    if ( !selected(1) ) return false;
    controller.handleCommand(CmdEndMarquee{});
    controller.updateMarqueeSelection();
    // 相同落点优先移除最后创建的框，移掉减选框应恢复 A。
    // 单次只移除一个框，不能同时删除重叠的原始加选框。
    // 剩余框重新求值，不依赖被移除框上一次覆盖的实体缓存。
    controller.handleCommand(CmdRemoveMarqueeAt{ "Basic2DCanvas", 150, 300 });
    controller.updateMarqueeSelection();
    if ( !selected(0) || !selected(1) || ctx.marqueeBoxes.size() != 2 )
        return false;
    controller.handleCommand(CmdRemoveMarqueeAt{ "Basic2DCanvas", 150, 300 });
    controller.updateMarqueeSelection();
    if ( selected(0) || !selected(1) ) return false;
    controller.handleCommand(CmdRemoveMarqueeAt{ "Basic2DCanvas", 250, 300 });
    if ( !ctx.marqueeBoxes.empty() || !ctx.selectedNoteEntities.empty() )
        return false;
    // 基线包含手动选择和同号 Sample，移除框时不能将其一并清除。
    // 先建立手动基线再添加矩形，与仅由框形成的集合对照。
    // 玩家轨道上的减选不能通过同号句柄错误移除采样。
    // 最后一个减选框移除后不再有几何驱动，但手动基线继续有效。
    controller.handleCommand(CmdSelectEntity{
        notes[0], true, ChartObjectKind::PlayerNote, SelectionOperation::Add });
    setChartObjectSelected(ctx, ChartObjectKind::AudioSample, sample, true);
    // 只有一个活动框时，缩回零面积会让整个准备集合为空。
    // 先检查覆盖时确实减掉 A，避免整个手势未命中却令恢复断言通过。
    // 恢复检查同时核对 Sample，防止空框分支误用清空全部选择。
    // 这里不发送释放事件，必须由连续刷新入口完成恢复。
    // 此时仍须在按住期间恢复基线，不能依赖 EndMarquee 补偿。
    controller.handleCommand(CmdStartMarquee{
        "Basic2DCanvas", 120, 400, false, SelectionOperation::Subtract });
    controller.handleCommand(CmdUpdateMarquee{ 180, 100 });
    controller.updateMarqueeSelection();
    if ( selected(0) ) return false;
    controller.handleCommand(CmdUpdateMarquee{ 120, 400 });
    controller.updateMarqueeSelection();
    if ( !selected(0) || !ctx.selectedSampleEntities.contains(sample) )
        return false;
    // 释放丢弃零面积临时框；接下来再次框选应重新捕获恢复后的选择。
    // 起笔保存的基线不能在归零时提前销毁，否则下一次恢复将丢失手动选择。
    controller.handleCommand(CmdEndMarquee{});
    box(SelectionOperation::Subtract, 120, 180);
    if ( selected(0) || !ctx.selectedSampleEntities.contains(sample) )
        return false;
    controller.handleCommand(CmdRemoveMarqueeAt{ "Basic2DCanvas", 150, 300 });
    if ( !selected(0) || !ctx.selectedSampleEntities.contains(sample) )
        return false;
    // 追加点击已选对象应幂等，减选点击未选对象也不能反向选中。
    // 手动点击结束之前的框序列，后续加选从已经解析出的实体集合重新开始。
    // 这可以防止点击移出的对象在下一次框选脏刷新时被旧矩形选回。
    // 同一命令执行两次，刻意区分显式模式与经典 Ctrl 的切换。
    // 无关的 A 与 Sample 必须保留，不能误当作替换模式清除。
    for ( int i = 0; i < 2; ++i )
        controller.handleCommand(CmdSelectEntity{ notes[2],
                                                  false,
                                                  ChartObjectKind::PlayerNote,
                                                  SelectionOperation::Add });
    if ( !selected(0) || !selected(2) ) return false;
    for ( int i = 0; i < 2; ++i )
        controller.handleCommand(
            CmdSelectEntity{ notes[2],
                             false,
                             ChartObjectKind::PlayerNote,
                             SelectionOperation::Subtract });
    if ( !selected(0) || selected(2) ) return false;
    // 显式参数缺省后仍保留经典替换与 Ctrl 追加框选，不继承减选解释。
    // 该检查通过不带新字段的聚合命令执行，覆盖原调用点的缺省初始化。
    // 不在测试里切换实体标志来模拟 Ctrl，而由现有工具实际解析追加参数。
    // 经典框仍按并集合并，显式框的运算字段不会改变原来的几何规则。
    // 首个经典框清除手动基线，第二个 Ctrl 框再追加另一项。
    // 模式标志需要复位，经典框不能继续参与显式减选序列。
    controller.handleCommand(
        CmdStartMarquee{ "Basic2DCanvas", 120, 400, false });
    controller.handleCommand(CmdUpdateMarquee{ 180, 100 });
    controller.handleCommand(CmdEndMarquee{});
    controller.updateMarqueeSelection();
    controller.handleCommand(
        CmdStartMarquee{ "Basic2DCanvas", 220, 400, true });
    controller.handleCommand(CmdUpdateMarquee{ 280, 100 });
    controller.handleCommand(CmdEndMarquee{});
    controller.updateMarqueeSelection();
    if ( !selected(0) || !selected(1) || ctx.marqueeUsesOperations )
        return false;
    // 为空的显式模式保留经典 Ctrl 点击切换；清空同时解除所有框和基线。
    // 第一次取消 A，第二次重新选入 A，验证旧切换规则仍然成立。
    // 空目标加 clearOthers 是非破坏性的清空，不得误发删除命令。
    // 最后检查基线释放，后续手势不能继承已经结束的选择序列。
    controller.handleCommand(CmdSelectEntity{ notes[0], false });
    if ( selected(0) ) return false;
    controller.handleCommand(CmdSelectEntity{ notes[0], false });
    if ( !selected(0) ) return false;
    controller.handleCommand(CmdSelectEntity{ entt::null, true });
    return ctx.selectedNoteEntities.empty() &&
           ctx.selectedSampleEntities.empty() && ctx.marqueeBoxes.empty() &&
           !ctx.marqueeUsesOperations && ctx.marqueeBaselineNotes.empty();
}
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
    // 镜像只改变轨道，不改变右键锚点和批次内部的时间间隔。
    // 先撤销普通粘贴，保证本轮对象完全由镜像命令创建。
    context.actionStack.undo(context);
    controller.handleCommand(
        CmdPaste{ .m_mirrored = true, .m_targetTime = 3.25 });
    if ( context.noteRegistry.view<NoteComponent>().size() != 2 ) return false;
    for ( auto entity : context.noteRegistry.view<NoteComponent>() ) {
        const auto& note = context.noteRegistry.get<NoteComponent>(entity);
        // 四轨镜像将第零、一轨映射为第三、二轨，不能仅比较时间验证镜像成功。
        if ( note.m_trackIndex != 3 && note.m_trackIndex != 2 ) return false;
        const double expected = note.m_trackIndex == 3 ? 3.25 : 3.75;
        if ( std::abs(note.m_timestamp - expected) > 1e-9 ) return false;
    }
    // 显式位置只作用于本次命令，不能偷偷改用户的默认粘贴偏好。
    // 撤销清除上一批结果，再用无锚点命令验证兼容入口。
    // 镜像批次也由同一动作栈整体撤销，默认粘贴不复用镜像标志。
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
        if ( !testSelectionOperations() ) {
            XERROR("Selection operation geometry or baseline regression");
            ok = false;
        }
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
        // 三个候选包含同号采样与折线的不同部位，必须保留完整复合身份。
        // 相同矩形确保命中顺序取决于真实图层规则，而不是测试坐标误差。
        // 节点与头部同属一个音符实体，采样则故意复用相同实体整数。
        // 这个组合能够同时检出只按 entity 去重和丢弃 subIndex 的回归。
        // 三个候选严格按快照顺序插入，预期列表使用画布原有的逆序拾取。
        // 不加载真实资源，避免图形设备或纹理状态影响身份验证结论。
        // 后续仅删除其中一个部位，不通过清空整个快照简化陈旧条目场景。
        // 测试使用新实体号，避免前面的菜单穿透计数掺入本轮选择断言。
        snapshot.hitboxes = { { static_cast<entt::entity>(70),
                                MMM::Common::Render::HoverPart::Head,
                                -1,
                                100,
                                100,
                                100,
                                100 },
                              { static_cast<entt::entity>(70),
                                MMM::Common::Render::HoverPart::PolylineNode,
                                2,
                                100,
                                100,
                                100,
                                100 },
                              { static_cast<entt::entity>(70),
                                MMM::Common::Render::HoverPart::SampleAnchor,
                                -1,
                                100,
                                100,
                                100,
                                100,
                                MMM::Logic::ChartObjectKind::AudioSample } };
        frame(interaction, snapshot, false);
        io.AddMouseButtonEvent(1, true);
        ok &= frame(interaction, snapshot, false);
        // 观察发布结果，而不是仅检查私有优先项是否被赋值。
        // 后续真实左键需要把同一完整身份写入拖动起笔载荷。
        int                         chosen      = 0;
        bool                        nodeDragged = false;
        MMM::Logic::ChartObjectKind chosenKind{};
        const auto                  layerSubscription = bus.subscribe<
            MMM::Event::LogicCommandEvent>([&](const auto& event) {
            if ( const auto* command = std::get_if<MMM::Logic::CmdSelectEntity>(
                     &event.command) ) {
                ++chosen;
                chosenKind = command->kind;
            }
            if ( const auto* command =
                     std::get_if<MMM::Logic::CmdStartDrag>(&event.command) )
                nodeDragged =
                    command->entity == static_cast<entt::entity>(70) &&
                    command->kind == MMM::Logic::ChartObjectKind::PlayerNote &&
                    command->hitPart ==
                        static_cast<std::uint8_t>(
                            MMM::Common::Render::HoverPart::PolylineNode) &&
                    command->hitSubIndex == 2;
        });
        // 列表按渲染逆序，第二项为折线节点；菜单选择必须真的发布选择命令。
        // 直接调用的仅是菜单项激活辅助函数，候选采集与下一次拖动均为真实输入。
        // 若生产代码只更新菜单勾选而没有改变拾取优先级，后续断言会失败。
        ok &= MMM::Canvas::EditorInteractionSurfaceTestAccess::choose(
            interaction, 1, snapshot);
        ok &= chosen == 1 &&
              chosenKind == MMM::Logic::ChartObjectKind::PlayerNote;
        io.AddMouseButtonEvent(1, false);
        frame(interaction, snapshot, false);
        frame(interaction, snapshot, true, { 50, 250 });
        frame(interaction, snapshot, false, { 50, 250 });
        // 返回重叠区域后仍应抓到刚选的节点，不能自动退回最上面的采样。
        // 关闭菜单的点击不属于拖动；释放后另起一笔才允许编辑。
        // 中间空闲帧使 hover 正常刷新，不依赖打开菜单前的缓存命中。
        frame(interaction, snapshot, false);
        frame(interaction, snapshot, true);
        frame(interaction, snapshot, false);
        ok &= nodeDragged;
        // 同号跨 Registry 候选独立选择，不能只按实体整数查找和去重。
        // AudioSample 是独立领域，发给 PlayerNote 注册表会修改错误对象。
        // 两次菜单激活不需要靠重新创建控制器清理状态。
        ok &= MMM::Canvas::EditorInteractionSurfaceTestAccess::choose(
            interaction, 0, snapshot);
        ok &= chosenKind == MMM::Logic::ChartObjectKind::AudioSample;
        const int beforeStale = chosen;
        // 节点在菜单停留时被其他编辑移除；陈旧条目必须没有命令副作用。
        // 只删除节点 hitbox，保留同实体头部，避免整实体检查误判为安全。
        snapshot.hitboxes.erase(snapshot.hitboxes.begin() + 1);
        ok &= !MMM::Canvas::EditorInteractionSurfaceTestAccess::choose(
            interaction, 1, snapshot);
        // 同路径重新载入也会复用实体号，实例代数必须阻止旧菜单操作新谱面。
        // 文件路径保持不变，使测试明确覆盖代数检查而非仅路径比较。
        // 计数保持不变证明失效选择不会静默发送到当前活动会话。
        ++snapshot.beatmapInstanceId;
        ok &= !MMM::Canvas::EditorInteractionSurfaceTestAccess::choose(
            interaction, 0, snapshot);
        ok &= chosen == beforeStale;
        bus.unsubscribe<MMM::Event::LogicCommandEvent>(layerSubscription);
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
