#pragma once

#include <array>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace MMM::Config
{
/// @brief 插值曲线的稳定配置标识，顺序与领域枚举由消费方静态核对。
/// @note Config 不依赖谱面模型；持久化不使用本地化名称或实体编号。
inline constexpr std::array<std::string_view, 18> TIMING_CURVE_PREFERENCE_NAMES{
    "Linear",
    "EaseIn",
    "EaseOut",
    "SmoothStep",
    "Sine",
    "Exponential",
    "Bezier",
    "Cubic",
    "Quartic",
    "Quintic",
    "SquareRoot",
    "CubeRoot",
    "Logarithmic",
    "Reciprocal",
    "SineIn",
    "SineOut",
    "HyperbolicTangent",
    "Custom"
};

/// @brief 上次确认的插值工具选项，不包含谱面位置、参数值或运行期缓存。
/// @details 新段起值从当前位置继承，时间范围来自本次手势，避免跨谱面误用。
/// 自定义公式只保存源文本，下一次按新段定义域重新编译。
struct TimingInterpolationPreferences {
    /// @brief 稳定英文曲线标识；旧配置默认线性。
    std::string m_curve{ "Linear" };
    /// @brief 是否以节拍为自变量；BPM 创建入口仍须强制秒域。
    bool m_useBeats{};
    /// @brief 秒域的输出采样密度，不保存拍轴对象。
    double m_samplesPerSecond{ 16.0 };
    /// @brief 拍域采样步长的正整数分子与分母。
    int m_beatNumerator{ 1 }, m_beatDenominator{ 2 };
    /// @brief 贝塞尔控制点 X1、Y1、X2、Y2，位于单位方形且 X1 不大于 X2。
    std::array<double, 4> m_controls{ .25, .25, .75, .75 };
    /// @brief 自定义绝对参数公式，最大长度与编辑器输入容量一致。
    std::string m_expression;
};

/// @brief 写出工具选项，所有字段均为轻量值或公式源码。
/// @param json 配置根中的插值工具节点，接收稳定的序列化字段。
/// @param value 上次确认使用的选项，不保存工作副本的时间范围。
void to_json(nlohmann::json& json, const TimingInterpolationPreferences& value);
/// @brief 读取工具选项；缺失或类型损坏的字段使用安全默认值。
/// @param json 允许旧配置缺少的节点或部分字段，不要求含有完整工具定义。
/// @param value 接收规范化结果；公式仍按新段的真实定义域校验。
/// @note 不编译表达式、不访问文件系统；定义域校验归创建入口负责。
void from_json(const nlohmann::json&           json,
               TimingInterpolationPreferences& value);
}  // namespace MMM::Config
