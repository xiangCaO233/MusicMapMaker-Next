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
/// - 显式返回可以降低目标水位，绘制练习仅调用已登记的定向补偿；
/// - 回看锁定只作用于当前配置步骤，下一步骤重新使用正常自动推进；
/// - 跨步骤请求交给页面层消费，本层不拥有主题或分支的数据引用；
/// - 目标缺席时只显示导航气泡，不复用上一帧坐标绘制高亮；
/// - 模态环境中两个导航按钮分别保存按下起点，不允许拖移串键；
/// - 最后目标完成后保留 Completed，交给路线会话衔接下一步骤；
/// - stop 清理配置和几何，后续控件上报成为无操作。
///
/// 目标解析：
/// - target ID 与翻译文本、ImGui 标签和窗口地址解耦；
/// - 当前引导只比较配置声明的少量候选，不维护全局控件表；
/// - 同帧候选按配置索引决定优先级，列表末项优先；
/// - 同一语义目标重复上报时合并同视口矩形，允许用户从多个并列项中选择；
/// - 合并后的视觉范围不替代业务命中，只有具体控件确认才完成阶段；
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
/// @warning UI 热路径：只向已有 DrawList 追加矩形，不分配临时资源。
void addFilledRect(ImDrawList& draw, const ImVec2& minimum,
                   const ImVec2& maximum, ImU32 color)
{
    // 目标贴近视口边缘时部分遮罩宽高为零，跳过可避免退化三角形。
    if ( maximum.x > minimum.x && maximum.y > minimum.y )
        draw.AddRectFilled(minimum, maximum, color);
}

/// @brief 在一个暗化矩形内扣除提示气泡所占区域。
/// @param draw 当前视口前景绘制列表。
/// @param regionMin 原暗化矩形左上角。
/// @param regionMax 原暗化矩形右下角。
/// @param bubbleMin 提示气泡左上角。
/// @param bubbleMax 提示气泡右下角。
/// @warning UI 热路径：固定四个候选矩形，不排序或分配容器。
void addMaskRegionExcludingBubble(ImDrawList& draw, const ImVec2& regionMin,
                                  const ImVec2& regionMax,
                                  const ImVec2& bubbleMin,
                                  const ImVec2& bubbleMax)
{
    constexpr ImU32 MASK_COLOR = IM_COL32(0, 0, 0, 190);
    const ImVec2    overlapMin{ std::max(regionMin.x, bubbleMin.x),
                                std::max(regionMin.y, bubbleMin.y) };
    const ImVec2    overlapMax{ std::min(regionMax.x, bubbleMax.x),
                                std::min(regionMax.y, bubbleMax.y) };
    if ( overlapMax.x <= overlapMin.x || overlapMax.y <= overlapMin.y ) {
        // 气泡不与此暗区相交时保留原矩形，避免无谓拆分绘制命令。
        addFilledRect(draw, regionMin, regionMax, MASK_COLOR);
        return;
    }
    // 上下两带占整宽，中间仅保留气泡左右两侧；四块互不覆盖。
    // 所有输出仍落在原暗化矩形内，避免跨视口提交多余几何。
    addFilledRect(draw, regionMin, { regionMax.x, overlapMin.y }, MASK_COLOR);
    addFilledRect(draw, { regionMin.x, overlapMax.y }, regionMax, MASK_COLOR);
    addFilledRect(draw,
                  { regionMin.x, overlapMin.y },
                  { overlapMin.x, overlapMax.y },
                  MASK_COLOR);
    addFilledRect(draw,
                  { overlapMax.x, overlapMin.y },
                  { regionMax.x, overlapMax.y },
                  MASK_COLOR);
}

/// @brief 在视口内分别为目标与提示保留亮区。
/// @param draw 当前视口前景绘制列表。
/// @param viewportMin 视口左上角。
/// @param viewportMax 视口右下角。
/// @param targetMin 目标亮区左上角。
/// @param targetMax 目标亮区右下角。
/// @param bubbleMin 提示气泡左上角。
/// @param bubbleMax 提示气泡右下角。
/// @warning UI 热路径：只拆分固定四个目标外区域，不进行排序或动态分配。
void addMaskOutsideTargetAndBubble(ImDrawList& draw, const ImVec2& viewportMin,
                                   const ImVec2& viewportMax,
                                   const ImVec2& targetMin,
                                   const ImVec2& targetMax,
                                   const ImVec2& bubbleMin,
                                   const ImVec2& bubbleMax)
{
    // 先按目标形成互不重叠的四个暗区，再从每块中扣除提示窗口。
    // 两块亮区之间因此仍为暗色，且两者局部相交时不会叠加遮罩。
    addMaskRegionExcludingBubble(draw,
                                 viewportMin,
                                 { viewportMax.x, targetMin.y },
                                 bubbleMin,
                                 bubbleMax);
    addMaskRegionExcludingBubble(draw,
                                 { viewportMin.x, targetMax.y },
                                 viewportMax,
                                 bubbleMin,
                                 bubbleMax);
    addMaskRegionExcludingBubble(draw,
                                 { viewportMin.x, targetMin.y },
                                 { targetMin.x, targetMax.y },
                                 bubbleMin,
                                 bubbleMax);
    addMaskRegionExcludingBubble(draw,
                                 { targetMax.x, targetMin.y },
                                 { viewportMax.x, targetMax.y },
                                 bubbleMin,
                                 bubbleMax);
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
    m_reviewedActionSatisfied = false;
    // ImGui 控件坐标只能在提交它的当前帧使用，绝不能沿用旧布局矩形。
    m_anchor.reset();
    m_acknowledgeButtonCenter.reset();
    m_previousButtonCenter.reset();
    m_keepAlive = false;
    if ( m_state == State::Highlighting )
        // 每帧重新等待控件上报，防止窗口关闭后继续绘制旧矩形。
        m_state = State::Waiting;
}

/// @brief 启动配置声明的目标流程。
/// @param targets 候选目标按流程排列，后出现的可见项覆盖前项。
/// @param prompt 纯文字步骤的操作提示或目标高亮时的气泡说明。
/// @param previousStepAvailable 路线中是否存在可回看的前一步。
/// @param reviewing 回看时只允许显式确认，防止已有状态立即跳过。
/// @param requiresAction 是否禁止手动确认跳过。
void Spotlight::start(const std::vector<std::string>& targets,
                      std::string prompt, bool previousStepAvailable,
                      bool reviewing, bool requiresAction)
{
    // 新路线不继承旧练习；正常前进保留产物，回到旧目标才消费补偿。
    if ( !previousStepAvailable && !reviewing ) m_rollbacks.clear();
    if ( reviewing ) {
        // 回到前一步时撤掉该步旧成果，随后重新进入可绘制状态。
        // 只按登记的语义目标匹配，教程文本或历史学习进度不参与判定。
        for ( const auto& target : targets ) rollbackTarget(target);
    }
    ++m_stepToken;
    // 复制只发生在用户点击“进入引导”或步骤自动衔接时，不进入常规帧路径。
    m_targets                 = targets;
    m_previousStepAvailable   = previousStepAvailable;
    m_previousStepRequested   = false;
    m_reviewing               = reviewing;
    m_requiresAction          = requiresAction;
    m_reviewedActionSatisfied = false;
    m_previousPressed = m_previousMouseWasDown = false;
    m_previousButtonCenter.reset();
    m_prompt = std::move(prompt);
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
    // 结束演练保留用户已绘制的成果，只有返回才执行补偿。
    m_rollbacks.clear();
    // 跨路线请求不能泄漏到下次进入，引导退出不保留返回手势。
    m_previousStepAvailable = m_previousStepRequested = m_reviewing = false;
    m_requiresAction                                                = false;
    m_reviewedActionSatisfied                                       = false;
    m_previousPressed = m_previousMouseWasDown = false;
    m_previousButtonCenter.reset();
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

/// @brief 判断是否存在当前目标或当前步骤之前的引导。
/// @warning UI 每帧多次查询，只读取本实例的值状态。
bool Spotlight::canGoBack() const
{
    // 步骤内目标优先于跨步骤；未启动时即使成员仍有值也不提供返回能力。
    return active() && (m_stage > 0 || m_previousStepAvailable);
}

/// @brief 优先回退步骤内目标，首目标则请求路线层切换到前一步。
void Spotlight::requestPrevious()
{
    // 重复点击只保留一个跨步骤请求，不能累积成一次跨越多个步骤。
    if ( !canGoBack() || m_previousStepRequested ) return;
    // 当前步骤若已提交但尚未衔接，也必须清掉，避免留下被跳过的练习。
    for ( const auto& target : m_targets ) rollbackTarget(target);
    // 清除本帧几何与输入锁存，返回点击不得同时确认重新出现的目标。
    // 下一帧必须重新解析回看目标，不能把后续窗口的旧坐标当作前序控件。
    // 目标若已关闭，导航气泡仍会出现，但不擅自重新打开业务窗口。
    m_anchor.reset();
    m_acknowledgeButtonCenter.reset();
    m_previousButtonCenter.reset();
    m_acknowledgePressed = m_previousPressed = false;
    m_reviewing                              = true;
    m_state                                  = State::Waiting;
    if ( m_stage > 0 )
        --m_stage;
    else
        m_previousStepRequested = true;
}

/// @brief 一次性消费跨步骤请求，由路线层定位前序步骤。
bool Spotlight::consumePreviousStepRequest()
{
    // 本层不持有业务路线指针，避免 UI 生命周期与导航状态相互耦合。
    // 消费不退出回看模式；只有 start/stop 才能建立另一轮正常演练。
    return std::exchange(m_previousStepRequested, false);
}

/// @brief 只在真实绘制成功时登记业务提供的补偿命令。
void Spotlight::registerRollback(std::string_view      targetId,
                                 std::function<void()> rollback)
{
    // 必须在完成通知之前登记，过期步骤的晚到业务事件不能挂到新目标。
    if ( !awaitingTarget(targetId) || !rollback ) return;
    m_rollbacks.push_back({ std::string(targetId), std::move(rollback) });
}

/// @brief 消费匹配目标的补偿后移除记录，确保返回操作幂等。
void Spotlight::rollbackTarget(std::string_view targetId)
{
    // 此列表只包含教学创建，不通过历史完成度或实体数量推断业务副作用。
    for ( auto it = m_rollbacks.begin(); it != m_rollbacks.end(); ) {
        if ( it->targetId != targetId ) {
            ++it;
            continue;
        }
        it->action();
        // 命令携带值语义的会话与步骤身份，擦除闭包不影响已入队的回滚。
        it = m_rollbacks.erase(it);
    }
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

/// @brief 判断业务控件是否需要为当前阶段准备专用视觉目标。
/// @param targetId 待比较的稳定语义 ID。
/// @return 当前水位与目标一致时返回 true。
bool Spotlight::awaitingTarget(std::string_view targetId) const
{
    return active() && !completed() && m_stage < m_targets.size() &&
           m_targets[m_stage] == targetId;
}

/// @brief 允许当前可见演练页面在本帧继续显示引导。
void Spotlight::keepAlive()
{
    if ( active() ) m_keepAlive = true;
}

/// @brief 跳过当前已经定位到的目标阶段。
void Spotlight::acknowledgeCurrentStage()
{
    // 业务强制步骤没有手动跳过入口，即使外部直接调用确认也不能越权。
    if ( m_requiresAction && !m_reviewedActionSatisfied ) return;
    if ( m_previousStepRequested ) return;
    // 返回与确认具有互斥语义，待路线切换期间不能误确认被离开的步骤。
    if ( m_targets.empty() && m_state == State::Waiting ) {
        // 纯文字步骤的“知道了”同样完成当前步骤，避免路线无法继续。
        m_state = State::Completed;
        return;
    }
    // 已关闭的弹窗不会被导航自动重开；回看时允许显式略过缺席目标。
    if ( m_reviewing && m_state == State::Waiting ) {
        // 缺席目标只允许用户主动略过；普通等待态仍必须等目标真正出现。
        completeStage(m_stage);
        return;
    }
    if ( m_state != State::Highlighting || !m_anchor ) return;
    completeStage(m_anchor->priority);
}

/// @brief 回看已完成的强制步骤时保留停留机会，不能因旧成果立即前进。
void Spotlight::reportReviewedActionSatisfied()
{
    // 本方法不改变阶段水位；用户仍有机会停留或返回前一步。
    // 条件由业务帧重新上报，不能从已完成学习进度中推测物件状态。
    if ( m_requiresAction && m_reviewing ) m_reviewedActionSatisfied = true;
}

/// @brief 通知状态机某个语义目标已经由业务逻辑正确完成。
/// @param targetId 与当前配置中的目标 ID 一致。
/// @param explicitAction 是否为用户回看后实际执行的新手势。
void Spotlight::completeTarget(std::string_view targetId, bool explicitAction)
{
    // 回看忽略持续上报的已满足条件，但重画成功属于明确的新操作。
    if ( !active() || completed() || (m_reviewing && !explicitAction) ||
         m_previousStepRequested )
        return;
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
/// @param drawOutline 是否绘制 Spotlight 自身的脉冲外框。
/// @note 关闭外框只影响装饰描边，遮罩孔洞、提示定位与完成状态保持不变。
void Spotlight::reportTarget(std::string_view targetId, const ImVec2& minimum,
                             const ImVec2& maximum, ImGuiViewport* viewport,
                             bool drawOutline)
{
    if ( !active() || completed() || maximum.x <= minimum.x ||
         maximum.y <= minimum.y || m_previousStepRequested )
        return;
    // 配置列表通常只有数项；线性查找避免为逐帧注册建立哈希表和分配节点。
    const auto it = std::find(m_targets.begin(), m_targets.end(), targetId);
    if ( it == m_targets.end() ) return;
    const auto priority = static_cast<std::size_t>(it - m_targets.begin());
    // 返回后后续窗口可能仍然打开，不能仅凭其可见性抢走回看目标。
    if ( m_reviewing && priority != m_stage ) return;
    // 已完成阶段永久忽略；后续目标可见本身证明界面已经越过中间阶段。
    if ( priority < m_stage ) return;
    if ( priority > m_stage ) {
        m_stage = priority;
        m_anchor.reset();
    }
    auto* resolvedViewport = viewport ? viewport : ImGui::GetWindowViewport();
    if ( m_anchor && m_anchor->priority == priority &&
         m_anchor->viewport == resolvedViewport ) {
        // 同一目标可由多项并列控件上报；合并区域让全部候选保持可见可选。
        // 合并只发生在相同阶段，不能把前后两个动作扩大为同一个孔洞。
        // 视口必须一致，否则单个 ForegroundDrawList 无法正确表达坐标。
        // 外接矩形避免热路径维护动态容器，也不改变具体控件的完成判定；
        // 因此标签之间的间隙虽然透明，却不会误推进状态机。
        m_anchor->minimum.x = std::min(m_anchor->minimum.x, minimum.x);
        m_anchor->minimum.y = std::min(m_anchor->minimum.y, minimum.y);
        m_anchor->maximum.x = std::max(m_anchor->maximum.x, maximum.x);
        m_anchor->maximum.y = std::max(m_anchor->maximum.y, maximum.y);
        // 任一并列目标要求自绘时关闭合并外框，避免把间隔误画成控件边界。
        m_anchor->drawOutline = m_anchor->drawOutline && drawOutline;
    } else {
        // 不同视口不能共享一个前景遮罩，保留本次上报作为当前锚点。
        m_anchor = Anchor{ .priority    = priority,
                           .minimum     = minimum,
                           .maximum     = maximum,
                           .viewport    = resolvedViewport,
                           .drawOutline = drawOutline };
    }
    m_state = State::Highlighting;
}

/// @brief 绘制不阻挡目标操作、但允许确认当前阶段的引导层。
/// @param dpiScale 当前内容缩放。
/// @param acknowledgeLabel 当前语言的确认按钮文本。
/// @param previousLabel 当前语言的返回按钮文本，空值保留旧调用方式。
void Spotlight::render(float dpiScale, const char* acknowledgeLabel,
                       const char* previousLabel)
{
    if ( !active() || completed() || !m_keepAlive ) return;
    const bool promptOnly = m_targets.empty();
    // 有目标的引导只在本帧重新解析到当前阶段时显示，等待态不绘制旧高亮。
    // 目标暂时缺席时只显示导航气泡，不伪造高亮；用户仍能返回前一步。
    const bool waitingNavigation = canGoBack() || m_reviewing;
    if ( m_state != State::Highlighting && !promptOnly && !waitingNavigation )
        return;

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

    const bool hasPrevious = previousLabel && previousLabel[0] != '\0';
    const bool hasAcknowledge =
        (!m_requiresAction || m_reviewedActionSatisfied) &&
        (m_anchor || promptOnly || m_reviewing) && acknowledgeLabel &&
        acknowledgeLabel[0] != '\0';
    if ( m_prompt.empty() && !hasAcknowledge && !hasPrevious ) return;
    // 目标步骤与纯文字步骤都提供确认入口，确保路线会话可以显式继续。
    const float  margin = 12.0f * dpiScale;
    const auto&  style  = ImGui::GetStyle();
    const ImVec2 buttonTextSize =
        hasAcknowledge ? ImGui::CalcTextSize(acknowledgeLabel) : ImVec2{};
    const ImVec2 buttonSize{ buttonTextSize.x + style.FramePadding.x * 2.0f,
                             buttonTextSize.y + style.FramePadding.y * 2.0f };
    const ImVec2 previousTextSize =
        hasPrevious ? ImGui::CalcTextSize(previousLabel) : ImVec2{};
    // 返回按钮与确认按钮一起参与布局，不能把额外按钮挤到目标框外侧。
    // 禁用态返回按钮仍占相同尺寸，首步与后续步骤切换时保持稳定布局。
    // 等待态可能只剩返回按钮，其高度也必须参与气泡的包围框计算。
    // 本层不读取翻译单例，生产传本地化文本，测试传固定标签。
    const float previousWidth = hasPrevious ? previousTextSize.x +
                                                  style.FramePadding.x * 2.0f +
                                                  style.ItemSpacing.x
                                            : 0.0f;
    const float buttonWidth =
        previousWidth +
        (hasAcknowledge ? buttonSize.x + style.ItemSpacing.x : 0.0f);
    // 文本与按钮共同限制在视口内，窄窗口优先压缩提示文字。
    float maxTextWidth =
        std::min(420.0f * dpiScale,
                 std::max(1.0f,
                          viewport->Size.x - margin * 2.0f -
                              style.WindowPadding.x * 2.0f - buttonWidth));
    if ( holeMin && holeMax ) {
        // 细长工具栏优先把提示放在侧边；先按可用侧宽收窄文字，保证气泡
        // 能完整离开目标矩形，而不是覆盖最上方的工具按钮。
        // 两侧取较大值只决定排版宽度，最终方向仍由 fits 检查决定。
        // 容量扣除双边距，保证视口边缘与目标边缘各保留一份间隔。
        // 空间不足时不压缩成难读的窄列，而是继续尝试上下布局。
        const float leftCapacity  = holeMin->x - viewportMin.x - margin * 2.0f;
        const float rightCapacity = viewportMax.x - holeMax->x - margin * 2.0f;
        const float sideCapacity  = std::max(leftCapacity, rightCapacity);
        constexpr float MIN_SIDE_BUBBLE_WIDTH = 240.0f;
        if ( sideCapacity >= MIN_SIDE_BUBBLE_WIDTH * dpiScale ) {
            maxTextWidth =
                std::min(maxTextWidth,
                         std::max(1.0f,
                                  sideCapacity - style.WindowPadding.x * 2.0f -
                                      buttonWidth));
        }
    }
    const ImVec2 textSize =
        m_prompt.empty() ? ImVec2{}
                         : ImGui::CalcTextSize(
                               m_prompt.c_str(), nullptr, false, maxTextWidth);
    const ImVec2 bubbleSize{
        textSize.x + buttonWidth + style.WindowPadding.x * 2.0f,
        std::max({ textSize.y,
                   buttonSize.y,
                   hasPrevious
                       ? previousTextSize.y + style.FramePadding.y * 2.0f
                       : 0.0f }) +
            style.WindowPadding.y * 2.0f
    };
    // 预先计算气泡外框，遮罩稍后才能一次为目标、说明和按钮留出透明区。
    ImVec2 bubbleMin;
    if ( hintAnchor ) {
        // 依次尝试下、上、左、右四个不覆盖目标的位置。工具栏等纵向长窗
        // 会自然落到侧边；空间均不足时才允许气泡与目标区域相交。
        // 上下优先保持短提示的既有布局，纵向占满的目标才会使用侧边。
        // 中心坐标预先限制在视口内，避免选定方向后发生二次跳边。
        // fits 使用扩张后的 hole 边界，提示与脉冲描边之间不会紧贴。
        // 最终统一 clamp 仅处理极窄视口，不参与正常方向选择。
        const float centeredX =
            std::clamp(hintAnchor->x - bubbleSize.x * 0.5f,
                       viewportMin.x + margin,
                       std::max(viewportMin.x + margin,
                                viewportMax.x - bubbleSize.x - margin));
        const float centeredY =
            std::clamp((holeMin->y + holeMax->y - bubbleSize.y) * 0.5f,
                       viewportMin.y + margin,
                       std::max(viewportMin.y + margin,
                                viewportMax.y - bubbleSize.y - margin));
        const bool fitsBelow =
            holeMax->y + margin + bubbleSize.y <= viewportMax.y - margin;
        const bool fitsAbove =
            holeMin->y - margin - bubbleSize.y >= viewportMin.y + margin;
        const bool fitsLeft =
            holeMin->x - margin - bubbleSize.x >= viewportMin.x + margin;
        const bool fitsRight =
            holeMax->x + margin + bubbleSize.x <= viewportMax.x - margin;
        if ( fitsBelow )
            bubbleMin = { centeredX, holeMax->y + margin };
        else if ( fitsAbove )
            bubbleMin = { centeredX, holeMin->y - margin - bubbleSize.y };
        else if ( fitsLeft )
            bubbleMin = { holeMin->x - margin - bubbleSize.x, centeredY };
        else if ( fitsRight )
            bubbleMin = { holeMax->x + margin, centeredY };
        else
            // 时间线等大窗格无相邻空间时沿用顶部回退，后续描边会避开气泡。
            bubbleMin = { centeredX, holeMin->y - margin - bubbleSize.y };
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
    bool                  acknowledged    = false;
    bool                  previousClicked = false;
    std::optional<ImVec2> previousMin;
    std::optional<ImVec2> previousMax;
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
        if ( hasPrevious ) {
            if ( !m_prompt.empty() ) ImGui::SameLine();
            // 首步保留禁用按钮以明确导航边界，不误触发跨路线跳转。
            // 模态补充判定也检查 canGoBack，不能绕过 ImGui 的禁用状态。
            // 实际控件矩形用于点击补判，气泡预估位置不能替代最终布局。
            ImGui::PushID("WalkthroughSpotlightPrevious");
            ImGui::PushItemFlag(ImGuiItemFlags_NoFocus, true);
            ImGui::BeginDisabled(!canGoBack());
            previousClicked = FeedbackButton(previousLabel);
            ImGui::EndDisabled();
            ImGui::PopItemFlag();
            previousMin            = ImGui::GetItemRectMin();
            previousMax            = ImGui::GetItemRectMax();
            m_previousButtonCenter = { (previousMin->x + previousMax->x) * 0.5f,
                                       (previousMin->y + previousMax->y) *
                                           0.5f };
            ImGui::PopID();
        }
        if ( hasAcknowledge ) {
            // 按钮保持在说明右侧，让每个大遮罩都有明确且紧邻的退出入口。
            if ( !m_prompt.empty() || hasPrevious ) ImGui::SameLine();
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
    // 模态窗口外的返回按钮采用与确认按钮相同的完整按下/释放判定。
    // 独立锁存避免从一个按钮按下、拖到另一个按钮释放时错误导航。
    // 控件缺席时仍采样鼠标状态，避免出现帧把既有按住当作新点击。
    // 拖出一次后保持失败，必须重新按下才建立下一次有效手势。
    const auto& io = ImGui::GetIO();
    const bool  previousInside =
        canGoBack() && previousMin && previousMax &&
        containsPoint(io.MousePos, *previousMin, *previousMax);
    const bool previousDown = io.MouseDown[ImGuiMouseButton_Left];
    if ( previousDown ) {
        if ( !m_previousMouseWasDown )
            m_previousPressed = previousInside;
        else if ( !previousInside )
            m_previousPressed = false;
    } else {
        previousClicked |=
            m_previousMouseWasDown && m_previousPressed && previousInside;
        m_previousPressed = false;
    }
    m_previousMouseWasDown = previousDown;
    if ( previousClicked ) {
        // 本帧立即撤掉旧高亮；跨步骤请求在路线下一次更新时消费。
        // 不再执行同帧确认分支，避免一个鼠标释放同时产生两个导航结果。
        // requestPrevious 本身不持久化进度，也不会操作底层业务弹窗。
        requestPrevious();
        return;
    }
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
        // 目标与提示分别挖孔，不能让两者的外接矩形照亮无关内容。
        addMaskOutsideTargetAndBubble(draw,
                                      viewportMin,
                                      viewportMax,
                                      *holeMin,
                                      *holeMax,
                                      bubbleMin,
                                      bubbleMax);

        if ( m_anchor->drawOutline ) {
            // 目标描边仍严格跟随控件，不随较宽的提示透明区扩张。
            const float pulse =
                0.5f +
                0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
            const auto drawTargetOutline = [&](const ImVec2& clipMin,
                                               const ImVec2& clipMax) {
                if ( clipMax.x <= clipMin.x || clipMax.y <= clipMin.y ) return;
                draw.PushClipRect(clipMin, clipMax, true);
                draw.AddRect(*holeMin,
                             *holeMax,
                             ImGui::GetColorU32(ImGuiCol_CheckMark),
                             style.FrameRounding,
                             ImDrawFlags_None,
                             (2.0f + pulse) * dpiScale);
                draw.PopClipRect();
            };
            // ForegroundDrawList 总在普通窗口之后合成；将提示矩形从描边裁掉，
            // 才能保证时间线顶部回退场景中黄色边框不会压住文字和确认按钮。
            // 四个裁剪区覆盖提示框之外的所有方向，边框在框下自然断开。
            // 裁剪只影响描边命令，目标与提示共同形成的透明遮罩保持不变。
            drawTargetOutline(viewportMin, { viewportMax.x, bubbleMin.y });
            drawTargetOutline({ viewportMin.x, bubbleMax.y }, viewportMax);
            drawTargetOutline({ viewportMin.x, bubbleMin.y },
                              { bubbleMin.x, bubbleMax.y });
            drawTargetOutline({ bubbleMax.x, bubbleMin.y },
                              { viewportMax.x, bubbleMax.y });
        }
    }
}

/// @brief 查询本帧被配置优先级解析选中的目标。
/// @return Anchor 存在时借用对应配置字符串，否则为空。
std::string_view Spotlight::resolvedTargetId() const
{
    if ( !m_anchor || m_anchor->priority >= m_targets.size() ) return {};
    return m_targets[m_anchor->priority];
}

/// @brief 返回当前帧最终采用目标的合并屏幕矩形。
/// @return 有有效锚点时返回值副本，否则返回空值。
std::optional<Spotlight::TargetBounds> Spotlight::resolvedTargetBounds() const
{
    if ( !m_anchor ) return std::nullopt;
    return TargetBounds{ .minimum = m_anchor->minimum,
                         .maximum = m_anchor->maximum };
}

/// @brief 查询当前帧实际提交的确认按钮中心。
/// @return 按钮存在时返回屏幕坐标，否则为空。
std::optional<ImVec2> Spotlight::acknowledgeButtonCenter() const
{
    return m_acknowledgeButtonCenter;
}

/// @brief 返回本帧实际提交的返回按钮中心。
std::optional<ImVec2> Spotlight::previousButtonCenter() const
{
    return m_previousButtonCenter;
}
}  // namespace MMM::UI::Walkthrough
