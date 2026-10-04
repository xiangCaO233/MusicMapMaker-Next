#include "canvas/TimingInterpolationControls.h"

#include "config/TimingInterpolationPreferences.h"
#include "mmm/timing/Timing.h"
#include "mmm/timing/TimingFunction.h"
#include "mmm/timing/TimingInterpolation.h"
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace MMM::Canvas
{
// 配置标识独立于 UI 翻译；增添领域枚举时必须同步稳定标识列表。
static_assert(Config::TIMING_CURVE_PREFERENCE_NAMES.size() ==
              static_cast<std::size_t>(TimingCurve::Custom) + 1);

/// @brief 只恢复可复用工具选项，不用旧时长或参数覆盖本次拖选。
/// @param curve 新段的工作副本，时间范围和端值已由创建入口确定。
/// @param effect 本次选择的效果类型，不从上次使用的泳道推断。
/// @param preferences 已经过配置读取器规范化的工具选项。
/// @pre 仅用于新建段落；编辑已有实体不得调用此默认恢复入口。
/// @details 控制点为比例，跨不同段长可以保留同一种变化形状。
/// 公式是绝对参数函数，定义域准备后仍须通过当前效果的范围校验。
/// @warning 打开编辑器的低频入口；不遍历红线、编译公式或写入文件。
/// @note BPM 不允许拍域，避免其自身定义与拍轴形成循环。
void applyTimingInterpolationPreferences(
    TimingInterpolation& curve, TimingEffect effect,
    const Config::TimingInterpolationPreferences& preferences)
{
    const auto& names = Config::TIMING_CURVE_PREFERENCE_NAMES;
    // 未知标识可能来自更新版本，不能将查找末位置窄化为非法曲线枚举。
    const auto found =
        std::find(names.begin(), names.end(), preferences.m_curve);
    curve.m_curve    = found == names.end()
                           ? TimingCurve::Linear
                           : static_cast<TimingCurve>(found - names.begin());
    curve.m_variable = preferences.m_useBeats && effect != TimingEffect::BPM
                           ? TimingVariable::Beat
                           : TimingVariable::Time;
    curve.m_samplesPerSecond = preferences.m_samplesPerSecond;
    // 分拍整数与密度均保留；启用拍域时密度随后由新拍轴重新推导。
    // 此处没有 BPM 数据，不能借旧平均速度提前生成一个虚假的拍轴。
    curve.m_beatNumerator   = preferences.m_beatNumerator;
    curve.m_beatDenominator = preferences.m_beatDenominator;
    curve.m_controlX1       = preferences.m_controls[0];
    curve.m_controlY1       = preferences.m_controls[1];
    curve.m_controlX2       = preferences.m_controls[2];
    curve.m_controlY2       = preferences.m_controls[3];
    // 缓存绑定原段定义域；下一个段落必须由窗口重新准备函数与拍轴。
    curve.m_function.reset();
    curve.m_beatAxis.reset();
}

/// @brief 在确认保存的低频入口捕获选项；公式仅持久化源码。
/// @param curve 已完成当前效果及范围校验的段落工作副本。
/// @return 与配置根序列化接口一致的轻量选项对象。
/// @pre 取消、错误输入和尚未完成手势不进入此函数。
/// @details 采用值返回，配置不观察窗口内部缓冲或 ImGui 对象。
/// @warning 仅明确保存动作调用；可能复制公式字符串，不属于常态绘制。
/// @note 起终参数与时长来自谱面，不能作为全局默认写入下一张谱面。
Config::TimingInterpolationPreferences captureTimingInterpolationPreferences(
    const TimingInterpolation& curve)
{
    Config::TimingInterpolationPreferences result;
    const auto index = static_cast<std::size_t>(curve.m_curve);
    // 默认对象提供线性回退；范围外的枚举不能访问固定标识数组。
    if ( index < Config::TIMING_CURVE_PREFERENCE_NAMES.size() )
        result.m_curve = Config::TIMING_CURVE_PREFERENCE_NAMES[index];
    result.m_useBeats         = curve.m_variable == TimingVariable::Beat;
    result.m_samplesPerSecond = curve.m_samplesPerSecond;
    result.m_beatNumerator    = curve.m_beatNumerator;
    result.m_beatDenominator  = curve.m_beatDenominator;
    result.m_controls         = { curve.m_controlX1,
                                  curve.m_controlY1,
                                  curve.m_controlX2,
                                  curve.m_controlY2 };
    if ( curve.m_curve == TimingCurve::Custom && curve.m_function )
        // 只借用不可变函数源式，配置不持有共享数学缓存的所有权。
        result.m_expression = timingFunctionExpression(*curve.m_function);
    return result;
}

/// @brief 约束拖动坐标，不允许两枚控制点横向交叉。
/// @param curve 当前窗口的工作副本，修改不会直接发布谱面命令。
/// @param index 零代表第一控制点，一代表第二控制点。
/// @param x 在完整自变量域中的比例，超出边界时夹取。
/// @param y 在起终参数差值中的比例，与真实端值方向无关。
/// @return 有效投影改变至少一个坐标时为真；无变化返回假。
/// @details X 顺序用于领域贝塞尔反解，不能允许手势制造时间回折。
/// Y 不要求两控制点有序，先快后慢与先慢后快都应允许。
/// @note 拖到图表外仍生效，但最多到合法端点，不创建区间外控制点。
/// @warning 连续输入只修改窗口工作副本，曲线与输出预览同帧更新。
bool moveTimingBezierControl(TimingInterpolation& curve, int index, double x,
                             double y)
{
    // 非有限鼠标投影和非法句柄不应污染下一帧绘制列表。
    if ( index < 0 || index > 1 || !std::isfinite(x) || !std::isfinite(y) )
        return false;
    double& targetX = index == 0 ? curve.m_controlX1 : curve.m_controlX2;
    // 引用仅存在于本次调用，返回后没有挂在 UI 上的成员地址。
    double& targetY = index == 0 ? curve.m_controlY1 : curve.m_controlY2;
    // 边缘夹取保持单调 X；纵轴独立，可自由改变曲率但不超出端值包围。
    const double otherX = std::clamp(
        index == 0 ? curve.m_controlX2 : curve.m_controlX1, 0.0, 1.0);
    x = std::clamp(x, index == 0 ? 0.0 : otherX, index == 0 ? otherX : 1.0);
    y = std::clamp(y, 0.0, 1.0);
    // 相同坐标不产生变更反馈，避免静止按住手柄时重复标记编辑状态。
    if ( targetX == x && targetY == y ) return false;
    targetX = x;
    targetY = y;
    return true;
}

/// @brief 将单位贝塞尔图投影到屏幕，通过稳定句柄 ID 保持拖动状态。
/// @param curve 上方数值控件使用的同一组工作副本坐标。
/// @param size 图表外框像素尺寸，调用方保证正宽高并预留标签空间。
/// @return 任一手柄在本帧改变坐标时为真。
/// @pre 调用方已选择贝塞尔曲线，且当前 ImGui 窗口正在提交内容。
/// @details 起终点固定为零与一，端值由上方参数输入控制；手柄仅改变曲率。
/// 横轴为自变量比例，不等于内部三次曲线的参数 t。
/// 图形与数字输入共用真值，没有需要额外确认或同步的第二份曲线。
/// @note 背景、折线和手柄颜色来自当前 ImGui 样式，适应主题切换。
/// @warning 每帧最多绘制 64 段；拖动即刻生效，不进行采样数组或配置写入。
bool renderTimingBezierControls(TimingInterpolation& curve, const ImVec2& size)
{
    const auto   origin = ImGui::GetCursorScreenPos();
    const ImVec2 inner(origin.x + 16, origin.y + 16);
    const ImVec2 extent(std::max(1.f, size.x - 32), std::max(1.f, size.y - 32));
    // 为手柄留出边缘空间，零与一的位置都能完整显示与命中。
    ImGui::Dummy(size);
    const auto next = ImGui::GetCursorScreenPos();
    // 图形 Y 轴向上增长，屏幕 Y 轴向下增长，投影与拖动必须相互逆变换。
    const auto project = [&](double x, double y) {
        // 数字输入可能处于非法中间态；显示使用有限夹取，不改写用户文本。
        x = std::isfinite(x) ? std::clamp(x, 0.0, 1.0) : 0.0;
        y = std::isfinite(y) ? std::clamp(y, 0.0, 1.0) : 0.0;
        return ImVec2(inner.x + static_cast<float>(x) * extent.x,
                      inner.y + (1.f - static_cast<float>(y)) * extent.y);
    };
    bool                changed = false;
    std::array<bool, 2> highlighted{};
    ImGui::PushID("TimingBezierControls");
    // 每枚手柄的 ID 不依赖坐标，移动其命中矩形不会丢失活动拖动状态。
    for ( int index = 0; index < 2; ++index ) {
        const double x     = index == 0 ? curve.m_controlX1 : curve.m_controlX2;
        const double y     = index == 0 ? curve.m_controlY1 : curve.m_controlY2;
        const auto   point = project(x, y);
        ImGui::SetCursorScreenPos({ point.x - 10, point.y - 10 });
        ImGui::PushID(index);
        // 无外观命中区不使用普通按钮反馈；句柄本身提供悬浮高亮。
        ImGui::InvisibleButton("handle", { 20, 20 });
        highlighted[index] = ImGui::IsItemHovered() || ImGui::IsItemActive();
        // 鼠标离开命中框后仍显示活动反馈，表明此次拖动继续控制原手柄。
        if ( highlighted[index] )
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        // 激活帧的鼠标位移属于到达手柄的移动，不能误当成拖动增量。
        if ( ImGui::IsItemActive() && !ImGui::IsItemActivated() &&
             ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) ) {
            // 使用增量保持抓取偏移，点击手柄边缘时不会突然跳到鼠标中心。
            const auto delta = ImGui::GetIO().MouseDelta;
            // X 和 Y 分别按自己的屏幕跨度归一，窗口长宽不同也不扭曲拖动。
            changed |= moveTimingBezierControl(
                curve, index, x + delta.x / extent.x, y - delta.y / extent.y);
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    // 控件位置更新后再绘曲线，保证手柄、辅助线与下方输出即时一致。
    auto*      draw  = ImGui::GetWindowDrawList();
    const auto color = ImGui::GetColorU32(ImGuiCol_PlotLines);
    draw->AddRectFilled(origin,
                        { origin.x + size.x, origin.y + size.y },
                        ImGui::GetColorU32(ImGuiCol_FrameBg),
                        ImGui::GetStyle().FrameRounding);
    const auto start = project(0, 0), end = project(1, 1);
    const auto first  = project(curve.m_controlX1, curve.m_controlY1),
               second = project(curve.m_controlX2, curve.m_controlY2);
    // 辅助线明确每枚控制点对应哪个端点，两个端点不作为可拖动句柄。
    draw->AddLine(start, first, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    draw->AddLine(second, end, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ImVec2 previous = start;
    // 单位三次曲线最多两次转弯，固定分段不承担高频函数预览职责。
    // 几何反映归一曲率，真实参数范围与输出落点由下方预览另行展示。
    for ( int index = 1; index <= 64; ++index ) {
        const double t = index / 64.0, u = 1 - t;
        // 按参数 t 画几何贝塞尔，不能把控制点 X 当等分时间直接套到 Y。
        const auto point =
            project(3 * u * u * t * curve.m_controlX1 +
                        3 * u * t * t * curve.m_controlX2 + t * t * t,
                    3 * u * u * t * curve.m_controlY1 +
                        3 * u * t * t * curve.m_controlY2 + t * t * t);
        draw->AddLine(previous, point, color, 2);
        previous = point;
    }
    draw->AddCircleFilled(start, 3, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    draw->AddCircleFilled(end, 3, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    // 点号随手柄移动；数值输入保留用于精确定位，二者共享同一组标量。
    const std::array points{ first, second };
    for ( int index = 0; index < 2; ++index ) {
        draw->AddCircleFilled(
            points[index],
            highlighted[index] ? 8 : 6,
            ImGui::GetColorU32(highlighted[index] ? ImGuiCol_PlotLinesHovered
                                                  : ImGuiCol_SliderGrab));
        draw->AddText({ points[index].x + 10, points[index].y - 10 },
                      ImGui::GetColorU32(ImGuiCol_Text),
                      index == 0 ? "1" : "2");
    }
    // 手柄采用局部光标位置，结束后恢复正常布局，不挤压后续数值输入。
    ImGui::SetCursorScreenPos(next);
    // 光标恢复后提交零尺寸项目，满足 ImGui 的父边界登记契约。
    ImGui::Dummy({ 0, 0 });
    return changed;
}
}  // namespace MMM::Canvas
