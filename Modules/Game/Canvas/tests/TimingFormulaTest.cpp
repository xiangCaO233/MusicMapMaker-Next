#include "canvas/TimingFormula.h"
#include "canvas/TimingFunctionEditorState.h"
#include "canvas/TimingInterpolationPreview.h"

#include "log/colorful-log.h"
#include "mmm/timing/Timing.h"
#include "mmm/timing/TimingFunction.h"
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
/// @brief 用截图中的高频公式覆盖长区间的峰谷、真实采样与缓存复用。
/// @return 原曲线没有混叠成低频、局部缩放恢复细节且真实绘制坐标有限时为真。
/// @details 周期与落点来自独立 cos 解析式，不能用被测折线生成预期答案。
/// 长段全景允许按像素压缩，但峰谷不能因 stride 抽取而消失。
/// 放大局部后检查周期数，防止只画一个填充色带掩盖真实函数。
/// 末尾不是完整采样周期，仍必须保留用户指定的精确终点。
/// 缓存原函数而非屏幕坐标，移动窗口不能触发表达式重新求值。
/// 本用例只修改内存工作副本，不载入活动谱面或创建音频设备。
bool testInterpolationPreview()
{
    // 截图使用 cos(1000*t)，1000 是角频率而非每秒周期数。
    // 周期约为 6.28 毫秒，一毫秒输出间隔足以表达该曲线。
    // 因此这里的低频外观应归因于显示抽取，不能降低用户输出密度。
    MMM::TimingInterpolation curve;
    curve.m_duration         = 2.22973;
    curve.m_samplesPerSecond = 1000;
    double      start        = 0;
    std::string error;
    if ( !MMM::setTimingInterpolationFunction(
             curve, "cos(1000*t)", start, error) )
        return false;
    MMM::Canvas::TimingInterpolationPreview preview;
    bool ok = preview.update(curve, start, 1100);
    // 2.22973 秒应有约 710 个过零，旧 128 段折线只能表达不超过 128 次。
    // 检查整个蓝线的有序真值，不以黄色标记数量替代波形精度。
    const auto crossings = [&]() {
        std::size_t count  = 0;
        const auto& points = preview.points();
        for ( std::size_t i = 1; i < points.size(); ++i )
            count += std::signbit(points[i - 1].m_value) !=
                     std::signbit(points[i].m_value);
        return count;
    };
    ok &= crossings() >= 709 && crossings() <= 710;
    // 非整周期时段尾仍单独写出；向下取整会丢失用户指定的终点。
    // 数量检查与位置检查分开，避免等分相同点数但错误网格蒙混通过。
    ok &= preview.visibleSampleCount() == 2231;
    // 所有黄色代表点都必须落在真实毫秒网格；最后短周期端点例外。
    // 解析值直接由原秒时间算出，不能用显示蓝线做另一份近似。
    // 坐标采用相对整段比例，缩放后也能还原同一绝对时间。
    // 毫秒整数检查留出浮点乘除误差，不能将误差误判为新采样点。
    // 最后端点不强制对齐毫秒网格，它代表用户输入的精确时长。
    for ( const auto& point : preview.samples() ) {
        const double time = point.m_progress * curve.m_duration;
        ok &= std::abs(point.m_value - std::cos(1000 * time)) < 1e-8;
        ok &= point.m_progress == 1.0 ||
              std::abs(time * 1000 - std::round(time * 1000)) < 1e-8;
    }
    ok &= !preview.samples().empty() &&
          preview.samples().back().m_progress == 1.0;
    // 相同键返回缓存，保留数组身份；稳定绘制帧不得重新遍历全部采样。
    const auto* cached = preview.points().data();
    // 只检查布尔返回不足以发现暗中重建，数组身份也必须保持不变。
    ok &= !preview.update(curve, start, 1100) &&
          preview.points().data() == cached;
    // 更长区间有六万个输出点，但几何仍受屏幕宽度预算限制。
    // 每个像素跨过多个周期，保留峰谷才能避免伪造一条低频曲线。
    // 六十秒保持输出数量压力，振荡频率受领域积分缓存的合法性预算约束。
    // 40 弧度每秒仍有约 382 个完整周期，足以暴露固定 128 段的混叠。
    // 本测试针对显示层，不提高数学编译器的节点或递归上限。
    curve.m_duration = 60;
    if ( !MMM::setTimingInterpolationFunction(
             curve, "cos(40*t)", start, error) )
        return false;
    ok &= preview.update(curve, start, 1100);
    ok &= preview.visibleSampleCount() == 60001 &&
          preview.points().size() <= 4400;
    bool hasPeak = false, hasTrough = false;
    // 点数预算不是正确性的充分条件：保留端点但丢掉极值同样会失真。
    // 正负峰分别检查，不能以绝对值最大值替代两侧振荡包围。
    for ( const auto& point : preview.points() ) {
        hasPeak |= point.m_value > .99;
        hasTrough |= point.m_value < -.99;
    }
    ok &= hasPeak && hasTrough;
    // 缩放围绕中点保留锚点，局部长度变为 .6 秒，约有八次过零。
    // 真实输出仍是每毫秒一个点，视野变化不能改变谱面采样密度。
    preview.zoomAt(.5, .01);
    // 同一函数缓存对象只改变视野，仍要重建；不能仅比较表达式身份。
    ok &= preview.update(curve, start, 1100);
    ok &= crossings() >= 7 && crossings() <= 9;
    ok &= preview.visibleSampleCount() >= 599 &&
          preview.visibleSampleCount() <= 601;
    // 拖动到边缘保持缩放倍率，恢复全景重新生成对应缓存。
    const double span = preview.viewEnd() - preview.viewStart();
    // 大位移故意超过边界，验证夹取不会把局部窗口扩大成全景。
    preview.pan(10000);
    ok &= std::abs(preview.viewEnd() - 1) < 1e-12 &&
          std::abs(preview.viewEnd() - preview.viewStart() - span) < 1e-12;
    preview.resetView();
    ok &= preview.update(curve, start, 1100) && preview.viewStart() == 0 &&
          preview.viewEnd() == 1;
    // 真正走绘制 helper，验证被压缩的高频图没有非法坐标或预算膨胀。
    // 不需要 GPU 也能检查生产 DrawList，解析精度由前面的断言独立保障。
    // 第一帧允许 ImGui 建立窗口状态，第二帧检查实际提交的可见几何。
    // 与公式排版测试共用上下文，但使用独立窗口名以隔离布局缓存。
    for ( int frame = 0; frame < 2; ++frame ) {
        ImGui::NewFrame();
        // 图表坐标固定在窗口内部，避免默认窗口位置导致全部几何被剪裁。
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 640, 560 });
        ImGui::Begin("Interpolation preview test",
                     nullptr,
                     ImGuiWindowFlags_NoSavedSettings);
        MMM::Canvas::drawTimingInterpolationPreview(
            preview,
            { 20, 50 },
            { 600, 150 },
            -1,
            1,
            IM_COL32(70, 195, 255, 255),
            IM_COL32(250, 205, 95, 255));
        ImGui::End();
        ImGui::Render();
    }
    const auto* data = ImGui::GetDrawData();
    // 最低顶点数排除空窗口假阳性，上限发现输出点未经压缩直接绘制。
    ok &= data && data->TotalVtxCount > 100 && data->TotalVtxCount < 100000;
    if ( data )
        for ( const auto* list : data->CmdLists )
            for ( const auto& vertex : list->VtxBuffer )
                ok &=
                    std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y);
    // 预览也承担拍域自变量图，映射不使用平均 Hz 偷换成秒域。
    curve.m_variable     = MMM::TimingVariable::Beat;
    curve.m_beatDuration = 3;
    // 此时六十秒只对应三拍，参考图的函数终值应为四而不是六十一。
    // 拍域参考图没有导出黄点，避免将拍坐标伪装成毫秒落点。
    if ( !MMM::setTimingInterpolationFunction(curve, "1+t", start, error) )
        return false;
    ok &= preview.update(curve, start, 1100, true) && preview.samples().empty();
    ok &= std::abs(preview.points().front().m_value - 1) < 1e-9 &&
          std::abs(preview.points().back().m_value - 4) < 1e-9;
    // 函数参数改变必须失效，不能只在窗口宽度变化时刷新图形。
    if ( !MMM::setTimingInterpolationFunction(curve, "2+t", start, error) )
        return false;
    // 宽度和定义域未变，仅函数值变化，能隔离缓存源键遗漏的问题。
    ok &= preview.update(curve, start, 1100, true) &&
          std::abs(preview.points().front().m_value - 2) < 1e-9;
    if ( !ok ) XERROR("TimingFormulaTest: 高频插值预览、采样或缓存回归失败");
    return ok;
}
/// @brief 从真实字形四边形检查墨迹重叠，防止包围盒只在数值上看起来合法。
/// @param list 已完成本帧绘制的单个窗口几何，不跨窗口比较。
/// @return 任意两枚可见字形没有超过像素容差的交叠时返回真。
/// @details 字形使用非零 UV 矩形，纯色网格、括号曲线及背景不会被算作文字。
/// 几何与 UV 都必须呈轴对齐矩形，不能误把抗锯齿曲线的顶点当成字形。
/// 判定针对字形墨迹，而非 advance 或源码字节数，包含实际回退数学字体。
/// 不比较不同窗口绘制列表，弹窗与底层窗口合法覆盖不属于排版错误。
/// 一像素多的容差覆盖贴近边缘的字体抗锯齿，不容许整枚括号压住数字。
/// 这是离线测试路径，允许收集顶点；正式公式渲染不增加碰撞检测开销。
bool glyphsDoNotOverlap(const ImDrawList& list)
{
    std::vector<ImVec4> glyphs;
    const auto&         vertices = list.VtxBuffer;
    for ( int i = 0; i + 3 < vertices.Size; ++i ) {
        const auto& a = vertices[i];
        const auto& b = vertices[i + 1];
        const auto& c = vertices[i + 2];
        const auto& d = vertices[i + 3];
        // AddText 的四个顶点从左上角顺时针排布，UV 同样形成独立矩形。
        // 纯色绘制的 UV 相同，会在此排除；不同基线的分子分母仍正常参与比较。
        if ( a.pos.y != b.pos.y || b.pos.x != c.pos.x || c.pos.y != d.pos.y ||
             d.pos.x != a.pos.x || a.uv.y != b.uv.y || b.uv.x != c.uv.x ||
             c.uv.y != d.uv.y || d.uv.x != a.uv.x || a.uv.x == b.uv.x ||
             a.uv.y == d.uv.y )
            continue;
        const ImVec4 current(a.pos.x, a.pos.y, c.pos.x, c.pos.y);
        for ( const auto& other : glyphs ) {
            const float overlapX =
                std::min(current.z, other.z) - std::max(current.x, other.x);
            const float overlapY =
                std::min(current.w, other.w) - std::max(current.y, other.y);
            // 上下标或分式可共享横向范围，只有二维墨迹同时重叠才失败。
            if ( overlapX > 1.5f && overlapY > 1.5f ) {
                XERROR("TimingFormulaTest: 字形重叠 {} × {} 像素",
                       overlapX,
                       overlapY);
                return false;
            }
        }
        glyphs.push_back(current);
        i += 3;
    }
    return true;
}
/// @brief 拟合应用必须更新真正用于下方预览和外部写出的段落副本。
/// @return 合法曲线即时生效，失效或越界候选不破坏现有副本时返回真。
/// @details 采用首尾相同、内部弯曲的解析样本，避免只更新端点也能通过。
/// 预览检查非采样时刻，外部写出检查实际采样时刻，两者共享数学定义。
/// 测试从恒定直线开始，模拟用户截图中成功拟合但下方仍为直线的回归。
/// 不触发逻辑命令，因此应用与最终谱面保存继续保持独立。
bool testFitApplication()
{
    MMM::Canvas::TimingFunctionEditorState state;
    MMM::TimingInterpolation               curve;
    curve.m_duration                             = 2;
    curve.m_samplesPerSecond                     = 2;
    double                                 start = 1;
    std::array<MMM::TimingCurvePoint, 129> points{};
    // 独立解析式生成实际秒域样本，不能用被测插值器反向构造期望值。
    for ( std::size_t i = 0; i < points.size(); ++i ) {
        const double t = 2 * i / 128.0;
        points[i]      = { t, 1 + (t - 1) * (t - 1) };
    }
    auto fit = MMM::fitTimingFunction(points, 2);
    if ( !fit ) return false;
    auto function = MMM::compileTimingFunction(fit->m_expression, 2);
    if ( !function ) return false;
    // 拟合和编译两步都必须真实成功，不能用直接构造的缓存绕过求解器。
    // 编辑器保留误差及源码用于展示，同时将同一个不可变缓存交给段落。
    state.m_fit         = std::move(*fit);
    state.m_fitFunction = std::move(*function);
    // 成功应用还必须清理旧输入错误，否则预览更新后保存仍会被错误禁用。
    state.m_error = "旧公式错误";
    if ( !state.applyFit(curve, MMM::TimingEffect::SCROLL, start) )
        return false;
    // 两端、源文本和缓存标记必须一起更新，下一帧不会重新解释旧直线。
    bool ok = curve.m_curve == MMM::TimingCurve::Custom &&
              std::abs(start - 2) < 1e-6 &&
              std::abs(curve.m_endValue - 2) < 1e-6 && state.m_error.empty() &&
              state.m_compiledDuration == 2 &&
              std::string_view(state.m_expression.data()) ==
                  state.m_fit->m_expression;
    // 预览取非采样时间，既要弯曲也要保持准确秒域含义。
    ok &= std::abs(MMM::evaluateTimingInterpolation(curve, start, .375) -
                   1.0625) < 1e-6;
    // Scroll 参数是绝对数值，不可当成起终值之间的归一化变化比例。
    // 时长仍由段落保存为秒，采样器负责转换成事件时间戳的毫秒。
    MMM::Timing timing;
    timing.m_timingEffect          = MMM::TimingEffect::SCROLL;
    timing.m_timingEffectParameter = start;
    timing.m_interpolation         = curve;
    // 模拟外部写出入口读取编辑副本，UI 候选蓝线的存在并不算验证成功。
    // 五个事件由已有的二 Hz 密度决定，拟合本身不得重置用户输出选项。
    const auto samples = MMM::sampleTimingInterpolations({ timing });
    // 二 Hz、两秒必须写出五个事件，中点为一，端点为二。
    // 只替换手绘蓝线、但保留底层直线模型的实现会在这里失败。
    ok &= samples.size() == 5;
    if ( samples.size() == 5 )
        ok &= std::abs(samples[2].m_timingEffectParameter - 1) < 1e-6 &&
              std::abs(samples.back().m_timingEffectParameter - 2) < 1e-6;
    // 失败测试用成功应用后的副本作基线，确保拒绝不会退回最初的直线。
    // 文本逐字比较，除了数值正确，还要保证下一帧输入框不会出现旧源码。
    const auto previous = curve;
    const auto text     = state.m_expression;
    // 改成不匹配的秒域后拒绝旧拟合，不部分更新起终值或文本。
    curve.m_duration = 3;
    ok &= !state.applyFit(curve, MMM::TimingEffect::SCROLL, start) &&
          curve.m_function == previous.m_function &&
          start == timing.m_timingEffectParameter && state.m_expression == text;
    // 恢复时长后再检查效果约束，避免前一项失效时长掩盖 BPM 领域问题。
    curve = previous;
    // 起终值为正仍不能证明 BPM 合法，中间的负过冲必须整体拒绝。
    auto invalid = MMM::compileTimingFunction("120-200*sin(pi*t/2)", 2);
    if ( !invalid ) return false;
    // 该函数在数学上有定义，但对 BPM 非法，拒绝应来自当前效果校验。
    // Scroll 与 BPM 可用范围不同，不能只依赖数学编译器的成功状态。
    state.m_fit->m_expression = "120-200*sin(pi*t/2)";
    state.m_fitFunction       = std::move(*invalid);
    ok &= !state.applyFit(curve, MMM::TimingEffect::BPM, start) &&
          curve == previous && state.m_expression == text;
    // 比较完整段落描述，保证拒绝时没有残留错误函数、端值或采样密度。
    // 用例不发布会话命令，验证的应用范围始终只到窗口内的工作副本。
    if ( !ok ) XERROR("TimingFormulaTest: 拟合应用或输出预览回归失败");
    return ok;
}
/// @brief 手绘拟合在拍域应用，秒数不同不能使合法拍域候选失效。
/// @details 100 BPM 的 1.2 秒等于两拍；候选定义域应为两拍而非 1.2。
/// 图形预览依实际秒数换算拍位，输出间隔仍由半拍决定。
/// @return 应用候选后公式、定义域与实际采样语义全部一致时为真。
bool testBeatFitApplication()
{
    // 秒域和拍域特意不相等，能发现 applyFit 仍比较持续秒数的旧实现。
    // 拟合候选来自真实求解器，不能手工塞入一个预期函数跳过拟合路径。
    // 129 点覆盖两拍，二次函数中点与端点值由独立解析样本生成。
    // 候选函数的完整域合法性由编译器证明，UI 只应用有效候选。
    // 半拍落点为 0.3 秒，而绝对参数应取 f(0.5)=1.25。
    // 输出密度由分拍决定，拟合应用不得重置分子、分母或派生密度。
    // 测试只修改窗口副本，不发布动作或写入实际用户谱面。
    // 从画布保存到最终模型的事务由逻辑测试另行覆盖。
    MMM::Timing bpm;
    bpm.m_bpm = bpm.m_timingEffectParameter = 100;
    MMM::TimingInterpolation curve;
    curve.m_variable = MMM::TimingVariable::Beat;
    curve.m_duration = 1.2;
    if ( !MMM::bindTimingInterpolationBeatAxis(curve, 0, { bpm }) )
        return false;
    std::array<MMM::TimingCurvePoint, 129> points{};
    // 使用独立拍域样本，不能从被测时间换算器反向生成拟合参考。
    for ( std::size_t i = 0; i < points.size(); ++i ) {
        const double beat = 2 * i / 128.0;
        points[i]         = { beat, 1 + beat * beat };
    }
    auto fitted = MMM::fitTimingFunction(points, 2);
    if ( !fitted ) return false;
    auto compiled = MMM::compileTimingFunction(fitted->m_expression, 2);
    if ( !compiled ) return false;
    MMM::Canvas::TimingFunctionEditorState state;
    state.m_fit         = std::move(*fitted);
    state.m_fitFunction = std::move(*compiled);
    double start        = 0;
    // 此处同时检查实际 UI 状态 helper，避免模型支持拍轴但拟合仍校验秒域。
    const bool ok =
        state.applyFit(curve, MMM::TimingEffect::SCROLL, start) &&
        state.m_compiledDuration == 2 &&
        std::abs(MMM::evaluateTimingInterpolation(curve, start, .25) - 1.25) <
            1e-6 &&
        std::abs(MMM::timingInterpolationSampleElapsed(curve, 1) - .3) < 1e-7;
    if ( !ok ) XERROR("TimingFormulaTest: 拍域拟合应用失败");
    return ok;
}

/// @brief 验证公式真实提交几何，嵌套分式与聚合上下限不会产生非法顶点。
/// @param source 使用正式符号或函数式输入的公式。
/// @param enlargedStyle 模拟较大的内边距及滚动条，复现高 DPI 下的纵向溢出。
/// @return 完整两帧均有有效绘制数据时返回真。
/// @details 无窗口或 GPU 后端，字体图集只在内存构建。
/// 帧间复用同一已编译数学树，禁止把每帧重解析当成渲染步骤。
/// 第一帧建立子窗口尺寸，第二帧验证稳定后的内容布局。
/// 每个表达式先编译成功，再进入真实排版器。
/// 公式树包括绑定变量，不以大符号字符串替代主体结构。
/// 测试窗口独立于应用布局，保证无显示服务器也能运行。
/// 屏幕空间边界与实际函数定义域无关。
/// 测试不截取固定顶点数量，允许字号或矢量细分算法调整。
/// 有限坐标是绘制安全约束，数值精度另由 MMM 测试负责。
bool render(const char* source, bool enlargedStyle = false)
{
    const auto function = MMM::compileTimingFunction(source, 2);
    if ( !function ) {
        XERROR("TimingFormulaTest {}: {}", source, function.error());
        return false;
    }
    bool ok = true;
    for ( int frame = 0; frame < 2; ++frame ) {
        // 固定位置和尺寸排除桌面环境、窗口布局及显示器缩放的干扰。
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(640, 560));
        if ( enlargedStyle ) {
            // 放大样式而非隐藏滚动条，验证内容空间确实足够容纳完整公式。
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 20));
            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 28.f);
        }
        ImGui::Begin("Math test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        MMM::Canvas::renderTimingFormula(**function);
        ImGui::End();
        if ( enlargedStyle ) ImGui::PopStyleVar(2);
        ImGui::Render();
        // 读取真实子窗口的滚动范围，不能仅凭没有绘出滚动条判定问题消失。
        // 内部窗口观察只在测试使用，正式渲染继续采用公开 BeginChild 接口。
        // 范围必须为零，防止样式隐藏滚动条后鼠标滚轮仍可挪动公式。
        const auto* window = ImGui::FindWindowByName("Math test");
        if ( !window || window->DC.ChildWindows.Size != 1 ) return false;
        const auto* formula = window->DC.ChildWindows[0];
        if ( formula->ScrollbarY || formula->ScrollMax.y > .01f ||
             formula->Scroll.y != 0 ) {
            XERROR("TimingFormulaTest: 公式仍可纵向滚动，范围 {}",
                   formula->ScrollMax.y);
            return false;
        }
        // 高 DPI 的长公式仍需允许横向阅读，不能通过禁止所有滚动掩盖溢出。
        if ( enlargedStyle &&
             (!formula->ScrollbarX || formula->ScrollMax.x <= 0) )
            return false;
        const auto* data = ImGui::GetDrawData();
        // 顶点存在不能证明数学答案正确，因此模型测试另用解析答案核对。
        // 此处只验证实际渲染路径完成，且上下限的坐标没有 NaN 或无穷。
        if ( !data || data->TotalVtxCount < 20 ) return false;
        for ( const auto* list : data->CmdLists ) {
            // 有限坐标不能证明字形可读，同时验证真实排版墨迹没有相互覆盖。
            ok &= glyphsDoNotOverlap(*list);
            for ( const auto& vertex : list->VtxBuffer )
                ok &=
                    std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y);
        }
    }
    return ok;
}
}  // namespace
/// @brief 覆盖三种聚合、根式、对数底数和嵌套分式的无后端排版。
/// @details 不写 imgui.ini，不读取用户配置，失败亦销毁上下文。
/// ImGui 字体和上下文均通过官方创建与销毁入口管理。
/// 上下文只用于本用例，不能与其他测试共享全局状态。
/// 图集先构建，使渲染器字体测量有明确初始化前置条件。
/// 没有纹理上传，也不初始化 Vulkan、GLFW 或音频。
/// 嵌套积分与求和覆盖公式第三子树的递归绘制。
/// 双层分式覆盖上下高度超过普通单行的情况。
/// 字体像素只由 ImGui 释放，测试不保存其观察指针。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640, 560);
    io.DeltaTime   = 1.f / 60;
    // 与正式资源字体栈相同，后备 face 只补数学字形，不能盖过基础字母。
    // 基础与合并 face 都使用显式参考字号，避免动态字体合并前置条件冲突。
    ImFontConfig base;
    base.SizePixels = 13;
    auto* font      = io.Fonts->AddFontDefault(&base);
    // 合并 face 保持单采样，与正式资源加载路径一致。
    ImFontConfig merge;
    merge.MergeMode   = true;
    merge.OversampleH = merge.OversampleV = 1;
    const bool mathLoaded =
        io.Fonts->AddFontFromFileTTF(argv[1], 13, &merge) != nullptr;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    // 图集像素由 ImGui 拥有，测试不转移所有权或创建显卡纹理。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    bool ok = mathLoaded && pixels && width > 0 && height > 0;
    ok &= testFitApplication();
    ok &= testBeatFitApplication();
    ok &= testInterpolationPreview();
    // 不能用替代问号冒充字形存在，直接检查真实合并 face 的字符映射。
    for ( const ImWchar codepoint :
          { 0x03c0, 0x221b, 0x222b, 0x03a3, 0x03a0, 0x221a, 0x2212 } )
        ok &= font->IsGlyphInFont(codepoint);
    // 单独负数直接覆盖负号与括号挤压数字的情形。
    // Horner 复合式模拟真实拟合输出，乘号紧邻负系数仍须保留各自占位。
    // 分式和幂让括号包围高度变化，不能只验证普通单行表达式。
    // 双参数函数另检查逗号及两个负参数之间的间距。
    for ( const char* source :
          { "sum(k,1,10,k²)",
            "prod(k,1,5,k)",
            "int(x,0,t,x²)",
            "int(x,0,t,sum(k,1,3,x^k))",
            "π/(1+t^2)+sqrt(t)+log10(1+t)+log2(1+t)",
            "(1/(1+t))/(1/(2+t))",
            "-0.799",
            "0.337899+t/1.730769*(-0.799+t/1.730769*(0.125+t/1.730769))",
            "(-0.799)/(1+t)",
            "max(-0.799,-(1+t))",
            "(-0.799)^2" } )
        ok &= render(source);
    // 原有固定高度在这些样式下被横向滚动条挤出纵向溢出。
    // 同时覆盖深层分式和拟合 Horner 式，高度应取实际数学结构。
    ok &= render("0.337899+t/1.730769*(-0.799+t/1.730769*(0.125+t/1.730769))",
                 true);
    ImGui::DestroyContext();
    return ok ? 0 : 1;
}
