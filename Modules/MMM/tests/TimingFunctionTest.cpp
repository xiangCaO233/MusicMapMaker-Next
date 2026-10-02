#include "mmm/timing/TimingFunction.h"

#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "mmm/timing/Timing.h"
#include "mmm/timing/TimingFunctionFit.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <numbers>
#include <string>

namespace
{
/// @brief 发布构建仍执行断言，日志明确说明失败的数学或持久化契约。
/// @param condition 与实现独立的数学或持久化断言。
/// @param message 明确标识失败契约的短文字。
/// @return 原条件值，允许调用方累积多个失败。
/// 测试不用标准 assert，避免发布配置移除验证。
/// 失败经项目日志宏报告，不依赖标准输出或异常。
bool check(bool condition, const char* message)
{
    if ( !condition ) XERROR("TimingFunctionTest: {}", message);
    return condition;
}
/// @brief 数学检查使用绝对误差，不混入外部格式的毫秒量化误差。
/// @param a 被测计算结果。
/// @param b 独立参考答案。
/// @param tolerance 当前用例允许的绝对误差。
/// @return 两个值的差小于该误差时为真。
/// 非有限结果不能靠比较偶然通过。
/// 解析原函数用例不从相同积分缓存生成参考值。
bool near(double a, double b, double tolerance = 1e-6)
{
    return std::abs(a - b) < tolerance;
}
/// @brief 解析器须遵守数学优先级，并支持正式数学符号输入。
/// @details 负号、右结合的幂和函数参数由独立样本验证。
/// 验证数学树包含结构节点，而不是仅仅替换字符串后显示文本。
/// @return 所有优先级、Unicode 和非法输入用例均符合契约时为真。
/// 幂采用右结合，负号低于幂，而负指数仍被接受。
/// 正式符号与 ASCII 别名必须计算出同一答案。
/// 科学计数法需完整指数，不能把损坏指数当成两个表达式。
/// 测试同时检查数学树类型，避免界面只显示普通替换文本。
/// 语法白名单不接受宿主脚本入口或赋值语句。
/// 未知函数不能被当成未初始化变量继续运行。
/// 表达式长度与括号深度分别验证，不能相互抵扣。
/// 固定失败样本不依赖系统 locale 或不同平台的数字扩展。
/// 每个失败以返回值处理，不测试异常恢复。
bool testGrammar()
{
    bool ok = true;
    // 各样本独立编译，避免某个表达式失败后解引用空结果。
    // 常数表达式同样要建立合法的秒域积分缓存。
    for ( auto [source, expected] : { std::pair{ "-2^2", -4.0 },
                                      { "2^-2", .25 },
                                      { "2^3^2", 512.0 },
                                      { "1.25e2+5", 130.0 },
                                      { "sin(π/2)+√(9)+∛(8)", 6.0 },
                                      { "2² × 3 − 4 ÷ 2", 10.0 },
                                      { "min(3,4)+max(2,5)", 8.0 } } ) {
        const auto function = MMM::compileTimingFunction(source, 2);
        ok &=
            check(function && near(MMM::evaluateTimingFunction(**function, .5),
                                   expected),
                  source);
    }
    // 幂和分式必须是不同排版结构，π 不可退回成普通 ASCII 标识符。
    // 先确认整体表达式成功，后续检查才借用其树视图。
    // 结构存在与数值正确分别验证，排版不能改变数学答案。
    // π 节点检查正式名称，避免内部常量被显示成截断十进制。
    const auto math = MMM::compileTimingFunction("π/(1+t^2)+sqrt(t)", 2);
    if ( !math ) return check(false, "formula compile");
    const auto nodes = MMM::timingFunctionMathNodes(**math);
    for ( auto kind : { MMM::TimingMathKind::Fraction,
                        MMM::TimingMathKind::Power,
                        MMM::TimingMathKind::Root } )
        ok &= check(
            std::any_of(nodes.begin(),
                        nodes.end(),
                        [kind](const auto& n) { return n.m_kind == kind; }),
            "structured math node");
    ok &= check(std::any_of(nodes.begin(),
                            nodes.end(),
                            [](const auto& n) { return n.m_text == "π"; }),
                "Unicode pi");
    // 未知名称、赋值及平台特有数字扩展均不属于数学表达式语言。
    // 这些输入必须通过返回值失败，不能执行宿主脚本或依赖异常恢复。
    for ( const char* source : { "t=1",
                                 "os.execute(1)",
                                 "unknown(t)",
                                 "0x10",
                                 "nan",
                                 "1e",
                                 "sqrt(-1)",
                                 "sin(1,2)",
                                 "t+" } )
        ok &= check(!MMM::compileTimingFunction(source, 2), source);
    const std::string nested =
        std::string(40, '(') + "t" + std::string(40, ')');
    ok &= check(!MMM::compileTimingFunction(nested, 2), "nesting budget");
    ok &= check(!MMM::compileTimingFunction(std::string(2050, '1'), 2),
                "source budget");
    return ok;
}
/// @note 聚合数量测试故意把全部体放在上限，覆盖入口计数后的容器增长。
/// @brief 聚合表达式使用独立数学答案检查，局部变量不能逃逸或覆盖时间 t。
/// @details 上下限和体程序分别编译；同时覆盖 Unicode 幂、整数域和嵌套作用域。
/// @return 所有聚合计算、绑定及预算样本均通过时为真。
/// 示例答案对应符号面板的 Tooltip 模板。
/// 求和上限不包括非整数索引，实际整数通过 ceil/floor 选择。
/// 反向积分保持负方向，空整数范围保留单位元。
/// 内层同名绑定遮蔽外层，体结束后恢复外层值。
/// 积分局部变量与实际时间 t 不得互相覆盖。
/// 逐项整数域验证须允许 (-1)^k，不能误用连续实数指数规则。
/// 时间相关上下限额外覆盖聚合结果的不连续边界。
/// 上下限树与体树都需保存在统一数学节点数组中。
/// 预算失败必须发生在编译时，不能等待运行到长循环后失败。
/// 定积分内部奇点的样本位置避开普通规则网格。
/// 聚合原始文本还覆盖序列化与变速中的局部变量保持。
/// 这些测试不依赖 UI 显示成功，数值答案均从数学定义给出。
bool testAggregates()
{
    bool ok = true;
    // 有限求和与累乘按离散整数求值，空范围采用各自的单位元。
    for ( auto [source, expected] :
          { std::pair{ "sum(k,1,10,k²)", 385.0 },
            { "prod(k,1,5,k)", 120.0 },
            { "sum(k,4,2,k)", 0.0 },
            { "prod(k,4,2,k)", 1.0 },
            { "sum(k,1,5,(-1)^k)", -1.0 },
            { "sum(k,1.2,4.8,k)", 9.0 },
            { "sum(k,1,3,sum(k,1,2,k))", 9.0 },
            { "sum(k,1,3,k*t)", 6.0 },
            { "int(x,0,t,x²)", 1.0 / 3 },
            { "int(x,t,0,x²)", -1.0 / 3 },
            { "int(x,0,t,sin(x))", 1 - std::cos(1.0) },
            { "integral(x,0,t,exp(x))", std::exp(1.0) - 1 } } ) {
        const auto function = MMM::compileTimingFunction(source, 2);
        if ( !function ) {
            XERROR("{}: {}", source, function.error());
            ok = false;
            continue;
        }
        ok &= check(near(MMM::evaluateTimingFunction(**function, 1), expected),
                    source);
    }
    // 内层变量与外层变量名字相同时应遮蔽，退出内层后还原原作用域。
    // 序列化只保留表达式，聚合局部槽和积分缓存都应能重新构建。
    MMM::TimingInterpolation aggregateCurve;
    aggregateCurve.m_duration  = 2;
    double      aggregateStart = 0;
    std::string aggregateError;
    ok &= check(
        MMM::setTimingInterpolationFunction(aggregateCurve,
                                            "120+int(x,0,t,x²)+sum(k,1,3,k*t)",
                                            aggregateStart,
                                            aggregateError),
        "aggregate source definition");
    // 文件只保存文本与时长，重新读取会创建不同的资源对象。
    // 相等性必须描述段落语义，不能要求共享指针地址一致。
    // 聚合体索引是实现细节，不出现在 JSON 契约里。
    const nlohmann::json aggregateJson = aggregateCurve;
    const auto aggregateLoaded = MMM::readTimingInterpolation(aggregateJson);
    ok &= check(aggregateLoaded && *aggregateLoaded == aggregateCurve,
                "aggregate JSON roundtrip");
    if ( aggregateCurve.m_function ) {
        const auto speedSource = MMM::rescaleTimingFunctionExpression(
            *aggregateCurve.m_function, 2, 2);
        const auto speed = MMM::compileTimingFunction(speedSource, 1);
        ok &= check(speed && near(MMM::evaluateTimingFunction(**speed, .5),
                                  2 * (120 + 1.0 / 3 + 6)),
                    "aggregate scaling preserves local variable");
    }
    const auto scoped =
        MMM::compileTimingFunction("sum(k,1,3,k+sum(k,1,2,k)+k)", 2);
    ok &= check(scoped && near(MMM::evaluateTimingFunction(**scoped, 1), 21),
                "nested scope restored");
    // upper=floor(t) 在整数时间边界改变项数。
    // 分别检查空集合、一项和两项，不能仅检查整段末值。
    // 这种离散操作合法，但累计积分缓存仍需稳定处理跳变。
    const auto changing = MMM::compileTimingFunction("sum(k,1,floor(t),k)", 2);
    ok &= check(changing &&
                    near(MMM::evaluateTimingFunction(**changing, .5), 0) &&
                    near(MMM::evaluateTimingFunction(**changing, 1.5), 1) &&
                    near(MMM::evaluateTimingFunction(**changing, 2), 3),
                "time dependent sum limit");
    // 三种聚合的树均须保留第三个子树，渲染器才能绘制上下限和体表达式。
    for ( auto [source, kind] :
          { std::pair{ "int(x,0,t,x²)", MMM::TimingMathKind::Integral },
            { "sum(k,1,10,k²)", MMM::TimingMathKind::Sum },
            { "prod(k,1,5,k)", MMM::TimingMathKind::Product } } ) {
        const auto function = MMM::compileTimingFunction(source, 2);
        if ( !function ) {
            ok = false;
            continue;
        }
        const auto nodes = MMM::timingFunctionMathNodes(**function);
        ok &= check(nodes.back().m_kind == kind && nodes.back().m_third >= 0,
                    "aggregate math structure");
    }
    // 不允许未绑定变量、保留名称和超预算范围，避免热路径执行无限循环。
    // 积分体内部奇点同样属于定义域，不能因采样没有碰到就通过。
    for ( const char* source : { "sum(k,1,10,k)+k",
                                 "sum(t,1,10,t)",
                                 "prod(k,1,257,k)",
                                 "sum(k,-1e12,-1e12,k)",
                                 "sum(k,1,4)",
                                 "int(x,0,t,1/(x-.1234567))",
                                 "sum(k,1,256,sum(j,1,256,j+k))" } )
        ok &= check(!MMM::compileTimingFunction(source, 2), source);
    // 上下限本身可创建聚合体，外层取体指针前必须再次检查预留容量。
    // 十六个同层体都合法，第十七个外层体不能触发重分配使旧体指针悬空。
    std::string many = "1";
    for ( int i = 0; i < 16; ++i ) many += "+sum(j,1,1,j)";
    ok &= check(!MMM::compileTimingFunction("sum(k,1," + many + ",k)", 2),
                "aggregate bounds capacity budget");
    return ok;
}
/// @brief 完整闭区间验证不能被粗输出采样绕过。
/// @details 奇点特意放在不规则时刻，端值均有限仍应拒绝。
/// 正负参数范围和根式端点同时覆盖，防止区间运算漏掉内部极值。
/// @return 全部域拒绝和缓存积分精度样本均通过时为真。
/// 可见首尾合法不能作为完整区间合法的证据。
/// 奇次幂跨零包围需包含负值，不能沿用偶次幂下界。
/// 三角极点、负底数分数幂和指数溢出各自有独立拒绝样本。
/// 积分精度使用多个非网格时刻，检验局部累计插值而非只检验总量。
/// 根式端点导数不连续仍有合法函数值，不应一概拒绝。
/// 正负段外时间检查端值延伸，避免缓存把范围外直接截为零。
/// BPM 中途变负需拒绝，即使原函数两端完全相同。
/// 其他 Timing 效果允许负参数，不能误套 BPM 专属限制。
/// 测试不通过导出 Hz 调整缓存精度。
/// 时长两秒同时区分真实秒域与归一进度。
bool testDomainAndIntegral()
{
    bool ok = true;
    // 除零、对数端点和 tan 内部极点均在真实时间区间中检查。
    // 验证不传入 Hz，说明合法性与导出采样率没有隐藏关联。
    for ( const char* source : { "1/(t-.123456789)",
                                 "log(t)",
                                 "tan(t)",
                                 "exp(1000+t)",
                                 "pow(-2,t)" } )
        ok &= check(!MMM::compileTimingFunction(source, 2), source);
    const auto cubic = MMM::compileTimingFunction("(t-1)^3", 2);
    if ( !cubic ) return check(false, "signed integer power");
    const auto [low, high] = MMM::timingFunctionRange(**cubic);
    // 奇次幂跨零时仍保留负下界，不能按偶次幂规则误判成非负。
    ok &= check(low <= -1 && high >= 1, "odd-power interval bounds");
    for ( const char* source : { "120+30*t+2*t^2",
                                 "120+20*sin(pi*t)",
                                 "120+sqrt(t)",
                                 "120+cbrt(t)" } ) {
        const auto function = MMM::compileTimingFunction(source, 2);
        if ( !function ) {
            XERROR("{}: {}", source, function.error());
            ok = false;
            continue;
        }
        // 对应的参考原函数由数学公式给出，不从同一数值缓存推导。
        for ( double time : { .0001, .137, .75, 1.9, 2.0 } ) {
            double expected;
            if ( std::string(source).find("2*t^2") != std::string::npos )
                expected =
                    120 * time + 15 * time * time + 2 * time * time * time / 3;
            else if ( std::string(source).find("sin") != std::string::npos )
                expected =
                    120 * time + 20 * (1 - std::cos(std::numbers::pi * time)) /
                                     std::numbers::pi;
            else if ( std::string(source).find("sqrt") != std::string::npos )
                expected = 120 * time + 2 * std::pow(time, 1.5) / 3;
            else
                expected = 120 * time + 3 * std::pow(time, 4.0 / 3) / 4;
            ok &= check(
                near(MMM::integrateTimingFunction(**function, time), expected),
                "cached integral accuracy");
        }
        // 段外按相应端值外推，不能重新开始曲线或截掉负拍位。
        ok &= check(near(MMM::integrateTimingFunction(**function, -1), -120),
                    "integral before segment");
        ok &= check(near(MMM::integrateTimingFunction(**function, 3) -
                             MMM::integrateTimingFunction(**function, 2),
                         MMM::evaluateTimingFunction(**function, 2)),
                    "integral tail");
    }
    MMM::TimingInterpolation curve;
    curve.m_duration  = 2;
    double      start = 120;
    std::string error;
    // 首尾都是 120 的曲线中途变负，BPM 必须拒绝；其他参数保留有符号语义。
    ok &= check(MMM::setTimingInterpolationFunction(
                    curve, "120-200*sin(pi*t/2)", start, error),
                "finite negative-interior function");
    ok &= check(
        !MMM::isValidTimingInterpolation(curve, MMM::TimingEffect::BPM, start),
        "BPM whole-domain bound");
    ok &= check(MMM::isValidTimingInterpolation(
                    curve, MMM::TimingEffect::SCROLL, start),
                "signed Scroll domain");
    return ok;
}
/// @brief 定义保留、采样和变速必须都使用实际秒域的同一函数。
/// @details 原生文件保留单段，JSON 不持久化执行字节码。
/// 更新密度只影响外部事件数量，不改变表达式或中间值。
/// @param output CTest 传入的构建树临时输出目录。
/// @return 端点、相等性、JSON、原生保存、采样及变速均通过时为真。
/// 测试不写 tests/data，也不修改用户的预设资源。
/// 首尾值由绝对函数计算，终点不是对已有参数做相对插值。
/// 重新编译得到不同共享地址，语义相同仍应比较相等。
/// 修改时长但复用旧缓存必须失效，不能偷偷继续执行原域。
/// 原生格式只保留一条定义，不展开成若干不可编辑事件。
/// 导出采样则按指定密度生成普通点，参数取实际秒位置。
/// 输入文件通过项目真实保存与加载入口往返。
/// 变速用独立副本，测试最后再次核对源谱面没有变化。
/// BPM 变速需要同时缩放时间自变量和函数输出。
/// 最终参考值按原多项式独立计算，不沿用变换表达式执行器。
/// 跨平台端点容许少量 libm 末位舍入，但不容许实质篡改。
/// 浮点扰动用 nextafter 构造，避免十进制常量产生不同预期。
/// 持久化的源文本保持用户输入符号，不转换成含缓存地址的表示。
/// 整个对象可拥有独立重编译资源，但相同定义不产生虚假撤销步骤。
/// 样本点偏移使用外部锚点，局部秒域和绝对谱面时刻分开核对。
/// 变速副本的密度保持总输出数量，不随速度改变曲率定义。
/// 测试只检查当前任务的函数定义，不要求已有音符内容发生变化。
/// 单个临时目录只属于该 CTest 用例，不与其他并行测试共享写入。
/// 修改函数时长后重新编译属于显式操作，校验器不能隐式补救。
/// 所有保存与读取结果都检查返回值，失败文件不能继续使用。
/// 参数范围、数学语法和持久化稳定性分别有独立失败信息。
/// 原生回环与 JSON 回环共同覆盖文件入口及定义入口。
/// 输出采样测试是数学秒域检查，不声称非原生格式可保留原曲线。
/// 最后的源值检查确保变速操作没有通过共享可变缓存改动来源。
bool testPersistence(const std::filesystem::path& output)
{
    MMM::TimingInterpolation curve;
    curve.m_duration         = 2;
    curve.m_samplesPerSecond = 2;
    double      start        = 0;
    std::string error;
    if ( !MMM::setTimingInterpolationFunction(
             curve, "120+30*t+2*t²", start, error) )
        return check(false, "custom initialization");
    bool ok     = check(near(start, 120) && near(curve.m_endValue, 188),
                        "absolute endpoints from f(t)");
    auto second = curve;
    // 独立编译产生不同对象，但定义语义相同不能制造虚假更新命令。
    MMM::setTimingInterpolationFunction(second, "120+30*t+2*t²", start, error);
    ok &= check(second == curve, "semantic equality ignores cache identity");
    const nlohmann::json json   = curve;
    auto                 loaded = MMM::readTimingInterpolation(json);
    ok &= check(loaded && *loaded == curve && json.contains("function"),
                "JSON source roundtrip");
    // 跨平台 libm 的末位差异可接受，修改真实终值仍必须拒绝。
    auto roundedJson         = json;
    roundedJson["end_value"] = std::nextafter(curve.m_endValue, 1000.0);
    ok &= check(MMM::readTimingInterpolation(roundedJson).has_value(),
                "cross-platform endpoint rounding");
    auto invalidJson         = json;
    invalidJson["end_value"] = curve.m_endValue + 1;
    ok &= check(!MMM::readTimingInterpolation(invalidJson),
                "reject contradictory endpoint");
    // 修改时长而不重新编译必须失效，缓存不能跨定义域复用。
    second.m_duration = 3;
    ok &= check(
        !MMM::isValidTimingInterpolation(second, MMM::TimingEffect::BPM, start),
        "duration invalidates cache");
    MMM::Timing timing;
    timing.m_timestamp             = 1000;
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = timing.m_bpm = start;
    timing.m_beat_length                          = 60000 / start;
    timing.m_interpolation                        = curve;
    // 两秒、每秒两次切分应该输出含两端的五点。
    // 第二个输出时刻为 0.5 秒，绝对参数答案按原多项式给出。
    // 尾点时间包含外部 1 秒锚点，不能把段落时长当成绝对终点。
    const auto samples = MMM::sampleTimingInterpolations({ timing });
    ok &= check(samples.size() == 5 &&
                    near(samples[1].m_timingEffectParameter, 135.5) &&
                    near(samples.back().m_timestamp, 3000),
                "density samples actual seconds");
    MMM::BeatMap map;
    map.m_baseMapMetadata.preference_bpm = 120;
    map.m_timings.push_back(timing);
    map.sync();
    // 临时目录由测试命令决定，错误使用 error_code 返回。
    // 保存失败仍保留进程状态，不能继续把空文件当成有效回环。
    // 输出资源不进入源码夹具或 Git LFS 跟踪范围。
    std::error_code ec;
    std::filesystem::create_directories(output, ec);
    if ( ec ) return check(false, "output directory");
    ok &= check(map.saveToFile(output / "custom.mmm"), "native save");
    const auto native = MMM::BeatMap::loadFromFile(output / "custom.mmm");
    ok &= check(
        native.m_timings.size() == 1 &&
            native.m_timings.front().m_interpolation == timing.m_interpolation,
        "native keeps editable function");
    // 二倍速副本要求 g(t)=2*f(2*t)，而不能仅改变函数的 duration。
    MMM::BeatmapSpeedTransformOptions options;
    options.speed = 2;
    const auto scaled =
        MMM::BeatmapSpeedTransform::createSpeedVersion(map, options);
    ok &= check(scaled.success, "speed transform custom function");
    if ( scaled.success ) {
        const auto& changed = scaled.beatmap.m_timings.front();
        ok &= check(
            changed.m_interpolation && near(MMM::evaluateTimingInterpolation(
                                                *changed.m_interpolation,
                                                changed.m_timingEffectParameter,
                                                .5),
                                            304),
            "speed transforms t and BPM output");
    }
    ok &= check(map.m_timings.front().m_interpolation == timing.m_interpolation,
                "speed preserves source");
    return ok;
}
/// @brief 已知初等函数的绘制样本应拟合回可编辑组合表达式。
/// @details 用不等于一秒的时长区分绝对秒 t 和归一化时间。
/// 不要求系数字符串相同，通过独立点求值和误差验证实际结果。
/// @return 每个已知函数家族都能产生合法且足够精确的候选时为真。
/// 样本包含线性、多项式、周期函数、对数和根式。
/// 相同时刻点数与实际手绘画布一致，覆盖完整秒域。
/// 三秒时长防止只对一秒段落成立的归一化实现漏检。
/// 拟合误差使用参数单位，不能以小屏幕像素误差替代。
/// 返回函数必须再次通过数学编译器而非只报告拟合器成功。
/// 重建公式需在若干不同内部时刻复现已知值。
/// 不要求特定家族字符串，等价低误差组合也属于合法结果。
/// 秩不足输入只应失败，不允许产生无限系数。
/// 不完整绘制不能用参考曲线补齐后冒充用户意图。
/// 固定输入不涉及鼠标采样的帧率或窗口尺寸。
bool testFit()
{
    bool ok = true;
    for ( int kind = 0; kind < 5; ++kind ) {
        std::array<MMM::TimingCurvePoint, 129> points{};
        for ( std::size_t i = 0; i < points.size(); ++i ) {
            const double x = i / 128.0;
            double       value;
            if ( kind == 0 )
                value = 120 + 30 * x;
            else if ( kind == 1 )
                value = 120 + 30 * x * x;
            else if ( kind == 2 )
                value = 120 + 20 * std::sin(2 * std::numbers::pi * x);
            else if ( kind == 3 )
                value = 120 + 10 * std::log1p(9 * x);
            else
                value = 120 + 10 * std::sqrt(x);
            points[i] = { 3 * x, value };
        }
        // 已知函数先直接生成样本，不从被测函数拟合器导出原数据。
        // 输入按递增实际时间排列，每个族共享同一采样密度。
        // 误差比较覆盖平滑振荡和端点根式两类形状。
        const auto fit = MMM::fitTimingFunction(points, 3);
        if ( !fit ) {
            XERROR("fit {}: {}", kind, fit.error());
            ok = false;
            continue;
        }
        // 拟合误差以参数单位报告，不能用归一化后很小的误差误导用户。
        ok &= check(fit->m_rmsError < 1e-5 && fit->m_maxError < 1e-4,
                    "elementary fit residuals");
        const auto compiled = MMM::compileTimingFunction(fit->m_expression, 3);
        ok &= check(compiled.has_value(), "fitted source compiles");
        if ( compiled )
            for ( auto i : { 7, 53, 101 } )
                ok &= check(near(MMM::evaluateTimingFunction(**compiled,
                                                             points[i].m_time),
                                 points[i].m_value,
                                 1e-4),
                            "fitted source reproduces samples");
    }
    // 不完整时间域和重复时刻不属于完整可编辑段落，必须明确失败。
    std::array<MMM::TimingCurvePoint, 8> invalid{};
    ok &=
        check(!MMM::fitTimingFunction(invalid, 3), "reject incomplete drawing");
    return ok;
}
}  // namespace
/// @brief 测试只在构建树写出临时谱面，不读取或修改用户配置。
/// @param argc 只接受一个临时输出目录参数。
/// @param argv CTest 为该用例独立配置的路径。
/// @return 任一数学、绘制拟合或持久化契约失败均返回一。
/// 测试各组独立执行，前一组失败不跳过后续组。
/// 不存在自动下载资源、用户配置或图形设备依赖。
/// 运行日志只输出失败条件，成功由 CTest 返回码证明。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    const bool grammar = testGrammar(), domain = testDomainAndIntegral(),
               persistence = testPersistence(argv[1]), fit = testFit(),
               aggregates = testAggregates();
    return grammar && domain && persistence && fit && aggregates ? 0 : 1;
}
