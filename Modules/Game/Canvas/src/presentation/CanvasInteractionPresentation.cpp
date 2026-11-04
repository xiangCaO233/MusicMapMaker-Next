#include "presentation/CanvasInteractionPresentation.h"
#include "canvas/Basic2DCanvasInteraction.h"
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
    float width, float height)
{
    // 快捷键先于普通指针，保留已有命令顺序和去重策略。
    c.handleHotkeys(snapshot);
    c.handleInteractions(snapshot, width, height);
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
