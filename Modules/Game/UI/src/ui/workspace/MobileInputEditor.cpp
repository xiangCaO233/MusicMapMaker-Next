#include "ui/workspace/MobileInputEditor.h"
#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "mmm/SafeParse.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include "ui/workspace/TouchReadPress.h"
#include <fmt/format.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
/**
 * @file MobileInputEditor.cpp
 * @brief 为既有单值控件提供显式事务，并约束移动表单的正文滚动区域。
 * @details 控件包装层只借用本帧数值地址，不增加一套属性存储或业务命令。
 * 草稿与原值分离，因此软键盘输入、焦点切换和系统返回不会直接改源值。
 * 操作按钮仍由父弹窗提交，正文超高只产生局部滚动，不推动整个工作区。
 */
namespace MMM::UI
{
namespace
{
/// @brief UI 线程独占的单次精调草稿，不保留任何业务值地址。
struct PrecisionState {
    TouchReadPress m_press;      ///< 原始触点归属，滑动／多指／取消不打开精调。
    ImGuiContext*  m_context{};  ///< 只比较身份，不能在上下文销毁后解引用。
    ImGuiID        m_target{};   ///< 控件完整 ID 同时包含父窗口与业务 PushID。
    int            m_frame{}, m_seen{};  ///< 窗口隐藏后不允许旧草稿恢复提交。
    std::uint64_t  m_generation{};       ///< 操作方案切换使旧请求失效。
    ImGuiDataType  m_type{};  ///< 指针仅在相同类型的当前控件处重新借用。
    /// @brief 打开时冻结的源值；提交前与当前字段复核，拒绝外部修改冲突。
    double m_original{};
    /// @brief 最近一次合法草稿；不完整输入保留文本但禁止步进及应用。
    double m_draft{};
    /// @brief 合法业务区间；无有效区间时使用实际数值类型的表示边界。
    double m_min{}, m_max{};
    /// @brief 本次弹窗的精调步长，不持久化或改变原滑块的拖动速度。
    double m_step{};
    std::array<char, 96>
        m_text{};  ///< 有界文本支持科学计数法，不通过 locale 解析。
    // 错误文本保留在 m_text，合法性只控制步进和应用许可。
    // m_focus 是一次性请求，不代表输入法可见，也不能用来关闭系统键盘。
    bool m_valid{}, m_focus{};  ///< 初始聚焦一次，后续不能反复夺回输入焦点。
};
/// @brief 每个 UI 线程只编辑一个数字，上下文切换时显式丢弃旧状态。
thread_local PrecisionState state;
/// @brief 类型范围保持独立，整数只支持能被 double 精确表示的 int32。
/// @param type 控件实际存储类型，不能以显示格式猜测大小或符号。
/// @param data 仅在当前调用期间借用的字段或边界地址，可为空。
/// @param value 成功时输出有限值；失败不能用于写回。
/// @return 不支持的类型或非有限源值返回 false，原控件仍可正常工作。
/// @warning 不支持 64 位整数，避免经 double 中转损失低位后静默改值。
bool readScalar(ImGuiDataType type, const void* data, double& value)
{
    // 未指定边界是正常契约；不得解引用空地址或保留上次控件的范围。
    if ( !data ) return false;
    switch ( type ) {
    case ImGuiDataType_Float: value = *static_cast<const float*>(data); break;
    case ImGuiDataType_Double: value = *static_cast<const double*>(data); break;
    case ImGuiDataType_S32: value = *static_cast<const int*>(data); break;
    default: return false;
    }
    return std::isfinite(value);
}
/// @brief 只有显式应用才调用，转换范围在解析草稿时已经校验。
/// @param type 必须与打开草稿时以及本帧字段的类型相同。
/// @param data 重新借用的本帧地址，不从缓存取出旧对象指针。
/// @param value 已通过有限性、整型小数及业务范围检查的草稿。
/// @warning 调用者必须排除只读、目标隐藏以及源值已经变化的情况。
void writeScalar(ImGuiDataType type, void* data, double value)
{
    // 类型白名单与读取端对称；默认分支不能把未知类型当作浮点写入。
    switch ( type ) {
    case ImGuiDataType_Float:
        *static_cast<float*>(data) = static_cast<float>(value);
        break;
    case ImGuiDataType_Double: *static_cast<double*>(data) = value; break;
    case ImGuiDataType_S32:
        *static_cast<int*>(data) = static_cast<int>(value);
        break;
    default: break;
    }
}
/// @brief 只在打开或步进时刷新字符串，浮点按真实存储类型给出最短回显。
/// @details 输入框持有独立字符串，不在每帧格式化覆盖用户尚未完成的输入。
/// 小数点、指数或负号输入一半时，文本与最近合法草稿可以暂时不同。
/// 步进按钮只在文本合法时启用，避免用旧合法值悄悄丢弃错误文本。
void formatDraft()
{
    // float 不先扩大成 double 格式化，避免把表示误差暴露成长串小数。
    auto result = state.m_type == ImGuiDataType_Float
                      ? fmt::format_to_n(state.m_text.data(),
                                         state.m_text.size() - 1,
                                         "{}",
                                         static_cast<float>(state.m_draft))
                      : fmt::format_to_n(state.m_text.data(),
                                         state.m_text.size() - 1,
                                         "{}",
                                         state.m_draft);
    // format_to_n 返回所需长度；截取长度后补零，不能按该值直接越界写入。
    state.m_text[std::min(result.size, state.m_text.size() - 1)] = '\0';
}
/// @brief 全文严格解析；错误或越界保留输入，不静默截断或保存非有限值。
/// @details 解析器的前缀成功不等于整段合法，必须检查消费长度及错误码。
/// 不采用 locale 敏感的逗号小数；也不自动把空串或溢出替换成零。
/// 输入无效时只撤去提交许可，不把上一次合法草稿写入业务字段。
void parseDraft()
{
    std::string_view text(state.m_text.data());
    // Apple 的低版本回退也限制为十进制，不能接受十六进制或 NaN 扩展。
    const bool decimal =
        !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' ||
                   c == 'e' || c == 'E';
        });
    // SafeParse 处理部署目标差异，消费长度与语义检查仍由这个表单负责。
    // 字符白名单只是语法前置过滤，不能代替解析器对指数或符号顺序的检查。
    const auto parsed = Internal::parseFloatingPrefix(text);
    // S32 除范围外还必须是整数，不能依赖强制转换偷偷丢掉小数部分。
    // 浮点边界已按目标类型设置，因此 float 转换也不能溢出为无穷大。
    state.m_valid =
        decimal && parsed.error == std::errc{} &&
        parsed.parsedLength == text.size() && std::isfinite(parsed.value) &&
        parsed.value >= state.m_min && parsed.value <= state.m_max &&
        (state.m_type != ImGuiDataType_S32 ||
         std::trunc(parsed.value) == parsed.value);
    // 不完整输入继续留在缓冲里，用户可修正；错误提示不抢焦点或清空输入。
    if ( state.m_valid ) state.m_draft = parsed.value;
}
}  // namespace
/// @brief 将系统收起键盘解释为结束文本焦点，而非应用当前业务草稿。
/// @param wasVisible 上一 UI 帧的平台遮挡状态。
/// @param visible 当前平台快照的遮挡状态，不由 WantTextInput 反推。
/// @warning 每帧固定检查；只在收起边沿清除焦点，不执行存盘或业务命令。
void releaseMobileTextFocusAfterKeyboardDismiss(bool wasVisible, bool visible)
{
    // SDL 后端依据 ImGui 文本焦点请求 IME；系统返回已收起时必须结束该请求。
    // 普通焦点切换与键盘出现不能走这里，否则会吞掉刚打开字段的首次输入。
    if ( wasVisible && !visible && ImGui::GetIO().WantTextInput )
        ImGui::ClearActiveID();
}
/// @brief 输入提交后按真实 ItemRect 滚动正文，复合数值框也采用同一口径。
/// @details InputDouble 等复合控件的导航矩形未必代表实际活动输入框。
/// 在对应控件后立即读取矩形，不在正文结束时借用最后一个提示项或旧导航位置。
/// @warning 高频固定检查；仅滚动当前子窗，禁止递归把父模态操作行推出边界。
void keepMobileInputItemVisible()
{
    if ( workspaceAllowsHoverFeedback() || !ImGui::IsItemActive() ) return;
    auto* window = ImGui::GetCurrentWindow();
    // 根窗口没有独立正文与底部行，不能因普通字段激活而滚动整个模态。
    if ( !(window->Flags & ImGuiWindowFlags_ChildWindow) ) return;
    ImGui::ScrollToRect(
        window,
        ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()),
        ImGuiScrollFlags_KeepVisibleEdgeY | ImGuiScrollFlags_NoScrollParent);
}
/// @brief 不改变页面整体大小，正文可以在键盘上方滚动且按钮始终可见。
/// @param id 表单内稳定的正文身份，不能包含随输入内容变化的标题。
/// @details 子窗口始终存在，键盘动画只改变尺寸约束，焦点不跨窗口迁移。
/// 正文采用自动高度，上限以下不留大块空白，上限以上保留滚动能力。
/// @warning 每帧仅操作 ImGui 栈与几何，不查询 JNI、文件系统或业务锁。
MobileInputBodyScope::MobileInputBodyScope(const char* id)
{
    if ( workspaceAllowsHoverFeedback() ) return;
    // 不使用旧物理像素，确认行下限与移动操作按钮使用同一 DPI。
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    // EndChild 后只需要一行间隔；三倍留白会在高 DPI 大键盘下耗尽正文。
    // 操作行高度继续遵守触摸下限，不通过缩小按钮来换取输入空间。
    const float footer = std::max(ImGui::GetFrameHeight(), 48.0f * scale) +
                         ImGui::GetStyle().ItemSpacing.y;
    // 同一移动表单始终保留子窗口，IME 切换只改变约束，输入框 ID 不变。
    // 自动高度让短表单紧凑，超出上限后由正文滚动，底部按钮不随正文移动。
    // 自动尺寸的父窗不能用上一帧小高度限制正文，否则首次打开会被锁死。
    // 固定高度的精调窗口则遵守自身边界，避免子窗把底部按钮推到窗外。
    const auto* window     = ImGui::GetCurrentWindow();
    const float modalLimit = Utils::centeredModalWindowMaxSize().y;
    const float windowLimit =
        (window->Flags & ImGuiWindowFlags_AlwaysAutoResize)
            ? modalLimit
            : std::min(window->Size.y, modalLimit);
    // 光标位置已经包含标题与正文之前的提示，预留高度不应重复扣除。
    // 极小可用区域收敛为一像素正文，父窗口操作行仍优先保留。
    const float bodyLimit =
        std::max(1.0f,
                 windowLimit - ImGui::GetCursorPosY() -
                     ImGui::GetStyle().WindowPadding.y - footer);
    ImGui::SetNextWindowSizeConstraints(
        { 0, 0 }, { std::numeric_limits<float>::max(), bodyLimit });
    // 将键盘导航扁平化到父窗，Tab 必须能从正文走到应用和取消按钮。
    // 该标志只影响焦点分组，滚动仍属于正文，不改变整个弹窗的位置。
    ImGui::BeginChild(id,
                      { 0, 0 },
                      ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysAutoResize |
                          ImGuiChildFlags_NavFlattened);
    // BeginChild 即使内容被裁剪也要求 EndChild，不能用返回值决定配对。
    m_open = true;
}
/// @brief 自动释放只影响布局栈，不触发任何应用或取消命令。
/// @details 若调用方因只读或校验分支提前返回，也不会遗留子窗口栈。
/// 正常路径仍应显式 end，将操作行放回父窗。
MobileInputBodyScope::~MobileInputBodyScope()
{
    end();
}
/// @brief 显式结束允许调用方在父窗口提交同一行确认／取消。
void MobileInputBodyScope::end()
{
    // 幂等结束允许显式操作行与析构共同使用，不会重复弹出窗口。
    if ( !m_open ) return;
    ImGui::EndChild();
    m_open = false;
}
/// @brief 从当前控件的真实矩形认领长按，在原父 popup 层级内编辑草稿。
/// @warning UI
/// 热路径不访问文件系统或会话锁；原始业务指针只在显式应用的本帧使用。
/// @param type 与原始 ImGui 控件一致的标量类型。
/// @param value 当前控件借出的源值地址，仅在显式应用的调用内写回。
/// @param minimum 可空的下界；只有上下界都有效且严格有序才启用业务范围。
/// @param maximum 可空的上界，不保存该指针供下一帧使用。
/// @param step 默认精调步长；无效浮点步长回退为 0.01，整数恒为一。
/// @return 源值实际变化的应用帧返回 true，其余帧返回 false。
/// @details 多个控件共享一个草稿，但归属为完整 ImGui ID，绝不按地址猜测。
bool mobileScalarPrecisionItem(ImGuiDataType type, void* value,
                               const void* minimum, const void* maximum,
                               double step)
{
    // 鼠标方案保持既有双击／组合键入口，不显示额外的触屏精调模态。
    if ( workspaceAllowsHoverFeedback() ||
         !Interaction::activeProfile().touchGestures() )
        return false;
    const auto generation = Interaction::profileGeneration();
    const int  frame      = ImGui::GetFrameCount();
    // 跨上下文不能比较旧窗口内容，帧号回退也意味着上下文可能被重建。
    // 目标连续缺席一帧即失效，防止关闭面板后在同名新字段上提交旧草稿。
    // 方案代际变化优先于 ID，即使用户切回触控也不能复活上次操作。
    if ( state.m_context != ImGui::GetCurrentContext() ||
         frame < state.m_frame || state.m_generation != generation ||
         (state.m_target && frame > state.m_seen + 1) )
        state = {};
    // 无事务时也记住上下文和方案代际，首次长按只需冻结当前字段值。
    state.m_context    = ImGui::GetCurrentContext();
    state.m_generation = generation;
    state.m_frame      = frame;
    // 只读取本次刚提交控件的身份，不缓存任何 ImGuiWindow 或业务所有权。
    const ImGuiID id = ImGui::GetItemID();
    double        current{};
    if ( !id || !readScalar(type, value, current) ) return false;
    const auto item     = ImGui::GetCurrentContext()->LastItemData;
    const bool disabled = (item.ItemFlags & ImGuiItemFlags_Disabled) != 0;
    // 已在其它模态中的数值也能打开子精调；只要求本窗口仍为真实可交互目标。
    // IsWindowHovered 会拒绝被更上层模态遮挡的父字段，不借用背景的旧触点。
    const bool eligible =
        !disabled &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    // 使用原始 Down 起点，而非抬手后的合成鼠标 Hover；滚动经过不认领。
    // 通用分类器负责多指、位移峰值、系统 Cancel 和单次长按交付。
    const bool hit = TouchReadPress::originInside(ImGui::GetItemRectMin(),
                                                  ImGui::GetItemRectMax());
    const auto press =
        state.m_press.update(id, hit && eligible, hit && eligible);
    // 在原控件的父层级打开模态，嵌套于 Timing 等弹窗时不切断父 popup。
    // 同名精调 popup 因控件 ID 隔离，多个属性不会争用同一个打开请求。
    ImGui::PushID(static_cast<int>(id));
    if ( !state.m_target && press == TouchReadPress::Result::Hold ) {
        // 只有静止长按进入草稿事务；打开本身不更新源值或发送业务命令。
        state.m_target   = id;
        state.m_type     = type;
        state.m_original = state.m_draft = current;
        // float 与整数遵守自身表示范围；double 使用它自己的有限边界。
        // int 的最小值不等于负的最大值，需要单独使用 lowest。
        const double limit =
            type == ImGuiDataType_Float ? std::numeric_limits<float>::max()
            : type == ImGuiDataType_S32 ? std::numeric_limits<int>::max()
                                        : std::numeric_limits<double>::max();
        state.m_min = type == ImGuiDataType_S32
                          ? std::numeric_limits<int>::lowest()
                          : -limit;
        state.m_max = limit;
        double low{}, high{};
        // DragScalar 的空范围或相等边界表示未限定，不能错误锁死精调。
        if ( readScalar(type, minimum, low) &&
             readScalar(type, maximum, high) && low < high ) {
            state.m_min = low;
            state.m_max = high;
        }
        // 选择步长属于弹窗草稿设置，不回写 AppConfig 或原控件拖动速度。
        state.m_step  = type == ImGuiDataType_S32         ? 1
                        : std::isfinite(step) && step > 0 ? step
                                                          : .01;
        state.m_valid = state.m_focus = true;
        formatDraft();
        FeedbackOpenPopup("###MobileScalarPrecision");
    }
    bool changed = false;
    if ( state.m_target == id ) {
        // 当前目标每帧重新借用地址，栈上临时参数也不会形成悬挂写入。
        state.m_seen = frame;
        // 源值被别的操作更新时旧草稿不覆盖它；也不保留任何栈上值地址。
        const bool stale =
            type != state.m_type || current != state.m_original || disabled;
        // 位置和尺寸共同使用键盘安全区；不把物理像素混入逻辑 DPI。
        // 每帧跟随快照，动画期间仍可继续输入，不等待键盘完成再更新。
        const auto                     area = workspaceTextInputWindowArea();
        Utils::CenteredModalPopupScope modal;
        if ( modal.begin("###MobileScalarPrecision",
                         nullptr,
                         ImGuiWindowFlags_NoTitleBar,
                         { std::min(area.m_size.x * .9f,
                                    420.0f * Config::AppConfig::instance()
                                                 .getWindowContentScale()),
                           std::min(area.m_size.y * .9f,
                                    330.0f * Config::AppConfig::instance()
                                                 .getWindowContentScale()) },
                         false) ) {
            // 字段和步进放入正文，操作行留在父模态，长内容只滚动局部。
            MobileInputBodyScope body("PrecisionFields");
            ImGui::TextUnformatted(TR("ui.mobile.precision.title").data());
            // 长按仍按住时不请求文本焦点；原触点结束后才聚焦一次。
            // 否则首次打开模态的按下状态可能立即撤销刚设置的输入焦点。
            if ( state.m_focus && !Graphic::mobileTouchSnapshot().m_count ) {
                ImGui::SetKeyboardFocusHere();
                state.m_focus = false;
            }
            // 输入框填满正文可见宽度，错误提示和步长可以独立向下滚动。
            ImGui::SetNextItemWidth(-1);
            // 软键盘 Enter 只结束／继续输入；只有下面的应用按钮能够写回源值。
            if ( ImGui::InputText("##PrecisionValue",
                                  state.m_text.data(),
                                  state.m_text.size(),
                                  ImGuiInputTextFlags_CharsScientific |
                                      ImGuiInputTextFlags_AutoSelectAll) )
                parseDraft();
            keepMobileInputItemVisible();
            ImGui::TextUnformatted(TR("ui.mobile.precision.step").data());
            // 固定四个选项无动态容器；整数不显示不能表示的分数步长。
            // 显示文本不作为持久化键，PushID 由固定数值生成。
            for ( double candidate : { .001, .01, .1, 1.0 } ) {
                if ( type == ImGuiDataType_S32 && candidate < 1 ) continue;
                ImGui::PushID(static_cast<int>(candidate * 1000));
                const auto label = candidate == .001  ? "0.001"
                                   : candidate == .01 ? "0.01"
                                   : candidate == .1  ? "0.1"
                                                      : "1";
                if ( FeedbackSelectableButton(label,
                                              state.m_step == candidate) )
                    state.m_step = candidate;
                ImGui::PopID();
                ImGui::SameLine();
            }
            ImGui::NewLine();
            // 校验不通过时允许继续修正文本，但不得用步进覆盖错误草稿。
            ImGui::BeginDisabled(!state.m_valid || stale);
            for ( int direction : { -1, 1 } ) {
                if ( FeedbackButton(direction < 0 ? "-" : "+",
                                    { ImGui::GetFrameHeight() * 2, 0 }) ) {
                    // 步进先检查有限性，再夹取业务边界，不能写出无穷大。
                    const double next =
                        state.m_draft + direction * state.m_step;
                    // 极端 double 步进可能溢出；拒绝这一步而非污染合法草稿。
                    if ( std::isfinite(next) ) {
                        state.m_draft =
                            std::clamp(next, state.m_min, state.m_max);
                        formatDraft();
                    }
                }
                if ( direction < 0 ) ImGui::SameLine();
            }
            ImGui::EndDisabled();
            // 错误信息位于正文，不挤掉父窗的取消入口。
            if ( !state.m_valid )
                ImGui::TextWrapped("%s",
                                   TR("ui.mobile.precision.invalid").data());
            // 正文结束后才能绘制操作行，IME 不会把确认按钮带入滚动末尾。
            body.end();
            ImGui::BeginDisabled(!state.m_valid || stale);
            if ( FeedbackButton(TR("ui.timeline.event_editor.apply").data()) ) {
                // 同值应用也会关闭弹窗，但不报告 changed，避免空业务历史。
                // 写回后立即清除目标，本帧及后续帧都不会重复交付同一草稿。
                writeScalar(type, value, state.m_draft);
                // 以实际存储后的值判断，float 舍入回原值不产生空编辑边沿。
                double written{};
                changed =
                    readScalar(type, value, written) && current != written;
                ImGui::CloseCurrentPopup();
                state.m_target = 0;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            // 冲突与取消只销毁本地草稿，不用撤销命令回滚别人的源值变化。
            if ( FeedbackButton(TR("ui.timeline.event_editor.cancel").data()) ||
                 stale ) {
                ImGui::CloseCurrentPopup();
                state.m_target = 0;
            }
            ImGui::EndPopup();
        } else
            // 系统返回或其它关闭路径不会补提交，下一次长按重新取源值。
            state.m_target = 0;
    }
    // 恢复调用方原有 ID 栈，不能让后续属性继承当前控件的作用域。
    ImGui::PopID();
    // 嵌套弹窗不能把 LastItem 留成内部按钮，外层仍观察原数值控件的身份。
    auto& context        = *ImGui::GetCurrentContext();
    context.LastItemData = item;
    if ( changed ) {
        // 既有属性可能只在 IsItemDeactivatedAfterEdit 时提交高成本配置。
        // 精调应用等价于原控件一次完成编辑，不能只返回 changed 丢掉结束边沿。
        // 只在真实值变化的本帧发布，不伪造鼠标按下或夺取 ActiveId。
        context.LastItemData.StatusFlags |=
            ImGuiItemStatusFlags_Edited | ImGuiItemStatusFlags_HasDeactivated |
            ImGuiItemStatusFlags_Deactivated;
        context.DeactivatedItemData = { id, frame, true, true };
    }
    // 包装器沿用原 changed 契约，业务层仍自行决定如何提交属性命令。
    // 本函数不直接创建历史项或触发存盘，避免双重提交。
    return changed;
}
}  // namespace MMM::UI
