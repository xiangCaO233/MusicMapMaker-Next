#include "MobileCanvasInteractionPresentation.h"
#include "MobileCanvasGesture.h"
#include "MobileCanvasNavigation.h"
#include "MobilePointerOwnership.h"
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
#include <glm/vec2.hpp>
#include <imgui_internal.h>
#include <optional>
#include <string>

namespace MMM::Canvas
{
namespace
{
/// @brief 原控制器调用期间借用一份左键快照，离开作用域立即恢复真实触点。
/// @details 不修改全局事件队列，也不让菜单或其他窗口看到合成边沿。
/// @par 输入字段的成组约束
/// - Down 与 Clicked 共同描述起笔，单独保留任一个都可能漏掉事务。
/// - Released 独立描述收尾，不能在同一次调用里同时起笔又结束。
/// - ClickedPos 只在起笔回放时改变，维持原工具的拖动参照点。
/// - MousePos 在等待阶段保持真实坐标，确保吸附线即时跟随。
/// - 坐标回到原点时清零 MouseDelta，不制造不存在的摄像机位移。
/// - 回放边沿清零累计拖动量，不借用另一物理触点的距离。
/// - 持续时间局部改变，不影响后续按钮的按住动画。
/// - 双击次数保留后端判断，不用触摸计时重新合并点击。
/// - 所有字段在析构恢复，其他画布和浮层仍读取真实设备输入。
/// @note 不调用事件队列，不会制造下一帧残留的合成输入。
/// @note 只能围绕原控制器使用，不能跨过弹层或业务按钮绘制。
/// @note 发布命令后直接返回，不等待逻辑线程应用或回传快照。
/// @note 等待阶段从未向编辑器起笔，因此不是事后撤销事务。
/// @note 多画布独占各自策略，状态不放到全局静态变量中共享。
class TouchInputScope
{
public:
    /// @brief 保存物理状态后呈现本帧已经确定的编辑意图。
    /// @param mode 等待阶段隐藏左键，起笔与收尾分别呈现唯一边沿。
    /// @param point 已锁存的原点或终点，不能使用当前菜单指针代替。
    /// @param count 原生按下时保存的点击次数，延迟回放不重新计算。
    /// @param hidePointer 全部抬起或多指接管时局部清除悬浮，不影响其他控件。
    /// @details 所有修改严格局限于 routeInput 的同步调用栈。
    /// 不调用 AddMouseButtonEvent，因为入队事件要到下一轮 NewFrame 才生效。
    /// 也不直接调用编辑命令，否则会绕过权限、投影和当前工具的事务规则。
    /// 合成 Down 与 Clicked 配对，避免画笔把一个无按住的边沿解释为取消。
    /// Release 独立成帧，让单帧快速触摸也经过正式的起笔及提交生命周期。
    /// 等待阶段 MousePos 保持真实值，吸附线与只读位置反馈仍即时更新。
    /// 双击沿用后端给出的次数，不因 450ms 手势阈值改变桌面双击时限。
    /// 该作用域不执行可见按钮，后续弹层按原始触点处理释放保护。
    /// @warning 每帧栈上构造，仅保存固定大小字段，无堆分配或线程同步。
    TouchInputScope(MobileCanvasGesture::Output mode, ImVec2 point,
                    unsigned short count, bool hidePointer = false)
        : m_io(ImGui::GetIO())
        , m_pos(m_io.MousePos)
        , m_delta(m_io.MouseDelta)
        , m_clickedPos(m_io.MouseClickedPos[0])
        , m_down(m_io.MouseDown[0])
        , m_clicked(m_io.MouseClicked[0])
        , m_released(m_io.MouseReleased[0])
        , m_double(m_io.MouseDoubleClicked[0])
        , m_count(m_io.MouseClickedCount[0])
        , m_duration(m_io.MouseDownDuration[0])
        , m_previous(m_io.MouseDownDurationPrev[0])
        , m_distance(m_io.MouseDragMaxDistanceSqr[0])
    {
        using O = MobileCanvasGesture::Output;
        // Pass 完全保留连续拖动。等待阶段连 Down 都屏蔽，画笔与框选均无法起笔。
        if ( mode == O::Pass ) return;
        m_io.MouseDown[0]          = mode == O::Begin;
        m_io.MouseClicked[0]       = mode == O::Begin;
        m_io.MouseReleased[0]      = mode == O::Release;
        m_io.MouseDoubleClicked[0] = mode == O::Begin && count == 2;
        m_io.MouseClickedCount[0] =
            mode == O::Begin ? std::max<unsigned short>(1, count) : 0;
        m_io.MouseDownDuration[0]       = mode == O::Begin ? 0.0f : -1.0f;
        m_io.MouseDownDurationPrev[0]   = mode == O::Release ? 0.0f : -1.0f;
        m_io.MouseDragMaxDistanceSqr[0] = 0;
        if ( mode == O::Suppress && hidePointer ) {
            // 触摸没有离开后继续悬浮的语义，不能留下桌面 tooltip 和待绘制虚影。
            // 仅在画布调用栈内隐藏；恢复真实 IO 后按钮仍收到正常释放边沿。
            m_io.MousePos   = { -FLT_MAX, -FLT_MAX };
            m_io.MouseDelta = {};
        }
        if ( mode == O::Begin || mode == O::Release ) {
            // 起笔必须位于最初触点，释放必须位于最后触点，不能跳到菜单位置。
            m_io.MousePos   = point;
            m_io.MouseDelta = {};
            if ( mode == O::Begin ) m_io.MouseClickedPos[0] = point;
        }
    }
    /// @brief 恢复原始帧；即使工具提前返回也不会污染后续 UI。
    /// @details 坐标、累计拖动量和持续时间必须一起恢复。
    /// 只恢复 MouseDown 会使之后窗口误判拖动或双击，造成输入穿透。
    /// 原始释放帧仍留给菜单保护使用，不由画布回放消耗系统事件。
    /// 没有后台任务或延迟析构，作用域结束后不存在指向 IO 的新观察者。
    ~TouchInputScope()
    {
        m_io.MousePos                   = m_pos;
        m_io.MouseDelta                 = m_delta;
        m_io.MouseClickedPos[0]         = m_clickedPos;
        m_io.MouseDown[0]               = m_down;
        m_io.MouseClicked[0]            = m_clicked;
        m_io.MouseReleased[0]           = m_released;
        m_io.MouseDoubleClicked[0]      = m_double;
        m_io.MouseClickedCount[0]       = m_count;
        m_io.MouseDownDuration[0]       = m_duration;
        m_io.MouseDownDurationPrev[0]   = m_previous;
        m_io.MouseDragMaxDistanceSqr[0] = m_distance;
    }

private:
    ImGuiIO& m_io;  ///< 只借用 UI 线程当前上下文，不拥有或跨帧保存。
    ImVec2   m_pos, m_delta, m_clickedPos;  ///< 精确还原合成输入前的坐标。
    bool m_down, m_clicked, m_released, m_double;  ///< 左键全部边沿成对保存。
    unsigned short m_count;  ///< 双击计数仍由原生输入层维护。
    float          m_duration, m_previous,
        m_distance;  ///< 拖动阈值和持续时间不泄漏给后续控件。
};
/// @brief 触摸画布专用交互，不包含桌面右键菜单布局或修饰键实现。
class MobileCanvasInteractionPresentation final
    : public CanvasInteractionPresentation
{
public:
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
        const auto point    = ImGui::GetMousePos();
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
            if ( !m_ownership.blocksSingle() )
                output = m_gesture.update(
                    ImGui::IsMouseClicked(0),
                    ImGui::IsMouseDown(0),
                    ImGui::IsMouseReleased(0),
                    point,
                    ImGui::GetTime(),
                    10.0f * scale,
                    eligible || (raw.m_epoch != 0 && m_gesture.active() &&
                                 !interrupted));
            // 轻点的 Begin/Release
            // 回放仍保留落点，不能因原始触点已抬起吞掉提交。
            // 多指导航和真正结束的空闲帧才撤去普通鼠标悬浮反馈。
            const bool hidePointer =
                raw.m_epoch != 0 && (m_ownership.blocksSingle() ||
                                     (raw.m_count == 0 && !m_gesture.active()));
            TouchInputScope input(
                output, m_gesture.replayPoint(), m_clickCount, hidePointer);
            routeInput(canvas, snapshot, width, height, true);
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
            m_waitRelease       = true;
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
        auto&      app     = Config::AppConfig::instance();
        auto&      zoom    = app.getEditorConfig().visual.timelineZoom;
        const auto display = ImGui::GetIO().DisplaySize;
        const auto delta =
            m_navigation.update(raw,
                                allowed,
                                display.x,
                                display.y,
                                zoom,
                                10.0f * app.getWindowContentScale());
        if ( delta.m_changed ) {
            // 与普通拾取共用屏幕到谱面平面的逆投影，三维视角不另猜移动比例。
            const glm::vec2 oldScreen{ delta.m_anchorX - frame.m_x,
                                       delta.m_anchorY - frame.m_y };
            const glm::vec2 newScreen =
                oldScreen + glm::vec2(delta.m_x, delta.m_y);
            const auto oldPoint =
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
                m_navigationZoom = delta.m_zoom;
                UI::Interaction::dispatchCommand(Event::LogicCommandEvent(
                    Logic::CmdPanCanvas{ .cameraId = camera,
                                         .deltaX   = newPoint->x - oldPoint->x,
                                         .deltaY   = newPoint->y - oldPoint->y,
                                         .viewportWidth  = width,
                                         .viewportHeight = height,
                                         .renderScaleY = snapshot.renderScaleY,
                                         .timelineZoom = delta.m_zoom,
                                         .anchorY      = oldPoint->y }));
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
            zoom = m_navigationZoom;
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
