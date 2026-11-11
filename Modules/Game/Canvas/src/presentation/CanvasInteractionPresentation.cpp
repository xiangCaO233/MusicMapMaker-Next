#include "presentation/CanvasInteractionPresentation.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/interaction/EditorInteraction.h"
#include <algorithm>

namespace MMM::Canvas
{
/// @brief 借用引用保持菜单与原控制器共用同一份状态，避免复制或跨帧悬空。
CanvasMenuContext CanvasInteractionPresentation::context(
    Basic2DCanvasInteraction& c)
{
    // 引用按 DTO 声明顺序绑定，窗口身份和实例身份不能混用。
    // 该集合不得跨出当前 UI 更新栈，也不能传递给后台任务。
    // 返回值为栈上引用 DTO，不承担任何资源生命周期。
    // 候选仍借用控制器原容器，不在桥中另建一份状态。
    return { c.m_cameraId,
             c.m_selectionPasteTime,
             c.m_selectionMenuPoint,
             c.m_lastMouseCommand.pos,
             c.m_selectionMenuBeatmapKey,
             c.m_selectionMenuBeatmapInstance,
             c.m_menuHoverLayers,
             c.m_currentHoverCandidates };
}
/// @brief 不改变业务路径，平台只控制此次调用看到的输入快照。
/// @warning 每帧固定转发，禁止在桥中等待逻辑线程或复制谱面。
void CanvasInteractionPresentation::routeInput(
    Basic2DCanvasInteraction& c, const Common::Render::RenderSnapshot* snapshot,
    float width, float height, bool cancellable)
{
    // 快捷键先于普通指针，保留已有命令顺序和去重策略。
    const bool previous            = c.m_pointerCancellationEnabled;
    c.m_pointerCancellationEnabled = cancellable;
    c.handleHotkeys(snapshot);
    c.handleInteractions(snapshot, width, height);
    c.m_pointerCancellationEnabled = previous;
}
/// @brief 输入归属改变时明确取消预览，随后清除旧释放事件的 UI 门闩。
/// @warning 只在手势接管边沿执行，无会话等待、注册表读取或历史回放。
void CanvasInteractionPresentation::cancelInput(
    Basic2DCanvasInteraction& c, const Common::Render::RenderSnapshot& snapshot)
{
    // 此入口仅终止尚未提交的指针事务，不关闭画布或切换当前工具。
    // 单次接管只发一条对应工具的取消，重复触摸帧由平台门闩拦截。
    // 命令按原相机 ID 路由，多个画布不能取消彼此的编辑。
    // 框选检查点由开始命令显式启用，旧桌面调用保持原来成本。
    // 移动对象的初始组件由工具持有，UI 不读取可变 ECS。
    // 撤销栈保持完整；不能用一次普通 Undo 模拟取消预览。
    // 对象拖动与画笔互斥，优先级与原控制器释放路径一致。
    // 取消尚未起笔的长按候选时，没有本地门闩，因此不发编辑命令。
    // 通过原事件队列保持开始、更新、取消顺序，不读取滞后的渲染标志。
    // 接管不会把第二根手指的坐标当作最终放置坐标。
    // 旧释放帧仍由平台屏蔽，清除本地来源后不应产生清空选择命令。
    // 清理连续命令基线，下一笔相同位置也必须能正常发布。
    using UI::Interaction::dispatchCommand;
    if ( c.m_leftPressStartedObjectDrag ) {
        // 对象分支优先，不能根据当前工具把对象拖动误当作空白框选。
        dispatchCommand(
            Event::LogicCommandEvent(Logic::CmdEndDrag{ c.m_cameraId, true }));
    } else if ( c.m_leftPressStartedOnCanvas ) {
        if ( snapshot.currentTool == Logic::EditTool::Marquee ) {
            dispatchCommand(
                Event::LogicCommandEvent(Logic::CmdEndMarquee{ true }));
        } else if ( snapshot.currentTool == Logic::EditTool::Draw ) {
            // 折线续写取消还要恢复被暂时替换的原物件，不能只丢弃新预览。
            dispatchCommand(Event::LogicCommandEvent(Logic::CmdEndBrush{
                .cameraId                      = c.m_cameraId,
                .cancel                        = true,
                .createStandalone              = c.m_standaloneBrush,
                .walkthroughToken              = c.m_brushWalkthroughToken,
                .walkthroughMinimumSubNotes    = c.m_brushMinimumSubNotes,
                .restoreReplacedObjectOnCancel = true }));
        }
    }
    // 释放旧手指不会再提交原编辑，也不能清空手势前的选择。
    c.m_leftPressStartedOnCanvas        = false;
    c.m_leftPressStartedInTrackLayout   = false;
    c.m_leftPressStartedOnEntity        = false;
    c.m_leftPressStartedObjectDrag      = false;
    c.m_leftPressDragged                = false;
    c.m_cancelBrushOnNextRelease        = false;
    c.m_mouseBrushCancelAwaitingRelease = false;
    c.m_isMiddleCanvasPanning           = false;
    c.m_colorStrokeEntities.clear();
    c.resetContinuousEditCommands();
}
/// @brief 重叠候选仍使用通用身份验证，平台菜单不重新拾取后台 ECS。
void CanvasInteractionPresentation::renderLayers(
    Basic2DCanvasInteraction& c, const Common::Render::RenderSnapshot& snapshot)
{
    // 候选验证仍检查当前谱面，不能仅凭弹层缓存判断实体是否仍存在。
    // 显示入口不读取 ECS，选择动作最终通过逻辑命令发布。
    c.renderHoverLayerMenu(snapshot);
}
/// @brief 桌面属性入口保留原有所有权，动作注册表不因平台切换重复创建。
void CanvasInteractionPresentation::renderExtras(Basic2DCanvasInteraction& c,
                                                 UI::UIManager* manager)
{
    // 桌面旧扩展项保留其固定窗口 ID 和管理器所有权。
    // 移动端提供自己的按钮网格，不调用这个桌面布局桥。
    c.renderSelectionMenuExtras(manager);
}
/// @brief 空选区批注复用原确认窗口，只有保存时才建立编辑事务。
void CanvasInteractionPresentation::requestAnnotation(
    Basic2DCanvasInteraction& c, double time)
{
    // 清除旧条目的身份及正文，防止把新建请求误当成覆盖旧批注。
    // 新建正文使用既有固定缓冲，不额外分配一套移动专属编辑对象。
    // 时间戳夹紧由原业务语义决定，不改动当前播放头或谱面时间。
    // 弹层确认前只设置请求标志，取消不会产生空批注。
    c.m_annotationEditor.annotationId.clear();
    c.m_annotationEditor.author.clear();
    c.m_annotationEditor.timestamp = std::max(0.0, time);
    c.m_annotationEditor.content.fill('\0');
    c.m_annotationEditor.requestOpen = true;
}
}  // namespace MMM::Canvas
