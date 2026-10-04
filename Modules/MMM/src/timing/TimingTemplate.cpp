#include "mmm/timing/TimingTemplate.h"

#include "mmm/timing/BpmNormalization.h"
#include "mmm/timing/TimingFunction.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <utility>

namespace MMM
{
namespace
{
/// @brief 红线及其累计拍位仅服务一次模板转换，不长期借用会话注册表。
// 拍轴仅描述 BPM，不使用 Scroll 的位移积分代替节拍。
// 第一条红线的累计量设为零；相对模板消除这个任意原点。
// 记录是低频转换的局部值，排序不会重排来源谱面。
// 函数对象沿用只读共享缓存，离开源会话后仍能查询。
// 普通时间点的附带 BPM 不得进入这个列表。
struct TempoPoint {
    /// @brief 保存红线值副本，连续函数对象保持既有只读共享生命周期。
    Timing m_timing;
    /// @brief 从第一条红线起累计的拍数；相对坐标不依赖拍零点定义。
    double m_beat{ 0.0 };
};

/// @brief 从红线计算局部累计拍数，首点以前延用起值。
/// @note BPM 为正保证严格单调，是反解有唯一解的前提。
// 调用方须先规范化 BPM，再进入积分查询。
// elapsed 可以为负，允许基准以前的预置 SV。
// 连续段在段尾以后延用终值，与编辑器拍线保持一致。
// 返回拍数而非秒数，因此统一在这里除以六十。
// 该函数不会按导出采样密度切分曲线。
// 模板偏移精度不受输出 Timing 事件密度影响。
double localBeats(const Timing& line, double elapsed)
{
    // Timing 使用毫秒存储，而积分接口消费秒，换算只在调用处进行。
    // 域外常值外推允许模板包含锚点以前的分拍。
    return (line.m_interpolation
                ? integrateTimingInterpolation(*line.m_interpolation,
                                               line.m_timingEffectParameter,
                                               elapsed)
                : elapsed * line.m_timingEffectParameter) /
           60.0;
}

/// @brief 建立有序拍轴，拒绝不能单调反解的 BPM 段。
/// @note 重复红线稳定排序，查询同刻使用最后一个定义。
// timings 可以混合四种效果，构造器只采纳真实红线。
// fallback 用于没有红线的谱面和普通零 BPM 的兼容处理。
// 错误返回后没有可用拍轴，调用方不得继续部分转换。
// 同刻红线稳定保留输入顺序，查询取最后一项。
// 不能先按 BPM 数值排序，否则同刻覆盖语义会改变。
// 连续 BPM 需要整个函数定义，而不是只复制两端参数。
// 预先累计每段拍数，使单点查询只需二分定位红线。
// 构造成本属于用户编辑，不进入播放或悬停查询。
std::expected<std::vector<TempoPoint>, std::string> buildTempo(
    std::span<const Timing> timings, double fallback)
{
    std::vector<TempoPoint> result;
    for ( const auto& line : timings ) {
        if ( line.m_timingEffect != TimingEffect::BPM ) continue;
        if ( !std::isfinite(line.m_timestamp) )
            return std::unexpected("BPM 时间戳不是有限值。");
        // 来源红线不被修改，回退值只写入本次换算的副本。
        // 曲线校验覆盖函数整个定义域，不能只看起值为正。
        // 非法曲线会使全轴失败，避免一半点使用回退拍长。
        auto copy = line;
        copy.m_timingEffectParameter =
            normalizeBpmValue(line.m_timingEffectParameter, fallback);
        // 普通零 BPM 沿用引擎回退规则，曲线则必须满足严格正 BPM 定义。
        // 否则拍数可能倒退，反解不再具有唯一落点。
        if ( copy.m_interpolation &&
             !isValidTimingInterpolation(*copy.m_interpolation,
                                         TimingEffect::BPM,
                                         copy.m_timingEffectParameter) )
            return std::unexpected("BPM 段落无法建立合法拍轴。");
        result.push_back({ std::move(copy), 0.0 });
    }
    // 没有红线时用谱面首选 BPM，而不是依赖某个窗口的当前拍线。
    if ( result.empty() ) {
        Timing line;
        line.m_timingEffectParameter = normalizeBpmValue(fallback);
        result.push_back({ std::move(line), 0.0 });
    }
    // 时间戳已经证明有限，排序比较器不会遇到 NaN。
    // 首红线以前的负相对坐标在查询时线性外推。
    // 后续累计量依次继承上一段末尾，确保跨红线连续。
    // 同刻两红线的累计增量为零，保留后项优先。
    std::stable_sort(
        result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.m_timing.m_timestamp < b.m_timing.m_timestamp;
        });
    for ( std::size_t i = 1; i < result.size(); ++i ) {
        // 累计量跨越每个真实红线，不能用锚点 BPM 恒定换算整组。
        const auto& previous = result[i - 1];
        result[i].m_beat =
            previous.m_beat + localBeats(previous.m_timing,
                                         (result[i].m_timing.m_timestamp -
                                          previous.m_timing.m_timestamp) /
                                             1000.0);
        if ( !std::isfinite(result[i].m_beat) )
            return std::unexpected("累计拍数超出范围。");
    }
    return result;
}

/// @brief 在有序红线上查询连续拍位，允许首红线以前的负相对拍数。
// axis 必须由 buildTempo 成功产生，始终至少包含一条红线。
// seconds 是绝对秒时间，不是相对选区起点。
// upper_bound 在同刻红线后返回，减一得到最终覆盖项。
// 首点之前不能把时间夹到零，否则提前 SV 会失去距离。
// 拍原点任意，但 make 与 place 内的差值消去原点。
// 函数只借用局部轴，没有会话锁或文件访问。
double beatAt(const std::vector<TempoPoint>& axis, double seconds)
{
    auto it = std::upper_bound(
        axis.begin(), axis.end(), seconds, [](double t, const auto& point) {
            return t * 1000.0 < point.m_timing.m_timestamp;
        });
    // 首点前与尾点后均使用同一红线的外推契约，不夹紧负偏移。
    if ( it != axis.begin() ) --it;
    return it->m_beat + localBeats(it->m_timing,
                                   seconds - it->m_timing.m_timestamp / 1000.0);
}

/// @brief 反解连续拍位，曲线内部使用固定迭代上限的单调二分。
// beat 使用与 beatAt 相同的累计原点。
// 正 BPM 保证红线累计量单调，才允许二分定位。
// 首点前和普通红线使用线性公式，不放大迭代区间。
// 曲线段内部用积分值比较，避免依赖可能为零的曲率导数。
// 固定四十八次求解不会因长谱面形成无上限循环。
// 这个反解保留实际 BPM 曲线，而不是按事件样本插值。
// 模板按拍定位到另一个谱面时，只有坐标被换算，参数不变。
double secondsAt(const std::vector<TempoPoint>& axis, double beat)
{
    auto it = std::upper_bound(
        axis.begin(), axis.end(), beat, [](double b, const auto& point) {
            return b < point.m_beat;
        });
    if ( it != axis.begin() ) --it;
    const auto&  line  = it->m_timing;
    const double local = beat - it->m_beat, start = line.m_timestamp / 1000.0;
    if ( !line.m_interpolation || local <= 0 )
        return start + local * 60.0 / line.m_timingEffectParameter;
    const auto&  curve = *line.m_interpolation;
    const double whole = localBeats(line, curve.m_duration);
    // 曲线结束后保持终值，长距离外推无需扩大搜索区间。
    if ( local >= whole )
        return start + curve.m_duration +
               (local - whole) * 60.0 / curve.m_endValue;
    // 只在曲线内部二分，所以区间上界始终是已验证时长。
    // 段尾外推已提前处理，不因未来 BPM 很长而增加误差。
    // 查询到下一条红线时会选新段，不沿用旧段外推。
    double low = 0, high = curve.m_duration;
    for ( int i = 0; i < 48; ++i ) {
        const double middle = (low + high) * 0.5;
        if ( localBeats(line, middle) < local )
            low = middle;
        else
            high = middle;
    }
    return start + (low + high) * 0.5;
}

/// @brief 独立验证模板结构，文件读取和放置共用同一边界。
// 定义校验独立于放置位置，负偏移本身是合法值。
// 名称长度按 UTF-8 字节检查，与固定输入缓冲区一致。
// 模板至少一个点，删除最后一点后可编辑但不能保存或放置。
// 锚点索引是组内身份，不能借用源 ECS entity 作为持久化基准。
// 基准偏移必须为零，避免一组存在两个互相矛盾的原点。
// 未知坐标轴拒绝，不尝试猜测用户想按秒还是按拍。
// BPM 参与模板定位时只能用秒轴，避免自身改变拍轴。
// 普通效果参数允许负值，保持 Jump 与反向 Scroll 的语义。
// 插值段有额外终点，普通点不根据 endOffset 生成占用区间。
std::string definitionError(const TimingTemplate& value)
{
    if ( value.m_name.empty() || value.m_name.size() > 256 ||
         value.m_points.empty() || value.m_points.size() > 4096 ||
         value.m_anchor >= value.m_points.size() )
        return "名称须为 1–256 字节，模板须有 1–4096 点，并选择有效基准。";
    if ( value.m_variable != TimingVariable::Time &&
         value.m_variable != TimingVariable::Beat )
        return "未知的模板定位轴。";
    if ( std::abs(value.m_points[value.m_anchor].m_offset) > 1e-9 )
        return "基准点偏移必须为零。";
    for ( const auto& point : value.m_points ) {
        const auto& line = point.m_timing;
        // 显式检查枚举，未知文件类型不能静默成为 BPM。
        if ( line.m_timingEffect != TimingEffect::BPM &&
             line.m_timingEffect != TimingEffect::SCROLL &&
             line.m_timingEffect != TimingEffect::JUMP &&
             line.m_timingEffect != TimingEffect::HS )
            return "未知的时间点类型。";
        if ( !std::isfinite(point.m_offset) ||
             !std::isfinite(line.m_timingEffectParameter) ||
             (line.m_timingEffect == TimingEffect::BPM &&
              line.m_timingEffectParameter < 0) )
            return "偏移和参数须为有限值，BPM 不能为负。";
        // 禁止含红线的拍域模板是定义规则，文件和 UI 都不能绕过。
        // 其他三个类型可以混合使用同一拍轴和同一基准。
        // 不依据名称或当前播放 BPM 推断模板类型。
        if ( value.m_variable == TimingVariable::Beat &&
             line.m_timingEffect == TimingEffect::BPM )
            return "含 BPM 的模板只能依据时间戳。";
        if ( line.m_interpolation &&
             (!std::isfinite(point.m_endOffset) ||
              point.m_endOffset <= point.m_offset ||
              !isValidTimingInterpolation(*line.m_interpolation,
                                          line.m_timingEffect,
                                          line.m_timingEffectParameter)) )
            return "模板中存在非法插值段落。";
    }
    return {};
}
}  // namespace

/// @brief 选区到相对坐标的转换保留原值和段落，基准不是隐式首点。
// selected 的时间单位来自领域模型，必须是毫秒。
// anchor 指定来源选区的索引，通常可选恢复普通 SV 的点。
// variable 决定保存秒距离还是连续拍距离。
// tempo 是来源谱面的完整红线，不限于当前可见区间。
// 函数返回独立值副本，不修改来源时间点或选择集合。
// 段尾用同一坐标轴转换，跨 BPM 时不假定恒定拍长。
// 未排序 selected，保持调用者表格中的基准身份。
// 捕获错误时没有可保存的半个模板。
std::expected<TimingTemplate, std::string> makeTimingTemplate(
    std::span<const Timing> selected, std::size_t anchor,
    TimingVariable variable, std::span<const Timing> tempo, double fallbackBpm)
{
    if ( selected.empty() || selected.size() > 4096 ||
         anchor >= selected.size() )
        return std::unexpected("请先框选 1–4096 个时间点并选择基准。");
    auto axis = buildTempo(tempo, fallbackBpm);
    if ( !axis ) return std::unexpected(axis.error());
    TimingTemplate result;
    result.m_variable     = variable;
    result.m_anchor       = anchor;
    const auto coordinate = [&](double seconds) {
        return variable == TimingVariable::Beat ? beatAt(*axis, seconds)
                                                : seconds;
    };
    // 先取得基准坐标，所有条目减同一个原点。
    // 这样位于基准以前的点自然产生有符号负偏移。
    // 源绝对时间戳仍在副本里，但放置只消费相对偏移。
    const double base = coordinate(selected[anchor].m_timestamp / 1000.0);
    for ( const auto& timing : selected ) {
        const double        seconds = timing.m_timestamp / 1000.0;
        TimingTemplatePoint point{ coordinate(seconds) - base, timing, 0.0 };
        if ( timing.m_interpolation )
            point.m_endOffset =
                coordinate(seconds + timing.m_interpolation->m_duration) - base;
        result.m_points.push_back(std::move(point));
    }
    if ( const auto error = definitionError(result); !error.empty() )
        return std::unexpected(error);
    return result;
}

/// @brief 基准切换只平移坐标，不能改变条目间距离或段落长度。
// anchor 越界时不改动任何字段，便于 UI 保持旧工作副本。
// 切换基准是平移而非缩放，正负偏移可能同时出现。
// 段首和段尾一起平移，插值长度保持不变。
// 条目顺序不变，表格行和库中锚点索引仍能对应。
// 用户目标基准时间由 UI 独立决定，不在领域转换里猜测。
// 本操作不检查目标范围，保存定义不依赖当前谱面的占用状态。
bool reanchorTimingTemplate(TimingTemplate& value, std::size_t anchor)
{
    if ( anchor >= value.m_points.size() ) return false;
    const double shift = value.m_points[anchor].m_offset;
    for ( auto& point : value.m_points ) {
        point.m_offset -= shift;
        point.m_endOffset -= shift;
    }
    value.m_anchor = anchor;
    return true;
}

/// @brief 按目标谱面的拍轴反解整组，失败时不返回部分条目。
// value 是未绑定目标实体的完整定义，不包含命令或撤销身份。
// anchorSeconds 指向所选基准，而不是整组最早的时间点。
// tempo 是目标谱面红线，按拍模板因此可以复用于不同 BPM。
// 输出遵守 Timing 的毫秒单位，逻辑命令边界再转为秒。
// 任何错误均返回 unexpected，不暴露此前生成的部分结果。
// 内部普通点同刻合法，不自动合并可能有语义的重复效果。
// 段落仍保存为一个 Timing，不按采样率展开成独立事件。
// 实际目标 ECS 的占用校验归逻辑入口，不由领域函数借用注册表。
// 返回对象拥有所有属性副本，可以构造单次批量撤销动作。
// 求逆、函数重新编译和拍轴重建都属于低频编辑提交。
std::expected<std::vector<Timing>, std::string> placeTimingTemplate(
    const TimingTemplate& value, double anchorSeconds,
    std::span<const Timing> tempo, double fallbackBpm)
{
    if ( const auto error = definitionError(value); !error.empty() )
        return std::unexpected(error);
    if ( !std::isfinite(anchorSeconds) || anchorSeconds < 0 )
        return std::unexpected("放置基准时间须为非负有限秒数。");
    auto axis = buildTempo(tempo, fallbackBpm);
    if ( !axis ) return std::unexpected(axis.error());
    const double baseBeat = beatAt(*axis, anchorSeconds);
    const auto   timeAt   = [&](double offset) {
        return value.m_variable == TimingVariable::Beat
                   ? secondsAt(*axis, baseBeat + offset)
                   : anchorSeconds + offset;
    };
    // 此列表只有在所有条目和段落均成功后才返回。
    // 落点验证早于内部冲突比较，避免 NaN 污染区间运算。
    // 负相对偏移允许，负绝对时间则拒绝整组。
    // 时间有限仍可能在秒乘毫秒时溢出，因此另行检查最终单位。
    std::vector<Timing> result;
    for ( const auto& point : value.m_points ) {
        const double start = timeAt(point.m_offset);
        if ( !std::isfinite(start) || start < 0 )
            return std::unexpected(
                "模板有时间点落在零秒之前或超出范围，请后移基准。");
        auto line        = point.m_timing;
        line.m_timestamp = start * 1000.0;
        if ( !std::isfinite(line.m_timestamp) )
            return std::unexpected("时间戳超出范围。");
        if ( line.m_interpolation ) {
            auto& curve = *line.m_interpolation;
            // 改变目标 BPM 会改变拍域模板的实际秒时长。
            // 预设曲线使用归一化形状，调整时长即可保持两端参数。
            // 自定义秒函数需要重写变量尺度，不能只覆盖 duration。
            // 自定义拍函数留到新拍轴绑定时验证新定义域。
            // 输出采样密度随原定义复制，不用目标拍长任意重置 Hz。
            const double oldDuration = curve.m_duration;
            curve.m_duration         = timeAt(point.m_endOffset) - start;
            // 时间函数随模板段长缩放，保持形状；拍函数保留每拍语义。
            // 拍轴重新绑定会更新累计映射和函数定义域，不能沿用来源缓存。
            if ( curve.m_variable == TimingVariable::Time && curve.m_function &&
                 curve.m_duration != oldDuration ) {
                std::string error;
                const auto  expression = rescaleTimingFunctionExpression(
                    *curve.m_function, oldDuration / curve.m_duration, 1.0);
                if ( !setTimingInterpolationFunction(
                         curve,
                         expression,
                         line.m_timingEffectParameter,
                         error) )
                    return std::unexpected(error);
            }
            if ( !std::isfinite(curve.m_duration) || curve.m_duration < .001 )
                return std::unexpected("目标段落时长至少为 1 ms。");
        }
        // 同组段落互相占用内部区间时整组拒绝，普通同刻点允许并存。
        // 目标注册表的冲突仍由逻辑线程在实际写入时复核。
        for ( const auto& previous : result ) {
            if ( previous.m_timingEffect != line.m_timingEffect ) continue;
            const double a = previous.m_timestamp / 1000.0;
            const double b = a + (previous.m_interpolation
                                      ? previous.m_interpolation->m_duration
                                      : 0);
            const double end =
                start +
                (line.m_interpolation ? line.m_interpolation->m_duration : 0);
            if ( (previous.m_interpolation && line.m_interpolation &&
                  std::min(b, end) > std::max(a, start) + 1e-9) ||
                 (previous.m_interpolation && start > a + 1e-9 &&
                  start < b - 1e-9) ||
                 (line.m_interpolation && a > start + 1e-9 && a < end - 1e-9) )
                return std::unexpected("模板内部同类型段落或时间点交叠。");
        }
        result.push_back(std::move(line));
    }
    // 混合秒域模板中的红线也参与段落拍轴，先确定所有落点再重建映射。
    // 输出 BPM 排在原红线之后，同刻采用本次放置的定义。
    // 先建立完整输出再绑定段落，支持秒轴模板同时包含红线和其他效果。
    // 新红线可能改变同组 Scroll 段的按拍采样位置。
    // 因此不能在生成首个条目时立即用不完整的目标拍轴绑定。
    // 新红线的时间已确定，不参与拍域模板位置反解的循环依赖。
    // 最终绑定失败仍拒绝完整组，不降级为普通时间点。
    std::vector<Timing> effectiveTempo(tempo.begin(), tempo.end());
    for ( const auto& line : result ) {
        if ( line.m_timingEffect == TimingEffect::BPM )
            effectiveTempo.push_back(line);
    }
    // 拍轴依赖目标 BPM，来源窗口中的缓存不能直接复用。
    // 秒域曲线没有拍轴，但仍需检查改变段长后的数学定义。
    // 同一次结果中所有段落消费同一份 effectiveTempo。
    // 这次重建只发生在编辑提交，不影响逐帧绘制成本。
    for ( auto& line : result ) {
        if ( !line.m_interpolation ) continue;
        if ( !bindTimingInterpolationBeatAxis(*line.m_interpolation,
                                              line.m_timestamp / 1000.0,
                                              effectiveTempo,
                                              fallbackBpm) ||
             !isValidTimingInterpolation(*line.m_interpolation,
                                         line.m_timingEffect,
                                         line.m_timingEffectParameter) )
            return std::unexpected(
                "目标 BPM 下的段落范围、函数或采样密度不合法。");
    }
    return result;
}

/// @brief 序列化只保存定义，元数据沿用独立来源属性表。
// JSON 保存明确版本，后续协议变更可以显式迁移。
// 坐标轴用稳定字符串，不能以本地化显示文字作为文件标识。
// 基准索引和有符号偏移足以跨谱面定位，不保存源谱面路径。
// 段落通过既有序列化接口保留曲线、表达式和输出密度。
// 拍轴缓存依赖目标红线，不写入个人模板库。
// 来源元数据按原始来源分组保留，放置后仍可往返外部格式。
// 输出数组维持条目顺序，使基准索引可重复读取。
void to_json(nlohmann::json& output, const TimingTemplate& value)
{
    output = { { "version", 1 },
               { "name", value.m_name },
               { "variable",
                 value.m_variable == TimingVariable::Beat ? "beat" : "time" },
               { "anchor", value.m_anchor },
               { "points", nlohmann::json::array() } };
    for ( const auto& point : value.m_points ) {
        const auto&    line = point.m_timing;
        nlohmann::json row  = {
            { "offset", point.m_offset },
            { "end", point.m_endOffset },
            { "effect", timingEffectToString(line.m_timingEffect) },
            { "value", line.m_timingEffectParameter },
            { "metadata", line.m_metadata.timing_properties }
        };
        if ( line.m_interpolation )
            row["interpolation"] = *line.m_interpolation;
        output["points"].push_back(std::move(row));
    }
}

/// @brief 文件字段先检查类型与范围，再执行不会依赖异常的转换。
// 输入可能来自用户编辑过的配置文件，不假定字段类型可靠。
// 顶层版本、数组和索引先验证，之后才窄化为本机索引类型。
// 整数索引采用无符号表示，不接受负数绕回超大索引。
// 名称和效果仅在确认字符串后读取，解析错误通过返回值表达。
// 未知效果不能调用公共兼容解析器后静默变成 Scroll。
// 元数据限制来源编号、键长度、字符串长度和属性数量。
// 字段范围的检查先于枚举转换，避免未定义来源进入属性表。
// 曲线仍由公共读取器解析，不复制表达式解析或数学验证算法。
// 所有条目读取后统一检查定义，基准零偏移与 BPM 轴规则同样生效。
// 读取成功只表示定义有效，放置位置和目标冲突需另外验证。
std::expected<TimingTemplate, std::string> readTimingTemplate(
    const nlohmann::json& input)
{
    const auto failure = [] {
        return std::unexpected(std::string("时间点模板格式损坏或版本不支持。"));
    };
    if ( !input.is_object() || !input.contains("version") ||
         input["version"] != 1 || !input.contains("name") ||
         !input["name"].is_string() || !input.contains("variable") ||
         !input["variable"].is_string() || !input.contains("anchor") ||
         !input["anchor"].is_number_unsigned() || !input.contains("points") ||
         !input["points"].is_array() || input["points"].size() > 4096 )
        return failure();
    TimingTemplate result;
    result.m_name       = input["name"].get<std::string>();
    const auto variable = input["variable"].get<std::string>();
    if ( variable != "time" && variable != "beat" ) return failure();
    result.m_variable =
        variable == "beat" ? TimingVariable::Beat : TimingVariable::Time;
    if ( input["anchor"].get<std::uint64_t>() >= input["points"].size() )
        return failure();
    result.m_anchor = input["anchor"].get<std::size_t>();
    for ( const auto& row : input["points"] ) {
        if ( !row.is_object() || !row.contains("offset") ||
             !row["offset"].is_number() || !row.contains("end") ||
             !row["end"].is_number() || !row.contains("value") ||
             !row["value"].is_number() || !row.contains("effect") ||
             !row["effect"].is_string() )
            return failure();
        TimingTemplatePoint point;
        point.m_offset    = row["offset"].get<double>();
        point.m_endOffset = row["end"].get<double>();
        const auto effect = row["effect"].get<std::string>();
        if ( effect != "bpm" && effect != "scroll" && effect != "jump" &&
             effect != "hs" )
            return failure();
        point.m_timing.m_timingEffect          = timingEffectFromString(effect);
        point.m_timing.m_timingEffectParameter = row["value"].get<double>();
        // 来源属性保存为 [来源编号, 属性对象] 数组，不调用不可信 enum/map
        // 的隐式转换。
        // 数字范围与每个字符串单独验证，未知扩展不会污染已加载模板。
        if ( row.contains("metadata") ) {
            const auto& metadata = row["metadata"];
            if ( !metadata.is_array() || metadata.size() > 3 ) return failure();
            for ( const auto& source : metadata ) {
                if ( !source.is_array() || source.size() != 2 ||
                     !source[0].is_number_integer() || !source[1].is_object() ||
                     source[0].get<std::int64_t>() < 0 ||
                     source[0].get<std::uint64_t>() > 2 ||
                     source[1].size() > 256 )
                    return failure();
                auto& properties =
                    point.m_timing.m_metadata
                        .timing_properties[static_cast<TimingMetadataType>(
                            source[0].get<int>())];
                for ( const auto& [key, text] : source[1].items() ) {
                    if ( !text.is_string() || key.size() > 1024 ||
                         text.get_ref<const std::string&>().size() > 65536 )
                        return failure();
                    properties[key] = text.get<std::string>();
                }
            }
        }
        // 只有合法完整段落才保留为可选值。
        // 不允许损坏曲线被丢弃后变成看似正常的普通点。
        // 不可信输入没有实体分配、网络发送或撤销动作。
        if ( row.contains("interpolation") ) {
            point.m_timing.m_interpolation =
                readTimingInterpolation(row["interpolation"]);
            if ( !point.m_timing.m_interpolation ) return failure();
        }
        result.m_points.push_back(std::move(point));
    }
    if ( const auto error = definitionError(result); !error.empty() )
        return std::unexpected(error);
    return result;
}
/// @brief 校验外部文件的全部模板，任何一项损坏都拒绝整个导入候选。
/// @note 单模板沿用独立定义版本；模板库还需校验外层版本。
/// @warning 仅用于低频文件导入，不在每帧 UI 路径执行。
std::expected<std::vector<TimingTemplate>, std::string>
readTimingTemplateBundle(const nlohmann::json& input)
{
    std::vector<TimingTemplate> result;
    // 导入候选与目标个人库完全独立，解析失败不修改任何调用方容器。
    // 单定义和模板库均使用同一个版本化读取契约。
    // 顶层裸数组没有版本契约，不能猜测为模板库。
    if ( !input.is_object() )
        return std::unexpected("模板文件必须是模板或模板库对象。");
    if ( !input.contains("templates") ) {
        // 单定义同样经过完整领域校验，不能仅凭 name 判断有效。
        auto parsed = readTimingTemplate(input);
        if ( !parsed ) return std::unexpected(parsed.error());
        result.push_back(std::move(*parsed));
        return result;
    }
    // 空库不构成可添加内容，超预算文件在读取子定义前拒绝。
    if ( !input.contains("version") || input["version"] != 1 ||
         !input["templates"].is_array() || input["templates"].empty() ||
         input["templates"].size() > 256 )
        return std::unexpected("模板库版本不支持、为空或超过 256 个模板。");
    for ( const auto& item : input["templates"] ) {
        auto parsed = readTimingTemplate(item);
        // 返回 expected 错误时局部候选随值销毁，不发布前半份内容。
        if ( !parsed ) return std::unexpected(parsed.error());
        // 个人库以名称为键，同文件重复名不能依赖先后顺序覆盖。
        // 输入总数受限，低频线性查重避免引入长期辅助索引。
        if ( std::any_of(result.begin(), result.end(), [&](const auto& saved) {
                 return saved.m_name == parsed->m_name;
             }) )
            return std::unexpected("模板文件中有重复名称：" + parsed->m_name);
        result.push_back(std::move(*parsed));
    }
    // 调用方获得完整值列表；取消添加不会影响来源或个人库。
    return result;
}
}  // namespace MMM
