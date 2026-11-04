#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "config/skin/translation/Translation.h"
#include "event/logic/LogicCommandEvent.h"
#include "presentation/CanvasInteractionPresentation.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/utils/UIWidgetUtils.h"
#include <imgui.h>
#include <utility>

namespace MMM::Canvas
{
namespace
{
/// @brief 经典桌面菜单与指针路由，完全不包含移动手势或触控按钮头。
class DesktopCanvasInteractionPresentation final
    : public CanvasInteractionPresentation
{
public:
    /// @param canvas 原画布控制器，保留其完整鼠标手势状态。
    /// @param sourceManager 同步菜单动作接收者，不跨帧保存。
    /// @param currentSnapshot 与原画布相同的不可变显示快照。
    /// @param frame 既有表面路由给出的边界和焦点。
    /// @param targetWidth 原交互投影宽度，不由菜单布局重新测量。
    /// @param targetHeight 原交互投影高度，维持既有轨道拾取范围。
    /// @details 本实现从原控制器菜单提取，命令发布先后顺序不变。
    /// 没有长按阈值、触点回放或移动按钮参与桌面输入。
    /// @brief 保留已提交桌面路径的命令顺序、修饰键和标准菜单布局。
    /// @warning 每帧沿用原控制器，不进行等待、文件访问或谱面复制。
    void update(Basic2DCanvasInteraction& canvas, UI::UIManager* sourceManager,
                const Common::Render::RenderSnapshot* currentSnapshot,
                const UI::Interaction::SurfaceFrame& frame, float targetWidth,
                float targetHeight) override
    {
        // 只借用原菜单状态，不重置选区、粘贴偏好或快捷键配置。
        auto ctx = context(canvas);

        // 按相机隔离菜单身份，两个画布不能共享一次右键打开状态。
        ImGui::PushID(ctx.cameraId.c_str());
        const bool menuOpen = ImGui::IsPopupOpen("SelectionEditMenu");
        ImGui::PopID();
        // 策略来自操作方案，画布不比较持久化方案字符串。
        // 仅选中工具提供此菜单，绘制工具的右键擦除不受影响。
        const bool selectionMenu =
            UI::Interaction::activeProfile().selectionContextMenu() &&
            currentSnapshot->currentTool == Logic::EditTool::Marquee &&
            currentSnapshot->hasBeatmap;
        // 菜单打开期间冻结画布指针目标，粘贴仍以右键落点为参照。
        // 菜单文字上的移动不能重新计算谱面时间或改变活动轨道。
        // 不遍历选择集合；复制和剪切由逻辑层执行时读取权威选择。
        // 菜单点击不进入框选状态机，也不覆盖原选择或产生额外编辑事务。
        if ( !menuOpen ) {
            routeInput(canvas, currentSnapshot, targetWidth, targetHeight);
        }
        // 先让正常鼠标路径发布本次落点，再打开菜单，保持命令队列顺序。
        // 左键框选或中键平移尚未结束时不抢走它们的释放事件。
        // 内容区与窗口悬浮必须同时成立，标题栏和留白不能触发编辑菜单。
        // 播放中不新开菜单，打开后开始播放则禁用条目。
        const auto pointer = ImGui::GetMousePos();
        const bool inside  = pointer.x >= frame.m_x && pointer.y >= frame.m_y &&
                             pointer.x < frame.m_x + frame.m_width &&
                             pointer.y < frame.m_y + frame.m_height;
        // 菜单 ID 作用域不包围原控制器，保持批注等既有控件身份不变。
        ImGui::PushID(ctx.cameraId.c_str());
        if ( selectionMenu && !menuOpen && frame.m_hovered && inside &&
             !currentSnapshot->isPlaying && !currentSnapshot->isSelecting &&
             !UI::Interaction::mouseDown(0) && !UI::Interaction::mouseDown(2) &&
             UI::Interaction::mouseClicked(1) ) {
            // 锁存本帧实际显示的吸附线；菜单中的鼠标移动和后续快照不能改写。
            // 无吸附时使用可见鼠标时间，仍不回退到播放头。
            // 使用同一快照的 snappedTime，与本帧高亮线的绘制时间一致。
            // 不写入全局粘贴偏好，避免一次右键操作影响后续快捷键。
            ctx.pasteTime =
                currentSnapshot->isSnapped &&
                        !UI::Interaction::activeProfile().bypassSnapping()
                    ? currentSnapshot->snappedTime
                    : currentSnapshot->hoveredTime;
            ctx.menuPoint = ctx.pointerPoint;
            // 菜单可跨多个快照停留；记录谱面实例以隔离同路径重载。
            // 所有候选仍是值语义 ID，不借用逻辑线程里的对象或容器。
            ctx.beatmapKey      = currentSnapshot->beatmapPathKey;
            ctx.beatmapInstance = currentSnapshot->beatmapInstanceId;
            // 坐标已经过当前相机逆投影，菜单点击不重新取屏幕坐标换算。
            ImGui::OpenPopup("SelectionEditMenu");
        }
        // 标准菜单沿用 ImGui 的关闭和点击捕获，无需新增可见按钮样式。
        // 动作身份复用顶栏编辑菜单，系统剪贴板仅在用户激活时访问。
        // 不在每帧探测系统剪贴板，避免平台调用阻塞菜单绘制。
        // 管理器只在同步激活时借用，不保存到后续帧。
        if ( ImGui::BeginPopup("SelectionEditMenu") ) {
            // 异步切换会话后 ImGui 弹窗仍可能存在，需要主动终止旧上下文。
            // 请求关闭与禁用同时执行，确保本帧后续菜单项不能被激活。
            const bool sameBeatmap =
                ctx.beatmapKey == currentSnapshot->beatmapPathKey &&
                ctx.beatmapInstance == currentSnapshot->beatmapInstanceId;
            if ( !selectionMenu || !sameBeatmap ) ImGui::CloseCurrentPopup();
            ImGui::BeginDisabled(!selectionMenu || !sameBeatmap ||
                                 currentSnapshot->isPlaying);
            renderLayers(canvas, *currentSnapshot);
            // 模式只影响下一笔，活动框在起笔命令中锁存自己的集合操作。
            // 单选菜单使追加与减选互斥，不借用全局 Ctrl 或 Shift 状态。
            // 勾选项表示下一笔模式，不重解释已有框选的结果。
            auto& profile = UI::Interaction::activeProfile();
            for ( const auto& [operation, key] :
                  { std::pair{ Logic::SelectionOperation::Replace,
                               "ui.selection.replace" },
                    std::pair{ Logic::SelectionOperation::Add,
                               "ui.selection.add" },
                    std::pair{ Logic::SelectionOperation::Subtract,
                               "ui.selection.subtract" } } ) {
                if ( ImGui::MenuItem(
                         TR(key).data(),
                         nullptr,
                         profile.selectionOperation() == operation) )
                    profile.setSelectionOperation(operation);
            }
            ImGui::Separator();
            // 历史操作指向当前编辑器，沿用原撤销栈；空历史由逻辑层忽略。
            // 菜单激活不携带快捷键来源，避免转入独立 BPM 工具的历史。
            if ( ImGui::MenuItem(TR("ui.edit.undo").data()) )
                UI::Interaction::executeAction("UndoAction", sourceManager);
            if ( ImGui::MenuItem(TR("ui.edit.redo").data()) )
                UI::Interaction::executeAction("RedoAction", sourceManager);
            ImGui::Separator();
            if ( ImGui::MenuItem(TR("ui.edit.cut").data()) )
                UI::Interaction::executeAction("CutAction", sourceManager);
            if ( ImGui::MenuItem(TR("ui.edit.copy").data()) )
                UI::Interaction::executeAction("CopyAction", sourceManager);
            if ( ImGui::MenuItem(TR("ui.edit.paste").data()) )
                UI::Interaction::executeAction(
                    "PasteAction",
                    sourceManager,
                    UI::MainMenuItemActivation{
                        .m_source =
                            UI::MainMenuActivationSource::InteractionProfile,
                        .m_pasteTime = ctx.pasteTime });
            // 镜像粘贴与普通粘贴共享右键打开时的吸附时间，不能回退播放头。
            // 锚点作为本次激活载荷传递，不改变快捷键和顶栏菜单的定位偏好。
            // 实际轨道镜像仍由既有动作处理器和命令层完成，菜单不转换数据。
            if ( UI::FeedbackMenuItem(TR("ui.edit.mirror_paste").data()) )
                UI::Interaction::executeAction(
                    "MirrorPasteAction",
                    sourceManager,
                    UI::MainMenuItemActivation{
                        .m_source =
                            UI::MainMenuActivationSource::InteractionProfile,
                        .m_pasteTime = ctx.pasteTime });
            // 原地镜像只作用于当前选区，保持既有权限、撤销与混合领域规则。
            if ( UI::FeedbackMenuItem(TR("ui.edit.mirror").data()) )
                UI::Interaction::executeAction("MirrorAction", sourceManager);
            ImGui::Separator();
            // 删除与 Delete
            // 快捷键发布同一命令，权限和撤销仍由会话统一管理。
            // 只在菜单项激活时发布一次，不读取或直接修改可变选择集合。
            if ( ImGui::MenuItem(TR("ui.common.delete").data()) )
                UI::Interaction::dispatchCommand(
                    Event::LogicCommandEvent(Logic::CmdDeleteSelected{}));
            // 全选沿用 Ctrl+A 的当前轨道区范围，使用右键前保存的画布落点。
            // 菜单中的移动不会把玩家区、草稿区或 BGM 区的选择范围串用。
            if ( ImGui::MenuItem(TR("ui.edit.select_all").data()) )
                UI::Interaction::executeAction("SelectAllAction",
                                               sourceManager);
            // 清空选择是非破坏性状态操作，不使用删除物件命令。
            if ( ImGui::MenuItem(TR("ui.selection.clear").data()) )
                UI::Interaction::dispatchCommand(Event::LogicCommandEvent(
                    Logic::CmdSelectEntity{ entt::null, true }));
            // 与经典
            // Ctrl+右键复用同相机命中规则，仅移除右键起点下的一个框。
            if ( ImGui::MenuItem(TR("ui.selection.remove_marquee").data()) )
                UI::Interaction::dispatchCommand(
                    Event::LogicCommandEvent(Logic::CmdRemoveMarqueeAt{
                        ctx.cameraId, ctx.menuPoint.x, ctx.menuPoint.y }));
            // 扩展入口沿用当前谱面校验和播放禁用作用域，不另起输入状态机。
            renderExtras(canvas, sourceManager);
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        // 即使菜单本帧关闭，也不补跑画布逻辑，防止释放点击穿透。
        ImGui::PopID();
    }
};
}  // namespace
/// @brief 桌面目标只创建桌面策略；安卓工厂位于另一个编译单元。
std::unique_ptr<CanvasInteractionPresentation>
createCanvasInteractionPresentation()
{
    return std::make_unique<DesktopCanvasInteractionPresentation>();
}
}  // namespace MMM::Canvas
