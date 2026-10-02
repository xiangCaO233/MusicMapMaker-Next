#include "mmm/timing/TimingFunction.h"

#include "mmm/SafeParse.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fmt/format.h>
#include <iterator>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
#include <vector>

namespace MMM
{
namespace
{
/// @brief 字符预算同时限制解析工作量和保存文本大小。
constexpr std::size_t MAX_SOURCE = 2048;
/// @brief 固定求值栈的容量，编译时拒绝超出预算的表达式。
constexpr std::size_t MAX_CODE = 256;
/// @brief 积分缓存与导出密度无关，改变 Hz 不会改变拍位。
constexpr std::size_t INTEGRAL_CELLS = 1024;
/// @brief 只包含数学运算；没有跳转、赋值或外部调用指令。
/// Constant、Time 和 LocalVariable 是入栈指令，其他指令消费已有操作数。
/// 一元操作保留栈深，二元操作使栈深减一，指令长度同时限制栈深。
/// 没有跳转，聚合最坏代价由独立预算限制为 65536 次基本操作。
/// 文件不能直接构造内部指令，只能经由 Parser 读取表达式。
/// 二元连续区间由 binary() 显式识别，新增指令须更新该识别。
/// 枚举不参与序列化，调整内部布局不会改变已保存函数的含义。
enum class Op {
    Constant,
    Time,
    LocalVariable,
    Aggregate,
    Negate,
    Add,
    Subtract,
    Multiply,
    Divide,
    Power,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Sinh,
    Cosh,
    Tanh,
    Exp,
    Log,
    Log10,
    Log2,
    Sqrt,
    Cbrt,
    Abs,
    Floor,
    Ceil,
    Round,
    Min,
    Max
};
/// @brief 后缀指令中只有常数需要附带数值。
/// @details 附带值也用于局部变量槽和聚合体索引。
/// 这些索引由编译器产生，不允许用户直接输入内部字节码。
/// 总体指令数包含所有聚合函数体，不能通过嵌套规避预算。
/// 操作码的二元范围必须保持连续，其他二元函数由 binary 显式补充。
struct Instruction {
    /// @brief 算术或初等函数操作码。
    Op m_op;
    /// @brief 常数指令携带的有限数值。
    double m_value{};
};
/// @brief 有界数学聚合操作，局部变量只在其函数体内可见。
/// @details 求和与累乘只枚举闭区间内整数，不把积分的连续域规则套用到它们。
/// 空求和返回零，空累乘返回一；反向定积分则保留方向符号。
/// 这些类型不写入谱面，持久化始终保留完整函数式。
/// 上下限先在外层词法作用域求值，函数体再使用绑定槽。
/// 变量槽与公开时间分开，所以积分体内仍能引用实际秒数 t。
/// 嵌套聚合不创建回调闭包，运行时只传递固定大小值数组。
enum class AggregateKind { Integral, Sum, Product };
/// @brief 聚合体保存独立程序，外层只计算上下限并触发有界执行。
struct Aggregate {
    /// @brief 定积分、有限求和或有限累乘。
    AggregateKind m_kind;
    /// @brief 被绑定的局部变量槽，不占用公开时间变量 t。
    std::size_t m_variable;
    /// @brief 数学排版显示的局部变量名。
    std::string m_name;
    /// @brief 已解析函数体，允许访问 t 和词法作用域中的局部变量。
    std::vector<Instruction> m_body;
};
/// @brief 根程序与聚合体一起拥有，不让内部指令跨对象悬空。
struct Program {
    /// @brief 计算最终 Timing 参数的根后缀程序。
    std::vector<Instruction> m_code;
    /// @brief 最多 16 个聚合体，嵌套深度由局部变量槽数限制。
    std::vector<Aggregate> m_aggregates;
};
/// @brief 名称白名单阻止用户表达式调用脚本或任意宿主功能。
struct FunctionSpec {
    /// @brief 文本中的固定函数名。
    std::string_view m_name;
    /// @brief 对应的纯数学操作码。
    Op m_op;
    /// @brief 参数数目，解析器据此验证逗号和括号。
    int m_arity;
};
/// @brief 常见初等函数及双参数组合函数；log 和 ln 均为自然对数。
/// 反三角函数返回弧度，三角函数输入也统一采用弧度。
/// 双曲函数与三角函数分别列出，不将 tanh 识别成变量 t。
/// pow(a,b) 与 a^b 共用操作码，采用相同定义域约束。
/// min/max 可构成分段组合，但不支持用户定义递归函数。
/// 函数名区分大小写，不使用区域设置或模糊匹配。
/// 新增函数必须同时具备标量求值、区间校验和排版名称。
constexpr FunctionSpec FUNCTIONS[] = {
    { "sin", Op::Sin, 1 },     { "cos", Op::Cos, 1 },
    { "tan", Op::Tan, 1 },     { "asin", Op::Asin, 1 },
    { "acos", Op::Acos, 1 },   { "atan", Op::Atan, 1 },
    { "sinh", Op::Sinh, 1 },   { "cosh", Op::Cosh, 1 },
    { "tanh", Op::Tanh, 1 },   { "exp", Op::Exp, 1 },
    { "log", Op::Log, 1 },     { "ln", Op::Log, 1 },
    { "log10", Op::Log10, 1 }, { "log2", Op::Log2, 1 },
    { "sqrt", Op::Sqrt, 1 },   { "cbrt", Op::Cbrt, 1 },
    { "abs", Op::Abs, 1 },     { "floor", Op::Floor, 1 },
    { "ceil", Op::Ceil, 1 },   { "round", Op::Round, 1 },
    { "pow", Op::Power, 2 },   { "min", Op::Min, 2 },
    { "max", Op::Max, 2 }
};
/// @brief 仅识别 ASCII 标识符，避免区域设置影响表达式语法。
bool isName(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
/// @brief 数字字面量只允许十进制，不接受十六进制或 NaN 扩展。
bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}
/// @brief 有界递归下降解析器；错误以文本返回，不使用异常。
/// @details 生成后缀代码后不再保留语法树，播放期间无需解析。
/// 语法层级为 expression → product → unary → primary。
/// 加减和乘除按左结合循环消费，幂通过递归实现右结合。
/// 括号内重新从 expression 开始，但仍共享深度预算。
/// 数学符号先转成同一语法，不另引入一套计算规则。
/// 数字扫描限定十进制，再由跨平台 helper 转成 double。
/// 失败的部分代码只属于局部对象，不发布给逻辑或渲染线程。
class Parser
{
public:
    /// @brief 视图只在编译调用期间借用，不由解析器长期持有。
    explicit Parser(std::string_view source) : m_source(source)
    {
        // reserve 后最多建立 16 个体，嵌套解析期间 body
        // 观察地址不会因扩容失效。
        m_program.m_aggregates.reserve(16);
    }
    /// @brief 解析完整输入，拒绝未消费尾部及过长文本。
    /// @return 完整后缀程序或首次语法错误的位置说明。
    /// 必须消费全部输入，多余尾部不能被忽略。
    /// 空白可位于 token 之间，但不能拆开数字或函数名。
    /// 成功转移程序所有权，失败的局部值自动回收。
    std::expected<Program, std::string> parse()
    {
        // 文本预算在递归开始前检查，超大文件不会消耗无界栈或内存。
        if ( m_source.empty() || m_source.size() > MAX_SOURCE )
            return std::unexpected("函数为空或超过 2048 字符。");
        if ( !expression() ) return std::unexpected(m_error);
        skip();
        if ( m_pos != m_source.size() )
            return std::unexpected(location("存在多余字符"));
        return std::move(m_program);
    }

private:
    /// @brief 固定 ASCII 空白可在各平台得到同一消费结果。
    /// 只移动游标，不改写借用文本或创建临时字符串。
    /// 不调用受 locale 影响的 isspace，确保跨平台规则一致。
    void skip()
    {
        while ( m_pos < m_source.size() &&
                (m_source[m_pos] == ' ' || m_source[m_pos] == '\t' ||
                 m_source[m_pos] == '\n' || m_source[m_pos] == '\r') )
            ++m_pos;
    }
    /// @brief 只在匹配时消费一个符号，供各级语法共享。
    /// @param c 当前语法层允许的一个分隔符。
    /// @return 匹配并消费为真，不匹配则保留 token 给上层。
    /// 空白可被消费，因此调用者不必重复扫描 token 边界。
    bool take(char c)
    {
        skip();
        if ( m_pos < m_source.size() && m_source[m_pos] == c ) {
            ++m_pos;
            return true;
        }
        return false;
    }
    /// @brief 错误位置按字符从一计数，便于用户定位输入框。
    std::string location(std::string_view reason) const
    {
        return fmt::format("第 {} 字符：{}。", m_pos + 1, reason);
    }
    /// @brief 保留首次语法错误，避免递归回退覆盖真正原因。
    bool fail(std::string_view reason)
    {
        if ( m_error.empty() ) m_error = location(reason);
        return false;
    }
    /// @brief 指令预算在写入之前检查，运行时栈由这个预算保证。
    /// @param op 已由语法识别的纯算术指令。
    /// @param value 常数读取的有限值，其他指令忽略此字段。
    /// @return 预算足够为真，失败后不得使用部分程序。
    /// 先检查再写入，长运算链不能突破求值栈容量。
    bool emit(Op op, double value = 0)
    {
        if ( m_instructionCount >= MAX_CODE )
            return fail("表达式过于复杂，最多 256 个运算项");
        ++m_instructionCount;
        m_currentCode->push_back({ op, value });
        return true;
    }
    /// @brief 加减是最低优先级；循环解析避免长链耗尽递归栈。
    /// @return 一个完整乘除项及后续加减链解析成功为真。
    /// a-b-c 生成 a b Subtract c Subtract，保留左结合。
    /// 看不到本层运算符时返回，逗号和右括号留给外层。
    /// 先解析右值再写操作码，错误输入不会产生栈下溢。
    bool expression()
    {
        if ( !product() ) return false;
        while ( true ) {
            if ( take('+') ) {
                if ( !product() || !emit(Op::Add) ) return false;
            } else if ( take('-') ) {
                if ( !product() || !emit(Op::Subtract) ) return false;
            } else
                return true;
        }
    }
    /// @brief 乘除高于加减，除零留给整段定义域检查。
    /// @return 一个完整一元项及后续乘除链解析成功为真。
    /// 乘除只负责语法，随后才能检查分母是否跨零。
    /// 不接受隐式乘法，2*pi 的边界必须明确以免名称歧义。
    /// 星号或除号不能作为原子项起点，连续运算符会失败。
    bool product()
    {
        if ( !unary() ) return false;
        while ( true ) {
            if ( take('*') ) {
                if ( !unary() || !emit(Op::Multiply) ) return false;
            } else if ( take('/') ) {
                if ( !unary() || !emit(Op::Divide) ) return false;
            } else
                return true;
        }
    }
    /// @brief 限制递归深度；负号低于幂，故 -2^2 等于 -4。
    /// @return 符号链或带可选幂的完整原子项解析成功为真。
    /// 幂的右值可以带负号，所以 2^-2 合法。
    /// 右值仍经 unary，因此 2^3^2 等价于 2^(3^2)。
    /// 进入 primary 前计深度，括号和函数嵌套也受限。
    /// 短路保证失败后不再追加运算符。
    bool unary()
    {
        if ( ++m_depth > 32 ) {
            --m_depth;
            return fail("括号或幂的嵌套超过 32 层");
        }
        // 每一条返回路径都恢复深度，后续并列参数不继承前一参数的深度。
        bool result;
        if ( take('+') )
            result = unary();
        else if ( take('-') )
            result = unary() && emit(Op::Negate);
        else
            result = primary() && (!take('^') || (unary() && emit(Op::Power)));
        --m_depth;
        return result;
    }
    /// @brief 常数、变量、括号和白名单函数是全部原子语法。
    /// @return 常数、时间变量、括号结果或函数调用结果。
    /// 只有 t 和 pi/e 有变量语义，其他名字不访问宿主。
    /// 函数逗号只属于参数列表，普通表达式中不能出现。
    /// 每个参数可以嵌套，但整个函数调用只产生一个值。
    /// 括号后还可继续运算，完整消费由 parse 统一检查。
    bool primary()
    {
        if ( take('(') ) {
            if ( !expression() ) return false;
            return take(')') || fail("缺少右括号");
        }
        skip();
        if ( m_pos == m_source.size() ) return fail("缺少数值或函数");
        if ( isDigit(m_source[m_pos]) || m_source[m_pos] == '.' ) {
            const auto begin = m_pos;
            while ( m_pos < m_source.size() && isDigit(m_source[m_pos]) )
                ++m_pos;
            if ( m_pos < m_source.size() && m_source[m_pos] == '.' ) {
                ++m_pos;
                while ( m_pos < m_source.size() && isDigit(m_source[m_pos]) )
                    ++m_pos;
            }
            // 科学计数法的符号只能跟在 e/E 后；字面量前的符号由 unary 处理。
            if ( m_pos < m_source.size() &&
                 (m_source[m_pos] == 'e' || m_source[m_pos] == 'E') ) {
                ++m_pos;
                if ( m_pos < m_source.size() &&
                     (m_source[m_pos] == '+' || m_source[m_pos] == '-') )
                    ++m_pos;
                while ( m_pos < m_source.size() && isDigit(m_source[m_pos]) )
                    ++m_pos;
            }
            const auto text = m_source.substr(begin, m_pos - begin);
            // Apple 旧部署目标由固定 C locale helper 解析，不调用不可部署重载。
            // 词法扫描已排除扩展表示，还必须检查消费长度和有限性。
            const auto parsed = Internal::parseFloatingPrefix(text);
            if ( parsed.error != std::errc{} ||
                 parsed.parsedLength != text.size() ||
                 !std::isfinite(parsed.value) )
                return fail("无效或溢出的十进制数");
            return emit(Op::Constant, parsed.value);
        }
        const auto begin = m_pos;
        while ( m_pos < m_source.size() &&
                (isName(m_source[m_pos]) || isDigit(m_source[m_pos])) )
            ++m_pos;
        const auto name = m_source.substr(begin, m_pos - begin);
        if ( name == "t" ) return emit(Op::Time);
        // 由内向外查找，允许聚合体遮蔽外层同名局部变量而不改写 t。
        for ( std::size_t slot = m_localCount; slot > 0; --slot )
            if ( name == m_locals[slot - 1] )
                return emit(Op::LocalVariable, slot - 1);
        if ( name == "int" || name == "integral" || name == "sum" ||
             name == "prod" )
            return aggregate(name);
        if ( name == "pi" ) return emit(Op::Constant, std::numbers::pi);
        if ( name == "e" ) return emit(Op::Constant, std::numbers::e);
        const auto spec = std::find_if(
            std::begin(FUNCTIONS), std::end(FUNCTIONS), [name](const auto& f) {
                return f.m_name == name;
            });
        if ( spec == std::end(FUNCTIONS) ) return fail("未知变量或函数");
        if ( !take('(') ) return fail("函数后缺少左括号");
        // 精确参数个数使执行器无需动态栈检查，也不能通过多余参数逃逸。
        for ( int i = 0; i < spec->m_arity; ++i ) {
            if ( i > 0 && !take(',') ) return fail("函数参数数量不正确");
            if ( !expression() ) return false;
        }
        if ( !take(')') ) return fail("函数参数数量不正确或缺少右括号");
        return emit(spec->m_op);
    }
    /// @brief 读取 int(x,a,b,body)、sum(k,a,b,body) 或 prod(k,a,b,body)。
    /// @details 上下限位于外层作用域，局部变量仅在 body 中绑定。
    /// @param name 已识别的聚合函数白名单名称。
    /// @return 四个参数和作用域都完整时返回真。
    /// @pre 当前游标位于函数名之后，首参数必须是标识符。
    /// @note 体程序地址依赖一次预留的聚合容器容量。
    /// @note 上下限可以包含聚合，因此生成当前体前必须再次检查容量。
    /// 名称不能与常数、时间或数学函数重名，避免排版与执行歧义。
    /// 嵌套同名局部变量允许遮蔽，退出体后恢复原绑定数量。
    /// 解析失败保留首个错误，不发布半成品 Program。
    /// 聚合体不能作为新的用户函数调用，也不允许递归定义。
    /// 所有体共享指令预算；体的独立向量不意味着独立预算。
    bool aggregate(std::string_view name)
    {
        if ( m_localCount >= m_locals.size() ||
             m_program.m_aggregates.size() >= 16 )
            return fail("积分或求和的嵌套/数量超出预算");
        if ( !take('(') ) return fail("聚合函数后缺少左括号");
        skip();
        const auto begin = m_pos;
        while ( m_pos < m_source.size() && isName(m_source[m_pos]) ) ++m_pos;
        const auto variable = m_source.substr(begin, m_pos - begin);
        if ( variable.empty() || variable == "t" || variable == "pi" ||
             variable == "e" || variable == "int" || variable == "integral" ||
             variable == "sum" || variable == "prod" ||
             std::any_of(
                 std::begin(FUNCTIONS),
                 std::end(FUNCTIONS),
                 [variable](const auto& f) { return variable == f.m_name; }) )
            return fail("局部变量名无效，不能覆盖 t、pi、e 或数学函数");
        if ( !take(',') || !expression() || !take(',') || !expression() ||
             !take(',') )
            return fail("需要局部变量、下限、上限和函数体四个参数");
        // 上下限可以先创建内层聚合，入口检查之后容器数量仍可能增加。
        // 必须在取地址前重新限制数量，否则扩容会使正在写入的体指针悬空。
        if ( m_program.m_aggregates.size() >= 16 )
            return fail("聚合函数总数超出预算");
        const auto id   = m_program.m_aggregates.size();
        const auto kind = (name == "int" || name == "integral")
                              ? AggregateKind::Integral
                          : name == "sum" ? AggregateKind::Sum
                                          : AggregateKind::Product;
        m_program.m_aggregates.push_back(
            { kind, m_localCount, std::string(variable), {} });
        auto* previousCode       = m_currentCode;
        m_currentCode            = &m_program.m_aggregates[id].m_body;
        m_locals[m_localCount++] = variable;
        // 任意失败路径都恢复外层程序和作用域，不留下悬空绑定。
        const bool success = expression() && take(')');
        --m_localCount;
        m_currentCode = previousCode;
        if ( !success ) return fail("聚合函数体或右括号不完整");
        return emit(Op::Aggregate, id);
    }
    /// @brief 编译期间借用的源文本。
    std::string_view m_source;
    /// @brief 当前解析游标，不允许越过源视图。
    std::size_t m_pos{};
    /// @brief 递归调用深度，仅编译阶段使用。
    int m_depth{};
    /// @brief 返回给编辑器的首个错误。
    std::string m_error;
    /// @brief 编译完成后转交给不可变函数对象。
    Program m_program;
    /// @brief 当前写入的根程序或聚合体，地址由预留容量保证稳定。
    std::vector<Instruction>* m_currentCode{ &m_program.m_code };
    /// @brief 所有体共同计数，不能以嵌套聚合逃逸 256 指令预算。
    std::size_t m_instructionCount{};
    /// @brief 同时最多四层局部绑定，不允许无限递归求值。
    std::array<std::string_view, 4> m_locals{};
    /// @brief 当前词法作用域中可见的绑定数量。
    std::size_t m_localCount{};
};

/// @brief 纯标量数学操作，与区间验证使用相同操作码。
/// @warning 每帧求值路径；只执行白名单数学函数，没有资源访问。
/// @param op 已完成定义域验证的内部操作码。
/// @param a 一元输入或二元左值。
/// @param b 二元右值，一元操作忽略它。
/// @return 标准双精度数学运算的标量结果。
/// @pre 调用方已完成闭区间验证，不能传入文件字节码。
/// 求值不生成错误字符串或日志，避免播放时隐含分配。
/// 实数幂的负底数只能在常整数指数分支获准。
double scalar(Op op, double a, double b = 0)
{
    switch ( op ) {
    case Op::Negate: return -a;
    case Op::Add: return a + b;
    case Op::Subtract: return a - b;
    case Op::Multiply: return a * b;
    case Op::Divide: return a / b;
    case Op::Power: return std::pow(a, b);
    case Op::Sin: return std::sin(a);
    case Op::Cos: return std::cos(a);
    case Op::Tan: return std::tan(a);
    case Op::Asin: return std::asin(a);
    case Op::Acos: return std::acos(a);
    case Op::Atan: return std::atan(a);
    case Op::Sinh: return std::sinh(a);
    case Op::Cosh: return std::cosh(a);
    case Op::Tanh: return std::tanh(a);
    case Op::Exp: return std::exp(a);
    case Op::Log: return std::log(a);
    case Op::Log10: return std::log10(a);
    case Op::Log2: return std::log2(a);
    case Op::Sqrt: return std::sqrt(a);
    case Op::Cbrt: return std::cbrt(a);
    case Op::Abs: return std::abs(a);
    case Op::Floor: return std::floor(a);
    case Op::Ceil: return std::ceil(a);
    case Op::Round: return std::round(a);
    case Op::Min: return std::min(a, b);
    case Op::Max: return std::max(a, b);
    default: return a;
    }
}
/// @brief 二元指令必须从后缀栈取出两个操作数。
bool binary(Op op)
{
    return (op >= Op::Add && op <= Op::Power) || op == Op::Min || op == Op::Max;
}
/// @brief 固定栈解释后缀程序；栈结构在解析阶段已保证。
/// @warning 每帧函数查询；不创建容器或复制共享指针。
/// @param code 结构有效且定义域合法的后缀程序。
/// @param time 实际秒数，公开入口负责截取到合法定义域。
/// @return 栈中唯一的表达式结果。
/// 每次入栈数量不超过指令总数，无需动态扩容。
/// 二元指令先弹右值，减法和除法不会反转操作数。
/// 不建立语法树，公式树已在编译阶段由同一程序生成。
double execute(const Program& program, const std::vector<Instruction>& code,
               double time, std::array<double, 4> locals = {});
/// @brief 聚合体的八点 Gauss 积分，局部变量通过值槽传递。
/// @param program 拥有所有嵌套体的只读程序。
/// @param aggregate 当前积分体及绑定槽。
/// @param time 不随积分局部变量改变的段内实际时间。
/// @param locals 外层绑定的值副本，积分节点只改当前槽。
/// @param left 有方向积分的起点。
/// @param right 有方向积分的终点。
/// @return 八点 Gauss–Legendre 的双精度积分近似。
/// @warning 求值路径可调用；只使用常量节点和固定值槽，没有动态分配。
/// @note 非符号积分，精度由外层误差比较及最大细分层数共同限制。
/// @note 节点不包含区间端点，根式端点仍由独立定义域证明验证。
/// 正权重使近似处于体函数的参数包围乘有符号宽度内。
/// 对称节点成对相加，左右两侧共用同一外层时间参数。
/// 中点使用无溢出算法，不能把两个有限的大端点直接相加。
/// 局部变量值不写回外层，嵌套同名变量也保持词法作用域。
double aggregateQuadrature(const Program& program, const Aggregate& aggregate,
                           double time, std::array<double, 4> locals,
                           double left, double right)
{
    constexpr std::array nodes{ .1834346424956498,
                                .5255324099163290,
                                .7966664774136267,
                                .9602898564975363 };
    constexpr std::array weights{ .3626837833783620,
                                  .3137066458778873,
                                  .2223810344533745,
                                  .1012285362903763 };
    const double         center = std::midpoint(left, right),
                         half   = (right - left) * .5;
    double               sum    = 0;
    for ( std::size_t i = 0; i < nodes.size(); ++i ) {
        locals[aggregate.m_variable] = center - half * nodes[i];
        const double first = execute(program, aggregate.m_body, time, locals);
        locals[aggregate.m_variable] = center + half * nodes[i];
        sum += weights[i] *
               (first + execute(program, aggregate.m_body, time, locals));
    }
    return half * sum;
}
/// @brief 数值定积分的误差驱动细分，最大五层，保证每次执行有固定上限。
/// @param whole 当前整区间近似，用于两半区间误差估计。
/// @param tolerance 当前子区间分配的绝对误差目标。
/// @param depth 已进行的细分层数，最多五层。
/// @return 有方向的有限深度数值近似，不返回解析原函数。
/// @warning 每次函数求值均可能进入；最大工作量由静态成本预检限制。
/// @note 达到深度上限返回两半近似，不能宣称任意振荡函数精确积分。
/// 平滑体通常无需深度细分，端点根式允许使用更多层数。
/// 误差目标随细分平分，避免每个子区间独占整段误差额度。
/// 递归仅传递标量和固定值槽，不建立可变节点容器。
/// 两个子区间共用边界，反向上下限的半区间仍保持反向。
/// 更深的用户聚合表达式另受指令成本限制，不可无界叠加。
double aggregateIntegral(const Program& program, const Aggregate& aggregate,
                         double time, std::array<double, 4> locals, double left,
                         double right, double whole, double tolerance,
                         int depth)
{
    const double middle = std::midpoint(left, right);
    const double first =
        aggregateQuadrature(program, aggregate, time, locals, left, middle);
    const double second =
        aggregateQuadrature(program, aggregate, time, locals, middle, right);
    // 平滑函数通常一轮即可；端点根式可细分，但不建立热路径动态容器。
    if ( depth == 5 || std::abs(first + second - whole) <= tolerance )
        return first + second;
    return aggregateIntegral(program,
                             aggregate,
                             time,
                             locals,
                             left,
                             middle,
                             first,
                             tolerance * .5,
                             depth + 1) +
           aggregateIntegral(program,
                             aggregate,
                             time,
                             locals,
                             middle,
                             right,
                             second,
                             tolerance * .5,
                             depth + 1);
}
/// @param program 包含根与体程序的不可变资源。
/// @param code 当前执行的根或聚合体后缀指令。
/// @param time 当前时间，绑定局部变量不会改变该值。
/// @param locals 最多四层的局部变量环境副本。
/// @return 唯一栈结果，公开入口只传入已通过域验证的程序。
/// 编译器保证每个消费指令前有足够栈元素。
/// 同一对象中的体索引稳定，执行不读取表达式字符串。
/// 离散聚合先取 ceil 与 floor，再逐项访问整数。
/// 整数转换范围已由区间验证限制，避免浮点越界转换。
/// 聚合结果覆盖上下限在栈上的位置，不增加临时栈深。
/// 空求和与空累乘保持各自单位元，不求值不存在的项。
/// @brief 固定栈执行根程序或体程序，聚合体也受编译时成本上限约束。
/// @warning 函数求值热路径；值槽传递局部变量，不分配栈外环境或函数闭包。
double execute(const Program& program, const std::vector<Instruction>& code,
               double time, std::array<double, 4> locals)
{
    std::array<double, MAX_CODE> stack{};
    std::size_t                  size = 0;
    for ( const auto& instruction : code ) {
        if ( instruction.m_op == Op::Constant )
            stack[size++] = instruction.m_value;
        else if ( instruction.m_op == Op::Time )
            stack[size++] = time;
        else if ( instruction.m_op == Op::LocalVariable )
            stack[size++] =
                locals[static_cast<std::size_t>(instruction.m_value)];
        else if ( instruction.m_op == Op::Aggregate ) {
            const double upper = stack[--size], lower = stack[size - 1];
            const auto&  aggregate =
                program.m_aggregates[static_cast<std::size_t>(
                    instruction.m_value)];
            double value = aggregate.m_kind == AggregateKind::Product ? 1 : 0;
            if ( aggregate.m_kind == AggregateKind::Integral ) {
                const double whole = aggregateQuadrature(
                    program, aggregate, time, locals, lower, upper);
                value = aggregateIntegral(program,
                                          aggregate,
                                          time,
                                          locals,
                                          lower,
                                          upper,
                                          whole,
                                          1e-9 * std::max(1.0, std::abs(whole)),
                                          0);
            } else {
                const auto first = static_cast<long long>(std::ceil(lower));
                const auto last  = static_cast<long long>(std::floor(upper));
                for ( auto k = first; k <= last; ++k ) {
                    locals[aggregate.m_variable] = static_cast<double>(k);
                    const auto item =
                        execute(program, aggregate.m_body, time, locals);
                    if ( aggregate.m_kind == AggregateKind::Sum )
                        value += item;
                    else
                        value *= item;
                }
            }
            stack[size - 1] = value;
        } else if ( binary(instruction.m_op) ) {
            const double right = stack[--size];
            stack[size - 1] = scalar(instruction.m_op, stack[size - 1], right);
        } else
            stack[size - 1] = scalar(instruction.m_op, stack[size - 1]);
    }
    return stack[0];
}
/// @brief 闭区间用于检查整个定义域，不依赖导出采样碰巧命中奇点。
struct Interval {
    /// @brief 保守下界。
    double m_low{};
    /// @brief 保守上界。
    double m_high{};
};
/// @brief 测试闭区间是否包含特殊值，端点也属于定义域。
bool contains(Interval a, double value)
{
    return a.m_low <= value && a.m_high >= value;
}
/// @brief 判断周期极值或极点是否落在闭区间。
/// @param a 连续闭区间，包含所有内部时刻而非仅两个样本。
/// @param first 一个代表点，如 tan 极点 π/2。
/// @param period 来自数学常数的正周期。
/// @return 有整数 k 使 first+k*period 落入区间时为真。
/// 端点极点也无定义，不能用严格不等式漏掉它。
/// 巨大参数保守返回真，浮点约简不足不是合法证明。
bool hasPeriodicPoint(Interval a, double first, double period)
{
    // 极大参数无法可靠约简时保守认定存在，不能漏过 tan 的极点。
    if ( std::abs(a.m_low) > 1e14 || std::abs(a.m_high) > 1e14 ) return true;
    return std::ceil((a.m_low - first) / period) <=
           std::floor((a.m_high - first) / period);
}
/// @brief 对一个操作传播区间并拒绝可能无定义的输入。
/// @details 相关子表达式采用保守包围，无法证明合法时拒绝保存。
/// @note 子区间划分能缩小包围，但不会以离散取样代替除零判断。
/// @param op 与标量执行器一致的运算。
/// @param a 已证明有限的一元输入或二元左输入包围。
/// @param b 二元右输入包围，一元运算忽略它。
/// @return 连续域内的结果包围，不能证明合法则返回空。
/// 子表达式会丢失变量相关性，包围可能偏宽而保守拒绝。
/// min/max 可以不可导，只要全部值有限即可。
/// 周期极值和根式零点不能仅靠四角求值覆盖。
/// 负指数遇零立即拒绝，不依赖 pow 的平台错误返回。
/// 每级操作均检查有限性，指数溢出不能继续传播。
std::optional<Interval> intervalOperation(Op op, Interval a, Interval b)
{
    Interval result;
    if ( op == Op::Add )
        result = { a.m_low + b.m_low, a.m_high + b.m_high };
    else if ( op == Op::Subtract )
        result = { a.m_low - b.m_high, a.m_high - b.m_low };
    else if ( op == Op::Multiply || op == Op::Divide ) {
        if ( op == Op::Divide && contains(b, 0) ) return std::nullopt;
        // 乘除四个角覆盖所有内部极值；分母已保证不跨零。
        const std::array corners{ scalar(op, a.m_low, b.m_low),
                                  scalar(op, a.m_low, b.m_high),
                                  scalar(op, a.m_high, b.m_low),
                                  scalar(op, a.m_high, b.m_high) };
        result = { *std::min_element(corners.begin(), corners.end()),
                   *std::max_element(corners.begin(), corners.end()) };
    } else if ( op == Op::Power ) {
        // 零宽且等于截断值的指数才能证明为常整数。
        // 随时间变化的指数，即便端点为整数也不能接受负底数。
        const bool integer =
            b.m_low == b.m_high && std::trunc(b.m_low) == b.m_low;
        if ( (!integer && a.m_low < 0) || (contains(a, 0) && b.m_low <= 0) )
            return std::nullopt;
        // 常整数指数允许负底数；偶次幂跨零时必须补充内部最小值。
        // 指数为零且底数包含零采用严格数学定义，避免平台 pow(0,0) 差异。
        const std::array corners{ std::pow(a.m_low, b.m_low),
                                  std::pow(a.m_low, b.m_high),
                                  std::pow(a.m_high, b.m_low),
                                  std::pow(a.m_high, b.m_high) };
        result = { *std::min_element(corners.begin(), corners.end()),
                   *std::max_element(corners.begin(), corners.end()) };
        // 偶次幂需补充内部零值，奇次幂则必须保留已有负下界。
        // 用更小下界合并，不能把跨零三次幂误认为非负。
        if ( integer && b.m_low > 0 && contains(a, 0) )
            result.m_low = std::min(result.m_low, 0.0);
        // 底数一在变指数情况下始终为一，补入结果包围。
        // 此边界规则同时保持退化区间的明确取值。
        if ( contains(a, 1) ) {
            result.m_low  = std::min(result.m_low, 1.0);
            result.m_high = std::max(result.m_high, 1.0);
        }
    } else if ( op == Op::Min )
        result = { std::min(a.m_low, b.m_low), std::min(a.m_high, b.m_high) };
    else if ( op == Op::Max )
        result = { std::max(a.m_low, b.m_low), std::max(a.m_high, b.m_high) };
    else {
        // 这些函数的定义域必须整段满足，不能只检查首尾值。
        if ( (op == Op::Log || op == Op::Log10 || op == Op::Log2) &&
             a.m_low <= 0 )
            return std::nullopt;
        if ( op == Op::Sqrt && a.m_low < 0 ) return std::nullopt;
        if ( (op == Op::Asin || op == Op::Acos) &&
             (a.m_low < -1 || a.m_high > 1) )
            return std::nullopt;
        if ( op == Op::Tan &&
             hasPeriodicPoint(a, std::numbers::pi / 2, std::numbers::pi) )
            return std::nullopt;
        // 合法域内的反三角、根式、对数和双曲正切是单调函数。
        // acos 和取负方向相反，因此用 min/max 统一端点方向。
        const double left = scalar(op, a.m_low), right = scalar(op, a.m_high);
        result = { std::min(left, right), std::max(left, right) };
        // 单调函数只需端值；周期函数另外覆盖内部极值。
        if ( op == Op::Sin ) {
            if ( hasPeriodicPoint(
                     a, std::numbers::pi / 2, 2 * std::numbers::pi) )
                result.m_high = 1;
            if ( hasPeriodicPoint(
                     a, -std::numbers::pi / 2, 2 * std::numbers::pi) )
                result.m_low = -1;
        }
        if ( op == Op::Cos ) {
            if ( hasPeriodicPoint(a, 0, 2 * std::numbers::pi) )
                result.m_high = 1;
            if ( hasPeriodicPoint(a, std::numbers::pi, 2 * std::numbers::pi) )
                result.m_low = -1;
        }
        // abs 与 cosh 在零处存在内部最小值，不能仅采用两端。
        // 补最小值不会改变端点已经覆盖的最大值。
        if ( op == Op::Abs && contains(a, 0) ) result.m_low = 0;
        if ( op == Op::Cosh && contains(a, 0) ) result.m_low = 1;
    }
    // 溢出在编译时拒绝，即便当前导出密度不会生成那个取值。
    if ( !std::isfinite(result.m_low) || !std::isfinite(result.m_high) )
        return std::nullopt;
    return result;
}
/// @brief 在完整时间子区间解释程序，得到参数的保守范围。
/// @param code 结构有效的程序，不直接执行文件字节码。
/// @param time 含首尾与全部内部时刻的一段时间闭区间。
/// @return 保守结果包围，任意子表达式域无效立即失败。
/// 采用与标量执行相同弹栈顺序，只改变操作数类型。
/// 常数是零宽区间，才能识别常整数指数和固定分母。
/// 域验证与积分精度验证使用独立预算和算法。
/// @param program 聚合体由当前对象稳定拥有。
/// @param locals 每个绑定槽当前可能的闭区间。
/// @param work 可选输出，估计一次标量执行的最坏基本操作数。
/// @note 本函数只在编译时运行，不在图形或会话热路径证明区间。
/// @note 超过 65536 基本操作的单次表达式不发布。
/// 固定整数上下限逐项证明，允许 (-1)^k 这种离散合法表达式。
/// 随 t 改变的上下限需要涵盖所有可能访问的整数。
/// 该包围不声称不同时间上的各项同时达到各自极值。
/// 求和范围包含空集合的零值；累乘范围包含单位元一。
/// 定积分体必须在上下限覆盖的全部实数上有定义。
/// 保守包围会拒绝某些实际合法但高度相关的表达式。
/// 拒绝提供明确说明，不能以较粗输出密度跳过定义域检查。
/// 每层体独立检查有限值，乘积溢出在构建阶段失败。
/// 代价估计包含内层聚合，嵌套体不能各自重置预算。
/// 即使固定上下限结果为空，也必须保持整数转换可表示。
std::optional<Interval> executeInterval(const Program&                  program,
                                        const std::vector<Instruction>& code,
                                        Interval                        time,
                                        std::array<Interval, 4> locals = {},
                                        std::size_t*            work = nullptr)
{
    std::array<Interval, MAX_CODE> stack{};
    std::size_t                    size = 0, cost = code.size();
    for ( const auto& instruction : code ) {
        if ( instruction.m_op == Op::Constant )
            stack[size++] = { instruction.m_value, instruction.m_value };
        else if ( instruction.m_op == Op::Time )
            stack[size++] = time;
        else if ( instruction.m_op == Op::LocalVariable )
            stack[size++] =
                locals[static_cast<std::size_t>(instruction.m_value)];
        else if ( instruction.m_op == Op::Aggregate ) {
            const auto  upper = stack[--size], lower = stack[size - 1];
            const auto& aggregate =
                program.m_aggregates[static_cast<std::size_t>(
                    instruction.m_value)];
            const double low  = std::min(lower.m_low, upper.m_low),
                         high = std::max(lower.m_high, upper.m_high);
            // 求和的整数转换和项数都先在 double 域检查，避免极限输入溢出。
            if ( aggregate.m_kind != AggregateKind::Integral &&
                 (low < -1e9 || high > 1e9 ||
                  std::floor(upper.m_high) - std::ceil(lower.m_low) + 1 > 256) )
                return std::nullopt;
            Interval result;
            if ( aggregate.m_kind == AggregateKind::Integral ) {
                locals[aggregate.m_variable] = { low, high };
                std::size_t bodyWork         = 0;
                const auto  body             = executeInterval(
                    program, aggregate.m_body, time, locals, &bodyWork);
                if ( !body ) return std::nullopt;
                // 正权重积分处于函数体包围乘有符号区间宽度的范围中。
                const Interval width{ upper.m_low - lower.m_high,
                                      upper.m_high - lower.m_low };
                const auto     bounds =
                    intervalOperation(Op::Multiply, *body, width);
                if ( !bounds ) return std::nullopt;
                result = *bounds;
                cost += bodyWork * 1520;
            } else {
                const auto first =
                    static_cast<long long>(std::ceil(lower.m_low));
                const auto last =
                    static_cast<long long>(std::floor(upper.m_high));
                const bool fixed =
                    lower.m_low == lower.m_high && upper.m_low == upper.m_high;
                result = aggregate.m_kind == AggregateKind::Product
                             ? Interval{ 1, 1 }
                             : Interval{};
                Interval    bodyRange{ 0, 0 };
                std::size_t bodyWork = 0;
                // 整数聚合体逐个离散值证明，不能要求中间所有实数也有定义。
                // 例如 (-1)^k 在整数 k 上合法，在非整数 k 上则不合法。
                for ( auto k = first; k <= last; ++k ) {
                    locals[aggregate.m_variable] = { static_cast<double>(k),
                                                     static_cast<double>(k) };
                    std::size_t itemWork         = 0;
                    const auto  item             = executeInterval(
                        program, aggregate.m_body, time, locals, &itemWork);
                    if ( !item ) return std::nullopt;
                    bodyWork         = std::max(bodyWork, itemWork);
                    bodyRange.m_low  = std::min(bodyRange.m_low, item->m_low);
                    bodyRange.m_high = std::max(bodyRange.m_high, item->m_high);
                    if ( fixed ) {
                        const auto value = intervalOperation(
                            aggregate.m_kind == AggregateKind::Sum
                                ? Op::Add
                                : Op::Multiply,
                            result,
                            *item);
                        if ( !value ) return std::nullopt;
                        result = *value;
                    }
                }
                const auto count =
                    static_cast<std::size_t>(std::max(0LL, last - first + 1));
                cost += bodyWork * count;
                if ( !fixed && count > 0 ) {
                    // 变动整数上限可能产生空集，因此求和包围含零，乘积包围含一。
                    if ( aggregate.m_kind == AggregateKind::Sum )
                        result = { bodyRange.m_low * count,
                                   bodyRange.m_high * count };
                    else {
                        const double magnitude =
                            std::pow(std::max({ 1.0,
                                                std::abs(bodyRange.m_low),
                                                std::abs(bodyRange.m_high) }),
                                     count);
                        result = { bodyRange.m_low < 0 ? -magnitude : 0,
                                   magnitude };
                    }
                }
            }
            if ( cost > 65536 || !std::isfinite(result.m_low) ||
                 !std::isfinite(result.m_high) )
                return std::nullopt;
            stack[size - 1] = result;
        } else {
            Interval right{};
            if ( binary(instruction.m_op) ) right = stack[--size];
            const auto result =
                intervalOperation(instruction.m_op, stack[size - 1], right);
            if ( !result ) return std::nullopt;
            stack[size - 1] = *result;
        }
    }
    if ( work ) *work = cost;
    return stack[0];
}
/// @brief 八点 Gauss 积分只在编译准备阶段执行。
/// @details 对平滑初等函数比固定梯形法更准确，预算仍由缓存格数限定。
/// @param code 编译阶段借用的纯数学程序。
/// @param left 当前积分子区间起点秒数。
/// @param right 当前积分子区间终点秒数。
/// @return 当前区间积分，不含以前区间的累计量。
/// 节点在区间内部，根式端点导数奇异不会直接进入求值。
/// 随后比较两半区间与局部插值，总积分不能证明局部精度。
double quadrature(const Program& program, double left, double right)
{
    constexpr std::array nodes{ .1834346424956498,
                                .5255324099163290,
                                .7966664774136267,
                                .9602898564975363 };
    constexpr std::array weights{ .3626837833783620,
                                  .3137066458778873,
                                  .2223810344533745,
                                  .1012285362903763 };
    const double center = (left + right) * .5, half = (right - left) * .5;
    double       sum = 0;
    // 对称节点同时求值，不需要在播放时再次扫描整个时间区间。
    for ( std::size_t i = 0; i < nodes.size(); ++i )
        sum += weights[i] *
               (execute(program, program.m_code, center - half * nodes[i]) +
                execute(program, program.m_code, center + half * nodes[i]));
    return sum * half;
}
}  // namespace

/// @brief 编译后的不可变函数；只通过只读接口暴露给调用方。
/// @details 源文本、程序、范围和积分同时发布，读者无需互斥锁。
/// 编译期间可修改状态，但只有 const 对象可以离开入口。
/// 源文本负责保存，程序负责执行，数学树只负责排版。
/// 全部缓存属于同一时长，更改定义域必须生成新对象。
/// 共享生命周期覆盖撤销、快照和模态副本的交错使用。
/// 发布后没有惰性缓存、互斥锁或按播放位置补算。
class TimingFunction
{
public:
    /// @brief 原生保存与再次编辑使用的表达式。
    std::string m_source;
    /// @brief 仅在编译时生成的有界后缀程序。
    Program m_program;
    /// @brief 实际秒定义域长度，不能用于另一时长而不重新验证。
    double m_duration{};
    /// @brief 与执行代码同步构建的公式排版树，避免显示与实际运算分叉。
    std::vector<TimingMathNode> m_math;
    /// @brief 区间传播证明的整个参数包围。
    Interval m_range;
    /// @brief 累计积分的有序节点，根式端点附近可自适应细分。
    struct Knot {
        /// @brief 相对段首秒数，严格递增。
        double m_time;
        /// @brief 该节点的函数值，即累计积分的一阶导数。
        double m_value;
        /// @brief 从段首累计至当前节点的参数积分。
        double m_integral;
    };
    /// @brief 编译阶段建立，发布后只作有界二分查询，最多 65537 个节点。
    std::vector<Knot> m_knots;
    /// @brief 以积分和局部三次插值误差决定是否继续细分。
    /// @note 细分只在编译阶段进行，不按播放位置延迟计算。
    /// @param left 未追加区间起点，应与当前末节点时间一致。
    /// @param right 成功后成为新的缓存末节点。
    /// @param tolerance 初始格点分配的绝对误差预算。
    /// @param depth 细分深度，防止不连续函数耗尽递归栈。
    /// @return 追加成功且未耗尽资源预算为真。
    /// 左半先追加、右半随后追加，保持时间严格递增。
    /// 全部准备完成才发布，部分失败序列不会被热路径读取。
    /// 累计量从前一末值接续，查询可直接恢复总体积分。
    /// @details 细分同时验证总面积与中点累计面积，不能仅靠最终积分。
    /// 局部累计量还与以两端函数值为斜率的 Hermite 预测比较。
    /// 根式端点导数可以很大，但节点数与递归深度仍有明确上限。
    /// 节点时间严格递增，公开查询不处理重复时间带来的零宽区间。
    /// 深度上限用于处理不连续参数，失败不会发布不满足误差目标的缓存。
    /// 每个初始单元独立获得误差额度，不能用较长单元掩盖局部误差。
    /// 缓存总节点上限与导出事件上限独立，二者职责不同。
    /// 编译完成前不存在跨线程读者，容器扩容不会与渲染访问并发。
    bool appendIntegral(double left, double right, double tolerance, int depth)
    {
        const double middle     = (left + right) * .5;
        const double first      = execute(m_program, m_program.m_code, left),
                     last       = execute(m_program, m_program.m_code, right);
        const double integral   = quadrature(m_program, left, right);
        const double half       = quadrature(m_program, left, middle);
        const double secondHalf = quadrature(m_program, middle, right);
        const double estimate =
            integral * .5 + (right - left) * (first - last) * .125;
        // 同时比较数值积分与 Hermite 中点，防止只验证总积分而漏掉局部振荡。
        // 预算耗尽时明确拒绝，不发布精度不足的缓存给拍位反解使用。
        if ( std::abs(half - estimate) > tolerance ||
             std::abs(integral - half - secondHalf) > tolerance ) {
            if ( depth >= 28 || m_knots.size() >= 65536 ) return false;
            return appendIntegral(left, middle, tolerance, depth + 1) &&
                   appendIntegral(middle, right, tolerance, depth + 1);
        }
        const double cumulative = m_knots.back().m_integral + integral;
        if ( !std::isfinite(cumulative) || m_knots.size() >= 65537 )
            return false;
        m_knots.push_back({ right, last, cumulative });
        return true;
    }
};

/// @brief 编译、证明定义域并准备积分缓存后一次发布。
/// @warning 低频编辑或载入路径；共享所有权保证删除实体后旧 UI 快照仍可使用。
/// @param expression 原始输入，可包含支持的 Unicode 数学符号。
/// @param duration 秒域闭区间长度，至少为 1 ms。
/// @return 只读函数或明确错误，不携带部分可执行程序。
/// 文本、指令、递归与积分节点预算分别在相应入口检查。
/// Unicode 只改变输入表示，不改变运算优先级和白名单。
/// 完整参数包围用于 BPM 校验及非单调曲线显示。
/// 保存保留源文本，不把可编辑公式退化为内部程序。
/// 全部缓存完成后才转换为共享 const 对象。
std::expected<std::shared_ptr<const TimingFunction>, std::string>
compileTimingFunction(std::string_view expression, double duration)
{
    if ( !std::isfinite(duration) || duration < .001 )
        return std::unexpected("函数段落时长须至少为 1 ms。");
    if ( expression.size() > MAX_SOURCE )
        return std::unexpected("函数超过 2048 UTF-8 字节。");
    std::string normalized;
    // 用户可直接使用数学符号；归一化只用于解析，保存仍保留原输入文本。
    // 根式模板要求括号，符号幂遵循与 ^ 相同的右结合语法。
    for ( std::size_t i = 0; i < expression.size(); ) {
        bool replaced = false;
        for ( auto [symbol, replacement] :
              { std::pair<std::string_view, std::string_view>{ "π", "pi" },
                { "×", "*" },
                { "÷", "/" },
                { "−", "-" },
                { "²", "^2" },
                { "³", "^3" },
                { "√", "sqrt" },
                { "∛", "cbrt" } } ) {
            if ( expression.substr(i).starts_with(symbol) ) {
                normalized += replacement;
                i += symbol.size();
                replaced = true;
                break;
            }
        }
        if ( !replaced ) normalized += expression[i++];
    }
    auto code = Parser(normalized).parse();
    if ( !code ) return std::unexpected(code.error());
    auto result        = std::make_shared<TimingFunction>();
    result->m_source   = expression;
    result->m_program  = std::move(*code);
    result->m_duration = duration;
    // 由同一后缀程序建立数学树，聚合函数体通过局部名称环境递归排版。
    const auto appendMath = [&](auto&&                          self,
                                const std::vector<Instruction>& instructions,
                                std::array<std::string_view, 4>
                                    names) -> int {
        std::array<int, MAX_CODE> indices{};
        std::size_t               stackSize = 0;
        for ( const auto& instruction : instructions ) {
            TimingMathNode node;
            // 数学树复用同一程序的操作码，预览与运行不允许拥有两套语法。
            // 每个指令产生一个结构节点，父节点索引只能指向之前建立的子树。
            // 聚合体先递归生成主体再生成外层节点，根仍是整个序列最后一项。
            if ( instruction.m_op == Op::Time )
                node.m_text = "t";
            else if ( instruction.m_op == Op::LocalVariable )
                node.m_text =
                    names[static_cast<std::size_t>(instruction.m_value)];
            else if ( instruction.m_op == Op::Constant )
                node.m_text = instruction.m_value == std::numbers::pi ? "π"
                              : instruction.m_value == std::numbers::e
                                  ? "e"
                                  : fmt::format("{:.17g}", instruction.m_value);
            else if ( instruction.m_op == Op::Aggregate ) {
                const auto& aggregate =
                    result->m_program.m_aggregates[static_cast<std::size_t>(
                        instruction.m_value)];
                node.m_second                    = indices[--stackSize];
                node.m_first                     = indices[--stackSize];
                auto innerNames                  = names;
                innerNames[aggregate.m_variable] = aggregate.m_name;
                node.m_third = self(self, aggregate.m_body, innerNames);
                node.m_text  = aggregate.m_name;
                node.m_kind  = aggregate.m_kind == AggregateKind::Integral
                                   ? TimingMathKind::Integral
                               : aggregate.m_kind == AggregateKind::Sum
                                   ? TimingMathKind::Sum
                                   : TimingMathKind::Product;
            } else {
                if ( binary(instruction.m_op) )
                    node.m_second = indices[--stackSize];
                node.m_first = indices[--stackSize];
                switch ( instruction.m_op ) {
                case Op::Negate: node.m_kind = TimingMathKind::Negate; break;
                case Op::Add: node.m_kind = TimingMathKind::Add; break;
                case Op::Subtract:
                    node.m_kind = TimingMathKind::Subtract;
                    break;
                case Op::Multiply:
                    node.m_kind = TimingMathKind::Multiply;
                    break;
                case Op::Divide: node.m_kind = TimingMathKind::Fraction; break;
                case Op::Power: node.m_kind = TimingMathKind::Power; break;
                case Op::Sqrt: node.m_kind = TimingMathKind::Root; break;
                case Op::Cbrt: node.m_kind = TimingMathKind::CubeRoot; break;
                case Op::Abs: node.m_kind = TimingMathKind::Absolute; break;
                default: {
                    node.m_kind = TimingMathKind::Function;
                    const auto spec =
                        std::find_if(std::begin(FUNCTIONS),
                                     std::end(FUNCTIONS),
                                     [&](const auto& f) {
                                         return f.m_op == instruction.m_op;
                                     });
                    node.m_text =
                        spec == std::end(FUNCTIONS) ? "" : spec->m_name;
                    if ( node.m_text == "log" ) node.m_text = "ln";
                    break;
                }
                }
            }
            indices[stackSize++] = static_cast<int>(result->m_math.size());
            result->m_math.push_back(std::move(node));
        }
        return indices[0];
    };
    // 局部变量名称只用于数学排版，不能改变执行器中的固定槽索引。
    // 三元聚合节点分别引用上下限与主体，避免以普通函数逗号横排。
    // 树的总节点数与所有体指令总量相同，同样受 256 上限约束。
    appendMath(appendMath, result->m_program.m_code, {});
    // 合并所有子区间包围，首尾相同不代表中间参数恒定。
    // 波形的内部峰谷也因此进入图形显示的取值范围。
    result->m_range = { std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity() };
    // 区间验证覆盖整个闭区间，奇点位于两个样本之间也会被拒绝。
    // 允许的函数形式不必单调，因此 BPM 校验必须采用整个包围。
    for ( std::size_t i = 0; i < 256; ++i ) {
        const auto range =
            executeInterval(result->m_program,
                            result->m_program.m_code,
                            { duration * i / 256, duration * (i + 1) / 256 });
        if ( !range )
            return std::unexpected(
                fmt::format("在 {:.6g}–{:.6g} "
                            "秒内无法证明函数有限且有定义，请检查除零、对数、根"
                            "式或幂的定义域。",
                            duration * i / 256,
                            duration * (i + 1) / 256));
        result->m_range.m_low = std::min(result->m_range.m_low, range->m_low);
        result->m_range.m_high =
            std::max(result->m_range.m_high, range->m_high);
    }
    const double step = duration / INTEGRAL_CELLS;
    // 正常函数预留基础节点，极端曲率才在低频准备时扩展。
    // 发布后节点不可变，播放期间不会再次扩容。
    // 积分缓存只存一个真实定义域的结果，与后续格式输出 Hz 无关。
    // 在创建对象时分配容量，播放查询借用不可变节点无需互斥锁。
    // 累计积分用作 BPM 的拍位映射，曲线参数本身仍由程序求值。
    result->m_knots.reserve(INTEGRAL_CELLS + 1);
    result->m_knots.push_back(
        { 0, execute(result->m_program, result->m_program.m_code, 0), 0 });
    const double tolerance = 1e-8 * step *
                             std::max({ 1.0,
                                        std::abs(result->m_range.m_low),
                                        std::abs(result->m_range.m_high) });
    // 积分预算独立于输出 Hz；只在根式端点或高曲率区间增加内部节点。
    // 使用闭区间的精确尾端，避免乘除取整令缓存时长与定义域不一致。
    for ( std::size_t i = 0; i < INTEGRAL_CELLS; ++i ) {
        const double left  = duration * i / INTEGRAL_CELLS;
        const double right = duration * (i + 1) / INTEGRAL_CELLS;
        if ( !result->appendIntegral(left, right, tolerance, 0) )
            return std::unexpected(
                "函数变化过快或积分超出范围，请减小频率或缩短段落。");
    }
    // 完整 const 对象一次发布，其他线程不能看到半成品状态。
    // 所有权转移只发生在编译返回，不进入逐次求值路径。
    return std::shared_ptr<const TimingFunction>(std::move(result));
}
/// @brief 借用公式节点序列，根节点位于最后一项。
/// @param function 当前持有其生命周期的已编译对象。
/// @return 稳定索引的只读节点视图，不转移任何字符串或共享所有权。
/// @warning 每帧排版入口可借用；对象销毁后不能保留视图。
/// 节点顺序保证子节点先于父节点，渲染器可使用有界缓存。
/// 聚合体在同一数组中，不需要跨资源追踪子树。
std::span<const TimingMathNode> timingFunctionMathNodes(
    const TimingFunction& function)
{
    return function.m_math;
}
/// @brief 返回原始文本视图，调用方负责保持对象生命周期。
/// @param function 持续存活的只读编译对象。
/// @return 原始输入视图，包含用户输入的 Unicode 形式。
/// 编译器的归一化副本不替代保存文本。
/// 重新载入必须同时使用保存的时长，不仅复制这段视图。
/// 撤销副本持有资源时源文本地址稳定，无须每帧复制。
std::string_view timingFunctionExpression(const TimingFunction& function)
{
    return function.m_source;
}
/// @brief 返回定义域长度，供编辑和加载校验缓存是否适用。
/// @param function 与积分缓存一起发布的完整对象。
/// @return 原始闭区间右端秒数。
/// 表达式变量 t 是秒数，而不是自动归一化的 0–1 进度。
/// 编辑终点改变时旧对象失效，必须重新编译。
/// 外部采样率变化不影响这里返回的定义域。
double timingFunctionDuration(const TimingFunction& function)
{
    return function.m_duration;
}
/// @brief 返回保守包围，用于合法性校验和曲线显示范围。
/// @param function 已通过完整定义域检查的对象。
/// @return 包含全部内部峰谷的保守下界与上界。
/// 返回值可能宽于真实范围，不是离散样本极值。
/// BPM 领域据此阻止段内非正值或超过允许范围的过冲。
/// 曲线图也据此留出纵向空间，首尾相同不表示恒定函数。
std::pair<double, double> timingFunctionRange(const TimingFunction& function)
{
    return { function.m_range.m_low, function.m_range.m_high };
}
/// @brief 执行已验证程序，时间超出段落时保持相应端值。
/// @warning 每帧求值；借用不可变对象，不复制所有权或分配临时容器。
/// @param function 已完成域检查且资源仍存活的对象。
/// @param time 段内秒数，段外按最近端点截取。
/// @return 实际 Timing 参数值，不再与两个端点作 lerp。
/// @pre 调用方传入有限时间参数。
/// 积分与聚合只能访问编译时证明过的绑定范围。
/// 求值不修改累计积分缓存，也不改变原始表达式。
double evaluateTimingFunction(const TimingFunction& function, double time)
{
    return execute(function.m_program,
                   function.m_program.m_code,
                   std::clamp(time, 0.0, function.m_duration));
}
/// @brief 在累计积分缓存中作局部三次插值，域外常值外推。
/// @warning 每帧拍位查询；固定次数标量操作，没有解析或数值积分。
/// @param function 拥有按原函数准备的累计积分节点。
/// @param time 从段落起点起计算的有符号秒数。
/// @return 从零到 time 的参数积分，单位为参数乘秒。
/// @pre 时间有限；BPM 拍位调用方另外除以六十。
/// 前段按起值延伸，尾段按终值延伸，避免负拍位被截为零。
/// 内部使用二分查找相邻节点，不全量遍历积分容器。
/// 局部 Hermite 插值的斜率来自参数值，维持积分与导数含义。
/// 正参数曲线会限幅局部斜率，防止插值产生反向拍位。
/// 导出密度不会修改该缓存，因此粗写出密度不影响编辑器拍位。
double integrateTimingFunction(const TimingFunction& function, double time)
{
    if ( time <= 0 ) return time * function.m_knots.front().m_value;
    if ( time >= function.m_duration )
        return function.m_knots.back().m_integral +
               (time - function.m_duration) * function.m_knots.back().m_value;
    // 节点严格有序且覆盖整个定义域，二分最多访问 16 层，不遍历时间线。
    const auto next = std::upper_bound(
        function.m_knots.begin(),
        function.m_knots.end(),
        time,
        [](double value, const auto& knot) { return value < knot.m_time; });
    const auto&  left     = *(next - 1);
    const auto&  right    = *next;
    const double step     = right.m_time - left.m_time;
    const double u        = (time - left.m_time) / step;
    const double integral = right.m_integral - left.m_integral;
    double       first = left.m_value * step, last = right.m_value * step;
    // 正参数的累计积分必须单调，否则拍位反解可能选择错误分支。
    // 使用局部斜率约束，正常平滑曲线不会触发这一保护。
    if ( function.m_range.m_low > 0 && integral > 0 ) {
        const double length = std::hypot(first / integral, last / integral);
        if ( length > 3 ) {
            first *= 3 / length;
            last *= 3 / length;
        }
    }
    return left.m_integral + (-2 * u * u * u + 3 * u * u) * integral +
           (u * u * u - 2 * u * u + u) * first + (u * u * u - u * u) * last;
}
/// @brief 变速副本只重写独立变量 t，函数名称和常数保持原样。
/// @param function 源谱面拥有的表达式，不就地改写。
/// @param timeScale 变速后自变量的正缩放。
/// @param valueScale BPM 随速度缩放，其他 Timing 参数使用一。
/// @return 显式 g(t)=valueScale*f(timeScale*t) 的文本。
/// 词法替换只匹配独立 t，不把 tan、tanh 或局部变量名部分替换。
/// 局部变量不允许命名为 t，所以积分上下限和体中的公开时间一致缩放。
/// 返回文本由调用方按新时长重新编译，不能直接复用旧缓存。
/// 结果超预算或定义域失败时变速副本返回失败，源文件保持原状。
std::string rescaleTimingFunctionExpression(const TimingFunction& function,
                                            double timeScale, double valueScale)
{
    std::string result = fmt::format("({:.17g})*(", valueScale);
    const auto  source = timingFunctionExpression(function);
    // 按标识符边界扫描，不能把 tan 或 sqrt 的字符 t 当成时间变量。
    for ( std::size_t i = 0; i < source.size(); ) {
        if ( isName(source[i]) ) {
            const auto begin = i++;
            while ( i < source.size() &&
                    (isName(source[i]) || isDigit(source[i])) )
                ++i;
            const auto name = source.substr(begin, i - begin);
            if ( name == "t" )
                result += fmt::format("(t*{:.17g})", timeScale);
            else
                result += name;
        } else
            result += source[i++];
    }
    result += ')';
    return result;
}
}  // namespace MMM
