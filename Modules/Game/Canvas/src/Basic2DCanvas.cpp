/// @file Basic2DCanvas.cpp
/// @brief 实现主谱面画布窗口、会话生命周期和协作视野覆盖层。
///
/// 本文件负责 ImGui 窗口与 EditorEngine 会话之间的编排，
/// 具体鼠标工具行为位于 Basic2DCanvasInteraction，
/// Vulkan 资源重载与命令录制位于 Basic2DCanvas_Rendering.cpp。
/// 主线程只消费并行阶段准备好的不可变 RenderSnapshot，
/// 不在窗口更新中直接遍历逻辑 ECS 或等待逻辑线程。
///
/// 多谱面窗口通过 cameraId 绑定到稳定会话，
/// 可见标题可以随谱名、脏状态和协作状态变化，
/// 但 ### 后的内部窗口 ID 始终使用稳定 canvasName。
/// 最后一个会话关闭时会重置为 Logo 占位页而不是销毁窗口，
/// 以维持编辑器中心停靠结构和欢迎页返回入口。
///
/// 创作教程的临时目标也由本视图负责，原因是目标必须同时依赖：
/// - 当前谱面的真实玩家轨道投影；
/// - 当前相机经过中键横移后的水平偏移；
/// - BPM Timing 与当前分拍数构成的吸附网格；
/// - Scroll、Jump 和 HS 共同决定的时间到屏幕坐标映射；
/// - ImGui 本帧鼠标边沿与逻辑线程上一代画笔快照。
/// 目标只存在于教学 UI，不写入谱面模型，也不进入离屏 Vulkan 顶点。
/// 合法手势仍沿用正常 CmdStartBrush/UpdateBrush/EndBrush 命令链，
/// 失败手势则在结束命令上携带取消标记，由逻辑线程原子丢弃临时画笔。
/// 这种分工避免 UI 直接创建 Note，也避免用普通 Undo 误伤更早的编辑历史。
#include "canvas/Basic2DCanvas.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/CanvasTabTitle.h"
#include "canvas/CollaborationPeerColor.h"
#include "canvas/CollaborationViewportProjection.h"
#include "canvas/ComposeHoldTarget.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/CanvasCamera.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include "ui/walkthrough/WelcomeView.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fmt/format.h>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

namespace MMM::Canvas
{
namespace
{
/// @brief 上报真实谱面编辑器的 Dock 标签，并在标签切到前台后完成点击目标。
/// @param manager 提供全局演练高亮状态机。
/// @param window 当前谱面编辑器 ImGui 窗口。
/// @param isBeatmap 当前窗口是否对应真实谱面而非 Logo 占位。
/// @warning UI 热路径：每帧最多遍历当前 Dock 节点的少量标签。
void reportBeatmapTabWalkthroughTarget(UI::UIManager* manager,
                                       ImGuiWindow* window, bool isBeatmap)
{
    // 标签锚点协议：
    // - Logo 会话永远不能满足“已打开谱面”的教学前提；
    // - 浮动窗口没有 Dock 标签，因此等待用户先恢复可点击的标签结构；
    // - 目标矩形必须从 TabBar 的滚动后坐标计算，不能借用窗口标题栏；
    // - 只有鼠标实际点击标签正文时才完成，标签当前可见不等同于用户选择；
    // - 同一语义 ID 可由多张谱面上报，Spotlight 合并为共同可选高亮范围。
    if ( !manager || !isBeatmap || !window || !window->DockNode ||
         !window->DockNode->TabBar ) {
        return;
    }
    auto* tabBar = window->DockNode->TabBar;
    for ( const auto& tab : tabBar->Tabs ) {
        if ( tab.Window != window && tab.ID != window->TabId ) continue;
        const bool central = (tab.Flags & (ImGuiTabItemFlags_Leading |
                                           ImGuiTabItemFlags_Trailing)) == 0;
        // 中央标签会被 TabBar 横向滚动，Leading/Trailing 固定项则不应用滚动量。
        const float x =
            tabBar->BarRect.Min.x +
            (central ? std::trunc(tab.Offset - tabBar->ScrollingAnim)
                     : tab.Offset);
        const float height = GImGui->FontSize + tabBar->FramePadding.y * 2.0F;
        // 两端裁剪防止滚动到栏外的标签产生跨越其它 Dock 区域的高亮孔洞。
        const ImVec2 minimum{ std::max(x, tabBar->BarRect.Min.x),
                              tabBar->BarRect.Min.y };
        const ImVec2 maximum{ std::min(x + tab.Width, tabBar->BarRect.Max.x),
                              std::min(tabBar->BarRect.Min.y + height,
                                       tabBar->BarRect.Max.y) };
        manager->walkthroughSpotlight().reportTarget(
            "editor.beatmap-tab", minimum, maximum, window->Viewport);
        const ImVec2 mouse = ImGui::GetMousePos();
        // 关闭按钮占据标签最右侧一个字高；点击该区只关闭，不视为选择谱面。
        // 这里读取按下边沿而非当前选中状态，确保用户作出一次明确选择。
        // 后台谱面同样执行 Begin 并上报矩形，因此无需预先固定活动会话。
        // 点击范围排除关闭区，避免关闭某张谱面时误推进介绍步骤。
        const float closeLeft =
            maximum.x - tabBar->FramePadding.x - GImGui->FontSize;
        const bool inCloseButton = window->HasCloseButton &&
                                   mouse.x >= closeLeft &&
                                   mouse.y >= minimum.y && mouse.y < maximum.y;
        const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                             mouse.x >= minimum.x && mouse.x < maximum.x &&
                             mouse.y >= minimum.y && mouse.y < maximum.y &&
                             !inCloseButton;
        if ( clicked )
            manager->walkthroughSpotlight().completeTarget(
                "editor.beatmap-tab");
        return;
    }
}

/// @brief 上报主画布各语义区域，并按完整可见性完成横向拖动目标。
/// @param manager 提供演练高亮状态机。
/// @param snapshot 当前真实谱面的渲染快照。
/// @param canvasPosition 画布内容左上角屏幕坐标。
/// @param canvasSize 画布内容逻辑尺寸。
/// @details 阶段三与阶段四使用不同语义 ID，但共用同一份实际轨道投影。
/// 这样阶段三曾经完成的介绍不会跳过阶段四重新要求的主轨道复位动作。
/// 拖动步骤突出整个画布，介绍步骤只突出目标区域；两者不能交换矩形，
/// 否则用户可能在玩家区仍有一部分离屏时提前通过完整可见性检查。
/// @warning UI 热路径：只执行一次轨道投影和固定数量的矩形裁剪。
void reportCanvasWalkthroughTargets(
    UI::UIManager* manager, const Common::Render::RenderSnapshot& snapshot,
    const ImVec2& canvasPosition, const ImVec2& canvasSize)
{
    // 区域教学必须复用实际渲染使用的快照字段：
    // - trackCount 决定玩家区和默认辅助轨宽；
    // - canvasHorizontalOffsetX 是中键横移后的唯一完成判定来源；
    // - professionalMode 通过 draftLanesEnabled 决定草稿区是否真实存在；
    // - bmsEditingEnabled 决定 BGM 区是否能够显示和接收编辑；
    // - 运行时追加轨也属于用户需要完整看到的可操作区域。
    // 这里不读取逻辑会话或输入手势，避免 UI 线程与逻辑线程产生第二套状态。
    if ( !manager || !snapshot.hasBeatmap || canvasSize.x <= 1.0F ||
         canvasSize.y <= 1.0F ) {
        return;
    }
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    const auto& layout = visual.trackLayoutForKeyCount(snapshot.trackCount);
    const auto  projection =
        Logic::calculateCanvasLaneProjection(canvasSize.x,
                                             snapshot.trackCount,
                                             snapshot.bgmTrackCount,
                                             layout,
                                             snapshot.canvasHorizontalOffsetX,
                                             true,
                                             snapshot.bmsEditingEnabled,
                                             snapshot.draftLanesEnabled,
                                             snapshot.draftTrackCount,
                                             true);
    if ( !projection.valid ) return;

    auto& spotlight = manager->walkthroughSpotlight();
    // 介绍步骤允许目标部分位于屏幕外，但高亮只能覆盖本帧实际可见交集。
    // 这与拖动步骤的“完整可见”判定刻意不同：前者负责解释，后者负责验收。
    const auto reportRegion =
        [&](std::string_view id, float left, float right) {
            const float clippedLeft  = std::clamp(left, 0.0F, canvasSize.x);
            const float clippedRight = std::clamp(right, 0.0F, canvasSize.x);
            // 完全离屏或退化区域保持等待，不能绘制宽度为零的遮罩孔洞。
            if ( clippedRight <= clippedLeft ) return;
            spotlight.reportTarget(
                id,
                { canvasPosition.x + clippedLeft, canvasPosition.y },
                { canvasPosition.x + clippedRight,
                  canvasPosition.y + canvasSize.y },
                ImGui::GetWindowViewport());
        };
    const auto fullyVisible = [&](float left, float right) {
        // 半像素容差吸收投影与 ImGui 逻辑像素换算误差，不放宽一整条轨道。
        constexpr float EPSILON = 0.5F;
        return right > left && left >= -EPSILON &&
               right <= canvasSize.x + EPSILON;
    };

    // 拖动画布阶段突出整个交互面，具体区域完整进入视口后由业务状态完成。
    // 三个目标可以每帧同时上报；Spotlight 只会接纳当前步骤声明的那个 ID。
    // 因而此处无需查询当前步骤，也不会让某一区域完成状态误推进下一步。
    spotlight.reportTarget(
        "editor.canvas.pan-draft",
        canvasPosition,
        { canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y },
        ImGui::GetWindowViewport());
    spotlight.reportTarget(
        "editor.canvas.pan-annotation",
        canvasPosition,
        { canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y },
        ImGui::GetWindowViewport());
    spotlight.reportTarget(
        "editor.canvas.pan-bgm",
        canvasPosition,
        { canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y },
        ImGui::GetWindowViewport());
    spotlight.reportTarget(
        "compose.canvas.pan-player",
        canvasPosition,
        { canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y },
        ImGui::GetWindowViewport());
    if ( projection.draftLaneCount > 0 &&
         fullyVisible(projection.draftLeftX, projection.draftRightX) ) {
        // 关闭专业模式时计数为零，路线继续等待并由提示说明如何开启。
        spotlight.completeTarget("editor.canvas.pan-draft");
    }
    if ( fullyVisible(projection.annotationLeftX,
                      projection.annotationRightX) ) {
        // 批注区不依赖 BGM 开关，按自身完整边界单独验收横移阶段。
        // 左右边界都进入画布才算完成，不能只凭批注中心或局部露出推进。
        // 该区域始终存在，因此不使用轨道数量作为额外启用条件。
        // 独立目标保证批注完成后仍需继续横移并单独验收 BGM 区。
        spotlight.completeTarget("editor.canvas.pan-annotation");
    }
    if ( projection.bgmLaneCount > 0 &&
         fullyVisible(projection.bgmLeftX, projection.bgmRightX) ) {
        // BMS 关闭或没有可访问轨道时不能把空区间误判成已完整显示。
        spotlight.completeTarget("editor.canvas.pan-bgm");
    }
    if ( fullyVisible(projection.player.leftX, projection.player.rightX) ) {
        // 创作阶段重新验收玩家区，不复用阶段三已经完成过的进度状态。
        spotlight.completeTarget("compose.canvas.pan-player");
    }

    // 玩家区始终是主介绍锚点，辅助区域则只在对应功能实际启用时上报。
    // 区域上报顺序不代表路线顺序，JSON 步骤才是唯一编排来源。
    // Spotlight 按语义 ID 过滤无关矩形，未进入本主题时这些调用保持无操作。
    // 所有矩形共享画布纵向范围，使说明不会误导为只作用于某个时间片段。
    // player 使用实际布局边界，不能假定画布中央或固定四轨宽度。
    reportRegion("editor.canvas.player",
                 projection.player.leftX,
                 projection.player.rightX);
    reportRegion("compose.canvas.player",
                 projection.player.leftX,
                 projection.player.rightX);
    if ( projection.draftLaneCount > 0 )
        // 草稿区按自身右锚点和独立轨宽计算，不能从玩家区简单向左镜像。
        reportRegion("editor.canvas.draft",
                     projection.draftLeftX,
                     projection.draftRightX);
    reportRegion("editor.canvas.annotation",
                 projection.annotationLeftX,
                 projection.annotationRightX);
    // 批注沟槽不依赖 BGM 轨数，保持为独立步骤以免被 BGM 大区域吞没。
    // 它可能在 BGM 被关闭时仍然存在，因此不放进 BGM 条件分支。
    if ( projection.bgmLaneCount > 0 )
        // BGM 采用自身单轨宽度，持久轨和运行时追加轨共同组成介绍范围。
        reportRegion(
            "editor.canvas.bgm", projection.bgmLeftX, projection.bgmRightX);
}

/// @brief 判断鼠标是否悬停在当前 ImGui 窗口的内容区域内。
/// @return 鼠标位于当前窗口内容区域时返回 true。
/// @details 窗口级 hover 包含标题栏、标签和装饰区域，
/// 因而还需用当前内容起点与可用尺寸进行一次矩形命中。
/// AllowWhenBlockedByActiveItem 允许滚轮在其它控件保持 active 时
/// 仍切换到鼠标所在谱面，但鼠标按钮按下状态由调用方另行排除。
/// @warning UI 热路径：主画布每帧更新时调用，只读取当前 ImGui
/// 窗口几何与鼠标位置。
bool isMouseHoveringCurrentWindowContent()
{
    // 先利用 ImGui 的窗口层级和遮挡判断，避免仅凭几何坐标把
    // 被其它浮窗覆盖的后台画布误认为鼠标目标。
    if ( !ImGui::IsWindowHovered(
             ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ) {
        return false;
    }

    const ImVec2 mousePos    = ImGui::GetMousePos();
    const ImVec2 contentPos  = ImGui::GetCursorScreenPos();
    const ImVec2 contentSize = ImGui::GetContentRegionAvail();
    // 折叠、刚创建或过窄的窗口没有可靠内容矩形，直接拒绝命中。
    if ( contentSize.x <= 0.0f || contentSize.y <= 0.0f ) {
        return false;
    }

    // 使用闭区间覆盖内容边缘，滚轮恰好位于纹理边界时仍归属画布。
    return mousePos.x >= contentPos.x &&
           mousePos.x <= contentPos.x + contentSize.x &&
           mousePos.y >= contentPos.y &&
           mousePos.y <= contentPos.y + contentSize.y;
}

/// @brief 判断当前主画布内容区是否正在接收滚轮操作。
/// @return 未按住鼠标键且滚轮发生在当前窗口内容区时返回 true。
/// @details 鼠标按钮按下期间的滚轮可能属于拖动工具修饰，
/// 不应用于跨谱面切换活动会话；0.01 阈值过滤触控板残余抖动。
/// @warning UI 热路径：主画布每帧更新时调用；只读取 ImGui 输入状态和窗口几何。
bool isWheelOverCurrentWindowContent()
{
    const auto& io = ImGui::GetIO();
    return std::abs(io.MouseWheel) > 0.01f && !ImGui::IsAnyMouseDown() &&
           isMouseHoveringCurrentWindowContent();
}

/// @brief 判断当前主画布 ImGui 窗口本帧是否真实可见。
/// @return 窗口内容区域可见且不是隐藏 Dock Tab 时返回 true。
/// @details WasActive、Hidden、Collapsed 与 SkipItems 覆盖普通窗口退化状态，
/// DockTabIsVisible 进一步排除同一停靠节点中被其它标签遮住的画布。
/// 最终尺寸检查避免为尚未完成布局的零面积窗口准备离屏快照。
/// @warning UI 热路径：主画布每帧更新时调用；只读取当前 ImGuiWindow 状态。
bool isCurrentCanvasWindowVisible()
{
    // GetCurrentWindow 只在 LayoutContext 已经 Begin 当前画布后调用；
    // 空指针保护仍用于应对窗口创建被上层提前跳过的帧。
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if ( !window || !window->WasActive || window->Hidden || window->Collapsed ||
         window->SkipItems ) {
        return false;
    }
    if ( window->DockIsActive && !window->DockTabIsVisible ) {
        // 停靠节点仍 active 不代表当前标签可见，后台标签必须停止
        // 离屏录制和完整交互，避免多谱面时浪费渲染工作。
        return false;
    }

    const ImVec2 contentSize = ImGui::GetContentRegionAvail();
    return contentSize.x > 1.0f && contentSize.y > 1.0f;
}

/// @brief 从渲染快照估算指定视觉时间对应的绝对 Y。
/// @param snapshot 当前主画布快照。
/// @param time 远端视口边界的视觉时间。
/// @return 与当前滚动分段一致的绝对 Y。
/// @details scrollSegments 按时间递增，upper_bound 找到目标后的首段；
/// 实际使用前一段作为当前速度区间，并从该段锚点线性外推。
/// 调用方保证列表非空，早于第一段的时间使用第一段向前外推，
/// 晚于末段的时间则自然使用末段向后外推。
/// @warning UI 热路径：每个远端用户最多调用两次，只执行一次二分查找。
double collaborationAbsYAtTime(const Common::Render::RenderSnapshot& snapshot,
                               double                                time)
{
    // 比较器采用 value < segment.time，与按时间升序的段表匹配；
    // 不能改成 lower_bound，否则恰好落在段起点时会选到前一速度段。
    const auto it = std::upper_bound(
        snapshot.scrollSegments.begin(),
        snapshot.scrollSegments.end(),
        time,
        [](double value, const Common::Render::ScrollSegment& segment) {
            return value < segment.time;
        });
    const auto& segment = it == snapshot.scrollSegments.begin()
                              ? snapshot.scrollSegments.front()
                              : *std::prev(it);
    // absY 是分段起点累计距离，局部时间差乘当前速度得到连续绝对坐标。
    return segment.absY + (time - segment.time) * segment.speed;
}

/// @brief 将协作视觉时间投影到当前主画布本地 Y 坐标。
/// @param snapshot 当前主画布快照。
/// @param time 待投影视觉时间。
/// @param canvasHeight 当前画布高度。
/// @return 以主画布左上角为原点的 Y 坐标。
/// @details 判定线是当前视觉时间的投影锚点。
/// 有滚动分段时，以目标与当前绝对 Y 差乘 renderScaleY 后向上投影；
/// 无滚动分段时回退到可见时间范围的线性投影 helper。
/// 非有限或近零 renderScaleY 回退为 1，避免协作覆盖层产生 NaN。
/// @warning UI 热路径：只执行常量数值计算和两次滚动分段二分查找。
float collaborationTimeToCanvasY(const Common::Render::RenderSnapshot& snapshot,
                                 double time, float canvasHeight)
{
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    // VisualConfig 同时定义判定线位置与轨道布局，两者必须取自同一
    // 配置快照，远端时间矩形才会与本地谱面轨道准确重合。
    // 轨道数至少按一轨查询配置，避免空快照访问不存在的零轨布局。
    const float judgmentLineY =
        canvasHeight * visual.judgmentLinePositionForKeyCount(
                           std::max(snapshot.trackCount, 1));
    if ( snapshot.scrollSegments.empty() ) {
        // 老快照或尚未建立 SV 分段时仍可使用可见时间上下界绘制
        // 协作视野；optional 失败则把标记收敛到判定线。
        return projectCollaborationViewportTime(time,
                                                snapshot.currentTime,
                                                snapshot.visibleTimeStart,
                                                snapshot.visibleTimeEnd,
                                                judgmentLineY,
                                                canvasHeight)
            .value_or(judgmentLineY);
    }
    const double currentAbsY =
        collaborationAbsYAtTime(snapshot, snapshot.currentTime);
    const double targetAbsY = collaborationAbsYAtTime(snapshot, time);
    // renderScaleY 属于快照同一代际；退化值只影响覆盖层显示，
    // 不应阻止主画布本身继续渲染。
    const double scale = std::isfinite(snapshot.renderScaleY) &&
                                 std::abs(snapshot.renderScaleY) > 1e-6F
                             ? static_cast<double>(snapshot.renderScaleY)
                             : 1.0;
    return judgmentLineY -
           static_cast<float>((targetAbsY - currentAbsY) * scale);
}

/// @brief 对教程随机种子执行一次无分配混合。
/// @param value 当前种子。
/// @return 可继续取模选择轨道或候选点的混合值。
/// @details 种子只决定一次教程会话的视觉练习路径，不参与谱面随机性。
/// 混合函数无全局引擎、无锁且不分配；进入步骤时加入帧号，使重复演练通常
/// 得到不同路线，同一轮则把结果保存在成员中，防止目标逐帧跳动。
std::uint64_t mixWalkthroughSeed(std::uint64_t value)
{
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

/// @brief 判断屏幕点是否位于闭区间矩形内。
/// @param point 屏幕坐标点。
/// @param minimum 矩形左上角。
/// @param maximum 矩形右下角。
/// @return 点位于矩形内时返回 true。
bool walkthroughPointInRect(const ImVec2& point, const ImVec2& minimum,
                            const ImVec2& maximum)
{
    return point.x >= minimum.x && point.x <= maximum.x &&
           point.y >= minimum.y && point.y <= maximum.y;
}

/// @brief 在目标框中心绘制不拦截输入的灯泡提示符。
/// @param drawList 当前画布窗口绘制列表。
/// @param center 图标中心。
/// @param radius 灯泡主体半径。
/// @param color 提示颜色。
/// @details 使用基础图元而非字体字形，保证自定义皮肤没有灯泡图标时仍可见。
/// 图元只进入当前窗口 DrawList，不创建 ImGui Item，因此不会抢走画布点击。
/// @warning UI 热路径：放置教学步骤每帧提交固定数量线段和圆形。
void drawWalkthroughLightBulb(ImDrawList* drawList, const ImVec2& center,
                              float radius, ImU32 color)
{
    if ( !drawList || radius <= 1.0F ) return;
    const ImVec2 bulbCenter{ center.x, center.y - radius * 0.22F };
    drawList->AddCircle(bulbCenter, radius * 0.58F, color, 20, 2.5F);
    drawList->AddLine({ center.x - radius * 0.34F, center.y + radius * 0.28F },
                      { center.x + radius * 0.34F, center.y + radius * 0.28F },
                      color,
                      2.5F);
    drawList->AddLine({ center.x - radius * 0.25F, center.y + radius * 0.48F },
                      { center.x + radius * 0.25F, center.y + radius * 0.48F },
                      color,
                      2.5F);
    // 六条短射线强化灯泡语义，不依赖皮肤字体是否包含对应图标字形。
    for ( int index = 0; index < 6; ++index ) {
        const float angle =
            -IM_PI + (static_cast<float>(index) + 0.5F) * IM_PI / 3.0F;
        const ImVec2 direction{ std::cos(angle), std::sin(angle) };
        const ImVec2 from{ bulbCenter.x + direction.x * radius * 0.78F,
                           bulbCenter.y + direction.y * radius * 0.78F };
        const ImVec2 to{ bulbCenter.x + direction.x * radius,
                         bulbCenter.y + direction.y * radius };
        drawList->AddLine(from, to, color, 2.0F);
    }
}

}  // namespace

/// @brief 创建绑定到逻辑会话的二维主画布。
/// @param name 稳定画布名，用于 ImGui 内部 ID 与皮肤配置。
/// @param w 初始离屏目标宽度。
/// @param h 初始离屏目标高度。
/// @param syncBuffer 逻辑线程发布渲染快照的共享缓冲。
/// @param cameraId 逻辑相机标识；为空时回退到 name。
/// @details 构造阶段创建独立交互控制器和背景视频播放器，
/// 但 Vulkan 纹理及 pipeline 仍由渲染器初始化路径延后建立。
/// 交互控制器只保存 cameraId，不取得 Canvas 所有权。
Basic2DCanvas::Basic2DCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Common::Render::RenderSnapshotBuffer> syncBuffer,
    const std::string&                                    cameraId)
    : IUIView(name)
    , IRenderableView(name)
    , m_canvasName(name)
    , m_cameraId(cameraId.empty() ? name : cameraId)
    , m_syncBuffer(std::move(syncBuffer))
{
    // 初始尺寸用于首个 RenderContext；后续尺寸变化通过事件同步给相机。
    m_targetWidth  = w;
    m_targetHeight = h;

    m_interaction =
        std::make_unique<Basic2DCanvasInteraction>(m_canvasName, m_cameraId);
    m_backgroundVideoPlayer = std::make_unique<BackgroundVideoPlayer>();
}

/// @brief 销毁主画布拥有的交互控制器和背景视频播放器。
/// @details unique_ptr 成员按声明逆序自动释放；GPU 资源清理由基类和
/// 渲染器生命周期负责，因此析构体无需显式等待设备空闲。
Basic2DCanvas::~Basic2DCanvas() {}

/// @brief 绘制单键与后续异轨长条的路径，并验证完整拖拽放置手势。
/// @param sourceManager 提供当前演练步骤和完成入口。
/// @param snapshot 当前玩家轨布局、分拍与画笔快照。
/// @param canvasScreenPosition 画布内容左上角屏幕坐标。
/// @param canvasSize 画布逻辑像素尺寸。
/// @details 整个步骤维持以下状态机：
///
/// 1. 仅当 Spotlight 正等待 place-note 或 place-hold 时生成目标；
/// 2. 直接读取逻辑渲染器本帧实际提交的玩家区分拍线；
/// 3. 丢弃无法完整容纳真实普通 Note 渲染矩形的候选；
/// 4. 为起点和终点选择不同候选，轨道数允许时也选择不同玩家轨；
/// 5. 用成员保存选择，直到步骤结束或用户切换到另一张谱面；
/// 6. 每帧重算屏幕矩形，使相机或窗口变化后仍跟随相同谱面时间；
/// 7. 蓝色框表示必须按下的位置，黄色灯泡框表示必须松开的位置；
/// 8. 左键在画布内按下即建立一次尝试，直接点击目标也会被记录为失败；
/// 9. 成功要求正确起点、真实拖动、逻辑画笔激活、正确修饰键及正确终点；
/// 10. 失败时要求交互控制器把同帧 EndBrush 改为 cancel，不产生历史节点。
///
/// UI 状态只验证教学手势，不替代 DrawTool 的领域校验、吸附和 Note 创建。
/// brushObserved 来自逻辑快照，确保被主音轨绑定等规则拒绝的起笔不能假完成。
/// 完成通知发生在正常 EndBrush 之前；命令仍按 Start、Update、End 顺序提交，
/// 因此最终 Note 使用释放帧的正常画笔更新，而不是由教程直接构造。
///
/// @par 坐标约束
/// 成员目标保存玩家轨号和渲染器公布的分拍时间，不保存窗口像素。每帧从
/// playerBeatLines 取回同一条真实拍线的中心 Y，并使用 playerNoteWidth 与
/// playerNoteHeight 构造和普通 Note 完全相同的外接矩形。UI 播放补间位移也
/// 同步加到中心 Y，避免逻辑快照发布后画面继续滚动而提示框停在旧位置。
/// X 只使用统一玩家轨投影，不会把草稿轨、批注沟槽或 BGM 轨误算为玩家轨。
/// 拍线集合只包含渲染器最终通过以下检查的普通玩家区网格：
/// - BPM 段与可见 Scroll 区间确实相交；
/// - 当前常显或近光标模式使该线拥有非零透明度；
/// - 该中心坐标没有被更早的拍线占用同一像素行；
/// - 中心落在玩家轨道纵向裁剪范围内。
/// 教程再以完整 Note 高度收窄候选，保证框不会越过轨道上下边缘。
/// 起点和终点保存精确谱面时间，以便快照滚动时查回新的屏幕 Y；
/// 如果某条线已经离开实际绘制集合，整条随机路线会重新选择，
/// 不能继续沿用旧坐标制造一个画面中不存在、也无法吸附的目标。
///
/// @par 输入约束
/// 教程只观察左键边沿，不主动捕获鼠标。真正的画笔输入继续由交互控制器
/// 处理，所以画布外释放、播放状态、对象 Hover 与连续命令去重规则都保持一致。
/// 一次尝试从画布内任意位置按下即锁存；起点不正确也必须等到释放后取消，
/// 防止错误按下已经激活的临时画笔残留到下一次尝试。
///
/// @par 失败恢复
/// 失败不发送普通 CmdUndo，因为起笔可能被逻辑规则拒绝而从未创建动作，
/// 此时 Undo 会错误撤销用户在教程前完成的编辑。cancel EndBrush 只清理当前
/// BrushState，既移除预览，也不会创建新动作或改变既有撤销栈。
///
/// @par 单键到长条的状态交接
/// 单键成功释放时复制逻辑画笔的实际轨号和时间，后续步骤只借用这个值。
/// 参考带有谱面实例身份，切换标签后不能把另一张谱面的拍位当作当前参考。
/// 步骤间 Spotlight 可能短暂进入等待态，因此参考与临时拖动目标分开保存。
/// 下一轮单键开始时清除参考，显式跳过单键也不会沿用上轮练习的旧物件。
/// 长条的两个端点位于同一条玩家轨，该轨必须不同于参考单键所在轨道。
/// 时间优先从参考同拍向后延伸；后方不可见时才取附近前驱到参考同拍。
/// 两个分支都按时间递增排列，从而遵守 DrawTool 的非负持续时间规则。
///
/// @par 长条手势验收
/// Shift 必须在按下左键之前生效，并保持到左键释放；Ctrl 始终不允许。
/// 任一采样帧违反修饰键规则都会锁存失败，稍后恢复按键也不会清除失败。
/// 指针横向必须始终在目标轨内，避免折线模式下跨轨再返回留下子段。
/// 释放帧既检查鼠标在终点框内，也检查最新逻辑画笔的类型、头尾时间与轨号。
/// 只有真实 HOLD 且持续时间大于零时才允许把本轮练习标为完成。
/// 目标因滚动、缩放或临时显隐而失效时，已开始的手势必须在释放时取消。
/// 目标缺失不能跳过取消清理，否则错误放置仍可能进入用户的谱面历史。
///
/// @par 不具备练习条件时的行为
/// 缺少参考单键、单轨谱面或原拍位不在视野中时，仅展示恢复条件提示。
/// 用户可以返回原拍位、重新演练，或显式确认跳过当前步骤。
/// 此时不能选择任意其他可见拍位来替代“刚才单键附近”的要求。
/// 目标缓存失效后仍使用原单键参考选择，不会累计时间偏移而逐轮远离它。
/// @warning UI 热路径：非目标步骤仅做状态判断；目标随机化只在进入步骤时执行。
void Basic2DCanvas::updateComposeWalkthrough(
    UI::UIManager*                        sourceManager,
    const Common::Render::RenderSnapshot& snapshot,
    const ImVec2& canvasScreenPosition, const ImVec2& canvasSize)
{
    if ( !sourceManager ) return;
    auto&      spotlight = sourceManager->walkthroughSpotlight();
    const bool placingHold =
        spotlight.awaitingTarget("compose.canvas.place-hold");
    const bool placingNote =
        spotlight.awaitingTarget("compose.canvas.place-note");
    if ( m_interaction )
        m_interaction->setWalkthroughPlacement(placingNote || placingHold);
    const std::string_view targetId =
        placingHold ? "compose.canvas.place-hold" : "compose.canvas.place-note";
    /// @brief 目标几何失效时仍消费失败释放，避免临时画笔漏提交。
    const auto cancelUnresolvedRelease = [&]() {
        if ( m_walkthroughNoteAttemptActive &&
             ImGui::IsMouseReleased(ImGuiMouseButton_Left) ) {
            if ( m_interaction ) m_interaction->cancelBrushOnNextRelease();
            m_walkthroughNoteAttemptActive = false;
        }
    };
    // 路线页面可能在鼠标手势中途被“知道了”或结束引导关闭。此时只清理
    // 教程自己的观察状态，不发送取消命令；显式跳过代表用户接管普通绘制。
    if ( (!placingNote && !placingHold) || !snapshot.hasBeatmap ||
         canvasSize.x <= 1.0F || canvasSize.y <= 1.0F ) {
        // 离开放置步骤后清除整段易失状态，下次重练会生成新的随机路径。
        m_walkthroughNoteDragTarget.reset();
        m_walkthroughNoteAttemptActive   = false;
        m_walkthroughNoteStartedAtSource = false;
        m_walkthroughNoteDragged         = false;
        m_walkthroughNoteBrushObserved   = false;
        m_walkthroughNoteModifierUsed    = false;
        return;
    }

    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    const auto& layout = visual.trackLayoutForKeyCount(snapshot.trackCount);
    // 使用与 DrawTool 相同的统一分区投影，不能按窗口中央猜测玩家轨道。
    // 草稿、批注和 BGM 的启用状态都会改变辅助区域，但玩家轨边界仍由
    // player 子投影权威给出，水平偏移则来自当前相机快照。
    const auto projection =
        Logic::calculateCanvasLaneProjection(canvasSize.x,
                                             snapshot.trackCount,
                                             snapshot.bgmTrackCount,
                                             layout,
                                             snapshot.canvasHorizontalOffsetX,
                                             true,
                                             snapshot.bmsEditingEnabled,
                                             snapshot.draftLanesEnabled,
                                             snapshot.draftTrackCount,
                                             true);
    if ( !projection.valid || snapshot.trackCount <= 0 ) {
        cancelUnresolvedRelease();
        return;
    }

    // 步骤切换必须丢弃单键的跨轨路径，但保留其成功落点作为长条参考。
    // 新一轮单键练习清除旧参考，不能把历史教程的落点带入这一轮。
    if ( m_walkthroughNoteDragTarget &&
         m_walkthroughNoteDragTarget->isHold != placingHold ) {
        m_walkthroughNoteDragTarget.reset();
    }
    if ( placingNote && !m_walkthroughNoteDragTarget )
        m_walkthroughPlacedNote.reset();

    const float noteWidth  = snapshot.playerNoteWidth;
    const float noteHeight = snapshot.playerNoteHeight;
    if ( noteWidth <= 1.0F || noteHeight <= 1.0F ||
         snapshot.playerBeatLines.size() < 2U ) {
        cancelUnresolvedRelease();
        return;
    }

    const float trackTop    = canvasSize.y * layout.top;
    const float trackBottom = canvasSize.y * layout.bottom;
    /// @brief 查询缓存目标在本帧仍然实际绘出的分拍线中心。
    /// @param time 缓存的精确分拍时间。
    /// @return 可完整容纳普通 Note 时返回应用播放补间后的逻辑 Y。
    const auto findBeatLineY = [&](double time) -> std::optional<float> {
        for ( const auto& line : snapshot.playerBeatLines ) {
            if ( std::abs(line.time - time) >= 1e-7 ) continue;
            const float y = line.y + m_preparedSnapshot.appliedYOffset;
            // 教程框必须完整落在玩家轨纵向边界内；中心可见但 Note 被裁掉的
            // 拍线不能作为练习目标，否则实际物件与提示框都只显示一部分。
            if ( y - noteHeight * 0.5F < trackTop ||
                 y + noteHeight * 0.5F > trackBottom ) {
                return std::nullopt;
            }
            return y;
        }
        return std::nullopt;
    };

    // 相机滚动、播放或近光标拍线模式都可能让已选拍线离开当前真实绘制集合。
    // 此时丢弃旧路线并从当前可见线重选，不能继续显示一个已经不存在的拍位。
    if ( m_walkthroughNoteDragTarget &&
         m_walkthroughNoteDragTarget->beatmapInstanceId ==
             snapshot.beatmapInstanceId &&
         (!findBeatLineY(m_walkthroughNoteDragTarget->sourceTime) ||
          !findBeatLineY(m_walkthroughNoteDragTarget->destinationTime)) ) {
        // 手势中途失去目标后，这次尝试不得继承下一条路径的起点资格。
        // 释放仍走取消入口，保留上一颗单键及此前有效编辑。
        m_walkthroughNoteStartedAtSource = false;
        cancelUnresolvedRelease();
        if ( m_walkthroughNoteAttemptActive ) return;
        m_walkthroughNoteDragTarget.reset();
    }

    // 长条首尾均从刚才单键附近的真实拍线选择，单轨内纵向拖动才能保证
    // 开启折线编辑时仍然创建普通 Hold，而不是横移形成 Flick 或 Polyline。
    if ( placingHold && (!m_walkthroughNoteDragTarget ||
                         m_walkthroughNoteDragTarget->beatmapInstanceId !=
                             snapshot.beatmapInstanceId) ) {
        if ( !m_walkthroughPlacedNote ||
             m_walkthroughPlacedNote->beatmapInstanceId !=
                 snapshot.beatmapInstanceId ) {
            // 用户显式跳过单键或切换谱面时没有可靠参考，不能假装已完成单键。
            // 保留“知道了”出口，正文说明返回单键步骤后再做完整练习。
            spotlight.reportTarget(targetId,
                                   canvasScreenPosition,
                                   { canvasScreenPosition.x + canvasSize.x,
                                     canvasScreenPosition.y + canvasSize.y },
                                   ImGui::GetWindowViewport(),
                                   false);
            return;
        }
        const auto hold = chooseComposeHoldTarget(
            snapshot.playerBeatLines,
            m_walkthroughPlacedNote->destinationTime,
            m_walkthroughPlacedNote->destinationTrack,
            snapshot.trackCount,
            noteHeight,
            trackTop - m_preparedSnapshot.appliedYOffset,
            trackBottom - m_preparedSnapshot.appliedYOffset,
            mixWalkthroughSeed(snapshot.beatmapInstanceId));
        if ( !hold ) {
            // 没有异轨或原拍位不在视野中时仅保留提示，让用户可移动视野或跳过。
            spotlight.reportTarget(targetId,
                                   canvasScreenPosition,
                                   { canvasScreenPosition.x + canvasSize.x,
                                     canvasScreenPosition.y + canvasSize.y },
                                   ImGui::GetWindowViewport(),
                                   false);
            return;
        }
        m_walkthroughNoteDragTarget = WalkthroughNoteDragTarget{
            .beatmapInstanceId = snapshot.beatmapInstanceId,
            .sourceTrack       = hold->track,
            .destinationTrack  = hold->track,
            .sourceTime        = hold->startTime,
            .destinationTime   = hold->endTime,
            .isHold            = true,
        };
    }

    // 每次进入该步骤只生成一次随机路线。候选来自渲染器已完成 BPM、首拍偏移、
    // Scroll/SV、自动显隐和像素行去重后的最终结果，不再在 UI 层近似量化。
    if ( !m_walkthroughNoteDragTarget ||
         m_walkthroughNoteDragTarget->beatmapInstanceId !=
             snapshot.beatmapInstanceId ) {
        struct Candidate {
            double time{ 0.0 };
            float  y{ 0.0F };
        };
        std::array<Candidate, 64> candidates{};
        std::size_t               candidateCount = 0;
        // 固定容量覆盖常规视野而不让 UI 每次进入步骤分配候选容器。
        // 渲染器已经按像素行去重，遍历顺序中的每项均代表不同可见位置。
        for ( const auto& line : snapshot.playerBeatLines ) {
            if ( candidateCount >= candidates.size() ) break;
            const float y = line.y + m_preparedSnapshot.appliedYOffset;
            if ( y - noteHeight * 0.5F >= trackTop &&
                 y + noteHeight * 0.5F <= trackBottom ) {
                candidates[candidateCount++] = { line.time, y };
            }
        }
        // 少于两个合法分拍无法满足“其他位置”的练习约束。保持等待比退化成
        // 同点点击更安全；用户改变缩放或播放位置后下一帧会重新尝试生成。
        if ( candidateCount < 2 ) return;

        // 谱面实例身份隔离不同标签；帧号只在首次生成时采样，成员缓存保证
        // 同一轮路线稳定。固定常量只作为混合盐，不承载任何谱面业务值。
        std::uint64_t seed = mixWalkthroughSeed(
            static_cast<std::uint64_t>(snapshot.beatmapInstanceId) ^
            (static_cast<std::uint64_t>(ImGui::GetFrameCount()) << 32U) ^
            0x434F4D504F53454FULL);
        const std::size_t destinationIndex = seed % candidateCount;
        seed                               = mixWalkthroughSeed(seed);
        std::size_t sourceIndex =
            (destinationIndex + 1U + seed % (candidateCount - 1U)) %
            candidateCount;
        // 起终点至少错开两个 Note 高度，路径才明确表达拖动而非近距离抖动。
        // 从随机起点循环寻找，保持候选随机性同时只选择真实拍线。
        const float minimumPathHeight = noteHeight * 2.0F;
        std::size_t inspected         = 0U;
        while ( inspected < candidateCount - 1U &&
                std::abs(candidates[sourceIndex].y -
                         candidates[destinationIndex].y) < minimumPathHeight ) {
            sourceIndex = (sourceIndex + 1U) % candidateCount;
            if ( sourceIndex == destinationIndex )
                sourceIndex = (sourceIndex + 1U) % candidateCount;
            ++inspected;
        }
        if ( std::abs(candidates[sourceIndex].y -
                      candidates[destinationIndex].y) < minimumPathHeight ) {
            return;
        }
        // 终点轨道覆盖当前真实轨数；起点在多轨谱面中通过非零偏移选到
        // 另一轨。单轨谱面无法满足跨轨，只保留跨分拍的完整拖拽语义。
        const int destinationTrack =
            static_cast<int>(mixWalkthroughSeed(seed) %
                             static_cast<std::uint64_t>(snapshot.trackCount));
        int sourceTrack = destinationTrack;
        if ( snapshot.trackCount > 1 ) {
            seed = mixWalkthroughSeed(seed);
            sourceTrack =
                (destinationTrack + 1 +
                 static_cast<int>(seed % static_cast<std::uint64_t>(
                                             snapshot.trackCount - 1))) %
                snapshot.trackCount;
        }
        m_walkthroughNoteDragTarget = WalkthroughNoteDragTarget{
            .beatmapInstanceId = snapshot.beatmapInstanceId,
            .sourceTrack       = sourceTrack,
            .destinationTrack  = destinationTrack,
            .sourceTime        = candidates[sourceIndex].time,
            .destinationTime   = candidates[destinationIndex].time,
        };
    }

    const auto& target = *m_walkthroughNoteDragTarget;
    // 路径保存谱面时间而非屏幕坐标。每帧回查渲染快照保证框继续贴住同一条
    // 实际分拍线；找不到时前面的状态分支已经清除并重选。
    const float playerWidth =
        projection.player.rightX - projection.player.leftX;
    if ( playerWidth <= 1.0F ) return;
    const float laneWidth =
        playerWidth / static_cast<float>(snapshot.trackCount);
    const auto sourceY      = findBeatLineY(target.sourceTime);
    const auto destinationY = findBeatLineY(target.destinationTime);
    if ( !sourceY || !destinationY ) return;
    // DPI 只影响装饰线宽与 Spotlight 外部留白，绝不能修改 Note 外接框尺寸。
    const float dpiScale =
        std::max(Config::AppConfig::instance().getWindowContentScale(), 1.0F);
    const auto makeRect = [&](int track, float y) {
        // 与 NoteRenderSystem::renderTap 相同：普通 Note 按实际纹理宽度在玩家
        // 单轨内居中，纵向以真实分拍线为中心，不使用教程自定义大小。
        const float trackLeft =
            projection.player.leftX + laneWidth * static_cast<float>(track);
        const float left  = trackLeft + (laneWidth - noteWidth) * 0.5F;
        const float right = left + noteWidth;
        return std::array<ImVec2, 2>{
            ImVec2{ canvasScreenPosition.x + left,
                    canvasScreenPosition.y + y - noteHeight * 0.5F },
            ImVec2{ canvasScreenPosition.x + right,
                    canvasScreenPosition.y + y + noteHeight * 0.5F },
        };
    };
    const auto sourceRect = makeRect(target.sourceTrack, *sourceY);
    const auto destinationRect =
        makeRect(target.destinationTrack, *destinationY);
    const ImVec2 sourceCenter{
        (sourceRect[0].x + sourceRect[1].x) * 0.5F,
        (sourceRect[0].y + sourceRect[1].y) * 0.5F,
    };
    const ImVec2 destinationCenter{
        (destinationRect[0].x + destinationRect[1].x) * 0.5F,
        (destinationRect[0].y + destinationRect[1].y) * 0.5F,
    };

    // Spotlight 只需要一个外接目标孔洞；两个独立框和箭头由画布绘制层明确区分。
    // 外接框还覆盖完整拖动路径，使暗化遮罩不会把中间轨迹重新盖暗。
    const ImVec2 targetMinimum{
        std::min(sourceRect[0].x, destinationRect[0].x) - 8.0F * dpiScale,
        std::min(sourceRect[0].y, destinationRect[0].y) - 8.0F * dpiScale,
    };
    const ImVec2 targetMaximum{
        std::max(sourceRect[1].x, destinationRect[1].x) + 8.0F * dpiScale,
        std::max(sourceRect[1].y, destinationRect[1].y) + 8.0F * dpiScale,
    };
    spotlight.reportTarget(targetId,
                           targetMinimum,
                           targetMaximum,
                           ImGui::GetWindowViewport(),
                           false);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // 教学图元必须裁剪在画布内容内。提示气泡由 Spotlight 在前景层绘制，
    // 这里不创建额外窗口，避免改变 Dock 焦点或画布 Hover 判定。
    drawList->PushClipRect(canvasScreenPosition,
                           { canvasScreenPosition.x + canvasSize.x,
                             canvasScreenPosition.y + canvasSize.y },
                           true);
    const float pulse =
        0.65F + 0.35F * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0F);
    const ImU32 sourceColor = IM_COL32(70, 220, 255, 255);
    const ImU32 destinationColor =
        IM_COL32(255, 238, 72, static_cast<int>(210.0F + 45.0F * pulse));
    // 起点使用稳定青色，目标用呼吸黄色；填充保持低透明度，让用户仍能看到
    // 下方轨道线和已有 Note，避免教程框被误认为谱面中已经存在的物件。
    drawList->AddRectFilled(sourceRect[0],
                            sourceRect[1],
                            IM_COL32(30, 150, 190, 42),
                            5.0F * dpiScale);
    drawList->AddRect(sourceRect[0],
                      sourceRect[1],
                      sourceColor,
                      5.0F * dpiScale,
                      0,
                      2.5F * dpiScale);
    drawList->AddRectFilled(destinationRect[0],
                            destinationRect[1],
                            IM_COL32(255, 238, 72, 42),
                            5.0F * dpiScale);
    drawList->AddRect(destinationRect[0],
                      destinationRect[1],
                      destinationColor,
                      5.0F * dpiScale,
                      0,
                      (2.5F + pulse) * dpiScale);
    drawList->AddLine(
        sourceCenter, destinationCenter, destinationColor, 2.5F * dpiScale);
    // 箭头只表达方向，不作为命中区。灯泡位于最终释放点正中，两个装饰均
    // 不参与 ImGui 输入，因此鼠标事件仍完整到达 Basic2DCanvasInteraction。
    const ImVec2 path{ destinationCenter.x - sourceCenter.x,
                       destinationCenter.y - sourceCenter.y };
    const float  pathLength = std::sqrt(path.x * path.x + path.y * path.y);
    if ( pathLength > 1.0F ) {
        const ImVec2 direction{ path.x / pathLength, path.y / pathLength };
        const ImVec2 normal{ -direction.y, direction.x };
        const float  arrowSize = 9.0F * dpiScale;
        const ImVec2 arrowBase{ destinationCenter.x - direction.x * arrowSize,
                                destinationCenter.y - direction.y * arrowSize };
        drawList->AddTriangleFilled(
            destinationCenter,
            { arrowBase.x + normal.x * arrowSize * 0.55F,
              arrowBase.y + normal.y * arrowSize * 0.55F },
            { arrowBase.x - normal.x * arrowSize * 0.55F,
              arrowBase.y - normal.y * arrowSize * 0.55F },
            destinationColor);
    }
    drawWalkthroughLightBulb(drawList,
                             destinationCenter,
                             std::min(noteHeight * 0.32F, noteWidth * 0.18F),
                             destinationColor);
    drawList->PopClipRect();

    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 canvasMaximum{ canvasScreenPosition.x + canvasSize.x,
                                canvasScreenPosition.y + canvasSize.y };
    if ( ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
         walkthroughPointInRect(mouse, canvasScreenPosition, canvasMaximum) ) {
        // 画布内任何按下都属于一次教学尝试；只有起点框内按下才可能成功。
        // 这样直接点击目标框也会在释放时取消，不留下绕过路径创建的 Note。
        m_walkthroughNoteAttemptActive = true;
        m_walkthroughNoteStartedAtSource =
            walkthroughPointInRect(mouse, sourceRect[0], sourceRect[1]);
        m_walkthroughNoteDragged       = false;
        m_walkthroughNoteBrushObserved = false;
        m_walkthroughNoteModifierUsed =
            (ImGui::GetIO().KeyShift != placingHold) || ImGui::GetIO().KeyCtrl;
    }
    if ( m_walkthroughNoteAttemptActive ) {
        // ImGui 拖动阈值证明这不是起点内的普通点击。逻辑快照确认画笔真正
        // 通过权限、轨道领域和资源绑定门禁，防止无对象提交也推进教程。
        m_walkthroughNoteDragged |=
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0F);
        m_walkthroughNoteBrushObserved |=
            snapshot.brush.isActive && !snapshot.brush.createsAudioSample &&
            snapshot.currentTool == Logic::EditTool::Draw;
        m_walkthroughNoteModifierUsed |=
            (ImGui::GetIO().KeyShift != placingHold) || ImGui::GetIO().KeyCtrl;
        // 长条练习全程限定在选定轨道内，横移后返回也不能误当纯 Hold 成功。
        // 使用轨道边界而非 Note 纹理宽度，允许轨道内部的自然指针抖动。
        if ( placingHold ) {
            const float laneLeft =
                canvasScreenPosition.x + projection.player.leftX +
                laneWidth * static_cast<float>(target.sourceTrack);
            m_walkthroughNoteStartedAtSource &=
                mouse.x >= laneLeft && mouse.x < laneLeft + laneWidth;
        }
    }
    if ( ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
         m_walkthroughNoteAttemptActive ) {
        // 长条必须检查逻辑层已经形成正时长 Hold 且两端精确对齐目标。
        // 仅在黄色框内释放不足以证明成功：中途横移可能已经变成折线。
        const bool brushMatches =
            !placingHold ||
            (snapshot.brush.type == ::MMM::NoteType::HOLD &&
             snapshot.brush.track == target.sourceTrack &&
             std::abs(snapshot.brush.time - target.sourceTime) < 1e-6 &&
             std::abs(snapshot.brush.time + snapshot.brush.duration -
                      target.destinationTime) < 1e-6 &&
             snapshot.brush.duration > 0.0);
        const bool success =
            m_walkthroughNoteStartedAtSource && m_walkthroughNoteDragged &&
            m_walkthroughNoteBrushObserved && !m_walkthroughNoteModifierUsed &&
            brushMatches && snapshot.currentTool == Logic::EditTool::Draw &&
            walkthroughPointInRect(
                mouse, destinationRect[0], destinationRect[1]);
        if ( success ) {
            // 这里只完成教学目标；同帧后续 Interaction 仍发送普通 EndBrush，
            // 由 DrawTool 创建最终 Note 并生成正常可撤销历史。
            if ( placingNote ) {
                // EndBrush 使用最后一次逻辑更新的实际时间与轨道，后续长条
                // 必须围绕这颗单键而不是释放指针的近似位置生成。
                m_walkthroughPlacedNote                  = target;
                m_walkthroughPlacedNote->destinationTime = snapshot.brush.time;
                m_walkthroughPlacedNote->destinationTrack =
                    snapshot.brush.track;
            }
            spotlight.completeTarget(targetId);
        } else if ( m_interaction ) {
            // Interaction 随后发布 cancel=true 的结束命令，原子丢弃失败预览。
            // 请求必须在本帧 Interaction::update 之前设置，释放分支消费后立即
            // 复位；因此它不可能泄漏到用户下一次正常画笔手势。
            m_interaction->cancelBrushOnNextRelease();
        }
        m_walkthroughNoteAttemptActive   = false;
        m_walkthroughNoteStartedAtSource = false;
        m_walkthroughNoteDragged         = false;
        m_walkthroughNoteBrushObserved   = false;
        m_walkthroughNoteModifierUsed    = false;
    }
}

/// @brief 更新画布 ImGui 窗口和交互状态。
/// @param sourceManager UI 管理器观察指针，用于欢迎页和协作房间访问。
/// @details 每帧依次处理关闭请求、会话标签状态、协作生命周期、
/// 稳定停靠与聚焦、可见性、离屏表面提交、活动画布交互及保存确认。
/// 多谱面后台标签只保留拖放、悬浮和瞬态 UI 收尾，
/// 不得发送完整编辑工具命令；滚轮位于后台内容区时会先切换活动会话。
/// 关闭最后一个真实会话会将其重置为 Logo 占位页，
/// 从而保留编辑器中心窗口并允许欢迎页继续取得焦点。
/// @warning 热路径：主渲染线程每帧执行；背景纹理同步必须保持在路径变化分支内，
/// 后台画布只同步悬停指针；滚轮发生时切换一次 Session 焦点，再交由活动画布
/// 处理完整滚轮语义。
void Basic2DCanvas::update(UI::UIManager* sourceManager)
{
    auto& engine = Logic::EditorEngine::instance();
    // cameraId 是窗口与会话的稳定连接键；索引会随标签关闭和重排变化，
    // 因此需要使用时重新解析，不能跨帧缓存旧索引。
    auto findSessionIndex = [this, &engine]() -> int32_t {
        for ( int32_t i = 0; i < engine.getSessionCount(); ++i ) {
            const auto* entry = engine.getSessionEntry(i);
            if ( entry && entry->cameraId == m_cameraId ) {
                // 返回当前容器中的即时索引，调用方在同一同步步骤内使用。
                return i;
            }
        }
        return -1;
    };

    // 关闭请求先经过脏谱面确认。未保存内容存在时恢复窗口 open 状态，
    // 由后面的模态框决定保存、丢弃或取消，不能立即销毁会话。
    if ( !m_isOpen ) {
        if ( !m_closeConfirmed && m_currentSnapshot &&
             m_currentSnapshot->isDirty ) {
            m_isOpen          = true;
            m_showSaveConfirm = true;
        } else if ( shouldKeepOpenForLastSessionReset() ) {
            // 最后一个真实会话不从 Dock 中移除，而是原位变成 Logo
            // 占位页；这维持启动布局并为欢迎页保留编辑器标签顺序。
            if ( int32_t myIdx = findSessionIndex(); myIdx != -1 ) {
                engine.resetSessionToLogoPlaceholder(
                    myIdx, TR("canvas.welcome").data());
            }
            m_isOpen         = true;
            m_closeConfirmed = false;
        }
    }

    bool    showClose         = false;
    bool    isLogoPlaceholder = false;
    int32_t myIndex           = findSessionIndex();
    // Logo 占位页没有可关闭的谱面语义，隐藏标签关闭按钮；真实会话
    // 才把 m_isOpen 交给 ImGui 标题栏关闭控件。
    if ( myIndex != -1 ) {
        const auto* entry = engine.getSessionEntry(myIndex);
        isLogoPlaceholder = entry && entry->isLogoPlaceholder;
        showClose         = entry && !isLogoPlaceholder;
    }

    auto* collaborationRoom =
        sourceManager ? sourceManager->getCollaborationRoom() : nullptr;
    // Connecting、Hosting、Connected 等非 Idle 状态都属于同一次房间
    // 生命周期，避免短暂离线时把协作标签错误转换为普通本地谱面。
    const bool roomLifecycleActive =
        collaborationRoom &&
        collaborationRoom->state() !=
            Network::Collaboration::CollaborationRoomState::Idle;
    m_isCollaborationCanvas = resolveCollaborationCanvasState(
        m_isCollaborationCanvas,
        isLogoPlaceholder,
        m_wasLogoPlaceholder,
        roomLifecycleActive,
        m_wasCollaborationRoomLifecycleActive,
        myIndex == engine.getActiveSessionIndex());
    // 保存上一帧占位与房间状态，使纯状态 helper 能识别新建协作会话、
    // 断线以及占位页被真实项目替换的边界。
    m_wasLogoPlaceholder                  = isLogoPlaceholder;
    m_wasCollaborationRoomLifecycleActive = roomLifecycleActive;

    std::string_view collaborationStatusLabel;
    if ( m_isCollaborationCanvas ) {
        // Hosting 与 Connected 才显示在线；连接中断但会话仍保留时
        // 显示离线标签，而不是移除协作身份提示。
        const auto state =
            collaborationRoom
                ? collaborationRoom->state()
                : Network::Collaboration::CollaborationRoomState::Idle;
        const bool online =
            state == Network::Collaboration::CollaborationRoomState::Hosting ||
            state == Network::Collaboration::CollaborationRoomState::Connected;
        collaborationStatusLabel = TR(online ? "canvas.collaboration.online"
                                             : "canvas.collaboration.offline")
                                       .data();
    }

    const std::string title = makeCanvasTabTitle(
        TR("canvas.editor").data(),
        m_currentSnapshot && m_currentSnapshot->hasBeatmap,
        m_currentSnapshot ? m_currentSnapshot->beatmapName : std::string_view{},
        m_currentSnapshot && m_currentSnapshot->isDirty,
        collaborationStatusLabel);
    // 标题 helper 统一组合编辑器名、谱名、脏标志与协作状态；
    // 无快照或 Logo 占位页时不会泄漏上一谱面的可见标题。

    // 仅显式请求回到中心时设置 dockId；正常帧让 imgui.ini 保留用户
    // 调整后的停靠位置，避免每帧强制拖回默认中心。
    ImGuiID dockId =
        m_shouldDockToCenter ? UI::MainDockSpaceUI::getCenterDockId() : 0;
    std::string windowName = fmt::format("{}###{}", title, m_canvasName);
    // ### 左侧只负责展示，右侧稳定 ID 负责 Dock 与窗口状态；
    // 谱名变化、保存脏标志变化都不会创建新的 ImGui 窗口。
    const auto* welcome = sourceManager->getView<UI::WelcomeView>("Welcome");
    const bool  keepWelcomeFocus =
        isLogoPlaceholder && welcome && welcome->isOpen();
    if ( m_shouldFocusNextFrame ) {
        // 欢迎页打开时占位编辑器不能抢走启动焦点；其它显式聚焦请求
        // 在 Begin 之前提交给 ImGui，确保停靠标签被切到前台。
        if ( !keepWelcomeFocus ) {
            ImGui::SetNextWindowFocus();
        }
        m_shouldFocusNextFrame = false;
    }
    UI::LayoutContext lctx(m_layoutCtx,
                           windowName,
                           true,
                           keepWelcomeFocus
                               ? ImGuiWindowFlags_NoFocusOnAppearing
                               : ImGuiWindowFlags_None,
                           showClose ? &m_isOpen : nullptr,
                           dockId,
                           ImGuiCond_Always);
    reportBeatmapTabWalkthroughTarget(
        sourceManager, ImGui::GetCurrentWindow(), !isLogoPlaceholder);
    // 标签目标先于内容可见性计算提交，使后台谱面也能提供可点击入口。
    // 内容区域目标则只在标签真正可见并拥有有效 RenderContext 时上报。
    // ImGuiCond_Always 只应用显式非零 dockId；普通帧传入零时保留
    // imgui.ini 中的用户布局，不覆盖其手动拖动结果。
    m_lastDockId = ImGui::IsWindowDocked() ? ImGui::GetWindowDockID() : 0;
    if ( m_shouldDockToCenter && ImGui::IsWindowDocked() ) {
        // ImGui 已确认本帧进入停靠节点后再消费请求；若窗口仍浮动，
        // 标志保留到后续帧继续尝试。
        m_shouldDockToCenter = false;
    }

    m_isCanvasVisible = isCurrentCanvasWindowVisible();
    // 可见性同步到会话调度器，使逻辑线程只准备当前可见 Canvas 的
    // 完整渲染数据，同时后台标签仍保留自身对象与轻量状态。
    engine.setSessionCanvasVisible(m_cameraId, m_isCanvasVisible);

    if ( m_isCanvasVisible ) {
        // 只有真实可见标签创建 RenderContext；其 RAII 生命周期负责
        // 检查目标尺寸并在离开作用域前完成离屏表面布局。
        RenderContext rctx(
            this, m_canvasName.c_str(), m_targetWidth, m_targetHeight, nullptr);

        // 键盘或鼠标使当前窗口获得根级焦点时，同步活动 Session。
        // 只有索引确实变化才发布，避免每帧重复触发会话切换副作用。
        if ( ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ) {
            int32_t activeIdx = engine.getActiveSessionIndex();
            int32_t myIdx     = findSessionIndex();
            // 根窗口及其子控件均可激活谱面，表格或画布内弹出控件获得
            // 焦点时不会让该标签仍被当作后台会话。
            if ( myIdx != -1 && myIdx != activeIdx ) {
                engine.setActiveSessionIndex(myIdx);
                XINFO(
                    "Basic2DCanvas: Focus switched active session to index {} "
                    "(cameraId={})",
                    myIdx,
                    m_cameraId);
            }
        }

        if ( m_currentSnapshot ) {
            // 背景资源只在路径或媒体类型变化时同步；纹理加载与视频
            // 打开包含文件系统操作，绝不能无条件进入每帧路径。
            if ( m_currentSnapshot->backgroundPath != m_loadedBgPath ||
                 m_currentSnapshot->backgroundIsVideo !=
                     m_loadedBackgroundIsVideo ) {
                updateBackgroundTexture();
            }
            if ( m_currentSnapshot->backgroundIsVideo ) {
                // 视频帧更新只在快照确认当前背景为视频时执行，静态图
                // 不轮询播放器，也不会意外保留旧视频的时间推进。
                updateBackgroundVideoFrame();
            }
        }

        // 先提交离屏纹理，后续交互控制器创建的 ImGui 命中区域才会
        // 位于画面之上；同时保存屏幕坐标供协作覆盖层对齐。
        const ImVec2 canvasScreenPosition = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize           = rctx.getRenderSize();
        rctx.renderSurface();
        if ( m_currentSnapshot )
            // 快照与刚绘制的纹理使用同一代布局，避免教学框和画面错位。
            reportCanvasWalkthroughTargets(sourceManager,
                                           *m_currentSnapshot,
                                           canvasScreenPosition,
                                           canvasSize);

        // 后台谱面收到内容区滚轮时先切换活动会话和窗口焦点，
        // 本帧若 cameraId 已切换成功即可继续消费这一次滚轮操作。
        bool isActiveCanvas = engine.getActiveCameraId() == m_cameraId;
        if ( !isActiveCanvas && myIndex != -1 &&
             isWheelOverCurrentWindowContent() ) {
            ImGui::SetWindowFocus();
            engine.setActiveSessionIndex(myIndex);
            m_shouldFocusNextFrame = true;
            isActiveCanvas         = engine.getActiveCameraId() == m_cameraId;
            XINFO(
                "Basic2DCanvas: Wheel switched active session to index {} "
                "(cameraId={})",
                myIndex,
                m_cameraId);
        }

        if ( isActiveCanvas ) {
            // 活动画布先发布并绘制协作视野，再运行编辑交互；后者可在
            // 同一 ImGui 层建立命中区域，而覆盖层始终使用本帧快照。
            updateCollaborationViewports(
                sourceManager, canvasScreenPosition, canvasSize);
            if ( m_currentSnapshot ) {
                // 教程先判定本帧释放是否合法，以便交互控制器随后选择提交或
                // 取消 CmdEndBrush；目标装饰不创建 ImGui Item，不拦截画布输入。
                updateComposeWalkthrough(sourceManager,
                                         *m_currentSnapshot,
                                         canvasScreenPosition,
                                         canvasSize);
            }
            m_interaction->update(sourceManager,
                                  m_currentSnapshot,
                                  m_logicalWidth,
                                  m_logicalHeight);
        } else {
            // 后台画布仍接受文件拖入，并清理悬浮/瞬态 UI，保证鼠标
            // 离开旧标签后不会残留高亮；编辑工具命令则完全跳过。
            m_interaction->handleDrops(sourceManager);
            m_interaction->updateHoverState(m_logicalWidth, m_logicalHeight);
            m_interaction->updateTransientUi();
        }
    }

    // 模态框放在画布可见分支之外：用户关闭标签后内容区可能已隐藏，
    // 但脏谱面的保存决策仍必须继续呈现并完成。
    if ( m_showSaveConfirm ) {
        ::MMM::UI::FeedbackOpenPopup("Save Confirmation###SaveConfirmModal");
    }

    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    ::MMM::UI::Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin("Save Confirmation###SaveConfirmModal") ) {
        // 快照可能在关闭流程中失效，标题回退为 Unknown 仅影响提示，
        // 不改变会话索引或保存目标。
        std::string mapName =
            m_currentSnapshot ? m_currentSnapshot->beatmapName : "Unknown";
        ImGui::Text("%s", TR_FMT("ui.exit.confirm_msg_fmt", mapName).c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        // 三个按钮共享统一反馈包装与 DPI 尺寸；固定顺序对应保存、
        // 明确丢弃和返回编辑，避免关闭动作存在含糊默认分支。

        if ( ::MMM::UI::FeedbackButton(TR("ui.file.save").data(),
                                       ImVec2(120 * dpiScale, 0)) ) {
            // 保存命令面向活动会话，先将当前 Canvas 对应索引设为活动，
            // 防止多谱面下保存到此前聚焦的其它标签。
            int32_t myIdx = findSessionIndex();
            if ( myIdx != -1 ) {
                engine.setActiveSessionIndex(myIdx);
            }
            // 发布保存后记录关闭已确认；具体磁盘写入由逻辑命令处理，
            // UI 不在模态框中同步等待文件系统完成。
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdSaveBeatmap{}));
            m_closeConfirmed  = true;
            m_isOpen          = false;
            m_showSaveConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(TR("ui.exit.dont_save").data(),
                                       ImVec2(120 * dpiScale, 0)) ) {
            // 丢弃分支不发布保存命令，但仍标记关闭确认，允许会话管理器
            // 在下一阶段真正移除或重置该标签。
            m_closeConfirmed  = true;
            m_isOpen          = false;
            m_showSaveConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.cancel").data(),
                                       ImVec2(120 * dpiScale, 0)) ) {
            // 取消恢复窗口并设置一次性通知标志，调用者可据此撤销
            // 正在进行的标签关闭编排，而不会把取消误当作保存失败。
            m_isOpen          = true;
            m_showSaveConfirm = false;
            m_closeCancelled  = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/// @brief 发布本地视野并在主画布上绘制远端参与者视野。
/// @param sourceManager UI 管理器观察指针，用于取得当前协作房间。
/// @param canvasScreenPosition 画布内容左上角屏幕坐标。
/// @param canvasSize 画布内容逻辑尺寸。
/// @details 本地视野包含播放/视觉时间、轨道区域对应的可见时间范围，
/// 以及相对于当前画布宽度的水平偏移比例。
/// 跟随远端参与者时，每个新 sequence 最多发布一次 Seek 和水平 Pan，
/// 防止同一远端状态在每帧重复应用。
///
/// 与本地时间范围相交的远端视野按设置绘制填充、矩形或轨道边缘；
/// 完全离屏的远端视野绘制在上/下边缘，并提供点击跳转命中区。
/// 所有覆盖层限制在当前画布矩形内，不写入离屏渲染快照。
/// @warning UI 热路径：活动协作画布每帧调用；参与者规模受房间上限约束，
/// 不得访问文件系统、等待网络或复制房间所有权。
void Basic2DCanvas::updateCollaborationViewports(
    UI::UIManager* sourceManager, const ImVec2& canvasScreenPosition,
    const ImVec2& canvasSize)
{
    // 缺少 UI、快照、谱面或有效尺寸时无法构造时间与轨道投影；
    // 直接返回且不发布不完整的本地视野。
    if ( !sourceManager || !m_currentSnapshot ||
         !m_currentSnapshot->hasBeatmap || canvasSize.x <= 1.0F ||
         canvasSize.y <= 1.0F ) {
        return;
    }
    auto* room = sourceManager->getCollaborationRoom();
    if ( !room || !room->isActive() || room->localPeerId() == 0 ) {
        // 房间结束后清空跟随去重状态，下一次进入房间即使 peerId
        // 恰好复用也会从 sequence 零开始重新同步。
        m_lastFollowedPeerId           = 0;
        m_lastFollowedViewportSequence = 0;
        return;
    }

    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    // 渲染模式是本地显示偏好，不写入 ParticipantViewport；不同用户
    // 可独立选择填充、轮廓或轨道边缘而不影响网络协议。
    const auto viewportRenderMode = Config::AppConfig::instance()
                                        .getEditorSettings()
                                        .collaborationViewportRenderMode;
    const auto& layout =
        visual.trackLayoutForKeyCount(m_currentSnapshot->trackCount);
    // 使用当前轨道数对应布局，使动态扩轨后的协作范围立即跟随
    // 新轨道外包边界，而不是沿用固定主题默认矩形。

    // 快照时间已经位于统一视觉域，先复制全画布可见范围作为回退；
    // 合法轨道布局随后会把范围收窄到实际谱面轨道区域。
    Network::Collaboration::ParticipantViewport localViewport;
    localViewport.playbackTime = m_currentSnapshot->playbackTime;
    localViewport.visualTime   = m_currentSnapshot->currentTime;
    // playbackTime 用于点击/跟随跳转，visualTime 用于判断远端在当前
    // 视野上方或下方；两者不能因视觉偏移而混用。
    localViewport.visibleTimeStart = m_currentSnapshot->visibleTimeStart;
    localViewport.visibleTimeEnd   = m_currentSnapshot->visibleTimeEnd;
    if ( std::isfinite(layout.top) && std::isfinite(layout.bottom) &&
         layout.top < layout.bottom ) {
        // 轨道上下边界是归一化画布坐标，转换为局部 Y 后以判定线
        // 为锚点反投影到时间；辅助区和窗口空白不计入共享视野。
        const float judgmentLineY =
            canvasSize.y * visual.judgmentLinePositionForKeyCount(
                               std::max(m_currentSnapshot->trackCount, 1));
        const float trackTopY =
            canvasSize.y * std::clamp(layout.top, 0.0F, 1.0F);
        const float trackBottomY =
            canvasSize.y * std::clamp(layout.bottom, 0.0F, 1.0F);
        // 分别反投影两条物理边界，不假设当前 SV 为正或时间与 Y
        // 保持简单线性关系；helper 在无可靠解时返回空值。
        const auto trackBottomTime = unprojectCollaborationViewportTime(
            trackBottomY,
            m_currentSnapshot->currentTime,
            m_currentSnapshot->visibleTimeStart,
            m_currentSnapshot->visibleTimeEnd,
            judgmentLineY,
            canvasSize.y);
        const auto trackTopTime = unprojectCollaborationViewportTime(
            trackTopY,
            m_currentSnapshot->currentTime,
            m_currentSnapshot->visibleTimeStart,
            m_currentSnapshot->visibleTimeEnd,
            judgmentLineY,
            canvasSize.y);
        if ( trackBottomTime && trackTopTime ) {
            // 预览方向未来向上，因此底部对应起始时间、顶部对应结束时间。
            localViewport.visibleTimeStart = *trackBottomTime;
            localViewport.visibleTimeEnd   = *trackTopTime;
        }
    }
    // 水平偏移按画布宽度归一化，远端不同窗口尺寸可恢复相同相对
    // 视野；前置尺寸检查保证此处除数严格为正。
    localViewport.horizontalOffsetRatio =
        static_cast<double>(m_currentSnapshot->canvasHorizontalOffsetX) /
        static_cast<double>(canvasSize.x);
    room->publishLocalViewport(localViewport,
                               m_currentSnapshot->isSeekScrubbing);
    // Seek 拖动状态随视野一并发布，房间可合并高频预览更新并在
    // 最终提交时及时广播稳定位置，不阻塞本地画布响应。

    const auto followedPeerId = room->followedPeerId();
    if ( followedPeerId != m_lastFollowedPeerId ) {
        // 切换跟随对象后旧 sequence 不可用于新参与者去重。
        m_lastFollowedPeerId           = followedPeerId;
        m_lastFollowedViewportSequence = 0;
    }
    const auto& viewports = room->participantViewports();
    // participantViewports 是房间维护的最新值缓存；本函数只观察，
    // 不删除过期项，生命周期清理由网络会话统一负责。
    // 跳转同时对齐播放时间与横向轨道位置；纵向位置由 Seek 后的新
    // 快照重建，不能再叠加本地 Pan Y。
    const auto jumpToViewport =
        [this, &canvasSize](
            const Network::Collaboration::ParticipantViewport& viewport) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSeek{ viewport.playbackTime }));
            const float desiredHorizontalOffset =
                static_cast<float>(viewport.horizontalOffsetRatio *
                                   static_cast<double>(canvasSize.x));
            const float horizontalDelta =
                desiredHorizontalOffset -
                m_currentSnapshot->canvasHorizontalOffsetX;
            // 极小差值不发布 Pan，抑制不同窗口宽度反复换算产生的
            // 亚像素抖动；非有限远端输入也不得进入逻辑命令。
            if ( std::isfinite(horizontalDelta) &&
                 std::abs(horizontalDelta) > 0.05F ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdPanCanvas{
                        .cameraId       = m_cameraId,
                        .deltaX         = horizontalDelta,
                        .deltaY         = 0.0F,
                        .viewportWidth  = canvasSize.x,
                        .viewportHeight = canvasSize.y,
                        .renderScaleY   = m_currentSnapshot->renderScaleY,
                    }));
            }
        };
    if ( followedPeerId != 0 ) {
        const auto followed = viewports.find(followedPeerId);
        // sequence 只在远端发布新视野时增长；每个值最多应用一次，
        // 避免静止参与者使本地用户无法临时滚动查看其它位置。
        if ( followed != viewports.end() &&
             followed->second.sequence != m_lastFollowedViewportSequence ) {
            const auto& viewport = followed->second;
            jumpToViewport(viewport);
            m_lastFollowedViewportSequence = viewport.sequence;
        }
    }

    const auto& participants = room->participants();
    const auto  localId      = room->localPeerId();
    // 本地范围先规范为 minimum/maximum，兼容未来向上导致端点顺序
    // 与数值增减方向不一致的快照表示。
    const double localMinimum = std::min(m_currentSnapshot->visibleTimeStart,
                                         m_currentSnapshot->visibleTimeEnd);
    const double localMaximum = std::max(m_currentSnapshot->visibleTimeStart,
                                         m_currentSnapshot->visibleTimeEnd);
    if ( !std::isfinite(localMinimum) || !std::isfinite(localMaximum) ) {
        return;
    }

    // 轨道投影统一考虑 BGM、BMS、草稿轨与水平偏移；协作矩形只覆盖
    // 实际内容外包边界，不把左右辅助区或布局间隙误算为远端视野。
    const auto localLaneProjection = Logic::calculateCanvasLaneProjection(
        canvasSize.x,
        m_currentSnapshot->trackCount,
        m_currentSnapshot->bgmTrackCount,
        layout,
        m_currentSnapshot->canvasHorizontalOffsetX,
        true,
        m_currentSnapshot->bmsEditingEnabled,
        m_currentSnapshot->draftLanesEnabled,
        m_currentSnapshot->draftTrackCount,
        true);
    if ( !localLaneProjection.valid ) {
        return;
    }
    // 独立辅助区可以互换顺序或留出空隙，统一取所有区域外包边界。
    const auto contentBounds = localLaneProjection.contentBounds();
    // contentBounds 会合并普通轨、BGM 区和草稿轨，避免只取主轨道
    // 时远端视野在扩展编辑模式下显得过窄。
    const auto horizontalRange = projectCollaborationViewportHorizontalRange(
        contentBounds.leftX, contentBounds.rightX, canvasSize.x);
    if ( !horizontalRange ) {
        return;
    }

    /// @brief 判断远端视口是否需要绘制上方或下方离屏提示。
    /// @return 上方提示返回 true，下方提示返回 false，不需要提示则返回空值。
    /// @details 与本地范围相交的视野由矩形分支绘制；只有完全位于
    /// 当前范围之外且 visualTime 有效的参与者才返回方向。
    const auto classifyOffscreenIndicator =
        [&viewports, &participants, localId, localMinimum, localMaximum, this](
            Network::Collaboration::PeerId peerId) -> std::optional<bool> {
        if ( peerId == localId || !participants.contains(peerId) ) {
            return std::nullopt;
        }
        const auto viewportIt = viewports.find(peerId);
        if ( viewportIt == viewports.end() ) {
            return std::nullopt;
        }
        const auto& viewport = viewportIt->second;
        // 远端范围同样规范化后再做闭区间相交测试；刚好接触边界时
        // 仍按可见矩形处理，避免箭头与矩形在边界附近闪烁切换。
        const double remoteMinimum =
            std::min(viewport.visibleTimeStart, viewport.visibleTimeEnd);
        const double remoteMaximum =
            std::max(viewport.visibleTimeStart, viewport.visibleTimeEnd);
        if ( !std::isfinite(remoteMinimum) || !std::isfinite(remoteMaximum) ||
             !std::isfinite(viewport.visualTime) ||
             (remoteMaximum >= localMinimum &&
              remoteMinimum <= localMaximum) ) {
            return std::nullopt;
        }
        return viewport.visualTime > m_currentSnapshot->currentTime;
    };

    std::array<std::size_t, 2> indicatorCounts{};
    // 预先统计上下两个方向的离屏标记数量，后续布局 helper 才能
    // 在有限水平范围内均匀安排每个箭头槽位。
    for ( const auto& [peerId, viewport] : viewports ) {
        (void)viewport;
        const auto direction = classifyOffscreenIndicator(peerId);
        if ( direction ) {
            ++indicatorCounts[*direction ? 1U : 0U];
        }
    }

    ImDrawList*  drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMaximum{
        canvasScreenPosition.x + canvasSize.x,
        canvasScreenPosition.y + canvasSize.y,
    };
    // 覆盖层使用屏幕坐标绘制，但裁剪到画布内容矩形，不能覆盖标签、
    // 相邻 Dock 窗口或底部状态栏。
    drawList->PushClipRect(canvasScreenPosition, canvasMaximum, true);

    for ( const auto& [peerId, viewport] : viewports ) {
        // 本地视野由当前画布本身表达，不重复绘制；远端视野只有在
        // 参与者元数据仍存在时才能取得稳定姓名与颜色。
        if ( peerId == localId ) {
            continue;
        }
        const auto participant = participants.find(peerId);
        if ( participant == participants.end() ) {
            continue;
        }

        const double remoteMinimum =
            std::min(viewport.visibleTimeStart, viewport.visibleTimeEnd);
        const double remoteMaximum =
            std::max(viewport.visibleTimeStart, viewport.visibleTimeEnd);
        if ( !std::isfinite(remoteMinimum) || !std::isfinite(remoteMaximum) ) {
            // 网络缓存中的异常范围只影响该参与者覆盖层，不中断其它
            // 参与者绘制，也不传播到 ImGui 坐标计算。
            continue;
        }

        const float leftX   = horizontalRange->leftX;
        const float rightX  = horizontalRange->rightX;
        const float centerX = (leftX + rightX) * 0.5F;
        // 颜色由服务端稳定 participantId 派生，而不是临时 peerId；
        // 重连更换连接编号后仍保持用户颜色一致。
        const ImU32 color =
            collaborationPeerColor(participant->second.participantId, 255);
        const bool following = followedPeerId == peerId;

        if ( remoteMaximum >= localMinimum && remoteMinimum <= localMaximum ) {
            // 相交视野的两个时间端点分别经过当前 SV 分段投影，
            // 即使远端范围顺序颠倒也通过 min/max 形成有效矩形。
            float firstY = collaborationTimeToCanvasY(
                *m_currentSnapshot, viewport.visibleTimeStart, canvasSize.y);
            float secondY = collaborationTimeToCanvasY(
                *m_currentSnapshot, viewport.visibleTimeEnd, canvasSize.y);
            float topY = std::clamp(
                std::min(firstY, secondY), 1.0F, canvasSize.y - 1.0F);
            float bottomY = std::clamp(
                std::max(firstY, secondY), 1.0F, canvasSize.y - 1.0F);
            // 极短或刚接触边界的视野仍保留至少三像素高度，确保
            // 轮廓在高倍率与相近颜色背景中可见。
            if ( bottomY - topY < 3.0F ) {
                bottomY = topY + 3.0F;
            }
            const ImVec2 rectangleMinimum{
                canvasScreenPosition.x + leftX,
                canvasScreenPosition.y + topY,
            };
            const ImVec2 rectangleMaximum{
                canvasScreenPosition.x + rightX,
                canvasScreenPosition.y + std::min(bottomY, canvasSize.y - 1.0F),
            };
            // following 仅增强轮廓，不改变参与者底色，用户可以同时
            // 识别身份颜色与当前跟随目标。
            const float outlineThickness = following ? 3.0F : 2.0F;
            if ( viewportRenderMode ==
                 Config::CollaborationViewportRenderMode::Filled ) {
                // Filled 模式使用低透明度同色背景，轮廓仍在后面绘制，
                // 避免遮挡谱面物件同时保留范围整体感。
                drawList->AddRectFilled(
                    rectangleMinimum,
                    rectangleMaximum,
                    collaborationPeerColor(participant->second.participantId,
                                           24));
            }
            if ( viewportRenderMode ==
                 Config::CollaborationViewportRenderMode::TrackEdge ) {
                // TrackEdge 只在内容左缘绘制括号，适合减少多人协作时
                // 横跨全部轨道的线框重叠。
                constexpr float BRACKET_CAP_WIDTH = 12.0F;
                const float     bracketRight      = std::min(
                    rectangleMinimum.x + BRACKET_CAP_WIDTH, rectangleMaximum.x);
                drawList->AddLine(rectangleMinimum,
                                  { rectangleMinimum.x, rectangleMaximum.y },
                                  color,
                                  outlineThickness);
                drawList->AddLine(rectangleMinimum,
                                  { bracketRight, rectangleMinimum.y },
                                  color,
                                  outlineThickness);
                drawList->AddLine({ rectangleMinimum.x, rectangleMaximum.y },
                                  { bracketRight, rectangleMaximum.y },
                                  color,
                                  outlineThickness);
            } else {
                // Outline 与 Filled 都使用完整矩形轮廓；跟随对象通过
                // 更粗线宽与普通远端用户区分。
                drawList->AddRect(rectangleMinimum,
                                  rectangleMaximum,
                                  color,
                                  0.0F,
                                  0,
                                  outlineThickness);
            }

            const ImVec2 textSize =
                ImGui::CalcTextSize(participant->second.creator.c_str());
            // 姓名优先放在矩形上方；超出画布顶部时移入矩形内部，
            // X 坐标则同时约束左右边缘，保证整段文本可见。
            const float labelHeight = textSize.y + 6.0F;
            float       labelY      = rectangleMinimum.y - labelHeight;
            if ( labelY < canvasScreenPosition.y ) {
                labelY = rectangleMinimum.y + 1.0F;
            }
            const float labelX =
                std::clamp(rectangleMinimum.x,
                           canvasScreenPosition.x,
                           std::max(canvasScreenPosition.x,
                                    canvasMaximum.x - textSize.x - 10.0F));
            const ImVec2 labelMinimum{ labelX, labelY };
            const ImVec2 labelMaximum{ labelX + textSize.x + 10.0F,
                                       labelY + labelHeight };
            // 标签底色使用高不透明度参与者色，文字固定为白色；
            // 与低透明度视野填充区分，避免姓名被谱面纹理淹没。
            drawList->AddRectFilled(
                labelMinimum,
                labelMaximum,
                collaborationPeerColor(participant->second.participantId, 220),
                3.0F);
            drawList->AddText({ labelX + 5.0F, labelY + 3.0F },
                              IM_COL32(255, 255, 255, 255),
                              participant->second.creator.c_str());
            continue;
        }

        // 完全离开当前时间范围的参与者改用边缘箭头；未来位于上方，
        // 过去位于下方，与主画布时间向上滚动的视觉方向一致。
        const bool remoteAhead =
            viewport.visualTime > m_currentSnapshot->currentTime;
        const std::size_t directionIndex = remoteAhead ? 1U : 0U;
        std::size_t       indicatorSlot  = 0;
        // 以 peerId 稳定排序计算当前槽位，不额外分配或排序容器；
        // 相同参与者集合在每帧会得到一致的水平位置。
        for ( const auto& [candidatePeerId, candidateViewport] : viewports ) {
            (void)candidateViewport;
            if ( candidatePeerId >= peerId ) {
                continue;
            }
            const auto candidateDirection =
                classifyOffscreenIndicator(candidatePeerId);
            if ( candidateDirection && *candidateDirection == remoteAhead ) {
                ++indicatorSlot;
            }
        }
        const float tipY  = remoteAhead ? canvasScreenPosition.y + 7.0F
                                        : canvasMaximum.y - 7.0F;
        const float baseY = remoteAhead ? tipY + 13.0F : tipY - 13.0F;
        // 箭头尖端留出七像素边距，底边向内容区展开，确保形状不会
        // 被画布裁剪边界截断。
        const float arrowLocalX = layoutCollaborationViewportIndicatorX(
                                      leftX,
                                      rightX,
                                      canvasSize.x,
                                      indicatorSlot,
                                      indicatorCounts[directionIndex])
                                      .value_or(centerX);
        // 布局 helper 失败时回退到轨道中心；屏幕 X 再叠加画布起点，
        // 不受当前 Dock 在桌面中的绝对位置影响。
        const float arrowX = canvasScreenPosition.x + arrowLocalX;
        drawList->AddTriangleFilled({ arrowX, tipY },
                                    { arrowX - 8.0F, baseY },
                                    { arrowX + 8.0F, baseY },
                                    color);
        const ImVec2 textSize =
            ImGui::CalcTextSize(participant->second.creator.c_str());
        const float textX =
            std::clamp(arrowX - textSize.x * 0.5F,
                       canvasScreenPosition.x + 2.0F,
                       std::max(canvasScreenPosition.x + 2.0F,
                                canvasMaximum.x - textSize.x - 2.0F));
        const float textY =
            remoteAhead ? baseY + 2.0F : baseY - textSize.y - 2.0F;
        // 姓名位于箭头底边朝画布内部的一侧，不会越过窗口上下边界。
        drawList->AddText(
            { textX, textY }, color, participant->second.creator.c_str());

        // 箭头与姓名组成一个联合命中矩形，保存并恢复 ImGui 光标，
        // 以免不可见按钮改变画布后续控件布局。
        const ImVec2 savedCursorPosition = ImGui::GetCursorScreenPos();
        const ImVec2 hitMinimum{
            // 命中区合并箭头和文本并额外扩展四像素，随后钳制到画布，
            // 既便于点击又不会遮挡相邻窗口。
            std::max(canvasScreenPosition.x,
                     std::min(arrowX - 11.0F, textX - 4.0F)),
            std::max(canvasScreenPosition.y,
                     std::min({ tipY, baseY, textY }) - 4.0F),
        };
        const ImVec2 hitMaximum{
            std::min(canvasMaximum.x,
                     std::max(arrowX + 11.0F, textX + textSize.x + 4.0F)),
            std::min(canvasMaximum.y,
                     std::max({ tipY, baseY, textY + textSize.y }) + 4.0F),
        };
        if ( hitMaximum.x > hitMinimum.x && hitMaximum.y > hitMinimum.y ) {
            // participantId 参与 ImGui ID，多个远端用户使用同名按钮时
            // 仍有独立 active/hover 状态。
            ImGui::PushID(participant->second.participantId.c_str());
            ImGui::SetCursorScreenPos(hitMinimum);
            if ( ImGui::InvisibleButton("##CollaborationViewportJump",
                                        { hitMaximum.x - hitMinimum.x,
                                          hitMaximum.y - hitMinimum.y }) ) {
                // 点击边缘提示复用跟随跳转逻辑，但不会改变 followedPeerId；
                // 用户仍可临时查看该参与者而不进入持续跟随。
                jumpToViewport(viewport);
            }
            if ( ImGui::IsItemHovered() ) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos(savedCursorPosition);
        }
    }
    // 所有 ImGui 临时命中控件均已恢复光标位置，此处只需结束覆盖层
    // 裁剪，后续保存模态框可以正常绘制到完整窗口范围。
    drawList->PopClipRect();
}

/// @brief 请求关闭当前画布标签。
/// @details 清除上一轮确认/取消结果并将窗口置为待关闭；
/// 下一次 update 根据快照脏状态决定直接关闭、重置占位页或打开确认框。
/// 调用方不应立即销毁对象，因为保存确认仍需要当前快照和会话标识。
void Basic2DCanvas::requestClose()
{
    m_closeCancelled = false;
    m_closeConfirmed = false;
    m_isOpen         = false;
}

/// @brief 读取并消费最近一次关闭取消结果。
/// @return 用户在保存确认中选择取消时返回 true，且仅返回一次。
/// @details exchange 保证上层关闭编排不会在后续帧重复处理同一次取消。
bool Basic2DCanvas::consumeCloseCancelled()
{
    return std::exchange(m_closeCancelled, false);
}

/// @brief 请求在后续更新中将画布停靠到编辑器中心节点。
/// @details 标志会一直保留到 ImGui 确认窗口已停靠，
/// 因此节点尚未建立或窗口暂时隐藏时不会丢失请求。
void Basic2DCanvas::requestDockToCenter()
{
    m_shouldDockToCenter = true;
}

/// @brief 请求下一次更新时将画布窗口聚焦到前台。
/// @details 聚焦请求在下一次 Begin 前消费；欢迎页覆盖 Logo 占位页时
/// 会有意跳过 SetNextWindowFocus，避免启动欢迎页一闪而过。
void Basic2DCanvas::requestFocus()
{
    m_shouldFocusNextFrame = true;
}

/// @brief 获取画布当前所在的 ImGui Dock 节点。
/// @return 当前窗口停靠节点 ID；未停靠时返回 0。
/// @details 返回值由最近一次可见 update 捕获，供其它窗口与当前画布
/// 协调停靠位置，不用于判断会话是否活动。
ImGuiID Basic2DCanvas::getDockId() const
{
    return m_lastDockId;
}

/// @brief 判断当前帧是否需要准备画布快照。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要准备时返回 true。
/// @details 只有已建立同步缓冲且标签真实可见时参与并行准备。
/// 即使窗口已收到关闭请求，保存确认或脏快照仍需要最后一份数据，
/// 因此这些状态会暂时维持快照消费直到用户完成决策。
/// @warning UI 调度热路径：每帧调用，只读取稳定标志与当前快照状态。
bool Basic2DCanvas::needsParallelUiPrepare(
    const UI::UiFrameSnapshot& snapshot) const
{
    (void)snapshot;
    return m_syncBuffer && m_isCanvasVisible &&
           (m_isOpen || m_showSaveConfirm ||
            (m_currentSnapshot && m_currentSnapshot->isDirty));
}

/// @brief 在线程池中拉取并准备画布渲染快照。
/// @param snapshot 当前帧 UI 快照。
/// @details 普通主画布以 preview=false 调用共享准备 helper，
/// 复用上一份偏移快照和已应用 Y 偏移以平滑连续滚动。
/// 准备槽位与当前绘制槽位分离，本函数不触碰 ImGui 或 Vulkan 状态。
/// @warning 并行 UI 准备路径：不得等待逻辑线程或修改会话容器。
void Basic2DCanvas::prepareUiFrameData(const UI::UiFrameSnapshot& snapshot)
{
    (void)snapshot;
    m_preparedSnapshot = prepareCanvasSnapshot(
        m_syncBuffer.get(), m_lastOffsetSnapshot, m_lastAppliedYOffset, false);
    m_hasPreparedSnapshot = true;
}

/// @brief 将准备好的画布快照切换到主线程可见状态。
/// @details 当前快照、偏移基线与已应用 Y 偏移来自同一准备代际，
/// 必须在帧边界成组替换；空快照会同时清除偏移状态。
/// @warning UI 帧边界热路径：只交换快照引用，不进行文件或 GPU 操作。
void Basic2DCanvas::swapPreparedUiFrameData()
{
    // 调度器在无准备任务的帧也可能调用交换；保持旧快照可避免
    // 短暂遮挡或 Dock 标签切换造成不必要的内容清空。
    if ( !m_hasPreparedSnapshot ) {
        return;
    }

    // 三项准备结果作为单一逻辑状态提交，命令录制不会看到一半新、
    // 一半旧的滚动补偿数据。
    m_currentSnapshot     = m_preparedSnapshot.snapshot;
    m_lastOffsetSnapshot  = m_preparedSnapshot.offsetSnapshot;
    m_lastAppliedYOffset  = m_preparedSnapshot.appliedYOffset;
    m_hasPreparedSnapshot = false;

    if ( !m_currentSnapshot ) {
        // 没有当前快照时旧偏移不能成为下一代增量基线。
        m_lastOffsetSnapshot = nullptr;
        m_lastAppliedYOffset = 0.0f;
    }
}

/// @brief 判断主画布是否需要参与本帧 UI 更新。
/// @return 当前 Dock 标签真实可见时返回 true。
/// @warning UI 调度热路径：只读取由 update 缓存的可见性。
bool Basic2DCanvas::isDirty() const
{
    return m_isCanvasVisible;
}

/// @brief 判断是否应为当前画布录制离屏命令。
/// @return 当前 Dock 标签真实可见时返回 true。
/// @details 与 isDirty 使用同一可见性，确保后台标签既不更新交互，
/// 也不提交无用的 Vulkan 离屏渲染工作。
bool Basic2DCanvas::shouldRecordOffscreen() const
{
    return m_isCanvasVisible;
}

/// @brief 查询窗口在会话关闭规则下的有效打开状态。
/// @return 窗口必须继续存在或原始 open 标志为 true 时返回 true。
/// @details 最后会话重置与脏谱面确认都可暂时覆盖 m_isOpen=false，
/// 使上层不会在 update 完成关闭编排之前提前销毁 Canvas。
bool Basic2DCanvas::isOpen() const
{
    if ( shouldKeepOpenForLastSessionReset() ) {
        return true;
    }
    if ( !m_isOpen && !m_closeConfirmed && m_currentSnapshot &&
         m_currentSnapshot->isDirty ) {
        // 尚未确认的脏快照必须保留窗口对象以呈现保存模态框。
        return true;
    }
    return m_isOpen;
}

/// @brief 判断关闭请求是否应转换为最后会话的 Logo 占位重置。
/// @return 当前 Canvas 是唯一真实会话且已请求关闭时返回 true。
/// @details 多会话可直接移除标签；只有最后一个会话保留编辑器骨架。
/// 已经是 Logo 占位页时返回 false，避免反复执行同一重置。
bool Basic2DCanvas::shouldKeepOpenForLastSessionReset() const
{
    if ( m_isOpen ) {
        return false;
    }

    auto& engine = Logic::EditorEngine::instance();
    if ( engine.getSessionCount() != 1 ) {
        // 规则只适用于全局唯一会话，不能仅凭当前索引位于末尾判断。
        return false;
    }

    const auto* entry = engine.getSessionEntry(0);
    return entry && entry->cameraId == m_cameraId && !entry->isLogoPlaceholder;
}

/// @brief 取得当前 ImGui 字体图集的垂直栅格缩放。
/// @return 有效正缩放；平台后端报告异常值时回退为 1。
/// @details 字体纹理重载只需关注 framebuffer 的 Y 缩放，
/// 与 ImGui 构建字体图集时采用的栅格密度保持一致。
float Basic2DCanvas::currentFontRasterScale()
{
    const float scale = ImGui::GetIO().DisplayFramebufferScale.y;
    return std::isfinite(scale) && scale > 0.0F ? scale : 1.0F;
}

/// @brief 将主画布离屏目标尺寸变化通知逻辑相机。
/// @param oldW 变化前物理宽度。
/// @param oldH 变化前物理高度。
/// @param w 变化后物理宽度。
/// @param h 变化后物理高度。
/// @details 事件通过 cameraId 路由，UI 不直接修改 CanvasCamera 投影。
/// @warning 低频尺寸变化路径：只发布事件，不同步等待逻辑更新。
void Basic2DCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                               uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_cameraId;
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    Event::EventBus::instance().publish(e);
}

/// @brief 汇总字体偏好、DPI 与缺失字形产生的资源重载请求。
/// @return 本次是否需要重载，并同时消费内部重载标志。
/// @details ASCII/CJK 字体路径或栅格缩放变化会使整个字体图集失效；
/// 逻辑快照还可按需请求当前可见标签缺失的 Unicode 码点。
/// 已有字形和已经排队的码点都会去重，避免每帧重复触发重载。
/// @warning UI 热路径：常规帧只比较设置与有限请求数组；
/// 真正的字体文件读取和图集构建由后续低频重载阶段完成。
bool Basic2DCanvas::needReload()
{
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    const auto& currentAsciiFont = settings.preferredAsciiFont;
    const auto& currentCjkFont   = settings.preferredCjkFont;
    if ( currentAsciiFont != m_loadedAsciiFontPreference ) {
        // 偏好值与上次成功加载值比较，失败重载会在下一帧继续请求。
        m_needReload = true;
    }
    if ( currentCjkFont != m_loadedCjkFontPreference ) {
        m_needReload = true;
    }
    if ( std::abs(currentFontRasterScale() - m_loadedFontRasterScale) >
         1e-3F ) {
        // 容差过滤平台后端微小浮点波动，只响应实际 DPI 迁移。
        m_needReload = true;
    }
    if ( m_currentSnapshot ) {
        // 逻辑线程只回报当前可见标签真正缺失的码点；收到新码点后才触发
        // 低频图集重建，避免每帧扫描整个项目资源表。
        for ( std::size_t index = 0U;
              index < m_currentSnapshot->requestedUnicodeGlyphCount;
              ++index ) {
            const auto codepoint =
                m_currentSnapshot->requestedUnicodeGlyphs[index];
            if ( m_unicodeFontMetrics.glyph(codepoint) ||
                 std::find(m_requestedUnicodeCodepoints.begin(),
                           m_requestedUnicodeCodepoints.end(),
                           codepoint) != m_requestedUnicodeCodepoints.end() ) {
                continue;
            }
            // 只追加尚未加载且尚未排队的码点；vector 规模受快照请求
            // 上限约束，字体重建成功后由资源路径统一更新度量。
            m_requestedUnicodeCodepoints.push_back(codepoint);
            m_needReload = true;
        }
    }
    // 消费式返回避免渲染器在同一请求上重复进入资源重建。
    return std::exchange(m_needReload, false);
}

}  // namespace MMM::Canvas
