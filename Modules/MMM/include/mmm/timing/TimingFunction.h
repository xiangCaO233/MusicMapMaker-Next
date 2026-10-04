#pragma once

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace MMM
{
/// @brief 不可变的数学表达式、定义域证明和累计积分缓存。
/// @details 完整状态只在实现文件中出现，使用方只能借用求值。
class TimingFunction;
/// @brief 从已解析表达式产生的排版节点，供渲染分式、根式和上标。
enum class TimingMathKind {
    Text,
    Negate,
    Add,
    Subtract,
    Multiply,
    Fraction,
    Power,
    Root,
    CubeRoot,
    Absolute,
    Function,
    Integral,
    Sum,
    Product
};
/// @brief 子节点索引总是小于父节点，形成有界不可变排版树。
struct TimingMathNode {
    /// @brief 排版结构，不是可执行脚本。
    TimingMathKind m_kind{ TimingMathKind::Text };
    /// @brief 常数、变量或函数名，数学常数采用 Unicode 符号。
    std::string m_text;
    /// @brief 第一个子节点；文本节点为 -1。
    int m_first{ -1 };
    /// @brief 第二个子节点；一元节点为 -1。
    int m_second{ -1 };
    /// @brief 聚合函数体节点；上下限分别使用 first 与 second。
    int m_third{ -1 };
};
/// @brief 借用与程序同源的排版树，不在每帧重新解析公式。
/// @warning UI 热路径只读借用；对象生命周期由快照或编辑工作副本保持。
std::span<const TimingMathNode> timingFunctionMathNodes(
    const TimingFunction& function);

/// @brief 从 f(t) 编译受限数学表达式，t
/// 为距段首的秒数或拍数，单位由调用方选择。
/// @param expression 只接受数值、变量 t、常数 pi/e 和公开数学函数。
/// @param duration 需要证明定义域合法的闭区间长度。
/// @return 编译成功的不可变对象，或包含位置和失败原因的中文说明。
/// @note 解析和积分准备只在用户编辑或文件读取时执行，不在播放热路径执行。
/// @note 表达式不支持赋值、循环、脚本、文件或外部资源访问。
/// @warning 返回值跨逻辑与 UI 快照共享生命周期；热路径须借用引用求值。
std::expected<std::shared_ptr<const TimingFunction>, std::string>
compileTimingFunction(std::string_view expression, double duration);

/// @brief 取得原始函数文本，保持可编辑性，不导出内部字节码。
/// @return 与函数对象生命周期一致的只读视图。
std::string_view timingFunctionExpression(const TimingFunction& function);
/// @brief 取得编译时验证的自变量域长度，修改范围后须重新编译。
double timingFunctionDuration(const TimingFunction& function);
/// @brief 返回整个闭区间的保守参数界限，不只检查端点。
std::pair<double, double> timingFunctionRange(const TimingFunction& function);

/// @brief 用有界栈执行已编译表达式，返回绝对 Timing 参数。
/// @warning 每帧求值路径；没有解析、分配、共享所有权复制或互斥等待。
double evaluateTimingFunction(const TimingFunction& function, double time);
/// @brief 查询预先建立的累计积分，域外按端值外推。
/// @return 参数乘自变量单位；拍轴的时间积分不能直接消费这个拍域累计量。
/// @warning
/// 拍位热路径；只作有界二分和局部插值，不逐次执行数值积分或表达式解析。
double integrateTimingFunction(const TimingFunction& function, double time);

/// @brief 为变速副本重写时间变量及输出倍率，保留 f(t) 的实际秒语义。
/// @details 只替换独立变量 t，不改写 tan、tanh 等函数名。
std::string rescaleTimingFunctionExpression(const TimingFunction& function,
                                            double                timeScale,
                                            double                valueScale);
}  // namespace MMM
