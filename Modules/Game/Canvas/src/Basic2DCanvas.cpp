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
#include "canvas/Basic2DCanvas.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/CanvasTabTitle.h"
#include "canvas/CollaborationPeerColor.h"
#include "canvas/CollaborationViewportProjection.h"
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
