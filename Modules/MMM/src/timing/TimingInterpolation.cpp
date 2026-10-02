#include "mmm/timing/TimingInterpolation.h"

#include "mmm/timing/BpmNormalization.h"
#include "mmm/timing/Timing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <numbers>
#include <utility>

namespace MMM
{
namespace
{
/// @brief 计算起终点为零和一的三次贝塞尔坐标。
/// @param t 贝塞尔参数，并非实际时间比例。
/// @param first 第一控制点在当前轴上的坐标。
/// @param second 第二控制点在当前轴上的坐标。
/// @return 当前轴的三次多项式值。
/// @note 两端固定，因此只需保存两个内部控制坐标。
double cubicCoordinate(double t, double first, double second)
{
    const double remaining = 1.0 - t;
    return 3.0 * remaining * remaining * t * first +
           3.0 * remaining * t * t * second + t * t * t;
}

/// @brief 取得曲线在单位区间内的形状值。
/// @warning 热路径；贝塞尔反解有固定迭代上限，不依赖时间等待。
/// @param interpolation 包含曲线种类和贝塞尔控制点的有效段定义。
/// @param x 归一化时间，范围外按最近端点处理。
/// @return 归一化参数值，不包含所属 Timing 的两端参数。
/// @pre 公开入口的调用方须先通过统一合法性校验。
/// @note 控制点在单位方形中，参数不会越过两端数值范围。
/// @note 同一形状适用于 BPM、Scroll、Jump 与 HS。
double curveProgress(const TimingInterpolation& interpolation, double x)
{
    // 外推采用两端常值，保证曲线查询与段尾继承语义一致。
    // 精确端点提前返回，避免二分误差让用户给定值略微偏离。
    x = std::clamp(x, 0.0, 1.0);
    if ( x == 0.0 || x == 1.0 ) return x;
    switch ( interpolation.m_curve ) {
    // 曲线的单位端点固定，直线即归一化时间本身。
    // EaseIn 和 EaseOut 使用互补的二次曲线，保证数值位于单位区间。
    // SmoothStep 在两个端点的斜率为零，适用于需要平滑衔接的变化。
    // 正弦函数和指数函数也按同一端点契约归一化。
    case TimingCurve::Linear: return x;
    case TimingCurve::EaseIn: return x * x;
    case TimingCurve::EaseOut: return x * (2.0 - x);
    case TimingCurve::SmoothStep: return x * x * (3.0 - 2.0 * x);
    case TimingCurve::Sine: return (1.0 - std::cos(std::numbers::pi * x)) * 0.5;
    case TimingCurve::Exponential: return std::expm1(4.0 * x) / std::expm1(4.0);
    case TimingCurve::Bezier: {
        // 横轴也有控制点，不能直接把时间比例当成贝塞尔参数。
        // 横轴单调且端点固定为零与一，整个有效解始终位于这个区间。
        // 不采用无上限 Newton 迭代，水平切线不会导致求解发散。
        double low  = 0.0;
        double high = 1.0;
        for ( int iteration = 0; iteration < 32; ++iteration ) {
            const double middle = (low + high) * 0.5;
            if ( cubicCoordinate(middle,
                                 interpolation.m_controlX1,
                                 interpolation.m_controlX2) < x )
                low = middle;
            else
                high = middle;
        }
        return cubicCoordinate((low + high) * 0.5,
                               interpolation.m_controlY1,
                               interpolation.m_controlY2);
    }
    }
    return x;
}
}  // namespace

/// @brief 对整段数据进行有限值和资源预算校验。
/// @param interpolation 未受信任的输入或文件段落。
/// @param effect 所属 Timing 类型，决定参数的领域约束。
/// @param startValue 所属 Timing 提供的段首值。
/// @return 所有数值、资源预算和领域约束均满足时返回真。
/// @note 不包含时间戳与同类型重叠检查，调用方拥有这些上下文。
/// @note 贝塞尔纵轴同样受限，正 BPM 曲线不会中途过零。
/// @note 允许负 Scroll 等效果，不能将所有效果套用 BPM 约束。
bool isValidTimingInterpolation(const TimingInterpolation& interpolation,
                                TimingEffect effect, double startValue)
{
    // 统一集中有限性检查，密度和控制点同样不能带 NaN 或无穷。
    // startValue 属于外部 Timing 字段，也必须加入校验集合。
    // 先拒绝非有限值，再比较界限，避免 NaN 的比较结果绕过限制。
    const std::array values{
        interpolation.m_duration,         interpolation.m_endValue,
        interpolation.m_samplesPerSecond, interpolation.m_controlX1,
        interpolation.m_controlY1,        interpolation.m_controlX2,
        interpolation.m_controlY2,        startValue
    };
    // 数值先验证有限性，之后乘法、取整和贝塞尔反解才具有确定边界。
    if ( !std::all_of(values.begin(),
                      values.end(),
                      [](double value) { return std::isfinite(value); }) ||
         interpolation.m_duration < 0.001 ||
         interpolation.m_samplesPerSecond <= 0.0 ||
         interpolation.m_duration * interpolation.m_samplesPerSecond <= 0.0 ||
         interpolation.m_duration * interpolation.m_samplesPerSecond >
             static_cast<double>(MAX_TIMING_INTERPOLATION_SAMPLES) ||
         interpolation.m_curve < TimingCurve::Linear ||
         interpolation.m_curve > TimingCurve::Bezier )
        return false;
    // 控制点限制到单位方形，保证正 BPM 不会由曲线过冲变成负数。
    for ( double value : { interpolation.m_controlX1,
                           interpolation.m_controlY1,
                           interpolation.m_controlX2,
                           interpolation.m_controlY2 } ) {
        if ( value < 0.0 || value > 1.0 ) return false;
    }
    // 控制点时间顺序不能反向，否则一个实际时刻可能对应多个参数解。
    // 此限制也让低频编辑器与每帧求值保持同一个合法曲线集合。
    if ( interpolation.m_controlX1 > interpolation.m_controlX2 ) return false;
    if ( effect == TimingEffect::BPM ) {
        return startValue >= MIN_NORMALIZED_BPM &&
               startValue <= MAX_NORMALIZED_BPM &&
               interpolation.m_endValue >= MIN_NORMALIZED_BPM &&
               interpolation.m_endValue <= MAX_NORMALIZED_BPM;
    }
    // 其他效果保留符号，允许反向 Scroll 与已有格式的有符号效果。
    return true;
}

/// @brief 在原始曲线上求值，外部密度不会改变编辑器查询结果。
/// @param interpolation 已校验的曲线定义。
/// @param startValue 起始参数；终值来自定义中的 endValue。
/// @param progress 实际时间比例，不是贝塞尔内部参数。
/// @return 起终值之间的参数，超出区间则返回相应端值。
/// @note 输出密度只是写出规则，不改变这里的函数或曲率。
/// @note std::lerp 对方向相反的端值也提供稳定线性组合。
double evaluateTimingInterpolation(const TimingInterpolation& interpolation,
                                   double startValue, double progress)
{
    return std::lerp(startValue,
                     interpolation.m_endValue,
                     curveProgress(interpolation, progress));
}

/// @brief 用解析原函数计算 BPM 等参数的累计量。
/// @param interpolation 已校验的段落定义。
/// @param startValue 段首参数，BPM 调用方须保证它严格为正。
/// @param elapsedSeconds 相对段首的秒数。
/// @return 参数乘秒；调用方自行换算为拍数或其它累计量。
/// @details 对常用曲线使用解析原函数，对贝塞尔先反解时间坐标。
/// @note 段前使用首值、段后使用终值，不另外生成锚点或重启相位。
/// @note 这一接口与采样器分离，粗密度导出不会降低拍位定位精度。
/// @note 固定迭代是数值算法上限，不是等待状态或时钟的轮询。
double integrateTimingInterpolation(const TimingInterpolation& interpolation,
                                    double startValue, double elapsedSeconds)
{
    // 首锚点以前沿用首值，因此负拍号仍与旧的等速 BPM 相位兼容。
    // 正时间只在曲线内部积分，再把终点后的常值尾段单独相加。
    if ( elapsedSeconds <= 0.0 ) return startValue * elapsedSeconds;
    const double inside = std::min(elapsedSeconds, interpolation.m_duration);
    // 先截到曲线内部再归一化；尾段在函数末尾单独累加。
    // 输出采样周期完全不参与这个时间比例，修改密度不会漂移拍位。
    const double x             = inside / interpolation.m_duration;
    double       shapeIntegral = 0.0;
    // 多项式与常用缓动函数直接使用原函数；密度不参与积分。
    switch ( interpolation.m_curve ) {
    case TimingCurve::Linear: shapeIntegral = x * x / 2.0; break;
    case TimingCurve::EaseIn: shapeIntegral = x * x * x / 3.0; break;
    case TimingCurve::EaseOut: shapeIntegral = x * x - x * x * x / 3.0; break;
    case TimingCurve::SmoothStep:
        shapeIntegral = x * x * x - x * x * x * x / 2.0;
        break;
    case TimingCurve::Sine:
        shapeIntegral =
            (x - std::sin(std::numbers::pi * x) / std::numbers::pi) / 2.0;
        break;
    case TimingCurve::Exponential:
        shapeIntegral = (std::expm1(4.0 * x) / 4.0 - x) / std::expm1(4.0);
        break;
    case TimingCurve::Bezier: {
        // 参数曲线的积分是 y(u) * x'(u)，六阶原函数可以精确计算。
        double low = 0.0, high = 1.0;
        for ( int iteration = 0; iteration < 32; ++iteration ) {
            const double middle = (low + high) * 0.5;
            if ( cubicCoordinate(middle,
                                 interpolation.m_controlX1,
                                 interpolation.m_controlX2) < x )
                low = middle;
            else
                high = middle;
        }
        const double u = (low + high) * 0.5;
        // y 的常数项为零；dx 为时间坐标的一阶导数。
        // 两个低阶多项式相乘后按幂次积分，避免采用输出采样来近似拍数。
        const std::array y{ 0.0,
                            3.0 * interpolation.m_controlY1,
                            3.0 * interpolation.m_controlY2 -
                                6.0 * interpolation.m_controlY1,
                            1.0 + 3.0 * interpolation.m_controlY1 -
                                3.0 * interpolation.m_controlY2 };
        const std::array dx{ 3.0 * interpolation.m_controlX1,
                             6.0 * interpolation.m_controlX2 -
                                 12.0 * interpolation.m_controlX1,
                             3.0 + 9.0 * interpolation.m_controlX1 -
                                 9.0 * interpolation.m_controlX2 };
        // 只有十二区间乘积，固定循环不创建动态临时容器。
        for ( std::size_t yi = 1; yi < y.size(); ++yi )
            for ( std::size_t xi = 0; xi < dx.size(); ++xi )
                shapeIntegral += y[yi] * dx[xi] *
                                 std::pow(u, static_cast<int>(yi + xi + 1)) /
                                 static_cast<double>(yi + xi + 1);
        break;
    }
    }
    // 常量首值与曲线形状积分分别累加，起终方向相反仍使用同一公式。
    // 这样 BPM 查询无需生成或排序 Timing 采样，也不会重启段内拍位。
    const double integral =
        startValue * inside + (interpolation.m_endValue - startValue) *
                                  interpolation.m_duration * shapeIntegral;
    // 段尾之后继承终值，查询拍位不会重新回到段首 BPM。
    return integral + std::max(0.0, elapsedSeconds - interpolation.m_duration) *
                          interpolation.m_endValue;
}

/// @brief 数量包含段尾，非整周期区间仍保留用户指定的时间终点。
/// @param interpolation 提供时长和每秒采样次数的段落。
/// @return 间隔数量向上取整后加一个端点；预算无效时返回零。
/// @note 密度允许非整数，采样周期始终由 1 / Hz 决定。
/// @note 不能用总数量均分区间，否则非整周期时会改变指定采样率。
/// @note 数量上限在浮点转整数之前检查，避免不可信输入溢出。
std::size_t timingInterpolationSampleCount(
    const TimingInterpolation& interpolation)
{
    // 先向上取整间隔数，终点不是完整周期时额外保留最后一个短周期。
    // 周期内的样本仍用整数索引除密度，不改成均分总时长。
    // 上限约束最终数组容量，避免读取极端密度后申请不可控内存。
    const double intervals =
        std::ceil(interpolation.m_duration * interpolation.m_samplesPerSecond);
    if ( !std::isfinite(intervals) || intervals < 1.0 ||
         intervals > static_cast<double>(MAX_TIMING_INTERPOLATION_SAMPLES) )
        return 0;
    return static_cast<std::size_t>(intervals) + 1;
}

/// @brief 从段落定义生成写出用独立事件，采样时间由整数索引直接计算。
/// @param timings 保留编辑语义的原生 Timing 列表。
/// @return 普通事件与段落输出样本的按时间排序列表。
/// @details 只在格式写出时调用；不会把结果写回编辑模型。
/// @note 每个生成事件清除段落定义，递归保存不会再次展开。
/// @note BPM 派生字段必须和效果参数一致，格式 writer 不负责反推曲线。
/// @note 当前同刻分组有限去重，用来消除相邻段共用的端点。
/// @note 无插值输入直接返回原列表，旧谱面的重复事件语义不改变。
/// @warning 保存低频路径，可分配样本容器；不得用于每帧 UI 或更新循环。
std::vector<Timing> sampleTimingInterpolations(
    const std::vector<Timing>& timings)
{
    // 没有段落时保持普通时间点的顺序与重复事件语义。
    if ( std::none_of(timings.begin(), timings.end(), [](const auto& timing) {
             return timing.m_interpolation.has_value();
         }) )
        return timings;
    // 原始事件通过值复制保存，格式展开永远不改变编辑器中的段落对象。
    // 外部格式缺乏段落语义，结果中的所有事件都不再携带插值定义。
    std::vector<Timing> result;
    for ( const auto& timing : timings ) {
        if ( !timing.m_interpolation ) {
            result.push_back(timing);
            continue;
        }
        const auto& interpolation = *timing.m_interpolation;
        if ( !isValidTimingInterpolation(interpolation,
                                         timing.m_timingEffect,
                                         timing.m_timingEffectParameter) )
            continue;
        // 计数与创建窗口共用一个函数，用户看到的预计数量就是这里的写出预算。
        // 段首 index 为零，精确段尾只通过最后一个索引产生一次。
        const auto count = timingInterpolationSampleCount(interpolation);
        for ( std::size_t index = 0; index < count; ++index ) {
            // 最后一次落在精确段尾；之前按 Hz 固定间隔而非均分缩短采样周期。
            const double elapsed = std::min(
                interpolation.m_duration,
                static_cast<double>(index) / interpolation.m_samplesPerSecond);
            // 值副本保留格式来源属性和采样相关领域字段。
            // 唯一被删除的语义是插值定义本身与过期的来源拍位。
            auto sampled = timing;
            sampled.m_interpolation.reset();
            sampled.m_timestamp += elapsed * 1000.0;
            sampled.m_timingEffectParameter =
                evaluateTimingInterpolation(interpolation,
                                            timing.m_timingEffectParameter,
                                            elapsed / interpolation.m_duration);
            if ( sampled.m_timingEffect == TimingEffect::BPM ) {
                sampled.m_bpm         = sampled.m_timingEffectParameter;
                sampled.m_beat_length = 60000.0 / sampled.m_bpm;
            }
            // 旧 Malody beat 只适用于段首，插值采样的拍位必须由保存器重新计算。
            if ( auto source = sampled.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 source != sampled.m_metadata.timing_properties.end() )
                source->second.erase("beat");
            result.push_back(std::move(sampled));
        }
    }
    std::stable_sort(
        result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.m_timestamp < right.m_timestamp;
        });
    // 只检查当前同刻分组，不逆向扫描整个谱面；共同边界采用最后值。
    // 每组每种效果最多一条，保证 Jump 不因相邻段而重复触发。
    std::vector<Timing> unique;
    // 同刻最多四种效果，组内检查的成本受效果种类而非全谱长度限制。
    // 不同效果保持独立，不能将同刻 BPM 与 Scroll 错误地合并。
    // 相邻两段共用端点时，后一段首值优先于前一段尾值。
    // 稳定排序保留输入顺序，使边界覆盖结果能够重复得到。
    std::size_t groupBegin = 0;
    for ( auto& timing : result ) {
        if ( unique.empty() ||
             std::abs(unique.back().m_timestamp - timing.m_timestamp) >= 1e-6 )
            groupBegin = unique.size();
        const auto existing = std::find_if(
            unique.begin() + groupBegin, unique.end(), [&](const auto& item) {
                return item.m_timingEffect == timing.m_timingEffect;
            });
        if ( existing != unique.end() )
            *existing = std::move(timing);
        else
            unique.push_back(std::move(timing));
    }
    return unique;
}

/// @brief 用具名字段存储段落，避免枚举布局影响原生文件可读性。
/// @param json 接收段落的原生 JSON 对象。
/// @param interpolation 已校验的值定义，不含所属实体或时间锚点。
/// @note 控制数组固定为 X1、Y1、X2、Y2，不随图形坐标轴方向改变。
/// @note 曲线编号有明确范围，新增函数时须维持既有编号含义。
void to_json(nlohmann::json& json, const TimingInterpolation& interpolation)
{
    json = { { "duration", interpolation.m_duration },
             { "end_value", interpolation.m_endValue },
             { "samples_per_second", interpolation.m_samplesPerSecond },
             { "curve", static_cast<int>(interpolation.m_curve) },
             { "control",
               { interpolation.m_controlX1,
                 interpolation.m_controlY1,
                 interpolation.m_controlX2,
                 interpolation.m_controlY2 } } };
}

/// @brief 校验类型后读取 JSON，错误文件不会进入异常路径。
/// @param json 原生文件或联机文档提供的段落对象。
/// @return 类型和数值均合法时返回定义，否则返回空。
/// @note 先验证字段类型再读取，禁止通过异常捕获处理损坏文档。
/// @note 所属 Timing 随后补充起始参数与 BPM 约束。
/// @note 未知字段可忽略，但缺少必要字段不得推断成普通时间点。
/// @note 无效控制点与未知曲线拒绝载入，避免不同平台产生不同曲线。
std::optional<TimingInterpolation> readTimingInterpolation(
    const nlohmann::json& json)
{
    if ( !json.is_object() ) return std::nullopt;
    TimingInterpolation interpolation;
    for ( auto [key, target] :
          { std::pair{ "duration", &interpolation.m_duration },
            std::pair{ "end_value", &interpolation.m_endValue },
            std::pair{ "samples_per_second",
                       &interpolation.m_samplesPerSecond } } ) {
        const auto value = json.find(key);
        if ( value == json.end() || !value->is_number() ) return std::nullopt;
        *target = value->get<double>();
        if ( !std::isfinite(*target) ) return std::nullopt;
    }
    // 曲线编号先读为浮点并限制范围，超大无符号 JSON 整数不会转换溢出。
    // 再转换枚举；四个控制坐标各自独立检查 JSON 数值类型。
    const auto curve   = json.find("curve");
    const auto control = json.find("control");
    if ( curve == json.end() || !curve->is_number_integer() ||
         control == json.end() || !control->is_array() || control->size() != 4 )
        return std::nullopt;
    const auto curveValue = curve->get<double>();
    if ( curveValue < 0 || curveValue > static_cast<int>(TimingCurve::Bezier) )
        return std::nullopt;
    interpolation.m_curve = static_cast<TimingCurve>(curveValue);
    // JSON 数组是存储字段而非可执行函数，逐项读取保持确定顺序。
    // 所有坐标检查完成后再调用领域校验，不接受部分有效的贝塞尔定义。
    const std::array targets{ &interpolation.m_controlX1,
                              &interpolation.m_controlY1,
                              &interpolation.m_controlX2,
                              &interpolation.m_controlY2 };
    for ( std::size_t index = 0; index < targets.size(); ++index ) {
        if ( !(*control)[index].is_number() ) return std::nullopt;
        *targets[index] = (*control)[index].get<double>();
    }
    // 与类型无关的校验在此完成，所属时间点随后补充 BPM 起终边界。
    if ( !isValidTimingInterpolation(interpolation, TimingEffect::SCROLL, 1.0) )
        return std::nullopt;
    return interpolation;
}
}  // namespace MMM
