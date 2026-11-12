#pragma once
#include "MobileCanvasNavigation.h"
#include "canvas/PlanarObservationInput.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "event/logic/LogicCommandEvent.h"
#include "logic/EditorEngine.h"
#include "ui/interaction/EditorInteraction.h"
#include <glm/vec2.hpp>
#include <imgui.h>
#include <optional>
#include <string>

namespace MMM::Canvas
{
/// @brief 移动画布与时间线共用多指命令路由，设备布局不进入公共控制器头。
/// @param navigation 只归本视图的双指分类器，不在视图间共享代际。
/// @param navigationZoom
/// 同一手势的即时倍率，不读取滞后的逻辑回流作为下一步基线。
/// @details 主画布可提供三维逆投影，时间线保持二维且忽略横向平移。
/// 两者都先更新本地反馈，再发布连续定位；收尾只发布一次最终缩放与结束命令。
/// @warning 每帧处理固定数量触点；不得等待会话、复制谱面或读取文件。
inline void routeMobileNavigation(
    MobileCanvasNavigation& navigation, float& navigationZoom,
    const Graphic::MobileTouchSnapshot& raw, bool allowed,
    const std::string& camera, const UI::Interaction::SurfaceFrame& frame,
    const Common::Render::RenderSnapshot& snapshot, float width, float height)
{
    // 输入位置是逻辑像素，主画布可选逆投影来自同一帧的固定矩阵。
    // Timeline 不提供三维矩阵，直接采用纵向逻辑像素作为时间轴位移。
    // 原始多指分类器只给增量，不访问会话或读取可变的 ScrollCache。
    // 投影失败时不发布替代落点，避免三维地平线附近突然跳转。
    // 连续命令携带当前倍率，逻辑层同时求解缩放锚点与平移。
    // UI 倍率只在手势结束时写入配置，过程反馈不等待配置回流。
    // 命令以稳定相机名路由，不因为当前焦点变化而切换目标。
    // 历史轻触与导航互斥，出现位移后不再当作 Undo/Redo。
    // 系统取消不会制造历史轻触，但已显示的导航位置可以正常收尾。
    auto&      app     = Config::AppConfig::instance();
    auto&      zoom    = app.getEditorConfig().visual.timelineZoom;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto delta   = navigation.update(raw,
                                           allowed,
                                           display.x,
                                           display.y,
                                           zoom,
                                           10.0f * app.getWindowContentScale());
    if ( delta.m_changed ) {
        // 与普通拾取共用屏幕到谱面平面的逆投影，三维视角不另猜移动比例。
        const glm::vec2 oldScreen{ delta.m_anchorX - frame.m_x,
                                   delta.m_anchorY - frame.m_y };
        const glm::vec2 newScreen = oldScreen + glm::vec2(delta.m_x, delta.m_y);
        const auto      oldPoint =
            frame.m_projection
                ? planarObservationUnproject(*frame.m_projection, oldScreen)
                : std::optional<glm::vec2>(oldScreen);
        const auto newPoint =
            frame.m_projection
                ? planarObservationUnproject(*frame.m_projection, newScreen)
                : std::optional<glm::vec2>(newScreen);
        if ( oldPoint && newPoint ) {
            // 两个点必须来自同一矩阵，分别在不同帧求交会带入摄像机变化。
            // 旧点提供缩放锚点，新旧差值提供与渲染一致的平移方向。
            // 一条命令同时求解平移与缩放锚点，避免两条异步命令相互覆盖。
            // 矩阵无有效交点时不钳制到边缘，也不提交尚未显示过的倍率。
            navigationZoom = delta.m_zoom;
            UI::Interaction::dispatchCommand(Event::LogicCommandEvent(
                Logic::CmdPanCanvas{ .cameraId      = camera,
                                     .deltaX        = newPoint->x - oldPoint->x,
                                     .deltaY        = newPoint->y - oldPoint->y,
                                     .viewportWidth = width,
                                     .viewportHeight = height,
                                     .renderScaleY   = snapshot.renderScaleY,
                                     .timelineZoom   = delta.m_zoom,
                                     .anchorY        = oldPoint->y }));
        }
    }
    if ( delta.m_finished ) {
        // 收尾后保留旧手势的单指屏蔽门闩；导航结束不等于重新允许编辑。
        // 下一笔编辑必须由新的原始触摸代际开始。
        // UI
        // 值立即显示最终倍率；仅发布一个缩放字段，不广播整份设置或等待会话。
        // 逻辑侧先保留当前直接显示值，下一轮配置可见后再无缝移除局部覆盖。
        // 一次手势只有一次收尾；第二指先抬起时就提交，不等待残留指针。
        // 外部取消也保留已完成的视图变换，因为它没有修改谱面物件。
        zoom = navigationZoom;
        Logic::EditorEngine::instance().updateTimelineZoom(zoom);
        UI::Interaction::dispatchCommand(
            Event::LogicCommandEvent(Logic::CmdPanCanvas{
                .cameraId = camera, .finishNavigation = true }));
    }
    // 历史轻触只有全部抬起后才发动作；已移动或取消的手势不会进入这里。
    // 历史目标显式带画布身份，关闭目标后丢弃而不回退其他标签。
    // 短触分类与动作执行分开，权限、撤销和联机仍由原业务入口处理。
    if ( delta.m_undo )
        UI::Interaction::dispatchCommand(
            Event::LogicCommandEvent(Logic::CmdUndo{ .cameraId = camera }));
    if ( delta.m_redo )
        UI::Interaction::dispatchCommand(
            Event::LogicCommandEvent(Logic::CmdRedo{ camera }));
}

}  // namespace MMM::Canvas
