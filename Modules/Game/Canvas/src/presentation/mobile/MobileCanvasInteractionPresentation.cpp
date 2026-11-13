#include "MobileCanvasInteractionPresentation.h"
#include "MobileCanvasGesture.h"
#include "MobileCanvasNavigation.h"
#include "MobileNavigationRouting.h"
#include "MobileNoteHandles.h"
#include "MobilePointerInputScope.h"
#include "MobilePointerOwnership.h"
#include "MobilePolylineNodeMenu.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/PlanarObservationInput.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/logic/LogicCommandEvent.h"
#include "logic/EditorEngine.h"
#include "presentation/CanvasInteractionPresentation.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <glm/vec2.hpp>
#include <imgui_internal.h>
#include <optional>
#include <string>

namespace MMM::Canvas
{
namespace
{

/// @brief 触摸画布专用交互，不包含桌面右键菜单布局或修饰键实现。
class MobileCanvasInteractionPresentation final
    : public CanvasInteractionPresentation
{
public:
    /// @brief 触屏不绘制鼠标跟随详情，经典与纯鼠标方案保留原检视。
    /// @warning 每帧只读操作方案能力，不能等待逻辑线程或创建提示状态。
    bool allowsHoverInspection() const override
    {
        // 按住把手的简短说明由移动呈现绘制，避免两层提示同时遮挡画布。
        return !UI::Interaction::activeProfile().touchGestures();
    }
    /// @brief 等待中的手势也保有指针，防止摄像机在长按期间平移。
    bool hasGesture() const override
    {
        return m_gesture.active() || m_navigation.active() ||
               m_ownership.active(Graphic::mobileTouchSnapshot());
    }
    /// @brief 先判断触摸意图，再把已经确定的编辑边沿交给原控制器。
    /// @param canvas 同步借用当前画布，策略不拥有会话或渲染快照。
    /// @param manager 菜单动作执行时借用，不留存到下一帧。
    /// @param snapshot
    /// 当前画布发布的不可变显示数据，菜单身份必须来自同一份数据。
    /// @param frame 已通过外部操作方案路由的画布屏幕边界。
    /// @param width 原有投影使用的内容宽度，策略不重新计算轨道几何。
    /// @param height 原有投影使用的内容高度，不包含页签栏。
    /// @details 仅移动目标编译本类，桌面无需运行时判断触屏能力。
    /// 待定按下时也调用普通控制器更新位置，但看不到编辑左键。
    /// 长按成立前没有 StartMarquee、StartBrush 或颜色修改命令。
    /// 因此菜单关闭无需通过 Undo 恢复，已有选区和用户历史均保持原样。
    /// 已认定的拖动直接继续原控制器，撤销事务与释放提交路径保持不变。
    /// 弹窗打开后冻结菜单位置，菜单按钮上的移动不会改变粘贴吸附时间。
    /// 谱面实例参与校验，同路径文件重新加载也不能使用旧菜单目标。
    /// 播放中禁用编辑入口，仍由正式动作处理器复核权限与目标。
    /// 菜单操作不重复初始化动作注册表，也不保存 UIManager 的所有权。
    /// @warning 每帧固定状态更新；取消仅发命令，不读取 ECS 或等待逻辑线程。
    void update(Basic2DCanvasInteraction& canvas, UI::UIManager* manager,
                const Common::Render::RenderSnapshot* snapshot,
                const UI::Interaction::SurfaceFrame& frame, float width,
                float height) override
    {
        // 安全帧完成方案切换后清除旧方案弹层，不能让触控菜单沿用鼠标菜单身份。
        // 仅在代际变化时关闭；常态帧不能反复关闭纯鼠标方案自己打开的弹层。
        const auto generation = UI::Interaction::profileGeneration();
        if ( m_generation != generation ) {
            m_generation = generation;
            m_gesture.cancelPending();
            m_handle.reset();
            auto ctx = context(canvas);
            ImGui::PushID(ctx.cameraId.c_str());
            if ( ImGui::BeginPopup("SelectionEditMenu") ) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
            m_waitRelease = false;
        }
        // 桌面解释器随画布创建一次，能力变化不在热路径上分配新解释器。
        // 原始指针输入直接透传，不让长按阈值延迟经典方案的起笔。
        // 未激活的解释器不消费本帧事件，两个状态机不能同时响应一次按下。
        // 操作方案决定是否解释触控，安卓平台本身不再隐式启用长按菜单。
        // 非触控方案复用完整桌面解释器，保留经典右键与纯鼠标扩展菜单。
        if ( !UI::Interaction::activeProfile().touchGestures() ) {
            m_desktop->update(canvas, manager, snapshot, frame, width, height);
            return;
        }
        auto ctx = context(canvas);
        ImGui::PushID(ctx.cameraId.c_str());
        const bool menuOpen = ImGui::IsPopupOpen("SelectionEditMenu");
        ImGui::PopID();
        // 屏幕边界与窗口悬浮共同限定新候选；页签和抽屉留白不属于编辑区。
        // 使用屏幕逻辑像素，容差随后按窗口内容倍率转换，不猜测设备分辨率。
        if ( m_handle && (m_handleInstance != snapshot->beatmapInstanceId ||
                          snapshot->currentTool != Logic::EditTool::Marquee) ) {
            // 场景切换不能把旧部件身份带入新谱面，也不能交给下一工具提交。
            cancelInput(canvas, *snapshot);
            m_gesture.cancel();
            m_handle.reset();
        }
        const auto point = ImGui::GetMousePos();
        // 仅选择方案显示把手；原点命中锁存到整次手势结束，不能随指针切换实体。
        // 播放及弹层接管时隐藏目标，已有取消桥负责回退未提交的事务。
        // 把手不建立独立 ImGui 控件，避免控制器与按钮同时消费同一次释放。
        // 待定触点只保存值身份；跨过原有容差后才开始正式编辑。
        // 头尾采用不同 HoverPart，长条主体的整体移动无需额外模式切换。
        // 当前绘制快照只负责提供可见范围，最终存在性仍由逻辑层检查。
        const bool handlesAllowed =
            snapshot->hasBeatmap && !snapshot->isPlaying &&
            snapshot->currentTool == Logic::EditTool::Marquee && !menuOpen &&
            !UI::workspaceBlocksCanvasInput() &&
            !ImGui::IsPopupOpen(
                nullptr,
                ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if ( handlesAllowed ) {
            const auto hit = MobileNoteHandles::render(
                snapshot->hitboxes,
                { frame.m_x, frame.m_y },
                point,
                Config::AppConfig::instance().getWindowContentScale(),
                frame.m_projection,
                frame.m_snapshotYOffset,
                m_handle ? &*m_handle : nullptr,
                ctx.preferred ? &*ctx.preferred : nullptr);
            // 延迟回放期间忽略后来触点，旧手势不能借用新手指的命中。
            // 即使快照提前刷新，活动事务仍持有开始时的部件身份。
            // 空白起点明确清空旧身份，后续跨过把手也只能继续框选。
            // 真正的系统取消由所有权状态机优先处理，不当作正常抬手。
            if ( ImGui::IsMouseClicked(0) && !m_gesture.active() ) {
                // 被其它窗口遮住的画布不能借用几何把手发布悬浮身份。
                m_handle         = frame.m_hovered ? hit : std::nullopt;
                m_handleInstance = snapshot->beatmapInstanceId;
            }
        } else
            m_handle.reset();
        const bool eligible = snapshot->hasBeatmap && !snapshot->isPlaying &&
                              frame.m_hovered && point.x >= frame.m_x &&
                              point.y >= frame.m_y &&
                              point.x < frame.m_x + frame.m_width &&
                              point.y < frame.m_y + frame.m_height &&
                              !UI::workspaceBlocksCanvasInput();
        // 原始输入与 ImGui 鼠标可能相差一个队列帧，接管不能依赖 MouseDown。
        // 代际峰值保留到全部抬起后，防止快捏合被拆成一次延迟轻点。
        // 坐标从窗口归一化输入换成逻辑像素，不能使用 framebuffer 分辨率。
        // 多画布只认第一指起点，不随第二指落点切换会话。
        // 抽屉遮罩属于外部中断，越出原矩形仅是正常的连续拖动。
        // 菜单里的新触点交给控件，不能成为被遮住画布的新候选。
        // 取消桥通过原命令队列排在上次预览之后，不等待后台确认。
        // 结束代际仍屏蔽旧鼠标边沿，但不会永久持有方案切换门闩。
        // 单指路径继续复用起点回放，拾取与显示投影不用复制一份实现。
        const auto& raw     = Graphic::mobileTouchSnapshot();
        const auto  display = ImGui::GetIO().DisplaySize;
        // 通用属性弹层同样遮住画布，不能仅识别本类自己的长按菜单。
        // 任意层弹窗都先取得输入，关闭后仍需一次新的触摸代际。
        const bool overlay = ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const float originX = raw.m_originX * display.x;
        const float originY = raw.m_originY * display.y;
        const bool  acceptsOrigin =
            snapshot->hasBeatmap && !snapshot->isPlaying && !overlay &&
            frame.m_hovered && !UI::workspaceBlocksCanvasInput() &&
            originX >= frame.m_x && originX < frame.m_x + frame.m_width &&
            originY >= frame.m_y && originY < frame.m_y + frame.m_height;
        // 初始归属还要通过真实窗口命中，避免导航穿透同位置的音频等浮层。
        // 越出画布不取消已认定的拖动；遮罩、播放或系统取消才中断事务。
        const bool interrupted = !snapshot->hasBeatmap || snapshot->isPlaying ||
                                 UI::workspaceBlocksCanvasInput() || overlay;
        const bool taken = m_ownership.update(raw, acceptsOrigin, interrupted);
        // 新触点从画布外开始时，也要丢弃上一轻点尚未完成的回放。
        // 外部手势不能令旧回放永久占有画布，或在之后误补发释放。
        if ( taken || (m_ownership.blocksSingle() && m_gesture.active()) ) {
            cancelInput(canvas, *snapshot);
            m_gesture.cancel();
            m_handle.reset();
        }
        navigate(raw,
                 m_ownership.ownsGesture() && !interrupted,
                 ctx.cameraId,
                 frame,
                 *snapshot,
                 width,
                 height);
        auto output = MobileCanvasGesture::Output::Suppress;
        if ( !menuOpen ) {
            // 先发布正常悬浮位置，长按菜单才能使用当前触点的吸附时间。
            // 输入作用域只保护原画布调用，弹窗始终读取真实按下与释放状态。
            const float scale =
                Config::AppConfig::instance().getWindowContentScale();
            // 双击计数仅在原生按下时锁存，延迟到释放回放时 IO 计数已清零。
            if ( ImGui::IsMouseClicked(0) )
                m_clickCount = ImGui::GetIO().MouseClickedCount[0];
            // 绘制只在移动或释放后起笔，静止按住不能被选择菜单夺走。
            // 其他工具仍可长按，桌面回退路径不经过这个触控开关。
            if ( !m_ownership.blocksSingle() )
                output = m_gesture.update(
                    ImGui::IsMouseClicked(0),
                    ImGui::IsMouseDown(0),
                    ImGui::IsMouseReleased(0),
                    point,
                    ImGui::GetTime(),
                    10.0f * scale,
                    eligible || (raw.m_epoch != 0 && m_gesture.active() &&
                                 !interrupted),
                    snapshot->currentTool != Logic::EditTool::Draw);
            // 轻点的 Begin/Release
            // 回放仍保留落点，不能因原始触点已抬起吞掉提交。
            // 多指导航和真正结束的空闲帧才撤去普通鼠标悬浮反馈。
            const bool hidePointer =
                raw.m_epoch != 0 && (m_ownership.blocksSingle() ||
                                     (raw.m_count == 0 && !m_gesture.active()));
            MobilePointerInputScope input(
                output,
                m_gesture.replayPoint(),
                snapshot->currentTool == Logic::EditTool::Marquee
                    ? 1
                    : m_clickCount,
                hidePointer);
            // 选择工具的连续轻点只选择，不借用桌面双击 seek。
            // 拖动起点仍回放原命中；快速释放由桥补交最后矩形或物件位置。
            // 触控容差内的抖动没有完成拖动标志，因此不会改变物件数据。
            // 轻点只执行选择，不能沿用 StartDrag/EndDrag 制造空撤销记录。
            // 已排队的释放可能与下一触点同帧到达，使用旧手势的收尾分类。
            const bool tapOnly =
                (snapshot->currentTool == Logic::EditTool::Marquee ||
                 snapshot->currentTool == Logic::EditTool::Draw) &&
                ((output == MobileCanvasGesture::Output::Begin &&
                  !m_gesture.dragging()) ||
                 (output == MobileCanvasGesture::Output::Release &&
                  !m_gesture.finishedDrag()));
            routeInput(canvas,
                       snapshot,
                       width,
                       height,
                       true,
                       (snapshot->currentTool == Logic::EditTool::Marquee ||
                        snapshot->currentTool == Logic::EditTool::Draw) &&
                           m_gesture.finishedDrag(),
                       tapOnly,
                       false,
                       m_handle ? &*m_handle : nullptr);
            // 释放路由已完成，空闲帧必须清除把手覆盖，避免触摸结束仍有 Hover。
            // 显式悬浮撤销由同步桥处理；本地锁存也在同一收尾边沿清空。
            // 连续拖动沿用 Pass 中的真实 Up；快速回放才返回专用 Release。
            // 两种收尾必须同时清锁存，不能只覆盖快速拖动用例。
            if ( output == MobileCanvasGesture::Output::Release ||
                 (output == MobileCanvasGesture::Output::Pass &&
                  !ImGui::IsMouseDown(0)) )
                m_handle.reset();
            if ( output == MobileCanvasGesture::Output::Begin &&
                 m_gesture.dragging() &&
                 snapshot->currentTool == Logic::EditTool::Draw &&
                 !UI::Interaction::activeProfile().noteHoldBrush(
                     UI::Interaction::Surface::MainCanvas) ) {
                // 同一 UI 帧按原点起笔、实际点更新的顺序入队，保留第一次转向。
                // 回放只包围公共指针入口，不向原始事件队列插入任何合成触摸。
                // 简单长条和桌面回退不经过此路径，菜单与其它窗口仍读真实输入。
                MobilePointerInputScope continued(
                    MobileCanvasGesture::Output::Pass, point, 1, false, true);
                routeInput(
                    canvas, snapshot, width, height, true, false, false, true);
            }
        } else {
            // 弹层接管后不补发候选轻点，编辑起笔从未发生，无需发送 End 或
            // Cancel。
            m_gesture.cancelPending();
        }
        ImGui::PushID(ctx.cameraId.c_str());
        if ( output == MobileCanvasGesture::Output::Menu ) {
            ctx.pasteTime =
                snapshot->isSnapped &&
                        !UI::Interaction::activeProfile().bypassSnapping()
                    ? snapshot->snappedTime
                    : snapshot->hoveredTime;
            ctx.menuPoint       = ctx.pointerPoint;
            ctx.beatmapKey      = snapshot->beatmapPathKey;
            ctx.beatmapInstance = snapshot->beatmapInstanceId;
            ctx.layers          = ctx.candidates;
            // 扩大的把手区域可以位于原框外，菜单也必须知道这次锁定的部件。
            if ( m_handle &&
                 std::find(ctx.layers.begin(), ctx.layers.end(), *m_handle) ==
                     ctx.layers.end() )
                ctx.layers.insert(ctx.layers.begin(), *m_handle);
            // 旧优先层必须属于本次长按候选，不能跨位置串用。
            // 扩大的把手可能在原框外，所以显式补入冻结目标。
            // 节点令牌保留到命令线程，不仅靠下标识别。
            const auto validPreferred =
                ctx.preferred && std::find(ctx.layers.begin(),
                                           ctx.layers.end(),
                                           *ctx.preferred) != ctx.layers.end()
                    ? ctx.preferred
                    : std::nullopt;
            const auto target = validPreferred ? validPreferred
                                : m_handle     ? m_handle
                                : ctx.layers.empty()
                                    ? std::nullopt
                                    : std::optional{ ctx.layers.front() };
            m_nodeMenu.capture(*snapshot, target);
            m_waitRelease = true;
            ImGui::OpenPopup("SelectionEditMenu");
        }
        if ( ImGui::BeginPopup("SelectionEditMenu") ) {
            const bool same =
                ctx.beatmapKey == snapshot->beatmapPathKey &&
                ctx.beatmapInstance == snapshot->beatmapInstanceId;
            // 已切换会话的菜单立即禁用并关闭，旧实体与粘贴时间不得跨谱面使用。
            if ( !same || !snapshot->hasBeatmap ) ImGui::CloseCurrentPopup();
            ImGui::BeginDisabled(!same || snapshot->isPlaying ||
                                 !snapshot->hasBeatmap);
            renderMenu(canvas, manager, *snapshot, ctx);
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

private:
    /// @brief 起笔把手值身份，接管和方案切换立即清除，不借用旧快照。
    std::optional<HoverLayerCandidate> m_handle;
    /// @brief 节点长按只持有值载荷，切换谱面后外层关闭旧弹窗。
    MobilePolylineNodeMenu m_nodeMenu;
    /// @brief 实例身份隔离同路径重新载入，旧实体编号不跨会话复用。
    std::uintptr_t m_handleInstance{};
    /// @brief 多指导航只发布增量和最终倍率，不修改键盘状态或借用桌面中键。
    /// @warning 每帧固定数学，只有释放边沿更新配置值；不取得会话长锁。
    /// @param raw 原始手势值快照，只在本次 UI 栈内借用。
    /// @param allowed 包含画布起点归属、播放与弹层阻挡的最终导航许可。
    /// @param camera 命令绑定原始画布，焦点改变不允许重投活动会话。
    /// @param frame 内容区原点与逆投影矩阵，不能保存到下一帧。
    /// @param snapshot 本帧显示缩放，用于消除预览压缩等附加比例。
    /// @param width 逻辑画布宽度，与普通指针命令使用相同投影域。
    /// @param height 逻辑画布高度，不包含页签或窗口边框。
    /// @details 原始坐标先换成屏幕逻辑像素，再反投影到谱面平面。
    /// 二维和三维共用命令；没有有效平面交点时不制造替代落点。
    /// 最终设置只提交已经实际显示过的倍率，不能提交被投影拒绝的目标。
    /// 配置值只在释放边沿发布，连续帧不复制完整设置或保存项目文件。
    /// 所有权仍由 UI 外壳管理，本方法不创建会话或后台任务。
    void navigate(const Graphic::MobileTouchSnapshot& raw, bool allowed,
                  const std::string&                    camera,
                  const UI::Interaction::SurfaceFrame&  frame,
                  const Common::Render::RenderSnapshot& snapshot, float width,
                  float height)
    {
        // 路由 helper 与 Timeline 共用固定大小的原始多指分类，不共享事务实例。
        // camera 仍来自当前主画布，不能在焦点改变后把导航发往另一个标签。
        // 三维画布继续提供原投影，提取函数不改变主画布的坐标反算。
        // helper 仅发布命令，不保存 canvas、manager 或 snapshot 的所有权。
        // 已经取消的单指笔画不会因导航手势收尾而恢复。
        // 第二指先抬起即结束导航，第一指残留仍由 ownership 门闩屏蔽。
        // 连续值以局部导航倍率为基线，不能被后台较慢的快照回流反向覆盖。
        // 最终倍率只在结束边沿写配置，不逐帧复制 EditorConfig。
        // 本函数保留原调用位置，始终在单指回放之前判断导航接管。
        // 无有效地平面交点时不制造替代坐标，三维边界行为保持不变。
        // 两指与三指轻触的历史分类仍在同一原始代际结束后判断。
        // 系统 Cancel 不解释为重做，避免设备截图手势意外修改谱面。
        // 公共 helper 只有移动私有调用方可见，不进入桌面公共头文件。
        routeMobileNavigation(m_navigation,
                              m_navigationZoom,
                              raw,
                              allowed,
                              camera,
                              frame,
                              snapshot,
                              width,
                              height);
    }
    /// @brief 长按弹层复用桌面业务动作，布局与触摸释放保护仅在移动端编译。
    /// @param canvas 用于关联批注和重叠层菜单，不能切到其他画布。
    /// @param manager 短期同步动作接收者，不参与手势状态生命周期。
    /// @param snapshot 当前不可变快照，只提供播放和谱面身份等展示条件。
    /// @param ctx 打开时锁存的位置及候选，所有引用只在调用期间有效。
    /// @details 菜单只有下一次独立触点的松手才允许激活动作。
    /// 原长按的按住及释放帧都禁用，防止弹层出现在手指下立刻执行删除。
    /// 两列按钮保留固定宽度；较长本地化标签由移动按钮内部换行。
    /// 依赖选区的入口动态禁用，粘贴和历史操作仍允许无选择时使用。
    /// 粘贴时间随这次激活载荷传递，不能覆盖全局粘贴偏好。
    /// 关闭菜单只是 UI 行为，命令执行继续使用正式动作的撤销及联机路径。
    /// 清空选择与删除分开，前者只解除高亮、不生成谱面数据修改。
    /// 专业模式物件音量维持桌面门禁，菜单不尝试提升操作能力。
    /// 时间戳批注使用已存在编辑器缓冲，最终保存前不写入任何对象。
    /// @warning 每帧绘制固定按钮，不主动读取剪贴板或复制谱面。
    void renderMenu(Basic2DCanvasInteraction& canvas, UI::UIManager* manager,
                    const Common::Render::RenderSnapshot& snapshot,
                    CanvasMenuContext&                    ctx)
    {

        // 长按刚弹出时手指仍按在屏幕上，必须等待该触点及释放帧彻底结束后才允许激活按钮。
        // 连释放帧一并排除，保证只有下一次独立的轻点触摸才能触发菜单中的操作按钮。
        if ( !ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0) ) {
            m_waitRelease = false;
        }
        ImGui::BeginDisabled(m_waitRelease);

        // 若长按起点存在多个重叠物件部位，顶部仍保留重叠层优先选择入口，
        // 方便触屏用户在密集折线节点或音符与采样重叠处精确切换目标层。
        if ( ctx.layers.size() > 1 ) {
            renderLayers(canvas, snapshot);
            ImGui::Separator();
        }

        // 按钮尺寸随窗口内容缩放自适应，单列宽度留出充足图文空间，高度满足 44dp
        // 触控下限。
        const float scale = std::max(
            1.0f, Config::AppConfig::instance().getWindowContentScale());
        const float  gap        = ImGui::GetStyle().ItemSpacing.x;
        const float  cellWidth  = 136.0f * scale;
        const float  cellHeight = 64.0f * scale;
        const ImVec2 buttonSize{ cellWidth, cellHeight };

        // 读取逻辑线程已发布的选区标志与专业模式配置，不扫描 ECS 注册表。
        // 空选区时仅禁用依赖选区的破坏性或属性类操作，保留粘贴、撤销、重做、全选和批注可用。
        const bool hasSelection =
            Logic::EditorEngine::instance().hasActiveChartObjectSelection();
        const bool professionalMode =
            Config::AppConfig::instance().getEditorSettings().professionalMode;

        // 统一封装单个网格动作按钮：推入稳定 ID
        // 隔离同名标签，并在激活后立即收起菜单。
        // 通过工作区外观代理调用按钮，在移动端呈现带图标的触控卡片，在桌面测试中回退标准按钮。
        const auto actionCell = [&](const char* id,
                                    const char* labelKey,
                                    const char* icon,
                                    bool        enabled = true) -> bool {
            ImGui::PushID(id);
            ImGui::BeginDisabled(!enabled);
            const bool clicked = UI::workspaceActionButton(
                TR(labelKey).data(), icon, buttonSize);
            ImGui::EndDisabled();
            ImGui::PopID();
            // 任一动作激活后主动关闭当前弹窗，无需用户额外点击画布空白区域收起菜单。
            if ( clicked ) {
                ImGui::CloseCurrentPopup();
            }
            return clicked;
        };

        // 节点入口遵循上方显式重叠层选择，菜单本身不修改选区或数据。
        const auto validPreferred =
            ctx.preferred && std::find(ctx.layers.begin(),
                                       ctx.layers.end(),
                                       *ctx.preferred) != ctx.layers.end()
                ? ctx.preferred
                : std::nullopt;
        // 这里只接受本次候选里显式选中的重叠层。
        // 另一位置的旧优先项不能冒充当前节点。
        // 密集重叠先选层，再提供局部破坏性动作。
        // 整物件菜单保持原复制、粘贴与删除语义。
        m_nodeMenu.render(
            snapshot, validPreferred, ctx.layers.size() > 1, buttonSize, gap);

        // 第一行：复制与粘贴（粘贴使用长按锁存的吸附时间作为落点锚点）。
        // 复制仅在存在活动选区时启用；粘贴保持可用，由动作处理器在激活时读取剪贴板内容。
        if ( actionCell(
                 "TouchCopy", "ui.edit.copy", UI::ICON_MMM_COPY, hasSelection) )
            UI::Interaction::executeAction("CopyAction", manager);
        ImGui::SameLine(0.0f, gap);
        if ( actionCell("TouchPaste", "ui.edit.paste", UI::ICON_MMM_PASTE) )
            UI::Interaction::executeAction(
                "PasteAction",
                manager,
                UI::MainMenuItemActivation{
                    .m_source =
                        UI::MainMenuActivationSource::InteractionProfile,
                    .m_pasteTime = ctx.pasteTime });

        // 剪切必须经过既有动作，保证复制成功与删除的原有顺序和格式。
        // 删除不直接访问 ECS，逻辑线程负责选择范围、权限与撤销记录。
        // 第二行：剪切与删除选中物件。
        // 剪切标记来源并在粘贴成功后移除原物件；删除直接向逻辑命令队列发送
        // CmdDeleteSelected。
        if ( actionCell("TouchCut",
                        "ui.edit.cut",
                        UI::ICON_MMM_SCISSORS,
                        hasSelection) )
            UI::Interaction::executeAction("CutAction", manager);
        ImGui::SameLine(0.0f, gap);
        if ( actionCell("TouchDelete",
                        "ui.settings.shortcut.delete_selected",
                        UI::ICON_MMM_ERASER,
                        hasSelection) )
            UI::Interaction::dispatchCommand(
                Event::LogicCommandEvent(Logic::CmdDeleteSelected{}));

        // 历史动作读取当前会话的撤销栈，不在 UI 中缓存能否撤销的过期状态。
        // 关闭菜单后新快照按正常通道发布，不阻塞等待命令执行完成。
        // 第三行：撤销与重做，归属当前活动谱面历史栈。
        // 空历史栈由底层会话安静忽略，不在每帧查询撤销栈深度。
        if ( actionCell("TouchUndo", "ui.edit.undo", UI::ICON_MMM_UNDO) )
            UI::Interaction::executeAction("UndoAction", manager);
        ImGui::SameLine(0.0f, gap);
        if ( actionCell("TouchRedo", "ui.edit.redo", UI::ICON_MMM_REDO) )
            UI::Interaction::executeAction("RedoAction", manager);

        // 全选继续采用当前轨道范围，不能把手机屏幕宽度误当作谱面范围。
        // 镜像使用正式动作，不在触摸层手工改写轨道编号或折线节点。
        // 第四行：全选与原地水平镜像。
        // 全选复用移动端全轨道选择语义，避免因触点最后停留在某一分区而漏选其他轨道物件。
        if ( actionCell("TouchSelectAll",
                        "ui.mobile.select_all",
                        UI::ICON_MMM_SELECT_ALL) )
            UI::Interaction::executeAction("SelectAllObjectsAction", manager);
        ImGui::SameLine(0.0f, gap);
        if ( actionCell("TouchMirror",
                        "ui.edit.mirror",
                        UI::ICON_MMM_MIRROR,
                        hasSelection) )
            UI::Interaction::executeAction("MirrorAction", manager);

        // 镜像粘贴与普通粘贴共用锁存时间，避免随菜单位置变化漂移。
        // 批注以是否存在活动选区区分关联物件和独立时间戳两种既有流程。
        // 第五行：镜像粘贴与添加批注（有选中物件时批注关联物件，无选中时在长按时间新建批注）。
        // 镜像粘贴在一个独立历史事务中完成粘贴与轨道水平翻转，锚定长按触发时的吸附时间。
        if ( actionCell("TouchMirrorPaste",
                        "ui.edit.mirror_paste",
                        UI::ICON_MMM_PASTE) )
            UI::Interaction::executeAction(
                "MirrorPasteAction",
                manager,
                UI::MainMenuItemActivation{
                    .m_source =
                        UI::MainMenuActivationSource::InteractionProfile,
                    .m_pasteTime = ctx.pasteTime });
        ImGui::SameLine(0.0f, gap);
        if ( actionCell("TouchAnnotate",
                        "ui.mobile.annotate",
                        UI::ICON_MMM_COMMENT) ) {
            if ( hasSelection ) {
                // 存在选中物件时沿用关联物件批注流程，保留物件范围确认。
                UI::Interaction::executeAction(
                    "AddSelectedObjectAnnotationAction", manager);
            } else {
                // 无选中物件时直接复用画布时间戳批注弹窗，负预卷时间夹紧到零。
                // 清空 annotationId 与 author
                // 表示本次操作为新建独立时间戳批注而非编辑旧批注。
                requestAnnotation(canvas, ctx.pasteTime);
            }
        }

        // 属性与音量只在确认后修改，弹层本身不复制选择对象集合。
        // 两种属性入口沿用原业务窗口，触控层只负责可发现的按钮。
        // 第六行：编辑物件元数据与专业模式物件音量。
        // 物件音量仅在专业模式开启且存在选中物件时开放，与桌面顶栏和属性菜单保持同一门禁。
        if ( actionCell("TouchMetadata",
                        "ui.edit.note_metadata",
                        UI::ICON_MMM_PEN,
                        hasSelection) )
            UI::Interaction::executeAction("OpenNoteMetadataAction", manager);
        ImGui::SameLine(0.0f, gap);
        if ( actionCell("TouchVolume",
                        "ui.mobile.selected_volume",
                        UI::ICON_MMM_VOLUME_HIGH,
                        hasSelection && professionalMode) )
            UI::Interaction::executeAction("EditSelectedObjectVolumeAction",
                                           manager);

        ImGui::Separator();

        // 底部辅助行：跳转到长按落点时间与清空当前选择。
        // 跳转时间扣除当前生效的视觉偏移，并对非有限退化投影结果做防御性过滤。
        if ( actionCell("TouchSeekHere",
                        "ui.selection.seek_here",
                        UI::ICON_MMM_PLAY) ) {
            const double offset    = Config::AppConfig::instance()
                                         .getVisualConfig()
                                         .getEffectiveVisualOffset();
            const double audioTime = ctx.pasteTime - offset;
            if ( std::isfinite(ctx.pasteTime) && std::isfinite(audioTime) ) {
                UI::Interaction::dispatchCommand(
                    Event::LogicCommandEvent(Logic::CmdSeek{ audioTime }));
            }
        }
        ImGui::SameLine(0.0f, gap);
        // 清空选择发送空实体选择命令，仅解除高亮而不修改谱面中的任何物件数据。
        // 空选区时禁用该按钮，向用户明确提示当前画布不存在可清除的选中物件。
        if ( actionCell("TouchClearSelection",
                        "ui.selection.clear",
                        UI::ICON_MMM_ERASER,
                        hasSelection) ) {
            UI::Interaction::dispatchCommand(Event::LogicCommandEvent(
                Logic::CmdSelectEntity{ entt::null, true }));
        }

        // 与开头的 BeginDisabled 严格配对，恢复弹窗外层作用域的交互状态。
        ImGui::EndDisabled();
    }
    unsigned short m_clickCount{
        1
    };  ///< 回放保留原生双击计数，长按不伪造双击。
    /// @brief 每画布复用一个桌面解释器，切换方案不创建资源或迁移手势。
    std::unique_ptr<CanvasInteractionPresentation> m_desktop{
        createDesktopCanvasInteractionPresentation()
    };
    /// @brief 每画布缓存方案代际，切换时丢弃已关闭方案的弹层状态。
    std::uint64_t          m_generation{ UI::Interaction::profileGeneration() };
    MobileCanvasGesture    m_gesture;  ///< 每画布独占，不向公共头传播手势类型。
    MobilePointerOwnership m_ownership;  ///< 原始多指接管门闩仅在移动实现可见。
    MobileCanvasNavigation
          m_navigation;  ///< 双指坐标、距离与历史轻触门闩不传播到公共头。
    float m_navigationZoom{
        1.0f
    };  ///< 最后真正送入逻辑侧的倍率，拒绝无效投影的目标。
    bool m_waitRelease{};  ///< 菜单打开的原始触点与释放帧都不能激活按钮。
};
}  // namespace
/// @brief 安卓与 CPU 回归共用同一生产实现，桌面工厂不引用此函数。
std::unique_ptr<CanvasInteractionPresentation>
createMobileCanvasInteractionPresentation()
{
    return std::make_unique<MobileCanvasInteractionPresentation>();
}
}  // namespace MMM::Canvas
