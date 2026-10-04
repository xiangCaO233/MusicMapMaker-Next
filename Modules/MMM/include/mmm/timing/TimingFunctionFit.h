#pragma once

#include <expected>
#include <span>
#include <string>

namespace MMM
{
/// @brief 用户绘制的曲线点，横轴为距段首的实际秒数或拍数。
struct TimingCurvePoint {
    /// @brief 相对段首的自变量位置，不是归一化比例。
    double m_time{};
    /// @brief 该时刻期望的绝对 Timing 参数。
    double m_value{};
};
/// @brief 可复查和再次编辑的拟合结果，不直接修改谱面。
struct TimingFunctionFit {
    /// @brief 可由受限表达式编译器读取的 f(t)。
    std::string m_expression;
    /// @brief 被选中的函数族，供用户理解拟合方式。
    std::string m_family;
    /// @brief 输入绘制点上的均方根误差，单位与参数相同。
    double m_rmsError{};
    /// @brief 输入绘制点上的最大绝对误差。
    double m_maxError{};
};
/// @brief 比较多项式、三角、指数、对数及根式组合，选出简洁有效的拟合。
/// @param points 顺序递增且覆盖整个段落的绘制点，数量限制为 8–256。
/// @param duration 当前自变量区间长度，与绘制点使用同一单位。
/// @return 有限且可编译的函数与误差，或输入和求解错误。
/// @note 使用重正交化 QR，避免正规方程放大病态误差。
/// @warning 仅在用户点击拟合时求解，不在每帧或播放线程执行。
std::expected<TimingFunctionFit, std::string> fitTimingFunction(
    std::span<const TimingCurvePoint> points, double duration);
}  // namespace MMM
