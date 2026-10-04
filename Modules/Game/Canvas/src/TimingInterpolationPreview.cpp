#include "canvas/TimingInterpolationPreview.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace MMM::Canvas
{
namespace
{
/// @brief 按像素列压缩有序顶点，每列保留实际首尾与参数极值。
/// @param input 原函数和真实输出样本，均按横轴递增。
/// @param columns 显示列数；样本标记采用较宽列，避免圆点挤成无意义斑块。
/// @param first 当前可见区间起点，输入中不包含它左侧的点。
/// @param last 当前可见区间终点，必须严格大于 first。
/// @return 按原时间顺序排列的代表点，不把峰谷重排成锯齿。
/// @pre input 按横轴递增，参数值有限，columns 至少为一。
/// @details 连续列之间保留边界点，缓慢曲线压缩后仍保持走势。
/// 同一列可能包含多个周期，此时极值表示像素内的参数包围。
/// 输出容量只与屏幕列数有关，与段落持续时间无关。
/// 空输入产生空输出，参考图因此可与没有采样点的模式共用接口。
/// 函数不持有模型或表达式所有权，返回值只包含轻量数值。
/// 压缩不添加新的时间坐标，不改变可追溯到导出索引的黄色点。
/// @warning 仅缓存变化时调用，扫描有界预览数组，不从绘制帧重复执行。
std::vector<TimingPreviewPoint> reducePoints(
    const std::vector<TimingPreviewPoint>& input, int columns, double first,
    double last)
{
    std::vector<TimingPreviewPoint> result;
    result.reserve(
        std::min(input.size(), static_cast<std::size_t>(columns) * 4));
    // 相同像素内保留极值，而不是每隔固定数量取一点造成周期混叠。
    // 每组仍使用原点的时间和值，不把桶中心冒充真实采样时间。
    const auto column = [&](double progress) {
        return std::min(
            columns - 1,
            static_cast<int>((progress - first) / (last - first) * columns));
    };
    for ( std::size_t begin = 0; begin < input.size(); ) {
        // 有序横轴保证每列连续，只需向前扫描一次，不按列重复搜索全数组。
        // 最大值和最小值可与首尾重合，索引去重统一处理这些退化情形。
        const int   current = column(input[begin].m_progress);
        std::size_t end = begin + 1, low = begin, high = begin;
        while ( end < input.size() &&
                column(input[end].m_progress) == current ) {
            if ( input[end].m_value < input[low].m_value ) low = end;
            if ( input[end].m_value > input[high].m_value ) high = end;
            ++end;
        }
        // 四个索引的小排序只在重建时进行，按时间连接才不会制造交叉折线。
        // 去重使单点列与恒定列不会重复提交同一个标记。
        std::array indices{ begin, low, high, end - 1 };
        std::sort(indices.begin(), indices.end());
        auto previous = input.size();
        for ( const auto index : indices ) {
            if ( index != previous ) result.push_back(input[index]);
            previous = index;
        }
        begin = end;
    }
    return result;
}
}  // namespace

/// @brief 构建像素分辨率连续曲线，并合入每一个可见的真实输出样本。
/// @details 连续图至少四倍像素采样；输出点最多为模型允许的 65537 个。
/// 非整周期尾点仍通过领域采样器取得，拍域反解也不使用平均 Hz 代替。
/// 视图缓存包括拍轴身份，BPM 映射改变但表达式未变时也必须更新。
/// @param curve UI 工作副本，调用方已完成效果合法性检查。
/// @param startValue 预设曲线起值，自定义函数使用自己的绝对参数。
/// @param width 图表内部的实际像素宽度，不含边框留白。
/// @param variableDomain 手绘参考图使用自变量域，输出图使用实际秒域。
/// @return 参数变化并已重建时为真，相同键直接复用时为假。
/// @note 缓存不捕获会话或实体，谱面关闭后不存在悬空注册表引用。
/// @note 输出与参考图分别拥有缓存，局部缩放不会改动手绘格点。
/// @note 宽度先限制再转整数，异常窗口尺寸不能溢出显示预算。
/// @warning 稳定帧在缓存键比较后返回；全部求值与分配只发生于输入变化。
bool TimingInterpolationPreview::update(const TimingInterpolation& curve,
                                        double startValue, float width,
                                        bool variableDomain)
{
    // 图形宽度仅决定显示预算，不反向修改导出的真实采样密度。
    const int columns = static_cast<int>(std::clamp(
        std::isfinite(width) ? std::ceil(width) : 1.0f, 1.0f, 4096.0f));
    if ( m_source && *m_source == curve &&
         m_source->m_function.get() == curve.m_function.get() &&
         m_source->m_beatAxis.get() == curve.m_beatAxis.get() &&
         m_startValue == startValue && m_columns == columns &&
         m_variableDomain == variableDomain && m_cachedStart == m_viewStart &&
         m_cachedEnd == m_viewEnd )
        return false;
    // 语义相同但缓存对象不同也需重建，拍轴准备可能改变真实秒时间映射。
    // 所有权只在这个失效分支捕获；稳定帧不复制 shared_ptr。
    m_source         = curve;
    m_startValue     = startValue;
    m_columns        = columns;
    m_variableDomain = variableDomain;
    m_cachedStart    = m_viewStart;
    m_cachedEnd      = m_viewEnd;
    m_points.clear();
    m_samples.clear();
    m_visibleSampleCount = 0;
    // 无效编辑中间值只清空预览，不能将 NaN 投影到绘制列表。
    if ( !std::isfinite(curve.m_duration) || curve.m_duration <= 0.0 ||
         !std::isfinite(startValue) )
        return true;
    const auto valueAt = [&](double progress) {
        // 统一求值入口使连续图和真实输出点共享数学定义及拍域转换。
        return variableDomain
                   ? evaluateTimingInterpolationVariable(
                         curve,
                         startValue,
                         timingInterpolationVariableDuration(curve) * progress)
                   : evaluateTimingInterpolation(curve, startValue, progress);
    };
    std::vector<TimingPreviewPoint> output;
    if ( !variableDomain ) {
        const auto count = timingInterpolationSampleCount(curve);
        // 拍域时间由非线性反解取得，不能根据展示平均 Hz 估计索引。
        // 样本时间单调，因此只用对数次查询定位当前视野的边界。
        // 缩放和平移的连续输入不应为局部预览重复反解全段六万点。
        const auto bound = [&](double limit, bool upper) {
            std::size_t low = 0, high = count;
            while ( low < high ) {
                const auto   middle = low + (high - low) / 2;
                const double progress =
                    timingInterpolationSampleElapsed(curve, middle) /
                    curve.m_duration;
                if ( progress < limit || (upper && progress == limit) )
                    low = middle + 1;
                else
                    high = middle;
            }
            return low;
        };
        // 左界包含等值点，右界取严格更大的点，精确端点不会漏显示。
        // 相邻视野共享边界样本是正常的，缓存不写回或重复生成模型事件。
        const auto first = bound(m_viewStart, false);
        const auto last  = bound(m_viewEnd, true);
        // 局部内存容量也随可见数量收缩，而不是继续按完整段落申请。
        // 空视野允许零个输出点，蓝线仍可独立表达两样本之间的函数。
        output.reserve(last - first);
        // 遍历当前可见样本集合，不在每帧重复扫描或更改真实采样位置。
        // 黄色代表点随后按列保留峰谷，绝不按 stride 随意跳过波峰。
        for ( std::size_t index = first; index < last; ++index ) {
            // 末点由领域 helper 返回精确时长，非整周期不能均分或截掉。
            const double progress =
                timingInterpolationSampleElapsed(curve, index) /
                curve.m_duration;
            output.push_back({ progress, valueAt(progress) });
        }
    }
    m_visibleSampleCount = output.size();
    // 蓝线还必须经过真实采样点，避免独立均分折线与黄色落点互相矛盾。
    // 两个有序序列线性合并，不对数万采样点额外全排序。
    std::vector<TimingPreviewPoint> combined;
    const int                       intervals = std::max(512, columns * 4);
    // 四倍像素网格用于保留列内弯曲；真实输出点独立加入以避免互相错位。
    // 放大后只采样局部区间，同一预算可以分辨更高的真实时间频率。
    combined.reserve(output.size() + static_cast<std::size_t>(intervals) + 1);
    std::size_t sample = 0;
    for ( int index = 0; index <= intervals; ++index ) {
        const double progress = std::lerp(
            m_viewStart, m_viewEnd, static_cast<double>(index) / intervals);
        while ( sample < output.size() && output[sample].m_progress < progress )
            combined.push_back(output[sample++]);
        combined.push_back({ progress, valueAt(progress) });
    }
    while ( sample < output.size() ) combined.push_back(output[sample++]);
    m_points = reducePoints(combined, columns, m_viewStart, m_viewEnd);
    // 黄色圆点需要更宽间距，但每个显示桶仍保留真实峰谷与两端样本。
    m_samples =
        reducePoints(output, std::max(1, columns / 5), m_viewStart, m_viewEnd);
    return true;
}

/// @brief 缩放围绕光标锚点，范围始终留在完整段落内。
/// @note 最小视野只约束显示，完全不改变模型采样间隔。
/// @param position 光标在当前图表内的比例，图表外位置夹取到端点。
/// @param factor 小于一放大，大于一缩小，非法输入不改变视图。
/// @details 锚点使用缩放前视野换算，连续滚轮输入不会漂移目标时间。
/// @warning 输入事件入口，只更新固定数量的标量，无锁或资源访问。
void TimingInterpolationPreview::zoomAt(double position, double factor)
{
    if ( !std::isfinite(position) || !std::isfinite(factor) || factor <= 0 )
        return;
    position             = std::clamp(position, 0.0, 1.0);
    const double oldSpan = m_viewEnd - m_viewStart;
    const double span    = std::clamp(oldSpan * factor, 1e-8, 1.0);
    const double anchor  = m_viewStart + oldSpan * position;
    // 靠近边缘时优先保持视野在段内，不能通过缩放看到段落外的虚构采样。
    m_viewStart = std::clamp(anchor - span * position, 0.0, 1.0 - span);
    m_viewEnd   = m_viewStart + span;
}

/// @brief 以当前视野跨度平移，边缘夹取不改变缩放倍率。
/// @param delta 视野宽度的有符号倍数，拖动向右时调用方提供负值。
/// @note 极大拖动仍只移动到合法端点，不扩大当前可见跨度。
/// @warning 输入事件入口，不生成数学采样或编辑命令。
void TimingInterpolationPreview::pan(double delta)
{
    if ( !std::isfinite(delta) ) return;
    const double span = m_viewEnd - m_viewStart;
    m_viewStart       = std::clamp(m_viewStart + delta * span, 0.0, 1.0 - span);
    m_viewEnd         = m_viewStart + span;
}

/// @brief 恢复完整视图，缓存键使下一帧按全段重新准备。
/// @note 缓存失效由范围键检测，不提前释放数据造成绘制时悬空。
/// @warning 双击输入入口，仅更新两个标量，不持有会话锁。
void TimingInterpolationPreview::resetView()
{
    m_viewStart = 0.0;
    m_viewEnd   = 1.0;
}

/// @brief 将缓存的绝对参数投影为折线与真实样本代表点。
/// @param preview 当前视野已经准备的有序代表点集合。
/// @param origin 图表内部左上角，不含周围提示文字的位置。
/// @param size 图表有效像素尺寸，两种编辑图使用同一投影规则。
/// @param minimum 纵轴下界，与 maximum 相同表示恒定函数。
/// @param maximum 纵轴上界，范围由领域模型或用户绘制轴决定。
/// @param lineColor 原函数折线颜色，投影不修改函数数值。
/// @param sampleColor 非零时绘制真实采样代表点，零时只显示参考线。
/// @note 恒定函数显示在中线，不能除以零或被误画成图表底部。
/// @note 像素压缩已经按时间排序，绘制层无需重新整理峰谷顺序。
/// @warning 每帧绘图只访问有界代表点，不求值或重建函数缓存。
void drawTimingInterpolationPreview(const TimingInterpolationPreview& preview,
                                    const ImVec2& origin, const ImVec2& size,
                                    double minimum, double maximum,
                                    unsigned int lineColor,
                                    unsigned int sampleColor)
{
    if ( !std::isfinite(minimum) || !std::isfinite(maximum) ||
         !std::isfinite(maximum - minimum) )
        return;
    const double span    = maximum - minimum;
    const auto   project = [&](const TimingPreviewPoint& point) {
        return ImVec2(
            origin.x +
                static_cast<float>((point.m_progress - preview.viewStart()) /
                                   (preview.viewEnd() - preview.viewStart())) *
                    size.x,
            origin.y +
                size.y *
                    (1.0f - static_cast<float>(
                                span > 1e-12 ? (point.m_value - minimum) / span
                                             : 0.5)));
    };
    auto* draw = ImGui::GetWindowDrawList();
    // 参考图可能故意设窄纵轴，剪裁防止高频峰值覆盖旁边控件。
    draw->PushClipRect(
        origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    const auto& points = preview.points();
    for ( std::size_t index = 1; index < points.size(); ++index )
        draw->AddLine(project(points[index - 1]),
                      project(points[index]),
                      lineColor,
                      1.3f);
    if ( sampleColor )
        // 标记和值来自缓存真值，不在渲染帧重新调用可能昂贵的表达式。
        for ( const auto& point : preview.samples() )
            draw->AddCircleFilled(project(point), 1.8f, sampleColor);
    draw->PopClipRect();
}
}  // namespace MMM::Canvas
