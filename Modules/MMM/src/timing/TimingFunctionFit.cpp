#include "mmm/timing/TimingFunctionFit.h"

#include "mmm/timing/TimingFunction.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <numbers>

namespace MMM
{
namespace
{
/// @brief 最多六次多项式或三对谐波，所有求解存储均有固定上限。
constexpr std::size_t MAX_COLUMNS = 7;
/// @brief 限制输入工作量，也覆盖画布的 129 个绘制格点。
constexpr std::size_t MAX_POINTS = 256;
/// @brief 拟合函数族；各列是这个函数族的线性组合基底。
enum class Family {
    Polynomial,
    Fourier,
    Exponential,
    Logarithm,
    SquareRoot,
    CubeRoot
};
/// @brief 在归一时间计算基函数，改善长短段落的数值条件。
/// @param family 当前比较的初等函数族。
/// @param column 常数列为零，其余列由函数族决定。
/// @param x 实际时间除以段落时长，取值在闭区间 0–1。
/// @return 当前基函数值，不包含待求解的系数。
/// @note 多项式使用小次数，Fourier 使用最多三对周期项。
/// 指数和对数固定内部尺度，拟合只解线性组合系数。
/// 固定尺度避免把低频按钮变成无界非线性搜索。
/// 所有族都有常数列，因此也可拟合带绝对偏移的参数。
/// 生成函数时显式写出 t/duration，公开 t 不改成归一变量。
double basis(Family family, std::size_t column, double x)
{
    if ( column == 0 ) return 1;
    switch ( family ) {
    case Family::Polynomial: return std::pow(x, static_cast<int>(column));
    case Family::Fourier: {
        const double angle = 2 * std::numbers::pi * x * ((column + 1) / 2);
        return column % 2 ? std::sin(angle) : std::cos(angle);
    }
    case Family::Exponential: return std::expm1(4 * x);
    case Family::Logarithm: return std::log1p(9 * x);
    case Family::SquareRoot: return std::sqrt(x);
    case Family::CubeRoot: return std::cbrt(x);
    }
    return 0;
}
/// @brief 可维护的族名称，不把内部枚举写入谱面格式。
/// @param family 候选表达式采用的基函数族。
/// @return 用于误差预览的稳定文字，不作为机器格式标识。
/// 拟合家族只描述生成方式，不声称表达式只有该一种表示。
/// 实际保存的是函数式，重开不需要再运行拟合器。
/// 名称独立于内部枚举排列，后续新增候选不影响历史谱面。
/// 名称只用于候选解释，不决定拟合优先级。
/// 简化家族与复杂家族都需要通过同一全域数学编译。
/// 不依赖用户系统语言或区域设置产生函数名。
/// 拟合选择结果随数据误差改变，名称不能用作持久化缓存键。
const char* familyName(Family family)
{
    switch ( family ) {
    case Family::Polynomial: return "多项式复合";
    case Family::Fourier: return "正弦与余弦复合";
    case Family::Exponential: return "指数复合";
    case Family::Logarithm: return "对数复合";
    case Family::SquareRoot: return "平方根复合";
    case Family::CubeRoot: return "立方根复合";
    }
    return "";
}
/// @brief 对拟合设计矩阵执行两遍改进 Gram–Schmidt QR。
/// @details 列数很少但多项式相关，重正交化减少系数不稳定。
/// @return 满秩且系数有限时返回真，退化候选直接跳过。
/// @param points 已校验的严格递增秒域样本。
/// @param duration 正有限段落时长，用于基函数归一化。
/// @param family 当前候选族。
/// @param columns 当前族参与求解的列数，不超过固定上限。
/// @param coefficients 成功时输出有限系数，失败时调用方丢弃候选。
/// @pre 点数至少八且不超过 256，列数不超过七。
/// 矩阵按列存放，正交化只遍历有效样本而非整个容量。
/// 两遍改进 Gram–Schmidt 比直接形成正规方程更能抵抗列相关。
/// 绝对秩阈值保护近乎退化的列，不通过巨大系数伪装拟合成功。
/// QR 的正交列投影样本值后，只需要一次三角回代。
/// 算法不假定用户曲线单调，负 Scroll 也可参与求解。
/// 所有工作容器有固定上限，候选失败不需要异常恢复。
bool solve(std::span<const TimingCurvePoint> points, double duration,
           Family family, std::size_t columns,
           std::array<double, MAX_COLUMNS>& coefficients)
{
    std::array<std::array<double, MAX_POINTS>, MAX_COLUMNS>  q{};
    std::array<std::array<double, MAX_COLUMNS>, MAX_COLUMNS> r{};
    std::array<double, MAX_COLUMNS>                          projection{};
    for ( std::size_t column = 0; column < columns; ++column ) {
        for ( std::size_t row = 0; row < points.size(); ++row )
            q[column][row] =
                basis(family, column, points[row].m_time / duration);
        // 第二遍正交化补偿浮点误差；不通过 AᵀA 再次平方条件数。
        for ( int pass = 0; pass < 2; ++pass )
            for ( std::size_t previous = 0; previous < column; ++previous ) {
                double dot = 0;
                for ( std::size_t row = 0; row < points.size(); ++row )
                    dot += q[previous][row] * q[column][row];
                r[previous][column] += dot;
                for ( std::size_t row = 0; row < points.size(); ++row )
                    q[column][row] -= dot * q[previous][row];
            }
        double norm = 0;
        for ( std::size_t row = 0; row < points.size(); ++row )
            norm = std::hypot(norm, q[column][row]);
        // 秩不足时不放大接近零的列，后续简单函数族仍可继续比较。
        if ( norm < 1e-10 || !std::isfinite(norm) ) return false;
        r[column][column] = norm;
        for ( std::size_t row = 0; row < points.size(); ++row ) {
            q[column][row] /= norm;
            projection[column] += q[column][row] * points[row].m_value;
        }
    }
    // 上三角回代只访问已经求出的系数，避免完整矩阵求逆。
    for ( std::size_t remaining = columns; remaining > 0; --remaining ) {
        const auto column = remaining - 1;
        double     value  = projection[column];
        for ( std::size_t next = column + 1; next < columns; ++next )
            value -= r[column][next] * coefficients[next];
        coefficients[column] = value / r[column][column];
        if ( !std::isfinite(coefficients[column]) ) return false;
    }
    return true;
}
/// @brief 输出秒域表达式，系数保留足够有效位以供重新编译。
/// @param family 已通过求解和误差比较的函数族。
/// @param columns 有效系数数量，与求解矩阵一致。
/// @param c 双精度系数，至少保存十七位有效数字。
/// @param duration 实际秒域长度，不隐式存入宿主变量。
/// @return 能由受限数学编译器重新读取的完整表达式。
/// 多项式采用 Horner 结构，避免重复计算多个高次幂。
/// 其他族使用加权初等函数组合，与 basis 保持一一对应。
/// 傅里叶项保留 π 与整数谐波，不预先量化成采样点。
/// 所有系数都带括号，负系数和指数不依赖字符串拼接优先级。
/// 结果稍后重新编译，不允许不可保存的求解器私有函数逃逸。
/// 生成失败仅影响候选，不修改原始绘制点或当前谱面。
std::string expression(Family family, std::size_t columns,
                       const std::array<double, MAX_COLUMNS>& c,
                       double                                 duration)
{
    const auto x = fmt::format("(t/{:.17g})", duration);
    if ( family == Family::Polynomial ) {
        // Horner 形式减少重复幂运算，同时缩短大次数函数的保存文本。
        std::string value = fmt::format("({:.17g})", c[columns - 1]);
        for ( std::size_t n = columns - 1; n > 0; --n )
            value = fmt::format("({:.17g}+{}*{})", c[n - 1], x, value);
        return value;
    }
    std::string value = fmt::format("({:.17g})", c[0]);
    for ( std::size_t column = 1; column < columns; ++column ) {
        std::string term;
        // 归一化仅存在于公式中的显式 t/duration，公开 t 仍是实际秒。
        switch ( family ) {
        case Family::Fourier:
            term = fmt::format("{}(2*pi*{}*{})",
                               column % 2 ? "sin" : "cos",
                               (column + 1) / 2,
                               x);
            break;
        case Family::Exponential: term = fmt::format("(exp(4*{})-1)", x); break;
        case Family::Logarithm: term = fmt::format("log(1+9*{})", x); break;
        case Family::SquareRoot: term = fmt::format("sqrt({})", x); break;
        case Family::CubeRoot: term = fmt::format("cbrt({})", x); break;
        default: break;
        }
        value += fmt::format("+({:.17g})*{}", c[column], term);
    }
    return value;
}
}  // namespace

/// @brief 校验绘制点后比较有效候选，误差接近时优先简单表达式。
/// @warning 用户点击拟合的低频路径；不读注册表、不发布中间谱面修改。
/// @param points 覆盖完整段落的实际秒域采样。
/// @param duration 当前编辑定义域长度。
/// @return 最优有效候选及 RMS、最大绝对误差，或输入错误说明。
/// 调用方必须主动触发拟合，不能在连续鼠标事件中重复求解。
/// 候选包括不同次数的多项式和正弦余弦组合。
/// 指数、对数、平方根与立方根各有常数与形状两列。
/// 误差在实际参数值中计算，显示数字可直接与纵轴比较。
/// 排名误差按数据跨度归一化，以便比较不同量纲的段落。
/// 列数惩罚在误差接近时优先简单函数，而非永远选最高阶。
/// 复杂度惩罚不改变对外报告的真实 RMS 或最大误差。
/// 编译候选验证整个定义域，不把样本点通过视为全域合法。
/// 该函数不检查 BPM 专属上下限，领域效果由调用方再验证。
/// 没有保证任意手绘曲线都能精确表达，误差必须保留供用户判断。
/// 不输出未约束自由递归函数，也不执行用户脚本。
std::expected<TimingFunctionFit, std::string> fitTimingFunction(
    std::span<const TimingCurvePoint> points, double duration)
{
    if ( !std::isfinite(duration) || duration < .001 || points.size() < 8 ||
         points.size() > MAX_POINTS )
        return std::unexpected(
            "拟合需要至少 8 个绘制点，且段落时长至少为 1 ms。");
    if ( std::abs(points.front().m_time) > 1e-9 ||
         std::abs(points.back().m_time - duration) > 1e-9 )
        return std::unexpected("请从段落起点画到终点，覆盖整个范围。");
    double minimum = points.front().m_value, maximum = minimum, previous = -1;
    for ( const auto& point : points ) {
        // 不接受倒序、重复或域外点，QR 输入不能夹带 NaN 或无穷。
        if ( !std::isfinite(point.m_time) || !std::isfinite(point.m_value) ||
             point.m_time <= previous || point.m_time < 0 ||
             point.m_time > duration )
            return std::unexpected("绘制点须有限且按时间严格递增。");
        previous = point.m_time;
        minimum  = std::min(minimum, point.m_value);
        maximum  = std::max(maximum, point.m_value);
    }
    // 常值或极小振幅使用至少一的误差尺度，避免除以零。
    // 输入检查完成才建立候选，非有限样本不能进入矩阵投影。
    // 所有家族共享同一误差定义，比较才具有实际含义。
    const double      scale     = std::max(1.0, maximum - minimum);
    double            bestScore = std::numeric_limits<double>::infinity();
    TimingFunctionFit best;
    for ( auto family : { Family::Polynomial,
                          Family::Fourier,
                          Family::Exponential,
                          Family::Logarithm,
                          Family::SquareRoot,
                          Family::CubeRoot } ) {
        const std::size_t maximumColumns =
            (family == Family::Polynomial || family == Family::Fourier)
                ? MAX_COLUMNS
                : 2;
        // 较低次数先参与比较，复杂度惩罚只作为很小的平局偏好。
        // Fourier 列必须组成完整正弦/余弦对，不能留下单边最高频项。
        // 每个候选使用新的系数数组，退化结果不会污染下一个解。
        for ( std::size_t columns = 2; columns <= maximumColumns; ++columns ) {
            if ( family == Family::Fourier && columns % 2 == 0 ) continue;
            std::array<double, MAX_COLUMNS> coefficients{};
            if ( !solve(points, duration, family, columns, coefficients) )
                continue;
            double squared = 0, maxError = 0;
            for ( const auto& point : points ) {
                double predicted = 0;
                for ( std::size_t column = 0; column < columns; ++column )
                    predicted += coefficients[column] *
                                 basis(family, column, point.m_time / duration);
                const double error = std::abs(predicted - point.m_value);
                squared += error * error;
                maxError = std::max(maxError, error);
            }
            const double rms = std::sqrt(squared / points.size());
            // 复杂度惩罚避免手绘噪声被高阶多项式过拟合，改善后续人工编辑。
            const double score = rms / scale + columns * 1e-4;
            if ( score >= bestScore ) continue;
            auto source = expression(family, columns, coefficients, duration);
            // 求解成功不代表定义域合法，使用同一生产编译器验证结果。
            if ( !compileTimingFunction(source, duration) ) continue;
            bestScore = score;
            best = { std::move(source), familyName(family), rms, maxError };
        }
    }
    if ( !std::isfinite(bestScore) )
        return std::unexpected(
            "没有得到有限且定义域合法的拟合函数，请调整绘制范围。");
    return best;
}
}  // namespace MMM
