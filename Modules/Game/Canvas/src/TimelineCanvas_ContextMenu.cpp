#include "canvas/TimelineCanvas.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "config/VisualConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/imgui/ClipboardBridge.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <utility>

namespace MMM::Canvas
{
/// @brief 将命中目标并入或移出本地 Timing 选择，不直接修改谱面数据。
/// @param entity 已由调用方按泳道、部位和可选设置验证过的目标。
/// @param preserveGroup 抓取工具点击已选项时保留组，选中工具直接替换。
/// @param additiveSelection 经典 Ctrl/Shift 的切换意图，显式方案模式优先。
/// @details 本函数不验证目标是否可选，选中工具和抓取工具保留各自原有门禁。
/// 返回值只控制后续拖动，单纯更新选择不会向逻辑层提交谱面修改。
/// @return 可以从该目标开始拖动时返回真，减选操作返回假。
/// @warning 只在点击边沿操作本地集合，不读 ECS 或等待逻辑线程。
bool TimelineCanvas::selectTimingTarget(entt::entity entity, bool preserveGroup,
                                        bool additiveSelection)
{
    const bool wasSelected = m_selectedTimingEntities.contains(entity);
    const auto operation =
        UI::Interaction::activeProfile().selectionOperation();
    // 纯鼠标的减选是差集，重复点击不反向加入对象，也不能拖动被移出的对象。
    if ( operation == Logic::SelectionOperation::Subtract ) {
        m_selectedTimingEntities.erase(entity);
        return false;
    }
    // 经典修饰键是切换，纯鼠标追加则是幂等并集；两种规则必须独立解释。
    if ( !operation && additiveSelection && wasSelected ) {
        m_selectedTimingEntities.erase(entity);
        return false;
    }
    const bool replace = operation == Logic::SelectionOperation::Replace ||
                         (!operation && !additiveSelection);
    // 抓取已选项保留整组移动；选中工具的替换始终只留下直接点击的目标。
    if ( replace && (!preserveGroup || !wasSelected) )
        m_selectedTimingEntities.clear();
    m_selectedTimingEntities.insert(entity);
    return true;
}

/// @brief 全选 Timing 画布可见的可编辑目标，与键盘入口保持同一过滤规则。
/// @details 使用快照目标而非 ECS 遍历，保留 BPM 参与选择的配置开关。
/// @post 当前集合只含本快照中可编辑的实体；没有目标时为空。
/// 同时间同类型的重复几何通过实体集合去重，不产生重复编辑条目。
/// @warning 仅在用户激活全选时收集可见目标，不增加逐帧全选扫描。
void TimelineCanvas::selectAllTimingEvents()
{
    m_selectedTimingEntities.clear();
    // 不从主画布借用音符选区；时间线仅操作自身可编辑的时间点。
    // 这个入口也用于 Ctrl+A，保证新增鼠标入口不会改变经典全选范围。
    for ( const auto& target : collectVisibleTimingTargets() ) {
        if ( isTimingTargetSelectable(target) )
            m_selectedTimingEntities.insert(target.entity);
    }
}

/// @brief 维护纯鼠标 Timing 右键菜单，所有业务仍通过时间线自身入口执行。
/// @param origin 当前时间线图像的屏幕原点。
/// @param size 图像交互尺寸，不含父窗口边框或工具栏。
/// @param hovered 上层确认指针属于当前画布。
/// @return 菜单开启、持续或刚关闭的帧返回真，阻止后续输入穿透。
/// @pre 当前快照存在且已通过 hasBeatmap 检查，并处于时间线输入域。
/// @details 每个视图用名称隔离弹窗，谱面路径与实例代数隔离菜单目标。
/// 普通右键菜单不会接管画笔擦除、活动拖动或已经打开的编辑弹窗。
/// @warning 每帧不读 ECS 或剪贴板；只在用户选择菜单项后组装编辑命令。
bool TimelineCanvas::handleTimingContextMenu(ImVec2 origin, ImVec2 size,
                                             bool hovered)
{
    ImGui::PushID(m_name.c_str());
    // ImGui 可能在本帧开始就关闭外部点击的弹窗，仍需记住上一帧所有权。
    const bool wasOpen =
        ImGui::IsPopupOpen("TimingEditMenu") || m_timingContextWasOpen;
    const auto tool = m_currentSnapshot->currentTool;
    // 工具和播放状态动态校验，菜单停留期间也不能绕过编辑门禁。
    // 经典方案返回假，不改变其右键交互或键盘快捷键路径。
    const bool enabled =
        UI::Interaction::activeProfile().selectionContextMenu() &&
        (tool == Logic::EditTool::Move || tool == Logic::EditTool::Marquee) &&
        !m_currentSnapshot->isPlaying;
    const auto mouse = ImGui::GetMousePos();
    const auto local = ImVec2(mouse.x - origin.x, mouse.y - origin.y);
    // hovered 是窗口级信息，额外用图像范围排除边距和画布外右键。
    const bool inside =
        local.x >= 0 && local.y >= 0 && local.x < size.x && local.y < size.y;
    const bool anotherPopup = ImGui::IsPopupOpen(
        nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // 其他弹窗优先拥有输入；这里不强制抢焦点，也不关闭用户现有草稿。
    bool opened = false;
    // 右上角 56 像素保留给既有工具按钮，不同时打开两个菜单。
    // 左键和中键按住时不插入右键事务，避免吞掉尚未提交的编辑手势。
    // 画笔右键仍负责擦除；已有拖动或其他模态不能被右键菜单夺走释放事件。
    if ( enabled && !wasOpen && !anotherPopup && hovered && inside &&
         !(local.x >= size.x - 56 && local.y <= 56) &&
         !hasActivePointerGesture() && !m_isPopupOpen && !m_isCreatePopupOpen &&
         !m_isInterpolationEditorOpen && !m_requestInterpolationEditor &&
         !ImGui::GetIO().WantTextInput &&
         !UI::ShortcutUtils::isShortcutRecordingActive() &&
         !UI::Interaction::mouseDown(0) && !UI::Interaction::mouseDown(2) &&
         UI::Interaction::mouseClicked(1) ) {
        // 在打开时锁存吸附结果，鼠标随后移到菜单不再改变粘贴锚点。
        // 复用 Timing 的坐标反投影与吸附，不用播放头时间或主画布音符吸附。
        // 零点钳制与既有时间线粘贴规则一致，避免创建负时间事件。
        bool snapped        = false;
        m_timingContextTime = std::max(
            0.0,
            snapTimingTime(
                size, canvasTimeAtLocalY(size, local.y), local.y, snapped));
        m_timingContextBeatmapKey = m_currentSnapshot->beatmapPathKey;
        m_timingContextInstance   = m_currentSnapshot->beatmapInstanceId;
        // 同路径重新打开也是新实例，不能仅靠文件名认为旧实体仍然有效。
        // 空选区可以直接右键一个时间点；已有多选保持原集合，不因菜单位置丢失。
        // pickTimingTarget 仍按专业模式泳道限制类型，不能只比较纵坐标。
        // 不可选 BPM 保持空选择，菜单里的复制和删除因此仍然禁用。
        if ( m_selectedTimingEntities.empty() ) {
            const auto target = pickTimingTarget(origin, size, local.y);
            if ( target && isTimingTargetSelectable(*target) )
                m_selectedTimingEntities.insert(target->entity);
        }
        ImGui::OpenPopup("TimingEditMenu");
        opened = true;
    }
    if ( ImGui::BeginPopup("TimingEditMenu") ) {
        // 每帧重新验证会话，而不是等点击时把旧菜单操作发送到新谱面。
        const bool sameBeatmap =
            m_timingContextBeatmapKey == m_currentSnapshot->beatmapPathKey &&
            m_timingContextInstance == m_currentSnapshot->beatmapInstanceId;
        if ( !enabled || !sameBeatmap ) ImGui::CloseCurrentPopup();
        // CloseCurrentPopup 只请求关闭，本帧剩余项目必须同步禁用。
        ImGui::BeginDisabled(!enabled || !sameBeatmap);
        // 历史属于当前时间线会话，不走可能针对主画布音符的全局编辑动作。
        // 撤销栈为空时由逻辑层忽略，与原快捷键行为一致。
        if ( UI::FeedbackMenuItem(TR("ui.edit.undo").data()) )
            publishTimelineCommand(Event::LogicCommandEvent(Logic::CmdUndo{}));
        if ( UI::FeedbackMenuItem(TR("ui.edit.redo").data()) )
            publishTimelineCommand(Event::LogicCommandEvent(Logic::CmdRedo{}));
        ImGui::Separator();
        const bool hasSelection = !m_selectedTimingEntities.empty();
        // 空选择禁用剪切、复制和删除；粘贴不依赖本地是否选中时间点。
        // 这些入口负责批量命令和历史边界，不在菜单中重复组装业务载荷。
        if ( UI::FeedbackMenuItem(
                 TR("ui.edit.copy").data(), nullptr, false, hasSelection) )
            copySelectedTimingEvents(false);
        if ( UI::FeedbackMenuItem(
                 TR("ui.edit.cut").data(), nullptr, false, hasSelection) )
            copySelectedTimingEvents(true);
        if ( UI::FeedbackMenuItem(TR("ui.edit.paste").data()) ) {
            // 只在点击时读取系统剪贴板，保留原 Timing 粘贴的拍位和元数据语义。
            // 外部内容可被原桥接器解析；无法导入时仍保留编辑器内部剪贴板。
            // 不主动更改全局粘贴时间偏好，锚点仅作用于本次右键操作。
            UI::ClipboardBridge::importEditorClipboardFromSystem();
            pasteTimingClipboard(m_timingContextTime);
        }
        if ( UI::FeedbackMenuItem(
                 TR("ui.common.delete").data(), nullptr, false, hasSelection) )
            // 批量删除由共用函数一次提交，撤销不需要逐个恢复时间点。
            deleteSelectedTimingEvents();
        ImGui::Separator();
        if ( UI::FeedbackMenuItem(TR("ui.edit.select_all").data()) )
            selectAllTimingEvents();
        // 清空只改变即时选择，不删除时间点，也不追加谱面撤销历史。
        if ( UI::FeedbackMenuItem(TR("ui.selection.clear").data(),
                                  nullptr,
                                  false,
                                  hasSelection) ) {
            m_selectedTimingEntities.clear();
            // 旧框选基线不能在下一次框选中复活已清空的选择。
            m_timingMarqueeBaseSelection.clear();
        }
        ImGui::Separator();
        // 与主画布共享同一应用级模式，跨表面操作不需要记住两套选择开关。
        // 切换只作用于下一笔，已完成的选区和撤销历史不受模式设置影响。
        // 活动手势不能打开菜单，框选也会在起笔锁存模式以保证拖动过程稳定。
        auto& profile = UI::Interaction::activeProfile();
        for ( const auto& [operation, key] :
              { std::pair{ Logic::SelectionOperation::Replace,
                           "ui.selection.replace" },
                std::pair{ Logic::SelectionOperation::Add, "ui.selection.add" },
                std::pair{ Logic::SelectionOperation::Subtract,
                           "ui.selection.subtract" } } ) {
            // 勾选表示后续点击或框选的集合语义，不表示当前选中了几个目标。
            // 复用反馈菜单封装，按钮音效与其他时间线菜单保持一致。
            if ( UI::FeedbackMenuItem(
                     TR(key).data(),
                     nullptr,
                     profile.selectionOperation() == operation) )
                profile.setSelectionOperation(operation);
        }
        ImGui::Separator();
        // 导航复用菜单打开时的 Timing 吸附锚点，不使用菜单内指针或当前播放头。
        // 没有激活时不做时间换算或命令组装，普通菜单帧只绘制静态项目。
        const bool seek =
            UI::FeedbackMenuItem(TR("ui.selection.seek_here").data());
        const bool play =
            UI::FeedbackMenuItem(TR("ui.selection.play_here").data());
        if ( seek || play ) {
            // 与画布坐标换算保持一致，CmdSeek 接收移除视觉偏移后的播放时间。
            // 无效锚点不能仅跳过 Seek 后仍开启播放，否则会在旧播放头误播。
            const double offset    = Config::AppConfig::instance()
                                         .getVisualConfig()
                                         .getEffectiveVisualOffset();
            const double audioTime = m_timingContextTime - offset;
            if ( std::isfinite(m_timingContextTime) &&
                 std::isfinite(audioTime) ) {
                // 发布仍经过时间线既有路由，保持用户输入与会话归属一致。
                // 此操作不改变本地选区或框选基线，也不编辑任何 Timing 参数。
                publishTimelineCommand(
                    Event::LogicCommandEvent(Logic::CmdSeek{ audioTime }));
                // 定位必须先于开始播放；只跳转不会改变原本的暂停状态。
                // 依赖事件队列维持顺序，不插入 sleep 或阻塞等待音频同步。
                if ( play )
                    publishTimelineCommand(Event::LogicCommandEvent(
                        Logic::CmdSetPlayState{ true }));
            }
        }
        ImGui::Separator();
        // 点和段落是下一笔的显式意图，菜单本身不生成 Timing 实体。
        // 菜单只在空闲抓取或选择工具中打开，不会重解释进行中的区间拖动。
        // 临时开关与主画布共用方案状态，但不修改 Timing 的 BPM 或 Scroll 值。
        // 已冻结的菜单锚点保持不变，下一次打开才读取新的吸附状态。
        // 绘制类型由点按与拖动自动区分，此处只保留临时吸附覆盖。
        // 开关值不进入剪贴板、谱面保存或撤销历史。
        // 使用反馈菜单确保文字行和勾选区域都有相同的点击反馈。
        if ( UI::FeedbackMenuItem(TR("ui.mouse_drawing.bypass_snap").data(),
                                  nullptr,
                                  profile.bypassSnapping()) )
            profile.setBypassSnapping(!profile.bypassSnapping());
        ImGui::EndDisabled();
        // 标准菜单激活后自然关闭，返回值仍拦截本帧鼠标释放，防止穿透编辑。
        ImGui::EndPopup();
    }
    m_timingContextWasOpen = ImGui::IsPopupOpen("TimingEditMenu");
    ImGui::PopID();
    // 下一帧可以重新取得窗口焦点；这里不主动调用 SetWindowFocus 抢占弹窗。
    // 关闭帧也消费输入；返回画布的下一次独立按下才可以开启新编辑手势。
    return wasOpen || opened || m_timingContextWasOpen;
}

}  // namespace MMM::Canvas
