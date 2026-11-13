#include "TimelineInteractionPresentation.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/VisualConfig.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/interaction/EditorInteraction.h"
#include <cmath>
#include <optional>

namespace MMM::Canvas
{
/// @brief 平台端点编辑借用既有时间轴，不复制 BPM、HS 或吸附算法。
/// @warning 连续帧 snap=false 只反投影；snap=true 仅允许释放时调用。
double TimelineInteractionPresentation::rangeTime(TimelineCanvas& c,
                                                  const ImVec2&   size,
                                                  float localY, bool snap)
{
    const auto time = c.canvasTimeAtLocalY(size, localY);
    if ( !snap ) return time;
    // 原助手负责已有时间点、拍线、忽略吸附开关和曲线 BPM 的统一语义。
    // 临时拍点容器限制在这次离散释放，不进入端点每帧跟随路径。
    bool snapped{};
    return c.snapTimingTime(size, time, localY, snapped);
}
/// @brief 正反投影来自同一画布，窗口移动或纵向补偿不会改变把手时间。
/// @warning 每帧固定快照查询，不持有会话所有权。
float TimelineInteractionPresentation::rangeY(TimelineCanvas& c,
                                              const ImVec2& size, double time)
{
    return static_cast<float>(c.canvasYAtTime(size, time));
}
/// @brief 最终端点只提交一条原生 Timing 更新，保留全部曲线与采样参数。
/// @warning 正常释放的离散路径复制值；不在预览帧复制共享缓存所有权。
void TimelineInteractionPresentation::commitRange(
    TimelineCanvas& c, const Common::Render::TimingInterpolationElement& value)
{
    // 不展开段落为采样点，不合并相邻段，不触碰其它已选时间点。
    // 不传元数据覆盖，时间变化时由原命令清理失效拍位并保留其它格式属性。
    c.publishTimelineCommand(Event::LogicCommandEvent(
        Logic::CmdUpdateTimelineEvent{ value.entity,
                                       value.time,
                                       value.value,
                                       std::nullopt,
                                       value.interpolation }));
}
/// @brief 通用入口只进行一次平台分派，具体解释器拥有私有输入状态。
/// @warning 每帧虚调用，不获取会话互斥锁或创建新的呈现对象。
void TimelineCanvas::handleTimingCanvasInteraction(const ImVec2& origin,
                                                   const ImVec2& size,
                                                   bool hovered, bool focused,
                                                   bool checked)
{
    m_timingPresentation->update(
        *this, origin, size, hovered, focused, checked);
}
/// @brief 平台候选与原业务预览共同构成输入占有，避免待定阶段切换方案。
/// @warning 每帧仅读取本地标量；不访问逻辑线程可变数据。
bool TimelineCanvas::hasActivePointerGesture() const
{
    return m_timingPresentation->hasGesture() || m_isTimingDragging ||
           m_isTimingErasing || m_isTimingMarqueeSelecting ||
           m_isTimingDrawPreviewing || m_isInterpolationDragging ||
           m_isTableRowDragSelecting || m_isAudioTimeSliderScrubbing;
}
/// @brief 桌面默认路径保持原参数和物理边沿，不使用触控门槛。
/// @warning 每帧转发；原控制器继续负责输入域和播放权限。
void TimelineInteractionPresentation::update(TimelineCanvas& c,
                                             const ImVec2&   origin,
                                             const ImVec2& size, bool hovered,
                                             bool focused, bool checked)
{
    // 不修改输入源和点击计数，桌面双击、修饰键和右键行为继续由原函数解释。
    // 原来的独立测试入口仍自行执行 profile dispatch，正式 update 可标记已检查。
    // 此默认实现没有待定触点或移动菜单状态，隐藏时也不额外改变业务标志。
    c.handleTimingCanvasPointerInput(origin, size, hovered, focused, checked);
}
/// @brief 当前快照仅借用到本帧结束，派生类不得跨帧保存指针。
const Common::Render::RenderSnapshot* TimelineInteractionPresentation::snapshot(
    TimelineCanvas& c)
{
    return c.m_currentSnapshot;
}
/// @brief 与时间反投影采用同一显示偏移，保证捏合中点不会因布局补偿漂移。
/// @details 只读 UI 已应用的本地值，不重新请求相机或跨线程等待快照。
float TimelineInteractionPresentation::contentYOffset(TimelineCanvas& c)
{
    // 这是画布已经应用的显示值，不触发新的视口测量或相机同步。
    return c.m_lastAppliedYOffset;
}
/// @brief 两个分类标志只在同步业务调用内有效，不污染后续桌面控件。
/// @warning 每帧固定标量赋值，不改变全局触摸队列。
void TimelineInteractionPresentation::route(TimelineCanvas& c,
                                            const ImVec2&   origin,
                                            const ImVec2& size, bool hovered,
                                            bool focused, bool checked,
                                            bool tapOnly, bool finalUpdate)
{
    // tapOnly 只控制本次指针调用，不能泄漏到别的 Timeline 或主画布。
    // completedDrag 允许释放帧更新末端，经典鼠标默认仍使用原有规则。
    // 两个标志都不进入可保存配置，重开应用不会继承上次手势的分类。
    // 原函数内部的提前返回不影响作用域之后的复位。
    c.m_pointerTapOnly     = tapOnly;
    c.m_pointerFinalUpdate = finalUpdate;
    c.handleTimingCanvasPointerInput(origin, size, hovered, focused, checked);
    c.m_pointerTapOnly = c.m_pointerFinalUpdate = false;
}
/// @brief 未提交预览没有模型副作用；接管不得借 Undo 回退用户已有编辑。
/// @details 清理点、区间、移动、擦除和框选标志，保留已打开参数草稿。
void TimelineInteractionPresentation::cancel(TimelineCanvas& c)
{
    // Timing 的拖动只在抬手时发布整批更新，当前预览尚无模型副作用。
    // 不调用撤销，避免把接管之前已完成的有效编辑一并回退。
    // 右键擦除的临时目标也属于本笔，不能在下一次松手时迟到删除。
    // 参数弹窗标志不在这里清除，已经交给用户确认的草稿拥有独立生命周期。
    // 同理不修改音频滑块或表格选择，这些入口拥有各自的输入区域。
    c.m_isTimingDragging = c.m_isTimingErasing = false;
    c.m_isTimingMarqueeSelecting = c.m_isTimingDrawPreviewing = false;
    c.m_isInterpolationDragging                               = false;
    c.m_timingDragEntries.clear();
    c.m_timingEraseTargetEntities.clear();
    c.m_timingMarqueeBaseSelection.clear();
    c.m_timingDragPreviewDelta = 0;
    c.m_hoveredTimingEntity    = entt::null;
}
/// @brief 菜单共用相同的投影与吸附规则，不采用主画布音符时间。
void TimelineInteractionPresentation::prepareMenu(TimelineCanvas& c,
                                                  const ImVec2&   origin,
                                                  const ImVec2&   size)
{
    // 目标命中同时考虑类型泳道、插值正文和用户允许的选择范围。
    // 只在打开边沿扫描候选，菜单持续帧不会重复建立选择。
    // 冻结的是秒时间而非屏幕坐标，菜单移动和后续按钮布局不会重算锚点。
    c.prepareTimingContextMenu(origin, size);
}
/// @brief 路径和实例同时匹配，播放切换后立即禁用旧菜单。
bool TimelineInteractionPresentation::validMenu(TimelineCanvas& c)
{
    // 文件路径不足以证明是同一份谱面，同路径重开后的实体编号可能被复用。
    // 菜单冻结的是值身份而不是快照地址，不依赖渲染缓冲交换后的地址稳定性。
    // 播放中一律拒绝编辑，暂停后仍需重新打开菜单获得当前目标。
    return c.m_currentSnapshot && c.m_currentSnapshot->hasBeatmap &&
           !c.m_currentSnapshot->isPlaying &&
           c.m_timingContextBeatmapKey == c.m_currentSnapshot->beatmapPathKey &&
           c.m_timingContextInstance == c.m_currentSnapshot->beatmapInstanceId;
}
/// @brief 依赖选择的动作只检查本地集合，参数入口使用打开时锁存的目标。
bool TimelineInteractionPresentation::enabled(TimelineCanvas&  c,
                                              TimingMenuAction a)
{
    // 可用性不意味着动作已经执行，也不读取系统剪贴板以预测能否粘贴。
    // 实际载荷和范围校验继续由对应业务入口负责。
    switch ( a ) {
    // Copy 只写内部格式，Paste 保留相对时差并把首项放到冻结锚点。
    // 这两种动作都不借用当前按钮坐标，因此点击菜单不会改变目标时间。
    case TimingMenuAction::Copy:
    case TimingMenuAction::Cut:
    case TimingMenuAction::Delete: return !c.m_selectedTimingEntities.empty();
    // 参数入口在菜单结束后调用，原窗口继续承担最终输入校验和提交。
    // 段落使用完整曲线草稿，不把插值正文退化成普通 marker。
    case TimingMenuAction::Edit: return c.m_timingContextEntity != entt::null;
    default: return true;
    }
}
/// @brief 平台菜单不重新实现剪贴板格式、批量修改和段落编辑规则。
/// @details 调用仅发生在按钮激活边沿，连续绘制不读系统剪贴板。
void TimelineInteractionPresentation::perform(TimelineCanvas&  c,
                                              TimingMenuAction a)
{
    // 这里只处理用户激活边沿；静态菜单绘制不能生成编辑事件。
    // 语义入口保持独立，不能用模拟快捷键触发另一个当前焦点窗口的动作。
    // 时间线自己的发布函数会保留会话路由，避免误投主画布的音符选择。
    // 选择模式属于方案本地状态，不向后台线程广播整份设置。
    // 全选只收集快照中的可编辑目标，仍遵循 BPM 可参与选择配置。
    // 删除和剪切各有原业务协议，不能在菜单中自行删 ECS 实体。
    // Undo/Redo 复用同一会话历史，空历史由逻辑层自然忽略。
    auto& p = UI::Interaction::activeProfile();
    switch ( a ) {
    case TimingMenuAction::Copy: c.copySelectedTimingEvents(false); break;
    case TimingMenuAction::Paste:
        c.pasteTimingClipboard(c.m_timingContextTime);
        break;
    case TimingMenuAction::Cut:
        // 沿用桌面先建立 Timing 剪贴板、再整批删除源事件的顺序。
        c.copySelectedTimingEvents(true);
        break;
    case TimingMenuAction::Delete: c.deleteSelectedTimingEvents(); break;
    case TimingMenuAction::Undo:
        c.publishTimelineCommand(Event::LogicCommandEvent(Logic::CmdUndo{}));
        break;
    case TimingMenuAction::Redo:
        c.publishTimelineCommand(Event::LogicCommandEvent(Logic::CmdRedo{}));
        break;
    case TimingMenuAction::SelectAll: c.selectAllTimingEvents(); break;
    // 清空只解除时间线选择，不删除对象，也不创建撤销节点。
    // 活动框选不能打开菜单，所以这里无需中止另一个指针事务。
    case TimingMenuAction::Clear: c.m_selectedTimingEntities.clear(); break;
    case TimingMenuAction::Replace:
        p.setSelectionOperation(Logic::SelectionOperation::Replace);
        break;
    case TimingMenuAction::Add:
        p.setSelectionOperation(Logic::SelectionOperation::Add);
        break;
    case TimingMenuAction::Subtract:
        p.setSelectionOperation(Logic::SelectionOperation::Subtract);
        break;
    case TimingMenuAction::Snap:
        p.setBypassSnapping(!p.bypassSnapping());
        break;
    case TimingMenuAction::Edit: c.editTimingContextTarget(); break;
    case TimingMenuAction::Seek: {
        // 与桌面定位相同，谱面时间转换为音频时间；菜单不会隐式开始播放。
        // 坐标反投影给出谱面视觉时间，音频定位必须减掉同口径的视觉偏移。
        // 非有限时间不能进入 CmdSeek，也不隐式触发播放。
        const double t =
            c.m_timingContextTime - Config::AppConfig::instance()
                                        .getVisualConfig()
                                        .getEffectiveVisualOffset();
        if ( std::isfinite(t) )
            c.publishTimelineCommand(
                Event::LogicCommandEvent(Logic::CmdSeek{ t }));
        break;
    }
    }
}
/// @brief 集合借用仅供起笔基线及取消恢复，不暴露到平台公共接口。
std::unordered_set<entt::entity>& TimelineInteractionPresentation::selection(
    TimelineCanvas& c)
{
    // 借用只在当前 UI 栈有效，派生对象保存的是起笔时的值副本。
    // 接管恢复使用 swap，取消边沿不需要再次分配整份集合。
    // 正常结束只清理副本，不能覆写用户刚刚完成的有效选择。
    return c.m_selectedTimingEntities;
}
}  // namespace MMM::Canvas
