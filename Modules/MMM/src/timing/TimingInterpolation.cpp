#include "mmm/timing/TimingInterpolation.h"

#include "mmm/timing/BpmNormalization.h"
#include "mmm/timing/Timing.h"
#include "mmm/timing/TimingFunction.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>
#include <utility>

namespace MMM
{
namespace
{
/// @brief 自定义端点允许跨平台数学库的末位舍入差异，仍拒绝实质参数偏离。
/// @param first 保存的外部 Timing 参数。
/// @param second 当前平台按同一函数计算的端值。
/// @return 差值在相对机器精度容差内时为真。
/// @pre 入口已验证两端有限，不把 NaN 或无穷作为函数参数。
/// @warning 每次校验只执行常量标量运算，不读取文件或重新编译。
bool sameFunctionValue(double first, double second)
{
    // 公式在平台间重编译，libm 不保证逐位相同；仅容许几十个末位差异。
    // 保留文件原值而非静默改写，序列化往返仍具有稳定字段语义。
    const double tolerance =
        64 * std::numeric_limits<double>::epsilon() *
        std::max({ 1.0, std::abs(first), std::abs(second) });
    return std::abs(first - second) <= tolerance;
}

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
    case TimingCurve::Cubic: return x * x * x;
    case TimingCurve::Quartic: return x * x * x * x;
    case TimingCurve::Quintic: return x * x * x * x * x;
    case TimingCurve::SquareRoot: return std::sqrt(x);
    case TimingCurve::CubeRoot: return std::cbrt(x);
    case TimingCurve::Logarithmic: return std::log1p(9 * x) / std::log(10.0);
    case TimingCurve::Reciprocal: return 2 * x / (1 + x);
    case TimingCurve::SineIn: return 1 - std::cos(std::numbers::pi * x / 2);
    case TimingCurve::SineOut: return std::sin(std::numbers::pi * x / 2);
    case TimingCurve::HyperbolicTangent:
        return std::tanh(3 * x) / std::tanh(3.0);
    case TimingCurve::Custom: return x;  // 绝对参数函数由公开求值入口单独处理。
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
/// @details 自定义曲线必须拥有已编译对象，不能只检查枚举或文本。
/// 函数定义域长度必须与段落时长一致，旧缓存不能直接套到新范围。
/// 首尾值须与同一函数的端点求值一致，禁止独立端值暗中改写函数。
/// BPM 检查完整保守包围，其他效果继续接受有符号参数。
/// 密度与曲线合法性独立，低密度不能掩盖中间奇点或负 BPM。
/// 所有预设仍保持原来的单位区间范围语义。
/// 非法定义返回假，由 UI 与逻辑提交分别给出相应反馈。
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
         (interpolation.m_variable == TimingVariable::Time &&
          interpolation.m_duration * interpolation.m_samplesPerSecond >
              static_cast<double>(MAX_TIMING_INTERPOLATION_SAMPLES)) ||
         interpolation.m_curve < TimingCurve::Linear ||
         interpolation.m_curve > TimingCurve::Custom )
        return false;
    // 分拍必须为正且预算有界；BPM 禁止使用自己的拍长定义自变量。
    // 文件中的派生拍长用于重新编译，播放前由完整红线时间轴重新绑定。
    if ( interpolation.m_variable != TimingVariable::Time &&
         interpolation.m_variable != TimingVariable::Beat )
        return false;
    if ( interpolation.m_variable == TimingVariable::Beat &&
         (effect == TimingEffect::BPM || interpolation.m_beatNumerator <= 0 ||
          interpolation.m_beatDenominator <= 0 ||
          interpolation.m_beatNumerator > 65536 ||
          interpolation.m_beatDenominator > 65536 ||
          !std::isfinite(interpolation.m_beatDuration) ||
          interpolation.m_beatDuration <= 0 ||
          timingInterpolationSampleCount(interpolation) == 0) )
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
    if ( interpolation.m_curve == TimingCurve::Custom ) {
        // 函数缓存与时长绑定；编辑范围后必须重新编译，不能复用旧定义域。
        // 两端来自表达式，外部 Timing 参数不得与函数首值产生跳变。
        if ( !interpolation.m_function ||
             timingFunctionDuration(*interpolation.m_function) !=
                 timingInterpolationVariableDuration(interpolation) ||
             !sameFunctionValue(
                 evaluateTimingFunction(*interpolation.m_function, 0),
                 startValue) ||
             !sameFunctionValue(
                 evaluateTimingFunction(
                     *interpolation.m_function,
                     timingInterpolationVariableDuration(interpolation)),
                 interpolation.m_endValue) )
            return false;
        const auto [minimum, maximum] =
            timingFunctionRange(*interpolation.m_function);
        return effect != TimingEffect::BPM ||
               (minimum >= MIN_NORMALIZED_BPM && maximum <= MAX_NORMALIZED_BPM);
    }
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
    // 时间比例先转成当前自变量；变 BPM 时不能把拍数当成线性秒数。
    return evaluateTimingInterpolationVariable(
        interpolation,
        startValue,
        timingInterpolationVariableAtSeconds(
            interpolation,
            std::clamp(progress, 0.0, 1.0) * interpolation.m_duration));
}

/// @brief 在所选自变量域中执行预设或绝对参数函数。
/// @param variable 距段首的秒数或拍数，由段落轴类型决定。
/// @note 手绘拟合在此域等距采点，输出预览则先完成时间到拍位换算。
/// @warning 每帧绘图路径，固定算术与缓存查找，不分配或复制共享所有权。
// 自定义函数输出绝对参数，预设才使用起终值插值。
// 贝塞尔横轴坐标属于当前自变量比例，不固定为秒比例。
// 手绘直接传入拍位，时间预览须先转换为拍位。
// 分拍不改变函数，也不把连续预览变成导出阶梯。
// 稀疏输出之间仍可显示真实曲率。
// 非法域返回首值，避免编辑副本造成除零。
double evaluateTimingInterpolationVariable(
    const TimingInterpolation& interpolation, double startValue,
    double variable)
{
    const double duration = timingInterpolationVariableDuration(interpolation);
    if ( !std::isfinite(duration) || duration <= 0 ) return startValue;
    variable = std::clamp(variable, 0.0, duration);
    // 自定义函数直接输出绝对参数，预设只将自变量归一为曲线进度。
    if ( interpolation.m_curve == TimingCurve::Custom &&
         interpolation.m_function )
        return evaluateTimingFunction(*interpolation.m_function, variable);
    return std::lerp(startValue,
                     interpolation.m_endValue,
                     curveProgress(interpolation, variable / duration));
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
/// @details 自定义函数采用编译时建立的累计积分缓存。
/// 缓存精度不取决于导出切分密度，拍位反解可以使用同一映射。
/// 预设的解析积分与自定义的缓存积分共同返回参数乘秒。
/// 段前与段后均采用端值外推，避免定义域外丢失拍位。
/// 返回值不包含外部基准拍位，调用方负责叠加之前的段落。
/// 同一函数对象不会随查询时间更新节点，不引入播放锁。
double integrateTimingInterpolation(const TimingInterpolation& interpolation,
                                    double startValue, double elapsedSeconds)
{
    // 节拍效果的累计量仍以秒计，不能直接把拍域积分误当作时间积分。
    // 该分支仅服务非 BPM 参数；红线始终使用下面的解析秒域实现。
    if ( interpolation.m_variable == TimingVariable::Beat ) {
        if ( elapsedSeconds <= 0 ) return startValue * elapsedSeconds;
        const double end = std::min(elapsedSeconds, interpolation.m_duration);
        constexpr std::array nodes{ -.8611363115940526,
                                    -.3399810435848563,
                                    .3399810435848563,
                                    .8611363115940526 };
        constexpr std::array weights{ .3478548451374539,
                                      .6521451548625461,
                                      .6521451548625461,
                                      .3478548451374539 };
        double               result = 0;
        // 固定分段高斯预算与输出密度分离，低密度不会改变连续参数积分。
        for ( int part = 0; part < 64; ++part ) {
            const double middle = end * (part + .5) / 64;
            const double half   = end / 128;
            for ( std::size_t i = 0; i < nodes.size(); ++i )
                result +=
                    half * weights[i] *
                    evaluateTimingInterpolation(
                        interpolation,
                        startValue,
                        (middle + half * nodes[i]) / interpolation.m_duration);
        }
        return result +
               std::max(0.0, elapsedSeconds - end) * interpolation.m_endValue;
    }
    // 首锚点以前沿用首值，因此负拍号仍与旧的等速 BPM 相位兼容。
    // 正时间只在曲线内部积分，再把终点后的常值尾段单独相加。
    if ( interpolation.m_curve == TimingCurve::Custom &&
         interpolation.m_function )
        return integrateTimingFunction(*interpolation.m_function,
                                       elapsedSeconds);
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
    // 幂、根式、对数和三角预设各自使用解析积分，不依赖输出密度。
    case TimingCurve::Cubic: shapeIntegral = std::pow(x, 4) / 4; break;
    case TimingCurve::Quartic: shapeIntegral = std::pow(x, 5) / 5; break;
    case TimingCurve::Quintic: shapeIntegral = std::pow(x, 6) / 6; break;
    case TimingCurve::SquareRoot:
        shapeIntegral = 2 * std::pow(x, 1.5) / 3;
        break;
    case TimingCurve::CubeRoot:
        shapeIntegral = 3 * std::pow(x, 4.0 / 3.0) / 4;
        break;
    case TimingCurve::Logarithmic:
        shapeIntegral =
            ((1 + 9 * x) * std::log1p(9 * x) - 9 * x) / (9 * std::log(10.0));
        break;
    case TimingCurve::Reciprocal:
        shapeIntegral = 2 * (x - std::log1p(x));
        break;
    case TimingCurve::SineIn:
        shapeIntegral =
            x - 2 * std::sin(std::numbers::pi * x / 2) / std::numbers::pi;
        break;
    case TimingCurve::SineOut:
        shapeIntegral =
            2 * (1 - std::cos(std::numbers::pi * x / 2)) / std::numbers::pi;
        break;
    case TimingCurve::HyperbolicTangent:
        shapeIntegral = std::log(std::cosh(3 * x)) / (3 * std::tanh(3.0));
        break;
    case TimingCurve::Custom: break;  // 有效自定义函数已在积分入口提前返回。
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
// 拍域预算取拍长除以有理分拍，与展示平均 Hz 无关。
// 变 BPM 不让计数与真实反解出现不同数量。
// 不足完整分拍仍写出段尾，不扩大用户时间范围。
// 舍入处理只针对积分后接近整数的拍域预算。
// 秒域保持 ceil(duration * Hz) 的非整周期语义。
// 非有限预算在 size_t 转换前拒绝。
// 零数量表示不能提交，不允许尝试创建数组。
std::size_t timingInterpolationSampleCount(
    const TimingInterpolation& interpolation)
{
    // 先向上取整间隔数，终点不是完整周期时额外保留最后一个短周期。
    // 周期内的样本仍用整数索引除密度，不改成均分总时长。
    // 上限约束最终数组容量，避免读取极端密度后申请不可控内存。
    double raw =
        interpolation.m_variable == TimingVariable::Beat
            ? interpolation.m_beatDuration * interpolation.m_beatDenominator /
                  interpolation.m_beatNumerator
            : interpolation.m_duration * interpolation.m_samplesPerSecond;
    // 红线积分和有理分拍可能把整数预算变成 n+一个舍入误差，不能多插重复尾点。
    // 只在拍域消除双精度舍入，不改变用户按秒设置的非整周期契约。
    if ( interpolation.m_variable == TimingVariable::Beat &&
         std::isfinite(raw) &&
         std::abs(raw - std::round(raw)) <=
             16 * std::numeric_limits<double>::epsilon() *
                 std::max(1.0, std::abs(raw)) )
        raw = std::round(raw);
    const double intervals = std::ceil(raw);
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
    const std::vector<Timing>& timings, double fallbackBpm)
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
        auto interpolation = *timing.m_interpolation;
        // 保存必须消费当前完整红线数据，不能依赖 UI 或旧快照中的拍长。
        if ( !bindTimingInterpolationBeatAxis(interpolation,
                                              timing.m_timestamp / 1000.0,
                                              timings,
                                              fallbackBpm) )
            return {};
        if ( !isValidTimingInterpolation(interpolation,
                                         timing.m_timingEffect,
                                         timing.m_timingEffectParameter) )
            continue;
        // 计数与创建窗口共用一个函数，用户看到的预计数量就是这里的写出预算。
        // 段首 index 为零，精确段尾只通过最后一个索引产生一次。
        const auto count = timingInterpolationSampleCount(interpolation);
        for ( std::size_t index = 0; index < count; ++index ) {
            // 最后一次落在精确段尾；之前按 Hz 固定间隔而非均分缩短采样周期。
            const double elapsed =
                timingInterpolationSampleElapsed(interpolation, index);
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
/// @details 原生格式保留源函数式，不持久化内部指令或积分节点。
/// 每个文件中的时长决定重新编译时的定义域。
/// Custom 的文本字段与枚举配套出现，普通预设无需该字段。
/// 输出密度继续独立保存，重新编辑仍只有一个段落实体。
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
    // 时间轴字段缺省保持兼容；节拍模式同时保存分拍和函数定义域。
    // 积分映射不写文件，载入后按该谱面的实际红线重新准备。
    if ( interpolation.m_variable == TimingVariable::Beat ) {
        json["variable"]      = "beat";
        json["beat_step"]     = { interpolation.m_beatNumerator,
                                  interpolation.m_beatDenominator };
        json["beat_duration"] = interpolation.m_beatDuration;
    }
    // 只持久化可编辑文本，内部字节码和积分缓存不属于文件协议。
    if ( interpolation.m_curve == TimingCurve::Custom &&
         interpolation.m_function )
        json["function"] = timingFunctionExpression(*interpolation.m_function);
}

/// @brief 校验类型后读取 JSON，错误文件不会进入异常路径。
/// @param json 原生文件或联机文档提供的段落对象。
/// @return 类型和数值均合法时返回定义，否则返回空。
/// @note 先验证字段类型再读取，禁止通过异常捕获处理损坏文档。
/// @note 所属 Timing 随后补充起始参数与 BPM 约束。
/// @note 未知字段可忽略，但缺少必要字段不得推断成普通时间点。
/// @note 无效控制点与未知曲线拒绝载入，避免不同平台产生不同曲线。
/// @details Custom 加载必须重新编译，不能信任文件中的端点声明。
/// 文本、时长和密度分别预检，旧预设格式仍可按原枚举读取。
/// 函数终值与文件终值不一致时拒绝，避免显示和运行使用不同参数。
/// 失败时不构造带空函数缓存的可编辑段落。
/// 统一读取入口也用于联机定义，协议不能绕过本地数学校验。
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
    if ( curveValue < 0 || curveValue > static_cast<int>(TimingCurve::Custom) )
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
    // 轴名称不依赖枚举整数布局，旧文件缺省为时间。
    // 分拍逐项检查整数范围，禁止强制窄化超大 JSON 数值。
    // 拍长服务函数编译，完整载入后再按红线准备。
    // 本入口没有完整时间线，不能反解绝对秒数。
    // 联机文档复用本入口，远端不能绕过分拍校验。
    // 源式按当前轴编译，拍域不会误用秒域定义域。
    // 内部 BPM 片段和积分节点不属于文件协议。
    // 失败不构造半合法函数或带未知轴的可编辑段落。
    // 不接受未知轴名或隐式转换，旧文件没有轴字段则仍按时间计算。
    if ( auto axis = json.find("variable"); axis != json.end() ) {
        if ( !axis->is_string() ) return std::nullopt;
        if ( *axis == "beat" ) {
            interpolation.m_variable = TimingVariable::Beat;
            auto step                = json.find("beat_step");
            auto span                = json.find("beat_duration");
            if ( step == json.end() || !step->is_array() || step->size() != 2 ||
                 span == json.end() || !span->is_number() )
                return std::nullopt;
            for ( std::size_t i = 0; i < 2; ++i ) {
                if ( !(*step)[i].is_number_integer() ) return std::nullopt;
                const double value = (*step)[i].get<double>();
                if ( value < 1 || value > 65536 ) return std::nullopt;
            }
            interpolation.m_beatNumerator   = (*step)[0].get<int>();
            interpolation.m_beatDenominator = (*step)[1].get<int>();
            interpolation.m_beatDuration    = span->get<double>();
        } else if ( *axis != "time" )
            return std::nullopt;
    }
    double startValue = 1.0;
    if ( interpolation.m_curve == TimingCurve::Custom ) {
        const auto expression = json.find("function");
        if ( expression == json.end() || !expression->is_string() )
            return std::nullopt;
        auto compiled = compileTimingFunction(
            expression->get_ref<const std::string&>(),
            timingInterpolationVariableDuration(interpolation));
        if ( !compiled ) return std::nullopt;
        interpolation.m_function = std::move(*compiled);
        startValue = evaluateTimingFunction(*interpolation.m_function, 0);
    }
    // 与类型无关的校验在此完成，所属时间点随后补充 BPM 起终边界。
    if ( !isValidTimingInterpolation(
             interpolation, TimingEffect::SCROLL, startValue) )
        return std::nullopt;
    return interpolation;
}
/// @brief 定义语义比较，独立编译的同一函数仍属于同一个段落值。
// 缓存地址不属于用户编辑语义，不比较编译准备过程。
// 独立编译的同一源式不能产生虚假撤销记录。
// 时间模式无关的分拍字段不保存，比较时同样忽略。
// 拍模式比较分拍和定义域，真实轴配置变化仍可撤销。
// 形状和源式保持原比较契约，不依赖实体身份。
// 比较函数只借用缓存，不复制共享所有权。
bool TimingInterpolation::operator==(const TimingInterpolation& other) const
{
    // 缓存地址和积分准备过程不属于编辑语义，不产生虚假的撤销步骤。
    const bool sameFunction =
        m_curve != TimingCurve::Custom ||
        (m_function && other.m_function &&
         timingFunctionExpression(*m_function) ==
             timingFunctionExpression(*other.m_function)) ||
        (!m_function && !other.m_function);
    return m_duration == other.m_duration && m_endValue == other.m_endValue &&
           m_samplesPerSecond == other.m_samplesPerSecond &&
           m_variable == other.m_variable &&
           (m_variable == TimingVariable::Time ||
            (m_beatNumerator == other.m_beatNumerator &&
             m_beatDenominator == other.m_beatDenominator &&
             m_beatDuration == other.m_beatDuration)) &&
           m_curve == other.m_curve && m_controlX1 == other.m_controlX1 &&
           m_controlY1 == other.m_controlY1 &&
           m_controlX2 == other.m_controlX2 &&
           m_controlY2 == other.m_controlY2 && sameFunction;
}
/// @brief 整段编译成功才更新工作副本，避免错误文本破坏已有函数。
/// @param interpolation 编辑中的定义，成功后转成 Custom。
/// @param expression 可保存的完整数学表达式。
/// @param startValue 成功后同步为 f(0)，失败不覆盖旧起值。
/// @param error 成功清空，失败为编译器的具体中文说明。
/// @return 整个定义域合法且缓存完成时返回真。
/// 编译先在独立对象中完成，错误不会发布半更新的状态。
/// 终值始终取 f(duration)，避免与程序语义不一致。
/// 该入口只处理数学合法性，BPM 全域限制由公共校验器补充。
/// 所有权只在成功时转移，撤销栈可继续持有旧函数对象。
bool setTimingInterpolationFunction(TimingInterpolation& interpolation,
                                    std::string_view     expression,
                                    double& startValue, std::string& error)
{
    auto compiled = compileTimingFunction(
        expression, timingInterpolationVariableDuration(interpolation));
    if ( !compiled ) {
        error = compiled.error();
        return false;
    }
    // 起终值由当前自变量域的表达式推导，不要求用户重复填写相同信息。
    interpolation.m_function = std::move(*compiled);
    interpolation.m_curve    = TimingCurve::Custom;
    startValue = evaluateTimingFunction(*interpolation.m_function, 0);
    interpolation.m_endValue = evaluateTimingFunction(
        *interpolation.m_function,
        timingInterpolationVariableDuration(interpolation));
    error.clear();
    return true;
}
/// @brief 非单调自定义函数使用编译时的区间包围，预设沿用端值范围。
/// @param interpolation 预设或自定义曲线定义。
/// @param startValue 普通预设的起值，自定义对象已拥有真实首值。
/// @return 覆盖整个段内的参数范围。
/// 预设在单位区间中不越过端值，所以端值即可决定包围。
/// 自定义函数可有内部峰谷，必须借用完整域证明的包围。
/// 范围同时用于 UI 图形纵轴与 BPM 参数校验。
/// 不扫描导出事件，函数身份与实际曲线不会随采样率变化。
std::pair<double, double> timingInterpolationRange(
    const TimingInterpolation& interpolation, double startValue)
{
    if ( interpolation.m_curve == TimingCurve::Custom &&
         interpolation.m_function )
        return timingFunctionRange(*interpolation.m_function);
    return std::minmax(startValue, interpolation.m_endValue);
}
}  // namespace MMM
