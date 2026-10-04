#include "mmm/timing/TimingInterpolation.h"

#include "mmm/timing/BpmNormalization.h"
#include "mmm/timing/Timing.h"
#include "mmm/timing/TimingFunction.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace MMM
{
/// @brief 不可变的相对秒域到拍域映射，由完整红线时间线低频构造。
/// @details 每个区间保存所属 BPM 段和累计拍位，不按导出密度近似 BPM。
/// BPM 插值继续使用秒域解析积分，因此节拍轴不存在循环依赖。
/// 所有查询只借用片段，实体删除或旧快照存活均不改变缓存内容。
class TimingBeatAxis
{
public:
    /// @brief 一个 BPM 控制区间，起点和终点均相对插值段首。
    struct Piece {
        /// @brief 区间起点秒数，用作二分搜索的有序键。
        double m_start{};
        /// @brief 区间终点秒数，不包含下一条红线的控制权。
        double m_end{};
        /// @brief 区间起点前已经积累的拍数。
        double m_beat{};
        /// @brief 常量红线值或 BPM 插值起始参数。
        double m_bpm{ 120 };
        /// @brief 区间起点在所属 BPM 函数中的相对秒数。
        double m_offset{};
        /// @brief 仅允许秒域 BPM，确保拍数积分严格递增。
        std::optional<TimingInterpolation> m_curve;

        /// @brief 计算本区间内经过若干秒产生的拍数。
        /// @warning 热路径只借用解析积分，不能展开采样或复制函数所有权。
        double beats(double seconds) const
        {
            // BPM 插值可能在本区间内部结束，公共积分会正确使用常量尾段。
            if ( m_curve )
                return (integrateTimingInterpolation(
                            *m_curve, m_bpm, m_offset + seconds) -
                        integrateTimingInterpolation(
                            *m_curve, m_bpm, m_offset)) /
                       60;
            return m_bpm * seconds / 60;
        }
    };
    /// @brief 全部 BPM 区间，按起点秒数和起点拍数严格排序。
    std::vector<Piece> m_pieces;
    /// @brief 秒域总长度，查询域外时夹到端点。
    double m_duration{};
    /// @brief 拍域总长度，来源于真实 BPM 积分。
    double m_beats{};

    /// @brief 通过有序 BPM 区间求段内拍位。
    /// @warning 逐帧查询路径，只二分固定缓存并执行一次有界积分。
    double beatAt(double seconds) const
    {
        if ( seconds <= 0 ) return 0;
        if ( seconds >= m_duration ) return m_beats;
        auto it = std::upper_bound(m_pieces.begin(),
                                   m_pieces.end(),
                                   seconds,
                                   [](double value, const Piece& piece) {
                                       return value < piece.m_start;
                                   });
        // 构建保证至少一个区间，从段首开始，因此 upper_bound 不会等于 begin。
        --it;
        return it->m_beat + it->beats(seconds - it->m_start);
    }

    /// @brief 反解分拍落点到秒数，常量 BPM 直接计算，曲线 BPM 二分积分。
    /// @warning 固定迭代上限是数值求根预算；不等待线程、网络或视野状态。
    double secondsAt(double beat) const
    {
        if ( beat <= 0 ) return 0;
        if ( beat >= m_beats ) return m_duration;
        auto it = std::upper_bound(m_pieces.begin(),
                                   m_pieces.end(),
                                   beat,
                                   [](double value, const Piece& piece) {
                                       return value < piece.m_beat;
                                   });
        --it;
        // 先定位目标拍数所属区间，只反解该局部区间。
        // 常量 BPM 直接换算，不需要数值求根。
        // 曲线尾部继承由积分处理，不额外创建伪红线。
        // 求根精度独立于分拍，修改分母不改变连续曲率。
        const double delta = beat - it->m_beat;
        if ( !it->m_curve ) return it->m_start + delta * 60 / it->m_bpm;
        // 合法红线严格为正，积分单调，不需要猜测或使用采样率作求根步长。
        double low = 0, high = it->m_end - it->m_start;
        for ( int i = 0; i < 48; ++i ) {
            const double middle = (low + high) / 2;
            if ( it->beats(middle) < delta )
                low = middle;
            else
                high = middle;
        }
        return it->m_start + (low + high) / 2;
    }
};

/// @brief 根据选定轴返回函数定义域，秒域与拍域不能混为同一个时长。
// 函数定义域与放置范围是两个量，秒轴使用秒长，拍轴使用真实拍长。
// 分拍仅控制采样周期，不能把 t 偷偷改成采样下标。
// 半拍采样时 t 仍以拍计量，两次采样之间的函数保持连续。
// 因此拟合图按拍均分，输出图按秒显示，二者通过同一映射关联。
double timingInterpolationVariableDuration(const TimingInterpolation& curve)
{
    return curve.m_variable == TimingVariable::Beat ? curve.m_beatDuration
                                                    : curve.m_duration;
}

/// @brief 将显示时间换算为函数自变量；未准备的拍域不生成伪造线性映射。
/// @warning 热路径只借用不可变缓存，构造和释放由描述生命周期管理。
// 秒轴保持原持续时间夹取，兼容旧文件的含义。
// 拍轴仅消费准备好的缓存，不用平均 Hz 伪造映射。
// 尚未绑定的 JSON 定义可校验，但不能用于播放求值。
// 载入、保存和脏重建负责准备，查询不读取可变会话。
double timingInterpolationVariableAtSeconds(const TimingInterpolation& curve,
                                            double                     seconds)
{
    if ( curve.m_variable == TimingVariable::Time )
        return std::clamp(seconds, 0.0, curve.m_duration);
    // 尚未绑定的定义仅可用于读取和验证，不能用于播放或导出求值。
    return curve.m_beatAxis ? curve.m_beatAxis->beatAt(seconds) : 0;
}

/// @brief 输出间隔采用真实拍位反解；不足完整分拍的段尾仍额外保留。
/// @warning 查询次数由预览预算或输出预算限制，不分配完整中间数组。
// 采样相位从本段首开始，不引用全谱绝对拍号。
// 有理分拍直接转为拍数，不提前舍入成整数毫秒。
// 最后短周期保留原終点，不吸附到下一个完整拍。
// 预览限制代表点数，导出上限由统一数量校验决定。
double timingInterpolationSampleElapsed(const TimingInterpolation& curve,
                                        std::size_t                index)
{
    const auto count = timingInterpolationSampleCount(curve);
    if ( count == 0 ) return 0;
    if ( index >= count - 1 ) return curve.m_duration;
    if ( curve.m_variable == TimingVariable::Time )
        return std::min(curve.m_duration,
                        static_cast<double>(index) / curve.m_samplesPerSecond);
    // 采样从当前段首开始数分拍，而非吸附到全谱绝对拍号。
    const double beat = static_cast<double>(index) * curve.m_beatNumerator /
                        curve.m_beatDenominator;
    return curve.m_beatAxis ? curve.m_beatAxis->secondsAt(beat) : 0;
}

/// @brief 准备拍轴和派生密度，只有整体成功才替换定义中的缓存。
/// @details 输入时间点使用毫秒，缓存统一秒；普通 BPM 与连续红线共用积分语义。
/// 所有权复制、排序和自定义函数重编译只能出现在这个低频入口。
/// 未包含红线时使用有效回退 BPM，不能使用独立 Scroll 的附带 BPM。
/// 曲线型 BPM 在段内结束后保留终值，下一条 BPM 到来时才切换控制权。
/// @warning 仅编辑、载入、保存和脏缓存重建调用；禁止每帧调用。
bool bindTimingInterpolationBeatAxis(TimingInterpolation&       curve,
                                     double                     startSeconds,
                                     const std::vector<Timing>& timings,
                                     double                     fallbackBpm)
{
    if ( curve.m_variable == TimingVariable::Time ) return true;
    // 有理分拍的输入与预算先检查，防止除零及不可信整数造成无限样本。
    if ( curve.m_variable != TimingVariable::Beat ||
         curve.m_beatNumerator <= 0 || curve.m_beatDenominator <= 0 ||
         curve.m_beatNumerator > 65536 || curve.m_beatDenominator > 65536 ||
         !std::isfinite(startSeconds) || !std::isfinite(curve.m_duration) ||
         curve.m_duration < .001 ||
         !std::isfinite(startSeconds + curve.m_duration) )
        return false;
    // 借用仅到本次构建结束，指针排序不改写谱面容器的原顺序。
    // 普通 Timing 的附带 BPM 不属于红线，不能影响分拍周期。
    // 无效红线在构建阶段拒绝，热路径无需再次检查整条时间线。
    // 每个效果段独立从段首数拍，不依赖全谱绝对拍号的余数。
    std::vector<const Timing*> redLines;
    // 借用源数据仅到本次构建结束，发布的缓存持有独立值副本。
    for ( const auto& timing : timings ) {
        if ( timing.m_timingEffect != TimingEffect::BPM ) continue;
        if ( !std::isfinite(timing.m_timestamp) ) return false;
        // 红线禁止拍域，即使某个网络文档绕过所属类型校验也不能产生递归。
        if ( timing.m_interpolation &&
             !isValidTimingInterpolation(*timing.m_interpolation,
                                         TimingEffect::BPM,
                                         timing.m_timingEffectParameter) )
            return false;
        redLines.push_back(&timing);
    }
    std::stable_sort(redLines.begin(),
                     redLines.end(),
                     [](const Timing* left, const Timing* right) {
                         return left->m_timestamp < right->m_timestamp;
                     });
    // 缓存一次性发布，旧快照及撤销记录继续持有旧对象。
    // 所有权只在低频描述复制时保留，查询不增加引用计数。
    // 生命周期与 ECS 身份解耦，删除实体不使旧快照悬空。
    // 片段只保留秒域 BPM 曲线，不能形成循环缓存链。
    auto axis        = std::make_shared<TimingBeatAxis>();
    axis->m_duration = curve.m_duration;
    // 首条红线之前沿用其常值，与编辑器的负拍位回退保持一致。
    double bpm = normalizeBpmValue(fallbackBpm);
    if ( !redLines.empty() ) {
        const auto& first = *redLines.front();
        bpm = normalizeBpmValue(first.m_timingEffectParameter > 0
                                    ? first.m_timingEffectParameter
                                    : first.m_bpm);
    }
    const Timing* active = nullptr;
    double        begin  = 0;
    // 闭合每个区间时保存真实积分起点；曲线不能在裁剪后的段首重新起算。
    const auto append = [&](double end) {
        if ( end <= begin ) return;
        TimingBeatAxis::Piece piece;
        piece.m_start = begin;
        piece.m_end   = end;
        piece.m_beat  = axis->m_beats;
        piece.m_bpm   = bpm;
        if ( active && active->m_interpolation ) {
            piece.m_curve  = active->m_interpolation;
            piece.m_offset = startSeconds + begin - active->m_timestamp / 1000;
        }
        // 积分起点可在红线函数中部，差分维持原始相位。
        // 片段长度和 BPM 为正，保证拍位映射严格递增。
        // 先累积拍数再保存完整片段，中间状态不发布。
        // 片段可跨越 BPM 曲线尾部，后续持续使用终值。
        axis->m_beats += piece.beats(end - begin);
        axis->m_pieces.push_back(std::move(piece));
        begin = end;
    };
    // 起点前的红线只更新控制状态，不创建负长度片段。
    // 段内红线先闭合旧区间，再取得后续控制权。
    // 同刻红线保持输入顺序，后者覆盖但不重复积分。
    // 精确段尾的新红线不影响段内积分和已闭合边界。
    for ( const auto* timing : redLines ) {
        const double offset = timing->m_timestamp / 1000 - startSeconds;
        if ( offset >= curve.m_duration ) break;
        // 同刻红线按照稳定输入顺序由后一条取得控制权，不创建零长度区间。
        if ( offset > 0 ) append(offset);
        active = timing;
        bpm    = normalizeBpmValue(timing->m_timingEffectParameter > 0
                                       ? timing->m_timingEffectParameter
                                       : timing->m_bpm);
    }
    append(curve.m_duration);
    if ( !std::isfinite(axis->m_beats) || axis->m_beats <= 0 ) return false;
    auto prepared = curve;
    // 平均 Hz 只用于展示，落点仍逐个目标拍位反解。
    // 分拍乘法进入浮点域，防止整数乘法溢出。
    // 拍长、密度和映射一起发布，不能保留半更新状态。
    // 分拍不重写源式，定义域变化才需要重新证明。
    prepared.m_beatDuration     = axis->m_beats;
    prepared.m_samplesPerSecond = axis->m_beats * curve.m_beatDenominator /
                                  curve.m_beatNumerator / curve.m_duration;
    prepared.m_beatAxis         = std::move(axis);
    // BPM 或范围变化会改变拍域长度；源表达式保留，但须重新证明定义域。
    // 仅定义域改变时才编译，调整分拍密度不会触发数学程序重建。
    if ( prepared.m_curve == TimingCurve::Custom && prepared.m_function &&
         timingFunctionDuration(*prepared.m_function) !=
             prepared.m_beatDuration ) {
        std::string error;
        double      start = 0;
        if ( !setTimingInterpolationFunction(
                 prepared,
                 timingFunctionExpression(*prepared.m_function),
                 start,
                 error) )
            return false;
    }
    // 计数统一处理整数拍位舍入，避免重复尾点。
    // 预算失败保留原定义，用户可缩短范围或调低分母。
    // 不生成全部样本来检查数量，避免数据放大内存。
    // 发布后不修改向量，逻辑和渲染可并行借用。
    if ( timingInterpolationSampleCount(prepared) == 0 ) return false;
    curve = std::move(prepared);
    return true;
}
}  // namespace MMM
