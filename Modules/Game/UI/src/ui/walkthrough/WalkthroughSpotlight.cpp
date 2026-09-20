#include "ui/walkthrough/WalkthroughSpotlight.h"

#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cmath>
#include <imgui_internal.h>
#include <utility>

/// @file WalkthroughSpotlight.cpp
/// @brief 配置驱动的任意控件目标解析、前景遮罩与阶段确认提示实现。
/// @details 遮罩本身不接管输入；仅提示气泡创建固定 ID 的小窗口，使“知道了”
/// 按钮可点击。气泡外的鼠标和键盘仍由原控件处理。
///
/// 帧生命周期：
/// - beginFrame 丢弃上一帧的控件矩形并清除页面续租；
/// - 已接入控件在自身绘制后立即调用 reportLastItem；
/// - 画布和复合控件可以调用 reportTarget 上报显式屏幕矩形；
/// - 演练页面可见时调用 keepAlive，隐藏后本帧不再绘制；
/// - UIManager 在全部视图更新结束后统一调用 render；
/// - 业务控件成功完成目标或点击“知道了”都会推进同一状态机；
/// - 后续控件尚未出现时进入等待态，不重新遮住已完成控件；
/// - 最后目标完成后保留 Completed，交给路线会话衔接下一步骤；
/// - stop 清理配置和几何，后续控件上报成为无操作。
///
/// 目标解析：
/// - target ID 与翻译文本、ImGui 标签和窗口地址解耦；
/// - 当前引导只比较配置声明的少量候选，不维护全局控件表；
/// - 同帧候选按配置索引决定优先级，列表末项优先；
/// - 菜单项出现后可自然覆盖一级菜单目标；
/// - 向导下一页出现后可自然覆盖仍在后台绘制的菜单目标；
/// - 后续候选一旦出现便推进当前阶段，消失后也不回退到前序目标；
/// - 已完成候选永久跳过，直到启动另一个步骤并重置状态机；
/// - 有目标但未上报时不绘制；纯文字步骤只显示 prompt，不伪造亮区。
///
/// 阶段确认约定：
/// - 状态机记录待完成目标索引而不是控件地址，布局重建不影响结果；
/// - 完成当前目标时一并跳过所有更早目标，流程只会向前推进；
/// - 业务成功由控件显式通知，突出层不会用鼠标手势猜测执行结果；
/// - 当前界面未切换时已完成目标保持无罩，避免遮罩立即反弹；
/// - 后续目标由业务窗口提交后，状态机才恢复突出显示；
/// - 最后一项目标确认后调用 stop，页面在下一帧同步退出引导状态；
/// - 没有控件目标的纯文字提示不存在可确认的遮罩阶段。
///
/// 遮罩几何：
/// - 目标按 DPI 外扩后裁剪到所在视口；
/// - 四个不重叠实心矩形围出完全透明的矩形孔洞；
/// - 方案不需要 Vulkan 模板缓冲，也不需要重绘原始控件；
/// - CheckMark 主题色描边通过正弦线宽产生轻量呼吸效果；
/// - 提示和“知道了”组成紧邻目标的小窗口；
/// - 气泡限制宽度并始终夹在视口安全边距内；
/// - 遮罩同时为目标和气泡保留透明区域，按钮不会被再次暗化；
/// - 键盘或外部应用步骤仅显示居中的提示气泡。
/// - 提示窗口使用固定内部 ID，不写入 ini 布局或参与 Dock；
/// - NoNav 与 NoFocusOnAppearing 阻止提示自动取得键盘导航焦点；
/// - 只有气泡矩形接收鼠标输入，遮罩及透明区域不建立命中窗口；
/// - 模态窗口会阻挡普通悬浮，确认按钮额外检查自身矩形内的鼠标释放；
/// - 矩形判定不改变 ActiveId 或 NavWindow，避免提示抢走原业务焦点；
/// - 按钮沿用 FeedbackButton，保持皮肤悬浮动画和音效入口一致。

namespace MMM::UI::Walkthrough
{
namespace
{
/// @brief 以视口边界裁剪一个屏幕空间点。
/// @param point 原始点。
/// @param minimum 视口左上角。
/// @param maximum 视口右下角。
/// @return 每个分量都位于边界内的新点。
/// @warning UI 热路径：只执行固定次数的标量 clamp，不分配内存。
ImVec2 clampPoint(const ImVec2& point, const ImVec2& minimum,
                  const ImVec2& maximum)
{
    return { std::clamp(point.x, minimum.x, maximum.x),
             std::clamp(point.y, minimum.y, maximum.y) };
}

/// @brief 仅在矩形拥有正面积时提交暗化区域。
/// @param draw 当前视口的前景绘制列表。
/// @param minimum 左上角。
/// @param maximum 右下角。
/// @param color 已包含透明度的遮罩颜色。
/// @warning UI 热路径：每帧最多调用四次，只向已有 DrawList 追加矩形。
void addFilledRect(ImDrawList& draw, const ImVec2& minimum,
                   const ImVec2& maximum, ImU32 color)
{
    // 目标贴近视口边缘时部分遮罩宽高为零，跳过可避免退化三角形。
    if ( maximum.x > minimum.x && maximum.y > minimum.y )
        draw.AddRectFilled(minimum, maximum, color);
}

/// @brief 判断屏幕坐标是否落在半开矩形内。
/// @param point 待判断坐标。
/// @param minimum 矩形左上角。
/// @param maximum 矩形右下角。
/// @return 坐标位于矩形内时返回 true。
/// @warning UI 热路径：每帧只执行固定次数的标量比较。
bool containsPoint(const ImVec2& point, const ImVec2& minimum,
                   const ImVec2& maximum)
{
    return point.x >= minimum.x && point.x < maximum.x &&
           point.y >= minimum.y && point.y < maximum.y;
}
}  // namespace

/// @brief 清除上一帧目标几何并等待可见页面续租。
/// @warning UI 热路径：每帧调用，只重置 optional 与布尔值。
void Spotlight::beginFrame()
{
    // ImGui 控件坐标只能在提交它的当前帧使用，绝不能沿用旧布局矩形。
    m_anchor.reset();
    m_acknowledgeButtonCenter.reset();
    m_keepAlive = false;
    if ( m_state == State::Highlighting )
        // 每帧重新等待控件上报，防止窗口关闭后继续绘制旧矩形。
        m_state = State::Waiting;
}

/// @brief 启动配置声明的目标流程。
/// @param targets 候选目标按流程排列，后出现的可见项覆盖前项。
/// @param prompt 纯文字步骤的操作提示或目标高亮时的气泡说明。
void Spotlight::start(const std::vector<std::string>& targets,
                      std::string                     prompt)
{
    // 复制只发生在用户点击“进入引导”或步骤自动衔接时，不进入常规帧路径。
    m_targets = targets;
    m_prompt  = std::move(prompt);
    m_anchor.reset();
    m_stage                   = 0;
    m_state                   = State::Waiting;
    m_keepAlive               = true;
    m_acknowledgeMouseWasDown = false;
    m_acknowledgePressed      = false;
    m_acknowledgeButtonCenter.reset();
}

/// @brief 停止引导并清除可见目标与提示文本。
void Spotlight::stop()
{
    m_state                   = State::Inactive;
    m_stage                   = 0;
    m_keepAlive               = false;
    m_acknowledgeMouseWasDown = false;
    m_acknowledgePressed      = false;
    m_acknowledgeButtonCenter.reset();
    m_anchor.reset();
    m_targets.clear();
    m_prompt.clear();
}

/// @brief 查询引导是否已经由用户启动。
/// @return 已启动且尚未停止时返回 true。
bool Spotlight::active() const
{
    return m_state != State::Inactive;
}

/// @brief 查询当前步骤是否已完成并等待路线会话接续。
/// @return 最后一个目标已由业务结果或“知道了”确认时返回 true。
bool Spotlight::completed() const
{
    return m_state == State::Completed;
}

/// @brief 允许当前可见演练页面在本帧继续显示引导。
void Spotlight::keepAlive()
{
    if ( active() ) m_keepAlive = true;
}

/// @brief 跳过当前已经定位到的目标阶段。
void Spotlight::acknowledgeCurrentStage()
{
    if ( m_targets.empty() && m_state == State::Waiting ) {
        // 纯文字步骤的“知道了”同样完成当前步骤，避免路线无法继续。
        m_state = State::Completed;
        return;
    }
    if ( m_state != State::Highlighting || !m_anchor ) return;
    completeStage(m_anchor->priority);
}

/// @brief 通知状态机某个语义目标已经由业务逻辑正确完成。
/// @param targetId 与当前配置中的目标 ID 一致。
void Spotlight::completeTarget(std::string_view targetId)
{
    if ( !active() || completed() ) return;
    const auto it = std::find(m_targets.begin(), m_targets.end(), targetId);
    if ( it == m_targets.end() ) return;
    completeStage(static_cast<std::size_t>(it - m_targets.begin()));
}

/// @brief 完成指定目标阶段并清除当前帧旧几何。
/// @param priority 已由业务结果或“知道了”确认的目标索引。
void Spotlight::completeStage(std::size_t priority)
{
    if ( !active() || completed() || priority < m_stage ||
         priority >= m_targets.size() )
        return;
    // 最后目标完成后保留明确终态，让路线会话决定继续下一步或整体结束。
    if ( priority + 1 >= m_targets.size() ) {
        m_anchor.reset();
        m_state = State::Completed;
        return;
    }
    // 完成后进入等待态，直到后续目标在新布局中提供本帧有效矩形。
    m_stage = priority + 1;
    m_anchor.reset();
    m_state = State::Waiting;
}

/// @brief 捕获最近一个可见 ImGui Item 的实际屏幕矩形。
/// @param targetId 控件稳定语义 ID。
void Spotlight::reportLastItem(std::string_view targetId)
{
    // 被裁剪的列表项没有可靠可见几何，不应在屏幕外留下突出框。
    if ( !active() || !ImGui::IsItemVisible() ) return;
    reportTarget(targetId,
                 ImGui::GetItemRectMin(),
                 ImGui::GetItemRectMax(),
                 ImGui::GetWindowViewport());
}

/// @brief 在当前流程中选择优先级最高的已上报目标。
/// @param targetId 控件稳定语义 ID。
/// @param minimum 屏幕空间左上角。
/// @param maximum 屏幕空间右下角。
/// @param viewport 区域所属视口。
void Spotlight::reportTarget(std::string_view targetId, const ImVec2& minimum,
                             const ImVec2& maximum, ImGuiViewport* viewport)
{
    if ( !active() || completed() || maximum.x <= minimum.x ||
         maximum.y <= minimum.y )
        return;
    // 配置列表通常只有数项；线性查找避免为逐帧注册建立哈希表和分配节点。
    const auto it = std::find(m_targets.begin(), m_targets.end(), targetId);
    if ( it == m_targets.end() ) return;
    const auto priority = static_cast<std::size_t>(it - m_targets.begin());
    // 已完成阶段永久忽略；后续目标可见本身证明界面已经越过中间阶段。
    if ( priority < m_stage ) return;
    if ( priority > m_stage ) {
        m_stage = priority;
        m_anchor.reset();
    }
    // 同一阶段可以由复合控件重复上报，最后一份可见几何作为当前锚点。
    m_anchor =
        Anchor{ .priority = priority,
                .minimum  = minimum,
                .maximum  = maximum,
                .viewport = viewport ? viewport : ImGui::GetWindowViewport() };
    m_state = State::Highlighting;
}

/// @brief 绘制不阻挡目标操作、但允许确认当前阶段的引导层。
/// @param dpiScale 当前内容缩放。
/// @param acknowledgeLabel 当前语言的确认按钮文本。
void Spotlight::render(float dpiScale, const char* acknowledgeLabel)
{
    if ( !active() || completed() || !m_keepAlive ) return;
    const bool promptOnly = m_targets.empty();
    // 有目标的引导只在本帧重新解析到当前阶段时显示，等待态不绘制旧高亮。
    if ( m_state != State::Highlighting && !promptOnly ) return;

    ImGuiViewport* viewport = m_anchor && m_anchor->viewport
                                  ? m_anchor->viewport
                                  : ImGui::GetMainViewport();
    if ( !viewport ) return;

    const ImVec2          viewportMin = viewport->Pos;
    const ImVec2          viewportMax{ viewport->Pos.x + viewport->Size.x,
                              viewport->Pos.y + viewport->Size.y };
    std::optional<ImVec2> holeMin;
    std::optional<ImVec2> holeMax;
    std::optional<ImVec2> hintAnchor;
    if ( m_anchor ) {
        const float padding = std::max(4.0f, 7.0f * dpiScale);
        holeMin             = clampPoint(
            { m_anchor->minimum.x - padding, m_anchor->minimum.y - padding },
            viewportMin,
            viewportMax);
        holeMax = clampPoint(
            { m_anchor->maximum.x + padding, m_anchor->maximum.y + padding },
            viewportMin,
            viewportMax);
        hintAnchor = ImVec2{ (holeMin->x + holeMax->x) * 0.5f, holeMax->y };
    }

    const bool hasAcknowledge = (m_anchor || promptOnly) && acknowledgeLabel &&
                                acknowledgeLabel[0] != '\0';
    if ( m_prompt.empty() && !hasAcknowledge ) return;
    // 目标步骤与纯文字步骤都提供确认入口，确保路线会话可以显式继续。
    const float  margin = 12.0f * dpiScale;
    const auto&  style  = ImGui::GetStyle();
    const ImVec2 buttonTextSize =
        hasAcknowledge ? ImGui::CalcTextSize(acknowledgeLabel) : ImVec2{};
    const ImVec2 buttonSize{ buttonTextSize.x + style.FramePadding.x * 2.0f,
                             buttonTextSize.y + style.FramePadding.y * 2.0f };
    const float  buttonWidth =
        hasAcknowledge ? buttonSize.x + style.ItemSpacing.x : 0.0f;
    // 文本与按钮共同限制在视口内，窄窗口优先压缩提示文字。
    const float maxTextWidth =
        std::min(420.0f * dpiScale,
                 std::max(1.0f,
                          viewport->Size.x - margin * 2.0f -
                              style.WindowPadding.x * 2.0f - buttonWidth));
    const ImVec2 textSize =
        m_prompt.empty() ? ImVec2{}
                         : ImGui::CalcTextSize(
                               m_prompt.c_str(), nullptr, false, maxTextWidth);
    const ImVec2 bubbleSize{
        textSize.x + buttonWidth + style.WindowPadding.x * 2.0f,
        std::max(textSize.y, buttonSize.y) + style.WindowPadding.y * 2.0f
    };
    // 预先计算气泡外框，遮罩稍后才能一次为目标、说明和按钮留出透明区。
    ImVec2 bubbleMin;
    if ( hintAnchor ) {
        // 默认放在目标下方；空间不足时翻到上方，水平方向始终限制在视口内。
        bubbleMin = { hintAnchor->x - bubbleSize.x * 0.5f,
                      hintAnchor->y + margin };
        if ( bubbleMin.y + bubbleSize.y > viewportMax.y - margin )
            bubbleMin.y = m_anchor->minimum.y - margin - bubbleSize.y;
    } else {
        // 无目标步骤把提示放在视口上方中央，方便继续使用键盘或外部窗口。
        bubbleMin = { viewportMin.x + (viewport->Size.x - bubbleSize.x) * 0.5f,
                      viewportMin.y + 72.0f * dpiScale };
    }
    const ImVec2 bubbleLimitMin{ viewportMin.x + margin,
                                 viewportMin.y + margin };
    // 极窄视口可能容不下完整气泡，仍需保持 clamp 上界不小于下界。
    const ImVec2 bubbleLimitMax{
        std::max(bubbleLimitMin.x, viewportMax.x - bubbleSize.x - margin),
        std::max(bubbleLimitMin.y, viewportMax.y - bubbleSize.y - margin)
    };
    bubbleMin = clampPoint(bubbleMin, bubbleLimitMin, bubbleLimitMax);
    const ImVec2 bubbleMax{ bubbleMin.x + bubbleSize.x,
                            bubbleMin.y + bubbleSize.y };
    // 提示窗口只覆盖自身矩形，窗口外输入会穿透到原有控件和目标亮区。
    ImGui::SetNextWindowPos(bubbleMin, ImGuiCond_Always);
    ImGui::SetNextWindowSize(bubbleSize, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleColor(ImGuiCol_Border,
                          ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
                        std::max(1.0f, dpiScale));
    constexpr ImGuiWindowFlags HINT_FLAGS =
        ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
    bool                  acknowledged = false;
    std::optional<ImVec2> acknowledgeMin;
    std::optional<ImVec2> acknowledgeMax;
    if ( ImGui::Begin("###WalkthroughSpotlightHint", nullptr, HINT_FLAGS) ) {
        // 只调整最终显示顺序，不调用 FocusWindow；提示应压过后来创建的停靠页
        // 与模态弹窗，同时保持原业务窗口的键盘焦点和 Popup 栈不变。
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if ( !m_prompt.empty() ) {
            // Group 固定文字整体宽度，使右侧确认按钮始终与提示并排。
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + maxTextWidth);
            ImGui::TextUnformatted(m_prompt.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
        }
        if ( hasAcknowledge ) {
            // 按钮保持在说明右侧，让每个大遮罩都有明确且紧邻的退出入口。
            if ( !m_prompt.empty() ) ImGui::SameLine();
            ImGui::PushID("WalkthroughSpotlightAcknowledge");
            // 非模态环境仍使用统一按钮行为，并禁止点击提示时改变业务焦点。
            ImGui::PushItemFlag(ImGuiItemFlags_NoFocus, true);
            acknowledged = FeedbackButton(acknowledgeLabel);
            ImGui::PopItemFlag();
            // 实际 Item 坐标包含提示窗口最终布局结果，不能由请求位置反推。
            acknowledgeMin            = ImGui::GetItemRectMin();
            acknowledgeMax            = ImGui::GetItemRectMax();
            m_acknowledgeButtonCenter = {
                (acknowledgeMin->x + acknowledgeMax->x) * 0.5f,
                (acknowledgeMin->y + acknowledgeMax->y) * 0.5f
            };
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if ( acknowledgeMin && acknowledgeMax ) {
        // 模态窗口会阻止独立提示窗口成为 HoveredWindow；用实际按钮
        // 矩形补充释放判定，不放宽遮罩外的任何业务输入。
        // 原始鼠标状态只读取一次，后续判定保持同一帧输入快照。
        const ImGuiIO& io      = ImGui::GetIO();
        const ImVec2   pointer = io.MousePos;
        const bool     pointerInside =
            containsPoint(pointer, *acknowledgeMin, *acknowledgeMax);
        const bool mouseDown = io.MouseDown[ImGuiMouseButton_Left];
        if ( mouseDown ) {
            // 只在按下边沿记录起点；按住后拖出按钮会取消本次确认。
            // 按住后再从外部拖入也不会触发，避免仅靠释放位置误确认。
            if ( !m_acknowledgeMouseWasDown )
                m_acknowledgePressed = pointerInside;
            else if ( !pointerInside )
                m_acknowledgePressed = false;
        } else {
            // 从按钮内按下并在按钮内释放才推进阶段，行为与普通按钮一致。
            // FeedbackButton 已在普通窗口处理点击，按位合并使两条路径幂等。
            acknowledged |= m_acknowledgeMouseWasDown && m_acknowledgePressed &&
                            pointerInside;
            m_acknowledgePressed = false;
        }
        m_acknowledgeMouseWasDown = mouseDown;
    } else {
        // 目标缺席期间同步按键基线，避免控件出现时把既有按住误判为新点击。
        m_acknowledgeMouseWasDown =
            ImGui::GetIO().MouseDown[ImGuiMouseButton_Left];
        m_acknowledgePressed = false;
    }

    // 普通按钮结果与模态矩形补充判定都直接推进同一状态机。
    if ( acknowledged ) {
        acknowledgeCurrentStage();
        return;
    }

    ImDrawList& draw = *ImGui::GetForegroundDrawList(viewport);
    if ( holeMin && holeMax ) {
        // 把提示窗口并入透明区域，Foreground 遮罩不会盖住按钮。
        // 联合外接矩形可能比两块区域之间更宽，但能保持四块遮罩互不重叠。
        const ImVec2    revealMin{ std::min(holeMin->x, bubbleMin.x),
                                std::min(holeMin->y, bubbleMin.y) };
        const ImVec2    revealMax{ std::max(holeMax->x, bubbleMax.x),
                                std::max(holeMax->y, bubbleMax.y) };
        constexpr ImU32 MASK_COLOR = IM_COL32(0, 0, 0, 190);
        // 四块矩形围出目标与提示的联合透明区，不依赖模板缓冲。
        addFilledRect(
            draw, viewportMin, { viewportMax.x, revealMin.y }, MASK_COLOR);
        addFilledRect(
            draw, { viewportMin.x, revealMax.y }, viewportMax, MASK_COLOR);
        addFilledRect(draw,
                      { viewportMin.x, revealMin.y },
                      { revealMin.x, revealMax.y },
                      MASK_COLOR);
        addFilledRect(draw,
                      { revealMax.x, revealMin.y },
                      { viewportMax.x, revealMax.y },
                      MASK_COLOR);

        // 目标描边仍严格跟随控件，不随较宽的提示透明区扩张。
        const float pulse =
            0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
        draw.AddRect(*holeMin,
                     *holeMax,
                     ImGui::GetColorU32(ImGuiCol_CheckMark),
                     style.FrameRounding,
                     ImDrawFlags_None,
                     (2.0f + pulse) * dpiScale);
    }
}

/// @brief 查询本帧被配置优先级解析选中的目标。
/// @return Anchor 存在时借用对应配置字符串，否则为空。
std::string_view Spotlight::resolvedTargetId() const
{
    if ( !m_anchor || m_anchor->priority >= m_targets.size() ) return {};
    return m_targets[m_anchor->priority];
}

/// @brief 查询当前帧实际提交的确认按钮中心。
/// @return 按钮存在时返回屏幕坐标，否则为空。
std::optional<ImVec2> Spotlight::acknowledgeButtonCenter() const
{
    return m_acknowledgeButtonCenter;
}
}  // namespace MMM::UI::Walkthrough
