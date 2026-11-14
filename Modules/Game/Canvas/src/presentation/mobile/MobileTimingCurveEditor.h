#pragma once
#include "MobileCanvasNavigation.h"
#include "canvas/TimingFunctionEditorState.h"
#include "canvas/TimingInterpolationControls.h"
#include "canvas/TimingInterpolationPreview.h"
#include "config/skin/translation/Translation.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <imgui.h>

namespace MMM::Canvas
{
/// @brief 参数模态内的独立触控图表，不将移动状态传播到画布公开头。
/// @details 每个图有自己的代际，不能由滑入图内的手指补开始绘制。
/// 贝塞尔和手绘的取消只恢复本笔窗口副本，不访问谱面历史。
/// 输出图只导航本地视野，二指轻点不转为谱面撤销。
/// 图表建立不可委托的子窗，外部正文继续使用普通触屏滚动。
/// @par 状态与业务边界
/// 控制点只写模态窗口工作副本，外层保存才发布原有段落命令。
/// 手绘格点是拟合输入，不等于真正写出的 Timing 事件列表。
/// 输出导航只改横向显示范围，采样密度与函数定义始终保持独立。
/// 取消只还原本笔触及的轻量值，不能撤销此前已经保存的谱面编辑。
/// @par 所有权
/// 三个图分别锁定起始代际，隐藏图不能在滑过时接管同一根手指。
/// 同一手势内峰值指头数不会下降，因此第二指抬起后也不会继续原笔画。
/// 新窗口必须 reset，不能消费打开菜单之前已经结束的触点。
/// 不保存 ImGuiWindow 地址，布局重建不会使手势持有失效窗口指针。
/// @par 投影约定
/// 控制点沿用原单位贝塞尔图，横轴表示自变量比例，不是内部参数 t。
/// 原绘图保留 16 逻辑像素边缘，触摸热区另外扩大，不改变函数几何。
/// 手绘图横轴覆盖完整自变量域，局部修改不能扩展段落持续时间。
/// 纵轴记录实际参数而不是像素，窗口尺寸改变不会重新采样已有手绘。
class MobileTimingCurveEditor
{
    /// @brief 同一窗口中的不同图表分别保持起笔归属和取消门闩。
    struct Contact {
        std::uint64_t m_epoch{};     ///< 仅新物理代际可开始，结束后不补发输入。
        bool m_owned{}, m_active{};  ///< 所有权与本笔存活状态分开保存。
        int  m_frame{ -1 };  ///< 最后提交布局帧，用于识别折叠或无效轴隐藏。
        /// @brief 借用一帧的触点意图，不持有原生事件或窗口指针。
        struct Input {
            ImVec2 m_point{}, m_start{};  ///< 屏幕逻辑坐标，释放采用最终位置。
            bool   m_begin{}, m_end{}, m_cancel{}, m_active{};
        };
        /// @brief 原始触点绕开普通按钮延迟，起点仍须位于当前可见剪裁区域。
        /// @warning 每帧固定触点查询，无事件合成、分配或跨线程等待。
        /// @param origin 当前图表屏幕原点，包含父页滚动带来的位移。
        /// @param size 逻辑尺寸，与 DisplaySize 的坐标空间一致。
        /// @param multi 仅输出预览允许多指，其它图遇到第二指必须取消。
        /// @return 始末沿和当前坐标均按值返回，不能跨帧保留原始触点引用。
        Input poll(ImVec2 origin, ImVec2 size, bool multi = false)
        {
            m_frame         = ImGui::GetFrameCount();
            const auto& raw = Graphic::mobileTouchSnapshot();
            const auto& io  = ImGui::GetIO();
            Input       out;
            out.m_start = { raw.m_originX * io.DisplaySize.x,
                            raw.m_originY * io.DisplaySize.y };
            out.m_point = raw.m_count
                              ? ImVec2(raw.m_contacts[0].m_x * io.DisplaySize.x,
                                       raw.m_contacts[0].m_y * io.DisplaySize.y)
                              : io.MousePos;
            // 整帧内已抬起的快速输入仍保留原始起点；最终位置借用同帧鼠标事件。
            // 不读取数组尾部的残留触点，压缩后的联系人索引没有稳定身份含义。
            if ( !raw.m_epoch ) return out;
            if ( m_epoch != raw.m_epoch ) {
                m_epoch = raw.m_epoch;
                // 起点归属只采样一次；之后图外移动仍属于原图或原滚动页面。
                // 当前绘制剪裁包含祖先窗口，不仅仅比较未裁剪的图表包围框。
                const auto clipMin =
                    ImGui::GetWindowDrawList()->GetClipRectMin();
                const auto clipMax =
                    ImGui::GetWindowDrawList()->GetClipRectMax();
                // 起点和剪裁同时命中，隐藏在滚动区外的图不能抢走页面输入。
                // 以原始 Down 冻结归属，移动后经过另一个图也不会交接。
                m_owned =
                    out.m_start.x >= std::max(origin.x, clipMin.x) &&
                    out.m_start.x < std::min(origin.x + size.x, clipMax.x) &&
                    out.m_start.y >= std::max(origin.y, clipMin.y) &&
                    out.m_start.y < std::min(origin.y + size.y, clipMax.y);
                m_active    = m_owned;
                out.m_begin = m_owned;
            }
            if ( !m_active ) return out;
            // 已经结束的代际保持惰性，空闲重复帧不能补交另一次 Up。
            // 抢占门闩只会在下一真实 Down 时重新评估。
            // 第二指只对预览导航合法；编辑图整笔回退，残留首指不能重新落笔。
            // Cancel 永远不作为正常 Up，零触点只结束已有事务。
            out.m_cancel = raw.m_cancelled || (!multi && raw.m_peak > 1);
            out.m_end    = raw.m_count == 0 && !out.m_cancel;
            out.m_active = true;
            if ( out.m_cancel || out.m_end ) m_active = false;
            return out;
        }
    };

public:
    /// @brief 新段落副本丢弃旧图表身份，不允许尚未抬起的旧触点接管新窗口。
    /// @warning 低频窗口初始化，只清理固定值，不复制数学缓存。
    /// @pre 调用者已经载入新的参数副本，旧笔画无需写回或反向发布。
    /// @note 记录当前代际而非清零，打开菜单的释放不会立即落在新图上。
    void reset()
    {
        const auto epoch = Graphic::mobileTouchSnapshot().m_epoch;
        m_bezier = m_sketch = m_preview = {};
        m_bezier.m_epoch = m_sketch.m_epoch = m_preview.m_epoch = epoch;
        // 局部导航的配对与倍率同样失效，不能让上一窗口的两指距离影响新图。
        m_control     = -1;
        m_sketchDirty = false;
        m_navigation  = {};
        m_zoom        = 1;
        m_symbolInput = m_symbolHelp = nullptr;
    }
    /// @brief 图表内触控由本类负责，父页面不把笔画解释为纵向滚动。
    /// @return 始终已 BeginChild，调用者即使不可见也须配对 EndChild。
    /// @warning 每帧仅登记固定子窗，窗口内边距不再重复占用绘图区。
    /// @param id 由调用者提供稳定图表身份，不包含可变数值或语言文本。
    /// @param size 当前正文可用宽度和触摸高度，不修改全局 UI 比例。
    /// @note NoScrollbar 与 NoScrollWithMouse 组合是通用触屏滚动的画布边界。
    /// @note 仅图内阻止委托，外部表单和底部保存按钮不在该子窗中。
    bool beginSurface(const char* id, ImVec2 size)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        ImGui::BeginChild(
            id,
            size,
            ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        return true;
    }
    /// @brief 手指直接抓控制点，扩大热区而不放大几何图形。
    /// @details 抓取偏置固定，取消恢复四个起笔值；单调 X 由原助手约束。
    /// @warning 连续帧只更新四个标量并复用原固定预算绘图。
    /// @param curve 与数字输入共用同一工作副本，结束后不保留引用。
    /// @param size 水平跨度来自实际窗口，不用设备分辨率猜测比例。
    /// @param scale 只影响触摸热区和最低高度，不改变控制点的归一坐标。
    /// @return 当前帧移动或取消恢复改变了副本时为真。
    bool bezier(TimingInterpolation& curve, ImVec2 size, float scale)
    {
        size.y = std::max(size.y, 240 * scale);
        beginSurface("##TouchBezier", size);
        const auto origin = ImGui::GetCursorScreenPos();
        // 完整图的不可见命中区阻止 ImGui 把图内拖动识别为移动整个模态窗口。
        // 实际编辑仍消费原始触点，按钮返回值不执行任何模型动作。
        ImGui::InvisibleButton("##TouchBezierInput", size);
        ImGui::SetCursorScreenPos(origin);
        const ImVec2 inner{ origin.x + 16, origin.y + 16 };
        const ImVec2 extent{ std::max(1.f, size.x - 32),
                             std::max(1.f, size.y - 32) };
        const auto   in = m_bezier.poll(origin, size);
        if ( in.m_begin ) {
            // 起笔复制四个 double，而非复制 TimingInterpolation
            // 的共享缓存所有权。
            // 即使之后夹到边界，取消仍能精确恢复起笔前的全部控制点。
            m_original = { curve.m_controlX1,
                           curve.m_controlY1,
                           curve.m_controlX2,
                           curve.m_controlY2 };
            m_control  = -1;
            float best = 24 * scale;
            for ( int i = 0; i < 2; ++i ) {
                const float x =
                    inner.x + static_cast<float>(m_original[i * 2]) * extent.x;
                const float y =
                    inner.y +
                    (1 - static_cast<float>(m_original[i * 2 + 1])) * extent.y;
                const float distance =
                    std::hypot(in.m_start.x - x, in.m_start.y - y);
                // 重叠时固定采用较近控制点，不能在移动中重新命中另一枚。
                if ( distance < best ) {
                    best      = distance;
                    m_control = i;
                }
            }
            m_start = in.m_start;
        }
        bool changed = false;
        // 空白起手没有选中控制点，不从途中最近点补建一次编辑。
        if ( in.m_active && m_control >= 0 && !in.m_cancel ) {
            // 绝对位移避免帧合并丢增量；轻点边缘不把点吸到手指中心。
            // 释放坐标会取整，原始 Down 可能有子像素；一像素内精确恢复原值。
            // 只消除返回起点的投影误差，不将正常拖动改成累计增量。
            const float dx       = in.m_point.x - m_start.x,
                        dy       = in.m_point.y - m_start.y;
            const bool  atOrigin = std::abs(dx) < 1 && std::abs(dy) < 1;
            changed              = moveTimingBezierControl(
                curve,
                m_control,
                m_original[m_control * 2] + (atOrigin ? 0 : dx) / extent.x,
                m_original[m_control * 2 + 1] - (atOrigin ? 0 : dy) / extent.y);
        }
        if ( in.m_cancel && m_control >= 0 ) {
            // 第二指与系统中断共用恢复路径，不向原动作栈发布 Undo。
            curve.m_controlX1 = m_original[0];
            curve.m_controlY1 = m_original[1];
            curve.m_controlX2 = m_original[2];
            curve.m_controlY2 = m_original[3];
            changed           = true;
        }
        // 共享绘图仅消费真值，不运行原鼠标命中，桌面函数的默认行为不变。
        renderTimingBezierControls(curve, size, false);
        if ( in.m_end || in.m_cancel ) m_control = -1;
        ImGui::EndChild();
        return changed;
    }
    /// @brief 连续手绘即时修改固定格点；正常松手才使旧拟合失效。
    /// @details 旧拟合作为蓝色参考保留，取消不需要复制或重编译函数缓存。
    /// @warning 每次最多覆盖 129 个格点，容量固定，无动态分配或模型命令。
    /// @param s 数组属于当前模态，不访问会话注册表或文件。
    /// @param origin 实际图原点，横纵投影分别使用自己的屏幕跨度。
    /// @param size 图外触点夹取到边界，快速扫到右边缘仍补齐终点。
    /// @return 真表示已消费该平台图表的输入，原鼠标状态机不得再次处理。
    bool sketch(TimingFunctionEditorState& s, ImVec2 origin, ImVec2 size)
    {
        // 此命中区只有输入所有权，不使用可见按钮外观或点击音效。
        ImGui::InvisibleButton("##TouchSketchInput", size);
        const auto in = m_sketch.poll(origin, size);
        if ( in.m_begin ) {
            // 备份真实旧格点与覆盖标志；不能用清零代替精确取消。
            m_values          = s.m_values;
            m_drawn           = s.m_drawn;
            s.m_previousIndex = -1;
            m_sketchDirty     = false;
        }
        if ( in.m_active && !in.m_cancel ) {
            // 初帧先记录真实起点，快速横扫也会覆盖左侧起始格点。
            if ( in.m_begin ) sketchPoint(s, in.m_start, origin, size);
            sketchPoint(s, in.m_point, origin, size);
            m_sketchDirty = true;
        }
        if ( in.m_cancel ) {
            // 旧拟合从未失效，恢复格点即可继续比较或重新采用原结果。
            s.m_values = m_values;
            s.m_drawn  = m_drawn;
        } else if ( in.m_end && m_sketchDirty ) {
            // 只在完成边沿释放旧候选，取消路径因此完整保留以前的拟合。
            s.m_fit.reset();
            s.m_fitFunction.reset();
            s.m_fitError.clear();
        }
        if ( in.m_end || in.m_cancel ) s.m_previousIndex = -1;
        return true;
    }
    /// @brief 输出图单指横移、双指平移捏合；显式按钮替代滚轮和双击。
    /// @details 二／三指不执行 Undo／Redo；导航不改变输出函数或密度。
    /// @warning 每帧只变更本地视野，缓存重建仍由原预览键决定。
    /// @note 复用主画布的缩放死区，双指平移中的自然开合不微调倍率。
    /// @note 返回的历史轻触标志故意不消费，模态图不是谱面编辑表面。
    /// @note 一指阶段发生的横移按增量应用，加入第二指后不再回放第一指。
    bool preview(TimingInterpolationPreview& p, ImVec2 origin, ImVec2 size,
                 float scale)
    {
        ImGui::InvisibleButton("##TouchPreviewInput", size);
        const auto  in  = m_preview.poll(origin, size, true);
        const auto& raw = Graphic::mobileTouchSnapshot();
        if ( in.m_begin ) m_last = in.m_start;
        if ( in.m_active && !in.m_cancel && raw.m_peak == 1 ) {
            const float dx = in.m_point.x - m_last.x;
            // 只平移横向视野，不把纵向抖动改成参数编辑或局部缩放。
            p.pan(-dx / size.x);
            m_last = in.m_point;
        }
        const auto& io  = ImGui::GetIO();
        const auto  nav = m_navigation.update(raw,
                                              m_preview.m_owned,
                                              io.DisplaySize.x,
                                              io.DisplaySize.y,
                                              m_zoom,
                                              10 * scale);
        if ( nav.m_changed ) {
            // 单指与第二指的加入只切换导航归属，不生成额外画点。
            // 原导航返回绝对倍率，预览接受视野比例，两者按旧／新倍率换算。
            // 缩放锚点保持原中点，再应用平移；不能先平移而造成检查目标漂移。
            p.zoomAt((nav.m_anchorX - origin.x) / size.x, m_zoom / nav.m_zoom);
            p.pan(-nav.m_x / size.x);
            m_zoom = nav.m_zoom;
        }
        return true;
    }
    /// @brief 图表结束后显示独立触摸按钮，不让按钮参与图表拖动。
    /// @warning 离散点击仅调整视野，不调用拟合、编译或谱面命令。
    /// @note 按钮位于子窗结束之后，始终留在原表单滚动链中。
    /// @note 全段只重置显示范围，不清空手绘点或改变拟合函数。
    void previewButtons(TimingInterpolationPreview& p, float scale)
    {
        // 三个按钮共享同一行，但命中区仍分开，间距始终取用户主题设置。
        // 这里不记录默认缩放，下一笔捏合按实际视野继续操作。
        const float  width = std::max(1.f,
                                      (ImGui::GetContentRegionAvail().x -
                                       2 * ImGui::GetStyle().ItemSpacing.x) /
                                          3);
        const ImVec2 size{ width, 44 * scale };
        // 等分当前可用宽度，英文标签不会通过固定像素宽度溢出小屏右边界。
        if ( UI::FeedbackButton(TR("ui.timing.curve.zoom_out").data(), size) )
            p.zoomAt(.5, 2);
        ImGui::SameLine();
        if ( UI::FeedbackButton(TR("ui.timing.curve.reset_view").data(), size) )
            p.resetView();
        ImGui::SameLine();
        if ( UI::FeedbackButton(TR("ui.timing.curve.zoom_in").data(), size) )
            p.zoomAt(.5, .5);
        ImGui::TextWrapped("%s", TR("ui.timing.curve.touch_preview").data());
    }
    /// @brief 点击插入符号时同时记住说明，不需要手指保持悬浮。
    /// @note 参数观察静态模板表，既不分配字符串，也不捕获输入缓冲。
    /// @warning 每帧仅检查按钮结果，未点击时不改动当前帮助选择。
    void symbolHelp(const char* input, const char* help, bool clicked)
    {
        if ( clicked ) {
            m_symbolInput = input;
            m_symbolHelp  = help;
        }
    }
    /// @brief 符号网格下面展示最近模板的语法与限制，说明可以随正文滚动。
    /// @warning 每帧只读取静态文本，不编译或反射数学函数。
    /// @note 当前符号说明不是 Tooltip，手指松开后仍能阅读并滚动查看。
    void finishSymbols()
    {
        // 没有选择符号时保持紧凑；选择后的帮助不随手指松开消失。
        // 固定格式串使翻译中的百分号不能成为额外格式指令。
        if ( !m_symbolHelp ) return;
        ImGui::TextWrapped(TR("ui.timing.curve.symbol_expression").data(),
                           m_symbolInput);
        ImGui::TextWrapped("%s", TR(m_symbolHelp).data());
    }
    /// @brief 折叠、轴失效或曲线切换隐藏活动图时恢复本笔，不能保存半笔。
    /// @details 窗口仍存在但图未提交与正常 Up 不同，恢复不借用谱面历史。
    /// @warning 每帧固定标量检查，真正隐藏时最多复制 129 个轻量格点。
    /// @param c 与贝塞尔图共用的当前副本，不触碰起终参数或段长。
    /// @param s 与自由绘图共用的格点状态，保留之前有效的拟合候选。
    void finish(TimingInterpolation& c, TimingFunctionEditorState& s)
    {
        const int frame = ImGui::GetFrameCount();
        if ( m_bezier.m_active && m_bezier.m_frame != frame ) {
            // 图被折叠不等于释放；恢复后禁止同代际再次隐式起笔。
            if ( m_control >= 0 ) {
                c.m_controlX1 = m_original[0];
                c.m_controlY1 = m_original[1];
                c.m_controlX2 = m_original[2];
                c.m_controlY2 = m_original[3];
            }
            m_bezier.m_active = false;
            m_control         = -1;
        }
        if ( m_sketch.m_active && m_sketch.m_frame != frame ) {
            // 无效轴输入也会隐藏手绘图，不能留下依赖旧轴的半笔修改。
            s.m_values        = m_values;
            s.m_drawn         = m_drawn;
            s.m_previousIndex = -1;
            m_sketch.m_active = false;
        }
    }

private:
    /// @brief 按原手绘规则线性补齐途经格点，反向也保持有序函数横轴。
    /// @warning 事件帧固定数组更新，不生成函数拟合或预览采样缓存。
    /// @note index 总是落在 0..128，采样数量不随设备密度或帧率增长。
    /// @note 纵轴存实际参数，之后改变显示上下界不会重写已绘数值。
    static void sketchPoint(TimingFunctionEditorState& s, ImVec2 p, ImVec2 o,
                            ImVec2 size)
    {
        // 正式调用前已验证纵轴和段落范围，这里只做屏幕到固定格点的转换。
        // 触点越过边缘后仍保持同一笔，夹取确保数组索引永不溢出。
        const auto index = static_cast<int>(
            std::lround(std::clamp((p.x - o.x) / size.x, 0.f, 1.f) * 128));
        const double value = std::lerp(
            s.m_maximum,
            s.m_minimum,
            static_cast<double>(std::clamp((p.y - o.y) / size.y, 0.f, 1.f)));
        if ( s.m_previousIndex < 0 ) s.m_previousIndex = index;
        // 首样本直接落到真实起点；后续跨越多少格点就补齐多少，不跨笔连线。
        const auto low  = std::min(index, s.m_previousIndex),
                   high = std::max(index, s.m_previousIndex);
        for ( int i = low; i <= high; ++i ) {
            // 分母保留手势方向，反向扫过仍从前一个值连续插到当前值。
            // 同一格点再次移动只覆写该点，避免零分母。
            s.m_drawn[i] = true;
            s.m_values[i] =
                index == s.m_previousIndex
                    ? value
                    : std::lerp(s.m_previousValue,
                                value,
                                static_cast<double>(i - s.m_previousIndex) /
                                    (index - s.m_previousIndex));
        }
        s.m_previousIndex = index;
        // 下一帧从本次真实末点开始，不能从区间低端开始而反转手势方向。
        s.m_previousValue = value;
    }
    Contact m_bezier, m_sketch,
        m_preview;  ///< 各图分别冻结物理代际与起点归属。
    MobileCanvasNavigation
        m_navigation;  ///< 复用已有双指距离死区，不转交历史动作。
    std::array<double, 4>   m_original{};  ///< 本笔四个坐标，仅取消时恢复。
    std::array<double, 129> m_values{};  ///< 手绘取消备份，不包含共享函数缓存。
    std::array<bool, 129>   m_drawn{};   ///< 未画区间必须与本笔之前保持一致。
    ImVec2 m_start{}, m_last{};          ///< 贝塞尔起点和预览上一横坐标。
    int    m_control{ -1 };  ///< 一笔固定一枚控制点，未命中只占有空白图区。
    float  m_zoom{ 1 };      ///< 双指导航倍率基线，只属于本地视野。
    bool   m_sketchDirty{};  ///< 只有正常结束的手绘才使旧拟合失效。
    const char *m_symbolInput{},
        *m_symbolHelp{};  ///< 静态模板观察，不拥有输入焦点。
};
}  // namespace MMM::Canvas
